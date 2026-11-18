#include <gtest/gtest.h>

#include <cstring>

#include "ReaderLineSpacingMath.h"

// r163 B1: the persisted step ladder became a percent of the font size, in 1%
// steps. These cases pin the two things a migration can get wrong:
//   1. the percent a stored ladder slot maps to,
//   2. that the percent produces the SAME layout multiplier the ladder did.

namespace {

// The multipliers the seven-step ladder shipped with (ReaderLineSpacing.h before
// r163), spelled as the literals the code used.
constexpr float kOldLadderMultiplier[] = {1.0f, 1.1f, 1.2f, 1.3f, 1.45f, 1.6f, 1.75f};

}  // namespace

TEST(LineSpacingMath, LadderSlotsMapToThePercentTheyRenderedAt) {
  ASSERT_EQ(lineSpacing::kLadderCount, 7u);
  EXPECT_EQ(lineSpacing::percentFromLadderSlot(0), 100);  // LINE_SPACING_100
  EXPECT_EQ(lineSpacing::percentFromLadderSlot(1), 110);
  EXPECT_EQ(lineSpacing::percentFromLadderSlot(2), 120);  // the shipped default
  EXPECT_EQ(lineSpacing::percentFromLadderSlot(3), 130);
  EXPECT_EQ(lineSpacing::percentFromLadderSlot(4), 145);
  EXPECT_EQ(lineSpacing::percentFromLadderSlot(5), 160);
  EXPECT_EQ(lineSpacing::percentFromLadderSlot(6), 175);
}

TEST(LineSpacingMath, OutOfRangeSlotsFallBackToTheDefault) {
  EXPECT_EQ(lineSpacing::percentFromLadderSlot(-1), lineSpacing::kDefaultPercent);
  EXPECT_EQ(lineSpacing::percentFromLadderSlot(7), lineSpacing::kDefaultPercent);
  EXPECT_EQ(lineSpacing::percentFromLadderSlot(200), lineSpacing::kDefaultPercent);
}

TEST(LineSpacingMath, PreLadderSavesMapThroughTheSlotAddedLater) {
  // "lineSpacing" 0..3 = Tight/Normal/Wide/Extra Wide. The ladder's 100% slot
  // was added afterwards, so these are ladder slots 1..4.
  EXPECT_EQ(lineSpacing::percentFromLegacySlot(0), 110);
  EXPECT_EQ(lineSpacing::percentFromLegacySlot(1), 120);
  EXPECT_EQ(lineSpacing::percentFromLegacySlot(2), 130);
  EXPECT_EQ(lineSpacing::percentFromLegacySlot(3), 145);
  EXPECT_EQ(lineSpacing::percentFromLegacySlot(4), lineSpacing::kDefaultPercent);
  EXPECT_EQ(lineSpacing::percentFromLegacySlot(-1), lineSpacing::kDefaultPercent);
}

// The proof that an upgrade does not move a reader's lines: every ladder slot's
// percent multiplies the font size by exactly the float the ladder used.
TEST(LineSpacingMath, MigratedPercentIsBitIdenticalToTheOldLadderMultiplier) {
  for (int slot = 0; slot < static_cast<int>(lineSpacing::kLadderCount); ++slot) {
    const uint8_t percent = lineSpacing::percentFromLadderSlot(slot);
    const float now = lineSpacing::multiplier(percent);
    EXPECT_EQ(now, kOldLadderMultiplier[slot]) << "slot " << slot << " -> " << int(percent) << "%";
    // Not merely close: the same IEEE-754 value, so a section file written with
    // the ladder multiplier still matches lineCompression in the header.
    EXPECT_EQ(std::memcmp(&now, &kOldLadderMultiplier[slot], sizeof(float)), 0) << "slot " << slot;
  }
}

TEST(LineSpacingMath, ShippedDefaultIsUnchanged) {
  EXPECT_EQ(lineSpacing::kDefaultPercent, 120);
  EXPECT_EQ(lineSpacing::percentFromLadderSlot(2), lineSpacing::kDefaultPercent);
  EXPECT_EQ(lineSpacing::percentFromLegacySlot(1), lineSpacing::kDefaultPercent);
  EXPECT_EQ(lineSpacing::multiplier(lineSpacing::kDefaultPercent), 1.2f);
}

TEST(LineSpacingMath, EveryPercentIsAOnePercentStep) {
  // 101 selectable values, strictly increasing, each equal to its own ratio.
  ASSERT_EQ(lineSpacing::kMaxPercent - lineSpacing::kMinPercent, 100);
  for (int p = lineSpacing::kMinPercent; p <= lineSpacing::kMaxPercent; ++p) {
    const float m = lineSpacing::multiplier(static_cast<uint8_t>(p));
    EXPECT_FLOAT_EQ(m, static_cast<float>(p) / 100.0f);
    if (p > lineSpacing::kMinPercent) {
      EXPECT_GT(m, lineSpacing::multiplier(static_cast<uint8_t>(p - 1))) << "percent " << p;
    }
  }
}

TEST(LineSpacingMath, PercentOutsideTheRangeClamps) {
  EXPECT_EQ(lineSpacing::clampPercent(0), lineSpacing::kMinPercent);
  EXPECT_EQ(lineSpacing::clampPercent(99), lineSpacing::kMinPercent);
  EXPECT_EQ(lineSpacing::clampPercent(100), 100);
  EXPECT_EQ(lineSpacing::clampPercent(200), 200);
  EXPECT_EQ(lineSpacing::clampPercent(255), lineSpacing::kMaxPercent);
  EXPECT_EQ(lineSpacing::multiplier(0), lineSpacing::multiplier(lineSpacing::kMinPercent));
  EXPECT_EQ(lineSpacing::multiplier(255), lineSpacing::multiplier(lineSpacing::kMaxPercent));
}
