#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "ReadingStatsStore.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {

// The last path segment, for a store entry whose title never arrived (the reader was
// closed before the first render). Never empty as long as the path is not, so the row
// always names something.
std::string fileNameOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

ReadingStatsActivity::ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("ReadingStats", renderer, mappedInput) {}

void ReadingStatsActivity::onEnter() {
  UiListActivity::onEnter();
  // The record only changes while a reader is open, so one snapshot per visit is enough;
  // opening this page cannot make the numbers stale.
  refreshRows();
}

const char* ReadingStatsActivity::headerTitle() const { return I18N.get(StrId::STR_READING_STATISTICS); }

void ReadingStatsActivity::refreshRows() {
  char value[16];
  int row = 0;

  const auto addBlock = [&](const std::string& name, const readingstats::Tally& tally) {
    // Row 0 carries the block name and the time; the two metrics are indented under it by
    // name, the way the battery page labels its blocks' metrics.
    rowLabels_[row] = name;
    readingstats::formatDuration(tally.seconds, value, sizeof(value));
    rowValues_[row] = value;
    ++row;

    rowLabels_[row] = I18N.get(StrId::STR_STATS_PAGES_TURNED);
    snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(tally.pages));
    rowValues_[row] = value;
    ++row;

    rowLabels_[row] = I18N.get(StrId::STR_STATS_PAGES_PER_HOUR);
    readingstats::formatPagesPerHour(tally, value, sizeof(value));
    rowValues_[row] = value;
    ++row;
  };

  const auto& store = READING_STATS;
  addBlock(I18N.get(StrId::STR_STATS_ALL_BOOKS), store.getOverall());

  for (const auto& book : store.getBooks()) {
    // MAX_ROWS is derived from kMaxBooks, so this cannot overrun even if the store
    // somehow returns more entries than it is allowed to keep.
    if (row + ROWS_PER_BLOCK > MAX_ROWS) break;
    addBlock(book.title.empty() ? fileNameOf(book.path) : book.title, book.tally);
  }

  rowCount_ = row;
  for (int i = 0; i < rowCount_; ++i) {
    rowItems_[i] = fui::ListItem{};
    rowItems_[i].label = rowLabels_[i].c_str();
    rowItems_[i].value = rowValues_[i].c_str();
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

void ReadingStatsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Same band as AboutActivity and BatteryStats: titled header above, button hints below,
  // no footer button of its own.
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + UITheme::getInstance().getTitledHeaderHeight()), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_;
  props.count = rowCount_;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}
