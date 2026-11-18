#include "UiTabListActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cassert>

#include "MappedInputManager.h"
#include "audio/UiSounds.h"  // AUDIO1: tab-ring move / select feedback
#include "components/UIScale.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

UiTabListActivity::UiTabListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const bool wantsTouchLongPress)
    : UiListActivity(name, renderer, mappedInput, wantsTouchLongPress) {}

void UiTabListActivity::onEnter() {
  // Size the per-tab state before the base resets activeNav() (which indexes
  // into it).
  tabNavs.assign(static_cast<size_t>(tabCount()), fui::ListNav{});
  UiListActivity::onEnter();
  app.on(ACTION_TAB, &UiTabListActivity::tabActionTrampoline, this);
}

fui::ListNav& UiTabListActivity::activeNav() {
  if (tabNavs.empty()) return nav;  // pre-onEnter fallback
  // Invariant: subclasses keep activeTab() inside [0, tabCount()), and
  // tabCount() does not change after onEnter() sized tabNavs.
  assert(activeTab() >= 0 && static_cast<size_t>(activeTab()) < tabNavs.size());
  return tabNavs[static_cast<size_t>(activeTab())];
}

int UiTabListActivity::ringPos() const {
  if (tabNavs.empty()) return 0;
  assert(activeTab() >= 0 && static_cast<size_t>(activeTab()) < tabNavs.size());
  return tabNavs[static_cast<size_t>(activeTab())].selected;
}

void UiTabListActivity::tabActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiTabListActivity*>(user);
  if (event.value < 0 || event.value >= self->tabCount()) return;
  self->onTabAction(event.value);
}

void UiTabListActivity::onRowAction(const fui::ActionEvent& event) {
  activeNav().selected = event.value + 1;  // ring position, not row index
  if (event.longPress) {
    onRowLongPress(event.value);
    return;
  }
  uisounds::onEvent(uisounds::Event::Select, uisounds::Tag::TabList);
  activateIndex(event.value);
}

void UiTabListActivity::moveRingTo(const int ringIndex) {
#if FREEINK_CAP_TOUCH
  closeSwipeDelete();
#endif
  // AUDIO3: same rule as UiListActivity::moveSelectionTo -- a tab press that
  // cannot move (already on that tab) is silent, not an error tone.
  if (uisounds::moveIsAudible(ringPos(), ringIndex)) {
    uisounds::onEvent(uisounds::Event::MenuMove, uisounds::Tag::TabList);
  }
  activeNav().requestSelection(ringIndex);
  requestUpdate();
}

void UiTabListActivity::navigateButtons() {
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) ||
      mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    navigationStartedOnTabs = ringPos() == 0;
  }
  // Buttons walk the tab band (index 0) plus the rows (1..listCount).
  const int ringSize = listCount() + 1;
  buttonNavigator.onNextPress([this, ringSize] { moveRingTo(ButtonNavigator::nextIndex(ringPos(), ringSize)); });
  buttonNavigator.onPreviousPress(
      [this, ringSize] { moveRingTo(ButtonNavigator::previousIndex(ringPos(), ringSize)); });
  buttonNavigator.onNextContinuous([this] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
    } else if (ringPos() == 0 && listCount() > 0) {
      activeNav().selected = 1;
    }
    stepTab(1);
  });
  buttonNavigator.onPreviousContinuous([this] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
    } else if (ringPos() == 0 && listCount() > 0) {
      activeNav().selected = 1;
    }
    stepTab(-1);
  });
}

void UiTabListActivity::syncTabListViewport(UiScreen& screen, fui::ListProps& props) {
  syncListViewport(screen, props, 1);
}

void UiTabListActivity::buildTabBar(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = UITheme::getInstance().getTheme();
  // r77 tab chrome. Two theme knobs, both default-off so a theme that does not
  // opt in paints exactly the r76 tab bar:
  //   * tabBarRuleThickness() -- the band's bottom rule. 0 resolves to the legacy
  //     expression max(1, headerUnderlineSize), i.e. the 1 px hairline.
  //   * tabActiveUnderlineThickness() -- when set, the active tab is marked by an
  //     underline of that weight and NOTHING is filled: no dithered pill, no band
  //     wash, no 2 px underline.
  const int activeUnderline = theme.tabActiveUnderlineThickness();
  const bool plainTabs = activeUnderline > 0;
  const int ruleThickness = theme.tabBarRuleThickness();

  // Tabs. The selected pill dims to a dither when the selection is down in
  // the list (the legacy focused/unfocused tab distinction).
  // Stack array, not a heap vector: this runs on every render and the tab
  // count is small and fixed.
  constexpr int MAX_TABS = 8;
  const int count = tabCount() < MAX_TABS ? tabCount() : MAX_TABS;
  fui::TabItem tabs[MAX_TABS];
  for (int i = 0; i < count; i++) {
    tabs[i].label = tabLabel(i);
    tabs[i].value = static_cast<int16_t>(i);
    tabs[i].selected = activeTab() == i;
    tabs[i].indicator = tabIndicator(i);
  }
  fui::TabBarProps tabProps;
  tabProps.tabs = tabs;
  tabProps.count = static_cast<uint16_t>(count);
  tabProps.action = ACTION_TAB;
  tabProps.inputMask = fui::InputTouch;
  // Pill shape and label size are theme-driven. Lyra uses equal-width slots
  // with small labels so wide text (e.g. "Controls") still fits at large UI scales.
  // Full-slot (RoundedRaff): the pill fills its slot like the legacy layout
  // (slot minus a 4px frame, 8px clearance above the divider) with
  // body-size labels; zero horizontal contentInset disables the tabBar's
  // label-width shrink.
  const bool tabsFocused = ringPos() == 0;
  if (metrics.tabPillFullSlot) {
    tabProps.text = screen.theme().bodyText;
    tabProps.tabInset = fui::Insets{4, 4, 7, 4};
    tabProps.contentInset = fui::Insets{2, 0, 2, 0};
  } else {
    // LIB8: the body face when the screen asked for it (library: 4 short labels,
    // 79.8 px widest against a 143 px pill), else the theme's small face.
    //
    // BAND2 (r101.12): the label face is resolved at DRAW time from the target's
    // FONT_BODY *slot*, not from the tokens -- a TextStyle carries a slot index,
    // and the font id behind it is whatever the target binds when the glyphs are
    // laid out. UiListActivity::applyThemeListChrome -> fitThemeRowLabels rebinds
    // FONT_BODY to the theme's ROW face (Ember 24, line height 60) to paint its
    // rows, and that binding survives the compose -- while the tab bar paints
    // FIRST. So paint 1 drew the uiScale body face (UI_12) and every later paint
    // drew the row face: the exact BAND1 leak, one layer down in the text path.
    // Bind the canonical makeUiTarget() face here, immediately before anything
    // measures or draws these labels; the list rebinds the slot for its own rows
    // straight after this call. FONT_SMALL needs no equivalent: nothing rebinds it
    // differently per compose (titledHeaderBand binds SMALL_FONT_ID on every pass,
    // always ahead of the tabs), so the small face is already stable.
    uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, uiScaleSpec().bodyFontId);
    tabProps.text = tabLabelsUseBodyFace() ? screen.theme().bodyText : screen.theme().smallText;
    tabProps.gap = static_cast<int16_t>(metrics.tabSpacing);
    // Unfocused state: no bottom inset, so the pill (and the 2px selected
    // underline drawn along its bottom edge) reaches the band's 1px divider —
    // legacy Lyra drew the underline sitting on that rule, not floating above.
    // A theme with the fill-free treatment instead keeps the active underline
    // 4 px clear of the band rule: two 6 px strokes sharing an edge would read
    // as one 12 px blob, not as an indicator over a rule.
    const int16_t underlineClear = plainTabs ? static_cast<int16_t>(4) : static_cast<int16_t>(0);
    tabProps.tabInset = tabsFocused ? fui::Insets{2, 4, 4, 4} : fui::Insets{2, 4, underlineClear, 4};
    tabProps.contentInset = fui::Insets{2, 0, 2, 0};
  }
  const int16_t tabLineHeight = screen.target().lineHeight(tabProps.text.font);
  // U-2: the band height is the THEME's metric, never a literal. On the touch path this
  // used to be a hardcoded 50 px -- SMALLER than the 48 design -> 65 px band the Andro theme
  // authored, and short enough to leave a 44 px tab slot (3.8 mm): the exact size r76
  // rejected as unhittable while every other control on this panel was scaled up.
  // metrics.tabBarHeight was never read on touch, so the theme had no say here at all.
  //
  // The operator's standing rule then asks for a touch target HALF AGAIN the theme's touch
  // minimum, not merely equal to it. minTouchSize alone (54 px on Andro, derived from the
  // body line box) would leave a 59 px slot = 1.09x, so the band also has to keep the SLOT
  // (band minus the rule under it) at ceil(1.5 x minTouchSize) = 81 px. The band is the
  // larger of the two, so a theme that authors a taller tab bar still wins.
  const int16_t tabHeightMetric = static_cast<int16_t>(metrics.tabBarHeight);
  const int16_t tabTouchMin = static_cast<int16_t>(std::max<int16_t>(44, screen.theme().minTouchSize));
  const int16_t tabRule =
      static_cast<int16_t>(ruleThickness > 0 ? ruleThickness : std::max(1, metrics.headerUnderlineSize));
  const int16_t tabSlotFloor = static_cast<int16_t>((tabTouchMin * 3 + 1) / 2);  // ceil(1.5 x)
  const int16_t preferredTabHeight =
      mappedInput.hasTouch() ? std::max<int16_t>(tabHeightMetric, static_cast<int16_t>(tabSlotFloor + tabRule))
                             : tabHeightMetric;
  const int16_t tabBand = preferredTabHeight > tabLineHeight + 10 ? preferredTabHeight : tabLineHeight + 10;
  // The one measurement for this item: the band the tab bar occupies, the theme number it
  // came from, the SLOT that band leaves (band minus the rule), and the touch minimum both
  // numbers were checked against. tabBand >= tabRule + ceil(1.5 x touchMin) is the pass.
  LOG_DBG("TAB", "%s band=%d metric=%d slot=%d touchMin=%d", name.c_str(), tabBand, tabHeightMetric,
          static_cast<int>(tabBand - tabRule), tabTouchMin);
  tabProps.minTouchSize = tabTouchMin;

  if (tabPillMaxPad > 0) {
    // Cap each pill at its label plus this padding: the equal-width slots (and
    // so the tab positions) stay exactly where they were, only the pill stops
    // stretching across the whole slot. The SDK shrinks the pill to content
    // width and centers it in its slot when the horizontal contentInset is
    // nonzero.
    tabProps.contentInset.left = tabPillMaxPad;
    tabProps.contentInset.right = tabPillMaxPad;
  }

  // Legacy Lyra two-state treatment: with the selection on the tab band, the
  // band fills gray and the active tab is a solid pill; with the selection
  // down in the list, the band is plain and the active tab keeps a gray box
  // with an underline. The 1px rule under the band is always there, drawn
  // full-width below (not by tabBar, whose rect is inset for side padding).
  fui::StyleSet tabStyles;
  tabStyles.explicitlySet = true;
  tabStyles.normal.foreground = fui::Paint::solid(fui::Color::Black);
  if (plainTabs) {
    // No fill anywhere: the band is paper, the inactive labels are black, and the
    // ACTIVE tab is told apart by the underline below (tabProps.selectedUnderline,
    // set from the theme's weight at the bottom of this function). Nothing here
    // changes the tab geometry or the hit rects -- fui::tabBar registers each
    // tab's slot, which is the rect the label and its underline are laid out in.
    tabStyles.normal.background = fui::Paint::none();
    tabStyles.selected.background = fui::Paint::none();
    tabStyles.selected.foreground = fui::Paint::solid(fui::Color::Black);
    tabProps.selectedUnderline = static_cast<int16_t>(activeUnderline);
  } else if (tabsFocused) {
    tabStyles.selected.background = fui::Paint::solid(fui::Color::Black);
    tabStyles.selected.foreground = fui::Paint::solid(fui::Color::White);
    tabStyles.selected.radius = screen.theme().listRowRadius;
  } else if (metrics.tabPillFullSlot) {
    // Legacy RoundedRaff unfocused treatment: same pill, dimmed to dark gray,
    // text stays inverted; no underline.
    tabStyles.selected.background = fui::Paint::dither(fui::Color::DarkGray);
    tabStyles.selected.foreground = fui::Paint::solid(fui::Color::White);
    tabStyles.selected.radius = screen.theme().listRowRadius;
  } else {
    tabStyles.selected.background = fui::Paint::dither(fui::Color::LightGray);
    tabStyles.selected.foreground = fui::Paint::solid(fui::Color::Black);
    tabProps.selectedUnderline = 2;
  }
  // Focus/flash states keep the pill instead of falling back to an unset
  // (blank) style.
  tabStyles.focused = tabStyles.selected;
  tabStyles.active = tabStyles.selected;
  tabProps.tabStyles = tabStyles;
  const fui::Rect contentTabRect = screen.takeTop(tabBand);
  const fui::Rect frameRect = screen.frame().screen();
  // Tab chrome is a full-width screen band like the legacy GUI tab bar. The
  // remaining list content still stays inside the device safe area.
  const fui::Rect tabRect{frameRect.x, contentTabRect.y, frameRect.width, contentTabRect.height};
  // Focused band wash is the Lyra treatment; legacy RoundedRaff keeps the
  // band plain in both states.
  if (tabsFocused && !metrics.tabPillFullSlot && !plainTabs) {
    screen.target().fill(tabRect, fui::Paint::dither(fui::Color::LightGray));
  }
  // The band chrome (wash, divider) spans the full screen width, but the tab
  // slots keep the content side padding so the outer pills never touch the
  // bezel. The divider is drawn here rather than by tabBar(), which would
  // inset it along with the slots; the slot band is shortened by the divider
  // so the pills end on it. The divider matches the header's bottom rule.
  // ONE rule of the theme's weight (Andro: 6 px, the header rule's weight)
  // instead of the 1 px hairline; a theme that does not opt in resolves to the
  // legacy expression, to the pixel.
  const auto divider = tabRule;
  const auto side = static_cast<int16_t>(metrics.contentSidePadding);
  const fui::Rect slotsRect{static_cast<int16_t>(tabRect.x + side), tabRect.y,
                            static_cast<int16_t>(tabRect.width - 2 * side),
                            static_cast<int16_t>(tabRect.height - divider)};
  tabProps.divider = false;
  fui::tabBar(screen.frame(), slotsRect, tabProps);
  screen.target().fill(fui::Rect{tabRect.x, static_cast<int16_t>(tabRect.bottom() - divider), tabRect.width, divider},
                       fui::Paint::solid(fui::Color::Black));
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
}
