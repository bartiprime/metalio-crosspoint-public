#include "WifiConnect.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <atomic>
#include <cstring>
#include <esp_timer.h>

namespace wificonnect {

namespace {

// Last AP we successfully joined. Reusing its channel and BSSID turns the next connect into a
// directed join: the station transmits on one channel instead of scanning every channel for the
// SSID. Cleared implicitly on reboot; a miss just means we scan once more.
struct ApCache {
  char ssid[33] = {0};
  uint8_t channel = 0;
  uint8_t bssid[6] = {0};
  unsigned long capturedMs = 0;
  bool valid = false;
} g_cache;

// A cached BSSID is only good while the AP stays put. Ten minutes is long enough to make repeat
// joins in one session instant, short enough that a rebooted or roamed AP is rediscovered rather
// than aimed at forever (which presented as "connection timed out" on networks that used to work).
constexpr unsigned long kCacheMaxAgeMs = 10UL * 60UL * 1000UL;

bool sameSsid(const char* a, const char* b) { return a != nullptr && b != nullptr && strncmp(a, b, 32) == 0; }


// Per-SSID SAE PWE mode, learned on the metal instead of guessed at build time: 0 = try both
// (driver default), 1 = Hash-to-Element only, 2 = hunt-and-peck only. Some APs accept only one of
// the two and simply never answer the other, which the station reports as an auth timeout. Memory
// is per boot; a wrong guess only costs one extra attempt.
struct SaePref {
  char ssid[33] = {0};
  uint8_t mode = 0;
};
constexpr int kMaxSaePrefs = 8;
SaePref g_saePrefs[kMaxSaePrefs];
int g_saePrefCount = 0;

// After an auth-class failure the AP may still be holding us off (SAE anti-clogging, or a temporary
// blacklist for a station that retried instantly). One and a half seconds turns "fails twice, works
// on the third" into "fails once, works on the second".
constexpr unsigned long kAuthBackoffMs = 1500;
unsigned long g_notBeforeMs = 0;

// ---- auto-reconnect ladder (r44) ------------------------------------------------------------------
// The vendor reconnects on EVERY STA_DISCONNECTED -- five immediate attempts, then a rescan round on
// a timer, repeating -- and never waits for a UI action. Ours acted only on auth-class reasons, so a
// beacon timeout (200) or NO_AP_FOUND (201) left the station down until the user tapped the network
// again. Bounded rather than infinite (the vendor loops forever): an unattended e-reader must not
// spend a battery retrying an AP that is gone. WIFI_REASON_ASSOC_LEAVE (8) is the driver's signal
// for a disconnect WE asked for, which is how the ten deliberate WiFi.disconnect() sites stay quiet
// without touching any of them.
constexpr int kMaxImmediateReconnects = 5;
constexpr int kMaxReconnectRounds = 6;              // 6 x 10 s after the immediate burst
constexpr unsigned long kReconnectRoundMs = 10000;
std::atomic<int> g_reconnectAttempts{0};
std::atomic<int> g_reconnectRounds{0};
std::atomic<bool> g_reconnectWanted{false};
esp_timer_handle_t g_reconnectTimer = nullptr;

void reconnectTimerCb(void*) {
  if (!g_reconnectWanted.load()) return;
  LOG_INF("WIFI", "reconnect round %d/%d", g_reconnectRounds.load(), kMaxReconnectRounds);
  g_reconnectAttempts.store(0);
  esp_wifi_connect();  // the station config has bssid_set=false, so this rescans by construction
}

void scheduleReconnectRound() {
  if (!g_reconnectWanted.load()) return;
  if (g_reconnectRounds.fetch_add(1) + 1 > kMaxReconnectRounds) {
    LOG_INF("WIFI", "giving up after %d reconnect rounds; a manual connect starts over", kMaxReconnectRounds);
    g_reconnectWanted.store(false);
    return;
  }
  if (g_reconnectTimer == nullptr) {
    esp_timer_create_args_t args{};
    args.callback = reconnectTimerCb;
    args.name = "wifi_reconnect";
    if (esp_timer_create(&args, &g_reconnectTimer) != ESP_OK) {
      LOG_ERR("WIFI", "could not create the reconnect timer");
      return;
    }
  }
  esp_timer_stop(g_reconnectTimer);
  esp_timer_start_once(g_reconnectTimer, kReconnectRoundMs * 1000ULL);
}

const char* reasonText(uint8_t reason) {
  switch (reason) {
    case 2:
      return "AUTH_EXPIRE";
    case 8:
      return "ASSOC_LEAVE";
    case 15:
      return "4WAY_HANDSHAKE_TIMEOUT";
    case 200:
      return "BEACON_TIMEOUT";
    case 201:
      return "NO_AP_FOUND";
    case 202:
      return "AUTH_FAIL";
    case 203:
      return "ASSOC_FAIL";
    case 204:
      return "HANDSHAKE_TIMEOUT";
    case 205:
      return "CONNECTION_FAIL";
    default:
      return "other";
  }
}

// Reasons that justify trying the other PWE derivation. 202 (AUTH_FAIL) is deliberately absent: it is
// what a wrong password produces, and letting it escalate pins a mode that may be wrong for this AP
// for the rest of the boot -- a wrong-password attempt would then poison the next correct one.
bool authClassFailure(uint8_t reason) {
  return reason == 2 || reason == 15 || reason == 203 || reason == 204 || reason == 205;
}

uint8_t saeModeFor(const char* ssid) {
  for (int i = 0; i < g_saePrefCount; ++i) {
    if (sameSsid(g_saePrefs[i].ssid, ssid)) return g_saePrefs[i].mode;
  }
  return 0;
}

void setSaeMode(const char* ssid, uint8_t mode) {
  for (int i = 0; i < g_saePrefCount; ++i) {
    if (sameSsid(g_saePrefs[i].ssid, ssid)) {
      g_saePrefs[i].mode = mode;
      return;
    }
  }
  if (g_saePrefCount >= kMaxSaePrefs) return;
  strncpy(g_saePrefs[g_saePrefCount].ssid, ssid, sizeof(g_saePrefs[0].ssid) - 1);
  g_saePrefs[g_saePrefCount].mode = mode;
  ++g_saePrefCount;
}

// SSID of the join currently in flight, so a disconnect event can attribute the failure.
char g_pendingSsid[33] = {0};
uint8_t g_pendingMode = 0;

void wifiEventHandler(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base != WIFI_EVENT || id != WIFI_EVENT_STA_DISCONNECTED) return;
  const auto* d = static_cast<const wifi_event_sta_disconnected_t*>(data);
  LOG_INF("WIFI", "disconnected reason=%u (%s) rssi=%d", static_cast<unsigned>(d->reason),
          reasonText(d->reason), static_cast<int>(d->rssi));

  // No ladder for a disconnect we caused (reason 8) or before we ever joined anything.
  if (!g_reconnectWanted.load() || d->reason == WIFI_REASON_ASSOC_LEAVE || g_pendingSsid[0] == '\0') return;

  if (!authClassFailure(d->reason)) {
    // Link loss we did not ask for: the AP rebooted, roamed, or dropped us. Retry at once, five
    // times, then space the attempts out.
    const int n = g_reconnectAttempts.fetch_add(1) + 1;
    if (n <= kMaxImmediateReconnects) {
      LOG_INF("WIFI", "reconnect %d/%d after reason %s", n, kMaxImmediateReconnects, reasonText(d->reason));
      esp_wifi_connect();
    } else {
      scheduleReconnectRound();
    }
    return;
  }

  g_notBeforeMs = millis() + kAuthBackoffMs;

  // Escalate the PWE mode: both -> H2E -> hunt-and-peck. The next begin() for this SSID picks the
  // new value up, so a retry explores the one derivation the AP did not already reject.
  if (g_pendingMode < 2) {
    const uint8_t next = g_pendingMode + 1;
    setSaeMode(g_pendingSsid, next);
    LOG_INF("WIFI", "auth-class failure on '%s'; next attempt will use sae=%s", g_pendingSsid,
            next == 1 ? "H2E" : "hunt-and-peck");
  } else {
    LOG_INF("WIFI", "'%s' failed with every SAE mode; AP likely rejects SAE outright", g_pendingSsid);
  }
  // Anti-clogging: an AP that just rejected us will reject an instant retry too, so wait a round
  // instead of hammering. The escalation above is picked up by the next begin()/connect.
  scheduleReconnectRound();
}

bool cacheUsable(const char* ssid) {
  return g_cache.valid && sameSsid(g_cache.ssid, ssid) &&
         (millis() - g_cache.capturedMs) < kCacheMaxAgeMs;
}

}  // namespace

bool hasCachedAp(const char* ssid) { return cacheUsable(ssid); }

void forgetAp() {
  if (!g_cache.valid) return;
  LOG_INF("WIFI", "dropping cached AP '%s'; next join will scan", g_cache.ssid);
  g_cache.valid = false;
}

void suspendAutoReconnect() {
  g_reconnectWanted.store(false);
  g_reconnectAttempts.store(0);
  g_reconnectRounds.store(0);
  if (g_reconnectTimer != nullptr) esp_timer_stop(g_reconnectTimer);
}

void remember() {
  wifi_ap_record_t ap{};
  if (WiFi.status() != WL_CONNECTED) return;
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
  strncpy(g_cache.ssid, reinterpret_cast<const char*>(ap.ssid), sizeof(g_cache.ssid) - 1);
  g_cache.channel = ap.primary;
  memcpy(g_cache.bssid, ap.bssid, sizeof(g_cache.bssid));
  g_cache.capturedMs = millis();
  g_cache.valid = true;
  // Telemetry only since r44: joins scan, they are not directed at this BSSID. Do not let the wording
  // imply otherwise -- a stale claim in a log line is as misleading as one in a document.
  LOG_INF("WIFI", "remembered AP '%s' ch=%u (telemetry only; joins scan)", g_cache.ssid, static_cast<unsigned>(g_cache.channel));
  LOG_INF("WIFI", "joined '%s' akm=%d (3=WPA2_PSK 4=WPA2_WPA3_PSK 6=WPA3_PSK) rssi=%d", g_cache.ssid,
          static_cast<int>(ap.authmode), static_cast<int>(ap.rssi));
}

bool begin(const char* ssid, const char* password) {
  if (ssid == nullptr || ssid[0] == '\0') return false;
  const bool hasPassword = password != nullptr && password[0] != '\0';

  // Already joined to this SSID? Re-writing the station config aborts the established link, so leave
  // it alone. This is what made a second activity's "connect" tear down a working connection.
  wifi_ap_record_t current{};
  if (WiFi.status() == WL_CONNECTED && esp_wifi_sta_get_ap_info(&current) == ESP_OK &&
      strncmp(reinterpret_cast<const char*>(current.ssid), ssid, 32) == 0) {
    remember();
    return true;
  }

  // A join needs bignum scratch for the SAE exchange. Below ~40 KB an SAE attempt can fail in a way
  // that is indistinguishable from a rejected password, so make the number visible in the log.
  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_INF("WIFI", "joining '%s' (free heap %u bytes)%s", ssid, static_cast<unsigned>(freeHeap),
          freeHeap < 40000 ? " -- LOW, SAE may fail" : "");

  // Honour the backoff set by a previous auth-class failure.
  if (g_notBeforeMs != 0 && static_cast<long>(millis() - g_notBeforeMs) < 0) {
    const unsigned long waitMs = g_notBeforeMs - millis();
    LOG_INF("WIFI", "backing off %lums before retrying '%s'", waitMs, ssid);
    delay(waitMs);
  }
  g_notBeforeMs = 0;

  // Modem sleep adds latency to the handshake and to the DHCP exchange that follows it. The transfer
  // paths already disable it; do it here so the join itself runs at full rate.
  WiFi.setSleep(false);

  // DIRECTED JOIN REMOVED (r44). Pinning the channel and BSSID of the last successful association
  // skipped the scan, so a roamed / re-booted / mesh-steered AP answered with a beacon timeout
  // (200) or NO_AP_FOUND (201); our handler retried neither, the caller burned the whole budget,
  // and only THEN did the cache get dropped -- so the second attempt scanned and worked. That is the
  // reported "first attempt fails, second succeeds". The vendor never directs a join either
  // (bssid_set=false + WIFI_CONNECT_AP_BY_SIGNAL). The record is kept for telemetry only.
  const uint8_t* bssid = nullptr;
  const uint8_t channel = 0;
  if (cacheUsable(ssid)) {
    g_cache.capturedMs = millis();  // keep the "seen recently" hint fresh for hasCachedAp()
  }

  // A user-initiated join starts the reconnect ladder over: an earlier give-up is forgiven.
  g_reconnectWanted.store(true);
  g_reconnectAttempts.store(0);
  g_reconnectRounds.store(0);
  if (g_reconnectTimer != nullptr) esp_timer_stop(g_reconnectTimer);

  // Configure WITHOUT connecting, so the station config can be patched before the first SAE attempt
  // rather than mid-handshake. STA::begin(..., tryConnect=false) still writes the config and starts
  // DHCP (libraries/WiFi/src/STA.cpp:418+), it just skips esp_wifi_connect().
  WiFi.begin(ssid, hasPassword ? password : nullptr, channel, bssid, /*tryConnect=*/false);

  // Read/patch/write the station config, retrying while the driver is still settling. The
  // auto-connect path (clock sync, OPDS, plugin delivery) starts right after a scan, so the
  // disconnect from the previous association is often still in flight here and esp_wifi_set_config
  // answers ESP_ERR_WIFI_STATE. That used to abort the attempt before a single connect was tried,
  // which is why the automatic join failed and tapping "connect" a moment later worked.
  // Install the disconnect handler once: it is the only place the failure reason is available.
  static bool s_handlerInstalled = false;
  if (!s_handlerInstalled) {
    if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr) == ESP_OK) {
      s_handlerInstalled = true;
    } else {
      LOG_ERR("WIFI", "could not register disconnect handler; reason codes will not be logged");
    }
  }
  strncpy(g_pendingSsid, ssid, sizeof(g_pendingSsid) - 1);
  g_pendingMode = saeModeFor(ssid);

  wifi_config_t cfg{};
  esp_err_t cfgErr = ESP_ERR_WIFI_STATE;
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
      LOG_DBG("WIFI", "get_config attempt %d failed", attempt + 1);
      delay(250);
      continue;
    }

    // SAE PWE derivation. 0 = both (H2E first, then hunt-and-peck), 1 = H2E only, 2 = hunt-and-peck
    // only. The value comes from what this SSID was learned to need; see saeModeFor(). WPA2-only APs
    // never run SAE, so the field is inert for them.
    switch (g_pendingMode) {
      case 1:
        cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_HASH_TO_ELEMENT;
        break;
      case 2:
        cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_HUNT_AND_PECK;
        break;
      default:
        cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        break;
    }

    // PMF: capable but not required. WPA3 mandates PMF; a WPA2 or transition-mode AP must still be
    // allowed to associate without it, which is exactly what required=false means.
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    cfgErr = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (cfgErr == ESP_OK) break;
    LOG_DBG("WIFI", "set_config attempt %d: %s", attempt + 1, esp_err_to_name(cfgErr));
    delay(250);
  }
  if (cfgErr != ESP_OK) {
    LOG_ERR("WIFI", "esp_wifi_set_config gave up after 10 attempts: %s", esp_err_to_name(cfgErr));
    return false;
  }

  // Retry while the driver is busy. esp_wifi_connect() returns ESP_ERR_WIFI_STATE while a scan or a
  // teardown is still running -- exactly the state an activity inherits when the previous screen
  // left Wi-Fi mid-flight. That error used to be treated as fatal with no retry, which is why the
  // first attempt from an activity failed and a manual second attempt always worked.
  esp_err_t err = ESP_ERR_WIFI_STATE;
  for (int attempt = 0; attempt < 10; ++attempt) {
    err = esp_wifi_connect();
    if (err == ESP_OK || err == ESP_ERR_WIFI_CONN) break;  // CONN: an attempt is already running
    LOG_DBG("WIFI", "connect attempt %d: %s", attempt + 1, esp_err_to_name(err));
    delay(250);
  }
  if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
    LOG_ERR("WIFI", "esp_wifi_connect gave up after 10 attempts: %s", esp_err_to_name(err));
    return false;
  }

  LOG_INF("WIFI", "associating%s sae=%s pmf=capable (ssid='%s')", channel ? " directed" : "",
          g_pendingMode == 0 ? "both" : (g_pendingMode == 1 ? "H2E" : "hunt-and-peck"), ssid);
  return true;
}

}  // namespace wificonnect