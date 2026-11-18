#include <gtest/gtest.h>

#include "util/PowerGestureClassifier.h"

namespace {

using powergesture::Classifier;
using powergesture::Config;
using powergesture::Kind;

// The Metalio thresholds the awake-UI loop uses: 2.2 s to the power-off dialog,
// 100 ms of continuous UP before a release is believed, 500 ms multi-click.
Config metalioConfig() {
  Config cfg;
  cfg.holdMs = 2200;
  cfg.releaseSettleMs = 100;
  cfg.multiClickMs = 500;
  return cfg;
}

// Press at t, release at t+held, then advance past the settle window.
powergesture::Verdict pressAndRelease(Classifier& classifier, const unsigned long t, const unsigned long held,
                                      const unsigned long settle = 100) {
  classifier.observeLevel(t, true);
  classifier.observeLevel(t + held, false);
  return classifier.poll(t + held + settle);
}

class PowerGestureClassifierTest : public ::testing::Test {
 protected:
  Classifier classifier{metalioConfig()};
};

TEST_F(PowerGestureClassifierTest, ShortPressIsClick) {
  classifier.beginBootMs(0, false);
  const auto v = pressAndRelease(classifier, 1000, 120);
  EXPECT_EQ(v.kind, Kind::Click);
  EXPECT_EQ(v.pressMs, 120u);
  EXPECT_EQ(v.clicks, 1);
}

TEST_F(PowerGestureClassifierTest, ClickBoundaryJustUnderHold) {
  classifier.beginBootMs(0, false);
  const auto v = pressAndRelease(classifier, 1000, 2199);
  EXPECT_EQ(v.kind, Kind::Click);
  EXPECT_EQ(v.pressMs, 2199u);
}

TEST_F(PowerGestureClassifierTest, HoldBoundaryExactlyAtHold) {
  classifier.beginBootMs(0, false);
  const auto v = pressAndRelease(classifier, 1000, 2200);
  EXPECT_EQ(v.kind, Kind::Hold);
  EXPECT_EQ(v.pressMs, 2200u);
  EXPECT_EQ(v.clicks, 0);
}

TEST_F(PowerGestureClassifierTest, HoldWellPastThreshold) {
  classifier.beginBootMs(0, false);
  const auto v = pressAndRelease(classifier, 1000, 6500);
  EXPECT_EQ(v.kind, Kind::Hold);
  EXPECT_EQ(v.pressMs, 6500u);
}

TEST_F(PowerGestureClassifierTest, SecondClickInsideWindowIsMultiClick) {
  classifier.beginBootMs(0, false);
  ASSERT_EQ(pressAndRelease(classifier, 1000, 100).kind, Kind::Click);  // released 1100
  const auto v = pressAndRelease(classifier, 1400, 100);               // released 1500, 400 ms later
  EXPECT_EQ(v.kind, Kind::MultiClick);
  EXPECT_EQ(v.clicks, 2);
}

TEST_F(PowerGestureClassifierTest, SecondClickExactlyAtWindowEdgeIsMultiClick) {
  classifier.beginBootMs(0, false);
  ASSERT_EQ(pressAndRelease(classifier, 1000, 100).kind, Kind::Click);  // released 1100
  // released at 1600: exactly multiClickMs (500) after the previous release.
  const auto v = pressAndRelease(classifier, 1500, 100);
  EXPECT_EQ(v.kind, Kind::MultiClick);
  EXPECT_EQ(v.clicks, 2);
}

TEST_F(PowerGestureClassifierTest, SecondClickOneMsPastWindowIsFreshClick) {
  classifier.beginBootMs(0, false);
  ASSERT_EQ(pressAndRelease(classifier, 1000, 100).kind, Kind::Click);  // released 1100
  const auto v = pressAndRelease(classifier, 1501, 100);                // released 1601
  EXPECT_EQ(v.kind, Kind::Click);
  EXPECT_EQ(v.clicks, 1);
}

TEST_F(PowerGestureClassifierTest, TripleClickCountsThreeAndIsCapped) {
  classifier.beginBootMs(0, false);
  EXPECT_EQ(pressAndRelease(classifier, 1000, 100).clicks, 1u);
  EXPECT_EQ(pressAndRelease(classifier, 1200, 100).clicks, 2u);
  const auto third = pressAndRelease(classifier, 1400, 100);
  EXPECT_EQ(third.kind, Kind::MultiClick);
  EXPECT_EQ(third.clicks, 3u);

  Classifier capped([] {
    Config cfg = metalioConfig();
    cfg.maxClicks = 2;
    return cfg;
  }());
  capped.beginBootMs(0, false);
  EXPECT_EQ(pressAndRelease(capped, 1000, 100).clicks, 1u);
  EXPECT_EQ(pressAndRelease(capped, 1200, 100).clicks, 2u);
  EXPECT_EQ(pressAndRelease(capped, 1400, 100).clicks, 2u);  // capped, no uint8 wrap
}

TEST_F(PowerGestureClassifierTest, HoldEndsABurst) {
  classifier.beginBootMs(0, false);
  ASSERT_EQ(pressAndRelease(classifier, 1000, 100).kind, Kind::Click);
  ASSERT_EQ(pressAndRelease(classifier, 1200, 2300).kind, Kind::Hold);
  // A click right after a hold is a fresh click, not a multi-click continuation.
  const auto v = pressAndRelease(classifier, 1700, 100);
  EXPECT_EQ(v.kind, Kind::Click);
  EXPECT_EQ(v.clicks, 1);
}

TEST_F(PowerGestureClassifierTest, ReleaseInsideSettleWindowIsNotYetAVerdict) {
  classifier.beginBootMs(0, false);
  classifier.observeLevel(1000, true);
  classifier.observeLevel(1100, false);
  EXPECT_EQ(classifier.poll(1150).kind, Kind::None);  // 50 ms of UP: not believed
  EXPECT_EQ(classifier.poll(1200).kind, Kind::Click);  // exactly settleMs: believed
}

TEST_F(PowerGestureClassifierTest, GlitchDuringHoldDoesNotRestartTheClock) {
  // A failed expander read reports the key up for one tick; the SAME press must
  // keep its original DOWN stamp, or a 3 s hold measures as a short one.
  classifier.beginBootMs(0, false);
  classifier.observeLevel(1000, true);
  classifier.observeLevel(1200, false);  // one UP tick (within settle)
  classifier.observeLevel(1250, true);   // back down: same press
  EXPECT_EQ(classifier.pressStartMs(), 1000u);
  EXPECT_EQ(classifier.heldMs(4000), 3000u);
  EXPECT_EQ(classifier.poll(4100).kind, Kind::None);  // still down, nothing to report
  classifier.observeLevel(4000, false);
  const auto v = classifier.poll(4200);
  EXPECT_EQ(v.kind, Kind::Hold);
  EXPECT_EQ(v.pressMs, 3000u);  // measured to the real release, not the glitch
}

TEST_F(PowerGestureClassifierTest, SettleWindowDoesNotStretchPressDuration) {
  // Duration is measured to the FIRST UP tick, so a long settle can never push
  // a press over the hold bound.
  classifier.beginBootMs(0, false);
  classifier.observeLevel(1000, true);
  classifier.observeLevel(2190, false);  // 1190 ms press, well under hold
  const auto v = classifier.poll(9000);
  EXPECT_EQ(v.kind, Kind::Click);
  EXPECT_EQ(v.pressMs, 2190u - 1000u);
}

// ---- soft restart -------------------------------------------------------

TEST_F(PowerGestureClassifierTest, PressInheritedAcrossRestartIsDiscarded) {
  // The image reboots while the finger is still on the key. millis() is back at
  // 40 ms; the press must not be read as a click when it is finally released.
  classifier.beginBootMs(40, true);
  EXPECT_TRUE(classifier.pressInherited());
  EXPECT_TRUE(classifier.pressDown());
  classifier.observeLevel(900, false);
  const auto v = classifier.poll(1000);
  EXPECT_EQ(v.kind, Kind::Discarded);
  EXPECT_EQ(v.clicks, 0);
}

TEST_F(PowerGestureClassifierTest, InheritedPressDoesNotFrameABurst) {
  classifier.beginBootMs(40, true);
  classifier.observeLevel(900, false);
  ASSERT_EQ(classifier.poll(1000).kind, Kind::Discarded);
  // The first real press after the restart is a plain Click, not a MultiClick
  // continuing a burst that belonged to the previous boot.
  const auto v = pressAndRelease(classifier, 1200, 100);
  EXPECT_EQ(v.kind, Kind::Click);
  EXPECT_EQ(v.clicks, 1);
}

TEST_F(PowerGestureClassifierTest, InheritedLongHoldIsNotAHold) {
  // A user who holds the key across a silent restart gets no power-off dialog
  // from the inherited press; they must release and press again.
  classifier.beginBootMs(40, true);
  classifier.observeLevel(5000, false);
  const auto v = classifier.poll(5100);
  EXPECT_EQ(v.kind, Kind::Discarded);
  EXPECT_NE(v.kind, Kind::Hold);
}

TEST_F(PowerGestureClassifierTest, RestartWithKeyUpStartsClean) {
  Classifier before(metalioConfig());
  before.beginBootMs(0, false);
  ASSERT_EQ(pressAndRelease(before, 1000, 100).kind, Kind::Click);

  // Soft restart: new epoch, key up. The old burst must not survive.
  classifier.beginBootMs(30, false);
  EXPECT_FALSE(classifier.pressDown());
  EXPECT_EQ(classifier.poll(200).kind, Kind::None);
  const auto v = pressAndRelease(classifier, 400, 100);
  EXPECT_EQ(v.kind, Kind::Click);
  EXPECT_EQ(v.clicks, 1);
}

TEST_F(PowerGestureClassifierTest, ResetPressDropsThePressButKeepsTheBurst) {
  // Entry into standby: the press that slept the device must not be
  // re-classified, but the click history still frames the next click.
  classifier.beginBootMs(0, false);
  ASSERT_EQ(pressAndRelease(classifier, 1000, 100).kind, Kind::Click);
  classifier.observeLevel(1200, true);
  classifier.resetPress(1300);
  EXPECT_FALSE(classifier.pressDown());
  EXPECT_EQ(classifier.poll(1400).kind, Kind::None);
  // Released 200 ms after the pre-standby click: the burst was kept, so this is
  // still a multi-click and not a fresh one.
  const auto v = pressAndRelease(classifier, 1400, 100);
  EXPECT_EQ(v.kind, Kind::MultiClick);
  EXPECT_EQ(v.clicks, 2);
}

TEST_F(PowerGestureClassifierTest, ResetClearsEverything) {
  classifier.beginBootMs(0, false);
  ASSERT_EQ(pressAndRelease(classifier, 1000, 100).kind, Kind::Click);
  classifier.reset(2000, false);
  const auto v = pressAndRelease(classifier, 2100, 100);
  EXPECT_EQ(v.kind, Kind::Click);
  EXPECT_EQ(v.clicks, 1);
}

TEST_F(PowerGestureClassifierTest, HeldMsIsZeroWhenUpOrInherited) {
  classifier.beginBootMs(0, false);
  EXPECT_EQ(classifier.heldMs(500), 0u);
  classifier.observeLevel(1000, true);
  EXPECT_EQ(classifier.heldMs(1600), 600u);
  classifier.observeLevel(1600, false);
  EXPECT_EQ(classifier.heldMs(1650), 0u);  // released: no duration while up

  Classifier inherited(metalioConfig());
  inherited.beginBootMs(10, true);
  EXPECT_EQ(inherited.heldMs(500), 0u);  // inherited press has no measurable duration
}

TEST_F(PowerGestureClassifierTest, TimestampWrapDoesNotChangeClassification) {
  // millis() wraps every ~49.7 days; unsigned subtraction keeps the duration sane.
  Classifier c(metalioConfig());
  c.beginBootMs(0, false);
  const unsigned long start = 0xFFFFFF00UL;
  c.observeLevel(start, true);
  c.observeLevel(start + 300, false);  // wraps past zero
  const auto v = c.poll(start + 400);
  EXPECT_EQ(v.kind, Kind::Click);
  EXPECT_EQ(v.pressMs, 300u);
}

TEST_F(PowerGestureClassifierTest, KindNamesAreStable) {
  EXPECT_STREQ(powergesture::kindName(Kind::None), "none");
  EXPECT_STREQ(powergesture::kindName(Kind::Click), "click");
  EXPECT_STREQ(powergesture::kindName(Kind::MultiClick), "multi-click");
  EXPECT_STREQ(powergesture::kindName(Kind::Hold), "hold");
  EXPECT_STREQ(powergesture::kindName(Kind::Discarded), "discarded");
}

}  // namespace
