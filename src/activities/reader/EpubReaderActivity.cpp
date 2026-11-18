#include "EpubReaderActivity.h"

#include <BlePageTurner.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <esp_system.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>

#include "../../util/BookmarkFile.h"
#include "BookmarkEntry.h"
#include "ChapterBuildGate.h"
#include "audio/UiSounds.h"  // AUDIO1: page-turn / refused-turn feedback
#include "ClipSelectionActivity.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderClippingListActivity.h"
#include "EpubReaderFootnoteSelectActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "ProgressSaves.h"
#include "ProgressStore.h"
#include "ProgressWriter.h"
#include "VocabHistoryActivity.h"
#include "platform/CpuStats.h"
#include "QrDisplayActivity.h"
#include "ReaderActivity.h"
#include "ReaderFontSizes.h"
#include "ReaderHandoff.h"
#include "ReaderLineSpacing.h"
#include "ReaderToolbarUi.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFont.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/settings/TextSettingsActivity.h"
#include "activities/util/InPlaceValueStep.h"
#include "clippings/ClippingText.h"
#include "clippings/ClippingsManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"
#include "util/SlideSwitch.h"

// r96.1 D1: CP_IMG_CLEAN_ON_ARRIVAL lives in src/platform/PlatformFeatures.h, and
// THIS file did not include it. `#if CP_IMG_CLEAN_ON_ARRIVAL` on an undefined
// macro is 0, so r96 shipped with the arm block compiled out: the arrival paint
// stayed the plain image-branch drive (GL16/GL2), and the only clean in the
// transition was the `pagesUntilFullRefresh = 1` that fires on the page AFTER the
// image page. That is exactly the operator's report -- "navigating away from an
// image does the clean; navigating to a page with an image does no clean" -- and
// it is provable from the r96 artifact: the arm's log string is not in the image.
#include "../../platform/PlatformFeatures.h"

// r163 C3: DisplayLockGuard lives in readerhandoff (ReaderHandoff.h). Qualified once here rather
// than at each of the eight call sites, and the guard locals below are not named `display` so they
// cannot shadow the global HalDisplay instance.
using readerhandoff::DisplayLockGuard;

namespace {
// The X4 Pro and X4 Classic carry the X4's panel but sit outside isXteinkDevice()
// (that helper also gates power management). Overlay refresh choices are per-panel:
// this family runs the grayscale anti-aliasing pass, so chrome painted over a
// fresh page needs the HALF ghost-cleanup and closing re-renders the page.
bool xteinkClassPanel() {
  return gpio.isXteinkDevice() || BoardConfig::isX4Pro() || BoardConfig::isX4Classic() || BoardConfig::isEegoA4();
}

constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr float bookmarkProgressEpsilon = 0.0001f;
constexpr int CLIP_SELECTION_PAGE_WINDOW = 3;

// Word i's font: headings and CSS font-size runs can differ from the section font.
int clippingWordFont(const GfxRenderer& renderer, const int fontId, const TextBlock& block, const uint16_t i) {
  return TextBlock::renderFontId(renderer, block.wordFontId(i, fontId), fontId);
}

Rect clippingWordRect(const GfxRenderer& renderer, const int fontId, const PageLine& line, const uint16_t i,
                      const int top, const int left, const int wordWidth) {
  const auto& block = line.getBlock();
  const int wordFont = clippingWordFont(renderer, fontId, *block, i);
  const char* text = block->wordText(i);
  const auto style = static_cast<EpdFontFamily::Style>(block->wordStyle(i) & ~EpdFontFamily::UNDERLINE);
  const int skipX =
      clippingText::hasEmSpacePrefix(text) ? renderer.getTextAdvanceX(wordFont, "\xe2\x80\x83", style) : 0;
  int width = wordWidth - skipX;
  if (i + 1 < block->wordCount() && block->wordXpos(i + 1) > block->wordXpos(i)) {
    width = std::min(width, static_cast<int>(block->wordXpos(i + 1) - block->wordXpos(i) - skipX));
  }
  const int lineFont = TextBlock::renderFontId(renderer, block->blockFontId(fontId), fontId);
  return Rect{left + line.xPos + block->wordXpos(i) + skipX,
              top + line.yPos + block->getRubyShift(renderer.getFontAscenderSize(lineFont)) +
                  block->wordYOffset(renderer, fontId, i),
              width, renderer.getLineHeight(wordFont)};
}

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

constexpr char READ_FOLDER[] = "/read";

bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!ClippingStore::moveBook(srcPath, dstPath)) {
    LOG_ERR("ERS", "Failed to move finished book and clippings to '/Read' folder");
    return;
  }

  // Protected-book sidecars travel with the book; a protected book separated
  // from its key no longer opens, so a failed move rolls everything back.
  static constexpr const char* SIDECARS[] = {".key", ".rights"};
  for (size_t i = 0; i < std::size(SIDECARS); i++) {
    const std::string from = srcPath + SIDECARS[i];
    if (!Storage.exists(from.c_str())) continue;
    const std::string to = dstPath + SIDECARS[i];
    if (Storage.rename(from.c_str(), to.c_str())) continue;
    LOG_ERR("ERS", "Failed to move sidecar %s -> %s", from.c_str(), to.c_str());
    for (size_t j = 0; j < i; j++) {
      Storage.rename((dstPath + SIDECARS[j]).c_str(), (srcPath + SIDECARS[j]).c_str());
    }
    if (!Storage.rename(dstPath.c_str(), srcPath.c_str())) {
      LOG_ERR("ERS", "Failed to restore epub after sidecar move failure: %s -> %s", dstPath.c_str(), srcPath.c_str());
    }
    return;
  }

  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  bool cacheMoved = false;
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    cacheMoved = Storage.rename(oldCachePath.c_str(), newCachePath.c_str());
    if (!cacheMoved) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, cacheMoved ? newCachePath : oldCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

// A button contact is closed right now. Reads the Xteink ADC ladder and power pin only; on other
// boards a press stops the build once the loop has debounced it.
bool buttonContactClosed() { return gpio.rawInputActive(); }

// C14 (upstream aaf50e44): the selection to restore when the dictionary lookup screen
// comes back. Only single-page selections can be restored -- after a clipping the reader
// has moved to the END page, so a multi-page range no longer maps onto the pages that get
// loaded. ClipSelectionActivity::onEnter places the CURSOR on lastWord, so the end the
// operator was extending from has to be handed over as lastWord.
ClipSelectionActivity::Selection lookupReturnSelectionFor(const ClippingResult& clipping) {
  if (clipping.startPageOffset != clipping.endPageOffset) return {};
  if (clipping.cursorAtStart) return {0, 0, clipping.endWordIndex, clipping.startWordIndex, true};
  return {0, 0, clipping.startWordIndex, clipping.endWordIndex, true};
}

}  // namespace

EpubReaderActivity::~EpubReaderActivity() {
  ImageBlock::setExtractor(nullptr, nullptr);
  // ActivityManager destroys activities with its RenderLock already held;
  // taking another here self-deadlocks (renderingMutex is non-recursive).
  settleOverlayRefresh();
  discardOverlayPage();  // free the overlay's page snapshot if one is held
  if (edgeDragPageStored) edgeDragDropSnapshot();  // FL1: and the edge drag's, if a drag was live at teardown
  // A radio still stopped for this book's build may start again in the next one.
  if (radioReleasedForBuild.load()) bleturner::afterPaint();

  if (footnoteDepth > 0 && epub) saveLinkStack();

  // r163 C4: the destructor never saves. onExit() has already flushed the accepted snapshot; what
  // is still pending here belongs to another book's slot or to a write the writer task is finishing,
  // and deriving a position from a half-torn-down section would be exactly the stale save the rule
  // forbids. All that happens here is the report.
  if (epub && readerprogress::ProgressStore::get().dirty(epub->getCachePath().c_str())) {
    LOG_ERR("PRG", "Reader closed with a progress snapshot still pending for %s", epub->getCachePath().c_str());
  }
  // r163 C2, device-side proof of the domain's discipline: one task per burst of writes, not one
  // per session. A reading session that turned 200 pages should report a handful, not 200.
  LOG_DBG("PRG", "Progress writer: %d task(s) spawned, %d job(s) still queued",
          readerprogress::ProgressWriter::get().spawnedTasks(), readerprogress::ProgressWriter::get().queued());

  section.reset();
  CLIPPINGS.unload();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

bool EpubReaderActivity::loadBook() {
  auto loadedEpub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
  if (!loadedEpub) {
    LOG_ERR("ERS", "Failed to allocate EPUB object");
    return false;
  }

  const bool uncached = !Storage.exists((loadedEpub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
    disableFastInitialRefresh();
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }

  bool loaded;
  {
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    loaded = loadedEpub->load(true, SETTINGS.embeddedStyle == 0);
  }
  if (!loaded) {
    // Surfaced by handleLoadFailure() as a dialog; loadedEpub dies with this
    // scope, so carry the reason out in a member.
    loadProtectionError = loadedEpub->getProtectionError();
    LOG_ERR("ERS", "Failed to load EPUB%s%s", loadProtectionError.empty() ? "" : ": ", loadProtectionError.c_str());
    return false;
  }
  epub = std::move(loadedEpub);

  ImageBlock::clearRenderFailures();
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest);
  });

  epub->setupCacheDir();

  HalFile f;
  // True once progress.bin yielded a usable record. Spine 0 is a *valid* saved
  // position ("I paged back to the start"), so it cannot double as "no position".
  bool hasSavedProgress = false;

  // r163 C4: a snapshot that is still pending for this book is newer than the file -- a fast exit,
  // or a write that had not landed when the reader closed. It wins over progress.bin, which is
  // exactly what makes "close the book and open it again at once" keep the page.
  progressrecord::Record pending;
  if (readerprogress::ProgressStore::get().peek(epub->getCachePath().c_str(), pending)) {
    hasSavedProgress = true;
    currentSpineIndex = pending.spineIndex;
    nextPageNumber = pending.pageNumber;
    if (nextPageNumber == UINT16_MAX) {
      LOG_DBG("ERS", "Ignoring stale last-page sentinel from the pending snapshot");
      nextPageNumber = 0;
    }
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = pending.pageCount;
    if (pending.hasVisibleTextOffset) {
      cachedVisibleTextOffset = pending.visibleTextOffset;
    } else {
      cachedVisibleTextOffset.reset();
    }
    LOG_DBG("ERS", "Resumed from the pending snapshot: %d, %d", currentSpineIndex, nextPageNumber);
  } else if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[10];
    int dataSize = f.read(data, sizeof(data));
    if (dataSize == 4 || dataSize == 6 || dataSize == 10) {
      hasSavedProgress = true;
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    } else if (dataSize == 10) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      cachedVisibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                                (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
    }
  }

  // Only a book with no saved position at all starts at its text reference.
  // Gating on `currentSpineIndex == 0` instead meant a reader who had paged back
  // into chapter 1 was thrown forward to the cover's text reference on reopen.
  if (!hasSavedProgress) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      cachedVisibleTextOffset.reset();
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  loadLinkStack();
  loadCachedBookmarks();
  if (!CLIPPINGS.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), "epub")) {
    LOG_ERR("CLIP", "Failed to load clippings for %s", epub->getPath().c_str());
  }
  return true;
}

ChapterPosition EpubReaderActivity::chapterPosition() const {
  if (section) return {section->currentPage, section->estimatedTotalPages()};
  return {nextPageNumber, cachedChapterTotalPageCount};
}

int EpubReaderActivity::bookPercentFor(const ChapterPosition& position) const {
  if (!epub || epub->getBookSize() == 0 || !position.hasTotal()) return 0;
  // The page index can run past the chapter's estimated total while it is still
  // building, so the fraction is clamped before the cast.
  const float fraction = epub->calculateProgress(currentSpineIndex, position.chapterFraction());
  return static_cast<int>(std::clamp(fraction, 0.0f, 1.0f) * 100.0f + 0.5f);
}

void EpubReaderActivity::openReaderMenu() {
  pendingManualTurn = 0;
  if (usesToolbarMenu()) {
    // Reached from a child activity's result handler (footnotes, bookmarks,
    // go-to-percent... cancelled back to the menu), so the framebuffer holds
    // that screen, not the page: re-render the page and let renderBook() put
    // the toolbar on top. The in-reader fast path is openOverlay().
    overlay = Overlay::Toolbar;
    focusedTool = 0;
    panelHoldJumped = false;
    panelCursorShown = !mappedInput.hasTouch();
    if (!toolbarUi) toolbarUi = std::make_unique<ReaderToolbarUi>(renderer);
    toolbarUi->begin();
    discardOverlayPage();
    requestUpdate();
    return;
  }
  // Child screens (chapter list, text settings) release the section to free its
  // pagination buffers; chapterPosition() covers that with the cached position.
  const ChapterPosition position = chapterPosition();
  const int bookProgressPercent = bookPercentFor(position);

  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(
          renderer, mappedInput, epub->getTitle(), position.displayPage(), position.totalPages, bookProgressPercent,
          SETTINGS.orientation, !currentPageFootnotes.empty(), !cachedBookmarks.empty(), CLIPPINGS.hasClippings()),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);

        if (SETTINGS.orientation != menu.orientation) {
          applyOrientation(menu.orientation);
        }

        toggleAutoPageTurn(menu.pageTurnOption);
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
        }
      });
}

// Runs on the render task, under its lock, right before every chapter build. The page turner's
// radio holds about 50 KB while it runs, and no heap figure taken before a build tells whether the
// build fits next to it (X3: with the radio up, 55 KB free and a 47 KB block, a 6 KB chapter ran
// out of memory). So the radio stops for every build and starts again once the chapter is built
// and its page is shown. A radio that did not stop within the module's 3 s, or a start still
// settling, usually lets go soon after: the build waits up to BUILD_ROOM_WAIT_MS more, then is
// refused rather than run next to it. The stop finishes on the main loop (the module's tick) and
// the start on its own task; neither needs the render lock this task holds.
bool EpubReaderActivity::makeRoomForChapterBuild() {
  bool asked = false;
  const auto roomMade = [this, &asked] {
    // The first ask stops the radio. A later one asks again only once a start that was still
    // settling has left the radio up.
    if (!radioReleasedForBuild.load() && (!asked || !bleturner::status().starting)) {
      radioReleasedForBuild = bleturner::beforeChapterBuild() != bleturner::BuildRelease::NotHeld;
    }
    asked = true;
    return chapterBuildMayRun(bleturner::status());
  };
  const auto nowMs = [] { return static_cast<uint32_t>(millis()); };
  const auto nap = [](const uint32_t ms) { delay(ms); };
  if (waitForBuildRoom(roomMade, nowMs, nap, BUILD_ROOM_WAIT_MS, BUILD_ROOM_STEP_MS)) return true;
  LOG_ERR("ERS", "Bluetooth radio still holds the heap after %u ms; chapter build refused",
          static_cast<unsigned>(BUILD_ROOM_WAIT_MS));
  return false;
}

bool EpubReaderActivity::buildTickHeapGate() {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxBlock = ESP.getMaxAllocHeap();
  buildHeapPaused = freeHeap < BACKGROUND_BUILD_MIN_FREE_HEAP || maxBlock < BACKGROUND_BUILD_MIN_MAX_ALLOC;
  return !buildHeapPaused;
}

void EpubReaderActivity::showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (!buildPopupPending || !renderer.hasFrameBuffer()) return;
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  pagesUntilFullRefresh = 1;
  buildPopupPending = false;
}

// Debounced state of every button. Covers the frame between a raw release and its debounced
// release edge, which would otherwise run a build step before the page turn.
bool EpubReaderActivity::anyButtonPressed() const {
  using Button = MappedInputManager::Button;
  for (const Button button :
       {Button::Back, Button::Confirm, Button::Left, Button::Right, Button::Up, Button::Down, Button::Power}) {
    if (mappedInput.isPressed(button)) return true;
  }
  return false;
}

bool EpubReaderActivity::backgroundBuildTick() {
  DisplayLockGuard displayGuard;
  // Not under an open toolbar or panel: those repaint straight onto the page in the framebuffer.
  if (!displayGuard.owns() || overlay != Overlay::None || !backgroundBuildWanted()) return false;
  // A button is down or a page turn is queued: the next loop frames act on it first.
  if (buttonContactClosed() || anyButtonPressed() || pendingManualTurn != 0 || !buildTickHeapGate()) return false;
  // A build step can lend the framebuffer (image probes), which hands it back white while the
  // panel still shows the page; redraw so nothing is later painted over the blank buffer.
  const uint32_t loansBefore = renderer.frameBufferLoanCount();
  const bool built = section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK, &buttonContactClosed);
  if (renderer.frameBufferLoanCount() != loansBefore) {
    pageBufferStale = true;
    requestUpdate();
  }
  if (!built) {
    LOG_ERR("ERS", "Background section build failed");
    backgroundBuildFailedSpine = currentSpineIndex;
    nextPageNumber = section->currentPage;
    section.reset();
    requestUpdate();
  } else if (section->isBuildComplete() && applyDeferredReposition()) {
    requestUpdate();
  }
  return true;
}

// r163 A2: how far ahead the glyph lookahead runs, for the font that is reading.
//
// A glyph scan is the part of a page turn that SD/PSRAM fonts make expensive: an SD .cpfont asks
// the card for glyphs the built-in fonts already hold, so one page can cost tens of milliseconds
// of work that the turn then waits on. A window of pages is worth more on such a font than on a
// fast one, so the depth follows the measured per-page cost of *that* font -- and starts over when
// the reading font changes, rather than inheriting the previous font's number.
int EpubReaderActivity::idlePrewarmDepth() const {
  if (prewarmFontId != SETTINGS.getReaderFontId()) return PREWARM_MIN_DEPTH;
  const int steps = static_cast<int>(prewarmAvgMs / PREWARM_MS_PER_DEPTH_STEP);
  const int depth = PREWARM_MIN_DEPTH + steps;
  if (depth >= PREWARM_MAX_DEPTH) return PREWARM_MAX_DEPTH;
  return depth < PREWARM_MIN_DEPTH ? PREWARM_MIN_DEPTH : depth;
}

// Every condition a warm may run under. The render lock is *held* by the caller (a glyph scan
// must not run next to a compose, and holding the lock is the proof there is none); the rest is
// the same discipline backgroundBuildTick() applies, because a scan competes with the build and
// the radio for the same heap and the same card.
bool EpubReaderActivity::idlePrewarmMayRun() const {
  if (!section || section->isBuilding()) return false;  // a chapter build owns the heap
  if (overlay != Overlay::None) return false;           // a panel is repainting over the page
  if (!renderer.hasFrameBuffer()) return false;
  const auto* fcm = renderer.getFontCacheManager();
  if (!fcm || fcm->isScanning()) return false;          // a scan or prewarm is already running
  if (buttonContactClosed() || anyButtonPressed() || pendingManualTurn != 0) return false;
  if (bleturner::holdsHeap()) return false;             // the page turner's radio is up
  // The build tick's own floors: free >= 32 KiB and one block >= 16 KiB.
  if (ESP.getFreeHeap() < BACKGROUND_BUILD_MIN_FREE_HEAP ||
      ESP.getMaxAllocHeap() < BACKGROUND_BUILD_MIN_MAX_ALLOC) {
    return false;
  }
  // The partial extension build is about to start on this page: leave it the card.
  if (section->isPartial() && section->currentPage + PARTIAL_REBUILD_START_MARGIN >=
                                  static_cast<int>(section->pageCount)) {
    return false;
  }
  return true;
}

// One page of the lookahead per call, never a loop: a queued page turn is at worst one scan
// behind, and the window is abandoned the moment the page on screen changes (the next idle frame
// re-anchors it). Depth comes from the font's own measured cost; the window budget stops a slow
// font from spending idle time without bound.
void EpubReaderActivity::idlePrewarmTick() {
  DisplayLockGuard displayGuard;
  if (!displayGuard.owns()) return;
  if (!idlePrewarmMayRun()) return;
  if (lastRenderCompleteMs == 0 || millis() - lastRenderCompleteMs <= PREWARM_DEBOUNCE_MS) return;

  const int fontId = SETTINGS.getReaderFontId();
  if (fontId != prewarmFontId) {
    prewarmFontId = fontId;
    prewarmAvgMs = 0;  // unknown font: start from the floor and learn it as pages are warmed
  }
  // A turn, a spine change or a re-pagination opens a new window on the new page. A new session
  // abandons the window outright: its remaining pages were measured against a layout that no
  // longer exists, and warming them would pin glyphs the next paint never asks for.
  const uint32_t session = sessionGeneration_.current();
  if (prewarmSessionGeneration != session || prewarmSpine != currentSpineIndex ||
      prewarmAnchorPage != section->currentPage) {
    prewarmSessionGeneration = session;
    prewarmSpine = currentSpineIndex;
    prewarmAnchorPage = section->currentPage;
    prewarmNextPage = section->currentPage + 1;
    prewarmWindowMs = 0;
  }

  const int depth = idlePrewarmDepth();
  // The window is [anchor+1, anchor+depth): prewarmNextPage is only ever set to anchor+1 (above) or
  // incremented (below), so a lower-bound test here would be unreachable code pretending to be a
  // guard.
  if (prewarmNextPage >= prewarmAnchorPage + 1 + depth) return;
  if (prewarmWindowMs >= PREWARM_WINDOW_BUDGET_MS) return;
  if (prewarmNextPage >= static_cast<int>(section->pageCount)) return;

  const int page = prewarmNextPage;
  ++prewarmNextPage;  // consumed even when the load fails, so one bad page cannot stall the window
  const auto loaded = section->loadPage(page);
  if (!loaded) return;
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) return;

  const auto t0 = millis();
  {
    auto scope = fcm->createPrewarmScope();
    loaded->render(renderer, fontId, 0, 0);
    scope.endScanAndPrewarm();
  }
  const unsigned long cost = millis() - t0;
  prewarmWindowMs += cost;
  // Smoothed, so one unusually dense page does not swing the depth; the first measurement seeds it.
  prewarmAvgMs = prewarmAvgMs == 0 ? cost : (prewarmAvgMs * 3 + cost) / 4;
  LOG_DBG("ERS", "Idle prewarm: page %d in %lums (depth %d, window %lums)", page, cost, idlePrewarmDepth(),
          prewarmWindowMs);
}

// ---- r163 C1: preparing the next chapter's first page -------------------------------------

// The reader's own state for the prefetch decision. Everything here is free to read: the spine
// index and count are in RAM, the layout signature is the string hash renderBook() already
// computes, and the generation is one atomic load.
chapterprefetch::Live EpubReaderActivity::prefetchLive() const {
  chapterprefetch::Live live;
  live.spineIndex = currentSpineIndex;
  live.spineCount = epub ? epub->getSpineItemsCount() : 0;
  live.generation = sessionGeneration_.current();
  live.nextSpineExists = currentSpineIndex + 1 < live.spineCount;
  if (buildViewportWidth == 0 || buildViewportHeight == 0) return live;
  live.layoutSignature =
      readerRenderSpecSignature(SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight));
  live.layoutKnown = true;
  return live;
}

// Every gate a prepare must hold, read fresh. `displayOwned` is passed in rather than acquired
// here: the tick owns one guard across the whole frame's work, so the lock it decided under is
// the lock the work runs under.
chapterprefetch::Gates EpubReaderActivity::prefetchGates(const bool displayOwned) const {
  chapterprefetch::Gates gates;
  gates.displayOwned = displayOwned;
  gates.frameBuffer = renderer.hasFrameBuffer();
  gates.overlayOpen = overlay != Overlay::None;
  gates.overlayRefreshPending = overlayRefreshPending;
  gates.pageBufferStale = pageBufferStale.load();
  gates.liveSection = section != nullptr;
  gates.liveBuilding = section && section->isBuilding();
  gates.scanning = [this] {
    // No font cache manager is treated as "busy": the prefetch must not run on a renderer whose
    // scan state cannot be read.
    const auto* fcm = renderer.getFontCacheManager();
    return !fcm || fcm->isScanning();
  }();
  gates.queuedTurn = pendingManualTurn != 0 || pendingPageJump.has_value() || !pendingAnchor.empty() ||
                     pendingPercentJump || pendingOffsetJump.has_value();
  gates.inputDown = buttonContactClosed() || anyButtonPressed();
  gates.radioHoldsHeap = bleturner::holdsHeap();
  // The live chapter is at its partial watermark: renderBook()/the loop are about to start its
  // extension build, which owns the heap and the card until it catches up.
  gates.liveNeedsBuild = section && section->isPartial() &&
                         section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount);
  gates.freeHeap = ESP.getFreeHeap();
  gates.maxAlloc = ESP.getMaxAllocHeap();
  return gates;
}

// One idle frame: decide, then either prepare the next chapter's first page or drop a stale one.
//
// The work is bounded on both sides -- one attempt per chapter, at most kPagesPerAttempt pages,
// an interruptible parse loop (&buttonContactClosed, as backgroundBuildTick uses) and a spine
// above kMaxSpineBytes refused before anything is opened, because the zip inflate is the one step
// that cannot be interrupted.
bool EpubReaderActivity::chapterPrefetchTick() {
  if (!epub) return false;
  // EVERY branch below mutates `prefetchSection`/`prefetchState`, and the render task mutates the
  // same pair in adoptPreparedChapter(). RenderLock is the one thing that serialises them: the
  // render task holds it for the whole of render(), and this guard takes it in TRY mode. So a frame
  // that does not get it owns nothing and must touch nothing -- Discard/Adopt used to run before
  // any owns() check, which let this task reset a unique_ptr the render task was inside.
  // Not blocking cannot deadlock: Try never waits, the guard is released before the next frame, and
  // the only other holder is the render task, which never takes this function's path.
  DisplayLockGuard displayGuard;
  if (!displayGuard.owns()) return false;
  const chapterprefetch::Live live = prefetchLive();
  const chapterprefetch::Gates gates = prefetchGates(displayGuard.owns());

  switch (chapterprefetch::nextAction(prefetchState, live, gates)) {
    case chapterprefetch::Action::Discard:
      LOG_DBG("ERS", "Prepared chapter %d is stale (spine %d, generation %lu)", prefetchState.target.spineIndex,
              currentSpineIndex, static_cast<unsigned long>(live.generation));
      discardPreparedChapter();
      return true;
    case chapterprefetch::Action::Adopt:
      // The render task adopts, and it runs before this loop does. An artefact still here means
      // the open declined it (a backward turn, or a live cache already in the slot): drop it.
      discardPreparedChapter();
      return true;
    case chapterprefetch::Action::Prepare:
      break;
    case chapterprefetch::Action::None:
    default:
      return false;
  }

  const int next = live.spineIndex + 1;
  // Two facts off the card, and both refuse this spine for the rest of the chapter
  // (State::refusedSpine expires by itself when the reader moves, because it names the *next*
  // spine). Without the refusal the idle loop would stat the card twice a frame.
  const std::string nextPath = epub->getCachePath() + "/sections/" + std::to_string(next) + ".bin";
  const size_t nextEnd = epub->getCumulativeSpineItemSize(next);
  const size_t nextStart = next > 0 ? epub->getCumulativeSpineItemSize(next - 1) : 0;
  const size_t nextBytes = nextEnd > nextStart ? nextEnd - nextStart : 0;
  const bool alreadyWarm = Storage.exists(nextPath.c_str());
  if (alreadyWarm || chapterprefetch::spineTooBigForPrefetch(nextBytes)) {
    prefetchState.refusedSpine = next;
    LOG_DBG("ERS", "Not preparing chapter %d: %s (%u bytes)", next,
            alreadyWarm ? "already cached" : "too big to inflate", static_cast<unsigned>(nextBytes));
    return false;
  }

  const ReaderRenderSpec spec = SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight);
  // nothrow: a prepare that cannot allocate its own section is a refused prepare, not an abort.
  auto prepared = makeUniqueNoThrow<Section>(epub, next, renderer, Section::Cache::Prefetch);
  if (!prepared) {
    prefetchState.refusedSpine = next;
    return false;
  }
  // A `.pre.bin` left by an earlier visit is not adoptable (adoption requires a ready artefact in
  // this process), so clear it rather than let a stale layout sit on the card.
  prepared->clearCache();

  const uint32_t loansBefore = renderer.frameBufferLoanCount();
  const unsigned long startedMs = millis();
  if (!prepared->startBuild(spec, nullptr, /*shedFontCaches=*/false)) {
    // OOM, a card that will not write, or a radio that took the heap between the gate and here.
    LOG_ERR("ERS", "Prepared chapter %d: build refused", next);
    prefetchState.refusedSpine = next;
    return true;
  }
  const bool built = prepared->buildSomeMore(chapterprefetch::kPagesPerAttempt, &buttonContactClosed);
  const bool complete = built && prepared->isBuildComplete();
  const uint32_t recordBytes = prepared->buildRecordBytes();
  if (!complete) {
    // Commits the pages reached as a partial at `.pre.bin` and frees the parser: a held artefact
    // must cost no heap until it is adopted.
    prepared->suspendBuild();
  }
  // A layout image probe can lend the framebuffer and hand it back blank while the panel still
  // shows the page -- the same hazard, and the same guard, as backgroundBuildTick().
  if (renderer.frameBufferLoanCount() != loansBefore) {
    pageBufferStale = true;
    requestUpdate();
  }

  const int pages = prepared->pageCount;
  if (!built || !chapterprefetch::withinBudget(pages, recordBytes)) {
    LOG_ERR("ERS", "Prepared chapter %d unusable (%d pages, %u record bytes)", next, pages,
            static_cast<unsigned>(recordBytes));
    prefetchState.refusedSpine = next;
    return true;
  }

  prefetchState.target = {next, live.layoutSignature};
  prefetchState.armedGeneration = live.generation;
  prefetchState.pages = pages;
  prefetchState.bytes = recordBytes;
  prefetchState.ready = true;
  prefetchSection = std::move(prepared);
  LOG_INF("ERS", "Prepared chapter %d: %d page(s), %u record bytes, %lums", next, pages,
          static_cast<unsigned>(recordBytes), millis() - startedMs);
  return true;
}

void EpubReaderActivity::discardPreparedChapter() {
  if (prefetchSection) {
    // Abandoned, not suspended: an invalidation must cost no writes. The spine's unzipped HTML
    // stays cached (Section promotes it before any layout), and that is the expensive half.
    prefetchSection->abandonBuild();
    prefetchSection->clearCache();
    prefetchSection.reset();
  }
  prefetchState = {};
}

// Runs on the render task, at the top of a chapter open and before any Section is created. The
// rename is the whole adoption: the live Section then finds a partial of the new spine where a
// cold open would have found nothing, and takes the deferred-extension path it already has.
bool EpubReaderActivity::adoptPreparedChapter() {
  if (!epub) return false;
  // Only a forward chapter turn lands on page 0 (pageTurn sets nextPageNumber = 0). A backward
  // turn asks for the chapter's last page, a sync/bookmark jump asks for a text offset (which the
  // prepared pages may not even contain) and a percent jump rebuilds the whole chapter: none of
  // those is answered by a prepared *first page*, so none of them adopts.
  const bool landsOnPageZero = nextPageNumber == 0 && !pendingPageJump.has_value() && pendingAnchor.empty() &&
                               !pendingOffsetJump.has_value() && !pendingPercentJump;
  const chapterprefetch::Live live = prefetchLive();
  if (!chapterprefetch::canAdopt(prefetchState, live, landsOnPageZero)) return false;
  if (!prefetchSection) return false;

  const int spine = live.spineIndex;
  const std::string livePath = epub->getCachePath() + "/sections/" + std::to_string(spine) + ".bin";
  // Never replace a chapter cache the reader already has: a prepared first page is smaller than
  // a warm chapter, so adoption must never make an open worse than today's.
  if (Storage.exists(livePath.c_str())) return false;
  const std::string preparedPath = prefetchSection->sectionPath();
  if (preparedPath.empty() || !Storage.exists(preparedPath.c_str())) return false;

  const int pages = prefetchState.pages;
  const bool renamed = Storage.rename(preparedPath.c_str(), livePath.c_str());
  // Dropped either way, and its paths were its own: nothing of a live chapter moved.
  prefetchSection.reset();
  prefetchState = {};
  if (!renamed) {
    Storage.remove(preparedPath.c_str());
    LOG_ERR("ERS", "Failed to adopt prepared chapter %d", spine);
    return false;
  }
  LOG_INF("ERS", "Adopted prepared chapter %d (%d page(s)): the open skips its build", spine, pages);
  return true;
}

void EpubReaderActivity::openDictionaryWordSelect() {
  if (SETTINGS.dictionaryName[0] == '\0') {
    // No dictionary: this dialog is the end of the road, so the selection captured for a
    // restore must not outlive it (it would re-seat the cursor on a later, unrelated open).
    pendingLookupSelection = {};
    showDictionaryMessage = true;
    dictionaryMessageTime = millis();
    requestUpdate();
    return;
  }
  startClipSelection(-1, -1, ClipSelectionActivity::Mode::Lookup);
}

void EpubReaderActivity::startClipSelection(const int initialX, const int initialY,
                                            const ClipSelectionActivity::Mode mode) {
  // C14: consume the pending restore once, whatever this call turns out to do.
  const auto restoreSelection = pendingLookupSelection;
  pendingLookupSelection = {};
  if (!section || !epub || section->currentPage < 0 || section->currentPage >= section->pageCount) return;

  const int pageNumber = section->currentPage;
  const uint16_t pageCount = section->pageCount;
  const uint16_t spineIndex = static_cast<uint16_t>(currentSpineIndex);
  std::vector<std::unique_ptr<Page>> pages;
  pages.reserve(CLIP_SELECTION_PAGE_WINDOW);
  for (int pageOffset = 0; pageOffset < CLIP_SELECTION_PAGE_WINDOW && pageNumber + pageOffset < pageCount;
       ++pageOffset) {
    auto page = section->loadPage(pageNumber + pageOffset);
    if (!page) break;
    if (pageOffset == 0 && initialX >= 0 && CLIPPINGS.hasClippings()) {
      RenderLock lock;
      const int clippingIndex = clippingAtPoint(*page, initialX, initialY);
      if (clippingIndex >= 0) {
        const bool removed = CLIPPINGS.removeClippingAt(static_cast<size_t>(clippingIndex));
        if (!removed) LOG_ERR("CLIP", "Failed to delete highlighted clipping %d", clippingIndex);
        clippingMessage = removed ? StrId::STR_CLIPPING_REMOVED : StrId::STR_CLIPPING_DELETE_FAILED;
        showClippingMessage = true;
        clippingMessageTime = millis();
        requestUpdate();
        return;
      }
    }
    pages.push_back(std::move(page));
  }
  if (pages.empty()) {
    LOG_ERR("CLIP", "Failed to load page %d for clipping selection", pageNumber);
    requestUpdate();
    return;
  }

  if (buildViewportWidth == 0 || buildViewportHeight == 0) {
    LOG_ERR("CLIP", "Cannot anchor clipping before the reader viewport is initialized");
    requestUpdate();
    return;
  }
  const uint32_t layoutSignature =
      readerRenderSpecSignature(SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight));

  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += SETTINGS.screenMargin;
  marginLeft += SETTINGS.screenMargin;

  std::string chapterTitle;
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex >= 0) chapterTitle = epub->getTocItem(tocIndex).title;
  std::string bookTitle = epub->getTitle();
  std::string author = epub->getAuthor();

  auto activity = makeUniqueNoThrow<ClipSelectionActivity>(renderer, mappedInput, std::move(pages), marginLeft,
                                                           marginTop, initialX, initialY, mode);
  // C14: reopen on the word the operator was looking up, cursor only (no anchor).
  if (activity && restoreSelection.valid) activity->setInitialSelection(restoreSelection);
  if (!activity) {
    LOG_ERR("CLIP", "Failed to allocate clipping selection activity");
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(activity), [this, spineIndex, pageNumber, pageCount, layoutSignature,
                                               bookTitle = std::move(bookTitle), author = std::move(author),
                                               chapterTitle = std::move(chapterTitle)](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    const auto& clipping = std::get<ClippingResult>(result.data);
    const uint16_t startPage = static_cast<uint16_t>(pageNumber + clipping.startPageOffset);
    const uint16_t endPage = static_cast<uint16_t>(pageNumber + clipping.endPageOffset);
    if (section && currentSpineIndex == spineIndex && endPage < section->pageCount) {
      section->currentPage = endPage;
      currentPageVisibleOffset = section->getVisibleTextOffsetForPage(endPage);
    }
    if (clipping.action == ClippingResult::Action::Bookmark) {
      addBookmark();
      showBookmarkMessage = true;
      bookmarkMessageTime = millis();
      requestUpdate();
      return;
    }
    if (clipping.action == ClippingResult::Action::Lookup) {
      auto page = section ? section->loadPage(section->currentPage) : nullptr;
      if (!page) {
        LOG_ERR("CLIP", "Failed to load lookup page");
        requestUpdate();
        return;
      }
      int top, right, bottom, left;
      renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
      auto lookup = makeUniqueNoThrow<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(page),
                                                                    left + SETTINGS.screenMargin,
                                                                    top + SETTINGS.screenMargin, clipping.text,
                                                                    bookTitle);
      if (!lookup) {
        LOG_ERR("CLIP", "OOM: dictionary lookup activity");
        requestUpdate();
        return;
      }
      // C14: the lookup left the page, so returning has to put the cursor back. Remembered
      // here (single-page only) and consumed by the re-opened selection screen.
      lookupReturnSelection = lookupReturnSelectionFor(clipping);
      startActivityForResult(std::move(lookup), [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          pendingLookupSelection = lookupReturnSelection;
          openDictionaryWordSelect();
        }
      });
      return;
    }
    const uint16_t paragraphIndex =
        section ? section->getParagraphIndexForPage(startPage).value_or(UINT16_MAX) : UINT16_MAX;
    const auto addResult = CLIPPINGS.addClipping(
        spineIndex, startPage, endPage, pageCount, clipping.startWordIndex, clipping.endWordIndex, clipping.wordCount,
        chapterTitle.c_str(), paragraphIndex, clipping.text, layoutSignature, clipping.startOffset, clipping.endOffset);
    bool exported = false;
    if (addResult == ClippingStore::AddResult::Added) {
      exported = ClippingsManager::saveClipping(bookTitle, author, chapterTitle, startPage + 1, clipping.text);
    }
    clippingMessage = addResult == ClippingStore::AddResult::LimitReached ? StrId::STR_CLIPPING_LIMIT_REACHED
                      : exported                                          ? StrId::STR_CLIPPING_SAVED
                      : addResult == ClippingStore::AddResult::Added      ? StrId::STR_CLIPPING_EXPORT_FAILED
                                                                          : StrId::STR_CLIPPING_FAILED;
    showClippingMessage = true;
    clippingMessageTime = millis();
    requestUpdate();
  });
}

void EpubReaderActivity::openFootnoteSelect(const bool reopenMenuOnCancel) {
  if (!section || currentPageFootnotes.empty()) return;
  if (currentPageFootnotes.size() == 1) {
    navigateToHref(currentPageFootnotes[0].href, true);
    return;
  }

  auto page = section->loadPage(section->currentPage);
  if (!page) return;

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  auto selector = makeUniqueNoThrow<EpubReaderFootnoteSelectActivity>(renderer, mappedInput, std::move(page),
                                                                      orientedMarginLeft, orientedMarginTop);
  if (!selector) {
    LOG_ERR("ERA", "OOM: EpubReaderFootnoteSelectActivity");
    return;
  }
  startActivityForResult(std::move(selector), [this, reopenMenuOnCancel](const ActivityResult& result) {
    if (result.isCancelled) {
      if (reopenMenuOnCancel) {
        openReaderMenu();
      } else {
        requestUpdate();
      }
      return;
    }
    const auto& footnoteResult = std::get<FootnoteResult>(result.data);
    navigateToHref(footnoteResult.href, true);
    requestUpdate();
  });
}

void EpubReaderActivity::openClippings() {
  if (!CLIPPINGS.hasClippings()) {
    requestUpdate();
    return;
  }
  auto list = makeUniqueNoThrow<EpubReaderClippingListActivity>(renderer, mappedInput);
  if (!list) {
    LOG_ERR("CLIP", "OOM: clipping list");
    return;
  }
  startActivityForResult(std::move(list), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      if (const auto* progress = std::get_if<ProgressChangeResult>(&result.data)) {
        applyProgressChange(*progress);
        return;
      }
    }
    requestUpdate();
  });
}

namespace {
// FL1 sensor geometry. The side band is a thumb's reach from the bezel and sits
// inside the SDK's 25%-of-width edge anchor, so it never overlaps the page-turn
// tap zones' middle third; the vertical guards keep clear of the 14% top/bottom
// bands the light panel, the reader menu and Home already own.
constexpr int kEdgeDragBandPx = 44;
constexpr int kEdgeDragGuardTopPct = 20;
constexpr int kEdgeDragGuardBottomPct = 80;
// Vertical travel for one full sweep of the range: a comfortable wrist movement
// in the 400 px the guards leave, at ~4 px per percent.
constexpr int kEdgeDragSweepPx = 380;
constexpr int kEdgeDragBarWidth = 14;
constexpr int kEdgeDragBarInset = 6;
// The dimmest setting the light can hold; 0 is what the lamp switch is for
// (same floor as the frontlight panel).
constexpr int kEdgeDragMinBrightness = 1;
}  // namespace

// ---------------------------------------------------------------------------
// FL1 — side-edge frontlight drag. Default OFF (SETTINGS.edgeDragFrontlight).
//
// OFF costs one flag test per loop() pass: no touch query, no state, no arm
// zone, so no frame is swallowed and nothing polls the panel.
//
// The claim/no-claim decision is entirely in edgedrag::update() (see
// activities/util/EdgeDragStep.h): it only claims a contact that starts in a
// side band, moves vertically past the input layer's own tap-release slop, and
// stays vertically dominant. Everything else -- a tap in the band, a back
// swipe, a paging sweep -- is Abort, i.e. it falls through to the owners this
// reader already has.
//
// The repaint is SLIDE1's lane: each step is one diff-masked DU with the row-9
// completion suppressed and the shim's grey/clean cadence pinned off
// (inplacestep::armOwnedPaint), exactly what the frontlight panel's own slider
// does per percent. Without it every step would be a whole-panel GL16 flash.
bool EpubReaderActivity::handleEdgeDrag() {
  if (!SETTINGS.edgeDragFrontlight) return false;  // OFF: the gesture does not exist
  if (!mappedInput.hasTouch() || !Frontlight.present()) return false;
  // A reader-surface gesture, so it rides the reader's own master touch switch:
  // with touch reader controls off (the palm-on-glass mode) there is no drag.
  if (SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_OFF) return false;
  // The end-of-book menu and a lent framebuffer both own the surface; a lent
  // framebuffer must never be drawn on (see pageBufferStale).
  if (endOfBookMenuActive() || pageBufferStale.load()) {
    edgeDragDropSnapshot();  // never restore into a lent framebuffer
    edgedrag::reset(edgeDrag);
    return false;
  }
  // A page render landed mid-drag: it needed the renderer's single stored-BW
  // slot, so the snapshot is no longer the frame on the glass (and the slot may
  // now belong to the overlay path). Let the render own the screen.
  if (edgeDragPageStored && !edgeDragSnapshotCurrent()) {
    edgeDragPageStored = false;
    edgedrag::reset(edgeDrag);
    return false;
  }

  int downX = 0;
  int downY = 0;
  int curX = 0;
  int curY = 0;
  const bool pressed = mappedInput.wasScreenTouchPressed(downX, downY);
  const bool held = mappedInput.isScreenTouchHeld(curX, curY);
  // On the press frame the live sample is the down point; on the lift there is
  // no live sample and the machine only needs `held == false`.
  const int x = held ? curX : downX;
  const int y = held ? curY : downY;

  edgedrag::Config cfg;
  edgeDragFillConfig(cfg);
  const edgedrag::Action action = edgedrag::update(edgeDrag, cfg, pressed, held, x, y);

  // A right-edge drag on a panel with no colour temperature has nothing to set:
  // drop it rather than claiming the contact for a bar that changes nothing.
  if (edgeDrag.edge == edgedrag::Edge::Right && !Frontlight.hasColorTemperature()) {
    edgedrag::reset(edgeDrag);
    return false;
  }

  if (edgeDrag.active) {
    // A claimed drag and the auto page-turn timer both want this frame; the
    // finger wins, and it stays cancelled: a page turn under the bar would
    // replace the frame the snapshot restores from. A mere contact in the band
    // does NOT cancel it -- a tap there still means what it meant before.
    automaticPageTurnActive = false;
  }

  switch (action) {
    case edgedrag::Action::Engage:
      pendingManualTurn = 0;
      applyEdgeDragValue(/*persist=*/false);
      paintEdgeDrag(/*erase=*/false);
      return true;
    case edgedrag::Action::Step:
      applyEdgeDragValue(/*persist=*/false);
      paintEdgeDrag(/*erase=*/false);
      return true;
    case edgedrag::Action::Commit:
      applyEdgeDragValue(/*persist=*/true);
      paintEdgeDrag(/*erase=*/true);
      edgedrag::reset(edgeDrag);  // the gesture is over; nothing left armed
      return true;
    case edgedrag::Action::Abort:
      return false;  // a tap or a horizontal swipe: the other owners keep it
    case edgedrag::Action::None:
      break;
  }
  return edgeDrag.active;
}

void EpubReaderActivity::edgeDragFillConfig(edgedrag::Config& c) const {
  c.screenW = renderer.getScreenWidth();
  c.screenH = renderer.getScreenHeight();
  c.bandPx = kEdgeDragBandPx;
  c.guardTopPct = kEdgeDragGuardTopPct;
  c.guardBottomPct = kEdgeDragGuardBottomPct;
  // Claim exactly where the input layer stops calling the contact a tap: its
  // release slop is this configured threshold - 1 (InputManager::
  // touchTapReleaseSlopPx), so a claimed drag can never deliver a page-turn tap
  // on the same lift at ANY swipe-sensitivity setting.
  c.activatePx = SETTINGS.getSwipeThresholdPx();
  if (c.activatePx <= 0) c.activatePx = 90;
  c.sweepPx = kEdgeDragSweepPx;
  if (edgeDrag.edge == edgedrag::Edge::Right) {
    c.valueMin = 0;
    c.valueMax = 100;
    c.currentValue = Frontlight.warmth();
  } else {
    c.valueMin = kEdgeDragMinBrightness;
    c.valueMax = 100;
    c.currentValue = std::max(static_cast<int>(Frontlight.brightness()), kEdgeDragMinBrightness);
  }
}

void EpubReaderActivity::applyEdgeDragValue(const bool persist) {
  if (edgeDrag.edge == edgedrag::Edge::Left) {
    const uint8_t brightness =
        static_cast<uint8_t>(std::clamp(edgeDrag.value, kEdgeDragMinBrightness, 100));
    Frontlight.setBrightness(brightness);
    SETTINGS.frontlightBrightness = brightness;
    // Asking for a level asks for light: the frontlight panel does the same on a
    // slider move, and a slide switch bound to the light owns on/off instead.
    if (!Frontlight.isOn() && !slideSwitchOwnsLight()) {
      Frontlight.setOn(true);
      SETTINGS.frontlightOn = 1;
    }
  } else if (edgeDrag.edge == edgedrag::Edge::Right && Frontlight.hasColorTemperature()) {
    const uint8_t warmth = static_cast<uint8_t>(std::clamp(edgeDrag.value, 0, 100));
    Frontlight.setWarmth(warmth);
    SETTINGS.frontlightWarmth = warmth;
  } else {
    return;
  }
  // One flash write per gesture, on the lift -- never per step.
  if (persist) SETTINGS.saveToFile();
}

void EpubReaderActivity::drawEdgeDragBar() const {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int top = screenH * kEdgeDragGuardTopPct / 100;
  const int span = screenH * kEdgeDragGuardBottomPct / 100 - top;
  const int x = edgeDrag.edge == edgedrag::Edge::Left ? kEdgeDragBarInset
                                                      : screenW - kEdgeDragBarInset - kEdgeDragBarWidth;
  // The WHOLE track is repainted every step: card, frame, then fill from the
  // bottom, so each step is idempotent over the previous one and the DU diff is
  // only the pixels the level moved.
  renderer.fillRect(x, top, kEdgeDragBarWidth, span, false);
  renderer.drawRect(x, top, kEdgeDragBarWidth, span, true);
  const int lo = edgeDrag.edge == edgedrag::Edge::Right ? 0 : kEdgeDragMinBrightness;
  const int innerHeight = span - 6;
  const int filled = innerHeight * (std::clamp(edgeDrag.value, lo, 100) - lo) / (100 - lo);
  if (filled > 0) {
    renderer.fillRect(x + 3, top + 3 + innerHeight - filled, kEdgeDragBarWidth - 6, filled, true);
  }
}

void EpubReaderActivity::paintEdgeDrag(const bool erase) {
  RenderLock lock;  // the render task shares the framebuffer
  settleOverlayRefresh();
  if (erase) {
    if (!edgeDragSnapshotCurrent()) return;
    // The snapshot is the page as it was before the bar: put it back and drive
    // one DU, so the diff the panel already holds is exactly the bar area.
    // resyncPanelBaseline=false: the glass never showed this frame, it shows the
    // bar we are erasing, and the baseline must keep tracking what was pushed.
    renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
    edgeDragPageStored = false;
    inplacestep::armOwnedPaint(renderer);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  if (!edgeDragPageStored) {
    // The page the glass is showing. Same call the toolbar overlay uses to back
    // its own panel; if the heap cannot hold it, the drag still works -- it just
    // shows no bar.
    if (!renderer.hasFrameBuffer()) return;
    edgeDragPageStored = renderer.storeBwBuffer();
    if (!edgeDragPageStored) return;
    edgeDragStoreRenderMs = lastRenderCompleteMs;
  }
  drawEdgeDragBar();
  inplacestep::armOwnedPaint(renderer);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

bool EpubReaderActivity::edgeDragSnapshotCurrent() const {
  return edgeDragPageStored && edgeDragStoreRenderMs == lastRenderCompleteMs;
}

void EpubReaderActivity::edgeDragDropSnapshot() {
  if (!edgeDragPageStored) return;
  // Only free what is still ours: if a page render has happened since the store,
  // the renderer's single stored-BW slot now belongs to the overlay/page path and
  // freeing it would pull the page snapshot out from under that path.
  if (edgeDragStoreRenderMs == lastRenderCompleteMs) renderer.discardStoredBwBuffer();
  edgeDragPageStored = false;
}

void EpubReaderActivity::loop() {
  if (loadFailurePopup.isActive()) {
    loadFailurePopup.handleInput(mappedInput, [this] { requestUpdate(); });
    return;
  }
  if (!epub) {
    finish();
    return;
  }

  rememberBookOnceRendered();
  // r163 C2/C3: the background progress writes' completions, through their slots. Before any
  // input is handled, so a failure reaches the next paint.
  pollProgressWrites();

  // The radio stopped for a chapter build: once the render task has let go of the lock, that page
  // is on screen; once the build tick below has finished the chapter, no parser is left alive and
  // the radio may start again.
  if (radioReleasedForBuild.load()) {
    DisplayLockGuard displayGuard;
    if (displayGuard.owns() && !(section && section->isBuilding())) {
      radioReleasedForBuild = false;
      bleturner::afterPaint();
    }
  }

  // The remote linked, or failed to, after the status bar showed its note: paint the page again.
  {
    DisplayLockGuard displayGuard;
    if (displayGuard.owns() && linkNoteTitle.repaint(bleturner::linkNote())) requestUpdate();
  }

  // Someone else turned the screen while this reader was stacked (the control
  // center's orientation tile). Reflow before the next render, or the page
  // would be drawn with a layout built for the previous frame size.
  if (appliedOrientation != SETTINGS.orientation) {
    applyOrientation(SETTINGS.orientation);
    requestUpdate();
    return;
  }

  // Glyph lookahead lives at the no-input tail of loop() (idlePrewarmTick), after the build
  // tick: it is idle work, not something to run ahead of a queued turn.

  {
    DisplayLockGuard displayGuard;
    // Never next to the radio: once the reader reaches the watermark, the build starts in
    // renderBook(), which stops the radio first.
    if (displayGuard.owns() && section && !section->isBuilding() && section->isPartial() && buildViewportWidth > 0 &&
        !partialRebuildStartFailed && !bleturner::holdsHeap() &&
        section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount)) {
      const ReaderRenderSpec buildSpec = SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight);
      if (!section->startBuild(buildSpec)) {
        partialRebuildStartFailed = true;
        LOG_ERR("ERS", "Failed to start deferred partial extension build");
      } else {
        LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
                section->pageCount);
      }
    }
  }

  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();
  clearEndOfBookOptionsIfNeeded();

  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  const auto touch =
      ReaderUtils::detectTouchPageTurn(renderer, mappedInput, ReaderUtils::isRtlBookLanguage(epub->getLanguage()));

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (showDictionaryMessage && (millis() - dictionaryMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }

  if (showFontDamagedMessage && (millis() - fontDamagedMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showFontDamagedMessage = false;
    requestUpdate();
  }

  // The toolbar reader menu owns all input while shown, ahead of the automatic page turn
  // below: the More panel's rate popup switches automatic turning on and leaves the panel
  // open, so the timer must neither flip the page under it nor eat the panel's next
  // Confirm/Back release.
  if (overlay != Overlay::None) {
    if (usesToolbarMenu()) {
      // Hold the interval at zero elapsed so closing the panel starts a fresh one.
      lastPageTurnTime = millis();
      handleOverlayInput();
      return;
    }
    // The style was switched off while an overlay was up (Settings reached via
    // the More panel); fall back to the clean page.
    overlay = Overlay::None;
    discardOverlayPage();
    requestUpdate();
    return;
  }

  int selectionX = 0;
  int selectionY = 0;
  if (!atEndOfBook && SETTINGS.touchReaderControls && mappedInput.wasScreenLongPress(selectionX, selectionY)) {
    automaticPageTurnActive = false;
    pendingManualTurn = 0;
    startClipSelection(selectionX, selectionY);
    return;
  }

  // FL1: side-edge frontlight drag. Placed behind the overlay, the long-press
  // selection and the end-of-book menu (all of which own the frame above), and
  // AHEAD of the automatic page turn so a dragging finger cancels the timer.
  if (handleEdgeDrag()) return;

  switch (mappedInput.homeButtonAction()) {
    case HomeButtonAction::ReaderMenu:
    case HomeButtonAction::Bookmark:
    case HomeButtonAction::Sync:
    case HomeButtonAction::Dictionary:
    case HomeButtonAction::Footnotes:
    case HomeButtonAction::CreateClipping:
      automaticPageTurnActive = false;
      break;
    default:
      break;
  }

  if (automaticPageTurnActive) {
    const bool touchStopsAutoTurn = ReaderUtils::isTouchMenuGesture(renderer, mappedInput);
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) || touchStopsAutoTurn) {
      automaticPageTurnActive = false;
      if (touchStopsAutoTurn) haptic_feedback::deferTouchAction();
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      const bool succeeded = pageTurn(true);
      notePageTurn(true, succeeded);
      requestUpdate();
      return;
    }
  }

  if (showClippingMessage && (millis() - clippingMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showClippingMessage = false;
    requestUpdate();
  }

  // While the end-of-book suggestion menu is up it owns Confirm/Back/navigation, so it
  // gets this tick's input first and the long-press shortcuts below stay inert behind it
  // -- a hold there must not drop a bookmark onto the suggestion screen or paint the
  // dictionary word picker over it. Anything the menu does not handle (long-press Back to
  // the file browser, say) still falls through to the regular handlers.
  if (handleEndOfBookMenu()) {
    return;
  }
  const bool endOfBookMenuOpen = endOfBookMenuActive();

  const unsigned long confirmHoldMs = confirmLongPressThreshold();
  // wasLongPressed() suppresses the release that follows it, so leave it unpolled while
  // the end-of-book menu owns Confirm -- otherwise the menu never sees that release.
  const bool confirmLongPressed = !endOfBookMenuOpen && confirmHoldMs != 0 &&
                                  mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, confirmHoldMs);
  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (confirmLongPressed) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        addBookmark();
        showBookmarkMessage = true;
        bookmarkMessageTime = millis();
        requestUpdate();
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        if (launchKOReaderSync()) {
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        openDictionaryWordSelect();
        return;
      case CrossPointSettings::LP_MENU_CREATE_CLIPPING:
        startClipSelection();
        return;
      case CrossPointSettings::LP_MENU_FOOTNOTES:
        if (footnoteDepth > 0) {
          restoreSavedPosition();
        } else {
          openFootnoteSelect(false);
        }
        return;
      case CrossPointSettings::LP_MENU_READER_MENU:
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  if (!endOfBookMenuOpen) {
    switch (mappedInput.homeButtonAction()) {
      case HomeButtonAction::Bookmark:
        if (!showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        return;
      case HomeButtonAction::Sync:
        launchKOReaderSync();
        return;
      case HomeButtonAction::Dictionary:
        if (!showDictionaryMessage) openDictionaryWordSelect();
        return;
      case HomeButtonAction::CreateClipping:
        startClipSelection();
        return;
      case HomeButtonAction::ReaderMenu:
        if (usesToolbarMenu() && section)
          openOverlay(Overlay::Toolbar);
        else
          openReaderMenu();
        return;
      default:
        break;
    }
  }

  // Link taps take priority over the reader-menu and page-turn zones.
  if (!atEndOfBook && !currentPageLinks.empty() && SETTINGS.touchReaderControls && mappedInput.hasTouch()) {
    int touchX = 0;
    int touchY = 0;
    if (mappedInput.wasScreenTapped(touchX, touchY)) {
      const auto* link = EpubReaderUtils::linkAtPoint(currentPageLinks, touchX, touchY, currentPageLinkMarginLeft,
                                                      currentPageLinkMarginTop);
      if (link) {
        navigateToHref(link->href, true);
        haptic_feedback::deferTouchAction();
        return;
      }
    }
  }

  const bool touchMenu = ReaderUtils::isTouchMenuGesture(renderer, mappedInput);
  if (confirmReleased || touchMenu) {
    if (touchMenu) haptic_feedback::deferTouchAction();
    // Toolbar style: the page is on screen and in the framebuffer, so paint the
    // toolbar over it (one refresh) instead of pushing a full-screen menu.
    if (usesToolbarMenu() && section) {
      pendingManualTurn = 0;
      openOverlay(Overlay::Toolbar);
    } else {
      openReaderMenu();
    }
    return;  // no build step before the menu shows
  }

  // r163: a footnote's Back is the TAP, and nothing else. The held-time test that used to sit here
  // (getHeldTime() < GO_BACK_OR_HOME_MS) was the old inverted pair talking: a HOLD never reaches this
  // line any more, because ActivityManager consumes it while the finger is still down (goHome) and
  // suppresses its release. Any release that arrives is a tap -- including one whose measured held
  // time is long only because a blocking waveform swallowed the samples in the middle.
  if (footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    restoreSavedPosition();
    return;
  }

  if (handleBackNavigation()) {
    return;
  }

  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::CREATE_CLIPPING &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    startClipSelection();
    return;
  }

  if ((!endOfBookMenuOpen && mappedInput.homeButtonAction() == HomeButtonAction::Footnotes) ||
      (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
       mappedInput.wasReleased(MappedInputManager::Button::Power) &&
       !mappedInput.wasReleased(MappedInputManager::Button::Down))) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      openFootnoteSelect(false);
    }
    return;
  }

  constexpr unsigned long kMinManualTurnGapMs = 200;
  const bool turnGuardActive = RenderLock::peek() || (millis() - lastPageTurnTime) < kMinManualTurnGapMs;
  if (pendingManualTurn != 0 && !turnGuardActive) {
    if (!section) {
      pendingManualTurn = 0;
      return;
    }
    const bool forward = pendingManualTurn > 0;
    pendingManualTurn = 0;
    const bool succeeded = pageTurn(forward);
    // A queued repeat that could not move is a step failure: stop for the rest of
    // the press instead of re-queueing the same turn ten times a second.
    if (!succeeded && pendingManualTurnRepeat) pageRepeat.stop();
    pendingManualTurnRepeat = false;
    notePageTurn(forward, succeeded);
    // r163 3.14: the turn was accepted and did not move -- the ERROR cue (3 short pulses).
    if (!succeeded) haptic_feedback::deferErrorAction();
    if (succeeded && pendingManualTurnTouch) haptic_feedback::deferPageTurnAction();
    pendingManualTurnTouch = false;
    requestUpdate();
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;

  // r163 F5: press-anchored hold repeat (ReaderActivity::loop has the same block
  // and the rationale). A repeat tick is submitted to the manual-turn queue, so it
  // obeys the 200 ms guard and the RenderLock instead of painting under a live
  // render; a tick that arrives while one is still queued cancels the press rather
  // than stacking pages.
  const bool repeatEligible = SETTINGS.longPressButtonBehavior == CrossPointSettings::OFF && !fromTilt &&
                              !touch.prev && !touch.next;
  bool repeatTick = false;
  if (repeatEligible) {
    const int heldDir = ReaderUtils::heldPageTurnDirection(mappedInput);
    if (heldDir == 0) {
      pageRepeat.reset();
    } else if (!pageRepeat.armed() || pageRepeat.direction() != heldDir) {
      pageRepeat.arm(heldDir, millis() - mappedInput.getHeldTime());
    } else {
      repeatTick = pageRepeat.due(millis());
    }
  } else {
    pageRepeat.reset();
  }

  if (!prevTriggered && !nextTriggered && !repeatTick) {
    // Nothing to act on in this frame: settle a coalesced progress save first (a submit, not a
    // write), then build more of the chapter, and only if that did nothing, warm the glyphs of the
    // pages ahead. Never both of the last two on one frame -- the build owns the heap and the card
    // when it runs, which is exactly what the lookahead must not compete with.
    progressFlushTick();
    // r163 C1: preparing the next chapter's first page outranks the glyph lookahead (it is one
    // attempt per chapter, and it is what the next chapter open would otherwise pay). It answers
    // false on a frame where it did nothing, so the lookahead still gets its frame.
    if (!backgroundBuildTick()) {
      if (!chapterPrefetchTick()) idlePrewarmTick();
    }
    return;
  }

  if (handleEndOfBookPageTurn(repeatTick ? pageRepeat.direction() < 0 : prevTriggered,
                              repeatTick ? pageRepeat.direction() > 0 : nextTriggered)) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    return;
  }

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool longPress = !fromTilt && heldMs >= ReaderUtils::SKIP_HOLD_MS;
  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    const bool succeeded = skipPages(nextTriggered ? 1 : -1);
    notePageTurn(false, succeeded);
    if (!succeeded) haptic_feedback::deferErrorAction();  // r163 3.14: same ERROR class
    if (succeeded && (touch.prev || touch.next)) haptic_feedback::deferTouchAction(true);
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    if (touch.prev || touch.next) haptic_feedback::deferTouchAction(true);
    requestUpdate();
    return;
  }

  if (!section) {
    requestUpdate();
    return;
  }

  if (repeatTick) {
    // Queue the repeat like a press would: the guard below is what stops it
    // outrunning the panel. A tick that lands on top of a queued turn cancels the
    // press -- the backlog is dropped, never stacked.
    if (pendingManualTurn != 0) {
      pageRepeat.stop();
      requestUpdate();
      return;
    }
    pendingManualTurn = pageRepeat.direction() > 0 ? 1 : -1;
    pendingManualTurnTouch = false;
    pendingManualTurnRepeat = true;
    requestUpdate();
    return;
  }

  if (turnGuardActive) {
    pendingManualTurn = prevTriggered ? -1 : 1;
    pendingManualTurnTouch = touch.prev || touch.next;
    pendingManualTurnRepeat = false;  // an edge turn, not a repeat tick
    return;
  }

  const bool succeeded = pageTurn(!prevTriggered);
  notePageTurn(!prevTriggered, succeeded);
  if (!succeeded) haptic_feedback::deferErrorAction();  // r163 3.14: same ERROR class
  if (succeeded && (touch.prev || touch.next)) haptic_feedback::deferPageTurnAction();
  requestUpdate();
}

void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) return;
  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) return;

  percent = clampPercent(percent);

  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) targetSize = bookSize - 1;

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) return;

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  const int found = epub->getSpineIndexForSize(targetSize);
  if (found >= 0) {
    targetSpineIndex = found;
    prevCumulative = (found > 0) ? epub->getCumulativeSpineItemSize(found - 1) : 0;
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  pendingSpineProgress = std::clamp(pendingSpineProgress, 0.0f, 1.0f);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::applyProgressChange(const ProgressChangeResult& sync) {
  if (sync.hasVisibleTextOffset && sync.spineIndex >= 0 && sync.spineIndex < epub->getSpineItemsCount()) {
    RenderLock lock;
    clearDeferredReposition();
    const auto page = section && currentSpineIndex == sync.spineIndex
                          ? section->getPageForVisibleTextOffset(sync.visibleTextOffset)
                          : std::nullopt;
    if (page) {
      section->currentPage = *page;
    } else {
      currentSpineIndex = sync.spineIndex;
      pendingOffsetJump = sync.visibleTextOffset;
      nextPageNumber = std::max(0, sync.page);
      section.reset();
    }
    requestUpdate();
    return;
  }

  int targetSpineIndex = sync.spineIndex;
  int targetPage = sync.page;
  const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
  const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 && currentSpineIndex == sync.spineIndex &&
                                              sync.page >= 0 && sync.page < sync.totalPages &&
                                              activeTotalPages == sync.totalPages;

  if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
    const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
    CrossPointPosition fallback =
        ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
    targetSpineIndex = fallback.spineIndex;
    targetPage = fallback.pageNumber;
  }

  RenderLock lock;
  clearDeferredReposition();

  if (currentSpineIndex != targetSpineIndex) {
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = targetPage;
    section.reset();
  } else if (section && section->currentPage != targetPage) {
    const int clampedTargetPage = std::max(0, targetPage);
    section->currentPage = clampedTargetPage;
  } else if (!section) {
    nextPageNumber = targetPage;
  }
  requestUpdate();
}

int EpubReaderActivity::currentTocIndex() const {
  if (!epub) return 0;
  if (section) return std::max(0, section->getTocIndexForPage(section->currentPage));
  // Child screens may have released the section; its saved anchor map remains readable.
  Section cachedSection(epub, currentSpineIndex, renderer);
  return std::max(0, cachedSection.getTocIndexForPage(nextPageNumber));
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (result.isCancelled) {
      openReaderMenu();
    } else {
      const auto& sync = std::get<ProgressChangeResult>(result.data);

      applyProgressChange(sync);
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      int tocIndex;
      // Release the section while the chapter list is up (mirrors the
      // TEXT_SETTINGS path): picking a chapter resets it anyway, and its
      // tens-of-KB footprint is the difference between the chapter list
      // holding its CJK glyph arena (RAM-only repaints) and re-reading
      // glyphs from SD on every row step. Cancel restores via the same
      // cached-position rebuild TEXT_SETTINGS uses.
      {
        RenderLock lock;
        tocIndex = currentTocIndex();
        if (section) {
          rememberCurrentContentOffset();
          cachedSpineIndex = currentSpineIndex;
          cachedChapterTotalPageCount = section->pageCount;
          nextPageNumber = section->currentPage;
        }
        section.reset();
      }
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, tocIndex),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              openReaderMenu();
              return;
            }
            const auto& chapterResult = std::get<ChapterResult>(result.data);
            RenderLock lock;
            clearDeferredReposition();
            currentSpineIndex = chapterResult.spineIndex;
            pendingAnchor = chapterResult.anchor;
            nextPageNumber = 0;
            section.reset();
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      openFootnoteSelect(true);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TEXT_SETTINGS: {
      startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                    TextSettingsActivity::Tab::Family),
                             [this](const ActivityResult&) {
                               {
                                 RenderLock lock;
                                 if (section) {
                                   rememberCurrentContentOffset();
                                   cachedSpineIndex = currentSpineIndex;
                                   cachedChapterTotalPageCount = section->pageCount;
                                   nextPageNumber = section->currentPage;
                                 }
                                 section.reset();
                               }
                               openReaderMenu();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BLUETOOTH:
    case EpubReaderMenuActivity::MenuAction::NIGHT_MODE:
      // Handled in-place by EpubReaderMenuActivity so its On/Off value updates
      // without closing the menu.
      break;
    case EpubReaderMenuActivity::MenuAction::FRONTLIGHT:
      // Handled in-place by EpubReaderMenuActivity using the live frontlight HAL.
      break;
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      const int initialPercent = bookPercentFor(chapterPosition());
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              openReaderMenu();
            } else {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY: {
      openDictionaryWordSelect();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VOCAB_HISTORY: {
      // r163 B6: a plain list screen over the persisted store; returning from it reopens
      // the menu, exactly as the percentage picker does.
      startActivityForResult(std::make_unique<VocabHistoryActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { openReaderMenu(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult&) { openReaderMenu(); });
          break;
        }
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock;
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgressNow(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock;
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SAVE_CLIPPING: {
      startClipSelection();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VIEW_CLIPPINGS: {
      openClippings();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
  }
}

unsigned long EpubReaderActivity::confirmLongPressThreshold() const {
  // The setting is hidden on these boards, so a stale stored value must not act.
  if (BoardConfig::hasTouch()) return 0;
  switch (SETTINGS.longPressMenuFunction) {
    case CrossPointSettings::LP_MENU_BOOKMARK:
    case CrossPointSettings::LP_MENU_DICTIONARY:
    case CrossPointSettings::LP_MENU_CREATE_CLIPPING:
    case CrossPointSettings::LP_MENU_FOOTNOTES:
      return ReaderUtils::BOOKMARK_HOLD_MS;
    case CrossPointSettings::LP_MENU_KOSYNC:
      return KOREADER_STORE.hasCredentials() ? ReaderUtils::GO_HOME_MS : 0;
    case CrossPointSettings::LP_MENU_READER_MENU:
    case CrossPointSettings::LP_MENU_DISABLED:
    default:
      return 0;
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;

  RenderLock renderLock;

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;

  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos;
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  // Durable before the book is released: the sync then hands this position to the server, and
  // the activity keeps nothing to retry from.
  if (!saveProgressNow(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;
  }

  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    if (section) {
      nextPageNumber = section->currentPage;
    }
    discardOverlayPage();
    ImageBlock::releaseRenderCache();
    ImageBlock::setExtractor(nullptr, nullptr);
    section.reset();
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseSdFontCaches();
    }
    // No rendering may run while the chapter mapper borrows the framebuffer.
    {
      GfxRenderer::FrameBufferLoan loan(renderer);
      localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
    }
    // The destructor can no longer save the back-stack once epub is gone.
    if (footnoteDepth > 0) saveLinkStack();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, localPos, std::move(localKoPos), std::move(localChapterName)));
  return true;
}

void EpubReaderActivity::applyInitialOrientation() {
  ReaderActivity::applyInitialOrientation();
  appliedOrientation = SETTINGS.orientation;
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // Also runs when SETTINGS already holds the new value but this layout was
  // built for the old one — that is what an external change looks like here.
  if (SETTINGS.orientation == orientation && appliedOrientation == orientation) {
    return;
  }

  RenderLock lock(*this);
  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }

  if (SETTINGS.orientation != orientation) {
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();
  }
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  appliedOrientation = orientation;
  section.reset();
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    RenderLock lock;
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

bool EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (!section) return false;
  // A "Bluetooth failed" note has been read: the page this turn paints shows the title again.
  bleturner::acknowledgeLinkNote();
  {
    RenderLock lock;
    clearDeferredReposition();
  }
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1 || section->isBuilding()) {
      section->currentPage++;
      lastPageTurnTime = millis();
      uisounds::onEvent(uisounds::Event::PageTurn, uisounds::Tag::PageTurn);
      return true;
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex++;
      section.reset();
      lastPageTurnTime = millis();
      uisounds::onEvent(uisounds::Event::PageTurn, uisounds::Tag::PageTurn);
      return true;
    } else {
      currentSpineIndex = epub->getSpineItemsCount();
      lastPageTurnTime = millis();
      uisounds::onEvent(uisounds::Event::PageTurn, uisounds::Tag::PageTurn);
      return true;
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
      lastPageTurnTime = millis();
      uisounds::onEvent(uisounds::Event::PageTurn, uisounds::Tag::PageTurn);
      return true;
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      currentSpineIndex--;
      section.reset();
      lastPageTurnTime = millis();
      uisounds::onEvent(uisounds::Event::PageTurn, uisounds::Tag::PageTurn);
      return true;
    }
  }
  // AUDIO1: the turn was refused (top of the book, backwards, or no section).
  uisounds::onEvent(uisounds::Event::Error, uisounds::Tag::PageTurn);
  return false;
}

bool EpubReaderActivity::skipPages(int amount) {
  if (!section) return false;
  bleturner::acknowledgeLinkNote();
  if (amount > 0) {
    RenderLock lock;
    nextPageNumber = 0;
    currentSpineIndex++;
    section.reset();
    return true;
  } else {
    if (section->currentPage > 0) {
      section->currentPage = 0;
      return true;
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex--;
      section.reset();
      return true;
    }
  }
  return false;
}

// Failed protected open: show the standard option dialog (wrapped message)
// instead of silently falling back to the previous screen. Exact error
// strings are set by openProtectedBook (ContentProtection.cpp).
bool EpubReaderActivity::handleLoadFailure() {
  if (loadProtectionError.empty()) return false;
  const std::string& perr = loadProtectionError;
  StrId msg = StrId::STR_DRM_PROTECTED_FILE;
  bool offerSync = false;
  if (perr == "access expired") {
    msg = StrId::STR_LOAN_EXPIRED;
  } else if (perr == "loan date unverified") {
    msg = StrId::STR_LOAN_TIME_UNVERIFIED;
    offerSync = true;
  }
  const char* options[2] = {I18N.get(offerSync ? StrId::STR_CLOCK_SYNC_NOW : StrId::STR_OK_BUTTON),
                            I18N.get(StrId::STR_OK_BUTTON)};
  loadFailurePopup.showMessage("", I18N.get(msg), options, offerSync ? 2 : 1, 0, [this, offerSync](const int index) {
    if (offerSync && index == 0) {
      beginLoanTimeSync();
      return;
    }
    finish();
  });
  requestUpdate();
  return true;  // stay alive; the popup's Back dismiss lands in loop()'s !epub finish
}

void EpubReaderActivity::beginLoanTimeSync() {
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("ERS", "OOM: Wi-Fi selection for loan time sync");
    finish();
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
      finish();
      return;
    }
    // One call, one commit: arm FULL immediately before the popup. drawPopup() parks and re-arms
    // (r49), so this promotion survives the popup's own HALF paint and lands on the frame after it.
    renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
    GUI.drawPopup(renderer, tr(STR_SYNCING_TIME));
    const bool synced = trustedtime::syncNow(5000);
    WiFi.disconnect(false);
    delay(30);
    if (!synced) {
      // Reopening would hit the same unverified-time refusal: offer a retry.
      const char* options[2] = {I18N.get(StrId::STR_RETRY), I18N.get(StrId::STR_OK_BUTTON)};
      loadFailurePopup.showMessage("", I18N.get(StrId::STR_CLOCK_SYNC_FAIL), options, 2, 0, [this](const int index) {
        if (index == 0) {
          beginLoanTimeSync();
          return;
        }
        finish();
      });
      requestUpdate();
      return;
    }
    APP_STATE.openEpubPath = bookPath;
    APP_STATE.saveToFile();
    // Reboot straight back into this book with a clean heap (no-op on touch
    // boards, which fall through to the in-place relaunch below).
    silentRestartToReader();
    activityManager.goToReader(bookPath);
  });
}

void EpubReaderActivity::render(RenderLock&& lock) {
  if (loadFailurePopup.isActive()) {
    renderer.clearScreen();
    loadFailurePopup.processRender(renderer, mappedInput);
    return;
  }
  ReaderActivity::render(std::move(lock));
}

bool EpubReaderActivity::remoteShortcut(const bool readerMenu) {
  if (!section) return false;
  automaticPageTurnActive = false;
  if (!readerMenu) {
    startClipSelection();
  } else if (usesToolbarMenu()) {
    openOverlay(Overlay::Toolbar);
  } else {
    openReaderMenu();
  }
  return true;
}

bool EpubReaderActivity::isAtEndOfBook() const { return epub && currentSpineIndex >= epub->getSpineItemsCount(); }

void EpubReaderActivity::onReturnFromEndOfBook() {
  if (epub && epub->getSpineItemsCount() > 0) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = 0;
    pendingPageJump = std::numeric_limits<uint16_t>::max();
  }
}

// A started build runs to the end of the chapter, one bounded step per idle loop frame, so the
// parser and its page LUT are freed soon (and a radio stopped for the build comes back with no
// parser alive). Not while the radio holds the heap, and not in a spine whose background build
// failed.
bool EpubReaderActivity::backgroundBuildWanted() const {
  return section && section->isBuilding() && !bleturner::holdsHeap() && currentSpineIndex != backgroundBuildFailedSpine;
}

bool EpubReaderActivity::skipLoopDelay() {
  // The main loop holds the render lock while querying this hint. It keeps yielding while a button
  // is down: a press that starts inside a long build step gets its second debounce sample ~5 ms
  // after the step instead of after a 10 ms sleep.
  return overlay == Overlay::None && !buildHeapPaused && backgroundBuildWanted();
}

void EpubReaderActivity::renderBook() {
  currentPageLinks.clear();
  if (!epub) return;
  // A radio start in flight takes ~50 KB while the stack comes up: the page waits for it rather than
  // take the heap beside it (X3: abort() in a page restore at free 22,676 B). About 20 ms, once a visit.
  bleturner::settleStart();
  // Runs under the render task's RenderLock; catches every requestUpdate()
  // exit from the overlay while its deferred chrome refresh is still pending.
  settleOverlayRefresh();

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  const auto showBuildError = [this](const char* message = nullptr) {
    renderer.clearScreen();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    GUI.drawPopup(renderer, message ? message : tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };

  if (currentSpineIndex < 0) currentSpineIndex = 0;
  if (currentSpineIndex > epub->getSpineItemsCount()) currentSpineIndex = epub->getSpineItemsCount();

  if (currentSpineIndex == epub->getSpineItemsCount()) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);

  // r163 C3: the session this paint describes. A different Section, spine or layout invalidates
  // every result prepared for the previous one; a repaint of the same page in the same chapter
  // (status bar, overlay, screenshot) is not a new session, so idle work survives it.
  sessionGeneration_.observe(
      {section.get(), currentSpineIndex, readerRenderSpecSignature(renderSpec)});

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    // r163 C1: a chapter the idle loop already laid out opens from that file -- the section file
    // of its first pages is already where a build would have put it, so the open takes the
    // deferred-extension path below instead of inflating, parsing and laying out. Returns false
    // for every case it does not own, and then nothing has changed.
    adoptPreparedChapter();
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    partialRebuildStartFailed = false;

    const bool cacheLoaded = section->loadSectionFile(renderSpec);
    if (cacheLoaded) {
      cachedChapterTotalPageCount = 0;
      cachedVisibleTextOffset.reset();
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    const bool explicitOffsetJump = pendingOffsetJump.has_value();
    const std::optional<uint32_t> offsetJump =
        explicitOffsetJump ? pendingOffsetJump
        : (pendingPageJump.has_value() || !pendingAnchor.empty() || currentSpineIndex != cachedSpineIndex)
            ? std::nullopt
            : cachedVisibleTextOffset;
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        pagesUntilFullRefresh = 1;
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        if (!makeRoomForChapterBuild()) {
          section.reset();
          showBuildError(tr(STR_BT_BUILD_REFUSED));
          return;
        }
        GfxRenderer::FrameBufferLoan loan(renderer);
        if (!section->createSectionFile(renderSpec, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          section.reset();
          loan.end();
          showBuildError();
          return;
        }
        loan.end();
      } else {
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        if (section->isPartial() &&
            (anchorJump ? section->getPageForAnchor(pendingAnchor).has_value()
                        : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            pagesUntilFullRefresh = 1;
          }
          buildPopupPending = !showPopup;
          // Section (re)builds are the heap-hungriest path (per-word
          // allocations for the whole section). Under TTF heap pressure, shed
          // every rebuildable font cache first — dropped glyphs re-fault on
          // demand after the build. Skipped for cpfont/builtin reading: the
          // release drops the persistent advance table and mini tables, which
          // would force SD metric re-reads on every fresh chapter for no gain.
          if (!renderer.getTtfFonts().empty()) {
            if (auto* fcm = renderer.getFontCacheManager()) {
              fcm->releaseSdFontCaches();
            }
          }
          if (!makeRoomForChapterBuild()) {
            section.reset();
            buildPopupPending = false;
            showBuildError(tr(STR_BT_BUILD_REFUSED));
            return;
          }
          LOG_DBG("ERS", "Heap before section build: %u (max block %u)", (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
          const unsigned long buildStartMs = millis();
          bool started;
          {
            GfxRenderer::FrameBufferLoan loan(renderer);
            started = section->startBuild(renderSpec, [this] { showBuildPopup(renderer, pagesUntilFullRefresh); });
          }
          if (!started) {
            LOG_ERR("ERS", "Failed to start section build");
            section.reset();
            buildPopupPending = false;
            showBuildError();
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump               ? !section->findAnchor(pendingAnchor)
                  : offsetJump.has_value() ? !section->buildReachedVisibleTextOffset(*offsetJump)
                                           : static_cast<int>(section->pageCount) <= target)) {
            if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
              showBuildPopup(renderer, pagesUntilFullRefresh);
            }
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              section.reset();
              buildPopupPending = false;
              showBuildError();
              return;
            }
          }
          buildPopupPending = false;
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) section->currentPage = 0;
    }

    if (offsetJump.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*offsetJump)) {
        section->currentPage = *offsetPage;
        clearDeferredReposition();
      }
    }
    if (explicitOffsetJump) {
      clearDeferredReposition();
    }
    pendingOffsetJump.reset();

    if (!pendingAnchor.empty()) {
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) newPage = section->pageCount - 1;
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  // The two loops below build on until the page to show exists.
  if ((section->isPartial() || section->isBuilding()) && section->currentPage >= static_cast<int>(section->pageCount) &&
      !makeRoomForChapterBuild()) {
    section.reset();
    showBuildError(tr(STR_BT_BUILD_REFUSED));
    return;
  }
  if (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
  }
  while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    if (!section->isBuilding() && !section->startBuild(renderSpec)) {
      LOG_ERR("ERS", "Failed to start partial extension build");
      section.reset();
      showBuildError();
      return;
    }
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }
  if (section->isBuilding()) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }

  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  applyDeferredReposition();

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    pageBufferStale = false;
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  {
    // r97 Phase 2 (a): the compose split's page-load bucket. One timer pair per
    // page load, at the call boundary -- Section::loadPage() reads the cached
    // page file off SD and deserialises every element, so this is the whole
    // "load/page deserialise" phase and nothing below it is instrumented
    // per element.
#if CP_CPU_STATS
    const int64_t loadStartUs = cpustats::nowUs();
#endif
    auto p = section->loadPage(section->currentPage);
#if CP_CPU_STATS
    cpustats::notePageLoadUs(cpustats::nowUs() - loadStartUs);
#endif
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;

    currentPageVisibleOffset = p->visibleTextOffset;
    currentPageFootnotes = std::move(p->footnotes);
    currentPageLinks = std::move(p->links);
    currentPageLinkMarginLeft = orientedMarginLeft;
    currentPageLinkMarginTop = orientedMarginTop;

    // The overlay and non-tiled grayscale renderer share the renderer's single
    // stored-BW slot. Release the old page snapshot before renderContents()
    // needs that slot, then snapshot the newly rendered page below.
    discardOverlayPage();

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
    lastRenderCompleteMs = millis();
    markPageRendered();
  }

  if (currentSpineIndex != lastSavedSpineIndex || section->currentPage != lastSavedPage ||
      section->pageCount != lastSavedPageCount) {
    // Hoisted: the probe runs the build's EMA smoothing per call, so read it once.
    const uint16_t chapterTotalPages = section->estimatedTotalPages();
    if (saveProgress(currentSpineIndex, section->currentPage, chapterTotalPages)) {
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = section->currentPage;
      lastSavedPageCount = chapterTotalPages;
    }
  }

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
  }

  // Glyphs of a damaged .cpfont draw as blanks; say why, once per loaded font.
  if (SdCardFont::takeCorruptGlyphNotice()) {
    showFontDamagedMessage = true;
    fontDamagedMessageTime = millis();
  }
  if (showFontDamagedMessage) {
    GUI.drawPopup(renderer, tr(STR_FONT_DAMAGED));
  }

  if (showClippingMessage) {
    GUI.drawPopup(renderer, I18N.get(clippingMessage));
  }

  pageBufferStale = false;  // the page is back in the framebuffer

  // Toolbar menu: overlay the toolbar / panel on top of the freshly rendered page.
  if (overlay != Overlay::None && usesToolbarMenu()) {
    // The page just re-rendered under the overlay: refresh the snapshot that
    // backs panel->toolbar restores (any previous copy is stale).
    overlayPageStored = renderer.storeBwBuffer();
    renderOverlay();
    // An open option picker rides on top of the freshly drawn panel.
    if (overlayPopup.isActive()) overlayPopup.render(renderer);
    // FAST, same as openOverlay: HALF's inverting pass flashes the sheet
    // (white, in night mode) on every repaint under an open panel. Any AA
    // residue a FAST differential leaves under the chrome has not shown in
    // practice; restore a HALF cleanup here if text ever visibly ghosts
    // through the sheet (see #2190 for the mechanism).
    pushOverlayRefresh();
  }
}

void EpubReaderActivity::onEndOfBookRendered() {
  automaticPageTurnActive = false;
  if (pendingSyncSaveError) {
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  }
}

bool EpubReaderActivity::applyDeferredReposition() {
  if ((!cachedVisibleTextOffset.has_value() && cachedChapterTotalPageCount == 0) || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  if (currentSpineIndex == cachedSpineIndex) {
    int newPage = section->currentPage;
    bool mappedOffset = false;
    if (cachedVisibleTextOffset.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*cachedVisibleTextOffset)) {
        newPage = *offsetPage;
        mappedOffset = true;
      }
    }
    if (!mappedOffset && cachedChapterTotalPageCount > 0 && section->pageCount != cachedChapterTotalPageCount) {
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
      newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    }
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
  }
  clearDeferredReposition();
  return changed;
}

void EpubReaderActivity::clearDeferredReposition() {
  cachedChapterTotalPageCount = 0;
  cachedVisibleTextOffset.reset();
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  // End of book is represented as spineIndex == spineCount (see pageTurn), so a
  // save that lands there would persist an index no section owns. On resume the
  // load path clamps it back to spineCount and re-enters the end-of-book screen:
  // the reader loses the page it was actually on. Persist the LAST real section
  // instead -- the page number saved alongside it is still that section's page.
  const int spineCount = epub->getSpineItemsCount();
  if (spineCount > 0 && spineIndex >= spineCount) {
    LOG_DBG("ERS", "Progress save at end of book: clamping spine %d -> %d", spineIndex, spineCount - 1);
    spineIndex = spineCount - 1;
  }

  if (currentPage < 0 || currentPage > 0xFFFF || pageCount < 0 || pageCount > 0xFFFF || spineIndex < 0 ||
      spineIndex > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, currentPage, pageCount);
    return false;
  }

  // r163 C4: ACCEPTED, not written. The position merges into this book's pending snapshot, so ten
  // page turns before the write lands cost one write of the tenth position. `false` means the
  // snapshot could not even be taken (every slot held by another book's running write) and the
  // caller must not believe the page was remembered.
  progressrecord::Record record;
  record.spineIndex = static_cast<uint16_t>(spineIndex);
  record.pageNumber = static_cast<uint16_t>(currentPage);
  record.pageCount = static_cast<uint16_t>(pageCount);
  if (section && spineIndex == currentSpineIndex && currentPage < section->pageCount) {
    const std::optional<uint32_t> offset =
        (currentPage == section->currentPage && currentPageVisibleOffset.has_value())
            ? currentPageVisibleOffset
            : section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
    if (offset.has_value()) {
      record.visibleTextOffset = *offset;
      record.hasVisibleTextOffset = true;
    }
  }
  const bool accepted = readerprogress::ProgressStore::get().accept(
      epub->getCachePath().c_str(), record, sessionGeneration_.current(), static_cast<uint32_t>(millis()));
  if (accepted) {
    lastProgressAcceptMs.store(static_cast<uint32_t>(millis()), std::memory_order_release);
  } else if (!progressRefusalLogged) {
    // A refusal is not a save. The render path leaves lastSaved* alone and retries on the next
    // paint; the durable callers get a false and stop. And the reader is told once per episode --
    // a slot set that refused this position will refuse the next one too.
    LOG_ERR("PRG", "Progress snapshot refused: every book slot holds a position that is not on the card yet");
    progressRefusalLogged = true;
    // Read by the same paint (showPendingSyncSaveError) and by every later one, so no repaint is
    // requested from here: this is the render path, and a request from inside it is a loop.
    pendingSyncSaveError = true;
  }
  return accepted;
}

// r163 C4: the DURABLE variant of saveProgress, for the two paths that cannot continue on a
// pending snapshot: the KOReader handover releases the book (and keeps nothing to retry from), and
// the cache clear deletes the directory this record lives in. It accepts the position into this
// book's pending snapshot and then insists that snapshot reaches the card before it returns.
//
// IT BLOCKS, and that is the difference from saveProgress: flushAcceptedProgress(true) hands the
// snapshot to the writer task and waits up to PROGRESS_FLUSH_WAIT_MS for the completion, then
// writes it here on this task when the writer refused the job or did not reach it in time. The
// worst case is that wait plus one 10-byte write and its rename -- the cost this path already paid
// when it wrote synchronously. A false means the snapshot could not even be accepted, or the card
// refused it; every caller treats a false as "do not continue".
bool EpubReaderActivity::saveProgressNow(int spineIndex, int currentPage, int pageCount) {
  if (!epub) return false;
  if (!saveProgress(spineIndex, currentPage, pageCount)) return false;
  return flushAcceptedProgress(true);
}

// r163 C4: hand the pending snapshot to the background writer. `wait` is the exit path: there is no
// next tick to retry on, so the position is written here if the queue refuses it or the writer does
// not reach it in time.
bool EpubReaderActivity::flushAcceptedProgress(const bool wait) {
  if (!epub) return false;
  auto& store = readerprogress::ProgressStore::get();
  const std::string& cachePath = epub->getCachePath();
  const char* path = cachePath.c_str();

  readerprogress::ProgressSaves::Handoff handoff;
  if (store.take(path, handoff)) {
    readerprogress::WriteJob job;
    if (cachePath.size() < sizeof(job.cachePath)) {
      std::memcpy(job.cachePath, path, cachePath.size() + 1);
      job.spineIndex = handoff.record.spineIndex;
      job.pageNumber = handoff.record.pageNumber;
      job.pageCount = handoff.record.pageCount;
      job.visibleTextOffset = handoff.record.visibleTextOffset;
      job.hasVisibleTextOffset = handoff.record.hasVisibleTextOffset;
      job.generation = handoff.generation;
      job.token = handoff.token;
      job.slot = handoff.slot;
      if (!readerprogress::ProgressWriter::get().submit(job)) {
        // Refused by the bounded queue. The snapshot stays pending and is retried on the next idle
        // frame; on the exit path there is no next frame, so it is written here instead.
        store.release(handoff.slot, handoff.token);
        if (!progressRefusalLogged) {
          LOG_ERR("PRG", "Progress writer refused a job; the position stays pending");
          progressRefusalLogged = true;
        }
        if (!wait) return false;
        const bool written = ProgressFile::write(cachePath, handoff.record);
        store.complete(handoff.slot, handoff.token, written);
        return written;
      }
    } else {
      // A cache path longer than the job's buffer: it cannot be carried to the writer task.
      store.release(handoff.slot, handoff.token);
      if (!wait) return false;
      const bool written = ProgressFile::write(cachePath, handoff.record);
      store.complete(handoff.slot, handoff.token, written);
      return written;
    }
  }

  if (!wait) return true;
  waitForProgressFlush();
  // One lock, one answer: asking dirty() and then inFlight() is two windows onto a state the
  // writer task is changing, and the pair that comes back may never have existed.
  const auto pending = store.state(path);
  if (!pending.dirty) return true;
  if (pending.inFlight) {
    // The writer still holds it; it will land or report through its completion. Writing a second
    // copy here would race the first on progress.bin.tmp.
    LOG_ERR("PRG", "Progress write still in flight after %u ms; leaving it to the writer",
            static_cast<unsigned>(PROGRESS_FLUSH_WAIT_MS));
    return false;
  }
  // Pending and nobody is writing it: write it now, on this task.
  if (!store.take(path, handoff)) return false;
  const bool written = ProgressFile::write(cachePath, handoff.record);
  store.complete(handoff.slot, handoff.token, written);
  return written;
}

void EpubReaderActivity::waitForProgressFlush() {
  if (!epub) return;
  auto& store = readerprogress::ProgressStore::get();
  const char* path = epub->getCachePath().c_str();
  const uint32_t deadline = static_cast<uint32_t>(millis()) + static_cast<uint32_t>(PROGRESS_FLUSH_WAIT_MS);
  while (store.dirty(path) && static_cast<uint32_t>(millis()) < deadline) {
    pollProgressWrites();  // settles nothing itself; the writer task settles the store
    delay(5);
  }
}

void EpubReaderActivity::pollProgressWrites() {
  readerprogress::WriteDone done;
  bool failed = false;
  while (readerprogress::ProgressWriter::get().poll(sessionGeneration_.current(), done)) {
    if (!done.written) failed = true;
  }
  if (failed) {
    // A write that failed is reported the way a synchronous failure always has been: one popup, on
    // the next paint. A completion from a session that is gone is dropped by the slot set, and the
    // store was already settled by the writer task, so nothing is left waiting.
    pendingSyncSaveError = true;
    requestUpdate();
  }
}

void EpubReaderActivity::progressFlushTick() {
  if (!epub) return;
  auto& store = readerprogress::ProgressStore::get();
  const char* path = epub->getCachePath().c_str();
  if (!store.dirty(path)) return;
  // A burst of page turns settles into one write of the last position. The window is measured from
  // the last accept, so a reader that turns every 200 ms still writes as it goes.
  if (static_cast<uint32_t>(millis()) - lastProgressAcceptMs.load(std::memory_order_acquire) < PROGRESS_COALESCE_MS) {
    return;
  }
  flushAcceptedProgress(false);
}

void EpubReaderActivity::onExit() {
  // r163 C1: a prepared next chapter is dropped on every way out of the book -- no writes, and
  // nothing of it survives into the next visit as an adoptable artefact.
  discardPreparedChapter();
  // r163 C4: the last accepted position reaches the card before the book is let go. This flushes
  // the SNAPSHOT, never a live read of the section -- by now the section may be mid-teardown, and a
  // destructor that saves what it finds is exactly the stale save the rule forbids.
  flushAcceptedProgress(true);
  ReaderActivity::onExit();
}

void EpubReaderActivity::prepareForSleep() {
  // r163 C1: sleep is not a chapter turn. The artefact is dropped rather than left on the card.
  discardPreparedChapter();
  flushAcceptedProgress(true);
  ReaderActivity::prepareForSleep();
}

void EpubReaderActivity::rememberCurrentContentOffset() {
  cachedVisibleTextOffset.reset();
  if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    cachedVisibleTextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(section->currentPage));
  }
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  const int fontId = SETTINGS.getReaderFontId();
  ImageBlock::clearRenderFailures();

  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  auto* fcm = renderer.getFontCacheManager();
  // r97 Phase 2 (b): the font measure/prewarm scan, and (e) the glyph-cache
  // miss count for this compose. Both are snapshots taken at the boundaries of
  // the existing scope -- no per-glyph or per-word timer is added.
  //
  // (b) EXCLUDES the status-bar paint: renderStatusBar() is instrumented once,
  // inside itself, and is counted in (c). The scanning call below is inside the
  // prewarm window, so its time is subtracted here instead of being reported
  // twice. A double count would make the split over-attribute, and an
  // over-attributing split is worse than an honest "other".
#if CP_CPU_STATS
  FontDecompressor* fdc = fcm->getDecompressor();
  const uint32_t missBase = fdc ? fdc->getStats().cacheMisses : 0;
  const int64_t statusBeforePrewarmUs = cpustats::statusUsInRender();
  const int64_t prewarmStartUs = cpustats::nowUs();
#endif
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  // Scan the status bar too: a CJK book/chapter title redirected to the SD
  // fallback font joins the page's single batch prewarm instead of triggering
  // its own SD pass after the scope ends.
  renderStatusBar();
  scope.endScanAndPrewarm();
#if CP_CPU_STATS
  cpustats::noteMeasureUs(cpustats::nowUs() - prewarmStartUs -
                          (cpustats::statusUsInRender() - statusBeforePrewarmUs));
#endif
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
#if CP_IMG_CLEAN_ON_ARRIVAL
  // r96.3 D1: the arm decision, hoisted out of the image branch so the one commit site
  // below can report it (and so the branch cannot silently not run).
  bool armArrivalCleanForLog = false;
#endif
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  const bool cleanImageBasePending = manualRefreshPending || pagesUntilFullRefresh <= 1;
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  // r162 — WHAT `textAntiAliasing` DOES, stated once because this block has misled
  // two sessions.
  //
  // The `needsAnyGrayscale` block below is the DUAL-PLANE overlay path: it uploads
  // LSB/MSB bit-planes and calls displayGray(). It stays gated on the driver
  // descriptor, which answers "unsupported" for this panel ON PURPOSE (see the r97
  // note) — that path IS the black-flip and must keep answering that. On this board
  // the term is therefore dead weight: `needsAnyGrayscale` is false whatever the
  // setting says, which is exactly why the reader's AA toggle looked inert.
  //
  // The setting's real wire is the GLYPH BLITTER: ActivityManager applies
  // `renderer.setTextBinary(SETTINGS.textAntiAliasing == 0)` before every reader
  // paint (r162 — the r161 CP_READER_AA_LANE term is gone, so the arm no longer
  // depends on the lane). ON = 2-bit coverage glyphs, OFF = thresholded glyphs, on
  // the SAME refresh request. r162 also put the page turn back on GL16
  // (ReaderUtils::pageTurnRefreshMode), so AA reaches the glass as a diff drive
  // with no wipe prefix.
  //
  // r97: the `absoluteImageGrayscale` term was removed here. It read
  //   pageHasImages && !gpio.deviceIsX3() && display.getController() == HalDisplay::Controller::UC8279
  // and could never be true on this board: the Metalio E-Ink 4 Plus is an ED047TC2 panel on every
  // profile we build, so every branch, ternary and log line behind it was unreachable code that a
  // reader still had to reason about. What it did is preserved nowhere on purpose -- if a UC8279
  // board is ever built, the absolute-quality path has to be re-derived from the driver descriptor,
  // not resurrected from this branch.
  // Capability BEFORE intent. Every branch below that uploads grey planes is gated on the driver's
  // descriptor, except the final `if (needsAnyGrayscale)` block -- which pushes through
  // PanelDriver::displayGray(), whose default implementation calls display() with whatever is in the
  // framebuffer. On a driver with no overlay grayscale (this 1-bit panel) that pushed a re-rendered
  // MSB BIT-PLANE as if it were the page: the reader showed the correct frame, then flipped to white
  // text on black ~1-2 s later, and every page turn cost two full-screen waveforms. Folding
  // `.supported()` in here makes the intent follow the hardware instead of the setting.
  const auto grayscale = renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Overlay);
  const bool needsAnyGrayscale =
      (needsTextGrayscale || (pageHasImages && !BoardConfig::isEegoA4())) && grayscale.supported();
  const bool tiledGrayscale = needsAnyGrayscale && grayscale.stripUploads;
  // Paper Mono only (no other panel combines): defer the B/W base activation so
  // the gray planes join it in a single waveform. Displaying the base
  // separately makes the gray pass re-drive the whole text body — a visible
  // flash on every AA page.
  const bool combinedGrayscaleBase =
      tiledGrayscale && !pageHasImages && grayscale.base == HalDisplay::GrayscaleBase::Combined;
  const bool overlapRefresh = tiledGrayscale && grayscale.asyncBase && !pageHasImages;
  auto renderGrayscalePass = [&]() {
    // A4 gray planes replace the full page, including its clipping highlights.
    if (BoardConfig::isEegoA4()) drawClippingHighlights(*page, fontId, orientedMarginTop, orientedMarginLeft);
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
    // A4 replaces the whole frame with its gray planes, including the status bar.
    if (BoardConfig::isEegoA4()) renderStatusBar();
  };

  if (pageHasImagesNeedingDecode) {
    page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    renderer.clearScreen();
  }

  // r97 Phase 2 (d): clipping highlights, timed as one phase at the call
  // boundary. On a page with no clipping this is a load and a scan, and it will
  // read as such -- which is the point of measuring it.
#if CP_CPU_STATS
  const int64_t clipStartUs = cpustats::nowUs();
#endif
  drawClippingHighlights(*page, fontId, orientedMarginTop, orientedMarginLeft);
#if CP_CPU_STATS
  cpustats::noteClipUs(cpustats::nowUs() - clipStartUs);
#endif
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  // (e): hot-group decompressions the glyph path had to pay for in THIS compose.
  // A non-zero count on a page turn means the prewarm scope missed glyphs the
  // page then needed -- the one number that tells a pipelining change whether it
  // must carry the font cache with it.
#if CP_CPU_STATS
  if (fdc) cpustats::noteGlyphMisses(fdc->getStats().cacheMisses - missBase);
#endif
  const auto tBwRender = millis();

  if (pageHasImages) {
#if CP_IMG_CLEAN_ON_ARRIVAL
    // r96 D1: an image page arrives on CLEAN glass. Every image is drawn over
    // whatever the panel already held, and an image page used to resolve to HALF or
    // the page-turn mode (below) - never a from-white drive - so the ghosting the
    // operator reported came from the glass, not from the tone pipeline.
    //
    // Reuses the r93 one-shot verbatim: forceCleanNextRefresh() is consumed by the
    // `forceClean` term in RefreshPolicy on the very next displayBuffer(), which is
    // the one two lines below. A repaint of the same image page - status bar, a
    // clipping highlight, a menu over it - must not pay another 815 ms clean, which is
    // why the arm is a TRANSITION (r96.3), not a per-page latch (r96-r96.2).
    //
    // r96.2 D1 — WHY THE GUARD IS COMMITTED AFTER THE PAINT, NOT AT THE ARM.
    // r96.1 wrote (spine, page) at the arm and then drove. If that drive was not the
    // one that took the flag -- a promotion resolved the request, another paint ran
    // in between, the drive was skipped -- the pair was already recorded and the
    // clean could never be re-armed for that page: the operator saw "neither edge
    // cleans". Now the arm only records the INTENT, the paint is checked against the
    // renderer's consume counter, and the guard is committed only when the clean
    // actually landed. If it did not, the pair stays unset and the next paint of this
    // page re-arms instead of silently never cleaning again.
    // r96.3 D1 — THE TRANSITION TEST. `displayedSpine_/Page_` is the page that is
    // actually on the glass; the clean is armed when the page arriving now is a DIFFERENT
    // one. There is no "already cleaned this page" record any more, so nothing can get
    // stuck: image -> text -> image cleans on both arrivals, a repaint of the page you
    // are already on cleans never, and a departure cleans never.
    //
    // `displayedCleanPending_` carries the r96.2 lesson forward: if the arm was made and
    // the paint below did NOT take it, the arrival is still outstanding, so the next paint
    // of this same page re-arms instead of the clean silently disappearing.
    const int cleanPage = section ? static_cast<int>(section->currentPage) : -1;
    const bool samePageOnGlass = (displayedSpine_ == currentSpineIndex && displayedPage_ == cleanPage);
    // The retry term is scoped to the page it belongs to: a pending arm recorded for a
    // DIFFERENT page must not follow the reader here.
    const bool pendingHere = displayedCleanPending_ && samePageOnGlass;
    const bool armArrivalClean = !samePageOnGlass || pendingHere;
    armArrivalCleanForLog = armArrivalClean;
    if (armArrivalClean) {
      cleanArmedSpine_ = currentSpineIndex;
      cleanArmedPage_ = cleanPage;
      cleanArmCounter_ = renderer.forcedCleanPaintsConsumed();
      renderer.forceCleanNextRefresh();
      LOG_INF("ERS",
              "armImageClean spine=%d page=%d force=1 counter=%lu prev=%d/%d pending=%d: forcing a from-white clean "
              "on arrival",
              currentSpineIndex, cleanPage, static_cast<unsigned long>(cleanArmCounter_), displayedSpine_, displayedPage_,
              displayedCleanPending_ ? 1 : 0);
    }
#endif
    // Image pages use one base refresh before the grayscale pass. FAST leaves
    // the panel receptive to the gray waveform; pending cleanup still honors
    // the scheduled/manual HALF refresh. With CP_IMG_CLEAN_ON_ARRIVAL the armed
    // forceClean term overrides this to a single from-white GC16 and nothing else.
    //
    // GRAY16 (#15). THE ROUTING SIGNAL AND ITS FALLBACK, in this order:
    //   1. THE GUESS is `page->hasImages()` — the page's element list contains a
    //      PageImage (Page.h:hasImages, built by the chapter parser). It is a
    //      STRUCTURAL test, not a pixel heuristic, so it cannot fire on a text page
    //      that merely contains a glyph that looks dark.
    //   2. THE VERIFICATION is `renderer.regionCarriesTone(imageBox)`: after the
    //      compose, does the IMAGE AREA actually hold a nibble between ink and
    //      paper? A page whose image failed to decode, was a 1-bit scan, or was
    //      clipped to nothing answers "no", and that is the surface the wrong
    //      guess falls back to: the ordinary page-turn mode, i.e. exactly the mode
    //      and the cost this build had before GRAY16. Nothing is re-rendered, no
    //      placeholder appears, the page is simply driven as binary.
    //   3. THE DRIVE, when both hold, is FULL — the from-white GC16. It is the
    //      ONLY 16-level mode that conditions the pixels it leaves white, and that
    //      is exactly what this page needs: the previous screen is text, and the
    //      picture's white margins are FLAT WHITE, so they must be driven back to
    //      white rather than assumed to be there. See the table comparison below.
    //
    // WHY NOT GL16 (r101.2 — this is the ghosting the operator reported on item 15).
    // GL16 (my_gl_25_0, type 5) and GC16 (my_gc_25_0, type 2) are the SAME LENGTH,
    // 48 phases, ~800 ms; they differ in CONTENT. Counted from the two tables:
    //   my_gc_25_0  15->15 = 18 dark + 17 light phases  (a full conditioning cycle)
    //   my_gl_25_0  15->15 = 0 + 0, plus the single light tick gl16_white_tick()
    //                       adds at boot
    // and GL16 is driven by `epd_hl_update_screen(MODE_GL16)` — diff-based, with
    // MODE_SKIP_UNCHANGED, i.e. only the pixels whose target differs from the
    // recorded baseline are pulsed at all. The recorded baseline for a white
    // margin is 0xF (it was paper before), so 15->15 is chosen, and GL16 holds:
    // the old text under the picture's white areas is never touched. GC16-from-white
    // resets the baseline to white (highlevel.c: epd_hl_update_screen_from_white
    // memset back_fb 0xFF) and clears MODE_SKIP_UNCHANGED, so every pixel is driven
    // and the 15->15 cycle runs. Same phases, same ~800 ms, residue gone.
    //
    // COST, stated: one drive, unchanged from what CP_IMG_CLEAN_ON_ARRIVAL already
    // paid on the arrival (its forceClean term resolved to this same FULL). The
    // difference is that a REPAINT of an image page (status bar, a clipping
    // highlight, a menu dismiss) used to be a cheap GL16 diff and is now the same
    // ~800 ms from-white GC16 — that is the price of the picture not ghosting, and
    // CP_SKIP_REDRIVE already suppresses the repaints whose frame is identical.
    int pageImageCountForLog = 0;
    for (const auto& element : page->elements) {
      if (element->getTag() == TAG_PageImage) ++pageImageCountForLog;
    }
    // The verification half of the routing signal: does the IMAGE AREA hold an
    // intermediate level? Scoped to the union of the page's image rects (not the
    // frame) so the reader's own AA text cannot answer the question for the
    // picture — see GfxRenderer::regionCarriesTone.
    bool tonePresent = false;
    if (pixfmt::kGray16Images && GfxRenderer::fourBitCanvas()) {
      int16_t boxX = 0, boxY = 0, boxW = 0, boxH = 0;
      if (page->getImageBoundingBox(boxX, boxY, boxW, boxH)) {
        tonePresent = renderer.regionCarriesTone(orientedMarginLeft + boxX, orientedMarginTop + boxY, boxW, boxH);
      }
    }
    const HalDisplay::RefreshMode imageDrive =
        tonePresent ? HalDisplay::FULL_REFRESH
                    : (cleanImageBasePending ? HalDisplay::HALF_REFRESH : ReaderUtils::pageTurnRefreshMode());
    const auto tImageDrive0 = millis();
    renderer.displayBuffer(imageDrive);
    // THE COMPOSE LOG (#15). One line per image page, emitted at the point where the
    // frame and the drive are both known, so the numbers are measured on the device
    // and not inferred:
    //   compose_ms — prewarm + full page render INCLUDING the JPEG/PNG decode and the
    //                16-level write into the canvas (t0 -> tBwRender).
    //   req        — the mode this call site REQUESTED. The refresh policy can still
    //                resolve it (a promoted HALF, a clean-on-arrival FULL); the
    //                `[panel] show mode=... ms=...` line that follows is the waveform
    //                that actually ran, and it is the one to quote for the cost.
    //   drive_ms   — wall time of this paint's displayBuffer(), i.e. the same figure
    //                the [panel] line reports as `ms=`, named with the page it belongs to.
    // The ratio is the decision input for hardware JPEG decode: if compose_ms is a
    // small fraction of drive_ms, the decoder is not what an image page is waiting
    // for and H/W decode buys a fraction of a fraction. If compose_ms is
    // comparable, it does. Either way the answer is in this line, not in an estimate.
    LOG_INF("ERS",
            "img16 spine=%d page=%d imgs=%d levels=%d tone=%d req=%d drive=%s compose_ms=%lu drive_ms=%lu",
            currentSpineIndex, section ? static_cast<int>(section->currentPage) : -1, pageImageCountForLog,
            (pixfmt::kGray16Images && GfxRenderer::fourBitCanvas()) ? 16 : 4, tonePresent ? 1 : 0,
            static_cast<int>(imageDrive),
            // The waveform the request names, in the panel shim's own vocabulary, so
            // this line is checkable against the `[panel] show mode=` line that follows.
            tonePresent                                    ? "GC16-from-white"
            : imageDrive == HalDisplay::HALF_REFRESH       ? "GC16"
            : imageDrive == HalDisplay::GL16_REFRESH       ? "GL16"
            : imageDrive == HalDisplay::FULL_REFRESH       ? "GC16-from-white"
                                                           : "DU",
            static_cast<unsigned long>(tBwRender - t0), static_cast<unsigned long>(millis() - tImageDrive0));
#if CP_IMG_CLEAN_ON_ARRIVAL
    if (armArrivalClean) {
      const uint32_t now = renderer.forcedCleanPaintsConsumed();
      if (now > cleanArmCounter_) {
        // The paint did what it said it would: the arrival is served.
        displayedCleanPending_ = false;
        LOG_INF("ERS", "cleanConsumed spine=%d page=%d force=1 counter=%lu", cleanArmedSpine_, cleanArmedPage_,
                static_cast<unsigned long>(now));
      } else {
        // Armed and not spent: keep the arrival OUTSTANDING so the next paint of this same
        // page re-arms. This is the one line that would have caught r96.1 without a device.
        displayedCleanPending_ = true;
        LOG_ERR("ERS", "cleanNOTconsumed spine=%d page=%d force=1 counter=%lu (arm not taken; will retry)",
                cleanArmedSpine_, cleanArmedPage_, static_cast<unsigned long>(now));
      }
    }
    // r96.1 D1: `pagesUntilFullRefresh = 1` is the OTHER edge of this same story.
    // It makes the page AFTER an image page resolve to FULL_REFRESH, so leaving an
    // image page flashed a from-white GC16 over the next page -- the clean the
    // operator reported on the wrong side of the page turn. The arrival drive above
    // IS a from-white clean now, so it satisfies the ghost-cleaning cadence and the
    // counters restart exactly as displayWithRefreshCycle() restarts them after its
    // own FULL. Departures from an image page go back to the normal page-turn mode.
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    if (pagesUntilFullRefresh < 1) pagesUntilFullRefresh = 1;
    pagesUntilQuickRefresh = SETTINGS.getQuickRefreshPages();
#else
    // No arrival clean: keep the pre-r96 behaviour of cleaning the next page.
    pagesUntilFullRefresh = 1;
#endif
  } else if (combinedGrayscaleBase) {
    // Stash the base without activating; displayGrayBuffer() below commits
    // base + grays as one waveform.
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh, pagesUntilQuickRefresh);
  } else if (needsAnyGrayscale) {
    if (pagesUntilFullRefresh <= 1) {
      // A cleanup refresh settles X3 correctly only when its grayscale
      // preconditioning waveform runs before the gray planes are written.
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      renderer.preconditionGrayscale();
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
      pagesUntilQuickRefresh = SETTINGS.getQuickRefreshPages();
    } else if (overlapRefresh) {
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, pagesUntilQuickRefresh, /*async=*/true);
    } else {
      renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
      // Same bookkeeping as ReaderUtils::displayWithRefreshCycle, which this branch bypasses.
      if (pagesUntilFullRefresh > 1) pagesUntilFullRefresh--;
      if (pagesUntilQuickRefresh > 1) pagesUntilQuickRefresh--;
      else pagesUntilQuickRefresh = SETTINGS.getQuickRefreshPages();
    }
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, pagesUntilQuickRefresh);
  }
  const auto tDisplay = millis();
#if CP_IMG_CLEAN_ON_ARRIVAL
  // r96.3 D1: THIS page is now the one on the glass, whatever it was. Every branch above
  // has driven its base frame by now (the image branch's from-white clean is one of them),
  // so the transition record moves here and only here -- one place, all page kinds. A
  // repaint of this page arrives with displayedSpine_/Page_ already equal to it and
  // therefore never cleans again; the next DIFFERENT page is an arrival.
  displayedSpine_ = currentSpineIndex;
  displayedPage_ = section ? static_cast<int>(section->currentPage) : -1;
  // A pending arm belongs to the page it was armed for. Moving to any other page retires it
  // (the failing paint is gone from the glass), so it cannot leak into a later arrival.
  if (displayedCleanPending_ && !(displayedSpine_ == cleanArmedSpine_ && displayedPage_ == cleanArmedPage_)) {
    displayedCleanPending_ = false;
  }
  if (pageHasImages) {
    LOG_DBG("ERS", "displayed spine=%d page=%d images=1 pending=%d (armed=%d)", displayedSpine_, displayedPage_,
            displayedCleanPending_ ? 1 : 0, armArrivalCleanForLog ? 1 : 0);
  }
#endif

  if (tiledGrayscale) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();
    const size_t planeBytes = static_cast<size_t>(gwBytes) * gh;

    auto renderPlaneToBuffer = [&](const bool lsbPlane, uint8_t* buf) {
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(buf + static_cast<size_t>(y) * gwBytes, y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
      }
    };

    constexpr size_t PLANE_BUF_HEADROOM = 60000;
    constexpr size_t PLANE_BUF_MAX_ALLOC_RESERVE = 16 * 1024;
    const auto planeBufFits = [planeBytes] {
      return ESP.getFreeHeap() >= planeBytes + PLANE_BUF_HEADROOM &&
             ESP.getMaxAllocHeap() >= planeBytes + PLANE_BUF_MAX_ALLOC_RESERVE;
    };
    auto lsbPlaneBuf = (overlapRefresh && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
    auto msbPlaneBuf = (lsbPlaneBuf && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;

    if (lsbPlaneBuf) {
      renderPlaneToBuffer(true, lsbPlaneBuf.get());
      if (msbPlaneBuf) renderPlaneToBuffer(false, msbPlaneBuf.get());
      const auto tGrayRender = millis();

      renderer.waitRefreshComplete();
      const auto tWait = millis();

      renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, gh);
      if (msbPlaneBuf) {
        renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf.get(), 0, gh);
      } else {
        renderPlaneToBuffer(false, lsbPlaneBuf.get());
        renderer.writeGrayscalePlaneStrip(false, lsbPlaneBuf.get(), 0, gh);
      }
      const auto tGrayWrite = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tEnd = millis();

      LOG_DBG("ERS",
              "Page render (tiled async): prewarm=%lums bw_render=%lums display=%lums gray_render=%lums "
              "wait=%lums gray_write=%lums gray_display=%lums cleanup=%lums total=%lums (planes buffered: %d)",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayRender - tDisplay, tWait - tGrayRender,
              tGrayWrite - tWait, tGrayDisplay - tGrayWrite, tEnd - tGrayDisplay, tEnd - t0, msbPlaneBuf ? 2 : 1);
    } else {
      auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
      renderer.waitRefreshComplete();
      if (!scratch) {
        LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
        if (overlapRefresh || combinedGrayscaleBase) {
          // The BW refresh ran the shadow-free async path, so controller RAM's
          // differential baseline was never rebuilt. Even with AA skipped it must
          // be re-synced from the intact BW framebuffer, or the next differential
          // update diffs against stale contents. On the combined-base path the
          // base activation is still deferred; this cleanup commits it so the
          // page reaches the panel even without its grays.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }
      } else {
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
        }
        const auto tGrayLsb = millis();

        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
        }
        const auto tGrayMsb = millis();

        renderer.setRenderMode(GfxRenderer::BW);
        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();

        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
                "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
                tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
      }
    }
  } else {
    // Belt and braces: this is the block that pushes, so it re-checks the capability itself rather
    // than trusting a flag computed upstream.
    if (needsAnyGrayscale && grayscale.supported()) {
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        return;
      }
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderGrayscalePass();
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderGrayscalePass();
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

int EpubReaderActivity::clippingAtPoint(const Page& page, const int x, const int y) const {
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int fontId = SETTINGS.getReaderFontId();
  const uint32_t signature =
      readerRenderSpecSignature(SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight));
  uint16_t wordIndex = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const auto& block = line.getBlock();
    if (!block || !block->valid()) continue;
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const char* text = block->wordText(i);
      if (!clippingText::hasVisibleText(text)) continue;
      const auto style = static_cast<EpdFontFamily::Style>(block->wordStyle(i) & ~EpdFontFamily::UNDERLINE);
      const int width = renderer.getTextAdvanceX(clippingWordFont(renderer, fontId, *block, i), text, style,
                                                 block->getBlockStyle().characterSpacing);
      if (width <= 0) continue;
      const uint16_t index = wordIndex++;
      const Rect rect =
          clippingWordRect(renderer, fontId, line, i, top + SETTINGS.screenMargin, left + SETTINGS.screenMargin, width);
      if (x < rect.x || x >= rect.x + rect.width || y < rect.y || y >= rect.y + rect.height) continue;
      const auto range = block->wordSourceRange(i);
      for (size_t j = 0; j < CLIPPINGS.clippingCount(); ++j) {
        if (clippingContainsWord(*CLIPPINGS.clippingAt(j), currentSpineIndex, section->currentPage, section->pageCount,
                                 signature, index, range.start, range.end))
          return static_cast<int>(j);
      }
    }
  }
  return -1;
}

void EpubReaderActivity::drawClippingHighlights(const Page& page, const int fontId, const int orientedMarginTop,
                                                const int orientedMarginLeft) const {
  if (!section || !CLIPPINGS.hasClippings() || section->currentPage < 0 || section->currentPage >= section->pageCount) {
    return;
  }

  size_t chapterClippings = 0;
  for (size_t i = 0; i < CLIPPINGS.clippingCount(); ++i) {
    if (CLIPPINGS.clippingAt(i)->spineIndex == currentSpineIndex) ++chapterClippings;
  }
  if (!chapterClippings) return;

  const auto started = millis();
  size_t highlightedWords = 0;
  const uint16_t currentPage = static_cast<uint16_t>(section->currentPage);
  const uint32_t layoutSignature =
      readerRenderSpecSignature(SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight));
  const auto isHighlighted = [&](const uint16_t wordIndex, const TextBlock::SourceRange range) {
    // ponytail: scan at most 256 C3 / 1024 S3 clippings per word; index by chapter if this becomes slow.
    for (size_t index = 0; index < CLIPPINGS.clippingCount(); ++index) {
      const Clipping& clipping = *CLIPPINGS.clippingAt(index);
      if (clippingContainsWord(clipping, static_cast<uint16_t>(currentSpineIndex), currentPage, section->pageCount,
                               layoutSignature, wordIndex, range.start, range.end))
        return true;
    }
    return false;
  };

  const auto fillHighlight = [&](const int x, const int y, const int width, const int height) {
    if (BoardConfig::isEegoA4() && renderer.getRenderMode() != GfxRenderer::BW) {
      // Gray planes start at zero and use set bits for ink, opposite to B/W.
      for (int py = y + (y & 1); py < y + height; py += 2) {
        for (int px = x + (x & 1); px < x + width; px += 2) renderer.drawPixel(px, py, false);
      }
    } else {
      renderer.fillRectDither(x, y, width, height, Color::LightGray);
    }
  };

  uint16_t pageWordIndex = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const auto& block = line.getBlock();
    if (!block || !block->valid()) continue;
    bool hasPreviousHighlight = false;
    int previousHighlightX = 0;
    int previousHighlightWidth = 0;
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const char* text = block->wordText(i);
      if (!clippingText::hasVisibleText(text)) continue;
      const auto style = static_cast<EpdFontFamily::Style>(block->wordStyle(i) & ~EpdFontFamily::UNDERLINE);
      int width = renderer.getTextAdvanceX(clippingWordFont(renderer, fontId, *block, i), text, style,
                                           block->getBlockStyle().characterSpacing);
      if (width <= 0) continue;
      const uint16_t wordIndex = pageWordIndex++;
      if (!isHighlighted(wordIndex, block->wordSourceRange(i))) {
        hasPreviousHighlight = false;
        continue;
      }

      const Rect rect = clippingWordRect(renderer, fontId, line, i, orientedMarginTop, orientedMarginLeft, width);
      const int x = rect.x;
      const int y = rect.y;
      width = rect.width;
      if (width > 0) {
        const int highlightRight = x + width;
        const int previousHighlightRight = previousHighlightX + previousHighlightWidth;
        if (hasPreviousHighlight && previousHighlightRight < x) {
          fillHighlight(previousHighlightRight, y, x - previousHighlightRight, renderer.getLineHeight(fontId));
        } else if (hasPreviousHighlight && highlightRight < previousHighlightX) {
          fillHighlight(highlightRight, y, previousHighlightX - highlightRight, renderer.getLineHeight(fontId));
        }
        fillHighlight(x, y, width, renderer.getLineHeight(fontId));
        previousHighlightX = x;
        previousHighlightWidth = width;
        hasPreviousHighlight = true;
        ++highlightedWords;
      } else {
        hasPreviousHighlight = false;
      }
    }
  }
  LOG_DBG("CLIP", "Highlight pass: chapter_clips=%u highlighted_words=%u time=%lums",
          static_cast<unsigned>(chapterClippings), static_cast<unsigned>(highlightedWords), millis() - started);
}

void EpubReaderActivity::renderStatusBar() const {
  // r97 Phase 2 (c): the status-bar phase, timed once inside the function rather
  // than at each of its four call sites -- a call site added later cannot forget
  // the timer this way. RAII so an early return would still be counted; there is
  // none today.
#if CP_CPU_STATS
  struct StatusTimer {
    const int64_t t0;
    explicit StatusTimer(const int64_t t) : t0(t) {}
    ~StatusTimer() { cpustats::noteStatusUs(cpustats::nowUs() - t0); }
  } statusTimer(cpustats::nowUs());
#endif
  const int currentPage = section ? section->currentPage + 1 : 1;
  const float pageCount = section ? section->estimatedTotalPages() : 1;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub ? (epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100) : 0;

  std::string title;
  int textYOffset = 0;
  const auto sb = SETTINGS.statusBarSpec();

  const auto note = linkNoteTitle.draw(sb.showsTitle(), bleturner::linkNote());
  if (note == bleturner::LinkNote::Connected) {
    const auto peer = bleturner::linked();
    char connected[64];
    snprintf(connected, sizeof(connected), tr(STR_BT_CONNECTED_TO), peer.name[0] != '\0' ? peer.name : peer.addr);
    title = connected;
  } else if (note == bleturner::LinkNote::LowMemory) {
    title = tr(STR_BT_LOW_MEMORY);
  } else if (note != bleturner::LinkNote::None) {
    title = note == bleturner::LinkNote::Connecting ? tr(STR_BT_CONNECTING) : tr(STR_BT_FAILED);
  } else if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    // Cached per spine: this used to re-walk the TOC off SD on every repaint.
    title = chapterTitleForSpine(currentSpineIndex);
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub ? epub->getTitle() : "";
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    section ? section->isBuilding() : false);
}

// ---------------------------------------------------------------------------
// Toolbar reader menu
// ---------------------------------------------------------------------------

namespace {
constexpr StrId kTextRowNames[] = {StrId::STR_FONT, StrId::STR_FONT_SIZE, StrId::STR_LINE_SPACING,
                                   StrId::STR_PARA_ALIGNMENT, StrId::STR_FOCUS_READING};
constexpr StrId kAlignIds[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                               StrId::STR_BOOK_S_STYLE};
constexpr int kTextRowCount = static_cast<int>(std::size(kTextRowNames));
static_assert(std::size(kAlignIds) == CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, "alignment labels");
}  // namespace

bool EpubReaderActivity::usesToolbarMenu() const {
  // Both board classes drive the same chrome: touch through the FreeInkUI tap
  // targets, buttons through the focused-tool pill and the panel cursor.
  return SETTINGS.readerMenuStyle == CrossPointSettings::READER_MENU_TOOLBAR;
}

std::string EpubReaderActivity::chapterTitleForSpine(const int spineIndex) const {
  if (!epub) return tr(STR_UNNAMED);
  if (spineIndex == cachedChapterTitleSpine) return cachedChapterTitleText;
  const int tocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  std::string title = tocIndex != -1 ? epub->getTocItem(tocIndex).title : std::string(tr(STR_UNNAMED));
  cachedChapterTitleSpine = spineIndex;
  cachedChapterTitleText = title;
  return title;
}

std::string EpubReaderActivity::currentChapterTitle() const {
  if (!epub) return "";
  return chapterTitleForSpine(currentSpineIndex);
}

std::string EpubReaderActivity::textRowName(int row) const {
  return row >= 0 && row < kTextRowCount ? I18N.get(kTextRowNames[row]) : "";
}

std::string EpubReaderActivity::textRowValue(int row) const {
  static constexpr StrId kFamily[] = {StrId::STR_NOTO_SANS};
  switch (row) {
    case 0:
      if (SETTINGS.sdFontFamilyName[0] != '\0') return SETTINGS.sdFontFamilyName;
      return I18N.get(kFamily[SETTINGS.fontFamily % CrossPointSettings::FONT_FAMILY_COUNT]);
    case 1:
      return std::to_string(SETTINGS.fontPointSize) + " pt";
    case 2:
      return lineSpacing::label(SETTINGS.lineSpacingPercent);
    case 3:
      return I18N.get(kAlignIds[SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT]);
    default:
      return "";
  }
}

// Live apply: persist, re-paginate, and let renderBook() redraw the page with
// the open panel back on top -- the book itself is the preview.
void EpubReaderActivity::applyTextSettingLive() {
  applyReaderTextSettings();
  discardOverlayPage();  // the stored page is laid out with the old settings
  requestUpdate();
}

// Settings-style option pickers for the Text panel's enum rows. Every
// selection applies immediately to the page under the sheet.
void EpubReaderActivity::showTextRowPopup(const int row) {
  switch (row) {
    case 1: {
      // The point sizes the active family actually ships.
      const auto sizes = readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.sdFontFamilyName);
      if (sizes.empty()) return;
      std::vector<std::string> labels;
      labels.reserve(sizes.size());
      for (const uint8_t size : sizes) labels.push_back(std::to_string(size) + " pt");
      const uint8_t cur = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
      int curIdx = 0;
      for (size_t i = 0; i < sizes.size(); ++i) {
        if (sizes[i] == cur) curIdx = static_cast<int>(i);
      }
      overlayPopup.show(StrId::STR_FONT_SIZE, labels, curIdx, [this, sizes](int idx) {
        if (idx < 0 || idx >= static_cast<int>(sizes.size())) return;
        SETTINGS.fontPointSize = sizes[idx];
        applyTextSettingLive();
      });
      break;
    }
    case 2: {
      // 1% steps, shown as a window of values around the current percent.
      const auto w = lineSpacing::window(SETTINGS.lineSpacingPercent);
      overlayPopup.show(StrId::STR_LINE_SPACING, w.labels, w.current, [this, w](int idx) {
        SETTINGS.lineSpacingPercent = w.percentAt(idx);
        applyTextSettingLive();
      });
      break;
    }
    case 3:
      overlayPopup.show(StrId::STR_PARA_ALIGNMENT, kAlignIds, static_cast<int>(std::size(kAlignIds)),
                        SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, [this](int idx) {
                          SETTINGS.paragraphAlignment = static_cast<uint8_t>(idx);
                          applyTextSettingLive();
                        });
      break;
    default:
      return;
  }
  paintOverlayPopup();
}

void EpubReaderActivity::discardOverlayPage() {
  if (!overlayPageStored) return;
  renderer.discardStoredBwBuffer();
  overlayPageStored = false;
}

// Push freshly painted overlay chrome. Where the panel supports it the refresh
// is fired deferred: the loop keeps polling input while the waveform runs, so
// the chrome answers taps and buttons the moment it is visible instead of only
// after a blocking displayBuffer() returns. Caller must hold the RenderLock.
void EpubReaderActivity::pushOverlayRefresh() {
  if (renderer.supportsAsyncRefresh()) {
    renderer.displayBufferAsync(HalDisplay::FAST_REFRESH);
    overlayRefreshPending = true;
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

// Wait out a pending deferred overlay refresh and reseed the panel's
// differential baseline from the framebuffer: the shadow-free async path skips
// the post-refresh resync, so without this the next FAST diff would run
// against the frame from before the chrome and leave stale pixels on the
// glass. Caller must hold the RenderLock.
void EpubReaderActivity::settleOverlayRefresh() {
  if (!overlayRefreshPending) return;
  overlayRefreshPending = false;
  renderer.cleanupGrayscaleWithFrameBuffer();  // waits, then reseeds the baseline
}

void EpubReaderActivity::openOverlay(Overlay target) {
  mappedInput.resetHomeButtonInput();
  const Overlay previous = overlay;
  overlay = target;
  if (!toolbarUi) toolbarUi = std::make_unique<ReaderToolbarUi>(renderer);
  if (previous == Overlay::None) toolbarUi->begin();
  // Buttons show a cursor from the start; touch boards only once a button moves it.
  panelCursorShown = !mappedInput.hasTouch();
  switch (target) {
    case Overlay::Toolbar:
      focusedTool = 0;
      break;
    case Overlay::Contents: {
      RenderLock lock;
      panelIndex = currentTocIndex();
      // Fresh viewport opening on the current chapter, cursor shown or not.
      toolbarUi->nav().reset(panelIndex);
      toolbarUi->nav().top = panelIndex;
      break;
    }
    case Overlay::Text:
      panelIndex = 0;
      toolbarUi->nav().reset();
      break;
    case Overlay::More:
      panelIndex = 0;
      buildMoreActions();
      toolbarUi->nav().reset();
      break;
    default:
      break;
  }
  panelHoldJumped = false;

  // The page is already on screen and still in the framebuffer, so paint the
  // chrome straight onto it and push one refresh. requestUpdate() would
  // re-render the whole page first: slow, and visibly wrong, since that repaint
  // lands before the overlay does.
  //
  // Refresh mode: FAST for every overlay paint, first open included. The AA
  // pass only grays glyph edges, and residue a FAST differential leaves under
  // the sheet has not shown in practice; it also self-heals on the
  // Xteink-class panels, whose close path re-renders the page. If text or
  // images ever visibly ghost through the chrome, restore a HALF cleanup on
  // the first open (see #2190 for the mechanism).
  if (section && !pageBufferStale) {
    // Serialize against the render task: renderBook may be mid-page (status
    // bar included) in the shared framebuffer, and painting the chrome from
    // the loop task at the same time interleaves the two frames.
    RenderLock lock;
    settleOverlayRefresh();
    if (previous == Overlay::None) {
      // Snapshot the clean page so stepping back from a panel to the toolbar
      // (and closing, where supported) can restore it without a re-render.
      overlayPageStored = renderer.storeBwBuffer();
    } else if (overlayPageStored) {
      // Overlay -> overlay: wipe the previous chrome (toolbar header, sheet,
      // progress row) back to the clean page so none of it shows around or
      // through the new sheet; re-store for the next transition. No baseline
      // resync: the glass still shows the old chrome, and the differential
      // must keep diffing against it to erase it.
      renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
      overlayPageStored = renderer.storeBwBuffer();
    }
    renderOverlay();
    pushOverlayRefresh();
  } else {
    requestUpdate();  // no page in the framebuffer: renderBook() draws the overlay once it is
  }
}

// Grayscale panels re-render to restore AA; other boards restore the B/W snapshot.
void EpubReaderActivity::closeOverlayToPage() {
  mappedInput.resetHomeButtonInput();
  overlay = Overlay::None;
  overlayPopup.dismiss();  // an option picker cannot outlive its panel
  toolbarUi.reset();       // ~1 KB of interaction table + props, only needed while open
  if (BoardConfig::isEegoA4()) pagesUntilFullRefresh = 1;
  if (!xteinkClassPanel() && overlayPageStored) {
    RenderLock lock;  // the render task shares the framebuffer
    settleOverlayRefresh();
    // No baseline resync: the glass shows the chrome, and erasing it needs
    // the differential to keep diffing against the last pushed frame.
    renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
    overlayPageStored = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  discardOverlayPage();
  requestUpdate();  // redraw the clean page
}

void EpubReaderActivity::renderOverlay() {
  if (!epub || !section || !toolbarUi) return;

  ReaderToolbarUi::Model model;
  // The toolbar's tool pill is the button-navigation cursor: tap-first (same
  // convention as the panel lists), it only shows once a button has moved it.
  // Panels override below: there the pill marks the open panel on every board.
  model.activeTool = (overlay == Overlay::Toolbar && !panelCursorShown) ? -1 : focusedTool;
  // Strings the model points at live here until render() returns.
  std::string chapterTitle, pageInfo;

  if (overlay == Overlay::Toolbar) {
    chapterTitle = currentChapterTitle();
    const int pageCount = section->estimatedTotalPages();
    const float chapterProgress =
        pageCount > 0 ? static_cast<float>(section->currentPage + 1) / static_cast<float>(pageCount) : 0.0f;
    const float bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress);
    pageInfo = std::to_string(section->currentPage + 1) + "/" + std::to_string(pageCount) + "   " +
               std::to_string(clampPercent(static_cast<int>(bookProgress * 100.0f + 0.5f))) + "%";
    model.chapterTitle = chapterTitle.c_str();
    model.pageInfo = pageInfo.c_str();
    model.progressPermille = static_cast<int>(bookProgress * 1000.0f + 0.5f);
    toolbarUi->setModel(model);
    toolbarUi->render();
    return;
  }

  // Panels (Contents / Text / More): a bottom sheet over the page.
  model.panel = true;
  if (!mappedInput.hasTouch()) {
    model.denseRows = true;
  }
  // Tap-first: the cursor is only drawn once a button has moved it, so a
  // tapped row does not stay inverted after its action.
  model.selectedIndex = panelCursorShown ? panelIndex : -1;
  if (overlay == Overlay::Contents) {
    model.panelTitle = tr(STR_TOOL_CONTENTS);
    model.itemCount = epub->getTocItemsCount();
    model.rowText = [this](int i) {
      const auto item = epub->getTocItem(i);
      const int depth = item.level > 1 ? (item.level - 1) * 2 : 0;
      return std::string(depth, ' ') + item.title;
    };
  } else if (overlay == Overlay::Text) {
    model.panelTitle = tr(STR_TOOL_TEXT);
    model.itemCount = kTextRowCount;
    model.rowText = [this](int i) { return textRowName(i); };
    model.rowValue = [this](int i) { return textRowValue(i); };
    model.rowCheckbox = [](void*, int i, freeink::ui::ListItem& item) {
      if (i == 4) GUI.setCheckboxRow(item, SETTINGS.focusReadingEnabled);
    };
  } else {
    model.panelTitle = tr(STR_TOOL_MORE);
    model.itemCount = static_cast<int>(moreItems.size());
    model.rowText = [this](int i) { return moreRowName(i); };
    model.rowValue = [this](int i) { return moreRowValue(i); };
    model.rowCheckboxContext = this;
    model.rowCheckbox = [](void* ctx, int i, freeink::ui::ListItem& item) {
      const auto* self = static_cast<EpubReaderActivity*>(ctx);
      using MA = EpubReaderMenuActivity::MenuAction;
      const auto action = self->moreItems[i].action;
      if (action == MA::NIGHT_MODE) GUI.setCheckboxRow(item, SETTINGS.screenInverted);
      if (action == MA::BLUETOOTH) GUI.setCheckboxRow(item, SETTINGS.ble.enabled);
      if (action == MA::FRONTLIGHT) GUI.setCheckboxRow(item, Frontlight.isOn());
    };
  }
  toolbarUi->setModel(model);
  toolbarUi->render();
}

void EpubReaderActivity::handleOverlayInput() {
  if (!toolbarUi) return;

  // A modal option picker over the panel owns all input while open.
  if (overlayPopup.isActive()) {
    overlayPopup.handleInput(mappedInput, [this] {
      if (overlayPopup.isActive()) {
        paintOverlayPopup();  // highlight moved
        return;
      }
      // Dismissed or selected: erase the dialog -- clean page back, then the
      // panel over it (the dialog can overhang the sheet onto the page).
      RenderLock lock;
      settleOverlayRefresh();
      if (overlayPageStored) {
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
        renderOverlay();
        pushOverlayRefresh();
      } else {
        requestUpdate();
      }
    });
    return;
  }
  const auto fastRedraw = [this] {
    RenderLock lock;  // the render task shares the framebuffer
    settleOverlayRefresh();
    renderOverlay();
    pushOverlayRefresh();
  };

  // Jump to another spine item (chapter scrub). The overlay stays up and is
  // re-drawn over the new page by renderBook().
  const auto gotoSpine = [this](int target) {
    const int spineCount = epub->getSpineItemsCount();
    target = std::clamp(target, 0, spineCount - 1);
    if (target != currentSpineIndex) {
      RenderLock lock;
      clearDeferredReposition();
      nextPageNumber = 0;
      currentSpineIndex = target;
      section.reset();
    }
    requestUpdate();
  };
  const auto toolOverlay = [](int tool) {
    return tool == 0 ? Overlay::Contents : (tool == 1 ? Overlay::Text : Overlay::More);
  };

  // Touch first: FreeInkUI routes the frame against the tap targets the last
  // render registered and hands back the action it mapped to.
  const auto routed = toolbarUi->route(mappedInput);

  // --- Toolbar ---
  if (overlay == Overlay::Toolbar) {
    switch (routed.event) {
      case ReaderToolbarUi::Event::Dismiss:
        closeOverlayToPage();
        return;
      case ReaderToolbarUi::Event::Tool:
        focusedTool = routed.value;
        openOverlay(toolOverlay(focusedTool));
        return;
      case ReaderToolbarUi::Event::PrevChapter:
        gotoSpine(currentSpineIndex - 1);
        return;
      case ReaderToolbarUi::Event::NextChapter:
        gotoSpine(currentSpineIndex + 1);
        return;
      case ReaderToolbarUi::Event::Scrub:
        gotoSpine(static_cast<int>((static_cast<float>(routed.permille) / 1000.0f) *
                                       static_cast<float>(epub->getSpineItemsCount() - 1) +
                                   0.5f));
        return;
      default:
        break;
    }
    if (routed.routed) return;  // a touch frame the chrome consumed (or dead space)

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeOverlayToPage();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      focusedTool = (focusedTool + 2) % 3;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      focusedTool = (focusedTool + 1) % 3;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openOverlay(toolOverlay(focusedTool));
      return;
    }
    const bool prev = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool next = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (prev || next) {
      gotoSpine(currentSpineIndex + (next ? 1 : -1));
    }
    return;
  }

  // --- Panels (Contents / Text / More) ---
  const int count = overlay == Overlay::Contents ? epub->getTocItemsCount()
                    : overlay == Overlay::Text   ? kTextRowCount
                                                 : static_cast<int>(moreItems.size());
  const int pageRows = std::max(1, toolbarUi->visibleRows());

  // Activate the highlighted row: change a value / jump to a chapter / run an
  // action. Shared by the Confirm button and a row tap.
  const auto activateRow = [this, count, &fastRedraw] {
    if (panelIndex < 0 || panelIndex >= count) return;
    if (overlay == Overlay::Text) {
      if (panelIndex == 0) {
        // Full font picker (built-in + SD fonts, live preview) -- the same
        // screen Settings uses; a popup cannot scroll a long font list.
        overlay = Overlay::None;
        overlayPopup.dismiss();
        discardOverlayPage();
        {
          RenderLock lock;  // the picker paints the framebuffer next
          settleOverlayRefresh();
        }
        startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                      TextSettingsActivity::Tab::Family),
                               [this](const ActivityResult&) {
                                 applyReaderTextSettings();
                                 overlay = Overlay::Text;  // back to the Text panel
                                 panelIndex = 0;
                                 if (toolbarUi) toolbarUi->begin();  // the picker drew its own FUI screen
                                 requestUpdate();                    // re-render page + Text panel
                               });
      } else if (panelIndex == 4) {
        // Focus Reading is a genuine on/off: a tap toggles and applies live.
        SETTINGS.focusReadingEnabled = SETTINGS.focusReadingEnabled ? 0 : 1;
        applyTextSettingLive();
      } else {
        // Enum rows open the Settings-style option picker.
        showTextRowPopup(panelIndex);
      }
    } else if (overlay == Overlay::Contents) {
      const auto item = epub->getTocItem(panelIndex);
      if (item.spineIndex != -1) {
        RenderLock lock;
        clearDeferredReposition();
        currentSpineIndex = item.spineIndex;
        pendingAnchor = item.anchor;
        nextPageNumber = 0;
        section.reset();
      }
      overlay = Overlay::None;
      discardOverlayPage();
      requestUpdate();
    } else if (overlay == Overlay::More) {
      activateMoreRow(panelIndex);
    }
  };

  // Steps up to the toolbar -- the Back button and a tap on the page above
  // the sheet.
  const auto dismissPanel = [this, &fastRedraw] {
    overlay = Overlay::Toolbar;
    // Restore the snapshotted page under the toolbar instead of re-rendering
    // it (2+ refreshes -> one FAST). Re-store right away so another panel
    // round-trip can restore again.
    if (overlayPageStored) {
      {
        RenderLock lock;  // the render task shares the framebuffer
        settleOverlayRefresh();
        // No baseline resync: the glass shows the panel, and erasing it needs
        // the differential to keep diffing against the last pushed frame.
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
      }
      fastRedraw();  // takes its own RenderLock
      return;
    }
    requestUpdate();
  };

  // Pages the list by one screen of rows through the nav (measured page size,
  // no-op at the ends). A shown cursor rides along so the buttons continue
  // from what is visible; on touch boards only the viewport moves.
  const auto pageList = [this, count, pageRows, &fastRedraw](int direction) {
    if (count <= 0) return;
    const bool moved = toolbarUi->nav().scrollBy(direction * pageRows, count);
    if (panelCursorShown) {
      panelIndex = std::clamp(panelIndex + direction * pageRows, 0, count - 1);
      fastRedraw();
      return;
    }
    if (moved) fastRedraw();
  };

  switch (routed.event) {
    case ReaderToolbarUi::Event::Dismiss:
      dismissPanel();
      return;
    case ReaderToolbarUi::Event::Tool: {
      // Sheet-bottom tool switcher: hop straight to another panel.
      const Overlay target = toolOverlay(routed.value);
      if (target != overlay) {
        focusedTool = routed.value;
        openOverlay(target);
      }
      return;
    }
    case ReaderToolbarUi::Event::Row:
      // A tap on the right-edge strip pages the sheet instead (upper half =
      // previous page, lower half = next): swipes are unreliable on etched
      // glass, and a long contents list needs a fast way through.
      if (routed.x >= renderer.getScreenWidth() - 44) {
        pageList(routed.y >= renderer.getScreenHeight() - (renderer.getScreenHeight() * 62) / 200 ? 1 : -1);
        return;
      }
      panelIndex = routed.value;
      panelCursorShown = false;
      activateRow();
      return;
    default:
      break;
  }
  // Swipe up/down pages the list. Checked before the routed-frame return:
  // FUI routes every touch frame over the sheet, so a swipe's frames count as
  // routed (without dispatching -- too much travel for a tap) and the gesture
  // would otherwise never be seen.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    pageList(swipe == MappedInputManager::SwipeDir::Up ? 1 : -1);
    return;
  }
  if (routed.routed) return;  // consumed by the chrome (title band, dead space)

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    dismissPanel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateRow();
    return;
  }

  // Up/Down (side) and Left/Right (front) move the cursor: a tap steps one
  // row, holding past PANEL_HOLD_MS jumps PANEL_HOLD_STEP rows in one go, which
  // is how you cross a hundreds-of-chapters contents list without a press per
  // row. The jump fires once on the hold and swallows the release that ends it,
  // so it never doubles up with the tap step.
  if (count > 0) {
    const bool up = mappedInput.isPressed(MappedInputManager::Button::Up) ||
                    mappedInput.isPressed(MappedInputManager::Button::Left);
    const bool down = mappedInput.isPressed(MappedInputManager::Button::Down) ||
                      mappedInput.isPressed(MappedInputManager::Button::Right);
    if (!panelHoldJumped && (up || down) && mappedInput.getHeldTime() >= PANEL_HOLD_MS) {
      const int step = down ? PANEL_HOLD_STEP : -PANEL_HOLD_STEP;
      panelIndex = std::clamp(panelIndex + step, 0, count - 1);
      panelHoldJumped = true;
      panelCursorShown = true;
      fastRedraw();
      return;
    }

    const bool releasedUp = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool releasedDown = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (releasedUp || releasedDown) {
      if (!panelHoldJumped) {
        panelIndex = releasedUp ? ButtonNavigator::previousIndex(panelIndex, count)
                                : ButtonNavigator::nextIndex(panelIndex, count);
        panelCursorShown = true;
        fastRedraw();
      }
      panelHoldJumped = false;
    }
  }
}

// First paint of the option picker over the panel (and highlight repaints).
// The dialog draws over the current framebuffer without clearing; erasing it
// on dismissal is the popup gate's restore in handleOverlayInput().
void EpubReaderActivity::paintOverlayPopup() {
  RenderLock lock;
  settleOverlayRefresh();
  overlayPopup.render(renderer);
  pushOverlayRefresh();
}

void EpubReaderActivity::applyReaderTextSettings() {
  {
    RenderLock lock;
    // The layout spec changed: re-pagination does not change TOC titles, but a
    // cached title must never outlive a book/section swap, so drop it here too.
    cachedChapterTitleSpine = -1;
    cachedChapterTitleText.clear();
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    // Release the old parser and page table before settings serialization needs heap.
    section.reset();  // force re-pagination with the new settings
  }
  SETTINGS.saveToFile();
  // (Re)load or unload the selected SD-card font for the current family/size.
  // The reader otherwise only loads SD fonts on book open, so without this an
  // in-reader font change wouldn't take effect until re-opening the book.
  // Locked because the render task can load or unload SD font variants too.
  RenderLock lock;
  sdFontSystem.ensureLoaded(renderer);
}

// The More panel carries everything the classic list menu offers except the
// two entries that have their own tool (chapters -> Contents, text -> Text).
void EpubReaderActivity::buildMoreActions() {
  using MA = EpubReaderMenuActivity::MenuAction;
  EpubReaderMenuActivity::buildMenuItems(moreItems, !currentPageFootnotes.empty(), !cachedBookmarks.empty(),
                                         CLIPPINGS.hasClippings());
  moreItems.erase(std::remove_if(moreItems.begin(), moreItems.end(),
                                 [](const auto& item) {
                                   return item.action == MA::SELECT_CHAPTER || item.action == MA::TEXT_SETTINGS;
                                 }),
                  moreItems.end());
}

std::string EpubReaderActivity::moreRowName(int row) const {
  return row >= 0 && row < static_cast<int>(moreItems.size()) ? I18N.get(moreItems[row].labelId) : "";
}

std::string EpubReaderActivity::moreRowValue(int row) const {
  using MA = EpubReaderMenuActivity::MenuAction;
  static constexpr StrId kOrient[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED,
                                      StrId::STR_LANDSCAPE_CCW};
  static_assert(std::size(kOrient) == CrossPointSettings::ORIENTATION_COUNT, "orientation labels");
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return "";
  switch (moreItems[row].action) {
    case MA::ROTATE_SCREEN:
      return I18N.get(kOrient[SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT]);
    case MA::AUTO_PAGE_TURN:
      return (autoTurnOption == 0 || autoTurnOption >= static_cast<int>(std::size(PAGE_TURN_RATES)))
                 ? std::string(tr(STR_STATE_OFF))
                 : std::to_string(PAGE_TURN_RATES[autoTurnOption]);
    default:
      return "";
  }
}

void EpubReaderActivity::activateMoreRow(int row) {
  using MA = EpubReaderMenuActivity::MenuAction;
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return;
  {
    RenderLock lock;  // several actions launch screens that paint the framebuffer
    settleOverlayRefresh();
  }
  const auto action = moreItems[row].action;
  // In-place toggles keep the panel open and re-render the page beneath it.
  switch (action) {
    case MA::ROTATE_SCREEN: {
      static constexpr StrId kOrientIds[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                             StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
      static_assert(std::size(kOrientIds) == CrossPointSettings::ORIENTATION_COUNT, "orientation options");
      overlayPopup.show(StrId::STR_ORIENTATION, kOrientIds, static_cast<int>(std::size(kOrientIds)),
                        SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT, [this](int idx) {
                          if (idx == SETTINGS.orientation) return;
                          applyOrientation(static_cast<uint8_t>(idx));
                          // The stored page is laid out for the old orientation.
                          discardOverlayPage();
                          requestUpdate();
                        });
      paintOverlayPopup();
      return;
    }
    case MA::AUTO_PAGE_TURN: {
      std::vector<std::string> labels;
      labels.reserve(std::size(PAGE_TURN_RATES));
      labels.emplace_back(tr(STR_STATE_OFF));
      for (size_t i = 1; i < std::size(PAGE_TURN_RATES); ++i) labels.push_back(std::to_string(PAGE_TURN_RATES[i]));
      overlayPopup.show(StrId::STR_AUTO_TURN_PAGES_PER_MIN, labels, autoTurnOption, [this](int idx) {
        autoTurnOption = idx;
        toggleAutoPageTurn(static_cast<uint8_t>(idx));
      });
      paintOverlayPopup();
      return;
    }
    case MA::BLUETOOTH:
      EpubReaderMenuActivity::toggleBluetooth();
      {
        RenderLock lock;
        renderOverlay();
        pushOverlayRefresh();
      }
      return;
    case MA::NIGHT_MODE:
      SETTINGS.screenInverted = SETTINGS.screenInverted == 0 ? 1 : 0;
      SETTINGS.saveToFile();
      discardOverlayPage();
      requestUpdate();
      return;
    case MA::FRONTLIGHT: {
      const bool lightOn = !Frontlight.isOn();
      Frontlight.setOn(lightOn);
      SETTINGS.frontlightOn = lightOn ? 1 : 0;
      SETTINGS.saveToFile();
      {
        RenderLock lock;  // the render task shares the framebuffer
        renderOverlay();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      }
      return;
    }
    default:
      break;
  }
  // Leaf actions open their own screen / perform the action; close the overlay first.
  overlay = Overlay::None;
  if (action == MA::GO_TO_PERCENT && overlayPageStored) {
    // The percent dialog is a popup over the current frame: wipe the toolbar
    // chrome back to the clean page first so the dialog draws over the page,
    // not the sheet. No refresh push — the dialog's first frame carries it.
    RenderLock lock;
    settleOverlayRefresh();
    renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
    overlayPageStored = false;
  } else {
    discardOverlayPage();
  }
  if (action == MA::TOGGLE_BOOKMARK) {
    // No child activity here to trigger the re-render the list menu relies on:
    // show the same confirmation popup the long-press path does.
    addBookmark();
    showBookmarkMessage = true;
    bookmarkMessageTime = millis();
    requestUpdate();
    return;
  }
  onReaderMenuConfirm(action);
  // Actions that neither open a screen nor leave the reader (a sync with no
  // credentials, say) would otherwise leave the closed panel on screen.
  if (action != MA::GO_HOME && action != MA::DELETE_CACHE) requestUpdate();
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';
  int targetSpineIndex = sameFile ? currentSpineIndex : epub->resolveHrefToSpineIndex(hrefStr);

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    return;
  }

  if (savePosition) {
    // Full: drop the oldest entry so Back always returns from the newest jump.
    if (footnoteDepth == MAX_FOOTNOTE_DEPTH) {
      std::copy(savedPositions + 1, savedPositions + MAX_FOOTNOTE_DEPTH, savedPositions);
      footnoteDepth--;
    }
    // A child screen (menu, footnote list) may have released the section;
    // nextPageNumber then holds the page it was on.
    const int page = section ? section->currentPage : nextPageNumber;
    savedPositions[footnoteDepth] = {currentSpineIndex, page};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, page);
  }

  {
    RenderLock lock;
    clearDeferredReposition();
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::saveLinkStack() const {
  // [depth] then depth x (spine u16 LE, page u16 LE)
  uint8_t data[1 + MAX_FOOTNOTE_DEPTH * 4];
  size_t size = 0;
  data[size++] = static_cast<uint8_t>(footnoteDepth);
  for (int i = 0; i < footnoteDepth; i++) {
    const SavedPosition& pos = savedPositions[i];
    data[size++] = pos.spineIndex & 0xFF;
    data[size++] = (pos.spineIndex >> 8) & 0xFF;
    data[size++] = pos.pageNumber & 0xFF;
    data[size++] = (pos.pageNumber >> 8) & 0xFF;
  }
  HalFile f;
  if (!Storage.openFileForWrite("ERS", epub->getCachePath() + "/links.bin", f)) return;
  if (f.write(data, size) != size) LOG_ERR("ERS", "Failed to write link stack");
}

void EpubReaderActivity::loadLinkStack() {
  const std::string path = epub->getCachePath() + "/links.bin";
  if (!Storage.exists(path.c_str())) return;
  {
    HalFile f;
    uint8_t data[1 + MAX_FOOTNOTE_DEPTH * 4];
    if (Storage.openFileForRead("ERS", path, f)) {
      const int size = f.read(data, sizeof(data));
      const int depth = size > 0 ? data[0] : 0;
      if (depth >= 1 && depth <= MAX_FOOTNOTE_DEPTH && size == 1 + depth * 4) {
        const int spineCount = epub->getSpineItemsCount();
        bool valid = true;
        for (int i = 0; i < depth; i++) {
          const uint8_t* p = data + 1 + i * 4;
          savedPositions[i] = {p[0] | (p[1] << 8), p[2] | (p[3] << 8)};
          valid = valid && savedPositions[i].spineIndex < spineCount;
        }
        if (valid) {
          footnoteDepth = depth;
          LOG_DBG("ERS", "Loaded link stack, depth %d", depth);
        }
      }
    }
  }
  // Consumed once: a later exit rewrites it, and an unclean shutdown must not
  // resurrect a stale stack.
  Storage.remove(path.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  // C5 (upstream f7716534): no unconditional 16-slot reserve. It cost 16 * sizeof(BookmarkEntry) of
  // heap on EVERY book open -- including the many books with no bookmarks at all -- and
  // BookmarkFile::load() grows the vector to the real count anyway. This is a perf/heap change with
  // no visible difference; the cache still holds exactly the bookmarks the file contains.
  cachedBookmarks.clear();
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  BookmarkFile::load(epub->getPath(), cachedBookmarks);
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) return;
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock;
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    const std::optional<uint32_t> offset =
        currentPageVisibleOffset.has_value() ? currentPageVisibleOffset
        : (currentPage >= 0 && currentPage < section->pageCount)
            ? section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))
            : std::nullopt;
    if (offset.has_value()) {
      entry.visibleTextOffset = *offset;
      entry.hasVisibleTextOffset = true;
    }
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  if (!BookmarkFile::save(epub->getPath(), cachedBookmarks)) {
    LOG_ERR("ERS", "Failed to save bookmarks");
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

int EpubReaderActivity::getProgressBasisPoints() const {
  if (isAtEndOfBook()) return 10000;
  if (!epub || !section || epub->getBookSize() == 0) return getProgressPercent() * 100;
  const int totalPages = section->estimatedTotalPages();
  if (totalPages <= 0) return getProgressPercent() * 100;
  const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(totalPages);
  const int basisPoints =
      static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 10000.0f + 0.5f);
  return std::clamp(basisPoints, 0, 10000);
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  localPos.hasResolvedSpineIndex = true;
  localPos.hasMappedPage = true;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    if (const auto offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))) {
      localPos.visibleTextOffset = *offset;
      localPos.hasVisibleTextOffset = true;
    }
  }
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
