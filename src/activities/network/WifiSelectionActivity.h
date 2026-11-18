#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "system/PowerNeed.h"  // r163 E6: ForegroundNetwork, a soft need
#include "util/ButtonNavigator.h"

struct Rect;
struct ThemeMetrics;
struct WifiCredential;

// Structure to hold WiFi network information
struct WifiNetworkInfo {
  std::string ssid;
  int32_t rssi;
  bool isEncrypted;
  bool hasSavedPassword;             // Whether we have saved credentials for this network
  bool isHiddenPlaceholder = false;  // Synthetic "Add hidden network..." list entry
};

// WiFi selection states
enum class WifiSelectionState {
  AUTO_CONNECTING,    // Trying to connect to the last known network
  SCANNING,           // Scanning for networks
  NETWORK_LIST,       // Displaying available networks
  HIDDEN_SSID_ENTRY,  // Entering SSID for a hidden network
  PASSWORD_ENTRY,     // Entering password for selected network
  CONNECTING,         // Attempting to connect
  CONNECTED,          // Successfully connected
  SAVE_PROMPT,        // Asking user if they want to save the password
  CONNECTION_FAILED,  // Connection failed
  FORGET_PROMPT       // Asking user if they want to forget the network
};

/**
 * WifiSelectionActivity is responsible for scanning WiFi APs and connecting to them.
 * It will:
 * - Enter scanning mode on entry
 * - List available WiFi networks
 * - Allow selection and launch KeyboardEntryActivity for password if needed
 * - Save the password if requested
 * - Call onComplete callback when connected or cancelled
 *
 * The onComplete callback receives true if connected successfully, false if cancelled.
 */
class WifiSelectionActivity final : public Activity, private UiAppHost {
  ButtonNavigator buttonNavigator;

  WifiSelectionState state = WifiSelectionState::SCANNING;
  // State the last committed frame was built from, so render() can tell a
  // wholesale screen replacement from a repaint of the same page (see the
  // refresh-hint rule there). -1 = nothing painted yet.
  int lastRenderedState = -1;
  size_t selectedNetworkIndex = 0;
  std::vector<WifiNetworkInfo> networks;
  // Number of real (scanned) networks, excluding the synthetic hidden-network entry
  size_t realNetworkCount = 0;

  // Row buffers derived from `networks`, rebuilt only when it changes
  // (processWifiScanResults()) instead of on every repaint — buildListScreen()
  // used to re-derive a "+ * ||||" status string per network on every render
  // (cursor move, tap flash, ...).
  std::vector<std::string> networkStatuses;
  std::vector<freeink::ui::ListItem> networkRowItems;
  void rebuildNetworkRowItems();

  // Selected network for connection
  std::string selectedSSID;
  bool selectedRequiresPassword = false;

  // Connection result
  std::string connectedIP;
  std::string connectionError;

  // Password to potentially save (from keyboard or saved credentials)
  std::string enteredPassword;

  // Cached MAC address string for display
  std::string cachedMacAddress;

  // Whether network was connected using a saved password (skip save prompt)
  bool usedSavedPassword = false;

  // Whether to attempt auto-connect on entry
  const bool allowAutoConnect;

  // Whether we are attempting to auto-connect or auto-scan saved networks.
  bool autoConnecting = false;

  // r163 E6: the SOFT `ForegroundNetwork` hold for this screen's lifetime. Declared
  // (and counted) so a scan/join is visible, and inert by design: it neither blocks
  // automatic standby nor refreshes the inactivity clock. preventAutoSleep() above is
  // UNCHANGED and remains this screen's only effect on the sleep decision.
  powerneed::ScopedNeed foregroundNetworkNeed_{powerneed::Need::ForegroundNetwork};

  // True from the Confirm press that stops auto-connect until that button is released.
  bool manualNetworkListRequested = false;

  // Saved SSIDs already attempted during the current auto-connect session.
  std::vector<std::string> autoAttemptedSsids;

  // Save/forget prompt selection (0 = Yes, 1 = No)
  int savePromptSelection = 0;
  int forgetPromptSelection = 0;

  // Connection timeout
  // 30 s, measured: with the SAE PWE fix (util/WifiConnect.h) association succeeds at 30 s and times
  // out at 15 s on the same AP, so the SAE handshake here is genuinely slower than 15 s -- the STA is
  // falling back to hunt-and-peck. A ceiling below the real handshake time reports a working join as a
  // failure, which is worse than a slower true failure.
  static constexpr unsigned long CONNECTION_TIMEOUT_MS = 30000;
  // Was 20000. The same AP that joins in 18-25 s with a manual tap (30 s budget) was being aborted
  // by the auto path, then joining on the next tap -- the "auto connect is unreliable" report.
  // One budget for both paths; the difference was never principled.
  static constexpr unsigned long AUTO_CONNECTION_TIMEOUT_MS = 30000;
  // WL_IDLE_STATUS at the deadline means the supplicant is still working, not that it failed. One
  // extension, once per attempt, before the join is called a failure.
  static constexpr unsigned long JOIN_GRACE_MS = 5000;
  unsigned long connectionStartTime = 0;
  // 0 until the single grace extension has been granted for this attempt.
  unsigned long joinGraceMs = 0;

  // The UiAppHost app hosts the network list and the save/forget prompts
  // (themed rows and dialogs, touch routing); every other state keeps its
  // legacy centered-text rendering.
  // Viewport memory (top/visibleRows) for the network list; `selected` is
  // mirrored from selectedNetworkIndex at build/move time.
  freeink::ui::ListNav listNav;

  static void listScreen(UiScreen& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onScanEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onPromptEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiScreen& screen);
  void buildPromptDialog(UiScreen& screen);

  void renderNetworkList(const Rect* screen, const ThemeMetrics* metrics);
  void renderPasswordEntry(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnecting(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnected(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnectionFailed(const Rect* screen, const ThemeMetrics* metrics) const;

  void startWifiScan(bool autoScan = false);
  void processWifiScanResults();
  void appendHiddenNetworkEntry();
  void selectNetwork(int index);
  void promptHiddenSsid();
  void promptPasswordEntry();
  void attemptConnection();
  void checkConnectionStatus();
  bool tryAutoConnectCredential(const WifiCredential& cred);
  bool tryNextSavedNetworkFromScan();
  void handleAutoConnectFailure();
  void showNetworkListFromAutoConnect();
  bool hasAttemptedAutoSsid(const std::string& ssid) const;
  static std::string getSignalStrengthIndicator(int32_t rssi);

  void onComplete(bool connected);

 public:
  explicit WifiSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool autoConnect = true);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  // Answers Button::Back in loop(): the manager must not navigate for this screen.
  bool ownsBackKey() const override { return true; }
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
};
