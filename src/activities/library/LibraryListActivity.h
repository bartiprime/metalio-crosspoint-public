#pragma once

#include <LibraryIndexFile.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"

// One Library screen: every indexed book on the card shown by recency, title,
// author, or series. The Recent shelf orders by file modification time (when a book
// landed on the card) and pins the recently OPENED books from RecentBooksStore
// on top, so active reads and fresh arrivals share one list.
//
// The two-slot row is the whole point rather than a styling choice: the problem
// being solved is "I cannot find my books because I do not know the authors",
// and that is answered by a column the eye can sweep, not by a tidier filename.
//
// Rows render through fui::list on the UiTabListActivity ring (0 = the sort
// strip, 1..N = the books), which is what brings touch to rows and tabs. Titles
// are truncated to one line by the widget — more books on the screen, even if
// half a name is hidden.
//
// Search matches title, author, series and filename against folds the index
// stored at build time, ranked prefix-of-title > title > series > author >
// filename. Only the visible window of rows is materialized per render (strings and
// ListItems for at most one page). The ordinary shelf therefore keeps one page
// of strings; an active search additionally uses one fallible uint16_t slot per
// indexed book so an allocation failure remains recoverable on the C3.
class LibraryListActivity final : public UiTabListActivity {
 public:
  LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;

 protected:
  // --- UiListActivity / UiTabListActivity contract ---------------------------
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
#if FREEINK_CAP_TOUCH
  // LIB7: the bucket rows are series, not books, so a swipe cannot delete one.
  bool canSwipeDelete(int index) const override {
    return !groupsCollapsed && !seriesIndexActive() && index >= 0 && index < listCount();
  }
  void swipeDelete(int index) override;
#endif
  int tabCount() const override;
  int activeTab() const override;
  const char* tabLabel(int index) const override;
  freeink::ui::TabIndicator tabIndicator(int index) const override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void navigateButtons() override;
  // The FreeInkUI header owns both the title and search touch target.
  void drawChrome() override {}
  void drawFooter() override;
  // OptionPopup is a self-contained modal: it owns rendering (and the button
  // hints) whenever it is up.
  void render(RenderLock&& lock) override;

  // --- LIB6: the < / > pager -------------------------------------------------
  // (operator, 6 Nov 2026: the shelf had no way to turn a page). This screen owns
  // the shared footer pager -- ACTION_PAGE = 253, the same bar the settings tree
  // and the reader menu paint -- unconditionally rather than only under a theme
  // that pages its settings list, because the request was for THIS screen. The
  // band is reserved from the screen's own content rect (takeBottom), so the
  // arrows can only ever land on rows: never under the tab band, never under the
  // status row. With one page reservePageTabs() reserves nothing.
  bool usesListFooterPager() const override { return true; }
  bool listPagerOptIn() const override { return true; }
  // LIB8 (operator, 6 Nov 2026: the four labels are too small). The body face:
  // UI_12, the top rung of the baked UI ladder.
  bool tabLabelsUseBodyFace() const override { return true; }

 private:
  // The screen's own actions, after the base's ACTION_ROW / ACTION_TAB.
  static constexpr freeink::ui::ActionId ACTION_SEARCH = ACTION_TAB_USER;
  static constexpr freeink::ui::ActionId ACTION_REBUILD = ACTION_SEARCH + 1;
  static constexpr freeink::ui::ActionId ACTION_BACK = ACTION_REBUILD + 1;

  // LIB1: the library no longer rebuilds itself behind a static popup on entry.
  // A full card is minutes of reading, so entry raises a banner the reader can
  // defer, and the walk itself runs on LibraryIndexActivity, which can show the
  // count and stop between books.
  void promptIndexBuild();
  void runIndexBuild();
  // The blocking form, still used after a delete: the shelf has to be
  // reconciled before it can be redrawn, and there is no screen to defer to
  // inside a result callback. Same stepper, driven straight through.
  bool rebuildIndex();

  // Input
  void openSelectedBook();
  void openSearch();
  // Shared tail of row activation and the options menu's Open entry.
  void openBookByPath(const std::string& path);
  void promptRebuildIndex();
  void resetAfterRebuild();
  // Recent-row long-press menu: open / remove from recents / delete / rebuild.
  void showRecentBookOptions(int entry);
  void promptRemoveRecentBook(const std::string& path, const std::string& title);
  // Long-press delete owns the gesture where grouping does not apply: the
  // Recent sort, degraded lists, and any active search result.
  bool deleteEligible() const;
  // Resolves the row's path and title, then confirms via promptDeleteBookByPath.
  void promptDeleteBook(int entry);
  void promptDeleteBookByPath(const std::string& path, const std::string& title);
  bool collapseGroups(int bookEntry);
  void expandGroup(int groupEntry);
  void restoreExpandedList();
  void selectTab(int index, bool toggleIfActive);
  void toggleSortDirection();
  // Sub-screens act on button press, so a button still held when we resume must
  // not also act here. Records what to swallow on the next release.
  void swallowHeldReleases();
  // Staged back-out shared by Button::Back and the header back arrow.
  void handleBackAction();
  static void searchActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void rebuildActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void backActionTrampoline(const freeink::ui::ActionEvent& event, void* user);

  // Data
  void applyFilter();
  int bookRowCount() const;
  int rowFor(int entry) const;
  // fileName, when asked for, is the on-card name the row's icon derives from
  // (the display title may come from metadata and carry no extension).
  bool rowTextFor(int entry, std::string& title, std::string& author, std::string* fileName = nullptr);
  uint32_t titleInitialFor(int entry);
  // The series the row's book declares, exactly as its OPF wrote it. Empty means
  // a standalone — the shelf shows that as its own bucket and never invents a
  // series from a title.
  bool seriesFor(int entry, std::string& series);
  bool buildGroupStarts();
  int groupForBook(int bookEntry) const;
  bool groupable() const;

  // --- LIB7: the Series tab is a LIST of series, not inline groups -----------
  // (operator, 6 Nov 2026: "we just want the series to be listed and then when we
  // click the series it will only show the books in the series"). The grouping
  // DECISION is untouched -- still LIB1's bucket-per-declared-series, one bucket
  // for the books that declare none -- only the presentation changes: the Series
  // tab opens on the buckets, and a bucket opens as a filtered book list.
  //
  // The bucket list needs no new data structure: the Series sort orders by series
  // NAME, so every series is one contiguous run of rows, and that is exactly what
  // buildGroupStarts() already records in groupStarts[]/groupCount. A row's book
  // count is the distance to the next start.
  bool seriesIndexActive() const;
  // Builds that map for the Series sort when it is not current and caches the row
  // count the viewport asks for. LOOP TASK ONLY: it walks the index.
  void ensureSeriesIndex();
  // Opens bucket `entry`: the drill list is the [start, next start) slice of the
  // Series sort, so entering a series costs one string read.
  void enterSeries(int entry);
  // Back to the bucket list, at the position it was left on.
  void exitSeries();
  // The series a SORT ROW declares (rowFor() has already been applied for
  // entries). Empty means the standalone bucket.
  bool seriesForRow(int row, std::string& series);
  int seriesBucketEnd(int entry) const;

  // --- LIB10: the drill shows the file name at ONE fixed face, two lines ------
  // (operator, 6 Nov 2026: *"we will keep the current row height, but shrink the
  // fontsize dynamically to accommodate the 2-3 lines word wrap"*, then, after
  // seeing it: *"Fixed Filename Size is what i want the format to be but in two
  // lines word wrapped, but right now its single line."*) A drilled row therefore
  // carries the book's FILE NAME and it wraps to two lines -- at the theme's ONE
  // dense file-name face, inside the row height the list already has.
  //
  // WHY ONE FACE AND NOT A PER-ROW LADDER. The first cut picked each row's face
  // from the theme's ladder by measuring that row's own text. That makes the
  // layout's own input -- which rows are in the window -- decide the layout: a
  // long name wraps to three lines, the row grows, one fewer row fits, the next
  // window holds different names, a different face wins, and the page size
  // oscillates forever (on the operator's 17-book series: 9 rows / 2 pages, then
  // 8 rows / 3 pages, repeatedly). One face for the whole drill removes the
  // dependency: same face -> same line height -> every row the same height ->
  // measureRowSetPageRows() prices a uniform set and the tiling is stable.
  //
  // ROW HEIGHT IS NEVER TOUCHED. The LIST lays the rows out single-line (the
  // measured row height is exactly ListProps::rowHeight, whatever the names are),
  // and the second line is drawn inside that same box afterwards. A name too long
  // for two lines is ellipsised at the second line, not given a taller row.
  bool drillFileNames() const;
  // The one face every drill row is set in: the theme's dense file-name face
  // (Andro: Ember 18). A NAMED FACE, not a slot, so nothing a list or a header
  // rebinds on the shared target changes the answer (BAND1 / LABEL1 / B-P1-2).
  // 0 when the theme offers none or the binary did not bake it.
  int drillFace();
  // How many lines a drill row may wrap to: 2 when two of the face's line boxes
  // fit `boxH`, else 1. A function of (boxH, theme) ONLY -- never of the row's
  // text and never of which rows are on screen, which is what keeps it a fixed
  // point.
  int drillLineCap(int16_t boxH);
  // Repaints the wrapped file names over the rows list() just laid out, inside
  // the same compose, inside the SAME row boxes.
  void drawDrillFileNames(UiScreen& screen, freeink::ui::ListProps& props, int face);
  static constexpr int kDrillRowCap = 16;

  // Screen building
  void buildHeader(UiScreen& screen);
  // Materializes ListItems and their strings for the visible window only.
  void buildRows(UiScreen& screen);
  static void formatInitialHeading(uint32_t initial, std::string& out);
  void formatAuthorHeading(const std::string& author, std::string& out) const;
  void drawPositionReadout() const;
  void drawHoldHelp() const;
  const char* headerTitle() const override;

  // Ring 0 is the strip; the selected BOOK is ring - 1, with the strip keeping
  // row 0 as the working selection exactly as the pre-ring code did.
  int selectedEntry() const;
  bool tabsFocused() const { return ringPos() == 0; }

  // --- pinned recently-opened overlay ---------------------------------------
  // On the unfiltered Recent shelf the RecentBooksStore entries sit on top, in
  // read order; the modification-time list follows with those books skipped.
  // Entries below pinnedCount() are store rows; the rest go through rowFor().
  int pinnedCount() const;
  // Re-match the store against the index (chunked scan). Call whenever the
  // index or the store changes.
  void resolvePinned();
  // Direction-space translation of the matched rows. Call on sort toggles.
  void refreshOverlap();

  library::LibraryIndexFile index;
  int activeTabIndex = 0;
  library::SortOrder sortOrder = library::SortOrder::RecentDesc;
  // One bit per tab; Recent starts descending (newest first).
  uint8_t descendingTabs = 1u << 0;
  // Set when the walk finished but the sort did not, so the screen can say the
  // order is discovery order rather than silently showing a wrong one.
  bool degraded = false;

  // Rows surviving the current query, as positions in the active sort order.
  // Empty query means no filtering and this owns no allocation, so the ordinary
  // shelf pays nothing proportional to the library for the feature.
  std::string query;
  // The active query, pre-quoted for the header: headerTitle() returns a
  // stable c_str the render task can hold across a build.
  std::string headerSearchTitle;
  std::unique_ptr<uint16_t[]> filtered;
  uint16_t filteredCount = 0;
  bool filterFailed = false;

  // One start row per group. Grouping is only offered for the sorted <=512-book
  // index, so this fallible allocation is at most 1 KiB and is reused after its
  // first successful allocation.
  std::unique_ptr<uint16_t[]> groupStarts;
  uint16_t groupCapacity = 0;
  uint16_t groupCount = 0;
  bool groupsCollapsed = false;
  freeink::ui::ListNav expandedNav;

  // LIB7: set while the reader is inside one series. The rows are the
  // [seriesDrillStart, seriesDrillStart + seriesDrillCount) slice of the Series
  // sort (rowFor() maps entry -> row through it), and seriesIndexNav is where the
  // bucket list was left, so Back returns to the same row.
  bool seriesDrill = false;
  uint16_t seriesDrillStart = 0;
  uint16_t seriesDrillCount = 0;
  freeink::ui::ListNav seriesIndexNav;
  // The header shows the series name while drilling, so the reader knows which
  // filter is on without going back.
  std::string seriesDrillName;
  // Rows in the bucket list (one per series, standalone included) and whether
  // groupStarts/groupCount currently describe the SERIES sort. listCount() is
  // called from the render task and must not walk the index, hence the cache.
  uint16_t seriesIndexCount = 0;
  bool seriesMapReady = false;

  // Visible-window row storage, reused across renders (buildRows). Bounded by
  // the densest page, never by the library. Headings get their own storage:
  // the surname-first inversion must not overwrite the author slot, whose raw
  // value the next row's group comparison reads.
  std::vector<freeink::ui::ListItem> winItems;
  std::vector<std::string> winTitles;
  std::vector<std::string> winAuthors;
  std::vector<std::string> winHeaders;

  // Pinned overlay state: per store entry its RecentAsc row (0xFFFF when the
  // book is not in the index), and the current-direction rows to skip, sorted
  // ascending, so unpinned entries map to sort rows with a <=10-step walk.
  uint16_t pinnedAscRows[RecentBooksStore::MAX_RECENT_BOOKS] = {};
  uint16_t overlapRows[RecentBooksStore::MAX_RECENT_BOOKS] = {};
  uint8_t pinnedTotal = 0;
  uint8_t overlapCount = 0;
  // What the last resolvePinned() was computed against: the store's books
  // (by path hash) and the exact index they were matched in. Every screen that
  // returns here re-enters onEnter, and the scan behind those rows is the one
  // thing on this screen that has to walk the whole record section.
  bool pinnedCacheValid = false;
  bool pinnedCacheOpen = false;
  uint32_t pinnedCacheGeneration = 0;
  uint32_t pinnedCacheSize = 0;
  uint16_t pinnedCacheBooks = 0;
  uint8_t pinnedCacheCount = 0;
  uint64_t pinnedPathHashes[RecentBooksStore::MAX_RECENT_BOOKS] = {};

  bool lockNextConfirmRelease = false;
  bool lockNextBackRelease = false;

  // Row options modal (Recent long-press menu); owned here so it outlives the
  // touch event that opened it.
  OptionPopup optionPopup;
  // Backs the defer banner's c_str; the render task may be mid-frame when the
  // next status is built.
  std::string indexStatusLine;
};
