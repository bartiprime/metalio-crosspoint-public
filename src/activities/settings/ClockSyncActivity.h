#pragma once

#include "activities/Activity.h"

// Manual NTP resync action. Runs a forced sync (bypassing the once-per-device debounce),
// reports success/failure, then waits for Back. If WiFi is not connected yet, it reuses the
// normal WiFi selection flow first.
class ClockSyncActivity final : public Activity {
 public:
  explicit ClockSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClockSync", renderer, mappedInput) {
    // This activity picks its own modes: FAST for the transient "Syncing..." frame, exactly one FULL
    // for the result. The global transition policy would arm AUTO on every requestUpdate and AUTO
    // resolves to HALF, which silently rewrote the caller's choice.
    transitionRefreshPolicy_ = false;
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // Answers Button::Back in loop(): the manager must not navigate for this screen.
  bool ownsBackKey() const override { return true; }
  bool skipLoopDelay() override { return true; }
  void render(RenderLock&&) override;

 private:
  enum State { SYNCING, SUCCESS, NO_WIFI, FAILED };
  State state = SYNCING;
  char syncedTime[16] = {0};
  bool shouldTearDownWifiOnExit = false;
  // Snapshot of SETTINGS.clockHasBeenSynced taken at onEnter. The Wi-Fi selection child runs its
  // own post-connect sync and sets that flag on success; comparing entry-vs-now is what tells us the
  // clock was synced DURING this flow, rather than having been synced at some point in the past.
  uint8_t clockWasSyncedAtEntry = 0;

  void runSync();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
};
