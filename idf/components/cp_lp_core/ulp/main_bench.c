/*
 * r96 LP bench program: heartbeat + RTC-pin edge counters + optional HP wake.
 *
 * THIS IS A SECOND PROGRAM, not a replacement for ulp/main.c. The r87 program is
 * a bounded 400 000-iteration counter whose `done` bit says "ran to completion";
 * it stays exactly as it is, because the Debug "LP core" row and the r88 record
 * ("pass d=3503 per 2 ms") are measured against it. This one is unbounded, has a
 * heartbeat instead of an end, and exists to answer two questions the r87 program
 * cannot:
 *
 *   1. What is this LP core's iteration rate, stably?  HP samples hb twice and
 *      divides (cpustats-independent, src/platform/LpCoreService.cpp).
 *   2. Does the LP core KEEP RUNNING while the HP core is in light sleep?
 *      (docs/engineering/LPCORE_PPA_R96_PLAN.md §2 A5 — the pivot for r97.)
 *      HP takes one 60 s slice, then reads hb back: a counter that advanced by
 *      ~rate x seconds means yes; a frozen counter means the LP core does not
 *      survive light sleep and the idle-page sentry has to be re-scoped.
 *
 * It also counts EDGES on the two RTC-domain interrupt lines the sentry would
 * watch, and can raise the HP core when one arrives:
 *   LP_IO_NUM_5 = GPIO5 = touch INT (FT6336U)      — soc_caps.h SOC_RTCIO_PIN_COUNT=8
 *   LP_IO_NUM_2 = GPIO2 = expander/accel INT (TCA9555 + IMU wire-OR)
 * Both are inside the LP GPIO window; nothing else on this board is.
 *
 * HARD RULES (plan §4.3, §4.6, §5.1):
 *   * NO I2C. The LP core shares our two I2C1 pads, and bus ownership is r98+.
 *   * NO PSRAM. The LP core has no PSRAM window (lp_core_riscv.ld:29-37).
 *   * No writes outside the globals below. They live in the LP RAM window
 *     reserved by CONFIG_ULP_COPROC_RESERVE_MEM=4096, so the image + these words
 *     must fit that reservation; the built .bin size is checked, not assumed.
 *
 * ORDER: magic and version are written FIRST, so "never started" (magic 0) is
 * distinguishable from "started but hung" (magic set, hb frozen).
 *
 * Unbounded loop, but not unstoppable: the HP core sets cp_lp_bench_stop and the
 * program returns, which halts the LP core (IDF halts it on return from main).
 */
#include <stdint.h>

#include "ulp_lp_core.h"
#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_utils.h"

/* Exported to the HP core by esp32ulp_mapgen.py as ulp_cp_lp_bench_<name>. */
volatile uint32_t cp_lp_bench_magic = 0;   /* 'LB01' once the program is running */
volatile uint32_t cp_lp_bench_ver = 0;     /* program version, bumped with behaviour */
volatile uint32_t cp_lp_bench_hb = 0;      /* heartbeat: +1 every pass, never reset by LP */
volatile uint32_t cp_lp_bench_edges5 = 0;  /* level CHANGES seen on GPIO5 (touch INT) */
volatile uint32_t cp_lp_bench_edges2 = 0;  /* level CHANGES seen on GPIO2 (expander INT) */
volatile uint32_t cp_lp_bench_level5 = 0;  /* last sampled level, GPIO5 */
volatile uint32_t cp_lp_bench_level2 = 0;  /* last sampled level, GPIO2 */
volatile uint32_t cp_lp_bench_woke = 0;    /* times the HP wake was raised */
volatile uint32_t cp_lp_bench_armed = 0;   /* HP: 1 = raise the HP core on an edge */
volatile uint32_t cp_lp_bench_stop = 0;    /* HP: 1 = leave the loop and halt */

#define CP_LP_BENCH_MAGIC 0x4C423031u /* 'LB01' */
#define CP_LP_BENCH_VER 1u

int main(void) {
  cp_lp_bench_magic = CP_LP_BENCH_MAGIC;
  cp_lp_bench_ver = CP_LP_BENCH_VER;
  cp_lp_bench_stop = 0;
  cp_lp_bench_woke = 0;
  cp_lp_bench_edges5 = 0;
  cp_lp_bench_edges2 = 0;

  /* Both lines are inputs in the RTC domain. The HP side has already configured
   * them as GPIOs; this only enables the RTC-IO input path the LP core reads. */
  ulp_lp_core_gpio_init(LP_IO_NUM_5);
  ulp_lp_core_gpio_input_enable(LP_IO_NUM_5);
  ulp_lp_core_gpio_init(LP_IO_NUM_2);
  ulp_lp_core_gpio_input_enable(LP_IO_NUM_2);

  uint32_t last5 = ulp_lp_core_gpio_get_level(LP_IO_NUM_5);
  uint32_t last2 = ulp_lp_core_gpio_get_level(LP_IO_NUM_2);
  cp_lp_bench_level5 = last5;
  cp_lp_bench_level2 = last2;

  for (;;) {
    cp_lp_bench_hb = cp_lp_bench_hb + 1u;

    const uint32_t l5 = ulp_lp_core_gpio_get_level(LP_IO_NUM_5);
    if (l5 != last5) {
      last5 = l5;
      cp_lp_bench_level5 = l5;
      cp_lp_bench_edges5 = cp_lp_bench_edges5 + 1u;
      if (cp_lp_bench_armed != 0) {
        ulp_lp_core_wakeup_main_processor();
        cp_lp_bench_woke = cp_lp_bench_woke + 1u;
      }
    }

    const uint32_t l2 = ulp_lp_core_gpio_get_level(LP_IO_NUM_2);
    if (l2 != last2) {
      last2 = l2;
      cp_lp_bench_level2 = l2;
      cp_lp_bench_edges2 = cp_lp_bench_edges2 + 1u;
      if (cp_lp_bench_armed != 0) {
        ulp_lp_core_wakeup_main_processor();
        cp_lp_bench_woke = cp_lp_bench_woke + 1u;
      }
    }

    if (cp_lp_bench_stop != 0) break;
  }

  return 0;
}
