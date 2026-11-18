#include "ActivityManager.h"

#include "RefreshPolicy.h"
#include "display/RefreshPolicy.h"  // cp_refresh_* ride on this TU's paint path; the activities/RefreshPolicy.h above is a different header

#include <BlePageTurner.h>
#include <BoardConfig.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <Memory.h>
#include <VectorFontSupport.h>

#include <algorithm>

#include "Activity.h"
#include "audio/UiSounds.h"  // AUDIO1: tap-Back feedback
#include "display/BatteryIconRegion.h"
#include "platform/CpuStats.h"
#include "platform/PlatformFeatures.h"
#include "platform/UsbMscFeature.h"
#include "CrossPointSettings.h"
#include "HapticFeedback.h"
#include "OpdsServerStore.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "components/HeaderBackTapTarget.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/HomeActivity.h"
#include "library/LibraryListActivity.h"
#include "network/CrossPointWebServerActivity.h"
#if CP_USB_MSC
#include "network/UsbMscActivity.h"
#endif
#include "plugins/PluginCatalogActivity.h"
#include "reader/ReaderActivity.h"
#include "reader/RemoteTurnGate.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/SettingsListActivity.h"
#include "util/BmpViewerActivity.h"
#include "util/FrontlightPanelActivity.h"
#include "util/FullScreenMessageActivity.h"

static portMUX_TYPE activityManagerSpinlock = portMUX_INITIALIZER_UNLOCKED;

ActivityManager::ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
  assert(renderingMutex != nullptr && "Failed to create rendering mutex");
  stackActivities.reserve(10);
}

ActivityManager::~ActivityManager() { assert(false); /* should never be called */ }

void ActivityManager::begin() {
  // r163: the first screen gets a start time too. screenVisitStartMs = 0 disabled the touch back
  // control's "the contact must have started on THIS screen" gate for the first visit (the check
  // reads 0 as "gate off"), so a finger already resting on the arrow at boot could hold straight
  // to Home from a screen it never touched. Taking the clock here is the same moment the first
  // screen's life begins.
  screenVisitStartMs = millis();

#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t renderTaskCore = 1;
#else
  constexpr BaseType_t renderTaskCore = 0;
#endif
#if CROSSPOINT_VECTOR_FONTS
  // FreeType rasterization runs on this task, and the deepest observed chain
  // is a glyph fault DURING LAYOUT: expat + parser + line-layout frames
  // (~3.5KB on Xtensa) with the scan converter's FT_RENDER_POOL_SIZE (4KB)
  // stack-resident band pool on top — a measured ~8KB peak that trips the
  // canary on an 8KB stack. Vector-font boards all have PSRAM-class RAM.
  constexpr uint32_t renderTaskStackBytes = 16384;
#else
  constexpr uint32_t renderTaskStackBytes = 8192;
#endif
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender",
                          renderTaskStackBytes,  // Stack size
                          this,                  // Parameters
                          1,                     // Priority
                          &renderTaskHandle,     // Task handle
                          renderTaskCore  // Keep long renders/cover decodes off CPU 0's idle watchdog when available
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    if (pendingAction != PendingAction::None) {
      // A navigation is already queued, so this screen is about to be replaced. Painting it first
      // costs one full waveform (0.2 s on DU, ~1 s on the old grayscale table) that the user never
      // sees -- and that dead paint is most of the lag between tapping a menu row and the submenu
      // appearing. Skip it; the swap below satisfies renderSkipPending with a paint of the
      // INCOMING screen, so nothing is left stale.
      renderSkipPending = true;
    } else if (currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      // Night mode is a global output polarity applied to every activity.
      // The sleep screen forces normal polarity itself (SleepActivity).
      display.setInverted(SETTINGS.screenInverted != 0);
      const uint32_t renderingVisit = screenVisit;
#if CP_CPU_STATS
      // r96 `bench render`: the TOTAL of one paint, from just after RenderLock was
      // acquired to the moment render() returned. The drive inside it is measured
      // separately by GfxRenderer::displayBuffer(), so
      //   compose = total - drive
      // with no timer inside the renderer's own loops.
      const int64_t cpRenderT0 = cpustats::nowUs();
      cpustats::beginRender();
#endif
      // CP_ICON_PARTIAL_REPAINT / CP_ICON_SAFE_PATCH: this paint invalidates the
      // recorded battery-icon rectangle. Only a paint that reaches
      // BaseTheme::drawBatteryOutline() re-arms it, which is exactly the "is the icon
      // on the screen I am about to show?" test -- a menu, a full-screen image or the
      // sleep chip leaves it unset and the icon repaint is skipped rather than drawn
      // over them.
#if CP_ICON_SAFE_PATCH
      renderer.resetBatteryIconRect();
#elif CP_ICON_PARTIAL_REPAINT
      cpBatteryIconBeginPaint();
#endif
#if CP_BATT_PERCENT_PATCH
      // BATT6: same test for the percentage NUMBER (display/BatteryPercentPatch.cpp).
      // A paint that reaches no note site (a full-screen image, the sleep chip, the
      // number hidden in Settings) leaves no box, and the change producer then costs
      // nothing on that screen.
      renderer.resetBatteryPercentRect();
#endif
      currentActivity->paintDeclinedFrame = false;
      // r101.9 WAKE3 (diagnostics only): name the screen this paint will compose,
      // so a panel drive issued inside render() can be attributed. Two stores and
      // a pointer, cleared immediately after the paint; nothing branches on it.
      cp_paint_owner_note(currentActivity->name.c_str());
      // r161 MENU AA / r162 READER TEXT AA: the glyph blitter is the ONLY producer of
      // intermediate optical levels on this panel (PixFormat.h: "these are
      // OPTICAL levels, and the ONLY source that may write them"), so the switch
      // is applied there, per paint, from the user setting:
      //   * menus/UI  -> menuAntiAliasing. OFF thresholds every glyph to ink, the
      //                  composed frame carries no grey, and the FAST paint falls
      //                  through `useGray8` to the binary DU table — the "1-bit
      //                  menus" the setting promises, chosen by the FRAME, not by
      //                  forcing a drive mode over grey pixels.
      //   * reader    -> textAntiAliasing, on its own. r162 removed the
      //                  CP_READER_AA_LANE gate that used to AND in here: that
      //                  gate made the reader toggle inert at the lane's default
      //                  and it was never needed, because the setting's job is
      //                  only to choose coverage glyphs or thresholded ones. The
      //                  refresh request is unchanged (a page turn still asks for
      //                  GL16) and nothing about the drive is gated on this.
      //                  NOTE: OFF binarises the WHOLE frame for that paint —
      //                  covers and in-book images included, not just text.
      // Setting A `false` is the AA path every earlier build had, unchanged.
      if (currentActivity->isReaderActivity()) {
        renderer.setTextBinary(SETTINGS.textAntiAliasing == 0);
      } else {
        renderer.setTextBinary(SETTINGS.menuAntiAliasing == 0);
      }
      currentActivity->render(std::move(lock));
      const uint32_t paintSeq = cp_paint_seq();
      cp_paint_owner_clear();
#if CP_CPU_STATS
      cpustats::endRender(cpustats::nowUs() - cpRenderT0);
#endif
      // r100.4: ONE line per paint, naming the screen. Without it "did the dialog ever paint?"
      // was only answerable from the panel's own drive line, which does not say who painted.
      // A transition that paints with no input in between now reads:
      //   [MAP] enter PowerOff from Home action=push
      //   [ACT] paint PowerOff visit=N held=0
      // `held=1` is the entry gate withholding the drive (a composition, not a frame on glass).
      const bool held = renderer.lastPaintAttemptHeld();
      const bool declined = currentActivity->paintDeclinedFrame;
      const bool drove = !held && !declined;
#if CP_ICON_SAFE_PATCH
      // BATT5: what this paint drew only becomes "what the glass shows" if the paint
      // actually drove. A held entry frame or a declined frame must not stamp the rect
      // with an epoch the glass will never take -- a failed drive is caught one step
      // later, because the renderer only advances the glass epoch on a landed drive.
      renderer.commitBatteryIconRect(drove);
#elif CP_ICON_PARTIAL_REPAINT
      // BATT2: what this paint drew only becomes "what the glass shows" if the paint
      // actually drove. A held entry frame or a declined frame must not consume the
      // charging edge -- that is the silent case where the icon stayed stale.
      cpBatteryIconEndPaint(drove);
#endif
#if CP_BATT_PERCENT_PATCH
      // BATT6: the same verdict for the number's box. A held or declined paint leaves
      // epoch 0, so the change producer treats the box's value as "not on the glass"
      // and waits for a paint that did drive instead of patching a frame the panel
      // never took.
      renderer.commitBatteryPercentRect(drove);
#endif
      LOG_INF("ACT", "paint %s visit=%u p=%lu held=%d drove=%d", currentActivity->name.c_str(),
              static_cast<unsigned>(renderingVisit), static_cast<unsigned long>(paintSeq), held ? 1 : 0, drove ? 1 : 0);
      if (drove) paintedVisit.store(renderingVisit, std::memory_order_release);
    }
    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&activityManagerSpinlock);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(&activityManagerSpinlock);
    if (waiter) {
      xTaskNotify(waiter, 1, eIncrement);
    }
  }
}

namespace {
// r163: kLongPressBackHomeMs now lives on ActivityManager (ActivityManager.h) so the reader's tap
// test and the footnote guard share the manager's threshold instead of their own.
// r100.5: the staged ladder the paint watchdog re-requests on, and how many attempts it makes
// before it gives up and says so. The FIRST step is deliberately shorter than any entry paint
// (150 ms): a paint that is going to happen is signalled within one loop pass, so 150 ms of silence
// already means the request produced nothing, and waiting 1200 ms let the whole press finish first.
// Later steps back off so the retries themselves can never pace the panel (a retry is only filed
// while no render is in flight).
constexpr unsigned long kEnterPaintWatchMs[] = {150, 350, 700, 1200};
constexpr uint8_t kEnterPaintWatchSteps = 4;
}  // namespace

// THE GLOBAL BACK KEY. One key, two gestures, decided here so every screen gets them by existing:
//   TAP  -> one level back (pop; Home at the bottom of the stack)
//   HOLD -> Home
// The hold has always lived here. The tap joins it, which is what makes the key global: the screens
// that already handled Back keep doing so (they declare ownsBackKey() and this function never acts on
// them, so nothing double-fires), and the screens that never handled it -- and every screen written
// later -- get the generic rule for free.
//
// Deliberately NOT a second event source. Everything below reads the same Button::Back predicates the
// activities read, on the same frame, so a screen's Back means exactly what it meant before.
//
// Returns true when it NAVIGATED. The one pass a queued transition is pending is the one pass this
// function stays out of the way; the caller must still run the transition below, so the return value
// -- not `pendingAction` -- is what the caller keys off.
bool ActivityManager::handleGlobalBackKey() {
  if (pendingAction != PendingAction::None) {
    // A navigation is already queued; this press belongs to the screen that is leaving.
    return false;
  }

  // A press and a release can land in the same frame (a fast tap, the header arrow, an edge swipe --
  // the touch folds report both edges at once). Either way the press "began here".
  const bool pressed = mappedInput.wasPressed(MappedInputManager::Button::Back);
  if (pressed) {
    backPressLatched = true;
    backPressVisit = screenVisit;
    // The press's own start, not the millis() we happened to notice it on: getHeldTime() is already
    // millis()-buttonPressStart, so this is the same instant the hold below will reconstruct. Stamping
    // "now" instead would make the gate below compare two clocks a tick apart and could reject a
    // genuine hold outright.
    backPressSeenMs = millis() - mappedInput.getHeldTime();
  }

  // One gesture back to the top: a held Back returns Home from any depth below it. Checked before the
  // activity's own handling so a settings page three levels deep costs one press, not three.
  //
  // r88: the on-screen back control is held the same way as the physical Back button. A TAP on it is
  // one level (the activity's ordinary Back path); a HOLD is the only touch gesture that reaches Home.
  // The hold needs its own press/hold timer because the touch pipeline reports only the control's
  // release -- see MappedInputManager::wasBackControlHold.
  //
  // r163 TAP vs HOLD. This branch used to be the one place in the firmware that could turn a *tap*
  // into Home from a screen that owns Back, and that is the reported defect: a single tap in
  // Settings > System landed on the main screen instead of the settings root, because SettingsActivity
  // has no onBackButton() override and so never answers Back itself -- this branch ran first and
  // navigated. The hold was measured from HalGPIO's raw press timestamp
  // (InputManager::getHeldTime -> buttonPressStart), which is only stamped when the whole button
  // state goes 0 -> nonzero (InputManager::applyStateChange). A press that began before this screen
  // existed, or whose press-down sample was swallowed (the panel's blocking drives and the expander
  // bus guard do both), therefore reported a held time measured from some EARLIER contact -- already
  // past 1200 ms on the very first pass, i.e. a tap read as a hold. Fixed in two parts, both here:
  //
  //  * `backPressTimed` is the same per-visit latch the tap already uses (beganOnThisScreen): no
  //    press edge observed on THIS visit, no hold.
  //  * `notBeforeMs` makes the timing explicit: the implied press start (millis() - held) must not
  //    predate the moment we watched it go down. Same shape as main.cpp's allowSleepAt gate on the
  //    power key, which judges a press at the moment it STARTED, not at release.
  //
  // The on-screen back control keeps its own timer (r88) but gets the same treatment through the
  // visit start: a contact that began on the previous screen cannot become a hold here.
  const bool backPressTimed = backPressLatched && backPressVisit == screenVisit;
  const bool backHeld =
      !currentActivity->isHomeActivity() &&
      ((backPressTimed && mappedInput.wasLongPressed(MappedInputManager::Button::Back, kLongPressBackHomeMs,
                                                     backPressSeenMs)) ||
       mappedInput.wasBackControlHold(kLongPressBackHomeMs, screenVisitStartMs));
  if (backHeld) {
    backPressLatched = false;  // the release is suppressed; never let it read as a tap as well
    if (currentActivity->handleLongPressBack()) return false;  // the activity owns this gesture
    LOG_DBG("ACT", "long-press Back -> Home from %s", currentActivity->name.c_str());
    goHome();
    return true;
  }

  if (!mappedInput.wasReleased(MappedInputManager::Button::Back)) return false;
  // The press began on the PREVIOUS screen (a queued transition in between): its release is not a
  // second tap, it is the tail of the one that already navigated.
  const bool beganOnThisScreen = pressed || (backPressLatched && backPressVisit == screenVisit);
  backPressLatched = false;
  // Home is the bottom of the stack: a Back tap there has nothing to go back to and stays harmless.
  // An activity that declared ownership answers this release itself, further down this pass.
  if (!beganOnThisScreen || currentActivity->isHomeActivity() || currentActivity->ownsBackKey()) return false;
  LOG_DBG("ACT", "tap-Back -> one level from %s (stack %zu)", currentActivity->name.c_str(),
          stackActivities.size());
  // AUDIO1: a user Back tap that actually navigates. One shot for the whole
  // one-level path (the activity-owned Back paths emit their own in
  // UiListActivity::handleButtons), never for a programmatic transition.
  uisounds::onEvent(uisounds::Event::Back, uisounds::Tag::Back);
  popActivity();  // empty stack: the pop path lands on Home
  return true;
}

void ActivityManager::loop() {
  if (mappedInput.consumeSuppressedRelease()) return;

  if (currentActivity && currentActivity->requiresExclusiveStorageLoop()) {
    currentActivity->loop();
    // An exclusive-storage activity must restart rather than navigate away:
    // processing a pending action here could re-enable filesystem users while
    // the USB host still owns the raw SD card.
    if (requestedUpdate.exchange(false) && renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
    return;
  }

  if (currentActivity) {
    // THE GLOBAL BACK KEY: tap = one level, hold = Home, for every screen (see handleGlobalBackKey).
    // Runs before the activity's own loop so the hold is never delivered twice. Only a pass this
    // function NAVIGATED on skips the rest; a transition that was already queued must still run.
    if (handleGlobalBackKey()) return;

    if (!currentActivity->isHomeActivity() && mappedInput.wasHomeGesture()) {
      if (currentActivity->handleHomeGesture()) {
        return;
      }
      goHome();
      return;
    }

    // Tap-first control-center entry: a tap on the status-bar band of the
    // top-level tab screens opens it, mirroring the top-edge swipe (which some
    // panels' etched glass makes unreliable). The reader keeps its clean page
    // (no status bar there to tap). Touch boards only, like the swipe itself.
    bool statusBarTap = false;
    if (mappedInput.hasTouch() && (currentActivity->name == "Home" || currentActivity->name == "FileBrowser" ||
                                   currentActivity->name == "Settings" || currentActivity->name == "SettingsList" ||
                                   currentActivity->name == "NetworkModeSelection")) {
      int tx = 0;
      int ty = 0;
      // The header back button shares this band; its taps stay Back.
      statusBarTap = mappedInput.wasScreenTapped(tx, ty) && ty < 44 && !HeaderBackTapTarget::contains(tx, ty);
    }
    if (currentActivity->name != "FrontlightPanel" && (statusBarTap || mappedInput.wasLightPanelGesture())) {
      haptic_feedback::deferTouchAction();
      pushActivity(std::make_unique<FrontlightPanelActivity>(renderer, mappedInput));
      return;
    }

    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  while (pendingAction != PendingAction::None) {
    // The page turner's radio gives its heap back before the next screen allocates. Its teardown
    // can span loop passes and runs outside the render lock.
    if (pendingAction != PendingAction::ReplaceForSleep &&
        !bleturner::beforeScreenChange(0, pendingActivity && pendingActivity->isReaderActivity())) {
      return;
    }
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      ++screenVisit;
      screenVisitStartMs = millis();  // r163: the hold gates are relative to when this screen began
      ActivityResult pendingResult = std::move(currentActivity->result);
      // r93: a reader that pops away with nothing under it lands on Home too (the load-failure and
      // clock-sync failure exits call finish()). exitActivity() destroys it, so the "was this a
      // book" fact has to be taken now, before the stack-empty branch can no longer see it.
      const bool poppedReader = currentActivity->isReaderActivity();
      // r97 MAP1: the popped screen's name for the transition line (the activity is
      // destroyed two lines below).
      const std::string poppedName = currentActivity->name;

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        if (poppedReader) renderer.forceCleanNextRefresh();
        // r97 MAP1: same one-line-per-transition rule as the enter path above.
        LOG_INF("MAP", "enter Home from %s action=pop cleanArmed=%d", poppedName.c_str(),
                renderer.forcedCleanArmed() ? 1 : 0);
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // r97 MAP1: returning to a PARENT screen. onEnter() is not re-run on this path (the
        // restored activity only repaints), so the line goes after the result handler: an arm
        // made by the handler is exactly what the arriving paint will consume.
        LOG_INF("MAP", "enter %s from %s action=pop cleanArmed=%d", currentActivity->name.c_str(),
                poppedName.c_str(), renderer.forcedCleanArmed() ? 1 : 0);

        // Request an update to ensure the popped activity gets re-rendered
        // r100.4: and arm the paint debt first, so a restored parent whose own loop() asks for no
        // paint still repaints on this pass (the request below settles it).
        // r162: ...and ask that paint for HALF. onEnter() is not re-run on this path, so a
        // UiListActivity's `firstRender` is false and its `top` is unchanged: `pageChanged` reads
        // false and the repaint would take the FAST lane, whose grey composite erases to paper first
        // (the back-navigation white flash). The forward direction (firstRender => HALF => GC16 over
        // the diff) is the behaviour to match. One shot: the paint that commits consumes it, so every
        // later in-page FAST repaint (tap flash, scroll) keeps its DU drive.
        currentActivity->armHalfPaintOwed();
        enterPaintOwed = true;
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      // r97 MAP1: names for the transition line below, taken BEFORE the outgoing activity is
      // destroyed. Logging only -- nothing here reads them back.
      const std::string mapFrom = currentActivity ? currentActivity->name : std::string("-");
      const std::string mapTo = pendingActivity->name;
      const bool mapIsPush = pendingAction == PendingAction::Push;
      // r97.2: the entry gate must NOT hold a screen whose whole purpose is the drive its onEnter()
      // issues and after which the device leaves this loop for good (the sleep image, the crash
      // screen). Those have nothing to complete and no repaint to be repaid with.
      [[maybe_unused]] const bool mapGateExempt = mapTo == "Sleep" || mapTo == "Boot" || mapTo == "Crash";

      if (pendingAction == PendingAction::Replace || pendingAction == PendingAction::ReplaceForSleep) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Push) {
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        // The parent's header back rect must not route taps on the pushed
        // screen (which may draw no header of its own).
        HeaderBackTapTarget::clear();
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      ++screenVisit;
      screenVisitStartMs = millis();  // r163: the hold gates are relative to when this screen began
      pendingAction = PendingAction::None;
      // r100.4 PAINT DEBT: entering a screen owes the glass a frame, whether or not the incoming
      // activity asks for one itself. Armed here (before onEnter, so a request from onEnter
      // settles it) and discharged at the tail of this pass. See the header comment.
      enterPaintOwed = true;
      currentActivity = std::move(pendingActivity);

#if CP_ENTRY_SINGLE_DRIVE
      // r97.2: the new activity is published above, BEFORE its onEnter() runs, and the render lock is
      // dropped here -- so the render task can wake on a notify that was already pending and paint
      // this screen while onEnter() is still building its content (list rows, the Home cover thumb).
      // That paint used to drive the panel, and the repaint that carried the real content drove it
      // again: two whole-panel drives for one transition (measured 812 ms + 807 ms entering
      // Settings). Hold the first drive until onEnter() returns, then pay the held paint back below.
      //
      // r100.4: armed BEFORE the lock is dropped. The gate used to be armed after it, which left a
      // window in which the render task could render the half-built screen AND drive it -- a
      // whole-panel flash of the intermediate state, or, for the power-off dialog, a blank page
      // (its popup is shown inside onEnter). Arming under the same lock that publishes the activity
      // closes that window by construction.
      if (!mapGateExempt) renderer.holdEntryDrive(CP_ENTRY_GATE_TIMEOUT_MS);
#endif
      lock.unlock();  // onEnter may acquire its own lock
      currentActivity->onEnter();
#if CP_ENTRY_SINGLE_DRIVE
      // A held paint must be repaid or the screen is simply late, never unpainted.
      if (renderer.releaseEntryDrive()) {
        LOG_INF("MAP", "entry gate released after %s onEnter: held paint repaid", mapTo.c_str());
        requestUpdate();
      }
#endif

      // r97 MAP1: ONE line per screen transition. The [GFX] `paint ...` lines that follow
      // belong to this transition, and `cleanArmed` says whether the arriving paint will be a
      // from-white clean (read AFTER onEnter, because a screen can arm one there). Nothing
      // downstream reads this -- it exists so the non-list transition map in
      // docs/engineering/R97_ROUND.md can be confirmed on glass instead of inferred.
      LOG_INF("MAP", "enter %s from %s action=%s cleanArmed=%d", mapTo.c_str(), mapFrom.c_str(),
              mapIsPush ? "push" : "replace", renderer.forcedCleanArmed() ? 1 : 0);

      // The render task skipped the outgoing screen's paint (see renderTaskLoop); pay it back here
      // with a deferred update for the screen that actually replaced it.
      if (renderSkipPending) {
        renderSkipPending = false;
        requestUpdate();
      }

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  // r100.5 PAINT DEBT: the transition owes the glass a frame, settled by a request from the screen
  // itself (paintRequestVisit) or by a DRIVEN paint (paintedVisit, now written only for a paint that
  // actually reached the glass). If neither happened, nothing was ever going to repaint the
  // incoming screen, so ask for it now.
  //
  // A REQUEST is not proof of a frame -- that is what r100.4 got wrong. The manager used to record
  // `paintedVisit` for any render() that returned, including one that deliberately drove nothing
  // (PowerOffActivity before its popup exists) and one whose drive the entry gate withheld. Both the
  // debt below and the backstop then concluded the dialog had been painted while the glass still
  // showed Home, and nothing ever asked again. Whichever branch is taken, the frame is now WATCHED
  // until a paint actually drives.
  if (enterPaintOwed) {
    const uint32_t owedVisit = screenVisit;
    const bool requested = paintRequestVisit == owedVisit;
    const bool painted = paintedVisit.load(std::memory_order_acquire) == owedVisit;
    enterPaintOwed = false;
    if (!requested && !painted) {
      LOG_INF("ACT", "enter paint owed: %s asked for no paint -- requesting it at the transition (visit %u)",
              currentActivity ? currentActivity->name.c_str() : "-", static_cast<unsigned>(owedVisit));
      requestedUpdate = true;
    }
    if (!painted) {
      enterPaintWatchVisit = owedVisit;
      enterPaintWatchStep = 0;
      enterPaintWatchAtMs = millis() + kEnterPaintWatchMs[0];
    }
  }
  // WATCHDOG: the screen in front has a request but no frame. This is the "the request produced no
  // paint" case -- a notify the render task consumed without driving, a render that declined, or a
  // drive that never reached the glass. It re-requests on a short staged ladder instead of one
  // 1200 ms check, because 1200 ms was already outside the press that summoned the dialog: the
  // operator let go before the net fired, which is why the failure read as "it waits for the
  // release". Every attempt is only made while no render is in flight (RenderLock::peek()), so it
  // can never put a duplicate drive behind a paint that is already composing.
  if (enterPaintWatchVisit != 0) {
    if (paintedVisit.load(std::memory_order_acquire) == enterPaintWatchVisit) {
      enterPaintWatchVisit = 0;
    } else if (static_cast<int32_t>(millis() - enterPaintWatchAtMs) >= 0) {
      const char* watched = currentActivity ? currentActivity->name.c_str() : "-";
      if (RenderLock::peek()) {
        // A paint is composing right now; wait for it rather than queueing a second drive.
        const uint8_t step = enterPaintWatchStep < kEnterPaintWatchSteps ? enterPaintWatchStep : kEnterPaintWatchSteps - 1;
        enterPaintWatchAtMs = millis() + kEnterPaintWatchMs[step];
      } else if (enterPaintWatchStep >= kEnterPaintWatchSteps) {
        LOG_ERR("ACT", "paint watchdog: %s still has no frame after %u re-requests -- giving up (visit %u)",
                watched, static_cast<unsigned>(kEnterPaintWatchSteps),
                static_cast<unsigned>(enterPaintWatchVisit));
        enterPaintWatchVisit = 0;
      } else {
        ++enterPaintWatchStep;
        if (enterPaintWatchStep == 1)
          LOG_INF("ACT", "paint watchdog: no frame for %s yet -- re-requesting (1/%u, visit %u, %lu ms)",
                  watched, static_cast<unsigned>(kEnterPaintWatchSteps),
                  static_cast<unsigned>(enterPaintWatchVisit),
                  static_cast<unsigned long>(millis() - enterPaintWatchAtMs));
        else
          LOG_ERR("ACT",
                  "paint watchdog: still no frame for %s -- re-requesting (%u/%u, visit %u, %lu ms)",
                watched, static_cast<unsigned>(enterPaintWatchStep),
                static_cast<unsigned>(kEnterPaintWatchSteps),
                static_cast<unsigned>(enterPaintWatchVisit), static_cast<unsigned long>(millis() - enterPaintWatchAtMs));
        enterPaintWatchAtMs = millis() + kEnterPaintWatchMs[enterPaintWatchStep - 1];
        requestedUpdate = true;
      }
    }
  }

  if (requestedUpdate.exchange(false)) {
    // Using direct notification to signal the render task to update
    // Increment counter so multiple rapid calls won't be lost
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  }
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
  // The outgoing screen's header back button must not eat taps on the next
  // screen; the next header draw re-records it.
  HeaderBackTapTarget::clear();
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  // Entering or leaving an activity replaces the whole screen: apply the activity-transition policy.
  refreshpolicy::apply(renderer, SETTINGS.activityTransitionRefresh);
  mappedInput.resetHomeButtonInput();
  mappedInput.discardPendingTouch();
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  // D2: a replacement is either the open book's session passing through (the reader, KOReaderSync)
  // or a different screen -- which ends the session so a later book cannot inherit its origin.
  if (newActivity) noteReaderSessionReplaced(newActivity->isReaderActivity(), newActivity->name.c_str());
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    // r100.4: the first screen after boot owes a paint too (same contract, see enterPaintOwed).
    enterPaintOwed = true;
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer() {
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput));
}

void ActivityManager::goToJoinNetwork() {
  // Post heap-defrag reboot: enter the web-server activity straight in Join
  // Network mode (skips mode selection, does not reboot again).
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput, /*startInJoinNetwork=*/true));
}

void ActivityManager::goToUsbMsc() {
#if CP_USB_MSC
  auto activity = makeUniqueNoThrow<UsbMscActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("ACT", "OOM: USB drive mode activity");
    return;
  }
  replaceActivity(std::move(activity));
#else
  LOG_ERR("ACT", "USB drive mode requested in a build without USB-MSC capability");
#endif
}

void ActivityManager::goToSettings(const SettingsSection openSection) {
  replaceActivity(std::make_unique<SettingsListActivity>(renderer, mappedInput, openSection));
}

void ActivityManager::goToFileBrowser(std::string path) {
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToLibrary() {
  auto activity = makeUniqueNoThrow<LibraryListActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("ACT", "OOM: library activity");
    return;
  }
  replaceActivity(std::move(activity));
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0]));
  } else {
    replaceActivity(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}

void ActivityManager::goToPlugins(bool showOpds) {
  replaceActivity(std::make_unique<PluginCatalogActivity>(renderer, mappedInput, showOpds, /*rootMode=*/true));
}

// THE READER'S ONE LEVEL, recorded at the only moment it is knowable.
//
// goToReader() replaces the whole screen (the reader is never pushed), so the activity that is
// current when it is called IS the screen the book is being opened from. Only the three roots are
// remembered: a book opened from the Library returns to the Library, one opened from Home's recents
// returns to Home, one opened from the file browser returns to the browser at the book's folder.
//
// Everything else keeps the previous answer ON PURPOSE. The reader is also re-entered from screens
// that sit ABOVE it -- KOReaderSyncActivity restarts into a book after syncing, EpubReaderActivity
// switches books from its own menu -- and those are not a level change: the book the user is
// looking at is still the one opened from the Library/Home/browser, so Back must still land there.
// A boot-time resume (main.cpp) either has no current activity (keep the default, file browser) or
// is standing on Home.
//
// D2 (r165.3): does the screen replacing the current one still belong to the open book's session?
//
// THE DEFECT. noteReaderOrigin() used to ask `!isReaderActivity()` to tell "a fresh session" from
// "the same session passing through a non-reader screen". KOReaderSyncActivity REPLACES the reader
// (EpubReaderActivity hands the whole screen to it before syncing, and the sync screen restarts
// into a book), so at that moment there is no reader in front AND none on the stack: the answer was
// "fresh", the Library target was wiped, and Back from the synced book landed in the file browser --
// two levels out from Library.
//
// THE RULE. A session opens when a book is opened and closes when the reader is left, either for its
// origin (leaveReaderToOrigin) or for any screen that is not the reader's own continuation. The
// reader's continuation is exactly three things: the reader itself (a book switch from its menu),
// KOReaderSyncActivity (the sync hand-off, which returns to a reader) and SleepActivity.
//
// r165.6: sleep is not leaving the book. goToSleep() REPLACES the reader with SleepActivity and the
// wake resume re-enters the same book (main.cpp's standby resume calls goToReader(openEpubPath)),
// so before this term a Library -> book -> sleep -> wake -> Back walked out to the file browser:
// sleep closed the session, and the resume's noteReaderOrigin("Sleep") then matched no root with
// the session already closed, which resets the target to the default. Every other replacement --
// Home, the library, settings -- still ends the session, so the NEXT book starts from the default
// and can never inherit this one's origin.
void ActivityManager::noteReaderSessionReplaced(const bool incomingIsReader, const char* incomingName) {
  if (!readerSessionOpen) return;
  if (incomingIsReader) return;
  if (incomingName == nullptr) {
    readerSessionOpen = false;
    return;
  }
  const std::string incoming(incomingName);
  if (incoming == "KOReaderSync" || incoming == "Sleep") return;
  readerSessionOpen = false;
}

void ActivityManager::noteReaderOrigin(const std::string& from) {
  // A fresh reading session -- no book is open anywhere in this chain -- starts from the default,
  // so an origin recorded for an earlier book can never leak into this one (an OPDS/plugin Details
  // screen opens a book with a replace chain that has no root name to offer). A session that is
  // still open (the reader switching books from its own menu, KOReaderSync restarting into a book
  // after a sync) is the SAME book: keep the target the book was opened with.
  if (!readerSessionOpen) readerReturnTarget = ReaderReturnTarget::FileBrowser;
  readerSessionOpen = true;

  if (from == "Library") {
    readerReturnTarget = ReaderReturnTarget::Library;
  } else if (from == "Home") {
    readerReturnTarget = ReaderReturnTarget::Home;
  } else if (from == "FileBrowser") {
    readerReturnTarget = ReaderReturnTarget::FileBrowser;
  }
}

void ActivityManager::leaveReaderToOrigin(const std::string& path) {
  // The reader is being left for the screen the book was opened from: that ENDS the session, so a
  // book opened later from somewhere with no root name starts from the default.
  readerSessionOpen = false;
  switch (readerReturnTarget) {
    case ReaderReturnTarget::Library:
      goToLibrary();
      return;
    case ReaderReturnTarget::Home:
      goHome();
      return;
    case ReaderReturnTarget::FileBrowser:
      break;
  }
  goToFileBrowser(path);
}

void ActivityManager::goToReader(std::string path, const bool allowFastInitialRefresh) {
  if (path.empty()) {
    goToFileBrowser("/");
    return;
  }

  if (FsHelpers::hasImageExtension(path)) {
    auto activity = makeUniqueNoThrow<BmpViewerActivity>(renderer, mappedInput, std::move(path));
    if (!activity) {
      LOG_ERR("ACT", "OOM: bitmap viewer activity");
      return;
    }
    replaceActivity(std::move(activity));
    return;
  }

  auto activity = ReaderActivity::create(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  if (activity) {
    noteReaderOrigin(currentActivity ? currentActivity->name : std::string());
    replaceActivity(std::move(activity));
  }
}

void ActivityManager::goToSleep(bool fromTimeout) {
  replaceActivity(std::make_unique<SleepActivity>(renderer, mappedInput, fromTimeout));
  if (pendingAction == PendingAction::Replace) pendingAction = PendingAction::ReplaceForSleep;
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::goToBoot() { replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goHome(HomeMenuItem initialMenuItem, bool cleanInitialRefresh) {
  // r93: Home reached FROM THE READER is repainted over a frame the book left residue on -- its
  // anti-aliased greys plus a page turn's GL16 drives -- so the arriving paint is forced to a
  // clean from-white GC16 (one drive, ~815 ms), then Home returns to its normal policy.
  //
  // "Came from the reader" is the ACTIVITY BACK-STACK, not a flag the reader sets on its way out:
  // the reader is current when Back is pressed on its first page or Home is chosen from its menu,
  // but the menu, a footnote, the dictionary and clip selection all sit ABOVE it on the stack and
  // none of them is a reader activity, so only the stack answers "is there a book under here".
  // The reader is never a stack parent of Home in any other sense (goToReader replaces, it does
  // not push), and paths that leave the reader without returning to Home -- sleep, power off, USB
  // drive mode -- go through replaceActivity() and never reach this function, so the flag cannot
  // leak into an unrelated paint. Boot and the sleep-wake resume call goHome() with an empty
  // stack, so they are excluded by the same test.
  // r96 D3 (operator: "lets expand this to all actions that go back to the home").
  //
  // THE PREDICATE, in one place: arm the one-shot clean when Home is being entered
  // FROM another activity — any activity, not only the reader. `currentActivity`
  // is the activity that is on the glass right now, because goHome() is called
  // before replaceActivity() installs Home.
  //
  // The three exclusions are the cases the operator named as must-NOT-fire, and
  // each is excluded by the activity that is current at that moment rather than by
  // an extra flag:
  //   * Boot       — cold boot / silent restart lands on Home from BootActivity;
  //   * Sleep      — the sleep-wake resume calls goHome() while SleepActivity is
  //                  current (metalioStandbySleep -> goHome), and that paint is
  //                  already the promoted FULL (main.cpp:1407-1417, r96.1: plus the
  //                  one-shot clean term). r96 justified this exclusion with "a
  //                  promoted FULL + wake prime" — the prime does not exist in the
  //                  build (CP_RICKY_WAKE_PRIME is 0, RefreshPolicy.h:206), and the
  //                  wake turns out to be a single drive. The exclusion is still
  //                  right, for the reason that survives: the wake paint IS the
  //                  from-white clean, so arming a second one here would double it;
  //   * Home       — an in-place repaint / re-entry of Home itself (Back on the
  //                  bottom of the stack, a result handler) is not a transition.
  // currentActivity == nullptr (the reader popped away and left an empty stack)
  // cannot be tested here, which is why the poppedReader arm inside the Pop branch
  // above is kept: it takes the fact BEFORE exitActivity() destroys the activity.
  //
  // WHAT THIS ADDS vs r93: Settings, SettingsList, FileBrowser, Library,
  // OpdsBookBrowser, PluginCatalog, CrossPointWebServer, UsbDrive/UsbMsc,
  // dictionary, plug-ins and every other screen that returns to Home. Those used
  // to arrive on whatever mode the policy picked over the previous screen's
  // residue; they now arrive on one from-white GC16 (~815 ms) and nothing else.
  if (currentActivity != nullptr) {
    const std::string& from = currentActivity->name;
    if (from != "Home" && from != "Sleep" && from != "Boot") {
      renderer.forceCleanNextRefresh();
    }
  }
  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    if (activityName == "FileBrowser") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "Library") {
      initialMenuItem = HomeMenuItem::LIBRARY;
    } else if (activityName == "OpdsBookBrowser" || activityName == "PluginCatalog") {
      initialMenuItem = HomeMenuItem::OPDS_BROWSER;
    } else if (activityName == "CrossPointWebServer") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    } else if (activityName == "Settings" || activityName == "SettingsList") {
      initialMenuItem = HomeMenuItem::SETTINGS_MENU;
    }
  }
  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem, cleanInitialRefresh));
}
void ActivityManager::goToCrashReport() { replaceActivity(std::make_unique<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  // Entering or leaving an activity replaces the whole screen: apply the activity-transition policy.
  refreshpolicy::apply(renderer, SETTINGS.activityTransitionRefresh);
  mappedInput.resetHomeButtonInput();
  mappedInput.discardPendingTouch();
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
  pendingActionSinceMs = millis();
}

void ActivityManager::popActivity() {
  // Entering or leaving an activity replaces the whole screen: apply the activity-transition policy.
  refreshpolicy::apply(renderer, SETTINGS.activityTransitionRefresh);
  mappedInput.resetHomeButtonInput();
  mappedInput.discardPendingTouch();
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::requiresExclusiveStorageLoop() const {
  return currentActivity && currentActivity->requiresExclusiveStorageLoop();
}

bool ActivityManager::isReaderActivity() const {
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->isReaderActivity(); }) ||
         (currentActivity && currentActivity->isReaderActivity());
}

bool ActivityManager::isForegroundReader() const {
  return pendingAction == PendingAction::None && currentActivity && currentActivity->isReaderActivity();
}

// The power-key classifier's "is the power-off dialog up?" query. Derived from
// what the manager has, never a flag the caller sets: the queued push
// (pendingActivity) counts as "opening", the entered activity counts as
// "open", and a push that was discarded before entering counts as NEITHER --
// which is the whole point (see the header comment).
bool ActivityManager::isActivityNamed(const char* name) const {
  if (!name) return false;
  if (currentActivity && currentActivity->name == name) return true;
  if (pendingAction == PendingAction::Push && pendingActivity && pendingActivity->name == name) return true;
  return false;
}

bool ActivityManager::enteredActivityNamed(const char* name) const {
  return name && currentActivity && currentActivity->name == name;
}

bool ActivityManager::pendingPushNamed(const char* name) const {
  return name && pendingAction == PendingAction::Push && pendingActivity && pendingActivity->name == name;
}

uint32_t ActivityManager::pendingPushAgeMs() const {
  if (pendingAction != PendingAction::Push) return 0;
  return static_cast<uint32_t>(millis() - pendingActionSinceMs);
}

bool ActivityManager::isRemoteReaderContext() const {
  return pendingAction == PendingAction::None && currentActivity && currentActivity->acceptsRemoteInput();
}

bool ActivityManager::remoteClick(const RemoteButton button) {
  if (!isRemoteReaderContext() || RenderLock::peek() || requestedUpdate.load()) return false;
  return mappedInput.injectRemoteClick(button);
}

bool ActivityManager::isForegroundHomeShown() const {
  return pendingAction == PendingAction::None && currentActivity && currentActivity->name == "Home" &&
         paintedVisit.load(std::memory_order_acquire) == screenVisit;
}

bool ActivityManager::remoteShortcut(const bool readerMenu) {
  if (!isForegroundReader()) return false;
  auto* reader = static_cast<ReaderActivity*>(currentActivity.get());
  if (!remoteTurnAccepted(reader->pageRendered.load(std::memory_order_acquire), RenderLock::peek(),
                          reader->inputOverPage())) {
    return false;
  }
  return reader->remoteShortcut(readerMenu);
}

bool ActivityManager::remoteTurn(const bool forward, const bool chapter) {
  if (!isRemoteReaderContext() || mappedInput.hasRemoteClick()) return false;
  auto* reader = isForegroundReader() ? static_cast<ReaderActivity*>(currentActivity.get()) : nullptr;
  if (!reader || reader->inputOverPage()) {
    return !chapter && remoteClick(forward ? RemoteButton::Next : RemoteButton::Previous);
  }
  if (!remoteTurnAccepted(reader->pageRendered.load(std::memory_order_acquire), RenderLock::peek(),
                          reader->inputOverPage())) {
    return false;
  }
  const bool turned = chapter ? reader->skipPages(forward ? 1 : -1) : reader->pageTurn(forward);
  if (turned) requestUpdate();
  return turned;
}

bool ActivityManager::handleForcedRefresh() { return currentActivity && currentActivity->handleForcedRefresh(); }

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    return currentActivity->getScreenshotInfo();
  }
  return {};
}

// Debug channel: the live stack, front first. `name` is protected on Activity with
// ActivityManager as the friend, which is why this lives here and not in a helper.
int ActivityManager::activityStackNames(const char** out, int cap) const {
  if (out == nullptr || cap <= 0) return 0;
  int n = 0;
  if (currentActivity) out[n++] = currentActivity->name.c_str();
  for (auto it = stackActivities.rbegin(); it != stackActivities.rend() && n < cap; ++it) {
    if (*it) out[n++] = (*it)->name.c_str();
  }
  return n;
}

void ActivityManager::prepareForSleep() {
  RenderLock lock;
  for (const auto& activity : stackActivities) activity->prepareForSleep();
  if (currentActivity) currentActivity->prepareForSleep();
}

void ActivityManager::requestUpdate(bool immediate) {
  // r100.4: a request for the screen that is in front settles that screen's entry paint debt (see
  // enterPaintOwed). A request made by the OUTGOING activity -- same pass, before the transition
  // bumps screenVisit -- does not: it is stamped with the visit it was made for.
  paintRequestVisit = screenVisit;
  enterPaintOwed = false;
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
  }
}
bool ActivityManager::requestUpdateAndWait() {
  // A-P1-6: the caller gets a verdict, not just a return. `driven` below is the
  // ONLY success predicate (a paint for THIS visit actually reached the glass);
  // every other exit is false. Nothing here decides what a caller does with it:
  // USB detach and the power-off park still run on their own hardware check,
  // because refusing them when a paint is merely slow (6 s worst case) is a
  // separate decision this item deliberately does not make.
  if (!renderTaskHandle) {
    return false;
  }

  // Same debt settlement as requestUpdate(): this call paints the screen in front.
  paintRequestVisit = screenVisit;
  enterPaintOwed = false;

  // Atomic section to perform checks
  taskENTER_CRITICAL(&activityManagerSpinlock);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(&activityManagerSpinlock);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");

  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");

  // Clear a deferred requestUpdate() left pending earlier in this pass. The notify
  // below already paints everything set so far, and the main loop would otherwise
  // drain that flag right after this returns and paint the SAME frame a second
  // time. On a caller that arms a whole-panel promotion per frame --
  // WifiSelectionActivity::attemptConnection() re-arms FULL for CONNECTING -- that
  // duplicate is a second full GC16 flash per connect attempt.
  //
  // AUDIT (every caller of requestUpdateAndWait, ~35 sites across activities plus
  // main.cpp:2523): each one sets the state it wants shown BEFORE the call, so the
  // notify's frame paints it; none reads or depends on a later duplicate frame,
  // and a caller that changes state after the blocking wait asks for its own paint
  // (requestUpdate/AndWait again). The stray frame could not be relied on anyway:
  // it races the end of the block. main.cpp's setup() wait is followed only by the
  // button-settle debounce, so clearing the flag merely removes a duplicate boot
  // frame of the same Home/Reader screen.
  requestedUpdate.store(false);
  // r101.10 PWR3: A RENDER THAT DROVE NOTHING IS NOT AN ANSWER. The waiter used to be woken after
  // ANY render, including one whose drive the entry gate withheld or one that declined its frame
  // (PowerOffActivity before its popup exists) -- which is exactly the silent case that let the
  // power-off path park a panel showing the screen before the dialog. The rule the paint-debt code
  // states (see the enterPaintOwed block below) applies to the BLOCKING wait too: only a paint that
  // actually reached the glass for the screen in front settles it. Bounded, so a screen that cannot
  // drive can never hang the caller: after the last attempt the wait returns and the caller's own
  // hardware check (metalioPowerOffFrameDriven) is what decides.
  //
  // r101.11 NTP1: FOUR THINGS r101.10 GOT WRONG HERE, and they froze the NTP sync screen.
  //
  //  (1) THE HANDSHAKE IS A ONE-SHOT RENDEZVOUS, NOT A LEVEL. renderTaskLoop() takes
  //      waitingTaskHandle and CLEARS it before notifying (see the waiter block at the end of the
  //      paint). r101.10's retry loop only re-poked renderTaskHandle: attempt 2 blocked in
  //      ulTaskNotifyTake() on a registration the first wake had already consumed, so nobody could
  //      ever notify it again. The log stopped after "(1/3)" and never printed "(2/3)". The handle
  //      is now RE-REGISTERED, under the spinlock, at the top of EVERY attempt.
  //  (2) EVERY WAIT IS BOUNDED. pdMS_TO_TICKS(2000): a legit HALF/GC16 from white is 1169 ms
  //      measured on this panel, so 2 s clears the longest real drive with margin while a render
  //      task that never answers costs a logged timeout instead of the whole UI. Worst case on a
  //      wedged render task is 3 x 2 s = 6 s, then the caller's own hardware check decides.
  //  (3) A STALE COUNT CANNOT PASS FOR AN ANSWER. Notifies are eIncrement, so a wake delivered
  //      before the waiter armed would make the take below return immediately; the count is
  //      therefore drained before arming, AND success is only ever declared by the driven
  //      predicate (paintedVisit == screenVisit && a new drive sequence), never by "the take
  //      returned".
  //  (4) THE REGISTRATION IS DROPPED ON EVERY EXIT PATH, including timeout and give-up. A handle
  //      left set would let the NEXT call take an old wake and treat a paint it never received as
  //      done -- the one-frame-behind class PWR3 exists to close.
  constexpr uint8_t kPaintWaitAttempts = 3;
  constexpr uint32_t kPaintWaitMs = 2000;
  constexpr TickType_t kPaintWaitTicks = pdMS_TO_TICKS(kPaintWaitMs);
  const uint32_t seqBefore = cp_paint_seq();
  bool driven = false;
  uint8_t attempt = 0;
  for (attempt = 1; attempt <= kPaintWaitAttempts; ++attempt) {
    // (3) drain anything a previous render left in this task's notification count.
    ulTaskNotifyTake(pdTRUE, 0);
    // (1) the registration the previous wake consumed, re-armed before the poke -- and, A-P2-8, the
    // guards the entry check applied re-checked before it is re-armed: registering is the same
    // claim they authorised. With NDEBUG they vanish from the entry path, and a caller that has
    // lost the right to wait would otherwise block here for 2 s per attempt instead of asserting.
    // The check accepts OUR OWN stale registration (a timed-out attempt leaves it behind), so a
    // legitimate retry is never refused early. On a violation: give up undriven and let the
    // caller's own hardware check decide, rather than block.
    bool refused = false;
    taskENTER_CRITICAL(&activityManagerSpinlock);
    auto mutexHolderNow = xSemaphoreGetMutexHolder(renderingMutex);
    const bool otherWaiter = waitingTaskHandle != nullptr && waitingTaskHandle != currTaskHandler;
    const bool renderCaller = currTaskHandler == renderTaskHandle;
    const bool holdsLockNow = mutexHolderNow == currTaskHandler;
    if (!otherWaiter && !renderCaller && !holdsLockNow) {
      waitingTaskHandle = currTaskHandler;
    } else {
      refused = true;
    }
    taskEXIT_CRITICAL(&activityManagerSpinlock);
    if (refused) {
      LOG_ERR("ACT", "requestUpdateAndWait: retry %u refused (other waiter=%d, render task=%d, holds lock=%d)",
              static_cast<unsigned>(attempt), otherWaiter ? 1 : 0, renderCaller ? 1 : 0, holdsLockNow ? 1 : 0);
      break;
    }
    xTaskNotify(renderTaskHandle, 1, eIncrement);
    // (2) bounded: a drive that has not landed in 2 s is not going to land.
    if (ulTaskNotifyTake(pdTRUE, kPaintWaitTicks) == 0) {
      LOG_ERR("ACT", "requestUpdateAndWait: no wake within %u ms for visit=%u (attempt %u/%u, p=%lu)",
              static_cast<unsigned>(kPaintWaitMs), static_cast<unsigned>(screenVisit),
              static_cast<unsigned>(attempt), static_cast<unsigned>(kPaintWaitAttempts),
              static_cast<unsigned long>(cp_paint_seq()));
      continue;
    }
    driven = paintedVisit.load(std::memory_order_acquire) == screenVisit && cp_paint_seq() != seqBefore;
    if (driven) break;
    LOG_INF("ACT", "requestUpdateAndWait: render drove nothing (held/declined); re-requesting paint (%u/%u)",
            static_cast<unsigned>(attempt), static_cast<unsigned>(kPaintWaitAttempts));
  }
  // (4) one exit path: whatever happened above, this task is no longer the waiter.
  taskENTER_CRITICAL(&activityManagerSpinlock);
  if (waitingTaskHandle == currTaskHandler) waitingTaskHandle = nullptr;
  taskEXIT_CRITICAL(&activityManagerSpinlock);
  if (!driven) {
    LOG_ERR("ACT", "requestUpdateAndWait: no driven paint for visit=%u after %u attempts (p=%lu)",
            static_cast<unsigned>(screenVisit), static_cast<unsigned>(kPaintWaitAttempts),
            static_cast<unsigned long>(cp_paint_seq()));
  }
  return driven;
}

// RenderLock

RenderLock::RenderLock(Mode mode) {
  isLocked = xSemaphoreTake(activityManager.renderingMutex, mode == Mode::Try ? 0 : portMAX_DELAY) == pdTRUE;
  assert((mode == Mode::Try || isLocked) && "Blocking render lock acquisition failed");
}

RenderLock::RenderLock(Activity&) : RenderLock(Mode::Blocking) {}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };
