#include "ReaderActivity.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <KOReaderDocumentId.h>
#include <Memory.h>
#include <TrustedTime.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderActivity.h"
#include "ReadingStatsStore.h"
#include "ReadingStatsStore.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "XtcReaderActivity.h"
#include "util/PluginEvents.h"

ReaderActivity::ReaderActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::string bookPath, const bool allowFastInitialRefresh)
    : Activity(name, renderer, mappedInput), bookPath(std::move(bookPath)) {
  // Page turns are DU (~0.94 s) with a clean every N pages; the global in-screen refresh setting
  // would override that tuned cadence, so the reader keeps its own policy.
  transitionRefreshPolicy_ = false;
  if (allowFastInitialRefresh) {
    // The counter must honour the configured cadence exactly: "every 5 pages" means the 5th page
    // turn cleans, "every page" means every page. This used to clamp 1 up to 2, so the REFRESH_1
    // setting silently ran at half the requested rate.
    const int refreshFrequency = SETTINGS.getRefreshFrequency();
    pagesUntilFullRefresh = refreshFrequency < 1 ? 1 : refreshFrequency;
    pagesUntilQuickRefresh = SETTINGS.getQuickRefreshPages();
  }
}

std::unique_ptr<ReaderActivity> ReaderActivity::create(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       std::string path, const bool allowFastInitialRefresh) {
  // ActivityManager requires heap ownership; each branch allocates exactly one screen-lifetime object.
  std::unique_ptr<ReaderActivity> activity;
  if (FsHelpers::hasXtcExtension(path)) {
    activity = makeUniqueNoThrow<XtcReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  } else {
    activity = makeUniqueNoThrow<EpubReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  }

  if (!activity) {
    LOG_ERR("READER", "OOM: reader activity");
  }
  return activity;
}

void ReaderActivity::applyInitialOrientation() { ReaderUtils::applyOrientation(renderer, SETTINGS.orientation); }

void ReaderActivity::disableFastInitialRefresh() { pagesUntilFullRefresh = 0; }

void ReaderActivity::notePageTurn(const bool forward, const bool succeeded) {
  RenderLock lock(*this);
  readerSession.noteTurn(forward, succeeded);
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  // Heap ledger for field crash reports: free vs largest block distinguishes a
  // leak (free falls) from fragmentation (free stable, largest collapses).
  LOG_INF("MEM", "reader enter: free=%u max_block=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  if (!Storage.exists(bookPath.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", bookPath.c_str());
    finish();
    return;
  }

  // Clear remembered book after opening it
  if (!APP_STATE.openEpubPath.empty()) {
    APP_STATE.openEpubPath.clear();
    APP_STATE.saveToFile();
  }

  sdFontSystem.ensureLoaded(renderer);
  applyInitialOrientation();

  if (!loadBook()) {
    if (!handleLoadFailure()) finish();
    return;
  }

  // A reader entry is an ARRIVAL over a frame THIS activity never drew: Home, the KOReader sync
  // screen, a post-reboot popup, the sleep chip. The panel latch still describes that other
  // screen, so without this the first paint of the book can resolve to a diff (HALF/DU) against
  // a frame the panel's software baseline believes it drove -- and if that baseline and the glass
  // ever disagree, the diff never restores the missing pixels and the book reads as a blank page
  // while every later overlay (popup, sleep screen) still paints. forceCleanNextRefresh() is the
  // policy's non-cancellable from-white term, so the first page paint of every entry is a
  // whole-panel clean: one ~815 ms drive per book open, never a diff against somebody else's
  // frame. Idempotent with the wake arm in main.cpp and with a boot whose latch is already clean.
  renderer.forceCleanNextRefresh();

  requestUpdate();
}

void ReaderActivity::rememberBookOnceRendered() {
  if (bookRemembered || !pageRendered.load(std::memory_order_acquire)) return;
  bookRemembered = true;
  APP_STATE.openEpubPath = bookPath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(bookPath, getBookTitle(), getBookAuthor(), getBookThumbBmpPath());
  const pluginevents::Var openVars[] = {{"book", bookPath.c_str()}};
  pluginevents::emit(pluginevents::Event::ReaderOpen, openVars, 1);
}

void ReaderActivity::onExit() {
  Activity::onExit();

  // Keep rebuildable font buffers from pinning the heap between reading sessions.
  if (auto* fontCache = renderer.getFontCacheManager()) {
    fontCache->releaseSdFontCaches();
  }

  LOG_INF("MEM", "reader exit: free=%u max_block=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  // Flush BEFORE the ReaderExit event: the session's final progress must be
  // durable before a subscriber can act on the exit notification.
  flushReaderSession();

  if (pluginevents::anySubscriber(pluginevents::Event::ReaderExit)) {
    char percent[8];
    snprintf(percent, sizeof(percent), "%d", getScreenshotInfo().progressPercent);
    const pluginevents::Var vars[] = {{"book", bookPath.c_str()}, {"percent", percent}};
    pluginevents::emit(pluginevents::Event::ReaderExit, vars, 2);
  }

  // Returning to the home UI: restore the session's orientation instead of forcing Portrait.
  // Hard-coding Portrait here left the renderer in portrait after every book, and the home UI
  // does not set an orientation of its own -- it reads renderer.getOrientation() -- so on a
  // landscape-native panel the whole UI came back rotated 90 degrees.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  endOfBookOptions.reset();
  endOfBookOptionsReady.store(false, std::memory_order_release);
}

void ReaderActivity::prepareForSleep() { flushReaderSession(); }

void ReaderActivity::checkpointReadingStats() {
  if (readerSession.activeMillis() < kStatsCheckpointMs) return;
  recordReadingStats();
}

void ReaderActivity::recordReadingStats() {
  if (bookPath.empty() || !readerSession.hasActivity()) return;
  uint32_t pages = 0;
  uint32_t activeMs = 0;
  readerSession.takeStatistics(pages, activeMs);
  // One write per checkpoint, never one per page turn: the counters above are plain
  // increments, and only this call reaches the card.
  READING_STATS.record(bookPath, getBookTitle(), pages, activeMs / 1000u);
}

void ReaderActivity::flushReaderSession() {
  // r163 3.3 -- reading statistics. Hand this session's counters to the store before the
  // reset AND regardless of what the plugin event decides: a card with no subscribers and
  // no synced clock still reads, so its pages and dwells must still be counted.
  recordReadingStats();

  if (!readerSession.isEmitWorthy() || !pluginevents::anySubscriber(pluginevents::Event::ReaderSession)) {
    readerSession.reset();
    return;
  }

  const std::string document = KOReaderDocumentId::calculate(bookPath);
  const bool validDocument =
      document.size() == 32 && std::all_of(document.begin(), document.end(), [](const unsigned char c) {
        return std::isdigit(c) || (c >= 'a' && c <= 'f');
      });
  if (validDocument) {
    char startTime[24];
    char endTime[24];
    char duration[16];
    char startProgress[8];
    char endProgress[8];
    snprintf(startTime, sizeof(startTime), "%lld", static_cast<long long>(readerSession.startTime()));
    snprintf(endTime, sizeof(endTime), "%lld", static_cast<long long>(readerSession.endTime()));
    snprintf(duration, sizeof(duration), "%lu", static_cast<unsigned long>(readerSession.durationSeconds()));
    snprintf(startProgress, sizeof(startProgress), "%u", readerSession.startProgressBp());
    snprintf(endProgress, sizeof(endProgress), "%u", readerSession.endProgressBp());
    const pluginevents::Var vars[] = {{"book", bookPath.c_str()},       {"document", document.c_str()},
                                      {"start_time", startTime},        {"end_time", endTime},
                                      {"duration_seconds", duration},   {"start_progress_bp", startProgress},
                                      {"end_progress_bp", endProgress}, {"progress_scale", "10000"}};
    pluginevents::emit(pluginevents::Event::ReaderSession, vars, 8);
  }
  readerSession.reset();
}

bool ReaderActivity::handleBackNavigation() {
  // r163: no goHome callback -- Home is the manager's HOLD now, and this handler only answers the tap.
  return ReaderUtils::handleBackNavigation(mappedInput, activityManager, bookPath.c_str());
}

void ReaderActivity::clearEndOfBookOptionsIfNeeded() {
  if (isAtEndOfBook() || !endOfBookOptionsReady.load(std::memory_order_acquire)) return;

  RenderLock lock(*this);
  endOfBookOptionsReady.store(false, std::memory_order_release);
  endOfBookOptions.reset();
}

bool ReaderActivity::endOfBookMenuActive() const {
  return isAtEndOfBook() && endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->menuActive();
}

bool ReaderActivity::handleEndOfBookMenu(const bool suppressConfirmRelease) {
  if (suppressConfirmRelease || !endOfBookMenuActive()) {
    return false;
  }

  std::string openPath;
  switch (endOfBookOptions->handleMenuInput(mappedInput, &openPath)) {
    case EndOfBookOptions::Action::OpenBook:
      activityManager.goToReader(openPath);
      return true;
    case EndOfBookOptions::Action::GoHome:
      onGoHome();
      return true;
    case EndOfBookOptions::Action::LastPage:
      onReturnFromEndOfBook();
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::Redraw:
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::None:
      return false;
  }

  return false;
}

bool ReaderActivity::handleEndOfBookPageTurn(const bool prevTriggered, const bool nextTriggered) {
  if (!isAtEndOfBook()) return false;

  if (endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->menuActive()) {
    return true;
  }
  if (nextTriggered) {
    onGoHome();
  } else if (prevTriggered) {
    onReturnFromEndOfBook();
    requestUpdate();
  }
  return true;
}

void ReaderActivity::loop() {
  rememberBookOnceRendered();
  // r163 3.3: a checkpoint of the reading statistics every kStatsCheckpointMs of counted
  // reading. Costs one comparison per pass and one SD write per window, so a pulled
  // battery loses at most that window instead of the whole session.
  checkpointReadingStats();
  clearEndOfBookOptionsIfNeeded();
  if (handleEndOfBookMenu()) return;
  if (handleFormatInput()) return;
  if (handleBackNavigation()) return;

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;

  // r163 F5: hold-to-repeat. A held page button keeps turning at 10 pages/s once
  // a short dwell passes, on a cadence anchored to the PRESS timestamp (see
  // util/PageRepeat.h) so a slow pass cannot shift the rate. It owns the gesture
  // only in OFF mode, where a turn fires on the press and the hold is otherwise
  // idle: CHAPTER_SKIP and ORIENTATION_CHANGE bind the long press themselves, and
  // a touch turn or a tilt is a one-shot, so the repeater stays disarmed there.
  const bool repeatEligible = SETTINGS.longPressButtonBehavior == CrossPointSettings::OFF && !fromTilt &&
                              !touch.prev && !touch.next;
  bool repeatTick = false;
  if (repeatEligible) {
    const int heldDir = ReaderUtils::heldPageTurnDirection(mappedInput);
    if (heldDir == 0) {
      // Release (or a second button joining the hold): this press is over.
      pageRepeat.reset();
    } else if (!pageRepeat.armed() || pageRepeat.direction() != heldDir) {
      // Arm on the press edge from when the press actually started -- the held
      // time already discounts a press that predates this screen.
      pageRepeat.arm(heldDir, millis() - mappedInput.getHeldTime());
    } else {
      repeatTick = pageRepeat.due(millis());
    }
  } else {
    pageRepeat.reset();
  }

  if (!prevTriggered && !nextTriggered && !repeatTick) return;
  // Both directions in one frame is not a turn. The ternary below used to collapse it to "forward"
  // (nextTriggered), so two opposite presses -- or an edge swipe landing with a button -- turned the
  // page in a direction nobody asked for. heldPageTurnDirection() already reads "both" as no
  // direction; the edge path now agrees, and the frame simply does nothing.
  if (!repeatTick && prevTriggered && nextTriggered) return;
  const bool forward = repeatTick ? pageRepeat.direction() > 0 : nextTriggered;
  if (handleEndOfBookPageTurn(forward, !forward)) return;

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool skip = !repeatTick && !fromTilt && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP &&
                    heldMs >= ReaderUtils::SKIP_HOLD_MS;

  const bool changed = skip ? skipPages(forward ? 10 : -10) : pageTurn(forward);
  // A repeat that cannot move is a step failure (page boundary, failed turn):
  // stop for the rest of this press rather than retrying ten times a second.
  if (repeatTick && !changed) pageRepeat.stop();
  // A skip is navigation, not reading: it never counts toward session dwell.
  notePageTurn(!skip && forward, changed);
  // r163 3.14: a turn that was asked for and did not move the page is the ERROR class --
  // three short pulses, the only pattern here that repeats three times, so it cannot be
  // mistaken for the hold's double or the power-off's slow double. Stashed, never inline:
  // the pulse must not block the frame this press acknowledged.
  if (!changed) haptic_feedback::deferErrorAction();
  if (changed && (touch.prev || touch.next)) {
    // A plain page turn is PAGE-TURN class (silent under "On Touch"); the long-press skip is the
    // hold feedback for a touch action, so it keeps the touch class.
    if (skip) {
      haptic_feedback::deferTouchAction(true);
    } else {
      haptic_feedback::deferPageTurnAction();
    }
  }
  requestUpdate();
}

void ReaderActivity::render(RenderLock&&) {
  if (isAtEndOfBook()) {
    if (!endOfBookOptions) {
      endOfBookOptions = makeUniqueNoThrow<EndOfBookOptions>(renderer);
      if (!endOfBookOptions) LOG_ERR("READER", "OOM: EndOfBookOptions");
    }
    // EOB1 (r111): the clear is NO LONGER unconditional -- it used to run on
    // entry AND on every selection repaint, which made the framebuffer-vs-glass
    // diff the whole panel every time (a cleared canvas leaves nothing for the
    // partial path to mask). eobpanel::plan() is the only place that decides
    // which appearance this is:
    //   Enter  -- the page underneath is replaced: clear + full chrome, and ONE
    //             from-white GC16 pinned, so the menu-transition setting cannot
    //             downgrade a whole-frame replacement to a diff.
    //   Select -- the canvas already carries the panel: no clear, only the rows
    //             whose paint changed, one DU (the ~310 ms class) with the
    //             row-9 completion suppressed and the binary lane pinned.
    // Both pins are armed only AFTER the window is known, so a repaint that
    // paints nothing can never leave a one-shot armed for the reader's next paint.
    int first = -1;
    int last = -1;
    const bool chromeValid = endOfBookOptions && endOfBookOptions->chromeValid();
    const bool selectionMoved = endOfBookOptions && endOfBookOptions->selectionChanged(&first, &last);
    if (!chromeValid) {
      const eobpanel::Plan plan = eobpanel::plan(eobpanel::Event::Enter);
      if (endOfBookOptions) {
        endOfBookOptions->loadOnce(bookPath);
        // Release-publish AFTER loadOnce() so the main task's acquire load can't
        // observe an object whose names/selector are still being populated.
        endOfBookOptionsReady.store(true, std::memory_order_release);
      }
      if (plan.clearScreen) renderer.clearScreen();
      if (plan.promoteFull) renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
      if (endOfBookOptions) endOfBookOptions->render(renderer, mappedInput);
      renderer.displayBuffer();
    } else if (selectionMoved) {
      const eobpanel::Plan plan = eobpanel::plan(eobpanel::Event::Select, first, last);
      if (plan.promoteFast) renderer.promoteNextRefresh(HalDisplay::FAST_REFRESH);
      if (plan.suppressCompletion) renderer.suppressCompletionDriveOnce();
      if (plan.binaryFast) renderer.forceBinaryDuOnce();
      if (endOfBookOptions->renderSelection(renderer)) {
        renderer.displayBuffer();
      }
    }
    onEndOfBookRendered();
    markPageRendered();
    readerSession.onRenderComplete(millis(), trustedtime::trustedNow(), getProgressBasisPoints());
    return;
  }

  renderBook();
  readerSession.onRenderComplete(millis(), trustedtime::trustedNow(), getProgressBasisPoints());
}

bool ReaderActivity::handleForcedRefresh() {
  {
    RenderLock lock(*this);
    pagesUntilFullRefresh = 1;
    forcedRefreshPending = true;
    // HALF is still a diff update, so on an unchanged page it drives nothing. A manual refresh has to
    // be a real full drive: promote this one paint to FULL (the panel then drives from a white
    // baseline instead of a zero difference).
    renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
  }
  requestUpdate();
  return true;
}
