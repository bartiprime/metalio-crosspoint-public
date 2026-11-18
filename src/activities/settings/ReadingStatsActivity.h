#pragma once
#include <string>

#include "activities/UiListActivity.h"
#include "util/ReadingStats.h"

// r163 3.3 -- the reading-statistics page (Settings > Reading > Reading statistics).
//
// A Settings SUB-PAGE drawn with the same list chrome every other settings screen uses
// (AboutActivity is the closest sibling: read-only rows, no footer button). It reports
// what the reader actually measured -- ReaderSession's forward page turns and the dwells
// between them, handed to ReadingStatsStore once per reader session -- and nothing that
// was inferred from bytes or page counts.
//
// ONE BLOCK OF THREE ROWS PER BOOK, plus an "All books" block first:
//   <block name>      time
//   Pages turned      count
//   Pages per hour    rate, or "--" when the recorded time cannot support one
//
// The rate's "--" is the point of the page: a book with 12 pages and 40 seconds of
// recorded time has an arithmetic answer (1080/h) that is not a reading speed. Under
// readingstats::kMinRateSeconds of time the row says it cannot tell yet, exactly the way
// BatteryStats prints "--" instead of dividing one sample by itself.
//
// BOUND: one block per stored book (readingstats::kMaxBooks = 12) plus the overall block,
// so the row set is fixed at 39 rows and every buffer below is a fixed array -- the render
// pass must not allocate.
class ReadingStatsActivity final : public UiListActivity {
 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int ROWS_PER_BLOCK = 3;
  static constexpr int MAX_ROWS = ROWS_PER_BLOCK * (readingstats::kMaxBooks + 1);

  void onEnter() override;

 private:
  int listCount() const override { return rowCount_; }
  void buildScreen(UiScreen& screen) override;
  // r73 settings chrome: theme-owned row type/icons + a band-filling pitch.
  bool usesThemeListChrome() const override { return true; }
  bool usesListFooterPager() const override { return true; }
  void activateIndex(int) override {}
  const char* headerTitle() const override;

  void refreshRows();

  std::string rowLabels_[MAX_ROWS];
  std::string rowValues_[MAX_ROWS];
  freeink::ui::ListItem rowItems_[MAX_ROWS]{};
  int rowCount_ = 0;
};
