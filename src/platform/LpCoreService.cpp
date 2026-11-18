#include "LpCoreService.h"

#include <cp_lp_core.h>
#include <esp_err.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <Logging.h>

#include <MetalioEInk4PlusBoard.h>
#include <metalio_sleep.h>

#include "activities/RenderLock.h"
#include "platform/PlatformFeatures.h"
#include "CpuStats.h"
#include "debug/UsbShell.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace lpcore {

namespace {
// The signature the ULP program stores once it has finished ('LP01').
constexpr uint32_t kMagic = 0x4C503031u;

State fromNative(const cp_lp_state_t s) {
  switch (s) {
    case CP_LP_STATE_NOT_BUILT:
      return State::NotBuilt;
    case CP_LP_STATE_IDLE:
      return State::Idle;
    case CP_LP_STATE_RUNNING:
      return State::Running;
    case CP_LP_STATE_DONE:
      return State::Done;
    case CP_LP_STATE_ERROR:
      return State::Error;
  }
  return State::Error;
}
}  // namespace

const char* stateName(const State s) {
  switch (s) {
    case State::Disabled:
      return "disabled";
    case State::NotBuilt:
      return "not built";
    case State::Idle:
      return "idle";
    case State::Running:
      return "running";
    case State::Done:
      return "ran";
    case State::Error:
      return "error";
  }
  return "?";
}

Status status() {
  Status st;
#if !CP_LP_CORE
  // Compiled out: do not call into the component at all, so a build with the
  // switch off links none of it (the same binary behaviour as r85).
  st.state = State::Disabled;
  return st;
#else
  st.state = fromNative(cp_lp_core_state());
  st.magic = cp_lp_core_magic();
  st.counter = cp_lp_core_counter();
  st.loops = cp_lp_core_loops();
  st.binSize = cp_lp_core_binary_size();
  st.done = cp_lp_core_done() != 0;
  st.pass = (st.magic == kMagic) && st.done;
  return st;
#endif
}

Status runOnce(const uint32_t sampleGapMs, const uint32_t waitMs) {
#if !CP_LP_CORE
  (void)sampleGapMs;
  (void)waitMs;
  LOG_INF("TEST", "lp core: disabled (CP_LP_CORE=0)");
  return status();
#else
  Status st = status();
  if (st.state == State::NotBuilt) {
    // Honest failure, not a fake pass: the ULP co-processor has to be compiled
    // in before there is a binary to load.
    LOG_INF("TEST", "lp core: not built — set CONFIG_ULP_COPROC_ENABLED=y and "
                    "CONFIG_ULP_COPROC_TYPE_LP_CORE=y, then rebuild");
    return st;
  }

  // The on-demand sequence. Every step below happens between the user's tap and
  // the row's repaint; nothing schedules a later start.
  //   1. park any earlier run (also done inside cp_lp_core_start, twice is fine);
  //   2. load the embedded image and boot the LP core from the HP core;
  //   3. take sample A of its counter immediately;
  //   4. wait the sample gap, take sample B — the difference is live execution;
  //   5. give the bounded program its chance to finish on its own;
  //   6. stop the LP core, then report.
  cp_lp_core_stop();

  const esp_err_t err = cp_lp_core_start();
  if (err != ESP_OK) {
    st = status();
    cp_lp_core_stop();
    LOG_ERR("TEST", "lp core: start failed (%s), state=%s", esp_err_to_name(err), stateName(st.state));
    st.state = State::Error;
    return st;
  }

  const uint32_t sampleA = cp_lp_core_counter();
  vTaskDelay(pdMS_TO_TICKS(sampleGapMs));
  const uint32_t sampleB = cp_lp_core_counter();
  // Unsigned subtraction: wrap-safe and exactly "how much it advanced".
  const uint32_t delta = sampleB - sampleA;

  // Bounded wait for the program to reach its own end. It stops waiting early if
  // the signature never appeared (dead LP core) so a FAIL is reported promptly
  // instead of burning the whole budget. Finishing late is NOT a failure — the
  // delta already proved execution — so this only decides `done`.
  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(waitMs);
  while (cp_lp_core_done() == 0 && cp_lp_core_magic() == kMagic && xTaskGetTickCount() < deadline) {
    vTaskDelay(1);
  }

  Status ran = status();  // read the LP-RAM results before stopping
  cp_lp_core_stop();      // clean stop: wakeup sources disabled, core parked
  ran.delta = delta;
  ran.pass = (ran.magic == kMagic) && (ran.done || delta > 0);
  ran.state = ran.pass ? State::Done : State::Error;

  LOG_INF("TEST", "lp core: %s magic=0x%08lx delta=%lu counter=%lu loops=%lu done=%d bin=%lu B",
          ran.pass ? "pass" : "FAIL", static_cast<unsigned long>(ran.magic),
          static_cast<unsigned long>(ran.delta), static_cast<unsigned long>(ran.counter),
          static_cast<unsigned long>(ran.loops), static_cast<int>(ran.done),
          static_cast<unsigned long>(ran.binSize));
  return ran;
#endif
}

void stop() {
#if CP_LP_CORE
  cp_lp_core_stop();
#endif
}

// ===========================================================================
// r96 LP BENCH
// ===========================================================================
#if CP_LP_BENCH

namespace {
// 'LB01' — the bench program's own signature (ulp/main_bench.c).
constexpr uint32_t kBenchMagic = 0x4C423031u;
// Gap between the two heartbeat samples. Longer than the r88 2 ms sample on
// purpose: the divisor is a wall-clock measurement, so a 20 ms window gives a
// 5 % worst-case timing error instead of a 50 % one.
constexpr uint32_t kBenchGapMs = 20;
}  // namespace

bool startBench() {
#if !CP_LP_CORE
  return false;
#else
  if (cp_lp_bench_bin_size() == 0) {
    LOG_INF("TEST", "lp bench: no bench program in this build (CP_LP_CORE_BUILT=0)");
    return false;
  }
  const esp_err_t err = cp_lp_bench_start();
  if (err != ESP_OK) {
    LOG_ERR("TEST", "lp bench: start failed (%s)", esp_err_to_name(err));
    return false;
  }
  cp_lp_bench_arm(1);
  return true;
#endif
}

void stopBench() {
#if CP_LP_CORE
  cp_lp_bench_stop();
#endif
}

Sentry readSentry() {
  Sentry s;
#if !CP_LP_CORE
  return s;
#else
  s.built = cp_lp_bench_bin_size() != 0;
  s.magic = cp_lp_bench_magic();
  s.ver = cp_lp_bench_ver();
  s.hb = cp_lp_bench_hb();
  s.edges5 = cp_lp_bench_edges5();
  s.edges2 = cp_lp_bench_edges2();
  s.level5 = cp_lp_bench_level5();
  s.level2 = cp_lp_bench_level2();
  s.woke = cp_lp_bench_woke();
  s.binSize = cp_lp_bench_bin_size();
  return s;
#endif
}

Bench bench(const int runsIn) {
  Bench b;
  b.benchBin = cp_lp_bench_bin_size();
  b.coreBin = cp_lp_core_binary_size();
  b.built = b.benchBin != 0;
  b.sampleGapMs = kBenchGapMs;
  if (!b.built) {
    LOG_INF("TEST", "lpbench: not built — CONFIG_ULP_COPROC_ENABLED / CP_LP_CORE are off");
    return b;
  }
  const int runs = (runsIn < 1) ? 1 : (runsIn > 32 ? 32 : runsIn);

  uint64_t deltaSum = 0;
  bool allMagic = true;
  for (int i = 0; i < runs; ++i) {
    // Every run reloads the image, and ulp_lp_core_load_binary() zeroes the whole
    // reserved window first, so hb starts at 0 on every run.
    if (cp_lp_bench_start() != ESP_OK) {
      allMagic = false;
      continue;
    }
    cp_lp_bench_arm(0);  // a bench run must not raise the HP core
    const uint32_t hbA = cp_lp_bench_hb();
    const int64_t t0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(kBenchGapMs));
    const uint32_t hbB = cp_lp_bench_hb();
    const int64_t elapsedUs = esp_timer_get_time() - t0;
    cp_lp_bench_stop();

    if (cp_lp_bench_magic() != kBenchMagic) allMagic = false;
    const uint32_t d = hbB - hbA;  // wrap-safe unsigned difference
    if (i == 0 || d < b.deltaMin) b.deltaMin = d;
    if (i == 0 || d > b.deltaMax) b.deltaMax = d;
    deltaSum += d;
    ++b.runs;

    // counts/s for this run, from the MEASURED elapsed time, not the nominal gap.
    const double rate = elapsedUs > 0 ? (static_cast<double>(d) * 1000000.0) / static_cast<double>(elapsedUs) : 0.0;
    if (i == 0 || rate < b.rateMin) b.rateMin = rate;
    if (i == 0 || rate > b.rateMax) b.rateMax = rate;
    LOG_INF("TEST", "lpbench run %d/%d delta=%lu gap=%lldus rate=%.0f/s", i + 1, runs,
            static_cast<unsigned long>(d), static_cast<long long>(elapsedUs), rate);
  }
  if (b.runs > 0) {
    b.deltaMean = static_cast<uint32_t>(deltaSum / static_cast<uint64_t>(b.runs));
    // Mean rate from the mean delta over the nominal gap: the per-run rates above
    // already carry the jitter, this line is the headline number.
    b.rateMean = (static_cast<double>(b.deltaMean) * 1000.0) / static_cast<double>(kBenchGapMs);
  }

  // Blocker #1 from planning/BACKLOG.md LPC1: "LP program completion inside the
  // 100 ms budget". Time the r87 bounded program to its own `done` bit instead of
  // reporting it as a pass/fail at an arbitrary deadline.
  cp_lp_bench_stop();
  if (cp_lp_core_start() == ESP_OK) {
    const int64_t t0 = esp_timer_get_time();
    while (cp_lp_core_done() == 0 && cp_lp_core_magic() == kMagic && (esp_timer_get_time() - t0) < 2000000) {
      vTaskDelay(1);
    }
    if (cp_lp_core_done() != 0) b.doneMs = (esp_timer_get_time() - t0) / 1000;
    cp_lp_core_stop();
  }

  b.pass = allMagic && b.runs == runs && b.deltaMean > 0;
  LOG_INF("TEST",
          "lpbench: %s runs=%d delta min/mean/max=%lu/%lu/%lu gap=%lums rate mean=%.0f min=%.0f max=%.0f "
          "done=%lldms benchbin=%luB corebin=%luB",
          b.pass ? "pass" : "FAIL", b.runs, static_cast<unsigned long>(b.deltaMin),
          static_cast<unsigned long>(b.deltaMean), static_cast<unsigned long>(b.deltaMax),
          static_cast<unsigned long>(b.sampleGapMs), b.rateMean, b.rateMin, b.rateMax,
          static_cast<long long>(b.doneMs), static_cast<unsigned long>(b.benchBin),
          static_cast<unsigned long>(b.coreBin));
  return b;
}

#else  // !CP_LP_BENCH — the switch is off: honest, cheap no-ops.

bool startBench() { return false; }
void stopBench() {}
Sentry readSentry() { return Sentry{}; }

Bench bench(int runsIn) {
  (void)runsIn;
  Bench b;
  // r97: the command that used to call this (`lpbench`) only exists when CP_LP_BENCH is 1,
  // so this stub is unreachable in a shipping build. The message no longer names the command:
  // a shipping image must not carry a command name it cannot answer.
  LOG_INF("TEST", "bench: not built in this configuration (CP_LP_BENCH=0)");
  return b;
}

#endif  // CP_LP_BENCH


// ===========================================================================
// r99 SENTRY — the wake-source instrument, and the LP program CP_LP_STANDBY runs.
//
// Present when either the bench switch (for `lpsentry`) or the standby switch
// (for the standby loop) is on. With both at 0 this whole block and the
// cp_lp_sentry_* object it calls are compiled/linked out.
// ===========================================================================
#if CP_LP_BENCH || CP_LP_STANDBY

namespace {
// 'SN01' — the sentry program's own signature (ulp/main_sentry.c).
constexpr uint32_t kSentryMagic = 0x534E3031u;
// Sampling period. 20 ms is short enough that a human press (>= 40 ms by the HP
// side's own bounce filter, METALIO_WAKE_BOUNCE_MS) spans at least two samples
// before the 3-sample debounce in the ULP program, and long enough that the LP
// core is asleep ~99.9 % of the time: at the r96-measured ~1.7 M iterations/s a
// pass is a few microseconds.
constexpr uint32_t kSentryPeriodUs = 20000;
}  // namespace

bool sentryBegin(const bool arm) {
  // NOT guarded on CP_LP_CORE: the sentry is the standby feature, and it must work
  // when only CP_LP_STANDBY is on. What it needs is the ULP program in the image,
  // which is exactly what the blob size says.
  if (cp_lp_sentry_bin_size() == 0) {
    LOG_INF("TEST", "lp sentry: no sentry program in this build (CP_LP_CORE_BUILT=0)");
    return false;
  }
  const esp_err_t err = cp_lp_sentry_start(kSentryPeriodUs);
  if (err != ESP_OK) {
    LOG_ERR("TEST", "lp sentry: start failed (%s)", esp_err_to_name(err));
    return false;
  }
  cp_lp_sentry_arm(arm ? 1 : 0);
  return true;
}

void sentryEnd() { cp_lp_sentry_stop(); }

LpSentry sentryRead() {
  LpSentry s;
  s.built = cp_lp_sentry_bin_size() != 0;
  s.magic = cp_lp_sentry_magic();
  s.ver = cp_lp_sentry_ver();
  s.hb = cp_lp_sentry_hb();
  s.lastCause = cp_lp_sentry_last_cause();
  s.wakeTimer = cp_lp_sentry_wake_timer();
  s.wakeIo = cp_lp_sentry_wake_io();
  s.wakeOther = cp_lp_sentry_wake_other();
  s.level2 = cp_lp_sentry_level2();
  s.level5 = cp_lp_sentry_level5();
  s.edges2 = cp_lp_sentry_edges2();
  s.edges5 = cp_lp_sentry_edges5();
  s.low2 = cp_lp_sentry_low2();
  s.pressed = cp_lp_sentry_pressed();
  s.woke = cp_lp_sentry_woke();
  s.binSize = cp_lp_sentry_bin_size();
  s.alive = s.magic == kSentryMagic;
  return s;
}

#else  // both switches off — honest no-ops, and no reference to the component.

bool sentryBegin(const bool arm) {
  (void)arm;
  return false;
}
void sentryEnd() {}
LpSentry sentryRead() { return LpSentry{}; }

#endif  // CP_LP_BENCH || CP_LP_STANDBY

// ===========================================================================
// r96 LP SLEEP SLICE — one real light-sleep slice with the LP core running.
// ===========================================================================
#if CP_LP_BENCH

namespace {

// 0.8 s from the request to the slice: long enough for the shell's answer frame
// to leave the wire (the whole reason this is deferred at all).
constexpr uint32_t kSliceDelayMs = 800;
constexpr uint32_t kSliceMinSec = 5;
// r96.2: 120 -> 60. The longer the slice, the longer an operator stares at a dead
// console; nothing in the measurement needs more than a minute.
constexpr uint32_t kSliceMaxSec = 60;
// r96.2 (D3) — THE BOUND. No single HP sleep call is ever armed for longer than this,
// and the total slice is the sum of the chunks. It is the same timer + EXT1 pair that
// metalioStandbyLoop() has been running on this device for hours, and now it is the
// only thing that can end the sleep: the LP core is stopped for the slice (see
// pollSleepSlice), so no ULP wake can substitute for it.
constexpr uint32_t kHpSleepChunkMs = 5000;

SleepSlice s_slice;
uint32_t s_sliceAtMs = 0;
uint32_t s_sliceSec = 0;

const char* wakeCauseName(const esp_sleep_wakeup_cause_t c) {
  switch (c) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "undefined (never slept)";
    case ESP_SLEEP_WAKEUP_ALL: return "all";
    case ESP_SLEEP_WAKEUP_EXT0: return "ext0";
    case ESP_SLEEP_WAKEUP_EXT1: return "ext1 (GPIO2 wake line)";
    case ESP_SLEEP_WAKEUP_TIMER: return "timer (slice end)";
    case ESP_SLEEP_WAKEUP_TOUCHPAD: return "touchpad";
    case ESP_SLEEP_WAKEUP_ULP: return "ulp";
    case ESP_SLEEP_WAKEUP_GPIO: return "gpio";
    case ESP_SLEEP_WAKEUP_UART: return "uart";
    default: return "other";
  }
}

}  // namespace

bool requestSleepSlice(const uint32_t secIn) {
  SleepSlice& r = s_slice;
  r = SleepSlice{};
  r.sec = (secIn == 0) ? 60u : secIn;
  if (r.sec < kSliceMinSec) r.sec = kSliceMinSec;
  if (r.sec > kSliceMaxSec) r.sec = kSliceMaxSec;

#if !CP_LPSLEEP_ALLOW_HP_SLEEP
  // r96.2 (D3): the hard off switch. `lpsleep 60` on r96.1 put the HP core into a
  // light sleep it never came back from and lost the arm record, so the command is
  // refused outright when the bounded-wake path is not trusted in this build.
  //
  // r96.3 (D3): DEFAULT IS NOW REFUSED, and the cheap hypothesis was tested and refuted.
  // The guess was that this bench reaches sleep by a different door from standby --
  // esp_light_sleep_start() directly, bypassing __wrap_pmu_sleep_start() and the MPLL/bias
  // restore. It does not: the call at the bottom of pollSleepSlice() is
  // metalio_sleep_light_sleep(), the same one metalioStandbyLoop() makes, and that is the
  // only function that opens s_restore_armed for __wrap_rtc_clk_mpll_configure().
  //
  // The r96.2 device log also holds a `lpsleep: pass sec=60 slept=60000ms
  // asleep=60049471us cause=timer` line, so a slice can complete. But a re-run this round
  // (`lpsleep 15`, same device) left NO lpsleep record at all and stopped answering the
  // shell; recovery was closing the port (a reset). The failure is not deterministic and
  // can occur before the first flushed record, i.e. inside startBench()/arm rather than in
  // sleep. Until the entry path is identical to standby's -- it is not yet: standby masks
  // the IO expander to the power key and disables the RTC alarm, this does neither -- and a
  // slice has been observed to leave the shell answering, refusing is the only honest
  // default.
  r.reason = "the HP light-sleep slice is disabled in this build (CP_LPSLEEP_ALLOW_HP_SLEEP=0): "
             "its entry path is standby's, but a slice has never left the USB shell answering; "
             "use `lpbench` (LP core, HP awake)";
  return false;
#endif

  if (cp_lp_bench_bin_size() == 0) {
    r.reason = "no bench program in this build (CP_LP_CORE_BUILT=0)";
    return false;
  }
  // R2/R4 (plan §6): never start a slice while the renderer owns the framebuffer
  // or while a drive just finished. Both are askable from the main task only,
  // which is where both callers run.
  if (RenderLock::peek()) {
    r.reason = "the render task holds RenderLock right now; retry when the screen is idle";
    return false;
  }
  // r96.1 D3: this asked the DURATION of the last drive and compared it to 1000 ms as
  // if it were an AGE. A drive here lasts 350-990 ms, so `since < 1000` was true after
  // every ordinary drive and `lpsleep` was refused forever ("a panel drive finished
  // less than 1 s ago; retry"), while a drive slower than a second would have let a
  // slice start immediately -- the exact opposite of the intent. The age is a
  // different number and comes from its own accessor; ULONG_MAX = never driven.
  const unsigned long sinceDrive = usbshell::lastPanelDriveAgeMs();
  if (usbshell::lastPanelJournal()[0] != '\0' && sinceDrive < 1000) {
    r.reason = "a panel drive finished less than 1 s ago; retry";
    return false;
  }
  // R5: an invalid wake GPIO would mean an unending slice. Refuse, exactly as
  // metalioStandbyLoop() does rather than sleeping unwakeably.
  const auto pin = static_cast<gpio_num_t>(freeink::metalio4plus::IO_EXPANDER_INT_GPIO);
  if (!esp_sleep_is_valid_wakeup_gpio(pin)) {
    r.reason = "GPIO2 is not a valid wake GPIO on this part; refusing to sleep";
    return false;
  }
  if (s_sliceAtMs != 0) {
    r.reason = "a slice is already armed";
    return false;
  }
  s_sliceSec = r.sec;
  s_sliceAtMs = millis() + kSliceDelayMs;
  return true;
}

void pollSleepSlice() {
  if (s_sliceAtMs == 0) return;
  if (static_cast<long>(millis() - s_sliceAtMs) < 0) return;
  s_sliceAtMs = 0;
  using namespace freeink::metalio4plus;
  SleepSlice& r = s_slice;
  r.sec = s_sliceSec;

  // r96.2 (D3) — WHY EVERY RECORD BELOW IS FLUSHED IMMEDIATELY.
  // /.crosspoint/debug.log is NOT a live log: logSdFlush() dumps a RAM ring on demand.
  // The r96.1 hang lost every line of this command, including the "armed" LOG_INF at
  // :417, because the ring was still in RAM when the device stopped answering and was
  // reset by the operator. logSdFlush() before each phase is what makes the next
  // occurrence diagnosable instead of a mystery.

  // 1. Sample the LP bench program, then STOP IT. This is the r96.2 change of
  //    substance: the r96.1 slice left the LP core running with cp_lp_bench_arm(1)
  //    (raise the HP core on an edge) while the HP core went down -- the one thing
  //    standby never does, because metalioStandbyLoop() sleeps with the LP core
  //    parked. Standby's slice (EXT1 + RTC timer, no LP core) is the only light-sleep
  //    configuration proven on this device, so the bench now measures the LP core
  //    either side of a slice instead of through it. Nothing here can strand the HP
  //    core that standby could not.
  if (!startBench()) {
    r.reason = "the LP bench program would not start";
    LOG_ERR("TEST", "lpsleep: abort — bench program did not start");
    logSdFlush();
    return;
  }
  r.magic = cp_lp_bench_magic();
  r.hb0 = cp_lp_bench_hb();
  stopBench();  // before the slice: no ULP wake path can hold or raise the HP core
  LOG_INF("TEST", "lpsleep: lp core sampled hb0=%lu magic=0x%08lx, stopped before the slice",
          static_cast<unsigned long>(r.hb0), static_cast<unsigned long>(r.magic));
  logSdFlush();

  // 2. Arm: the accel's INT1 is wire-OR'd onto GPIO2, so release it first or it
  //    can hold the line low and fire the wake instantly.
  const auto pin = static_cast<gpio_num_t>(IO_EXPANDER_INT_GPIO);
  releaseAccelIntLine();
  if (!waitForInterruptLineIdle(300)) {
    r.reason = "the GPIO2 INT line was still low at arm time; nothing was slept";
    LOG_ERR("TEST", "lpsleep: abort — INT line not idle");
    logSdFlush();
    return;
  }

  // 3. The slice, in bounded chunks. Every HP sleep call is armed for at most
  //    kHpSleepChunkMs, so the longest a single call can be in the dark is 5 s, and
  //    the total is the requested seconds. Both sources are re-armed per chunk
  //    because esp_sleep_disable_wakeup_source(ALL) clears the previous arm.
  const uint32_t totalMs = r.sec * 1000u;
  uint32_t sleptMs = 0;
  int64_t asleepUs = 0;
  esp_err_t lastErr = ESP_OK;
  esp_sleep_wakeup_cause_t cause = ESP_SLEEP_WAKEUP_UNDEFINED;
  bool armedAtLeastOnce = false;
  while (sleptMs < totalMs) {
    const uint32_t chunk = (totalMs - sleptMs) < kHpSleepChunkMs ? (totalMs - sleptMs) : kHpSleepChunkMs;
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    const esp_err_t ext1 = esp_sleep_enable_ext1_wakeup_io(1ULL << static_cast<unsigned>(pin),
                                                          ESP_EXT1_WAKEUP_ANY_LOW);
    const esp_err_t timer = esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(chunk) * 1000ULL);
    r.armErr = (ext1 != ESP_OK) ? ext1 : timer;
    if (ext1 != ESP_OK || timer != ESP_OK) {
      // No bounded wake source => never sleep. This is the guard that makes the
      // command unable to strand the device: without the timer there is nothing
      // that brings the HP core back.
      r.reason = "arming failed (see armErr); nothing was slept";
      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
      LOG_ERR("TEST", "lpsleep: abort — arm ext1=%d timer=%d", static_cast<int>(ext1), static_cast<int>(timer));
      logSdFlush();
      return;
    }
    LOG_INF("TEST", "lpsleep: armed sec=%lu chunk=%lums ext1=gpio%d ANY_LOW + timer; lpcore=stopped hb0=%lu",
            static_cast<unsigned long>(r.sec), static_cast<unsigned long>(chunk), static_cast<int>(pin),
            static_cast<unsigned long>(r.hb0));
    logSdFlush();
    armedAtLeastOnce = true;

    const int64_t t0 = esp_timer_get_time();
    lastErr = metalio_sleep_light_sleep(0);
    asleepUs += esp_timer_get_time() - t0;
    cause = esp_sleep_get_wakeup_cause();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    // Record the wake before anything else can go wrong, so the chunk that came back
    // is on the card even if the next one does not.
    LOG_INF("TEST", "lpsleep: chunk back slept=%lums cause=%s err=%d", static_cast<unsigned long>(chunk),
            wakeCauseName(cause), static_cast<int>(lastErr));
    logSdFlush();
    if (lastErr != ESP_OK) break;          // rejected: stop, do not retry in a loop
    if (cause == ESP_SLEEP_WAKEUP_EXT1) break;  // a touch woke us: that is the answer
    sleptMs += chunk;
  }

  // 4. Restart the LP program and sample it again: the point of the bench is that the
  //    LP core still works after a light sleep, and with the program stopped across
  //    the slice that is now provable rather than asserted.
  uint32_t hb1 = 0;
  if (startBench()) {
    hb1 = cp_lp_bench_hb();
    stopBench();
  }

  r.ran = armedAtLeastOnce;
  r.hpAsleepUs = asleepUs;
  r.wakeCause = static_cast<int>(cause);
  r.hb1 = hb1;
  r.dhb = hb1 - r.hb0;  // wrap-safe; 0 when the program could not be restarted
  r.sleepErr = lastErr;
  r.ok = armedAtLeastOnce && (lastErr == ESP_OK) && (sleptMs >= totalMs || cause == ESP_SLEEP_WAKEUP_EXT1);

  cpustats::noteLightSleepUs(asleepUs);

  LOG_INF("TEST",
          "lpsleep: %s sec=%lu slept=%lums asleep=%lldus cause=%s lp stopped-for-slice hb %lu->%lu (+%lu) err=%d",
          r.ok ? "pass" : "FAIL", static_cast<unsigned long>(r.sec), static_cast<unsigned long>(sleptMs),
          static_cast<long long>(asleepUs), wakeCauseName(cause), static_cast<unsigned long>(r.hb0),
          static_cast<unsigned long>(r.hb1), static_cast<unsigned long>(r.dhb), r.sleepErr);
  logSdFlush();
}

const SleepSlice& lastSleepSlice() { return s_slice; }

#else  // !CP_LP_BENCH

bool requestSleepSlice(uint32_t sec) {
  (void)sec;
  return false;
}
void pollSleepSlice() {}
const SleepSlice& lastSleepSlice() {
  static const SleepSlice kNone;
  return kNone;
}

#endif  // CP_LP_BENCH

}  // namespace lpcore

