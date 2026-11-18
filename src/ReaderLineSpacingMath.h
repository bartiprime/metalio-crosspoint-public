#pragma once

#include <algorithm>
#include <cstdint>

// Line height is a percent of the font size, persisted in 1% steps. This header
// is the numeric contract only (no settings/UI dependencies), so the mapping and
// the multiplier are host-testable: test/reader_line_spacing.
namespace lineSpacing {

inline constexpr uint8_t kMinPercent = 100;
inline constexpr uint8_t kMaxPercent = 200;
inline constexpr uint8_t kDefaultPercent = 120;

// The seven-step ladder of earlier builds: the percent each stored slot
// rendered at. Slot order is the old enum (LINE_SPACING_100 .. LINE_SPACING_175).
inline constexpr uint8_t kLadderPercent[] = {100, 110, 120, 130, 145, 160, 175};
inline constexpr uint8_t kLadderCount = static_cast<uint8_t>(sizeof(kLadderPercent) / sizeof(kLadderPercent[0]));

// Pre-ladder saves ("lineSpacing" 0..3 = Tight/Normal/Wide/Extra Wide) rendered
// at ladder slots 1..4, because the ladder's slot 0 (100%) was added later.
inline constexpr uint8_t kLegacySlotCount = 4;
inline constexpr uint8_t kLegacySlotOffset = 1;

inline uint8_t clampPercent(const int percent) {
  return static_cast<uint8_t>(std::clamp(percent, static_cast<int>(kMinPercent), static_cast<int>(kMaxPercent)));
}

// Ladder slot (0..6) -> the percent it rendered at. Out of range: the default.
inline uint8_t percentFromLadderSlot(const int slot) {
  return slot >= 0 && slot < static_cast<int>(kLadderCount) ? kLadderPercent[slot] : kDefaultPercent;
}

// "lineSpacing" 0..3 -> the percent it rendered at. Out of range: the default.
inline uint8_t percentFromLegacySlot(const int slot) {
  if (slot < 0 || slot >= static_cast<int>(kLegacySlotCount)) return kDefaultPercent;
  return percentFromLadderSlot(slot + static_cast<int>(kLegacySlotOffset));
}

// The factor the layout multiplies the font size by.
inline float multiplier(const uint8_t percent) { return static_cast<float>(clampPercent(percent)) / 100.0f; }

}  // namespace lineSpacing
