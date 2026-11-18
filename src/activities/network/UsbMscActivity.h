#pragma once

#include <HalPowerManager.h>
#include <HalStorage.h>

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "system/PowerNeed.h"  // r163 E6: powerNeed_, the hard UsbVirtualDisk need

// USB drive mode (scope C4): full-screen screen that hands the SD card to a USB
// host as a mass-storage device and gets it back by REBOOTING.
//
// Why a screen of its own instead of the SDK's UsbDriveActivity: that one drives
// the Arduino-TinyUSB UsbMassStorage implementation, which needs Arduino's
// CONFIG_TINYUSB_ENABLED and the USB peripheral this board's console lives on.
// This board takes the port over with the ANA_SW switch (GPIO36) and runs
// esp_tinyusb's MSC class on the OTG/HS-UTMI side instead, so the console stays
// where it is and only the pads move. See src/platform/UsbMsc*.{h,cpp}.
//
// The session is one-way: the START tap detaches the SdFat volume and it is
// never remounted. Exit is a cold boot to Home, which re-establishes the
// filesystem, the UI and the console route from a known-good state (the shipping
// vendor firmware does the same, and RickyOS's string "reboot before SD remount"
// says it outright).
//
// Entry behaves like r61 (4e4db525) -- the build the operator reports working: the host
// precheck runs first, and if a computer is on the cable the session arms immediately,
// painting its one and only frame BEFORE the filesystem is detached. There is no "offer
// to start" state and, deliberately, NO control whose label or action depends on which
// frame reached the glass: the single control is always EXIT, and what EXIT does is
// decided from session state in exitTrampoline()/loop(). A lost paint is therefore
// harmless again. No host -> refuse and stay put (nothing armed, nothing detached, the
// card and the shell untouched), and the cable itself is the retry: loop() watches it.
//
// Exiting works in both states, from the Back key and from the top-left header arrow,
// because loop() evaluates both BEFORE routing any touch, and because the exit is
// honoured now: the framework can only navigate away from a non-exclusive activity, so
// this screen reports itself exclusive only once the card has actually been handed over.
class UsbMscActivity final : public Activity, private UiAppHost {
 public:
  UsbMscActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("UsbMsc", renderer, mappedInput), UiAppHost(renderer) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // Answers Button::Back in loop(): the manager must not navigate for this screen.
  bool ownsBackKey() const override { return true; }
  void render(RenderLock&&) override;

  // r99.3 render guard: once the SD volume is detached for the host there is no
  // filesystem to draw fonts or assets from, so a repaint request from this state
  // is dropped instead of being issued as a render that cannot succeed. See the
  // definitions in the .cpp -- the suppression is logged once.
  void requestUpdate(bool immediate = false) override;
  // A-P1-6: false when the volume is detached and no paint can reach the glass.
  bool requestUpdateAndWait() override;

  // The card and the USB port belong to the host for the whole session: the panel must
  // never auto-sleep out from under a transfer, and nothing else in the firmware may
  // touch storage while the sectors are exported. Both switches are `sessionStarted` --
  // the one flag that says the card was handed over -- so the refused (NoHost) screen is
  // an ordinary screen in every respect.
  bool preventAutoSleep() override { return sessionStarted; }
  // r99.4 DRIVE4. This used to be a constant `true`, and that constant is what turned a
  // lost paint into a permanent one. ActivityManager::loop()'s exclusive branch calls the
  // activity's loop() and RETURNS before the `while (pendingAction != None)` transition
  // loop, deliberately: an exclusive-storage activity must restart rather than navigate.
  // A `finish()` from this screen therefore queued PendingAction::Pop that nothing could
  // ever consume -- and renderTaskLoop() SKIPS every render while a pending action is
  // queued, while still notifying any requestUpdateAndWait() waiter, so the caller saw
  // "painted" when nothing had been. The screen then froze on whatever frame it had, for
  // good, while the main task, input, haptics and the gadget all stayed alive.
  // Nothing is exported before a session starts, so there is no raw owner to protect then:
  // the neutral/refused screen must be an ordinary screen so the framework can honour its
  // exit. From the moment the card is handed over it is exclusive for the rest of the
  // session, which is exactly what that branch is guarding.
  bool requiresExclusiveStorageLoop() const override { return sessionStarted; }

 private:
  enum class Phase { Idle, Starting, WaitingForHost, Connected, Failed, NoHost };

  // r99.4: ONE action id, forever. r61's screen ended in an unconditional
  // `exitButton.action = ACTION_EXIT;`; the r99 `neutral ? ACTION_START : ACTION_EXIT`
  // is what let a lost paint leave a live ACTION_START on the glass.
  static constexpr freeink::ui::ActionId ACTION_EXIT = 2;

  // A host that never shows up (or a cable that is not plugged in) must not
  // strand the device as a dead drive with no way back: exit on its own.
  static constexpr unsigned long kHostWaitTimeoutMs = 5UL * 60UL * 1000UL;
  // How often the refused screen re-tests the cable for a host to appear.
  static constexpr unsigned long kNoHostPollMs = 500UL;
  // How long a bring-up failure stays on screen before the cold boot.
  static constexpr unsigned long kFailureHoldMs = 15UL * 1000UL;
  // Tap debounce, not a one-way latch: the exit can be retried if it fails.
  static constexpr unsigned long kExitDebounceMs = 750UL;
  // Hard fallback. Once the gadget is stopped the card is no longer exported,
  // so nothing may keep the device here: this resets unconditionally.
  static constexpr unsigned long kExitWatchdogMs = 3000UL;

  static void driveScreen(UiScreen& screen, void* user);
  static void exitTrampoline(const freeink::ui::ActionEvent& event, void* user);

  void buildDriveScreen(UiScreen& screen) const;
  void startSession();
  void fail(const char* why, int err = 0);
  void exitNeutral();
  void exitAndReboot();
  void armExitWatchdog();
  // DLG1 (r101.9): the drive-mode EXIT frame. Composed and driven by hand, from the
  // built-in UI_12 font only, because the SD volume is already detached (not remounted)
  // when it runs -- see the definition for what that rules out.
  void paintExitFrame();
  void logDispatch(const TouchRoute& route);

  Phase phase = Phase::Idle;
  // r99.5 DRIVE5: the HELD resource that spans the armed window. Taken in startSession() before
  // the shell is deactivated, the card detached or the pads moved, released when the session is
  // over. While it is held no task can apply a CPU/PLL frequency change (see
  // HalPowerManager::FullSpeedHold) -- the r99.4 panic was exactly such a change landing 1 ms
  // after the gadget started, decided from a predicate read one iteration earlier.
  HalPowerManager::FullSpeedHold powerHold_;
  // r163 E6: the card is exported, so automatic standby must not park the panel out from
  // under a transfer. Acquired at the same statement as the CPU hold (after `sessionStarted`,
  // before the shell is deactivated) and released with it, so the two can never disagree.
  // HARD, i.e. it blocks the INACTIVITY standby only: the power key still sleeps the device,
  // which is the vendor's `UsbVirtualDisk` semantic and the operator's only escape hatch.
  powerneed::ScopedNeed powerNeed_{powerneed::Need::UsbVirtualDisk};
  // True from the moment the session arms: the only switch that says the filesystem
  // was handed over, so onExit(), the sleep policy and the exclusive-storage gate can
  // tell "refused / not started" from "session".
  bool sessionStarted = false;
  // True from the moment the MSC gate is armed (the step immediately before the
  // detach): no render may be attempted after it. Set once, never cleared.
  bool paintBlocked = false;
  bool paintBlockedLogged = false;
  // Inert latch for the one-shot exit-source log. No branch depends on it.
  bool sawExitSource = false;
  bool exitRequested = false;
  unsigned long exitRequestedAt = 0;
  // True once the gadget is stopped and the card is ours again: from then on
  // the session must only ever leave by reset.
  bool sessionTornDown = false;
  unsigned long statusSince = 0;
  // Last time the refused screen tested the cable for a host (see loop()).
  unsigned long noHostPollAt = 0;
};
