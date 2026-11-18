#include "WifiSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <esp_mac.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "system/PowerNeed.h"  // r163 E6: ForegroundNetwork, a soft need
#include "util/WifiConnect.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/PluginEvents.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_SCAN = 2;
constexpr fui::ActionId ACTION_PROMPT = 3;
}  // namespace

WifiSelectionActivity::WifiSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const bool autoConnect)
    : Activity("WifiSelection", renderer, mappedInput), UiAppHost(renderer), allowAutoConnect(autoConnect) {}

void WifiSelectionActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<WifiSelectionActivity*>(user);
  if (self->state != WifiSelectionState::NETWORK_LIST) return;
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->networks.size())) return;
  self->selectedNetworkIndex = static_cast<size_t>(event.value);
  // Long-press a saved network to forget it (mirrors the Left-button hold in loop()).
  if (event.longPress) {
    if (self->networks[self->selectedNetworkIndex].hasSavedPassword) {
      self->selectedSSID = self->networks[self->selectedNetworkIndex].ssid;
      self->state = WifiSelectionState::FORGET_PROMPT;
      self->forgetPromptSelection = 0;  // Default to "Cancel"
      self->app.clearTapFlash();
      self->requestUpdate();
    }
    return;
  }
  // Selection leaves this screen (password entry / connecting); a lingering
  // flash would gray an unrelated row.
  self->app.clearTapFlash();
  self->selectNetwork(static_cast<int>(self->selectedNetworkIndex));
}

void WifiSelectionActivity::onScanEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<WifiSelectionActivity*>(user);
  if (self->state != WifiSelectionState::NETWORK_LIST) return;
  self->app.clearTapFlash();  // the scan screen replaces this one
  self->startWifiScan();
}

void WifiSelectionActivity::onPromptEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<WifiSelectionActivity*>(user);
  if (self->state == WifiSelectionState::SAVE_PROMPT) {
    self->savePromptSelection = event.value;
    self->app.clearTapFlash();  // the action leaves this screen
    if (self->savePromptSelection == 0) {
      RenderLock lock(*self);
      WIFI_STORE.addCredential(self->selectedSSID, self->enteredPassword);
    }
    self->onComplete(true);
    return;
  }
  if (self->state == WifiSelectionState::FORGET_PROMPT) {
    self->forgetPromptSelection = event.value;
    self->app.clearTapFlash();  // the action leaves this screen
    if (self->forgetPromptSelection == 1) {
      RenderLock lock(*self);
      WIFI_STORE.removeCredential(self->selectedSSID);
      const auto network = find_if(self->networks.begin(), self->networks.end(),
                                   [self](const WifiNetworkInfo& net) { return net.ssid == self->selectedSSID; });
      if (network != self->networks.end()) {
        network->hasSavedPassword = false;
      }
    }
    self->startWifiScan();
  }
}

void WifiSelectionActivity::onEnter() {
  Activity::onEnter();

  // r163 E6: a foreground network session, as a SOFT need. It is declared so the
  // `needs` debug surface can show that a scan/join is in flight -- and so a session
  // that never ends is visible as an aged hold -- and it deliberately changes NOTHING
  // about standby: this screen still answers preventAutoSleep() with `true`, exactly as
  // before, and a soft release does not touch the inactivity clock.
  foregroundNetworkNeed_.acquire(millis());

  // Load saved WiFi credentials - SD card operations need lock as we use SPI
  // for both
  {
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }

  // Reset state
  selectedNetworkIndex = 0;
  networks.clear();
  networkStatuses.clear();
  networkRowItems.clear();
  realNetworkCount = 0;
  // r89: this object survives a child activity (the keyboard) being pushed and popped, and it is
  // also re-entered on a re-entry. lastRenderedState must not carry the pre-keyboard frame's state
  // into the next paint: the screen on the glass is the keyboard's, so the first frame after the
  // return is a wholesale replacement whatever the state pair says.
  lastRenderedState = -1;
  state = WifiSelectionState::SCANNING;
  selectedSSID.clear();
  connectedIP.clear();
  connectionError.clear();
  enteredPassword.clear();
  usedSavedPassword = false;
  savePromptSelection = 0;
  forgetPromptSelection = 0;
  autoConnecting = false;
  manualNetworkListRequested = false;
  autoAttemptedSsids.clear();
  const size_t savedCredentialCount = WIFI_STORE.getCredentialCount();
  autoAttemptedSsids.reserve(savedCredentialCount);

  // Read the hardware-derived station MAC directly. WiFi.macAddress() depends
  // on the STA netif already existing, but this screen is entered while WiFi
  // is often still off (notably after an X4 Pro WiFi session).
  uint8_t mac[6] = {};
  char macStr[64];
  const esp_err_t macResult = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (macResult == ESP_OK) {
    snprintf(macStr, sizeof(macStr), "%s %02x-%02x-%02x-%02x-%02x-%02x", tr(STR_MAC_ADDRESS), mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
  } else {
    LOG_ERR("WIFI", "Failed to read station MAC (err=%d)", static_cast<int>(macResult));
    snprintf(macStr, sizeof(macStr), "%s --", tr(STR_MAC_ADDRESS));
  }
  cachedMacAddress = std::string(macStr);

  listNav.reset();
  resetUi();
  app.on(ACTION_ROW, &WifiSelectionActivity::onRowEvent, this);
  app.on(ACTION_SCAN, &WifiSelectionActivity::onScanEvent, this);
  app.on(ACTION_PROMPT, &WifiSelectionActivity::onPromptEvent, this);
  app.setScreen(&WifiSelectionActivity::listScreen, this);

  // Trigger first update to show scanning message
  requestUpdate();

  // Attempt to auto-connect to known networks. Try the last successful
  // network first for speed, then scan and try any visible saved networks by
  // signal strength. The user can interrupt this and show the scan result.
  if (allowAutoConnect && savedCredentialCount != 0) {
    const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
    if (!lastSsid.empty()) {
      const auto cred = WIFI_STORE.findCredential(lastSsid);
      if (cred && tryAutoConnectCredential(*cred)) {
        return;
      }
    }

    startWifiScan(true);
    return;
  }

  // Fallback to scanning
  startWifiScan();
}

void WifiSelectionActivity::onExit() {
  Activity::onExit();

  LOG_DBG("WIFI", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  // Stop any ongoing WiFi scan
  LOG_DBG("WIFI", "Deleting WiFi scan...");
  WiFi.scanDelete();
  LOG_DBG("WIFI", "Free heap after scanDelete: %d bytes", ESP.getFreeHeap());

  // Note: We do NOT disconnect WiFi here - the parent activity
  // (CrossPointWebServerActivity) manages WiFi connection state. We just clean
  // up the scan and task.

  // r163 E6: the foreground network session ends with this screen. SOFT, so the
  // release does not move the inactivity clock -- a session that ended does not
  // postpone the next automatic standby by even one tick.
  foregroundNetworkNeed_.release(millis());

  LOG_DBG("WIFI", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

void WifiSelectionActivity::startWifiScan(const bool autoScan) {
  autoConnecting = autoScan;
  manualNetworkListRequested = false;
  listNav.reset();
  state = WifiSelectionState::SCANNING;
  networks.clear();
  // The cached rows point into the cleared networks' strings; drop them too.
  networkStatuses.clear();
  networkRowItems.clear();
  requestUpdate();

  // Set WiFi mode to station
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Start async scan
  WiFi.scanNetworks(true);  // true = async scan
}

void WifiSelectionActivity::processWifiScanResults() {
  const int16_t scanResult = WiFi.scanComplete();

  if (scanResult == WIFI_SCAN_RUNNING) {
    // Scan still in progress
    return;
  }

  if (scanResult == WIFI_SCAN_FAILED) {
    networks.clear();
    realNetworkCount = 0;
    appendHiddenNetworkEntry();
    rebuildNetworkRowItems();
    autoConnecting = false;
    state = WifiSelectionState::NETWORK_LIST;
    selectedNetworkIndex = 0;
    requestUpdate();
    return;
  }

  // Scan complete, process results — deduplicate in-place, keeping strongest signal
  networks.clear();
  networks.reserve(scanResult);

  for (int i = 0; i < scanResult; i++) {
    char ssid[33];
    strlcpy(ssid, WiFi.SSID(i).c_str(), sizeof(ssid));
    const int32_t rssi = WiFi.RSSI(i);

    // Skip hidden networks (empty SSID)
    if (ssid[0] == '\0') {
      continue;
    }

    auto it =
        std::find_if(networks.begin(), networks.end(), [&ssid](const WifiNetworkInfo& n) { return n.ssid == ssid; });
    if (it == networks.end()) {
      WifiNetworkInfo network;
      network.ssid = ssid;
      network.rssi = rssi;
      network.isEncrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      network.hasSavedPassword = WIFI_STORE.hasSavedCredential(network.ssid);
      networks.push_back(std::move(network));
    } else if (rssi > it->rssi) {
      it->rssi = rssi;
      it->isEncrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
  }

  // Sort: saved-password networks first, then by signal strength (strongest first)
  std::sort(networks.begin(), networks.end(), [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) {
    if (a.hasSavedPassword != b.hasSavedPassword) {
      return a.hasSavedPassword;
    }
    return a.rssi > b.rssi;
  });

  realNetworkCount = networks.size();
  appendHiddenNetworkEntry();
  rebuildNetworkRowItems();

  WiFi.scanDelete();

  if (autoConnecting && !manualNetworkListRequested && tryNextSavedNetworkFromScan()) {
    return;
  }

  autoConnecting = false;
  state = WifiSelectionState::NETWORK_LIST;
  selectedNetworkIndex = 0;
  requestFullRefresh();  // A wholesale screen/state change must be FULL (whole-panel drive); HALF is diff-based on this panel and will ghost.
}

void WifiSelectionActivity::appendHiddenNetworkEntry() {
  // Synthetic list entry that lets the user type an SSID that is not broadcast.
  // ESP32 can join hidden APs as long as the SSID is supplied to WiFi.begin().
  WifiNetworkInfo placeholder;
  placeholder.rssi = 0;
  placeholder.isEncrypted = true;  // Treated as encrypted; an empty password still connects open APs
  placeholder.hasSavedPassword = false;
  placeholder.isHiddenPlaceholder = true;
  networks.push_back(std::move(placeholder));
}

// Derives networkStatuses/networkRowItems from `networks`. Called whenever
// `networks` changes (both branches of processWifiScanResults()) so
// buildListScreen() reuses the cached rows on every repaint instead of
// re-deriving a "+ * ||||" status string per network each time.
void WifiSelectionActivity::rebuildNetworkRowItems() {
  networkStatuses.assign(networks.size(), std::string());
  networkRowItems.clear();
  networkRowItems.reserve(networks.size());
  for (size_t i = 0; i < networks.size(); i++) {
    const auto& network = networks[i];
    if (!network.isHiddenPlaceholder) {
      networkStatuses[i] = std::string(network.hasSavedPassword ? "+ " : "") + (network.isEncrypted ? "* " : "") +
                           getSignalStrengthIndicator(network.rssi);
    }
    fui::ListItem item;
    item.label = network.isHiddenPlaceholder ? tr(STR_ADD_HIDDEN_NETWORK) : network.ssid.c_str();
    if (!networkStatuses[i].empty()) item.value = networkStatuses[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    networkRowItems.push_back(item);
  }
}

void WifiSelectionActivity::selectNetwork(const int index) {
  if (index < 0 || index >= static_cast<int>(networks.size())) {
    return;
  }

  const auto& network = networks[index];

  // Synthetic "Add hidden network..." entry: prompt the user to type the SSID first
  if (network.isHiddenPlaceholder) {
    promptHiddenSsid();
    return;
  }

  selectedSSID = network.ssid;
  selectedRequiresPassword = network.isEncrypted;
  usedSavedPassword = false;
  enteredPassword.clear();
  autoConnecting = false;

  // Check if we have saved credentials for this network
  const auto savedCred = WIFI_STORE.findCredential(selectedSSID);
  if (savedCred && !savedCred->password.empty()) {
    // Use saved password - connect directly
    enteredPassword = savedCred->password;
    usedSavedPassword = true;
    LOG_DBG("WiFi", "Using saved password for %s, length: %zu", selectedSSID.c_str(), enteredPassword.size());
    attemptConnection();
    return;
  }

  if (selectedRequiresPassword) {
    promptPasswordEntry();
  } else {
    // Connect directly for open networks
    attemptConnection();
  }
}

void WifiSelectionActivity::promptPasswordEntry() {
  // Show password entry
  state = WifiSelectionState::PASSWORD_ENTRY;
  // Don't allow screen updates while changing activity
  // Name the network on the prompt: the Wi-Fi list is behind the keyboard, so without this the
  // screen never says which AP the password belongs to.
  char pwTitle[96];
  snprintf(pwTitle, sizeof(pwTitle), "%s: %s", tr(STR_NETWORK_PREFIX), selectedSSID.c_str());
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, pwTitle,
                                                                 "",  // No initial text
                                                                 64,  // Max password length
                                                                 InputType::Text),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             state = WifiSelectionState::NETWORK_LIST;
                           } else {
                             enteredPassword = std::get<KeyboardResult>(result.data).text;
                             // state will be updated in next loop iteration
                           }
                         });
}

void WifiSelectionActivity::promptHiddenSsid() {
  selectedSSID.clear();
  selectedRequiresPassword = true;  // Hidden networks are usually encrypted; empty password still joins open APs
  usedSavedPassword = false;
  enteredPassword.clear();
  autoConnecting = false;

  // Suppress rendering during the activity transition (see render()).
  state = WifiSelectionState::HIDDEN_SSID_ENTRY;
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ENTER_WIFI_SSID),
                                                                 "",  // No initial text
                                                                 32,  // Max SSID length (IEEE 802.11: 32 bytes)
                                                                 InputType::Text),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             state = WifiSelectionState::NETWORK_LIST;
                             return;
                           }
                           selectedSSID = std::get<KeyboardResult>(result.data).text;
                           if (selectedSSID.empty()) {
                             state = WifiSelectionState::NETWORK_LIST;
                           }
                           // Otherwise stay in HIDDEN_SSID_ENTRY; loop() continues the flow.
                         });
}

bool WifiSelectionActivity::hasAttemptedAutoSsid(const std::string& ssid) const {
  return std::find(autoAttemptedSsids.begin(), autoAttemptedSsids.end(), ssid) != autoAttemptedSsids.end();
}

bool WifiSelectionActivity::tryAutoConnectCredential(const WifiCredential& cred) {
  if (hasAttemptedAutoSsid(cred.ssid)) {
    return false;
  }

  LOG_DBG("WIFI", "Attempting saved network: %s", cred.ssid.c_str());
  autoAttemptedSsids.push_back(cred.ssid);
  selectedSSID = cred.ssid;
  enteredPassword = cred.password;
  selectedRequiresPassword = !cred.password.empty();
  usedSavedPassword = true;
  autoConnecting = true;
  manualNetworkListRequested = false;
  // No requestUpdate() after this: attemptConnection() ends with requestUpdateAndWait(), so the
  // CONNECTING page for this SSID is already on the glass. Asking again re-painted the identical
  // frame -- one extra whole-panel drive for every saved network the ladder tried, in a burst the
  // user reads as the screen flickering twice before the join even starts.
  attemptConnection();
  return true;
}

bool WifiSelectionActivity::tryNextSavedNetworkFromScan() {
  for (const auto& network : networks) {
    if (!network.hasSavedPassword || hasAttemptedAutoSsid(network.ssid)) {
      continue;
    }

    const auto cred = WIFI_STORE.findCredential(network.ssid);
    if (cred && tryAutoConnectCredential(*cred)) {
      return true;
    }
  }
  return false;
}

void WifiSelectionActivity::handleAutoConnectFailure() {
  LOG_DBG("WIFI", "Saved network failed: %s", selectedSSID.c_str());
  WiFi.disconnect();

  if (!networks.empty()) {
    if (tryNextSavedNetworkFromScan()) {
      return;
    }
    autoConnecting = false;
    state = WifiSelectionState::NETWORK_LIST;
    selectedNetworkIndex = 0;
    requestFullRefresh();  // A wholesale screen/state change must be FULL (whole-panel drive); HALF is diff-based on this panel and will ghost.
    return;
  }

  startWifiScan(true);
}

void WifiSelectionActivity::showNetworkListFromAutoConnect() {
  LOG_DBG("WIFI", "User requested manual network list");
  WiFi.disconnect();
  autoConnecting = false;
  manualNetworkListRequested = true;

  if (networks.empty()) {
    startWifiScan(false);
    manualNetworkListRequested = true;
    return;
  }

  state = WifiSelectionState::NETWORK_LIST;
  selectedNetworkIndex = 0;
  requestUpdate();
}

void WifiSelectionActivity::attemptConnection() {
  state = autoConnecting ? WifiSelectionState::AUTO_CONNECTING : WifiSelectionState::CONNECTING;
  connectionStartTime = millis();
  joinGraceMs = 0;
  connectedIP.clear();
  connectionError.clear();
  // The status screen replaces the list: a DU/HALF repaint leaves the list (or the previous screen) on the glass.
  requestFullRefresh();  // A wholesale screen/state change must be FULL (whole-panel drive); HALF is diff-based on this panel and will ghost.

  // r47: compose the CONNECTING frame before the blocking station settle + radio bring-up below;
  // otherwise this activity's first frame is not built until this function returns, and nothing can
  // paint during the ~1.5 s preamble.
  requestUpdateAndWait();

  WiFi.persistent(false);  // Credentials are managed by WifiCredentialStore; suppress SDK NVS auto-connect
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);  // Abort any in-progress SDK auto-connect and clear NVS-saved SSID
  // Wait for the station to actually leave the old association. The auto-connect path reaches this
  // function immediately after a scan completes, so a fixed 100 ms was not always enough and the
  // following config write returned ESP_ERR_WIFI_STATE.
  {
    const unsigned long settleDeadline = millis() + 1500;
    const unsigned long settleStart = millis();
    while (WiFi.status() != WL_DISCONNECTED && millis() < settleDeadline) delay(10);
    LOG_DBG("WIFI", "station settled in %lums (status=%d)", millis() - settleStart,
            static_cast<int>(WiFi.status()));
  }

  // Fast scan: stop at the first AP advertising the target SSID instead of walking every channel
  // (~1.5-4 s on 2.4 GHz). The old all-channel scan existed to pick the strongest BSSID in a
  // multi-AP network; wificonnect::remember() now caches the channel and BSSID of the AP we actually
  // joined, so repeat joins are directed rather than scanned at all.
  WiFi.setScanMethod(WIFI_FAST_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  // Set hostname so routers show "CrossPoint-Reader-AABBCCDDEEFF" instead of "esp32-XXXXXXXXXXXX"
  uint8_t mac[6] = {};
  const esp_err_t macResult = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (macResult == ESP_OK) {
    char hostname[sizeof("CrossPoint-Reader-") + 12];
    snprintf(hostname, sizeof(hostname), "CrossPoint-Reader-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    WiFi.setHostname(hostname);
  } else {
    LOG_ERR("WIFI", "Failed to read station MAC for hostname (err=%d)", static_cast<int>(macResult));
  }

  // Via the helper, not WiFi.begin(): Arduino's station config never sets sae_pwe_h2e, so a WPA3 AP
  // that mandates Hash-to-Element never completes the SAE handshake (see util/WifiConnect.h).
  if (selectedRequiresPassword && !enteredPassword.empty()) {
    wificonnect::begin(selectedSSID.c_str(), enteredPassword.c_str());
  } else {
    wificonnect::begin(selectedSSID.c_str(), nullptr);
  }
}

void WifiSelectionActivity::checkConnectionStatus() {
  if (state != WifiSelectionState::CONNECTING && state != WifiSelectionState::AUTO_CONNECTING) {
    return;
  }

  const wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    wificonnect::remember();  // cache channel+BSSID so the next join skips the scan
    // Successfully connected
    IPAddress ip = WiFi.localIP();
    char ipStr[16];
    snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    connectedIP = ipStr;
    autoConnecting = false;

#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
    uint8_t connectedBssid[6] = {};
    WiFi.BSSID(connectedBssid);
    LOG_DBG("WIFI", "Connected BSSID: %02x:%02x:%02x:%02x:%02x:%02x, channel: %d, RSSI: %d dBm",
            static_cast<unsigned>(connectedBssid[0]), static_cast<unsigned>(connectedBssid[1]),
            static_cast<unsigned>(connectedBssid[2]), static_cast<unsigned>(connectedBssid[3]),
            static_cast<unsigned>(connectedBssid[4]), static_cast<unsigned>(connectedBssid[5]), WiFi.channel(),
            WiFi.RSSI());
#endif

    // Sync RTC from NTP on the first successful WiFi connection only. The DS3231
    // drifts ~2 ppm so one sync is enough; users can force a re-sync from
    // Settings > System > Clock > Sync clock now.
    if (halClock.isAvailable() && !SETTINGS.clockHasBeenSynced) {
      const bool synced = halClock.syncFromNTP();
      LOG_INF("CLK", "post-connect NTP sync returned %d", static_cast<int>(synced));
      if (synced) {
        SETTINGS.clockHasBeenSynced = 1;
        SETTINGS.saveToFile();
      }
    }

    // Every station join is a chance to snap the loan-clock floor to real
    // time (non-blocking; see TrustedTime).
    trustedtime::startSync();

    // Save this as the last connected network - SD card operations need lock as
    // we use SPI for both
    {
      RenderLock lock(*this);
      WIFI_STORE.setLastConnectedSsid(selectedSSID);
    }

    // Every station join is a window where plugin senders are deliverable, so
    // drain the plugin outboxes here (web server up and sleep entry are the
    // other such moments). Cheap no-op when nothing is queued.
    pluginevents::drain(&renderer);

    // If we entered a new password, ask if user wants to save it
    // Otherwise, immediately complete so parent can start web server
    if (!usedSavedPassword && !enteredPassword.empty()) {
      state = WifiSelectionState::SAVE_PROMPT;
      savePromptSelection = 0;  // Default to "Yes"
      requestUpdate();
    } else {
      // r89: this branch used to call onComplete() directly, and onComplete() is finish(), so the
      // activity was destroyed inside the same loop pass that learned the join succeeded -- before
      // any frame of the outcome could be composed. WifiSelectionState::CONNECTED has never been
      // assigned anywhere in this tree (grep: zero assignments), so renderConnected() was dead code
      // and the "Connected" page the operator expects could not appear on ANY path. Compose and
      // commit it here, with a whole-panel drive, and then complete.
      LOG_DBG("WIFI", "Connected with saved/open credentials, completing after the CONNECTED frame");
      state = WifiSelectionState::CONNECTED;
      requestFullRefresh();
      requestUpdateAndWait();
      onComplete(true);
    }
    return;
  }

  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    connectionError = tr(STR_ERROR_GENERAL_FAILURE);
    if (status == WL_NO_SSID_AVAIL) {
      connectionError = tr(STR_ERROR_NETWORK_NOT_FOUND);
    }
    if (autoConnecting) {
      handleAutoConnectFailure();
      return;
    }
    state = WifiSelectionState::CONNECTION_FAILED;
    requestFullRefresh();  // A wholesale screen/state change must be FULL (whole-panel drive); HALF is diff-based on this panel and will ghost.
    return;
  }

  // Check for timeout. The station can sit at WL_IDLE_STATUS while the supplicant keeps retrying
  // (SAE anti-clogging, 4-way retries) and never emit a disconnect reason before our deadline --
  // which is why this path could time out with an empty log. Grant one grace extension when the
  // station is demonstrably still working, and log what was actually true at the deadline.
  const unsigned long baseTimeoutMs = autoConnecting ? AUTO_CONNECTION_TIMEOUT_MS : CONNECTION_TIMEOUT_MS;
  const unsigned long timeoutMs = baseTimeoutMs + joinGraceMs;
  if (millis() - connectionStartTime > timeoutMs) {
    const wl_status_t atDeadline = WiFi.status();
    LOG_INF("WIFI", "join timed out after %lums (status=%d %s, cached_ap=%s, sae_grace=%lu)",
            millis() - connectionStartTime, static_cast<int>(atDeadline),
            atDeadline == WL_IDLE_STATUS ? "idle/still-trying" : "not-idle",
            wificonnect::hasCachedAp(selectedSSID.c_str()) ? "yes" : "no", joinGraceMs);

    if (joinGraceMs == 0 && atDeadline == WL_IDLE_STATUS) {
      joinGraceMs = JOIN_GRACE_MS;
      LOG_INF("WIFI", "station still working at the deadline; extending by %lums", JOIN_GRACE_MS);
      return;
    }

    // Only drop the cached BSSID when the station reported a real failure. A join that was still
    // progressing is not evidence that the cache is wrong, and forgetting it costs a rescan.
    if (atDeadline != WL_IDLE_STATUS) wificonnect::forgetAp();
    WiFi.disconnect();
    connectionError = tr(STR_ERROR_CONNECTION_TIMEOUT);
    if (autoConnecting) {
      handleAutoConnectFailure();
      return;
    }
    state = WifiSelectionState::CONNECTION_FAILED;
    requestFullRefresh();  // A wholesale screen/state change must be FULL (whole-panel drive); HALF is diff-based on this panel and will ghost.
    return;
  }
}

void WifiSelectionActivity::loop() {
  // Check scan progress
  if (state == WifiSelectionState::SCANNING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      WiFi.scanDelete();
      onComplete(false);
      return;
    }
    if (autoConnecting && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      autoConnecting = false;
      manualNetworkListRequested = true;
      requestUpdate();
    }
    processWifiScanResults();
    return;
  }

  // Check connection progress
  if (state == WifiSelectionState::CONNECTING || state == WifiSelectionState::AUTO_CONNECTING) {
    if (state == WifiSelectionState::AUTO_CONNECTING) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        WiFi.disconnect();
        onComplete(false);
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        showNetworkListFromAutoConnect();
        return;
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      // CONNECTING (a network picked on THIS screen): Back is one level -- abandon the attempt and
      // return to the list it was started from. It used to be a dead tap: the branch fell straight
      // through to checkConnectionStatus() and the only way out was to wait for the join to fail,
      // which a station making slow progress ("still working at the deadline", extended once) can
      // stretch into tens of seconds. Release-edged like every other Back on this screen, so a HOLD
      // still reaches Home through ActivityManager's global hold instead of being eaten here.
      WiFi.disconnect();
      connectionError.clear();
      state = WifiSelectionState::NETWORK_LIST;
      requestFullRefresh();  // A wholesale screen/state change must be FULL (whole-panel drive); HALF is diff-based on this panel and will ghost.
      return;
    }
    checkConnectionStatus();
    return;
  }

  // Reached once the hidden-network SSID has been entered (and was non-empty).
  if (state == WifiSelectionState::HIDDEN_SSID_ENTRY) {
    const auto savedCred = WIFI_STORE.findCredential(selectedSSID);
    if (savedCred && !savedCred->password.empty()) {
      // We already know this hidden network - connect with the saved password
      enteredPassword = savedCred->password;
      usedSavedPassword = true;
      LOG_DBG("WiFi", "Using saved password for hidden network %s", selectedSSID.c_str());
      attemptConnection();
    } else {
      // Prompt for the password (empty password connects to open hidden APs)
      promptPasswordEntry();
    }
    return;
  }

  if (state == WifiSelectionState::PASSWORD_ENTRY) {
    // Reach here once password entry finished in subactivity
    attemptConnection();
    return;
  }

  // Handle save prompt state
  if (state == WifiSelectionState::SAVE_PROMPT) {
    // Touch goes through the FreeInkApp: render() registered the dialog
    // button hit rects; route the snapshot and let onPromptEvent dispatch.
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onPromptEvent

    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (savePromptSelection > 0) {
        savePromptSelection--;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (savePromptSelection < 1) {
        savePromptSelection++;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (savePromptSelection == 0) {
        // User chose "Yes" - save the password
        RenderLock lock(*this);
        WIFI_STORE.addCredential(selectedSSID, enteredPassword);
      }
      // Complete - parent will start web server
      onComplete(true);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      // Skip saving, complete anyway
      onComplete(true);
    }
    return;
  }

  // Handle forget prompt state (connection failed with saved credentials)
  if (state == WifiSelectionState::FORGET_PROMPT) {
    // Touch goes through the FreeInkApp: render() registered the dialog
    // button hit rects; route the snapshot and let onPromptEvent dispatch.
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onPromptEvent

    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (forgetPromptSelection > 0) {
        forgetPromptSelection--;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (forgetPromptSelection < 1) {
        forgetPromptSelection++;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (forgetPromptSelection == 1) {
        RenderLock lock(*this);
        // User chose "Forget network" - forget the network
        WIFI_STORE.removeCredential(selectedSSID);
        // Update the network list to reflect the change
        const auto network = find_if(networks.begin(), networks.end(),
                                     [this](const WifiNetworkInfo& net) { return net.ssid == selectedSSID; });
        if (network != networks.end()) {
          network->hasSavedPassword = false;
        }
      }
      // Go back to network list (whether Cancel or Forget network was selected)
      startWifiScan();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      // Skip forgetting, go back to network list
      startWifiScan();
    }
    return;
  }

  // Handle connected state (should not normally be reached - connection
  // completes immediately)
  if (state == WifiSelectionState::CONNECTED) {
    // Safety fallback - immediately complete
    onComplete(true);
    return;
  }

  // Handle connection failed state
  if (state == WifiSelectionState::CONNECTION_FAILED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // If we were auto-connecting or using a saved credential, offer to forget
      // the network
      if (autoConnecting || usedSavedPassword) {
        autoConnecting = false;
        state = WifiSelectionState::FORGET_PROMPT;
        forgetPromptSelection = 0;  // Default to "Cancel"
      } else {
        // Go back to network list on failure for non-saved credentials
        state = WifiSelectionState::NETWORK_LIST;
      }
      requestUpdate();
      return;
    }
  }

  // Handle network list state
  if (state == WifiSelectionState::NETWORK_LIST) {
    if (manualNetworkListRequested) {
      if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
        manualNetworkListRequested = false;
      }
      return;
    }

    // Check for Back button to exit (cancel)
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onComplete(false);
      return;
    }

    // Check for Confirm button to select network or rescan
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!networks.empty()) {
        selectNetwork(selectedNetworkIndex);
      } else {
        startWifiScan();
      }
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      startWifiScan();
      return;
    }

    const bool leftPressed = mappedInput.wasPressed(MappedInputManager::Button::Left);
    if (leftPressed) {
      const bool hasSavedPassword = !networks.empty() && networks[selectedNetworkIndex].hasSavedPassword;
      if (hasSavedPassword) {
        selectedSSID = networks[selectedNetworkIndex].ssid;
        state = WifiSelectionState::FORGET_PROMPT;
        forgetPromptSelection = 0;  // Default to "Cancel"
        requestUpdate();
        return;
      }
    }

    // Touch goes through the FreeInkApp: render() registered the row hit
    // rects; route the snapshot and let onRowEvent dispatch. Long-press on a
    // network row fires "forget" while the finger is down.
    const auto route = routeTouch(mappedInput, /*withLongPress=*/true);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onRowEvent

    if (!networks.empty()) {
      // Swipes scroll the viewport; the selection stays put and button
      // navigation pulls the view back to it.
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
        const int delta = swipe == MappedInputManager::SwipeDir::Up ? listNav.visibleRows : -listNav.visibleRows;
        if (listNav.scrollBy(delta, static_cast<int>(networks.size()))) requestUpdate();
        return;
      }
    }

    const auto moveSelection = [this](const int index) {
      selectedNetworkIndex = static_cast<size_t>(index);
      listNav.selected = index;
      listNav.follow(static_cast<int>(networks.size()));
      requestUpdate();
    };
    buttonNavigator.onNext(
        [this, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectedNetworkIndex, networks.size())); });
    buttonNavigator.onPrevious([this, &moveSelection] {
      moveSelection(ButtonNavigator::previousIndex(selectedNetworkIndex, networks.size()));
    });
  }
}

std::string WifiSelectionActivity::getSignalStrengthIndicator(const int32_t rssi) {
  // Convert RSSI to signal bars representation
  if (rssi >= -50) {
    return "||||";  // Excellent
  }
  if (rssi >= -60) {
    return " |||";  // Good
  }
  if (rssi >= -70) {
    return "  ||";  // Fair
  }
  return "   |";  // Very weak
}

void WifiSelectionActivity::render(RenderLock&&) {
  // Don't render if we're in a keyboard-entry state - we're just transitioning
  // from the keyboard subactivity back to the main activity
  if (state == WifiSelectionState::PASSWORD_ENTRY || state == WifiSelectionState::HIDDEN_SSID_ENTRY) {
    return;
  }

  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  // Draw header
  // STR_NETWORKS_FOUND is ~37 bytes once the Arabic translation is substituted,
  // so 32 truncated it. See ClockSyncActivity for the same class of bug.
  char countStr[64];
  snprintf(countStr, sizeof(countStr), tr(STR_NETWORKS_FOUND), realNetworkCount);
  // drawHeader self-insets by the board's viewable margins, so it takes the
  // full-width rect at metrics.topPadding that every other caller uses; the
  // safe-area `screen` would inset it twice. r77: the THEME's titled band (the
  // doubled app bar + 6 px rule on Andro), and the sub-header starts under it.
  const int headerH = UITheme::getInstance().getTitledHeaderHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), headerH}, tr(STR_WIFI_NETWORKS),
                 countStr);
  GUI.drawSubHeader(renderer, Rect{screen.x, metrics.topPadding + headerH, screen.width, metrics.tabBarHeight},
                    cachedMacAddress.c_str());

  switch (state) {
    case WifiSelectionState::AUTO_CONNECTING:
      renderConnecting(&screen, &metrics);
      break;
    case WifiSelectionState::SCANNING:
      renderConnecting(&screen, &metrics);  // Reuse connecting screen with different message
      break;
    case WifiSelectionState::NETWORK_LIST:
      renderNetworkList(&screen, &metrics);
      break;
    case WifiSelectionState::HIDDEN_SSID_ENTRY:
      // Transitioning to/from the SSID keyboard subactivity - nothing to draw
      break;
    case WifiSelectionState::CONNECTING:
      renderConnecting(&screen, &metrics);
      break;
    case WifiSelectionState::CONNECTED:
      renderConnected(&screen, &metrics);
      break;
    case WifiSelectionState::SAVE_PROMPT:
    case WifiSelectionState::FORGET_PROMPT: {
      // The app's screen builder draws the option dialog panel itself.
      renderUi();
      const auto labels =
          mappedInput.mapLabels(state == WifiSelectionState::SAVE_PROMPT ? tr(STR_CANCEL) : tr(STR_BACK),
                                tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case WifiSelectionState::CONNECTION_FAILED:
      renderConnectionFailed(&screen, &metrics);
      break;
  }

  // r47: requestFullRefresh()'s one-shot promotion is global -- a paint landing between the promote
  // (from onEnter) and this frame consumes it, and this frame then falls back to displayBuffer()'s
  // default FAST_REFRESH = MODE_DU, a diff-area drive that cannot replace the screen underneath.
  // Assert the mode in the same render() call that consumes it.
  //
  // r80: the state LIST below was the bug. These frames replace the whole page -- a dialog card on a
  // blank field where a progress line was -- and DU only drives the pixels that changed, so the
  // frame that follows a join landed on the previous state's ink. That is exactly what the
  // "connected" page looked like: the confirmation dialog (SAVE_PROMPT, titled with STR_CONNECTED)
  // painted onto a stale connect screen or stayed blank. CONNECTING / AUTO_CONNECTING /
  // CONNECTION_FAILED were listed by hand and SAVE_PROMPT -- the one screen every first join
  // shows -- was not. One rule now instead of a list of states: a frame whose STATE differs from
  // the last committed frame is a wholesale replacement, so it is committed FULL.
  //
  // r165 WIFI1: and the rule is the ONLY term. The five-state list that survived r80 re-promoted FULL
  // for every repaint that happened to be in one of those states, unchanged frame or not: the
  // auto-connect ladder's retry, and each Left/Right in the save prompt, flashed the whole panel white
  // for pixels that a diff would have driven. A page POSED by re-entering the same state is not a
  // wholesale replacement; the explicit arms on the real transitions (attemptConnection,
  // handleAutoConnectFailure, processWifiScanResults, the success/failure paths) still carry FULL.
  const bool stateChanged = lastRenderedState != static_cast<int>(state);
  const int previousState = lastRenderedState;
  lastRenderedState = static_cast<int>(state);
  bool promoteFull = false;
  if (stateChanged) {
    renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
    promoteFull = true;
  }
  // r89: the state pair is the one thing the GFX paint line cannot show, and the previous diagnosis
  // went wrong precisely by guessing at it. "paint state=<new> prev=<committed> full=<1 if this frame
  // asked for a from-white drive>" is the line the next capture needs; GFX's own line immediately
  // after it carries the mode actually resolved and whether the rails were up under it.
  LOG_DBG("WIFI", "paint state=%d prev=%d full=%d", static_cast<int>(state), previousState, promoteFull ? 1 : 0);
  renderer.displayBuffer();
}

void WifiSelectionActivity::listScreen(UiScreen& screen, void* user) {
  static_cast<WifiSelectionActivity*>(user)->buildListScreen(screen);
}

void WifiSelectionActivity::buildListScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content below the header + MAC sub-band, above the legend line.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + UITheme::getInstance().getTitledHeaderHeight() +
                           metrics.tabBarHeight + metrics.verticalSpacing),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.verticalSpacing * 2),
      static_cast<int16_t>(safe.x)});

  if (state == WifiSelectionState::SAVE_PROMPT || state == WifiSelectionState::FORGET_PROMPT) {
    buildPromptDialog(screen);
    return;
  }

  if (networks.empty()) {
    screen.centeredText(tr(STR_NO_NETWORKS), screen.theme().bodyText);
    if (mappedInput.hasTouch()) {
      // Touch has no OK button to rescan with; offer the retry on screen instead
      // of the "Press OK" hint renderNetworkList draws for button boards.
      const auto& theme = screen.theme();
      const fui::Rect body = screen.body();
      const int16_t buttonWidth = static_cast<int16_t>(body.width / 2);
      const fui::Rect buttonRect{static_cast<int16_t>(body.x + (body.width - buttonWidth) / 2),
                                 static_cast<int16_t>(body.y + body.height * 2 / 3), buttonWidth, theme.rowHeight};
      fui::ButtonProps scan;
      scan.label = tr(STR_RETRY);
      scan.action = ACTION_SCAN;
      scan.inputMask = fui::InputTouch;
      scan.text = theme.bodyText;
      fui::button(screen.frame(), buttonRect, scan);
    }
    return;
  }

  // networkStatuses/networkRowItems are built once per processWifiScanResults()
  // call (see rebuildNetworkRowItems()) and reused here on every repaint.
  fui::ListProps props;
  props.items = networkRowItems.data();
  props.count = static_cast<uint16_t>(networkRowItems.size());
  props.action = ACTION_ROW;
  // Tap opens; long-press a saved network forgets it (physical buttons stay in loop()).
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.valueInset = 8;  // air between the signal bars and the row edge
  // Long SSIDs grow their row to a second line; the trailing value is
  // just the short status glyphs, so skip the balanced 60%-band wrap cap.
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.balanceWrappedLabelWithValue = false;
  // ANDRO4: this screen composes its own ListProps, so it never reached
  // UiListActivity::applyThemeListChrome() and drew the network rows in the UI body
  // face at the SDK's pitch while every list one level up (Settings > Network) wore
  // the theme's row face. Same knobs and same guard as ButtonRemapActivity: the row
  // face is used only when it carries every SSID's codepoints (SSIDs are arbitrary
  // UTF-8, and the Ember ids have no registered fallback), and the pitch is the
  // theme's listRowFillPitch(). A theme that states no row font (every theme but
  // Andro) gets exactly the props this screen built before.
  applyThemeRowChrome(renderer, uiTarget, UITheme::getInstance().getTheme(), networkRowItems.data(),
                      static_cast<int>(networkRowItems.size()), props, screen.body().height);
  listNav.selected = static_cast<int>(selectedNetworkIndex);
  props.partialTrailingRow = true;
  screen.syncListViewport(listNav, props, static_cast<int>(networks.size()));
  screen.list(props);
}

void WifiSelectionActivity::buildPromptDialog(UiScreen& screen) {
  const bool isForget = state == WifiSelectionState::FORGET_PROMPT;

  // Owned for the duration of the draw; the dialog wraps long SSIDs itself.
  const std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID;

  const int selection = isForget ? forgetPromptSelection : savePromptSelection;
  fui::DialogOption options[2];
  options[0].label = isForget ? tr(STR_CANCEL) : tr(STR_YES);
  options[1].label = isForget ? tr(STR_FORGET_NETWORK) : tr(STR_NO);
  for (int i = 0; i < 2; i++) {
    options[i].action = ACTION_PROMPT;
    options[i].value = static_cast<int16_t>(i);
    options[i].state = selection == i ? fui::StateFocused : fui::StateNormal;
  }

  fui::OptionDialogProps props;
  props.title = isForget ? tr(STR_FORGET_NETWORK) : tr(STR_CONNECTED);
  props.headline = ssidInfo.c_str();
  props.message = isForget ? tr(STR_FORGET_AND_REMOVE) : tr(STR_SAVE_PASSWORD);
  props.options = options;
  props.optionCount = 2;
  // Stacked full-width options (matching OptionPopup): side-by-side halves
  // truncate the long "Forget network" label.
  props.verticalOptions = true;
  props.titleText = screen.theme().smallText;
  props.titleText.bold = true;
  // TextStyle defaults to maxLines=1 (ellipsis truncation); let the SSID
  // headline and the question wrap. optionDialogHeight measures with the same
  // styles, so the dialog grows to fit the wrapped lines.
  props.headlineText = screen.theme().bodyText;
  props.headlineText.maxLines = 2;
  props.messageText = screen.theme().smallText;
  props.messageText.maxLines = 3;
  props.buttonText = screen.theme().smallText;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  // defaultPopupStyles() (fui::optionDialog's fallback when styles is left
  // unset) has no border; opt one in explicitly using the theme's popup frame
  // metrics, matching OptionPopup::render().
  const auto& metrics = UITheme::getInstance().getMetrics();
  props.styles = fui::defaultPopupStyles();
  props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
  props.styles.normal.borderWidth = static_cast<uint8_t>(metrics.popupFrameThickness);
  props.styles.normal.radius = static_cast<uint8_t>(metrics.popupCornerRadius);
  props.styles.selected = props.styles.normal;
  props.styles.focused = props.styles.normal;
  props.styles.active = props.styles.normal;
  props.styles.disabled = props.styles.normal;

  const fui::Rect body = screen.body();
  int16_t width = static_cast<int16_t>(renderer.getScreenWidth() * 3 / 4);
  if (width > body.width) width = body.width;
  const int16_t height = fui::optionDialogHeight(screen.target(), props, width);
  fui::optionDialog(screen.frame(), fui::centeredRect(body, fui::Size{width, height}), props);
}

void WifiSelectionActivity::renderNetworkList(const Rect* screen, const ThemeMetrics* metrics) {
  renderUi();
  if (networks.empty() && !mappedInput.hasTouch()) {
    // Below the centered "no networks" line the app drew. Touch boards get an
    // on-screen Retry button from the screen builder instead of this hint.
    const auto height = renderer.getLineHeight(UI_10_FONT_ID);
    const auto top = screen->y + (screen->height - height) / 2;
    UITheme::drawCenteredText(renderer, *screen, SMALL_FONT_ID, top + height + 10, tr(STR_PRESS_OK_SCAN));
  }

  GUI.drawHelpText(renderer,
                   Rect{screen->x, screen->y + screen->height - metrics->contentSidePadding - 15, screen->width, 20},
                   tr(STR_NETWORK_LEGEND));

  const bool hasSavedPassword = !networks.empty() && networks[selectedNetworkIndex].hasSavedPassword;
  const char* forgetLabel = hasSavedPassword ? tr(STR_FORGET_BUTTON) : "";

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONNECT), forgetLabel, tr(STR_RETRY));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnecting(const Rect* screen, const ThemeMetrics* metrics) const {
  constexpr int MAX_STATUS_LINES = 2;
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height) / 2;
  const int statusX = screen->x + metrics->contentSidePadding;
  const int statusWidth = screen->width - metrics->contentSidePadding * 2;

  if (state == WifiSelectionState::SCANNING) {
    const char* statusText = autoConnecting ? tr(STR_FINDING_SAVED_WIFI) : tr(STR_SCANNING);
    const Rect statusBounds{statusX, screen->y, statusWidth, screen->height};
    UITheme::drawCenteredWrappedText(renderer, statusBounds, UI_10_FONT_ID, statusText, MAX_STATUS_LINES);
    if (autoConnecting) {
      const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SHOW_NETWORKS), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else {
    const char* statusText = autoConnecting ? tr(STR_CONNECTING_SAVED_WIFI) : tr(STR_CONNECTING);
    const Rect statusBounds{statusX, screen->y, statusWidth, top - metrics->verticalSpacing - screen->y};
    UITheme::drawCenteredWrappedText(renderer, statusBounds, UI_12_FONT_ID, statusText, MAX_STATUS_LINES, true,
                                     EpdFontFamily::BOLD, UITheme::TextVerticalAlignment::BOTTOM);

    const std::string ssidInfo =
        renderer.truncatedText(UI_10_FONT_ID, (std::string(tr(STR_TO_PREFIX)) + selectedSSID).c_str(), statusWidth);
    UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, ssidInfo.c_str());
    if (autoConnecting) {
      const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SHOW_NETWORKS), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  }
}

void WifiSelectionActivity::renderConnected(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 4) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 30, tr(STR_CONNECTED), true, EpdFontFamily::BOLD);

  const std::string ssidInfo =
      renderer.truncatedText(UI_10_FONT_ID, (std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID).c_str(),
                             screen->width - metrics->contentSidePadding * 2);
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 10, ssidInfo.c_str());

  const std::string ipInfo = std::string(tr(STR_IP_ADDRESS_PREFIX)) + connectedIP;
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 40, ipInfo.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels("", tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnectionFailed(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 2) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 20, tr(STR_CONNECTION_FAILED), true,
                            EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 20, connectionError.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::onComplete(const bool connected) {
  ActivityResult result;
  result.isCancelled = !connected;
  if (connected) {
    result.data = WifiResult{true, selectedSSID, connectedIP};
  }
  setResult(std::move(result));
  // r89: the promotion has to OUTLIVE this activity. finish() only queues a pop; the parent's next
  // frame is then painted by ActivityManager's post-pop requestUpdate() (ActivityManager.cpp:243),
  // which arms nothing -- so the incoming screen was laid down with whatever the transition policy
  // resolves to, i.e. a diff (GC16 over the change mask at best, DU at worst) on top of a full-screen
  // Wi-Fi page. epdiy drives only the masked columns/lines and then commits back_fb as if the whole
  // frame landed (highlevel.c:240-282), so the Wi-Fi page's ink survives as light-grey ghost text at
  // its original positions, with the mask edges showing as vertical bands. The renderer's one-shot
  // promotion is exactly the channel that survives an activity swap (BaseTheme::drawPopup relies on
  // the same property, BaseTheme.cpp:801-808), so arm it here.
  renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
  LOG_DBG("WIFI", "completing (connected=%d); armed FULL for the incoming frame", connected ? 1 : 0);
  finish();
}
