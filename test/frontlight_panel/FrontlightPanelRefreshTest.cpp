// ---------------------------------------------------------------------------
// FLP1 host test (r101.21b) — the frontlight panel's repaint decision.
//
// THE CLAIMS, in the operator's words:
//   * "adjusting brightness/warmth causes a full-panel refresh; expected
//     partial (FAST) while adjusting"      -> an adjust resolves to CP_RM_FAST
//                                              AND the row-9 completion (a
//                                              second whole-panel GL16) does
//                                              not run for it.
//   * "one clean from white when leaving the panel" -> an exit request resolves
//     to CP_RM_FULL for every latch state, and it is ONE drive however many
//     times close() is reached.
//   * night tile / entering unchanged       -> the plan is bit-identical.
//
// Everything below is the renderer's real decision chain, in order:
//   cp_refresh_resolve_request()   (producer: AUTO ratio, then armed promotion)
//   cp_refresh_resolve_mode()      (policy: clean precedence, whole-frame,
//                                   no-HALF-after-FULL)
//   the row-9 completion condition (GfxRenderer.cpp: FAST && arm && !suppressed)
// ---------------------------------------------------------------------------

#include <cstring>

#include <gtest/gtest.h>

#include "activities/util/FrontlightPanelRefresh.h"
#include "display/RefreshPolicy.h"

namespace {

using flpanel::Event;
using flpanel::Plan;

// The request the producer would hand the policy, for a paint whose call site
// asked for `caller` and whose one-shot promotion (if any) is `promoted` (-1 =
// none armed). AUTO is passed as NOT pending: the promotion, when armed, wins
// over it by construction, and the point of the fix is that the panel no longer
// depends on the measured ratio.
int producer(int caller, int promoted) {
  return cp_refresh_resolve_request(caller, /*auto_pending=*/0, /*auto_mode=*/CP_RM_FAST,
                                    /*promoted_pending=*/(promoted >= 0 ? 1 : 0), promoted);
}

// The mode the policy drives, given what the latch says was last on the glass.
int policy(int request, int lastApplied) {
  const int fullRequest = (request == CP_RM_FULL) ? 1 : 0;
  return cp_refresh_resolve_mode(request, /*clean=*/0, lastApplied, fullRequest);
}

// The row-9 completion condition, verbatim from GfxRenderer::displayBuffer():
// it runs only for a FAST drive, when the EDRV1 arm is on and the FLP1 one-shot
// was not armed for this paint.
bool completionRuns(int applied, bool row9Arm, bool suppressed) {
  return applied == CP_RM_FAST && row9Arm && !suppressed;
}

// One-shot promotion model: a FLAG consumed by the next paint. Used to state
// "exactly once per exit" over repeated close() calls.
struct OneShot {
  bool armed = false;
  int drives = 0;
  void arm() { armed = true; }
  int paint() {
    if (!armed) return 0;
    armed = false;
    ++drives;
    return 1;
  }
};

}  // namespace

// ---- adjust --------------------------------------------------------------

TEST(Flp1, AdjustWithTheArmAsksForFastAndSuppressesTheCompletion) {
  const Plan p = flpanel::plan(Event::Adjust, true);
  EXPECT_TRUE(p.promoteFast) << "an adjust must arm the panel's own FAST mode";
  EXPECT_TRUE(p.suppressCompletion) << "an adjust must not append the row-9 completion";
  EXPECT_TRUE(p.requestPaint);
  EXPECT_FALSE(p.halfClean) << "an adjust must not take a clean waveform";
  EXPECT_FALSE(p.cleanOnExit);
}

TEST(Flp1, AdjustResolvesToFastAndDrivesOnce) {
  const Plan p = flpanel::plan(Event::Adjust, true);
  // render() asks for FAST; the panel's promotion says the same, so the
  // producer hands the policy CP_RM_FAST.
  const int requested = producer(CP_RM_FAST, p.promoteFast ? CP_RM_FAST : -1);
  EXPECT_EQ(requested, CP_RM_FAST);
  // The latch describes a previous panel paint (FAST), which is what an
  // adjustment sees: the panel's own paints write CP_LATCH_FAST.
  const int applied = policy(requested, CP_LATCH_FAST);
  EXPECT_EQ(applied, CP_RM_FAST)
      << "an adjustment must be one DU drive, not a GC16/HALF clean";
  // And exactly one drive: the row-9 completion is suppressed for this paint.
  EXPECT_FALSE(completionRuns(applied, /*row9Arm=*/true, p.suppressCompletion))
      << "one adjustment must not be two whole-panel drives";
}

TEST(Flp1, AdjustOutranksAUserChosenFullMenuRefresh) {
  ASSERT_TRUE(flpanel::plan(Event::Adjust, true).promoteFast);
  // The menu/transition setting says FULL, so the framework's requestUpdate()
  // would have armed FULL; the panel arms FAST first, and apply() returns early
  // on hasPromotedRefresh(), so the caller's own FAST survives instead.
  const int requested = producer(CP_RM_FAST, CP_RM_FAST);
  EXPECT_EQ(requested, CP_RM_FAST);
  EXPECT_EQ(policy(requested, CP_LATCH_FAST), CP_RM_FAST);
}

TEST(Flp1, AdjustWithTheArmOffReproducesTheReportedBug) {
  const Plan p = flpanel::plan(Event::Adjust, false);
  EXPECT_FALSE(p.promoteFast);
  EXPECT_FALSE(p.suppressCompletion);
  EXPECT_TRUE(p.requestPaint);
  // Today: the paint is whatever the in-screen policy resolves. A ratio above
  // the AUTO threshold resolves to HALF -- a GC16 over the diff, i.e. the grey
  // flash the operator reported.
  EXPECT_EQ(producer(CP_RM_FAST, -1), CP_RM_FAST);
  EXPECT_EQ(policy(CP_RM_FAST, CP_LATCH_FAST), CP_RM_FAST);
  EXPECT_EQ(cp_refresh_resolve_request(CP_RM_FAST, /*auto_pending=*/1, CP_RM_HALF, 0, 0), CP_RM_HALF)
      << "AUTO over the threshold is the HALF the arm is there to remove";
  EXPECT_EQ(policy(CP_RM_HALF, CP_LATCH_FAST), CP_RM_HALF);
  // And even on the FAST path the completion ran, which is the second
  // whole-panel drive per adjustment (two drives -> the flash).
  EXPECT_TRUE(completionRuns(CP_RM_FAST, /*row9Arm=*/true, p.suppressCompletion));
}

// ---- exit ----------------------------------------------------------------

TEST(Flp1, ExitArmsExactlyOneFromWhiteFullAndNothingElse) {
  const Plan p = flpanel::plan(Event::Exit, true);
  EXPECT_TRUE(p.cleanOnExit);
  EXPECT_FALSE(p.requestPaint) << "the pop queues the repaint; the panel must not ask again";
  EXPECT_FALSE(p.promoteFast) << "the exit paint is the clean, not an adjustment";
  EXPECT_FALSE(p.halfClean);
  EXPECT_FALSE(p.suppressCompletion) << "a FULL never grows a completion drive anyway";

  // The promotion is FULL, and the pop's paint asks for whatever the screen
  // underneath wants: the producer hands over CP_RM_FULL regardless.
  for (int caller : {CP_RM_FAST, CP_RM_HALF, CP_RM_GL16, CP_RM_FULL}) {
    const int requested = producer(caller, CP_RM_FULL);
    EXPECT_EQ(requested, CP_RM_FULL);
    // ... and the policy keeps it: rule 1 keeps an explicit FULL (it IS the
    // from-white GC16), rule 2 makes a whole-frame request GC16-or-stronger.
    for (int latch : {CP_LATCH_FAST, CP_LATCH_FULL, CP_LATCH_CLEAN}) {
      EXPECT_EQ(policy(requested, latch), CP_RM_FULL)
          << "caller=" << caller << " latch=" << latch << ": the exit clean was demoted";
    }
  }
}

TEST(Flp1, ExitDrivesOnceHoweverOftenCloseIsCalled) {
  const Plan p = flpanel::plan(Event::Exit, true);
  ASSERT_TRUE(p.cleanOnExit);
  // Two close() calls in one loop pass (a swipe and a release), then one paint:
  // a promotion is a flag, not a counter, so the glass gets ONE FULL.
  OneShot promo;
  promo.arm();  // close() #1: cancel, then promoteNextRefresh(FULL)
  promo.arm();  // close() #2: same single flag, re-set
  EXPECT_EQ(promo.paint(), 1);
  EXPECT_EQ(promo.drives, 1);
  EXPECT_FALSE(promo.armed) << "the promotion must be spent by the paint";
  EXPECT_EQ(flpanel::fromWhiteDrivesPerExit(true), 1);
}

TEST(Flp1, ExitWithTheArmOffKeepsTodaysBareClose) {
  const Plan p = flpanel::plan(Event::Exit, false);
  EXPECT_FALSE(p.cleanOnExit) << "arm off must not arm a clean";
  EXPECT_EQ(flpanel::fromWhiteDrivesPerExit(false), 0);
}

// ---- unchanged paths -----------------------------------------------------

TEST(Flp1, EnterIsNotTouchedByTheArm) {
  const Plan on = flpanel::plan(Event::Enter, true);
  const Plan off = flpanel::plan(Event::Enter, false);
  EXPECT_TRUE(on.requestPaint);
  EXPECT_FALSE(on.promoteFast);
  EXPECT_FALSE(on.suppressCompletion);
  EXPECT_FALSE(on.cleanOnExit);
  EXPECT_EQ(memcmp(&on, &off, sizeof(Plan)), 0) << "the arm must not change the opening paint";
}

TEST(Flp1, NightTileIsIdenticalUnderBothArms) {
  const Plan on = flpanel::plan(Event::NightTile, true);
  const Plan off = flpanel::plan(Event::NightTile, false);
  EXPECT_TRUE(on.halfClean) << "the inversion tile keeps its HALF clean";
  EXPECT_TRUE(on.requestPaint);
  EXPECT_EQ(memcmp(&on, &off, sizeof(Plan)), 0) << "the arm must not change the night tile";
  // And the drive that produces is the unchanged one: request HALF, latch FAST.
  EXPECT_EQ(policy(producer(CP_RM_HALF, -1), CP_LATCH_FAST), CP_RM_HALF);
  EXPECT_FALSE(completionRuns(CP_RM_HALF, true, on.suppressCompletion));
}

TEST(Flp1, DragReleaseDropsTheHalfCleanOnlyWhenArmed) {
  const Plan on = flpanel::plan(Event::DragRelease, true);
  EXPECT_FALSE(on.halfClean) << "arm on: the exit clean replaces the lift's HALF";
  EXPECT_FALSE(on.requestPaint) << "arm on: nothing is left to draw on a lift";
  const Plan off = flpanel::plan(Event::DragRelease, false);
  EXPECT_TRUE(off.halfClean) << "arm off: today's HALF clean on the lift";
  EXPECT_TRUE(off.requestPaint);
}

// ---- FLP2: the panel's adjust paint must leave on the binary DU lane --------
//
// The FLP1 table fixed the DECISION the panel makes. These cases cover the two
// layers BELOW it that could still turn one adjustment into a whole-panel drive:
// the frame's content (the vendor shim's grey lane, metalio_vendor_panel.cpp:644)
// and the inherited/ambient mode (rule 3 after a reader FULL, and the shim's
// 24-partial clean cadence, :719-726).

TEST(Flp2, AdjustOverAGreyFrameIsForcedBinary) {
  const Plan p = flpanel::plan(Event::Adjust, true);
  ASSERT_TRUE(p.binaryFast) << "an adjust must decline the grey lane for that paint";
  // The panel's frame is the screen underneath plus a 1-bit card. Over an open
  // EPUB the page is still there, so frameHasGrey can be true whether or not the
  // reader draws anti-aliased text (that is why turning AA off changed nothing).
  const bool frameHasGrey = true;
  EXPECT_FALSE(flpanel::shimTakesGreyWipe(/*fastRequest=*/true, frameHasGrey, p.binaryFast))
      << "a forced-binary adjust paint must never select my_waveform_wipe_gl(3)";
  // Same for the shim's from-white escalations: the mode is pinned to FAST,
  // so gc16Class (HALF/FULL) cannot be true and forceFromWhite cannot fire.
  EXPECT_EQ(flpanel::forcedDriveMode(CP_RM_FAST, p.binaryFast), CP_RM_FAST);
}

TEST(Flp2, AdjustOverAGreyFrameUnderTheFlp1ArmAloneWouldStillWipe) {
  // Control: without the FLP2 term, the identical FLP1 decision takes the wipe.
  // This is the "AA off changed nothing" evidence, encoded.
  const bool frameHasGrey = true;
  EXPECT_TRUE(flpanel::shimTakesGreyWipe(/*fastRequest=*/true, frameHasGrey, /*binaryForced=*/false));
}

TEST(Flp2, AdjustAfterAReaderFromWhiteFullIsNotEscalatedToHalf) {
  // The inherited-mode path the operator described: the reader underneath did a
  // from-white page clean, so the latch is FULL and cp_refresh_resolve_mode()
  // rule 3 demotes the panel's FAST request to HALF -- a GC16-class drive that
  // the shim escalates again when the frame has >= 5 levels.
  const Plan p = flpanel::plan(Event::Adjust, true);
  const int requestedFast = producer(CP_RM_FAST, CP_RM_FAST);
  const int resolvedByPolicy = policy(requestedFast, CP_LATCH_FULL);
  EXPECT_EQ(resolvedByPolicy, CP_RM_HALF) << "rule 3 is what inherits the reader's FULL";
  EXPECT_EQ(flpanel::forcedDriveMode(resolvedByPolicy, p.binaryFast), CP_RM_FAST)
      << "the FLP2 pin is the last word on the mode";
  // And with the arm off there is no pin: today's HALF survives untouched.
  EXPECT_EQ(flpanel::forcedDriveMode(resolvedByPolicy, flpanel::plan(Event::Adjust, false).binaryFast),
            CP_RM_HALF);
}

TEST(Flp2, AdjustIsNotSpentOnTheShimsAmbientCleanCadence) {
  // The shim's counter is ambient state accumulated by the reader's earlier
  // partial paints, so the 24-partial clean can land on a panel adjustment.
  const Plan p = flpanel::plan(Event::Adjust, true);
  EXPECT_FALSE(flpanel::shimTakesCadenceClean(/*fastRequest=*/true, p.binaryFast, /*cadenceDue=*/true));
  // Arm off: unchanged -- a due cadence still cleans (that is the reader's fix).
  EXPECT_TRUE(flpanel::shimTakesCadenceClean(/*fastRequest=*/true, /*binaryForced=*/false,
                                             /*cadenceDue=*/true));
}

TEST(Flp2, AdjustWithTheArmOffIsBitIdenticalToFlp1) {
  const Plan off = flpanel::plan(Event::Adjust, false);
  EXPECT_FALSE(off.binaryFast) << "arm off must not force anything";
  EXPECT_FALSE(off.promoteFast);
  EXPECT_FALSE(off.suppressCompletion);
  EXPECT_EQ(flpanel::forcedDriveMode(CP_RM_HALF, off.binaryFast), CP_RM_HALF);
  EXPECT_TRUE(flpanel::shimTakesGreyWipe(true, true, off.binaryFast)) << "today's wipe survives arm-off";
  // And a plain FAST stays FAST, exactly as r101.20 decided it.
  EXPECT_EQ(policy(producer(CP_RM_FAST, -1), CP_LATCH_FAST), CP_RM_FAST);
}

TEST(Flp2, ExitEnterNightTileAndTheLiftCarryNoBinaryTerm) {
  // Everything that is not an adjustment must stay bit-identical in decision
  // terms: the exit clean is still the single from-white FULL, the night tile is
  // still HALF, entering still asks for a plain repaint, the lift still drives
  // nothing under the arm.
  const Plan exitOn = flpanel::plan(Event::Exit, true);
  EXPECT_FALSE(exitOn.binaryFast);
  EXPECT_TRUE(exitOn.cleanOnExit);
  EXPECT_EQ(flpanel::forcedDriveMode(policy(producer(CP_RM_FAST, CP_RM_FULL), CP_LATCH_FAST), exitOn.binaryFast),
            CP_RM_FULL)
      << "the exit clean must not be pinnable to DU";

  EXPECT_FALSE(flpanel::plan(Event::NightTile, true).binaryFast);
  EXPECT_TRUE(flpanel::plan(Event::NightTile, true).halfClean);

  const Plan enter = flpanel::plan(Event::Enter, true);
  EXPECT_FALSE(enter.binaryFast);
  EXPECT_FALSE(enter.promoteFast || enter.suppressCompletion || enter.halfClean || enter.cleanOnExit);

  EXPECT_FALSE(flpanel::plan(Event::DragRelease, true).binaryFast);
}

// The table is a pure function of (Event, arm) and reads nothing else, and no
// event may carry another event's term -- that is how "nothing else changes"
// (enter, the night tile) is enforced rather than asserted in a comment.
TEST(Flp1, PlanIsPureAndTheEventsDoNotBleed) {
  for (bool arm : {false, true}) {
    for (auto e : {Event::Adjust, Event::DragRelease, Event::Enter, Event::Exit, Event::NightTile}) {
      const Plan a = flpanel::plan(e, arm);
      const Plan b = flpanel::plan(e, arm);
      EXPECT_EQ(memcmp(&a, &b, sizeof(Plan)), 0) << "the table is not deterministic for event "
                                                   << static_cast<int>(e);
      if (e == Event::Adjust) {
        EXPECT_FALSE(a.cleanOnExit);
        EXPECT_FALSE(a.halfClean) << "an adjustment must never take a clean waveform";
      }
      if (e == Event::Exit) {
        EXPECT_FALSE(a.promoteFast) << "an exit is a clean, never an adjustment";
        EXPECT_FALSE(a.suppressCompletion);
        EXPECT_FALSE(a.halfClean);
      }
      if (e == Event::Enter) {
        EXPECT_FALSE(a.cleanOnExit || a.promoteFast || a.suppressCompletion || a.halfClean || a.binaryFast)
            << "entering the panel must stay exactly as it was";
      }
    }
  }
}
