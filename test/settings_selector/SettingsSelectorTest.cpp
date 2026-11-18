// SET3 (r164): the settings rows with a discrete set of options open an option
// list instead of cycling the value in place.
//
// Three things have to be true and none of them needs a panel:
//   1. the DECISION (a row that is a choice opens a list; a boolean, a
//      continuous value and an action do not) is one table, shared by the
//      renderer and the activation path;
//   2. the DYNAMIC row (font family: built-in + whatever the SD card reports)
//      offers exactly the options it has, and option N stores what cycling to N
//      stored -- the round trip through the two persisted fields;
//   3. coming back from the list leaves the settings list where it was: the pop
//      path does not re-run onEnter(), and onEnter() is the only thing that
//      resets the nav, so the viewport top (the scroll position) survives.
//
// The third one is exercised against the SDK's REAL ListNav, not a model of it.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "SettingsRowKind.h"
#include "FontFamilyRow.h"
#include "components/lists/list.h"

namespace fui = freeink::ui;

// ---------------------------------------------------------------------------
// 1. The decision table.
// ---------------------------------------------------------------------------

namespace {

settingsrow::RowFacts enumFacts(const std::size_t options, const bool offOnPair = false,
                                const bool described = false) {
  settingsrow::RowFacts f;
  f.isEnum = true;
  f.optionCount = options;
  f.isOffOnPair = offOnPair;
  f.hasDescription = described;
  return f;
}

}  // namespace

TEST(SettingsRowKind, BooleanRowsStayToggles) {
  settingsrow::RowFacts toggle;
  toggle.isToggle = true;
  EXPECT_EQ(settingsrow::rowKind(toggle), settingsrow::Kind::Checkbox);

  // An OFF/ON enum pair is a boolean however it is spelled: a switch, not a
  // two-item list.
  EXPECT_EQ(settingsrow::rowKind(enumFacts(2, /*offOnPair=*/true)), settingsrow::Kind::Checkbox);
}

TEST(SettingsRowKind, TwoOptionChoiceIsAListNotACycle) {
  // The r164 rows that used to walk their value on every tap: FIT/CROP and
  // LIST/TOOLBAR. Two options that are not a boolean are a choice.
  EXPECT_EQ(settingsrow::rowKind(enumFacts(2)), settingsrow::Kind::Selector);
  EXPECT_EQ(settingsrow::rowKind(enumFacts(3)), settingsrow::Kind::Selector);
  EXPECT_EQ(settingsrow::rowKind(enumFacts(9)), settingsrow::Kind::Selector);
}

TEST(SettingsRowKind, ContinuousAndActionRowsAreUntouched) {
  settingsrow::RowFacts value;
  value.isValue = true;
  EXPECT_EQ(settingsrow::rowKind(value), settingsrow::Kind::Stepper);  // brightness, spacing, margins

  settingsrow::RowFacts action;
  action.isAction = true;
  EXPECT_EQ(settingsrow::rowKind(action), settingsrow::Kind::Action);

  settingsrow::RowFacts inert;
  EXPECT_EQ(settingsrow::rowKind(inert), settingsrow::Kind::Inert);
}

TEST(SettingsRowKind, DescribedChoiceKeepsItsDescription) {
  // POWER_OFF_SCREEN carries the "an image on a powered-off device reads as
  // still on" caveat; the modal popup is the only surface here that shows it.
  EXPECT_EQ(settingsrow::rowKind(enumFacts(5, false, /*described=*/true)), settingsrow::Kind::Popup);
  EXPECT_EQ(settingsrow::rowKind(enumFacts(2, false, /*described=*/true)), settingsrow::Kind::Popup);
}

TEST(SettingsRowKind, OneOptionIsNotAChoice) {
  // Font family with no SD fonts installed: a list of one is not a list.
  EXPECT_EQ(settingsrow::rowKind(enumFacts(0)), settingsrow::Kind::Stepper);
  EXPECT_EQ(settingsrow::rowKind(enumFacts(1)), settingsrow::Kind::Stepper);
}

TEST(SettingsRowKind, FooterHintAgreesWithTheTap) {
  EXPECT_TRUE(settingsrow::opensAnotherSurface(settingsrow::Kind::Selector));
  EXPECT_TRUE(settingsrow::opensAnotherSurface(settingsrow::Kind::Popup));
  EXPECT_TRUE(settingsrow::opensAnotherSurface(settingsrow::Kind::Action));
  EXPECT_FALSE(settingsrow::opensAnotherSurface(settingsrow::Kind::Checkbox));
  EXPECT_FALSE(settingsrow::opensAnotherSurface(settingsrow::Kind::Stepper));
  EXPECT_FALSE(settingsrow::opensAnotherSurface(settingsrow::Kind::Inert));
}

// ---------------------------------------------------------------------------
// 2. The dynamic font-family row.
// ---------------------------------------------------------------------------

namespace {

constexpr uint8_t kBuiltin = 1;  // CrossPointSettings::BUILTIN_FONT_COUNT
const std::vector<std::string> kSd = {"Literata", "Bookerly", "Bitter"};

}  // namespace

TEST(FontFamilyRow, OptionListIsBuiltinThenSdFamilies) {
  const auto labels = fontfamilyrow::labels("Noto Sans", kSd);
  ASSERT_EQ(labels.size(), kSd.size() + 1);
  EXPECT_EQ(labels[0], "Noto Sans");
  EXPECT_EQ(labels[1], "Literata");
  EXPECT_EQ(labels[2], "Bookerly");
  EXPECT_EQ(labels[3], "Bitter");

  // No SD fonts: the row has one option, which is why the row does not open a list.
  const auto alone = fontfamilyrow::labels("Noto Sans", {});
  ASSERT_EQ(alone.size(), 1u);
  EXPECT_EQ(alone[0], "Noto Sans");
}

TEST(FontFamilyRow, IndexStoresTheOptionItNames) {
  fontfamilyrow::Stored stored;  // built-in, no SD name
  for (uint8_t i = 0; i < kSd.size() + kBuiltin; i++) {
    const auto sel = fontfamilyrow::selectionForIndex(kBuiltin, kSd, i);
    ASSERT_TRUE(sel.valid) << "index " << static_cast<int>(i);
    stored = fontfamilyrow::applied(sel, stored);
    EXPECT_EQ(fontfamilyrow::indexFor(kBuiltin, kSd, stored.sdName.c_str(), stored.family), i)
        << "option " << static_cast<int>(i) << " does not round-trip";
  }
}

TEST(FontFamilyRow, RoundTripFromAnyStartingState) {
  // Start on an SD family, then pick every option: the walk must not depend on
  // where it began (the old setter only cleared the name on the built-in branch,
  // so a stale name could survive a built-in pick if the clear were lost).
  fontfamilyrow::Stored stored{0, "Bookerly"};
  ASSERT_EQ(fontfamilyrow::indexFor(kBuiltin, kSd, stored.sdName.c_str(), stored.family), 2);

  for (uint8_t i = 0; i < kSd.size() + kBuiltin; i++) {
    const auto sel = fontfamilyrow::selectionForIndex(kBuiltin, kSd, i);
    stored = fontfamilyrow::applied(sel, stored);
    EXPECT_EQ(fontfamilyrow::indexFor(kBuiltin, kSd, stored.sdName.c_str(), stored.family), i);
  }
}

TEST(FontFamilyRow, BuiltinPickClearsTheSdNameAndAnSdPickLeavesTheFamilyByte) {
  const fontfamilyrow::Stored onSd{0, "Literata"};
  const auto builtin = fontfamilyrow::applied(fontfamilyrow::selectionForIndex(kBuiltin, kSd, 0), onSd);
  EXPECT_EQ(builtin.family, 0);
  EXPECT_TRUE(builtin.sdName.empty());

  // The persisted family byte is NOT rewritten when an SD font is chosen: the
  // row's contract is "name wins, family is the fall-back", and that is what the
  // reader consults.
  const fontfamilyrow::Stored onBuiltin{0, ""};
  const auto sd = fontfamilyrow::applied(fontfamilyrow::selectionForIndex(kBuiltin, kSd, 3), onBuiltin);
  EXPECT_EQ(sd.family, 0);
  EXPECT_EQ(sd.sdName, "Bitter");
}

TEST(FontFamilyRow, OutOfRangeIndexWritesNothing) {
  const fontfamilyrow::Stored current{0, "Literata"};
  // One past the last option: the old setter's bounds check wrote nothing, and
  // a selector that somehow handed back such an index must do the same.
  const auto sel = fontfamilyrow::selectionForIndex(kBuiltin, kSd, static_cast<uint8_t>(kBuiltin + kSd.size()));
  EXPECT_FALSE(sel.valid);
  const auto after = fontfamilyrow::applied(sel, current);
  EXPECT_EQ(after.family, current.family);
  EXPECT_EQ(after.sdName, current.sdName);
}

TEST(FontFamilyRow, StaleSdNameFallsBackToTheBuiltinFamily) {
  // A font that was uninstalled: the name is still in the settings file. The row
  // must open on the built-in family, not on a phantom option.
  EXPECT_EQ(fontfamilyrow::indexFor(kBuiltin, kSd, "Removed", 0), 0);
  // No SD name at all: the family byte decides, and a corrupt one reads as 0.
  EXPECT_EQ(fontfamilyrow::indexFor(kBuiltin, kSd, "", 0), 0);
  EXPECT_EQ(fontfamilyrow::indexFor(kBuiltin, kSd, "", 7), 0);
  // An empty registry with a built-in family still resolves.
  EXPECT_EQ(fontfamilyrow::indexFor(kBuiltin, {}, "", 0), 0);
}

TEST(FontFamilyRow, OptionCountEqualsStorableStates) {
  // "The list shows exactly the options the row has": one option per distinct
  // persisted state, and every one of them round-trips.
  const auto labels = fontfamilyrow::labels("Noto Sans", kSd);
  std::vector<std::string> seen;
  for (uint8_t i = 0; i < labels.size(); i++) {
    const fontfamilyrow::Stored s{0, ""};
    const auto applied = fontfamilyrow::applied(fontfamilyrow::selectionForIndex(kBuiltin, kSd, i), s);
    const std::string key = std::to_string(applied.family) + "|" + applied.sdName;
    seen.push_back(key);
  }
  std::sort(seen.begin(), seen.end());
  EXPECT_EQ(std::unique(seen.begin(), seen.end()) - seen.begin(), static_cast<std::ptrdiff_t>(labels.size()));
}

// ---------------------------------------------------------------------------
// 3. Scroll position across the push/pop of the option list.
// ---------------------------------------------------------------------------
//
// The settings list is a UiListActivity. Its viewport top (the operator's scroll
// position) lives in nav.top and is only reset by UiListActivity::onEnter() ->
// activeNav().reset(). ActivityManager's restored-parent branch deliberately
// does not re-run onEnter() (see the wiring test), so the pop back from the
// option list repaints the SAME nav. These cases drive ListNav::syncToProps --
// the real build step -- across a simulated push/pop.

namespace {

constexpr int16_t kBodyTop = 120;
constexpr int16_t kBodyHeight = 360;
constexpr int16_t kRowHeight = 60;
constexpr int16_t kRowGap = 0;
constexpr int kRowCount = 24;  // a settings section taller than one screen

fui::Rect settingsBody() { return fui::Rect{0, kBodyTop, 480, kBodyHeight}; }

// One buildScreen(): the nav measures the band and writes selection+top into props.
void build(fui::ListNav& nav, fui::ListProps& props) {
  props.count = kRowCount;
  nav.syncToProps(settingsBody(), kRowHeight, kRowGap, kRowCount, props, 0);
}

}  // namespace

TEST(SettingsSelectorScroll, ReturnFromTheListKeepsTheScrollPosition) {
  fui::ListNav nav;
  fui::ListProps props;

  // Entry: onEnter() -> reset(), so the first build follows the selection.
  nav.reset();
  nav.selected = 0;
  build(nav, props);
  EXPECT_EQ(props.topIndex, 0);

  // The operator scrolls down without moving the selection (a swipe scrolls the
  // viewport only) and then taps a row: selection 4 is off the top of the window.
  nav.top = 7;
  nav.followOnBuild = false;
  build(nav, props);
  ASSERT_EQ(props.topIndex, 7);
  EXPECT_EQ(nav.top, 7);

  // Tap row 12 -> the option list is pushed. Nothing on this screen runs.
  nav.selected = 12;

  // Choose an option: the list pops. The parent is repainted WITHOUT onEnter()
  // (no reset), and its result handler calls rebuildSettings() -> rowItems are
  // rebuilt -> this build runs.
  build(nav, props);
  EXPECT_EQ(nav.top, 7) << "the list jumped back to the top after the selector closed";
  EXPECT_EQ(props.topIndex, 7);
  EXPECT_EQ(nav.selected.load(), 12);
}

TEST(SettingsSelectorScroll, ScrollIsWhatASelectorReturnMustNotDisturb) {
  // Same walk, but the selection is left where the operator last tapped it and
  // the viewport is deliberately NOT the selection's page (a swipe scroll).
  fui::ListNav nav;
  fui::ListProps props;
  nav.reset();
  nav.selected = 2;
  nav.top = 14;
  nav.followOnBuild = false;

  build(nav, props);
  ASSERT_EQ(props.topIndex, 14);

  // open + close the selector (no nav writes on either side)
  build(nav, props);
  build(nav, props);
  EXPECT_EQ(props.topIndex, 14);
}

TEST(SettingsSelectorScroll, AReEnteredScreenWouldLoseIt) {
  // The contrast that makes the test above meaningful: this is what the pop path
  // WOULD do if it re-ran onEnter() -- reset() drops the viewport to 0 and the
  // follow-on-build pulls it to the selection's page.
  fui::ListNav nav;
  fui::ListProps props;
  nav.top = 7;
  nav.selected = 12;
  nav.followOnBuild = false;
  build(nav, props);
  ASSERT_EQ(props.topIndex, 7);

  nav.reset();  // what UiListActivity::onEnter() does, and what a pop must NOT do
  build(nav, props);
  EXPECT_NE(props.topIndex, 7) << "reset() no longer changes the viewport; the guard above is now vacuous";
}
