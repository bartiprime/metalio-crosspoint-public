#include "SettingsListActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "DebugActivity.h"
#include "SettingsActivity.h"
#include "components/UITheme.h"
#include "components/icons/settingsRowIcons64.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
constexpr int16_t ICON_SIZE = 32;
// Row icons, indexed by SettingsSection -- same order as the enum.
constexpr const freeink::Icon* SECTION_ICONS[SETTINGS_SECTION_COUNT] = {
    &icon_smartphone_32, &icon_book_open_32, &icon_pointer_32, &icon_library_32, &icon_wifi_32, &icon_settings_32,
};
// The same rows out of the 64 px masks (settingsRowIcons64.h). A theme that asks
// for a row icon box bigger than the 32 px list assets gets these, so the box is
// sampled DOWN from 64 instead of upscaled from 32. Scaling stays in the SDK's
// list: props.iconSize + BitmapMode::Contain in list.h, one sampler for every
// screen -- no second upscaler to drift from it.
constexpr const freeink::Icon* SECTION_ICONS_64[SETTINGS_SECTION_COUNT] = {
    &icon_srow_smartphone_64, &icon_srow_book_open_64, &icon_srow_pointer_64,
    &icon_srow_library_64,    &icon_srow_wifi_64,      &icon_srow_settings_64,
};
}  // namespace

SettingsListActivity::SettingsListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const SettingsSection openSection)
    : UiListActivity("SettingsList", renderer, mappedInput), pendingSection(openSection) {}

void SettingsListActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRows();
}

void SettingsListActivity::onExit() {
  UiListActivity::onExit();
  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsListActivity::loop() {
  if (pendingSection != SettingsSection::End) {
    const SettingsSection section = pendingSection;
    pendingSection = SettingsSection::End;
    openSection(section);
    return;
  }
  UiListActivity::loop();
}

// Rebuilds row labels; call when entering and after a section returns (the
// UI language may have changed).
void SettingsListActivity::rebuildRows() {
  // Icon source follows the theme's row icon box: a box wider than the 32 px
  // list assets (Andro's 44) takes the 64 px row masks, everything else keeps
  // the 32 px set every theme has always drawn here.
  const bool largeIcons = UITheme::getInstance().getTheme().listRowIconSize() > ICON_SIZE;
  const freeink::Icon* const* icons = largeIcons ? SECTION_ICONS_64 : SECTION_ICONS;
  for (size_t i = 0; i < SETTINGS_SECTION_COUNT; ++i) {
    auto& item = rowItems_[i];
    item.label = I18N.get(SETTINGS_SECTION_TITLES[i]);
    item.icon = fui::bitmapFromIcon(*icons[i]);
    item.actionValue = static_cast<int16_t>(i);
  }
  {
    // Deliberately hardcoded English, same exemption as AboutActivity: a diagnostics screen has to
    // read identically on every unit. At the large row size it gets its own bug glyph -- the
    // pointing hand it shared with Controls reads as a duplicate once the icons are 44 px. The
    // 32 px set keeps that hand so the themes that do not resize rows paint this row exactly as
    // they did in r72.
    auto& item = rowItems_[SETTINGS_SECTION_COUNT];
    item.label = "Debug";
    item.icon = fui::bitmapFromIcon(largeIcons
                                        ? icon_srow_bug_64
                                        : *SECTION_ICONS[static_cast<size_t>(SettingsSection::Controls)]);
    item.actionValue = static_cast<int16_t>(SETTINGS_SECTION_COUNT);
  }
}

void SettingsListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + UITheme::getInstance().getTitledHeaderHeight()), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  // Rows are as tall as a label plus a small second line, giving the icons air.
  const auto& theme = screen.theme();
  const int16_t textHeight = static_cast<int16_t>(screen.target().lineHeight(theme.bodyText.font) +
                                                  screen.target().lineHeight(theme.smallText.font));
  const int16_t paddingY = mappedInput.hasTouch() ? theme.listTouchRowPaddingY : theme.listRowPaddingY;
  // A theme may own the row measure on touch (Andro: 68 design -> 92 px). When
  // it does not, the row stays content-derived -- label line + small line +
  // icon box + the theme's touch padding -- which is what every other theme
  // gets, unchanged. This is a virtual, not a metrics entry, so no theme but
  // Andro sees any change and the boot [UISCALE] audit has nothing new to know.
  const int themedRowHeight = mappedInput.hasTouch() ? UITheme::getInstance().getTheme().listTouchRowHeight() : 0;
  props.rowHeight = static_cast<int16_t>(
      themedRowHeight > 0 ? themedRowHeight : std::max<int16_t>(textHeight, ICON_SIZE) + 2 * paddingY);
  syncListViewport(screen, props);
  screen.list(props);
}

void SettingsListActivity::activateIndex(const int index) {
  // The section screen replaces this one; a lingering flash would gray an
  // unrelated element on return.
  app.clearTapFlash();
  if (index >= static_cast<int>(SETTINGS_SECTION_COUNT)) {
    if (auto activity = makeUniqueNoThrow<DebugActivity>(renderer, mappedInput)) {
      startActivityForResult(std::move(activity), nullptr);
    } else {
      LOG_ERR("SETTINGS", "OOM: DebugActivity");
    }
    return;
  }
  openSection(static_cast<SettingsSection>(index));
}

void SettingsListActivity::openSection(const SettingsSection section) {
  nav.selected = static_cast<int>(section);
  auto activity = makeUniqueNoThrow<SettingsActivity>(renderer, mappedInput, section);
  if (!activity) {
    LOG_ERR("SETTINGS", "OOM: SettingsActivity");
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) {
    rebuildRows();
    // A theme change inside the section must restyle this screen too.
    resetUi();
    requestUpdate();
  });
}

// r163: a single tap on Back is BACK, one level, on every screen. This screen used to answer the tap
// with onGoHome(), which made the settings root the one place in the firmware where a TAP (not a hold)
// meant "main screen" -- and, because ActivityManager's hold branch ran for any screen that owns the
// key, it is what made a tap land on Home one level deeper. finish() pops instead, and at the root the
// stack is already empty (goToSettings() replaces rather than pushes), so the pop path lands on Home
// exactly as before. Hold-to-Home is unchanged and lives in ActivityManager.
void SettingsListActivity::onBackButton() { finish(); }

void SettingsListActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // No trailing label: the version now identifies our build and lives on the
  // About page only (AboutActivity), instead of riding in this header.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), UITheme::getInstance().getTitledHeaderHeight()},
                 tr(STR_SETTINGS_TITLE));
}
