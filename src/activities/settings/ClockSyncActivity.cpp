#include "ClockSyncActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ClockSyncActivity::onEnter() {
  // One line per attempt: without it a dump cannot tell "the sync never ran" from "it ran and the
  // network refused", which is exactly the ambiguity that wasted the last two NTP rounds.
  LOG_INF("CLKSYNC", "clock sync entered (wifi status=%d connected=%d)", static_cast<int>(WiFi.status()),
          static_cast<int>(WiFi.status() == WL_CONNECTED));

  Activity::onEnter();
  // The push that landed here armed the global activity-transition promotion; this activity decides
  // its own modes, so drop it before the Syncing frame can paint with it.
  renderer.cancelPromotedRefresh();
  state = SYNCING;
  clockWasSyncedAtEntry = SETTINGS.clockHasBeenSynced;
  syncedTime[0] = '\0';

  if (WiFi.status() == WL_CONNECTED) {
    requestUpdate();
    return;
  }

  shouldTearDownWifiOnExit = true;
  launchWifiSelection();
}

void ClockSyncActivity::onExit() {
  Activity::onExit();

  if (shouldTearDownWifiOnExit && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void ClockSyncActivity::launchWifiSelection() {
  LOG_INF("CLK", "Manual sync requested without WiFi, launching WiFi selection");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ClockSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    LOG_INF("CLK", "WiFi selection cancelled before manual clock sync");
    finish();
    return;
  }

  state = SYNCING;
  // The transient "Syncing..." frame is NOT the place for the whole-panel drive. runSync() runs on
  // the very next tick and arms a FULL for the RESULT frame; a FULL here as well is two from-white
  // GC16 drives a second apart, which is the alternating black/white flash the operator sees. This
  // frame is painted as a binary DU (FAST) -- explicit, because the pop that got us here armed the
  // global transition policy and render() no longer inherits an armed mode.
  renderer.cancelPromotedRefresh();
  requestUpdate();
}

void ClockSyncActivity::runSync() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF("CLK", "Manual sync requested but WiFi is not connected after selection");
    state = NO_WIFI;
    requestFullRefresh();
    return;
  }

  // The Wi-Fi selection child runs its own post-connect sync and sets clockHasBeenSynced on success.
  // Issuing a SECOND configTzTime/SNTP exchange a tick later is what put "Sync failed" on screen
  // while the clock had in fact just been set: the child's attempt worked, this one failed and owned
  // the result. If the flag flipped during this visit, the clock is fresh - read it back and report.
  if (!clockWasSyncedAtEntry && SETTINGS.clockHasBeenSynced) {
    LOG_INF("CLK", "manual sync skipped: the Wi-Fi connect already synced the clock this visit");
    char fresh[9];
    if (halClock.formatTime(fresh, sizeof(fresh), SETTINGS.clockFormat == 1)) {
      snprintf(syncedTime, sizeof(syncedTime), "%s", fresh);
    }
    state = SUCCESS;
    requestFullRefresh();
    return;
  }

  const bool ok = halClock.syncFromNTP();
  if (!ok) {
    state = FAILED;
    requestFullRefresh();
    return;
  }

  // Mark as synced so the auto-sync hook stops firing on future WiFi connects.
  SETTINGS.clockHasBeenSynced = 1;
  SETTINGS.saveToFile();

  // Read the freshly synced time back for the user-facing confirmation.
  char buf[9];
  if (halClock.formatTime(buf, sizeof(buf), SETTINGS.clockFormat == 1)) {
    snprintf(syncedTime, sizeof(syncedTime), "%s", buf);
  }
  state = SUCCESS;
  requestFullRefresh();
}

void ClockSyncActivity::loop() {
  if (state == SYNCING) {
    // First-tick: render the "Syncing..." screen, then perform the (blocking) sync.
    // requestUpdateAndWait below forces the render before we block on WiFi.
    requestUpdateAndWait();
    runSync();
    return;
  }

  // Release edge (Activity.h, ownsBackKey), not press: a press-down answered here would leave before
  // the manager's hold could fire and holding Back could never reach Home. SYNCING above is a
  // blocking call and samples no input at all, so it has no tap to give.
  int x = 0;
  int y = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}

void ClockSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, UITheme::getInstance().getTitledHeaderHeight()}, tr(STR_CLOCK_SYNC));

  const int midY = pageHeight / 2;

  switch (state) {
    case SYNCING:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_CLOCK_SYNCING));
      break;
    case SUCCESS: {
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_OK), true, EpdFontFamily::BOLD);
      if (syncedTime[0] != '\0') {
        // Sized for the label in any language: STR_CURRENT_TIME is 26 bytes in
        // Russian (UTF-8 Cyrillic is 2 bytes per letter) versus 13 in English,
        // plus a separator and up to "08:56 PM".
        char line[64];
        snprintf(line, sizeof(line), "%s %s", tr(STR_CURRENT_TIME), syncedTime);
        renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, line);
      }
      break;
    }
    case NO_WIFI:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_NO_WIFI), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_CLOCK_SYNC_NO_WIFI_HINT));
      break;
    case FAILED:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_FAIL), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_CHECK_SERIAL_OUTPUT));
      break;
  }

  if (state != SYNCING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    // Terminal frame: the one whole-panel drive of this entry. runSync() armed exactly one FULL via
    // requestFullRefresh(), consumed here; a repaint of the same terminal state finds nothing armed
    // and stays on displayBuffer()'s FAST default.
    renderer.displayBuffer();
    return;
  }

  // Transient "Syncing..." screen: DU, then the result frame takes the FULL.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
