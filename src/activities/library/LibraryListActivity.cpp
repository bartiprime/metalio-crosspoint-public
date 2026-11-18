#include "LibraryListActivity.h"

#include <BoardConfig.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/settings/LibraryIndexActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UIScale.h"
#include "components/TitledHeaderBand.h"
#include "components/UITheme.h"
#include "components/icons/headerIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/search32.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr int SIDE_PADDING = 12;
constexpr unsigned long LONG_PRESS_MS = 1000;

constexpr int RECENT_TAB = 0;
constexpr int TITLE_TAB = 1;
constexpr int AUTHOR_TAB = 2;
constexpr int SERIES_TAB = 3;
constexpr int TAB_SLOTS = SERIES_TAB + 1;

constexpr bool isDescending(const library::SortOrder order) {
  return order == library::SortOrder::RecentDesc || order == library::SortOrder::TitleDesc ||
         order == library::SortOrder::AuthorDesc || order == library::SortOrder::SeriesDesc;
}

constexpr bool isRecentSort(const library::SortOrder order) {
  return order == library::SortOrder::RecentAsc || order == library::SortOrder::RecentDesc;
}

constexpr bool isAuthorSort(const library::SortOrder order) {
  return order == library::SortOrder::AuthorAsc || order == library::SortOrder::AuthorDesc;
}

constexpr bool isSeriesSort(const library::SortOrder order) {
  return order == library::SortOrder::SeriesAsc || order == library::SortOrder::SeriesDesc;
}

constexpr library::SortOrder orderForTab(const int tab, const uint8_t descendingTabs) {
  const bool descending = (descendingTabs & (1u << tab)) != 0;
  if (tab == TITLE_TAB) return descending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  if (tab == AUTHOR_TAB) return descending ? library::SortOrder::AuthorDesc : library::SortOrder::AuthorAsc;
  if (tab == SERIES_TAB) return descending ? library::SortOrder::SeriesDesc : library::SortOrder::SeriesAsc;
  return descending ? library::SortOrder::RecentDesc : library::SortOrder::RecentAsc;
}

const char* tabLabelFor(const int tab) {
  if (tab == TITLE_TAB) return tr(STR_LIBRARY_TAB_TITLE);
  if (tab == AUTHOR_TAB) return tr(STR_LIBRARY_TAB_AUTHOR);
  if (tab == SERIES_TAB) return tr(STR_LIBRARY_TAB_SERIES);
  return tr(STR_LIBRARY_TAB_RECENT);
}

}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Library", renderer, mappedInput, true) {
  // Four short tab labels: a full-slot pill would stretch across a quarter of
  // the screen, so cap it at the label plus padding (slots stay put).
  tabPillMaxPad = 16;
}

void LibraryListActivity::onEnter() {
  // One lock across the base lifecycle AND the data phase: the base onEnter
  // schedules a paint, and the render task must not read the index or the
  // filter before they are in place. The rebuild also needs the lock: the
  // render task's SD-loaded fonts read glyph data at draw time, and the walk
  // needs the card to itself.
  RenderLock lock(*this);
  UiTabListActivity::onEnter();
  app.on(ACTION_SEARCH, &LibraryListActivity::searchActionTrampoline, this);
  app.on(ACTION_REBUILD, &LibraryListActivity::rebuildActionTrampoline, this);
  app.on(ACTION_BACK, &LibraryListActivity::backActionTrampoline, this);

  // Recent is backed by the resident store. Prune before opening the index so
  // its persistence write never overlaps the long-lived index reader.
  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();

  // Rebuild when the index is missing, invalid, or was built with the other
  // metadata mode. Otherwise entering the screen stays instant.
  const bool readMetadata = SETTINGS.libraryUseMetadata != 0;
  const bool rebuildNeeded = library::isLibraryIndexDirty() || !index.open(library::libraryIndexPath()) ||
                             index.header().metadataEnabled != readMetadata;
  if (rebuildNeeded) {
    // Deferred, not done: see promptIndexBuild. The shelf opens with whatever
    // index exists (possibly none) and the banner offers the walk.
    index.close();
    promptIndexBuild();
  }
  degraded = index.isOpen() && index.ranksDegraded();
  if (index.isOpen() && index.dedupDegraded()) {
    LOG_ERR("LIB", "index was built without duplicate detection");
  }
  resolvePinned();
  // LIB7: re-entered on the Series tab (a child returned): the bucket map belongs
  // to the index that is open NOW, so it is rebuilt if it went stale.
  ensureSeriesIndex();

  // Entered while Confirm was still held (typical when launched from the home
  // menu): ignore its release, or we would open whatever sits at row 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate(true);
}

void LibraryListActivity::onExit() {
  index.close();
  Activity::onExit();
}

bool LibraryListActivity::rebuildIndex() {
  library::BuildStats stats;
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
  if (!ok) {
    LOG_ERR("LIB", "index build failed");
    return false;
  }
  LOG_INF("LIB", "reconciled: %u unchanged, %u added, %u renamed, %u removed, %u enriched (%u dup, %u unreadable)",
          static_cast<unsigned>(stats.unchanged), static_cast<unsigned>(stats.added),
          static_cast<unsigned>(stats.renamed), static_cast<unsigned>(stats.removed),
          static_cast<unsigned>(stats.enriched), static_cast<unsigned>(stats.duplicatesDropped),
          static_cast<unsigned>(stats.unreadableSkipped));
  if (stats.dedupDegraded) LOG_ERR("LIB", "rebuild completed without duplicate detection");
  return true;
}

// The banner shown when the index is missing, stale, or was built for the other
// metadata mode. Start spends the walk now; Later leaves the shelf exactly as it
// is, which is the whole point of a deferrable build.
void LibraryListActivity::promptIndexBuild() {
  library::IndexSummary summary;
  library::PartialSummary part;
  char line[80];
  if (library::readLibraryPartialSummary(part) && part.total > 0) {
    const unsigned counts[] = {static_cast<unsigned>(part.records), static_cast<unsigned>(part.total)};
    i18nFormatUnsigned(line, sizeof(line), tr(STR_LIBRARY_INDEX_PAUSED_FMT), counts, 2);
  } else if (library::readLibraryPartialSummary(part)) {
    const unsigned counts[] = {static_cast<unsigned>(part.records)};
    i18nFormatUnsigned(line, sizeof(line), tr(STR_LIBRARY_INDEX_PAUSED_UNKNOWN_FMT), counts, 1);
  } else if (library::readLibraryIndexSummary(summary)) {
    const unsigned counts[] = {static_cast<unsigned>(summary.books)};
    i18nFormatUnsigned(line, sizeof(line), tr(STR_LIBRARY_INDEX_COUNT_FMT), counts, 1);
  } else {
    std::snprintf(line, sizeof(line), "%s", tr(STR_LIBRARY_INDEX_NONE));
  }
  indexStatusLine = line;
  const char* options[] = {tr(STR_START), tr(STR_LATER)};
  optionPopup.show(tr(STR_LIBRARY_NEEDS_INDEXING), indexStatusLine.c_str(), options, 2, 0, [this](const int choice) {
    if (choice != 0) {
      requestUpdate();
      return;
    }
    runIndexBuild();
  });
  requestUpdate();
}

// Hand the walk to its own screen. The build owns the card while it runs, so
// this screen closes its index reader first and reopens it when the child
// returns -- the same discipline every other sub-activity here follows.
void LibraryListActivity::runIndexBuild() {
  if (index.isOpen()) index.close();
  auto activity = makeUniqueNoThrow<LibraryIndexActivity>(renderer, mappedInput, /*startImmediately=*/true);
  if (!activity) {
    LOG_ERR("LIB", "OOM: library index activity");
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) {
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
    degraded = index.isOpen() && index.ranksDegraded();
    resetAfterRebuild();
    requestUpdate(true);
  });
}

void LibraryListActivity::swallowHeldReleases() {
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
}

int LibraryListActivity::selectedEntry() const {
  const int entry = ringPos() - 1;
  return entry < 0 ? 0 : entry;
}

// The pinned overlay applies only to the shelf that reads as "what am I up
// to": the unfiltered Recent sort, newest first. A search result is a flat
// list the reader narrowed down on purpose, and the ascending toggle asks for
// oldest-first, which pinned fresh reads would contradict.
int LibraryListActivity::pinnedCount() const {
  if (activeTabIndex != RECENT_TAB || !query.empty() || !isDescending(sortOrder)) return 0;
  return pinnedTotal;
}

void LibraryListActivity::resolvePinned() {
  const auto& books = RECENT_BOOKS.getBooks();
  const int total = static_cast<int>(std::min<size_t>(books.size(), RecentBooksStore::MAX_RECENT_BOOKS));

  // Everything the match depends on, compared before any I/O: the store's own
  // books, and the index they were matched in. A rebuild bumps the header's
  // generation (and usually its size and count), so a stale map cannot survive
  // one; a delete or rename is a rebuild; a store edit changes the hashes.
  const bool open = index.isOpen();
  const uint32_t generation = open ? library::clixGeneration(index.header()) : 0;
  bool same = pinnedCacheValid && pinnedCacheOpen == open && pinnedCacheGeneration == generation &&
              pinnedCacheCount == total;
  if (same && open) same = pinnedCacheSize == index.header().selfSize && pinnedCacheBooks == index.bookCount();
  for (int i = 0; i < total && same; i++) {
    if (pinnedPathHashes[i] != library::clixPathHash(books[static_cast<size_t>(i)].path.data(),
                                                     books[static_cast<size_t>(i)].path.size()))
      same = false;
  }
  if (same) {
    // Re-entering this screen (a child returned) with the same store and the same
    // index: the rows are already right, and re-deriving them would walk the
    // record section again under RenderLock for no change.
    refreshOverlap();
    return;
  }
  pinnedCacheValid = false;

  pinnedTotal = static_cast<uint8_t>(total);
  for (int i = 0; i < pinnedTotal; i++) pinnedAscRows[i] = 0xFFFF;
  for (int i = 0; i < total; i++) {
    const std::string& path = books[static_cast<size_t>(i)].path;
    pinnedPathHashes[i] = library::clixPathHash(path.data(), path.size());
  }
  if (pinnedTotal > 0 && index.isOpen()) {
    library::BookIdentity identities[RecentBooksStore::MAX_RECENT_BOOKS];
    for (int i = 0; i < pinnedTotal; i++) {
      const std::string& path = books[static_cast<size_t>(i)].path;
      identities[i].pathHash = pinnedPathHashes[i];
      // Size is only a lookup prefilter; 0 (stat failed, e.g. the index handle
      // is the card's one open reader) falls back to hash-only matching.
      identities[i].fileSize = 0;
      HalFile file;
      if (Storage.openFileForRead("LIB", path.c_str(), file)) {
        identities[i].fileSize = static_cast<uint32_t>(file.fileSize());
      }
    }
    if (!index.recentRowsFor(identities, pinnedTotal, pinnedAscRows)) {
      // Without the match the overlay would duplicate every pinned book that is
      // also in the index; better to drop the pins than to show doubles.
      LOG_ERR("LIB", "recent-book lookup failed; overlay disabled");
      pinnedTotal = 0;
    }
  }
  pinnedCacheOpen = open;
  pinnedCacheGeneration = generation;
  pinnedCacheSize = open ? index.header().selfSize : 0;
  pinnedCacheBooks = open ? index.bookCount() : 0;
  pinnedCacheCount = static_cast<uint8_t>(total);
  pinnedCacheValid = true;
  refreshOverlap();
}

void LibraryListActivity::refreshOverlap() {
  overlapCount = 0;
  const int total = static_cast<int>(index.bookCount());
  for (int i = 0; i < pinnedTotal; i++) {
    if (pinnedAscRows[i] == 0xFFFF || pinnedAscRows[i] >= total) continue;
    const uint16_t row =
        isDescending(sortOrder) ? static_cast<uint16_t>(total - 1 - pinnedAscRows[i]) : pinnedAscRows[i];
    overlapRows[overlapCount++] = row;
  }
  std::sort(overlapRows, overlapRows + overlapCount);
}

void LibraryListActivity::openSelectedBook() {
  std::string path;
  if (selectedEntry() < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (selectedEntry() >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(selectedEntry())].path;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(selectedEntry())));
    if (ordinal == 0xFFFF) return;

    library::ClixRecord record{};
    if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
      LOG_ERR("LIB", "cannot resolve path for row %d", selectedEntry());
      return;
    }
  }
  openBookByPath(path);
}

// Shared by row activation and the options menu: the reader screen this opens
// has its own surfaces; a lingering tap flash would gray an unrelated element
// there. The index handle is released first — on hardware only one reader can
// hold a file open at a time, and the reader is about to open files of its own.
void LibraryListActivity::openBookByPath(const std::string& path) {
  app.clearTapFlash();
  index.close();
  onSelectBook(path);
}

void LibraryListActivity::activateIndex(const int index) {
  // LIB7: on the Series tab a row IS a series, so activating it opens that
  // series' books. The inline collapse/expand stays for Title and Author.
  // LIB9 (r101.12): not while DRILLED -- there the rows are the series' books,
  // and activating one must open the book. Treating an entry as a bucket index
  // re-entered enterSeries() with a BOOK row: entry 0 reopened the same series
  // and any other entry silently did nothing, so no drilled book could be opened.
  if (seriesIndexActive() && !seriesDrill) {
    enterSeries(index);
    return;
  }
  if (groupsCollapsed) {
    expandGroup(index);
  } else {
    openSelectedBook();
  }
}

// Row long-press prompts delete wherever grouping does not own the gesture:
// an active search is already a flat list the reader narrowed down on purpose
// ("find it, hold it, delete it"). Unfiltered Title/Author lists keep
// collapse-to-groups. The Recent shelf always opens the row options menu.
// LIB7: the series bucket list is an INDEX, not books -- there is nothing to
// delete there, so the gesture is the same as a tap (open the series).
bool LibraryListActivity::deleteEligible() const {
  return !groupsCollapsed && !seriesIndexActive() && (!query.empty() || !groupable());
}

void LibraryListActivity::onRowLongPress(const int index) {
  if (isRecentSort(sortOrder)) {
    showRecentBookOptions(index);
  } else if (deleteEligible()) {
    promptDeleteBook(index);
  } else if (!groupsCollapsed && groupable()) {
    collapseGroups(index);
  } else {
    activateIndex(index);
  }
}

// Recent-shelf long-press menu (button hold and touch long-press). The first
// rows may come from RecentBooksStore; the rest are index rows sorted by
// modification time. Only store rows can be removed from recents.
void LibraryListActivity::showRecentBookOptions(const int entry) {
  if (entry < 0 || entry >= listCount()) return;

  std::string path;
  std::string title;
  const bool isStoreRow = entry < pinnedCount();
  if (isStoreRow) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(entry)].path;
    title = books[static_cast<size_t>(entry)].title;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
    library::ClixRecord record{};
    std::string author;
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path) ||
        !rowTextFor(entry, title, author)) {
      LOG_ERR("LIB", "cannot resolve Recent row %d", entry);
      return;
    }
  }

  const char* STORE_OPTIONS[] = {tr(STR_OPEN), tr(STR_REMOVE_FROM_RECENTS), tr(STR_DELETE), tr(STR_LIBRARY_REBUILD)};
  const char* INDEX_OPTIONS[] = {tr(STR_OPEN), tr(STR_DELETE), tr(STR_LIBRARY_REBUILD)};
  app.clearTapFlash();
  optionPopup.show(tr(STR_LIBRARY), title.c_str(), isStoreRow ? STORE_OPTIONS : INDEX_OPTIONS, isStoreRow ? 4 : 3, 0,
                   [this, path, title, isStoreRow](const int choice) {
                     swallowHeldReleases();
                     switch (choice) {
                       case 0:
                         openBookByPath(path);
                         break;
                       case 1:
                         if (isStoreRow) {
                           promptRemoveRecentBook(path, title);
                         } else {
                           promptDeleteBookByPath(path, title);
                         }
                         break;
                       case 2:
                         if (isStoreRow)
                           promptDeleteBookByPath(path, title);
                         else
                           promptRebuildIndex();
                         break;
                       case 3:
                         if (isStoreRow) promptRebuildIndex();
                         break;
                       default:
                         break;
                     }
                   });
  requestUpdate();
}

#if FREEINK_CAP_TOUCH
void LibraryListActivity::swipeDelete(const int entry) {
  activeNav().selected = entry + 1;
  if (isRecentSort(sortOrder) && entry < pinnedCount()) {
    const auto& book = RECENT_BOOKS.getBooks()[static_cast<size_t>(entry)];
    promptDeleteBookByPath(book.path, book.title);
  } else {
    promptDeleteBook(entry);
  }
}
#endif

// Manual index refresh, same card discipline as the onEnter rebuild: the walk
// wants the card to itself, and the render task must not read the index (or
// the filter) around it.
void LibraryListActivity::promptRebuildIndex() {
  // The manual path is the same screen the banner starts: one implementation of
  // "walk the card", so a fix to the stepper lands on both.
  runIndexBuild();
}

void LibraryListActivity::resetAfterRebuild() {
  // A reindex or a card swap invalidates every search RESULT — they are positions
  // in the old sort order — but never the query itself: the reader typed it, and
  // re-running it against the new index is what they meant. The one case that
  // DOES cancel a live search is an index that cannot be opened at all: showing
  // "no results" for a shelf that is not there would be a lie.
  if (!query.empty() && !index.isOpen()) {
    query.clear();
    headerSearchTitle.clear();
  }
  // Sort positions, group starts, and pinned rows all point into the old order.
  applyFilter();
  resolvePinned();
  // applyFilter() dropped the bucket map with the old sort; the Series tab needs
  // it back before its next frame measures a viewport.
  ensureSeriesIndex();
  auto& nav = activeNav();
  const int count = listCount();
  if (count == 0) {
    nav.selected = 0;
  } else if (nav.selected > count) {
    nav.selected = count;
  }
  nav.followOnBuild = true;
}

void LibraryListActivity::promptRemoveRecentBook(const std::string& path, const std::string& title) {
  const bool reopenIndex = index.isOpen();
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: recent removal confirmation");
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path, reopenIndex](const ActivityResult& result) {
    swallowHeldReleases();
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    if (!result.isCancelled && RECENT_BOOKS.removeByPath(path)) {
      resolvePinned();
      closeRouting();
      auto& nav = activeNav();
      const int count = listCount();
      if (count == 0) {
        nav.selected = 0;
      } else if (nav.selected > count) {
        nav.selected = count;
      }
      nav.followOnBuild = true;
    }
  });
}

void LibraryListActivity::promptDeleteBook(const int entry) {
  if (!index.isOpen() || entry < 0 || entry >= bookRowCount()) return;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  if (ordinal == 0xFFFF) return;

  std::string path;
  library::ClixRecord record{};
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", entry);
    return;
  }
  std::string title;
  std::string author;
  rowTextFor(entry, title, author);
  promptDeleteBookByPath(path, title);
}

void LibraryListActivity::promptDeleteBookByPath(const std::string& path, const std::string& title) {
  // The dialog and the delete both want the card; reopen when we resume.
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_ITEM_CONFIRM), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: delete confirmation");
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path](const ActivityResult& result) {
    swallowHeldReleases();
    {
      // Same lock rationale as onEnter: the walk wants the card to itself, and
      // the render task must not read the index (or the filter) around the
      // rebuild.
      RenderLock lock(*this);
      if (!result.isCancelled) {
        LOG_DBG("LIB", "deleting %s", path.c_str());
        if (!removeBookFile(path)) LOG_ERR("LIB", "cannot delete %s", path.c_str());
        if (RECENT_BOOKS.removeByPath(path)) RECENT_BOOKS.saveToFile();
        GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
        rebuildIndex();
      }
      if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
      if (!result.isCancelled) {
        resetAfterRebuild();
      }
    }
    if (!result.isCancelled) {
      closeRouting();
      requestUpdate(true);
    }
  });
}

void LibraryListActivity::openSearch() {
  app.clearTapFlash();
  // No key filtering here on purpose. Greying out the letters that lead nowhere
  // was built, tested on device and removed: a letter you can see but cannot
  // reach reads as a broken keyboard, and the eye keeps returning to it.
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBRARY_SEARCH), query, 48,
                                                           InputType::Text);
  if (!keyboard) {
    LOG_ERR("LIB", "OOM: search keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    swallowHeldReleases();
    if (result.isCancelled) return;
    query = std::get<KeyboardResult>(result.data).text;
    applyFilter();
    auto& nav = activeNav();
    if (!query.empty() && filteredCount == 0 && !degraded) {
      // Up from the tab bar reopens Search even with no results.
      nav.selected = 0;
    } else {
      // A non-empty result belongs to the list: land on
      // its first surviving row, not on the strip.
      nav.selected = 1;
    }
    nav.top = 0;
    requestUpdate();
  });
}

void LibraryListActivity::stepTab(const int direction) {
  const int next = (activeTab() + (direction > 0 ? 1 : TAB_SLOTS - 1)) % TAB_SLOTS;
  selectTab(next, false);
}

void LibraryListActivity::onTabAction(const int index) {
  app.clearTapFlash();
  selectTab(index, true);
}

void LibraryListActivity::selectTab(const int index, const bool toggleIfActive) {
#if FREEINK_CAP_TOUCH
  closeSwipeDelete();
#endif
  if (index < 0 || index >= TAB_SLOTS) return;
  if (toggleIfActive && index == activeTab()) descendingTabs ^= static_cast<uint8_t>(1u << index);
  sortOrder = orderForTab(index, descendingTabs);
  // The filter and the overlap rows hold positions in the old order, so they
  // must be rebuilt.
  applyFilter();
  activeTabIndex = index;
  refreshOverlap();
  // LIB7: entering the Series tab lists the SERIES, so its bucket map is built
  // here (loop task, once) rather than in listCount(), which the render task
  // calls and which must stay free of index walks.
  ensureSeriesIndex();
  // Tab changes happen only while the bar owns focus. A tab's remembered row
  // must not pull focus back into the list after the switch.
  auto& nav = activeNav();
  nav.selected = 0;
  nav.top = 0;
  requestUpdate();
}

void LibraryListActivity::toggleSortDirection() { selectTab(activeTab(), true); }

int LibraryListActivity::tabCount() const { return TAB_SLOTS; }

int LibraryListActivity::activeTab() const { return activeTabIndex; }

const char* LibraryListActivity::tabLabel(const int index) const { return tabLabelFor(index); }

fui::TabIndicator LibraryListActivity::tabIndicator(const int index) const {
  if (index != activeTab()) return fui::TabIndicator::None;
  return isDescending(sortOrder) ? fui::TabIndicator::Down : fui::TabIndicator::Up;
}

int LibraryListActivity::bookRowCount() const {
  if (!query.empty()) return static_cast<int>(filteredCount);
  // Pinned books already in the index are skipped below the pins, not doubled;
  // pinned books the index missed still show, so the difference stays split.
  const int pinned = pinnedCount();
  return static_cast<int>(index.bookCount()) + (pinned > 0 ? pinned - overlapCount : 0);
}

// LIB7: three views live behind one count. The Series tab is a list of SERIES
// (its rows are buckets, not books), a drilled series is the slice of the Series
// sort that bucket covers, and every other tab is the old list.
bool LibraryListActivity::seriesIndexActive() const {
  return activeTabIndex == SERIES_TAB && !degraded && query.empty();
}

int LibraryListActivity::listCount() const {
  if (seriesDrill) return static_cast<int>(seriesDrillCount);
  if (seriesIndexActive()) return static_cast<int>(seriesIndexCount);
  return groupsCollapsed ? static_cast<int>(groupCount) : bookRowCount();
}

// Entry position on screen to row position in the sort order. Identity while
// unfiltered and unpinned, so the shelf costs nothing when nothing is typed.
// With pins active, entries below pinnedCount() belong to the store and must
// not reach this; the rest walk past the pinned books' own sort rows.
int LibraryListActivity::rowFor(const int entry) const {
  if (seriesDrill) {
    // The drill list IS a contiguous slice of the Series sort: entry 0 is the
    // bucket's first row, so this is one addition and nothing is re-read.
    if (entry < 0 || entry >= static_cast<int>(seriesDrillCount)) return seriesDrillStart;
    return seriesDrillStart + entry;
  }
  if (!query.empty()) {
    if (entry < 0 || entry >= static_cast<int>(filteredCount) || !filtered) return 0;
    return filtered[entry];
  }
  const int pinned = pinnedCount();
  if (pinned == 0) return entry;
  int row = entry - pinned;
  for (int i = 0; i < overlapCount; i++) {
    if (overlapRows[i] <= row) row++;
  }
  return row;
}

// LIB7: the Series tab's own view is the bucket list, so the collapse gesture has
// nothing to collapse there and a drilled series is already a filtered list.
bool LibraryListActivity::groupable() const {
  return !degraded && !isRecentSort(sortOrder) && !seriesIndexActive() && !seriesDrill && bookRowCount() > 0;
}

uint32_t LibraryListActivity::titleInitialFor(const int entry) {
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) return 0;
  return library::foldedGroupInitial(std::string_view(record.fold, record.foldLen));
}

// One record, one blob field. Only ever called for rows about to be drawn (or for
// one pass while a group map is built), never for the whole library at once.
bool LibraryListActivity::seriesFor(const int entry, std::string& series) {
  return seriesForRow(rowFor(entry), series);
}

// LIB7: the same read, addressed by SORT ROW. The bucket list holds rows, not
// entries, and seriesFor() would map them a second time.
bool LibraryListActivity::seriesForRow(const int row, std::string& series) {
  series.clear();
  // The pinned Recent rows come from RecentBooksStore, which stores no series.
  if (row < pinnedCount() || !index.isOpen()) return false;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(row));
  library::ClixRecord record{};
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) return false;
  return index.readSeries(record, series);
}

bool LibraryListActivity::buildGroupStarts() {
  const int count = bookRowCount();
  if (count <= 0) return false;
  if (groupCapacity < count) {
    auto starts = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(count));
    if (!starts) {
      LOG_ERR("LIB", "cannot allocate %u-byte group map", static_cast<unsigned>(count * sizeof(uint16_t)));
      return false;
    }
    groupStarts = std::move(starts);
    groupCapacity = static_cast<uint16_t>(count);
  }

  groupCount = 0;
  uint32_t previousInitial = 0;
  std::string previousAuthor;
  std::string previousSeries;
  std::string title;
  std::string author;
  std::string series;
  std::string blob;
  std::string_view field;
  const std::string noKey;
  previousAuthor.reserve(128);
  previousSeries.reserve(96);
  title.reserve(128);
  author.reserve(128);
  series.reserve(96);
  blob.reserve(512);

  // One decision per entry, whichever way the key was obtained.
  const auto consider = [&](const int entry, const std::string& keyText, const uint32_t keyInitial) {
    bool startsGroup = entry == 0;
    if (isAuthorSort(sortOrder)) {
      startsGroup = startsGroup || keyText != previousAuthor;
      previousAuthor = keyText;
    } else if (isSeriesSort(sortOrder)) {
      // One bucket per declared series, and exactly one for everything that
      // declared none. The empty name is never compared as if it were a series.
      startsGroup = startsGroup || keyText != previousSeries;
      previousSeries = keyText;
    } else {
      startsGroup = startsGroup || keyInitial != previousInitial;
      previousInitial = keyInitial;
    }
    if (startsGroup) groupStarts[groupCount++] = static_cast<uint16_t>(entry);
  };

  constexpr uint16_t BATCH = 16;
  auto records = makeUniqueNoThrow<library::ClixRecord[]>(BATCH);
  if (!records) {
    LOG_ERR("LIB", "cannot allocate %u-byte group chunk",
            static_cast<unsigned>(BATCH * sizeof(library::ClixRecord)));
    return false;
  }
  uint16_t ordinals[BATCH];

  // The key for every row is already in the index: the title's group initial is
  // in the record's own title fold, and the author/series group keys come out of
  // the record's name blob. Reading 16 rows at a time instead of walking four
  // blob fields per book is what turns a group map build from an SD-bound
  // gesture into a short one. Any read failure falls back to the per-row path
  // below, so a damaged card still groups exactly as it did before.
  //
  // `byInitial` is every order that groups on the title fold -- title order and
  // anything else that is not an author or series sort. Title order is also the
  // record section itself, so only there is a batch of records one read.
  const bool byInitial = !isAuthorSort(sortOrder) && !isSeriesSort(sortOrder);
  const bool titleOrder = sortOrder == library::SortOrder::TitleAsc || sortOrder == library::SortOrder::TitleDesc;
  for (int base = 0; base < count; base += BATCH) {
    const uint16_t n = static_cast<uint16_t>(std::min<int>(BATCH, count - base));
    const bool haveOrdinals = index.readOrdinals(sortOrder, static_cast<uint16_t>(base), ordinals, n);
    bool fast = haveOrdinals;

    if (fast && titleOrder) {
      // Title order is the record section, ascending or descending: read the
      // run once and index it back to front for the descending case.
      const bool descending = sortOrder == library::SortOrder::TitleDesc;
      const uint16_t recordBase = descending ? ordinals[n - 1] : ordinals[0];
      fast = index.readRecords(recordBase, records.get(), n);
    }

    for (uint16_t i = 0; i < n && fast; i++) {
      const int entry = base + i;
      const uint16_t ordinal = ordinals[i];
      if (ordinal == 0xFFFF) {
        fast = false;
        break;
      }
      if (titleOrder) {
        const bool descending = sortOrder == library::SortOrder::TitleDesc;
        const library::ClixRecord& record = records[descending ? (n - 1 - i) : i];
        // The key text is not consulted for a title-fold group.
        consider(entry, noKey, library::foldedGroupInitial(std::string_view(record.fold, record.foldLen)));
        continue;
      }
      library::ClixRecord& record = records[0];
      if (!index.readRecord(ordinal, record)) {
        fast = false;
        break;
      }
      if (byInitial) {
        consider(entry, noKey, library::foldedGroupInitial(std::string_view(record.fold, record.foldLen)));
        continue;
      }
      if (!index.readBlob(record, blob)) {
        fast = false;
        break;
      }
      std::string& key = isAuthorSort(sortOrder) ? author : series;
      const uint8_t which = isAuthorSort(sortOrder) ? library::CLIX_BLOB_AUTHOR : library::CLIX_BLOB_SERIES;
      if (library::clixBlobField(blob, record.nameLen, which, field))
        key.assign(field);
      else
        key.clear();
      consider(entry, key, 0);
    }
    if (fast) continue;

    // Fallback: exactly the per-row reads this function used before the batch,
    // so the group map cannot differ when a read fails mid-build.
    for (uint16_t i = 0; i < n; i++) {
      const int entry = base + i;
      if (isAuthorSort(sortOrder)) {
        rowTextFor(entry, title, author);
        consider(entry, author, 0);
      } else if (isSeriesSort(sortOrder)) {
        seriesFor(entry, series);
        consider(entry, series, 0);
      } else {
        consider(entry, author, titleInitialFor(entry));
      }
    }
  }
  LOG_DBG("LIB", "group map: %u groups, %u bytes", static_cast<unsigned>(groupCount),
          static_cast<unsigned>(groupCapacity * sizeof(uint16_t)));
  return groupCount > 0;
}

int LibraryListActivity::groupForBook(const int bookEntry) const {
  int group = 0;
  while (group + 1 < groupCount && groupStarts[group + 1] <= bookEntry) group++;
  return group;
}

// LIB7: the bucket list is exactly the Series sort's group map -- one start per
// declared series in name order, plus one for the books that declare none. Built
// once per entry to the tab (and after any rebuild), because buildGroupStarts()
// reads one record per book, which is not something the render task may do while
// the viewport is being measured.
void LibraryListActivity::ensureSeriesIndex() {
  if (!seriesIndexActive()) {
    seriesIndexCount = 0;
    return;
  }
  if (seriesMapReady) return;
  seriesIndexCount = 0;
  if (!index.isOpen() || bookRowCount() <= 0 || !buildGroupStarts()) {
    seriesMapReady = false;
    return;
  }
  seriesIndexCount = groupCount;
  seriesMapReady = true;
  LOG_DBG("LIB", "series index: %u buckets over %d books", static_cast<unsigned>(seriesIndexCount), bookRowCount());
}

// The bucket `entry` owns the rows [start, next start); the last bucket owns the
// rest of the sort, which is what makes the counts add up to the library.
int LibraryListActivity::seriesBucketEnd(const int entry) const {
  if (entry < 0 || entry >= static_cast<int>(groupCount)) return 0;
  return entry + 1 < static_cast<int>(groupCount) ? static_cast<int>(groupStarts[entry + 1]) : bookRowCount();
}

// LIB7: open one series. Nothing is filtered or copied -- the drill list is the
// bucket's own contiguous slice of the Series sort -- so this costs one string
// read for the header and no allocation proportional to the series.
void LibraryListActivity::enterSeries(const int entry) {
  if (!seriesMapReady || entry < 0 || entry >= static_cast<int>(groupCount)) return;
  const int start = static_cast<int>(groupStarts[entry]);
  const int end = seriesBucketEnd(entry);
  if (end <= start) return;
  std::string name;
  if (!seriesForRow(start, name)) name.clear();
  seriesDrillName = name.empty() ? std::string(tr(STR_LIBRARY_STANDALONE)) : name;
  seriesIndexNav = activeNav();
  seriesDrillStart = static_cast<uint16_t>(start);
  seriesDrillCount = static_cast<uint16_t>(end - start);
  seriesDrill = true;
  auto& nav = activeNav();
  nav.reset(1);
  nav.followOnBuild = true;
  LOG_DBG("LIB", "series drill: %s rows %u..%u", seriesDrillName.c_str(), static_cast<unsigned>(seriesDrillStart),
          static_cast<unsigned>(seriesDrillStart + seriesDrillCount));
  requestUpdate(true);
}

// LIB7: Back from a series returns to the bucket list AT THE ROW IT WAS LEFT ON:
// the index's own nav was parked in enterSeries().
void LibraryListActivity::exitSeries() {
  if (!seriesDrill) return;
  seriesDrill = false;
  seriesDrillStart = 0;
  seriesDrillCount = 0;
  seriesDrillName.clear();
  activeNav() = seriesIndexNav;
  requestUpdate(true);
}

bool LibraryListActivity::collapseGroups(const int bookEntry) {
  if (!groupable() || !buildGroupStarts()) return false;
  // The map now describes the TITLE/AUTHOR sort, not the Series sort.
  seriesMapReady = false;
  expandedNav = activeNav();
  groupsCollapsed = true;
  auto& nav = activeNav();
  nav.reset(groupForBook(bookEntry) + 1);
  requestUpdate();
  return true;
}

void LibraryListActivity::expandGroup(const int groupEntry) {
  if (!groupsCollapsed || groupEntry < 0 || groupEntry >= groupCount) return;
  const int bookEntry = groupStarts[groupEntry];
  groupsCollapsed = false;
  activeNav() = expandedNav;
  auto& nav = activeNav();
  nav.selected = bookEntry + 1;
  nav.top = bookEntry;
  nav.followOnBuild = true;
  requestUpdate();
}

void LibraryListActivity::restoreExpandedList() {
  if (!groupsCollapsed) return;
  groupsCollapsed = false;
  activeNav() = expandedNav;
  requestUpdate();
}

// One pass over the sort order, keeping what matches, ranked.
//
// WHAT IS SEARCHED: title, author, series and filename — four keys the build
// already folded and stored, so this pass calls fold() exactly ONCE, on the
// query. The old one folded every non-matching row's author, which at 1,204 books
// was the worst case it could hit.
//
// HOW A HIT IS JUDGED: a plain substring test on folded text (library::
// foldedContains). Not the shelf's word-prefix rule.
//
// HOW HITS ARE ORDERED: prefix-of-title > title > series > author > filename,
// then shelf order within a rank. That ordering is library::rankSearchMatch's,
// tested on its own; this function only buckets.
//
// The result array is allocated once with the exact upper bound and fails back to
// an explicit message rather than letting vector growth abort the firmware.
void LibraryListActivity::applyFilter() {
  groupsCollapsed = false;
  groupCount = 0;
  // LIB7: the bucket list and any drill are positions in the sort order that is
  // about to change, so both are dropped here -- one place, for every caller.
  seriesDrill = false;
  seriesDrillStart = 0;
  seriesDrillCount = 0;
  seriesDrillName.clear();
  seriesIndexCount = 0;
  seriesMapReady = false;
  filtered.reset();
  filteredCount = 0;
  filterFailed = false;
  // The header shows the active query in place of the screen title, so the
  // reader can see what narrowed the list without reopening the keyboard.
  headerSearchTitle = query.empty() ? std::string() : "“" + query + "”";
  if (query.empty()) return;

  const int total = static_cast<int>(index.bookCount());
  if (total <= 0) return;

  auto matches = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(total));
  auto ranks = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(total));
  // 16 records fill a 4096-byte buffer exactly, and one 32-byte read of a stored
  // permutation yields 16 rows: the pass below therefore costs ~2 reads per 16
  // books instead of two seeks per book. Same records, same four keys, same
  // rankSearchMatch() -- only the way the bytes are fetched changed.
  constexpr uint16_t BATCH = 16;
  auto records = makeUniqueNoThrow<library::ClixRecord[]>(BATCH);
  if (!matches || !ranks || !records) {
    LOG_ERR("LIB", "cannot allocate %u-byte search buffers", static_cast<unsigned>(total * 3));
    filterFailed = true;
    return;
  }

  const std::string needle = library::fold(query);
  const auto rankOf = [&needle](const library::ClixRecord& record) {
    const library::SearchKeys keys{
        std::string_view(record.fold, record.foldLen),
        std::string_view(record.authorFold, record.authorFoldLen),
        std::string_view(record.seriesFold, record.seriesFoldLen),
        std::string_view(record.nameFold, record.nameFoldLen),
    };
    return static_cast<uint8_t>(library::rankSearchMatch(keys, needle));
  };

  // Pass A, in ORDINAL order: the record section is contiguous by ordinal, so
  // each batch is one read. `ranks` holds the rank per ordinal, not per row.
  uint16_t perRank[7] = {};
  for (uint16_t base = 0; base < total; base = static_cast<uint16_t>(base + BATCH)) {
    const uint16_t n = std::min<uint16_t>(BATCH, static_cast<uint16_t>(total - base));
    if (index.readRecords(base, records.get(), n)) {
      for (uint16_t i = 0; i < n; i++) {
        ranks[base + i] = rankOf(records[i]);
        perRank[ranks[base + i]]++;
      }
      continue;
    }
    // One unreadable record must not zero its neighbours, so this batch falls
    // back to the per-record read the row loop used to do.
    for (uint16_t i = 0; i < n; i++) {
      library::ClixRecord one{};
      ranks[base + i] = index.readRecord(static_cast<uint16_t>(base + i), one) ? rankOf(one) : 0;
      perRank[ranks[base + i]]++;
    }
  }

  // Bucket offsets, best rank first: the array IS the display order, so no second
  // pass over the card is needed to sort it.
  uint16_t cursor[7] = {};
  uint16_t next = 0;
  for (int rank = static_cast<int>(library::MatchRank::TitlePrefix); rank >= 1; rank--) {
    cursor[rank] = next;
    next = static_cast<uint16_t>(next + perRank[rank]);
  }
  // Pass B, in ROW order: rows ascend within a rank, exactly as before, but the
  // row -> ordinal permutation arrives 16 rows at a time.
  uint16_t ordinals[BATCH];
  for (uint16_t base = 0; base < total; base = static_cast<uint16_t>(base + BATCH)) {
    const uint16_t n = std::min<uint16_t>(BATCH, static_cast<uint16_t>(total - base));
    const bool resolved = index.readOrdinals(sortOrder, base, ordinals, n);
    for (uint16_t i = 0; i < n; i++) {
      const uint16_t ordinal = resolved ? ordinals[i] : index.ordinalForRow(sortOrder, static_cast<uint16_t>(base + i));
      if (ordinal == 0xFFFF) continue;
      const uint8_t rank = ranks[ordinal];
      if (rank == 0) continue;
      matches[cursor[rank]++] = static_cast<uint16_t>(base + i);
    }
  }

  filtered = std::move(matches);
  filteredCount = next;
}

// Staged back-out, shared by the Back button and the header's back arrow:
// clear the search, expand collapsed groups, return focus to the tabs, then
// leave for home.
void LibraryListActivity::handleBackAction() {
  auto& nav = activeNav();
  if (!query.empty()) {
    query.clear();
    applyFilter();
    nav.selected = 0;
    nav.top = 0;
    requestUpdate();
  } else if (seriesDrill) {
    // LIB7: one level out of a series is the bucket list, at the row it was
    // left on -- not Home.
    exitSeries();
  } else if (groupsCollapsed) {
    restoreExpandedList();
  } else {
    // r88: no invisible "return focus to the tabs" stage before this. It looked
    // like a dead arrow: the one visible change was the cursor jumping to the
    // top row, so the operator had to tap the back control two or three times
    // to leave the Library at all. A back control that is not visibly staging
    // something (a search, collapsed groups) returns ONE LEVEL -- and this list
    // is opened as a root screen, so one level out of it is Home.
    onGoHome();
  }
}

void LibraryListActivity::searchActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->openSearch();
}

void LibraryListActivity::backActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->handleBackAction();
}

void LibraryListActivity::rebuildActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->promptRebuildIndex();
}

// Title and author for one entry, read straight from the index. Only ever
// called for rows about to be drawn, so at most a screenful of strings exists
// at once.
bool LibraryListActivity::rowTextFor(const int entry, std::string& title, std::string& author, std::string* fileName) {
  title.clear();
  author.clear();
  if (fileName) fileName->clear();
  if (entry < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry < 0 || entry >= static_cast<int>(books.size())) return false;
    const auto& book = books[static_cast<size_t>(entry)];
    title = book.title;
    author = book.author;
    if (fileName) *fileName = book.path;
    return true;
  }
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal != 0xFFFF && index.readRecord(ordinal, record)) {
    // The build already decided both fields — from the book's own metadata when
    // it has any, and with one spelling chosen per author across the library.
    // Re-parsing the name here would throw that away, and only works while the
    // name still looks like "Title - Author".
    //
    // Author, title and filename are three slices of ONE blob, so they cost one
    // read rather than a walk from the section start per field.
    std::string blob;
    if (index.readBlob(record, blob) && blob.size() >= record.nameLen) {
      std::string_view field;
      if (library::clixBlobField(blob, record.nameLen, library::CLIX_BLOB_AUTHOR, field)) author.assign(field);
      if (library::clixBlobField(blob, record.nameLen, library::CLIX_BLOB_TITLE, field) && !field.empty())
        title.assign(field);
      const std::string_view name(blob.data(), record.nameLen);
      if (title.empty()) title.assign(name);
      if (fileName) fileName->assign(name);
    } else {
      if (!index.readAuthor(record, author)) author.clear();
      if (!index.readTitle(record, title) || title.empty()) index.readName(record, title);
      if (fileName) index.readName(record, *fileName);
    }
  }
  if (title.empty()) title = tr(STR_LIBRARY_UNKNOWN_TITLE);
  return true;
}

bool LibraryListActivity::handleCustomInput() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;

  if (lockNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockNextConfirmRelease = false;
    return true;
  }
  if (lockNextBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockNextBackRelease = false;
    return true;
  }

  return false;
}

bool LibraryListActivity::handleButtons() {
  const int count = listCount();

  // Every hold action fires at the threshold, mid-hold, including the ones
  // that open a dialog (remove-recent, delete). The release that follows is
  // armed as suppressed by wasLongPressed() and consumed globally by
  // ActivityManager::loop() before any activity runs, so it cannot land in
  // the freshly opened confirmation and select its default.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    if (tabsFocused()) {
      if (!degraded) toggleSortDirection();
    } else if (isRecentSort(sortOrder)) {
      showRecentBookOptions(selectedEntry());
    } else if (deleteEligible()) {
      if (count > 0) promptDeleteBook(selectedEntry());
    } else if (!groupsCollapsed && groupable()) {
      collapseGroups(selectedEntry());
    } else {
      activateIndex(selectedEntry());
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    handleBackAction();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (tabsFocused()) {
      stepTab(1);
      return true;
    }
    if (count > 0) activateIndex(selectedEntry());
    return true;
  }

  return false;
}

void LibraryListActivity::navigateButtons() {
  const int count = listCount();
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) ||
      mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    navigationStartedOnTabs = tabsFocused();
  }
  buttonNavigator.onNextPress([this, count] {
    if (count > 0) moveRingTo(ringPos() == count ? 1 : ringPos() + 1);
  });
  buttonNavigator.onPreviousPress([this, count] {
    if ((!navigationStartedOnTabs || degraded) && count > 0) {
      moveRingTo(ringPos() <= 1 ? count : ringPos() - 1);
    }
  });
  // Search is an activation: defer it so holding Previous can still step tabs.
  buttonNavigator.onPreviousRelease([this] {
    if (navigationStartedOnTabs && tabsFocused() && !degraded) openSearch();
  });
  // A held button steps tabs while the strip has focus (the base behaviour
  // Settings keeps) and page-jumps once the selection is down in the rows,
  // where fast travel through a long shelf is what a hold means.
  buttonNavigator.onNextContinuous([this, count] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::nextPageIndex(selectedEntry(), count, activeNav().pageRows()) + 1);
    }
  });
  buttonNavigator.onPreviousContinuous([this, count] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(-1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::previousPageIndex(selectedEntry(), count, activeNav().pageRows()) + 1);
    }
  });
}

void LibraryListActivity::buildRows(UiScreen& screen) {
  auto& nav = activeNav();
  const int count = listCount();
  // LIB7: on the Series tab the rows are BUCKETS, so neither the alphabet
  // headings nor the inline series headings apply -- the bucket list is itself the
  // grouping, one level up.
  // LIB9 (r101.12): the bucket branch below reads groupStarts[entry] -- a GROUP
  // index. A drilled series is NOT the bucket list: its entries are BOOK rows in
  // the Series sort (rowFor() = seriesDrillStart + entry), while
  // seriesIndexActive() is still true for it (same tab, no query). Leaving it on
  // painted the bucket table against a book count -- entry 0/1 reprinted the bucket
  // rows ("MegafunBooks, N books" / "Standalone, N books") and entry 2..count-1
  // indexed groupStarts PAST groupCount, i.e. uninitialised uint16s read as series
  // names and negative book counts: "strings taken from elsewhere".
  const bool seriesIndex = seriesIndexActive() && !seriesDrill;
  const bool authorGrouped = !seriesIndex && isAuthorSort(sortOrder);
  const bool seriesGrouped = !seriesIndex && !seriesDrill && isSeriesSort(sortOrder);
  // A drilled series is one series' books: no alphabet headings either, they would
  // interleave letters into a two-book list.
  const bool grouped = !seriesIndex && !seriesDrill && !isRecentSort(sortOrder);
  // LIB10: this build is the series drill with the shrinking face live (the
  // settings row is the kill-switch and restores the fixed one-line face).
  const bool drillNamed = drillFileNames();

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  // Breathing room between rows; the dense theme default packs the two-line
  // rows edge-to-edge.
  props.rowGap = std::max<int16_t>(screen.theme().listRowGap, 6);
  props.headerUnderline = false;
  syncTabListViewport(screen, props);

  // Keep one extra entry in the reusable window for a clipped trailing row.
  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1) + 1;
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  if (!groupsCollapsed && winHeaders.size() < cap) winHeaders.resize(cap);
  winItems.clear();
  if (winItems.capacity() < cap) winItems.reserve(cap);

  int rows = 0;
  int headers = 0;
  uint32_t previousInitial = 0;
  // LIB7: the bucket row's "N books" line. One buffer for the whole window.
  char countBuffer[48] = {};
  std::string rowFile;
  rowFile.reserve(128);
  // The series heading is a whole name, not an initial, so the previous value has
  // to be kept as a string across the window.
  std::string previousSeries;
  std::string series;
  previousSeries.reserve(96);
  series.reserve(96);
  // Capture this after syncTabListViewport(), which may clamp nav.top.
  const int windowStart = static_cast<int>(props.topIndex);
  for (int entry = windowStart; entry < count && rows < static_cast<int>(cap); entry++) {
    std::string& title = winTitles[static_cast<size_t>(rows)];
    std::string& author = winAuthors[static_cast<size_t>(rows)];
    fui::ListItem item;
    if (seriesIndex) {
      // LIB7: one row per series -- its name and how many books it holds -- plus
      // the single Standalone row LIB1 already decides for the books that declare
      // no series. The count is the bucket's distance to the next start, so the
      // rows add up to the shelf by construction.
      const int bucketStart = groupStarts[entry];
      const int bucketEnd = seriesBucketEnd(entry);
      seriesForRow(bucketStart, title);
      if (title.empty()) title = tr(STR_LIBRARY_STANDALONE);
      const unsigned bucketBooks[] = {static_cast<unsigned>(bucketEnd - bucketStart)};
      i18nFormatUnsigned(countBuffer, sizeof(countBuffer), tr(STR_LIBRARY_INDEX_COUNT_FMT), bucketBooks, 1);
      author = countBuffer;
    } else if (groupsCollapsed) {
      const int bookEntry = groupStarts[entry];
      if (authorGrouped) {
        rowTextFor(bookEntry, title, author);
        formatAuthorHeading(author, title);
      } else if (seriesGrouped) {
        seriesFor(bookEntry, title);
        // An empty series is one visible bucket, not an unnamed group.
        if (title.empty()) title = tr(STR_LIBRARY_STANDALONE);
      } else {
        formatInitialHeading(titleInitialFor(bookEntry), title);
      }
    } else {
      if (!rowTextFor(entry, title, author, &rowFile)) continue;
      // LIB10: inside a series the row names the BOOK, by its FILE NAME. The
      // metadata title is what the operator could not read (it is not the name
      // the file has on the card, and the distinguishing part of his names --
      // "… Chapter 7201-7300.epub" -- is at the END). The author line goes with
      // it: every book in one series carries the same author, and the line it
      // costs is what pushed these rows past the theme's pitch.
      if (drillNamed && !rowFile.empty()) {
        title = rowFile;
        author.clear();
      }
      uint32_t initial = 0;
      bool startsGroup = false;
      if (authorGrouped) {
        startsGroup = rows == 0 || author != winAuthors[static_cast<size_t>(rows - 1)];
      } else if (seriesGrouped) {
        seriesFor(entry, series);
        startsGroup = rows == 0 || series != previousSeries;
        previousSeries = series;
      } else if (grouped) {
        initial = titleInitialFor(entry);
        startsGroup = rows == 0 || initial != previousInitial;
        previousInitial = initial;
      }
      if (startsGroup) {
        std::string& heading = winHeaders[static_cast<size_t>(headers++)];
        if (authorGrouped)
          formatAuthorHeading(author, heading);
        else if (seriesGrouped)
          heading = series.empty() ? std::string(tr(STR_LIBRARY_STANDALONE)) : series;
        else
          formatInitialHeading(initial, heading);
        item.sectionHeading = heading.c_str();
      }
      if (!authorGrouped && !author.empty()) item.subtitle = author.c_str();
    }

    item.label = title.c_str();
    // Group headings stay bare; every book row gets its file-type icon.
    if (!groupsCollapsed && !rowFile.empty()) item.icon = listIconFor(UITheme::getFileIcon(rowFile), 32);
    item.actionValue = static_cast<int16_t>(entry);
    winItems.push_back(item);
    rows++;
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
  // r99.9 (request D): the window only exists now, so this is the first moment
  // the shared row-face fit can see these titles. drawListWindow() binds the
  // largest tier at which every visible title fits its row on one line and then
  // draws -- the same call the chapter and clipping lists use, so no windowed list
  // can keep the preferred face (and its ellipsis) by forgetting the step.
  //
  // EXCEPT THE SERIES DRILL (LIB10). Its fit is not a choice to be made per
  // window: the whole point of the fixed-point fix is that the drill reads ONE
  // face for every row, every pass. drawListWindow() would walk the ladder
  // against the labels in THIS window and pick a different rung for a different
  // window -- the oscillation. So the drill binds the dense face itself and calls
  // list() directly, with the layout left single-line (maxLines 1) so the row
  // height is exactly props.rowHeight for every row; drawDrillFileNames() then
  // paints the second line inside that same box.
#if FREEINK_CAP_TOUCH
  configureSwipeDelete(screen, props);
#endif
  const int drillFaceId = drillNamed ? drillFace() : 0;
  if (drillFaceId != 0) {
    props.labelText.maxLines = 1;
    uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, drillFaceId);
    screen.list(props);
    drawDrillFileNames(screen, props, drillFaceId);
  } else {
    drawListWindow(screen, props);
  }
  const int next = nav.drawnRows;
  const auto body = screen.body();
  LOG_DBG("LIB", "page tab=%d top=%d full=%d loaded=%d body=%d..%d next=%d title=%s", activeTabIndex, windowStart,
          nav.drawnRows, rows, body.y, body.bottom(),
          next < rows ? winItems[static_cast<size_t>(next)].actionValue : -1,
          next < rows ? winItems[static_cast<size_t>(next)].label : "<none>");
  LOG_DBG("LIB", "page first=%d title=%s", rows > 0 ? winItems[0].actionValue : -1,
          rows > 0 ? winItems[0].label : "<none>");
}

// --- LIB10: the series drill's ONE face, two lines --------------------------
//
// The operator's rule, in his words: *"Fixed Filename Size is what i want the
// format to be but in two lines word wrapped, but right now its single line."*
// So the row is set at the theme's dense file-name face (Andro: Ember 18, the
// rung the file browser uses) and wrapped to two lines -- inside the row height
// the list already has.
//
// THE FIRST CUT PICKED A FACE PER ROW, and it could not converge. drillFaceFor()
// measured each row's own name against the theme's ladder, so the row's face --
// and with it the line height, the wrapped line count, and therefore the row
// height once a name needed a third line -- depended on WHICH ROWS WERE IN THE
// WINDOW. Different window -> different face -> different row count -> the next
// window is different again. On the operator's 17-book series it oscillated
// between "9 rows, 2 pages" and "8 rows, 3 pages" forever, and every pass drove
// an EQUALIZE_BLACK repaint. measureRowSetPageRows() was reporting the
// oscillation faithfully; the layout was the unstable thing.
//
// A layout whose result depends on its own input set is not a fixed point.
// One face for the whole drill is: the face is constant, so the line height is
// constant, so every row is ListProps::rowHeight tall, so
// measureRowSetPageRows() prices a uniform set and pages = ceil(count / rows)
// is stable across passes. Nothing below depends on the text except which words
// fall on which of the two lines.
bool LibraryListActivity::drillFileNames() const {
  // Kill-switch: Settings > System > "Fixed Filename Size". It reads 0 = this
  // item's format (dense fixed face, wrapped to two lines) and 1 = the behaviour
  // this view had before it (one line at the list's own face). The polarity is
  // unchanged from the commit that introduced the row, so a device that never
  // touched the setting keeps the drill format -- what CHANGED is the format
  // itself: one fixed face over two lines, not a per-row shrinking ladder.
  return seriesDrill && SETTINGS.libraryFixedRowFace == 0;
}

int LibraryListActivity::drillFace() {
  const int face = UITheme::getInstance().getTheme().listRowDenseFontId();
  if (face == 0 || !uiFaceLoaded(renderer, face)) return 0;
  return face;
}

int LibraryListActivity::drillLineCap(const int16_t boxH) {
  const int face = drillFace();
  if (face == 0 || boxH <= 0) return 0;
  const int lineH = renderer.getLineHeight(face);
  if (lineH <= 0) return 0;
  // Two lines only when the box genuinely holds two line boxes. Both numbers
  // come from the theme and the row pitch -- never from a name and never from
  // the window -- so every row in every pass gets the same answer. On this panel
  // Ember 18's line box is 45 px and the row pitch is 92 px: 2 * 45 = 90 <= 92,
  // so two lines fit with 2 px to spare inside an unchanged row.
  return (lineH * 2 <= boxH) ? 2 : 1;
}

// The names themselves. list() has just laid the rows out at ONE face (the
// dense face, bound before list() from the same theme read this takes) and
// drawn each name on one line, ellipsised at the row edge. This walk repaints
// each row's label box with the SAME face, wrapped to two lines, clipped to the
// box list() gave the row.
//
// Why a second pass at all: the SDK's row measure counts the wrapped lines of a
// label without honouring maxLines, so letting list() wrap would grow the row
// the moment a name needed a third line -- which is exactly the non-uniform
// layout that oscillated. list() therefore lays out single-line (every row is
// exactly props.rowHeight, uniform, a fixed point) and the two lines are painted
// inside that same box here. Nothing below changes a row's height.
//
// Everything below is derived from the props list() actually drew with: the row
// band, the row inset, the scroll strip it reserves, the icon box and the
// measured row heights. Nothing here invents a second geometry.
void LibraryListActivity::drawDrillFileNames(UiScreen& screen, fui::ListProps& props, const int face) {
  auto& nav = activeNav();
  const int drawn = nav.drawnRows;
  const int count = listCount();
  if (drawn <= 0 || props.items == nullptr || props.rowHeight <= 0 || face == 0) return;
  // A theme that marks the selection with a marker or an underline inside the
  // row would have that ink erased by the repaint below. This board's theme
  // paints no marker (touch has no focus cursor), so the repaint is safe; a
  // theme that does keeps the widget's own label rather than losing its marker.
  if (props.selectionMarker != fui::SelectionMarker::None) return;

  // LIB10 / U-4: THE PAGER MUST COUNT PAGES IT ACTUALLY HAS. With one uniform
  // pitch the tiling is exact by construction, which is what makes the shared
  // pager's count equal to the pages drawn; this check says so out loud, because
  // the failure it guards (a row on no page at all) is invisible until the
  // operator notices a book missing.
  {
    const int pages = pagerPageCount();
    const int lastStart = pagerPageStart(pages > 0 ? pages - 1 : 0);
    if (pages > 1 && lastStart + drawn < count) {
      LOG_ERR("LIB", "LIB10 pager tiling lost rows: count=%d pages=%d lastStart=%d drawn=%d", count, pages, lastStart,
              drawn);
    }
  }

  const fui::Rect body = screen.body();
  const int16_t gap = props.rowGap > 0 ? props.rowGap : 0;
  const int16_t sidePad = props.sidePadding < 0 ? 8 : props.sidePadding;
  const int16_t rowInset = props.rowInset < 0 ? 0 : props.rowInset;
  int16_t bandX = static_cast<int16_t>(body.x + rowInset);
  int16_t bandW = static_cast<int16_t>(body.width - rowInset * 2);
  // ui::list() reserves its scroll track's strip from EVERY nav-managed list,
  // whether or not this page overflows, so the rows are this much narrower.
  const int16_t scrollW = props.scrollIndicatorWidth < 0 ? 3 : props.scrollIndicatorWidth;
  const int16_t scrollInset = props.scrollIndicatorInset < 0 ? 0 : props.scrollIndicatorInset;
  if (props.scrollIndicator && rowInset < static_cast<int16_t>(scrollW + scrollInset + 2)) {
    const int16_t cut = static_cast<int16_t>(scrollW + scrollInset + 2 - rowInset);
    bandW = static_cast<int16_t>(bandW - cut);
    if (props.scrollIndicatorSide == 1) bandX = static_cast<int16_t>(bandX + cut);
  }

  auto& target = screen.frame().target();
  fui::StyleSet styles = props.rowStyles;
  if (props.rowRadius > 0) fui::setStyleRadius(styles, props.rowRadius);

  // Pass 1 -- the boxes. Measured while FONT_BODY is still bound to the face
  // list() drew with, so every height here is the height on the glass and the y
  // walk cannot drift from the painted rows. Bounded by the materialized window:
  // props.items is one page of a list whose count is the whole series.
  int16_t boxX[kDrillRowCap];
  int16_t boxY[kDrillRowCap];
  int16_t boxW[kDrillRowCap];
  int16_t boxH[kDrillRowCap];
  const int windowRows = props.itemsWindowCount > 0 ? static_cast<int>(props.itemsWindowCount) : drawn;
  const int laidOut = drawn < windowRows ? drawn : windowRows;
  int rows = 0;
  int cursorY = body.y;
  for (int i = 0; i < laidOut && rows < kDrillRowCap; ++i) {
    const fui::ListItem& item = props.items[i];
    const fui::ListRowLayout layout = fui::measureListRow(target, screen.frame().assets(), bandW, props, item);
    const int16_t iconSize = item.icon ? (props.iconSize > 0 ? props.iconSize : item.icon.width) : 0;
    const int16_t iconAdvance = iconSize > 0 ? static_cast<int16_t>(iconSize + props.textGap) : 0;
    boxX[rows] = static_cast<int16_t>(bandX + sidePad + iconAdvance);
    boxW[rows] = static_cast<int16_t>(bandW - sidePad * 2 - iconAdvance);
    boxY[rows] = static_cast<int16_t>(cursorY);
    boxH[rows] = layout.height;
    cursorY = static_cast<int16_t>(cursorY + layout.height + gap);
    ++rows;
  }

  // Pass 2 -- the ink. ONE face for every row (the parameter), and the line cap
  // decided by the row BOX alone, so a row cannot be painted in a shape the
  // layout did not price.
  int painted = 0;
  int twoLine = 0;
  for (int i = 0; i < rows; ++i) {
    const fui::ListItem& item = props.items[i];
    if (item.label == nullptr || item.label[0] == '\0' || boxW[i] <= 0 || boxH[i] <= 0) continue;
    const int lines = drillLineCap(boxH[i]);
    if (lines <= 0) continue;
    // A name whose codepoints the dense face does not carry (a Hebrew or CJK
    // file name in an Ember rung) is drawn at the UI body face instead of as
    // holes. That is a per-name ink decision and it cannot move a row: the box,
    // the cap and the row height were all fixed by the layout above.
    const int rowFace = uiLabelFitsFace(renderer, face, item.label) ? face : uiScaleSpec().bodyFontId;
    if (!uiFaceLoaded(renderer, rowFace)) continue;
    // The style list() painted this row with, in the state it is in THIS frame: a
    // tap flash inverts the row for one frame and the label has to invert with
    // it, or the flash shows a white hole in a black bar.
    const fui::BoxStyle& rowStyle = styles.resolve(screen.frame().stateFor(props.action, item.actionValue, item.state));
    const fui::Rect box{boxX[i], boxY[i], boxW[i], boxH[i]};
    // Erase what list() drew (the same name, one line, ellipsised). A transparent
    // row background IS paper, which is what white restores.
    const fui::Paint erase =
        rowStyle.background.kind == fui::PaintKind::None ? fui::Paint::solid(fui::Color::White) : rowStyle.background;
    target.fill(box, erase, rowStyle.radius, rowStyle.corners);
    fui::TextStyle text = fui::textStyleWithForeground(props.labelText, rowStyle.foreground);
    text.maxLines = static_cast<uint8_t>(lines);
    text.align = fui::TextAlign::Left;
    uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, rowFace);
    target.text(box, item.label, text);
    ++painted;
    if (lines >= 2) ++twoLine;
  }
  // Restore the face list() drew with. A slot left on the body fallback would be
  // the same leak BAND1/LABEL1/B-P1-2 were: whatever composes next reads this
  // slot.
  uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, face);
  LOG_DBG("LIB", "LIB10 drill: rows=%d painted=%d twoLine=%d face=%d", rows, painted, twoLine, face);
}

void LibraryListActivity::formatInitialHeading(uint32_t initial, std::string& out) {
  out.clear();
  if (initial == 0) {
    out.push_back('#');
    return;
  }
  if (initial >= 'a' && initial <= 'z') initial -= 'a' - 'A';
  utf8AppendCodepoint(initial, out);
}

void LibraryListActivity::formatAuthorHeading(const std::string& author, std::string& out) const {
  out = author.empty() ? std::string(tr(STR_LIBRARY_UNKNOWN_AUTHOR)) : author;
  if (author.empty()) return;
  const size_t lastSpace = out.find_last_of(' ');
  if (lastSpace != std::string::npos && lastSpace + 1 < out.size()) {
    out = out.substr(lastSpace + 1) + ", " + out.substr(0, lastSpace);
  }
}

void LibraryListActivity::buildHeader(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = UITheme::getInstance().getTheme();
  fui::HeaderProps header;
  // r77: the band, the title face + coverage guard, the leading arrow's size and
  // mask, the status cluster (or its removal on a theme without one) and the
  // theme's rule all come from the SHARED band implementation -- the same one
  // BaseTheme::drawHeader uses. This screen composes its own props only because
  // it carries two trailing actions (search, rebuild).
  header.minTouchSize = screen.theme().minTouchSize;
  header.trailingStyles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
  // r88: this screen's arrow is the SHARED band back control, not a private
  // action. It used to route its own ACTION_BACK, which meant (a) the band
  // cleared HeaderBackTapTarget, so the arrow could not take part in the central
  // back-control hold -> Home rule every other list gets, and (b) its handler
  // could stage the tap into invisibility (see handleBackAction). Registering
  // the rect instead gives the arrow the same two meanings as everywhere else:
  // tap = one level through Button::Back, hold = Home.
  if (!degraded) {
    // Keep both touch actions together on the right; button boards reach
    // rebuild through the row options menu.
    header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
    header.trailingAction = ACTION_SEARCH;
    if (mappedInput.hasTouch()) {
      header.trailingAdjacentIcon = fui::bitmapFromIcon(icon_refresh_cw_32);
      header.trailingAdjacentAction = ACTION_REBUILD;
    }
    // Vertical placement comes from applyHeaderStatus: buttons center on the
    // unified band.
  }
  const auto frameRect = screen.frame().screen();
  // Header and tabs share a screen-relative boundary, independent of bezel
  // insets: the band starts at the screen's own top padding, and its height is
  // the theme's titled band (doubled on Andro) -- the same number
  // listHeaderBandHeight() hands the content margin below.
  const Rect bandRect{static_cast<int>(frameRect.x), metrics.topPadding, static_cast<int>(frameRect.width),
                      listHeaderBandHeight()};
  titledHeaderBand::drawTitledHeaderBand(theme, screen.frame(), renderer, bandRect, headerTitle(), nullptr, header,
                                         /*backButton=*/mappedInput.hasTouch());
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The position readout owns the line above the hints; rows must not overlap
  // it.
  constexpr int READOUT_GAP = 4;  // the list's fold sits just above the readout's line box
  const int16_t readoutReserved = static_cast<int16_t>(renderer.getLineHeight(SMALL_FONT_ID) + READOUT_GAP);
  const Rect area = UITheme::getContentArea(renderer);
  const auto bottomInset = static_cast<int16_t>(renderer.getScreenHeight() - area.y - area.height);
  buildHeader(screen);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + listHeaderBandHeight()), 0,
                  static_cast<int16_t>(bottomInset + metrics.buttonHintsHeight + readoutReserved), 0});

  if (!degraded) buildTabBar(screen);
  if (bookRowCount() == 0) {
    const char* message = tr(STR_LIBRARY_NO_RESULTS);
    if (filterFailed) {
      message = tr(STR_LIBRARY_SEARCH_UNAVAILABLE);
    } else if (query.empty()) {
      message = tr(STR_LIBRARY_EMPTY);
    }
    screen.centeredText(message);
    return;
  }
  buildRows(screen);
}

// "12/69 books" at the bottom right: which book is selected, out of how many.
//
// NOT a page count. How many rows fit varies with the view (author headings
// consume band height), so a page total grows and shrinks as you scroll. The
// book position is stable by construction, and it answers the question the
// reader actually has: how far in am I, and how much is left.
void LibraryListActivity::drawPositionReadout() const {
  const int count = listCount();
  if (count <= 0) return;

  char buf[32];
  const char* positionFormat = groupsCollapsed ? tr(STR_LIBRARY_GROUP_POSITION) : tr(STR_LIBRARY_POSITION);
  snprintf(buf, sizeof(buf), positionFormat, selectedEntry() + 1, count);
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The position line is a CAPTION, so it is drawn in the theme's caption face
  // (Andro: Ember 12, the face its status bar and dialog readouts use) instead of
  // the 8 pt UI small face -- guarded per string, because the Ember ids have no
  // registered fallback. Its line box, not the small face's, positions the line.
  const int captionFace = UITheme::getInstance().getTheme().captionFontId();
  const int face = captionFace != 0 && BaseTheme::textFitsFace(renderer, captionFace, buf) ? captionFace : SMALL_FONT_ID;
  const int lineH = renderer.getLineHeight(face);
  const int width = renderer.getTextWidth(face, buf);
  // Bottom-right of the viewable area, clear of the bezel and rounded corner.
  const Rect area = UITheme::getContentArea(renderer);
  const int right = std::max(renderer.getScreenWidth() - area.x - area.width, int{BoardConfig::ACTIVE.cornerRadius});
  const int x = renderer.getScreenWidth() - right - width - SIDE_PADDING;
  const int y = area.y + area.height - metrics.buttonHintsHeight - lineH;
  renderer.drawText(face, x, y, buf, true);
}

const char* LibraryListActivity::headerTitle() const {
  if (!headerSearchTitle.empty()) return headerSearchTitle.c_str();
  // LIB7: a drilled series says which one, so the filter is visible in the band.
  if (seriesDrill) return seriesDrillName.c_str();
  return degraded ? tr(STR_LIBRARY_TITLE_UNSORTED) : tr(STR_LIBRARY);
}

void LibraryListActivity::drawHoldHelp() const {
  if (mappedInput.hasTouch() || groupsCollapsed) return;
  const char* help = nullptr;
  if (tabsFocused() && !degraded)
    help = tr(STR_LIBRARY_HOLD_SORT);
  else if (!tabsFocused() && isRecentSort(sortOrder) && listCount() > 0)
    help = tr(STR_LIBRARY_HOLD_OPTIONS);  // recent rows: hold opens the row menu
  else if (!tabsFocused() && deleteEligible() && listCount() > 0)
    help = tr(STR_HOLD_OPEN_TO_DELETE);
  else if (!tabsFocused() && groupable())
    help = tr(STR_LIBRARY_HOLD_GROUPS);
  if (!help) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const Rect area = UITheme::getContentArea(renderer);
  const int left = std::max(area.x, int{BoardConfig::ACTIVE.cornerRadius});
  const int y = area.y + area.height - metrics.buttonHintsHeight - lineHeight;
  GUI.drawHelpText(renderer,
                   Rect{left + SIDE_PADDING, y, renderer.getScreenWidth() / 2 - left - SIDE_PADDING, lineHeight}, help);
}

// OptionPopup is a self-contained modal: it owns the whole frame (hints
// included) whenever it is up, mirroring the FileBrowser pattern.
void LibraryListActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiTabListActivity::render(std::move(lock));
}

void LibraryListActivity::drawFooter() {
  drawPositionReadout();
  drawHoldHelp();

  const bool backGoesHome = tabsFocused() && !groupsCollapsed && !seriesDrill && query.empty();
  const char* backLabel = backGoesHome ? tr(STR_HOME) : tr(STR_BACK);
  const char* confirmLabel = groupsCollapsed ? tr(STR_SELECT) : tr(STR_OPEN);
  const bool canSearch = tabsFocused() && !degraded;
  const auto labels = mappedInput.mapLabels(backLabel, tabsFocused() ? tr(STR_TOGGLE) : confirmLabel,
                                            canSearch ? tr(STR_SEARCH) : tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
