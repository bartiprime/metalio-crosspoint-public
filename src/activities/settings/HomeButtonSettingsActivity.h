#pragma once

#include "HomeButtonSettings.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class HomeButtonSettingsActivity final : public UiListActivity {
 public:
  HomeButtonSettingsActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("HomeButtonSettings", renderer, input) {}

 private:
  static constexpr int GESTURE_COUNT = 3;

  freeink::ui::ListItem rows[GESTURE_COUNT]{};
  OptionPopup optionPopup;

  int listCount() const override { return GESTURE_COUNT; }
  const char* headerTitle() const override { return tr(STR_HOME_BUTTON); }
  void buildScreen(UiScreen& screen) override;
  // r73 settings chrome: theme-owned row type/icons + a band-filling pitch.
  bool usesThemeListChrome() const override { return true; }
  bool usesListFooterPager() const override { return true; }
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void render(RenderLock&&) override;
};
