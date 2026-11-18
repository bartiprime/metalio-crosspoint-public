#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// r163 E1/E2/E3 -- THE GAUGE'S OWN STATE MACHINE, as arithmetic.
//
// WHAT THIS IS. The vendor's gauge logic (`docs/battery-soc-and-charge.md` §3.2, §3.5,
// §4.2; `bq27220_gauge.h:66-79`) written against OUR driver instead of transplanting
// theirs:
//   E3  the charge flag with a +-5 mA deadband, and the DISPLAYED flag that hides the bolt
//       at 100 % (no lightning during the CV tail);
//   E2  the full-charge confirmation: 4300 mV held 30 s, exit at 4280 mV, reset on any read
//       failure or a >2 s sample gap, re-validated after the post-wake settle;
//   E1  the displayed-SoC pipeline: 60-sample average -> quantise down to 5 % -> at most
//       5 % per 30 s in the permitted direction -> one-shot post-wake recalibration.
//
// WHY IT IS A HEADER WITH NO I/O. Every number below is checkable against a simulator, so
// it is checked on the host (test/battery_gauge_state). The driver (HalPowerManager's
// 0x08/0x0C/0x2C reads) only supplies samples: it never computes a percentage, decides
// whether the bolt is drawn, or holds a timer across two samples. Anything here that could
// be wrong is therefore wrong in a test first.
//
// UNITS AND TIME. Times are a monotonic millisecond clock (millis()). Samples are accepted
// at most once per kSampleMinIntervalMs and the filter window is counted in SAMPLES, not
// seconds -- a "60-sample moving average" is literally that, so it stays faithful to the
// vendor even though our poll is 1500 ms rather than their 1 s.
//
// NOTHING HERE IS PERSISTED, ON PURPOSE. See FullCharge::wake() and SocMachine::wake(): a
// stored "we saw 4300 mV for 30 s" or a stored displayed percentage would outlive the
// evidence for it, and the next boot would start by asserting a state nobody measured. A
// reboot re-measures from zero; a standby (which returns in place on this board) keeps the
// state in RAM and re-validates it against a fresh sample after the settle window.
namespace gaugestate {


// --- E3 constants (the vendor's, `docs/battery-soc-and-charge.md` §4.2) ---------------
constexpr int16_t kChargeCurrentMa = 5;     // raw_charging  = current >  +5 mA
constexpr int16_t kDischargeCurrentMa = -5; // raw_discharge = current < -5 mA
                                            // |current| <= 5 mA is the deadband: neither.

// ---------------------------------------------------------------------------
// THE CHARGE FLAG, WITH A DEADBAND.
//
// `raw_charging = current > +5 mA`, `raw_discharging = current < -5 mA`, and a reading
// inside +-5 mA is neither -- it is a deadband, not a tie. The BQ27220's Current()
// register is quantised to whole mA and jitters by a count or two on a pack that is doing
// nothing, so "sign of the current" flips the bolt several times a second at rest. Inside
// the band the PREVIOUS verdict stands: no evidence is not evidence of change.
//
// What the user sees is a second question: displayCharging() below. A pack at 100 %
// sitting in CV tail is still drawing current, but drawing the bolt there says "leave it
// plugged in", so the UI shows the bolt only below 100 %.
// ---------------------------------------------------------------------------
enum class Charge : uint8_t { Unknown, Charging, Discharging };

class ChargeFlag {
 public:
  // Feed one Current() sample. `currentKnown` is false when the register did not answer.
  // A failed read is NOT "not charging": it leaves the previous verdict alone (the caller
  // decides how long to trust that -- HalPowerManager's stale window).
  Charge update(bool currentKnown, int16_t ma) {
    if (!currentKnown) return raw_;
    if (ma > kChargeCurrentMa) {
      raw_ = Charge::Charging;
    } else if (ma < kDischargeCurrentMa) {
      raw_ = Charge::Discharging;
    }
    // |ma| <= 5 mA: deadband. Hold.
    return raw_;
  }

  // Boot / after a read the caller refuses to trust: forget the verdict rather than
  // inherit one.
  void reset() { raw_ = Charge::Unknown; }

  // For a source that is NOT Current() -- the charger IC's own CHG_STAT. It can say
  // "charging"; it cannot say which way the current flows, so `Unknown` is the honest
  // answer when it says nothing. Never feed a synthesised mA through update() instead:
  // that would put a made-up number through the deadband and call it a measurement.
  void set(Charge c) { raw_ = c; }

  Charge raw() const { return raw_; }
  bool charging() const { return raw_ == Charge::Charging; }
  bool discharging() const { return raw_ == Charge::Discharging; }

 private:
  Charge raw_ = Charge::Unknown;
};

// The charge state as a USER should see it: the bolt only when the pack is actually
// taking current AND is not already full. This is the only honest answer for an icon:
// at 100 % the CV tail still draws current, and a bolt there reads as "still charging",
// which is the one thing the operator is trying to find out.
inline bool displayCharging(Charge raw, uint16_t displayedSoc) {
  return raw == Charge::Charging && displayedSoc < 100;
}

// ---------------------------------------------------------------------------
// E2 -- FULL-CHARGE CONFIRMATION WITH HYSTERESIS (`docs/battery-soc-and-charge.md` §3.2).
//
//   100 % is confirmed when 0x08 reads >= 4300 mV continuously for 30 s.
//   It is released when 0x08 reads <= 4280 mV. Between the two the confirmed state stands:
//   that 20 mV band is the hysteresis, and without it a pack resting at 4295 mV toggles
//   full/not-full on the last two bits of the reading.
//
// The hold is reset by ANY read failure and by a >2 s gap between valid samples, so a
// confirmation can never be assembled out of samples a sleeping or wedged gauge did not
// take. That gap rule is also why a standby cannot silently bank 30 s of "hold": on wake
// the hold is disarmed explicitly (wake()) and the post-settle sample revalidates.
//
// THE STATE LIVES IN RAM, in one member of HalPowerManager, and nowhere else. It is not a
// setting and not in BackupProfile: a persisted "confirmed at 100 %" is a claim about a
// battery that has since been unplugged or drained, and the next boot would display a full
// pack it never measured. A standby is not a reboot -- this object survives in place --
// so sleeping on the cable keeps the confirmation without persisting anything.
// ---------------------------------------------------------------------------
constexpr uint16_t kFullConfirmMv = 4300;     // 100 % is confirmed at 4300 mV held 30 s
constexpr uint16_t kFullExitMv = 4280;        // ... and released at 4280 mV
constexpr uint32_t kFullHoldMs = 30000;       // the 30 s hold
constexpr uint32_t kSampleGapResetMs = 2000;  // a >2 s gap between VALID samples resets it
// The post-wake settle: samples during it are not evidence (the pack is recovering from
// the resume's load transient), and the first valid one after it decides.
constexpr uint32_t kWakeSettleMs = 3000;

class FullCharge {
 public:
  // Feed one 0x08 sample. Returns confirmed().
  bool sample(uint32_t nowMs, bool ok, uint16_t mv) {
    if (settling_) {
      // A sample inside the settle window is not evidence, and a FAILED one does not zero
      // the confirmation -- it just means "not yet".
      if (nowMs < settleUntilMs_) return confirmed_;
      if (!ok) return confirmed_;  // stay in the settle lane until a valid sample lands
      settling_ = false;
      revalidate(nowMs, mv);
      return confirmed_;
    }

    if (!ok) {
      // Any read failure resets the confirm AND the hold: a latch built from samples we
      // could not read is a latch built from nothing.
      confirmed_ = false;
      holdActive_ = false;
      haveValid_ = false;
      return confirmed_;
    }

    if (haveValid_ && (nowMs - lastValidMs_) > kSampleGapResetMs) {
      confirmed_ = false;
      holdActive_ = false;
      haveValid_ = false;
    }

    lastValidMs_ = nowMs;
    haveValid_ = true;
    applySample(nowMs, mv);
    return confirmed_;
  }

  // Called ONCE per standby resume, before the first post-wake sample. Keeps the confirmed
  // state (the pack did not stop being full because the CPU slept), disarms the hold (the
  // sleep is not time spent at 4300 mV as far as this machine knows), and opens the settle
  // window. The first valid sample after it re-validates against the NEW voltage, which is
  // what stops the 100 -> 95 -> 100 cycle after a night in standby.
  void wake(uint32_t nowMs) {
    settling_ = true;
    settleUntilMs_ = nowMs + kWakeSettleMs;
    holdActive_ = false;
    haveValid_ = false;  // the resume gap must not be read as a >2 s sample gap
  }

  bool confirmed() const { return confirmed_; }
  bool holding() const { return holdActive_; }
  bool settling() const { return settling_; }
  // How long the current 4300 mV hold has stood (0 when no hold is running).
  uint32_t heldMs(uint32_t nowMs) const {
    if (!holdActive_ || nowMs < holdStartMs_) return 0;
    return nowMs - holdStartMs_;
  }

 private:
  // The first sample after the settle: revalidate the confirmation against it, and restart
  // the hold from here so a pack still at 4300 mV stands its full 30 s again before it can
  // be re-confirmed -- and cannot be released by the first 2-bit wobble either.
  void revalidate(uint32_t nowMs, uint16_t mv) {
    if (mv <= kFullExitMv) {
      confirmed_ = false;
      holdActive_ = false;
    } else if (confirmed_) {
      holdActive_ = false;  // inside the band: the confirmation stands
    }
    lastValidMs_ = nowMs;
    haveValid_ = true;
    applySample(nowMs, mv);
  }

  void applySample(uint32_t nowMs, uint16_t mv) {
    if (confirmed_) {
      if (mv <= kFullExitMv) {
        confirmed_ = false;
        holdActive_ = false;
      }
      return;  // confirmed is sticky until the exit threshold; the hold is irrelevant
    }
    if (mv >= kFullConfirmMv) {
      if (!holdActive_) {
        holdActive_ = true;
        holdStartMs_ = nowMs;
      } else if ((nowMs - holdStartMs_) >= kFullHoldMs) {
        confirmed_ = true;
        holdActive_ = false;
      }
    } else {
      holdActive_ = false;  // below the confirm threshold: the hold restarts from nothing
    }
  }

  bool confirmed_ = false;
  bool holdActive_ = false;
  uint32_t holdStartMs_ = 0;
  uint32_t lastValidMs_ = 0;
  bool haveValid_ = false;
  bool settling_ = false;
  uint32_t settleUntilMs_ = 0;
};

// ---------------------------------------------------------------------------
// E1 -- THE DISPLAYED-SOC STATE MACHINE (`docs/battery-soc-and-charge.md` §3.5).
//
//   raw 0x2C sample -> 60-sample moving average -> quantise DOWN to 5 %
//                   -> at most 5 % per 30 s, in the permitted direction.
//
// Direction comes from the charge flag, so it is an input: while CHARGING the value may
// only rise, while DISCHARGING only fall, and while neither it may do either (a pack at
// rest settles both ways as it cools). Because the quantiser is 5 %, "at most 5 % per 30 s"
// is exactly one quantum per window.
//
// WAKE: clear the filter, hold the displayed value for 3 s, then allow ONE multi-step
// recalibration -- a jump straight to the fresh target, bypassing the step limit -- and
// restart the 30 s timer from that moment. One shot, consumed by the first accepted
// sample: a second recalibration needs a second wake. That is what makes 100 -> 95 -> 100
// after a night in standby impossible while still letting the number catch up in one move
// when the pack really did change while the CPU was asleep.
// ---------------------------------------------------------------------------
constexpr uint8_t kFilterSamples = 60;          // 60-sample moving average
constexpr uint32_t kSampleMinIntervalMs = 1000; // ... at 1 sample/s max
constexpr uint8_t kQuantStep = 5;               // displayed SoC is quantised DOWN to 5 %
constexpr uint32_t kStepWindowMs = 30000;       // at most 5 % per 30 s
constexpr uint8_t kMaxStepPerWindow = 5;        // (one quantum, because of the quantiser)

class SocMachine {
 public:
  // Called once per standby resume, before the first post-wake sample. The displayed
  // value is HELD (not zeroed): the glass still shows it and the pack did not change state
  // because the CPU slept.
  void wake(uint32_t nowMs) {
    count_ = 0;
    head_ = 0;
    haveSample_ = false;
    settling_ = true;
    settleUntilMs_ = nowMs + kWakeSettleMs;
    recalcArmed_ = true;
  }

  // Feed one VALIDATED 0x2C sample (`ok` false = the gate refused it or the read failed).
  // `confirmedFull` is FullCharge's verdict: while the 30 s voltage hold stands, the
  // displayed value is 100 and the filter does not get a vote (the confirmation IS the
  // evidence; the average exists to smooth a noisy percentage, not to overrule a measured
  // 4300 mV). When it releases, the ordinary step limit takes over again, so the number
  // walks down instead of snapping.
  // Returns true when the displayed percentage changed.
  bool sample(uint32_t nowMs, bool ok, uint16_t rawSoc, Charge direction, bool confirmedFull = false) {
    if (settling_) {
      if (nowMs < settleUntilMs_) return false;
      if (!ok) return false;  // stay in the settle lane until a sample is worth settling on
      settling_ = false;
    }
    if (haveSample_ && (nowMs - lastSampleMs_) < kSampleMinIntervalMs) return false;

    if (haveSample_ && (nowMs - lastSampleMs_) > kSampleGapResetMs) {
      // A >2 s hole: the window no longer describes the last minute, so it is not a
      // 60-sample average any more. Start again from this sample.
      count_ = 0;
      head_ = 0;
    }
    if (!ok) {
      // A refused reading does not enter the average and does not move the display.
      lastSampleMs_ = nowMs;
      haveSample_ = true;
      return false;
    }

    lastSampleMs_ = nowMs;
    haveSample_ = true;
    if (rawSoc > 100) return false;  // never trust a value that is not a percentage

    ring_[head_] = rawSoc;
    head_ = static_cast<uint8_t>((head_ + 1) % kFilterSamples);
    if (count_ < kFilterSamples) ++count_;

    // The one-shot is spent by the first ACCEPTED sample, whatever it targets. Consuming it only
    // when the target differed left it armed across a wake whose first sample agreed with the
    // display, and the NEXT change -- possibly half an hour of real drift later -- then took the
    // unlimited jump the 5 %/30 s limit exists to prevent.
    const bool recalibrate = recalcArmed_;
    recalcArmed_ = false;

    // The confirmation outranks the average: it is a 30 s voltage hold, and it is what
    // makes "100 %" mean something. It is NOT a licence to jump otherwise -- only this.
    if (confirmedFull) {
      const bool changed = !haveShown_ || shown_ != 100;
      shown_ = 100;
      haveShown_ = true;
      lastStepMs_ = nowMs;
      return changed;
    }

    const uint16_t target = quantiseDown(average());
    return apply(nowMs, target, direction, recalibrate);
  }

  // Quantise DOWN to the 5 % grid: 53 -> 50, 58 -> 55, 100 -> 100, 4 -> 0. Down, not
  // nearest, so the displayed number is never kinder than the pack.
  static uint16_t quantiseDown(uint16_t percent) {
    return static_cast<uint16_t>((percent / kQuantStep) * kQuantStep);
  }

  uint16_t displayed() const { return shown_; }
  bool hasValue() const { return haveShown_; }
  bool settling() const { return settling_; }
  bool recalibrationPending() const { return recalcArmed_; }
  uint8_t samplesInFilter() const { return count_; }
  uint16_t filterAverage() const { return average(); }

 private:
  uint16_t average() const {
    if (count_ == 0) return 0;
    uint32_t sum = 0;
    for (uint8_t i = 0; i < count_; ++i) sum += ring_[i];
    return static_cast<uint16_t>(sum / count_);
  }

  bool apply(uint32_t nowMs, uint16_t target, Charge direction, bool recalibrate) {
    if (!haveShown_) {
      // Boot: there is no previous value to protect, so the first average IS the answer
      // (and it is already quantised down).
      shown_ = target;
      haveShown_ = true;
      lastStepMs_ = nowMs;
      return true;
    }
    if (target == shown_) return false;

    if (recalibrate) {
      // The one-shot post-wake recalibration: any distance, once, right after the settle.
      shown_ = target;
      lastStepMs_ = nowMs;
      return true;
    }

    // Direction gate: while charging the value may only rise, while discharging only
    // fall. A held value is not an error -- it is the vendor's rule, and it is what stops
    // the number walking backwards on the CV tail.
    if (direction == Charge::Charging && target < shown_) return false;
    if (direction == Charge::Discharging && target > shown_) return false;

    if ((nowMs - lastStepMs_) < kStepWindowMs) return false;

    if (target > shown_) {
      shown_ = static_cast<uint16_t>(shown_ + kMaxStepPerWindow);
      if (shown_ > target) shown_ = target;
    } else {
      shown_ = static_cast<uint16_t>(shown_ - kMaxStepPerWindow);
      if (shown_ < target) shown_ = target;
    }
    lastStepMs_ = nowMs;
    return true;
  }

  uint16_t ring_[kFilterSamples] = {0};
  uint8_t count_ = 0;
  uint8_t head_ = 0;
  uint16_t shown_ = 0;
  bool haveShown_ = false;
  uint32_t lastStepMs_ = 0;
  uint32_t lastSampleMs_ = 0;
  bool haveSample_ = false;
  bool settling_ = false;
  uint32_t settleUntilMs_ = 0;
  bool recalcArmed_ = false;
};

}  // namespace gaugestate
