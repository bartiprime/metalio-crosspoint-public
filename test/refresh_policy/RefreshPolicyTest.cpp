#include <gtest/gtest.h>

#include "RefreshPolicy.h"

// The four normalisation rules of RE_RICKYOS_PIPELINE.md row 4/5/8, as a pure
// function. No renderer, no HAL, no device: this is the whole point of the
// extraction.
namespace {

int resolve(int request, bool clean, int lastApplied, bool fullRequest) {
  return cp_refresh_resolve_mode(request, clean ? 1 : 0, lastApplied, fullRequest ? 1 : 0);
}

}  // namespace

// Row 5: a panel known-clean (never painted since power-on/deinit/park) forces the
// GC16 band, whatever the ratio-derived or promoted request says.
TEST(RefreshPolicy, CleanForcesGc16) {
  EXPECT_EQ(resolve(CP_RM_FAST, true, CP_LATCH_UNKNOWN, false), CP_RM_HALF);
  EXPECT_EQ(resolve(CP_RM_FAST, true, CP_LATCH_FAST, false), CP_RM_HALF);
  EXPECT_EQ(resolve(CP_RM_GL16, true, CP_LATCH_CLEAN, false), CP_RM_HALF);
}

// Row 4 rule 2: a whole-frame replacement is GC16, never a fast-band drive.
TEST(RefreshPolicy, FullForcesGc16) {
  EXPECT_EQ(resolve(CP_RM_FAST, false, CP_LATCH_FAST, true), CP_RM_HALF);
  EXPECT_EQ(resolve(CP_RM_GL16, false, CP_LATCH_UNKNOWN, true), CP_RM_HALF);
}

// Row 8: one half-promotion immediately after a FULL, then the latch is re-written
// as fast so the promotion cannot repeat (see the RefreshPolicy.h note on why the
// collapsed encoding exists).
TEST(RefreshPolicy, FastAfterFullPromotesToGc16) {
  EXPECT_EQ(resolve(CP_RM_FAST, false, CP_LATCH_FULL, false), CP_RM_HALF);
  EXPECT_EQ(resolve(CP_RM_HALF, false, CP_LATCH_FULL, false), CP_RM_HALF);
  // ... and only once: the promoted paint is recorded as fast, not full.
  EXPECT_EQ(resolve(CP_RM_FAST, false, CP_LATCH_FAST, false), CP_RM_FAST);
}

TEST(RefreshPolicy, FastAfterFastStaysFast) {
  EXPECT_EQ(resolve(CP_RM_FAST, false, CP_LATCH_FAST, false), CP_RM_FAST);
  EXPECT_EQ(resolve(CP_RM_FAST, false, CP_LATCH_UNKNOWN, false), CP_RM_FAST);
}

TEST(RefreshPolicy, PromotionBeatsCallSiteButNotClean) {
  // The armed promotion wins over what the call site asked for...
  EXPECT_EQ(cp_refresh_resolve_request(CP_RM_FAST, 0, CP_RM_FAST, 1, CP_RM_HALF), CP_RM_HALF);
  // ...and the measured AUTO decision still wins over a stale promotion...
  EXPECT_EQ(cp_refresh_resolve_request(CP_RM_FULL, 1, CP_RM_FAST, 1, CP_RM_FULL), CP_RM_FAST);
  // ...but neither reaches a clean panel.
  EXPECT_EQ(resolve(cp_refresh_resolve_request(CP_RM_HALF, 0, CP_RM_HALF, 1, CP_RM_FAST), true,
                    CP_LATCH_CLEAN, false),
            CP_RM_HALF);
}

// Documented deviation, asserted so it cannot drift silently: clean never demotes an
// explicit from-white FULL, because FULL *is* the GC16-from-white drive.
TEST(RefreshPolicy, CleanKeepsExplicitFull) {
  EXPECT_EQ(resolve(CP_RM_FULL, true, CP_LATCH_CLEAN, true), CP_RM_FULL);
}

// CP_RICKY_TURN_MODE: a page turn asks for the GL16 family, and nothing else moves.
// The switch is compiled into this header, so this asserts whichever value the
// build has — the default (1) is the shipped one.
TEST(RefreshPolicy, PageTurnRequestsGl16OnlyUnderTheSwitch) {
#if CP_RICKY_TURN_MODE
  EXPECT_EQ(cp_refresh_page_turn_mode(CP_RM_FAST), CP_RM_GL16);
  // ... and the turn then survives the policy stage on a non-clean panel...
  EXPECT_EQ(resolve(cp_refresh_page_turn_mode(CP_RM_FAST), false, CP_LATCH_FAST, false), CP_RM_GL16);
  // ... while a list scroll keeps the request it made (DU).
  EXPECT_EQ(resolve(CP_RM_FAST, false, CP_LATCH_FAST, false), CP_RM_FAST);
#else
  EXPECT_EQ(cp_refresh_page_turn_mode(CP_RM_FAST), CP_RM_FAST);
#endif
}

// r80 equivalence for the producer stage: with nothing armed, the call site decides.
TEST(RefreshPolicy, ProducerIsIdentityWhenNothingArmed) {
  EXPECT_EQ(cp_refresh_resolve_request(CP_RM_HALF, 0, CP_RM_FAST, 0, CP_RM_FULL), CP_RM_HALF);
  EXPECT_EQ(resolve(CP_RM_HALF, false, CP_LATCH_FAST, false), CP_RM_HALF);
}