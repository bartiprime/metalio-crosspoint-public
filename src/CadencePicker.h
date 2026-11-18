#pragma once

// CAD2: the firmware-side binding of the display order to the real enum.
//
// CadenceOrder.h holds the table as plain integers so the host test can compile it
// alone. THIS header is where the table meets CrossPointSettings, and it is the one
// place the mirror is checked: every display position, the slot count, the label for
// each of the two slots CAD1/ANDRO-era code cares about, and the operator's own
// requirement that Never is the bottom rung.
//
// Both pickers include this (Settings > Reader through SettingsList.h's
// buildCadenceSetting, the in-book More menu directly), so neither can compile against
// a table that disagrees with the enum.

#include "CadenceOrder.h"
#include "CrossPointSettings.h"

#include <cstdint>

namespace cadenceorder {

// ONE LABEL PER DISPLAY POSITION, AND ONE DISPLAY POSITION PER STORED SLOT.
// This is the CAD1 assert (labels == REFRESH_FREQUENCY_COUNT) re-expressed for a list
// whose order is no longer the enum's order: it now holds over the DISPLAY table.
static_assert(kCount == CrossPointSettings::REFRESH_FREQUENCY_COUNT,
              "the display-order table must cover every REFRESH_FREQUENCY slot");

// The mirror, slot by slot. These are the only numbers in CadenceOrder.h that could
// silently stop describing the enum, so each one is pinned to its enum member: change
// a stored value in CrossPointSettings.h and this fails to compile instead of
// re-pointing a saved device.
static_assert(kDisplayOrder[0] == CrossPointSettings::REFRESH_1, "position 0 is 1 page");
static_assert(kDisplayOrder[1] == CrossPointSettings::REFRESH_5, "position 1 is 5 pages");
static_assert(kDisplayOrder[2] == CrossPointSettings::REFRESH_10, "position 2 is 10 pages");
static_assert(kDisplayOrder[3] == CrossPointSettings::REFRESH_15, "position 3 is 15 pages");
static_assert(kDisplayOrder[4] == CrossPointSettings::REFRESH_30, "position 4 is 30 pages");
static_assert(kDisplayOrder[5] == CrossPointSettings::REFRESH_60, "position 5 is 60 pages");
// CAD2's whole point: Never is offered LAST, and its stored slot is untouched.
static_assert(kDisplayOrder[kCount - 1] == CrossPointSettings::REFRESH_NEVER, "Never is the bottom rung");

// ...and the labels, which are indexed by STORED SLOT, still name what those two
// slots mean. If a future rung renames Never or 60, this is what catches it.
static_assert(kSlotLabels[CrossPointSettings::REFRESH_NEVER] == StrId::STR_NEVER, "slot 5 is Never");
static_assert(kSlotLabels[CrossPointSettings::REFRESH_60] == StrId::STR_PAGES_60, "slot 6 is 60 pages");

// Every slot appears exactly once: a duplicate would make one position unreachable
// and another ambiguous.
constexpr bool isPermutation() {
  bool seen[kCount] = {};
  for (int i = 0; i < kCount; i++) {
    if (kDisplayOrder[i] >= kCount || seen[kDisplayOrder[i]]) return false;
    seen[kDisplayOrder[i]] = true;
  }
  return true;
}
static_assert(isPermutation(), "the display order must be a permutation of the stored slots");

// Which position a picker marks as current for a stored byte. A byte that is not a
// slot at all (only possible from a hand-edited file -- the load path clamps) falls
// back to the 15-page position, which is the field's own default, rather than
// pointing at a row that means nothing.
inline int positionOf(const uint8_t stored) {
  const int pos = displayIndexFor(stored);
  return pos >= 0 ? pos : displayIndexFor(CrossPointSettings::REFRESH_15);
}

}  // namespace cadenceorder
