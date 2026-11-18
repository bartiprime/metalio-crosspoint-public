// BATT5 — the "canvas == glass" licence, on the host.
//
// Everything below is the REAL header (lib/GfxRenderer/GlassEpoch.h) that
// GfxRenderer and src/display/BatteryIconPatch.cpp compile against, so a change to
// the arithmetic, to the gate, or to what counts as evidence fails here before it
// reaches the panel.
//
// The failure this file exists to prevent is a FALSE EQUAL: a licence granted over
// a framebuffer that is not the frame on the glass, which is what lets a region
// patch drive the wrong content (the corner-only blank screen). Every test below
// that ends in "refused" is that direction.

#include <gtest/gtest.h>

#include <cstdint>

#include "GlassEpoch.h"

using cpglass::Epochs;
using cpglass::Licence;
using cpglass::Refusal;

namespace {

// A licence in which every term is positive except the ones the test varies: the
// rect was recorded at the epoch the glass holds, the rect is plausible, and no
// one-shot guard is armed.
Licence baseLicence(const Epochs& e) {
  Licence l;
  l.canvasIsGlass = e.canvasIsGlass();
  l.hasRect = true;
  l.rectPlausible = true;
  l.rectEpoch = e.glass;
  l.glassEpoch = e.glass;
  l.guardsClear = true;
  return l;
}

// One driven paint: compose some pixels, then a drive that lands.
void paintAndDrive(Epochs& e, int pixels = 100) {
  for (int i = 0; i < pixels; ++i) e.noteCanvasWrite();
  e.noteDriveLanded();
}

}  // namespace

// ---------------------------------------------------------------------------
// The fact itself.
// ---------------------------------------------------------------------------

TEST(GlassEpoch, NothingDrivenYetIsNotEqual) {
  // 0 == 0 must NOT read as "the canvas is on the glass": at boot nothing has been
  // put on the panel, so a patch there would drive a frame the panel never had.
  Epochs e;
  EXPECT_FALSE(e.canvasIsGlass());
  EXPECT_FALSE(cpglass::licensed(baseLicence(e)));
}

TEST(GlassEpoch, EqualAfterADriveLands) {
  Epochs e;
  paintAndDrive(e);
  EXPECT_TRUE(e.canvasIsGlass());
  EXPECT_EQ(e.composed, e.glass);
}

TEST(GlassEpoch, ComposeAfterDriveBreaksEquality) {
  Epochs e;
  paintAndDrive(e);
  ASSERT_TRUE(e.canvasIsGlass());
  e.noteCanvasWrite();  // one pixel composed, nothing driven since
  EXPECT_FALSE(e.canvasIsGlass());
  EXPECT_EQ(cpglass::refusal(baseLicence(e)), Refusal::CanvasNotGlass);
}

TEST(GlassEpoch, DriveWithoutComposeStaysEqual) {
  // Two paints of the SAME frame: the second drive composes nothing, and it must
  // still leave the canvas on the glass.
  Epochs e;
  paintAndDrive(e);
  const uint32_t composedAfterFirst = e.composed;
  e.noteDriveLanded();
  EXPECT_EQ(e.composed, composedAfterFirst);
  EXPECT_TRUE(e.canvasIsGlass());
  EXPECT_TRUE(cpglass::licensed(baseLicence(e)));
}

TEST(GlassEpoch, FailedDriveBreaksEqualityEvenWithNoCompose) {
  // The dangerous case: an unchanged canvas, a drive that did NOT land. Equality
  // here would say "the panel is showing this frame" when it may be half-driven.
  Epochs e;
  paintAndDrive(e);
  ASSERT_TRUE(e.canvasIsGlass());
  e.noteDriveFailed();
  EXPECT_FALSE(e.canvasIsGlass());
  EXPECT_FALSE(cpglass::licensed(baseLicence(e)));
}

TEST(GlassEpoch, ComposingBackToTheSameCountIsNotPossible) {
  // Sanity on the counter: it is monotonic, so an "equal" cannot be re-established
  // by arithmetic drift. (A wrap-around needs 2^32 mutations; the test pins the
  // behaviour that matters -- a write always moves the composed value.)
  Epochs e;
  paintAndDrive(e);
  const uint32_t before = e.composed;
  e.noteCanvasWrite();
  EXPECT_NE(e.composed, before);
  EXPECT_NE(e.composed, e.glass);
}

// ---------------------------------------------------------------------------
// The gate: equality alone is not a licence.
// ---------------------------------------------------------------------------

TEST(IconPatchGate, LicensedWhenEveryTermHolds) {
  Epochs e;
  paintAndDrive(e);
  const Licence l = baseLicence(e);
  EXPECT_TRUE(cpglass::licensed(l));
  EXPECT_EQ(cpglass::refusal(l), Refusal::None);
  EXPECT_FALSE(cpglass::needsWholeActivityRepaint(cpglass::refusal(l)));
}

TEST(IconPatchGate, NoRectAsksForTheWholeActivityRepaint) {
  Epochs e;
  paintAndDrive(e);
  Licence l = baseLicence(e);
  l.hasRect = false;
  EXPECT_FALSE(cpglass::licensed(l));
  EXPECT_EQ(cpglass::refusal(l), Refusal::NoRect);
  EXPECT_TRUE(cpglass::needsWholeActivityRepaint(cpglass::refusal(l)));
}

TEST(IconPatchGate, BadRectAsksForTheWholeActivityRepaint) {
  Epochs e;
  paintAndDrive(e);
  Licence l = baseLicence(e);
  l.hasRect = true;
  l.rectPlausible = false;
  EXPECT_FALSE(cpglass::licensed(l));
  EXPECT_EQ(cpglass::refusal(l), Refusal::BadRect);
  EXPECT_TRUE(cpglass::needsWholeActivityRepaint(cpglass::refusal(l)));
}

TEST(IconPatchGate, RectFromAPaintThatNeverDroveIsRefused) {
  // The BATT2 case, now structural: the rect was recorded while composing, but the
  // paint was held / declined, so commitBatteryIconRect() left its epoch at 0.
  Epochs e;
  paintAndDrive(e);
  Licence l = baseLicence(e);
  l.rectEpoch = 0;  // uncommitted
  EXPECT_FALSE(cpglass::licensed(l));
  EXPECT_EQ(cpglass::refusal(l), Refusal::RectNotFromGlassFrame);
  EXPECT_FALSE(cpglass::needsWholeActivityRepaint(cpglass::refusal(l)));
}

TEST(IconPatchGate, AnArmedOneShotIsAPendingSkipNotAFallback) {
  Epochs e;
  paintAndDrive(e);
  Licence l = baseLicence(e);
  l.guardsClear = false;
  EXPECT_FALSE(cpglass::licensed(l));
  EXPECT_EQ(cpglass::refusal(l), Refusal::GuardArmed);
  EXPECT_FALSE(cpglass::needsWholeActivityRepaint(cpglass::refusal(l)));
}

TEST(IconPatchGate, EverySingleTermFalseRefuses) {
  // The patch path cannot run when the guard says no: one false term is enough, for
  // each of the five terms independently. This is the "cannot run when the guard
  // says no" proof, enumerated rather than argued.
  Epochs e;
  paintAndDrive(e);
  const Licence good = baseLicence(e);
  ASSERT_TRUE(cpglass::licensed(good));

  Licence l;

  l = good;
  l.canvasIsGlass = false;
  EXPECT_FALSE(cpglass::licensed(l));

  l = good;
  l.hasRect = false;
  EXPECT_FALSE(cpglass::licensed(l));

  l = good;
  l.rectPlausible = false;
  EXPECT_FALSE(cpglass::licensed(l));

  l = good;
  l.rectEpoch = good.glassEpoch + 1;  // a rect recorded by a later, undriven paint
  EXPECT_FALSE(cpglass::licensed(l));

  l = good;
  l.guardsClear = false;
  EXPECT_FALSE(cpglass::licensed(l));
}

TEST(IconPatchGate, EqualityAloneIsNotALicence) {
  // A clean proof that the rect terms are not decorative: with the fact holding and
  // nothing else, a missing rect still refuses.
  Epochs e;
  paintAndDrive(e);
  Licence l = baseLicence(e);
  l.hasRect = false;
  EXPECT_TRUE(l.canvasIsGlass);
  EXPECT_FALSE(cpglass::licensed(l));
}
