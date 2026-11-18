// r101 -- the cycle reset: the history counters restart at the same gated
// charge -> discharge edge that re-seeds the estimate window, instead of waiting for
// the operator to press Reset. This test drives the REAL BatteryStats.cpp against a
// model of NVS and of the charger status, one 30 s sample at a time.
//
// What it pins down:
//   * a plug that never reaches CHARGE_DWELL_MS does NOT wipe the counters;
//   * neither does a charge flag that flickers for a single sample;
//   * a real unplug zeroes awake / sleep / total / the charge session, and the fresh
//     cycle starts counting from that sample;
//   * the zeroed record is WRITTEN FORCED at the edge, so a reboot one second later
//     reads the new cycle, not the old one;
//   * the Reset button still clears everything, still persists, and still works
//     while the pack is charging (and does not disarm the next automatic reset);
//   * a charge that begins during standby is credited from the first charging
//     sample, which is the only dwell this code can honestly measure;
//   * the first boot / first unplug with no previous record is a no-op, not a fault.
//
// Built by run.sh; not part of test/CMakeLists.txt (same standalone shape as
// test/bq27220_gaugefix), because it compiles one production .cpp against stubs.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "stubs/display/EpdFlashGuard.h"
#include "stubs/HalPowerManager.h"
#include "stubs/Logging.h"
#include "stubs/Preferences.h"

#include "../../src/util/BatteryStats.cpp"

namespace {

int checksRun = 0;
int checksFailed = 0;

#define CHECK(condition)                                               \
  do {                                                                 \
    ++checksRun;                                                       \
    if (!(condition)) {                                                \
      ++checksFailed;                                                  \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
    }                                                                  \
  } while (0)

constexpr uint32_t SEC = 1000u;
constexpr uint32_t MIN = 60u * SEC;
constexpr uint32_t HOUR = 60u * MIN;

void state(const uint16_t soc, const bool charging) { powerManager.hostSet(soc, charging); }

// One sample: advance the clock by the accounting period and let update() read the
// gauge. Exactly the cadence the device runs at (SAMPLE_MS = 30 s).
void tick() {
  hostMillis() += 30000u;
  batteryStats.update();
}

// `minutes` worth of samples at the current gauge state.
void run(const int minutes) {
  for (int i = 0; i < minutes * 2; ++i) tick();
}

bool logHas(const char* needle) {
  for (const auto& line : hostLogLines) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

void section(const char* name) { std::printf("-- %s\n", name); }

// Every scenario starts from an empty NVS and a fresh object. The clock is NOT reset:
// millis() is monotonic on the device, so letting it run keeps the unsigned
// arithmetic honest.
//
// The object is re-created because BatteryStats is a process-wide singleton on the
// device and begin() intentionally keeps whatever record it already holds when NVS
// holds nothing valid (loaded_ is only false on the very first begin() of a boot).
// Reusing the instance here would leak the previous scenario's record into this one.
void fresh() {
  hostNvsReset();
  hostLogLines.clear();
  batteryStats = BatteryStats{};
  powerManager.hostSetPostWake(false);
  state(80, false);
  batteryStats.begin();
}

void testBriefPlugKeepsCounters() {
  section("a plug shorter than the dwell gate keeps the counters");
  fresh();
  state(80, false);
  run(30);
  const uint32_t before = batteryStats.snapshot().awake.timeMs;
  CHECK(before > 25 * MIN);

  state(79, true);
  run(4);  // 4 min on the cable: under CHARGE_DWELL_MS (5 min)
  const int writesBefore = hostNvsWrites();
  state(79, false);
  tick();  // candidate edge, rejected for dwell
  tick();  // the sample that would have confirmed it
  CHECK(batteryStats.snapshot().awake.timeMs >= before);
  CHECK(batteryStats.snapshot().total.timeMs >= before);
  CHECK(logHas("too short to be a charge cycle"));
  // ...and nothing was force-written on the rejected edge: the throttle still holds.
  CHECK(hostNvsWrites() - writesBefore <= 1);
}

void testGlitchedReadKeepsCounters() {
  section("one non-charging sample is not an unplug");
  fresh();
  state(90, false);
  run(40);
  const uint32_t before = batteryStats.snapshot().awake.timeMs;
  CHECK(before > 35 * MIN);

  state(90, true);
  run(10);  // a real 10 min charge, gate satisfied
  state(90, false);
  tick();  // candidate edge -> ARMED, not taken
  CHECK(batteryStats.snapshot().awake.timeMs >= before);
  state(90, true);
  tick();  // charger is back: the edge is void
  CHECK(batteryStats.snapshot().awake.timeMs >= before);
  CHECK(logHas("charge start at"));

  // The re-detected charge is only 1 min old, so its own edge is rejected too.
  state(90, false);
  tick();
  tick();
  CHECK(batteryStats.snapshot().awake.timeMs >= before);
}

void testGateMetZeroesHistory() {
  section("a confirmed unplug zeroes awake/sleep/total/charge");
  fresh();
  state(95, false);
  run(40);
  const uint32_t before = batteryStats.snapshot().awake.timeMs;
  CHECK(before > 35 * MIN);

  state(95, true);
  run(20);
  // A charge session must not touch the record: the cycle ends at the UNPLUG.
  CHECK(batteryStats.snapshot().awake.timeMs == before);

  state(95, false);
  tick();  // candidate: armed, not taken -- so this sample is still the old cycle
  CHECK(batteryStats.snapshot().awake.timeMs == before + 30000u);
  const int writesBefore = hostNvsWrites();
  tick();  // confirmed -> zeroed, then this sample's 30 s is booked to the new cycle

  const BatteryStats::Snapshot s = batteryStats.snapshot();
  CHECK(s.awake.timeMs == 30000u);  // exactly the confirming sample, not 40 min
  CHECK(s.awake.socDelta == 0);
  CHECK(s.sleeping.timeMs == 0);
  CHECK(s.sleeping.socDelta == 0);
  CHECK(s.charge.timeMs == 0);
  CHECK(s.charge.socDelta == 0);
  CHECK(s.total.timeMs == s.awake.timeMs);
  CHECK(s.total.timeMs < 2 * MIN);
  // The zeroed record is written at the edge even inside PERSIST_MS: a power cut
  // right after the unplug must not bring the old cycle back.
  CHECK(hostNvsWrites() > writesBefore);
  CHECK(hostNvsGuardDepth() == 0);

  run(5);
  CHECK(batteryStats.snapshot().awake.timeMs > 30000u);  // and it counts forward
  CHECK(batteryStats.snapshot().awake.timeMs < 6 * MIN);
}

void testResetPersistsAcrossReboot() {
  section("the zeroed record survives a reboot");
  fresh();
  state(95, false);
  run(40);
  const uint32_t before = batteryStats.snapshot().awake.timeMs;

  state(95, true);
  run(20);
  state(95, false);
  tick();
  tick();  // confirmed: cycle restarted

  batteryStats.begin();  // reboot, reading NVS
  const BatteryStats::Snapshot s = batteryStats.snapshot();
  CHECK(batteryStats.valid());
  CHECK(s.awake.timeMs <= 60000u);  // the NEW cycle
  CHECK(s.awake.timeMs < before);   // provably not the old one
  CHECK(s.charge.timeMs == 0);
}

void testResetButtonStillWorks() {
  section("the Reset button still clears everything, and still persists");
  fresh();
  state(70, false);
  run(60);
  state(70, true);
  run(10);
  CHECK(batteryStats.snapshot().awake.timeMs > 50 * MIN);

  const int guardedBefore = hostNvsGuardedWrites();
  batteryStats.reset();

  BatteryStats::Snapshot s = batteryStats.snapshot();
  CHECK(s.awake.timeMs == 0);
  CHECK(s.sleeping.timeMs == 0);
  CHECK(s.charge.timeMs == 0);
  CHECK(s.total.timeMs == 0);
  CHECK(s.awake.socDelta == 0);
  CHECK(s.charge.socDelta == 0);
  // The clear is a single NVS write, inside the panel guard.
  CHECK(hostNvsGuardedWrites() > guardedBefore);
  CHECK(hostNvsGuardDepth() == 0);

  batteryStats.begin();  // reboot
  s = batteryStats.snapshot();
  CHECK(s.awake.timeMs == 0);
  CHECK(s.charge.timeMs == 0);
}

void testResetDuringCharging() {
  section("Reset pressed mid-charge does not disarm the next automatic reset");
  fresh();
  state(60, false);
  run(30);
  state(60, true);
  run(3);
  batteryStats.reset();
  CHECK(batteryStats.snapshot().awake.timeMs == 0);
  CHECK(batteryStats.snapshot().charge.timeMs == 0);

  run(10);  // keep charging: the session (and its dwell clock) re-arms
  state(61, false);
  tick();
  tick();  // unplug confirmed
  const BatteryStats::Snapshot s = batteryStats.snapshot();
  CHECK(s.awake.timeMs == 30000u);  // the automatic reset still fired
  CHECK(s.charge.timeMs == 0);
}

void testChargeStartingInStandby() {
  section("a charge that begins during standby is credited from the wake");
  fresh();
  state(88, false);
  run(30);
  const uint32_t awakeBefore = batteryStats.snapshot().awake.timeMs;
  CHECK(awakeBefore > 25 * MIN);

  // 8 h of standby with the cable plugged in: one sample sees a huge millis() gap.
  state(88, true);
  hostMillis() += 8 * HOUR;
  batteryStats.update();
  const BatteryStats::Snapshot afterWake = batteryStats.snapshot();
  CHECK(afterWake.sleeping.timeMs >= 7 * HOUR);  // the gap was booked as sleep
  CHECK(afterWake.charging);
  CHECK(afterWake.awake.timeMs >= awakeBefore);

  // Unplug 3.5 min after the wake: the dwell clock can only be measured from the
  // first charging SAMPLE, so this is under the gate and nothing is wiped. The
  // conservative miss is the documented behaviour, not a silent data loss.
  run(3);
  state(89, false);
  tick();
  CHECK(batteryStats.snapshot().awake.timeMs >= awakeBefore);
  CHECK(batteryStats.snapshot().sleeping.timeMs >= 7 * HOUR);

  // A long enough charge after the wake does close the cycle.
  state(89, true);
  run(10);
  state(90, false);
  tick();
  tick();
  const BatteryStats::Snapshot s = batteryStats.snapshot();
  CHECK(s.awake.timeMs == 30000u);
  CHECK(s.sleeping.timeMs == 0);
}

void testFirstBootAndFirstUnplug() {
  section("first boot and the first ever unplug");
  hostNvsReset();
  hostLogLines.clear();
  batteryStats = BatteryStats{};
  state(50, false);
  batteryStats.begin();
  CHECK(batteryStats.valid());
  BatteryStats::Snapshot s = batteryStats.snapshot();
  CHECK(s.awake.timeMs == 0 && s.sleeping.timeMs == 0 && s.total.timeMs == 0);
  CHECK(hostNvsHas("battstat/rec"));  // a fresh record was written

  // First charge ever, gate satisfied, nothing to lose: must not underflow or fault.
  state(50, true);
  run(10);
  state(51, false);
  tick();
  tick();
  s = batteryStats.snapshot();
  CHECK(s.awake.timeMs == 30000u);
  CHECK(s.sleeping.timeMs == 0);
  CHECK(s.charge.timeMs == 0);
  CHECK(s.total.socDelta == 0);
}

void testEstimateWindowRestartsOnSameEdge() {
  section("the estimate window restarts on the very same edge");
  fresh();
  // 2 h of discharge, 1 % every 20 min: enough span and enough drop to rate.
  uint16_t soc = 70;
  state(soc, false);
  for (int i = 0; i < 2 * 120; ++i) {
    if (i % 40 == 0 && i != 0 && soc > 0) {
      --soc;
      state(soc, false);
    }
    tick();
  }
  CHECK(batteryStats.snapshot().discharge.timeMs > 0);
  CHECK(BatteryStats::haveRate(batteryStats.snapshot().discharge));

  state(soc, true);
  run(20);
  state(soc, false);
  tick();
  tick();  // confirmed unplug
  CHECK(batteryStats.snapshot().discharge.timeMs == 0);  // no stale rate under a new cycle
  CHECK(!BatteryStats::haveRate(batteryStats.snapshot().discharge));

  run(10);
  CHECK(batteryStats.snapshot().awake.timeMs < 11 * MIN);  // and the record restarted too
}

void testWindowCappedAtTwoHours() {
  section("the rolling window never exceeds 2 h");
  fresh();
  state(100, false);
  for (int i = 0; i < 4 * 120; ++i) {  // 4 h, dropping 1 % every hour
    if (i % 120 == 0 && i != 0 && powerManager.getBatteryPercentage() > 0) {
      state(static_cast<uint16_t>(powerManager.getBatteryPercentage() - 1), false);
    }
    tick();
  }
  CHECK(batteryStats.snapshot().discharge.timeMs <= BatteryStats::WINDOW_MS + 30000u);
  CHECK(batteryStats.snapshot().discharge.timeMs >= BatteryStats::WINDOW_MS - 30000u);
}

void testPostWakeSampleIsFresh() {
  section("the post-wake sample waits for the gauge's recalibration");
  fresh();
  state(100, false);
  run(20);
  const uint32_t awakeBefore = batteryStats.snapshot().awake.timeMs;
  const uint32_t sleepBefore = batteryStats.snapshot().sleeping.timeMs;
  CHECK(batteryStats.snapshot().awake.socDelta == 0);

  // 7.5 h in standby: millis() jumps, the gauge machine still HOLDS 100 % for its
  // 3 s settle before the one-shot recalibration jumps to the fresh target.
  hostMillis() += 7u * HOUR + 30u * MIN;
  powerManager.hostSetPostWake(true);
  state(100, false);  // the held value, exactly what getBatteryPercentage() returns
  const int writesBefore = hostNvsWrites();
  batteryStats.update();
  // Nothing was sampled: the held value must not become "current".
  CHECK(batteryStats.snapshot().awake.timeMs == awakeBefore);
  CHECK(batteryStats.snapshot().sleeping.timeMs == sleepBefore);
  CHECK(logHas("wake sample deferred"));
  CHECK(hostNvsWrites() == writesBefore);

  // ...and it stays deferred while the settle runs (two more passes inside it).
  hostMillis() += 1500u;
  batteryStats.update();
  hostMillis() += 1500u;
  batteryStats.update();
  CHECK(batteryStats.snapshot().sleeping.timeMs == sleepBefore);

  // The recalibration lands: the machine drops to 90 % and releases the wait.
  state(90, false);
  powerManager.hostSetPostWake(false);
  tick();
  // The whole interval is SLEEP, and the drop is SLEEP's -- not awake discharge.
  CHECK(batteryStats.snapshot().sleeping.timeMs > sleepBefore + 7u * HOUR);
  CHECK(batteryStats.snapshot().sleeping.socDelta == -10);
  CHECK(batteryStats.snapshot().awake.socDelta == 0);
  CHECK(batteryStats.snapshot().awake.timeMs < awakeBefore + 2 * MIN);
}

void testPostWakeWaitIsBounded() {
  section("a gauge that never settles cannot stop the accounting");
  fresh();
  state(70, false);
  run(10);
  const uint32_t awakeBefore = batteryStats.snapshot().awake.timeMs;

  // The wait is held open (a dead bus): after the bound, the recorder samples the
  // held value rather than freezing awake time forever.
  powerManager.hostSetPostWake(true);
  // 5 s steps so update() keeps crossing its own 30 s sample throttle, for a total
  // of 60 s: the deferral starts on the first pass past the throttle (30 s) and the
  // 15 s bound expires 15 s later.
  for (int i = 0; i < 12; ++i) {
    hostMillis() += 5000u;
    batteryStats.update();
  }
  CHECK(logHas("still not settled"));
  CHECK(batteryStats.snapshot().awake.timeMs > awakeBefore);
  powerManager.hostSetPostWake(false);
}

}  // namespace

int main() {
  testBriefPlugKeepsCounters();
  testGlitchedReadKeepsCounters();
  testGateMetZeroesHistory();
  testResetPersistsAcrossReboot();
  testResetButtonStillWorks();
  testResetDuringCharging();
  testChargeStartingInStandby();
  testFirstBootAndFirstUnplug();
  testEstimateWindowRestartsOnSameEdge();
  testWindowCappedAtTwoHours();
  testPostWakeSampleIsFresh();
  testPostWakeWaitIsBounded();

  std::printf("\n%d/%d checks passed", checksRun - checksFailed, checksRun);
  if (checksFailed != 0) std::printf("  (%d FAILED)", checksFailed);
  std::printf("\n");
  return checksFailed == 0 ? 0 : 1;
}
