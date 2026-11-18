#pragma once

#include "epd_board.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Metalio E-Ink4-Plus 板定义（并行 ED047TC2） */
extern const EpdBoardDefinition epd_board_metalio_eink4_plus;

/**
 * @brief 最近一次 TPS65185 poweron 是否成功
 */
bool epd_board_metalio_eink4_plus_tps_ok(void);

/**
 * @brief TCA9555 是否已探测并配置成功
 */
bool epd_board_metalio_eink4_plus_ioexp_ok(void);

/**
 * @brief 脉冲 FT6336U RST（TCA P12），低 ≥5ms 后拉高并等待 300ms
 */
void epd_board_metalio_eink4_plus_tp_reset(void);

/** Lease shared P12 against touch resets; verify P5/P12 are already high.
 * Does not write TCA outputs or take ownership of the shared power rail.
 * A successful acquire must be paired with release on the same task. */
esp_err_t epd_board_metalio_eink4_plus_camera_acquire(uint32_t timeout_ms);
void epd_board_metalio_eink4_plus_camera_release(void);

/**
 * @brief 震动马达输出（TCA P0.3），高有效；与 TPS 控制脚同口锁存
 * @param on true 开，false 关
 */
void epd_board_metalio_eink4_plus_motor_set(bool on);

/**
 * @brief 功放使能（TCA P0.4=PA_EN），高有效；与 TPS 控制脚同口锁存
 * @param on true 开，false 关
 */
void epd_board_metalio_eink4_plus_pa_set(bool on);
/** @brief P14 共享电源拉低 3 秒后拉高；调用方须阻止待机。 */
esp_err_t epd_board_metalio_eink4_plus_bt_power_reset(void);

/**
 * @brief 模拟开关（ESP GPIO36=ANA_SW）；虚拟 U 盘时置低，关闭后置高
 * @param high true 置高，false 置低
 */
void epd_board_metalio_eink4_plus_ana_sw_set(bool high);

/**
 * @brief 入睡关轨：P5 摄像屏电 → P14 VOUT（最后关）
 */
void epd_board_metalio_eink4_plus_sleep_cut_rails(void);

/**
 * @brief 浅睡醒后恢复 P14 → POWER_RAIL_SETTLE_MS（锁放开）→ 复查 → P5
 * @note 没有调用者（本树的待机退出走 pa_rail_set(true) + 80 ms，见 src/main.cpp）。
 *       4b（P5 之后的第二个 settle）按 operator 决定不取：本板无蜂窝模块。
 */
void epd_board_metalio_eink4_plus_sleep_restore_rails(void);

/**
 * @brief 单独控制 TCA P0.5 (CAM_SCR_EN) —— 不触碰 P1.4 / P1.2
 *
 * 待机省电的 P0.5 单轨开关（CP_CUT_CAM_RAIL_ON_STANDBY）。与 pa_rail_set() 对称：
 * 只改 Port0 的 P05 位，其余位由 p0_output_value() 从各自的锁存重建（马达、PA_EN、
 * TPS 三位都不动）。待机 park 时先切 P1.4 再切 P0.5；醒来先 P1.4 高、稳定 80 ms、
 * 再 P0.5 高。
 *
 * 必须走驱动里的锁存 s_cam_scr_on，不能在 src/ 里裸写 Port0：epd_poweron()、
 * ioexp_mask_for_sleep()（每个待机切片一次）和 ioexp_restore_keys() 都会用
 * p0_output_value() 重写 Port0，绕过锁存清掉的位会在第一个切片里被重新拉高。
 * @param on true 上电（P0.5 高），false 断电（P0.5 低）
 * @return true 已处于目标状态（含无需写入）；false TCA 未就绪或写入失败
 */
bool epd_board_metalio_eink4_plus_cam_rail_set(bool on);

/**
 * @brief 单轨 TCA P1.4 (BT_PA_PWR) —— 不触碰 P0.5
 *
 * 待机省电的 P14 单轨开关（CP_CUT_PA_RAIL_ON_STANDBY）。只在整机已按
 * “面板停 -> TPS SLEEP -> EPD 总线置低” 的顺序 park 之后拉低，醒来必须
 * 在 epd_poweron() 之前拉高并让其稳定 ≥80 ms。
 * @param on true 上电（P1.4 高），false 断电（P1.4 低）
 * @return true 已处于目标状态（含无需写入）；false TCA 未就绪或写入失败
 */
bool epd_board_metalio_eink4_plus_pa_rail_set(bool on);

/**
 * @brief 读 TCA 扩展脚输入电平（0..15；高口=io>>3，位=io&7）
 * @param io 扩展脚号（如 EPD_IO_VOL_UP）
 * @return true 高电平；TCA 未就绪或读失败时保持上次成功值（默认高）
 */
bool epd_board_metalio_eink4_plus_ioexp_get_level(int io);

/**
 * @brief 读 TCA 一整口输入（失败返回 false，*out 不变）
 */
bool epd_board_metalio_eink4_plus_ioexp_read_port(int port, uint8_t* out);

/**
 * @brief r106 `ioexp`: 一次读回 TCA9555 的四个口字节 + 本层的 Port0/Port1 锁存
 *
 * 为什么需要这个命令：B1 修的是“单轨 cut 只改一位”，而证明它真的只改了一位，
 * 必须看到 cut 前后的 OUTPUT 口（寄存器 0x02/0x03）——输入口（0x00/0x01）反映的是
 * 引脚电平，不是我们要写入的字节。四个读各自带 ok 标志：读失败与读到 0 必须可区分。
 */
typedef struct {
    uint8_t in0, in1;        /* 寄存器 0x00 / 0x01：引脚电平 */
    uint8_t out0, out1;      /* 寄存器 0x02 / 0x03：输出锁存（B1 读改写的那两个字节） */
    bool in0_ok, in1_ok, out0_ok, out1_ok;
    uint8_t latch0, latch1;  /* 本层重建的期望值：p0_output_value() / s_p1_out */
    uint8_t shadow0;         /* B1 的回退影子：最后一次成功读/写的 Port0 输出字节 */
    bool shadow0_valid;
} epd_ioexp_dump_t;

bool epd_board_metalio_eink4_plus_ioexp_dump(epd_ioexp_dump_t* out);

/**
 * @brief 等待 TCA INT（GPIO2 下降沿）；读输入口可清中断
 * @param timeout_ms 超时毫秒，0 表示不等待
 * @return true 等到中断
 */
bool epd_board_metalio_eink4_plus_ioexp_irq_wait(uint32_t timeout_ms);

/**
 * @brief 订阅 GPIO2 下降沿（与 TCA/SC7A20H INT 共用）；ISR 内 TaskNotifyGive
 * @param task 接收通知的任务；传 NULL 取消订阅
 */
void epd_board_metalio_eink4_plus_gpio2_irq_subscribe(TaskHandle_t task);

/**
 * @brief 按手册将 TPS65185 置入 SLEEP：PWRUP/VCOM↓ → WAKEUP↓ → 等待 ≥100ms
 * @note 深睡前调用；SLEEP 后 I2C 无效、寄存器复位
 */
void epd_board_metalio_eink4_plus_tps_enter_sleep(void);

/**
 * @brief 只读探测用的 TPS65185 唤醒闸（见 .c 中的 READ GUARD 注释）
 *
 * SLEEP 状态下 TPS 不应答 I2C（手册 8.4），所以调试读取必须先把它拉到
 * STANDBY（WAKEUP 高、PWRUP/VCOM 低）。只保存/恢复三个控制位，绝不抬高
 * PWRUP —— 唤醒前是睡的就恢复成睡，不会把轨留在开着的状态。
 *
 * @param timeout_ms 轮询 tps_probe() 的最长等待
 * @param was_asleep 输出：调用前是否处于 SLEEP
 * @param waited_ms  输出：实际等待到应答的毫秒数
 * @return true 表示现在可以读寄存器（本来就醒着，或唤醒后应答）
 * @note 成功调用后必须配对 epd_board_metalio_eink4_plus_tps_read_restore()
 */
bool epd_board_metalio_eink4_plus_tps_wake_for_read(uint32_t timeout_ms, bool* was_asleep,
                                                    unsigned* waited_ms);

/**
 * @brief 恢复 epd_board_metalio_eink4_plus_tps_wake_for_read() 之前的上电状态
 */
void epd_board_metalio_eink4_plus_tps_read_restore(void);

/**
 * @brief 入睡前：释放 LCD 外设，屏幕并行通信脚全部输出低
 * @note D0..D7/XCL/XLE/XOE/XSTL/SPV/CKV/MODE/BORDER
 */
void epd_board_metalio_eink4_plus_bus_hold_low(void);

/**
 * @brief 启动/深睡醒后：解除屏总线 gpio_hold（深睡 latch 会跨复位残留）
 */
void epd_board_metalio_eink4_plus_boot_release_holds(void);

/**
 * @brief 入睡前：gpio_hold_en 保持总线脚低电平（深/浅睡）
 */
void epd_board_metalio_eink4_plus_bus_latch_for_sleep(void);

/**
 * @brief 浅睡醒后：解除 hold 并重新 epd_lcd_init
 */
void epd_board_metalio_eink4_plus_bus_restore(void);

/**
 * @brief 入睡前：Port0/Port1 全部改输出；TPS 控制脚置低；音量/电源脚驱空闲高；P17 随后改输入唤醒
 */
void epd_board_metalio_eink4_plus_ioexp_mask_for_sleep(void);

/**
 * @brief 武装唤醒前：仅把电源键 P17 改回输入，并在改方向之后重新采样
 * @return true 改方向成功且 P17 已松开（高）；false 配置/读口失败，或键仍按着（低）
 * @note 改方向会产生一次假 INT 边沿，读输入口即清除；所以“改方向 -> 等 10 ms -> 读口”之后
 *       才能判断电平。调用方拿到 false 时不得入睡：那一下会被 EXT1 当成一次唤醒，
 *       与真实按键无法区分。
 */
bool epd_board_metalio_eink4_plus_ioexp_enable_power_wake_pin(void);

/**
 * @brief 浅睡醒后：恢复 Port0/Port1 默认输入输出（音量键等）
 */
void epd_board_metalio_eink4_plus_ioexp_restore_keys(void);

/**
 * @brief 向开关机芯片发关机脉冲：IO_P13 高/低各 PWR_PULSE_INTERVAL_MS，共 PWR_PULSE_COUNT 次
 */
void epd_board_metalio_eink4_plus_pwr_key_pulse_train(void);

#ifdef __cplusplus
}
#endif
