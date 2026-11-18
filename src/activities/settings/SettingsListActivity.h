#pragma once

#include <array>

#include "SettingsSection.h"
#include "activities/UiListActivity.h"

// Settings root: one row per SettingsSection, each opening a
// SettingsActivity with that group's settings.
class SettingsListActivity final : public UiListActivity {
  // Section opened on the first loop pass (End = none), e.g. returning to
  // Network after the Wi-Fi rows' silent restart.
  SettingsSection pendingSection;

  // Sections plus one trailing "Debug" row. It is not a SettingsSection: the debug page is its own
  // activity (hardcoded English, like About), so it needs no i18n string and no section enum entry.
  static constexpr size_t ROW_COUNT = SETTINGS_SECTION_COUNT + 1;
  std::array<freeink::ui::ListItem, ROW_COUNT> rowItems_{};

  void rebuildRows();
  void openSection(SettingsSection section);

  int listCount() const override { return static_cast<int>(ROW_COUNT); }
  void buildScreen(UiScreen& screen) override;

  // r73 settings chrome: theme-owned row type/icons + a band-filling pitch.
  bool usesThemeListChrome() const override { return true; }
  bool usesListFooterPager() const override { return true; }
  void activateIndex(int index) override;
  void onBackButton() override;
  void drawChrome() override;

 public:
  explicit SettingsListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                SettingsSection openSection = SettingsSection::End);
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
