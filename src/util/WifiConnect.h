#pragma once

// Station association with WPA3/SAE support.
//
// Why this exists: Arduino's `STAClass::begin()` builds the whole `wifi_config_t` itself
// (libraries/WiFi/src/STA.cpp:396-411) and never writes `sae_pwe_h2e`, so the field stays 0 =
// `WPA3_SAE_PWE_UNSPECIFIED`, which the driver treats as hunt-and-peck only. An AP that mandates
// Hash-to-Element -- which most current WPA3 routers do -- then never completes the SAE handshake,
// the station retries, and the UI's connect timer fires. That is the "WPA3 times out" symptom.
//
// It also confirms what Arduino already does right: `pmf_cfg.capable = true` is set, and `required`
// stays false. We set both explicitly anyway, because this file is the one place that owns the
// station config and a silent driver default must not decide whether WPA2 transition APs still join.
namespace wificonnect {

// Configures the station (without connecting), patches the SAE/PMF fields, then connects.
// `password` may be nullptr or empty for an open network.
bool begin(const char* ssid, const char* password);

// Records the channel and BSSID of the AP we just joined, keyed by SSID. TELEMETRY ONLY since r44:
// it used to drive a directed join, which is how a roamed AP turned into a 30 s timeout and a
// "second attempt works" symptom. Call once the link is up.
void remember();

// Clears the remembered AP record. Kept for the UI/log path; joins scan regardless since r44.
void forgetAp();

// True when an AP record for this SSID was taken recently. Telemetry for the timeout log line only --
// it no longer describes how the join was performed.
bool hasCachedAp(const char* ssid);

// Stops the background reconnect ladder and cancels its timer. MUST be called before a deliberate
// teardown that is not a plain disconnect (sleep, AP-mode switch): the ladder reconnects on every
// STA_DISCONNECTED that is not WIFI_REASON_ASSOC_LEAVE, and tearing the radio down for standby must
// never race it. begin() re-arms it.
void suspendAutoReconnect();

}  // namespace wificonnect