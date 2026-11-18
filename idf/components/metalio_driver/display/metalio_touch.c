/**
 * @file metalio_touch.c
 * @brief FT6336U：GPIO INT 下降沿唤醒 feed；按下期间 10ms 续采
 */

#include "metalio_touch.h"

#include "config.h"
#include "epd_board_metalio_eink4_plus.h"
#include "epd_font24.h"
#include "epdiy.h"
#include "metalio_i2c1.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#define TAG "metalio_touch"

#define FT_REG_TD_STATUS 0x02
#define FT_REG_CHIP_ID   0xA3
#define FT_REG_FW_VER    0xA6
#define FT_REG_INT_MODE  0xA4
#define FT_CHIP_ID_6336  METALIO_TOUCH_FT6336_CHIP_ID

#define FT_EVENT_DOWN    0
#define FT_EVENT_CONTACT 2

#define EPDIY_I2C_PORT I2C_NUM_1
#define I2C1_LOCK_MS   50
#define TOUCH_FEED_MS  10
#define TOUCH_FEED_PRIO 5
#define TOUCH_FEED_STACK 4096

static bool s_ok = false;
static SemaphoreHandle_t s_irq_sem = NULL;
static TaskHandle_t s_feed_task = NULL;
static metalio_touch_sample_cb_t s_sample_cb = NULL;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static metalio_touch_point_t s_ui_pt = {};
static bool s_ui_pressed = false;       // 芯片侧：屏内仍按着
static bool s_ui_seen = false;          // 本次按下是否已被 indev 读到
static bool s_replay_press = false;     // 短按整段错过：下一次 read 回放 PRESSED
static bool s_replay_release = false;   // 回放 PRESSED 后还需一次 RELEASED
static bool s_ui_suppress = false;
static volatile int s_feed_pause_depth = 0; // 待机 / 落墨嵌套暂停（ISR 只读）
static bool s_finger_down = false; // feed 侧：芯片报有触点
static uint32_t s_samples, s_read_errors, s_irq_count;
static int s_last_read_error;
static int s_last_raw_x, s_last_raw_y;

// TCH5a: identification registers. These statics are a CACHE of what was last
// actually read (init or a live read), kept only so the deferred post-init log
// line can print something. They are NOT evidence on their own: a re-init that
// returns early used to clear s_id_valid, which made `ver` report "no chip id"
// even though the controller had answered seconds earlier. Any claim about the
// part must come from metalio_touch_read_id_live().
static bool s_id_valid = false;
static bool s_id_logged = false;
static uint8_t s_chip_id = 0;
static uint8_t s_fw_ver = 0;

// Caller must hold the I2C1 bus lock (metalio_i2c1_lock) and have cleared the
// pause guard -- this is the bare register read used by both ft_read() and the
// live identification read, which needs both registers in ONE locked window so
// an in-flight firmware update cannot straddle 0xA3/0xA6.
static esp_err_t ft_read_i2c_locked(uint8_t reg, uint8_t* buf, size_t n) {
    return metalio_i2c1_transmit_receive_locked(EPDIY_I2C_PORT, TOUCH_I2C_ADDR, &reg, 1, buf, n, 50);
}

static esp_err_t ft_read(uint8_t reg, uint8_t* buf, size_t n) {
    if (n == 0) {
        return ESP_OK;
    }
    if (!metalio_i2c1_lock(I2C1_LOCK_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    // A pause may start while the feed task is waiting for the shared bus.
    // Recheck under the bus lock so it cannot start a read after OTA drains it.
    if (s_feed_pause_depth > 0) {
        metalio_i2c1_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ft_read_i2c_locked(reg, buf, n);
    metalio_i2c1_unlock();
    return ret;
}

static esp_err_t ft_write(uint8_t reg, uint8_t val) {
    if (!metalio_i2c1_lock(I2C1_LOCK_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = metalio_i2c1_write_reg_locked(EPDIY_I2C_PORT, TOUCH_I2C_ADDR,
                                                 reg, &val, 1, 50);
    metalio_i2c1_unlock();
    return ret;
}

static void map_to_logical(int raw_x, int raw_y, int* lx, int* ly) {
    int x = raw_x;
    int y = raw_y;
    if (raw_y >= EPD_LOGICAL_W && raw_x < EPD_LOGICAL_W) {
        x = raw_x;
        y = raw_y;
    } else if (raw_x >= EPD_LOGICAL_W) {
        x = EPD_LOGICAL_W - 1 - raw_y;
        y = raw_x;
    }
    if (x < 0) {
        x = 0;
    } else if (x >= EPD_LOGICAL_W) {
        x = EPD_LOGICAL_W - 1;
    }
    if (y < 0) {
        y = 0;
    } else if (y >= EPD_LOGICAL_H) {
        y = EPD_LOGICAL_H - 1;
    }
    *lx = x;
    *ly = y;
}

static void IRAM_ATTR touch_isr(void* /*arg*/) {
    ++s_irq_count;
    if (s_feed_pause_depth > 0) {
        return;
    }
    BaseType_t hp = pdFALSE;
    if (s_irq_sem != NULL) {
        xSemaphoreGiveFromISR(s_irq_sem, &hp);
    }
    if (hp == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/** 仅清 LVGL 边沿；保留 s_finger_down，否则盖板键每拍 suppress 会停采、松手收不到 */
static void clear_ui_press_locked(void) {
    s_ui_pressed = false;
    s_ui_seen = false;
    s_replay_press = false;
    s_replay_release = false;
}

static void publish_ui(bool pressed, int lx, int ly, int raw_x, int raw_y) {
    portENTER_CRITICAL(&s_mux);
    if (s_ui_suppress) {
        s_ui_pressed = false;
        s_ui_seen = false;
        // 盖板键占用：丢弃未完成的屏内回放，避免键抬起后误点 UI
        s_replay_press = false;
        s_replay_release = false;
    } else if (pressed) {
        s_ui_pt.x = lx;
        s_ui_pt.y = ly;
        s_ui_pt.raw_x = raw_x;
        s_ui_pt.raw_y = raw_y;
        if (!s_ui_pressed) {
            // 新按下：清掉上一拍残留回放，等本拍被读或抬起时再排队
            s_ui_seen = false;
            s_replay_press = false;
            s_replay_release = false;
        }
        s_ui_pressed = true;
    } else {
        // 抬起时若 indev 从未读到 PRESSED，排队回放 press→release
        if (s_ui_pressed && !s_ui_seen) {
            s_replay_press = true;
            s_replay_release = true;
        }
        s_ui_pressed = false;
        s_ui_seen = false;
    }
    portEXIT_CRITICAL(&s_mux);
}

static void feed_one_sample(void) {
    // TCH5a: first sample after the first real IRQ == first moment the chip is
    // identified AND the log sink has been brought up, so the cached values are
    // readable with `logs` instead of lost with the boot line.
    if (!s_id_logged && s_id_valid && s_irq_count > 0) {
        s_id_logged = true;
        ESP_LOGI(TAG, "[TOUCH] fw=0x%02X chip=0x%02X (post-init)", s_fw_ver, s_chip_id);
    }
    uint8_t buf[6] = {0};
    esp_err_t read_result = ft_read(FT_REG_TD_STATUS, buf, sizeof(buf));
    if (read_result == ESP_ERR_INVALID_STATE) {
        return; // Paused intentionally, not a touch-chip read failure.
    }
    ++s_samples;
    s_last_read_error = read_result;
    if (read_result != ESP_OK) {
        ++s_read_errors;
        return;
    }
    const int n = buf[0] & 0x0F;
    if (n == 0 || n > 2) {
        if (s_finger_down) {
            s_finger_down = false;
            ESP_LOGD(TAG, "touch up");
            publish_ui(false, 0, 0, 0, 0);
            if (s_sample_cb != NULL) {
                s_sample_cb(0, 0, 0, 0, 0);
            }
        }
        return;
    }

    const int ev = (buf[1] >> 6) & 0x03;
    if (ev != FT_EVENT_DOWN && ev != FT_EVENT_CONTACT) {
        return;
    }

    const int raw_x = ((buf[1] & 0x0F) << 8) | buf[2];
    const int raw_y = ((buf[3] & 0x0F) << 8) | buf[4];
    s_last_raw_x = raw_x;
    s_last_raw_y = raw_y;
    int lx = 0;
    int ly = 0;
    map_to_logical(raw_x, raw_y, &lx, &ly);

    static int s_last_lx = -1;
    static int s_last_ly = -1;
    const bool edge = !s_finger_down;
    if (edge || lx != s_last_lx || ly != s_last_ly) {
        ESP_LOGD(TAG, "touch %s raw=(%d,%d) logical=(%d,%d)", edge ? "down" : "move", raw_x, raw_y,
                 lx, ly);
        s_last_lx = lx;
        s_last_ly = ly;
    }
    s_finger_down = true;

    if (s_sample_cb != NULL) {
        s_sample_cb(1, raw_x, raw_y, lx, ly);
    }
    // sample_cb 可能已 set_ui_suppress（盖板键）
    publish_ui(true, lx, ly, raw_x, raw_y);
}

static void touch_feed_task(void* /*arg*/) {
    ESP_LOGI(TAG, "touch_feed start prio=%d period=%dms (INT gpio%d)", TOUCH_FEED_PRIO, TOUCH_FEED_MS,
             (int)TOUCH_INT_PIN);
    for (;;) {
        if (s_feed_pause_depth > 0) {
            while (xSemaphoreTake(s_irq_sem, 0) == pdTRUE) {
            }
            vTaskDelay(pdMS_TO_TICKS(TOUCH_FEED_MS));
            continue;
        }
        // IRQ is a latency hint. Poll as fallback so a missed edge cannot disable input.
        const TickType_t wait = pdMS_TO_TICKS(s_finger_down ? TOUCH_FEED_MS : 20);
        xSemaphoreTake(s_irq_sem, wait);
        if (s_feed_pause_depth > 0) {
            continue;
        }
        // 合并连抖边沿
        while (xSemaphoreTake(s_irq_sem, 0) == pdTRUE) {
        }
        feed_one_sample();
        // INT 仍低（手指按着）再补采，避免只靠超时
        if (gpio_get_level(TOUCH_INT_PIN) == 0) {
            feed_one_sample();
        }
    }
}

bool metalio_touch_init(void) {
    s_ok = false;
    s_finger_down = false;
    s_ui_pressed = false;
    s_ui_seen = false;
    s_replay_press = false;
    s_replay_release = false;
    s_ui_suppress = false;
    s_feed_pause_depth = 0;
    // NOTE (TCH5a): deliberately NOT clearing s_id_valid / s_chip_id / s_fw_ver
    // here. Those statics record what was last actually read from the part this
    // boot; a re-init that bails out early (or a second init after the first
    // succeeded) does not un-read the registers, and clearing the flag is exactly
    // what made `ver` report "no chip id" on a controller that had answered.
    // `ver` no longer depends on this flag anyway -- it does a live read.
    // s_id_logged IS reset: a re-init (e.g. after a controller firmware update)
    // should emit the deferred "[TOUCH] fw=... (post-init)" line again, with
    // whatever the post-init cache holds by then.
    s_id_logged = false;

    if (s_irq_sem == NULL) {
        s_irq_sem = xSemaphoreCreateBinary();
        if (s_irq_sem == NULL) {
            ESP_LOGE(TAG, "irq sem create fail");
            return false;
        }
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << TOUCH_INT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);

    epd_board_metalio_eink4_plus_tp_reset();

    uint8_t chip = 0;
    uint8_t fw = 0;
    esp_err_t id_err = ft_read(FT_REG_CHIP_ID, &chip, 1);
    ft_read(FT_REG_FW_VER, &fw, 1);
    if (id_err != ESP_OK) {
        ESP_LOGE(TAG, "no ACK at 0x%02X INT=%d (%s)", TOUCH_I2C_ADDR,
                 gpio_get_level(TOUCH_INT_PIN), esp_err_to_name(id_err));
        return false;
    }
    // 0x00：按下期间 INT 保持低 → 下降沿起采，feed 续采至抬起
    ft_write(FT_REG_INT_MODE, 0x00);

    esp_err_t isr = gpio_install_isr_service(0);
    if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service: %s", esp_err_to_name(isr));
    }
    gpio_isr_handler_remove(TOUCH_INT_PIN);
    if (gpio_isr_handler_add(TOUCH_INT_PIN, touch_isr, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "isr_handler_add GPIO%d fail", (int)TOUCH_INT_PIN);
        return false;
    }

    if (s_feed_task == NULL) {
        if (xTaskCreate(touch_feed_task, "touch_feed", TOUCH_FEED_STACK, NULL, TOUCH_FEED_PRIO,
                        &s_feed_task) != pdPASS) {
            ESP_LOGE(TAG, "touch_feed create fail");
            gpio_isr_handler_remove(TOUCH_INT_PIN);
            return false;
        }
    }

    // 上电 INT 已低：踢一脚避免漏首点
    if (gpio_get_level(TOUCH_INT_PIN) == 0) {
        xSemaphoreGive(s_irq_sem);
    }

    s_ok = true;
    ESP_LOGI(TAG, "ok chip=0x%02X%s fw=0x%02X INT=GPIO%d level=%d mode=irq+feed", chip,
             chip == FT_CHIP_ID_6336 ? " (FT6336)" : "", fw, (int)TOUCH_INT_PIN,
             gpio_get_level(TOUCH_INT_PIN));
    // TCH5a: the line above is emitted before the RAM log ring exists, so it is
    // unretrievable by `logs`/`logtail`. Cache the two register values for `ver`
    // and emit them a second time on the first touch after init, when the sink
    // is live. Two statics, no I2C, no allocation -- the hot path stays untouched.
    s_chip_id = chip;
    s_fw_ver = fw;
    s_id_valid = true;
    return true;
}

metalio_touch_id_t metalio_touch_id(void) {
    metalio_touch_id_t id = {.valid = s_id_valid, .chip = s_chip_id, .fw = s_fw_ver};
    return id;
}

// TCH5a: the live answer. Everything a caller wants to know about the part comes
// from here -- two I2C reads taken under the SAME recursive I2C1 mutex the touch
// feed task holds while it samples (metalio_i2c1_lock / _unlock), so a `ver`
// cannot interleave with a scan read or with the firmware update's register
// writes. Both registers are fetched inside ONE locked window: 0xA3 and 0xA6
// must describe the same part image, and an update in flight cannot straddle them.
//
// State: NOTHING is written, no reset pulse, no power change. The 0xA4 INT-mode
// setting from init and the current scan state are untouched, so there is nothing
// to restore. No wake is needed either: standby parks the controller at 0xA5 =
// 0x01 (system-info, scanning stopped, I2C reachable) and the driver never puts
// it into the 0xA5 = 0x03 hibernate mode where I2C stops answering. RST is
// specifically NOT pulsed here -- that would clear the 0xA4 INT-mode config and
// the pending-touch state that the feed task is using.
//
// Cost: two short transfers, only while `ver` runs. Not on the sample path; the
// feed task does not call this and nothing polls it.
metalio_touch_id_live_t metalio_touch_read_id_live(void) {
    metalio_touch_id_live_t id = {.ok = false, .chip = 0, .fw = 0, .chip_err = ESP_OK, .fw_err = ESP_OK};

    // One retry ~2 ms later, and only for a chip-id NACK: a transient NACK at the
    // tail of a scan is the common failure and a shell command can afford 2 ms.
    // A paused bus (OTA drained the reader) is NOT retried -- that is a refusal,
    // not a fault, and poking at it would only race the drain.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!metalio_i2c1_lock(I2C1_LOCK_MS)) {
            id.chip_err = ESP_ERR_TIMEOUT; // another task held the bus for 50 ms
            id.fw_err = ESP_OK;
            return id;
        }
        // Same pause recheck as ft_read(), under the lock, so `ver` cannot start
        // an I2C transaction after a pause/OTA drain has taken the bus.
        if (s_feed_pause_depth > 0) {
            metalio_i2c1_unlock();
            id.chip_err = ESP_ERR_INVALID_STATE;
            id.fw_err = ESP_OK;
            return id;
        }
        const esp_err_t chip_err = ft_read_i2c_locked(FT_REG_CHIP_ID, &id.chip, 1);
        esp_err_t fw_err = ESP_OK;
        if (chip_err != ESP_OK) {
            id.chip = 0;
        } else if ((fw_err = ft_read_i2c_locked(FT_REG_FW_VER, &id.fw, 1)) != ESP_OK) {
            id.fw = 0;
        }
        metalio_i2c1_unlock();

        id.chip_err = (int)chip_err;
        id.fw_err = (int)fw_err;

        if (chip_err != ESP_OK) {
            if (attempt == 0) {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            return id; // ok stays false; chip_err says why
        }
        break;
    }

    // 0xA3 answered: refresh the cache so the deferred post-init log line (and
    // metalio_touch_id()'s remaining consumers) agree with what `ver` printed.
    s_chip_id = id.chip;
    if (id.fw_err == 0) {
        s_fw_ver = id.fw;
    }
    s_id_valid = true;
    id.ok = (id.chip_err == 0 && id.fw_err == 0);
    return id;
}

void metalio_touch_set_sample_cb(metalio_touch_sample_cb_t cb) {
    s_sample_cb = cb;
}

void metalio_touch_set_ui_suppress(bool suppress) {
    portENTER_CRITICAL(&s_mux);
    s_ui_suppress = suppress;
    if (suppress) {
        clear_ui_press_locked();
    }
    portEXIT_CRITICAL(&s_mux);
}

void metalio_touch_set_feed_paused(bool paused) {
    bool now_paused = false;
    bool edge_resume = false;
    portENTER_CRITICAL(&s_mux);
    if (paused) {
        if (s_feed_pause_depth < 100) {
            s_feed_pause_depth += 1;
        }
        // Keep sampled press/release edges while the shared bus is briefly paused.
    } else if (s_feed_pause_depth > 0) {
        s_feed_pause_depth -= 1;
        edge_resume = (s_feed_pause_depth == 0);
    }
    now_paused = (s_feed_pause_depth > 0);
    portEXIT_CRITICAL(&s_mux);
    if (edge_resume && s_irq_sem != NULL && gpio_get_level(TOUCH_INT_PIN) == 0) {
        xSemaphoreGive(s_irq_sem);
    }
    if ((paused && s_feed_pause_depth == 1) || edge_resume) {
        ESP_LOGD(TAG, "feed %s depth=%d", now_paused ? "paused" : "resumed", s_feed_pause_depth);
    }
}

bool metalio_touch_has_pending_edges(void) {
    if (!s_ok) {
        return false;
    }
    bool pending = false;
    portENTER_CRITICAL(&s_mux);
    pending = s_replay_press || s_replay_release;
    portEXIT_CRITICAL(&s_mux);
    return pending;
}

bool metalio_touch_read(metalio_touch_point_t* out) {
    if (!s_ok) {
        return false;
    }
    bool pressed = false;
    metalio_touch_point_t pt = {};
    portENTER_CRITICAL(&s_mux);
    if (s_replay_press) {
        // 刷屏期间错过的短按：先交付一次 PRESSED
        s_replay_press = false;
        pressed = true;
        pt = s_ui_pt;
    } else if (s_ui_pressed) {
        s_ui_seen = true;
        pressed = true;
        pt = s_ui_pt;
    } else if (s_replay_release) {
        // 回放的 RELEASED：本拍返回 false，清标志
        s_replay_release = false;
        pressed = false;
    } else {
        pressed = false;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!pressed) {
        return false;
    }
    if (out != NULL) {
        *out = pt;
    }
    return true;
}

bool metalio_touch_ok(void) {
    return s_ok;
}

metalio_touch_stats_t metalio_touch_stats(void) {
    metalio_touch_stats_t stats = {
        .ok = s_ok, .samples = s_samples, .read_errors = s_read_errors,
        .irqs = s_irq_count, .last_error = s_last_read_error,
        .raw_x = s_last_raw_x, .raw_y = s_last_raw_y,
        .irq_level = gpio_get_level(TOUCH_INT_PIN), .pause_depth = s_feed_pause_depth,
    };
    return stats;
}
