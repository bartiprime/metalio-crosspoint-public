// r163 F5: the hold-repeat cadence. Pure timing logic, no reader or SDK, so the
// press-anchoring rules are pinned here rather than only on glass.
#include <gtest/gtest.h>

#include "util/PageRepeat.h"

namespace {

TEST(PageRepeat, NothingBeforeTheDwell) {
  PageRepeat repeat;
  repeat.arm(1, 1000);
  EXPECT_FALSE(repeat.due(1399));
  EXPECT_TRUE(repeat.due(400 + 1000));  // pressAt + 400
}

TEST(PageRepeat, CadenceIsAnchoredToThePressNotToTheLastTick) {
  PageRepeat repeat;
  repeat.arm(1, 1000);
  ASSERT_TRUE(repeat.due(1400));
  ASSERT_TRUE(repeat.due(1500));
  // A pass 50 ms late must not shift the following tick: it is still due at 1700,
  // not at 1750. Next due after the 1600 tick is 1700.
  EXPECT_FALSE(repeat.due(1599));
  ASSERT_TRUE(repeat.due(1650));
  EXPECT_FALSE(repeat.due(1699));
  EXPECT_TRUE(repeat.due(1700));
}

TEST(PageRepeat, OneTickPerCall) {
  PageRepeat repeat;
  repeat.arm(1, 1000);
  EXPECT_TRUE(repeat.due(1400));
  EXPECT_FALSE(repeat.due(1400));
}

TEST(PageRepeat, StalledPassDropsTheBacklogInsteadOfBursting) {
  PageRepeat repeat;
  repeat.arm(1, 1000);
  ASSERT_TRUE(repeat.due(1400));  // next 1500
  // 2500 is 1000 ms past the owed tick: the missed pages are abandoned and the
  // cadence resumes one interval from now.
  EXPECT_TRUE(repeat.due(2500));
  EXPECT_FALSE(repeat.due(2599));
  EXPECT_TRUE(repeat.due(2600));
}

TEST(PageRepeat, StopLatchesForTheRestOfThePress) {
  PageRepeat repeat;
  repeat.arm(1, 1000);
  repeat.stop();
  EXPECT_TRUE(repeat.stopped());
  EXPECT_FALSE(repeat.due(5000));
  repeat.reset();  // release edge
  EXPECT_FALSE(repeat.armed());
  EXPECT_FALSE(repeat.due(5000));
}

TEST(PageRepeat, ArmCarriesTheDirectionAndZeroIsARelease) {
  PageRepeat repeat;
  repeat.arm(-1, 5000);
  EXPECT_TRUE(repeat.armed());
  EXPECT_EQ(repeat.direction(), -1);
  EXPECT_TRUE(repeat.due(5400));
  repeat.arm(0, 6000);
  EXPECT_FALSE(repeat.armed());
  EXPECT_FALSE(repeat.due(9000));
}

}  // namespace
