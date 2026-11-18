/**
 * @file epd_board_metalio_eink4_plus.c
 * @brief Metalio ESP32-S31 4.7" 并行墨水屏板级（E0470A14-AF 1216x684）
 * @note 引脚来自板级 config.h / 原理图 V1.5；TPS 控制脚经 TCA9555
 */

#include "epd_board_metalio_eink4_plus.h"
#include "epdiy.h"

#include "lcd_driver.h"
#include "metalio_sd.h"
#include "pca9555.h"
#include "metalio_i2c1.h"
#include "tps65185.h"
#include "config.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_rom_sys.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdkconfig.h>

/* r85: CP_TPS_FAST_SEQUENCE — one flag toggles this board layer between RickyOS's
 * polled, settle-free power-up / tail-free per-drive cycle (1) and r84's
 * 50 + 50 ms settles (0). Why, and what a wrong call looks like on glass, is
 * beside the switch in src/display/RefreshPolicy.h. */
#include "../../../../src/display/RefreshPolicy.h"

/* r106 B1 (R103_GLASS_LOG §F1): the pure bit arithmetic for a single-pin write. */
#include "../../../../src/platform/TcaRmw.h"

#define TAG "epd_eink4p"

/* I2C：TPS/TCA 同总线；TCA 开漏 INT → ESP */
#define CFG_SCL I2C_SCL_PIN
#define CFG_SDA I2C_SDA_PIN
#define CFG_INTR IO_EXPANDER_INT_GPIO
#define EPDIY_I2C_PORT I2C_NUM_1

/* TCA 位/口：一律由 config.h 的 EPD_TPS_IO_* / EPD_IO_* 推出 */
#define CFG_IO_BIT(io)  (1u << ((unsigned)(io) & 7u))
#define CFG_IO_PORT(io) ((int)((unsigned)(io) >> 3))

#define CFG_PIN_VCOM_CTRL CFG_IO_BIT(EPD_TPS_IO_VCOM_CTRL)
#define CFG_PIN_PWRUP     CFG_IO_BIT(EPD_TPS_IO_PWRUP)
#define CFG_PIN_WAKEUP    CFG_IO_BIT(EPD_TPS_IO_WAKEUP)
#define CFG_PIN_PWRGOOD   CFG_IO_BIT(EPD_TPS_IO_PWR_GOOD)
#define CFG_PIN_INT       CFG_IO_BIT(EPD_TPS_IO_INT)
#define CFG_PIN_MOTOR     CFG_IO_BIT(EPD_IO_MOTOR)
#define CFG_PIN_PA_EN     CFG_IO_BIT(EPD_IO_PA_EN)
#define CFG_PIN_CAM_SCR   CFG_IO_BIT(EPD_IO_CAM_SCR_EN)
#define CFG_PIN_TP_RST    CFG_IO_BIT(EPD_IO_TP_RST)
#define CFG_PIN_BT_PA_PWR CFG_IO_BIT(EPD_IO_BT_PA_PWR)
#define CFG_PIN_PWR_PULSE CFG_IO_BIT(EPD_IO_PWR_PULSE)

#define CFG_PORT_CTRL CFG_IO_PORT(EPD_TPS_IO_VCOM_CTRL) // Port0：VCOM/PWRUP/WAKEUP/PWR_GOOD
#define CFG_PORT_INT  CFG_IO_PORT(EPD_TPS_IO_INT)       // Port1：TPS_INT / TP_RST / 关机脉冲

/* Port0：TPS/Motor/PA/摄像屏电 输出；PWR_GOOD 等为输入 */
#define CFG_P0_TPS_OUT_MASK (CFG_PIN_VCOM_CTRL | CFG_PIN_PWRUP | CFG_PIN_WAKEUP)
#define CFG_P0_OUTPUT_MASK                                                                 \
    (CFG_P0_TPS_OUT_MASK | CFG_PIN_MOTOR | CFG_PIN_PA_EN | CFG_PIN_CAM_SCR)
#define CFG_P0_INPUT_MASK ((uint8_t)(~CFG_P0_OUTPUT_MASK))

/* Port1：TP_RST/P14/关机脉冲为输出；P16=RTC_INT 等为输入 */
#define CFG_P1_RAIL_MASK   (CFG_PIN_TP_RST | CFG_PIN_BT_PA_PWR)
#define CFG_P1_OUTPUT_MASK (CFG_P1_RAIL_MASK | CFG_PIN_PWR_PULSE)
#define CFG_P1_INPUT_MASK  ((uint8_t)(~CFG_P1_OUTPUT_MASK))

/* TCA9555 configuration-space registers used by the r106 read-modify-write. */
#define CFG_TCA_OUTPUT0 0x02
#define CFG_TCA_OUTPUT1 0x03

/* 并行数据 / 控制：板级 config.h */
#define D0 EPD_PIN_D0
#define D1 EPD_PIN_D1
#define D2 EPD_PIN_D2
#define D3 EPD_PIN_D3
#define D4 EPD_PIN_D4
#define D5 EPD_PIN_D5
#define D6 EPD_PIN_D6
#define D7 EPD_PIN_D7

#define CKH EPD_PIN_XCL
#define LEH EPD_PIN_XLE
#define EPD_OE EPD_PIN_XOE
#define STH EPD_PIN_XSTL
#define STV EPD_PIN_SPV
#define EPD_MODE EPD_PIN_MODE
#define EPD_BORDER EPD_PIN_BORDER
#define CKV EPD_PIN_CKV

typedef struct {
    i2c_port_t port;
    bool pwrup;
    bool vcom_ctrl;
    bool wakeup;
} epd_config_register_t;

static int vcom = EPD_VCOM_MV;
static epd_config_register_t config_reg;
static bool interrupt_done = false;
static SemaphoreHandle_t s_ioexp_irq_sem = NULL;
static TaskHandle_t s_gpio2_irq_task = NULL;

static void IRAM_ATTR interrupt_handler(void* arg) {
    (void)arg;
    interrupt_done = true;
    BaseType_t hp = pdFALSE;
    if (s_ioexp_irq_sem != NULL) {
        xSemaphoreGiveFromISR(s_ioexp_irq_sem, &hp);
    }
    if (s_gpio2_irq_task != NULL) {
        vTaskNotifyGiveFromISR(s_gpio2_irq_task, &hp);
    }
    if (hp == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static lcd_bus_config_t lcd_config = {
    .clock = CKH,
    .ckv = CKV,
    .leh = LEH,
    .start_pulse = STH,
    .stv = STV,
    .data[0] = D0,
    .data[1] = D1,
    .data[2] = D2,
    .data[3] = D3,
    .data[4] = D4,
    .data[5] = D5,
    .data[6] = D6,
    .data[7] = D7,
    .data[8] = D7,
    .data[9] = D7,
    .data[10] = D7,
    .data[11] = D7,
    .data[12] = D7,
    .data[13] = D7,
    .data[14] = D7,
    .data[15] = D7,
};

static bool s_ioexp_ok = false;
static StaticSemaphore_t s_shared_reset_storage;
static SemaphoreHandle_t s_shared_reset_mu;
static portMUX_TYPE s_shared_reset_init = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t shared_reset_mutex(void) {
    portENTER_CRITICAL(&s_shared_reset_init);
    if (s_shared_reset_mu == NULL) {
        s_shared_reset_mu = xSemaphoreCreateMutexStatic(&s_shared_reset_storage);
    }
    portEXIT_CRITICAL(&s_shared_reset_init);
    return s_shared_reset_mu;
}

esp_err_t epd_board_metalio_eink4_plus_camera_acquire(uint32_t timeout_ms) {
    SemaphoreHandle_t mu = shared_reset_mutex();
    if (xSemaphoreTake(mu, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE("DvpCameraPower", "camera power: shared P12 reset lock timeout (%u ms)", (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    uint8_t p0 = 0, p1 = 0;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_ioexp_ok) {
        err = pca9555_read_input_ex(I2C_NUM_1, 0, &p0);
        if (err != ESP_OK) {
            ESP_LOGE("DvpCameraPower", "camera power: TCA @0x%02x Port0 read failed: %s (0x%x)",
                     pca9555_get_addr(), esp_err_to_name(err), (unsigned)err);
        } else {
            ESP_LOGI("DvpCameraPower", "camera power: TCA @0x%02x P0=0x%02x P5(CAM/SCR_EN)=%u",
                     pca9555_get_addr(), p0, (unsigned)((p0 & CFG_PIN_CAM_SCR) != 0));
            err = pca9555_read_input_ex(I2C_NUM_1, 1, &p1);
            if (err != ESP_OK) {
                ESP_LOGE("DvpCameraPower", "camera reset: TCA Port1 read failed: %s (0x%x)",
                         esp_err_to_name(err), (unsigned)err);
            } else {
                ESP_LOGI("DvpCameraPower", "camera reset: P1=0x%02x P12(shared RST)=%u; no reset pulse",
                         p1, (unsigned)((p1 & CFG_PIN_TP_RST) != 0));
            }
        }
        if (err == ESP_OK && (!(p0 & CFG_PIN_CAM_SCR) || !(p1 & CFG_PIN_TP_RST))) {
            ESP_LOGE("DvpCameraPower", "camera power/reset: P5 or P12 is LOW; shared outputs left unchanged");
            err = ESP_ERR_INVALID_STATE;
        }
    } else {
        ESP_LOGE("DvpCameraPower", "camera power: TCA expander is not ready");
    }
    if (err != ESP_OK) xSemaphoreGive(mu);
    return err;
}

void epd_board_metalio_eink4_plus_camera_release(void) {
    xSemaphoreGive(shared_reset_mutex());
}
static bool s_tps_ok = false;
static bool s_lcd_bus_held = false;       // 入睡已拆 LCD 并置低总线脚
// Port1 锁存：P12/P14 默认高（上电），关机脉冲默认低
static uint8_t s_p1_out = (uint8_t)(CFG_PIN_TP_RST | CFG_PIN_BT_PA_PWR);
static uint8_t s_motor_on = 0;             // Port0 马达位锁存，0 或 CFG_PIN_MOTOR
static uint8_t s_pa_on = 0;                // Port0 PA_EN 锁存，0 或 CFG_PIN_PA_EN
static bool s_bt_reset_pa_off = false;     // P14 复位期间屏蔽所有 PA 开启请求
static uint8_t s_cam_scr_on = CFG_PIN_CAM_SCR; // Port0 P05 摄像/屏电，默认高

/* r106 B1: the Port0 OUTPUT latch as last READ from (or WRITTEN to) the part.
 *
 * Why this exists beside p0_output_value(): p0_output_value() is a byte REBUILT
 * from this board's own latches, and on r105 that byte was 0x00 at a CAM cut
 * (every TPS rail was already down, motor and PA off), so the single-bit cut
 * cleared the whole port and aborted sdmmc. The fix is a read-modify-write of
 * TCA output register 0x02; this shadow is only the FALLBACK for a failed read,
 * and it is seeded from every successful read/write so it holds what the part
 * holds, not what this board meant. The vendor keeps the same kind of shadow
 * (`BringupExt.cpp`), which is why their rail cycling survives in the field. */
static uint8_t s_p0_shadow = 0;
static bool s_p0_shadow_valid = false;

static uint8_t p0_output_value(void) {
    uint8_t value = 0x00;
    if (config_reg.pwrup) {
        value |= CFG_PIN_PWRUP;
    }
    if (config_reg.vcom_ctrl) {
        value |= CFG_PIN_VCOM_CTRL;
    }
    if (config_reg.wakeup) {
        value |= CFG_PIN_WAKEUP;
    }
    value |= s_motor_on;
    if (!s_bt_reset_pa_off) value |= s_pa_on;
    value |= s_cam_scr_on;
    return value;
}

/* Read the Port0 OUTPUT register (0x02; Port1 is 0x03). The INPUT ports at
 * 0x00/0x01 are NOT usable here: they report pin levels, so a read-modify-write
 * built from them would drive every output to whatever the pins happened to be. */
static bool p0_output_read(uint8_t* out) {
    uint8_t v = 0;
    if (pca9555_read_reg_ex(config_reg.port, CFG_TCA_OUTPUT0, &v) != ESP_OK) {
        return false;
    }
    s_p0_shadow = v;
    s_p0_shadow_valid = true;
    if (out != NULL) *out = v;
    return true;
}


static void p0_write(void) {
    esp_err_t err = pca9555_set_value(config_reg.port, p0_output_value(), CFG_PORT_CTRL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "p0_write: %s", esp_err_to_name(err));
    }
}

static void p1_write(void) {
    esp_err_t err = pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "p1_write: %s", esp_err_to_name(err));
    }
}

bool epd_board_metalio_eink4_plus_tps_ok(void) {
    return s_tps_ok;
}

bool epd_board_metalio_eink4_plus_ioexp_ok(void) {
    return s_ioexp_ok;
}

void epd_board_metalio_eink4_plus_motor_set(bool on) {
    if (!s_ioexp_ok) {
        return;
    }
    s_motor_on = on ? CFG_PIN_MOTOR : 0;
    p0_write();
}

void epd_board_metalio_eink4_plus_pa_set(bool on) {
    if (!s_ioexp_ok) {
        return;
    }
    s_pa_on = on ? CFG_PIN_PA_EN : 0;
    p0_write();
}

esp_err_t epd_board_metalio_eink4_plus_bt_power_reset(void) {
    if (!s_ioexp_ok) return ESP_ERR_INVALID_STATE;
    s_bt_reset_pa_off = true;
    s_pa_on = 0;
    esp_err_t pa_err = pca9555_set_value(config_reg.port, p0_output_value(), CFG_PORT_CTRL);
    if (pa_err != ESP_OK) {
        s_bt_reset_pa_off = false;
        return pa_err;
    }
    s_p1_out &= (uint8_t)~CFG_PIN_BT_PA_PWR;
    esp_err_t low_err = pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    if (low_err == ESP_OK) {
        ESP_LOGI(TAG, "BT reset: P14 low for 3000 ms");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    // 即使拉低失败也尝试恢复电源，并保持共享端口其他位。
    s_p1_out |= CFG_PIN_BT_PA_PWR;
    esp_err_t high_err = pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    // 不恢复复位期间到达的 PA 开启请求，完成后仍保持低。
    s_pa_on = 0;
    s_bt_reset_pa_off = false;
    if (high_err != ESP_OK) return high_err;
    ESP_LOGI(TAG, "BT reset: P14 high");
    return low_err;
}

void epd_board_metalio_eink4_plus_ana_sw_set(bool high) {
    static bool s_gpio_ready = false;
    if (!s_gpio_ready) {
        gpio_reset_pin(ANA_SW_GPIO);
        gpio_set_direction(ANA_SW_GPIO, GPIO_MODE_OUTPUT);
        s_gpio_ready = true;
    }
    gpio_set_level(ANA_SW_GPIO, high ? 1 : 0);
}

void epd_board_metalio_eink4_plus_sleep_cut_rails(void) {
    if (!s_ioexp_ok) {
        return;
    }
    s_cam_scr_on = 0;
    p0_write();
    s_p1_out &= (uint8_t)~CFG_PIN_BT_PA_PWR;
    p1_write();
    ESP_LOGI(TAG, "sleep cut P5 then P14");
}

void epd_board_metalio_eink4_plus_sleep_restore_rails(void) {
    if (!s_ioexp_ok) {
        return;
    }
    // ---- 恢复顺序取 vendor v2.0.74（同文件 :344）：P1.4 -> 80 ms（锁已放开）-> 复查 -> P0.5 --
    //
    // 旧写法是四条语句连着走：置 P1.4 位、写 Port1、置 P0.5 位、写 Port0。两路上电负载的
    // 浪涌落在同一次上电里，P0.5 前面既没有稳定时间也没有复查，而且这四行全程占着锁。
    //
    // 为什么中间必须放锁：TCA9555 是按键扫描和触摸共用的器件，P1.4 之后要等 80 ms——如果
    // 这 80 ms 握着那把锁，每次唤醒都会把按键/触摸的 I2C 访问卡住这么久。所以是
    // 上锁 -> 拉起 P1.4 -> 解锁 -> 等 -> 重新上锁，锁只罩住两次写本身。
    //
    // 为什么解锁后要复查：等待期间 BT 复位流程可能接管 P1.4（s_bt_reset_pa_off），或者 P1.4
    // 被别的路径重新切掉。这两种情况下都不该再把 P0.5 拉起来——那等于在 P1.4 没起来的板子上
    // 又加一份浪涌。复查不过就停在 P1.4 已写入的状态，由下一次调用重来。
    SemaphoreHandle_t mu = shared_reset_mutex();
    if (xSemaphoreTake(mu, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_bt_reset_pa_off) {
        // BT 复位期间 P1.4 归它管；现在拉高会打断 3 s 低电平窗口。
        xSemaphoreGive(mu);
        ESP_LOGW(TAG, "sleep restore: P14 owned by BT reset, skipped");
        return;
    }
    s_p1_out |= CFG_PIN_BT_PA_PWR;
    const esp_err_t p1_err = pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    xSemaphoreGive(mu);
    if (p1_err != ESP_OK) {
        // P1.4 没真正起来，就别把第二路负载叠上去。
        ESP_LOGW(TAG, "sleep restore: P14 write failed (%s), P5 left down", esp_err_to_name(p1_err));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(POWER_RAIL_SETTLE_MS));

    if (xSemaphoreTake(mu, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_bt_reset_pa_off || (s_p1_out & CFG_PIN_BT_PA_PWR) == 0) {
        xSemaphoreGive(mu);
        ESP_LOGW(TAG, "sleep restore: P14 no longer wanted (bt_reset=%d), P5 left down",
                 s_bt_reset_pa_off ? 1 : 0);
        return;
    }
    s_cam_scr_on = CFG_PIN_CAM_SCR;
    const esp_err_t p0_err = pca9555_set_value(config_reg.port, p0_output_value(), CFG_PORT_CTRL);
    xSemaphoreGive(mu);
    if (p0_err != ESP_OK) {
        ESP_LOGW(TAG, "sleep restore: P5 write failed (%s)", esp_err_to_name(p0_err));
        return;
    }
    ESP_LOGI(TAG, "sleep restore P14 then P5 (settle %d ms)", POWER_RAIL_SETTLE_MS);

    // 明确不取 4b：vendor 在 P0.5 之后、恢复显示总线之前还有第二个 POWER_RAIL_SETTLE_MS。
    // 本板没有蜂窝模块，第二个 wait 保护的那条上电链路这里根本不存在，多等 80 ms 只是让每次
    // 唤醒更慢。不要“补回来”。
}

bool epd_board_metalio_eink4_plus_pa_rail_set(bool on) {
    if (!s_ioexp_ok) {
        return false;
    }
    // P1.4 only (BT_PA_PWR: 4G + BT-audio + NS4150 amp shared rail). P0.5 is
    // deliberately NOT touched here even when it is "off" -- sleep_cut_rails() above
    // is the both-rails call and stays unused. The evidence for P1.4 being safe to
    // cut alone, and the order it must be cut/restored in, is in
    // src/display/RefreshPolicy.h :: CP_CUT_PA_RAIL_ON_STANDBY.
    //
    // Shadows s_p1_out so ioexp_mask_for_sleep() / ioexp_restore_keys() -- both of
    // which preserve `s_p1_out & CFG_P1_RAIL_MASK` -- carry the cut across the slice
    // without a second write, and so a lost I2C transaction cannot leave the shadow
    // disagreeing with the part.
    const uint8_t before = s_p1_out;
    if (on) {
        s_p1_out |= CFG_PIN_BT_PA_PWR;
    } else {
        s_p1_out &= (uint8_t)~CFG_PIN_BT_PA_PWR;
    }
    if (s_p1_out == before) {
        return true;
    }
    const esp_err_t err = pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    if (err != ESP_OK) {
        s_p1_out = before;
        ESP_LOGW(TAG, "P14 %s: %s", on ? "on" : "off", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "P14 BT/PA rail %s (P1=0x%02X)", on ? "on" : "off", s_p1_out);
    return true;
}

bool epd_board_metalio_eink4_plus_cam_rail_set(bool on) {
    if (!s_ioexp_ok) {
        return false;
    }
    // P0.5 only (CAM_SCR_EN: 摄像/屏电). P1.4 is pa_rail_set() above; the both-rails
    // call sleep_cut_rails() stays the poweroff path's and is NOT used for standby --
    // the two rails are gated by separate trims so either can be A/B'd alone.
    //
    // r106 B1: READ-MODIFY-WRITE of the TCA OUTPUT latch (register 0x02), never a byte
    // rebuilt from this board's latches. On r105 this function wrote p0_output_value(),
    // which was 0x00 while the part held other output bits, so `trim cam cut` cleared
    // the whole port -- sdmmc aborted (0x109/0x107/0x108) and the device panic-rebooted
    // (R103_GLASS_LOG §F1). Now only bit 5 can change; every other output bit is
    // whatever the READ said it was. The vendor does the same through a shadow that
    // still holds the other pins.
    //
    // If the read fails the fallback shadow is s_p0_shadow, seeded from every successful
    // port-0 read/write, so it holds the part's byte rather than this board's intent;
    // failing that, p0_output_value(). The path taken is in the log line.
    //
    // s_cam_scr_on is still updated because epd_poweron(), ioexp_mask_for_sleep() and
    // ioexp_restore_keys() rebuild Port0 from p0_output_value(): a cut that is not in
    // that latch would be driven HIGH again by the first standby slice.
    uint8_t cur = 0;
    const bool readOk = p0_output_read(&cur);
    int path = TCA_RMW_PATH_READ;
    if (!readOk) {
        path = TCA_RMW_PATH_SHADOW;
        cur = s_p0_shadow_valid ? s_p0_shadow : p0_output_value();
    }
    const tca_rmw_plan_t plan = tca_rmw_apply_bit(cur, (uint8_t)CFG_PIN_CAM_SCR, on ? 1 : 0, path);
    if (plan.value == cur) {
        // The part already holds the requested bit: no write, but the latch still follows.
        s_cam_scr_on = on ? CFG_PIN_CAM_SCR : 0;
        if (readOk) {
            ESP_LOGI(TAG, "P05 CAM/SCR rail %s: read=0x%02X write=0x%02X (%s, no bit change)",
                     on ? "on" : "off", cur, plan.value, tca_rmw_path_name(path));
        } else {
            ESP_LOGI(TAG, "P05 CAM/SCR rail %s: read=FAIL write=0x%02X (%s, shadow already %s)",
                     on ? "on" : "off", plan.value, tca_rmw_path_name(path), on ? "high" : "low");
        }
        return true;
    }
    const esp_err_t err = pca9555_set_value(config_reg.port, plan.value, CFG_PORT_CTRL);
    if (err != ESP_OK) {
        // The part did not take the write: the latches keep the PREVIOUS state, so the
        // caller's "rail stayed up" report is true.
        if (readOk) {
            ESP_LOGW(TAG, "P05 CAM/SCR rail %s FAILED: read=0x%02X write=0x%02X (%s) err=%s",
                     on ? "on" : "off", cur, plan.value, tca_rmw_path_name(path), esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "P05 CAM/SCR rail %s FAILED: read=FAIL write=0x%02X (%s) err=%s",
                     on ? "on" : "off", plan.value, tca_rmw_path_name(path), esp_err_to_name(err));
        }
        return false;
    }
    s_cam_scr_on = on ? CFG_PIN_CAM_SCR : 0;
    s_p0_shadow = plan.value;
    s_p0_shadow_valid = true;
    if (readOk) {
        ESP_LOGI(TAG, "P05 CAM/SCR rail %s: read=0x%02X write=0x%02X (%s)", on ? "on" : "off", cur, plan.value,
                 tca_rmw_path_name(path));
    } else {
        ESP_LOGI(TAG, "P05 CAM/SCR rail %s: read=FAIL write=0x%02X (%s)", on ? "on" : "off", plan.value,
                 tca_rmw_path_name(path));
    }
    return true;
}

bool epd_board_metalio_eink4_plus_ioexp_get_level(int io) {
    static uint8_t s_last[2] = {0xFF, 0xFF}; // 读失败保持；默认当高
    if (!s_ioexp_ok || io < 0 || io > 15) {
        return true;
    }
    const int port = CFG_IO_PORT(io);
    uint8_t v = s_last[port];
    if (pca9555_read_input_ex(config_reg.port, port, &v) == ESP_OK) {
        s_last[port] = v;
    }
    return (s_last[port] & CFG_IO_BIT(io)) != 0;
}

bool epd_board_metalio_eink4_plus_ioexp_read_port(int port, uint8_t* out) {
    if (!s_ioexp_ok || out == NULL || port < 0 || port > 1) {
        return false;
    }
    return pca9555_read_input_ex(config_reg.port, port, out) == ESP_OK;
}

/* r106 `ioexp`: four port bytes + both latches, for the shell. Read-only; four
 * short I2C transactions and nothing else. A failed read leaves its byte at 0 and
 * its ok flag false, so "0x00" and "did not answer" are never the same answer. */
bool epd_board_metalio_eink4_plus_ioexp_dump(epd_ioexp_dump_t* out) {
    if (out == NULL) {
        return false;
    }
    out->in0 = out->in1 = out->out0 = out->out1 = 0;
    out->in0_ok = out->in1_ok = out->out0_ok = out->out1_ok = false;
    if (!s_ioexp_ok) {
        out->latch0 = out->shadow0 = out->latch1 = 0;
        out->shadow0_valid = false;
        return false;
    }
    out->in0_ok = pca9555_read_input_ex(config_reg.port, CFG_PORT_CTRL, &out->in0) == ESP_OK;
    out->in1_ok = pca9555_read_input_ex(config_reg.port, CFG_PORT_INT, &out->in1) == ESP_OK;
    out->out0_ok = pca9555_read_reg_ex(config_reg.port, CFG_TCA_OUTPUT0, &out->out0) == ESP_OK;
    out->out1_ok = pca9555_read_reg_ex(config_reg.port, CFG_TCA_OUTPUT1, &out->out1) == ESP_OK;
    if (out->out0_ok) {
        s_p0_shadow = out->out0;
        s_p0_shadow_valid = true;
    }
    out->latch0 = p0_output_value();
    out->latch1 = s_p1_out;
    out->shadow0 = s_p0_shadow_valid ? s_p0_shadow : 0;
    out->shadow0_valid = s_p0_shadow_valid;
    return true;
}

bool epd_board_metalio_eink4_plus_ioexp_irq_wait(uint32_t timeout_ms) {
    if (s_ioexp_irq_sem == NULL) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms == 0 ? 1 : timeout_ms));
        return false;
    }
    return xSemaphoreTake(s_ioexp_irq_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void epd_board_metalio_eink4_plus_gpio2_irq_subscribe(TaskHandle_t task) {
    s_gpio2_irq_task = task;
}

void epd_board_metalio_eink4_plus_tps_enter_sleep(void) {
    if (!s_ioexp_ok) {
        return;
    }
    // tip 的 poweroff 往往已把三脚拉低；SLEEP 过渡期勿再打 I2C，只补等待
    const bool already =
        !config_reg.vcom_ctrl && !config_reg.pwrup && !config_reg.wakeup;
    if (!already) {
        // 手册 8.4：PWRUP/VCOM↓ → WAKEUP↓ → 等 ≥100ms
        config_reg.vcom_ctrl = false;
        config_reg.pwrup = false;
        p0_write();
        vTaskDelay(pdMS_TO_TICKS(50));
        config_reg.wakeup = false;
        p0_write();
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "TPS SLEEP ready (already=%d)", already ? 1 : 0);
}

// ---------------------------------------------------------------------------
// READ GUARD — the PMIC-asleep trap.
//
// The TPS65185 answers I2C only in STANDBY or ACTIVE. After every drive
// (CP_EPD_POWERDOWN_AFTER_DRIVE) and on standby the board drives PWRUP/VCOM/WAKEUP
// low, i.e. the part sits in SLEEP, where it does not ACK at all (manual 8.4; the
// same statement is on epd_board_metalio_eink4_plus_tps_enter_sleep() in the
// header). A debug readout that arrives in that state sees one
// ESP_ERR_INVALID_RESPONSE per register and looks exactly like a dead bus.
//
// So a read-only probe WAKEs the part itself (WAKEUP high, PWRUP/VCOM low ==
// STANDBY, I2C usable) and then puts back precisely the three control bits it
// found. Nothing here raises PWRUP: a caller that found the board awake leaves it
// awake, a caller that found it asleep leaves it asleep -- a debug command must
// never leave the rails up (battery + ghosting).
// ---------------------------------------------------------------------------
static struct {
    bool saved;
    bool wakeup;
    bool pwrup;
    bool vcom_ctrl;
} s_read_guard;

bool epd_board_metalio_eink4_plus_tps_wake_for_read(uint32_t timeout_ms, bool* was_asleep,
                                                    unsigned* waited_ms) {
    if (was_asleep) *was_asleep = false;
    if (waited_ms) *waited_ms = 0;
    if (!s_ioexp_ok) return false;
    if (s_read_guard.saved) return true;  // one guard at a time; already readable

    s_read_guard.saved = true;
    s_read_guard.wakeup = config_reg.wakeup;
    s_read_guard.pwrup = config_reg.pwrup;
    s_read_guard.vcom_ctrl = config_reg.vcom_ctrl;

    if (was_asleep) *was_asleep = !(config_reg.wakeup || config_reg.pwrup);

    // Already STANDBY/ACTIVE: the part answers. Do not touch the rails at all.
    if (config_reg.wakeup || config_reg.pwrup) return true;

    config_reg.wakeup = true;
    config_reg.pwrup = false;
    config_reg.vcom_ctrl = false;
    p0_write();

    // Manual 8.4: WAKEUP high -> t_WAKE (typ 10 ms) until I2C is valid. Probe
    // instead of assuming -- a part whose rails were just cycled can take longer.
    for (unsigned ms = 0; ms < timeout_ms; ++ms) {
        if (tps_probe(config_reg.port) == ESP_OK) {
            if (waited_ms) *waited_ms = ms;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (waited_ms) *waited_ms = timeout_ms;
    return false;
}

void epd_board_metalio_eink4_plus_tps_read_restore(void) {
    if (!s_read_guard.saved) return;
    s_read_guard.saved = false;
    const bool wake = s_read_guard.wakeup;
    const bool up = s_read_guard.pwrup;
    const bool vc = s_read_guard.vcom_ctrl;
    if (config_reg.wakeup == wake && config_reg.pwrup == up && config_reg.vcom_ctrl == vc) {
        return;  // nothing was changed
    }
    config_reg.wakeup = wake;
    config_reg.pwrup = up;
    config_reg.vcom_ctrl = vc;
    p0_write();
    // Going back to SLEEP: manual 8.4 wants PWRUP/VCOM low, then WAKEUP low, then
    // >=100 ms before the next power-up. PWRUP/VCOM are already low on this path,
    // so only the tail is owed.
    if (!wake) {
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}

static const gpio_num_t k_epd_bus_pins[] = {
    D0, D1, D2, D3, D4, D5, D6, D7, CKH, LEH, EPD_OE, STH, STV, CKV, EPD_MODE, EPD_BORDER,
};

static void epd_ctrl_gpios_out_low(void) {
    gpio_reset_pin(EPD_OE);
    gpio_set_direction(EPD_OE, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_OE, 0);
    gpio_reset_pin(EPD_MODE);
    gpio_set_direction(EPD_MODE, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_MODE, 0);
    gpio_reset_pin(EPD_BORDER);
    gpio_set_direction(EPD_BORDER, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_BORDER, 0);
}

static void epd_lcd_reinit(void) {
    const EpdDisplay_t* display = epd_get_display();
    LcdEpdConfig_t config = {
        .pixel_clock = display->bus_speed * 1000 * 1000,
        .ckv_high_time = 60,
        .line_front_porch = 4,
        .le_high_time = 9,
        .bus_width = display->bus_width,
        .bus = lcd_config,
        .dummy_clocks_after_data = 10, // E0470A14-AF section 6.3.1.
        .white_border_pixels = DISPLAY_WHITE_BORDER_PIXELS,
    };
    epd_lcd_init(&config, display->width, display->height);
}

void epd_board_metalio_eink4_plus_bus_hold_low(void) {
    for (size_t i = 0; i < sizeof(k_epd_bus_pins) / sizeof(k_epd_bus_pins[0]); ++i) {
        gpio_hold_dis(k_epd_bus_pins[i]);
    }
    if (!s_lcd_bus_held) {
        epd_lcd_deinit();
        s_lcd_bus_held = true;
    }
    for (size_t i = 0; i < sizeof(k_epd_bus_pins) / sizeof(k_epd_bus_pins[0]); ++i) {
        const gpio_num_t pin = k_epd_bus_pins[i];
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_pull_mode(pin, GPIO_FLOATING);
        gpio_set_level(pin, 0);
    }
    ESP_LOGI(TAG, "EPD bus pins held low D0..D7/XCL/XLE/XOE/XSTL/SPV/CKV/MODE/BORDER");
}

void epd_board_metalio_eink4_plus_bus_latch_for_sleep(void) {
    for (size_t i = 0; i < sizeof(k_epd_bus_pins) / sizeof(k_epd_bus_pins[0]); ++i) {
        esp_err_t err = gpio_hold_en(k_epd_bus_pins[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "gpio_hold_en(%d): %s", (int)k_epd_bus_pins[i], esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "EPD bus pins latched for sleep");
}

void epd_board_metalio_eink4_plus_bus_restore(void) {
    if (!s_lcd_bus_held) {
        return;
    }
    for (size_t i = 0; i < sizeof(k_epd_bus_pins) / sizeof(k_epd_bus_pins[0]); ++i) {
        gpio_hold_dis(k_epd_bus_pins[i]);
    }
    epd_ctrl_gpios_out_low();
    epd_lcd_reinit();
    s_lcd_bus_held = false;
    ESP_LOGI(TAG, "EPD bus restored after sleep");
}

void epd_board_metalio_eink4_plus_boot_release_holds(void) {
    // 深睡 gpio_hold 跨 CPU 复位仍有效；不解则并行脚锁死为低，TPS 升压易失败
    for (size_t i = 0; i < sizeof(k_epd_bus_pins) / sizeof(k_epd_bus_pins[0]); ++i) {
        gpio_hold_dis(k_epd_bus_pins[i]);
    }
    MetalioSd_ReleaseHold();
    gpio_hold_dis(CFG_SDA);
    gpio_hold_dis(CFG_SCL);
    gpio_hold_dis(FL_COOL_PIN);
    gpio_hold_dis(FL_WARM_PIN);
    s_lcd_bus_held = false;
    ESP_LOGI(TAG, "boot: released EPD/SD/I2C/FL gpio holds");
}

void epd_board_metalio_eink4_plus_ioexp_mask_for_sleep(void) {
    if (!s_ioexp_ok) {
        return;
    }
    // Port0：TPS 三位输出低；Motor/PA/P5 保持当前锁存。
    //
    // P0.7 (PWR_GOOD) STAYS AN INPUT. Port0 used to be switched to `0x00` here, i.e.
    // every pin an output including P0.7 -- while P0.7 is *driven by the TPS65185*
    // (config.h:68 `EPD_TPS_IO_PWR_GOOD 7`; read through `readPowerGood()` and the
    // 200 ms x 3 gate below). Driving our low against that driver is a sustained
    // output-vs-output contention on the one pin every rail verdict depends on, and
    // it also makes our own input-port read of that bit meaningless (the TCA reports
    // the pin level we are forcing). `CFG_P0_INPUT_MASK` is the direction map the
    // rest of the file already uses (:579 restore, :665 init) and the map the vendor
    // ships (`BringupExt.cpp:604-607`: port0 outputs = `~0x3F` = `0xC0`), so park and
    // resume now agree instead of park being an anomaly.
    //
    // Consequence, stated where it will be read: P0.7 as an input means a charger
    // PWR_GOOD transition can assert the expander INT during standby. There is no
    // per-pin interrupt mask on a TCA9555, so the old `0x00` was the only way to keep
    // charger events out of the wake path. What pays for it now is the filter that
    // already exists: `metalioVerifyStandbyWake()` rejects any wake no press explains
    // and `discardNonPowerWakes()` covers the boot case. The cost is one extra
    // wake+sleep per charger event, not a lost wake -- the POWER latch is untouched.
    config_reg.pwrup = false;
    config_reg.vcom_ctrl = false;
    config_reg.wakeup = false;
    const uint8_t p0_out = p0_output_value();
    pca9555_set_value(config_reg.port, p0_out, CFG_PORT_CTRL);
    pca9555_set_config(config_reg.port, CFG_P0_INPUT_MASK, CFG_PORT_CTRL);

    // Port1：P12/P14 锁存保留；音量/电源脚驱空闲高（勿驱低，醒后改输入会假出按下→松开）；P13 低
    const uint8_t rail = (uint8_t)(s_p1_out & CFG_P1_RAIL_MASK);
    const uint8_t keys_idle = (uint8_t)(CFG_IO_BIT(EPD_IO_VOL_UP) | CFG_IO_BIT(EPD_IO_VOL_DOWN) |
                                        CFG_IO_BIT(EPD_IO_POWER));
    s_p1_out = (uint8_t)((rail | keys_idle) & (uint8_t)~CFG_PIN_PWR_PULSE);
    pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    pca9555_set_config(config_reg.port, 0x00, CFG_PORT_INT);

    uint8_t p0 = 0;
    uint8_t p1 = 0;
    pca9555_read_input_ex(config_reg.port, CFG_PORT_CTRL, &p0);
    pca9555_read_input_ex(config_reg.port, CFG_PORT_INT, &p1);
    ESP_LOGI(TAG, "sleep mask all-out TPS=low keys=idle-high INT_GPIO=%d P0=0x%02X P1=0x%02X",
             gpio_get_level(CFG_INTR), p0, p1);
}

bool epd_board_metalio_eink4_plus_ioexp_enable_power_wake_pin(void) {
    if (!s_ioexp_ok) {
        return false;
    }
    // 顺序取自 vendor v2.0.74（ioexp_enable_power_wake_pin）：改方向 -> 等稳定 -> 重新采样
    // -> 按电平判断键是否还按着。
    //
    // 为什么必须先改方向再采样：改方向（输出高 -> 输入）本身会在 TCA 上产生一次假的 INT
    // 边沿，而这一次读输入口就是消掉它的地方（读 OUT 口不清 INT）。在改方向之前采样看到的
    // 只是“输出锁存是高”，既不是键的真实状态，也清不掉这个边沿。
    //
    // 为什么要返回电平：旧写法无条件 return true，调用方无从知道“键还按着”。带着按下的键
    // 入睡，EXT1 会立刻被拉低，醒来的那一刻和一次真实按键完全无法区分——这正是偶发假唤醒
    // / 双唤醒的来源。true = 已松开（高），可以睡；false = 还按着（低）或读失败。
    if (pca9555_set_config(config_reg.port, CFG_IO_BIT(EPD_IO_POWER), CFG_PORT_INT) != ESP_OK) {
        ESP_LOGW(TAG, "enable P17 input fail");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t p1 = 0xFF;
    if (pca9555_read_input_ex(config_reg.port, CFG_PORT_INT, &p1) != ESP_OK) {
        ESP_LOGW(TAG, "enable P17 resample fail");
        return false;
    }
    return (p1 & CFG_IO_BIT(EPD_IO_POWER)) != 0;
}

void epd_board_metalio_eink4_plus_ioexp_restore_keys(void) {
    if (!s_ioexp_ok) {
        return;
    }
    pca9555_set_value(config_reg.port, p0_output_value(), CFG_PORT_CTRL);
    pca9555_set_config(config_reg.port, CFG_P0_INPUT_MASK, CFG_PORT_CTRL);

    // 保留 P12/P14 锁存；清关机脉冲；按键脚改回输入（P16 仍为 RTC_INT 输入）
    s_p1_out = (uint8_t)((s_p1_out & CFG_P1_RAIL_MASK) & (uint8_t)~CFG_PIN_PWR_PULSE);
    pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    pca9555_set_config(config_reg.port, CFG_P1_INPUT_MASK, CFG_PORT_INT);

    uint8_t p0 = 0;
    uint8_t p1 = 0;
    pca9555_read_input_ex(config_reg.port, CFG_PORT_CTRL, &p0);
    pca9555_read_input_ex(config_reg.port, CFG_PORT_INT, &p1);
    ESP_LOGI(TAG, "keys restore P0=0x%02X P1=0x%02X INT_GPIO=%d", p0, p1, gpio_get_level(CFG_INTR));
}

static void p1_pwr_pulse_set(bool high) {
    if (high) {
        s_p1_out |= CFG_PIN_PWR_PULSE;
    } else {
        s_p1_out &= (uint8_t)~CFG_PIN_PWR_PULSE;
    }
    p1_write();
}

void epd_board_metalio_eink4_plus_pwr_key_pulse_train(void) {
    if (!s_ioexp_ok) {
        ESP_LOGW(TAG, "pwr pulse skipped (no TCA)");
        return;
    }
    ESP_LOGI(TAG, "pwr pulse train P1%d(io=%d) x%d interval=%dms", EPD_IO_PWR_PULSE & 7,
             EPD_IO_PWR_PULSE, PWR_PULSE_COUNT, PWR_PULSE_INTERVAL_MS);
    for (int i = 0; i < PWR_PULSE_COUNT; ++i) {
        p1_pwr_pulse_set(true);
        vTaskDelay(pdMS_TO_TICKS(PWR_PULSE_INTERVAL_MS));
        p1_pwr_pulse_set(false);
        vTaskDelay(pdMS_TO_TICKS(PWR_PULSE_INTERVAL_MS));
    }
}

void epd_board_metalio_eink4_plus_tp_reset(void) {
    if (!s_ioexp_ok) {
        ESP_LOGW(TAG, "TP RST skipped (no TCA)");
        vTaskDelay(pdMS_TO_TICKS(300));
        return;
    }
    SemaphoreHandle_t mu = shared_reset_mutex();
    xSemaphoreTake(mu, portMAX_DELAY);
    s_p1_out &= (uint8_t)~CFG_PIN_TP_RST;
    pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    vTaskDelay(pdMS_TO_TICKS(10)); // Trst ≥ 5ms
    s_p1_out |= CFG_PIN_TP_RST;
    pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    vTaskDelay(pdMS_TO_TICKS(300)); // Trsi ≥ 300ms
    xSemaphoreGive(mu);
    ESP_LOGI(TAG, "TP RST pulse on TCA P1%d (io=%d)", EPD_IO_TP_RST & 7, EPD_IO_TP_RST);
}

static esp_err_t i2c_probe_addr(i2c_port_t port, uint8_t addr_7bit) {
    if (!metalio_i2c1_lock(50)) return ESP_ERR_TIMEOUT;
    const esp_err_t ret = metalio_i2c1_probe_locked(port, addr_7bit, 50);
    metalio_i2c1_unlock();
    return ret;
}

static void i2c_scan_log(i2c_port_t port) {
    // 只 probe：对每个 ACK 地址盲读 0x08/0x0F 会拖死 BQ27220 并触发 clear bus error
    ESP_LOGI(TAG, "I2C scan SDA=%d SCL=%d (probe only):", (int)CFG_SDA, (int)CFG_SCL);
    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
        if (i2c_probe_addr(port, addr) != ESP_OK) {
            continue;
        }
        ESP_LOGI(TAG, "  found 0x%02X", addr);
    }
}

/** @return true 找到并配置好扩展器 */
static bool probe_and_init_ioexp(i2c_port_t port) {
    static const uint8_t k_candidates[] = {0x20, 0x24, 0x21, 0x22, 0x23, 0x25, 0x26, 0x27};
    for (size_t i = 0; i < sizeof(k_candidates); ++i) {
        uint8_t addr = k_candidates[i];
        if (i2c_probe_addr(port, addr) != ESP_OK) {
            continue;
        }
        pca9555_set_addr(addr);
        ESP_LOGI(TAG, "TCA/PCA9555 ACK at 0x%02X", addr);

        /* Port0：VCOM/PWRUP/WAKEUP/Motor/PA/P05 输出，其余输入（含 PWR_GOOD） */
        if (pca9555_set_config(port, CFG_P0_INPUT_MASK, CFG_PORT_CTRL) != ESP_OK) {
            ESP_LOGW(TAG, "set_config port0 fail at 0x%02X", addr);
            continue;
        }
        s_cam_scr_on = CFG_PIN_CAM_SCR;
        s_motor_on = 0;
        s_pa_on = 0;
        if (pca9555_set_value(port, p0_output_value(), CFG_PORT_CTRL) != ESP_OK) {
            continue;
        }
        /* Port1：P12/P14 输出拉高、关机脉冲默认低；P16=RTC_INT 输入 */
        s_p1_out = (uint8_t)(CFG_PIN_TP_RST | CFG_PIN_BT_PA_PWR);
        if (pca9555_set_value(port, s_p1_out, CFG_PORT_INT) != ESP_OK) {
            continue;
        }
        if (pca9555_set_config(port, CFG_P1_INPUT_MASK, CFG_PORT_INT) != ESP_OK) {
            continue;
        }
        ESP_LOGI(TAG,
                 "TPS↔TCA: VCOM=P%d PWRUP=P%d WAKE=P%d PG=P%d | P5/P12/P14 out P16=RTC_IN | P0=0x%02X",
                 EPD_TPS_IO_VCOM_CTRL, EPD_TPS_IO_PWRUP, EPD_TPS_IO_WAKEUP, EPD_TPS_IO_PWR_GOOD,
                 CFG_P0_OUTPUT_MASK);
        return true;
    }
    return false;
}

static void epd_board_init(uint32_t epd_row_width) {
    (void)epd_row_width;
    epd_board_metalio_eink4_plus_boot_release_holds();

    esp_err_t err = metalio_i2c1_init(CFG_SDA, CFG_SCL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new I2C1 init: %s", esp_err_to_name(err));
        return;
    }

    config_reg.port = EPDIY_I2C_PORT;
    config_reg.pwrup = false;
    config_reg.vcom_ctrl = false;
    config_reg.wakeup = false;

    s_ioexp_ok = probe_and_init_ioexp(config_reg.port);
    // 深睡曾关 P14(VOUT)：TCA 锁存仍低，上面已拉高；等轨稳定再启 TPS
    if (s_ioexp_ok) {
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    epd_board_metalio_eink4_plus_ana_sw_set(true); // GPIO36 默认高（非虚拟 U 盘）
    i2c_scan_log(config_reg.port); // 启动即扫；此时 TPS 多半仍 SLEEP，可能只有 TCA
    if (!s_ioexp_ok) {
        ESP_LOGE(TAG, "TCA9555 not found; continue (panel power unavailable)");
    }

    gpio_reset_pin(CFG_INTR);
    gpio_set_direction(CFG_INTR, GPIO_MODE_INPUT);
    gpio_pullup_en(CFG_INTR);
    gpio_pulldown_dis(CFG_INTR);
    gpio_set_intr_type(CFG_INTR, GPIO_INTR_NEGEDGE);
    if (s_ioexp_irq_sem == NULL) {
        s_ioexp_irq_sem = xSemaphoreCreateBinary();
    }
    esp_err_t isr = gpio_install_isr_service(ESP_INTR_FLAG_EDGE);
    if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service: %s", esp_err_to_name(isr));
    } else {
        err = gpio_isr_handler_add(CFG_INTR, interrupt_handler, (void*)CFG_INTR);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "gpio_isr_handler_add: %s", esp_err_to_name(err));
        }
    }
    // 读口清 TCA INT，避免上电残留一直拉低
    if (s_ioexp_ok) {
        pca9555_read_input(config_reg.port, CFG_PORT_CTRL);
        pca9555_read_input(config_reg.port, CFG_PORT_INT);
    }

    epd_ctrl_gpios_out_low();
    epd_lcd_reinit();
    s_lcd_bus_held = false;
    ESP_LOGI(TAG, "init ok %dx%d bus=%d pclk=%dMHz I2C SDA=%d SCL=%d ioexp=%s addr=0x%02X",
             epd_get_display()->width, epd_get_display()->height, epd_get_display()->bus_width,
             epd_get_display()->bus_speed, CFG_SDA, CFG_SCL, s_ioexp_ok ? "ok" : "FAIL",
             pca9555_get_addr());
}

static void epd_board_deinit(void) {
    gpio_set_level(EPD_OE, 0);
    gpio_set_level(EPD_MODE, 0);
    gpio_set_direction(EPD_OE, GPIO_MODE_INPUT);
    gpio_set_direction(EPD_MODE, GPIO_MODE_INPUT);

    epd_lcd_deinit();

    if (s_ioexp_ok) {
        s_motor_on = 0;
        pca9555_set_config(config_reg.port, 0xFF, CFG_PORT_CTRL);
        pca9555_set_config(config_reg.port, 0xFF, CFG_PORT_INT);

        int tries = 0;
        while ((pca9555_read_input(config_reg.port, CFG_PORT_CTRL) & CFG_PIN_PWRGOOD) != 0) {
            if (tries >= 50) {
                ESP_LOGE(TAG, "TPS65185 shutdown timeout");
                break;
            }
            tries++;
            vTaskDelay(1);
        }

        vTaskDelay(50);
        pca9555_read_input(config_reg.port, CFG_PORT_CTRL);
        pca9555_read_input(config_reg.port, CFG_PORT_INT);
    }
    const esp_err_t i2c_err = metalio_i2c1_deinit();
    if (i2c_err != ESP_OK) ESP_LOGW(TAG, "new I2C1 deinit: %s", esp_err_to_name(i2c_err));
    gpio_isr_handler_remove(CFG_INTR);
}

static void epd_board_set_ctrl(epd_ctrl_state_t* state, const epd_ctrl_state_t* const mask) {
    if (mask->ep_output_enable) {
        gpio_set_level(EPD_OE, state->ep_output_enable ? 1 : 0);
    }
    if (mask->ep_mode) {
        gpio_set_level(EPD_MODE, state->ep_mode ? 1 : 0);
    }

    if (!s_ioexp_ok) {
        return;
    }
    if (mask->ep_output_enable || mask->ep_mode || mask->ep_stv) {
        esp_err_t err = pca9555_set_value(config_reg.port, p0_output_value(), CFG_PORT_CTRL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "pca9555_set_value: %s", esp_err_to_name(err));
        }
    }
}

/**
 * @brief 上电：PWRUP 保持低、WAKEUP 高进 STANDBY；读 REVID → 写 UPSEQ0 → 写 UPSEQ1；
 *        配置成功后拉高 PWRUP（上升沿启动升压）。GPIO PG 后再写 ENABLE/VCOM。
 */
static void epd_board_poweron_once(epd_ctrl_state_t* state) {
    s_tps_ok = false;
    if (!s_ioexp_ok) {
        ESP_LOGE(TAG, "poweron skipped: no IO expander");
        return;
    }
    epd_board_metalio_eink4_plus_bus_restore();
    // Restore the schematic's original VBORDER bias.
    gpio_set_level(EPD_BORDER, 0);
    epd_ctrl_state_t mask = {
        .ep_output_enable = true,
        .ep_mode = true,
        .ep_stv = true,
    };

    state->ep_output_enable = false;
    state->ep_stv = true;
    state->ep_mode = false;

    // 1) PWRUP low + WAKEUP high -> STANDBY, I2C usable.
    config_reg.wakeup = true;
    config_reg.pwrup = false;
    config_reg.vcom_ctrl = false;
    epd_board_set_ctrl(state, &mask);
#if CP_TPS_FAST_SEQUENCE
    // r85: no 50 ms here. RickyOS writes REVID/UPSEQ immediately after clearing
    // the control bit (RE_RICKYOS_POWER_TIMING.md §2.1 steps 3-6, §3 row 1) and
    // treats a NAK from a cold part as non-fatal; the writes below already warn
    // and continue, so this settle only adds latency to a sequence that has a
    // real gate further down (the PWR_GOOD poll). If UPSEQ ever starts NAKing on
    // glass, a 2 ms settle is the smallest thing that fixes it -- 50 ms bought
    // nothing the poll does not already wait for.
#else
    vTaskDelay(pdMS_TO_TICKS(50));
#endif

    // VENDOR SEMANTICS, restored verbatim in intent: the TPS probe and every
    // register read below are DIAGNOSTIC. A failed read must never abandon the
    // power-up -- the vendor retries the probe 3x and then raises PWRUP regardless
    // ("失败也继续" / "ok or not"). Aborting here turns one I2C hiccup into a blank
    // panel with a perfectly healthy app behind it.
    esp_err_t probe = ESP_FAIL;
    for (int i = 0; i < 3 && probe != ESP_OK; ++i) {
        probe = tps_probe(config_reg.port);
        if (probe != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // 2) Read the revision (logged only).
    uint8_t revid = 0;
    esp_err_t revid_err = tps_read_register_ex(config_reg.port, TPS_REG_REVID, &revid);
    ESP_LOGI(TAG, "STANDBY REVID(%s)=0x%02X probe=%s", esp_err_to_name(revid_err), revid,
             esp_err_to_name(probe));

    // 3) Boost timing registers -- failure is not fatal, vendor continues.
    esp_err_t up0 = tps_write_register(config_reg.port, TPS_REG_UPSEQ0, 0xE4);
    esp_err_t up1 = tps_write_register(config_reg.port, TPS_REG_UPSEQ1, 0x55);
#if CP_TPS_FAST_SEQUENCE
    // r85: with no settle in front of these writes a cold part may NAK them.
    // That is exactly RickyOS's contract -- its "UPSEQ write %s/%s probe=%s" line
    // is a WARNING (0x4051d304) and it raises PWRUP regardless. Warn and continue;
    // a missing UPSEQ is a slow/soft boost ramp, not a dead panel.
    if (up0 != ESP_OK || up1 != ESP_OK) {
        ESP_LOGW(TAG, "UPSEQ write %s/%s (NAK on a cold part, continuing)",
                 esp_err_to_name(up0), esp_err_to_name(up1));
    } else {
        ESP_LOGI(TAG, "UPSEQ0 write %s; UPSEQ1 write %s", esp_err_to_name(up0),
                 esp_err_to_name(up1));
    }
#else
    ESP_LOGI(TAG, "UPSEQ0 write %s; UPSEQ1 write %s", esp_err_to_name(up0),
             esp_err_to_name(up1));
#endif

    uint8_t int2_pre = 0;
    uint8_t pg_pre = 0;
    esp_err_t e_int2_pre = tps_read_register_ex(config_reg.port, TPS_REG_INT2, &int2_pre);
    esp_err_t e_pg_pre = tps_read_register_ex(config_reg.port, TPS_REG_PG, &pg_pre);
    ESP_LOGI(TAG, "before PWRUP: INT2(%s)=0x%02X PG(%s)=0x%02X", esp_err_to_name(e_int2_pre),
             int2_pre, esp_err_to_name(e_pg_pre), pg_pre);

    // 4) Raise PWRUP whether or not the registers above answered.
    config_reg.pwrup = true;
    config_reg.vcom_ctrl = true;
    epd_board_set_ctrl(state, &mask);
    ESP_LOGI(TAG, "PWRUP high (after UPSEQ, ok or not)");
#if CP_TPS_FAST_SEQUENCE
    // r85: no 50 ms head start. The 200 x 1 ms PWR_GOOD poll below is the real
    // gate (RE_RICKYOS_POWER_TIMING.md §3 row 2): RickyOS's next step after
    // asserting PWRUP is delay(1) inside its own poll. Its own log prints the
    // wait as "@%dms" further down, so a value >50 on glass would say this settle
    // was pure latency.
#else
    vTaskDelay(pdMS_TO_TICKS(50));
#endif

    uint8_t int2_post = 0;
    uint8_t pg_post = 0;
    esp_err_t e_int2_post = tps_read_register_ex(config_reg.port, TPS_REG_INT2, &int2_post);
    esp_err_t e_pg_post = tps_read_register_ex(config_reg.port, TPS_REG_PG, &pg_post);
    const char* post_label =
#if CP_TPS_FAST_SEQUENCE
        "after PWRUP (no settle)";
#else
        "after PWRUP+50ms";
#endif
    ESP_LOGI(TAG, "%s: INT2(%s)=0x%02X PG(%s)=0x%02X", post_label, esp_err_to_name(e_int2_post),
             int2_post, esp_err_to_name(e_pg_post), pg_post);

    // 5) Wait for PWR_GOOD, with the vendor's 3 attempts and PWRUP toggling.
    bool success = false;
    for (int retry = 0; retry < 3 && !success; ++retry) {
        if (retry > 0) {
            ESP_LOGW(TAG, "TPS startup retry %d/3", retry + 1);
            config_reg.pwrup = false;
            epd_board_set_ctrl(state, &mask);
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        config_reg.pwrup = true;
        config_reg.vcom_ctrl = true;
        epd_board_set_ctrl(state, &mask);

        for (int wait_ms = 0; wait_ms < 200; ++wait_ms) {
            uint8_t p0 = pca9555_read_input(config_reg.port, CFG_PORT_CTRL);
            if (p0 & CFG_PIN_PWRGOOD) {
                ESP_LOGI(TAG, "PWR_GOOD ok p0=0x%02X @%dms", p0, wait_ms);
                success = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    if (!success) {
        // Vendor behaviour: hold WAKE+PWRUP+VCOM high for bench measurement.
        config_reg.wakeup = true;
        config_reg.pwrup = true;
        config_reg.vcom_ctrl = true;
        epd_board_set_ctrl(state, &mask);
        s_tps_ok = false;
        ESP_LOGE(TAG, "TPS PowerOn failed: no PWR_GOOD after 3 attempts");
        return;
    }

    // 6) Enable the rails and set VCOM (single vendor call).
    if (probe == ESP_OK) {
        esp_err_t err = tps_write_register(config_reg.port, TPS_REG_ENABLE, 0x3F);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tps ENABLE write: %s", esp_err_to_name(err));
            s_tps_ok = false;
            return;
        }
        tps_set_vcom(config_reg.port, vcom);
    }

    state->ep_sth = true;
    epd_ctrl_state_t sth_mask = {.ep_sth = true};
    epd_board_set_ctrl(state, &sth_mask);

    int tries = 0;
    while (probe == ESP_OK && !((tps_read_register(config_reg.port, TPS_REG_PG) & 0xFA) == 0xFA)) {
        if (tries >= 200) {
            ESP_LOGE(TAG, "TPS PG register timeout");
            s_tps_ok = false;
            return;
        }
        tries++;
        vTaskDelay(1);
    }

    state->ep_output_enable = true;
    epd_ctrl_state_t out_mask = {.ep_output_enable = true};
    epd_board_set_ctrl(state, &out_mask);
    s_tps_ok = true;
    ESP_LOGI(TAG, "TPS PowerOn ok vcom=%d mV", vcom);
}


static void epd_board_poweroff(epd_ctrl_state_t* state);

// r85 FAILURE CONTRACT — latch, do not reboot.
//
// r84 ended this ladder with esp_restart(). That was survivable while the only
// power-up ran once at boot; with per-drive cycling (CP_EPD_POWERDOWN_AFTER_DRIVE
// = 1) a marginal rail would turn a slow page into a reboot loop.
//
// The contract is RickyOS's (RE_RICKYOS_SLEEP_WAKE.md §4, RE_RICKYOS_POWER_TIMING.md
// §5 last row): a failed power-up leaves the panel marked NOT READY (s_tps_ok =
// false, which the panel layer reads through
// epd_board_metalio_eink4_plus_tps_ok() and which now also clears its own
// g_powered gate in metalio_vendor_panel.cpp), logs ONE E line, and returns. The
// NEXT paint calls epd_board_poweron() again and gets a fresh set of attempts.
// Nothing else in the ladder changes: still 3 attempts, still 100 ms apart.
static bool s_poweron_failed_logged = false;

static void epd_board_poweron(epd_ctrl_state_t* state) {
    if (!s_ioexp_ok) {
        s_tps_ok = false;
        ESP_LOGE(TAG, "poweron skipped: no IO expander");
        return;
    }
    // Initial attempt plus at most three complete power-cycle retries.
    for (int attempt = 0; attempt <= 3; ++attempt) {
        epd_board_poweron_once(state);
        if (s_tps_ok) {
            s_poweron_failed_logged = false;
            return;
        }
        epd_board_poweroff(state);
        if (attempt < 3) {
            ESP_LOGW(TAG, "TPS power-on retry %d/3", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    // Latched failure: rails down, panel not ready, no reboot. The E line fires
    // once per failure episode so a repeatedly failing rail cannot flood the log;
    // subsequent paints log at W until a power-up succeeds.
    s_tps_ok = false;
    if (!s_poweron_failed_logged) {
        s_poweron_failed_logged = true;
        ESP_LOGE(TAG, "TPS power-on failed after 3 retries; rails down, panel not ready, next paint retries");
    } else {
        ESP_LOGW(TAG, "TPS power-on still failing; panel not ready, next paint retries");
    }
}

static void epd_board_measure_vcom(epd_ctrl_state_t* state) {
    (void)state;
    ESP_LOGW(TAG, "measure_vcom not implemented");
}

static void epd_board_poweroff(epd_ctrl_state_t* state) {
    epd_ctrl_state_t mask = {
        .ep_stv = true,
        .ep_output_enable = true,
        .ep_mode = true,
    };
    state->ep_stv = false;
    state->ep_output_enable = false;
    state->ep_mode = false;
    epd_board_set_ctrl(state, &mask);
    // E0470A14-AF 6.1: XOE LOW must precede VPOS/VNEG OFF by >=12us.
    esp_rom_delay_us(12);
    config_reg.vcom_ctrl = false;
    config_reg.pwrup = false;
    epd_board_set_ctrl(state, &mask);
    vTaskDelay(1);
    config_reg.wakeup = false;
    epd_board_set_ctrl(state, &mask);
}

static float epd_board_ambient_temperature(void) {
    return 25.0f;
}

static void set_vcom(int value) {
    vcom = value;
}

const EpdBoardDefinition epd_board_metalio_eink4_plus = {
    .init = epd_board_init,
    .deinit = epd_board_deinit,
    .set_ctrl = epd_board_set_ctrl,
    .poweron = epd_board_poweron,
    .poweroff = epd_board_poweroff,
    .measure_vcom = epd_board_measure_vcom,
    .get_temperature = epd_board_ambient_temperature,
    .set_vcom = set_vcom,
    .gpio_set_direction = NULL,
    .gpio_read = NULL,
    .gpio_write = NULL,
};
