// Host replay of the x-axis flick detector (lib/hal/FlickDetector.h) and the
// calibration derivation (lib/hal/FlickCalibration.h).
//
// Every case is a synthetic accelerometer trace at the sampling interval the
// reader loop actually provides (~10 ms). The event counts are the point: one
// flick is one page, and ordinary handling is zero pages. The traces model what
// the part measures, not a filtered abstraction:
//   * a flick is a biphasic lateral pulse (accelerate, then stop) of ~130 ms;
//   * a roll moves gravity, so the lateral component STEPS and then stays;
//   * a set-down is a vertical impulse with a scuff;
//   * a pick-up is a sustained vertical push.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "hal/FlickCalibration.h"
#include "hal/FlickDetector.h"

namespace {

constexpr float kDegToRad = 3.14159265358979f / 180.0f;
constexpr uint32_t kDt = 10;  // ms
using flick::Event;
using flick::Orientation;

struct Sample {
  float x, y, z;
};
using Trace = std::vector<Sample>;

// Gravity in the device frame for a screen held vertical in `o`, rolled by
// theta degrees about the content's up axis (the axis a lateral flick rolls
// around). Rolling is what moves gravity into the content's x axis.
void gravity(Orientation o, float thetaDeg, float& ax, float& ay, float& az) {
  const float c = cosf(thetaDeg * kDegToRad);
  const float s = sinf(thetaDeg * kDegToRad);
  ax = 0;
  ay = 0;
  az = 0;
  switch (o) {
    case Orientation::Inverted:
      ay = c;
      break;
    case Orientation::LandscapeCw:
      ax = -c;
      break;
    case Orientation::LandscapeCcw:
      ax = c;
      break;
    case Orientation::Portrait:
    default:
      ay = -c;
      break;
  }
  // A roll about the up axis tilts gravity into the content's RIGHT axis.
  const flick::Axis right = flick::screenRight(o);
  const float comp[3] = {ax, ay, az};
  const float gx = s * right.sign;  // gravity component along content-right
  float out[3] = {comp[0], comp[1], comp[2]};
  out[right.index] += gx;
  ax = out[0];
  ay = out[1];
  az = out[2];
}

void addLateral(Orientation o, float g, float& ax, float& ay, float& az) {
  const flick::Axis right = flick::screenRight(o);
  float comp[3] = {ax, ay, az};
  comp[right.index] += g * right.sign;
  ax = comp[0];
  ay = comp[1];
  az = comp[2];
}

// A flick: the hand accelerates sideways, then stops it. amp is the peak
// applied acceleration, durMs the length of the whole pulse.
Trace flickTrace(Orientation o, bool right, float amp, float durMs, int totalSamples = 100) {
  Trace t;
  for (int i = 0; i < totalSamples; ++i) {
    const float ms = static_cast<float>(i * kDt);
    float ax, ay, az;
    gravity(o, 0.0f, ax, ay, az);
    if (ms < durMs) {
      const float lobe = amp * sinf(ms / durMs * 6.2831853f) * (right ? 1.0f : -1.0f);
      addLateral(o, lobe, ax, ay, az);
    }
    t.push_back({ax, ay, az});
  }
  return t;
}

// A slow roll into a new resting angle, then held: 30 deg over 1.2 s.
Trace slowRollTrace(Orientation o = Orientation::Portrait) {
  Trace t;
  for (int i = 0; i < 250; ++i) {
    const float ms = static_cast<float>(i * kDt);
    const float th = ms < 200.0f ? 0.0f : 30.0f * fminf(1.0f, (ms - 200.0f) / 1200.0f);
    float ax, ay, az;
    gravity(o, th, ax, ay, az);
    t.push_back({ax, ay, az});
  }
  return t;
}

// A fast tilt that is then HELD: the angle arrives in 120 ms and stays.
Trace heldTiltTrace(float deg, float rampMs, Orientation o = Orientation::Portrait) {
  Trace t;
  for (int i = 0; i < 250; ++i) {
    const float ms = static_cast<float>(i * kDt);
    const float th = ms < 200.0f ? 0.0f : deg * fminf(1.0f, (ms - 200.0f) / rampMs);
    float ax, ay, az;
    gravity(o, th, ax, ay, az);
    t.push_back({ax, ay, az});
  }
  return t;
}

// Set down on a table: a vertical impulse and a scuff.
Trace setDownTrace() {
  Trace t;
  for (int i = 0; i < 100; ++i) {
    const float ms = static_cast<float>(i * kDt);
    float ax = 0, ay = -1, az = 0;
    if (ms >= 300.0f && ms < 350.0f) {
      const float u = (ms - 300.0f) / 50.0f;
      az = 1.5f * sinf(u * 3.14159f);
      ax = 0.45f * sinf(u * 3.14159f);
    }
    t.push_back({ax, ay, az});
  }
  return t;
}

// Picked up and held: a sustained vertical push with a little sideways drift.
Trace pickUpTrace() {
  Trace t;
  for (int i = 0; i < 120; ++i) {
    const float ms = static_cast<float>(i * kDt);
    float az = 0;
    if (ms >= 300.0f && ms < 900.0f) az = 0.6f * sinf((ms - 300.0f) / 600.0f * 3.14159f);
    t.push_back({0.08f, -1.0f, az});
  }
  return t;
}

// Two flicks 300 ms apart: the second is inside the 500 ms refractory.
Trace twoFlicksTrace() {
  const Trace a = flickTrace(Orientation::Portrait, true, 0.8f, 120.0f, 30);
  const Trace b = flickTrace(Orientation::Portrait, true, 0.8f, 120.0f, 100);
  Trace t(a.begin(), a.end());
  t.insert(t.end(), b.begin(), b.end());
  return t;
}

struct Counts {
  int next = 0;
  int prev = 0;
  int events() const { return next + prev; }
};

Counts run(const Trace& t, const flick::Config& cfg = {}, Orientation o = Orientation::Portrait,
           flick::Mode m = flick::Mode::Normal) {
  flick::State st;
  Counts c;
  for (const auto& s : t) {
    const Event e = flick::step(st, cfg, s.x, s.y, s.z, kDt, false, o, m);
    if (e == Event::Next) ++c.next;
    if (e == Event::Prev) ++c.prev;
  }
  return c;
}

// Calibration capture over a raw trace.
flickcal::Capture capture(const Trace& t, Orientation o = Orientation::Portrait) {
  flickcal::Capture c;
  uint32_t ms = 0;
  for (const auto& s : t) {
    c.add(s.x, s.y, s.z, ms, o);
    ms += kDt;
  }
  return c;
}

}  // namespace

// ---- the gesture -------------------------------------------------------

TEST(FlickDetector, RightFlickTurnsOnePage) {
  const Counts c = run(flickTrace(Orientation::Portrait, true, 0.8f, 130.0f));
  EXPECT_EQ(c.next, 1) << "a right flick must be exactly one NEXT page";
  EXPECT_EQ(c.prev, 0);
}

TEST(FlickDetector, LeftFlickTurnsOnePageTheOtherWay) {
  const Counts c = run(flickTrace(Orientation::Portrait, false, 0.8f, 130.0f));
  EXPECT_EQ(c.next, 0);
  EXPECT_EQ(c.prev, 1) << "a left flick must be exactly one PREV page";
}

TEST(FlickDetector, InvertedModeSwapsTheDirection) {
  const Counts c = run(flickTrace(Orientation::Portrait, true, 0.8f, 130.0f), {}, Orientation::Portrait,
                       flick::Mode::Inverted);
  EXPECT_EQ(c.next, 0);
  EXPECT_EQ(c.prev, 1);
}

TEST(FlickDetector, OrientationMapsTheAxisAndKeepsTheDirection) {
  struct Case {
    Orientation o;
    const char* name;
  } cases[] = {{Orientation::LandscapeCw, "landscape CW"},
               {Orientation::LandscapeCcw, "landscape CCW"},
               {Orientation::Inverted, "inverted"}};
  for (const auto& c : cases) {
    const Counts right = run(flickTrace(c.o, true, 0.8f, 130.0f), {}, c.o);
    const Counts left = run(flickTrace(c.o, false, 0.8f, 130.0f), {}, c.o);
    EXPECT_EQ(right.next, 1) << c.name;
    EXPECT_EQ(right.prev, 0) << c.name;
    EXPECT_EQ(left.next, 0) << c.name;
    EXPECT_EQ(left.prev, 1) << c.name;
  }
}

TEST(FlickDetector, SoftFlickStillFiresInTheRightDirection) {
  // 0.45 g is at the edge of the default trigger (0.40 g). The point of this
  // case is the DIRECTION: a biphasic pulse crosses zero, and the stop lobe is
  // sharper than the start, so a detector that reads the peak's sign turns the
  // wrong way on exactly the soft flicks.
  const Counts c = run(flickTrace(Orientation::Portrait, true, 0.45f, 160.0f));
  EXPECT_EQ(c.next, 1);
  EXPECT_EQ(c.prev, 0);
}

TEST(FlickDetector, VeryShortFlickFires) {
  const Counts c = run(flickTrace(Orientation::Portrait, true, 1.0f, 60.0f));
  EXPECT_EQ(c.next, 1);
}

TEST(FlickDetector, HoldingStillFiresNothing) {
  Trace t;
  for (int i = 0; i < 300; ++i) t.push_back({0.0f, -1.0f, 0.0f});
  EXPECT_EQ(run(t).events(), 0);
}

// ---- everything that must NOT page ------------------------------------

TEST(FlickDetector, SlowRollIntoANewAngleDoesNotFire) {
  EXPECT_EQ(run(slowRollTrace()).events(), 0);
}

TEST(FlickDetector, HeldTiltDoesNotFire) {
  // 25, 15 and 12 degrees, each arriving in 120 ms and then held for 2 s. The
  // old detector fired on all three; a held angle is not the gesture.
  for (const float deg : {25.0f, 15.0f, 12.0f}) {
    EXPECT_EQ(run(heldTiltTrace(deg, 120.0f)).events(), 0) << deg << " deg held";
  }
}

TEST(FlickDetector, PickUpDoesNotFire) {
  EXPECT_EQ(run(pickUpTrace()).events(), 0);
}

TEST(FlickDetector, SetDownDoesNotFire) {
  EXPECT_EQ(run(setDownTrace()).events(), 0);
}

TEST(FlickDetector, TwoFlicksInsideTheRefractoryTurnOnePage) {
  const Counts c = run(twoFlicksTrace());
  EXPECT_EQ(c.events(), 1) << "one flick, one page: the second is inside the refractory";
}

TEST(FlickDetector, TouchSuppressesTheGesture) {
  flick::State st;
  int events = 0;
  for (const auto& s : flickTrace(Orientation::Portrait, true, 0.8f, 130.0f)) {
    const Event e = flick::step(st, {}, s.x, s.y, s.z, kDt, /*touchActive=*/true, Orientation::Portrait,
                                flick::Mode::Normal);
    if (e != Event::None) ++events;
  }
  EXPECT_EQ(events, 0);
}

TEST(FlickDetector, OffResetsAndNeverFires) {
  flick::State st;
  int events = 0;
  for (const auto& s : flickTrace(Orientation::Portrait, true, 0.8f, 130.0f)) {
    const Event e = flick::step(st, {}, s.x, s.y, s.z, kDt, false, Orientation::Portrait, flick::Mode::Off);
    if (e != Event::None) ++events;
  }
  EXPECT_EQ(events, 0);
}

TEST(FlickDetector, FiresAgainAfterTheRefractory) {
  Trace t = flickTrace(Orientation::Portrait, true, 0.8f, 130.0f, 100);  // 1 s
  const Trace second = flickTrace(Orientation::Portrait, true, 0.8f, 130.0f, 100);
  t.insert(t.end(), second.begin(), second.end());  // 1 s later, well past 500 ms
  EXPECT_EQ(run(t).next, 2);
}

// ---- calibration maths -------------------------------------------------

TEST(FlickCalibration, CapturesMeasurePeakSignAndRise) {
  const auto left = capture(flickTrace(Orientation::Portrait, false, 0.85f, 130.0f, 90));
  const auto right = capture(flickTrace(Orientation::Portrait, true, 0.78f, 130.0f, 90));
  EXPECT_LT(left.peakG, -0.7f) << "left flick onset lobe is negative on content-right";
  EXPECT_GT(right.peakG, 0.7f);
  EXPECT_EQ(left.onsetSign, -1);
  EXPECT_EQ(right.onsetSign, +1);
  EXPECT_LE(left.riseMs(), 150u);
}

TEST(FlickCalibration, DerivesAFractionOfTheWeakerPeak) {
  const auto left = capture(flickTrace(Orientation::Portrait, false, 0.85f, 130.0f, 90));
  const auto right = capture(flickTrace(Orientation::Portrait, true, 0.78f, 130.0f, 90));
  const auto still = capture(Trace(90, Sample{0.0f, -1.0f, 0.0f}));
  const auto v = flickcal::derive(left, right, still);
  ASSERT_TRUE(v.ok);
  EXPECT_NEAR(v.cal.triggerG, 0.35f * 0.78f, 0.03f);
  EXPECT_NEAR(v.cal.startG, v.cal.triggerG * 0.5f, 1e-4f);
  EXPECT_EQ(v.cal.nextSign, +1) << "a RIGHT flick must be the NEXT page";
  EXPECT_TRUE(v.cal.valid);
}

TEST(FlickCalibration, FloorWinsForALightUnit) {
  // Both flicks just clear minPeakG, so 0.35 x peak is below the 0.20 g floor.
  const auto left = capture(flickTrace(Orientation::Portrait, false, 0.36f, 150.0f, 90));
  const auto right = capture(flickTrace(Orientation::Portrait, true, 0.36f, 150.0f, 90));
  const auto still = capture(Trace(90, Sample{0.0f, -1.0f, 0.0f}));
  const auto v = flickcal::derive(left, right, still);
  ASSERT_TRUE(v.ok) << flickcal::rejectName(v.reject);
  EXPECT_NEAR(v.cal.triggerG, 0.20f, 1e-5f);
}

TEST(FlickCalibration, RejectsAWeakFlick) {
  const auto left = capture(flickTrace(Orientation::Portrait, false, 0.05f, 130.0f, 90));
  const auto right = capture(flickTrace(Orientation::Portrait, true, 0.78f, 130.0f, 90));
  const auto still = capture(Trace(90, Sample{0.0f, -1.0f, 0.0f}));
  const auto v = flickcal::derive(left, right, still);
  EXPECT_FALSE(v.ok);
  EXPECT_EQ(v.reject, flickcal::Reject::WeakFlick);
}

TEST(FlickCalibration, RejectsAFlickTooCloseToTheNoiseFloor) {
  // 0.32 g clears minPeakG but the still window moves 0.20 g: 3x that is 0.60 g,
  // so the capture cannot separate the gesture from the hand.
  Trace noisyStill(90);
  noisyStill[0] = {0.0f, -1.0f, 0.0f};
  for (size_t i = 1; i < noisyStill.size(); ++i) {
    const float ax = (i % 2 == 0) ? 0.20f : -0.20f;
    noisyStill[i] = {ax, -1.0f, 0.0f};
  }
  const auto left = capture(flickTrace(Orientation::Portrait, false, 0.32f, 130.0f, 90));
  const auto right = capture(flickTrace(Orientation::Portrait, true, 0.32f, 130.0f, 90));
  const auto v = flickcal::derive(left, right, capture(noisyStill));
  EXPECT_FALSE(v.ok);
  EXPECT_EQ(v.reject, flickcal::Reject::WeakFlick);
}

TEST(FlickCalibration, RejectsAMovingStillWindow) {
  const auto left = capture(flickTrace(Orientation::Portrait, false, 0.85f, 130.0f, 90));
  const auto right = capture(flickTrace(Orientation::Portrait, true, 0.78f, 130.0f, 90));
  // "Hold still" recorded a 0.30 g sway: over the noise limit.
  const auto still = capture(flickTrace(Orientation::Portrait, true, 0.30f, 400.0f, 90));
  const auto v = flickcal::derive(left, right, still);
  EXPECT_FALSE(v.ok);
  EXPECT_EQ(v.reject, flickcal::Reject::NoisyStill);
}

TEST(FlickCalibration, RejectsTwoFlicksInTheSameDirection) {
  const auto left = capture(flickTrace(Orientation::Portrait, false, 0.80f, 130.0f, 90));
  const auto right = capture(flickTrace(Orientation::Portrait, false, 0.80f, 130.0f, 90));
  const auto still = capture(Trace(90, Sample{0.0f, -1.0f, 0.0f}));
  const auto v = flickcal::derive(left, right, still);
  EXPECT_FALSE(v.ok);
  EXPECT_EQ(v.reject, flickcal::Reject::SameDirection);
}

TEST(FlickCalibration, RejectsARollRecordedAsAFlick) {
  // 0.55 g that takes 800 ms to arrive is a roll, not a flick.
  const auto left = capture(flickTrace(Orientation::Portrait, false, 0.85f, 130.0f, 90));
  const auto roll = capture(flickTrace(Orientation::Portrait, true, 0.55f, 800.0f, 90));
  const auto still = capture(Trace(90, Sample{0.0f, -1.0f, 0.0f}));
  const auto v = flickcal::derive(left, roll, still);
  EXPECT_FALSE(v.ok);
  EXPECT_EQ(v.reject, flickcal::Reject::NoFastOnset);
}

TEST(FlickCalibration, RejectsACaptureWithTooFewSamples) {
  const auto left = capture(flickTrace(Orientation::Portrait, false, 0.85f, 130.0f, 4));
  const auto right = capture(flickTrace(Orientation::Portrait, true, 0.78f, 130.0f, 90));
  const auto still = capture(Trace(90, Sample{0.0f, -1.0f, 0.0f}));
  const auto v = flickcal::derive(left, right, still);
  EXPECT_FALSE(v.ok);
  EXPECT_EQ(v.reject, flickcal::Reject::TooFewSamples);
}

TEST(FlickCalibration, RejectsAViolentCapture) {
  const auto left = capture(flickTrace(Orientation::Portrait, false, 1.9f, 130.0f, 90));
  const auto right = capture(flickTrace(Orientation::Portrait, true, 0.78f, 130.0f, 90));
  const auto still = capture(Trace(90, Sample{0.0f, -1.0f, 0.0f}));
  const auto v = flickcal::derive(left, right, still);
  EXPECT_FALSE(v.ok);
  EXPECT_EQ(v.reject, flickcal::Reject::Impact);
}

TEST(FlickCalibration, DerivedThresholdsDriveTheDetector) {
  const auto left = capture(flickTrace(Orientation::Portrait, false, 0.85f, 130.0f, 90));
  const auto right = capture(flickTrace(Orientation::Portrait, true, 0.78f, 130.0f, 90));
  const auto still = capture(Trace(90, Sample{0.0f, -1.0f, 0.0f}));
  const auto v = flickcal::derive(left, right, still);
  ASSERT_TRUE(v.ok);

  flick::Config cfg;
  cfg.triggerG = v.cal.triggerG;
  cfg.startG = v.cal.startG;
  cfg.nextSign = v.cal.nextSign;

  const Counts r = run(flickTrace(Orientation::Portrait, true, 0.45f, 130.0f), cfg);
  const Counts l = run(flickTrace(Orientation::Portrait, false, 0.45f, 130.0f), cfg);
  EXPECT_EQ(r.next, 1);
  EXPECT_EQ(r.prev, 0);
  EXPECT_EQ(l.prev, 1);
  EXPECT_EQ(l.next, 0);
  // A calibrated unit must still ignore a held tilt at the lower threshold.
  EXPECT_EQ(run(heldTiltTrace(25.0f, 120.0f), cfg).events(), 0);
}
