#include <gtest/gtest.h>

#include "ScreenGeometry.h"

// /api/status reports the panel size so the web UI's EPUB image optimiser stops falling back to
// the X4 profile (480x800) on a panel it does not recognise. The HAL gives NATIVE geometry
// (landscape, as the controller sees it); the optimiser sizes against a PORTRAIT page. These are
// the numbers this device actually has:
//   metalio_driver/display/displays.c  ED047TC2_1216 = {1216, 684}
//   metalio_driver/config.h            logical portrait 684x1216
// The point of the test is that the pair is derived, not written down twice: the same function has
// to give the same portrait size whichever way the panel is described.

TEST(ScreenGeometry, MetalioNativeLandscapeBecomesPortrait) {
  const cp_web::ScreenSize s = cp_web::screenPortrait(1216, 684);
  EXPECT_EQ(s.width, 684);   // short edge
  EXPECT_EQ(s.height, 1216); // long edge
}

TEST(ScreenGeometry, AlreadyPortraitIsUnchanged) {
  const cp_web::ScreenSize s = cp_web::screenPortrait(684, 1216);
  EXPECT_EQ(s.width, 684);
  EXPECT_EQ(s.height, 1216);
}

TEST(ScreenGeometry, SquareIsStable) {
  const cp_web::ScreenSize s = cp_web::screenPortrait(600, 600);
  EXPECT_EQ(s.width, 600);
  EXPECT_EQ(s.height, 600);
}

// A HAL that cannot answer must not make the endpoint report a plausible-looking 0xN or a
// nonsense portrait pair; both fields go to 0 and the browser keeps its old fallback.
TEST(ScreenGeometry, UnreportedGeometryIsZero) {
  EXPECT_EQ(cp_web::screenPortrait(0, 0).width, 0);
  EXPECT_EQ(cp_web::screenPortrait(0, 0).height, 0);
  EXPECT_EQ(cp_web::screenPortrait(-1, 684).width, 0);
  EXPECT_EQ(cp_web::screenPortrait(-1, 684).height, 0);
}

// The old X4 profile, for contrast: the point of the change is that a 1216x684 panel must NOT
// resolve to these.
TEST(ScreenGeometry, DoesNotResolveToTheX4Fallback) {
  const cp_web::ScreenSize s = cp_web::screenPortrait(1216, 684);
  EXPECT_NE(s.width, 480);
  EXPECT_NE(s.height, 800);
}
