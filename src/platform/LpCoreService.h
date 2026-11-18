#pragma once

#include <stdint.h>

#include "PlatformFeatures.h"

// Thin facade over the ULP-LP-core component (idf/components/cp_lp_core).
//
// The application tree does not include any ULP header: everything IDF-version
// and SoC-specific is behind cp_lp_core.h. This file adds only the two things a
// debug row needs — a human-readable status and a "run it once and report"
// helper — plus the switch check.
//
// NOT WIRED TO ANYTHING. No sleep path, no page-idle path, no input path calls
// this. The only caller is the Debug page. See
// docs/engineering/LPCORE_AND_PPA_FRAMEWORK.md.
namespace lpcore {

enum class State {
  Disabled,  // CP_LP_CORE == 0: compiled out, no call made
  NotBuilt,  // switch is on but the ULP co-processor is not in sdkconfig
  Idle,      // binary loaded, program not started
  Running,   // started, signature not stored yet
  Done,      // LP core ran and wrote its signature
  Error,
};

struct Status {
  State state = State::Disabled;
  uint32_t magic = 0;
  uint32_t counter = 0;  // counter value when the run was stopped / completed
  uint32_t loops = 0;
  uint32_t binSize = 0;
  // Counter movement measured across the sample gap while the LP core was
  // running. This — not "the binary loaded" — is the evidence that the LP core
  // actually executed on this silicon.
  uint32_t delta = 0;
  bool done = false;  // the ULP program reached the end of its own bounded loop
  bool pass = false;  // magic matched AND (delta > 0 or the program finished)
};

// Current state without starting anything. Cheap; safe to call from a repaint.
Status status();

// ON DEMAND, the whole point of the row: stop any previous run, load the image,
// start the LP core, sample the counter twice `sampleGapMs` apart, wait up to
// `waitMs` for the program to finish on its own, then STOP the LP core and
// report. Nothing here runs at boot, on a timer, or from a background task —
// this function is the only place in the firmware that starts the LP core, and
// its only caller is the Debug row's tap.
Status runOnce(uint32_t sampleGapMs = 2, uint32_t waitMs = 100);

// Stop the LP core and disable its wakeup sources. runOnce() calls this itself
// before and after the run; it is exposed so a future caller can park the core.
void stop();

// "disabled", "not built", "idle", "ran", "error" — for the debug row value.
const char* stateName(State s);

// ===========================================================================
// r96 LP BENCH (CP_LP_BENCH) — the measurement half.
//
// Everything below drives the SECOND ULP program (cp_lp_bench_*), never the r87
// bounded one, except bench()'s last step which runs the r87 program once to
// time how long it takes to finish. Nothing here runs at boot, on a timer, or
// from a background task: the callers are the `lpbench` / `lpsleep` shell
// commands and the two Debug rows.
// ===========================================================================

// Result of `lpbench [n]`: n timed runs of the heartbeat program, plus one
// completion timing on the r87 bounded program.
struct Bench {
  bool built = false;      // the ULP co-processor is in this build
  int runs = 0;            // runs actually taken
  uint32_t sampleGapMs = 0;  // nominal gap between the two heartbeats
  uint32_t deltaMin = 0;   // heartbeat advance per run
  uint32_t deltaMean = 0;
  uint32_t deltaMax = 0;
  double rateMean = 0.0;   // counts/s over the measured gap
  double rateMin = 0.0;
  double rateMax = 0.0;
  // The r87 bounded program, timed from start to its own `done` bit. -1 = never
  // observed inside the wait budget (which is the r88 finding at a 100 ms
  // deadline, restated as a number rather than a FAIL).
  int64_t doneMs = -1;
  uint32_t benchBin = 0;   // bytes of the bench program image
  uint32_t coreBin = 0;    // bytes of the r87 program image
  bool pass = false;       // magic matched on every run and the rate was non-zero
};

Bench bench(int runs);

// What the bench program left in LP RAM. Read AFTER a sleep, which is the whole
// point: a heartbeat that advanced means the LP core ran through the HP's
// light-sleep slice.
struct Sentry {
  bool built = false;
  uint32_t magic = 0;
  uint32_t ver = 0;
  uint32_t hb = 0;
  uint32_t edges5 = 0;
  uint32_t edges2 = 0;
  uint32_t level5 = 0;
  uint32_t level2 = 0;
  uint32_t woke = 0;
  uint32_t binSize = 0;
};

// Load + start the bench program and arm the HP wake on an edge.
bool startBench();
void stopBench();
Sentry readSentry();

// ===========================================================================
// r99 SENTRY (CP_LP_BENCH for `lpsentry`, CP_LP_STANDBY for standby)
//
// Drives the THIRD ULP program (cp_lp_sentry_*, idf/components/cp_lp_core/
// ulp/main_sentry.c): event-driven on LP_TIMER | LP_IO(GPIO2), one main() per
// wakeup, records which source fired. Nothing here runs at boot or on a timer.
// ===========================================================================

// What the sentry program left in LP RAM. `alive` is the magic match: a false
// `alive` with a non-zero hb is impossible, and a true `alive` with a frozen hb
// across a sleep means the LP core stopped executing — the failure mode the r96
// round could not distinguish from a successful pass.
struct LpSentry {
  bool built = false;
  bool alive = false;
  uint32_t magic = 0;
  uint32_t ver = 0;
  uint32_t hb = 0;         // +1 per LP wakeup
  uint32_t lastCause = 0;  // raw PMU wakeup-cause bitmap
  uint32_t wakeTimer = 0;  // computed: woke on the LP timer
  uint32_t wakeIo = 0;     // computed: woke on LP_IO (GPIO2)
  uint32_t wakeOther = 0;
  uint32_t level2 = 0;     // last sampled GPIO2 level (0 = line asserted)
  uint32_t level5 = 0;     // last sampled GPIO5 level (FT6336U INT)
  uint32_t edges2 = 0;
  uint32_t edges5 = 0;
  uint32_t low2 = 0;       // wakeups that sampled GPIO2 low
  uint32_t pressed = 0;    // debounced key assertions
  uint32_t woke = 0;       // HP wake requests raised (arm mode only)
  uint32_t binSize = 0;
};

// Load + start the sentry at a 20 ms LP sampling period. `arm` = false is the
// Stage 1 instrument (records, raises nothing); true additionally raises the HP
// core on a debounced power-key assertion (what CP_LP_STANDBY uses).
// gpio2 IS TAKEN INTO THE RTC-IO FUNCTION for the duration — sentryEnd() gives
// it back. Returns false when the ULP is not in the build or the load failed.
bool sentryBegin(bool arm);
void sentryEnd();
LpSentry sentryRead();

// ---------------------------------------------------------------------------
// ONE real HP light-sleep slice with the LP bench program running (r96).
//
// This is the pivot measurement of the whole round: does the LP core keep
// counting while the HP core is in light sleep? (plan §2 A5, §8 row 2.)
//
// SEQUENCING. requestSleepSlice() validates and ARMS a deadline ~0.8 s out; the
// slice itself runs from pollSleepSlice(), which main.cpp's loop() calls. That
// split exists so a shell command can ANSWER BEFORE the device sleeps — the same
// shape as Kind::Reboot. Both callers (the `lpsleep` command and the Debug row)
// go through requestSleepSlice().
//
// WHAT IT DELIBERATELY DOES NOT DO:
//   * it does not park the panel, drop the rails or latch the EPD bus. Those are
//     metalioStandbySleep()'s job and they would make the slice a standby test
//     rather than an LP-core test. The panel keeps its bistable image;
//   * it does not mask the expander keys, so ANY GPIO2 source (power, volume,
//     charger, RTC alarm, IMU) can end the slice early. A wake that is not the
//     timer is reported with its cause instead of being retried.
// ---------------------------------------------------------------------------
struct SleepSlice {
  bool ran = false;
  bool ok = false;
  const char* reason = nullptr;  // refusal or abort reason
  uint32_t sec = 0;
  uint32_t hb0 = 0, hb1 = 0, dhb = 0;
  uint32_t edges5 = 0, edges2 = 0, woke = 0, level5 = 0, level2 = 0;
  uint32_t magic = 0;
  int64_t hpAsleepUs = 0;
  int wakeCause = -1;   // esp_sleep_wakeup_cause_t; -1 = never slept
  int32_t armErr = 0;   // esp_err_t from arming
  int32_t sleepErr = 0; // esp_err_t from the sleep call
};

// Validates and arms the slice. Returns false (with lastSleepSlice().reason set)
// when it refuses. Defaults to 60 s; clamped to 5..120 s.
bool requestSleepSlice(uint32_t sec = 60);
void pollSleepSlice();  // call once per main-loop iteration
const SleepSlice& lastSleepSlice();



}  // namespace lpcore
