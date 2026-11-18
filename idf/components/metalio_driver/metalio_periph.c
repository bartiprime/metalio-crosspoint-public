// Board-level peripheral primitives for the Metalio E-Ink 4 Plus.
//
// Every register address and value below is taken from the upstream vendor tree
// (CloudZao/Metalio-E-INK4-Plus), NOT invented:
//   SC7A20H    main/boards/common/sensor/sc7a20h.cc
//   PCF8563    main/boards/common/sensor/pcf8563.cc
//   BQ27220    main/boards/common/power/bq27220_gauge.cc
//   CX25601N   main/boards/common/power/cx25601n.cc
//   PMIC/expander  main/boards/metalio-e-ink4-plus/display/epd_board_metalio_eink4_plus.c
//
// All transfers go through the vendor metalio_i2c1_* API, which is implemented on
// the ONE Arduino Wire master in metalio_vendor_panel.cpp. Every multi-step
// sequence holds the vendor bus mutex (recursive), so it cannot be interleaved by
// an app-side Wire user.
//
// Note: the vendor's metalio_audio.cc depends on the whole vendor app framework
// (board.h / power_policy.h / audio_codec.h) and is NOT transplantable here; this
// file exposes only the rail/PA control the hardware needs, which the board API
// already provides.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"  // esp_rom_delay_us, for the haptic soft-PWM off-time
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bus/metalio_i2c1.h"
#include "bus/pca9555.h"
#include "epd_board_metalio_eink4_plus.h"

static const char* TAG = "metalio_periph";

// ---- SC7A20H (IMU, 0x19) --------------------------------------------------
#define IMU_ADDR            0x19
#define IMU_REG_WHO_AM_I    0x0F
#define IMU_REG_CTRL1       0x20
#define IMU_REG_CTRL2       0x21
#define IMU_REG_CTRL3       0x22
#define IMU_REG_CTRL4       0x23
#define IMU_REG_CTRL5       0x24
#define IMU_REG_CTRL6       0x25
#define IMU_REG_OUT_XL      0x28
#define IMU_REG_AOI1_CFG    0x30
#define IMU_REG_AOI1_SRC    0x31
#define IMU_REG_AOI1_THS    0x32
#define IMU_REG_AOI1_DUR    0x33
#define IMU_AUTO_INC        0x80

// Vendor values: ODR 100 Hz, XYZ on (0x57); HPIS1 (0x01); I1_AOI1 -> INT1 (0x40);
// BDU+HR +-2g (0x88); LIR_INT1 (0x08); INT1 open-drain + active-low (0x03).
#define IMU_CTRL1_VAL 0x57
#define IMU_CTRL2_VAL 0x01
#define IMU_CTRL3_VAL 0x40
#define IMU_CTRL4_VAL 0x88
#define IMU_CTRL5_VAL 0x08
#define IMU_CTRL6_VAL 0x03
// Activity interrupt, vendor values (common/sensor/sc7a20h.cc): THS 0x10 is
// ~256 mg at 16 mg/LSB, DUR 0 (instant), CFG written LAST so the interrupt only
// arms once the threshold is in place.
#define IMU_AOI1_THS_VAL 0x10
#define IMU_AOI1_DUR_VAL 0x00
#define IMU_AOI1_CFG_VAL 0x2A   // XHIE|YHIE|ZHIE, OR-combined
#define IMU_AOI1_IA_MASK 0x40   // AOI1_SRC.IA

// ---- PCF8563 (RTC, 0x51) --------------------------------------------------
#define RTC_ADDR        0x51
#define RTC_REG_CTRL1   0x00
#define RTC_REG_SECONDS 0x02

// ---- BQ27220 (gauge, 0x55) ------------------------------------------------
#define GAUGE_ADDR       0x55
#define GAUGE_REG_VOLT   0x08   // LE, mV
#define GAUGE_REG_CURRENT 0x0C  // LE, mA (signed)

// ---- CX25601N (charger, 0x6B) --------------------------------------------
#define CHG_ADDR        0x6B
#define CHG_REG_STATUS1 0x1E

// Vendor's own SOC mapping bounds (bq27220_gauge.cc): 3.30 V empty, 4.33 V full.
#define BATT_EMPTY_MV 3300
#define BATT_FULL_MV  4330

#define IO_TIMEOUT_MS 200

static bool imu_ok = false;

static esp_err_t rd_reg(uint8_t addr, uint8_t reg, uint8_t* buf, size_t len) {
  if (!metalio_i2c1_lock(IO_TIMEOUT_MS)) return ESP_ERR_TIMEOUT;
  esp_err_t e = metalio_i2c1_read_reg_stop_locked(I2C_NUM_1, addr, reg, buf, len, IO_TIMEOUT_MS);
  metalio_i2c1_unlock();
  return e;
}

static esp_err_t wr_reg(uint8_t addr, uint8_t reg, uint8_t val) {
  if (!metalio_i2c1_lock(IO_TIMEOUT_MS)) return ESP_ERR_TIMEOUT;
  esp_err_t e = metalio_i2c1_write_reg_locked(I2C_NUM_1, addr, reg, &val, 1, IO_TIMEOUT_MS);
  metalio_i2c1_unlock();
  return e;
}

// ---------------------------------------------------------------------------
// IMU
// ---------------------------------------------------------------------------
esp_err_t mn_imu_init(void) {
  uint8_t who = 0;
  esp_err_t e = rd_reg(IMU_ADDR, IMU_REG_WHO_AM_I, &who, 1);
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "IMU absent: %s", esp_err_to_name(e));
    return e;
  }
  // Vendor accepts any of 0x11/0x33/0x32/0x44 across SC7A20H revisions.
  if (who != 0x11 && who != 0x33 && who != 0x32 && who != 0x44) {
    ESP_LOGW(TAG, "IMU unexpected WHO_AM_I=0x%02X", who);
    return ESP_ERR_INVALID_RESPONSE;
  }

  const uint8_t regs[6][2] = {
      {IMU_REG_CTRL1, IMU_CTRL1_VAL}, {IMU_REG_CTRL2, IMU_CTRL2_VAL},
      {IMU_REG_CTRL3, IMU_CTRL3_VAL}, {IMU_REG_CTRL4, IMU_CTRL4_VAL},
      {IMU_REG_CTRL5, IMU_CTRL5_VAL}, {IMU_REG_CTRL6, IMU_CTRL6_VAL},
  };
  for (int i = 0; i < 6; ++i) {
    e = wr_reg(IMU_ADDR, regs[i][0], regs[i][1]);
    if (e != ESP_OK) {
      ESP_LOGW(TAG, "IMU cfg reg 0x%02X: %s", regs[i][0], esp_err_to_name(e));
      return e;
    }
  }
  // Vendor order: threshold and duration first, AOI1_CFG last (it arms INT1).
  const uint8_t aoi[3][2] = {
      {IMU_REG_AOI1_THS, IMU_AOI1_THS_VAL},
      {IMU_REG_AOI1_DUR, IMU_AOI1_DUR_VAL},
      {IMU_REG_AOI1_CFG, IMU_AOI1_CFG_VAL},
  };
  for (int i = 0; i < 3; ++i) {
    e = wr_reg(IMU_ADDR, aoi[i][0], aoi[i][1]);
    if (e != ESP_OK) {
      ESP_LOGW(TAG, "IMU AOI reg 0x%02X: %s", aoi[i][0], esp_err_to_name(e));
      return e;
    }
  }
  imu_ok = true;
  ESP_LOGI(TAG, "IMU ready (who=0x%02X, AOI1 armed -> INT1 wired-OR to GPIO2)", who);
  return ESP_OK;
}

esp_err_t mn_imu_read(int16_t* x, int16_t* y, int16_t* z) {
  if (!imu_ok) return ESP_ERR_INVALID_STATE;
  uint8_t raw[6] = {0};
  // Auto-increment bit so one transaction returns XL..ZH.
  esp_err_t e = rd_reg(IMU_ADDR, (uint8_t)(IMU_REG_OUT_XL | IMU_AUTO_INC), raw, 6);
  if (e != ESP_OK) return e;
  // Little-endian 12-bit left-justified data; vendor scale is 1 mg/LSB -> keep raw.
  if (x) *x = (int16_t)((int16_t)(raw[1] << 8 | raw[0]) >> 4);
  if (y) *y = (int16_t)((int16_t)(raw[3] << 8 | raw[2]) >> 4);
  if (z) *z = (int16_t)((int16_t)(raw[5] << 8 | raw[4]) >> 4);
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// RTC (PCF8563). BCD, registers 0x02..0x08 = sec,min,hour,day,wday,month,year.
// ---------------------------------------------------------------------------
static uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

esp_err_t mn_rtc_read_time(int* year, int* mon, int* day, int* hour, int* min, int* sec) {
  uint8_t r[7] = {0};
  esp_err_t e = rd_reg(RTC_ADDR, RTC_REG_SECONDS, r, 7);
  if (e != ESP_OK) return e;
  if (r[0] & 0x80) ESP_LOGW(TAG, "RTC voltage-low flag set (time may be invalid)");
  if (sec)  *sec  = bcd2dec(r[0] & 0x7F);
  if (min)  *min  = bcd2dec(r[1] & 0x7F);
  if (hour) *hour = bcd2dec(r[2] & 0x3F);
  if (day)  *day  = bcd2dec(r[3] & 0x3F);
  if (mon)  *mon  = bcd2dec(r[5] & 0x1F);
  if (year) *year = 2000 + bcd2dec(r[6]);
  return ESP_OK;
}

// Sakamoto: 0 = Sunday, matching struct tm's tm_wday which the vendor writes
// as tm_wday % 7 (common/sensor/pcf8563.cc:121). Writing 0x00 blindly, as an
// earlier revision did, wipes the hardware weekday field.
static int weekday_sunday0(int y, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

esp_err_t mn_rtc_write_time(int year, int mon, int day, int hour, int min, int sec) {
  if (year < 2000 || year > 2099) {
    ESP_LOGW(TAG, "RTC year %d outside PCF8563 range 2000-2099", year);
    return ESP_ERR_INVALID_ARG;
  }
  const int wday = weekday_sunday0(year, mon, day);
  uint8_t r[7] = {
      dec2bcd((uint8_t)sec), dec2bcd((uint8_t)min), dec2bcd((uint8_t)hour),
      dec2bcd((uint8_t)day), dec2bcd((uint8_t)wday), dec2bcd((uint8_t)mon),
      dec2bcd((uint8_t)(year % 100)),
  };
  if (!metalio_i2c1_lock(IO_TIMEOUT_MS)) return ESP_ERR_TIMEOUT;
  esp_err_t e = metalio_i2c1_write_reg_locked(I2C_NUM_1, RTC_ADDR, RTC_REG_SECONDS, r, 7, IO_TIMEOUT_MS);
  metalio_i2c1_unlock();
  return e;
}

// ---------------------------------------------------------------------------
// Fuel gauge
// ---------------------------------------------------------------------------
esp_err_t mn_gauge_read(int* mv, int* ma, int* pct) {
  uint8_t v[2] = {0}, c[2] = {0};
  esp_err_t e = rd_reg(GAUGE_ADDR, GAUGE_REG_VOLT, v, 2);
  if (e != ESP_OK) return e;
  const int voltage = (int)(v[0] | (v[1] << 8));
  if (mv) *mv = voltage;
  if (ma) {
    if (rd_reg(GAUGE_ADDR, GAUGE_REG_CURRENT, c, 2) == ESP_OK) {
      *ma = (int16_t)(c[0] | (c[1] << 8));
    } else {
      *ma = 0;
    }
  }
  if (pct) {
    // Vendor: linear 3.30 V (empty) .. 4.33 V (full), rounded half-up
    // (common/power/bq27220_gauge.cc: kBatteryEmptyV/kBatteryFullV, then
    // (int)(smoothed + 0.5f)).
    //
    // DIVERGENCE, deliberate: the vendor also runs a low-pass filter over the
    // percentage and refuses 100% until the pack has held >= kFullConfirmMv for
    // kFullHoldUs. This primitive is stateless and is called once per boot, so a
    // filter would have nothing to filter; the value here is the unfiltered,
    // unsmoothed equivalent. Anything that shows a live gauge must add the
    // filter on top rather than assume this matches the vendor's displayed number.
    const int span = BATT_FULL_MV - BATT_EMPTY_MV;
    int p = ((voltage - BATT_EMPTY_MV) * 100 + span / 2) / span;
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    *pct = p;
  }
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Charger status (raw byte; bit meanings live in cx25601n.h upstream)
// ---------------------------------------------------------------------------
esp_err_t mn_charger_read(uint8_t* status1) {
  if (!status1) return ESP_ERR_INVALID_ARG;
  return rd_reg(CHG_ADDR, CHG_REG_STATUS1, status1, 1);
}

// ---------------------------------------------------------------------------
// Haptics: why r89's duty made no audible difference, and what replaces it.
//
// WHAT THE VENDOR ACTUALLY DOES (read the whole engine, not the duty table):
//   Metalio-E-INK4-Plus/main/boards/metalio-e-ink4-plus/input/metalio_vibe.cc
//     * Two esp_timer callbacks. `vibe_pwm` toggles the motor bit and re-arms
//       itself for the OTHER phase: on for k_duty_on_us[level], off for
//       period - on. `vibe_pulse` stops the drive after VIBRATION_MOTOR_PULSE_MS
//       (50 ms, config.h:72).
//     * The motor bit is HELD at whatever was written last — the expander write
//       only happens on an edge, never in between (OnSoftPwm toggles a bool and
//       writes it).
//     * There is NO spin-up wait, NO duration change per intensity, and the
//       intensity is only applied while the drive is active
//       (ApplyActiveDrive(), metalio_vibe.cc:131-149).
//     * Level 3 does not PWM at all: it stops the PWM timer and holds the bit
//       high, so "100 %".
//   So the vendor expresses intensity ENTIRELY through mean duty, over a fixed
//   50 ms pulse.
//
// WHY OUR r89 PORT OF IT WAS IMPERCEPTIBLE — three independent reasons, all of
// which point the same way:
//   1. 50 ms is shorter than this motor's spin-up. A pager cell needs ~30 ms to
//      reach speed, so most of the pulse is spent accelerating. In that window
//      the rotor integrates torque, and 35 ms of drive (70 % of 50) vs 45 ms
//      (90 %) vs 50 ms (100 %) differ by only a few percent of *final* speed —
//      below the tactile JND. The vendor's own table only expresses itself in
//      continuous mode (MetalioVibe_Set(true)), which we never use.
//   2. 20 ms period with a 2 ms off window (level 2). Each motor_set() is a
//      TCA9555 I2C write: 2-byte write + driver + mutex. At 100 kHz that is
//      hundreds of microseconds, and up to ~1 ms with contention. A 2 ms gap
//      therefore shrinks to ~1 ms — and a 1 ms gap on a rotor whose mechanical
//      time constant is ~30-50 ms is, physically, no gap at all. Levels 2 and 3
//      became the same continuous drive, which is exactly what the operator
//      reported.
//   3. Nothing was measured. The duty loop trusted its own arithmetic while the
//      perturbation (write latency, tick granularity, preemption) was the same
//      order as the off window it was trying to create.
//
// THE FIX: distinct DURATION per level plus a period whose OFF window is at
// least an order of magnitude above the measured write cost, and timings
// measured with esp_timer_get_time so the achieved duty is a logged fact rather
// than an intention.
//
//     level  on_us   period_us   duty   min duration
//       1    12000     24000      50 %      60 ms
//       2    19000     24000      79 %      90 ms
//       3    held high   --      100 %     120 ms
//
// The 24 ms period keeps the off window at 12 ms (level 1) / 5 ms (level 2):
// both >> the ~0.3-1 ms expander write, so the gap survives. The duration grows
// with the level because that is the part the rotor can actually resolve: at
// level 1 the motor coasts down between edges and never reaches top speed; at
// level 3 it is driven continuously for twice as long.
//
// The caller's `ms` is a FLOOR minimum (src/HapticFeedback.h passes 50 for a
// tap, 30 for each half of a long press). A longer caller value wins, so the
// old call sites keep their meaning; nothing gets shorter than the profile.
//
// SAFETY, unchanged from r88: no timer, no retained state, the whole drive is a
// bounded blocking loop, and every exit path — including the expander dying
// mid-pulse — writes the motor bit low. Hard cap MN_HAPTIC_MAX_MS so a bad
// argument cannot wedge the caller.
// ---------------------------------------------------------------------------
typedef struct {
  uint32_t on_us;     // 0 = hold the bit high (100 %)
  uint32_t period_us;
  uint32_t min_ms;
} MnHapticProfile;

static const MnHapticProfile k_haptic_profile[4] = {
    {0, 0, 0},                 // 0 = off
    {12000u, 24000u, 60u},     // level 1: 50 % duty, 12 ms gap
    {19000u, 24000u, 90u},     // level 2: 79 % duty, 5 ms gap
    {0u, 0u, 120u},            // level 3: continuous, longest
};
#define MN_HAPTIC_MAX_MS 250u

// Timing log. ESP_LOGD would be the natural level, but this image is built with
// CONFIG_LOG_MAXIMUM_LEVEL=3 (INFO), which compiles ESP_LOGD out entirely — a
// DEBUG line here would print nothing and the operator would learn nothing. So
// the measurement goes out at INFO behind this one-line switch: set
// MN_HAPTIC_TIMING_LOG to 0 to silence it without touching anything else.
#ifndef MN_HAPTIC_TIMING_LOG
#define MN_HAPTIC_TIMING_LOG 1
#endif

// Delay `us` with ~1 tick of jitter and a busy-wait tail. The tick delay yields
// (the render task keeps running during a pulse); the busy-wait makes the last
// millisecond exact, because the off window is the thing being defended and
// vTaskDelay() can only round to whole ticks (1000 Hz here).
static void mn_delay_us(uint32_t us) {
  if (us == 0) return;
  const int64_t t_end = esp_timer_get_time() + (int64_t)us;
  int64_t now = esp_timer_get_time();
  while (t_end - now > 1500) {
    const uint32_t ms = (uint32_t)((t_end - now - 1500) / 1000);
    if (ms == 0) break;
    vTaskDelay(pdMS_TO_TICKS(ms));
    now = esp_timer_get_time();
  }
  while (esp_timer_get_time() < t_end) {
    // ~1 ms busy tail; short enough not to starve the other core's tasks.
  }
}

void mn_haptic_pulse_level(int ms, int level) {
  if (level < 0) level = 0;
  if (level > 3) level = 3;
  // A dead expander is not a reason to energise anything, and the vendor's
  // SetIntensity() returns early on exactly this condition (metalio_vibe.cc:32-35).
  if (!epd_board_metalio_eink4_plus_ioexp_ok()) return;

  const MnHapticProfile prof = k_haptic_profile[level];
  if (level == 0) {
    // Level 0 = "off": make sure the bit is low and stop.
    epd_board_metalio_eink4_plus_motor_set(false);
    return;
  }
  if (ms < 0) ms = 0;
  uint32_t duration_ms = (uint32_t)ms;
  if (duration_ms < prof.min_ms) duration_ms = prof.min_ms;
  if (duration_ms > MN_HAPTIC_MAX_MS) duration_ms = MN_HAPTIC_MAX_MS;

  const int64_t t_start = esp_timer_get_time();

  if (prof.on_us == 0) {
    // 100 % duty: hold the bit high for the whole duration (vendor level 3).
    epd_board_metalio_eink4_plus_motor_set(true);
    mn_delay_us(duration_ms * 1000u);
    epd_board_metalio_eink4_plus_motor_set(false);
#if MN_HAPTIC_TIMING_LOG
    ESP_LOGI("haptic", "lvl3 held %lu ms (measured %lld ms, on=100%%)",
             (unsigned long)duration_ms, (esp_timer_get_time() - t_start) / 1000);
#endif
    return;
  }

  const uint32_t off_us = prof.period_us - prof.on_us;
  const int64_t t_deadline = t_start + (int64_t)duration_ms * 1000;
  uint32_t cycles = 0;
  uint32_t measured_on = 0, measured_off = 0, write_us = 0;

  while (esp_timer_get_time() < t_deadline) {
    int64_t t0 = esp_timer_get_time();
    epd_board_metalio_eink4_plus_motor_set(true);
    const int64_t t1 = esp_timer_get_time();  // write cost of the ON edge
    mn_delay_us(prof.on_us);
    const int64_t t2 = esp_timer_get_time();
    epd_board_metalio_eink4_plus_motor_set(false);
    const int64_t t3 = esp_timer_get_time();  // write cost of the OFF edge
    // Keep the off window honest even if we are already past the deadline: an
    // off-sliver shorter than one I2C write is the failure mode being fixed.
    mn_delay_us(off_us);
    const int64_t t4 = esp_timer_get_time();
    ++cycles;
    measured_on += (uint32_t)(t2 - t1);
    measured_off += (uint32_t)(t4 - t3);
    write_us += (uint32_t)((t1 - t0) + (t3 - t2));
  }
  // Every exit path writes low, including a caller that got preempted past the
  // deadline inside the loop.
  epd_board_metalio_eink4_plus_motor_set(false);

#if MN_HAPTIC_TIMING_LOG
  const int64_t elapsed = esp_timer_get_time() - t_start;
  const uint32_t total = measured_on + measured_off;
  const unsigned duty_pct = total ? (unsigned)((measured_on * 100u) / total) : 0u;
  ESP_LOGI("haptic",
           "lvl%d %lu ms: %lu cycles, on=%lu us off=%lu us (%.0f us/cycle), achieved duty=%u%% "
           "(target %u%%), edge writes=%lu us total, elapsed=%lld ms",
           level, (unsigned long)duration_ms, (unsigned long)cycles, (unsigned long)measured_on,
           (unsigned long)measured_off, cycles ? (double)total / (double)cycles : 0.0, duty_pct,
           (unsigned)((prof.on_us * 100u) / prof.period_us), (unsigned long)write_us, elapsed / 1000);
#endif
}

// Duration-only pulse, kept for the existing call sites: full-strength
// (level 3), which is what mn_haptic_pulse() physically did before the duty
// work landed.
void mn_haptic_pulse(int ms) { mn_haptic_pulse_level(ms, 3); }

// EXACT-duration pulse (r101.9, HAPT2/HAPT3). mn_haptic_pulse_level() applies the
// per-level duration FLOOR from k_haptic_profile (60 / 90 / 120 ms) so that a short
// tap stays perceptible -- which is right for a tap and wrong for a cue that NAMES its
// duration: the power-off cue is 2 x 100 ms, and on the level-3 profile 100 would have
// come out as 120. This entry point takes `ms` literally on the same level-3 envelope
// (motor bit held high, 100 % duty) that mn_haptic_pulse() produces, applies no floor,
// and keeps the same safety contract as the level path: the expander is checked first,
// the drive is a bounded blocking hold, and every exit path writes the motor bit low.
void mn_haptic_pulse_exact(int ms) {
  // A dead expander is not a reason to energise anything (same guard as the level path).
  if (!epd_board_metalio_eink4_plus_ioexp_ok()) return;
  if (ms <= 0) {
    epd_board_metalio_eink4_plus_motor_set(false);
    return;
  }
  if (ms > (int)MN_HAPTIC_MAX_MS) ms = (int)MN_HAPTIC_MAX_MS;
  const int64_t t_start = esp_timer_get_time();
  epd_board_metalio_eink4_plus_motor_set(true);
  mn_delay_us((uint32_t)ms * 1000u);
  epd_board_metalio_eink4_plus_motor_set(false);
#if MN_HAPTIC_TIMING_LOG
  ESP_LOGI("haptic", "exact %d ms held (measured %lld ms, on=100%%)", ms,
           (esp_timer_get_time() - t_start) / 1000);
#endif
}

void mn_audio_power(bool on) {
  epd_board_metalio_eink4_plus_pa_set(on);   // TCA P0.4 = NS4150 CTRL
  if (on) epd_board_metalio_eink4_plus_bt_power_reset();
}

// ---------------------------------------------------------------------------
// BH1098G Bluetooth-audio module (UART1, TX=GPIO39, RX=GPIO38, 115200).
// Protocol and timings from upstream periph/metalio_audio.cc:
//   "AT+RX=2\r\n"  -> 700 ms -> "AT+MODE=1\r\n" -> wait <=1000 ms for "SET MODE 1"
//   any line containing "ERROR" or "FAIL" aborts.
// ---------------------------------------------------------------------------
#include "driver/uart.h"

#define BT_UART        UART_NUM_1
#define BT_TX_PIN      39
#define BT_RX_PIN      38
#define BT_BAUD        115200
#define BT_ACK_TIMEOUT_MS 1000

static bool bt_uart_ready = false;

static void bt_send(const char* s) {
  uart_write_bytes(BT_UART, s, strlen(s));
}

// Returns true if a line containing `want` was seen before the timeout.
static bool bt_wait_for(const char* want, int timeout_ms) {
  char line[96];
  size_t n = 0;
  const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
  while (esp_timer_get_time() < deadline) {
    uint8_t c = 0;
    const int got = uart_read_bytes(BT_UART, &c, 1, pdMS_TO_TICKS(50));
    if (got <= 0) continue;
    if (c == '\r' || c == '\n') {
      line[n] = 0;
      if (n > 0) {
        if (strstr(line, "ERROR") || strstr(line, "FAIL")) return false;
        if (strstr(line, want)) return true;
      }
      n = 0;
    } else if (n + 1 < sizeof(line)) {
      line[n++] = (char)c;
    } else {
      n = 0;  // overflow: drop the line rather than corrupt
    }
  }
  return false;
}

esp_err_t mn_audio_init(void) {
  // Rail + module reset first: the AT channel is dead until the module is powered.
  epd_board_metalio_eink4_plus_pa_set(false);
  epd_board_metalio_eink4_plus_bt_power_reset();
  vTaskDelay(pdMS_TO_TICKS(300));

  if (!bt_uart_ready) {
    const uart_config_t cfg = {
        .baud_rate = BT_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t e = uart_driver_install(BT_UART, 2048, 0, 0, NULL, 0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    e = uart_param_config(BT_UART, &cfg);
    if (e != ESP_OK) return e;
    uart_set_pin(BT_UART, BT_TX_PIN, BT_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_flush_input(BT_UART);
    bt_uart_ready = true;
  }

  bt_send("AT+RX=2\r\n");
  vTaskDelay(pdMS_TO_TICKS(700));
  uart_flush_input(BT_UART);
  bt_send("AT+MODE=1\r\n");
  if (!bt_wait_for("SET MODE 1", BT_ACK_TIMEOUT_MS)) {
    ESP_LOGW(TAG, "BH1098G did not confirm SET MODE 1");
    return ESP_ERR_TIMEOUT;
  }
  epd_board_metalio_eink4_plus_pa_set(true);   // NS4150 amp on
  ESP_LOGI(TAG, "BH1098G ready (RX=2, MODE=1, PA on)");
  return ESP_OK;
}

void mn_audio_deinit(void) {
  epd_board_metalio_eink4_plus_pa_set(false);
  if (bt_uart_ready) {
    uart_driver_delete(BT_UART);
    bt_uart_ready = false;
  }
}

// ---------------------------------------------------------------------------
// RTC interrupt: PCF8563 INT is wired to expander P1.6 (bit 14), NOT to a GPIO,
// and is OR-ed onto the expander INT line (GPIO2). Reading port 1 bit 6 tells us
// whether the RTC has an alarm/timer pending.
// ---------------------------------------------------------------------------
bool mn_rtc_int_pending(void) {
  uint8_t p1 = 0xFF;
  if (!metalio_i2c1_lock(IO_TIMEOUT_MS)) return false;
  const esp_err_t e = pca9555_read_input_ex(I2C_NUM_1, 1, &p1);
  metalio_i2c1_unlock();
  if (e != ESP_OK) return false;
  return (p1 & (1u << 6)) == 0;   // active low
}

// INT1 of the SC7A20H is open-drain and wired-OR with the TCA9555 INT and the
// RTC alarm onto GPIO2 (vendor config.h:90, "经 TLR2 与 TCA INT 线与到 GPIO2").
// AOI1_SRC is latched (CTRL5 LIR_INT1), so reading it is also the acknowledge.
bool mn_imu_activity(void) {
  if (!imu_ok) return false;
  uint8_t src = 0;
  if (rd_reg(IMU_ADDR, IMU_REG_AOI1_SRC, &src, 1) != ESP_OK) return false;
  return (src & IMU_AOI1_IA_MASK) != 0;
}

// Vendor decodes CHG_STAT as bits [3:4] of REG0x1E: read_bits(REG_STATUS1, 0x03, 3)
// in common/power/cx25601n.cc:648. Values: 0 not-charging/terminated, 1 trickle/
// precharge/CC, 2 CV taper, 3 top-off.
int mn_charger_status(void) {
  uint8_t raw = 0;
  if (mn_charger_read(&raw) != ESP_OK) return -1;
  return (raw >> 3) & 0x03;
}

const char* mn_charger_status_str(int st) {
  switch (st) {
    case 0: return "not-charging/terminated";
    case 1: return "trickle/precharge/CC";
    case 2: return "CV taper";
    case 3: return "top-off";
    default: return "no response";
  }
}
