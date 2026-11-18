#pragma once

// CAD2: THE ORDER THE CADENCE PICKERS SHOW, SEPARATE FROM WHAT A SAVED SLOT MEANS.
//
// REFRESH_FREQUENCY is persisted BY INDEX (0..6 are the bytes r101..r165 wrote), so
// renumbering it to put Never last would reinterpret every device that already saved
// a cadence: a reader who chose Never would come back on 60 pages and one who chose
// 60 would come back on Never. A settings migration is not an option either -- the
// backup restore path (lib/Backup/BackupProfile.cpp) drops any key it does not
// declare, so a "migrated already" marker cannot survive a restore.
//
// So the enum stays exactly as it is and the ORDER becomes a display fact held here:
// the pickers list position 0..6 and translate once, in both directions.
//
//   position : 0    1    2    3    4    5     6
//   stored   : 1p   5p   10p  15p  30p  60p   Never
//
// One table, read by BOTH surfaces -- Settings > Reader (via SettingsList.h's
// buildCadenceSetting) and the in-book More menu (EpubReaderMenuActivity) -- because
// two pickers that each own a copy of the order is how they drift apart.
//
// Pure: the StrId enum and plain integers, no settings class, no I18N lookup. That is
// what lets test/cadence_order/ drive the mapping on the host. CadencePicker.h binds
// this table to the real enum and static_asserts the two cannot disagree.

#include <I18nKeys.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace cadenceorder {

// How many slots REFRESH_FREQUENCY persists. Repeated here so this header needs no
// settings class; CadencePicker.h static_asserts it against REFRESH_FREQUENCY_COUNT
// and static_asserts every entry of kDisplayOrder against its enum member.
inline constexpr int kCount = 7;

// The stored slot shown at each list position, top to bottom.
inline constexpr uint8_t kDisplayOrder[kCount] = {0, 1, 2, 3, 4, 6, 5};

// One label per STORED SLOT, indexed by the slot itself (the enum's own order), so
// this table means the same thing the pickers' label array meant before CAD2:
// labels[slot]. Index 5 is Never, index 6 is 60 pages.
inline constexpr StrId kSlotLabels[kCount] = {StrId::STR_PAGES_1,  StrId::STR_PAGES_5,  StrId::STR_PAGES_10,
                                              StrId::STR_PAGES_15, StrId::STR_PAGES_30, StrId::STR_NEVER,
                                              StrId::STR_PAGES_60};

// The pickers' label list, DERIVED from the two tables above rather than written out
// again: a position's label is by construction the label of the slot it selects, so
// "lists 60 pages but stores Never" is not expressible.
constexpr std::array<StrId, kCount> buildLabels() {
  std::array<StrId, kCount> out{};
  for (int i = 0; i < kCount; i++) out[i] = kSlotLabels[kDisplayOrder[i]];
  return out;
}
inline constexpr std::array<StrId, kCount> kLabels = buildLabels();

// Where a stored slot sits in the pickers, or -1 when the byte is not a slot at all
// (a hand-edited or corrupt settings file; the load path clamps, so -1 means corrupt).
constexpr int displayIndexFor(const uint8_t stored) {
  for (int i = 0; i < kCount; i++) {
    if (kDisplayOrder[i] == stored) return i;
  }
  return -1;
}

// The stored slot at a picker position. False -- and `out` untouched -- when the
// position is not in the list, so a stray index writes nothing.
constexpr bool storedForDisplay(const int display, uint8_t& out) {
  if (display < 0 || display >= kCount) return false;
  out = kDisplayOrder[display];
  return true;
}

}  // namespace cadenceorder
