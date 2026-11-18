// CAD2: the cadence pickers' DISPLAY ORDER, on the host.
//
// The stored byte of REFRESH_FREQUENCY is an index into an append-only enum, so
// "put Never last" had to become a display fact rather than a renumbering. This test
// drives the one table that expresses it (src/CadenceOrder.h -- the same header both
// pickers compile against) and asserts the two properties the operator cares about:
//
//   * a saved slot still means itself (round trip, all seven values), and
//   * Never is listed LAST and is still stored as 5.
//
// It also pins the label of every position to the slot that position selects, so
// "lists 60 pages but stores Never" cannot be introduced without failing here.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <set>

#include "CadenceOrder.h"

namespace {

int roundTrips = 0;

const char* labelName(const StrId id) {
  switch (id) {
    case StrId::STR_PAGES_1:
      return "1 page";
    case StrId::STR_PAGES_5:
      return "5 pages";
    case StrId::STR_PAGES_10:
      return "10 pages";
    case StrId::STR_PAGES_15:
      return "15 pages";
    case StrId::STR_PAGES_30:
      return "30 pages";
    case StrId::STR_PAGES_60:
      return "60 pages";
    case StrId::STR_NEVER:
      return "Never";
    default:
      return "?";
  }
}

}  // namespace

// 1. Every stored value -> exactly one display position -> the same stored value.
TEST(CadenceOrder, EveryStoredValueRoundTrips) {
  std::set<int> positions;
  std::cout << "slot | position | label\n";
  for (uint8_t stored = 0; stored < cadenceorder::kCount; stored++) {
    const int pos = cadenceorder::displayIndexFor(stored);
    ASSERT_GE(pos, 0) << "slot " << int(stored) << " has no display position";
    ASSERT_LT(pos, cadenceorder::kCount);
    // A slot must own exactly one row, or the picker marks two / the row is dead.
    ASSERT_TRUE(positions.insert(pos).second) << "two slots share position " << pos;

    uint8_t back = 0xFF;
    ASSERT_TRUE(cadenceorder::storedForDisplay(pos, back));
    ASSERT_EQ(back, stored) << "position " << pos << " does not store the slot it shows";
    roundTrips++;

    std::cout << int(stored) << "    | " << pos << "        | " << labelName(cadenceorder::kLabels[pos]) << "\n";
  }
  // Seven slots, seven positions, seven round trips -- not "at least one".
  EXPECT_EQ(roundTrips, cadenceorder::kCount);
  EXPECT_EQ(int(positions.size()), cadenceorder::kCount);
  std::cout << "round trips: " << roundTrips << "/" << cadenceorder::kCount << "\n";
}

// 2. The order itself: the ladder the operator asked for, Never at the bottom.
TEST(CadenceOrder, LadderIsOneToSixtyThenNever) {
  const std::array<uint8_t, 7> expected = {0, 1, 2, 3, 4, 6, 5};  // 1,5,10,15,30,60,Never
  ASSERT_EQ(cadenceorder::kCount, int(expected.size()));
  for (int i = 0; i < cadenceorder::kCount; i++) {
    EXPECT_EQ(cadenceorder::kDisplayOrder[i], expected[i]) << "display position " << i;
  }
  EXPECT_EQ(cadenceorder::kLabels[0], StrId::STR_PAGES_1);
  EXPECT_EQ(cadenceorder::kLabels[5], StrId::STR_PAGES_60);
  EXPECT_EQ(cadenceorder::kLabels[cadenceorder::kCount - 1], StrId::STR_NEVER);
}

// 3. A device that saved Never: its row reads Never, and it is the last row.
TEST(CadenceOrder, SavedNeverShowsNeverOnTheLastRow) {
  constexpr uint8_t kSavedNever = 5;  // REFRESH_NEVER, unchanged by CAD2
  const int pos = cadenceorder::displayIndexFor(kSavedNever);
  EXPECT_EQ(pos, cadenceorder::kCount - 1);
  EXPECT_EQ(cadenceorder::kLabels[pos], StrId::STR_NEVER);
}

// 4. Picking the BOTTOM row stores Never (5) -- the requirement, end to end.
TEST(CadenceOrder, SelectingTheLastRowStoresNever) {
  uint8_t stored = 0xFF;
  ASSERT_TRUE(cadenceorder::storedForDisplay(cadenceorder::kCount - 1, stored));
  EXPECT_EQ(stored, 5);
}

// 5. Out of range writes nothing: the caller's byte is untouched and the call says no.
TEST(CadenceOrder, OutOfRangeSelectionWritesNothing) {
  const int badPositions[] = {-1, cadenceorder::kCount, cadenceorder::kCount + 37, 1000};
  int refusals = 0;
  for (const int pos : badPositions) {
    uint8_t untouched = 0xA5;
    EXPECT_FALSE(cadenceorder::storedForDisplay(pos, untouched)) << "position " << pos;
    EXPECT_EQ(untouched, 0xA5) << "position " << pos << " wrote the byte anyway";
    refusals++;
  }
  EXPECT_EQ(refusals, 4);
  // ...and a corrupt stored byte has no position at all (the load path clamps, so
  // this is only reachable from a hand-edited file).
  EXPECT_EQ(cadenceorder::displayIndexFor(7), -1);
  EXPECT_EQ(cadenceorder::displayIndexFor(255), -1);
}

// 6. Each position's label names the slot that position writes back.
TEST(CadenceOrder, LabelMatchesTheSlotItSelects) {
  int checked = 0;
  for (int pos = 0; pos < cadenceorder::kCount; pos++) {
    uint8_t stored = 0;
    ASSERT_TRUE(cadenceorder::storedForDisplay(pos, stored));
    EXPECT_EQ(cadenceorder::kLabels[pos], cadenceorder::kSlotLabels[stored])
        << "position " << pos << " shows a label for a slot it does not store";
    checked++;
  }
  EXPECT_EQ(checked, cadenceorder::kCount);
}

// 7. The slot table is still indexed by SLOT (the pre-CAD2 meaning of the array), so
//    the label for a saved value has not moved.
TEST(CadenceOrder, SlotLabelsAreStillIndexedBySlot) {
  EXPECT_EQ(cadenceorder::kSlotLabels[0], StrId::STR_PAGES_1);
  EXPECT_EQ(cadenceorder::kSlotLabels[1], StrId::STR_PAGES_5);
  EXPECT_EQ(cadenceorder::kSlotLabels[2], StrId::STR_PAGES_10);
  EXPECT_EQ(cadenceorder::kSlotLabels[3], StrId::STR_PAGES_15);
  EXPECT_EQ(cadenceorder::kSlotLabels[4], StrId::STR_PAGES_30);
  EXPECT_EQ(cadenceorder::kSlotLabels[5], StrId::STR_NEVER);
  EXPECT_EQ(cadenceorder::kSlotLabels[6], StrId::STR_PAGES_60);
}
