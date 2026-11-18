// ---------------------------------------------------------------------------
// EOB1 host test (r111) — the end-of-book panel's size and its repaint cost.
//
// THE CLAIMS, in the operator's words:
//   * "too small": the panel must use the space it has, and every row must clear
//     the touch floor with room to spare                     -> layout(), both
//                                                              orientations.
//   * "one tap flashes the whole panel": a selection is ONE DU drive over the
//     two rows that changed, never a whole-panel clean       -> plan(Select)
//                                                              through the REAL
//                                                              RefreshPolicy.h.
//   * entering the panel is ONE from-white GC16 and nothing more -> plan(Enter).
//
// Everything below is the renderer's real decision chain, in order:
//   cp_refresh_resolve_request()   (producer: AUTO ratio, then armed promotion)
//   cp_refresh_resolve_mode()      (policy: clean precedence, whole-frame,
//                                   no-HALF-after-FULL)
//   the FLP2 pin + the row-9 completion condition (GfxRenderer.cpp)
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "activities/eob/EndOfBookPanel.h"
#include "display/RefreshPolicy.h"

namespace {

using eobpanel::Box;
using eobpanel::Event;
using eobpanel::Layout;
using eobpanel::Plan;
using eobpanel::Tokens;

// The Metalio E-Ink 4 Plus, read from the board profile and the theme, not
// remembered: 684x1216 UI portrait (1216x684 native scan), viewableInsets
// {2,2,2,2}, uiDensity 1.35 on top of BaseMetrics (verticalSpacing 10,
// listSidePadding 20), body font UI_12 whose advanceY is 51 px.
constexpr int kAdvanceY = 51;
constexpr int kTouchFloor = 65;  // max(44, lineHeight + 14) = uiThemeTokens()
constexpr int kVScaled = 14;     // lround(10 * 1.35)
constexpr int kSideScaled = 27;  // lround(20 * 1.35)

Tokens plusTokens(const int safeW, const int safeH, const int rows) {
  Tokens t;
  t.safe = Box{2, 2, safeW, safeH};
  t.bodyLine = kAdvanceY;
  t.smallLine = kAdvanceY;  // ubuntu_10_regular advanceY is also 51
  t.minTouchSize = kTouchFloor;
  t.verticalSpacing = kVScaled;
  t.sidePadding = kSideScaled;
  t.rowCount = rows;
  return t;
}

// The request the producer would hand the policy, for a paint whose call site
// asked for `caller` and whose one-shot promotion (if any) is `promoted` (-1 =
// none armed). AUTO is passed as NOT pending: an armed promotion outranks it by
// construction, and the point of the fix is that the panel no longer depends on
// the measured ratio.
int producer(int caller, int promoted) {
  return cp_refresh_resolve_request(caller, /*auto_pending=*/0, /*auto_mode=*/CP_RM_HALF,
                                    /*promoted_pending=*/(promoted >= 0 ? 1 : 0), promoted);
}

// The mode the policy drives, given what the latch says was last on the glass.
int policy(int request, int lastApplied, bool fullRequest) {
  const int clean = 0;  // the panel has been painted: never the boot/park case
  return cp_refresh_resolve_mode(request, clean, lastApplied, fullRequest ? 1 : 0);
}

// What a plan actually puts on the glass, in drives, end to end.
struct Outcome {
  int applied = 0;
  int drives = 0;
};

Outcome drive(const Plan& p, int lastApplied, int caller, bool row9Arm = true) {
  const int promoted = p.promoteFull ? CP_RM_FULL : (p.promoteFast ? CP_RM_FAST : -1);
  const int requested = producer(caller, promoted);
  const bool fullRequest = p.clearScreen || p.promoteFull || requested == CP_RM_FULL;
  const int resolved = policy(requested, lastApplied, fullRequest);
  Outcome o;
  o.applied = p.binaryFast ? eobpanel::forcedDriveMode(resolved, true) : resolved;
  o.drives = eobpanel::drivesPerAppearance(p, o.applied, row9Arm);
  return o;
}

// What the pre-fix call site did: a clear on every render, a bare
// displayBuffer() with only the transition setting's request, no pin, no
// suppression. Modelled here so the test can state what was replaced, and it is
// what the source check next door asserts is gone.
Plan legacyPlan(Event e) {
  Plan p;
  p.clearScreen = true;
  p.paintChrome = true;
  if (e == Event::Enter) return p;
  return p;
}

}  // namespace

// ---- layout: the panel must use the space it has --------------------------

TEST(Eob1, PlusPortraitRowsFillNearlyHalfTheBand) {
  const Layout l = eobpanel::layout(plusTokens(680, 1212, 4));
  EXPECT_EQ(l.rowHeight, 104) << "1.6 x the 65 px touch floor";
  EXPECT_EQ(l.rowGap, 8);
  ASSERT_GT(l.band.h, 0);
  const int occupied = 4 * l.rowHeight + 3 * l.rowGap;
  const int pct = occupied * 100 / l.band.h;
  EXPECT_GE(pct, 45) << "target from the plan: 45-55 % of the band";
  EXPECT_LE(pct, 55);
  // The pre-fix row was 67 px (lineHeight 51 + 2 x the SDK's 8 px touch
  // padding) on a 73 px pitch: 292 px of the same band, i.e. ~32 %.
  EXPECT_GT(l.rowHeight, 67 + 20) << "the row that read as 'too small' was 67 px";
}

TEST(Eob1, EveryRowClearsTheTouchFloorInBothOrientations) {
  const Layout portrait = eobpanel::layout(plusTokens(680, 1212, 4));
  const Layout landscape = eobpanel::layout(plusTokens(1212, 680, 4));
  for (const Layout* l : {&portrait, &landscape}) {
    for (int i = 0; i < 4; ++i) {
      EXPECT_GE(l->rows[i].h, 44) << "absolute hit-target floor";
      EXPECT_GE(l->rows[i].h, kTouchFloor) << "the SDK's touch floor on this panel";
      EXPECT_GT(l->rows[i].w, 0);
      // Inside the band, and above the hint strip.
      EXPECT_GT(l->rows[i].y, l->band.y - 1);
      EXPECT_LE(eobpanel::bottom(l->rows[i]), l->band.y + l->band.h);
      EXPECT_LE(eobpanel::bottom(l->rows[i]), eobpanel::bottom(l->safe));
      if (i > 0) {
        EXPECT_EQ(l->rows[i].y - eobpanel::bottom(l->rows[i - 1]), l->rowGap)
            << "one pitch, no overlap";
      }
    }
    // Chrome above the rows, hints below them: nothing collides.
    EXPECT_LE(eobpanel::bottom(l->header), l->subtitle.y);
    EXPECT_LE(eobpanel::bottom(l->subtitle), l->band.y);
    EXPECT_LE(eobpanel::bottom(l->rows[3]), l->hints.y);
  }
  // A landscape band is ~440 px tall; four rows only fit by shrinking, and the
  // clamp must still clear the touch floor.
  EXPECT_LT(landscape.rowHeight, portrait.rowHeight);
  EXPECT_GE(landscape.rowHeight, kTouchFloor);
  EXPECT_LE(eobpanel::bottom(landscape.rows[3]), eobpanel::bottom(landscape.safe));
  // The rows are centred in the band, not glued to its top: that is what stops
  // a 440 px list under a 903 px band reading as an empty page.
  const int slack = landscape.band.h - (4 * landscape.rowHeight + 3 * landscape.rowGap);
  EXPECT_LE(slack, landscape.rowGap + 1) << "the landscape band is nearly full";
  EXPECT_GT(portrait.rows[0].y - portrait.band.y, 100) << "portrait block is centred";
}

TEST(Eob1, RowHeightIsClampedNotUnbounded) {
  EXPECT_EQ(eobpanel::rowHeightFor(20), 96) << "floor keeps small themes roomy";
  EXPECT_EQ(eobpanel::rowHeightFor(65), 104);
  EXPECT_EQ(eobpanel::rowHeightFor(90), 120) << "ceiling keeps four rows in the band";
}

// ---- the repaint decision table -------------------------------------------

TEST(Eob1, EnterIsOneFromWhiteGc16AndNothingElse) {
  const Plan p = eobpanel::plan(Event::Enter);
  EXPECT_TRUE(p.clearScreen) << "the panel replaces the page: one clean canvas";
  EXPECT_TRUE(p.paintChrome);
  EXPECT_TRUE(p.promoteFull) << "one deliberate from-white GC16, not whatever the setting says";
  EXPECT_FALSE(p.paintRows);
  // Every ambient request must still end at FULL, once.
  for (int caller = 0; caller <= 3; ++caller) {
    const Outcome o = drive(p, CP_LATCH_FAST, caller);
    EXPECT_EQ(o.applied, CP_RM_FULL);
    EXPECT_EQ(o.drives, 1) << "entry costs one drive, caller mode " << caller;
  }
  // A reader that just left the latch at FULL cannot turn it into a diff drive.
  EXPECT_EQ(drive(p, CP_LATCH_FULL, CP_RM_FAST).applied, CP_RM_FULL);
}

TEST(Eob1, SelectIsOneDuDriveForEveryAmbientState) {
  const Plan p = eobpanel::plan(Event::Select, 1, 2);
  EXPECT_FALSE(p.clearScreen) << "the clear is what made every tick a whole-panel drive";
  EXPECT_FALSE(p.paintChrome) << "title/subtitle/hints are static chrome";
  EXPECT_TRUE(p.paintRows);
  EXPECT_EQ(p.rowFirst, 1);
  EXPECT_EQ(p.rowLast, 2);
  EXPECT_TRUE(p.promoteFast);
  EXPECT_TRUE(p.suppressCompletion);
  EXPECT_TRUE(p.binaryFast);

  const int callerModes[] = {CP_RM_FULL, CP_RM_HALF, CP_RM_FAST, CP_RM_GL16};
  const int latches[] = {CP_LATCH_FAST, CP_LATCH_FULL};
  for (const int caller : callerModes) {
    for (const int latch : latches) {
      const Outcome o = drive(p, latch, caller);
      EXPECT_EQ(o.applied, CP_RM_FAST)
          << "caller=" << caller << " latch=" << latch
          << ": the pin is applied AFTER the policy, so rule 3 cannot demote it";
      EXPECT_EQ(o.drives, 1) << "one DU, no row-9 completion (caller=" << caller
                             << " latch=" << latch << ")";
    }
  }
}

TEST(Eob1, WithoutThePinsASelectionIsTwoWholePanelDrives) {
  // The counterfactual: ask for FAST and nothing else. With the shipped EDRV1
  // arm the renderer appends the row-9 GL16 -- a SECOND whole-panel drive (the
  // double flash), not the ~310 ms one the operator should see.
  Plan bare = eobpanel::plan(Event::Select, 1, 2);
  bare.suppressCompletion = false;
  const Outcome o = drive(bare, CP_LATCH_FAST, CP_RM_FAST);
  EXPECT_EQ(o.drives, 2) << "FAST + row-9 completion = two drives (EDRV1)";
  // And with the reader's FULL latch, an unpinned FAST request becomes HALF
  // (rule 3): the whole-panel GC16-over-the-diff the operator saw.
  Plan unpinned = bare;
  unpinned.promoteFast = false;
  unpinned.binaryFast = false;
  EXPECT_EQ(drive(unpinned, CP_LATCH_FULL, CP_RM_FAST).applied, CP_RM_HALF);
}

TEST(Eob1, BackPaintsNothing) {
  const Plan p = eobpanel::plan(Event::Back);
  EXPECT_FALSE(p.clearScreen);
  EXPECT_FALSE(p.paintChrome);
  EXPECT_FALSE(p.paintRows);
  EXPECT_EQ(p.drives(), 0) << "the reader repaints the page it returns to";
}

TEST(Eob1, TheLegacyClearIsTheWholePanelEveryAppearance) {
  // The pre-fix path, for the record: clear on every event, so the diff the
  // policy sees is the whole panel whatever else changed. That is the cost the
  // table above removes; the source check next door pins the call site.
  const Plan legacy = legacyPlan(Event::Select);
  EXPECT_TRUE(legacy.clearScreen);
  // One cleared appearance is one whole-panel drive, and the ~310 ms DU the
  // fix targets is not among the outcomes: HALF/FULL are 570-810 ms classes.
  const Outcome o = drive(legacy, CP_LATCH_FAST, CP_RM_FAST);
  EXPECT_TRUE(o.applied != CP_RM_FAST || o.drives != 1)
      << "pre-fix: either a HALF whole-panel drive or two drives, never one DU";
}
