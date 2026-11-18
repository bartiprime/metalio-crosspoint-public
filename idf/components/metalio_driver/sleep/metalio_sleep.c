/*
 * S31 light-sleep shim — port of the vendor's
 *   main/boards/common/power_policy/s31_sleep/{s31_sleep.cc,s31_sleep.h}
 *   (+ s31_sleep_sdk.cmake for the PMU PSRAM clock handoff backport)
 *
 * Why this exists at all: on the ESP32-S31 the PMU tears the MPLL down for every
 * light-sleep slice and rebuilds it on resume. The rebuild re-applies the *boot*
 * analog bias, which is not the 12/12 the calibration flow needs, and it does not
 * hand the 200 MHz octal PSRAM back to the MPLL clock source. The vendor hit both
 * (their log: "PMU wake has overwritten the startup bias (observed 7/7)") and
 * wrapped the SDK functions instead of patching the installed tree. Same approach
 * here: three --wrap hooks from CMakeLists.txt, everything else untouched.
 *
 *   --wrap=pmu_sleep_start              marks "we are inside the PMU sleep"
 *   --wrap=rtc_clk_mpll_configure       re-applies 12/12 bias + MSPI divider before
 *                                       the MPLL is re-calibrated on resume
 *   --wrap=esp_sleep_execute_event_callbacks   records which callback phase a
 *                                       failed/rejected sleep died in
 *
 * Nothing here logs to SD: during a sleep slice caches may be off. Trace state
 * lives in RTC_NOINIT memory and is replayed by the app on the next boot.
 */
#include "metalio_sleep.h"

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <soc/soc.h>

#include <esp_private/regi2c_ctrl.h>
#include <esp_private/sleep_event.h>
#include <hal/regi2c_ctrl_ll.h>
#include <hal/regi2c_impl.h>
#include <soc/hp_sys_clkrst_reg.h>
#include <soc/lp_clkrst_reg.h>
#include <soc/regi2c_bias.h>

#define TAG "SlpS31"

/* Survives CPU/watchdog resets when RTC memory is retained; no SD writes while asleep. */
typedef struct {
  uint32_t magic;
  uint32_t cycle;
  uint32_t phase;
  uint32_t applied;
  uint32_t pll_div;
  uint32_t pll_ctrl;
  uint32_t analog;
  uint32_t bias;
} SleepTrace;

static RTC_NOINIT_ATTR volatile SleepTrace s_trace;
static const uint32_t kTraceMagic = 0x534C5032u; /* "SLP2" */
static DRAM_ATTR bool s_sdk_sleep_active = false;
static DRAM_ATTR bool s_pmu_sleep_active = false;
/* Set only for the duration of one esp_light_sleep_start(); see the gate in
 * __wrap_rtc_clk_mpll_configure and metalio_sleep_init() for why cold boot must never open it. */
static DRAM_ATTR bool s_restore_armed = false;

/* ------------------------------------------------------------------------- */
/* Wrapped SDK entry points                                                    */
/* ------------------------------------------------------------------------- */

extern uint32_t pmu_sleep_start(uint32_t wakeup_opt, uint32_t reject_opt, uint32_t lslp_mem_inf_fpu, bool dslp);
extern uint32_t IRAM_ATTR __real_pmu_sleep_start(uint32_t wake, uint32_t reject, uint32_t mem, bool deep);

uint32_t IRAM_ATTR __wrap_pmu_sleep_start(uint32_t wake, uint32_t reject, uint32_t mem, bool deep) {
  if (s_trace.cycle != 0) s_trace.phase = 200;
  s_pmu_sleep_active = !deep;
  const uint32_t result = __real_pmu_sleep_start(wake, reject, mem, deep);
  s_pmu_sleep_active = false;
  if (s_trace.cycle != 0) s_trace.phase = 201;
  return result;
}

extern void rtc_clk_mpll_configure(uint32_t xtal_freq, uint32_t mpll_freq, bool thread_safe);
extern void IRAM_ATTR __real_rtc_clk_mpll_configure(uint32_t xtal, uint32_t frequency, bool clock_owned);

void IRAM_ATTR __wrap_rtc_clk_mpll_configure(uint32_t xtal, uint32_t frequency, bool clock_owned) {
  /* THREE gates, all of which must be open -- and the first one is the important one:
   *   s_restore_armed   only true for the duration of one esp_light_sleep_start() call, so a
   *                     COLD BOOT can never take the patched path (the reset loop this caused:
   *                     writing I2C_BIAS / the MSPI divider under a live MPLL kills the USB PHY
   *                     clock before the device can enumerate)
   *   s_pmu_sleep_active inside pmu_sleep_start()
   *   s_sdk_sleep_active inside metalio_sleep_light_sleep()
   * IRAM AUDIT: everything reachable from here is IRAM-resident -- this function (IRAM_ATTR),
   * _regi2c_impl_{read,read_mask,write_mask} (esp_hal_regi2c `* (noflash)` via
   * CONFIG_ESP_REGI2C_CTRL_FUNC_IN_IRAM=y), _regi2c_ctrl_ll_master_enable_clock and
   * regi2c_ctrl_ll_master_is_clock_enabled (always_inline in a HAL header), and the __real_ call.
   * No ESP_LOG, no allocation, no flash access: the cache is off while this runs. */
  if (!s_restore_armed || !s_pmu_sleep_active || !s_sdk_sleep_active) {
    __real_rtc_clk_mpll_configure(xtal, frequency, clock_owned);
    return;
  }
  /* Backport S31's upstream MPLL analog initialization before calibration:
   * https://github.com/espressif/esp-idf/commit/9726078b55430ef8d2ce4a460ebf21d889bb5c12
   * The PMU runs with interrupts masked and the other CPU stalled, so use the
   * cache-safe low-level register path without taking a cross-CPU lock. */
  const bool gate_was_enabled = regi2c_ctrl_ll_master_is_clock_enabled();
  s_trace.phase = 500;
  _regi2c_ctrl_ll_master_enable_clock(true);
  s_trace.bias = _regi2c_impl_read_mask(I2C_BIAS, 0, I2C_BIAS_DREG_1P1, 7, 4) |
                 ((uint32_t)_regi2c_impl_read_mask(I2C_BIAS, 0, I2C_BIAS_DREG_1P1_PVT, 3, 0) << 8);
  /* PMU wake has overwritten the startup bias (observed 7/7). Re-apply the
   * upstream 12/12 calibration prerequisite on every resume, not only at boot. */
  _regi2c_impl_write_mask(I2C_BIAS, 0, I2C_BIAS_DREG_1P1, 7, 4, 12);
  _regi2c_impl_write_mask(I2C_BIAS, 0, I2C_BIAS_DREG_1P1_PVT, 3, 0, 12);
  esp_rom_delay_us(10);
  const uint8_t kMpllBlock = 0x0b;
  _regi2c_impl_write_mask(kMpllBlock, 0, 1, 1, 0, 3); /* IR_CAL_EXT_CAP */
  _regi2c_impl_write_mask(kMpllBlock, 0, 1, 2, 2, 1); /* IR_CAL_ENX_CAP */
  REG_SET_FIELD(LP_AONCLKRST_MSPI_DIV_REG, LP_AONCLKRST_MSPI_FB_DIV, 9);
  _regi2c_impl_write_mask(kMpllBlock, 0, 3, 5, 4, 3); /* DHREF */
  _regi2c_impl_write_mask(kMpllBlock, 0, 1, 2, 2, 0); /* IR_CAL_ENX_CAP */
  s_trace.pll_div = REG_READ(LP_AONCLKRST_MSPI_DIV_REG);
  s_trace.pll_ctrl = REG_READ(HP_SYS_CLKRST_ANA_PLL_CTRL0_REG);
  s_trace.analog = _regi2c_impl_read(kMpllBlock, 0, 1) | ((uint32_t)_regi2c_impl_read(kMpllBlock, 0, 3) << 8);
  s_trace.bias = s_trace.bias | ((uint32_t)_regi2c_impl_read_mask(I2C_BIAS, 0, I2C_BIAS_DREG_1P1, 7, 4) << 16) |
                 ((uint32_t)_regi2c_impl_read_mask(I2C_BIAS, 0, I2C_BIAS_DREG_1P1_PVT, 3, 0) << 24);
  s_trace.phase = 501;
  __real_rtc_clk_mpll_configure(xtal, frequency, true);
  s_trace.phase = 502;
  _regi2c_ctrl_ll_master_enable_clock(gate_was_enabled);
}

extern void esp_sleep_execute_event_callbacks(esp_sleep_event_cb_index_t event_id, void* ext_arg);
extern void IRAM_ATTR __real_esp_sleep_execute_event_callbacks(esp_sleep_event_cb_index_t event, void* arg);

void IRAM_ATTR __wrap_esp_sleep_execute_event_callbacks(esp_sleep_event_cb_index_t event, void* arg) {
  /* Cache may be disabled here: touch RTC RAM only, never log or allocate. */
  if (s_sdk_sleep_active) s_trace.phase = 100u + (uint32_t)event;
  __real_esp_sleep_execute_event_callbacks(event, arg);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                  */
/* ------------------------------------------------------------------------- */

static SleepTrace s_previous_sleep;
static bool s_previous_valid = false;
static int s_reset_raw0 = 0, s_reset_raw1 = 0;

/* RESUME-ONLY LATCH.
 *
 * The MPLL/bias interception must NEVER run on a cold power-on boot. There, the stock IDF clock
 * init has to lock the PLL and leave the analog block alone -- the USB-Serial/JTAG PHY and the
 * octal PSRAM both run off that chain, and writing I2C_BIAS / the MSPI divider underneath them
 * puts the part in an immediate reset loop that never reaches USB enumeration (measured: COM4
 * appears and disappears continuously). This latch is the only thing that opens the patched path,
 * and it is set for the duration of one esp_light_sleep_start() call and nothing else.
 * (Defined above, next to the other sleep flags.) */

esp_err_t metalio_sleep_init(void) {
  /* Deliberately touches NO analog register. The vendor's Init() wrote the 12/12 calibration
   * startup bias here, but on this IDF the cold-boot analog state is already correct and the write
   * is not safe to do under a running MPLL. The 12/12 prerequisite is re-applied on every RESUME
   * instead (__wrap_rtc_clk_mpll_configure), which is the only place it matters. */
  ESP_LOGI(TAG, "S31 boot: stock clock/USB-PHY init left untouched; MPLL+bias restore armed for resume only");
  return ESP_OK;
}

void metalio_sleep_capture_reset(void) {
  s_reset_raw0 = (int)esp_rom_get_reset_reason(0);
  s_reset_raw1 = (int)esp_rom_get_reset_reason(1);
  const bool valid = s_trace.magic == kTraceMagic;
  s_previous_valid = valid;
  if (valid) {
    s_previous_sleep.magic = s_trace.magic;
    s_previous_sleep.cycle = s_trace.cycle;
    s_previous_sleep.phase = s_trace.phase;
    s_previous_sleep.applied = s_trace.applied;
    s_previous_sleep.pll_div = s_trace.pll_div;
    s_previous_sleep.pll_ctrl = s_trace.pll_ctrl;
    s_previous_sleep.analog = s_trace.analog;
    s_previous_sleep.bias = s_trace.bias;
  }
  ESP_LOGW(TAG, "boot reset_reason=%d previous_sleep: valid=%d cycle=%lu phase=%lu applied=0x%lx",
           (int)esp_reset_reason(), valid ? 1 : 0, valid ? (unsigned long)s_trace.cycle : 0ul,
           valid ? (unsigned long)s_trace.phase : 0ul, valid ? (unsigned long)s_trace.applied : 0ul);
  s_trace.magic = kTraceMagic;
  s_trace.cycle = 0;
  s_trace.phase = 0;
  s_trace.applied = 0;
  s_trace.pll_div = s_trace.pll_ctrl = s_trace.analog = s_trace.bias = 0;
}

/* USB Serial/JTAG misses app_main's first lines during enumeration, so the
 * previous-boot snapshot is replayed rather than read once. */
void metalio_sleep_report_reset(void) {
  static unsigned report = 0;
  const int64_t at[3] = {10000000, 30000000, 60000000};
  if (report >= 3 || esp_timer_get_time() < at[report]) return;
  ++report;
  ESP_LOGW(TAG, "boot reset replay[%u]: reset_reason=%d raw_cpu0=%d raw_cpu1=%d previous_sleep valid=%d cycle=%lu phase=%lu applied=0x%lx",
           report, (int)esp_reset_reason(), s_reset_raw0, s_reset_raw1, s_previous_valid ? 1 : 0,
           (unsigned long)s_previous_sleep.cycle, (unsigned long)s_previous_sleep.phase,
           (unsigned long)s_previous_sleep.applied);
  ESP_LOGW(TAG, "sleep regs replay: div=0x%lx ctrl=0x%lx analog=0x%lx bias=0x%lx",
           (unsigned long)s_previous_sleep.pll_div, (unsigned long)s_previous_sleep.pll_ctrl,
           (unsigned long)s_previous_sleep.analog, (unsigned long)s_previous_sleep.bias);
}

void metalio_sleep_begin_cycle(void) { s_trace.cycle = s_trace.cycle + 1; }

void metalio_sleep_checkpoint(int phase, unsigned applied) {
  s_trace.applied = applied;
  s_trace.phase = (uint32_t)phase;
  ESP_LOGI(TAG, "sleep trace cycle=%lu phase=%lu applied=0x%x", (unsigned long)s_trace.cycle, (unsigned long)phase,
           applied);
}

esp_err_t metalio_sleep_light_sleep(unsigned applied) {
  s_sdk_sleep_active = true;
  /* The ONLY place the resume-side MPLL/bias interception is allowed to run. Opened here, closed
   * immediately after the CPU comes back, so it cannot outlive the slice. */
  s_restore_armed = true;
  const esp_err_t err = esp_light_sleep_start();
  s_restore_armed = false;
  /* CPU retention may restore the continuation outside the PMU wrapper; never
   * let the wake-only register workaround leak into normal clock use. */
  s_pmu_sleep_active = false;
  s_sdk_sleep_active = false;
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  metalio_sleep_checkpoint(METALIO_SLP_PHASE_AFTER_SLEEP, applied); /* CPU returned, before any I2C */
  esp_rom_printf("SlpS31: sleep returned cycle=%lu err=%d cause=%d before I2C\n", (unsigned long)s_trace.cycle,
                 (int)err, (int)cause);
  return err;
}

uint32_t metalio_sleep_cycle(void) { return s_trace.cycle; }
uint32_t metalio_sleep_last_phase(void) { return s_trace.phase; }
