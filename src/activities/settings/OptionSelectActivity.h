#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

class MappedInputManager;

// SET3 (r164): the option list a discrete Settings row opens.
//
// One screen for every such row: the settings list is the parent, so a choice
// (or Back) is a pop, and the pop path does not re-run the parent's onEnter()
// (ActivityManager.cpp, the restored-parent branch) -- which is what keeps the
// settings list at the scroll position the operator left it at, since
// UiListActivity::onEnter() is the only thing that calls activeNav().reset().
//
// The options are copied in as strings: a dynamic row (font family, dictionary)
// builds its options at runtime and the list must show exactly what the row had
// when it was tapped, not what a later rebuild would produce.
class OptionSelectActivity final : public UiListActivity {
 public:
  OptionSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, StrId titleId,
                       std::vector<std::string> labels, int currentIndex);

  void onEnter() override;

 private:
  int listCount() const override { return static_cast<int>(rowItems_.size()); }
  void buildScreen(UiScreen& screen) override;
  // Same theme-owned chrome as the settings list it was opened from, so the
  // selector inherits Andro/PickyOS row treatment instead of drawing its own.
  bool usesThemeListChrome() const override { return true; }
  bool usesListFooterPager() const override { return true; }
  void activateIndex(int index) override;
  void onBackButton() override;
  const char* headerTitle() const override;

  const StrId titleId;
  std::vector<std::string> labels_;
  // Option the row is on when the list opens; also what "unchanged" means if
  // Back is pressed (the result then carries isCancelled and nothing is written).
  int currentIndex_ = 0;
  std::vector<freeink::ui::ListItem> rowItems_;
};
