#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// cp_lp_core — the ULP-LP-core plumbing, isolated in its own component on
// purpose.
//
// The ULP program is built as a SEPARATE cmake project by IDF's
// ulp_embed_binary() (components/ulp/project_include.cmake:142) and is linked
// into THIS component's archive as binary data. That call has to live in a
// component CMakeLists, so it lives here rather than in crosspoint_app, and
// src/platform/LpCoreService.cpp talks to this plain C API instead of to
// <ulp_lp_core.h>. Consequence, and the reason for the split: the application
// tree never includes a ULP header, so a port to a target without the ULP
// co-processor only has to deal with this one component.
//
// On this SoC (esp32s31) SOC_LP_CORE_SUPPORTED=1
// (components/soc/esp32s31/include/soc/soc_caps.h:57) and the LP core can be
// started by the HP core and run WHILE the HP core is awake — see
// examples/system/ulp/lp_core/inter_cpu_critical_section/, whose README lists
// ESP32-S31 and which has both cores increment one counter concurrently.
// The older "ULP only runs in deep sleep" rule is the ULP-RISC-V coprocessor on
// ESP32-S2/S3 (components/ulp/ulp_riscv), a different peripheral with a
// different API; it does not apply here.
// ---------------------------------------------------------------------------

// 1 when the ULP binary was built and embedded (needs
// CONFIG_ULP_COPROC_ENABLED=y and CONFIG_ULP_COPROC_TYPE_LP_CORE=y), else 0.
// Set by this component's CMakeLists as a PUBLIC compile definition.
#ifndef CP_LP_CORE_BUILT
#define CP_LP_CORE_BUILT 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CP_LP_STATE_NOT_BUILT = 0,  // ULP co-processor not compiled in; nothing to run
    CP_LP_STATE_IDLE,           // binary loaded, program not started yet
    CP_LP_STATE_RUNNING,        // started, LP core has not stored its signature yet
    CP_LP_STATE_DONE,           // LP core ran and wrote its signature + counter
    CP_LP_STATE_ERROR,          // load or start returned an error (see cp_lp_core_last_err)
} cp_lp_state_t;

// Loads the embedded ULP binary into LP RAM and starts the LP core once
// (ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU — a single start, no timer, no GPIO).
// Idempotent: a second call reloads and restarts. Returns ESP_ERR_NOT_SUPPORTED
// when CP_LP_CORE_BUILT is 0.
esp_err_t cp_lp_core_start(void);

// Stops the LP core and disables its wakeup sources. Safe to call at any time.
void cp_lp_core_stop(void);

cp_lp_state_t cp_lp_core_state(void);
esp_err_t cp_lp_core_last_err(void);

// Values written by the ULP program into LP RAM. Read directly by the HP core
// from the reserved RTC window (CONFIG_ULP_COPROC_RESERVE_MEM), so they survive
// a deep-sleep cycle in which the ULP ran — that is the only reason they are
// worth reading from a later session.
uint32_t cp_lp_core_magic(void);
uint32_t cp_lp_core_counter(void);
uint32_t cp_lp_core_loops(void);
// 1 once the ULP program reached the end of its own bounded loop and returned;
// 0 while it is still running or if it was stopped early. Together with
// cp_lp_core_magic() this is what distinguishes "executed" from "loaded".
uint32_t cp_lp_core_done(void);

// Size in bytes of the embedded ULP binary image (0 when not built).
uint32_t cp_lp_core_binary_size(void);

// ---------------------------------------------------------------------------
// r96 LP BENCH PROGRAM (idf/components/cp_lp_core/ulp/main_bench.c)
//
// A SEPARATE ULP program from the one above: unbounded heartbeat, level-change
// counters on two RTC-domain pins, and an optional raise of the HP core. The r87
// program and its `done` semantics are untouched — the "LP core" Debug row and
// the r88 delta are measured against that one.
//
// Only referenced from src/platform/LpCoreService.cpp under CP_LP_BENCH, so with
// that switch at 0 neither this object nor the bench blob is linked in.
//
// Pins, and why only these: LP_IO_NUM_5 = GPIO5 = touch INT, LP_IO_NUM_2 =
// GPIO2 = expander/accel INT. Both are inside SOC_RTCIO_PIN_COUNT (=8) on this
// part, so they are reachable from the LP domain. Nothing else on this board is.
// NO I2C and NO PSRAM: the LP core cannot see PSRAM at all
// (components/ulp/ld/lp_core_riscv.ld:29-37), and the I2C bus has an owner
// (the HP driver) that r96 does not share.
// ---------------------------------------------------------------------------
esp_err_t cp_lp_bench_start(void);   // load + run the bench program (stops any previous run)
void cp_lp_bench_stop(void);         // ask the program to leave its loop, then halt the core
void cp_lp_bench_arm(uint32_t on);   // 1 = raise the HP core when an edge is seen

uint32_t cp_lp_bench_magic(void);    // 'LB01' once the program is executing
uint32_t cp_lp_bench_ver(void);      // program version (1)
uint32_t cp_lp_bench_hb(void);       // heartbeat: monotonic while the LP core runs
uint32_t cp_lp_bench_edges5(void);   // GPIO5 level changes seen
uint32_t cp_lp_bench_edges2(void);   // GPIO2 level changes seen
uint32_t cp_lp_bench_level5(void);   // last sampled GPIO5 level
uint32_t cp_lp_bench_level2(void);   // last sampled GPIO2 level
uint32_t cp_lp_bench_woke(void);     // HP wake requests raised by the program
uint32_t cp_lp_bench_armed(void);    // current arm state
uint32_t cp_lp_bench_bin_size(void); // bench program image size in bytes (0 = not built)

// ---------------------------------------------------------------------------
// r99 SENTRY PROGRAM (idf/components/cp_lp_core/ulp/main_sentry.c)
//
// The instrument `docs/LP_CORE_CAPABILITY.md` §7 step 3 asks for, and the same
// program CP_LP_STANDBY runs in production. Unlike the two programs above it is
// EVENT-DRIVEN: it is armed on LP_TIMER (a sampling period) | LP_IO (GPIO2, the
// expander INT / power key), and the LP core wakes, runs main() once, records
// WHICH source fired, samples the two interrupt-line levels and re-arms.
//
//   mode 0 — RECORD ONLY. Counts wake sources, edges on GPIO2/GPIO5 and
//            debounced power-key assertions. Raises nothing. This is Stage 1:
//            it measures how often GPIO2 brings the LP core back with no
//            decision-making at all.
//   mode 1 — PRE-FILTER. Additionally raises the HP core (one wake request per
//            debounced assertion) so HP can stay in a sleep that does not arm
//            EXT1 on GPIO2 at all. CP_LP_STANDBY uses this.
//
// Enabling LP_IO wake requires GPIO2 to be in the RTC-IO input function, which
// is HP-owned state: cp_lp_sentry_start() takes the pad and cp_lp_sentry_stop()
// returns it. Never leave a run active across a path that reads GPIO2 through
// the normal GPIO driver.
//
// Like cp_lp_bench_*, this is only referenced from LpCoreService.cpp under
// CP_LP_BENCH or from main.cpp under CP_LP_STANDBY, so with both switches at 0
// neither this object nor the sentry blob is linked (the r87 guard).
// ---------------------------------------------------------------------------
esp_err_t cp_lp_sentry_start(uint32_t period_us); // load + run, armed on LP timer | LP IO
void cp_lp_sentry_stop(void);                     // halt the core and give GPIO2 back
void cp_lp_sentry_arm(uint32_t mode);             // 0 = record only, 1 = pre-filter + HP wake

uint32_t cp_lp_sentry_magic(void);      // 'SN01' once the program has run
uint32_t cp_lp_sentry_ver(void);
uint32_t cp_lp_sentry_hb(void);         // +1 per LP wakeup; frozen = not executing
uint32_t cp_lp_sentry_last_cause(void); // raw PMU wakeup-cause bitmap of the last wake
uint32_t cp_lp_sentry_wake_timer(void);
uint32_t cp_lp_sentry_wake_io(void);
uint32_t cp_lp_sentry_wake_other(void);
uint32_t cp_lp_sentry_level2(void);     // last sampled GPIO2 level
uint32_t cp_lp_sentry_level5(void);     // last sampled GPIO5 (FT6336U INT) level
uint32_t cp_lp_sentry_edges2(void);
uint32_t cp_lp_sentry_edges5(void);
uint32_t cp_lp_sentry_low2(void);       // wakeups that sampled GPIO2 low
uint32_t cp_lp_sentry_pressed(void);    // debounced key assertions
uint32_t cp_lp_sentry_woke(void);       // HP wake requests raised (mode 1 only)
uint32_t cp_lp_sentry_bin_size(void);

#ifdef __cplusplus
}
#endif
