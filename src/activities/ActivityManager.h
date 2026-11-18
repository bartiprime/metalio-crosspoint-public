#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "settings/SettingsSection.h"
#include "util/ScreenshotInfo.h"

class Activity;    // forward declaration
class RenderLock;  // forward declaration

enum class HomeMenuItem { NONE, FILE_BROWSER, LIBRARY, OPDS_BROWSER, FILE_TRANSFER, SETTINGS_MENU };

/**
 * ActivityManager
 *
 * This mirrors the same concept of Activity in Android, where an activity represents a single screen of the UI. The
 * manager is responsible for launching activities, and ensuring that only one activity is active at a time.
 *
 * It also provides a stack mechanism to allow activities to launch sub-activities and get back the results when the
 * sub-activity is done. For example, the WebServer activity can launch a WifiSelect activity to let the user choose a
 * wifi network, and get back the selected network when the user is done.
 *
 * Main differences from Android's ActivityManager:
 * - No onPause/onResume, since we don't have a concept of background activities
 * - onActivityResult is implemented via a callback instead of a separate method, for simplicity
 */
class ActivityManager {
  friend class RenderLock;

 public:
  // WHERE A BACK TAP INSIDE THE READER LANDS, and why it has to be stated at all: the reader is
  // entered with replaceActivity() (see goToReader), so the stack below it is empty and there is no
  // entry to pop. "One level back" therefore has to mean "the screen the book was opened FROM", and
  // the only moment that is knowable is the goToReader() call itself -- the activity that was
  // current when it was made. r163 hardcoded the file browser for every entry, so a book opened
  // from the Library tapped Back into the file browser at the book's folder: two levels, not one.
  enum class ReaderReturnTarget : uint8_t { FileBrowser, Library, Home };

 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  std::vector<std::unique_ptr<Activity>> stackActivities;
  std::unique_ptr<Activity> currentActivity;

  void exitActivity(const RenderLock& lock);
  // The one place a Back press is interpreted for the whole app: tap = one level, hold = Home.
  // See the definition -- it is THE reason the key works on a screen that has no handler.
  // True when it navigated (the caller must then skip the rest of this pass).
  bool handleGlobalBackKey();

  // Pending activity to be launched on next loop iteration
  std::unique_ptr<Activity> pendingActivity;
  enum class PendingAction { None, Push, Pop, Replace, ReplaceForSleep };
  PendingAction pendingAction = PendingAction::None;
  // When pendingAction was armed (millis()). Ages a queued push: see pendingPushAgeMs().
  uint32_t pendingActionSinceMs = 0;
  // Set by the render task when it skipped a paint because a navigation was already pending, so the
  // swap path knows it still owes the incoming screen one paint. Only touched under RenderLock.
  bool renderSkipPending = false;

  // Task to render and display the activity
  TaskHandle_t renderTaskHandle = nullptr;
  static void renderTaskTrampoline(void* param);
  [[noreturn]] virtual void renderTaskLoop();

  // Set by requestUpdateAndWait(); read and cleared by the render task after render completes.
  // Note: only one waiting task is supported at a time
  TaskHandle_t waitingTaskHandle = nullptr;

  // PAINT DEBT (r100.4): entering an activity MUST put a frame on the glass on the next pass,
  // with no input event. Nothing in the manager used to guarantee it -- the transition path only
  // re-requested a paint when the render task had explicitly skipped one (renderSkipPending) or
  // when the entry gate had withheld a drive -- so a screen whose own onEnter()/loop() asks for no
  // paint stayed stale until the next key or touch. That is the power-off dialog on Home: Home's
  // loop is edge-driven (it asks for a paint only while handling an event), so the dialog was
  // entered but nothing repainted it until a vol+/vol- edge arrived.
  //
  // The debt is armed by the transition and settled by the FIRST thing that can produce a paint of
  // the incoming screen: a request from the screen itself (requestUpdate/AndWait, which is what
  // most onEnter()s do -- paintRequestVisit records it), a completed paint (paintedVisit), or, if
  // neither happened, the manager requesting it at the end of the same pass. Settled by a REQUEST
  // rather than by completion on purpose: settling on completion would queue a second identical
  // drive behind the one the screen already asked for, and two whole-panel drives per transition
  // is exactly the regression r97.2 removed.
  //
  // Main-task only, like requestedUpdate, but written on the loop task and read on the render
  // task for the completion half -- hence the atomic-free single-writer discipline: only loop()
  // and requestUpdate() (both main task) ever write it.
  bool enterPaintOwed = false;
  // screenVisit whose paint was last explicitly requested (main task only).
  uint32_t paintRequestVisit = 0;
  // WATCHDOG for the transition's paint (see loop()). The entry request is only a REQUEST: the
  // render task can consume it without driving anything (a paint that finds nothing to draw yet, a
  // notify that lands on a screen still being built). The visit is re-requested on a short staged
  // ladder instead of one 1200 ms check, because 1200 ms was long enough to be outside the press
  // that opened the dialog: the operator released the key before the safety net ever fired, which
  // is what made the failure read as "it waits for the release". 0 = none armed.
  uint32_t enterPaintWatchVisit = 0;
  uint32_t enterPaintWatchAtMs = 0;
  uint8_t enterPaintWatchStep = 0;

  // Mutex to protect rendering operations from race conditions
  // Must only be used via RenderLock
  SemaphoreHandle_t renderingMutex = nullptr;

  // Whether to trigger a render after the current loop()
  // This variable must only be set by the main loop, to avoid race conditions
  std::atomic<bool> requestedUpdate{false};

  // Counts screen changes, so the Bluetooth page turner can tell one visit of a book from the next.
  // Written by the main loop under RenderLock; the render task captures it under that lock.
  uint32_t screenVisit = 0;
  // The screen visit whose frame the render task painted last.
  std::atomic<uint32_t> paintedVisit{0};
  // The global Back tap: a press that BEGAN on this screen may finish on the next one, and that
  // release must not be read as a second tap (one press = one action). The latch is stamped with the
  // screen visit, which every transition increments, so a press that outlives its screen is ignored
  // without any of the transition paths having to clear state.
  bool backPressLatched = false;
  uint32_t backPressVisit = 0;
  // The screen the current (or last) book was opened from -- see ReaderReturnTarget. Written only by
  // noteReaderOrigin() from goToReader(), read only by leaveReaderToOrigin().
  ReaderReturnTarget readerReturnTarget = ReaderReturnTarget::FileBrowser;
  // D2 (r165.3): is the book still open somewhere in this replace chain? Set when a book is opened,
  // and cleared when the reader is left for its origin (or for any screen that is not the reader's
  // own continuation -- see noteReaderSessionReplaced). The distinction the origin record needs is
  // "a DIFFERENT session" vs "this session passing through a non-reader screen", and
  // isReaderActivity() cannot answer it: KOReaderSyncActivity REPLACES the reader, so during a sync
  // the reader is neither current nor on the stack.
  bool readerSessionOpen = false;
  // r163: millis() at which THIS visit's Back press-down was observed, and millis() at which this
  // screen visit began. Both exist so a hold is only ever measured from a press this screen watched
  // go down, and so the on-screen back control's contact has to have STARTED here too. A press whose
  // start cannot be timed (it began before this screen existed, or a blocking waveform / the panel's
  // bus guard swallowed the press sample) must classify as a tap, never as a hold -- the same shape
  // as main.cpp's allowSleepAt gate on the power key, which judges the press at the moment it began.
  unsigned long backPressSeenMs = 0;
  unsigned long screenVisitStartMs = 0;

 public:
  // r163: THE hold threshold for Back, firmware-wide: tap = one level back, hold = Home. Public so the
  // reader's tap test (ReaderUtils::handleBackNavigation) and the footnote guard in EpubReaderActivity
  // can reason about the same number the manager uses, instead of the 1000/1200/1500 ms constants
  // per screen that let the reader drift into an inverted tap/long split.
  static constexpr unsigned long kLongPressBackHomeMs = 1200;

  explicit ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~ActivityManager();

  void begin();
  void loop();

  // Will replace currentActivity and drop all activities on stack
  void replaceActivity(std::unique_ptr<Activity>&& newActivity);

  // goTo... functions are convenient wrapper for replaceActivity()
  void goToFileTransfer();
  void goToJoinNetwork();  // File Transfer straight into Join Network (post heap-defrag reboot)
  // Drive mode on the esp_tinyusb/ANA_SW stack (CP_USB_MSC boards).
  void goToUsbMsc();
  // openSection jumps straight into one group (End = the section list).
  void goToSettings(SettingsSection openSection = SettingsSection::End);
  void goToFileBrowser(std::string path = {});
  void goToLibrary();
  void goToBrowser();
  void goToPlugins(bool showOpds);
  void goToReader(std::string path, bool allowFastInitialRefresh = false);
  void goToSleep(bool fromTimeout = false);
  void goToBoot();
  void goToFullScreenMessage(std::string message, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void goToCrashReport();
  void goHome(HomeMenuItem initialMenuItem = HomeMenuItem::NONE, bool cleanInitialRefresh = false);

  // Remember where the book was opened from. Called by goToReader() with the name of the activity
  // that was current at that moment. A fresh session (no reader under the call) starts from the file
  // browser; a call made while a reader is still under it -- the reader switching books, KOReaderSync
  // restarting into one -- is the same session and keeps the target the book was opened with.
  void noteReaderOrigin(const std::string& from);
  // D2: the screen that is replacing the current one -- is it part of the open book's session?
  // Called by replaceActivity() for every replacement. The reader itself (a book switch),
  // KOReaderSyncActivity (which replaces the reader to sync, then restarts into one) and
  // SleepActivity (r165.6: the wake resume re-enters the same book) continue the session;
  // anything else ends it, so a LATER book starts from the default rather than inheriting
  // this one's origin.
  void noteReaderSessionReplaced(bool incomingIsReader, const char* incomingName);
  // The reader's one-level Back tap: the screen the book was opened from, else the file browser at
  // the book's folder. ReaderUtils::handleBackNavigation delegates here so the policy is in ONE
  // place with the record that feeds it.
  void leaveReaderToOrigin(const std::string& path);
  ReaderReturnTarget readerReturn() const { return readerReturnTarget; }

  // This will move current activity to stack instead of deleting it
  void pushActivity(std::unique_ptr<Activity>&& activity);

  // Remove the currentActivity, returning the last one on stack
  // Note: if popActivity() on last activity on the stack, we will goHome()
  void popActivity();

  bool preventAutoSleep() const;
  bool requiresExclusiveStorageLoop() const;
  bool isReaderActivity() const;
  // The screen in front right now has this Activity::name, OR a push of it is
  // already queued. Used by the power-key classifier: the power-off dialog's
  // "is it open?" must be DERIVED, never a latch -- a latch that survives the
  // activity being destroyed before it entered (pushActivity/popActivity
  // discard a queued pendingActivity without onExit) leaves the power key dead
  // for the rest of the session.
  bool isActivityNamed(const char* name) const;
  // The two halves of the answer above, for a caller that has to tell "the screen is up" from
  // "the screen has been asked for". `enteredActivityNamed` is currentActivity only;
  // `pendingPushNamed` is the queued push only; `pendingPushAgeMs` is how long that push has been
  // waiting (0 when nothing is queued). The power-key classifier needs the split because its
  // bounded retry is only reachable while the dialog is NOT up: a push that is queued but never
  // entered (a competing navigation overwrote pendingAction, or the transition gate deferred it)
  // must not hold "the dialog is up" true for the rest of the session.
  bool enteredActivityNamed(const char* name) const;
  bool pendingPushNamed(const char* name) const;
  uint32_t pendingPushAgeMs() const;
  // Bluetooth page turner (lib/BlePageTurner): the book in front, whether it has painted a page, and
  // a page or chapter turn from a remote. False when no book in front took the turn.
  bool isForegroundReader() const;
  bool isRemoteReaderContext() const;
  bool remoteClick(RemoteButton button);
  // Home is in front and has painted a frame of its own (not one left from the screen before).
  bool isForegroundHomeShown() const;
  uint32_t visit() const { return screenVisit; }
  bool remoteTurn(bool forward, bool chapter);
  // A remote button mapped to the reader menu (true) or to a new clipping (false).
  bool remoteShortcut(bool readerMenu);
  bool handleForcedRefresh();
  bool skipLoopDelay() const;
  ScreenshotInfo getScreenshotInfo() const;
  // Debug channel (`activity` command): names of the live activity stack, index 0
  // first, i.e. the activity currently in front. The strings are owned by the
  // activities and stay valid while they are on the stack, which is the lifetime
  // of the call — copy them if they must outlive it. Main task only.
  int activityStackNames(const char** out, int cap) const;
  void prepareForSleep();

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  void requestUpdate(bool immediate = false);

  // Trigger a render and block until it has DRIVEN the screen in front.
  // Returns the verdict: true when a paint for this visit actually reached the
  // glass, false when the bounded wait gave up (no wake within 2 s per attempt,
  // a render that drove nothing, or a refused retry -- see the rules in the .cpp).
  // REPORTED, NOT ENFORCED: the worst case here is 6 s, so a caller that must not
  // refuse on a merely slow paint (USB detach, the power-off park) keeps its own
  // hardware check and treats this as evidence, not as a gate.
  // Must NOT be called from the render task or while holding a RenderLock.
  bool requestUpdateAndWait();
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
