#include "OptionSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <utility>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

OptionSelectActivity::OptionSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const StrId titleId, std::vector<std::string> labels,
                                           const int currentIndex)
    : UiListActivity("OptionSelect", renderer, mappedInput),
      titleId(titleId),
      labels_(std::move(labels)),
      currentIndex_(currentIndex) {}

void OptionSelectActivity::onEnter() {
  UiListActivity::onEnter();

  // AFTER the base call: UiListActivity::onEnter() resets the nav, so anything
  // written before it is lost. Opening on the current option (and letting the
  // first build's follow-on-build pull the viewport to it) is what makes a long
  // list -- every installed font family, say -- open where the value is rather
  // than at the top.
  nav.selected = currentIndex_ >= 0 && currentIndex_ < listCount() ? currentIndex_ : 0;

  // Built once: the labels are fixed for this visit and activateIndex() leaves
  // the screen immediately, so the "Selected" marker cannot go stale mid-visit.
  rowItems_.reserve(labels_.size());
  for (size_t i = 0; i < labels_.size(); i++) {
    fui::ListItem item;
    item.label = labels_[i].c_str();
    // The current option is the one the row is on; marking it is the whole point
    // of the list (the row's value text said the same thing one screen back).
    if (static_cast<int>(i) == currentIndex_) item.value = tr(STR_SELECTED);
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
}

const char* OptionSelectActivity::headerTitle() const { return I18N.get(titleId); }

void OptionSelectActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  // The row leaves this screen; a lingering tap flash would gray an unrelated
  // element on the next render of the settings list underneath.
  app.clearTapFlash();
  nav.selected = index;
  setResult(ActivityResult{OptionIndexResult{index}});
  finish();
}

void OptionSelectActivity::onBackButton() {
  // Back means "leave it as it was": the result says cancelled and the caller
  // writes nothing, so persistence is untouched by a visit that picks nothing.
  ActivityResult cancelled;
  cancelled.isCancelled = true;
  setResult(std::move(cancelled));
  finish();
}

void OptionSelectActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints — the same
  // margins the settings list under it uses, so the two screens read as one tree.
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + UITheme::getInstance().getTitledHeaderHeight()), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  // Same faces the settings list paints: a choice reads in the same type as the
  // row that opened it.
  props.labelText = mappedInput.hasTouch() ? screen.theme().bodyText : screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
