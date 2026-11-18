#pragma once

// SET3 (r164): the font-family row's option mapping, extracted so it can be
// proven on the host.
//
// The row is DYNAMIC: its options are the built-in family (index 0) followed by
// whatever families the SD card registry reports at this moment. Its stored
// state is not one byte either: the built-in family lives in
// CrossPointSettings::fontFamily and an SD family lives in the name field
// CrossPointSettings::sdFontFamilyName. So "which option is selected" and "what
// does picking option N persist" are two different functions, and both used to
// live inside lambdas in SettingsList.h where no test could reach them.
//
// The open selector now asks the SAME two functions the row's value text asks,
// which is what makes "the list shows what the row has" and "picking N stores
// what cycling to N stored" the same statement.
//
// Pure: no settings, no I18N, no device headers. The caller supplies the SD
// family names in row order and the stored state it read.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fontfamilyrow {

// The two persisted fields, as a value. `sdName` empty means "the built-in
// family in `family`".
struct Stored {
  uint8_t family = 0;
  std::string sdName;
};

// Option index the stored state selects. Mirrors the row's old getter exactly,
// including both fall-backs: an SD name that is no longer installed falls back
// to the built-in family, and an out-of-range built-in value reads as 0.
inline uint8_t indexFor(const uint8_t builtinCount, const std::vector<std::string>& sdFamilies,
                        const char* storedSdName, const uint8_t storedFamily) {
  if (storedSdName != nullptr && storedSdName[0] != '\0') {
    for (std::size_t i = 0; i < sdFamilies.size(); i++) {
      if (sdFamilies[i] == storedSdName) return static_cast<uint8_t>(builtinCount + i);
    }
    // Installed previously, gone now: fall through to the built-in family.
  }
  return storedFamily < builtinCount ? storedFamily : 0;
}

// What picking `index` persists. `valid` is false for an index the row does not
// offer (nothing is written, exactly as the old setter's bounds check did).
struct Selection {
  bool valid = false;
  // The built-in branch writes the family byte AND clears the name field; the
  // SD branch writes only the name field, leaving the family byte alone.
  bool setsFamily = false;
  uint8_t family = 0;
  const char* sdName = nullptr;
};

inline Selection selectionForIndex(const uint8_t builtinCount, const std::vector<std::string>& sdFamilies,
                                   const uint8_t index) {
  Selection sel;
  if (index < builtinCount) {
    sel.valid = true;
    sel.setsFamily = true;
    sel.family = index;
    return sel;
  }
  const std::size_t sdIndex = static_cast<std::size_t>(index) - builtinCount;
  if (sdIndex >= sdFamilies.size()) return sel;  // out of range: leave the state alone
  sel.valid = true;
  sel.sdName = sdFamilies[sdIndex].c_str();
  return sel;
}

// The state after applying a selection. Never invents a value: an invalid
// selection returns the state unchanged, because the old setter wrote nothing.
inline Stored applied(const Selection& sel, const Stored& current) {
  if (!sel.valid) return current;
  Stored next = current;
  if (sel.setsFamily) {
    next.family = sel.family;
    next.sdName.clear();
  } else {
    next.sdName = sel.sdName != nullptr ? sel.sdName : "";
  }
  return next;
}

// The option labels the row offers RIGHT NOW: the caller's built-in label first,
// then every SD family in registry order. A registry-less row is just the
// built-in label (one option), which is why rowKind() leaves that row alone.
inline std::vector<std::string> labels(const std::string& builtinLabel, const std::vector<std::string>& sdFamilies) {
  std::vector<std::string> out;
  out.reserve(sdFamilies.size() + 1);
  out.push_back(builtinLabel);
  out.insert(out.end(), sdFamilies.begin(), sdFamilies.end());
  return out;
}

}  // namespace fontfamilyrow
