#include "HomeActivity.h"

#include <Bitmap.h>
#include <BoardConfig.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Memory.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "HapticFeedback.h"
#include "audio/UiSounds.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "platform/PlatformFeatures.h"

namespace {

// E5 (HARDWARE_POWER_PLAN §5.4): one line only — BaseTheme::drawPopup() draws a
// single string and truncates anything wider than the panel.
static constexpr const char* kTouchDegradedNotice = "Touch unavailable: POWER restart may restore";

// The ONE home-menu geometry recipe. render() paints with this band and loop()
// hit-tests the MenuLayout derived from it, so the painted rows and the touch
// rows cannot diverge: r59's density scale drifted them apart because each side
// recomputed the band (and the row pitch) on its own.
Rect homeMenuBand(const GfxRenderer& renderer, const ThemeMetrics& metrics, const Rect& content) {
  const int pageHeight = renderer.getScreenHeight();
  // The band starts at homeTopPadding + homeCoverTileHeight + homeMenuTopOffset,
  // so its height must subtract exactly those three terms and nothing else. The
  // pre-r94 form also subtracted headerHeight, verticalSpacing and
  // buttonHintsHeight, which neither position the band nor bound its bottom: on
  // the Plus that put the rect 433..1490 on a 1216 px panel (1057 tall, 274 px
  // past the bottom). Nothing painted off-panel only because every painter reads
  // band.y and its own pitch; a full-band fill or a `fits`/tap test against
  // band.height would have overrun. This makes the rect agree with the layout.
  return Rect{content.x, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset,
              content.width, pageHeight - (metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset)};
}

// Rows the classic home menu actually paints: the four base entries (File
// Browser, Library, File transfer, Settings), the optional library/plugins slot,
// and at most ONE Continue Reading row. Both the painter and the touch grid must
// use this count, or a tap below the last drawn row would resolve to a row that is
// not on the glass.
int homeMenuRowCount(const ThemeMetrics& metrics, const bool hasLibrarySlot, const int recentCount) {
  int rows = 4 + (hasLibrarySlot ? 1 : 0);
  if (metrics.homeContinueReadingInMenu && recentCount > 0) ++rows;
  return rows;
}

}  // namespace

// r164 THUMB1 - the cover-thumb cache name gained a version (thumb_<h> -> thumb2_<h>).
// The recent-books store PERSISTS each entry's thumbnail template, and loadRecentBooks()
// honours a non-empty one, so without this an existing card would keep resolving the v1
// name, draw the old 1 bpp file and never regenerate - exactly the mixed state the
// versioned name exists to prevent. Only reflowable EPUBs are upgraded: XTC keeps the v1
// name (its generator is unchanged) and a txt/md companion cover is written 1 bpp under
// v1 by design (Epub::getThumbBmpPath()).
void upgradeThumbTemplate(RecentBook& book) {
  if (!FsHelpers::hasEpubExtension(book.path)) return;
  constexpr std::string_view kV1 = "/thumb_[HEIGHT].bmp";
  const std::string& path = book.coverBmpPath;
  if (path.size() < kV1.size() || path.compare(path.size() - kV1.size(), kV1.size(), kV1) != 0) return;
  book.coverBmpPath = path.substr(0, path.size() - kV1.size()) + "/thumb2_[HEIGHT].bmp";
  LOG_DBG("HOME", "cover thumb template upgraded to v2: %s", book.coverBmpPath.c_str());
}

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Library, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasLibrarySlot()) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(coverGridUi ? maxBooks : std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    RecentBook entry = book;
    upgradeThumbTemplate(entry);
    recentBooks.push_back(std::move(entry));
  }
}

void HomeActivity::fillCoverGridFromLibrary() {
  if (recentBooks.size() >= CoverGridHomeUi::MAX_BOOKS) return;
  // Keep the index and record together off the task stack; reuse for every row.
  struct LibraryReader {
    library::LibraryIndexFile index;
    library::ClixRecord record;
  };
  auto reader = makeUniqueNoThrow<LibraryReader>();
  if (!reader) {
    LOG_ERR("HOME", "OOM: library index");
    return;
  }
  auto& index = reader->index;
  auto& record = reader->record;
  if (!index.open(library::libraryIndexPath())) {
    // No usable index. Do NOT build one here: this runs inside onEnter, after the
    // entry gate armed and before this activity's own requestUpdate(), so the
    // "Rebuilding" popup's drive was swallowed and the walk blocked the first
    // paint for as long as the card took. The Library screen owns the build
    // (deferrable banner, cancellable stepper); the grid stays on the recents
    // store until it lands.
    LOG_INF("HOME", "no library index: cover grid stays on recent books");
    return;
  }
  for (uint16_t row = 0; row < index.bookCount() && recentBooks.size() < CoverGridHomeUi::MAX_BOOKS; ++row) {
    RecentBook book;
    if (!index.readRecord(index.ordinalForRow(library::SortOrder::RecentDesc, row), record) ||
        !index.readPath(record, book.path))
      continue;
    // Both checks are one stat each and both can end this row: a book already
    // pinned above, or one whose file is gone. Doing them before the title and
    // author reads is what keeps a card missing half its books from paying for
    // them, and ioFailed() is checked where the read happened rather than after
    // the whole row.
    if (std::any_of(recentBooks.begin(), recentBooks.end(),
                    [&](const RecentBook& existing) { return existing.path == book.path; }) ||
        RecentBooksStore::isMissing(book))
      continue;
    if (!index.readTitle(record, book.title) && !index.readName(record, book.title)) {
      if (index.ioFailed()) break;
      continue;
    }
    index.readAuthor(record, book.author);
    if (index.ioFailed()) break;
    recentBooks.push_back(std::move(book));
  }
}

void HomeActivity::resolveGridCoverPaths() {
  for (auto& book : recentBooks) {
    if (!book.coverBmpPath.empty()) continue;
    // Constructors only derive cache paths; no metadata parsing or image generation.
    // Keep these large objects off the task stack and release each before the next book.
    if (FsHelpers::hasReflowableBookExtension(book.path)) {
      auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
      if (!epub) {
        LOG_ERR("HOME", "OOM: EPUB thumbnail path");
        continue;
      }
      book.coverBmpPath = epub->getThumbBmpPath();
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
      if (!xtc) {
        LOG_ERR("HOME", "OOM: XTC thumbnail path");
        continue;
      }
      book.coverBmpPath = xtc->getThumbBmpPath();
    }
  }
}

void HomeActivity::loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect) {
  if (!book.coverBmpPath.empty() && Storage.exists(UITheme::getCoverThumbPath(book.coverBmpPath, height).c_str()))
    return;
  // Only one parser lives at a time; EPUB/XTC objects exceed the stack budget.
  if (FsHelpers::hasReflowableBookExtension(book.path)) {
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: cover EPUB");
      return;
    }
    book.coverBmpPath = epub->getThumbBmpPath();
    if (Storage.exists(epub->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (epub->generateThumbBmpFromSource(height)) {
      return;
    }
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: cover XTC");
      return;
    }
    book.coverBmpPath = xtc->getThumbBmpPath();
    if (Storage.exists(xtc->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (xtc->load() && xtc->generateThumbBmp(height)) {
      return;
    }
  }
  book.coverBmpPath.clear();
}

void HomeActivity::loadRecentCovers(int coverHeight, uint32_t deadlineMs) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  bool complete = true;
  for (RecentBook& book : recentBooks) {
#if CP_ENTRY_SINGLE_DRIVE
    // r97.2: bounded so the entry gate can hold Home's first drive until the covers exist WITHOUT
    // letting a slow card, a big epub or a corrupt thumb keep the screen unpainted. Checked between
    // books, never inside one: the bound is "one thumbnail of overshoot", and the gate's own
    // CP_ENTRY_GATE_TIMEOUT_MS is the hard stop the panel can never outlive.
    if (deadlineMs != 0 && millis() >= deadlineMs) {
      complete = false;
      LOG_INF("HOME", "cover load hit the %d ms entry budget at %d/%zu: painting what exists, rest on next repaint",
              CP_ENTRY_GATE_TIMEOUT_MS, progress, recentBooks.size());
      break;
    }
#endif
    // The cover grid shares one slot size; generating at any other height
    // would rescale the dithered thumb at draw time and alias badly.
    const int thumbHeight = coverGridUi ? coverGridUi->thumbHeightFor() : coverHeight;
    if (coverGridUi) {
      loadGridCover(book, thumbHeight, showingLoading, popupRect);
      ++progress;
      if (showingLoading) GUI.fillPopupProgress(renderer, popupRect, progress * 100 / recentBooks.size());
      continue;
    }
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub/txt/md, try to load the metadata for title/author and cover
        if (FsHelpers::hasReflowableBookExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = epub.generateThumbBmp(thumbHeight);
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = xtc.generateThumbBmp(thumbHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = complete;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  // E5 (docs/engineering/HARDWARE_POWER_PLAN.md §5.4): the probe ladder has
  // already run (boot, or a wake). If it exhausted, the UI would otherwise just
  // render its button-only layout and say nothing — that silence is what this
  // banner exists to end. RAM-only: the next boot re-probes and may clear it.
  // A board with no touch
  // controller at all is not degraded — BoardConfig::hasTouch() is the capability,
  // mappedInput.hasTouch() is "this board's controller answered".
  touchDegraded = BoardConfig::hasTouch() && !mappedInput.hasTouch();
  if (touchDegraded) {
    LOG_ERR("HOME", "touch absent after the probe ladder: showing the degraded banner (a power restart may restore it)");
  }

  hasOpdsServers = OPDS_STORE.hasServers();
  // Plugin System is the global opt-in. Once enabled, expose the Plugins entry
  // even when no SD plugins are installed yet so Plugin Hub can be installed
  // from inside the Plugins screen (or users can sideload plugins manually).
  hasPlugins = SETTINGS.pluginsEnabled != 0;

  const auto& metrics = UITheme::getInstance().getMetrics();
  if (UITheme::getInstance().hasCoverGridHome()) {
    // Screen-lifetime interaction tables and component properties exceed the stack budget.
    coverGridUi = makeUniqueNoThrow<CoverGridHomeUi>(renderer);
    if (!coverGridUi) LOG_ERR("HOME", "OOM: cover grid UI; using standard home");
  }
  loadRecentBooks(coverGridUi ? CoverGridHomeUi::MAX_BOOKS : metrics.homeRecentBooksCount);
  hasContinueReading = !recentBooks.empty();
  if (coverGridUi) {
    fillCoverGridFromLibrary();
    resolveGridCoverPaths();
    coverGridUi->begin(recentBooks, hasLibrarySlot(), hasContinueReading);
  }

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasLibrarySlot());

#if CP_ENTRY_SINGLE_DRIVE
  // r97.2: build the frame's content while ActivityManager's entry gate still holds Home's first
  // drive. On the wake path this is what makes the armed from-white clean land on the COMPLETE
  // screen: without it the clean was spent on a cover-less frame and the repaint that carried the
  // cover arrived unarmed as a differential drive plus row 9's GL16 -- two more whole-panel drives
  // after the clean, on one screen. Bounded by CP_ENTRY_GATE_TIMEOUT_MS: whatever is not ready by
  // then paints as-is and finishes on the next natural repaint.
  if (!recentsLoaded) {
    const int entryThumbHeight = coverGridUi ? CoverGridHomeUi::THUMB_HEIGHT : GUI.homeCoverThumbHeight(renderer);
    loadRecentCovers(entryThumbHeight > 0 ? entryThumbHeight : metrics.homeCoverHeight,
                     millis() + CP_ENTRY_GATE_TIMEOUT_MS);
  }
#endif

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  coverGridUi.reset();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool touchBoard = BoardConfig::hasTouch();

  // E5: the degraded banner is one-off — the first physical input dismisses it.
  // The press itself is still handled normally below; only the flag changes.
  if (touchDegraded && (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                        mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
                        mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                        mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                        mappedInput.wasPressed(MappedInputManager::Button::Left) ||
                        mappedInput.wasPressed(MappedInputManager::Button::Right) ||
                        mappedInput.wasPressed(MappedInputManager::Button::Power))) {
    touchDegraded = false;
  }

  // Entry-time selectorIndex is 0 -- the first recent book -- and is only a highlight, never a hit.
  // While the host has not published its interaction table no tap can resolve at all, so acting on
  // anything here could only fall through to that highlight: wait for the table instead of guessing.
  if (coverGridUi && !coverGridUi->routingReady()) {
    requestUpdate();
    return;
  }
  // r49: an open routing gate is not enough -- the table must be THIS screen's. Until the publish
  // generation moves past the value stamped at entry, any snapshot was built against something
  // other than the grid the user is looking at, so an activation could only land on the
  // entry-time highlight (selectorIndex 0 = the first recent book).
  if (coverGridUi && !coverGridUi->tapsAcceptable()) {
    LOG_INF("HOME", "tap dropped: not yet published (gen=%u)",
            static_cast<unsigned>(coverGridUi->publishGeneration()));
    requestUpdate();
    return;
  }

  // AUDIO3: the Home grid and the recent list emitted nothing at all, which is
  // one of the surfaces the operator's "some taps beep, some don't" report was
  // about. One tick per MOVE that actually moves (the cover grid wraps, so a
  // press on a single-book grid changes nothing and stays silent -- the same rule
  // as UiListActivity minus the refused tone), and one select per activation.
  // activateSelection() is the one activation funnel, so a tab tap, a cover tap, a
  // menu-row tap and Confirm are all covered by the single line inside it.
  auto moveSound = [](const int from, const int to) {
    if (from != to) uisounds::onEvent(uisounds::Event::MenuMove, uisounds::Tag::Home);
  };

  auto activateSelection = [this]() {
    uisounds::onEvent(uisounds::Event::Select, uisounds::Tag::Home);
    const int bookCount = static_cast<int>(recentBooks.size());
    const bool bookBand = selectorIndex < bookCount;
    // One loud line per activation so any surviving silent activator names itself.
    LOG_INF("HOME", "activate idx=%d books=%d routed=%d branch=%s", selectorIndex, bookCount,
            coverGridUi && coverGridUi->routingReady() ? 1 : 0, bookBand ? "book" : "menu");
    if (bookBand) {
      onRecentBookSelected(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
    switch (indexToMenuItem(menuIndex, hasLibrarySlot())) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::LIBRARY:
        onLibraryOpen();
        break;
      case HomeMenuItem::OPDS_BROWSER:  // the library slot
        hasPlugins ? onPluginsOpen() : onOpdsBrowserOpen();
        break;
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      default:
        break;
    }
  };

  // Cover grid home splits navigation by button group (see below); the flat
  // next/previous cycle is for the classic list home only.
  if (!coverGridUi) {
    buttonNavigator.onNext([this, menuCount, &moveSound] {
      const int from = selectorIndex;
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      moveSound(from, selectorIndex);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, menuCount, &moveSound] {
      const int from = selectorIndex;
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      moveSound(from, selectorIndex);
      requestUpdate();
    });
  }

  // Route a tap BEFORE the swipe classifier. Both read the same release edge, and the tab row sits
  // low on the panel where a slow drag reads as a swipe: routing second meant a tap on a tab could
  // be consumed as a scroll (the press highlight repainted, nothing committed) and needed a second
  // tap. The tap result is used further down.
  int tappedTab = -1;
  const int tappedIndex = coverGridUi ? coverGridUi->selectedAction(mappedInput, &tappedTab) : -1;
  // Which action actually answered the release. A value alone cannot separate a book slot from a
  // tab slot, and that ambiguity is exactly how a tab tap used to reach the book band.
  const freeink::ui::ActionId tapAction = coverGridUi ? coverGridUi->lastActionId() : 0;
  const int tapValue = coverGridUi ? coverGridUi->lastActionValue() : -1;
  // A tab hit is handled FIRST and unconditionally: it is the one input that must never be allowed
  // to reach the book band, whatever selectorIndex happens to hold.
  if (tappedTab >= 0) {
    if (tapAction == CoverGridHomeUi::ACTION_TAB && tapValue == tappedTab) {
      LOG_INF("HOME", "tab tap slot=%d -> idx=%d", tappedTab, static_cast<int>(recentBooks.size()) + tappedTab);
      selectorIndex = static_cast<int>(recentBooks.size()) + tappedTab;
      haptic_feedback::deferTouchAction();
      activateSelection();
    }
    return;
  }
  // r45 diagnostic (OPEN_CONCERNS #13): the operator reports that tapping a MENU item on Home opens the
  // currently-reading book. That is only possible if selectedAction() resolves a tab tap to an index below
  // recentBooks.size(), so log the resolution itself: the index, the two band sizes, and which band it hit.
  // One line per resolved tap; if a tab tap logs ->BOOK, the hit-test/rect mapping is the defect.
  if (tappedIndex >= 0) {
    const int bookCount = static_cast<int>(recentBooks.size());
    LOG_INF("HOME", "tap resolved idx=%d books=%d menu=%d -> %s", tappedIndex, bookCount,
            menuCount - bookCount, tappedIndex < bookCount ? "BOOK" : "MENU");
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    const int from = selectorIndex;
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    moveSound(from, selectorIndex);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    const int from = selectorIndex;
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    moveSound(from, selectorIndex);
    requestUpdate();
    return;
  }

  // Back on the root screen deliberately does nothing. It used to open the most recent book, which
  // turned every stray Back -- a header-rect tap, an edge swipe, or a press that survived a screen
  // transition -- into "why did my book just open". See docs/R33_REFRESH_INPUT.md.

  if (coverGridUi) {
    // Path A: only a SELECT whose value IS the slot the resolver reported. A tab id, a stale
    // pending value or a chrome hit all leave tappedIndex >= 0 (or tapAction foreign), and none of
    // them may activate a book.
    if (tappedIndex >= 0 && tappedIndex < menuCount && tapAction == CoverGridHomeUi::ACTION_SELECT &&
        tapValue == tappedIndex) {
      selectorIndex = tappedIndex;
      activateSelection();
      return;
    }
    // r47: MappedInputManager::hasTouch() is NOT board capability -- it forwards to
    // InputManager::hasTouch(), i.e. `touchDataEnabled`, which is false exactly while the touch
    // controller is not producing samples. The r46 guard used it and therefore disabled itself in
    // the one window where it mattered: no tap can route (UiAppHost::routeTouch returns early while
    // uiReady is false, which is why the offending transitions carry no [UI] tap line), and the
    // entry-time selectorIndex=0 -- the first recent book -- is left as the only thing to activate.
    // Board capability is the correct predicate: this board is touch-only and has no Confirm button.
    // (The refusal that used to live in activateSelection is gone: the publish-generation gate at
    // the top of loop() is the real check.)
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Names the previously silent path: r45's probe only sees taps that resolved to an index.
      LOG_INF("HOME", "confirm-release activate idx=%d books=%d touch=%d", selectorIndex,
              static_cast<int>(recentBooks.size()), touchBoard ? 1 : 0);
      if (!touchBoard) {
        activateSelection();  // the only non-touch case: a real button press, not a highlight
      } else {
        requestUpdate();  // an unresolved touch does nothing beyond repainting
      }
      return;
    }
    // Side page buttons walk the covers, front Left/Right walk the tabs
    // (selectorIndex is flat: books first, then the tab items). A press while
    // selection sits in the other band jumps into this band first.
    const int bookCount = static_cast<int>(recentBooks.size());
    const auto cycleBand = [this, &moveSound](const int base, const int count, const int dir) {
      if (count <= 0) return;
      int idx = selectorIndex - base;
      if (idx < 0 || idx >= count) {
        idx = dir > 0 ? 0 : count - 1;
      } else {
        idx = (idx + count + dir) % count;
      }
      const int from = selectorIndex;
      selectorIndex = base + idx;
      moveSound(from, selectorIndex);
      requestUpdate();
    };
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up},
                                         [&cycleBand, bookCount] { cycleBand(0, bookCount, -1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                         [&cycleBand, bookCount] { cycleBand(0, bookCount, +1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this, &cycleBand] {
      const int bookCount = static_cast<int>(recentBooks.size());
      cycleBand(bookCount, getMenuItemCount() - bookCount, -1);
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this, &cycleBand] {
      const int bookCount = static_cast<int>(recentBooks.size());
      cycleBand(bookCount, getMenuItemCount() - bookCount, +1);
    });
    return;
  }

  // Match the rendered cover grid, which lays tiles inside getContentArea()
  // (origin content.x + contentSidePadding, span content.width). Using
  // screen-relative getScreenWidth()/contentSidePadding here offset the touch
  // targets from the drawn covers by content.x on bezel-inset panels (EEGO A4).
  const Rect content = UITheme::getInstance().getContentArea(renderer);
  const int coverColumnCount = std::max(1, metrics.homeRecentBooksCount);
  const int recentCount = std::min(static_cast<int>(recentBooks.size()), coverColumnCount);
  const int coverColumnWidth = (content.width - 2 * metrics.contentSidePadding) / coverColumnCount;
  int touchedBook = -1;
  const auto coverTouch = mappedInput.colTouch(touchedBook, content.x + metrics.contentSidePadding, coverColumnWidth,
                                               recentCount, metrics.homeTopPadding,
                                               metrics.homeTopPadding + metrics.homeCoverTileHeight, coverColumnWidth);
  if (coverTouch != MappedInputManager::RowTouch::None) {
    if (coverTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedBook) {
        selectorIndex = touchedBook;
        uisounds::onEvent(uisounds::Event::MenuMove, uisounds::Tag::Home);
        requestUpdate();
      }
    } else {
      selectorIndex = touchedBook;
      haptic_feedback::deferTouchAction();
      activateSelection();  // resolved cover-tile hit
    }
    return;
  }

  const int bookCount = static_cast<int>(recentBooks.size());
  const int paintedSelected = metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - bookCount;
  // ONE geometry source: the band render() hands to drawButtonMenu() and the
  // MenuLayout the theme builds from it. Anchor, pitch, height and the paged
  // firstIndex all come from that struct, so a tap at y resolves to the row
  // painted at y - no second arithmetic path to drift out of step.
  const MenuLayout menuLayout = GUI.getMenuLayout(renderer, homeMenuBand(renderer, metrics, content),
                                                  homeMenuRowCount(metrics, hasLibrarySlot(), bookCount),
                                                  paintedSelected);
  int menuRow = -1;
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuLayout.top, menuLayout.step, menuLayout.rows, content.x,
                                              content.x + content.width, menuLayout.rowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedRow = menuLayout.firstIndex + menuRow;
    const int touchedIndex = metrics.homeContinueReadingInMenu ? touchedRow : touchedRow + bookCount;
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        uisounds::onEvent(uisounds::Event::MenuMove, uisounds::Tag::Home);
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      haptic_feedback::deferTouchAction();
      activateSelection();  // resolved menu-row hit
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();  // classic list: a real Confirm press, not a touch highlight
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  // Content columns clear the bezel insets on rounded panels. The header
  // self-insets in drawHeader, so only the cover/menu need it here.
  const Rect content = UITheme::getInstance().getContentArea(renderer);

  renderer.clearScreen();
  if (coverGridUi) {
    coverGridUi->setSelection(selectorIndex);
    UITheme::getInstance().drawCoverGridHome(*coverGridUi);
    // Front Left/Right walk the tabs, so their hints read Left/Right; the
    // side page buttons (unhinted) walk the covers.
    const auto labels = mappedInput.mapLabels(hasContinueReading ? tr(STR_RESUME) : "", tr(STR_SELECT),
                                              tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    // E5: no popup drive of its own -- drawn into THIS frame (commit=false), so the
    // degraded banner costs no extra waveform and leaves with the first input.
    if (touchDegraded) GUI.drawPopup(renderer, kTouchDegradedNotice, /*commit=*/false);
    // The first frame of a clean restart is a whole new screen, so commit it as an explicit FULL: a
    // HALF here diffs against a blank buffer and leaves a half-driven base under the UI.
    //
    // r163 3.17: no FIRST-ENTRY paint of this screen asks for FAST. A first-entry frame replaces the
    // screen outright, and FAST over a frame that carries AA glyph levels selects the grey composite
    // (metalio_vendor_panel.cpp: `useGray8 = !clean && ...`, and `clean` is `refresh != FAST`), whose
    // 8-phase erase prefix pushes every pixel to paper -- the cold-boot white flash. HALF is the
    // GC16-over-the-diff drive the transition's own AUTO arm already resolves to here; asking for it
    // at the call site makes the request and the resolved mode agree even if that arm was spent.
    // The late-cover catch-up is HALF for the same reason (see the follow-up below). The arm is
    // consumed HERE, so it can never outlive the paint that spends it. FULL wins when both apply.
    const bool firstFullPaint = !firstRenderDone && cleanInitialRefresh;
    const bool firstEntryHalf = !firstRenderDone && !cleanInitialRefresh;
    const bool coverCatchUpHalf = consumeHalfPaintOwed();
    if (coverCatchUpHalf) LOG_INF("HOME", "cover catch-up paint: HALF (r163 3.17 one-shot)");
    HalDisplay::RefreshMode homePaintMode = HalDisplay::FAST_REFRESH;
    if (firstFullPaint) {
      homePaintMode = HalDisplay::FULL_REFRESH;
    } else if (firstEntryHalf || coverCatchUpHalf) {
      homePaintMode = HalDisplay::HALF_REFRESH;
    }
    renderer.displayBuffer(homePaintMode);
    // Slot heights are recorded during the draw above; a change (first layout
    // pass, orientation switch) means the paths must point at those sizes and
    // any missing thumbs must be generated. Refreshing the paths right away
    // lets the next pass draw already-cached thumbs before generation runs.
    const bool coverSpecChanged = coverGridUi->takeThumbHeightChanged();
    if (coverSpecChanged) {
      coverGridUi->refreshCoverPaths();
      recentsLoaded = false;
    }
    if (!firstRenderDone && renderer.lastPaintAttemptHeld()) {
      // r163 3.17: CP_ENTRY_SINGLE_DRIVE held that drive, so NOTHING reached the glass and this frame
      // established nothing. Leave firstRenderDone false and return: the repaid paint (requested by
      // ActivityManager's releaseEntryDrive) then renders as the real first-entry paint -- HALF --
      // instead of falling through to a FAST grey paint over a screen it never drove. A held paint
      // consumes no one-shot either (GfxRenderer.cpp displayBuffer, the gate returns before the
      // producer), so the AUTO arm this screen arrived with survives for the repaid paint.
      LOG_INF("HOME", "first paint was held by the entry gate: repaid paint stays the first-entry paint");
      return;
    }
    if (!firstRenderDone) {
      firstRenderDone = true;
      bool followUpWanted = true;
#if CP_WAKE_ONE_DRIVE
      // r96.2 (D2): this frame WAS the wake clean. The follow-up exists to repaint
      // after thumbnails generate; on the wake that is a second full paint one second
      // after a from-white drive over the same content -- the visible second drive the
      // operator reported as a band. Defer it: covers load here, and the next natural
      // repaint (any input) draws them. The clean is the whole wake.
      if (renderer.lastPaintForcedClean()) {
        LOG_INF("HOME", "wake clean on glass: first-render follow-up paint suppressed (covers load on next repaint)");
        followUpWanted = false;
      }
#endif
      // r163 3.17: the follow-up exists for ONE reason -- thumbnails that were not in the frame
      // just driven (see loadRecentCovers / CP_ENTRY_SINGLE_DRIVE). When onEnter()'s gated preload
      // already finished, `recentsLoaded` is true and that frame is complete: the follow-up would
      // re-drive a byte-identical screen as an explicit FAST over AA glyph grey, i.e. the grey
      // wipe composite whose 8-phase erase prefix pushes the panel to paper. That is the cold-boot
      // white flash, and the cheapest way to remove it is to not ask for the paint at all.
      if (followUpWanted && recentsLoaded) {
        LOG_INF("HOME", "first-render follow-up skipped: covers were already in this frame (r163 3.17)");
        followUpWanted = false;
      }
      if (followUpWanted) {
        // r163 3.17: covers ARE missing, so this paint is genuinely needed -- but it must not take
        // the FAST lane. Ask for HALF (GC16 over the diff, no paper push) through the r162 one-shot,
        // and finish the load first so the HALF frame is the one that carries the new thumbs.
        armHalfPaintOwed();
        loadRecentCovers(CoverGridHomeUi::THUMB_HEIGHT);
        coverGridUi->refreshCoverPaths();
        requestUpdate();
        // requestUpdate() re-arms AUTO above and applyPromotedRefresh() lets the armed policy
        // replace the caller's mode, so the explicit HALF would resolve back to a ratio-derived
        // FAST on a near-identical frame. Clear the arming so it lands as HALF.
        renderer.cancelPromotedRefresh();
      }
    } else if (!recentsLoaded && !recentsLoading) {
      loadRecentCovers(CoverGridHomeUi::THUMB_HEIGHT);
      coverGridUi->refreshCoverPaths();
      requestUpdate();
    }
    return;
  }
  bool bufferRestored = coverRendered && coverBufferStored && restoreCoverBuffer();

  // Band spans topPadding..homeTopPadding: the cover tile starts at the fixed
  // homeTopPadding, so the height must shrink by topPadding or the band (and a
  // centered title, e.g. RoundedRaff's book title) sinks into the tile.
  // Home is the stack root: no back button in its header.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr,
                 nullptr, false);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = content.x;
  coverRectY = metrics.homeTopPadding;
  coverRectW = content.width;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{content.x, metrics.homeTopPadding, content.width, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_LIBRARY), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Library, Transfer, Settings};
  constexpr int kLibrarySlotAt = 2;  // one past Library

  if (hasLibrarySlot()) {
    // Index 2 -- one past Library. A stale 3 here would put Plugins/OPDS one row
    // below where the tap grid resolved it, i.e. the wrong row would open.
    menuItems.insert(menuItems.begin() + kLibrarySlotAt, hasPlugins ? tr(STR_PLUGINS) : tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + kLibrarySlotAt, Plugins);
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(renderer, homeMenuBand(renderer, metrics, content), static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // E5: same as the grid path -- into this frame, no separate drive.
  if (touchDegraded) GUI.drawPopup(renderer, kTouchDegradedNotice, /*commit=*/false);

  // Same clean-restart rule as the grid path above: the first frame is a whole new screen, so it is
  // an explicit FULL rather than a diff against a blank buffer.
  // r163 3.17: and the same first-entry rule -- a first-entry frame (and the late-cover catch-up)
  // asks for HALF, never FAST, because FAST over AA glyph grey is the wipe composite whose erase
  // prefix pushes the panel to paper. See the grid path for the full reasoning.
  const bool firstFullPaint = !firstRenderDone && cleanInitialRefresh;
  const bool firstEntryHalf = !firstRenderDone && !cleanInitialRefresh;
  const bool coverCatchUpHalf = consumeHalfPaintOwed();
  if (coverCatchUpHalf) LOG_INF("HOME", "cover catch-up paint: HALF (r163 3.17 one-shot, list path)");
  HalDisplay::RefreshMode homePaintMode = HalDisplay::FAST_REFRESH;
  if (firstFullPaint) {
    homePaintMode = HalDisplay::FULL_REFRESH;
  } else if (firstEntryHalf || coverCatchUpHalf) {
    homePaintMode = HalDisplay::HALF_REFRESH;
  }
  renderer.displayBuffer(homePaintMode);

  if (!firstRenderDone && renderer.lastPaintAttemptHeld()) {
    // r163 3.17: same as the grid path -- a held drive established nothing, so the repaid paint must
    // still be treated as the first-entry paint (HALF) rather than a FAST grey repaint.
    LOG_INF("HOME", "first paint was held by the entry gate: repaid paint stays the first-entry paint (list path)");
    return;
  }
  if (!firstRenderDone) {
    firstRenderDone = true;
    bool followUpWanted = true;
#if CP_WAKE_ONE_DRIVE
    // r96.2 (D2): same rule as the grid path -- if this frame was the wake clean, the
    // follow-up paint would be a SECOND whole-frame drive one second later over the
    // same content. The covers load on the next natural repaint instead.
    if (renderer.lastPaintForcedClean()) {
      LOG_INF("HOME", "wake clean on glass: first-render follow-up paint suppressed (list path)");
      followUpWanted = false;
    }
#endif
    // r163 3.17: nothing new to show means no paint. See the grid path for the full reasoning.
    if (followUpWanted && recentsLoaded) {
      LOG_INF("HOME", "first-render follow-up skipped: covers were already in this frame (r163 3.17, list path)");
      followUpWanted = false;
    }
    if (followUpWanted) {
      // r163 3.17: covers ARE missing -- load them, then pay for one HALF instead of the FAST wipe.
      armHalfPaintOwed();
      recentsLoading = true;
      const int themeThumbHeight = GUI.homeCoverThumbHeight(renderer);
      loadRecentCovers(themeThumbHeight > 0 ? themeThumbHeight : metrics.homeCoverHeight);
      requestUpdate();
      // Same as the grid path: clear the AUTO armed by requestUpdate() so the explicit HALF survives.
      renderer.cancelPromotedRefresh();
    }
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    const int themeThumbHeight = GUI.homeCoverThumbHeight(renderer);
    loadRecentCovers(themeThumbHeight > 0 ? themeThumbHeight : metrics.homeCoverHeight);
  }
}

void HomeActivity::onRecentBookSelected(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onLibraryOpen() { activityManager.goToLibrary(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onPluginsOpen() { activityManager.goToPlugins(hasOpdsServers); }
