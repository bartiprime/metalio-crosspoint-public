#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x;     // 逻辑竖屏 X
    int y;     // 逻辑竖屏 Y
    int raw_x; // 芯片原始 X
    int raw_y; // 芯片原始 Y
} metalio_touch_point_t;

typedef struct {
    bool ok;
    uint32_t samples, read_errors, irqs;
    int last_error, raw_x, raw_y, irq_level, pause_depth;
} metalio_touch_stats_t;
metalio_touch_stats_t metalio_touch_stats(void);

/* TCH5a: identification registers (0xA3 chip id, 0xA6 firmware version).
 *
 * metalio_touch_id() is the CACHE: the last values that were actually read from
 * the part this boot (filled by init and by the first live read). It is a
 * fallback for log lines -- never treat it as evidence about the controller's
 * current state, because it only says "somebody read it once".
 *
 * metalio_touch_read_id_live() performs the I2C transaction ON DEMAND, under the
 * shared I2C1 bus lock (the same one the touch feed task takes), and refreshes
 * the cache on success. Use it for any claim about the part -- e.g. whether the
 * vendor V04 firmware update actually applied.
 */
#define METALIO_TOUCH_FT6336_CHIP_ID 0x64

typedef struct {
    bool valid;   /* the chip-id register was read at least once this boot */
    uint8_t chip; /* 0xA3 value (0x64 = FT6336U) */
    uint8_t fw;   /* 0xA6 value, 0 if that read failed */
} metalio_touch_id_t;
metalio_touch_id_t metalio_touch_id(void); /* cached -- see above */

typedef struct {
    bool ok;       /* both registers answered THIS call */
    uint8_t chip;  /* 0xA3 value, 0 when the read failed */
    uint8_t fw;    /* 0xA6 value, 0 when the read failed */
    int chip_err;  /* esp_err_t of the 0xA3 read (0 = ok) -- why it failed if !ok */
    int fw_err;    /* esp_err_t of the 0xA6 read (0 = ok); 0 if 0xA3 failed first */
} metalio_touch_id_live_t;

/**
 * @brief Live I2C read of 0xA3/0xA6. Takes the I2C1 bus lock for the transaction
 *        (no reset, no register write, no state left changed). One retry ~2 ms
 *        later on a chip-id NACK. Never called from the touch hot path.
 * @note  On success the cached statics are refreshed, so the deferred
 *        "[TOUCH] fw=... (post-init)" line agrees with what was just printed.
 */
metalio_touch_id_live_t metalio_touch_read_id_live(void);

/**
 * @brief 采样回调：count=0 抬起；>0 按下（raw/logical 有效）
 * @note 在 touch_feed 任务调用，可做盖板键；勿长时间占 I2C/LVGL
 */
typedef void (*metalio_touch_sample_cb_t)(uint8_t count, int raw_x, int raw_y, int lx, int ly);

/**
 * @brief 复位并探测 FT6336U；GPIO5 下降沿中断 + 独立 feed 任务
 * @return true 芯片有应答
 */
bool metalio_touch_init(void);

/**
 * @brief 注册采样回调（可空）；盖板键 / 早震等挂这里
 */
void metalio_touch_set_sample_cb(metalio_touch_sample_cb_t cb);

/**
 * @brief 读屏内触点（无 I2C；由 feed 更新）
 * @return true 应报 PRESSED（含刷屏期间错过的短按回放）
 * @note 短按若整段落在 LVGL 未轮询窗口，会先回放一次 PRESSED，再 RELEASED
 */
bool metalio_touch_read(metalio_touch_point_t* out);

/**
 * @brief 是否还有未交付的 down/up（含短按回放的待 RELEASED）
 */
bool metalio_touch_has_pending_edges(void);

/**
 * @brief 供 indev：标记盖板键占用，屏内点不喂 LVGL
 */
void metalio_touch_set_ui_suppress(bool suppress);

/**
 * @brief 暂停/恢复 touch_feed（待机 Park/GC16 与浅睡前；对齐 397 PauseStandbyBackgroundWork）
 * @note 保留已采样的 press/release；恢复后继续交付，避免刷屏吞掉点击
 */
void metalio_touch_set_feed_paused(bool paused);

/**
 * @brief 触控芯片是否已探测成功
 */
bool metalio_touch_ok(void);

#ifdef __cplusplus
}
#endif
