#include "UsbMscActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <SilentRestart.h>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <driver/usb_serial_jtag.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UiDensity.h"
#include "components/UITheme.h"
#include "debug/UsbShell.h"
#include "fontIds.h"
#include "platform/UsbMscFeature.h"
#include "platform/UsbMscGadget.h"
#include "platform/UsbMscJournal.h"
#include "platform/UsbMscStorage.h"
#include "system/PowerNeed.h"  // r163 E6: UsbVirtualDisk, a hard need

namespace fui = freeink::ui;

namespace {

// The two lines that make the mode unambiguous: what it is, and how to leave it.
// Deliberately plain English literals -- the strings are hardware-specific
// instructions ("eject on your computer"), not UI vocabulary shared with the
// translated catalogue.
constexpr const char* kTitle = "USB drive mode";
constexpr const char* kExitHintWait = "Plug into a computer. Tap EXIT to leave drive mode.";
constexpr const char* kExitHintConnected = "The card is open on your computer.";
// r99.3: the ONLY frame the operator can ever get out of drive mode with, so its
// copy must be true before AND after the host enumerates -- there is no repaint
// left once the volume is detached (see startSession()). It therefore states the
// mode, not a host-connection event we would have to redraw to report.
constexpr const char* kExitHintActive =
    "Drive mode active. The card is on your computer. Tap EXIT DRIVE MODE when finished.";
constexpr const char* kExitButton = "EXIT DRIVE MODE";

// r99.4: NO NEUTRAL "OFFER TO START" STATE. Entry arms (see onEnter/startSession), so
// the only pre-session screen this activity can paint is the refusal, and the only
// control it can publish anywhere is EXIT.
constexpr const char* kIdleMessage = "USB DRIVE MODE";
constexpr const char* kIdleDetail = "Checking for a computer on the cable.";

// HOST PRECHECK: the refusal message. Drive mode moves the connector's pads from
// the USB-Serial/JTAG console to the OTG controller; with nothing on the other end
// nothing enumerates and the console is stranded. So the check runs before
// anything is taken away, and this is what the operator is told to do about it.
constexpr const char* kNoHostMessage = "NO COMPUTER ON THE CABLE";
constexpr const char* kNoHostDetail =
    "Plug this device into a computer with a USB data cable. Drive mode starts as soon as one is "
    "detected; EXIT leaves without starting.";

// The one rule that cannot be discovered from the panel and cannot be undone
// afterwards: yanking the cable mid-write leaves the volume with whatever the
// host had half-flushed. It is painted in EVERY phase, in its own outlined
// band above the status, so it reads as an instruction and not as the current
// state (the eject instruction itself lives here; the status lines no longer
// repeat it).
constexpr const char* kUnplugWarning =
    "Do NOT unplug the USB cable. Eject the drive on the computer first, then tap EXIT.";

// DRIVEUI1: the one fact the operator needs at a glance, and the reason it is a
// stamp rather than a sentence. Nothing is exported before the START tap and
// while a session is live the card belongs to the host, so the cable is either
// free to pull or it is not. BLACK FILL = free. OUTLINED = in use (conservative:
// an armed or failed session may still have the gadget up).
constexpr const char* kSafeToUnplug = "SAFE TO UNPLUG";
constexpr const char* kCardInUse = "CARD IN USE - DO NOT UNPLUG";

// DLG1 (r101.9): the EXIT frame. Plain literal for the same reason as the rest of this
// screen's copy (kTitle..kUnplugWarning above): it is an instruction about this hardware,
// and it is drawn by a path that may hold no translation asset at all.
//
// WHAT IT CLAIMS, AND WHY IT IS TRUE AT THE MOMENT IT IS PAINTED: exitAndReboot() has
// already stopped the gadget, torn the MSC bridge down and un-armed the gate, so no
// computer owns the card any more. The SdFat volume is NOT remounted on this path (see
// UsbMscStorage.h), so "the card is back on the glass" would be a promise the reboot
// keeps, not one this frame can verify -- hence "restarting", which is the part that is
// already happening.
constexpr const char* kExitFrame = "Card returned - restarting";

char* journalStamp(char* buf, size_t len, const char* what) {
  snprintf(buf, len, "[%lu] [INF] [USBMSC] %s", static_cast<unsigned long>(millis()), what);
  return buf;
}

// The fallback runs on the esp_timer task, so it does not care what the UI
// thread is blocked on: once the gadget is stopped the card is back in our
// hands and there is no state left worth preserving here.
void exitWatchdogFired(void*) {
  LOG_ERR("USB", "USB drive mode: exit watchdog fired, forcing restart");
  esp_restart();
}

// DIAG1: the stall tick. A sector op that never returns inside
// SdmmcBlockDevice::readSectors leaves the bridge with nothing to log -- the
// call simply does not come back -- so the age of the in-flight timestamp has
// to be watched from OUTSIDE the bridge. esp_timer's task is the only one
// guaranteed to keep running while the TinyUSB task is stuck, which is why this
// rides here and not on loop(). Runs only between startStallTick() and
// stopStallTick(), and usbmsc::diagTick() itself refuses outside a live session.
esp_timer_handle_t s_stallTick = nullptr;

void stallTickFired(void*) { usbmsc::diagTick(); }

void startStallTick() {
  if (s_stallTick == nullptr) {
    esp_timer_create_args_t args{};
    args.callback = &stallTickFired;
    args.name = "usbmscstall";
    if (esp_timer_create(&args, &s_stallTick) != ESP_OK) {
      LOG_ERR("USB", "USB drive mode: could not create stall tick");
      s_stallTick = nullptr;
      return;
    }
  }
  esp_timer_stop(s_stallTick);
  if (esp_timer_start_periodic(s_stallTick, 1000000ULL) != ESP_OK) {
    LOG_ERR("USB", "USB drive mode: could not arm stall tick");
  }
}

void stopStallTick() {
  if (s_stallTick != nullptr) esp_timer_stop(s_stallTick);
}

}  // namespace

void UsbMscActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.on(ACTION_EXIT, &UsbMscActivity::exitTrampoline, this);
  app.setScreen(&UsbMscActivity::driveScreen, this);

  phase = Phase::Idle;
  statusSince = millis();
  exitRequested = false;
  exitRequestedAt = 0;
  sessionTornDown = false;
  sessionStarted = false;
  paintBlocked = false;
  noHostPollAt = 0;

  // r99.4: TAKE THE ENTRY GATE OUT OF THE WAY FIRST.
  // CP_ENTRY_SINGLE_DRIVE arms the gate around onEnter() and releases it after it
  // returns -- i.e. AFTER startSession() has already detached the volume. A paint held
  // by the gate is repaid later, and a repaid paint composes from a cards-less
  // filesystem: exactly the "the frame is the last thing the operator has" hazard this
  // screen cannot afford. Releasing it here means startSession()'s paint really drives,
  // and requestUpdateAndWait() blocks until it has, while the card is still readable.
  renderer.releaseEntryDrive();

  // ENTRY IS r61's: precheck, then arm. There is no "offer to start" state and no
  // control whose meaning depends on a frame arriving (see the header).
  startSession();
}

// r99.4: called from onEnter() and from the refused screen's cable poll in loop().
// Order is r61's, with the r99.2 host precheck in front of everything: nothing is
// armed, detached or routed until a computer is known to be on the cable.
void UsbMscActivity::startSession() {
  if (sessionStarted) return;  // double-arm guard: the trap is one-way

  // HOST PRECHECK, AND IT COMES FIRST. Nothing below this block may run before it:
  // no shell deactivation, no arm, no detach, no MSC prepare, no gadget start, no
  // port route. `usb_serial_jtag_is_connected()` reports SOF packets from a real
  // host, so a charger or a bare cable reads as absent -- exactly the case where
  // arming would move the pads off the console with nothing to enumerate, which is
  // the shape of the reported freeze. A PC whose serial port is not open still
  // counts as connected, so this does not refuse a legitimate transfer.
  //
  // NOTE the ordering: `sessionStarted` is still false here, which is why the refused
  // screen is not a session -- it keeps the card, the shell and the HOST-mounted
  // filesystem, and it is the one state whose exit is an ordinary activity exit.
  if (!usb_serial_jtag_is_connected()) {
    LOG_INF("USB", "USB drive mode: no host on the cable -- not arming; watching the cable");
    phase = Phase::NoHost;
    statusSince = millis();
    noHostPollAt = millis();
    requestUpdateAndWait();  // real paint: the card is still mounted
    return;
  }

  sessionStarted = true;
  // r99.5 DRIVE5 -- THE HOLD COMES FIRST, BEFORE ANYTHING IS TOUCHED.
  //
  // r99.4 panicked 1 ms after this function's usbmgadget::start() when the session was armed by
  // the cable appearing: the main loop had already read preventAutoSleep()/
  // requiresExclusiveStorageLoop() (both `sessionStarted`) as false for that iteration, so its
  // idle branch applied setPowerSaving(true) -- a CPU/PLL drop -- straight after the gadget was
  // live. A predicate cannot protect the transition it describes, so from here the session holds
  // a full-speed need in HalPowerManager: setPowerSaving(true) is refused from this statement
  // until release(), whichever task and whichever stale decision issues it. Acquire restores
  // normal frequency immediately, i.e. BEFORE the pads move and before the PHY is started.
  powerHold_.acquire("usb drive mode");
  // r163 E6: and the power need, at the same instant and for the same reason -- from here
  // an auto-idle standby would park the panel and unmount nothing but stop the session.
  powerNeed_.acquire(millis());

  // r91 R6: drive mode takes the USB peripheral and hands the card to the host, so the
  // debug shell must not survive it -- deactivated HERE, before the gadget starts and
  // while the card is still writable (also the last point at which the toggle can be
  // persisted as OFF). Nothing re-enables it on exit: that is the operator's decision.
  usbshell::deactivateAndPersist("usb drive mode");

  // THE FRAME. phase/sessionStarted are advanced to their running values BEFORE the
  // paint because buildDriveScreen() reads them: the control this frame registers is
  // the EXIT one (kExitButton / ACTION_EXIT), published while the fonts and assets are
  // still readable off the card. From here on the operator's only affordance is EXIT,
  // and it is the same affordance no matter which frame is on the glass.
  phase = Phase::WaitingForHost;
  statusSince = millis();

  // Paint the instructions BEFORE anything is taken away. Once the volume is
  // detached this frame is all the user has to act on, and a re-render could not
  // read a single asset off the card.
  //
  // A-P1-6: this is the call site that most needs the verdict, because the card
  // is handed to the host a few statements later and a frame that never drove
  // leaves the operator on the PREVIOUS screen with no instructions and no EXIT.
  // It is recorded, not enforced: parking the session on a merely slow paint
  // (6 s worst case) would be its own defect.
  if (!requestUpdateAndWait()) {
    LOG_ERR("USB",
            "USB drive mode: the instructions frame did NOT drive before the card is handed over -- "
            "the panel may still show the previous screen");
  }

  // 1. Journal FIRST, and refuse to start without it. This is the last moment a
  //    write to /.crosspoint/debug.log is possible: the next call ends the
  //    filesystem. A session that cannot record itself is not worth having --
  //    it would be unverifiable exactly when it matters.
  char line[192];
  if (!usbmscjournal::writeLine(journalStamp(line, sizeof(line),
                                            "enter: SD card handed to the USB host (drive mode)"))) {
    fail("no SD card to journal to", ESP_ERR_INVALID_STATE);
    return;
  }

  // 2. Arm the single runtime gate. Every hardware entry point (port route, MSC
  //    prepare, gadget start) refuses while it is false, and it is disarmed
  //    again on the way out, so a user who never opens this screen cannot leave
  //    the console route no matter what else calls in.
  usbmsc::setArmed(true);

  // 3. Detach the filesystem and bind the 512-byte sector device to MSC.
  FsBlockDeviceInterface* const dev = Storage.detachForUsbHost();
  if (dev == nullptr) {
    // Still mounted, so this frame CAN be painted: say so on the glass.
    fail("could not detach the SD filesystem", ESP_ERR_INVALID_STATE);
    return;
  }

  // r99.3/r99.4: from HERE on there is no volume to render from, so every repaint
  // request is a no-op rather than a failed render (see requestUpdate*() below). The
  // frame painted above is the whole UI for the rest of the session. Set after the
  // detach, not before it, so the two failure paths above can still reach the glass.
  paintBlocked = true;

  esp_err_t err = usbmsc::prepare(dev);
  if (err != ESP_OK) {
    fail("MSC storage prepare failed", err);
    return;
  }

  // DIAG1: from here the instrument is armed. The tick only starts once the
  // bridge is bound, and it is stopped on the way out, so a stall verdict can
  // never be produced by a screen that is not a session.
  startStallTick();

  // 4. Route the pads to OTG and let the host enumerate.
  err = usbmgadget::start();
  if (err != ESP_OK) {
    fail("USB gadget start failed", err);
    return;
  }

  // r99.3: NO REPAINT HERE. The post-detach `phase = WaitingForHost; requestUpdate()`
  // that used to sit at this point could never produce a frame (no filesystem, no
  // fonts), so it only left the previously published hit rects in place. The
  // WaitingForHost frame is the one painted above; the loop still tracks the
  // gadget state, but its requestUpdate() calls are no-ops while detached.
  LOG_INF("USB", "USB drive mode: gadget started, drive UI published pre-detach");
}

void UsbMscActivity::fail(const char* why, const int err) {
  LOG_ERR("USB", "USB drive mode: %s (%s)", why, esp_err_to_name(static_cast<esp_err_t>(err)));
  phase = Phase::Failed;
  statusSince = millis();
  requestUpdate();
}

// r99.3 RENDER GUARD. Once the volume is detached the renderer has no fonts and
// no assets to draw with, so a queued repaint is not a slow frame -- it is a
// render against a filesystem that is gone. Every request from this state is
// dropped, and said out loud exactly once so the log proves the guard fired
// instead of a render silently succeeding.
void UsbMscActivity::requestUpdate(bool immediate) {
  if (paintBlocked) {
    if (!paintBlockedLogged) {
      paintBlockedLogged = true;
      LOG_INF("USB", "USB drive mode: repaint REQUEST SUPPRESSED (immediate=%d) -- volume detached",
              immediate ? 1 : 0);
    }
    return;
  }
  Activity::requestUpdate(immediate);
}

bool UsbMscActivity::requestUpdateAndWait() {
  if (paintBlocked) {
    requestUpdate(false);  // routes through the one-shot suppression log above
    // A-P1-6: nothing can reach the glass while the volume is detached, so the
    // caller is told plainly instead of being left to assume its frame landed.
    return false;
  }
  return Activity::requestUpdateAndWait();
}

// INSTRUMENTATION (b): one line per dispatched touch action, with the contact
// point. Edge-triggered by construction -- it only fires when a tap actually
// resolved to an action, which is exactly the event under investigation.
void UsbMscActivity::logDispatch(const TouchRoute& route) {
  if (!route) return;
  LOG_INF("USB", "TAP dispatch: action=%d value=%d state=0x%02x long=%d dragPermille=%d at (%d,%d) session=%d phase=%d",
          static_cast<int>(route.event.action), static_cast<int>(route.event.value),
          static_cast<unsigned>(route.event.state), route.event.longPress ? 1 : 0,
          static_cast<int>(route.event.dragPermille), static_cast<int>(route.snap.touchX),
          static_cast<int>(route.snap.touchY), sessionStarted ? 1 : 0, static_cast<int>(phase));
}

void UsbMscActivity::onExit() {
  // Backstop only: the normal exit is exitAndReboot(), which never returns. If an
  // activity transition still happens somehow, leave the port and the gate in a
  // safe state rather than exported-by-accident.
  //
  // A session that never started must leave through here UNTOUCHED: the refused
  // screen never detached the filesystem, so there is nothing to stop, nothing
  // to tear down, and the card is still mounted for the screen we return to.
  if (sessionStarted && !sessionTornDown) {
    usbmgadget::stop();
  }
  stopStallTick();       // DIAG1: no session, no stall verdict
  powerHold_.release();  // backstop: never leave the CPU hold behind on a framework-driven exit
  powerNeed_.release(millis());  // r163 E6: same backstop for the power need
  Activity::onExit();
}

void UsbMscActivity::loop() {
  if (exitRequested) return;

  // ---------------------------------------------------------------------------------
  // THE EXIT, AND IT IS EXPLICIT. Three sources, all evaluated here, BEFORE any touch
  // is routed to a control -- so nothing on this screen can swallow the way out.
  //
  //   backKey  -- the configured Back front button, press OR release edge.
  //   arrowTap -- the top-left header arrow. NOT an implied fold: BaseTheme::drawHeader()
  //               is called by this screen's render() with its default backButton=true,
  //               and TitledHeaderBand::drawTitledHeaderBand() then does
  //               HeaderBackTapTarget::set(band.x + 4, band.y + 4 + actionOffsetY, size,
  //               size), so the rect IS recorded for this screen. MappedInputManager::
  //               wasBackGesture() re-reads that rect against the tap-down point and
  //               returns it as its own predicate, which is what this call uses. Every
  //               one of these is a const query over the current input frame
  //               (InputManager::wasTouchTap() -> HalGPIO::wasTouchTap() ->
  //               InputManager::wasTouchTap(), which only reads touchReleasedEvent and
  //               the tap-down point) -- nothing is consumed, so asking for the arrow
  //               separately from the key cannot eat the event or double-fire it.
  //   Other two -- Power press and the home gesture, unchanged.
  //
  // WHY IT DID NOT WORK BEFORE, in one line: the predicates DID fire, and exitNeutral()
  // DID call finish(); the framework then queued PendingAction::Pop and could never run it,
  // because this activity reported itself as exclusive-storage (a constant `true`) and
  // ActivityManager::loop()'s exclusive branch returns before the transition loop. The
  // exit was not missing -- it was dropped. Both the key and the arrow landed in it.
  // ---------------------------------------------------------------------------------
  const bool backKey = mappedInput.wasReleased(MappedInputManager::Button::Back) ||
                       mappedInput.wasPressed(MappedInputManager::Button::Back);
  const bool arrowTap = mappedInput.wasBackGesture();
  const bool powerPressed = mappedInput.wasPressed(MappedInputManager::Button::Power);
  const bool homeGesture = mappedInput.wasHomeGesture();
  const bool exitWanted = backKey || arrowTap || powerPressed || homeGesture;

  // Inert logging kept from the r99.3 run: one line, once, per source. No branch depends
  // on it; it exists so a future report of "the arrow did nothing" can be answered from
  // the log instead of by a rebuild.
  if (exitWanted && !sawExitSource) {
    sawExitSource = true;
    LOG_INF("USB", "EXIT source observed: key=%d arrow=%d power=%d home=%d (session=%d phase=%d)", backKey ? 1 : 0,
            arrowTap ? 1 : 0, powerPressed ? 1 : 0, homeGesture ? 1 : 0, sessionStarted ? 1 : 0,
            static_cast<int>(phase));
  }

  if (exitWanted) {
    if (!sessionStarted) {
      // NOT ARMED (the refused screen): a plain screen exit. Nothing was detached, so
      // there is nothing to stop, nothing to tear down and no reboot.
      LOG_INF("USB", "USB drive mode: refused-screen exit (key=%d arrow=%d power=%d home=%d)", backKey ? 1 : 0,
              arrowTap ? 1 : 0, powerPressed ? 1 : 0, homeGesture ? 1 : 0);
      exitNeutral();
    } else {
      // ARMED: the card is exported. The framework must not navigate here (an activity
      // swap would re-enable filesystem users over a raw-owned card), so the exit is the
      // one-way one: stop the gadget, then cold boot to Home. Same path as the EXIT control.
      LOG_INF("USB", "USB drive mode: armed exit (key=%d arrow=%d power=%d home=%d)", backKey ? 1 : 0,
              arrowTap ? 1 : 0, powerPressed ? 1 : 0, homeGesture ? 1 : 0);
      exitAndReboot();
    }
    return;
  }

  if (!sessionStarted) {
    // NOT ARMED: this is either the moment before arming (onEnter arms immediately when a
    // host is present, so in practice this is the refused screen) or a refusal that is
    // still waiting for a cable. There is no START control to tap any more -- a control
    // whose meaning depends on a frame arriving is exactly what made a lost paint fatal --
    // so the retry is the cable itself: watch it, and arm the moment a host appears.
    if (phase == Phase::NoHost && (noHostPollAt == 0 || millis() - noHostPollAt >= kNoHostPollMs)) {
      noHostPollAt = millis();
      if (usb_serial_jtag_is_connected()) {
        LOG_INF("USB", "USB drive mode: host appeared on the cable -- arming");
        startSession();
        return;
      }
    }
    // Touch still has to be routed: the screen's single EXIT control is live even here.
    logDispatch(UiAppHost::routeTouch(mappedInput));
    return;
  }

  // Touch goes through the FreeInkApp: render() published the controls' hit rects,
  // the action trampoline dispatches the tap. A dispatched action is a leaf: it has
  // already run its handler, so there is nothing left to do this pass.
  if (const TouchRoute tap = UiAppHost::routeTouch(mappedInput)) {
    logDispatch(tap);
    return;
  }

  // The card is no longer exported: the gadget state means nothing, and any
  // requestUpdate() from here would repaint from a detached filesystem. Input is
  // still routed above, so a retry tap reaches exitAndReboot() and the armed
  // watchdog ends the session regardless.
  if (sessionTornDown) return;

  if (phase == Phase::Failed) {
    if (millis() - statusSince >= kFailureHoldMs) exitAndReboot();
    return;
  }

  switch (usbmgadget::state()) {
    case usbmgadget::State::Starting:
      if (phase != Phase::Starting) {
        phase = Phase::Starting;
        requestUpdate();
      }
      break;
    case usbmgadget::State::WaitingForHost:
      if (phase != Phase::WaitingForHost) {
        phase = Phase::WaitingForHost;
        statusSince = millis();
        requestUpdate();
      }
      break;
    case usbmgadget::State::Connected:
      if (phase != Phase::Connected) {
        phase = Phase::Connected;
        statusSince = millis();
        requestUpdate();
      }
      break;
    case usbmgadget::State::Error:
      if (phase != Phase::Failed) {
        phase = Phase::Failed;
        statusSince = millis();
        requestUpdate();
      }
      break;
    case usbmgadget::State::Idle:
      break;
  }

  // A device nobody plugged into is not a drive: leave rather than sit on an
  // exported card forever.
  if (phase == Phase::WaitingForHost && millis() - statusSince >= kHostWaitTimeoutMs) {
    LOG_INF("USB", "USB drive mode: no host after %lu s, exiting",
            static_cast<unsigned long>(kHostWaitTimeoutMs / 1000UL));
    exitAndReboot();
  }
}

// Arms the unconditional reset that ends this session no matter what. Safe to
// call twice: the retry path re-arms the same timer.
void UsbMscActivity::armExitWatchdog() {
  static esp_timer_handle_t watchdog = nullptr;
  if (watchdog == nullptr) {
    esp_timer_create_args_t args{};
    args.callback = &exitWatchdogFired;
    args.name = "usbmscexit";
    if (esp_timer_create(&args, &watchdog) != ESP_OK) {
      LOG_ERR("USB", "USB drive mode: could not create exit watchdog");
      return;
    }
  }
  esp_timer_stop(watchdog);  // no-op when it is not running
  if (esp_timer_start_once(watchdog, static_cast<uint64_t>(kExitWatchdogMs) * 1000ULL) != ESP_OK) {
    LOG_ERR("USB", "USB drive mode: could not arm exit watchdog");
    return;
  }
  LOG_INF("USB", "USB drive mode: exit watchdog armed (%lu ms)",
          static_cast<unsigned long>(kExitWatchdogMs));
}

// The one place "leave this screen" is decided, for the control AND for Back/Power/Home/
// the header arrow. Before the card is handed over this is an ordinary activity exit; after
// it, the one-way gadget stop + cold boot. Never a guess from the frame on the glass.
void UsbMscActivity::exitNeutral() {
  if (sessionStarted) {
    exitAndReboot();
    return;
  }
  LOG_INF("USB", "USB drive mode: left unarmed (nothing was detached, no reboot)");
  powerHold_.release();  // no-op unless the arm path took it and then failed before the reboot
  powerNeed_.release(millis());  // r163 E6: no-op, same reason
  finish();
}

void UsbMscActivity::exitAndReboot() {
  // Debounce, not a latch: a second tap inside this window is ignored, but an
  // exit that fails to reset the device stays retryable instead of leaving a
  // dead button behind.
  const unsigned long now = millis();
  if (exitRequested && now - exitRequestedAt < kExitDebounceMs) return;
  exitRequested = true;
  exitRequestedAt = now;

  if (!sessionTornDown) {
    // 1. Soft-disconnect, uninstall, and route the port back to the console. The
    //    storage object goes with it; the SdFat volume is NOT remounted here (see
    //    UsbMscStorage.h: a live remount is the vendor's retry/rollback machine
    //    and is not part of this design).
    usbmgadget::stop();
    usbmsc::teardown();
    usbmsc::setArmed(false);
    sessionTornDown = true;
    // r99.5 DRIVE5: the gadget is stopped, so the window the hold protects is over. Released
    // AFTER the stop, never before: the frequency machinery may act again only once nothing is
    // driving the OTG PHY or the SDMMC host.
    powerHold_.release();
    // r163 E6: same edge, same reason. The card is no longer exported, so the hold goes
    // with it -- and this is a HARD release, so the inactivity clock starts again here
    // rather than at whatever moment the device happens to come back to Home.
    powerNeed_.release(millis());

    // 2. Journal the transition. The volume is detached, so the line is captured in
    //    RTC memory and appended to /.crosspoint/debug.log by the next boot --
    //    which is this reboot.
    char line[192];
    usbmscjournal::deferLine(journalStamp(line, sizeof(line),
                                          "exit: gadget stopped, cold booting to Home"));
  }

  // 3. From here the card is no longer exported, so the device MUST leave this
  //    screen: the watchdog resets unconditionally if the normal path below
  //    blocks or returns.
  armExitWatchdog();

  // 3b. DLG1 (r101.9): THE EXIT FRAME. Optional, OFF by default, and OFF is the
  //     pre-DLG1 path byte for byte -- one log line, no compose, no drive (the two
  //     statements between here and the restart are the old delay(20) and the old
  //     restart, unchanged in order and content).
  //
  //     It sits AFTER armExitWatchdog() on purpose: the frame is a whole-panel drive
  //     (~the cost of any popup) and the timer is the backstop that covers a paint that
  //     never returns. Nothing above this line is reordered, and nothing below it
  //     changes: same watchdog, same 20 ms, same restart target, and the e-paper holds
  //     this frame across ESP.restart() until Home's forced-clean first paint lands
  //     (see setup()'s BootResume::Silent arm and R99_8_BLANK_FRAMES.md).
  if (SETTINGS.usbDriveExitDialog != 0) {
    paintExitFrame();
  } else {
    LOG_INF("USB", "USB drive mode: exit frame OFF (usbDriveExitDialog=0) -- exit path paints nothing");
  }

  delay(20);
  // 4. Exit by reboot, never by remount. setup() brings the filesystem, the UI
  //    and the console route back and lands on Home.
  restartToHomeAfterStorageHandoff();

  // Only reachable if that call returned instead of resetting. Never swallow
  // that: say it out loud, drop the latch so the user can tap EXIT again, and
  // leave the watchdog to finish the job. No repaint here: the frames on the
  // glass already say what is happening (the DLG1 exit frame, or the drive-mode
  // screen it replaced when that setting is off), and a retry re-enters this
  // function, so the exit frame -- if enabled -- is painted again by the branch
  // above rather than duplicated here.
  LOG_ERR("USB", "USB drive mode: handoff restart returned without resetting; retry is armed");
  exitRequested = false;
}

// DLG1 (r101.9). The one frame this screen paints on the way OUT, and the only paint in
// the tree that happens with the SD volume detached and never remounted.
//
// WHAT IS AVAILABLE HERE: nothing off the card. The volume was handed to the host in
// startSession() and this path does not remount it, so Storage is not a source of fonts,
// images, theme assets or translation tables. `.render()`/buildDriveScreen() are therefore
// unusable (they read the theme and would also re-announce the mode we are leaving), and
// so is every BMP/cover path.
//
// WHAT IS STILL AVAILABLE: the built-in UI_12 face. Its EpdFontData is compiled into the
// image (lib/EpdFont/builtinFonts/ubuntu_12_regular.h: `groups == nullptr`,
// `glyphMissCtx == nullptr`), so getGlyphBitmap() returns `&bitmap[glyph->dataOffset]`
// straight out of flash and never reaches the FontDecompressor or SdCardFont branches.
// No FontCacheManager prewarm, no .cpfont, no TTF: one flash array, the framebuffer in
// PSRAM, and the panel. That is the whole dependency list of this function.
//
// The lock is TRY: this runs on the activity's own loop() task, which is the task that
// renders, so it is free by construction. If it were ever not, skipping the frame is the
// pre-DLG1 behaviour instead of a wait on a mutex held by something that may itself be
// waiting for the storage we just took away.
void UsbMscActivity::paintExitFrame() {
  // r101.10 DLG2: A BUSY LOCK IS NOT A REASON TO DROP THE FRAME. The Try below is still the first
  // attempt (this runs on the activity's own loop() task, so it is free by construction), but a
  // single miss used to skip the frame silently -- and "the exit dialog never appears" is exactly
  // what the operator reported. A bounded retry now covers a lock a render wave is still holding
  // (a few tens of ms), and giving up is said out loud instead of looking like a paint.
  constexpr uint8_t kLockAttempts = 3;
  for (uint8_t attempt = 1; attempt <= kLockAttempts; ++attempt) {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) {
      LOG_INF("USB", "USB drive mode: exit frame render lock busy (attempt %u/%u); retrying",
              static_cast<unsigned>(attempt), static_cast<unsigned>(kLockAttempts));
      delay(20);
      continue;
    }

    const int screenWidth = renderer.getScreenWidth();
    const int screenHeight = renderer.getScreenHeight();
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, kExitFrame);
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int pad = 24;
    int boxWidth = textWidth + 2 * pad;
    if (boxWidth > screenWidth - 2 * pad) boxWidth = screenWidth - 2 * pad;
    const int boxHeight = lineHeight + 2 * pad;
    const int boxX = (screenWidth - boxWidth) / 2;
    const int boxY = (screenHeight - boxHeight) / 2;

    renderer.clearScreen();
    renderer.drawRect(boxX, boxY, boxWidth, boxHeight, 2);
    renderer.drawCenteredText(UI_12_FONT_ID, boxY + pad, kExitFrame);
    // HALF, not FULL: the one waveform every popup in this tree pays (BaseTheme::drawPopup
    // commits HALF). The residual it leaves is wiped by Home's forced-clean first paint,
    // which setup() arms for this very reboot (BootResume::Silent -> forceCleanNextRefresh).
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    // One line per drive, as DIALOG_REGISTER.md's checklist asks: the frame either reached
    // the glass through that displayBuffer() or the log says it did not.
    LOG_INF("USB", "USB drive mode: EXIT FRAME driven (mode=HALF, text=\"%s\", box=%dx%d, attempt=%u)", kExitFrame,
            boxWidth, boxHeight, static_cast<unsigned>(attempt));
    return;
  }
  LOG_ERR("USB", "USB drive mode: EXIT FRAME GAVE UP -- render lock busy after %u attempts; exit path paints nothing",
          static_cast<unsigned>(kLockAttempts));
}

void UsbMscActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, UITheme::getInstance().getTitledHeaderHeight()}, kTitle);

  renderUi();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void UsbMscActivity::driveScreen(UiScreen& screen, void* user) {
  static_cast<UsbMscActivity*>(user)->buildDriveScreen(screen);
}

void UsbMscActivity::exitTrampoline(const fui::ActionEvent& /*event*/, void* user) {
  // The single control is ALWAYS ACTION_EXIT (see buildDriveScreen), so this handler --
  // not the frame -- decides what "exit" means: a plain screen exit before anything was
  // armed, and the one-way gadget stop + cold boot once the card is exported.
  static_cast<UsbMscActivity*>(user)->exitNeutral();
}

void UsbMscActivity::buildDriveScreen(UiScreen& screen) const {
  const char* message = "";
  const char* detail = "";
  switch (phase) {
    case Phase::Idle:
      message = kIdleMessage;
      detail = kIdleDetail;
      break;
    case Phase::Starting:
      message = "PREPARING DRIVE";
      detail = kExitHintWait;
      break;
    case Phase::WaitingForHost:
      // r99.3: this is the frame painted BEFORE the detach and it stays on screen
      // for the whole session, so it must not claim a host event we cannot repaint.
      message = "DRIVE MODE ACTIVE";
      detail = kExitHintActive;
      break;
    case Phase::Connected:
      message = "CONNECTED";
      detail = kExitHintConnected;
      break;
    case Phase::Failed:
      message = "DRIVE MODE FAILED";
      detail = "Restarting to Home. Your books are untouched.";
      break;
    case Phase::NoHost:
      // Refused before anything was armed, detached or routed. Staying on this
      // screen is the point: plug the cable in and press START again.
      message = kNoHostMessage;
      detail = kNoHostDetail;
      break;
  }
  // The cable is free exactly while nothing has been handed over, which is the same
  // `sessionStarted` switch the sleep policy and the exclusive-storage gate use, so the
  // stamp can never disagree with the session state.
  const bool cardFree = !sessionStarted;

  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(metrics.topPadding + UITheme::getInstance().getTitledHeaderHeight()),
      static_cast<int16_t>(metrics.contentSidePadding), static_cast<int16_t>(metrics.buttonHintsHeight),
      static_cast<int16_t>(metrics.contentSidePadding)});

  // The exit affordance is a real button, pinned to the bottom of the body: on a
  // touch board the visible control IS the way out, and it must not be reachable
  // only by a hardware key the user may not realise exists.
  const int16_t buttonHeight = static_cast<int16_t>(metrics.menuRowHeight);
  const fui::Rect buttonArea = screen.takeBottom(buttonHeight, metrics.verticalSpacing);

  // r68-r70 typography scale, top to bottom: a state stamp (small, bold), the
  // phase in the title face, the explanation in the body face, and the eject
  // instruction in the small face as its own outlined band. Only the stamp and
  // the body are sized from tokens so the same code follows any theme.
  auto stampStyle = screen.theme().smallText;
  stampStyle.align = fui::TextAlign::Center;
  stampStyle.bold = true;
  stampStyle.maxLines = 1;
  stampStyle.inverted = cardFree;  // white ink on the filled stamp

  auto messageStyle = screen.theme().titleText;
  messageStyle.align = fui::TextAlign::Center;
  messageStyle.bold = true;
  messageStyle.maxLines = 2;

  auto detailStyle = screen.theme().bodyText;
  detailStyle.align = fui::TextAlign::Center;
  detailStyle.maxLines = 3;

  const int16_t stampPad = static_cast<int16_t>(uiDensity::px(8));
  const int16_t stampTextHeight = screen.target().lineHeight(stampStyle.font);
  const fui::Rect stampArea =
      screen.takeTop(static_cast<int16_t>(stampTextHeight + 2 * stampPad), metrics.verticalSpacing);
  const char* stamp = cardFree ? kSafeToUnplug : kCardInUse;
  if (cardFree) {
    // Filled stamp: the "go" state is an instruction, so it reads as one block
    // of ink rather than a box the eye has to parse.
    screen.target().fill(stampArea, fui::Paint::solid(fui::Color::Black), screen.theme().controlRadius);
  } else {
    screen.target().stroke(stampArea, fui::Paint::solid(fui::Color::Black), static_cast<uint8_t>(uiDensity::px(2)),
                           screen.theme().controlRadius);
  }
  screen.target().text(stampArea, stamp, stampStyle);

  // The eject instruction band belongs to a live session only: in the refused and
  // failed states nothing is exported, and telling the operator not to touch a cable
  // they are free to pull is exactly the ambiguity this item removes.
  if (!cardFree) {
    auto warnStyle = screen.theme().smallText;
    warnStyle.align = fui::TextAlign::Center;
    warnStyle.bold = true;
    warnStyle.maxLines = 3;
    const int16_t warnPad = static_cast<int16_t>(uiDensity::px(8));
    const int16_t warnTextWidth = static_cast<int16_t>(screen.body().width - 2 * warnPad);
    const int16_t warnTextHeight =
        fui::measureWrappedText(screen.target(), kUnplugWarning, warnStyle, warnTextWidth).height;
    const fui::Rect warnArea =
        screen.takeBottom(static_cast<int16_t>(warnTextHeight + 2 * warnPad), metrics.verticalSpacing);
    screen.target().stroke(warnArea, fui::Paint::solid(fui::Color::Black), static_cast<uint8_t>(uiDensity::px(2)),
                           screen.theme().controlRadius);
    screen.target().text(fui::Rect{static_cast<int16_t>(warnArea.x + warnPad),
                                   static_cast<int16_t>(warnArea.y + warnPad), warnTextWidth, warnTextHeight},
                         kUnplugWarning, warnStyle);
  }

  const fui::Rect body = screen.body();
  const int16_t messageHeight = fui::measureWrappedText(screen.target(), message, messageStyle, body.width).height;
  const int16_t detailHeight = fui::measureWrappedText(screen.target(), detail, detailStyle, body.width).height;
  const int16_t gap = screen.theme().spaceMd;
  const int16_t totalHeight = static_cast<int16_t>(messageHeight + gap + detailHeight);
  if (body.height > totalHeight) screen.spacer(static_cast<int16_t>((body.height - totalHeight) / 2));

  screen.target().text(screen.takeTop(messageHeight, gap), message, messageStyle);
  screen.target().text(screen.takeTop(detailHeight), detail, detailStyle);

  // ONE control, ONE meaning, ALWAYS. This is r61's line --
  // `exitButton.action = ACTION_EXIT;` unconditionally -- and it is the property that
  // makes a lost frame harmless: whatever frame the glass is holding, the control it
  // publishes (or published last) exits. r99 replaced this with
  // `neutral ? ACTION_START : ACTION_EXIT`, so a lost paint left a live ACTION_START
  // behind and the button became dead. What "exit" DOES is decided in
  // exitTrampoline()/loop(), from session state, never from the frame.
  fui::ButtonProps exitButton;
  exitButton.label = kExitButton;
  exitButton.action = ACTION_EXIT;
  // Themed button with an outline, same treatment as the catalogue's cancel
  // button, so it reads as a control rather than a label.
  exitButton.styles = screen.theme().button;
  exitButton.styles.normal.border = fui::Paint::solid(fui::Color::Black);
  exitButton.styles.normal.borderWidth = 1;
  exitButton.radius = screen.theme().controlRadius;
  const int16_t buttonWidth = static_cast<int16_t>(buttonArea.width / 2);
  const fui::Rect controlRect{static_cast<int16_t>(buttonArea.x + (buttonArea.width - buttonWidth) / 2),
                              buttonArea.y, buttonWidth, buttonArea.height};
  // INSTRUMENTATION (a): what this frame actually registers, at the moment it
  // registers it. One control at a time is the whole design, so one line per
  // build -- and it names the action id, which is the fact we need: the r99.2
  // failure was a published ACTION_START that outlived its screen.
  LOG_INF("USB",
          "SCREEN BUILD: phase=%d session=%d control=\"%s\" action=%d rect=(x=%d y=%d w=%d h=%d)",
          static_cast<int>(phase), sessionStarted ? 1 : 0, exitButton.label,
          static_cast<int>(exitButton.action), static_cast<int>(controlRect.x),
          static_cast<int>(controlRect.y), static_cast<int>(controlRect.width),
          static_cast<int>(controlRect.height));
  screen.button(exitButton, controlRect);
}
