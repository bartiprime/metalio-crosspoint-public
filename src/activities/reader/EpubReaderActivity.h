#pragma once

#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/PageLink.h>
#include <Epub/Section.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "BookmarkEntry.h"
#include "ChapterPosition.h"
#include "ChapterPrefetch.h"
#include "ClipSelectionActivity.h"
#include "EpubReaderMenuActivity.h"
#include "LinkNoteTitle.h"
#include "ProgressMapper.h"
#include "ReaderActivity.h"
#include "ReaderHandoff.h"
#include "ReaderToolbarUi.h"
#include "activities/util/EdgeDragStep.h"
#include "components/OptionPopup.h"

class EpubReaderActivity final : public ReaderActivity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  // r163 C3: the session a prepared result belongs to -- (Section, spine, layout), bumped in
  // renderBook when any of the three changes and read by the loop task. See ReaderHandoff.h.
  readerhandoff::SessionGeneration sessionGeneration_;
  int currentSpineIndex = 0;
  // r96.3 D1 (CP_IMG_CLEAN_ON_ARRIVAL): THE PAGE THAT IS ON THE GLASS, (spine, page),
  // -1/-1 = nothing displayed yet. This is a TRANSITION record, not a "have I cleaned
  // this page before" guard.
  //
  // r96/r96.1/r96.2 kept a `cleanImageSpine_/Page_` pair meaning "the page I last
  // cleaned", and armed the arrival clean only when the page differed FROM THAT PAIR.
  // The pair was never released when the reader left the page, so the guard could only
  // ever fire once per image page for the lifetime of the activity: on the operator's
  // device, flipping image -> text -> image cleaned once and then never again, and only
  // a re-entry into the book (a fresh activity, fresh members) cleaned again. That is
  // exactly what was reported: "it only happens when you first open the book".
  //
  // What the operator actually asked for is the ARRIVAL, so that is what is modelled:
  // remember the page that was last put on the glass, and clean when the page arriving
  // now is a DIFFERENT one and it has images. Consequences, all intended and each one
  // matching a request:
  //   * image -> text -> image  : both arrivals clean (the record moved in between);
  //   * repaint of the current page (status bar, clipping highlight, menu over it,
  //     an idle prewarm pass) : the record HAS NOT MOVED, so nothing cleans;
  //   * leaving the page (a departure): nothing cleans on the way out -- the arrival
  //     of the NEXT page decides;
  //   * leaving the book and coming back: `goToReader` builds a NEW EpubReaderActivity,
  //     so these members are -1 again and the first page is an arrival. Home is not a
  //     page, so arriving back on the same page legitimately is one.
  // There is no per-book guard left to get stuck.
  int displayedSpine_ = -1;
  int displayedPage_ = -1;
  // r96.2 D1, kept: the arm's INTENT, committed only when the paint that followed
  // actually consumed the flag (renderer.forcedCleanPaintsConsumed()). r96.3 adds that a
  // pending arm also keeps the arrival ARMED for the next paint of the same page -- the
  // retry r96.1 lost -- without re-cleaning once it has landed.
  int cleanArmedSpine_ = -1;
  int cleanArmedPage_ = -1;
  uint32_t cleanArmCounter_ = 0;
  bool displayedCleanPending_ = false;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  std::string pendingAnchor;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  std::optional<uint32_t> currentPageVisibleOffset;
  std::optional<uint32_t> pendingOffsetJump;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  int8_t pendingManualTurn = 0;
  bool pendingManualTurnTouch = false;
  // r163 F5: the queued turn came from a hold-repeat tick (see pageRepeat).
  bool pendingManualTurnRepeat = false;
  bool pendingPercentJump = false;
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool showFontDamagedMessage = false;
  unsigned long fontDamagedMessageTime = 0UL;
  bool showClippingMessage = false;
  StrId clippingMessage = StrId::STR_CLIPPING_SAVED;
  unsigned long clippingMessageTime = 0UL;
  bool currentPageBookmarked = false;
  // r163 A2: the glyph lookahead. The reader already prewarmed page+1 once per idle period;
  // this walks a window of pages ahead, one page per idle frame, and the window's depth is per
  // font (see idlePrewarmDepth()). State is the window itself, not "the last page warmed": the
  // window is anchored on the page on screen and abandoned the moment that page changes.
  int prewarmSpine = -1;
  // The session the window was opened in. A chapter turn or a layout change bumps the session and
  // abandons the window; a page turn re-anchors it on the new page (same session).
  uint32_t prewarmSessionGeneration = 0;
  int prewarmAnchorPage = -1;  // the page the window was opened from
  int prewarmNextPage = -1;    // next page in the window still to warm
  int prewarmFontId = -1;      // the font the measurement below belongs to
  unsigned long prewarmAvgMs = 0;     // smoothed per-page glyph-scan cost for prewarmFontId
  unsigned long prewarmWindowMs = 0;  // time the current window has already spent warming
  // Hard cap on the lookahead, whatever a font's measured cost is: six pages is a screen and a
  // half of text in front of the reader, and warming past it costs heap the build may want.
  static constexpr int PREWARM_MAX_DEPTH = 6;
  // Today's lookahead was the single page+1, and warming one page was worth doing on every font.
  // The floor is the second page: this adds to the old behaviour rather than trading it away.
  static constexpr int PREWARM_MIN_DEPTH = 2;
  static constexpr unsigned long PREWARM_DEBOUNCE_MS = 400;
  // Whole-window budget. A slow font may earn the full depth, but idle time is not free: once
  // this much has been spent the window stops until the next turn re-anchors it.
  static constexpr unsigned long PREWARM_WINDOW_BUDGET_MS = 150;
  // Measured cost per extra page of lookahead: a font that scans in ~10 ms per page earns one
  // step, one that scans in ~40 ms goes to the cap.
  static constexpr unsigned long PREWARM_MS_PER_DEPTH_STEP = 10;
  int idlePrewarmDepth() const;
  // All the conditions a warm may run under. Requires the render lock to be held.
  bool idlePrewarmMayRun() const;
  // One page of the lookahead, at most, per call: the window advances one page per idle frame so
  // a queued turn is never waiting behind a loop.
  void idlePrewarmTick();
  // ---- r163 C1: the next chapter's first page, prepared on an idle frame --------------------
  //
  // NOT a task, and not "prefetch the chapter". A chapter build holds a Section, rasterises into
  // the font cache and can borrow the framebuffer, all three of which the handoff contract's rule
  // 1 forbids a worker (ReaderHandoff.h:15-18); and a chapter open only blocks until its *target
  // page* exists (R163_CHAPTER_LOAD_TRACE.md §1f), so what is worth moving off the turn is the
  // inflate + parser prefix + first layout chunk for the next spine. The decisions live in
  // ChapterPrefetch.h so the host can test them; this side only reads state and does the I/O.
  //
  // The prepared section owns `sections/<spine>.pre.bin` (Section::Cache::Prefetch), so it can
  // never collide with the live chapter's file or its `.part` temp, and the live Section is
  // never touched: discard is a delete of its own file, adoption is a rename into an empty slot.
  std::unique_ptr<Section> prefetchSection;
  chapterprefetch::State prefetchState;
  // The four groups of gates, read from the reader on the frame that would prepare.
  chapterprefetch::Live prefetchLive() const;
  chapterprefetch::Gates prefetchGates(bool displayOwned) const;
  // One idle frame's worth of preparation, or a discard. True when it used the frame (so the
  // glyph lookahead does not compete with it on the same frame).
  bool chapterPrefetchTick();
  // Called on the render task at the top of a chapter open, before any Section exists: renames a
  // valid prepared file into the live slot so the open skips its build entirely. False -- the
  // unchanged path -- for every other case.
  bool adoptPreparedChapter();
  // Drops the artefact. `no writes` is the point: the section is abandoned, not suspended, so an
  // invalidation costs nothing but the delete.
  void discardPreparedChapter();
  unsigned long lastRenderCompleteMs = 0;
  bool bookmarkRemoved = false;
  std::vector<BookmarkEntry> cachedBookmarks;
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  bool pendingReadFolderMove = false;

  // Toolbar reader menu (SETTINGS.readerMenuStyle == READER_MENU_TOOLBAR): drawn
  // over the page instead of pushing the full-screen list menu. Select opens the
  // Toolbar; its tools open the Contents/Text/More bottom-sheet panels.
  enum class Overlay { None, Toolbar, Contents, Text, More };
  Overlay overlay = Overlay::None;
  int focusedTool = 0;  // toolbar tool focus: 0=Contents, 1=Text, 2=More
  int panelIndex = 0;   // selected row within the active panel
  // Panel list navigation: a tap steps one row, a hold jumps PANEL_HOLD_STEP rows in one go
  // (a contents list runs to hundreds of chapters). One jump per hold, not a repeat -- every
  // step repaints the panel, so repeating is bounded by the e-ink refresh anyway and reads as
  // sluggish. True once a hold has jumped, so the release that ends it is swallowed.
  static constexpr unsigned long PANEL_HOLD_MS = 1500;
  static constexpr int PANEL_HOLD_STEP = 10;
  bool panelHoldJumped = false;
  // Whether the panel draws its cursor row. Button boards always do; touch
  // boards only once a button has moved it, so a tapped row is not left inverted.
  bool panelCursorShown = false;
  // FreeInkUI chrome + tap targets for the overlay; created when it opens,
  // released when it closes.
  std::unique_ptr<ReaderToolbarUi> toolbarUi;
  // Modal option picker over the panel (same component the Settings screens
  // use), for enum rows: font size / line spacing / alignment / orientation /
  // auto page turn. Toggle rows stay one-tap toggles, as in Settings.
  OptionPopup overlayPopup;
  // True while a clean-page snapshot (renderer.storeBwBuffer) backs the open
  // overlay, letting panel->toolbar steps restore the page without a full
  // re-render. Discarded on close / whenever the page under the overlay changes.
  bool overlayPageStored = false;
  // A background build step lent the framebuffer: it came back white while the panel still shows the
  // page. Until renderBook() redraws, nothing may be painted straight onto it. Set by the loop task,
  // cleared by the render task.
  std::atomic<bool> pageBufferStale{false};
  // True while a deferred overlay chrome refresh (pushOverlayRefresh) may still
  // be running on the panel. settleOverlayRefresh() must run before the
  // framebuffer is touched or another differential refresh is pushed.
  bool overlayRefreshPending = false;
  void pushOverlayRefresh();
  void settleOverlayRefresh();
  // FL1: side-edge frontlight drag (SETTINGS.edgeDragFrontlight, default OFF).
  // A contact that starts in a side band and travels vertically past the
  // tap-slop distance is claimed and sweeps brightness (left) or warmth (right);
  // see activities/util/EdgeDragStep.h for the arm zone and the collision guard.
  edgedrag::State edgeDrag;
  // True while a clean page snapshot (renderer.storeBwBuffer) backs the drag, so
  // the bar can be drawn over the page and erased back to it without a re-render.
  // The renderer has ONE stored-BW slot, shared with the overlay/page path, so
  // the flag is only honoured while no page render has happened since the store
  // (`edgeDragStoreRenderMs` is the reader's render clock at that moment).
  bool edgeDragPageStored = false;
  unsigned long edgeDragStoreRenderMs = 0;
  bool edgeDragSnapshotCurrent() const;
  void edgeDragDropSnapshot();
  bool handleEdgeDrag();
  void edgeDragFillConfig(edgedrag::Config& c) const;
  void applyEdgeDragValue(bool persist);
  void drawEdgeDragBar() const;
  void paintEdgeDrag(bool erase);
  int autoTurnOption = 0;  // current auto page-turn rate index (More panel)
  std::vector<EpubReaderMenuActivity::MenuItem> moreItems;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  std::vector<PageLink> currentPageLinks;
  int currentPageLinkMarginLeft = 0;
  int currentPageLinkMarginTop = 0;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;
  // The back-stack outlives the reader (sleep, home) in links.bin so Back
  // still returns to where a followed link was tapped.
  void saveLinkStack() const;
  void loadLinkStack();

  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  bool partialRebuildStartFailed = false;

  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  // Requires the render lock; heap admission is checked separately by the build tick.
  bool backgroundBuildWanted() const;
  bool buildTickHeapGate();
  bool buildHeapPaused = false;
  // One bounded build step on the loop task, in a frame whose input did nothing. Lays out about
  // BACKGROUND_BUILD_PAGES_PER_TICK pages (one 1 KB parse step may add more) and hands the loop
  // back as soon as a button is down, so a short press is seen before more pages are built.
  // True when this frame did build work, which is what keeps the glyph lookahead off a frame the
  // build already owns (r163 A2).
  bool backgroundBuildTick();
  bool anyButtonPressed() const;
  // A background build failed in this spine. The error would likely recur, so from then on the
  // spine is built only as far as the reader goes; pages from earlier build steps stay readable.
  int backgroundBuildFailedSpine = -1;
  // Bluetooth page turner: every chapter build stops the radio until the chapter is built and its
  // page is shown. False: the radio still holds or is taking the heap, and the build must not run.
  std::atomic<bool> radioReleasedForBuild{false};
  bool makeRoomForChapterBuild();
  static constexpr uint32_t BUILD_ROOM_WAIT_MS = 5000;
  static constexpr uint32_t BUILD_ROOM_STEP_MS = 50;
  bool inputOverPage() const override { return overlay != Overlay::None || ReaderActivity::inputOverPage(); }
  // "Connecting Bluetooth…" or "Bluetooth failed" in place of the status bar title.
  mutable LinkNoteTitle linkNoteTitle;
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  bool buildPopupPending = false;
  void showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh);
  bool applyDeferredReposition();
  void clearDeferredReposition();
  void rememberCurrentContentOffset();
  // r163 C2/C4: accepted, not necessarily written. The position merges into the book's pending
  // snapshot (ProgressSaves.h) and a coalesced flush hands it to the background writer; a `false`
  // here means the snapshot could not even be accepted, and the caller must retry, not assume the
  // page was remembered.
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Written before this returns: for the paths that must be durable to continue (the KOReader
  // handover releases the book, the cache clear deletes the directory the record lives in).
  bool saveProgressNow(int spineIndex, int currentPage, int pageCount);
  // Hands the pending snapshot to the writer, if the coalescing window has closed. Called from the
  // loop's idle tail; `wait` is the exit path, which insists the position reaches the card.
  bool flushAcceptedProgress(bool wait);
  // The coalescing window: once 400 ms have passed since the last accept, the snapshot is handed to
  // the writer. Called from the loop's idle tail, which is where a page-turn burst settles.
  void progressFlushTick();
  // The background writes' completions, through the fixed slots of ReaderHandoff.h. Cheap: an
  // atomic load and a slot read per frame, no card and no callback.
  void pollProgressWrites();
  // Bounded wait for a flush, so an exit does not race the writer task.
  void waitForProgressFlush();
  // Written by the render task (accept) and read by the loop task (the coalescing window).
  std::atomic<uint32_t> lastProgressAcceptMs{0};
  bool progressRefusalLogged = false;
  static constexpr unsigned long PROGRESS_COALESCE_MS = 400;
  static constexpr unsigned long PROGRESS_FLUSH_WAIT_MS = 250;
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  int currentTocIndex() const;
  // Live section position, or the values cached before a child screen
  // released the section.
  ChapterPosition chapterPosition() const;
  int bookPercentFor(const ChapterPosition& position) const;
  void openReaderMenu();
  // Toolbar reader menu (see Overlay above).
  bool usesToolbarMenu() const;
  void openOverlay(Overlay target);
  void closeOverlayToPage();
  void discardOverlayPage();
  void handleOverlayInput();
  void renderOverlay();
  // Resolve a spine index to its chapter title. The status bar, the toolbar and
  // the clipping exporter all ask the same question several times per page turn,
  // and every miss is an SD round trip through BookMetadataCache (spine LUT seek
  // + read, TOC LUT seek + read). Memoise the last answer per spine -- a page turn
  // inside a chapter then costs zero metadata reads instead of two per repaint.
  std::string chapterTitleForSpine(int spineIndex) const;
  std::string currentChapterTitle() const;
  // Key of cachedChapterTitleText; -1 = empty. Mutable because renderStatusBar()
  // and the toolbar model are const.
  mutable int cachedChapterTitleSpine = -1;
  mutable std::string cachedChapterTitleText;
  // Text panel rows (font, size, line spacing, alignment, focus reading).
  std::string textRowName(int row) const;
  std::string textRowValue(int row) const;
  void showTextRowPopup(int row);
  // Persist + re-paginate + re-render under the open panel (live preview).
  void applyTextSettingLive();
  void paintOverlayPopup();
  // Persist the reader text settings, (re)load the selected SD font, and
  // re-paginate the current chapter so changes apply without re-opening the book.
  void applyReaderTextSettings();
  // More panel rows.
  void buildMoreActions();
  std::string moreRowName(int row) const;
  std::string moreRowValue(int row) const;
  void activateMoreRow(int row);
  void openFootnoteSelect(bool reopenMenuOnCancel);
  void openDictionaryWordSelect();
  // C14 (upstream aaf50e44): selection to restore when returning from a dictionary lookup.
  ClipSelectionActivity::Selection pendingLookupSelection;
  ClipSelectionActivity::Selection lookupReturnSelection;
  void startClipSelection(int initialX = -1, int initialY = -1,
                          ClipSelectionActivity::Mode mode = ClipSelectionActivity::Mode::Clip);
  int clippingAtPoint(const Page& page, int x, int y) const;
  void openClippings();
  void applyProgressChange(const ProgressChangeResult& progress);
  bool launchKOReaderSync();
  unsigned long confirmLongPressThreshold() const;
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();

  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void drawClippingHighlights(const Page& page, int fontId, int orientedMarginTop, int orientedMarginLeft) const;
  void renderStatusBar() const;
  void applyOrientation(uint8_t orientation);
  void applyInitialOrientation() override;
  // The orientation the current layout was built for. The control center's
  // orientation tile can move SETTINGS.orientation while this reader sits on
  // the activity stack, and Pop restores it without onEnter(), so the drift has
  // to be noticed here rather than assumed away.
  uint8_t appliedOrientation = 0;

  // Modal shown when a protected book refuses to open (loan expired /
  // date unverified); OK exits, "Sync time" (when offered) verifies the
  // clock over Wi-Fi and reopens the book.
  OptionPopup loadFailurePopup;
  // Protection error captured by loadBook() for handleLoadFailure(); the
  // failed Epub itself does not outlive loadBook().
  std::string loadProtectionError;

  bool loadBook() override;
  bool handleLoadFailure() override;
  // Wi-Fi join + SNTP for a loan whose date could not be verified, then a
  // clean re-open of the book.
  void beginLoanTimeSync();
  std::string getBookTitle() const override { return epub ? epub->getTitle() : ""; }
  std::string getBookAuthor() const override { return epub ? epub->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return epub ? epub->getThumbBmpPath() : ""; }
  int getProgressBasisPoints() const override;
  void renderBook() override;
  void onEndOfBookRendered() override;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                              bool allowFastInitialRefresh)
      : ReaderActivity("EpubReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~EpubReaderActivity() override;

  void loop() override;
  // r163 C4: the last accepted position reaches the card before the reader lets go of the book --
  // on leaving the screen and before sleep. Both flush the snapshot, never a live read of the
  // section: at this point that would be a position the reader has already left behind.
  void onExit() override;
  void prepareForSleep() override;
  // r163: the override that claimed the Back HOLD (short = Home, hold = file browser) is gone. The
  // reader no longer inverts the pair, so the manager's hold -- goHome at kLongPressBackHomeMs -- is
  // exactly what a book should do, and the release that follows it is suppressed there before this
  // activity can see it. handleBackNavigation() answers the tap: leave the book for the browser.
  void render(RenderLock&& lock) override;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool remoteShortcut(bool readerMenu) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  bool skipLoopDelay() override;

  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
