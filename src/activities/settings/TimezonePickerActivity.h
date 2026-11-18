#pragma once
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// Timezone selection list (src/util/Timezones.cpp table). Selecting an entry
// persists it, re-applies the clock's TZ rule, and returns.
class TimezonePickerActivity final : public UiListActivity {
 public:
  explicit TimezonePickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  // r73 settings chrome: theme-owned row type/icons + a band-filling pitch.
  bool usesThemeListChrome() const override { return true; }
  bool usesListFooterPager() const override { return true; }
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  std::vector<std::string> offsetLabels_;
  std::vector<freeink::ui::ListItem> rowItems_;
};
