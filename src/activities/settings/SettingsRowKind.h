#pragma once

// SET3 (r164): how one Settings row behaves when it is activated.
//
// This header exists for one reason: the DECISION used to be spread across two
// places that had to agree and could not be tested on the host --
// SettingsActivity::buildScreen() decided from the label table whether a row
// draws as a checkbox, and SettingsActivity::toggleSelectedSetting() decided
// again, from the option count, whether a tap cycles the value or opens a
// picker. "Renders as a checkbox but cycles like an enum" is exactly the kind of
// disagreement that ships. Both sites now build the same RowFacts and ask
// rowKind() once.
//
// Pure: no StrId, no I18N, no settings, no device headers. The caller resolves
// the StrId comparisons (the OFF/ON pair) and passes the answer in.

#include <cstddef>
#include <cstdint>

namespace settingsrow {

enum class Kind : uint8_t {
  // Nothing to do on activation (a row with no storage and no options).
  Inert,
  // Launches a sub-screen.
  Action,
  // A boolean: toggles in place and draws as a checkbox/switch.
  Checkbox,
  // A continuous value (brightness, spacing, margins): steps in place.
  Stepper,
  // A discrete choice whose row carries a description line: keeps the modal
  // option popup, which is the only surface on this screen that draws the
  // description (the settings list has no room for a second line).
  Popup,
  // A discrete choice: opens the option list (OptionSelectActivity).
  Selector,
};

struct RowFacts {
  bool isAction = false;
  bool isToggle = false;
  bool isEnum = false;
  bool isValue = false;
  // The row carries a one-line description that must stay on screen.
  bool hasDescription = false;
  // The enum's two labels are exactly OFF then ON, i.e. a boolean written as an
  // enum (the r-something rows that draw as a checkbox).
  bool isOffOnPair = false;
  // Number of options the row offers RIGHT NOW (dynamic rows build this at
  // runtime, so it is a fact about this visit, not about the type).
  std::size_t optionCount = 0;
};

inline Kind rowKind(const RowFacts& f) {
  if (f.isAction) return Kind::Action;
  if (f.isToggle) return Kind::Checkbox;
  if (f.isEnum) {
    // OFF/ON is a boolean however it is spelled; a binary toggle reads better
    // as a switch than as a list of two.
    if (f.isOffOnPair && f.optionCount == 2) return Kind::Checkbox;
    if (f.hasDescription) return Kind::Popup;
    // Two or more options is a choice the operator should SEE. Fewer is not a
    // choice at all (a font family with no SD fonts installed), so the row
    // stays inert rather than opening a list of one.
    if (f.optionCount >= 2) return Kind::Selector;
    return Kind::Stepper;
  }
  if (f.isValue) return Kind::Stepper;
  return Kind::Inert;
}

// Whether activating the row leaves this screen (so the footer's second button
// reads SELECT rather than TOGGLE). Kept beside rowKind() for the same reason:
// the hint must not disagree with what the tap does.
inline bool opensAnotherSurface(const Kind kind) {
  return kind == Kind::Action || kind == Kind::Popup || kind == Kind::Selector;
}

}  // namespace settingsrow
