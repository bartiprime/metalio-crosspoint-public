// r163 E3 -- the charge truth table, on the host.
//
// The table (`docs/battery-soc-and-charge.md` §4.2, `bq27220_gauge.h:66-79`) is:
//
//   raw_charging    = current > +5 mA
//   raw_discharging = current < -5 mA
//   |current| <= 5 mA -> NEITHER (a deadband: the previous verdict stands)
//   displayed       = raw_charging && displayed SoC < 100
//
// Everything below is the real header (src/util/BatteryGaugeState.h), so a change to the
// thresholds or to the deadband's meaning fails here before it reaches the battery.

#include <gtest/gtest.h>

#include "util/BatteryGaugeState.h"

using gaugestate::Charge;
using gaugestate::ChargeFlag;

TEST(ChargeFlagTest, AboveThresholdIsCharging) {
  ChargeFlag f;
  EXPECT_EQ(f.update(true, 6), Charge::Charging);
  EXPECT_TRUE(f.charging());
  EXPECT_FALSE(f.discharging());

  ChargeFlag g;
  EXPECT_EQ(g.update(true, 259), Charge::Charging);
}

TEST(ChargeFlagTest, BelowThresholdIsDischarging) {
  ChargeFlag f;
  EXPECT_EQ(f.update(true, -6), Charge::Discharging);
  EXPECT_TRUE(f.discharging());
  EXPECT_FALSE(f.charging());

  ChargeFlag g;
  EXPECT_EQ(g.update(true, -180), Charge::Discharging);
}

// The thresholds are EXCLUSIVE: +5 mA is inside the band, +6 is not. Pinned because an
// off-by-one here silently widens the band by 1 mA forever.
TEST(ChargeFlagTest, ThresholdsAreExclusive) {
  ChargeFlag atFive;
  EXPECT_EQ(atFive.update(true, 5), Charge::Unknown) << "+5 mA must be deadband, not charging";
  ChargeFlag atSix;
  EXPECT_EQ(atSix.update(true, 6), Charge::Charging);

  ChargeFlag atMinusFive;
  EXPECT_EQ(atMinusFive.update(true, -5), Charge::Unknown) << "-5 mA must be deadband, not discharging";
  ChargeFlag atMinusSix;
  EXPECT_EQ(atMinusSix.update(true, -6), Charge::Discharging);
}

// A pack that is charging stops charging: inside the band the last verdict stands, so
// 5 / 0 / -3 / +4 mA after a +40 mA charge still reads CHARGING.
TEST(ChargeFlagTest, DeadbandHoldsCharging) {
  ChargeFlag f;
  f.update(true, 40);
  const int16_t quiet[] = {5, 0, -3, +4, 1, -5, 5};
  for (const int16_t ma : quiet) {
    EXPECT_EQ(f.update(true, ma), Charge::Charging) << "at " << ma << " mA";
  }
  EXPECT_TRUE(f.charging());
}

TEST(ChargeFlagTest, DeadbandHoldsDischarging) {
  ChargeFlag f;
  f.update(true, -40);
  const int16_t quiet[] = {-5, 0, 3, -4, -1, 5, -5};
  for (const int16_t ma : quiet) {
    EXPECT_EQ(f.update(true, ma), Charge::Discharging) << "at " << ma << " mA";
  }
  EXPECT_TRUE(f.discharging());
}

TEST(ChargeFlagTest, DeadbandFromUnknownStaysUnknown) {
  ChargeFlag f;
  const int16_t quiet[] = {5, 0, -5, 3, -2};
  for (const int16_t ma : quiet) {
    EXPECT_EQ(f.update(true, ma), Charge::Unknown) << "at " << ma << " mA";
  }
}

// A failed read is not evidence of "not charging".
TEST(ChargeFlagTest, FailedReadKeepsVerdict) {
  ChargeFlag f;
  f.update(true, 120);
  EXPECT_EQ(f.update(false, 0), Charge::Charging);
  f.update(false, -900);
  EXPECT_TRUE(f.charging());

  ChargeFlag d;
  d.update(true, -120);
  EXPECT_EQ(d.update(false, 999), Charge::Discharging);

  ChargeFlag u;
  EXPECT_EQ(u.update(false, 500), Charge::Unknown);
}

// The defect this whole item exists for: `current > 0` jitters. Feed 200 samples that
// cross zero constantly and count the verdict changes -- a bare `> 0` would produce ~200.
TEST(ChargeFlagTest, IdleJitterDoesNotFlicker) {
  ChargeFlag f;
  int transitions = 0;
  Charge previous = Charge::Unknown;
  const int16_t jitter[] = {+6, +2, 0, -1, +5, -3, +8, -4, +1, 0};
  int samples = 0;
  for (int round = 0; round < 20; ++round) {
    for (const int16_t ma : jitter) {
      const Charge now = f.update(true, ma);
      if (samples > 0 && now != previous) ++transitions;
      previous = now;
      ++samples;
    }
  }
  EXPECT_EQ(samples, 200);
  EXPECT_EQ(transitions, 0) << "the deadband must absorb the whole +-5 mA jitter";
  EXPECT_TRUE(f.charging());
}

// ... and it does not LATCH: a real discharge still gets through.
TEST(ChargeFlagTest, LeavingTheBandChangesVerdict) {
  ChargeFlag f;
  f.update(true, 80);
  EXPECT_EQ(f.update(true, -7), Charge::Discharging);
  EXPECT_EQ(f.update(true, 7), Charge::Charging);
}

TEST(ChargeFlagTest, ResetForgets) {
  ChargeFlag f;
  f.update(true, 90);
  f.reset();
  EXPECT_EQ(f.raw(), Charge::Unknown);
}

// The charger IC can say "charging" but cannot say the direction.
TEST(ChargeFlagTest, SetIsForNonCurrentSources) {
  ChargeFlag f;
  f.set(Charge::Charging);
  EXPECT_TRUE(f.charging());
  f.set(Charge::Unknown);
  EXPECT_EQ(f.raw(), Charge::Unknown);
}

// --- the DISPLAYED flag: no bolt at 100 % --------------------------------------------
TEST(DisplayChargingTest, BoltIsHiddenAtFull) {
  EXPECT_FALSE(gaugestate::displayCharging(Charge::Charging, 100));
  EXPECT_TRUE(gaugestate::displayCharging(Charge::Charging, 95));
  EXPECT_TRUE(gaugestate::displayCharging(Charge::Charging, 0));
}

TEST(DisplayChargingTest, OnlyRawChargingShowsABolt) {
  EXPECT_FALSE(gaugestate::displayCharging(Charge::Discharging, 95));
  EXPECT_FALSE(gaugestate::displayCharging(Charge::Unknown, 50));
  EXPECT_FALSE(gaugestate::displayCharging(Charge::Discharging, 0));
}

// A pack in the CV tail: raw charging at 100 % draws no bolt; the moment the displayed SoC
// leaves 100 (the hysteresis exit in E2) the bolt is allowed back. This is the pair of
// values the icon reads.
TEST(DisplayChargingTest, CvTailNeverShowsTheBolt) {
  ChargeFlag f;
  f.update(true, 210);  // CV tail: real current, pack full
  EXPECT_FALSE(gaugestate::displayCharging(f.raw(), 100));
  EXPECT_TRUE(gaugestate::displayCharging(f.raw(), 95));
}

// =====================================================================================
// r163 E2 -- full-charge confirmation: 4300 mV held 30 s, exit at 4280 mV, reset on any
// read failure or a >2 s gap, and re-validated against the post-wake voltage.
// =====================================================================================

namespace {

// Our poll is BATTERY_POLL_MS (1500 ms); the machine accepts any spacing <= 2 s.
constexpr uint32_t kPollMs = 1500;

// Feed `count` valid samples starting at `fromMs`, one every `stepMs`, and return
// `fromMs + (count-1)*stepMs` so the caller can continue the timeline.
uint32_t feed(gaugestate::FullCharge& fc, uint32_t fromMs, int count, uint16_t mv, uint32_t stepMs = kPollMs) {
  uint32_t at = fromMs;
  for (int i = 0; i < count; ++i) {
    fc.sample(at, true, mv);
    at += stepMs;
  }
  return at - stepMs;
}

}  // namespace

TEST(FullChargeTest, ConfirmsAt4300Held30s) {
  gaugestate::FullCharge fc;
  uint32_t at = 0;
  // 20 samples = 28.5 s of hold: not yet.
  at = feed(fc, at, 20, 4300);
  EXPECT_FALSE(fc.confirmed()) << "28.5 s is not 30 s";
  EXPECT_TRUE(fc.holding());
  EXPECT_EQ(fc.heldMs(at), 28500u);

  // One more sample takes it past 30 s (the timer runs on wall time, not sample count).
  at += kPollMs;
  fc.sample(at, true, 4300);
  EXPECT_TRUE(fc.confirmed());
  EXPECT_FALSE(fc.holding()) << "the hold is finished, not still running";
}

TEST(FullChargeTest, ExactThirtySecondsConfirms) {
  gaugestate::FullCharge fc;
  // 1 s samples (the vendor's cadence): the timer is wall time, not sample count.
  for (uint32_t at = 0; at <= 29999; at += 1000) fc.sample(at, true, 4300);
  EXPECT_FALSE(fc.confirmed());
  fc.sample(30000, true, 4300);
  EXPECT_TRUE(fc.confirmed());
}

TEST(FullChargeTest, OneMillivoltShortNeverConfirms) {
  gaugestate::FullCharge fc;
  uint32_t at = 0;
  for (int i = 0; i < 100; ++i) {  // 150 s at 4299 mV
    fc.sample(at, true, 4299);
    at += kPollMs;
  }
  EXPECT_FALSE(fc.confirmed()) << "4299 mV is below the confirm threshold, however long it stands";
  EXPECT_FALSE(fc.holding());
}

// The point of the hysteresis: confirmed 100 % is not released by the first dip.
TEST(FullChargeTest, ExitOnlyAt4280) {
  gaugestate::FullCharge fc;
  feed(fc, 0, 25, 4300);  // last sample at 36000
  ASSERT_TRUE(fc.confirmed());

  fc.sample(37500, true, 4281);
  EXPECT_TRUE(fc.confirmed()) << "4281 mV is inside the band";

  fc.sample(39000, true, 4280);
  EXPECT_FALSE(fc.confirmed()) << "4280 mV is the exit threshold";
}

TEST(FullChargeTest, ReadFailureResetsConfirm) {
  gaugestate::FullCharge fc;
  feed(fc, 0, 25, 4300);
  ASSERT_TRUE(fc.confirmed());

  fc.sample(37500, false, 0);
  EXPECT_FALSE(fc.confirmed()) << "a failed read is not a measurement";
  EXPECT_FALSE(fc.holding());
}

TEST(FullChargeTest, ReadFailureResetsTheHold) {
  gaugestate::FullCharge fc;
  feed(fc, 0, 10, 4300);  // 13.5 s into a hold
  ASSERT_TRUE(fc.holding());
  fc.sample(14000, false, 0);
  EXPECT_FALSE(fc.holding());
  fc.sample(15500, true, 4300);
  EXPECT_EQ(fc.heldMs(15500), 0u) << "the hold restarts from the next valid sample";
}

TEST(FullChargeTest, GapOverTwoSecondsResets) {
  gaugestate::FullCharge fc;
  feed(fc, 0, 25, 4300);
  ASSERT_TRUE(fc.confirmed());
  fc.sample(38001, true, 4300);  // gap 2001 ms
  EXPECT_FALSE(fc.confirmed());
}

TEST(FullChargeTest, GapOfExactlyTwoSecondsIsFine) {
  gaugestate::FullCharge fc;
  feed(fc, 0, 25, 4300);
  ASSERT_TRUE(fc.confirmed());
  fc.sample(38000, true, 4290);  // 2000 ms is not > 2000 ms
  EXPECT_TRUE(fc.confirmed());
}

TEST(FullChargeTest, DipRestartsTheHold) {
  gaugestate::FullCharge fc;
  uint32_t at = feed(fc, 0, 15, 4300);  // 21 s, last sample at 21000
  fc.sample(at + kPollMs, true, 4290);  // dips out of the confirm band at 22500
  ASSERT_FALSE(fc.confirmed());
  EXPECT_FALSE(fc.holding());
  // 20 more samples from 24000 -> 28500 ms of hold, i.e. 28.5 s: still not confirmed,
  // because the first 21 s were voided by the dip.
  at = feed(fc, 24000, 20, 4300);
  EXPECT_FALSE(fc.confirmed());
  at += kPollMs;
  fc.sample(at, true, 4300);
  EXPECT_TRUE(fc.confirmed());
}

// --- standby ---------------------------------------------------------------------------
//
// The device sleeps in place with the framebuffer intact: millis() jumps, the object
// survives. The confirmation must survive too (the pack was full when it went to sleep and
// nothing unplugged it), and the first sample after the 3 s settle must re-validate it.
TEST(FullChargeTest, ConfirmSurvivesStandby) {
  gaugestate::FullCharge fc;
  feed(fc, 0, 25, 4300);
  ASSERT_TRUE(fc.confirmed());

  const uint32_t wakeAt = 8u * 3600u * 1000u;  // eight hours later
  fc.wake(wakeAt);
  EXPECT_TRUE(fc.confirmed()) << "sleeping is not a read failure";

  // Inside the settle: ignored, even at a voltage that would otherwise exit.
  fc.sample(wakeAt + 500, true, 4100);
  EXPECT_TRUE(fc.confirmed());
  // First valid sample after the settle: 4290 mV is inside the band -> still full.
  fc.sample(wakeAt + 3500, true, 4290);
  EXPECT_TRUE(fc.confirmed());
}

TEST(FullChargeTest, WakeRevalidatesDownward) {
  gaugestate::FullCharge fc;
  feed(fc, 0, 25, 4300);
  ASSERT_TRUE(fc.confirmed());
  fc.wake(1000000);
  fc.sample(1003500, true, 4279);
  EXPECT_FALSE(fc.confirmed()) << "the pack is not full on the new voltage, whatever it was before";
}

TEST(FullChargeTest, WakeCannotConfirmOnOneSample) {
  gaugestate::FullCharge fc;
  feed(fc, 0, 5, 4200);  // never confirmed
  fc.wake(1000000);
  fc.sample(1003500, true, 4310);
  EXPECT_FALSE(fc.confirmed()) << "the hold still needs its 30 s after a wake";
  EXPECT_TRUE(fc.holding());
}

TEST(FullChargeTest, WakeDoesNotLetASleepBankTheHold) {
  gaugestate::FullCharge fc;
  feed(fc, 0, 15, 4300);  // 21 s held, not confirmed
  ASSERT_FALSE(fc.confirmed());
  fc.wake(600000);  // slept 10 minutes at 4300 mV
  fc.sample(603500, true, 4300);
  EXPECT_FALSE(fc.confirmed()) << "the sleep is not 30 s of measured hold";
  EXPECT_EQ(fc.heldMs(603500), 0u);
}

// A failed sample inside the settle keeps the settle open rather than zeroing the
// confirmation or accepting the next sample early.
TEST(FullChargeTest, FailedSampleDuringSettleKeepsTheLane) {
  gaugestate::FullCharge fc;
  feed(fc, 0, 25, 4300);
  ASSERT_TRUE(fc.confirmed());
  fc.wake(1000000);
  fc.sample(1002000, false, 0);
  EXPECT_TRUE(fc.confirmed());
  EXPECT_TRUE(fc.settling());
  fc.sample(1003500, true, 4295);
  EXPECT_TRUE(fc.confirmed());
  EXPECT_FALSE(fc.settling());
}

// =====================================================================================
// r163 E1 -- the displayed-SoC machine: 60-sample average, quantise DOWN to 5 %, at most
// 5 % per 30 s in the permitted direction, and the one-shot post-wake recalibration.
// =====================================================================================
using gaugestate::Charge;
using gaugestate::SocMachine;

namespace {

// A tiny rig: a machine, its clock, and the last displayed value. `feed` advances the clock
// by `dtMs` (never more than the 2 s gap rule, except where a test means to) and returns
// whether the display changed.
struct Rig {
  SocMachine m;
  uint32_t t = 0;
  uint16_t shown = 0;
  bool ready = false;

  bool feed(uint16_t raw, Charge dir = Charge::Unknown, uint32_t dtMs = 1500, bool ok = true,
            bool confirmedFull = false) {
    t += dtMs;
    if (m.sample(t, ok, raw, dir, confirmedFull)) {
      shown = m.displayed();
      ready = true;
      return true;
    }
    if (!ready) shown = m.displayed();
    return false;
  }
};

// Fill the window with one value and return the display it settles on.
uint16_t fill(Rig& r, uint16_t raw, int count, Charge dir = Charge::Unknown) {
  for (int i = 0; i < count; ++i) r.feed(raw, dir);
  return r.m.displayed();
}

}  // namespace

TEST(SocMachineTest, QuantiseDownTable) {
  EXPECT_EQ(SocMachine::quantiseDown(53), 50u);
  EXPECT_EQ(SocMachine::quantiseDown(58), 55u);
  EXPECT_EQ(SocMachine::quantiseDown(55), 55u);
  EXPECT_EQ(SocMachine::quantiseDown(59), 55u);
  EXPECT_EQ(SocMachine::quantiseDown(60), 60u);
  EXPECT_EQ(SocMachine::quantiseDown(100), 100u);
  EXPECT_EQ(SocMachine::quantiseDown(4), 0u);
  EXPECT_EQ(SocMachine::quantiseDown(0), 0u);
}

// Quantise DOWN, not nearest: every value in [50,54] shows 50.
TEST(SocMachineTest, QuantiseIsDownNotNearest) {
  for (uint16_t raw = 50; raw <= 54; ++raw) {
    EXPECT_EQ(SocMachine::quantiseDown(raw), 50u) << "raw " << raw;
  }
  for (uint16_t raw = 95; raw <= 99; ++raw) {
    EXPECT_EQ(SocMachine::quantiseDown(raw), 95u) << "raw " << raw;
  }
}

// A steady 53 % is a 50 % display, after the full 60-sample window.
TEST(SocMachineTest, SteadyPackConvergesToQuantisedDown) {
  Rig r;
  EXPECT_EQ(fill(r, 53, 60), 50u);
  EXPECT_EQ(r.m.samplesInFilter(), 60u);
  EXPECT_EQ(r.m.filterAverage(), 53u);
}

// The window is 60 SAMPLES: one wild sample moves the average by 1/60 of the step, not by
// the step itself.
TEST(SocMachineTest, OneBadSampleCannotMoveSixty) {
  Rig r;
  fill(r, 100, 60);
  ASSERT_EQ(r.m.displayed(), 100u);
  r.feed(0);
  EXPECT_EQ(r.m.filterAverage(), 98u) << "(59*100 + 0) / 60 = 98: one sample moves the average "
                                        "by 1/60 of the step, which is the whole point of the window";
  // The average is 98 -> target 95, and the 30 s window HAS elapsed (the value was set at
  // t=1500), so exactly one 5-point step is correct -- not a fall to 0.
  EXPECT_EQ(r.m.displayed(), 95u);
  EXPECT_GE(100u - r.m.displayed(), 5u) << "a single wild sample costs at most one quantum";
}

TEST(SocMachineTest, FailedSamplesDoNotEnterTheFilter) {
  Rig r;
  fill(r, 90, 10);
  ASSERT_EQ(r.m.samplesInFilter(), 10u);
  for (int i = 0; i < 5; ++i) r.feed(0, Charge::Unknown, 1500, false);
  EXPECT_EQ(r.m.samplesInFilter(), 10u);
  r.feed(90);
  EXPECT_EQ(r.m.samplesInFilter(), 11u);
}

TEST(SocMachineTest, GapOverTwoSecondsRestartsTheWindow) {
  Rig r;
  fill(r, 80, 30);
  ASSERT_EQ(r.m.samplesInFilter(), 30u);
  r.feed(80, Charge::Unknown, 5000);  // a 5 s hole
  EXPECT_EQ(r.m.samplesInFilter(), 1u);
}

// THE STEP LIMIT, as an invariant over a long drive: no two changes closer than 30 s, and
// every change exactly 5 percentage points.
TEST(SocMachineTest, StepLimitUpHoldsEveryWindow) {
  Rig r;
  fill(r, 50, 60);  // settles at 50 and starts the 30 s clock
  ASSERT_EQ(r.m.displayed(), 50u);

  // The value was SET at the first sample (t = 1500); the 30 s clock runs from there.
  uint32_t lastChangeAt = 1500;
  int steps = 0;
  for (int i = 0; i < 400; ++i) {
    const uint16_t before = r.m.displayed();
    if (r.feed(100, Charge::Charging)) {
      const uint16_t after = r.m.displayed();
      EXPECT_EQ(after, before + 5) << "a step is exactly one quantum at i=" << i;
      EXPECT_GE(r.t - lastChangeAt, 30000u) << "two steps inside 30 s at i=" << i;
      lastChangeAt = r.t;
      ++steps;
    }
  }
  EXPECT_EQ(steps, 10) << "50 -> 100 is ten quanta, i.e. ten 30 s windows";
  EXPECT_EQ(r.m.displayed(), 100u);
  // 300 s of movement is 10 windows; 400 samples at 1.5 s is 600 s, so the last step had
  // room to happen and the value sat still afterwards.
  EXPECT_GE(r.t - lastChangeAt, 30000u);
}

TEST(SocMachineTest, StepLimitDownHoldsEveryWindow) {
  Rig r;
  fill(r, 100, 60);
  ASSERT_EQ(r.m.displayed(), 100u);

  uint32_t lastChangeAt = 1500;
  int steps = 0;
  for (int i = 0; i < 400; ++i) {
    const uint16_t before = r.m.displayed();
    if (r.feed(40, Charge::Discharging)) {
      const uint16_t after = r.m.displayed();
      EXPECT_EQ(after, before - 5);
      EXPECT_GE(r.t - lastChangeAt, 30000u);
      lastChangeAt = r.t;
      ++steps;
    }
  }
  EXPECT_EQ(steps, 12) << "100 -> 40 is twelve quanta";
  EXPECT_EQ(r.m.displayed(), 40u);
}

// Exactly at the window, not a millisecond earlier.
TEST(SocMachineTest, StepHappensAtExactlyThirtySeconds) {
  Rig r;
  r.feed(50);  // first sample: sets 50 at t=1500 and starts the 30 s clock
  ASSERT_EQ(r.m.displayed(), 50u);

  // 19 more samples land at 3000..30000 ms. The target has long since moved (one sample of
  // 100 beside a 50 average), but the window -- measured from the set at 1500 ms -- has
  // not elapsed.
  for (int i = 0; i < 19; ++i) {
    EXPECT_FALSE(r.feed(100, Charge::Charging)) << "step before 30 s at t=" << r.t;
    EXPECT_EQ(r.m.displayed(), 50u);
  }
  ASSERT_EQ(r.t, 30000u);

  // The sample AT 31500 ms is the first allowed step -- exactly one quantum.
  EXPECT_TRUE(r.feed(100, Charge::Charging));
  EXPECT_EQ(r.t, 31500u);
  EXPECT_EQ(r.m.displayed(), 55u);

  // And the next sample (1.5 s later) may not step again.
  EXPECT_FALSE(r.feed(100, Charge::Charging));
  EXPECT_EQ(r.m.displayed(), 55u);
}

// Monotonicity: while charging the number may not fall, however wrong the samples get.
TEST(SocMachineTest, ChargingMayOnlyRise) {
  Rig r;
  fill(r, 100, 60);
  ASSERT_EQ(r.m.displayed(), 100u);
  for (int i = 0; i < 200; ++i) r.feed(40, Charge::Charging);
  EXPECT_EQ(r.m.displayed(), 100u) << "a charging pack's display never walks backwards";
}

TEST(SocMachineTest, DischargingMayOnlyFall) {
  Rig r;
  fill(r, 40, 60);
  ASSERT_EQ(r.m.displayed(), 40u);
  for (int i = 0; i < 200; ++i) r.feed(100, Charge::Discharging);
  EXPECT_EQ(r.m.displayed(), 40u);
}

// At rest the value may move either way (as the pack cools or settles), still at 5 %/30 s.
TEST(SocMachineTest, IdleMayMoveBothWays) {
  Rig r;
  fill(r, 40, 60);
  ASSERT_EQ(r.m.displayed(), 40u);
  int up = 0;
  for (int i = 0; i < 200; ++i)
    if (r.feed(90)) ++up;
  EXPECT_GT(up, 0);
  for (int i = 0; i < 400; ++i) r.feed(20);
  EXPECT_LT(r.m.displayed(), 90u);
}

// --- wake -----------------------------------------------------------------------------

// The displayed value is HELD through the settle, then jumps ONCE to the fresh target.
TEST(SocMachineTest, WakeHoldsThenRecalibratesInOneStep) {
  Rig r;
  fill(r, 80, 60);
  ASSERT_EQ(r.m.displayed(), 80u);

  const uint32_t wakeAt = r.t + 3600000;  // an hour of standby
  r.t = wakeAt;
  r.m.wake(wakeAt);
  EXPECT_TRUE(r.m.settling());
  EXPECT_TRUE(r.m.recalibrationPending());

  // Inside the settle: not even a wildly different sample moves the display.
  r.feed(40, Charge::Discharging, 1500);
  EXPECT_EQ(r.m.displayed(), 80u);
  r.feed(40, Charge::Discharging, 1400);  // 2.9 s after the wake
  EXPECT_EQ(r.m.displayed(), 80u);
  EXPECT_TRUE(r.m.settling());

  // First sample after 3 s: the one allowed multi-step recalibration.
  r.feed(40, Charge::Discharging, 200);  // 3.1 s
  EXPECT_FALSE(r.m.settling());
  EXPECT_EQ(r.m.displayed(), 40u) << "one jump of any distance, allowed exactly once";
  EXPECT_FALSE(r.m.recalibrationPending());
}

// ... and the 30 s timer restarts from the recalibration, so the next move waits.
TEST(SocMachineTest, RecalibrationRestartsTheStepTimer) {
  Rig r;
  fill(r, 80, 60);
  const uint32_t wakeAt = r.t + 600000;
  r.t = wakeAt;
  r.m.wake(wakeAt);
  r.feed(40, Charge::Discharging, 3200);
  ASSERT_EQ(r.m.displayed(), 40u);
  const uint32_t recalAt = r.t;

  int moves = 0;
  while (r.t - recalAt < 28500) {
    if (r.feed(80, Charge::Charging)) ++moves;
  }
  EXPECT_EQ(moves, 0) << "no step inside the 30 s window that the recalibration restarted";
  EXPECT_EQ(r.m.displayed(), 40u);
  while (r.t - recalAt < 30000 - 1500) r.feed(80, Charge::Charging);
  const bool moved = r.feed(80, Charge::Charging);
  EXPECT_TRUE(moved);
  EXPECT_EQ(r.m.displayed(), 45u);
}

TEST(SocMachineTest, RecalibrationIsOneShot) {
  Rig r;
  fill(r, 80, 60);
  const uint32_t wakeAt = r.t + 600000;
  r.t = wakeAt;
  r.m.wake(wakeAt);
  r.feed(40, Charge::Discharging, 3200);
  ASSERT_EQ(r.m.displayed(), 40u);

  // A second, different target one sample later: NO second jump.
  r.feed(85, Charge::Charging, 1500);
  EXPECT_EQ(r.m.displayed(), 40u);
  EXPECT_FALSE(r.m.recalibrationPending()) << "consumed, and only wake() re-arms it";
}

// The one-shot is spent by the first accepted sample even when that sample AGREES with the display.
// It used to survive an agreeing wake, so the next change -- arbitrary minutes later, from a wake
// that had already been answered -- took the unlimited jump the 5 %/30 s limit exists to prevent.
TEST(SocMachineTest, AnAgreeingSampleSpendsTheRecalibration) {
  Rig r;
  fill(r, 80, 60);
  ASSERT_EQ(r.m.displayed(), 80u);
  const uint32_t wakeAt = r.t + 600000;
  r.t = wakeAt;
  r.m.wake(wakeAt);
  ASSERT_TRUE(r.m.recalibrationPending());

  r.feed(80, Charge::Discharging, 3200);  // first sample after the settle: same value
  EXPECT_EQ(r.m.displayed(), 80u);
  EXPECT_FALSE(r.m.recalibrationPending()) << "a wake that is answered is over";

  // Half an hour later the pack really is at 60: the number walks down one quantum, not to 60.
  r.t += 1800000;
  for (int i = 0; i < 12; ++i) r.feed(60, Charge::Discharging);
  EXPECT_EQ(r.m.displayed(), 75u) << "at most 5 % per 30 s, and the one-shot is already spent";
}

TEST(SocMachineTest, ASecondWakeRearmsTheRecalibration) {
  Rig r;
  fill(r, 80, 60);
  r.m.wake(r.t + 600000);
  r.t += 600000;
  r.feed(40, Charge::Discharging, 3200);
  ASSERT_EQ(r.m.displayed(), 40u);

  const uint32_t wake2 = r.t + 600000;
  r.t = wake2;
  r.m.wake(wake2);
  r.feed(75, Charge::Charging, 3200);
  EXPECT_EQ(r.m.displayed(), 75u) << "one jump per wake, not one per session";
}

// A failed sample inside the settle keeps the settle open.
TEST(SocMachineTest, FailedSampleDuringSettleKeepsTheSettle) {
  Rig r;
  fill(r, 80, 60);
  r.m.wake(r.t + 600000);
  r.t += 600000;
  r.feed(0, Charge::Discharging, 3200, false);
  EXPECT_TRUE(r.m.settling());
  EXPECT_EQ(r.m.displayed(), 80u);
  r.feed(45, Charge::Discharging, 1500);
  EXPECT_FALSE(r.m.settling());
  EXPECT_EQ(r.m.displayed(), 45u);
}

// 100 -> 95 -> 100 must not happen: with the pack at 100 and charging, noisy samples below
// it cannot move the display down, and the direction gate holds it.
TEST(SocMachineTest, NoHundredToNinetyFiveAndBack) {
  Rig r;
  fill(r, 100, 60, Charge::Charging);
  ASSERT_EQ(r.m.displayed(), 100u);
  const uint16_t noise[] = {99, 98, 100, 97, 100, 96, 100, 100};
  int drops = 0;
  for (int round = 0; round < 40; ++round) {
    for (const uint16_t raw : noise) {
      if (r.feed(raw, Charge::Charging) && r.m.displayed() < 100) ++drops;
    }
  }
  EXPECT_EQ(drops, 0);
  EXPECT_EQ(r.m.displayed(), 100u);
}

// The E2 confirmation pins 100 %, and its release walks down at 5 %/30 s instead of
// snapping.
TEST(SocMachineTest, ConfirmationPinsHundredAndReleaseWalksDown) {
  Rig r;
  fill(r, 92, 60);
  ASSERT_LE(r.m.displayed(), 95u);
  EXPECT_TRUE(r.feed(92, Charge::Charging, 1500, true, true));
  EXPECT_EQ(r.m.displayed(), 100u) << "a 30 s voltage hold outranks the average";

  // Confirmation released, samples now say 92: the display steps down, never snaps.
  // (Direction is Unknown here -- a released confirmation means the charger has stopped,
  // so the value is no longer under the "charging may only rise" gate.)
  const uint16_t before = r.m.displayed();
  r.feed(92, Charge::Unknown, 1500, true, false);
  EXPECT_EQ(r.m.displayed(), before) << "no instant drop on release";
  for (int i = 0; i < 30; ++i) r.feed(92, Charge::Unknown, 1500, true, false);
  EXPECT_EQ(r.m.displayed(), 95u) << "one 5 % step after the window";
}

// =====================================================================================
// THE 100 -> 95 -> 100 DEFECT, as a scenario. Before this round, waking at 100 % dropped
// the number because the reflected post-sleep voltage sat below the crude threshold, and
// the next sample put it back. With the confirmation holding and the re-validation inside
// the band, nothing in this sequence moves off 100 %.
// =====================================================================================
TEST(FullChargeTest, NoHundredToNinetyFiveAndBack) {
  gaugestate::FullCharge fc;
  feed(fc, 0, 25, 4300);
  ASSERT_TRUE(fc.confirmed());

  // Standby with the cable in. The resume transient dips the pack, then it recovers.
  fc.wake(2000000);
  EXPECT_TRUE(fc.confirmed());
  fc.sample(2003500, true, 4292);  // inside the band: still full
  EXPECT_TRUE(fc.confirmed());
  fc.sample(2005000, true, 4288);
  EXPECT_TRUE(fc.confirmed());
  fc.sample(2006500, true, 4301);
  EXPECT_TRUE(fc.confirmed());

  // And when the pack genuinely falls out of the band, it releases exactly once.
  fc.sample(2008000, true, 4270);
  EXPECT_FALSE(fc.confirmed());
}
