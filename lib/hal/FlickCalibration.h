#pragma once

// Guided flick calibration maths: three prompted windows (flick LEFT, flick
// RIGHT, hold still) -> per-unit thresholds, or a named reason the capture is
// unusable. No I2C, no millis(), no allocation, so the whole derivation is
// replayable on the host (test/imu_flick).
//
// A capture is measured against a FIXED baseline taken at the first sample of
// the window: the prompt tells the operator to start from rest, so the gravity
// vector is constant for the ~2 s the window lasts. That is deliberately not
// the detector's tracking baseline -- calibration wants the raw excursion, not
// the detector's opinion about it.
//
// Units are g. Rejection is always explicit: a capture that cannot beat the
// noise floor, or that looks like a drop, is refused with a reason rather than
// saved.

#include <cmath>
#include <cstdint>

#include "FlickDetector.h"

namespace flickcal {

// Bounds on what a believable capture looks like. These are properties of the
// gesture, not of the unit, so they are not calibrated.
struct Limits {
  float minPeakG = 0.30f;      // below this a flick is indistinguishable from handling
  float maxPeakG = 1.60f;      // beyond this it is a drop, or the part is railing
  float noiseMaxG = 0.25f;     // "hold still" may not move more than this
  float minSnr = 3.0f;         // each flick must be this many times the still-window peak
  float maxImbalance = 3.0f;   // left/right peaks may differ by at most this factor
  uint32_t riseMaxMs = 150;    // peak must arrive this fast, or it was a roll
  // Derived threshold = fraction * the weaker of the two flicks, clamped into
  // [floorG, capG]. The fraction leaves room for a flick softer than the one
  // used to calibrate; the floor keeps a unit with a very light touch usable.
  float fraction = 0.35f;
  float floorG = 0.20f;
  float capG = 0.80f;
  uint32_t minSamples = 8;  // ~80 ms at the sampling rate the screen runs at
};

// One prompted capture. The caller feeds every sample it reads between the
// prompt appearing and Confirm being pressed.
struct Capture {
  bool started = false;
  uint32_t t0Ms = 0;
  uint32_t tLastMs = 0;
  uint32_t n = 0;
  float base[3] = {0, 0, -1};
  float peakG = 0;        // signed lateral peak (sign = onset lobe)
  int8_t onsetSign = 0;   // sign of the first sample above kOnsetG
  uint32_t peakMs = 0;    // time of the peak, from window start
  uint32_t endMs = 0;     // time the signal last fell back inside kReleaseG after the peak
  float peakPerp = 0;
  float noisePeak = 0;  // max |lateral|; the "hold still" figure
  double sumSq = 0;     // |lateral|^2 sum, for rms
  bool impact = false;
  float live = 0;  // |lateral| of the latest sample, for the on-screen readout

  static constexpr float kOnsetG = 0.10f;
  static constexpr float kReleaseG = 0.07f;

  void reset() { *this = Capture{}; }

  void add(float ax, float ay, float az, uint32_t tMs, flick::Orientation o) {
    if (!started) {
      started = true;
      t0Ms = tMs;
      tLastMs = tMs;
      base[0] = ax;
      base[1] = ay;
      base[2] = az;
    }
    tLastMs = tMs;
    ++n;
    const float dx = ax - base[0], dy = ay - base[1], dz = az - base[2];
    const flick::Axis axis = flick::screenRight(o);
    const float comp[3] = {dx, dy, dz};
    const float lat = comp[axis.index] * axis.sign;
    const float dynMag = sqrtf(dx * dx + dy * dy + dz * dz);
    const float perp2 = dynMag * dynMag - lat * lat;
    const float perp = perp2 > 0.0f ? sqrtf(perp2) : 0.0f;
    const float mag = sqrtf(ax * ax + ay * ay + az * az);
    if (fabsf(mag - 1.0f) > 0.95f) impact = true;

    live = fabsf(lat);
    if (live > noisePeak) noisePeak = live;
    sumSq += static_cast<double>(lat) * lat;
    if (onsetSign == 0 && live >= kOnsetG) onsetSign = lat > 0.0f ? 1 : -1;
    if (live > fabsf(peakG)) {
      peakG = lat;
      peakMs = tMs - t0Ms;
    }
    if (perp > peakPerp) peakPerp = perp;
    if (fabsf(peakG) >= kOnsetG && live <= kReleaseG) endMs = tMs - t0Ms;
  }

  uint32_t samples() const { return n; }
  uint32_t durationMs() const { return n > 0 ? tLastMs - t0Ms : 0; }
  float rmsG() const { return n > 0 ? static_cast<float>(sqrt(sumSq / n)) : 0.0f; }
  // Time from the first excursion to the peak: the flick/roll discriminator.
  uint32_t riseMs() const { return onsetSign == 0 ? 0xFFFFFFFFu : peakMs; }
  bool enough(const Limits& lim = {}) const { return n >= lim.minSamples; }
};

struct Calibration {
  bool valid = false;
  float triggerG = 0.0f;  // peak |lateral| a flick must reach
  float startG = 0.0f;    // window open threshold (half the trigger)
  int8_t nextSign = flick::kDefaultNextSign;
  float leftPeakG = 0.0f;
  float rightPeakG = 0.0f;
  float noiseG = 0.0f;  // "hold still" peak, recorded so the margin is arguable
};

enum class Reject : uint8_t {
  None = 0,
  TooFewSamples,
  WeakFlick,
  NoisyStill,
  Impact,
  SameDirection,
  Imbalance,
  NoFastOnset,
};

struct Verdict {
  bool ok = false;
  Reject reject = Reject::None;
  Calibration cal;
};

// English, for the log and the `tilt` shell command. The screen maps Reject to
// its own localized strings.
inline const char* rejectName(Reject r) {
  switch (r) {
    case Reject::None:
      return "ok";
    case Reject::TooFewSamples:
      return "too few samples (the flick ended before it was seen)";
    case Reject::WeakFlick:
      return "flick too weak to separate from handling";
    case Reject::NoisyStill:
      return "too much movement while holding still";
    case Reject::Impact:
      return "looked like a drop or a knock";
    case Reject::SameDirection:
      return "both flicks read the same direction (check the axis)";
    case Reject::Imbalance:
      return "the two flicks differ too much in strength";
    case Reject::NoFastOnset:
      return "no fast onset: that was a roll, not a flick";
  }
  return "unknown";
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The derivation. Order matters: a capture that is unusable for a structural
// reason is reported as such before a threshold is computed from it.
inline Verdict derive(const Capture& left, const Capture& right, const Capture& still, const Limits& lim = {}) {
  Verdict v;
  const float lp = fabsf(left.peakG);
  const float rp = fabsf(right.peakG);

  if (!left.enough(lim) || !right.enough(lim) || !still.enough(lim)) {
    v.reject = Reject::TooFewSamples;
    return v;
  }
  if (left.impact || right.impact || still.impact) {
    v.reject = Reject::Impact;
    return v;
  }
  if (still.noisePeak > lim.noiseMaxG) {
    v.reject = Reject::NoisyStill;
    return v;
  }
  if (lp < lim.minPeakG || rp < lim.minPeakG) {
    v.reject = Reject::WeakFlick;
    return v;
  }
  if (lp > lim.maxPeakG || rp > lim.maxPeakG) {
    v.reject = Reject::Impact;
    return v;
  }
  // The noise floor from the still window must be genuinely below both flicks.
  const float floor = still.noisePeak * lim.minSnr;
  if (lp < floor || rp < floor) {
    v.reject = Reject::WeakFlick;
    return v;
  }
  if (left.onsetSign == 0 || right.onsetSign == 0 || left.onsetSign == right.onsetSign) {
    v.reject = Reject::SameDirection;
    return v;
  }
  // A roll ramps to its peak over hundreds of ms; a flick gets there at once.
  if (left.riseMs() > lim.riseMaxMs || right.riseMs() > lim.riseMaxMs) {
    v.reject = Reject::NoFastOnset;
    return v;
  }
  // Both windows must show the signal RETURNING to rest, not just leaving it.
  if (left.endMs <= left.peakMs || right.endMs <= right.peakMs) {
    v.reject = Reject::NoFastOnset;
    return v;
  }
  const float hi = lp > rp ? lp : rp;
  const float lo = lp > rp ? rp : lp;
  if (hi > lo * lim.maxImbalance) {
    v.reject = Reject::Imbalance;
    return v;
  }

  v.cal.valid = true;
  v.cal.leftPeakG = lp;
  v.cal.rightPeakG = rp;
  v.cal.noiseG = still.noisePeak;
  // The right flick must be the opposite of the left one; the onset sign of a
  // RIGHT flick is what turns to the next page.
  v.cal.nextSign = right.onsetSign;
  const float fraction = lim.fraction * lo;
  v.cal.triggerG = clampf(fraction, lim.floorG, lim.capG);
  v.cal.startG = v.cal.triggerG * 0.5f;
  v.ok = true;
  return v;
}

}  // namespace flickcal
