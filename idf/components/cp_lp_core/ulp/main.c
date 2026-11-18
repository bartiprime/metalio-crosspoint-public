/*
 * Minimal ULP-LP-core program: prove the LP core executed on demand, and nothing else.
 *
 * Compiled by a nested cmake project with
 * components/ulp/cmake/toolchain-lp-core-riscv.cmake (riscv32-esp-elf-gcc,
 * -march=rv32imc_zicsr_zifencei). IDF's esp32ulp_mapgen.py exports every
 * non-static global below to the HP core as `ulp_cp_lp_<name>`, and
 * ulp_lp_core_halt() is called automatically when main() returns.
 *
 * ORDER MATTERS (r87). The signature is written FIRST, then the counter spins:
 *   - magic = 'LP01' immediately -> the HP core can tell "the LP core booted MY
 *     image" from "the window reads stale or garbage" within microseconds;
 *   - counter then increments in a long, bounded loop -> the HP core can sample
 *     it twice 2 ms apart and get a non-zero delta, which is proof that the LP
 *     core was EXECUTING while the HP core was awake (not merely loaded);
 *   - done = 1 only when the loop finished on its own -> distinguishes "ran to
 *     completion" from "the HP core stopped it early".
 *
 * The loop is deliberately long (hundreds of thousands of increments, tens to
 * hundreds of ms at the LP core's clock), NOT a single increment: a program that
 * finishes before the HP core's first read cannot show a delta, and a delta is
 * the observable that separates "started" from "executed".
 *
 * Those four words live in the RTC window reserved by
 * CONFIG_ULP_COPROC_RESERVE_MEM, which is retained across deep sleep — that is
 * what makes them useful to a later session. Note that
 * ulp_lp_core_load_binary() zeroes the whole window before copying the image in
 * (components/ulp/lp_core/lp_core.c:298), so every start begins from counter == 0,
 * which is what makes the delta meaningful across repeated taps.
 */
#include <stdint.h>

#include "ulp_lp_core.h"

/* Exported to the HP core. volatile: written by the LP core, read by the HP. */
volatile uint32_t cp_lp_magic = 0;
volatile uint32_t cp_lp_counter = 0;
volatile uint32_t cp_lp_loops = 0;
volatile uint32_t cp_lp_done = 0;

/* 'LP01' — so a read-back of garbage (or of a stale/dead window) is
 * distinguishable from a read-back of the LP core's own write. */
#define CP_LP_MAGIC 0x4C503031u

/* Bounded, not an endless spin: the LP core must be able to return and halt
 * itself, so a caller who never stops it cannot leave it burning power forever.
 * Sized so the work outlasts the HP core's 2 ms sampling gap by a wide margin at
 * any plausible LP clock, while still finishing well inside the row's wait. */
#define CP_LP_LOOP_BUDGET 400000u

int main(void) {
  cp_lp_magic = CP_LP_MAGIC;

  for (uint32_t i = 0; i < CP_LP_LOOP_BUDGET; ++i) {
    cp_lp_counter = cp_lp_counter + 1u;
  }

  cp_lp_loops = CP_LP_LOOP_BUDGET;
  cp_lp_done = 1u;
  return 0;
}
