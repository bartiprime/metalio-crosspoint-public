/*
 * r99 SENTRY program: records WHICH wake source fired, then (mode 1 only)
 * pre-filters the power key so the HP core need not be woken for every edge.
 *
 * THIS IS A THIRD PROGRAM, not a replacement for ulp/main.c or ulp/main_bench.c.
 * Both of those stay exactly as they are, because the r88 record ("pass d=3503
 * per 2 ms") and the r96 bench numbers are quoted against them. This one is the
 * instrument `docs/LP_CORE_CAPABILITY.md` §7 step 3 asks for, and the same
 * program is what CP_LP_STANDBY runs in production.
 *
 * WAKE MODEL (components/ulp/lp_core/lp_core/lp_core_startup.c:25-58): the LP
 * core is reset on every wakeup, startup calls ulp_lp_core_update_wakeup_cause()
 * and then main() ONCE, and on return it re-arms the LP timer with
 * shared_mem->sleep_duration_ticks and halts. So this file is a "run to
 * completion per wakeup" program: no loop, no halt call, and every pass is one
 * sample of the pins. The `last` latches and all counters live in LP RAM, which
 * is not reloaded between wakeups, so state crosses wakeups by construction.
 *
 * WHAT IT DOES NOT DO, deliberately:
 *   * NO I2C. The FT6336U's coordinates and the TCA9555's key bitmap both live
 *     behind the HP I2C driver's mutex; LP I2C is a second master on the same
 *     pads (docs/engineering/LPCORE_AND_PPA_FRAMEWORK.md §2.3). The sentry can
 *     therefore only trust LINE LEVELS: GPIO2 (expander INT / power key, a
 *     wire-OR with the accel) and GPIO5 (FT6336U INT).
 *   * NO PSRAM. There is no external region in lp_core_riscv.ld.
 *   * No writes outside the globals below.
 *
 * ORDER: magic and version first, so magic==0 means "never started" and a set
 * magic with a frozen hb means "started, then stopped running" — the two failure
 * modes the r96 round could not tell apart.
 */
#include <stdint.h>

#include "ulp_lp_core.h"
#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_utils.h"

/* Exported to the HP core by esp32ulp_mapgen.py as ulp_cp_lp_sentry_<name>. */
volatile uint32_t cp_lp_sentry_magic = 0;      /* 'SN01' once running */
volatile uint32_t cp_lp_sentry_ver = 0;        /* program version */
volatile uint32_t cp_lp_sentry_hb = 0;         /* +1 per wakeup; frozen = not running */
volatile uint32_t cp_lp_sentry_last_cause = 0; /* raw PMU wakeup-cause bitmap */
volatile uint32_t cp_lp_sentry_wake_timer = 0;
volatile uint32_t cp_lp_sentry_wake_io = 0;
volatile uint32_t cp_lp_sentry_wake_other = 0;
volatile uint32_t cp_lp_sentry_level2 = 0;     /* last sampled GPIO2 level */
volatile uint32_t cp_lp_sentry_level5 = 0;     /* last sampled GPIO5 level */
volatile uint32_t cp_lp_sentry_edges2 = 0;     /* GPIO2 level changes seen */
volatile uint32_t cp_lp_sentry_edges5 = 0;     /* GPIO5 level changes seen */
volatile uint32_t cp_lp_sentry_low2 = 0;       /* wakeups with GPIO2 low */
volatile uint32_t cp_lp_sentry_pressed = 0;    /* debounced key assertions */
volatile uint32_t cp_lp_sentry_debounce = 0;   /* consecutive low samples */
volatile uint32_t cp_lp_sentry_woke = 0;       /* HP wake requests raised */
volatile uint32_t cp_lp_sentry_mode = 0;       /* HP: 0 = record only, 1 = pre-filter */
volatile uint32_t cp_lp_sentry_stop = 0;       /* HP: 1 = leave before arming */

#define CP_LP_SENTRY_MAGIC 0x534E3031u /* 'SN01' */
#define CP_LP_SENTRY_VER 1u

/* The LP domain's power key line is ACTIVE LOW (expander P1.7 idle-high, the
 * INT latch and the IMU INT1 wire-OR both pull the pad low). One "press" is
 * DEBOUNCE consecutive wakeups with the line low; at the default 20 ms LP timer
 * that is 60 ms of continuous assertion, which is the same order as the HP
 * side's METALIO_WAKE_BOUNCE_MS (40 ms) filter in metalioVerifyStandbyWake(). */
#define CP_LP_SENTRY_DEBOUNCE 3u

/* PMU wakeup-cause bits, from hal/esp32s31/include/hal/lp_core_ll.h:26,34.
 * Defined here rather than included so the LP program does not pull an HP-only
 * LL header into its build; the values are what ulp_lp_core_get_wakeup_cause()
 * returns verbatim after ulp_lp_core_update_wakeup_cause() at startup. */
#define CP_LP_CAUSE_IO (1u << 9)    /* LP_CORE_LL_WAKEUP_SOURCE_LP_IO */
#define CP_LP_CAUSE_TIMER (1u << 18) /* LP_CORE_LL_WAKEUP_SOURCE_LP_TIMER_1 */

/* Latches. Static: not exported, and they persist across wakeups because LP RAM
 * is never reloaded. Seeded by the image at load_binary() time. */
static uint32_t last2;
static uint32_t last5;
static uint32_t seeded;

int main(void) {
  if (seeded == 0) {
    /* First pass after load_binary(): take the levels as the baseline so the
     * edge counters do not fire on the very first sample. */
    seeded = 0x5EA3EDu;
    last2 = ulp_lp_core_gpio_get_level(LP_IO_NUM_2);
    last5 = ulp_lp_core_gpio_get_level(LP_IO_NUM_5);
  }

  cp_lp_sentry_magic = CP_LP_SENTRY_MAGIC;
  cp_lp_sentry_ver = CP_LP_SENTRY_VER;
  /* One per wakeup, before the stop check: a frozen hb then means "the LP core
   * stopped executing", never "the HP side asked it to". Nothing else in this
   * program may be the only writer of a global the shim reads — an unreferenced
   * global is dropped by the linker and never exported by esp32ulp_mapgen.py,
   * which turns into a compile error in cp_lp_sentry.c. */
  cp_lp_sentry_hb = cp_lp_sentry_hb + 1u;

  if (cp_lp_sentry_stop != 0) return 0;

  /* 1. WHICH SOURCE fired. This is the measurement: a timer-dominated wake
   *    count means the LP core is surviving on its own clock, an io-dominated
   *    one means GPIO2 is what brings it back (which is what the pre-filter
   *    then acts on). */
  const uint32_t cause = ulp_lp_core_get_wakeup_cause();
  cp_lp_sentry_last_cause = cause;
  if ((cause & CP_LP_CAUSE_IO) && (cause & CP_LP_CAUSE_TIMER)) {
    /* Both latched: count it as IO, which is the interesting half, and leave
     * the timer counter honest by not incrementing it. */
    cp_lp_sentry_wake_io = cp_lp_sentry_wake_io + 1u;
  } else if (cause & CP_LP_CAUSE_IO) {
    cp_lp_sentry_wake_io = cp_lp_sentry_wake_io + 1u;
  } else if (cause & CP_LP_CAUSE_TIMER) {
    cp_lp_sentry_wake_timer = cp_lp_sentry_wake_timer + 1u;
  } else if (cause != 0u) {
    cp_lp_sentry_wake_other = cp_lp_sentry_wake_other + 1u;
  }

  /* 2. Sample both interrupt lines. Levels only — see the header comment. */
  const uint32_t l2 = ulp_lp_core_gpio_get_level(LP_IO_NUM_2);
  if (l2 != last2) {
    last2 = l2;
    cp_lp_sentry_level2 = l2;
    cp_lp_sentry_edges2 = cp_lp_sentry_edges2 + 1u;
  }
  const uint32_t l5 = ulp_lp_core_gpio_get_level(LP_IO_NUM_5);
  if (l5 != last5) {
    last5 = l5;
    cp_lp_sentry_level5 = l5;
    cp_lp_sentry_edges5 = cp_lp_sentry_edges5 + 1u;
  }

  /* 3. Debounce the power-key line. Asserted-low, so a run of low samples is a
   *    press and any high sample resets the run. */
  if (l2 == 0u) {
    cp_lp_sentry_low2 = cp_lp_sentry_low2 + 1u;
    if (cp_lp_sentry_debounce < 0xFFFFFFFFu) cp_lp_sentry_debounce = cp_lp_sentry_debounce + 1u;
  } else {
    cp_lp_sentry_debounce = 0u;
  }

  if (cp_lp_sentry_debounce == CP_LP_SENTRY_DEBOUNCE) {
    cp_lp_sentry_pressed = cp_lp_sentry_pressed + 1u;
    if (cp_lp_sentry_mode != 0u) {
      /* THE ONLY DECISION THIS PROGRAM MAKES, and only in mode 1: one wake
       * request per debounced assertion. The HP side still runs its own
       * metalioVerifyStandbyWake() on the key and can still answer "not a
       * press", so a false positive here costs a spurious HP wake (today's
       * behaviour), never a wrong action. */
      ulp_lp_core_wakeup_main_processor();
      cp_lp_sentry_woke = cp_lp_sentry_woke + 1u;
    }
  }

  return 0; /* startup re-arms the LP timer and halts */
}
