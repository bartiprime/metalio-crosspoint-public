#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>

// RTC-backed boot log from the vendor panel driver (C linkage).
extern "C" void mn_note_dump(void);
extern "C" bool metalio_vendor_panel_begin(void);
// Hand-declared for the same reason as the line above: the component header is not on the app's
// include path. Called by the panel driver at both edges of every blocking waveform.
typedef void (*MetalioPollHook)(void);
extern "C" void metalio_vendor_panel_set_poll_hook(MetalioPollHook hook);
extern "C" void metalio_vendor_panel_set_priority(int priority);
// Journal hook: one line per panel push, appended to /.crosspoint/sleep.txt (see sleepJournal).
typedef void (*MetalioJournalHook)(const char* line);
extern "C" void metalio_vendor_panel_set_journal_hook(MetalioJournalHook hook);
// r129 (H23): drive gate. Called with 1 at the entry edge of a waveform and 0 at its exit edge; the
// sink holds the SD log writer for that window so no flash erase can land between two panel lines.
typedef void (*MetalioDriveGateHook)(int active);
extern "C" void metalio_vendor_panel_set_drive_gate_hook(MetalioDriveGateHook hook);
// metalio_vendor_panel_repower(): cycles the TPS65185 and leaves the rails UP without driving a
// waveform. Used on the wake path to move power-on ahead of the first paint (open concern 22).
extern "C" bool metalio_vendor_panel_repower(void);
// THE SILENT-REBOOT GLASS CONTRACT (see main.cpp setup()): tell the driver that the glass already
// holds a frame this firmware drove, so its boot clear must not whiten it away.
extern "C" void metalio_vendor_panel_keep_glass_on_boot(void);
extern "C" void mn_bus_recover(void);
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
// S31 light-sleep shim (idf/components/metalio_driver/sleep): vendor power_policy/s31_sleep port.
#include "metalio_sleep.h"
#include "display/RefreshPolicy.h"
#include "platform/StandbyTrims.h"
// The vendor's own board-level sleep primitives (the transplanted epd_board file): INT masking,
// TPS entry, parallel-bus hold/restore and the PMIC pulse train. Using these instead of open-coding
// the same register writes is deliberate -- their sequencing IS the device-tested one.
#include "epd_board_metalio_eink4_plus.h"
#endif
#include <BlePageTurner.h>
#include <BoardConfig.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <PowerManager.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <TrustedTime.h>
#include <UiLiteralAudit.h>
#include <VectorFontSupport.h>
#include <WiFi.h>
#include <XteinkDetect.h>
#include <builtinFonts/all.h>
#include <esp_heap_caps.h>
#include <esp_flash_encrypt.h>
#include <esp_secure_boot.h>
#include <esp_wifi.h>
#include <time.h>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "LcpPassphraseStore.h"
#include "MappedInputManager.h"
#include "MetalioPowerRequest.h"
#include "OpdsServerStore.h"
#include "OpdsTokenStore.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "VocabHistoryStore.h"
#include "SdCardFontSystem.h"
#include "util/DebugFlag.h"
#include "WifiCredentialStore.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/boot_sleep/BootActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "activities/util/PowerOffActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHost.h"
#include "components/UiDensity.h"
#include "display/BatteryIconRegion.h"
#if CP_ICON_SAFE_PATCH
#include "display/BatteryIconPatch.h"
#endif
#if CP_BATT_PERCENT_PATCH
#include "display/BatteryPercentPatch.h"
#endif
// r165.6: the compose-epoch life of the battery one-shot pins. Every armer below
// (BATT6's percentage fallback, BATT5's glyph fallback, the legacy USB-edge
// repaint) goes through the same tracker, so this is not tied to one switch.
#include "display/OneShotPaintPins.h"
#include "fontIds.h"
#include "platform/UsbMscJournal.h"
#include "platform/UsbMscStorage.h"
#include "platform/UsbSerialJtagHandoff.h"
#include "debug/BenchDrives.h"
#if CP_EXP
#include "debug/DotprodBench.h"   // SIMD1: `exp simd`
#include "platform/ExpArms.h"      // EXP1: the arm table
#endif
#include "debug/PanelProbe.h"
#include "debug/UsbShell.h"
#include "platform/CpuStats.h"
#include "platform/LpCoreService.h"
#include "platform/ParkSinks.h"     // Build A E4c-E4f: the four park sinks + their runtime arms
// r131: SdPowerCycle.h is back with the resume's 50 ms dwell before the remount (r109 CARD1)
#include "platform/SdPowerCycle.h"
#include "platform/HybridClock.h"   // Build A item C: XTAL while awake, RTC delta while parked, stored ppm
#include "platform/PpaService.h"
#include "platform/PpaBuffer.h"     // PPA1 `ppaframe`: the cache-line alignment the engine demands
#include "platform/PpaSrmPlan.h"    // PPA1 `ppaframe`: the pure "may the engine serve this?" decision
#include "platform/PlatformFeatures.h"
#include "system/ChargerCurrent.h"
#include "system/ChargeIdlePolicy.h"  // r163 3.5: charging holds a ChargerHost need
#include "system/PowerNeed.h"         // r163 E6: the declarative need table the policy asks
#include "util/BatteryStats.h"
#include "util/BatteryGaugeFix.h"
#include "audio/UiSounds.h"  // AUDIO1: short UI feedback clicks (both gates default off)
#include "util/ButtonNavigator.h"
#include "util/PluginEvents.h"
#include "util/PowerGestureClassifier.h"
#include "util/ScreenshotUtil.h"
#include "util/SystemKnobs.h"
#include "util/Timezones.h"
#include "util/WifiConnect.h"


#if CROSSPOINT_VECTOR_FONTS
// Rendering (incl. FreeType TTF rasterization) runs on the Arduino loop task.
// The default 8 KB stack overflows inside FreeType's FT_Open_Face / variable-font
// parsing. This runtime override applies even with the prebuilt (dio_opi) core,
// where CONFIG_ARDUINO_LOOP_STACK_SIZE from sdkconfig is baked in and ignored.
// Vector-font boards only: without TTF the stock loop stack has always sufficed,
// and non-PSRAM boards need the 16KB back in DRAM.
SET_LOOP_TASK_STACK_SIZE(24 * 1024)
#endif

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts());
static unsigned long allowSleepAt = 0;
static unsigned long lastX4ProPowerClickAt = 0;

namespace {
constexpr unsigned long X4PRO_POWER_DOUBLE_CLICK_MS = 500;
constexpr unsigned long X4PRO_POWER_CLICK_MAX_HOLD_MS = 300;
}  // namespace

// A wake hold must never become an in-app power-button action.  Boot may continue
// while the button is held; swallow the one release that ends that wake gesture.
static bool wakePowerReleasePending = false;

// Fonts
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(14, &notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(12, &notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(16, &notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(18, &notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&ubuntu_8_regular);
EpdFontFamily smallFontFamily(8, &smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(10, &ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(12, &ui12RegularFont, &ui12BoldFont);

// Amazon Ember UI ladder (fontIds.h): the Andro theme's type roles --
// 14 header/status, 14 author/secondary, 20 menu labels, 22 featured title
// (kept: the ladder is a published set of ids, so a size is added, never
// renumbered), 24 bold featured title. Registered unconditionally: Andro is a
// selectable theme on every build, so these faces must resolve whatever else
// is compiled in.
EpdFont amazonember12RegularFont(&amazonember_12_regular);
EpdFont amazonember12BoldFont(&amazonember_12_bold);
EpdFontFamily amazonember12FontFamily(12, &amazonember12RegularFont, &amazonember12BoldFont);

EpdFont amazonember14RegularFont(&amazonember_14_regular);
EpdFont amazonember14BoldFont(&amazonember_14_bold);
EpdFontFamily amazonember14FontFamily(14, &amazonember14RegularFont, &amazonember14BoldFont);

EpdFont amazonember18RegularFont(&amazonember_18_regular);
EpdFont amazonember18BoldFont(&amazonember_18_bold);
EpdFontFamily amazonember18FontFamily(18, &amazonember18RegularFont, &amazonember18BoldFont);

EpdFont amazonember20RegularFont(&amazonember_20_regular);
EpdFont amazonember20BoldFont(&amazonember_20_bold);
EpdFontFamily amazonember20FontFamily(20, &amazonember20RegularFont, &amazonember20BoldFont);

EpdFont amazonember22RegularFont(&amazonember_22_regular);
EpdFont amazonember22BoldFont(&amazonember_22_bold);
EpdFontFamily amazonember22FontFamily(22, &amazonember22RegularFont, &amazonember22BoldFont);

EpdFont amazonember24RegularFont(&amazonember_24_regular);
EpdFont amazonember24BoldFont(&amazonember_24_bold);
EpdFontFamily amazonember24FontFamily(24, &amazonember24RegularFont, &amazonember24BoldFont);

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
RTC_NOINIT_ATTR uint32_t silentRebootPayload;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;
constexpr uint32_t SILENT_REBOOT_TARGET_SETTINGS = 2;
constexpr uint32_t SILENT_REBOOT_TARGET_JOIN_NETWORK = 3;
constexpr uint32_t SILENT_REBOOT_TARGET_MAX = SILENT_REBOOT_TARGET_JOIN_NETWORK;
constexpr uint32_t SILENT_REBOOT_LIGHT_ON = 1U << 0;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,          // cold boot, flash, panic, or plain reboot
  Silent,          // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  SplashlessWake,  // wake from deep sleep with the splash suppressed by the SD flag
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

// A silent restart is internal maintenance, so the light must come back exactly
// as the user left it. SETTINGS.frontlightOn is the saved preference and
// legitimately diverges from the live state (a wake with Restore Light on Wake
// off leaves the light off while the saved "was on" preference is kept), so
// carry the live state across the reboot instead of re-deriving it from
// settings. Cleared with the magic in setup().
static void armSilentReboot(const uint32_t target) {
  silentRebootTarget = target;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
}

// Returns instead of rebooting when sleep supersedes the reboot; callers keep
// running in that case.
static void silentRestartTo(const uint32_t target, const char* targetName) {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  armSilentReboot(target);
  LOG_DBG("MAIN", "Silent restart (target=%s)", targetName);
  // E-ink retains the previous frame until the target's first paint lands
  // (~2-3s). Without an overlay, users don't see the reboot and fire input
  // through to the new activity. On Home, Select on the default
  // selectorIndex=0 opens the most-recent book, looking like a trampoline back
  // to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestart() { silentRestartTo(SILENT_REBOOT_TARGET_HOME, "home"); }

void silentRestartToReader() { silentRestartTo(SILENT_REBOOT_TARGET_READER, "reader"); }

void silentRestartToSettings() { silentRestartTo(SILENT_REBOOT_TARGET_SETTINGS, "settings"); }

void silentRestartToJoinNetwork() {
  if (deepSleepInProgress) return;
#if FREEINK_CAP_TOUCH
  // A software reset would cycle touch/frontlight rails; those boards proceed
  // into Join Network without the fresh-heap reboot (return, don't stop WiFi —
  // this runs on the way *in*, unlike the exit-time silentRestart()).
  if (BoardConfig::hasTouch()) return;
#endif
  armSilentReboot(SILENT_REBOOT_TARGET_JOIN_NETWORK);
  LOG_DBG("MAIN", "Silent restart (target=join-network)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void restartToHomeAfterStorageHandoff() {
  // This path has no way back: the SD volume is already detached and the USB
  // port is back on the console, so returning without a reset strands the
  // device in drive mode with only the power key left (that is the field bug).
  // Sleep does not take this path (drive mode pins the device awake), and if a
  // stale sleep flag ever did reach us, rebooting still wins: one lost sleep
  // transition is cheaper than a device the user has to power-cycle by hand.
  if (deepSleepInProgress) {
    LOG_ERR("MAIN", "Restart after storage handoff while sleep is in progress; rebooting anyway");
  }
  armSilentReboot(SILENT_REBOOT_TARGET_HOME);
  LOG_INF("MAIN", "Restart after storage handoff (target=home)");
  // No popup and no panel refresh here: the filesystem that backs the UI fonts
  // is detached, the reset lands in milliseconds, and a draw on this path was
  // the prime hang suspect.
  delay(50);
  handoffUsbOtgToSerialJtag();
  ESP.restart();
}

void toggleFrontlight() {
  if (!Frontlight.present()) return;
  const bool lightOn = !Frontlight.isOn();
  Frontlight.setOn(lightOn);
  SETTINGS.frontlightOn = lightOn ? 1 : 0;
  SETTINGS.saveToFile();
  LOG_INF("LIGHT", "Frontlight toggled %s", lightOn ? "on" : "off");
}

// Applies the slide switch's position to the bound function at boot, on each
// change, and when its binding changes; the physical position stays the source
// of truth, as on stock.
void handleSlideSwitch() {
  static int appliedAction = -1;
  static int appliedOn = -1;
  if (!gpio.hasToggleSwitch()) return;
  // Compare the position, not the change edge: transfer loops poll input and
  // clear the edge before this runs.
  const bool on = gpio.isToggleSwitchOn();
  if (appliedAction == SETTINGS.slideSwitchAction && appliedOn == static_cast<int>(on)) return;
  appliedAction = SETTINGS.slideSwitchAction;
  appliedOn = on;
  switch (SETTINGS.slideSwitchAction) {
    case CrossPointSettings::SLIDE_SWITCH_NIGHT_MODE:
      if ((SETTINGS.screenInverted != 0) == on) return;
      SETTINGS.screenInverted = on ? 1 : 0;
      break;
    case CrossPointSettings::SLIDE_SWITCH_TOUCH_CONTROLS:
      if ((SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) == on) return;
      SETTINGS.touchReaderControls = on ? CrossPointSettings::TOUCH_READER_ON : CrossPointSettings::TOUCH_READER_OFF;
      break;
    default:
      if (!Frontlight.present() || Frontlight.isOn() == on) return;
      Frontlight.setOn(on);
      SETTINGS.frontlightOn = on ? 1 : 0;
      break;
  }
  SETTINGS.saveToFile();
  activityManager.requestUpdate();
  LOG_INF("SWITCH", "Slide switch %s", on ? "on" : "off");
}

bool handleX4ProFrontlightDoubleClick() {
  if (!BoardConfig::isX4Pro() || !SETTINGS.doubleClickPwrLight || !gpio.wasReleased(HalGPIO::BTN_POWER)) {
    return false;
  }

  const unsigned long now = millis();
  if (gpio.getPowerButtonHeldTime() > X4PRO_POWER_CLICK_MAX_HOLD_MS) {
    lastX4ProPowerClickAt = 0;
    return false;
  }

  if (lastX4ProPowerClickAt == 0 || now - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
    lastX4ProPowerClickAt = now;
    return false;
  }

  lastX4ProPowerClickAt = 0;
  toggleFrontlight();
  return true;
}

// Bluetooth page turner (lib/BlePageTurner): what the module may ask of this firmware. The module
// owns the radio; loop() tells it what is on screen, and the screen changes and sleep ask it to stop.
namespace pageturner {
std::atomic<bool> holdsCpu{false};

bleturner::Heap heap() {
  const auto h = HalMemory::getInternalHeap();
  return {h.freeBytes, h.largestBlockBytes};
}

// Runs on the radio's start task. Only TRIES the render lock: the render task can hold it through a
// chapter build that is waiting for this very radio.
bool releaseCaches() {
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return false;
  fontCacheManager.releaseSdFontCaches();
  return true;
}

bool deliver(const bleturner::Action action) {
  switch (action) {
    case bleturner::Action::NextPage:
      return activityManager.remoteTurn(true, false);
    case bleturner::Action::PrevPage:
      return activityManager.remoteTurn(false, false);
    case bleturner::Action::NextChapter:
      return activityManager.remoteTurn(true, true);
    case bleturner::Action::PrevChapter:
      return activityManager.remoteTurn(false, true);
    case bleturner::Action::Confirm:
      return activityManager.remoteClick(RemoteButton::Confirm);
    case bleturner::Action::Back:
      return activityManager.remoteClick(RemoteButton::Back);
    case bleturner::Action::ReaderMenu:
      return activityManager.remoteShortcut(true);
    case bleturner::Action::SaveQuote:
      return activityManager.remoteShortcut(false);
    case bleturner::Action::None:
      return false;
  }
  return false;
}

bool yieldForRadio() { return true; }        // the reader keeps no parser alive between pages
bool fileTransferActive() { return false; }  // file transfer is a screen of its own: the radio is off there
void restartIntoBook() { silentRestartToReader(); }

// The radio needs a steady clock while it is up or changing state.
void holdFullSpeed(const bool hold) {
  holdsCpu.store(hold);
  if (hold) powerManager.setPowerSaving(false);
}

void log(const bool error, const char* format, va_list args) {
#ifdef ENABLE_SERIAL_LOG
  if (!error && LOG_LEVEL < 1) return;
  char line[160];
  vsnprintf(line, sizeof(line), format, args);
  logPrintf(error ? "ERR" : "INF", "BLE", "%s", line);
#else
  (void)error;
  (void)format;
  (void)args;
#endif
}

const bleturner::Host host{heap,          releaseCaches, deliver, yieldForRadio, fileTransferActive, restartIntoBook,
                           holdFullSpeed, log,           nullptr};
}  // namespace pageturner

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Plugin-event delivery on the way into deep sleep. sleep.enter is delivered
// now — over the live connection, or by bringing WiFi up when a plugin
// subscribes (e.g. fetching a fresh /sleep.bmp so THIS sleep shows it — the
// drain runs before goToSleep() renders the sleep screen). The connect path
// is bounded (join deadline + drain event budget), skipped on low battery,
// and sleep is never blocked on the network: a failed join or delivery just
// sleeps with the previous image and the queued events retry on the next
// drain (at-least-once). The caller's WiFi shutdown tears the radio down
// either way. Deferrable events already queued (reader.exit) ride along in
// the same drain.
static void deliverSleepPluginEvents(const bool allowNetwork) {
  // Activity-owned state must be queued before sleep.enter and before this
  // same-sleep drain. The hook is idempotent with ordinary activity teardown.
  activityManager.prepareForSleep();

  // Sleeping straight out of a book is the common flow, but the reader's own
  // reader.exit only fires later, inside goToSleep() — after this drain. Carry
  // the book and progress on sleep.enter itself so a sync handler bound to it
  // pushes current progress on THIS connection, not the next one.
  pluginevents::Var vars[2];
  size_t varCount = 0;
  char percent[8];
  const ScreenshotInfo info = activityManager.getScreenshotInfo();
  if (info.readerType != ScreenshotInfo::ReaderType::None && !APP_STATE.openEpubPath.empty()) {
    snprintf(percent, sizeof(percent), "%d", info.progressPercent);
    vars[varCount++] = {"book", APP_STATE.openEpubPath.c_str()};
    vars[varCount++] = {"percent", percent};
  }
  pluginevents::emit(pluginevents::Event::SleepEnter, vars, varCount);
  if (!allowNetwork) return;
  if (WiFi.status() == WL_CONNECTED) {
    pluginevents::drain(&renderer);
    return;
  }
  // Any connect-flagged queued event justifies the join, not only
  // sleep.enter: reader.session is queued while reading and delivered on this
  // same sleep, and a progress-sync plugin usually subscribes to it alone.
  if (!pluginevents::wantsConnectAny()) return;
  if (powerManager.getBatteryPercentage() < 20) return;
  // Sleep can be the first WiFi use since boot or deep-sleep wake.
  WIFI_STORE.loadFromFile();
  const auto cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!cred) return;

  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  WiFi.mode(WIFI_STA);
  // Same helper as the Wi-Fi screen: a WPA3 AP needs sae_pwe_h2e set to complete SAE, and this
  // background path would otherwise fail silently for those networks.
  wificonnect::begin(cred->ssid.c_str(), cred->password.c_str());
  // 8 s, not 20. This runs while entering sleep, so the whole wait is a freeze with a frozen popup
  // and no way to cancel: with the AP absent, every single sleep paid 20 s. A join that needs longer
  // than 8 s (slow SAE) simply does not deliver this event now -- the events stay queued and are
  // delivered on the next connect, which is the same outcome the timeout path already had.
  const unsigned long joinDeadline = millis() + 8000;
  const unsigned long joinStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() < joinDeadline) {
    delay(100);
  }
  LOG_INF("WIFI", "sleep-event join %s after %lums", WiFi.status() == WL_CONNECTED ? "ok" : "deferred",
          millis() - joinStart);
  if (WiFi.status() == WL_CONNECTED) {
    wificonnect::remember();   // cache channel+BSSID so the next join skips the scan
    trustedtime::startSync();  // snap the clock floor while the network is up
    pluginevents::drain(&renderer);
  } else {
    LOG_DBG("MAIN", "Sleep-event WiFi join timed out; deferring delivery");
    wificonnect::forgetAp();
  }
}

#if FREEINK_DEVICE_PICCO
// Charge strip along the bottom of the retained sleep screen.
static void drawSleepChargeStrip(const bool charging, const uint16_t percent) {
  RenderLock lock;
  const int h = renderer.getLineHeight(SMALL_FONT_ID) + 12;
  const int y = renderer.getScreenHeight() - h;
  renderer.fillRect(0, y, renderer.getScreenWidth(), h, false);
  char text[48];
  snprintf(text, sizeof(text), "%s  %u%%", charging ? tr(STR_CHARGING) : tr(STR_CHARGED), percent);
  renderer.drawCenteredText(SMALL_FONT_ID, y + 6, text);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// Shows the charge strip while the power HAL holds sleep on USB power, and
// removes it again if USB goes away. Returns true when the power button ended
// the hold.
static bool holdSleepWhileUsbPowered() {
  if (!gpio.isUsbConnected()) return false;
  // Keep the sleep frame so unplugging can remove the strip. 48 KB of PSRAM,
  // freed when the hold ends.
  const size_t frameSize = renderer.getBufferSize();
  auto* const sleepFrame = static_cast<uint8_t*>(heap_caps_malloc(frameSize, MALLOC_CAP_SPIRAM));
  if (sleepFrame) memcpy(sleepFrame, renderer.getFrameBuffer(), frameSize);
  const bool buttonPressed = powerManager.holdWhileUsbPowered(gpio, drawSleepChargeStrip);
  if (sleepFrame) {
    if (!buttonPressed) {
      RenderLock lock;
      memcpy(renderer.getFrameBuffer(), sleepFrame, frameSize);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
    heap_caps_free(sleepFrame);
  }
  return buttonPressed;
}
#endif

// Enter deep sleep mode
// The only channel that survives a boot loop: the USB-Serial/JTAG console works until the panel
// driver claims GPIO19. Prints the tail of the on-SD log before display init, so a device that dies
// later in boot still reports why.
static void dumpLogTailToStdout(const char* path, const size_t tailBytes) {
  HalFile f = Storage.open(path, O_RDONLY);
  if (!f) {
    printf("[CP] log tail: cannot open %s\n", path);
    fflush(stdout);
    return;
  }
  const size_t size = f.size();
  if (size == 0) {
    printf("[CP] log tail: %s is empty\n", path);
    fflush(stdout);
    return;
  }
  const size_t start = size > tailBytes ? size - tailBytes : 0;
  if (!f.seek(start)) {
    printf("[CP] log tail: seek failed\n");
    fflush(stdout);
    return;
  }
  printf("[CP] ---- log tail: last %u of %u bytes ----\n", static_cast<unsigned>(size - start),
         static_cast<unsigned>(size));
  fflush(stdout);
  char buf[256];
  while (true) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    fwrite(buf, 1, static_cast<size_t>(n), stdout);
  }
  printf("\n[CP] ---- end log tail ----\n");
  fflush(stdout);
}

// Inactivity clock for the auto-sleep timeout. File scope, not a loop-local static, so the standby
// resume path can reset it: millis() advances across light sleep, so time the CPU spent asleep must
// never be counted as user inactivity (r42 defect: a 6.9 h standby made the timer see hours of
// "idleness" and the device re-slept ~1.2 s after every wake).
static unsigned long lastActivityTime = 0;
static bool lastActivityTimePrimed = false;

// ---------------------------------------------------------------------------
// r91 R1 — THE predicate for "a debug session is holding this device awake".
//
// It is exactly usbshell::keepsDeviceAwake() (session flag on AND the REPL actually
// running) and there is no other test of SETTINGS.usbDebugShell anywhere in the power
// policy: the auto-sleep timeout, the idle low-power entry and (defensively) the
// standby auto-shutdown all ask this one function. Anything that turns the session
// off -- the Debug page toggle, manual sleep, a confirmed power-off, USB drive mode --
// therefore restores the user's own power policy with no per-site bookkeeping.
// ---------------------------------------------------------------------------
static inline bool usbShellHoldsAwake() { return usbshell::keepsDeviceAwake(); }

// ---------------------------------------------------------------------------
// r163 E6 -- THE POWER-NEED TICK, once per awake loop iteration.
//
// It does three things, in this order:
//   1. SYNC the charge policy's hold (item 3.5) from the pack's own verdict. `set`
//      is declarative, so no release path can be missed and the hold cannot be
//      leaked by a cable that was pulled while some other branch returned early.
//   2. AGE every hold (powerneed::poll) and ANNOUNCE a leak once per excursion.
//      A leaked need blocks automatic standby forever, so it must say so in the
//      log instead of leaving the operator with "it stopped sleeping".
//   3. Surface a double release, which is always a bug in the caller.
//
// Cheap: the charge verdict is the existing 1.5 s cache, and four counters are
// compared. No allocation, no I/O beyond that cache.
// ---------------------------------------------------------------------------
static void powerNeedTick() {
  const unsigned long now = millis();
  powerneed::set(powerneed::Need::ChargerHost,
                 chargepolicy::holdWhileCharging(SETTINGS.chargeIdlePolicy, powerManager.isCharging()), now);
  powerneed::sys().poll(now);

  const powerneed::Report r = powerneed::sys().report(now);
  for (uint8_t i = 0; i < powerneed::kNeedCount; ++i) {
    const auto n = static_cast<powerneed::Need>(i);
    // takeLeakAnnouncement() is true once per excursion (it is re-armed when the
    // hold ends), so this logs one line, not one per iteration.
    if (!powerneed::sys().takeLeakAnnouncement(n)) continue;
    LOG_WRN("PWR",
            "need LEAK: %s (%s) held %lu ms with no release -- automatic standby is BLOCKED until it is "
            "released; `needs` on the debug shell shows the holder age",
            powerneed::nameOf(n), powerneed::holdName(powerneed::holdOf(n)), r.status[i].heldMs);
  }

  static uint32_t doubleReleasesLogged = 0;
  if (r.doubleReleases != doubleReleasesLogged) {
    doubleReleasesLogged = r.doubleReleases;
    LOG_WRN("PWR", "need double release (%u since boot) -- a holder released a need it did not hold; "
                   "the count was clamped, not wrapped",
            static_cast<unsigned>(r.doubleReleases));
  }
}

#if FREEINK_DEVICE_METALIO_EINK4_PLUS
// ---------------------------------------------------------------------------------------------
// Standby on the Metalio E-Ink4 Plus = the vendor's power_policy model, not deep sleep.
//
// Vendor reference: main/boards/common/power_policy/{power_hw.cc,power_policy_standby_lp.cc}.
// The rails stay up for standby, so a cold boot would gain nothing and lose everything the user
// was looking at. Instead: park the panel, drop the touch controller to monitor mode, mask every
// expander key but POWER off the shared INT line, and sleep in slices on GPIO2 — then carry on
// exactly where we were. Hard power-off is a separate thing: a P1.3 pulse train until the PMIC
// collapses the 3.3 V rail.
//
// Why the old path failed (r28-r39, documented in docs/SLEEP_COMPARATIVE_REPORT.md): every
// esp_deep_sleep_start() on this SoC *returned*, the wrapper then esp_restart()ed, and the user
// got sleep screen -> flash -> back to the menu. Two boots inside one RAM ring proved it.
// ---------------------------------------------------------------------------------------------

// One light-sleep slice. The vendor caps a slice at 60 s so a stuck key or a missed interrupt can
// never wedge the loop for long; a wake on GPIO2 ends the slice early anyway.
constexpr uint64_t METALIO_STANDBY_SLICE_US = 60ULL * 1000 * 1000;

// >3 s in standby means "power off", not "wake" (SETTINGS.getPowerButtonDuration() is the
// sleep-on-hold threshold in the awake UI; this is its standby twin). Was 4000 ms: the operator
// reported the hold felt long, and 3 s keeps a wide margin over the ~400 ms tap/own-duration
// thresholds above while cutting a full second of dead hold.
constexpr unsigned long METALIO_POWER_OFF_HOLD_MS = 2200;
// Contact bounce / ESD glitch: shorter than this is not a press.
constexpr unsigned long METALIO_WAKE_BOUNCE_MS = 40;
// How long the level must stay UP before a release is believed. The power key is expander P1.7 and
// readButtons() reports 0xFFFF ("every key released") whenever one TCA8195/9555 read fails, so a
// single failed read looks exactly like a release for up to the 20 ms sample cache. Believing it
// would end a real press mid-hold -- and a re-press then stamps a fresh timestamp, which is how a
// 3 s hold can fail to measure 3 s. 100 ms is several read windows and invisible to the user.
constexpr unsigned long METALIO_POWER_RELEASE_SETTLE_MS = 100;
// Standby auto-shutdown is a product setting: SETTINGS.standbyAutoOffMinutes (0 = never,
// the default). The long press above remains the immediate way to power off.

// Standby journal throttle: the slice count lives in RAM and is reported on wake, so a normal 60 s
// slice never touches the card (~60 writes/hour, ~400 per overnight standby). Only anomalies write.
static uint32_t slpSliceCount = 0;
static uint32_t slpLowLevelSlices = 0;
static uint32_t slpArmRetries = 0;
// Held-P17-at-arm events (item 3). Separate from slpArmRetries so the journal can tell "the shared
// line would not go idle" from "the key was being held while we armed" -- two different faults.
static uint32_t slpHeldAtArm = 0;
// Peripherals-parked CONTRACT violations (docs/engineering/STANDBY_TIMER_WAKE_CONTRACT.md).
// A timer wake is a slice boundary, not an exit: the park must still be in force when it lands.
// Rate-limited like the other standby counters -- this should stay 0 for the life of a build.
static uint32_t slpParkViolations = 0;
#if CP_LP_STANDBY
// r99: standby-local LP sentry state. `slpSentryDropped` is sticky for the REST of
// this standby once the LP side proves it did not run (or ran too slowly) — the loop
// then arms EXT1 exactly as it does today. `slpSentryStarts` only controls the
// once-per-standby journal line. Both live in RAM: a normal standby writes nothing.
static bool slpSentryDropped = false;
static uint32_t slpSentryStarts = 0;
#endif

// r46: awake-UI power-key hold tracking. The awake loop runs this every tick so the sleep
// threshold (~400 ms) and the power-off threshold (4000 ms) stay separate -- the old branch only
// got control after the first threshold had already fired, so 4 s was unreachable on a direct
// hold. Reset on every entry into standby: a press that sleep owned must never be re-classified.
static bool metalioPowerHoldActive = false;
// When the power-off dialog was (re)requested, and how many pushes this press has
// spent on it (max 2: the push, plus one bounded retry if a queued navigation ate
// it). Both are loop-task only.
static unsigned long metalioPowerDialogArmedAt = 0;
static uint8_t metalioDialogPushCount = 0;
// r163 B5: the press tracker the sleep decision uses. It used to be three loose statics
// (`metalioPowerPressSeen`, `metalioPowerPressStartMs`, `metalioPowerReleaseAt`) written inline
// here; the same rules now live in `powergesture::Classifier`, which stamps the DOWN edge once
// and never re-stamps it while the finger stays down (unlike the input manager's
// powerButtonPressStart, which every new BTN_POWER edge resets -- an expander read that comes
// back as "all released" ends the press and a 3 s hold then measures far less). The classifier
// also names the gesture (click / multi-click / hold) and DISCARDS a press that predates the
// current boot epoch, which is what makes a hold that spans a silent restart harmless.
// Thresholds come from the constants above so the two can never drift.
// Board-gated like every other Metalio-only helper here, so a non-Metalio build neither
// instantiates the classifier nor warns about an unused static function.
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
static powergesture::Config metalioPowerGestureConfig() {
  powergesture::Config cfg;
  cfg.holdMs = METALIO_POWER_OFF_HOLD_MS;
  cfg.releaseSettleMs = METALIO_POWER_RELEASE_SETTLE_MS;
  cfg.multiClickMs = 500;
  return cfg;
}
static powergesture::Classifier metalioPowerGesture(metalioPowerGestureConfig());
#endif
// r100.3: true once a line has been written for a press the dialog owned (see the classifier's
// dialog-up branch, which used to be completely silent).
static bool metalioPowerIgnoredLogged = false;
// When a PowerOff push was last issued. NOT cleared by the dialog-up branch: it is the throttle
// that stops a transition which never lands from being re-pushed on every pass.
static unsigned long metalioLastPushMs = 0;
// How long a QUEUED PowerOff push may count as "the dialog is up" before it is declared stale.
// The classifier's bounded retry is only reachable while the dialog is NOT up, so a push that is
// queued but never entered must not hold the predicate true forever -- see metalioDialogState().
constexpr unsigned long METALIO_POWER_PUSH_GRACE_MS = 1500;

// Sleep forensics. /.crosspoint/debug.log is NOT a live log -- logSdFlush() dumps a RAM ring on
// demand and every boot-time line has long been evicted by then. Anything written here is
// appended straight to the card and flushed, so it survives a hang, a brownout or a crash.
static void sleepJournal(const char* fmt, ...) {
  Storage.mkdir("/.crosspoint");
  constexpr const char* kPath = "/.crosspoint/sleep.txt";
  HalFile f = Storage.open(kPath, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return;
  if (f.size() > 16384) {  // roll rather than grow without bound
    f.close();
    // Keep one previous generation. Deleting the file destroys the only forensic record of the
    // session being investigated -- a real 7 h session came within 1.6 KB of being erased here.
    Storage.remove("/.crosspoint/sleep.txt.old");
    if (!Storage.rename(kPath, "/.crosspoint/sleep.txt.old")) Storage.remove(kPath);
    f = Storage.open(kPath, O_WRONLY | O_CREAT | O_APPEND);
    if (!f) return;
  }
  char line[192];
  int n = snprintf(line, sizeof(line), "t=%lu ", static_cast<unsigned long>(millis()));
  if (n < 0 || n >= static_cast<int>(sizeof(line))) n = 0;
  va_list args;
  va_start(args, fmt);
  const int m = vsnprintf(line + n, sizeof(line) - static_cast<size_t>(n), fmt, args);
  va_end(args);
  if (m < 0) return;
  const size_t len = static_cast<size_t>(n) + static_cast<size_t>(m);
  f.write(line, len < sizeof(line) ? len : sizeof(line) - 1);
  f.write(static_cast<uint8_t>('\n'));
  f.flush();
  f.close();
}

// File-local C-string shim for the vendor panel's journal hook (it takes const char*, not a format).
static void sleepJournalCStr(const char* s) { sleepJournal("%s", s); }

// ---------------------------------------------------------------------------
// r89 — PANEL DRIVE HEALTH: did the drive we just asked for actually reach the glass?
//
// The panel shim already answers this once per show(), in its own journal line
// (idf/components/metalio_driver/metalio_vendor_panel.cpp:421-436):
//   [panel] show mode=GC16-from-white tps=1 err=0x0 ms=..
// `tps` is the TPS65185 rail verdict for THAT drive, and it is the only
// trustworthy evidence that the waveform had a live rail under it: show()
// powers up, and if the power-up did not come back (or if the PWR_GOOD read
// could not be taken and the code assumed "up", :358-365) it still falls
// through to the drive and epdiy still reports EPD_DRAW_SUCCESS. A waveform
// clocked against dead rails leaves the old frame on the glass while every
// buffer inside epdiy commits as if the new one landed -- and from that point
// every diff is computed against a lie, which is permanent ghosting.
//
// So: keep the last line's verdict, and let GfxRenderer treat
// "no rail under a successful drive" as a FAILED drive (row 12 of
// RE_RICKYOS_PIPELINE.md: drop the latch, drop the diff baseline, re-arm a
// from-white retry). Nothing else can see this; the driver's error code says 0.
// ---------------------------------------------------------------------------
namespace {
volatile int g_panelDriveTps = -1;      // -1 = no line seen yet
volatile int g_panelDriveFast = -1;     // 1 = DU/binary table, 0 = a cleaning table
volatile int g_panelDriveFromWhite = -1;  // 1 = the drive started from a white baseline
// PG1: PWR_GOOD (TCA P07) sampled by the shim at the END of the waveform, while the rails are
// still up. -1 = not reported (expander read failed, or an older shim). This is the one rail
// reading that is both independent of the power-up latch and meaningful: see the note in
// metalio_vendor_panel.cpp beside the `[panel] rails` line.
volatile int g_panelDrivePgood = -1;
// One per show(). The renderer samples this before issuing a drive and requires
// it to have advanced, so a stale line can never corroborate a new drive.
volatile uint32_t g_panelDriveSeq = 0;
// True while the power-off path also wants these lines appended to the sleep journal
// (the only store that survives the rail collapsing).
bool s_journalPanelLines = false;

// ---------------------------------------------------------------------------
// r96.3 (D2) — WAKE DRIVE CAPTURE.
//
// The operator reports the resume still bands, and the latest on-device measurement says
// the clean drive itself succeeded: `[panel] show mode=GC16-from-white tps=1 err=0x0 ms=765`.
// An err of 0 REFUTES the line-queue-underrun theory, so what is left is either a drive
// sequence that is not one drive, or a single from-white drive that physically bands on
// cold rails. Those two answers need different fixes and cannot be told apart from a
// report, so the resume counts its own drives instead of trusting the shape of the code:
//
//   [WAKE] drive N mode=.. err=0x.. ms=..     one line per drive inside the window
//   [WAKE] resume drives=N window=.. ms       one summary when the window closes
//
// Armed by the resume path (main.cpp, "resume: one-drive clean armed"), closed by loop()
// after CP_WAKE_CAPTURE_MS. drives=1 with a band on glass means rail settling / first-pass
// waveform; drives>1 means a drive we have not suppressed yet.
// ---------------------------------------------------------------------------
bool g_wakeCapture = false;
// Written on the render task inside show(), read on the main task: volatile because the
// point of the counter is to be seen, not to be fast.
volatile uint32_t g_wakeDriveN = 0;
volatile unsigned long g_wakeCaptureStartMs = 0;
volatile unsigned long g_wakeCaptureDeadlineMs = 0;
// Last drive's printable fields, parsed out of the shim's own line.
char g_panelDriveMode[24] = {0};
uint32_t g_panelDriveErr = 0;
unsigned long g_panelDriveMs = 0;

}  // namespace

// r115/r116: what the [DRIVE] line in the panel shim reports. Kept as C accessors OUTSIDE the anonymous
// namespace because that line is emitted from the metalio_driver component, which cannot see this
// file's statics: an extern "C" function inside the namespace would still be internal linkage.
extern "C" int cp_pm_enabled(void) {
#if defined(CONFIG_PM_ENABLE)
  return 1;
#else
  return 0;
#endif
}

// r129 (H24): the CPU frequency the [DRIVE] line reports. Added here rather than read in the panel
// component because getCpuFrequencyMhz() is an Arduino symbol. Pure read of the running clock; it
// does not set, hold or step anything -- logging only.
extern "C" int cp_cpu_mhz(void) { return static_cast<int>(getCpuFrequencyMhz()); }

// bit0 = ESP_PM_APB_FREQ_MAX held, bit1 = ESP_PM_CPU_FREQ_MAX held.
// r116: the r115 wake esp_pm locks were REMOVED FROM SOURCE, not flagged off. They existed only to
// arbitrate the CPU/APB frequency between this file and esp_pm (r105's CONFIG_PM_ENABLE), and r116
// compiles esp_pm out, so there is no second owner to arbitrate with. The accessor stays because
// the [DRIVE] line's shape is part of the proof a drive ran; it answers 0, permanently.
extern "C" int cp_wake_pm_lock_state(void) { return 0; }

// Sampled by GfxRenderer before a drive, to fence the health verdict to it.
extern "C" uint32_t cp_panel_drive_seq(void) { return g_panelDriveSeq; }

// Health of the drive with sequence number `since` + 1. Returns false when
// nothing was reported (other boards, or the shim refused before driving), in
// which case the caller must fall back to the driver's own error code.
extern "C" bool cp_panel_drive_health(uint32_t since, int* tps, int* fast) {
  if (g_panelDriveSeq == since) return false;
  if (g_panelDriveTps < 0) return false;
  if (tps) *tps = g_panelDriveTps;
  if (fast) *fast = g_panelDriveFast;
  return true;
}

// PG1 — the second, independent rail verdict for the drive `since` + 1: PWR_GOOD sampled at the
// end of its waveform (1 up, 0 down, -1 unknown/not reported). Separate from
// cp_panel_drive_health() because it answers a different question: the latch says "did the rails
// come up", this says "were they still up when the waveform finished". Deliberately conservative —
// any unknown is -1, so a caller can fire only on a definite 0.
extern "C" int cp_panel_drive_pgood(uint32_t since) {
  if (g_panelDriveSeq == since) return -1;
  return g_panelDrivePgood;
}

// Permanent journal hook: records the verdict of every drive, and forwards the
// line to the sleep journal while the power-off path is running (previously the
// hook was only installed there, so every other drive was unobservable).
static void panelJournalCStr(const char* line) {
  // BENCH1: the `benchdrives` harness reads THIS feed rather than measuring anything of its own
  // (see src/debug/BenchDrives.h). A no-op unless the harness is armed, so the drive path, the
  // journal and the log are byte-identical when it is not.
  benchdrives::notePanelLine(line);
  // r101.9 WAKE3 (diagnostics only) — WHICH PANEL LINE IS THIS. One show() emits TWO journal
  // lines: `[panel] show mode=.. ms=..` (metalio_vendor_panel.cpp:864) and
  // `[panel] rails tps=.. pgood=.. err=..` (:932). Only the first carries mode= and ms=, so a
  // `[WAKE] drive` row for the second inherits the FIRST row's mode/err/ms through the globals
  // below and looks like a second drive of the same waveform. The kind is read off the line
  // prefix, so it distinguishes "another waveform" from "the same drive's second journal line"
  // at no cost. Nothing below branches on it.
  const char* lineKind = "-";
  if (line != nullptr) {
    if (strncmp(line, "[panel] show SKIPPED", 20) == 0) {
      lineKind = "skip";
    } else if (strncmp(line, "[panel] show", 12) == 0) {
      lineKind = "show";
    } else if (strncmp(line, "[panel] rails", 13) == 0) {
      lineKind = "rails";
    } else if (strncmp(line, "[panel] recover", 15) == 0) {
      lineKind = "rec";
    } else if (strncmp(line, "[panel] pg=", 11) == 0) {
      // r115: was 12 against an 11-character literal, so every `[panel] pg=` line fell through to
      // src=other and the drive capture could not tell the pre-drive rail verdict from a foreign
      // journal write. The literal is 11 bytes (`[panel] ` = 8, `pg=` = 3).
      lineKind = "pg";
    } else if (strncmp(line, "[panel] boot", 12) == 0) {
      lineKind = "boot";
    } else if (strncmp(line, "[G16] erase sweep", 17) == 0) {
      lineKind = "sweep";
    } else if (strncmp(line, "[G16]", 5) == 0) {
      lineKind = "g16";
    } else if (strncmp(line, "[panel] ", 8) == 0) {
      lineKind = "other";
    }
  }
  if (line != nullptr) {
    const char* mode = strstr(line, "mode=");
    const char* tps = strstr(line, "tps=");
    if (tps != nullptr) g_panelDriveTps = atoi(tps + 4);
    if (mode != nullptr) {
      g_panelDriveFromWhite = strncmp(mode + 5, "GC16-from-white", 15) == 0 ? 1 : 0;
      g_panelDriveFast = strncmp(mode + 5, "DU", 2) == 0 ? 1 : 0;
      // r96.3 (D2): keep the fields the wake capture reports, so the [WAKE] line is the
      // shim's own numbers rather than a second measurement that could disagree with it.
      const char* m = mode + 5;
      size_t n = 0;
      while (m[n] != '\0' && m[n] != ' ' && n + 1 < sizeof(g_panelDriveMode)) ++n;
      memcpy(g_panelDriveMode, m, n);
      g_panelDriveMode[n] = '\0';
    }
    const char* errTok = strstr(line, "err=");
    if (errTok != nullptr) g_panelDriveErr = static_cast<uint32_t>(strtoul(errTok + 4, nullptr, 0));
    // PG1: the `[panel] rails ... pgood=<1|0|-1>` line the shim emits after the waveform.
    const char* pgoodTok = strstr(line, "pgood=");
    if (pgoodTok != nullptr) g_panelDrivePgood = atoi(pgoodTok + 6);
    const char* msTok = strstr(line, "ms=");
    if (msTok != nullptr) g_panelDriveMs = strtoul(msTok + 3, nullptr, 10);
    g_panelDriveSeq = g_panelDriveSeq + 1;
    if (g_wakeCapture) {
      ++g_wakeDriveN;
      // r101.9 WAKE3: ONE added field, `src=<lineKind>/<paint>`:
      //   lineKind = show|skip|rails|rec|g16|sweep|other — which panel journal line this is. One
      //     show() emits `[panel] show mode=.. ms=..` AND `[panel] rails tps=.. pgood=.. err=..`,
      //     and only the first carries mode=/ms=, so a second-kind row repeats the previous
      //     drive's mode/err/ms. A non-`show` kind does NOT mean a second waveform.
      //   paint    = `p<N>/R:<activity>` for an ActivityManager render-task paint (join to
      //     `[ACT] paint <activity> ... p=N`), or `p<N>/M` for any other task (Activity::onEnter
      //     first render, a follow-up request, the USB shell). Same N = same displayBuffer() call.
      // Both halves are pure reads of state that already exists; the drive above is unchanged.
      //
      // r115: AN INHERITED FIELD IS NOW MARKED. A line with no `mode=`/`ms=` still reuses the
      // previous show()'s values (deliberately: the rails/pg lines belong to the same drive), but a
      // row like `ms=765` that was never this line's own duration is exactly how a stale entry got
      // read as a second real drive. The field is now `? (inherited)` / `<mode>(inherited)` on the
      // lines that did not carry it, so a real drive is the only row with a bare number. This
      // changes the FORMAT, not the counting: g_wakeDriveN and drives=N are untouched.
      char modeField[40];
      char msField[28];
      if (strstr(line, "mode=") != nullptr) {
        snprintf(modeField, sizeof(modeField), "%s", g_panelDriveMode);
      } else {
        snprintf(modeField, sizeof(modeField), "%s(inherited)", g_panelDriveMode);
      }
      if (msTok != nullptr) {
        snprintf(msField, sizeof(msField), "%lu", static_cast<unsigned long>(g_panelDriveMs));
      } else {
        snprintf(msField, sizeof(msField), "? (inherited)");
      }
      LOG_INF("WAKE", "drive %lu mode=%s err=0x%x tps=%d ms=%s (+%lu ms after resume) src=%s/%s",
              static_cast<unsigned long>(g_wakeDriveN), modeField, static_cast<unsigned>(g_panelDriveErr),
              g_panelDriveTps, msField, static_cast<unsigned long>(millis() - g_wakeCaptureStartMs), lineKind,
              cp_paint_origin_text());
    }
  }
  // Also kept in RAM for the `panel` command, so the last RESOLVED mode and its
  // TPS verdict are readable without pulling the card. This runs on the render
  // task (inside show()); a reader can therefore see a half-updated string —
  // harmless for a diagnostic line, and cheaper than a lock on the paint path.
  usbshell::notePanelJournal(line);
  if (s_journalPanelLines) sleepJournalCStr(line);
}

// App-log sink for vendor/render-layer diagnostics (the epdiy render path, which is C and cannot
// include Logging.h). The operator's captured log is the app logger, not the ESP-IDF console, so a
// render.c ESP_LOGI("[mask] ...") is invisible in it; this is the reachable route. Deliberately
// LOG_INF only -- no SD write per paint, the sleep journal already costs one flush per show().
extern "C" void cp_panel_note(const char* line) { LOG_INF("PANEL", "%s", line); }

// ---------------------------------------------------------------------------
// CP_USB_SHELL — the UI half of the debug channel (docs/engineering/USB_DEBUG_CHANNEL.md §3.2).
//
// Everything below runs inside usbshell::drain(), which is called from exactly
// one place: the top of loop(), before activityManager.loop(). That single
// drain point is what makes the channel safe — a command is serialized against
// every gesture, page turn and sleep decision, and the shell task never holds a
// RenderLock, never calls an Activity, and never touches the framebuffer.
//
// `reboot` is the one command that stops the world, so it does not: it arms a
// deadline and loop() acts on it one iteration later, after the response has
// been flushed to the host.
// ---------------------------------------------------------------------------
#if CP_USB_SHELL
namespace {

unsigned long g_shellRebootAtMs = 0;

int shellPanel(char* out, size_t cap) { return panelprobe::panelText(out, cap); }

int shellPmic(char* out, size_t cap) { return panelprobe::pmicText(out, cap); }

// E5 (docs/engineering/HARDWARE_POWER_PLAN.md §5.4): `touch status | touch recover`.
// The ladder itself lives in InputManager (the same one boot and wake use); this
// handler only reports what it latched. Nothing here writes settings or NVS --
// touch capability is re-probed every boot and the latch is RAM-only.
int shellTouch(const char* args, char* out, size_t cap) {
  if (args == nullptr || args[0] == '\0' || strcmp(args, "status") == 0) {
    snprintf(out, cap, "touch: present=%d last_attempts=%d\n",
             gpio.hasTouch() ? 1 : 0, gpio.lastTouchProbeAttempts());
    snprintf(out + strlen(out), cap - strlen(out),
             "       latch: InputManager touchDataEnabled (RAM only; no NVS/settings key records touch)\n");
    return 0;
  }
  if (strcmp(args, "recover") != 0) {
    snprintf(out, cap, "touch: usage: touch status | touch recover\n");
    return -EINVAL;
  }
  const unsigned long t0 = millis();
  const bool ok = gpio.recoverTouch();
  snprintf(out, cap, "touch: recover -> present=%d attempts=%d ms=%lu\n", ok ? 1 : 0,
           gpio.lastTouchProbeAttempts(), millis() - t0);
  snprintf(out + strlen(out), cap - strlen(out),
           "       rail P5 cycle + TP_RST pulse only after a failed first probe; nothing persisted\n");
  return ok ? 0 : -EIO;
}

// Same repaint paths the UI uses: a bare `refresh` is exactly ActivityManager's
// requestUpdate() (RefreshPolicy decides), and an explicit mode is the one-shot
// promotion the Debug page's probe rows use, followed by the same request.
int shellRefresh(const char* mode, char* out, size_t cap) {
  if (mode == nullptr || mode[0] == '\0' || strcmp(mode, "auto") == 0) {
    activityManager.requestUpdate();
    snprintf(out, cap, "refresh: requested; RefreshPolicy picks the waveform\n");
    return 0;
  }
  HalDisplay::RefreshMode m;
  if (strcmp(mode, "full") == 0) {
    m = HalDisplay::FULL_REFRESH;
  } else if (strcmp(mode, "half") == 0) {
    m = HalDisplay::HALF_REFRESH;
  } else if (strcmp(mode, "fast") == 0) {
    m = HalDisplay::FAST_REFRESH;
  } else {
    snprintf(out, cap, "refresh: unknown mode '%s' (full|half|fast|auto)\n", mode);
    return -EINVAL;
  }
  renderer.promoteNextRefresh(m);
  activityManager.requestUpdate(true);
  snprintf(out, cap, "refresh: %s promoted and repaint requested (policy still owns HALF/FULL choice)\n", mode);
  return 0;
}

#if CP_EXP
// ---------------------------------------------------------------------------
// r101.21 EXP1 — `exp`, the one command family for the batch's experiment arms.
//
// Runs on the MAIN task (Kind::Exp) because `exp vid` drives the panel and the
// render/panel lock belongs to this task; `status` only reads RAM but shares the
// entry point so the two can never disagree about an arm's value.
//
// THE RULE EVERY ARM OBEYS (and the reason this command is safe to put in a
// shipping image): the arm is a bool in RAM whose static initialiser is the
// behaviour r101.20 already had. Nothing is persisted. A reboot is the reset.
// `exp status` prints exactly what the code path will do, one line per arm.
// ---------------------------------------------------------------------------
int shellExp(const char* args, char* out, size_t cap) {
  char buf[64];
  buf[0] = '\0';
  if (args != nullptr) {
    // Keep the whole sub-command line; the longest legal one is "row9 off".
    const size_t n = strlen(args);
    const size_t k = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
    memcpy(buf, args, k);
    buf[k] = '\0';
  }
  char* save = nullptr;
  char* tok = strtok_r(buf, " \t", &save);
  if (tok == nullptr || strcmp(tok, "status") == 0) {
    // One greppable line per arm, then one per park sink (Build A E4c-E4f). `exp status` is
    // the boot identity check for the whole batch, so the sink arms are part of it: a sink
    // reading `off` is the arm a coulomb window is charged to, not an accident.
    const size_t n = exparms::status(out, cap);
    if (n < cap) parksink::status(out + n, cap - n);  // one greppable line per arm; this IS the boot identity check
    return 0;
  }
  if (strcmp(tok, "clear") == 0) {
    exparms::clear();
    parksink::clearAll();
    snprintf(out, cap, "exp: cleared -- every arm and park sink back to its build default (a reboot does the same)\n");
    const size_t used = strlen(out);
    if (used < cap) parksink::status(out + used, cap - used);
    return 0;
  }
  if (strcmp(tok, "park") == 0) {
    // BUILD A E4c-E4f: `exp park usb=0|1 front=0|1 radio=0|1 sd=0|1` (any number, one line).
    // Refused while the device is parked: an arm changed half way through a park describes
    // neither A nor B, exactly like the `trim` refusal.
    if (standbytrim::parked()) {
      snprintf(out, cap, "exp park: REFUSED -- the device is parked (standby in progress); wake it first.\n");
      return -EBUSY;
    }
    char* val = strtok_r(nullptr, " \t", &save);
    if (val == nullptr || strcmp(val, "status") == 0) {
      // r109 EXP2: `exp park` with NO args answers with the four compact sink lines only -- the
      // full form (with each sink's help sentence) overflows the shell's ~1080 B body cap before
      // `radio` and `sd` arrive, which is exactly how H1's radio/sd half went UNREAD on r108.
      // `exp park status` still prints the full form, byte-for-byte as before.
      if (val != nullptr) {
        parksink::status(out, cap);
      } else {
        parksink::statusCompact(out, cap);
      }
      return 0;
    }
    int changed = 0;
    for (; val != nullptr; val = strtok_r(nullptr, " \t", &save)) {
      const char* eq = strchr(val, '=');
      const size_t nameLen = eq == nullptr ? 0 : static_cast<size_t>(eq - val);
      bool found = false;
      parksink::Sink s = parksink::Sink::Usb;
      for (size_t i = 0; i < static_cast<size_t>(parksink::Sink::Count); ++i) {
        const parksink::Sink cand = static_cast<parksink::Sink>(i);
        if (nameLen == strlen(parksink::name(cand)) && strncmp(val, parksink::name(cand), nameLen) == 0) {
          s = cand;
          found = true;
          break;
        }
      }
      if (!found || eq == nullptr || (eq[1] != '0' && eq[1] != '1') || eq[2] != '\0') {
        snprintf(out, cap, "exp park: bad item '%s' (want usb=0|1 front=0|1 radio=0|1 sd=0|1)\n", val);
        return -EINVAL;
      }
      parksink::setOverride(s, eq[1] == '1');
      ++changed;
    }
    snprintf(out, cap, "exp: %d park sink arm(s) set for this session (RAM only -- a reboot returns to the build default)\n",
             changed);
    const size_t used = strlen(out);
    if (used < cap) parksink::status(out + used, cap - used);
    return 0;
  }
  if (strcmp(tok, "row9") == 0) {
    const char* val = strtok_r(nullptr, " \t", &save);
    if (val == nullptr || (strcmp(val, "on") != 0 && strcmp(val, "off") != 0)) {
      snprintf(out, cap, "usage: exp row9 on|off   (EDRV1; `off` suppresses the row-9 completion drive)\n");
      return -EINVAL;
    }
    const bool on = strcmp(val, "on") == 0;
    exparms::set(exparms::Arm::Row9Completion, on);
    snprintf(out, cap,
             "exp: row9 completion drive %s (EDRV1; RAM only -- a reboot returns to the build default)\n",
             on ? "ON (r101.20 behaviour)" : "OFF for the rest of this session");
    return 0;
  }
  if (strcmp(tok, "flpanel") == 0) {
    const char* val = strtok_r(nullptr, " \t", &save);
    if (val == nullptr || (strcmp(val, "on") != 0 && strcmp(val, "off") != 0)) {
      snprintf(out, cap,
               "usage: exp flpanel on|off   (FLP1; `off` restores the pre-fix panel repaints: "
               "adjustments follow the in-screen policy and re-drive the row-9 completion, "
               "and leaving the panel arms no clean)\n");
      return -EINVAL;
    }
    const bool on = strcmp(val, "on") == 0;
    exparms::set(exparms::Arm::FrontlightPanelPartial, on);
    snprintf(out, cap,
             "exp: frontlight panel partial repaints %s (FLP1; RAM only -- a reboot returns to "
             "the build default)\n",
             on ? "ON (shipped behaviour: one FAST/DU per adjustment, one clean on exit)"
                : "OFF for the rest of this session (pre-fix behaviour)");
    return 0;
  }
  if (strcmp(tok, "vid") == 0) {
    // VID1: select the built-but-never-used 8-phase table for N FAST paints. The
    // duty stops itself at N -- a bench lane must not be left armed, because
    // sustained DC-imbalanced drive is the one irreversible failure class.
    int frames = 10;
    const char* val = strtok_r(nullptr, " \t", &save);
    if (val != nullptr) frames = static_cast<int>(strtol(val, nullptr, 10));
    if (frames < 1 || frames > 120) {
      snprintf(out, cap, "exp vid: frames must be 1..120 (default 10; 60 = ~6 s of glass)\n");
      return -EINVAL;
    }
    exparms::vidArm(static_cast<uint16_t>(frames));
    snprintf(out, cap,
             "exp vid: armed %d frame(s) of the 8-phase short-DU table (auto-stops at 0; "
             "each frame is a whole-panel drive, one `[VID] frame i/N ms=` line per frame)\n",
             frames);
    return 0;
  }
  if (strcmp(tok, "simd") == 0) {
    int iterations = 200;
    const char* val = strtok_r(nullptr, " \t", &save);
    if (val != nullptr) iterations = static_cast<int>(strtol(val, nullptr, 10));
    const dotprodb::Result r = dotprodb::run(iterations);
    if (!r.built) {
      snprintf(out, cap, "exp simd: esp-dsp is not on this build's include path\n");
      return -ENODEV;
    }
    // SIMD1 (r103): the bench owns the verdict. It prints one line per shape with
    // ref/kernel/ansi values and the timing pair, and prints a ratio ONLY when all
    // three agreed on every shape -- otherwise it names the disagreeing shape and
    // withholds the ratio.
    dotprodb::formatReport(r, out, cap);
    return 0;
  }
  snprintf(out, cap,
           "exp: unknown sub-command (status | clear | row9 on|off | flpanel on|off | vid [n] | "
           "simd [n] | park [usb=0|1 front=0|1 radio=0|1 sd=0|1])\n");
  return -EINVAL;
}
#endif

// The framebuffer is 1 BPP (bit set = white, MSB first) — that is what the
// renderer draws into and what the panel shim consumes. saveFramebufferAsBmp()
// writes a 1-bpp BMP of the LOGICAL page (rotated out of the physical frame through
// the live orientation), 684x1216 in portrait -> 88 B/row * 1216 rows + 62 B header.
// 8 bpp would be a 4x larger file invented from the same bits: the 1-bpp buffer
// carries no grey information, so a wider format would only waste the pipe.
int shellShot(const char* path, char* out, size_t cap) {
  if (activityManager.requiresExclusiveStorageLoop()) {
    snprintf(out, cap, "shot: storage is handed to the USB host; the SD volume is detached (EBUSY)\n");
    return -EBUSY;
  }
  const uint8_t* fb = renderer.getFrameBuffer();
  if (fb == nullptr) {
    snprintf(out, cap, "shot: framebuffer not allocated (lent to a chapter build?)\n");
    return -ENODEV;
  }
  char file[128];
  if (path != nullptr && path[0] != '\0') {
    snprintf(file, sizeof(file), "%s%s", path[0] == '/' ? "" : "/", path);
  } else {
    snprintf(file, sizeof(file), "/screenshots/shell-%lu.bmp", static_cast<unsigned long>(millis()));
  }
  bool ok = false;
  {
    // Blocking by design: the render task may be mid-frame, and the frame on
    // the glass is worth waiting for. Bounded by the panel's ~1.8 s worst case,
    // well inside the 30 s mailbox timeout this command declares.
    RenderLock lock;
    ok = ScreenshotUtil::saveFramebufferAsBmp(file, fb, renderer.getDisplayWidth(), renderer.getDisplayHeight(),
                                              renderer.getOrientation());
  }
  if (!ok) {
    snprintf(out, cap, "shot: write failed for %s (card present? directory writable?)\n", file);
    return -EIO;
  }
  LOG_INF("SHELL", "shot -> %s", file);
  snprintf(out, cap, "shot: %s (1 bpp, %dx%d, from the CURRENT framebuffer)\n", file, renderer.getDisplayWidth(),
           renderer.getDisplayHeight());
  return 0;
}

int shellActivity(char* out, size_t cap) {
  const char* names[12] = {};
  const int n = activityManager.activityStackNames(names, 12);
  char body[420];
  body[0] = '\0';
  for (int i = 0; i < n; ++i) {
    const size_t len = strlen(body);
    if (len + 1 >= sizeof(body)) break;
    snprintf(body + len, sizeof(body) - len, "%s%s", i == 0 ? "" : " > ", names[i] ? names[i] : "?");
  }
  snprintf(out, cap, "stack: %s\nreader=%d foreground_reader=%d visit=%lu\n", n ? body : "(none)",
           activityManager.isReaderActivity() ? 1 : 0, activityManager.isForegroundReader() ? 1 : 0,
           static_cast<unsigned long>(activityManager.visit()));
  return 0;
}

int shellSpine(char* out, size_t cap) {
  const ScreenshotInfo info = activityManager.getScreenshotInfo();
  const char* kind = info.readerType == ScreenshotInfo::ReaderType::Epub  ? "epub"
                     : info.readerType == ScreenshotInfo::ReaderType::Txt ? "txt"
                     : info.readerType == ScreenshotInfo::ReaderType::Xtc ? "xtc"
                                                                          : "none";
  snprintf(out, cap,
           "book: %s\ntype: %s\nspine: %d (0-based; +1 = chapter %d)\npage: %d / %d\nprogress: %d%%\n",
           info.title[0] ? info.title : "(no book)", kind, info.spineIndex, info.spineIndex + 1, info.currentPage,
           info.totalPages, info.progressPercent);
  return 0;
}


#if CP_LP_BENCH
// ---------------------------------------------------------------------------
// r96 `lpsleep [sec]` — ONE real light-sleep slice with the LP core running.
//
// Mailbox kind (Kind::LpSleep) because it puts the HP core down: it must be
// serialized behind any in-flight render, and it must ANSWER BEFORE the device
// sleeps. So this handler only validates and ARMS: lpcore::requestSleepSlice()
// sets a ~0.8 s deadline and loop() runs the slice from
// lpcore::pollSleepSlice() once this response is on the wire.
// ---------------------------------------------------------------------------
int shellLpSleep(const char* args, char* out, size_t cap) {
#if !CP_LP_BENCH
  snprintf(out, cap, "lpsleep: not built (CP_LP_BENCH=0)\n");
  return -ENOSYS;
#else
  uint32_t sec = 60;
  if (args != nullptr && args[0] != '\0') {
    const long v = strtol(args, nullptr, 10);
    if (v < 0 || v > 60) {
      snprintf(out, cap, "lpsleep: sec must be 5..60 (0 = the 60 s default)\n");
      return -EINVAL;
    }
    if (v > 0) sec = static_cast<uint32_t>(v);
  }
  if (!lpcore::requestSleepSlice(sec)) {
    const lpcore::SleepSlice& r = lpcore::lastSleepSlice();
    snprintf(out, cap, "lpsleep: REFUSED — %s\n", r.reason ? r.reason : "unknown reason");
    return -EBUSY;
  }
  const uint32_t clamped = lpcore::lastSleepSlice().sec;
  snprintf(out, cap,
           "lpsleep: armed %lu s, runs in ~1 s (this answer goes out first)\n"
           "  wake sources: GPIO2 (expander INT) EXT1 ANY_LOW + an RTC timer, re-armed per chunk\n"
           "  BOUND: no single HP sleep call is armed for more than 5 s; worst case is 5 s + settle\n"
           "  r96.2: the LP bench program is sampled, STOPPED for the slice, sampled again after it\n"
           "  the panel is NOT parked and no rail is dropped: this measures light sleep, not standby\n"
           "  after the wake: `logtail 40` for the result (every phase is flushed to the card first)\n",
           static_cast<unsigned long>(clamped));
  LOG_INF("TEST", "lpsleep: armed %lu s at t=%lu ms", static_cast<unsigned long>(clamped),
          static_cast<unsigned long>(millis()));
  return 0;
#endif
}
#endif  // CP_LP_BENCH

#if CP_PPA_BENCH
// ---------------------------------------------------------------------------
// r96 `ppabench [op] [w] [h] [src]` — one FULL-FRAME PPA operation against a
// byte-exact software reference.
//
// Always a mailbox kind: the `fb` source must be read under RenderLock on the
// main task, and one code path for both sources is one less invariant to get
// wrong. For `src=fb` the geometry is the framebuffer's own: 152 bytes x 684
// rows = 103968 bytes, treated as GRAY8 (w=152, h=684) — see PpaBuffer.h for why
// there is no sub-rectangle mode.
// ---------------------------------------------------------------------------
int shellPpaBench(const char* args, char* out, size_t cap) {
#if !CP_PPA_BENCH
  snprintf(out, cap, "ppabench: not built (CP_PPA_BENCH=0)\n");
  return -ENOSYS;
#else
  // Parse: op [w] [h] [src]
  char op[16] = "copy";
  char srcTok[8] = "buf";
  int w = 0, h = 0;
  {
    char buf[112];
    snprintf(buf, sizeof(buf), "%s", args ? args : "");
    char* tok[5] = {};
    int n = 0;
    for (char* t = strtok(buf, " "); t != nullptr && n < 5; t = strtok(nullptr, " ")) tok[n++] = t;
    if (n >= 1) snprintf(op, sizeof(op), "%s", tok[0]);
    if (n >= 2) w = static_cast<int>(strtol(tok[1], nullptr, 10));
    if (n >= 3) h = static_cast<int>(strtol(tok[2], nullptr, 10));
    if (n >= 4) snprintf(srcTok, sizeof(srcTok), "%s", tok[3]);
  }

  ppa::BenchOp bop = ppa::BenchOp::Copy;
  if (strcmp(op, "copy") == 0) {
    bop = ppa::BenchOp::Copy;
  } else if (strcmp(op, "rot90") == 0) {
    bop = ppa::BenchOp::Rot90;
  } else if (strcmp(op, "rot180") == 0) {
    bop = ppa::BenchOp::Rot180;
  } else if (strcmp(op, "rot270") == 0) {
    bop = ppa::BenchOp::Rot270;
  } else if (strcmp(op, "mirror") == 0 || strcmp(op, "mirror_x") == 0) {
    bop = ppa::BenchOp::MirrorX;
  } else if (strcmp(op, "scale") == 0 || strcmp(op, "scale0.5") == 0) {
    bop = ppa::BenchOp::ScaleHalf;
  } else {
    snprintf(out, cap, "ppabench: unknown op '%s' (copy|rot90|rot180|rot270|mirror|scale)\n", op);
    return -EINVAL;
  }
  const bool fromFb = (strcmp(srcTok, "fb") == 0);
  if (!fromFb && strcmp(srcTok, "buf") != 0) {
    snprintf(out, cap, "ppabench: src must be 'buf' (allocated pattern) or 'fb' (live framebuffer)\n");
    return -EINVAL;
  }

  ppa::BenchResult r;
  if (fromFb) {
    // R2 (plan §6): the framebuffer is single-writer by contract. Refuse when the
    // render task holds it rather than queueing behind a page paint.
    if (RenderLock::peek()) {
      snprintf(out, cap, "ppabench: REFUSED — the render task holds RenderLock; retry when the screen is idle\n");
      return -EBUSY;
    }
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) {
      snprintf(out, cap, "ppabench: REFUSED — could not take RenderLock\n");
      return -EBUSY;
    }
    const uint8_t* fb = renderer.getFrameBuffer();
    if (fb == nullptr) {
      snprintf(out, cap, "ppabench: REFUSED — the framebuffer is not allocated (lent to a chapter build?)\n");
      return -ENODEV;
    }
    // The framebuffer's own geometry, in BYTES: 1216 px / 8 = 152 per row, 684 rows.
#if defined(CP_RENDER_4BPP) && CP_RENDER_4BPP
    (void)fb;
    // 4 bpp canvas: 608 B/row. The PPA floors at GRAY8/RGB565 (1 and 2 B/px), so
    // there is no colour mode that addresses a nibble-per-pixel buffer — running
    // this bench against it would measure a mis-typed buffer and print a plausible
    // number. Refuse instead; the buffer mode still works.
    snprintf(out, cap,
             "ppabench: REFUSED — the framebuffer is 4 bpp (CP_RENDER_4BPP, %d B/row); the PPA has no "
             "nibble-per-pixel colour mode. Use the buffer mode.\n",
             renderer.getDisplayWidth() / 2);
    return -EINVAL;
#endif
    const int fbW = renderer.getDisplayWidth() / 8;
    const int fbH = renderer.getDisplayHeight();
    if (w > 0 && (w != fbW || h != fbH)) {
      snprintf(out, cap,
               "ppabench: fb mode is FULL-FRAME only — the framebuffer is %dx%d bytes (%d B), not %dx%d.\n"
               "          A sub-rectangle would need a cache-line-legal stride, and 152 B is not.\n",
               fbW, fbH, fbW * fbH, w, h);
      return -EINVAL;
    }
    r = ppa::benchFullFrame(bop, fbW, fbH, fb);
  } else {
    if (w <= 0) w = 608;   // half the panel: big enough to be a DMA burst, small enough to fit 3 buffers
    if (h <= 0) h = 342;
    r = ppa::benchFullFrame(bop, w, h, nullptr);
  }

  snprintf(out, cap,
           "ppabench: op=%s src=%s %dx%d -> %dx%d @%dB/px\n"
           "  flashEnc=%s  inPsram=%d outPsram=%d\n"
           "  ran=%d reason=%s\n"
           "  ppa : %8lld us  %7.2f MB/s\n"
           "  soft: %8lld us  %7.2f MB/s\n"
           "  exact=%s (%lu/%lu bytes differ)\n"
           "  speedup=%.2fx  driverErr=%ld\n",
           r.opName, fromFb ? "fb" : "buf", r.w, r.h, r.dw, r.dh, r.bytesPerPixel, r.flashEnc ? "on" : "off",
           static_cast<int>(r.inPsram), static_cast<int>(r.outPsram), static_cast<int>(r.ran),
           r.reason ? r.reason : "(none)", static_cast<long long>(r.ppaUs), r.ppaMbps,
           static_cast<long long>(r.softUs), r.softMbps,
           r.compared ? (r.exact ? "YES" : "NO") : "n/a", static_cast<unsigned long>(r.mismatchBytes),
           static_cast<unsigned long>(r.comparedBytes),
           (r.softUs > 0 && r.ppaUs > 0) ? (static_cast<double>(r.softUs) / static_cast<double>(r.ppaUs)) : 0.0,
           static_cast<long>(r.driverErr));
  LOG_INF("TEST", "ppabench %s %s %dx%d: ran=%d ppa=%lldus soft=%lldus exact=%d mism=%lu", r.opName,
          fromFb ? "fb" : "buf", r.w, r.h, static_cast<int>(r.ran), static_cast<long long>(r.ppaUs),
          static_cast<long long>(r.softUs), static_cast<int>(r.exact), static_cast<unsigned long>(r.mismatchBytes));
  return r.ran ? 0 : -EIO;
#endif
}

// ---------------------------------------------------------------------------
// PPA1 `ppaframe` — the RENDERER's own buffer, both ways.
//
//   ppaframe                 the plan for the live canvas: may the SRM engine
//                            address it, and what would a widening buffer cost
//   ppaframe cmp [op]        run the CPU transform AND the widen->PPA->narrow
//                            sandwich over the live canvas; report byte-exactness
//                            and the timing of each stage
//   ppaframe time [op] [n]   the same measurement n times (default 3, max 10),
//                            reporting the MIN of each stage
//
// `op` is copy | rot180 | mirror (default copy). The CPU side is the transform
// the sandwich would replace, so a sandwich that loses to a table-driven nibble
// loop has nothing left to offer — which is the answer this command exists to
// produce on the device rather than by assertion.
//
// Costs up to ~2.5 MB of PSRAM while it runs (two GRAY8 buffers + two 4 bpp
// outputs). Nothing is written to the canvas: it is read under RenderLock and
// released, so the screen is untouched and nothing is driven.
// ---------------------------------------------------------------------------
int shellPpaFrame(const char* args, char* out, size_t cap) {
#if !CP_PPA_BENCH
  snprintf(out, cap, "ppaframe: not built (CP_PPA_BENCH=0)\n");
  return -ENOSYS;
#else
  char sub[16] = "plan";
  char opTok[16] = "copy";
  int reps = 3;
  {
    char buf[112];
    snprintf(buf, sizeof(buf), "%s", args ? args : "");
    char* tok[4] = {};
    int n = 0;
    for (char* t = strtok(buf, " "); t != nullptr && n < 4; t = strtok(nullptr, " ")) tok[n++] = t;
    if (n >= 1) snprintf(sub, sizeof(sub), "%s", tok[0]);
    if (n >= 2) snprintf(opTok, sizeof(opTok), "%s", tok[1]);
    if (n >= 3) reps = static_cast<int>(strtol(tok[2], nullptr, 10));
  }

  ppa::FrameOp fop = ppa::FrameOp::Copy;
  if (strcmp(opTok, "copy") == 0) {
    fop = ppa::FrameOp::Copy;
  } else if (strcmp(opTok, "rot180") == 0 || strcmp(opTok, "rot") == 0) {
    fop = ppa::FrameOp::Rot180;
  } else if (strcmp(opTok, "mirror") == 0 || strcmp(opTok, "mirror_x") == 0) {
    fop = ppa::FrameOp::MirrorX;
  } else {
    snprintf(out, cap, "ppaframe: unknown op '%s' (copy|rot180|mirror)\n", opTok);
    return -EINVAL;
  }
  if (reps < 1) reps = 1;
  if (reps > 10) reps = 10;
  if (strcmp(sub, "plan") != 0 && strcmp(sub, "cmp") != 0 && strcmp(sub, "time") != 0) {
    snprintf(out, cap, "ppaframe: unknown sub '%s' (plan|cmp|time)\n", sub);
    return -EINVAL;
  }

  if (RenderLock::peek()) {
    snprintf(out, cap, "ppaframe: REFUSED — the render task holds RenderLock; retry when the screen is idle\n");
    return -EBUSY;
  }
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) {
    snprintf(out, cap, "ppaframe: REFUSED — could not take RenderLock\n");
    return -EBUSY;
  }
  const uint8_t* fb = renderer.getFrameBuffer();
  if (fb == nullptr) {
    snprintf(out, cap, "ppaframe: REFUSED — the framebuffer is not allocated (lent to a chapter build?)\n");
    return -ENODEV;
  }
  const int w = renderer.getDisplayWidth();
  const int h = renderer.getDisplayHeight();
  const int rowBytes = renderer.getDisplayWidthBytes();

#if !CP_RENDER_4BPP
  snprintf(out, cap,
           "ppaframe: REFUSED — this image draws a 1 bpp canvas (CP_RENDER_4BPP=0, %d B/row). The sandwich "
           "is defined for the 4 bpp nibble canvas; 1 bpp -> GRAY8 is an 8x widen, a different question. "
           "Rebuild with CP_RENDER_4BPP=1.\n",
           rowBytes);
  return -EINVAL;
#else
  const ppa::SrmOp sop = (fop == ppa::FrameOp::MirrorX)  ? ppa::SrmOp::MirrorX
                         : (fop == ppa::FrameOp::Rot180) ? ppa::SrmOp::Rot180
                                                         : ppa::SrmOp::Copy;

  // ---- plan -------------------------------------------------------------
  if (strcmp(sub, "plan") == 0) {
    ppa::SrmRequest req;
    req.format = ppa::SrmFormat::Packed4;
    req.op = sop;
    req.width = static_cast<uint16_t>(w);
    req.height = static_cast<uint16_t>(h);
    req.srcStride = static_cast<uint32_t>(rowBytes);
    req.dstAddr = reinterpret_cast<uintptr_t>(fb);
    req.dstBytes = static_cast<size_t>(rowBytes) * h;
    req.outAlign = ppa::Buffer::alignment();
    req.external = true;
    req.flashEncryption = esp_flash_encryption_enabled();
    req.ppaPresent = true;
    const ppa::SrmPlan p = ppa::planSrm(req);

    // And the same geometry as a byte-addressable surface, so the operator sees
    // that the refusal is about the FORMAT and not about this board.
    ppa::SrmRequest g = req;
    g.format = ppa::SrmFormat::Gray8;
    g.dstAddr = 0x4FF00000u;  // a cache-line-legal stand-in: this half is hypothetical
    g.dstBytes = (static_cast<size_t>(w) * h + 63u) & ~size_t{63};
    const ppa::SrmPlan gp = ppa::planSrm(g);

    snprintf(out, cap,
             "ppaframe plan: canvas %dx%d, 4 bpp, %d B/row, %lu B, addr=%08lx\n"
             "  op=%s format=packed4  SERVED=%s  reason=%s\n"
             "  widening a packed canvas would need %lu B of GRAY8 (%.2fx the canvas)\n"
             "  same geometry as gray8: SERVED=%s reason=%s  (hypothetical: we have no GRAY8 surface)\n"
             "  flashEnc=%s  cacheLine=%u B\n"
             "  => the render path stays on the CPU. `ppaframe cmp` measures what the sandwich would cost.\n",
             w, h, rowBytes, static_cast<unsigned long>(static_cast<size_t>(rowBytes) * h),
             static_cast<unsigned long>(reinterpret_cast<uintptr_t>(fb)), ppa::opName(sop),
             p.served ? "YES" : "NO", ppa::refusalName(p.why), static_cast<unsigned long>(p.widenBytes),
             (static_cast<double>(p.widenBytes) / static_cast<double>(static_cast<size_t>(rowBytes) * h)),
             gp.served ? "YES" : "NO", ppa::refusalName(gp.why), req.flashEncryption ? "ON" : "off",
             static_cast<unsigned>(req.outAlign));
    return 0;
  }

  // ---- cmp / time -------------------------------------------------------
  const int runs = (strcmp(sub, "time") == 0) ? reps : 1;
  ppa::FrameCmpResult first;
  ppa::FrameCmpResult last;
  int64_t minCpu = 0, minWiden = 0, minPpa = 0, minNarrow = 0;
  bool anyRan = false;

  for (int i = 0; i < runs; ++i) {
    ppa::FrameCmpResult r;
    if (!ppa::frameCompare(fop, fb, w, h, r)) {
      last = r;
      break;
    }
    anyRan = true;
    last = r;
    if (i == 0) {
      first = r;
      minCpu = r.cpuUs;
      minWiden = r.widenUs;
      minPpa = r.ppaUs;
      minNarrow = r.narrowUs;
    } else {
      if (r.cpuUs < minCpu) minCpu = r.cpuUs;
      if (r.widenUs < minWiden) minWiden = r.widenUs;
      if (r.ppaUs < minPpa) minPpa = r.ppaUs;
      if (r.narrowUs < minNarrow) minNarrow = r.narrowUs;
    }
  }

  if (!anyRan) {
    snprintf(out, cap,
             "ppaframe %s %s: SKIPPED — %s\n  plan for this canvas: %s\n", sub, ppa::frameOpName(fop),
             last.reason ? last.reason : "no reason recorded",
             (last.refusedBecause && last.refusedBecause[0]) ? last.refusedBecause : "not computed");
    return -EIO;
  }

  const int64_t sandwich = minWiden + minPpa + minNarrow;
  const double ratio = sandwich > 0 ? static_cast<double>(sandwich) / static_cast<double>(minCpu > 0 ? minCpu : 1)
                                    : 0.0;

  snprintf(out, cap,
           "ppaframe %s %s: canvas %dx%d, 4 bpp, %lu B, runs=%d\n"
           "  plan: SERVED=%s reason=%s (the engine cannot address this buffer; the sandwich is the only way)\n"
           "  pixels: %s (%lu of %lu canvas bytes differ from the CPU transform)\n"
           "  CPU  transform            %8lld us   (table-driven nibble %s)\n"
           "  PPA  widen 4bpp->gray8    %8lld us\n"
           "  PPA  srm call + msync     %8lld us\n"
           "  PPA  narrow gray8->4bpp   %8lld us\n"
           "  PPA  sandwich total       %8lld us   = %.2fx the CPU path\n"
           "  inPsram=%d flashEnc=%s scratch=%lu B driverErr=%ld\n",
           ppa::frameOpName(fop), sub, w, h, static_cast<unsigned long>(first.frameBytes), runs,
           first.planned ? "YES" : "NO", first.refusedBecause,
           first.identical ? "BYTE-IDENTICAL" : "DIFFERENT", static_cast<unsigned long>(last.differingBytes),
           static_cast<unsigned long>(last.comparedBytes), static_cast<long long>(minCpu),
           ppa::frameOpName(fop), static_cast<long long>(minWiden), static_cast<long long>(minPpa),
           static_cast<long long>(minNarrow), static_cast<long long>(sandwich), ratio,
           static_cast<int>(first.inPsram), first.flashEnc ? "ON" : "off",
           static_cast<unsigned long>(first.scratchBytes), static_cast<long>(last.driverErr));
  LOG_INF("TEST", "ppaframe %s %s %dx%d: identical=%d mism=%lu cpu=%lldus widen=%lldus ppa=%lldus narrow=%lldus",
          ppa::frameOpName(fop), sub, w, h, static_cast<int>(first.identical),
          static_cast<unsigned long>(last.differingBytes), static_cast<long long>(minCpu),
          static_cast<long long>(minWiden), static_cast<long long>(minPpa), static_cast<long long>(minNarrow));
  return 0;
#endif  // CP_RENDER_4BPP
#endif  // CP_PPA_BENCH
}
#endif  // CP_PPA_BENCH

void shellReboot(char* out, size_t cap) {
  g_shellRebootAtMs = millis() + 1000;
  LOG_INF("SHELL", "reboot requested by the USB shell: 1 s");
  snprintf(out, cap, "reboot: restarting in 1 s (silent restart to Home)\n");
}

}  // namespace
#endif  // CP_USB_SHELL

// Rails-first wake settle. Deliberate dead time after repower while the TPS65185 comes up; shared by
// the in-place standby resume and the auto-off path's wake-before-paint.
static constexpr unsigned long KWAKE_SETTLE_MS = 250;

// Throwaway FULL drive: the drive that eats the post-poweron banding (W22, docs/R55_R57_WAKE.md).
// Factored out in r82 so the auto-off path and the (switch-gated) standby resume cannot drift apart
// -- both must spend exactly one whole-panel from-white GC16 on the frame that is about to be
// replaced. Returns the drive duration in ms for the journal.
static unsigned long metalioWakePrimeDrive(GfxRenderer& renderer) {
  renderer.cancelPromotedRefresh();
  const unsigned long start = millis();
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  renderer.waitRefreshComplete();
  return millis() - start;
}

// Hard power-off: the vendor's PwrOffTask. The PMIC counts edges on expander P1.3 and collapses
// the 3.3 V rail, so this never returns -- and that is the point.
//
// It PAINTS NOTHING unless `wakeAndPaintFirst` is set. Three callers reach it: the confirmation
// dialog, which has already committed "Powered off" as a FULL drive and waited for it; standby paths
// where the glass is whatever it already was; and the standby auto-off timer, which asks for
// wakeAndPaintFirst. Re-driving unconditionally here (r48 did: repower + fillRect + displayBuffer +
// 300 ms settle) meant the panel took a second waveform microseconds before losing its rails. The
// vendor's own PwrOffTask paints, waits for the frame, then pulses the PMIC forever and never touches
// the panel again -- this is that shape.
//
// wakeAndPaintFirst exists because the auto-off branch fires with the panel parked and the rails down,
// so the frame it would leave on the glass would be the FIRST drive after epd_poweron() -- the one
// measured to band (docs/R55_R57_WAKE.md). So it wakes the panel exactly like the in-place resume
// does, spends the banding first drive on a throwaway frame, and lets the powered-off frame be the
// SECOND drive on warm rails, which r56 measured clean.
[[noreturn]] static void metalioHardPowerOff(const char* why, const bool wakeAndPaintFirst = false) {
  using namespace freeink::metalio4plus;
  // r91 R4: a confirmed power-off ends the debug session -- before anything is parked, so
  // the console is handed back to log-only while there is still a console to hand back.
  usbshell::clearSession("power off");
  sleepJournal("poweroff begin (%s)", why);
  Frontlight.setOn(false);
  {
    // Still take the lock: a render task mid-waveform must not be driving the panel while it is
    // parked. Nothing is composed under it.
    RenderLock lock;
    // The hook is installed permanently at setup (it carries the drive-health verdict); this
    // flag is what also appends the panel lines to the sleep journal for this power-off.
    s_journalPanelLines = true;
    if (!wakeAndPaintFirst) {
      if (metalioPowerOffFrameDriven()) {
        sleepJournal("poweroff: frame already on glass; parking panel unpainted");
      } else {
        // r101.10 PWR3: the dialog could not prove its frame reached the glass (a paint that drove
        // nothing, a declined entry frame). Parking here would leave the previous screen on a
        // powered-down device with no way to repaint it, so paint the frame now and block on it.
        const unsigned long lateFrameMs = metalioPaintPoweredOffFrame(renderer);
        sleepJournal("poweroff: frame was NOT on glass; painted before parking (%lu ms)", lateFrameMs);
      }
    display.deepSleep();  // park the ED047TC2 and drop the TPS65185 rails
    } else {
      // ---- wake first, exactly like metalioStandbySleep()'s resume (bus, wake, rails, settle) ----
      // r101.21 PWR4: THE CARD IS REMOUNTED BEFORE THE FRAME IS COMPOSED.
      //
      // CP_SD_PARK_ON_STANDBY (build default ON) unmounts the card for the whole standby, and
      // metalioStandbySleep()'s remount lives on the RESUME path -- which this branch never
      // reaches. The frame painted below is chosen by SETTINGS.powerOffScreen, and
      // POWER_OFF_CUSTOM reads /poweroff/* through Storage (PowerOffActivity.cpp,
      // paintCustomImageFrame). On a parked volume that read cannot succeed: paintCustomImageFrame()
      // returns false and the operator's configured power-off image silently degrades to the
      // generic TEXT dialog -- a frame, but not the one they configured. The same parked volume
      // swallows this power-off's journal lines, which is why the substitution used to be
      // undetectable from the card. Mount first, exactly like the resume (same call, same one
      // retry, before anything touches the panel), and record which way it went.
      const char* sdVerdict = "not-parked";
      if (standbytrim::active(standbytrim::Trim::SdPark)) {
        const unsigned long sdT0 = millis();
        bool sdMountOk = Storage.begin();
        if (!sdMountOk) {
          delay(20);
          sdMountOk = Storage.begin();  // one retry: the mount power-cycles the card and can miss once
        }
        sdVerdict = sdMountOk ? "ok" : "FAILED";
        sleepJournal("poweroff auto-off: SD remount %s in %lu ms before the frame", sdVerdict,
                     millis() - sdT0);
      }
      // Same rail order as the resume, P1.4 then P0.5, because this path runs from INSIDE the
      // standby loop (the auto-shutdown branch) while both rails may be cut.
      {
        bool paUp = true;
        if (standbytrim::active(standbytrim::Trim::PaRail)) {
          paUp = epd_board_metalio_eink4_plus_pa_rail_set(true);
          if (paUp) delay(80);  // same settle as the resume; nothing may draw before it
        }
        if (paUp && standbytrim::active(standbytrim::Trim::CamRail)) {
          epd_board_metalio_eink4_plus_cam_rail_set(true);
        }
      }
      epd_board_metalio_eink4_plus_bus_restore();
      display.wake();
      if (!metalio_vendor_panel_repower()) {
        LOG_INF("PANEL", "auto-off: repower reported rails not ok; the drive will retry via PWR_GOOD");
      }
      delay(KWAKE_SETTLE_MS);
      // Drive 1: the throwaway frame. Same content, but FULL always drives every row, so this is the
      // drive that eats the post-poweron banding.
      const unsigned long wakeDriveMs = metalioWakePrimeDrive(renderer);
      // Drive 2: the frame that stays on the glass.
      const unsigned long frameDriveMs = metalioPaintPoweredOffFrame(renderer);
      // One line, appended to the same journal as every other sleep/standby event: the sequence plus
      // both drive durations, so the next round can check the banding claim from a log alone.
      // `sd` says whether the card was available for the frame and `frame` says WHICH frame
      // actually reached the glass (image/text/dark/light/blank) -- a configured image that
      // degraded to TEXT is visible here without reading debug.log.
      sleepJournal(
          "poweroff auto-off: wake->drive1->drive2->park, sd=%s wake_drive=%lu ms frame_drive=%lu ms "
          "frame=%s",
          sdVerdict, wakeDriveMs, frameDriveMs, metalioPowerOffFrameLabel());
      // Re-park in the standby order before the rails go away (as metalioStandbySleep does).
      display.deepSleep();
      epd_board_metalio_eink4_plus_tps_enter_sleep();
      epd_board_metalio_eink4_plus_bus_hold_low();
#if CP_EPD_BUS_HOLD
      epd_board_metalio_eink4_plus_bus_latch_for_sleep();
#endif
    }
  }
  // The vendor's own pulse train (TCA P1.3, PWR_PULSE_COUNT edges, 100 ms high/low); the PMIC
  // counts the edges and collapses the 3.3 V rail. Bounded: four trains is four times the
  // vendor's edge count, and a rail that has not collapsed by then will not. Spinning here with
  // the panel parked and the rails up is the one outcome worth avoiding.
  constexpr int kMaxPulseTrains = 4;
  for (int attempt = 0; attempt < kMaxPulseTrains; ++attempt) {
    epd_board_metalio_eink4_plus_pwr_key_pulse_train();
    delay(200);
  }
  sleepJournal("poweroff: rail still up after %d pulse trains; deep-sleep fallback", kMaxPulseTrains);
  Storage.prepareForDeepSleep();  // unmount + release the SD pads: no writes after this
  for (;;) {
    // Fallback, not the normal exit: on this SoC esp_deep_sleep_start() has been observed to
    // return, so retry it rather than let a [[noreturn]] function fall off the end.
    esp_deep_sleep_start();
    delay(1000);
  }
}

// ---------------------------------------------------------------------------------------------
// Power-off confirmation handshake (declared in MetalioPowerRequest.h).
//
// The dialog is an activity: it paints "Powering off..." through the normal render path, waits
// for the frame, and only then raises the request. loop() acts on it at the top of the next
// iteration, so the shutdown never runs on the same stack as the paint.
// ---------------------------------------------------------------------------------------------
static std::atomic<bool> metalioPowerOffPending{false};

void metalioRequestPowerOff() { metalioPowerOffPending.store(true); }
bool metalioPowerOffRequested() { return metalioPowerOffPending.load(); }

// PAINT-GAP TIMING (declared in MetalioPowerRequest.h). Loop-task only, like the classifier that
// writes them: the DOWN edge of the press under classification and the tick DIALOG was decided.
// The dialog reads them once, at its first paint that reaches the glass.
static unsigned long metalioPowerDownEdgeStamp = 0;
static unsigned long metalioPowerDecisionStamp = 0;
void metalioPowerNoteDownEdge(const unsigned long ms) {
  metalioPowerDownEdgeStamp = ms;
  metalioPowerDecisionStamp = 0;  // a new press: this one has not decided anything yet
}
void metalioPowerNoteDialogDecision(const unsigned long ms) { metalioPowerDecisionStamp = ms; }
unsigned long metalioPowerDownEdgeMs() { return metalioPowerDownEdgeStamp; }
unsigned long metalioPowerDialogDecisionMs() { return metalioPowerDecisionStamp; }

void metalioPowerConfirmClosed(const bool closed) {
  // Kept for the dialog handshake, but there is no flag to clear any more: the
  // question "is the power-off dialog open?" is asked of the ActivityManager
  // (isActivityNamed("PowerOff")), so a dialog that is destroyed before it ever
  // enters cannot leave the power key latched off (see the classifier in loop()).
  (void)closed;
}

// Does this wake actually come from the power key?
struct StandbyWakeVerdict {
  bool power = false;      // a power key was down at (or just after) the wake
  unsigned long holdMs = 0;
  int gpioLevel = -1;
  esp_sleep_wakeup_cause_t cause = ESP_SLEEP_WAKEUP_UNDEFINED;
};

// We return from a light-sleep slice only on a CONFIRMED short power press; everything else
// (timer, charger INT, glitch) goes straight back to sleep.
static StandbyWakeVerdict metalioVerifyStandbyWake() {
  using namespace freeink::metalio4plus;
  StandbyWakeVerdict v;
  v.cause = esp_sleep_get_wakeup_cause();
  v.gpioLevel = digitalRead(IO_EXPANDER_INT_GPIO);

  bool readOk = false;
  // First read immediately: the TCA latches the press that caused the wake, and reading the port
  // is what clears it. Anything after this cannot tell "woke the CPU" from "pressed later".
  const bool downAtWake = powerKeyPressed(&readOk);
  if (!readOk) return v;  // expander unreachable: never treat that as a press
  if (!downAtWake) {
    // The key may already be released (a fast click lands between the wake and this read). The
    // vendor allows a 300 ms grace window for exactly that.
    const unsigned long graceEnd = millis() + 300;
    bool seen = false;
    while (millis() < graceEnd) {
      if (powerKeyPressed(&readOk) && readOk) {
        seen = true;
        break;
      }
      delay(10);
    }
    // A wake that no press can explain is spurious (charger PWR_GOOD, RTC, a glitch).
    if (!seen) return v;
  }
  v.power = true;
  // Measure the hold. Also covers "still down after 4 s" -> power off.
  const unsigned long t0 = millis();
  while (powerKeyPressed()) {
    delay(10);
    v.holdMs = millis() - t0;
    if (v.holdMs > METALIO_POWER_OFF_HOLD_MS) break;
  }
  // Wait for the finger to come off so the release edge cannot re-enter sleep immediately.
  const unsigned long releaseEnd = millis() + 2000;
  while (powerKeyPressed() && millis() < releaseEnd) delay(10);
  return v;
}

// Park everything, then sleep in slices until the power key wakes us. Returns on a confirmed
// short press; never returns on the power-off path.
static void metalioStandbyLoop() {
  using namespace freeink::metalio4plus;
  const auto pin = static_cast<gpio_num_t>(IO_EXPANDER_INT_GPIO);
  const unsigned long readyAt = millis();

  // The RTC alarm shares GPIO2 through the expander. Nothing programs one today, but a stray
  // alarm bit would hold the wake line and turn standby into a spin loop.
  halClock.disableAlarm();

  if (!esp_sleep_is_valid_wakeup_gpio(pin)) {
    // Without a wake source a slice would be unending. Wake the user instead of the device.
    sleepJournal("standby abort: GPIO%d not a valid wake GPIO", static_cast<int>(pin));
    return;
  }

  for (;;) {
    // 0 and the picker's top boundary both mean "never"; anything else is minutes asleep.
    const uint8_t autoOffMinutes = SETTINGS.standbyAutoOffMinutes;
    if (!usbShellHoldsAwake() && autoOffMinutes > 0 && autoOffMinutes < CrossPointSettings::STANDBY_AUTO_OFF_NEVER_MINUTES &&
        millis() - readyAt > static_cast<unsigned long>(autoOffMinutes) * 60000UL) {
      // The panel is parked and the rails are down, so a paint here would be the first drive after
      // epd_poweron() -- the one that bands -- and it would be the last frame on glass. Wake fully
      // first, then paint: the powered-off frame becomes the clean second drive (R55_R57_WAKE.md).
      metalioHardPowerOff("standby auto-shutdown", /*wakeAndPaintFirst=*/true);
    }

    // 1. Only POWER may assert INT: the vendor drives the whole expander to outputs with the key
    //    latches idle-high, then re-arms P17 alone. Every other input (volume keys, RTC, charger)
    //    is out of the wake path for the duration. The accel's INT1 is wired-OR onto the same line
    //    and is not under expander control, so it is told to route nothing first.
    releaseAccelIntLine();
    epd_board_metalio_eink4_plus_ioexp_mask_for_sleep();
    // 2. ARM P17 FIRST, then wait for the shared line. The order matters and it is the vendor's
    //    v2.0.74 order (their power_hw.cc moved ioexp_enable_power_wake_pin() ahead of
    //    WaitIntLineHigh()). Two things it buys:
    //      * the direction flip (output high -> input) makes a false INT edge on the expander, and
    //        the helper's resample right after the flip is what drops it. Waiting for "idle" while
    //        P17 is still an output waits on nothing: the output latch is idle while the key is not.
    //      * the helper now reports the LEVEL: false means POWER is still held. Sleeping with a held
    //        key fires EXT1 on the next instant, and that wake is indistinguishable from a real
    //        press -- the exact shape of a spurious/double wake.
    //    The helper is still the only thing that makes P17 an input; nothing else in the path does.
    if (!epd_board_metalio_eink4_plus_ioexp_enable_power_wake_pin()) {
      // Held key (or a failed read). Do not sleep. Rate-limited journal: this path spins at ~20 Hz
      // and it is reachable for as long as a press is being held across the arm window.
      if (++slpHeldAtArm <= 3 || (slpHeldAtArm % 100) == 0) {
        sleepJournal("arm: P17 still held at arm (not a wake, not slept on) n=%lu",
                     static_cast<unsigned long>(slpHeldAtArm));
      }
      epd_board_metalio_eink4_plus_ioexp_restore_keys();
      delay(50);
      continue;
    }
    // 3. With P17 armed and released, the line must be idle: a source still holding it low would
    //    fire the wake instantly and forever.
    if (!waitForInterruptLineIdle(300)) {
      // This path spins at ~20 Hz (delay(50) below) and would otherwise write to SD every pass.
      if (++slpArmRetries <= 3 || (slpArmRetries % 100) == 0) {
        sleepJournal("arm: INT still low, retry n=%lu", static_cast<unsigned long>(slpArmRetries));
      }
      epd_board_metalio_eink4_plus_ioexp_restore_keys();
      delay(50);
      continue;
    }

    // 4. Arm the wake sources for this slice. Two configurations, one code path:
    //
    //    TODAY (CP_LP_STANDBY=0, or the sentry would not start, or it has been dropped
    //    for this standby): EXT1 ANY_LOW on GPIO2 + the 60 s timer. Unchanged.
    //
    //    r99 (CP_LP_STANDBY=1, sentry running): the LP core owns GPIO2 at 20 ms
    //    resolution and raises the HP core itself on a debounced assertion, so EXT1 is
    //    deliberately NOT armed — that removal is the whole feature. `esp_sleep_enable_
    //    ulp_wakeup()` is what lets that raise land as a light-sleep wake, and the 60 s
    //    timer below is untouched: it is the guarantee that the device always wakes, so
    //    a missed press costs up to one slice of latency, never a hang.
    //
    // No GPIO isolation: isolating the pad is what made the expander unreadable on the
    // wake side in earlier rounds.
    // Slice accounting stays in RAM. An unexpected level=0 means the wake line was not idle at arm
    // time, which IS worth a write; rate-limited in case the line is stuck low for hours.
    // READ HERE, while GPIO2 is still in the normal GPIO function: under CP_LP_STANDBY the sentry
    // below takes the pad into the RTC-IO function, where digitalRead() no longer sees the line.
    ++slpSliceCount;
    const int armLevel = digitalRead(IO_EXPANDER_INT_GPIO);
    if (armLevel == 0 && slpLowLevelSlices < 3) {
      ++slpLowLevelSlices;
      sleepJournal("slp level=%d touch=%d slice=%lu", armLevel, freeink::metalio4plus::touchWakeMode(),
                   static_cast<unsigned long>(slpSliceCount));
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_err_t ext1 = ESP_OK;
    bool sentryThisSlice = false;
#if CP_LP_STANDBY
    // Starting the sentry takes GPIO2 into the RTC-IO function (see cp_lp_sentry.c),
    // so it happens AFTER the level checks above and is undone before ANY later read
    // of that pad. The pad is only out of the GPIO function while the HP core sleeps.
    sentryThisSlice = !slpSentryDropped && lpcore::sentryBegin(/*arm=*/true);
    if (sentryThisSlice && esp_sleep_enable_ulp_wakeup() != ESP_OK) {
      // Never sleep on a wake source that did not arm: give the pad back and use EXT1.
      lpcore::sentryEnd();
      sentryThisSlice = false;
      sleepJournal("lp standby: ulp wake arm failed — ext1 for this slice");
    }
    if (!slpSentryDropped && slpSentryStarts == 0) {
      sleepJournal("lp standby: sentry %s", sentryThisSlice ? "running (HP does not arm EXT1)"
                                                           : "NOT started — ext1 path, unchanged");
    }
    if (sentryThisSlice) ++slpSentryStarts;
#endif
    if (!sentryThisSlice) {
      ext1 = esp_sleep_enable_ext1_wakeup_io(1ULL << static_cast<unsigned>(pin), ESP_EXT1_WAKEUP_ANY_LOW);
    }
    const esp_err_t timer = esp_sleep_enable_timer_wakeup(METALIO_STANDBY_SLICE_US);
    if (ext1 != ESP_OK || timer != ESP_OK) {
      sleepJournal("standby abort: arm ext1=%d timer=%d", static_cast<int>(ext1), static_cast<int>(timer));
      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
#if CP_LP_STANDBY
      if (sentryThisSlice) lpcore::sentryEnd();  // give GPIO2 back before the resume path
#endif
      epd_board_metalio_eink4_plus_ioexp_restore_keys();
      return;  // stay awake rather than sleep unwakeably
    }

    metalio_sleep_checkpoint(METALIO_SLP_PHASE_WAKE_ARMED, 0);
#if CP_CPU_STATS
    const int64_t cpSlpT0 = cpustats::nowUs();
#endif
    const esp_err_t err = metalio_sleep_light_sleep(0);
#if CP_CPU_STATS
    cpustats::noteLightSleepUs(cpustats::nowUs() - cpSlpT0);
#endif
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
#if CP_LP_STANDBY
    // Read the sentry's record of the slice BEFORE stopping it, then give GPIO2 back to
    // the normal GPIO function — every read below (`powerLowAtWake`, the idle checks on
    // the next pass) needs the pad in that function.
    //
    // LIVENESS CHECK. A 60 s slice at the 20 ms LP period should show ~3000 LP wakeups.
    // `magic` unset or `hb` == 0 means the LP core never executed while the HP core was
    // asleep, and the pre-filter would then be a lie — so it is dropped for the REST of
    // this standby and every later slice arms EXT1 again. (docs/engineering/R96_ROUND.md
    // §5: the LP core's behaviour across our light-sleep slice is not established on this
    // silicon, and one run of the r96 command died silently. This check is what makes an
    // unknown safe: the worst case is that the feature turns itself off and today's path
    // resumes, mid-standby, with one journal line saying so.)
    lpcore::LpSentry slpSeen;
    if (sentryThisSlice) {
      slpSeen = lpcore::sentryRead();
      lpcore::sentryEnd();
      if (!slpSeen.alive || slpSeen.hb == 0) {
        slpSentryDropped = true;
        sleepJournal("lp standby: sentry did not run (magic=0x%08lx hb=%lu) — ext1 restored",
                     static_cast<unsigned long>(slpSeen.magic), static_cast<unsigned long>(slpSeen.hb));
      } else if (cause == ESP_SLEEP_WAKEUP_TIMER && slpSeen.hb < 2500) {
        // 60 s at 20 ms is 3000; below 2500 the LP clock is not what the period implies.
        // Not a failure — a number worth having in the journal, once per standby.
        slpSentryDropped = true;
        sleepJournal("lp standby: sentry slow (hb=%lu for a slice, want ~3000) — ext1 restored",
                     static_cast<unsigned long>(slpSeen.hb));
      }
    }
#endif
    // Latch the power pin FIRST: after the restore below the expander may be reconfigured.
    const bool powerLowAtWake = !digitalRead(IO_EXPANDER_INT_GPIO);

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
#if CP_LP_STANDBY
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ULP);
#endif
    epd_board_metalio_eink4_plus_ioexp_restore_keys();

    if (err != ESP_OK) {
      // ESP_ERR_SLEEP_REJECT: something (a flash op, a wifi beacon) vetoed the slice. Retry, but
      // never in a tight loop.
      sleepJournal("slice rejected err=%d cause=%d", static_cast<int>(err), static_cast<int>(cause));
      delay(20);
      continue;
    }
    // ---- THE TIMER-WAKE DECISION POINT ---------------------------------------------------------
    // CONTRACT (docs/engineering/STANDBY_TIMER_WAKE_CONTRACT.md; vendor power_hw.cc and
    // power_policy/README.en.md, "Timer wake: stay in standby"): a timer wake is a slice boundary,
    // never an exit. Everything parked before this loop STAYS parked here, because re-initialising
    // it would cost more than the slice it buys:
    //   * panel rails   -- display.deepSleep() + tps_enter_sleep() + the 16 pinned EPD pads, all in
    //                      metalioStandbySleep() before metalioStandbyLoop(). Repower happens
    //                      through metalio_vendor_panel_repower() on the resume path only.
    //   * PA / BT rail  -- the P1.4 latch cut by CP_CUT_PA_RAIL_ON_STANDBY in the same function and
    //                      restored after this loop returns. The expander mask/restore pair per
    //                      slice preserves that latch (CFG_P1_RAIL_MASK) and only flips DIRECTIONS.
    //   * SD card       -- Storage.prepareForDeepSleep() under CP_SD_PARK_ON_STANDBY; the remount
    //                      (Storage.begin()) is on the resume path, after this loop returns.
    //   * sensors       -- IMU INT1 routing is cleared per slice (CTRL_REG3=0); the IMU is never
    //                      brought up here, so there is nothing to restore.
    // Between the sleep call and the `continue` below there is clock/key-direction/wake-source work
    // only: metalio_sleep_light_sleep() is the SDK MPLL/PSRAM handoff, ioexp_restore_keys() restores
    // directions, sentryEnd() gives GPIO2 back.
    //
    // GUARD: the park must still be in force when a timer wake lands (standbytrim::parked() is set
    // for the whole standby and cleared only on the resume path). If it is not, something restored
    // peripherals inside a slice -- exactly what this contract forbids -- so say so rather than
    // hiding it. Cheap: one RAM read per slice.
    if (cause == ESP_SLEEP_WAKEUP_TIMER) {
      if (!standbytrim::parked()) {
        if (++slpParkViolations <= 3 || (slpParkViolations % 100) == 0) {
          sleepJournal("contract: timer wake with the park lifted (parked=0) n=%lu slice=%lu",
                       static_cast<unsigned long>(slpParkViolations),
                       static_cast<unsigned long>(slpSliceCount));
        }
      }
      continue;  // plain slice end: straight back to sleep
    }

    const StandbyWakeVerdict v = metalioVerifyStandbyWake();
    if (!v.power) {
      sleepJournal("spurious wake cause=%d int=%d powerLow=%d", static_cast<int>(cause), v.gpioLevel,
                   powerLowAtWake ? 1 : 0);
      continue;
    }
    if (v.holdMs > METALIO_POWER_OFF_HOLD_MS) {
      metalioHardPowerOff("power key held in standby");
    }
    if (v.holdMs < METALIO_WAKE_BOUNCE_MS) {
      sleepJournal("bounce ignored hold=%lu", v.holdMs);
      continue;
    }
    sleepJournal("wake hold=%lu cause=%d", v.holdMs, static_cast<int>(cause));
    return;
  }
}

// ---------------------------------------------------------------------------
// r98.3 -- WAKE-TIME RECORDER: one paired park/resume record per standby, in the sleep journal.
//
// WHY THIS EXISTS: a trim A/B needs the coulomb count across a standby, and the only way to read
// it today is `batt`, which needs USB -- and USB CHARGES the device (+257 mA measured), which
// swamps the 2-5 mA the trims are meant to move. So the measurement is taken on-device, at exactly
// two moments:
//   PARK   -- 0x10 RemainingCapacity, uptime, the effective trim state and the slice count, read at
//             the TOP of metalioStandbySleep(), before the frontlight/touch/panel park and before
//             Storage.prepareForDeepSleep(): the I2C bus, the gauge and the card are all still up,
//             and the line is written before the card can be parked.
//   RESUME -- 0x10 again once the touch/bus restore and the SD remount are done, with uptime,
//             elapsed asleep, the trim state and the delta. One pair = one segment, no USB, no
//             shell, nothing to babysit.
//
// The read goes through HalPowerManager::readRemainingCapacityChecked() -- the SAME validated
// double-read `batt` uses (two reads that must agree, plus a sanity bound), not a second
// implementation of the wire protocol.
//
// HARD RULE: a gauge read must NEVER delay or endanger the park path. A failed read logs
// `0x10=FAIL (<reason>)` and the device sleeps exactly as it would have.
// ---------------------------------------------------------------------------
struct StandbyWakerec {
  bool havePark = false;
  bool parkOk = false;
  uint16_t parkMah = 0;
  unsigned long parkUpMs = 0;
  uint32_t parkSlices = 0;
  uint8_t trimPa = 0;
  uint8_t trimSd = 0;
  char parkWhy[80] = {0};
};
static StandbyWakerec s_wakerec;

// True only while CP_SD_PARK_ON_STANDBY has the card unmounted; the auto-flush below refuses to
// touch the card inside that window.
static bool s_autoflushSdParked = false;

static void wakerecParkRead() {
  s_wakerec = StandbyWakerec{};
  s_wakerec.trimPa = standbytrim::active(standbytrim::Trim::PaRail) ? 1 : 0;
  s_wakerec.trimSd = standbytrim::active(standbytrim::Trim::SdPark) ? 1 : 0;
  s_wakerec.parkSlices = slpSliceCount;
  s_wakerec.parkUpMs = millis();
  s_wakerec.havePark = true;
  const HalPowerManager::CapCheck c =
      HalPowerManager::readRemainingCapacityChecked(BoardConfig::ACTIVE.batteryGauge.gaugeAddr);
  s_wakerec.parkOk = c.ok;
  s_wakerec.parkMah = c.mah;
  if (!c.ok) snprintf(s_wakerec.parkWhy, sizeof(s_wakerec.parkWhy), "%s", c.why);
}

// Written after the park journal lines and BEFORE the card is parked, so the record is durable.
static void wakerecParkLog() {
  // ---- Build A item C: the park half of the coulomb-delta pair, with the clock named -------------
  // Zero bus traffic: the coulomb value is the 0x10 read wakerecParkRead() already made, and no
  // I2C happens here. hybridclock::markPark() captures the RTC counter + esp_timer + epoch at
  // this instant, which is the instant the parked span starts. LOG_INF goes to the RAM ring
  // (flushed by standbyAutoFlush on the next line); the journal line is the durable copy on the
  // card. Both carry the same fields so one park/resume pair answers charge, device elapsed and
  // uptime in one place.
  hybridclock::markPark(s_wakerec.parkMah, s_wakerec.parkOk);
  {
    const hybridclock::Mark& pm = hybridclock::parkMark();
    char drift[32];
    hybridclock::driftLabel(drift, sizeof(drift));
    if (s_wakerec.parkOk) {
      LOG_INF("PWR",
              "park mark: coulomb=%u mAh epoch=%lld uptime=%lu ms rtc=%llu us drift=%s (parked span rides the "
              "RTC counter, internal RC; epoch rides the same ticks)",
              static_cast<unsigned>(pm.coulombMah), static_cast<long long>(pm.epoch), pm.uptimeMs,
              static_cast<unsigned long long>(pm.rtcUs), drift);
      sleepJournal("park mark: coulomb=%u mAh epoch=%lld uptime=%lu ms rtc=%llu us drift=%s",
                   static_cast<unsigned>(pm.coulombMah), static_cast<long long>(pm.epoch), pm.uptimeMs,
                   static_cast<unsigned long long>(pm.rtcUs), drift);
    } else {
      LOG_INF("PWR", "park mark: coulomb=FAIL (%s) epoch=%lld uptime=%lu ms rtc=%llu us drift=%s",
              s_wakerec.parkWhy, static_cast<long long>(pm.epoch), pm.uptimeMs,
              static_cast<unsigned long long>(pm.rtcUs), drift);
      sleepJournal("park mark: coulomb=FAIL (%s) epoch=%lld uptime=%lu ms rtc=%llu us drift=%s",
                   s_wakerec.parkWhy, static_cast<long long>(pm.epoch), pm.uptimeMs,
                   static_cast<unsigned long long>(pm.rtcUs), drift);
    }
  }

  if (s_wakerec.parkOk) {
    sleepJournal("wakerec park rc=0x10 %u mAh trim_pa=%u trim_sd=%u slices=%lu up=%lu ms", s_wakerec.parkMah,
                 static_cast<unsigned>(s_wakerec.trimPa), static_cast<unsigned>(s_wakerec.trimSd),
                 static_cast<unsigned long>(s_wakerec.parkSlices), s_wakerec.parkUpMs);
  } else {
    sleepJournal("wakerec park rc=0x10 FAIL (%s) trim_pa=%u trim_sd=%u slices=%lu up=%lu ms", s_wakerec.parkWhy,
                 static_cast<unsigned>(s_wakerec.trimPa), static_cast<unsigned>(s_wakerec.trimSd),
                 static_cast<unsigned long>(s_wakerec.parkSlices), s_wakerec.parkUpMs);
  }
}

// The second half of the pair. Elapsed time comes from millis(), which keeps counting across light
// sleep, so it is the real time asleep (plus the slice overhead) -- not CPU time.
static void wakerecResume() {
  const uint8_t gauge = BoardConfig::ACTIVE.batteryGauge.gaugeAddr;
  const uint8_t trimPa = standbytrim::active(standbytrim::Trim::PaRail) ? 1 : 0;
  const uint8_t trimSd = standbytrim::active(standbytrim::Trim::SdPark) ? 1 : 0;
  const unsigned long nowMs = millis();
  const HalPowerManager::CapCheck c = HalPowerManager::readRemainingCapacityChecked(gauge);

  if (!s_wakerec.havePark) {
    // No park half: this resume is not paired with a recorder line (a reboot between the two, or
    // an image that predates the recorder). Say so rather than inventing a delta.
    if (c.ok) {
      sleepJournal("wakerec resume rc=0x10 %u mAh (NO PARK RECORD -- rebooted, no delta) trim_pa=%u trim_sd=%u",
                   c.mah, static_cast<unsigned>(trimPa), static_cast<unsigned>(trimSd));
    } else {
      sleepJournal("wakerec resume rc=0x10 FAIL (%s) (NO PARK RECORD) trim_pa=%u trim_sd=%u", c.why,
                   static_cast<unsigned>(trimPa), static_cast<unsigned>(trimSd));
    }
    return;
  }

  const unsigned long asleepMs = nowMs - s_wakerec.parkUpMs;
  if (!c.ok) {
    sleepJournal("wakerec resume rc=0x10 FAIL (%s) asleep=%lu ms trim_pa=%u trim_sd=%u slices_at_park=%lu",
                 c.why, asleepMs, static_cast<unsigned>(trimPa), static_cast<unsigned>(trimSd),
                 static_cast<unsigned long>(s_wakerec.parkSlices));
  } else if (!s_wakerec.parkOk) {
    sleepJournal("wakerec resume rc=0x10 %u mAh asleep=%lu ms (PARK READ FAILED -- no delta) trim_pa=%u trim_sd=%u",
                 c.mah, asleepMs, static_cast<unsigned>(trimPa), static_cast<unsigned>(trimSd));
  } else {
    const long deltaMah = static_cast<long>(c.mah) - static_cast<long>(s_wakerec.parkMah);
    // mA = delta mAh * 3600000 / ms. Kept as centi-mA so a 2-3 mA standby shows as 2.40 rather
    // than rounding to 2 -- the effect being measured is smaller than one integer mA in a short
    // segment.
    long centiMa = 0;
    if (asleepMs > 0) centiMa = static_cast<long>((static_cast<long long>(deltaMah) * 360000000LL) /
                                                  static_cast<long long>(asleepMs));
    sleepJournal("wakerec resume rc=0x10 %u mAh d=%+ld mAh over %lu ms (%s%ld.%02ld mA) trim_pa=%u trim_sd=%u slices_at_park=%lu",
                 c.mah, deltaMah, asleepMs, centiMa < 0 ? "-" : "",
                 labs(centiMa) / 100, labs(centiMa) % 100, static_cast<unsigned>(trimPa),
                 static_cast<unsigned>(trimSd), static_cast<unsigned long>(s_wakerec.parkSlices));
  }
  // ---- Build A item C: the resume half of the coulomb-delta pair -------------------------------
  // Same rule as the park half: no bus traffic, because `c` is the read wakerecResume() already
  // made. markResume() computes the parked span from the RTC tick delta and applies the STORED
  // ppm factor. The line does not print a mA figure: the wakerec line right above already does,
  // and both denominators are device time -- the clock under test -- so the operator divides the
  // charge delta by the HOST wall clock instead.
  hybridclock::markResume(c.mah, c.ok);
  {
    const hybridclock::Mark& rm = hybridclock::resumeMark();
    const hybridclock::Segment& seg = hybridclock::lastSegment();
    char drift[32];
    hybridclock::driftLabel(drift, sizeof(drift));
    if (!c.ok) {
      LOG_INF("PWR", "resume mark: coulomb=FAIL (%s) epoch=%lld uptime=%lu ms rtc=%llu us drift=%s", c.why,
              static_cast<long long>(rm.epoch), rm.uptimeMs, static_cast<unsigned long long>(rm.rtcUs), drift);
      sleepJournal("resume mark: coulomb=FAIL (%s) epoch=%lld uptime=%lu ms rtc=%llu us drift=%s", c.why,
                   static_cast<long long>(rm.epoch), rm.uptimeMs, static_cast<unsigned long long>(rm.rtcUs), drift);
    } else if (!seg.valid) {
      // No park half: a reboot between the two, so there is no parked span and no delta.
      LOG_INF("PWR", "resume mark: coulomb=%u mAh epoch=%lld uptime=%lu ms rtc=%llu us (NO PARK MARK — no parked span)",
              static_cast<unsigned>(rm.coulombMah), static_cast<long long>(rm.epoch), rm.uptimeMs,
              static_cast<unsigned long long>(rm.rtcUs));
      sleepJournal("resume mark: coulomb=%u mAh epoch=%lld uptime=%lu ms rtc=%llu us (NO PARK MARK — no parked span)",
                   static_cast<unsigned>(rm.coulombMah), static_cast<long long>(rm.epoch), rm.uptimeMs,
                   static_cast<unsigned long long>(rm.rtcUs));
    } else {
      const long deltaMah = seg.bothOk ? (static_cast<long>(seg.resumeMah) - static_cast<long>(seg.parkMah)) : 0;
      const unsigned long parkedMs = static_cast<unsigned long>(seg.parkedCorrectedUs / 1000ULL);
      LOG_INF("PWR",
              "resume mark: coulomb=%u mAh epoch=%lld uptime=%lu ms rtc=%llu us | parked %lu ms raw %lu ms (drift "
              "%s)%s | d=%+ld mAh; mA from device time is PROVISIONAL — divide charge by the HOST wall clock",
              static_cast<unsigned>(rm.coulombMah), static_cast<long long>(rm.epoch), rm.uptimeMs,
              static_cast<unsigned long long>(rm.rtcUs), parkedMs,
              static_cast<unsigned long>(seg.parkedRawUs / 1000ULL), drift,
              seg.bothOk ? "" : " (one 0x10 read failed)", seg.bothOk ? deltaMah : 0L);
      sleepJournal("resume mark: coulomb=%u mAh epoch=%lld uptime=%lu ms rtc=%llu us | parked=%lu ms raw=%lu ms drift=%s%s d=%+ld mAh (mA from device time is provisional)",
                   static_cast<unsigned>(rm.coulombMah), static_cast<long long>(rm.epoch), rm.uptimeMs,
                   static_cast<unsigned long long>(rm.rtcUs), parkedMs,
                   static_cast<unsigned long>(seg.parkedRawUs / 1000ULL), drift,
                   seg.bothOk ? "" : " (0x10 read failed)", seg.bothOk ? deltaMah : 0L);
    }
  }

  s_wakerec.havePark = false;
}

// r98.3 -- CP_AUTOFLUSH_ON_STANDBY: dump the in-RAM log ring to /.crosspoint/debug.log at park and
// at resume, so `logtail` (which reads the FILE) shows the real state after a sleep without the
// operator pressing Debug -> "Print logs to SD". Costs one SD write per wake (~a few ms for a few
// KB, and at resume it runs inside the deliberate KWAKE_SETTLE_MS window), and a failure is
// silent: the device must sleep and wake whatever the card does. Kept behind a named switch
// because it exists only to make the standby A/B readable from the card and can be dropped once
// the wake-time recorder is trusted.
#ifndef CP_AUTOFLUSH_ON_STANDBY
#define CP_AUTOFLUSH_ON_STANDBY 1
#endif
static unsigned long standbyAutoFlush(const char* where) {
#if CP_AUTOFLUSH_ON_STANDBY
  // Never while the card is parked or unmounted: the write would fail, and a park must not wait
  // on a card that is deliberately gone. Skipped silently, by design.
  if (s_autoflushSdParked) return 0;
  const size_t pending = logSdPending();
  if (pending == 0) return 0;
  const unsigned long t0 = millis();
  const bool ok = logSdFlush();
  const unsigned long ms = millis() - t0;
  // Journal only now: the card just proved it can take a write.
  if (ok) {
    sleepJournal("autoflush %s ok %u B in %lu ms", where, static_cast<unsigned>(pending), ms);
  } else {
    sleepJournal("autoflush %s FAILED %u B in %lu ms", where, static_cast<unsigned>(pending), ms);
  }
  return ms;
#else
  (void)where;
  return 0;
#endif
}

// r128: parkArmSource() is gone with the sink legs -- the park is 751ce6c4's and arms no sink.

// r131: RESUME HALF = r114d's (254ef8eb), PARK HALF = r104's (751ce6c4), byte for byte ----------
// The other half of the r130 split. r129 bands on the first panel drive after a light-sleep
// resume; r130 restores r105's park and leaves r104's wake, r131 does the reverse. The park below
// is 751ce6c4's function prefix verbatim -- the six-step order, the park mark at the top, the sink
// legs and the unmount-before-rails ordering are NOT here -- and everything after
// `metalioStandbyLoop()` is the r105+ resume: rails-first (P1.4 -> 80 ms settle -> P0.5 -> bus ->
// card), the 80 ms CP_WAKE_RAIL_SETTLE spent AFTER epd_lcd_reinit() (r147: the bus is re-armed on
// the P0.5 edge, so the settle and the 50 ms SD dwell before the remount both run with the panel
// already clocked), the guarded wake prime / warm re-drive / one-drive-strict switches (present,
// all default 0) and the drive-capture window. `diff` of this function's prefix, from its opening
// line to `metalioStandbyLoop();`, against `git show 751ce6c4:src/main.cpp` is EMPTY -- that is the
// invariant this build is judged on. (The function as a whole cannot be: the resume lives inside
// it, and that is the half this build changes.)
//
// NOT here, on purpose: the erase sweep (H19) and the throwaway wake drive (H18) -- both already
// eliminated on glass, and either would confound the verdict. No prime, no re-drive: one content
// drive, as r129.

static void metalioStandbySleep(const bool fromTimeout) {
  using namespace freeink::metalio4plus;
  const bool lightWasOn = Frontlight.isOn();

  // r98.3: park half of the wake-time record. FIRST, because every byte of this function after it
  // parks something (frontlight, touch controller, panel, EPD bus) or drops a rail -- and the
  // gauge read must be taken with the I2C bus and the gauge untouched. It costs one validated
  // 0x10 double-read; a failure only changes the journal line, never the sleep.
  wakerecParkRead();

  Frontlight.setOn(false);
  // FT6336U -> 0xA5 = 0x01 (system-info: stops scanning, I2C stays reachable) under the bus lock.
  gpio.prepareForDeepSleep();
  // Park the panel: ED047TC2 to sleep + TPS65185 rails down. The image stays (bistable) and the
  // rails come back lazily on the next paint via epd_poweron().
  display.deepSleep();
  // Vendor park order after epd_poweroff: TPS SLEEP settle (manual 8.4: PWRUP/VCOM down -> WAKEUP
  // down -> wait >=100 ms), then the parallel bus held low so 16 floating EPD data/control pads
  // cannot leak into an unpowered panel for the whole standby.
  epd_board_metalio_eink4_plus_tps_enter_sleep();
  epd_board_metalio_eink4_plus_bus_hold_low();
#if CP_EPD_BUS_HOLD
  // ---- CP_EPD_BUS_HOLD: latch the parallel bus through standby (r82) -------------------------
  // What it holds, and why every pin is safe to hold (RE_RICKYOS_SLEEP_WAKE.md §2, table at
  // RickyOS 0x40555db0 = {8..19, 35, 37, 40}; the same 16 pins are k_epd_bus_pins here):
  //   GPIO8..15   D0..D7   EPD data lanes.       Sink only; the panel is unpowered.
  //   GPIO16      XCL      source driver clock.  Panel unpowered, no clock is being consumed.
  //   GPIO17      XLE      latch enable.         Same; latched low == no latch pulse.
  //   GPIO18      XOE      output enable.        Held low == outputs disabled, the safe state.
  //   GPIO4       XSTL     start pulse.          Same; no start pulse can be issued.
  //   GPIO35      SPV      source power.         EPD connector only, not the PMIC enable.
  //   GPIO40      CKV      gate clock.           Same as XCL.
  //   GPIO19      MODE     EPD mode select.      Held at the level epd_ctrl_gpios_out_low() sets.
  //   GPIO37      BORDER   border pin.           Held low, which is the border-off state.
  // NONE of these is a wake source on this board: the Plus has no power-button GPIO, and the only
  // wake line is the expander INT on GPIO2 (see metalioPowerWakePin / HalGPIO::prepareForDeepSleep).
  // GPIO2 is not in the list, is not touched here, and must never be added -- a held wake line
  // means the device cannot leave standby, and the only recovery is USB flashing via the BOOT
  // strap. Released by epd_board_metalio_eink4_plus_bus_restore() on resume and by
  // epd_board_metalio_eink4_plus_boot_release_holds() on a cold boot.
  epd_board_metalio_eink4_plus_bus_latch_for_sleep();
  sleepJournal("standby park: EPD bus latched low (16 pins held)");
#endif
  // Deliberately NO epd_board_metalio_eink4_plus_sleep_cut_rails(): it drives P0.5 and P1.4 low
  // in one step with no per-rail status, and this project's bring-up test found that pattern --
  // CAM_SCR + TP_RST + BT_PA together -- left the board dark and needing a 15 s power cycle (see
  // ensureBooted() in MetalioEInk4PlusBoard.h). The two rails are cut below through TWO separate
  // single-rail setters, each behind its own trim, each reporting its own TCA failure.
  //
  // The decision to cut P0.5 as well is the operator's, on vendor evidence: shipped firmware for
  // this exact board cuts P5 (CAM_SCR) then P14 (BT_PA_PWR) and nothing else, and restores
  // P14 -> settle -> P5. What it never touches -- and what we must never drive low -- is P1.2
  // (TP_RST); that pin is the difference between the vendor's safe cut and our 25 Oct dark board.
  //
  // PARK ORDER: P1.4 low, then P0.5 low. (The vendor cuts P5 first; ordering only has an
  // electrical cost on the way UP, where we follow them exactly.) By here: the frontlight is off,
  // the FT6336U is hibernated, display.deepSleep() has parked the ED047TC2 and dropped the TPS
  // rails, tps_enter_sleep() has taken PWRUP/VCOM/WAKEUP low and waited the manual's 120 ms, and
  // the 16 EPD pads are held low -- so the panel is electrically quiet and each rail is the last
  // load to leave 3V3, not a load removed while the TPS is still ramping.
  //
  // Each arm is selected by standbytrim::active(): the compile-time switch decides the DEFAULT
  // (both ON) and `trim pa off` / `trim cam off` overrides it for one session, so an A/B can run
  // from ONE flash. The runtime toggle has no parallel path -- it changes the predicate this code
  // reads. `trim status` prints which switch was compiled in, so a run can never be credited to a
  // trim that was not built.
  // From here until the resume clears it `trim` refuses to move an arm: the panel and the rails are
  // mid-park, and an arm changed half way describes neither A nor B.
  standbytrim::setParked(true);
  // ---- VENDOR ORDER: CAM (P0.5) first, then PA (P1.4) -----------------------------------------
  // The board's own firmware cuts P5 then P14 (epd_board_metalio_eink4_plus.c:331-342) and ships it, so
  // this order is adopted on the vendor's evidence rather than on ours. We do NOT yet have our own
  // result for it: in r101.20 the runtime override was wiped by a reset before the park ran, so the
  // journal shows a PA-only park and the CAM cut has never actually executed on this unit.
  // HYPOTHESIS, UNTESTED: taking the smaller step first and the bigger one last, on a converter that
  // still has a load on it, is kinder to the rail. Do not restate this as a finding.
  // KNOWN, MEASURED: cutting CAM_SCR costs the touch controller its supply, and resumeAfterSleep() is a
  // standby WAKE, not a boot-from-power-loss -- the FT6336U comes back unconfigured unless it is hard
  // reset via TP_RST and re-probed (defect TCH2). Fix TCH2 before trusting any CAM-rail result.
  // (Earlier revisions of this comment claimed r101.19 "lost power mid-park". That was wrong: the park
  // completed, the device woke on the power key, and touch was dead; the POWERON reset was the
  // operator's force-reset. Retracted 7 Nov 2026.)
  //
  // CAM defaults OFF (CP_CUT_CAM_RAIL_ON_STANDBY=OFF) on purpose: it is enabled per session with
  // `trim cam on` / the Debug row, and because the override is RAM-only a cold boot returns it to
  // OFF. A cut that turns out to be fatal therefore costs one power cycle, never a reflash.
  if (standbytrim::active(standbytrim::Trim::CamRail)) {
  // ---- CP_CUT_CAM_RAIL_ON_STANDBY: P0.5 CAM_SCR, cut FIRST (vendor order) ----------------------
  // A failed TCA write leaves the rail UP and says so in the journal. The cost of a failed cut is
  // current; the cost of pretending it worked would be an A/B credited to a rail that never moved.
  if (!epd_board_metalio_eink4_plus_cam_rail_set(false)) {
    sleepJournal("standby park: P0.5 cut FAILED (TCA) — rail left up");
  } else {
    sleepJournal("standby park: P0.5 CAM_SCR rail cut (P1.4 still up) [vendor order]%s",
                 standbytrim::overrideValue(standbytrim::Trim::CamRail) < 0 ? " [build default]" : " [runtime override]");
  }
  }
  if (standbytrim::active(standbytrim::Trim::PaRail)) {
  // ---- CP_CUT_PA_RAIL_ON_STANDBY: P1.4 BT/PA, cut SECOND (vendor order) ------------------------
  // Evidence that a low P1.4 is survivable on this board (RefreshPolicy.h has the long form):
  // shipped vendor firmware drives exactly this bit low for 3 s on purpose
  // (bt_power_reset -> metalio_periph.c:439), and no panel path references P1.4 at all -- the
  // panel's power is the TPS65185 plus ANA_SW GPIO36 (epd_board_...c:712).
  // If the TCA write fails the rail is left up and the journal says so -- a failed cut costs
  // current, never stability.
  if (!epd_board_metalio_eink4_plus_pa_rail_set(false)) {
    sleepJournal("standby park: P1.4 cut FAILED (TCA) — rail left up");
  } else {
    sleepJournal("standby park: P1.4 BT/PA rail cut%s",
                 standbytrim::overrideValue(standbytrim::Trim::PaRail) < 0 ? " [build default]" : " [runtime override]");
  }
  }
  // r98.3: park half of the wake-time record, then flush the log ring. Both sit here -- after the
  // park journal lines above and BEFORE the card can be parked below -- so they are durable on the
  // card before the SDMMC pads float. standbyAutoFlush() refuses to touch a parked card.
  wakerecParkLog();
  standbyAutoFlush("park");

  if (standbytrim::active(standbytrim::Trim::SdPark)) {
  // ---- CP_SD_PARK_ON_STANDBY -----------------------------------------------------------------
  // Journal FIRST, then unmount. sleepJournal() opens/writes/flushes/closes per line, and
  // SDCardManager::shutdown() runs FsVolume::end() (which flushes SdFat's cached FAT and directory
  // sectors) before floating the SDMMC pads -- so everything up to this point is durable on the
  // card when the pads go high-Z. Nothing is written between here and the resume remount: the
  // per-slice journal lines (arm retries, spurious wakes) are DROPPED while parked, by design.
  // That is the cost of the switch, and it is why the park/unpark lines carry the slice count and
  // the reason for the wake instead.
  sleepJournal("standby park: parking SD (CP_SD_PARK_ON_STANDBY); per-slice lines dropped until resume%s",
               standbytrim::overrideValue(standbytrim::Trim::SdPark) < 0 ? " [build default]" : " [runtime override]");
  Storage.prepareForDeepSleep();
  s_autoflushSdParked = true;  // r98.3: nothing may touch the card until the resume remount
  }

  metalio_sleep_begin_cycle();
  metalio_sleep_checkpoint(METALIO_SLP_PHASE_HARDWARE_PARKED, 0);
  slpSliceCount = 0;
  slpLowLevelSlices = 0;
  slpArmRetries = 0;
#if CP_LP_STANDBY
  slpSentryDropped = false;
  slpSentryStarts = 0;
#endif
  sleepJournal("standby enter timeout=%d touch=%d light=%d reader=%d", fromTimeout ? 1 : 0, touchWakeMode(),
               lightWasOn ? 1 : 0, activityManager.isReaderActivity() ? 1 : 0);

  // Records the checkpoint phase if the CPU never comes back; read on the next boot.
  metalio_sleep_checkpoint(METALIO_SLP_PHASE_BEFORE_SLEEP, 0);
  metalioStandbyLoop();

  // ---- resume (in place: no esp_restart, no boot screen, RAM and the heap untouched) --------
  metalio_sleep_checkpoint(METALIO_SLP_PHASE_RESTORED, 0);
  // r131: THIS RESUME'S PARK IS r104's (751ce6c4), and that park cuts no load: no usb pads, no
  // frontlight pads, no radio stack, no CNNT SD pads, no P0.5. Every restore below is therefore
  // guarded by a constant false and compiles to nothing -- the legs stay in the text because the
  // wake half is what this build is testing, not because anything was cut. A build that armed a
  // sink here without its restore would be a parked light / a card that never comes back.
  bool usbCut = false;
  bool radioCut = false;
  bool frontCut = false;
  bool sdCut = false;
  int camCutState = 0;
  // ---- r108 ORD2: RAILS FIRST, THEN THE CARD --------------------------------------------------
  // The park unmounts the card BEFORE it cuts a rail (r107 P3/ORD1: SD unmount -> P0.5 -> P1.4),
  // so the resume has to mirror it: P1.4 comes back, it settles 80 ms, P0.5 comes back, and only
  // then is the SD remounted. r107 remounted FIRST, which put the SDMMC re-init and its first
  // transactions on a bus whose rails were still down -- the same shape as the defect the park
  // half was fixed for, which is why this is a fix and not a preference.
  //
  // The journal is the one step that depends on the card, and the card is exactly what is not
  // mounted yet: a sleepJournal() before the remount would be a silent no-op. So each rail
  // outcome goes into the RAM ring here (LOG_INF, flushed later) and into sleep.txt as ONE line
  // written by the remount block below -- the first moment a write can land. Nothing else moves:
  // this function still has zero `return` statements, so no early exit can skip a restore, and
  // every restore is still guarded by its own flag (ORD3 included, via camCutState).
  LOG_INF("PWR", "resume order: P1.4 -> P0.5 -> SD mount (mirror of the park: SD unmount -> P0.5 -> P1.4)");

  const char* paRestoreText = "off";
  const char* camRestoreText = "off";
  bool paRailUp = true;  // stays true when the PA arm is off: nothing was cut, nothing to settle
  if (standbytrim::active(standbytrim::Trim::PaRail)) {
  // ---- CP_CUT_PA_RAIL_ON_STANDBY: re-assert P1.4 FIRST (reverse of the park) ------------------
  // The order is the safety argument: the shared BT/PA rail comes back first and gets its settle
  // while the EPD bus is restored, and only then does epd_poweron() start the TPS boost. Get it
  // backwards -- repower first, PA rail last -- and the BT module's inrush lands on 3V3 inside
  // the 200 ms x 3 PWR_GOOD window (epd_board_...c:915-947): the gate can time out, no rails come
  // up, and the frame is blank until the next paint retries.
  //
  // 80 ms is not invented: epd_board_init() waits exactly this long for a previously-cut P1.4 to
  // come up before it raises ANA_SW (:709-711), and it is the only settle figure the tree has for
  // this rail. The vendor uses the same number (POWER_RAIL_SETTLE_MS) between P14 and P5, and the
  // park's P0.5->P1.4 is mirrored here as P1.4 -> 80 ms -> P0.5 -> card.
  if (!epd_board_metalio_eink4_plus_pa_rail_set(true)) {
    paRailUp = false;
    paRestoreText = "FAILED";
    LOG_INF("PWR", "resume order: P1.4 re-assert FAILED (TCA) — panel repower may band");
  } else {
    delay(80);
    paRestoreText = "back 80ms";
    LOG_INF("PWR", "resume order: P1.4 BT/PA rail back (80 ms settle, before P0.5 and the card)");
  }
  }
  if (standbytrim::active(standbytrim::Trim::CamRail)) {
  // ---- CP_CUT_CAM_RAIL_ON_STANDBY: P0.5 LAST, and only if P1.4 is actually up ------------------
  // Vendor rule (:344-362): if P14 did not come back, P5 stays DOWN -- raising the second load on
  // a rail that is not up stacks two inrush events inside the same PWR_GOOD window. That is why
  // the failure above is remembered instead of ignored.
  //
  // r108 ORD3: `camCutState == 1` is the ONLY state that restores P0.5. State 2 means the park
  // SKIPPED the cut (SD was still mounted) and state 3 means the cut write failed with the rail
  // left up -- in both, the rail never went down, so writing the port here would "restore" a rail
  // that was never cut and make the park/resume record contradict itself. The rail is already up,
  // which is exactly what the touch wake below needs.
  // No extra settle is inserted after P0.5: the resume already spends KWAKE_SETTLE_MS (250 ms)
  // before the first drive, which is three times the vendor's 80 ms after P5 -- and this tree
  // deliberately declined the vendor's second wait for the same reason (no cellular module on
  // this board).
  if (camCutState != 1) {
    camRestoreText = camCutState == 2 ? "skipped(SD mounted)"
                     : camCutState == 3 ? "cut failed(up)"
                                        : "off";
    LOG_INF("PWR", "resume order: P0.5 not restored — the park did not cut it (state %d); rail is already up",
            camCutState);
  } else if (!paRailUp) {
    camRestoreText = "left down";
    LOG_INF("PWR", "resume order: P0.5 left DOWN — P1.4 is not up (vendor order)");
  } else if (!epd_board_metalio_eink4_plus_cam_rail_set(true)) {
    camRestoreText = "FAILED";
    LOG_INF("PWR", "resume order: P0.5 re-assert FAILED (TCA) — CAM/SCR rail left down");
  } else {
    camRestoreText = "back";
    LOG_INF("PWR", "resume order: P0.5 CAM_SCR rail back (after P1.4 + 80 ms)");
  }
  }

  // ---- r155: THE BUS IS RE-ARMED AFTER THE SETTLE, WHICH IS THE VENDOR'S ORDER ---------------
  // r147 re-armed the bus on the P0.5 rail edge, i.e. DURING the settle, and only then waited.
  // The vendor does the opposite: power_hw_standby_rails_restore() (power_hw.cc:213-216) performs
  // RestoreStandbyRails() *followed by* bus_restore(), and the wake path (power_hw.cc:388-392)
  // calls standby_rails_restore() before power_hw_sd_restore(). Backing that up, config.h:82
  // defines POWER_RAIL_SETTLE_MS 80 as the wait "after peripheral power-up, BEFORE restoring the
  // next rail or the display bus", and epd_board_metalio_eink4_plus.c:356/366 spends it between
  // P14 and P5. So the bus is clocked only once the rails are stable: rails -> settle -> bus.
  // r155 is exactly that, and nothing else: the rail order, the 80 ms settle, the 50 ms dwell and
  // the mount-after-bus rule are all unchanged from r147.
#if CP_WAKE_RAIL_SETTLE
  // The vendor's P5 wait, in its vendor position: after the last rail is up and BEFORE any pin of
  // the display bus is driven again. (r147 had this delay after the re-init.)
  vTaskDelay(pdMS_TO_TICKS(CP_WAKE_RAIL_SETTLE_MS));
  LOG_INF("WAKE", "rail settle %d ms after P0.5 up, before the bus reinit (vendor order: rails -> settle -> bus)",
          CP_WAKE_RAIL_SETTLE_MS);
#endif
  epd_board_metalio_eink4_plus_bus_restore();

  if (standbytrim::active(standbytrim::Trim::SdPark)) {
  // ---- CP_SD_PARK_ON_STANDBY: THE CARD COMES BACK LAST (r108 ORD2) -----------------------------
  // This used to be the first statement of the resume. It is now the last hardware step, after
  // both rails, because the card is the device whose bus the rails feed: it is re-initialised on
  // a live bus or not at all. One remount per standby -- the card was parked once at entry, not
  // once per slice. The activity state, the settings and the journal still live on the card, but
  // nothing above this point reads them.
  //
  // This is still the entire wake cost of the switch, and it is why it is measured rather than
  // assumed: SdmmcBlockDevice re-runs the board's timed power-cycle around each mount, so the
  // number can be anything from tens to hundreds of ms. If it comes back above KWAKE_SETTLE_MS
  // (250 ms) the card is a visible addition to resume latency and the next step is to park only
  // after N minutes of standby (METALIO_STANDBY_SLICE_US is 60 s, so N = 2 is the natural first
  // value) instead of on every entry.
  {
    // ---- E4f restore: re-claim the CNNT pads BEFORE the mount that first touches them --------
    // ded_sel must be back to 1 (and the pull-downs off) before sdmmc_host_init adopts the pads,
    // or the driver's own configuration fights the pad-control block for the first transfer.
    // Guarded by the cut flag, so a sink that was disarmed at park is not "restored" here. This
    // runs AFTER the rails on purpose: the pads belong to the same bus they feed.
    bool sdPadsOk = true;
    if (sdCut) {
      sdPadsOk = parksink::sdPadsRestore();
      LOG_INF("PWR", "resume sink sd: CNNT SDIO pads re-claimed (ded_sel=1) ok=%d", sdPadsOk ? 1 : 0);
    }
    const unsigned long sdT0 = millis();
    // r109 CARD1: the pads are back (or were never released) but the card's supply has only just
    // returned; let it settle before the first read of the mount, which is already preceded by the
    // driver's own HIGH->LOW power cycle. See platform/SdPowerCycle.h.
    sdpower::dwellBeforeMount("resume");
    bool sdOk = Storage.begin();
    if (!sdOk) {
      delay(20);
      sdOk = Storage.begin();  // one retry: the mount power-cycles the card and can miss once
    }
    const unsigned long sdMs = millis() - sdT0;
    // r109 CARD2: a remount is a fresh mount too -- if the volume under us changed (or came back
    // without its marker) the boot's vouching no longer applies, so the check runs again.
    if (sdOk) Storage.checkVolumeMarker();
    // r108 ORD2: the durable copy of the ORDER. Until this line the card was DOWN, so this is the
    // first write that can land -- it is written after the fact, which is why the ring line at the
    // top of the resume exists: together they show the order even when the mount itself failed.
    sleepJournal("standby resume: order P1.4 -> P0.5 -> SD mount; P1.4=%s P0.5=%s pads=%s SD=%s %lu ms/%lu sl",
                 paRestoreText, camRestoreText,
                 sdCut ? (sdPadsOk ? "re-claimed" : "FAILED") : "not cut",
                 sdOk ? "ok" : "FAILED", sdMs, static_cast<unsigned long>(slpSliceCount));
    s_autoflushSdParked = false;  // r98.3: the card is back; auto-flush may touch it again
  }
  } else {
  // The card never left (SD trim off), so the same durable record can be written inline now that
  // both rails are back.
  sleepJournal("standby resume: order P1.4 -> P0.5 -> SD mount; P1.4=%s P0.5=%s; SD stayed mounted (trim off)",
               paRestoreText, camRestoreText);
  }

  // ---- E4c/E4e restore: the console and the WiFi stack, both before any paint ----------------
  // USB pads FIRST: with a cable attached at wake, the host has to be able to enumerate
  // before the session produces anything worth reading. This lands after the SD remount
  // only because the journal line has to be written to a mounted card; the remount is a
  // ~70 ms step, and no card access happens between the two.
  if (usbCut) {
    const bool padsUp = parksink::usbPhyUp();
    LOG_INF("PWR", "resume sink usb: PHY+JTAG pads back (usb_pad_enable=%d)", padsUp ? 1 : 0);
    sleepJournal("resume sink usb: PHY pads back (usb_pad_enable=%d)", padsUp ? 1 : 0);
    usbCut = false;
  }
  if (radioCut) {
    int err = 0;
    const bool ok = parksink::radioUp(&err);
    LOG_INF("PWR", "resume sink radio: esp_wifi_init %s err=0x%X (Arduino's buffer cfg)", ok ? "ok" : "FAILED", err);
    sleepJournal("resume sink radio: esp_wifi_init %s err=0x%X", ok ? "ok" : "FAILED", err);
    radioCut = false;
  }
  // (r108 ORD2/ORD3: the P1.4 and P0.5 restores used to live here, AFTER the SD remount. They
  // moved above it -- rails first, card last -- so this spot now holds nothing but the note. The
  // touch wake below is still the last hardware step of the resume, and still fires only after
  // both rails are back.)
  // ---- TCH3 (r101.21): the FT6336U wake moves AFTER the P0.5 CAM_SCR restore ------------------
  // Why this ORDER and not the old one. `gpio.resumeAfterSleep()` writes 0xA5 = 0x00 and reads
  // the mode back. Before r101.21 that write happened immediately after the SD remount, i.e.
  // BEFORE the P1.4 (BT/PA) and P0.5 (CAM_SCR) restores -- so under the CAM arm it landed on a
  // controller whose rail was still down: the transfer NAKs, the read-back reports dead, and
  // touchIrqReady stays armed from the previous session while the glass answers nothing.
  //
  // Why this is safe to ship ARMED without the device (it is the one item in the batch that is
  // not behind a runtime arm, so the argument has to be stronger): with CP_CUT_CAM_RAIL_ON_STANDBY
  // OFF -- the shipped default (idf/CMakeLists.txt) -- P0.5 is never cut, so
  // epd_board_metalio_eink4_plus_cam_rail_set(true) in the block above is a re-assert of a rail
  // that never went down and the write is safe in either position. The only difference a default
  // image can see is that the touch wake now happens <=80 ms later in a resume that already
  // spends KWAKE_SETTLE_MS (250 ms) settling before the first drive. Revert is one call moved.
  // test/standby_resume_order/StandbyResumeOrderTest reads these text offsets, so the tripwire
  // went GREEN with this move (its EXPECTED_RED line is deleted in the same commit).
  const bool touchOk = gpio.resumeAfterSleep();  // 0xA5 = 0x00 with a mode read-back, rail up
  // The park is over: arms may be changed again, and the journal above has already recorded the
  // state that was measured.
  standbytrim::setParked(false);
  // r98.3: resume half of the wake-time record. The gauge bus never left, and the card is mounted
  // again by here (CP_SD_PARK_ON_STANDBY) or was never parked, so both the 0x10 read and its
  // journal line are safe. r106 B2: this resume mark is emitted BEFORE anything on the wake path
  // re-parks the card, and the RAM ring is flushed immediately afterwards, so the [PWR] resume line
  // is on the card even if the paint that follows never flushes.
  wakerecResume();
  // r163 E2: the gauge machines' wake edge -- keeps the full-charge confirmation (a pack
  // that was full before the sleep still is), opens the 3 s post-wake settle and re-arms
  // the one-shot SoC recalibration. Called AFTER the rails/bus are back and BEFORE the
  // first gauge sample of the new session, which is what the settle window is measured from.
  powerManager.notifyStandbyWake();
  standbyAutoFlush("resume mark");
  // Re-arm the parallel bus (gpio_hold_dis + epd_lcd_reinit) BEFORE the first paint, then re-open
  // the paint gate that deepSleep() closed.
  // r147: the bus restore and the 80 ms CP_WAKE_RAIL_SETTLE used to live HERE, after the SD
  // remount. Both moved up to the P0.5 rail edge (see the block above the SdPark section); what
  // is left here is the paint-gate re-open, the first thing in the resume that wants a clocked
  // panel -- and it now runs with the bus already armed.
  display.wake();
  if (frontCut) {
    // ---- E4d restore: re-attach the LEDC channels and re-drive the EXACT duty the park cut ----
    // brightness/warmth were never modified (parkPads only detaches and holds the pads), so
    // this is the same percentage and the same colour split the user left -- byte-for-byte,
    // because apply() is deterministic from those two values.
    const bool ok = Frontlight.releasePads();
    LOG_INF("PWR", "resume sink front: LEDC re-attached, brightness=%u%% warm=%u%% restored ok=%d",
            Frontlight.brightness(), Frontlight.warmth(), ok ? 1 : 0);
    sleepJournal("resume sink front: LEDC re-attached, brightness=%u%% warm=%u%% restored ok=%d",
                 Frontlight.brightness(), Frontlight.warmth(), ok ? 1 : 0);
    frontCut = false;
  } else if (lightWasOn && Frontlight.present()) {
    Frontlight.setOn(true);                      // brightness/warmth were never changed
  }
  // Rails-first wake (open concern 22). Measured on device: the FIRST drive after epd_poweron()
  // bands, whichever waveform it runs - on r56 a GC16 diff banded and the from-white 0.8 s later was
  // clean. So power the rails HERE, before the frame is composed, and let them settle while Home
  // draws. The first drive is then an ordinary mid-session drive, which is the case that has always
  // been clean (sleep entry, book open). KWAKE_SETTLE_MS is deliberate dead time on a resume that is
  // about to spend ~70 ms rendering anyway.
  // r112 F1': the instant the rails came back. The settle below is a DEADLINE measured from
  // here, so every millisecond this resume already spends (SD flush, rail restores) counts
  // toward CP_WAKE_SETTLE_MS instead of being added on top of it.
  const uint32_t wakeRepowerAtMs = millis();
  if (!metalio_vendor_panel_repower()) {
    LOG_INF("PANEL", "wake: repower reported rails not ok; the drive will retry via PWR_GOOD");
  }
  // r98.3 CP_AUTOFLUSH_ON_STANDBY: the resume flush runs here, inside the deliberate settle
  // window, and the settle below is shortened by exactly what the flush cost -- so the log on the
  // card is current after every wake AND the first paint is not delayed by it.
  {
    const unsigned long flushMs = standbyAutoFlush("resume");
    delay(flushMs < KWAKE_SETTLE_MS ? (KWAKE_SETTLE_MS - flushMs) : 0);
  }
#if CP_WAKE_SETTLE_EXTRA_MS > 0
  // r96.3 (D2) RAIL-SETTLE EXPERIMENT, default OFF. The only difference between the resume
  // clean and the mid-session cleans that never band is what state the rails were in: this
  // one lands ~KWAKE_SETTLE_MS (250 ms) after the TPS65185 came back from SLEEP, where the
  // rails were discharged for hours. Extra dead time here separates "the rails had not
  // settled" from "the waveform's first pass after power-on is wrong": if the band goes away
  // at +800 ms, it is settling and the fix is time (or a cheaper pre-charge); if it does not,
  // it is the waveform and no amount of waiting will help. Nothing else changes -- the drive
  // sequence stays one from-white GC16.
  delay(CP_WAKE_SETTLE_EXTRA_MS);
  LOG_INF("WAKE", "rail-settle test: extra %d ms before the resume clean (total settle %d ms)",
          CP_WAKE_SETTLE_EXTRA_MS, static_cast<int>(KWAKE_SETTLE_MS) + CP_WAKE_SETTLE_EXTRA_MS);
#endif
#if CP_RICKY_WAKE_PRIME
  // r82: spend the post-poweron banding on a throwaway frame, exactly as the auto-off path does
  // (metalioHardPowerOff). R55-R57 measured that the FIRST drive after epd_poweron() bands and the
  // second does not, which is why the auto-off path already does wake -> settle -> throwaway FULL
  // -> paint. This makes resume take the same shape; the frame driven here is the sleep screen that
  // is about to be replaced, so its content never matters, only that the panel has taken one
  // whole-panel drive on warm rails.
  const unsigned long primeDriveMs = metalioWakePrimeDrive(renderer);
  sleepJournal("resume wake-prime: throwaway FULL %lu ms", primeDriveMs);
#endif
  // The panel is unpowered-but-bistable and the sleep image is stale by one frame; force the
  // first paint after resume to be a full one so it cannot ghost over the sleep screen.
  //
  // r96.1 D2: arm BOTH one-shots, deliberately. `promoteNextRefresh()` is cancellable -- any call
  // site that wants to commit its own waveform calls cancelPromotedRefresh() before its drive
  // (HomeActivity's first-render follow-up does exactly that, and so do ClockSync / Debug /
  // FontDownload), and a cancelled promotion turns the wake paint into the page-turn mode: GL16,
  // whose erase/saturate phases are the inverted intermediate frame the operator reads as a black
  // flash mid-transition. `forceCleanNextRefresh()` is NOT cancellable -- it is a term the policy
  // reads at paint time (GfxRenderer.cpp:1948) -- so the arriving drive is a from-white FULL even
  // if the arriving screen parks promotions. The two agree by construction, so this cannot double
  // a drive: both are consumed by the SAME first displayBuffer().
  // ---- r113: F1' REVERTED. The r112 settle deadline (CP_WAKE_SETTLE_MS) was built on the
  // hypothesis that the first post-park drive bands because the rails/panel are not settled.
  // Falsified on device: at 1500 ms the bands were unchanged, and they are absent when the
  // sleep image is drawn on an already-powered panel. So no extra wait is imposed here - the
  // observed settle is logged only, so the next round can correlate it with the banding for
  // free. The drive stays a SINGLE GC16-from-white: no prime, no double flash.
  {
    LOG_INF("WAKE", "settle actual=%lu mode=FULL", static_cast<unsigned long>(millis() - wakeRepowerAtMs));
  }
  // ---- r114d: THE WAKE PRIME, BEFORE ANY CONTENT DRIVE ---------------------------------------
  //
  // r114c proved the cold-boot prime works but issued it from inside the first wake paint, so the
  // panel took a content drive first: on glass, banded drive (ms=1170) -> 69-scan clear -> clean
  // content drive. The routine was right; the order was wrong. This runs the SAME clear-mode scan
  // cold boot runs, synchronously, on the resume task, before promoteNextRefresh()/forceClean
  // arm anything and therefore before the Home transition's first displayBuffer() can reach the
  // panel. Cold boot's order is reproduced exactly: epd_clear -> TPS off/on -> first LUT drive.
  //
  // The drive capture is armed FIRST so the `(+N ms after resume)` on `[WAKE] drive 1` measures
  // the whole wake, prime included, and so the capture deadline can be extended by the prime.
  g_wakeDriveN = 0;
  g_wakeCaptureStartMs = millis();
  g_wakeCaptureDeadlineMs = g_wakeCaptureStartMs + static_cast<unsigned long>(CP_WAKE_CAPTURE_MS);
  g_wakeCapture = true;
  LOG_INF("WAKE", "resume: drive capture armed window=%d ms (one line per drive, summary at close)",
          CP_WAKE_CAPTURE_MS);
#if CP_WAKE_WARM_REDRIVE
  // r131: WARM RE-DRIVE, present but DEFAULT 0 (CP_WAKE_WARM_REDRIVE == OFF in this image). It is
  // written here so the switch is a one-line build change and nothing else: a resume-only warm-up
  // drive of the canvas ALREADY on the glass, issued BEFORE the wake prime, so the panel's weak
  // first-drive window is spent on pixels that are already there instead of the frame the reader
  // is about to see. The shim entry point was deleted with the rest of the r105..r127 scaffolding
  // in r128 -- restore it before setting this to non-zero, or this block will not link.
  extern "C" int metalio_vendor_panel_warm_redrive(int mode, unsigned* out_ms);
  {
    unsigned warmMs = 0;
    const int warmErr = metalio_vendor_panel_warm_redrive(CP_WAKE_WARM_REDRIVE, &warmMs);
    g_wakeCaptureDeadlineMs += warmMs;  // the warm drive is inside the wake: extend the window
    LOG_INF("WAKE", "warm re-drive mode=%d ms=%u err=0x%x (canvas already on the glass)",
            CP_WAKE_WARM_REDRIVE, warmMs, static_cast<unsigned>(warmErr));
  }
#endif
#if CP_WAKE_RAW_CLEAR
  // r131: the prime's entry point is declared inside the guard on purpose -- the shim stopped
  // exporting it in r128 (nothing called it), so at CP_WAKE_RAW_CLEAR=0 this file must not mention it.
  extern "C" int metalio_vendor_panel_wake_prime(int mode, unsigned* out_ms, int* out_scans);
  {
    unsigned primeMs = 0;
    int primeScans = 0;
    const unsigned long primeAt = millis() - g_wakeCaptureStartMs;
    LOG_INF("WAKE", "prime mode=%d scans_expected=%d starting at resume+%lu ms, BEFORE the first "
                    "content drive",
            CP_WAKE_PRIME_MODE, CP_WAKE_PRIME_MODE == CP_WAKE_PRIME_3_CYCLES   ? 69
                                : CP_WAKE_PRIME_MODE == CP_WAKE_PRIME_1_CYCLE  ? 23
                                : CP_WAKE_PRIME_MODE == CP_WAKE_PRIME_WHITE_ONLY ? 13
                                : CP_WAKE_PRIME_MODE == CP_WAKE_PRIME_BLACK_ONLY ? 10
                                                                                : 0,
            primeAt);
    const int primeErr = metalio_vendor_panel_wake_prime(CP_WAKE_PRIME_MODE, &primeMs, &primeScans);
    // The prime is inside the wake, so it must not eat the post-prime window of the capture.
    g_wakeCaptureDeadlineMs += primeMs;
    LOG_INF("WAKE", "prime done mode=%d scans=%d ms=%lu err=0x%x -- panel is white, next drive is "
                    "the ONLY content drive this wake",
            CP_WAKE_PRIME_MODE, primeScans, static_cast<unsigned long>(primeMs), static_cast<unsigned>(primeErr));
  }
#endif
  renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
  renderer.forceCleanNextRefresh();
  // r96.2 (D2): the wake is ONE from-white drive. The two one-shots above make the
  // FIRST paint a from-white FULL; CP_WAKE_ONE_DRIVE additionally stops the drive(s)
  // that used to follow it over the same content (row 9's GL16 completion and Home's
  // first-render follow-up), because on glass those read as a band after a clean. The
  // log line is the marker: everything between `[WAKE] arm` and the next paint must be
  // exactly one `[GFX] paint ... force=1 applied=3` line.
  LOG_INF("WAKE", "resume: one-drive clean armed (promotion FULL + forceClean, one-drive=%d)",
#if CP_WAKE_ONE_DRIVE
          1
#else
          0
#endif
  );
  // r97.1 WAKE1: the arm's STATE and its TARGET, in one greppable line. Read back through the
  // renderer's own accessor rather than assuming: forceCleanNextRefresh() has no return value,
  // and cp_refresh_policy_note_park() clears it, so "armed=0" here would mean something between
  // this line and the next displayBuffer() parked the panel.
  LOG_INF("WAKE", "arm: target=FULL(%d) forcedCleanArmed=%d promotionArmed=FULL -- the NEXT displayBuffer() consumes both",
          static_cast<int>(HalDisplay::FULL_REFRESH), renderer.forcedCleanArmed() ? 1 : 0);
  // r96.3 (D2): counting started above, before the prime, so the window and the drive-1 latency
  // both cover the whole wake. Everything from there to the window close is a drive on a resume,
  // and the operator's band is somewhere in that set. `drives=N` is the evidence that decides
  // between "a single clean that bands" and "a drive we have not suppressed".
  // The resume IS user activity. millis() advanced by the whole standby, so without this reset the
  // inactivity timer sees hours of "idleness" and auto-sleeps (fromTimeout=true) ~1.2 s later,
  // painting Quick-Resume-with-moon instead of the configured sleep screen.
  lastActivityTime = millis();
  // r100.2: the same reasoning for the POWER key. The press that woke the device was consumed in
  // standby (metalioVerifyStandbyWake waits up to 2 s for the finger to come off), but a finger
  // still down when the awake loop resumes would be tracked as a FRESH press here, and its release
  // would be classified as a short press -- sleep, immediately after a wake. This is the awake-UI
  // half of the powerReleasedSinceWake guard, and it costs one comparison.
  if (millis() > allowSleepAt) allowSleepAt = millis() + 1500;
  sleepJournal("resume touch=%d mode=%u light=%d slices=%lu", touchOk ? 1 : 0,
               static_cast<unsigned>(touchWakeMode()), lightWasOn ? 1 : 0,
               static_cast<unsigned long>(slpSliceCount));

  // Back to the screen the user fell asleep on. goToSleep() replaced the reader activity with
  // SleepActivity, but openEpubPath/progress are intact, so re-entering it rebuilds the same page
  // without a reboot.
  if (!APP_STATE.openEpubPath.empty() && APP_STATE.lastSleepFromReader && APP_STATE.readerActivityLoadCount == 0) {
    activityManager.goToReader(APP_STATE.openEpubPath, /*allowFastInitialRefresh=*/true);
  } else {
    activityManager.goHome();
  }
}
#endif

void enterDeepSleep(bool fromTimeout = false) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  // r91 R3: a manual sleep ends the debug session. The shell is a debug tool for an AWAKE
  // device; keeping the REPL (and R1's hold-awake) across standby would fight the user and
  // leave the host pipe pointing at a parked console. Session only -- the persisted
  // preference stays as the operator left it, so the menu row reads "ON (inactive)".
  usbshell::clearSession(fromTimeout ? "auto-sleep" : "manual sleep");
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();
  // Sleep may end in a power-off (battery death, latch); persist the clock
  // floor now so a later cold boot resumes from it.
  trustedtime::note();

  // Stop Bluetooth before plugin delivery can start WiFi; a timeout defers delivery.
  deepSleepInProgress = true;
  const bool bluetoothStopped = bleturner::beforeSleep(2000);
  if (!bluetoothStopped) LOG_ERR("SLP", "Bluetooth did not stop; deferring sleep-event delivery");
  deliverSleepPluginEvents(bluetoothStopped);

  // Timeout and manual sleep agree on the frame (see SleepActivity::onEnter), so the quick-resume
  // mode is decided by the setting alone here too.
  const bool isQuickResumeSleep = SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME;
  // Every sleep mode leaves a complete retained frame on the e-ink panel. Keep
  // it visible until the first useful reader or home paint replaces it.
  APP_STATE.showBootScreen = false;

  APP_STATE.saveToFile();

  // r97 BATT2: close the awake window and commit the record BEFORE the panel
  // parks -- the interval until the next sample is standby, and the stats layer
  // must not have to guess whether it was awake.
  batteryStats.onEnteringSleep();

  activityManager.goToSleep(fromTimeout);

  // sleep_frame.bin is read only by the cold-boot SplashlessWake path. Metalio standby resumes in
  // place with the framebuffer still in RAM, so dumping it (~64-86 KB) to the card buys nothing.
  bool frameDumpNeeded = true;
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::MetalioEInk4Plus) frameDumpNeeded = false;
#endif
  if (isQuickResumeSleep && frameDumpNeeded) {
    saveSleepFrameBuffer();
  } else if (Storage.exists(SLEEP_FRAME_FILE)) {
    // A stale Quick Resume frame must not replace the selected sleep screen during wake.
    Storage.remove(SLEEP_FRAME_FILE);
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  // Suspend the background reconnect ladder FIRST: it retries on every STA_DISCONNECTED that is not
  // ASSOC_LEAVE, so a reconnect racing this teardown would re-init the radio mid-standby.
  wificonnect::suspendAutoReconnect();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::MetalioEInk4Plus) {
    // Standby on this board is the vendor's light-sleep model, not deep sleep. It parks the panel
    // and the touch controller itself, keeps the SD card mounted, sleeps in slices on the expander
    // INT (only POWER can assert it) and returns here on a confirmed short press -- RAM, heap and
    // open book intact, no boot screen, no esp_restart(). A >4 s hold powers the board off through
    // the PMIC latch and never returns.
    // Standby owns the power key from here on: drop the awake-UI hold stamp so the press that
    // put us to sleep (or that wakes us) is never re-classified as a fresh in-app hold.
    metalioPowerHoldActive = false;
    metalioPowerGesture.resetPress(millis());
    metalioStandbySleep(fromTimeout);
    // BATT3: THIS RETURN IS NOT A REBOOT. Standby on this board wakes with RAM, the heap and
    // the open book intact and returns here, so every "we are on the way to sleep" latch has
    // to be dropped at the resume or it stays armed for the rest of the session. That is
    // exactly what happened to deepSleepInProgress: `enterDeepSleep()` sets it at :2049 and
    // nothing ever cleared it, so after the device's FIRST sleep the battery-icon poll
    // (main.cpp loop(), `if (!deepSleepInProgress) cpBatteryIconPollAndRepaint(...)`) was
    // never called again and the charging glyph could not repaint until the next cold boot.
    // It reads as "the icon used to work and then stopped": it works from boot until the
    // first standby, which on this unit is the first minute of the session.
    if (deepSleepInProgress) {
      deepSleepInProgress = false;
      LOG_INF("SLP", "standby resumed in place: deepSleepInProgress cleared (icon poll re-armed)");
    }
    // Belt and braces with the reset inside the resume block: any future early-return path out of
    // standby still leaves the inactivity clock fresh.
    lastActivityTime = millis();
    // r163 E6: the same reasoning for every NEED age. millis() advanced by the whole
    // standby, so a hold that spanned it would otherwise look old and announce a leak
    // that never happened. Counts and budgets are untouched -- a genuinely stuck hold
    // still ages out, one park later.
    powerneed::sys().rebase(millis());
    return;
  }
#endif

#if FREEINK_DEVICE_PICCO
  // The I2C LED driver keeps its state through deep sleep; switch it off here
  // (SETTINGS.frontlightOn keeps the preference for restore-on-wake).
  Frontlight.setOn(false);
  const bool wakeOnHeldButton = holdSleepWhileUsbPowered();
#else
  const bool wakeOnHeldButton = false;
#endif

  halTiltSensor.deepSleep();
  halClock.disableAlarm();  // the RTC INT shares GPIO2 with the wake source
  display.deepSleep();
  Storage.prepareForDeepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio, wakeOnHeldButton);
}

// BOOT1 -- THE EARLY BOOT FRAME.
//
// Between display.begin() and the first activity paint the panel is given nothing:
// on a cold boot the glass holds whatever the driver's boot clear left, and the
// operator sees ~2 s of white. Nothing blank is being DRIVEN there -- the init
// ladder (SD font scan, font cache, the remaining setup) simply has not produced a
// frame yet. So the fix is ordering, not a flash: put a frame on the panel as soon
// as the renderer can draw one, and let the rest of init run while the operator is
// looking at it.
//
// THE BANDING RULE, and why this frame is the one that pays it. The FIRST drive
// after epd_poweron() bands, whichever waveform it runs (measured; see the same
// note on the wake path, main.cpp "Rails-first wake" and the auto-off path). The
// tree's strategy is to spend that first drive on a frame whose content does not
// matter. This frame IS that consumer: a logo on white, whose banding is cosmetic
// and on screen only for the length of the init that follows. BootActivity's frame
// is then the SECOND drive -- the clean one -- and that is the frame that stays on
// glass. Do not try to make this drive clean; that is the trap the repo has already
// paid for.
//
// Logo only, no text, deliberately: drawCenteredText() needs a font inserted into
// the renderer, and the fonts are inserted further down setupDisplayAndFonts(),
// after this point. Drawing a literal or an empty string as a stand-in would put ink
// on the glass that the real boot screen does not have, so the caption waits for the
// second frame instead.
//
// Safe only after display.begin() + renderer.begin(): the caller below is the single
// call site and it sits after both. Nothing here paints before the panel driver is
// initialised, and a boot that fails earlier (SD mount) never reaches this path, so
// a painted logo can never stand in front of a failure code.
static void paintEarlyBootFrame() {
  BootActivity::paintBase(renderer);  // identical pixels to BootActivity's base layer
  // FULL: a from-white whole-panel drive is what this frame is (it replaces the whole
  // panel). It is also the drive the boot clear used before BOOT1, so the banding this
  // spends is the banding the boot screen used to wear.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  // BOOT1 instrument: the first of the two timestamps whose gap used to be a blank
  // panel. The second is "[PANEL] boot screen driven at N ms" (BootActivity::onEnter).
  LOG_INF("PANEL", "early boot frame (logo) driven at %lu ms -- first drive after poweron, band consumer",
          static_cast<unsigned long>(millis()));
}

// `earlyBootFrame` is opt-in and only ever passed by the ordinary cold-boot path
// (resume == Splash). A silent reboot and a splashless wake both rely on the glass
// keeping the previous frame, and the SD-failure path must show its error message and
// nothing else -- none of those may have a logo painted under/over them.
void setupDisplayAndFonts(bool seamless = false, bool earlyBootFrame = false) {
  // SUB-STEP PULSES inside the display init (counts 5-7, continuing setup()'s
  // ladder). GPIO6 = COOL, GPIO7 = WARM, driven directly because the frontlight
  // API is skipped on this board. Measured from video frames, not eyeballed.
  //
  // OFF BY DEFAULT: this is a bring-up diagnostic, not a product behaviour. It
  // costs 0.6 s per pulse + 1.2 s settle, and dpulse(5..7) = 18 pulses = ~14.4 s
  // of delay on every boot, during which the frontlight strobes warm/cold. It
  // also wrote GPIO6/7 without pinMode(), which is where the 46
  // "IO 6/7 is not set as GPIO" boot errors came from. Define CP_DPULSE to
  // bring it back for a bring-up session.
  auto dpulse = [](int k) {
#ifdef CP_DPULSE
    digitalWrite(6, LOW);
    for (int i = 0; i < k; ++i) {
      digitalWrite(7, HIGH); delay(250);
      digitalWrite(7, LOW);  delay(350);
    }
    digitalWrite(6, HIGH);
    delay(1200);
#else
    (void)k;
#endif
  };
#if !FREEINK_MCU_C3
  // C3 resolves its controller in HalGPIO::begin() before SPI claims the
  // display pins. X4 Pro skips that C3-only path, so probe here before
  // display.begin() selects and initializes its panel driver.
  static bool controllerResolved = false;
  if (!controllerResolved) {
    controllerResolved = true;
    if (freeink::applyXteinkDisplayController()) {
      LOG_DBG("MAIN", "Panel controller: UltraChip UC81xx variant detected");
    }
  }
#endif

#ifdef CP_DPULSE
  digitalWrite(6, LOW);   // COOL off: if it stays dark, death is INSIDE display.begin()
#endif
  display.begin(seamless);
  dpulse(5);              // sub-step: display.begin() returned (panel may have flashed white)

  // r116: THE COMPILED PM STATE, ON THE BOOT BANNER ITSELF. r105's CONFIG_PM_ENABLE 0->1 plus
  // CONFIG_FREERTOS_USE_TICKLESS_IDLE 0->1 is the only state difference left between the clean
  // r104 and the banded r105, so the image must state which side of that switch it was built
  // from rather than leaving it to a config diff. #ifdef, not #if: sdkconfig.h OMITS a disabled
  // option, so an absent define IS the off answer and there is no stale value to misread.
  {
    int pmOn = 0;
    int ticklessOn = 0;
#ifdef CONFIG_PM_ENABLE
    pmOn = 1;
#endif
#ifdef CONFIG_FREERTOS_USE_TICKLESS_IDLE
    ticklessOn = 1;
#endif
    LOG_INF("PM", "pm=%d tickless=%d", pmOn, ticklessOn);
  }
  renderer.begin();
  activityManager.begin();
  // BENCH1: the drive harness needs the same renderer/input every other debug instrument
  // uses; it stores the pair and does nothing else until a `benchdrives` command or the
  // Debug row arms it (and it refuses outright while debug mode is off).
  benchdrives::init(renderer, mappedInputManager);
  LOG_DBG("MAIN", "Display initialized");
  dpulse(6);              // sub-step: renderer + activityManager begun

  // BOOT1: ink now, before the font work below (which is where the ~2 s of white went).
  // The panel is driven here and only here; everything after this line runs while the
  // operator is already looking at the logo, and BootActivity's frame replaces it.
  if (earlyBootFrame) {
    paintEarlyBootFrame();
  }

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  renderer.insertFont(AMAZONEMBER_12_FONT_ID, amazonember12FontFamily);
  renderer.insertFont(AMAZONEMBER_14_FONT_ID, amazonember14FontFamily);
  renderer.insertFont(AMAZONEMBER_18_FONT_ID, amazonember18FontFamily);
    renderer.insertFont(AMAZONEMBER_20_FONT_ID, amazonember20FontFamily);
    renderer.insertFont(AMAZONEMBER_22_FONT_ID, amazonember22FontFamily);
    renderer.insertFont(AMAZONEMBER_24_FONT_ID, amazonember24FontFamily);

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);
  dpulse(7);              // sub-step: fonts (incl. SD fonts) loaded -- display init complete

  LOG_DBG("MAIN", "Fonts setup");
}

#ifdef CP_VENDOR_EARLY
// Vendor EPDiy facade. The file that used to define it (src/vendor_epd/vendor_epd_text.c) was
// deleted in r97 with the rest of the shadow tree -- the tree was never compiled (CP_VENDOR_EARLY
// and CP_VENDOR_EPD are defined in no build) and idf/components/metalio_driver is the vendor
// display stack that actually runs. Do not re-enable this define: it now has no definition site,
// and the live vendor path is metalio_vendor_panel_begin() below.
extern "C" int vendorEpdBootText(void);
#endif
// Maps PMA entry 7 to the external-RAM window (src/psram_diag.cpp). IDF's prebuilt
// esp_cpu_configure_region_protection() leaves that entry unmapped ("cfg=08000000 addr=00000000"),
// so the first store into PSRAM at 0x50000020 faults and the app reset-loops. The empty override in
// src/esp32s31_region_protect_override.c suppresses IDF's version; this restores the mapping once
// the app is running.
extern "C" void mccrossy_psram_diag(void);

// Reset-reason text: distinguishes a WDT panic from a brownout from a plain power-on.
// ESP_RST_POWERON=1, ESP_RST_PANIC=3, ESP_RST_TASK_WDT=6, ESP_RST_BROWNOUT=9.
static const char* cpResetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON (clean power-up)";
    case ESP_RST_EXT:       return "EXT (external pin)";
    case ESP_RST_SW:        return "SW (esp_restart)";
    case ESP_RST_PANIC:     return "PANIC (exception/abort)";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (starved idle task)";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (supply collapsed)";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB (host/DTR-RTS reset, e.g. a serial port open)";
    case ESP_RST_JTAG:      return "JTAG";
    case ESP_RST_EFUSE:     return "EFUSE error";
    case ESP_RST_PWR_GLITCH:return "PWR_GLITCH (supply glitch)";
    case ESP_RST_CPU_LOCKUP:return "CPU_LOCKUP (double exception)";
    default:                return "UNKNOWN";
  }
}

#if FREEINK_DEVICE_METALIO_EINK4_PLUS
// --- battery profile corrections the SDK's board table cannot express -----------------------------
// Two fields of BoardConfig::ACTIVE.batteryGauge are wrong for this board, and the Metalio profile
// in freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h has no way to say so (the gauge
// struct is positional: {0, 1, 100000, 0x55, 0x6B}). See BATTERY_GAUGE_AUDIT.md D1/D5.
//
// D1  designCapacityMah is 0 here while the pack is 2000 mAh. BatteryMonitor::loadDesignCapacity()
//     returns false on `designCapacityMah != 0` BEFORE it touches the bus, so the BQ27220 keeps TI's
//     3000 mAh Design Capacity -- a denominator 50% too large for the pack -- and the checksummed
//     Data-Memory write machine that already exists (and is host-tested) never runs. 2000 is the
//     pack's rating; 3000 is TI's silicon default, not a measurement of anything on this device.
// D5  chargerType still defaults to Bq25896, and BQ25896's charge status is REG0x0B. The part at 0x6B
//     here is a CX25601N, whose charge status is REG0x1E -- a different register with different
//     meaning, so the SDK's BQ25896 read would be answering a question nobody asked. The SDK enum
//     has no CX25601N member and freeink-sdk is a pinned submodule: rather than teach the SDK a
//     wrong map, make its charger path UNREACHABLE on this board. chargerAddr = 0 means exactly
//     "there is no charger IC I can read with my register map", which is the truth. The charge flag
//     comes from HalPowerManager::cxCharging(), which reads the CX25601N directly (r97.1).
//
// Both are plain assignments to the SDK's public `inline BoardProfile ACTIVE`: no submodule edit.
static void metalioFixBatteryProfile() {
  auto& g = BoardConfig::ACTIVE.batteryGauge;
  g.designCapacityMah = 2000;
  g.chargerAddr = 0;
  LOG_INF("BATT",
          "profile: designCapacityMah=%u chargerAddr=0x%02X (CX25601N at 0x6B is read by cxCharging; "
          "SDK BQ25896 map disabled)",
          static_cast<unsigned>(g.designCapacityMah), static_cast<unsigned>(g.chargerAddr));
}

// --- non-power wake filter -----------------------------------------------------------------------
// The Plus has no power-button GPIO: the only wake line is the expander INT (GPIO2), which is shared
// with the charger (PWR_GOOD, P0.7), the RTC alarm (P1.6) and the IMU. P0.7 cannot be masked without
// driving a pin the TPS is driving, so charger events wake the CPU. This filter runs BEFORE display
// init and puts it straight back to sleep unless the power key is actually down.
//
// Policy for now is discard. Routing a charger wake into a charge-strip redraw instead is filed in
// docs/OPEN_CONCERNS.md (#3/#4).
// One discard per boot, not a streak: a streak loop is what turned a charger wake into six back-to-back
// boot cycles (black/white flashing) that ended with the touch controller hibernated.
RTC_NOINIT_ATTR uint8_t nonPowerWakeDiscarded;  // 0 = not yet discarded since the last real wake

static void discardNonPowerWakes() {
  if (BoardConfig::ACTIVE.board != BoardConfig::Board::MetalioEInk4Plus) return;

  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_UNDEFINED) {  // cold boot / reset: nothing to filter
    nonPowerWakeDiscarded = 0;
    return;
  }
  // Only a pin wake can be a false one.
  if (cause != ESP_SLEEP_WAKEUP_EXT1 && cause != ESP_SLEEP_WAKEUP_EXT0 && cause != ESP_SLEEP_WAKEUP_GPIO) return;

  // USB present means PWR_GOOD is asserted by the charger, which is the very input that wakes us.
  // Discarding there can only loop, because the line is still low when we re-sleep.
  if (gpio.isUsbConnected()) {
    nonPowerWakeDiscarded = 0;
    LOG_INF("PWR", "wake kept: usb present (cause=%d)", static_cast<int>(cause));
    return;
  }

  bool readOk = false;
  const bool keyDown = freeink::metalio4plus::powerKeyPressed(&readOk);
  if (!readOk || keyDown) {
    // Fail safe two ways: an unreadable expander keeps the device awake rather than risking a device
    // that ignores its own power button, and a real press always wins.
    nonPowerWakeDiscarded = 0;
    LOG_INF("PWR", "wake kept: cause=%d keyDown=%d readOk=%d", static_cast<int>(cause), keyDown ? 1 : 0,
            readOk ? 1 : 0);
    return;
  }

  if (nonPowerWakeDiscarded) {
    nonPowerWakeDiscarded = 0;
    LOG_INF("PWR", "already discarded once this boot; booting normally");
    return;
  }
  nonPowerWakeDiscarded = 1;

  LOG_INF("PWR", "discarding non-power wake (cause=%d)", static_cast<int>(cause));
  // Deliberately NOT calling gpio.prepareForDeepSleep(): hibernating the FT6336U here is what left
  // touch dead on the boot that finally survived. The real sleep path parks it.
  powerManager.startDeepSleep(gpio, /*wakeOnHeldButton=*/false);
}
#endif

void setup() {
  // FIRST STATEMENT, before any peripheral or pin is touched: name the reason for
  // the previous reset. Power-on vs panic vs task-WDT vs brownout decides the whole
  // direction of the next investigation, and it must be read before init can perturb it.
  printf("[CP] RESET REASON: %d (%s)\n", (int)esp_reset_reason(), cpResetReasonName(esp_reset_reason()));
  fflush(stdout);
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  // FRONTLIGHT OFF, EXPLICITLY, AND AS EARLY AS POSSIBLE.
  //
  // This replaces the deleted BootBeacon static constructors. Those ran before main(),
  // drove GPIO6 HIGH and left it there, then strobed GPIO7 three times (~1.3 s of dead
  // boot time and a visible flash). Nothing else in the boot path asserts these pins LOW
  // before ensureBooted()/holdPowerRails() -- and those need a live I2C bus first, so the
  // YX6016N EN pin would float from reset until the first expander write. This block is
  // now the only thing that guarantees "no backlight during boot": two pinMode + two
  // digitalWrite, no I2C, no console, safe before any peripheral is initialised.
  pinMode(freeink::metalio4plus::FRONTLIGHT_COOL_PIN, OUTPUT);
  pinMode(freeink::metalio4plus::FRONTLIGHT_WARM_PIN, OUTPUT);
  digitalWrite(freeink::metalio4plus::FRONTLIGHT_COOL_PIN, LOW);
  digitalWrite(freeink::metalio4plus::FRONTLIGHT_WARM_PIN, LOW);
#endif
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  // Before anything reads the gauge: correct the two battery-profile fields the SDK's board table
  // cannot express (2000 mAh pack, and no BQ25896-map charger on a CX25601N board). Idempotent, no
  // I/O, and placed here so a gauge read during any later init already sees the right numbers.
  metalioFixBatteryProfile();
  // `trim` (the runtime A/B toggle) journals through the same sleep.txt the standby path writes,
  // so a run can be reconstructed from one file.
  standbytrim::setJournal(&sleepJournalCStr);
#endif
#ifdef CP_PSRAM_TEST
  // PSRAM availability in the ARDUINO build, reported without a console.
  // epdiy's epd_hl_init() needs two ~416 KB 4bpp framebuffers from PSRAM; if that
  // allocation fails there, the vendor stack dies before drawing anything.
  // 10 pulses = both allocations OK. 4 pulses = allocation FAILED. 7 = PSRAM heap is empty.
  pinMode(6, OUTPUT);
  pinMode(7, OUTPUT);
  digitalWrite(6, LOW);
  digitalWrite(7, LOW);
  delay(1500);
  static const size_t kSz = 416u * 1024u;
  void* a = heap_caps_aligned_alloc(64, kSz, MALLOC_CAP_SPIRAM);
  void* b = a ? heap_caps_aligned_alloc(64, kSz, MALLOC_CAP_SPIRAM) : nullptr;
  int pulses = 4;
  if (a && b) {
    // Touch every page: a mapped-but-broken window faults here, not at alloc time.
    volatile uint8_t* p = (volatile uint8_t*)a;
    for (size_t i = 0; i < kSz; i += 4096) p[i] = (uint8_t)i;
    volatile uint8_t* q = (volatile uint8_t*)b;
    for (size_t i = 0; i < kSz; i += 4096) q[i] = (uint8_t)(i ^ 0xFF);
    pulses = 10;
  } else if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
    pulses = 7;
  }
  for (;;) {
    digitalWrite(7, LOW);
    delay(2500);
    for (int i = 0; i < pulses; ++i) {
      digitalWrite(7, HIGH);
      delay(600);
      digitalWrite(7, LOW);
      delay(400);
    }
  }
#endif
#ifdef CP_LAMP_TEST
  // MINIMUM VIABLE APP. Nothing else: no PSRAM, no display, no I2C, no vendor code.
  // 8 s of steady warm light = the app reached setup() and GPIO7 works.
  // Then 1 s on / 1 s off forever = unmistakable, and matches no other emitter in the project.
  pinMode(6, OUTPUT);
  pinMode(7, OUTPUT);
  digitalWrite(6, LOW);
  digitalWrite(7, HIGH);
  delay(8000);
  digitalWrite(7, LOW);
  for (;;) {
    digitalWrite(7, HIGH);
    delay(1000);
    digitalWrite(7, LOW);
    delay(1000);
  }
#endif
  // FIRST, before anything allocates from PSRAM (framebuffers, vendor epdiy buffers, assets).
#ifdef CP_PSRAM_DIAG
  mccrossy_psram_diag();
#endif
#ifdef CP_VENDOR_EARLY
  // Run the vendor EPDiy stack FIRST, before this application claims I2C0 / SD / WiFi.
  //
  // epd_board_init() calls i2c_driver_install(EPDIY_I2C_PORT, ...) for the PCA9555/TPS65185. If our
  // app already installed a driver on that port, IDF returns ESP_ERR_INVALID_STATE, s_ioexp_ok stays
  // false, and epd_board_poweron() returns early -- no rails, no image, and no visible symptom,
  // because the whole stack logs to a console this board does not have. Running it first removes
  // that conflict entirely.
  //
  // Holding forever keeps the reference image on the glass. Reboot to run the application normally.
  const int vrc = vendorEpdBootText();
  // Telemetry, because the panel may stay blank and there is no serial console:
  //   one continuous 3 s warm glow        -> rc == 0 (vendor pipeline reported success)
  //   (rc & 0xF) warm pulses, twice        -> failed; the count is the low nibble of the return code
  //   nothing at all                      -> the call never returned (hung/crashed inside)
  pinMode(6, OUTPUT);
  pinMode(7, OUTPUT);
  digitalWrite(6, LOW);
  delay(200);
  if (vrc == 0) {
    digitalWrite(7, HIGH);
    delay(3000);
    digitalWrite(7, LOW);
  } else {
    for (int pass = 0; pass < 2; ++pass) {
      for (int i = 0; i < (vrc & 0x0F); ++i) {
        digitalWrite(7, HIGH);
        delay(300);
        digitalWrite(7, LOW);
        delay(300);
      }
      delay(1500);
    }
  }
  digitalWrite(6, HIGH);
  for (;;) delay(1000);
#endif
  // Bring-up channel = the FRONTLIGHT. USB console and the haptic motor have both
  // proven unreliable on this board, while GPIO6/GPIO7 are plain, latched GPIOs
  // that have worked at every stage (including from static constructors).
  //
  // Protocol: COOL steady = alive. Then, after each milestone, N quick WARM blinks
  // separated by a 1 s pause:
  //   1 = ensureBooted() returned (I2C + TCA9555 answered)
  //   2 = holdPowerRails() returned
  //   3 = HalSystem::begin() returned
  //   4 = gpio.begin() returned
  //   5 = powerManager.begin() returned
  //   6 = WiFi event handlers registered (ARDUINO_EVENT_WIFI_STA/AP_START)
  //   7 = halTiltSensor.begin() returned
  //   8 = halClock.begin() returned
  //   9 = Storage.begin() returned true (SD card mounted)
  //  10 = HalSystem::checkPanic() returned
  //  11 = APP_STATE.loadFromFile() returned (settings loaded)
  //  12 = all stores loaded (settings, recent, koreader, OPDS, LCP) + theme + plugin subs
  //  13 = bleturner::begin() returned (BLE pager init)
  //  14 = Frontlight.begin() returned -- LAST frontlight-coded marker: after this
  //       the app owns the light, so a steady glow means "reached the switch"
  //  15 = setupDisplayAndFonts() returned (display driver begin() succeeded)
  // Build flags: -DCP_BLINK_FROM=n blinks only milestones >= n (skip proven steps);
  // -DCP_SKIP_FRONTLIGHT_BEGIN omits Frontlight.begin() for a single-variable test.
  // SUB-STEPS (fast pulses, see pulseK / dpulse):
  //   1 = reached the wake-reason switch, 2 = switch done (no deep sleep),
  //   3 = resume resolved,
  //   4 = PSRAM store+readback OK -- signalled as ONE LONG 2.5 s WARM GLOW, not a pulse,
  //       because a 0.25 s pulse among fast pulses is miscountable
  //   5 = display.begin() returned, 6 = renderer + activityManager begun, 7 = fonts loaded.
  // No long glow after pulse 3 = PSRAM store fault. Long glow then dark = death inside display.begin().
  // Codes 6-11 were added on 25 Oct after video evidence showed the real reader
  // reaches 5 and then resets, with no marker between 5 and SD init.
  // Whatever the highest count you see, the NEXT milestone is the one that killed
  // it. Silence after a steady COOL light and no blinks means it died before
  // ensureBooted() returned.
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  constexpr int kCool = 6;
  constexpr int kWarm = 7;
#ifndef CP_BLINK_FROM
// 99 suppresses every milestone. With 15 milestones instrumented the ladder costs
// 1+2+...+15 = 120 s of warm glow plus 15 x 3 s of cool separator = ~165 s of
// strobing on EVERY boot, which is indistinguishable from a hang. Set this to 0
// (or -DCP_BLINK_FROM=0) for a deliberate bring-up session.
#define CP_BLINK_FROM 99
#endif
  auto blinkN = [](int n) {
    // CP_BLINK_FROM (build flag) suppresses every milestone below N. Once a step is
    // proven on hardware it never needs re-proving, and the ladder costs 1+2+...+N
    // seconds of wall time per boot, so long ladders blink only their tail.
    if (n < CP_BLINK_FROM) return;
    // DURATION encoding: after milestone N, WARM glows for exactly N seconds.
    //   milestone 1 -> 1 s warm     milestone 4 -> 4 s warm
    //   milestone 2 -> 2 s          milestone 5 -> 5 s warm
    //   milestone 3 -> 3 s
    // Then COOL returns and holds for 3 s as a separator. Counting rapid blinks
    // proved unreadable even at 400 ms, but "how many seconds was the last warm
    // glow" is trivial to eyeball or measure on video.
    digitalWrite(kCool, LOW);
    delay(300);
    digitalWrite(kWarm, HIGH);
    delay(1000 * n);
    digitalWrite(kWarm, LOW);
    digitalWrite(kCool, HIGH);
    delay(3000);
  };
  // FAST SUB-STEP ENCODING: k short warm pulses (~0.6 s each). Used for the dense
  // gates between milestones, where a 13 s glow per step would make a bisection
  // take minutes. Still measurable from video (extract frames, classify by colour:
  // warm = R >> B), so it trades human eyeballing for machine reading.
  auto pulseK = [](int k) {
    if (k < CP_BLINK_FROM) return;
    digitalWrite(kCool, LOW);
    for (int i = 0; i < k; ++i) {
      digitalWrite(kWarm, HIGH);
      delay(250);
      digitalWrite(kWarm, LOW);
      delay(350);
    }
    digitalWrite(kCool, HIGH);
    delay(1500);
  };
  // Call this immediately BEFORE a risky call: COOL off means "we are inside a
  // call right now". If the light stays off with no further blinks, that call is
  // where it hung or died.
  auto busy = []() {
#if CP_BLINK_FROM <= 0
    digitalWrite(kCool, LOW);
#endif
  };
#if CP_BLINK_FROM <= 0
  pinMode(kCool, OUTPUT);
  pinMode(kWarm, OUTPUT);
  digitalWrite(kCool, HIGH);   // COOL steady: setup() entered
  digitalWrite(kWarm, LOW);
#endif
#endif

  // BRING-UP BISECT: with -DCP_BRINGUP_MINIMAL the app does nothing except
  // prove it started. All globals above still construct and the full image is
  // still linked, so if this heartbeats, static init + the image are healthy
  // and the fault is inside the real setup() body. If it stays dark, the fault
  // is before main/setup entirely.
#ifdef CP_BRINGUP_MINIMAL
  // ANCHOR TEST: prove the app can live WITHOUT touching I2C at all.
  //
  // GPIO0 is both I2C SDA and the ROM download strap on this board. Every previous
  // build initialised I2C here (Wire.begin(0, 1)), and every one of them ended in
  // the ROM loader (boot:0x4a) after a short burst of activity. This build never
  // calls Wire/E0, never calls holdPowerRails(), never calls HalSystem::begin().
  //
  // If this stays alive (COOL steady + a 1 s WARM pulse every 3 s, forever) and
  // USB enumerates, then I2C-on-GPIO0 is what drags the chip back into download
  // mode, and the fix is a bus-recovery-aware I2C init.
  //
  // The keep[] array is load-bearing: without it LTO can drop loop() and every
  // global it references, so the constructors never land in .init_array and the
  // test proves nothing.
  volatile const void* keep[] = {reinterpret_cast<const void*>(&renderer),
                                 reinterpret_cast<const void*>(&activityManager),
                                 reinterpret_cast<const void*>(&fontCacheManager),
                                 reinterpret_cast<const void*>(&mappedInputManager),
                                 reinterpret_cast<const void*>(&notosans14FontFamily)};
  (void)keep;

#if defined(CP_I2C_PHASE)
  // Staged I2C bring-up: strictly additive, so the FIRST phase that powers the
  // board off identifies the culprit.
  //   phase 1: i2cBegin + read-only register access (no writes)
  //   phase 2: also write output/direction registers with vendor-safe values
  //   phase 3: also call holdPowerRails()
  {
    using namespace freeink::metalio4plus;
#if CP_I2C_PHASE >= 1
    const bool began = i2cBegin();
    uint16_t out0 = 0xFFFF, cfg0 = 0xFFFF, in0 = 0xFFFF;
    const bool r1 = tcaRead16(TCA_REG_OUTPUT0, out0);
    const bool r2 = tcaRead16(TCA_REG_CONFIG0, cfg0);
    const bool r3 = tcaRead16(TCA_REG_INPUT0, in0);
    printf("[CP-P1] i2c=%d rd_out=%d(%04x) rd_cfg=%d(%04x) rd_in=%d(%04x)\n",
           (int)began, (int)r1, out0, (int)r2, cfg0, (int)r3, in0);
    fflush(stdout);
#endif
#if CP_I2C_PHASE >= 2
    // Vendor defaults: screen/camera power ON, touch reset released, BT PA on;
    // TPS boost, motor and amp OFF.
    constexpr uint16_t kSafeOutputs = (1u << P0_CAM_SCR_EN) | (1u << P1_TOUCH_RESET) |
                                      (1u << P1_BT_PA_PWR);
    constexpr uint16_t kOutputMask = (1u << P0_VCOM_CTRL) | (1u << P0_PWRUP) |
                                     (1u << P0_WAKEUP) | (1u << P0_MOTOR) |
                                     (1u << P0_PA_EN) | (1u << P0_CAM_SCR_EN) |
                                     (1u << P1_TOUCH_RESET) | (1u << P1_POWER_PULSE) |
                                     (1u << P1_BT_PA_PWR);
    const bool w1 = tcaWrite16(TCA_REG_OUTPUT0, kSafeOutputs);
    const bool w2 = tcaWrite16(TCA_REG_CONFIG0, kOutputMask);
    printf("[CP-P2] wrote out=%d cfg=%d\n", (int)w1, (int)w2);
    fflush(stdout);
#endif
#if CP_I2C_PHASE >= 3
    BoardConfig::holdPowerRails();
    printf("[CP-P3] holdPowerRails returned\n");
    fflush(stdout);
#endif
  }
#endif

#if defined(CP_I2C_LADDER)
  // SIX-STEP LADDER. All phases in ONE binary: because a failure powers the board
  // OFF (terminal, no reset), the last duration-glowed step is the one that killed
  // it, and every earlier step is proven safe in the same run.
  //
  //   glow 1 s -> i2cBegin + read OUTPUT0/CONFIG0/INPUT0
  //   glow 2 s -> write OUTPUT0 with vendor-safe values
  //   glow 3 s -> write CONFIG0 (directions)
  //   glow 4 s -> write POLARITY0
  //   glow 5 s -> holdPowerRails()
  //   glow 6 s -> candidate FIX: vendor power-on order (wakeup -> 50 ms -> pwrup+vcom)
  //
  // After each step COOL holds 1.5 s, so "glow N s, then COOL, then glow N+1 s" means
  // step N passed. Death inside a step leaves you with the glow and no COOL after it.
  {
    using namespace freeink::metalio4plus;
    uint16_t out0 = 0xFFFF, cfg0 = 0xFFFF, pol0 = 0xFFFF, in0 = 0xFFFF;
    auto glow = [](int secs) {
      digitalWrite(kCool, LOW);
      delay(300);
      digitalWrite(kWarm, HIGH);
      delay(1000 * secs);
      digitalWrite(kWarm, LOW);
    };
    auto stepDone = [](int n, const char* name) {
      digitalWrite(kCool, HIGH);
      delay(1500);
      printf("[CP-LADDER] step %d (%s) SURVIVED\n", n, name);
      fflush(stdout);
    };

    glow(1);
    const bool i2cOk = i2cBegin();
    const bool rOut = tcaRead16(TCA_REG_OUTPUT0, out0);
    const bool rCfg = tcaRead16(TCA_REG_CONFIG0, cfg0);
    const bool rPol = tcaRead16(TCA_REG_POLARITY0, pol0);
    const bool rIn = tcaRead16(TCA_REG_INPUT0, in0);
    printf("[CP-LADDER] i2c=%d out=%d/%04x cfg=%d/%04x pol=%d/%04x in=%d/%04x\n",
           (int)i2cOk, (int)rOut, out0, (int)rCfg, cfg0, (int)rPol, pol0, (int)rIn, in0);
    printf("[CP-LADDER] PWR_GOOD=%d\n", (int)((in0 >> P0_PWR_GOOD) & 1u));
    fflush(stdout);
    stepDone(1, "reads");

    constexpr uint16_t kSafeOut = (1u << P0_CAM_SCR_EN) | (1u << P1_TOUCH_RESET) |
                                  (1u << P1_BT_PA_PWR);
    constexpr uint16_t kOutMask = (1u << P0_VCOM_CTRL) | (1u << P0_PWRUP) |
                                  (1u << P0_WAKEUP) | (1u << P0_MOTOR) |
                                  (1u << P0_PA_EN) | (1u << P0_CAM_SCR_EN) |
                                  (1u << P1_TOUCH_RESET) | (1u << P1_POWER_PULSE) |
                                  (1u << P1_BT_PA_PWR);
    constexpr uint16_t kInMask = (1u << P0_PWR_GOOD) | (1u << P1_RTC_INT) |
                                 (1u << P1_POWER) | (1u << P1_VOLUME_UP) |
                                 (1u << P1_VOLUME_DOWN);

    glow(2);
    const bool wOut = tcaWrite16(TCA_REG_OUTPUT0, kSafeOut);
    printf("[CP-LADDER] write OUT=%d\n", (int)wOut); fflush(stdout);
    stepDone(2, "OUTPUT0");

    glow(3);
    const bool wCfg = tcaWrite16(TCA_REG_CONFIG0, uint16_t((cfg0 & ~kOutMask) | kInMask));
    printf("[CP-LADDER] write CFG=%d\n", (int)wCfg); fflush(stdout);
    stepDone(3, "CONFIG0");

    glow(4);
    const bool wPol = tcaWrite16(TCA_REG_POLARITY0, uint16_t(pol0 & ~kInMask));
    printf("[CP-LADDER] write POL=%d\n", (int)wPol); fflush(stdout);
    stepDone(4, "POLARITY0");

    glow(5);
    BoardConfig::holdPowerRails();
    printf("[CP-LADDER] holdPowerRails returned\n"); fflush(stdout);
    stepDone(5, "holdPowerRails");

    glow(6);
    // Candidate FIX: the vendor's own power-on order.
    // wakeup HIGH first (TPS leaves sleep, I2C stays alive), 50 ms, then the boost.
    setOutput(P0_WAKEUP, true);
    delay(50);
    setOutput(P0_PWRUP, true);
    setOutput(P0_VCOM_CTRL, true);
    delay(50);
    uint16_t in1 = 0xFFFF;
    const bool rin = tcaRead16(TCA_REG_INPUT0, in1);
    printf("[CP-LADDER] after vendor power-on: in=%d/%04x PWR_GOOD=%d\n",
           (int)rin, in1, (int)((in1 >> P0_PWR_GOOD) & 1u));
    fflush(stdout);
    stepDone(6, "vendor-power-on");
  }
#endif

#if defined(CP_STEP_MASK)
  // MASK-DRIVEN ACTION EXECUTOR. One code path, one constant per image: CP_STEP_MASK
  // selects which of 16 individual actions to perform, in ascending bit order.
  //
  // This exists because the expander rail pins are now PROVEN innocent (all six low
  // together still reached the heartbeat), so the remaining suspects are the plain-GPIO
  // writes inside ensureBooted(): frontlight, ANA_SW, the EPD pins and the INT pins.
  //
  // Signalling: before each executed action a warm glow of min(ordinal,6) seconds, then
  // COOL for 1 s. Death at glow N means the Nth action in this mask is the killer.
  // Reaching the heartbeat means the whole mask is safe.
  {
    using namespace freeink::metalio4plus;
    uint16_t cfg = 0xFFFF, out = 0xFFFF, pol = 0xFFFF;
    bool i2cUp = false;
    int ordinal = 0;

    auto glow = [](int secs) {
      digitalWrite(kCool, LOW);
      delay(200);
      digitalWrite(kWarm, HIGH);
      delay(1000 * secs);
      digitalWrite(kWarm, LOW);
    };
    auto tick = [&](int bit, const char* name) {
      glow(ordinal < 6 ? ordinal + 1 : 6);
      ordinal++;
      printf("[CP-MASK] action bit %d (%s)\n", bit, name);
      fflush(stdout);
    };
    auto readyI2C = [&]() {
      if (!i2cUp) {
        i2cUp = i2cBegin();
        tcaRead16(TCA_REG_OUTPUT0, out);
        tcaRead16(TCA_REG_CONFIG0, cfg);
        tcaRead16(TCA_REG_POLARITY0, pol);
      }
      return i2cUp;
    };

    constexpr uint32_t kMask = CP_STEP_MASK;
#ifdef CP_STEP_SETTLE_MS
    constexpr int kSettle = CP_STEP_SETTLE_MS;  // pause AFTER each action
#else
    constexpr int kSettle = 0;
#endif
    // Drive expander pin(s) LOW without clobbering the other outputs, in the only safe order:
    // read the latch, clear the bits, write OUTPUT first, THEN clear the CONFIG direction bits.
    // (CONFIG bit 1 = input, so clearing it is what enables the driver -- writing CONFIG first
    // would let the power-on default latch of 0xFFFF source every one of those pins HIGH for the
    // duration of the transaction. This is audit section F's invariant, used rather than described.)
    auto expander_low = [&](uint16_t pins) -> bool {
      if (!readyI2C()) return false;
      uint16_t latch = 0xFFFF, cfg = 0xFFFF;
      if (!tcaRead16(TCA_REG_OUTPUT0, latch) || !tcaRead16(TCA_REG_CONFIG0, cfg)) return false;
      if (!tcaWrite16(TCA_REG_OUTPUT0, uint16_t(latch & ~pins))) return false;
      return tcaWrite16(TCA_REG_CONFIG0, uint16_t(cfg & ~pins));
    };

    for (int bit = 0; bit < 29; ++bit) {
      if (!(kMask & (1u << bit))) continue;
      switch (bit) {
        case 0:
          tick(bit, "expander OUTPUT0 = safe3");
          if (readyI2C()) tcaWrite16(TCA_REG_OUTPUT0, uint16_t((1u << P0_CAM_SCR_EN) | (1u << P1_TOUCH_RESET) | (1u << P1_BT_PA_PWR)));
          digitalWrite(kCool, HIGH); delay(1000);
          break;
        case 1:
          tick(bit, "expander CONFIG0 = 9 outputs");
          if (readyI2C()) {
            const uint16_t m2 = uint16_t((1u << P0_VCOM_CTRL) | (1u << P0_PWRUP) | (1u << P0_WAKEUP) |
                                         (1u << P0_MOTOR) | (1u << P0_PA_EN) | (1u << P0_CAM_SCR_EN) |
                                         (1u << P1_TOUCH_RESET) | (1u << P1_POWER_PULSE) | (1u << P1_BT_PA_PWR));
            tcaWrite16(TCA_REG_CONFIG0, uint16_t(cfg & ~m2));
          }
          digitalWrite(kCool, HIGH); delay(1000);
          break;
        case 2:
          tick(bit, "expander POLARITY0 clear inputs");
          if (readyI2C()) {
            const uint16_t inmask = uint16_t((1u << P0_PWR_GOOD) | (1u << P1_RTC_INT) |
                                             (1u << P1_POWER) | (1u << P1_VOLUME_UP) | (1u << P1_VOLUME_DOWN));
            tcaWrite16(TCA_REG_POLARITY0, uint16_t(pol & ~inmask));
          }
          digitalWrite(kCool, HIGH); delay(1000);
          break;
        case 3: tick(bit, "GPIO2 INPUT_PULLUP (expander INT)"); pinMode(IO_EXPANDER_INT_GPIO, INPUT_PULLUP); digitalWrite(kCool, HIGH); delay(1000); break;
        case 4: tick(bit, "GPIO5 INPUT (touch INT)");          pinMode(TOUCH_INT_PIN, INPUT);         digitalWrite(kCool, HIGH); delay(1000); break;
        case 5: tick(bit, "GPIO61 INPUT_PULLUP (boot key)");   pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP); digitalWrite(kCool, HIGH); delay(1000); break;
        case 6: tick(bit, "GPIO6 frontlight COOL out LOW");    pinMode(FRONTLIGHT_COOL_PIN, OUTPUT); digitalWrite(FRONTLIGHT_COOL_PIN, LOW); digitalWrite(kCool, HIGH); delay(1000); break;
        case 7: tick(bit, "GPIO7 frontlight WARM out LOW");    pinMode(FRONTLIGHT_WARM_PIN, OUTPUT); digitalWrite(FRONTLIGHT_WARM_PIN, LOW); digitalWrite(kCool, HIGH); delay(1000); break;
        case 8: tick(bit, "GPIO36 ANA_SW out LOW");            pinMode(ANA_SW_GPIO, OUTPUT);          digitalWrite(ANA_SW_GPIO, LOW);         digitalWrite(kCool, HIGH); delay(1000); break;
        case 9: tick(bit, "GPIO18 XOE out LOW");               pinMode(EPD_PIN_XOE, OUTPUT);          digitalWrite(EPD_PIN_XOE, LOW);         digitalWrite(kCool, HIGH); delay(1000); break;
        case 10: tick(bit, "GPIO19 MODE out LOW");             pinMode(EPD_PIN_MODE, OUTPUT);         digitalWrite(EPD_PIN_MODE, LOW);        digitalWrite(kCool, HIGH); delay(1000); break;
        case 11: tick(bit, "GPIO37 BORDER out LOW");           pinMode(EPD_PIN_BORDER, OUTPUT);       digitalWrite(EPD_PIN_BORDER, LOW);      digitalWrite(kCool, HIGH); delay(1000); break;
        case 12: tick(bit, "holdPowerRails() latch loop");     BoardConfig::holdPowerRails();         digitalWrite(kCool, HIGH); delay(1000); break;
        case 13: tick(bit, "TPS power-on (wakeup->pwrup->vcom)");
          if (readyI2C()) { setOutput(P0_WAKEUP, true); delay(50); setOutput(P0_PWRUP, true); setOutput(P0_VCOM_CTRL, true); delay(50); }
          digitalWrite(kCool, HIGH); delay(1000);
          break;
        case 14: tick(bit, "ensureBooted() FULL (original)");  freeink::metalio4plus::ensureBooted(); digitalWrite(kCool, HIGH); delay(1000); break;
        case 16: tick(bit, "GPIO16 XCL low");   pinMode(EPD_PIN_XCL, OUTPUT); digitalWrite(EPD_PIN_XCL, LOW); digitalWrite(kCool, HIGH); delay(1000); break;
        case 17: tick(bit, "GPIO17 XLE low");   pinMode(EPD_PIN_XLE, OUTPUT); digitalWrite(EPD_PIN_XLE, LOW); digitalWrite(kCool, HIGH); delay(1000); break;
        case 18: tick(bit, "GPIO35 SPV low");   pinMode(EPD_PIN_SPV, OUTPUT); digitalWrite(EPD_PIN_SPV, LOW); digitalWrite(kCool, HIGH); delay(1000); break;
        case 19: tick(bit, "GPIO40 CKV low");   pinMode(EPD_PIN_CKV, OUTPUT); digitalWrite(EPD_PIN_CKV, LOW); digitalWrite(kCool, HIGH); delay(1000); break;
        case 20: tick(bit, "GPIO4 XSTL low");   pinMode(EPD_PIN_XSTL, OUTPUT); digitalWrite(EPD_PIN_XSTL, LOW); digitalWrite(kCool, HIGH); delay(1000); break;
        case 21: tick(bit, "EPD clock pins 16/17/35/40/4 all LOW");
          pinMode(EPD_PIN_XCL, OUTPUT); pinMode(EPD_PIN_XLE, OUTPUT); pinMode(EPD_PIN_SPV, OUTPUT);
          pinMode(EPD_PIN_CKV, OUTPUT); pinMode(EPD_PIN_XSTL, OUTPUT);
          digitalWrite(EPD_PIN_XCL, LOW); digitalWrite(EPD_PIN_XLE, LOW); digitalWrite(EPD_PIN_SPV, LOW);
          digitalWrite(EPD_PIN_CKV, LOW); digitalWrite(EPD_PIN_XSTL, LOW);
          digitalWrite(kCool, HIGH); delay(1000);
          break;
        case 22: tick(bit, "chargeEnable pin write");
          if (BoardConfig::ACTIVE.power.chargeEnable >= 0) {
            const auto g = static_cast<gpio_num_t>(BoardConfig::ACTIVE.power.chargeEnable);
            gpio_hold_dis(g);
            pinMode(BoardConfig::ACTIVE.power.chargeEnable, OUTPUT);
            digitalWrite(BoardConfig::ACTIVE.power.chargeEnable,
                         BoardConfig::ACTIVE.power.chargeEnableActiveHigh ? HIGH : LOW);
          }
          digitalWrite(kCool, HIGH); delay(1000);
          break;
        case 23: tick(bit, "EVERY plain GPIO low (3..11 + 16,17,35,4)");
          pinMode(IO_EXPANDER_INT_GPIO, INPUT_PULLUP); pinMode(TOUCH_INT_PIN, INPUT);
          pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);
          pinMode(FRONTLIGHT_COOL_PIN, OUTPUT); pinMode(FRONTLIGHT_WARM_PIN, OUTPUT);
          pinMode(ANA_SW_GPIO, OUTPUT); pinMode(EPD_PIN_XOE, OUTPUT); pinMode(EPD_PIN_MODE, OUTPUT);
          pinMode(EPD_PIN_BORDER, OUTPUT); pinMode(EPD_PIN_XCL, OUTPUT); pinMode(EPD_PIN_XLE, OUTPUT);
          pinMode(EPD_PIN_SPV, OUTPUT); pinMode(EPD_PIN_XSTL, OUTPUT);
          digitalWrite(FRONTLIGHT_COOL_PIN, LOW); digitalWrite(FRONTLIGHT_WARM_PIN, LOW);
          digitalWrite(ANA_SW_GPIO, LOW); digitalWrite(EPD_PIN_XOE, LOW); digitalWrite(EPD_PIN_MODE, LOW);
          digitalWrite(EPD_PIN_BORDER, LOW); digitalWrite(EPD_PIN_XCL, LOW); digitalWrite(EPD_PIN_XLE, LOW);
          digitalWrite(EPD_PIN_SPV, LOW); digitalWrite(EPD_PIN_XSTL, LOW);
          digitalWrite(kCool, HIGH); delay(1000);
          break;
        // ---- L group: the three rails the VENDOR holds HIGH, driven LOW --------------
        // Never tested before. `ensureBooted()` clears all nine latch bits, but the earlier
        // ladders covered only the other six (six-low, survived) and these three driven HIGH
        // (survived). The state we actually drive has never been run on the board.
        case 24: tick(bit, "P0.5 CAM_SCR_EN (screen rail) LOW");
          expander_low(uint16_t(1u << P0_CAM_SCR_EN));
          digitalWrite(kCool, HIGH); delay(1000); break;
        case 25: tick(bit, "P1.2 TOUCH_RESET LOW (low = touch unpowered)");
          expander_low(uint16_t(1u << P1_TOUCH_RESET));
          digitalWrite(kCool, HIGH); delay(1000); break;
        case 26: tick(bit, "P1.4 BT_PA_PWR LOW");
          expander_low(uint16_t(1u << P1_BT_PA_PWR));
          digitalWrite(kCool, HIGH); delay(1000); break;
        case 27: tick(bit, "CAM_SCR + TP_RST + BT_PA all LOW (the untested state)");
          expander_low(uint16_t((1u << P0_CAM_SCR_EN) | (1u << P1_TOUCH_RESET) |
                                (1u << P1_BT_PA_PWR)));
          digitalWrite(kCool, HIGH); delay(1000); break;
        case 28: tick(bit, "all NINE expander outputs LOW (ensureBooted latch state)");
          expander_low(uint16_t((1u << P0_VCOM_CTRL) | (1u << P0_PWRUP) | (1u << P0_WAKEUP) |
                                (1u << P0_MOTOR) | (1u << P0_PA_EN) | (1u << P0_CAM_SCR_EN) |
                                (1u << P1_TOUCH_RESET) | (1u << P1_POWER_PULSE) |
                                (1u << P1_BT_PA_PWR)));
          digitalWrite(kCool, HIGH); delay(1000); break;
        case 15: tick(bit, "EPD XOE+MODE+BORDER all LOW");
          pinMode(EPD_PIN_XOE, OUTPUT); pinMode(EPD_PIN_MODE, OUTPUT); pinMode(EPD_PIN_BORDER, OUTPUT);
          digitalWrite(EPD_PIN_XOE, LOW); digitalWrite(EPD_PIN_MODE, LOW); digitalWrite(EPD_PIN_BORDER, LOW);
          digitalWrite(kCool, HIGH); delay(1000);
          break;
      }
    }
    printf("[CP-MASK] mask 0x%04x completed all %d actions\n", (unsigned)kMask, ordinal);
    fflush(stdout);
  }
#endif
#if defined(CP_LOW_LADDER)
  // INVERSE LADDER. The three "safe3" bits are proven innocent, so the killer is one
  // of the six pins we drive LOW as an output. Driving a pin LOW can override an
  // external pull-up that is holding a power-IC enable asserted.
  //
  // Each step makes exactly ONE pin an output and drives it LOW, announces itself with
  // a warm glow of (step) seconds, then restores COOL for 1.5 s. The last glow seen
  // names the pin that must never be driven low.
  //
  //   glow 1 s -> P1.3 POWER_PULSE  low   (most suspicious: it is the power-IC control)
  //   glow 2 s -> P0.2 WAKEUP       low   (TPS sleep)
  //   glow 3 s -> P0.1 PWRUP        low   (boost enable)
  //   glow 4 s -> P0.0 VCOM_CTRL    low
  //   glow 5 s -> P0.4 PA_EN        low   (audio amp)
  //   glow 6 s -> P0.3 MOTOR        low
  {
    using namespace freeink::metalio4plus;
    i2cBegin();
    auto glide = [](int secs) {
      digitalWrite(kCool, LOW);
      delay(300);
      digitalWrite(kWarm, HIGH);
      delay(1000 * secs);
      digitalWrite(kWarm, LOW);
    };
    auto lowStep = [](int bit, const char* name) {
      uint16_t cfg = 0xFFFF, out = 0xFFFF;
      tcaRead16(TCA_REG_CONFIG0, cfg);
      tcaRead16(TCA_REG_OUTPUT0, out);
      tcaWrite16(TCA_REG_OUTPUT0, uint16_t(out & ~(1u << bit)));  // value LOW first
      tcaWrite16(TCA_REG_CONFIG0, uint16_t(cfg & ~(1u << bit)));  // then become output
      printf("[CP-LOW] drove bit %d (%s) LOW and survived\n", bit, name);
      fflush(stdout);
      digitalWrite(kCool, HIGH);
      delay(1500);
    };
    glide(1); lowStep(P1_POWER_PULSE, "POWER_PULSE");
    glide(2); lowStep(P0_WAKEUP,      "WAKEUP");
    glide(3); lowStep(P0_PWRUP,       "PWRUP");
    glide(4); lowStep(P0_VCOM_CTRL,   "VCOM_CTRL");
    glide(5); lowStep(P0_PA_EN,       "PA_EN");
    glide(6); lowStep(P0_MOTOR,       "MOTOR");
  }
#endif
#if defined(CP_BIT_TEST)
  // Single-bit isolation: drive EXACTLY ONE TCA9555 output bit high and nothing else.
  // Answers "does this one rail bit alone power the board off?". On a fresh power-on the
  // expander's output register resets to 0xFF and its config to 0xFF (all inputs), so
  // writing OUTPUT0 first has no effect until that bit is made an output.
  {
    using namespace freeink::metalio4plus;
    constexpr int kBit = CP_BIT_TEST;
    const bool ok = i2cBegin();
    uint16_t cfg = 0xFFFF;
    tcaRead16(TCA_REG_CONFIG0, cfg);
    const bool w1 = tcaWrite16(TCA_REG_OUTPUT0, uint16_t(1u << kBit));
    const bool w2 = tcaWrite16(TCA_REG_CONFIG0, uint16_t(cfg & ~(1u << kBit)));
    printf("[CP-BIT] bit=%d i2c=%d out=%d cfg=%d -> bit is HIGH now\n",
           kBit, (int)ok, (int)w1, (int)w2);
    fflush(stdout);
  }
#endif

#if defined(CP_CFG_ONLY)
  // Flip directions to outputs WITHOUT writing OUTPUT0 first. The output register
  // still holds its power-on 0xFF, so every rail bit goes HIGH the instant the pin
  // becomes an output. Tests "is it the direction change, or the values?".
  {
    using namespace freeink::metalio4plus;
    constexpr uint16_t kOutMask = (1u << P0_VCOM_CTRL) | (1u << P0_PWRUP) |
                                  (1u << P0_WAKEUP) | (1u << P0_MOTOR) |
                                  (1u << P0_PA_EN) | (1u << P0_CAM_SCR_EN) |
                                  (1u << P1_TOUCH_RESET) | (1u << P1_POWER_PULSE) |
                                  (1u << P1_BT_PA_PWR);
    const bool ok = i2cBegin();
    uint16_t cfg = 0xFFFF;
    tcaRead16(TCA_REG_CONFIG0, cfg);
    const bool w = tcaWrite16(TCA_REG_CONFIG0, uint16_t(cfg & ~kOutMask));
    printf("[CP-CFGONLY] i2c=%d cfgwrite=%d -> all rails now driven by stale 0xFF\n",
           (int)ok, (int)w);
    fflush(stdout);
  }
#endif

#if defined(CP_CFG_BEFORE_OUT)
  // Reversed order: CONFIG0 first (rails go high from the stale latch), then OUTPUT0
  // with safe values. If the safe-order ladder survives but this dies, the glitch from
  // enabling outputs before loading values is what kills it.
  {
    using namespace freeink::metalio4plus;
    constexpr uint16_t kSafe = (1u << P0_CAM_SCR_EN) | (1u << P1_TOUCH_RESET) |
                               (1u << P1_BT_PA_PWR);
    constexpr uint16_t kOutMask = (1u << P0_VCOM_CTRL) | (1u << P0_PWRUP) |
                                  (1u << P0_WAKEUP) | (1u << P0_MOTOR) |
                                  (1u << P0_PA_EN) | (1u << P0_CAM_SCR_EN) |
                                  (1u << P1_TOUCH_RESET) | (1u << P1_POWER_PULSE) |
                                  (1u << P1_BT_PA_PWR);
    const bool ok = i2cBegin();
    uint16_t cfg = 0xFFFF;
    tcaRead16(TCA_REG_CONFIG0, cfg);
    const bool w1 = tcaWrite16(TCA_REG_CONFIG0, uint16_t(cfg & ~kOutMask));
    delay(20);
    const bool w2 = tcaWrite16(TCA_REG_OUTPUT0, kSafe);
    printf("[CP-CFGFIRST] i2c=%d cfg=%d out=%d (reversed order)\n",
           (int)ok, (int)w1, (int)w2);
    fflush(stdout);
  }
#endif

#if defined(CP_SAFE3_ONLY)
  // Vendor-safe OUTPUT0 + CONFIG0 only. No polarity write, no rails, no TPS.
  {
    using namespace freeink::metalio4plus;
    constexpr uint16_t kSafe = (1u << P0_CAM_SCR_EN) | (1u << P1_TOUCH_RESET) |
                               (1u << P1_BT_PA_PWR);
    constexpr uint16_t kOutMask = (1u << P0_VCOM_CTRL) | (1u << P0_PWRUP) |
                                  (1u << P0_WAKEUP) | (1u << P0_MOTOR) |
                                  (1u << P0_PA_EN) | (1u << P0_CAM_SCR_EN) |
                                  (1u << P1_TOUCH_RESET) | (1u << P1_POWER_PULSE) |
                                  (1u << P1_BT_PA_PWR);
    const bool ok = i2cBegin();
    uint16_t cfg = 0xFFFF;
    tcaRead16(TCA_REG_CONFIG0, cfg);
    const bool w1 = tcaWrite16(TCA_REG_OUTPUT0, kSafe);
    const bool w2 = tcaWrite16(TCA_REG_CONFIG0, uint16_t(cfg & ~kOutMask));
    printf("[CP-SAFE3] i2c=%d out=%d cfg=%d\n", (int)ok, (int)w1, (int)w2);
    fflush(stdout);
  }
#endif
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  uint32_t beat = 0;
  for (;;) {
    digitalWrite(kCool, HIGH);
    digitalWrite(kWarm, HIGH);
    delay(1000);
    digitalWrite(kWarm, LOW);
    delay(2000);
    printf("[CP-ANCHOR] alive %lu\n", static_cast<unsigned long>(++beat));
    fflush(stdout);
  }
#else
  for (;;) delay(1000);
#endif
#endif

#ifdef CP_CONSOLE_ONLY
  // DIAGNOSTIC MODE: do not touch Arduino's HWCDC at all. The factory bootloader
  // already owns the USB-Serial/JTAG peripheral, so Serial.begin() hangs -- the
  // board shows "frontlight on, no port, no buzz". Log with printf() to IDF's
  // console (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) and use buzzes as the channel
  // that works even with no USB.
  printf("\n[CP] setup() entered\n");
  fflush(stdout);

#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  // i2c-ng ORDERING, and it matters: esp32-hal-i2c-ng (what Arduino Wire uses) can
  // only CREATE a bus, never adopt one, while the vendor stack's mn_i2c_* helpers
  // can adopt an existing one. So Arduino must go first. When it did not, Wire's
  // transfers returned ESP_ERR_INVALID_STATE (259) forever -- the app then spun in
  // a touch/gauge retry loop before display init and never reached the UI.
  {
    // Vendor order: recover the bus BEFORE installing the controller. Doing this
    // after Wire.begin() detaches GPIO0/1 from the peripheral and every transfer
    // fails (the TCA9555 probe failure).
    mn_bus_recover();
    const bool wire_ok = Wire.begin(0, 1, 100000);
    // BQ27220 stretches SCL (the vendor used I2C_LL_MAX_TIMEOUT for exactly
    // this reason); Arduino Wire's default is too tight for multi-byte reads.
    Wire.setTimeOut(50);
    printf("[CP] Wire.begin(0,1,100k) -> %s\n", wire_ok ? "OK (Arduino owns the bus)" : "FAILED (someone else owns it)");
    fflush(stdout);
  }
  // Dump the previous boot's post-GPIO19 log (it lives in RTC memory, so it
  // survived the reset that we could not observe over USB).
  {
    mn_note_dump();
  }
  // NOTE: the panel is brought up AFTER holdPowerRails(), not here. On this board
  // metalio4plus::ensureBooted() (called by holdPowerRails) drives PWRUP/WAKEUP/
  // VCOM low as its "safe" state; initialising the panel before that would have
  // the app power the panel back down mid-boot and leave a blank screen.
#ifdef CP_NO_DISPLAY
  // BISECTION MODE: stop before display init. Driving GPIO19 (EPD_PIN_MODE, shared
  // with USB D-) is what kills the console for ~30 s per cycle, so staying here is
  // the only way to keep a readable log while the I2C/expander question is open.
  for (;;) {
    printf("[CP] alive (CP_NO_DISPLAY), wire alive, not touching GPIO19\n");
    fflush(stdout);
    delay(2000);
  }
#endif
#endif
#endif

#if defined(ENABLE_SERIAL_LOG) && !defined(CP_CONSOLE_ONLY)
  Serial.begin(115200);
  delay(300);  // let the host enumerate before the first line is printed
  Serial.println("\n[CP] setup() entered");
  Serial.flush();
#endif

#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  // Milestone 1: prove I2C + the TCA9555 answer before anything else touches them.
  busy();
  freeink::metalio4plus::ensureBooted();
  blinkN(1);
#endif

#ifndef CP_SKIP_RAILS
#ifdef CP_STOP_AFTER_RAILS
  blinkN(9);  // reached rails stage; stop here
  for (;;) { delay(1000); }
#endif
#ifndef CP_SKIP_RAILS
  busy();
  BoardConfig::holdPowerRails();
#endif
#else
  // CP_SKIP_RAILS: variant v4_real_norails -- run the real setup() without ANY
  // expander/rail write, to find out how far CrossPoint gets before needing them.
#endif
#ifdef ENABLE_SERIAL_LOG
  Serial.println("[CP] rails held (TCA9555 reached)");
  Serial.flush();
#endif
  // PRODUCTION BOOT ORDER: rails are latched (holdPowerRails/ensureBooted have set
  // the expander's safe levels), so bring the panel up now -- ~1-2 s after power-on --
  // and paint the splash. metalio_vendor_panel_begin() is idempotent (g_begun), so the
  // later HalDisplay::begin() reuses this init instead of repeating it.
  //
  // THE SILENT-REBOOT GLASS CONTRACT. On a heap-defrag ESP.restart() the glass is
  // holding a frame THIS firmware drove: silentRestartTo() paints the "Loading…" popup
  // 50 ms before the reset and e-paper keeps it. The driver's boot clear would whiten
  // that away and leave ~810 ms of nothing before the arriving screen's own drive --
  // the blank/white frame the operator reports after a KOReader sync return and after
  // an NTP clock sync (both exit through this reboot). Arming the contract here is
  // only sound because the routing block below arms forceCleanNextRefresh() for the
  // whole Silent branch: the first paint of this boot is then a whole-panel forced
  // drive (epd_hl_update_screen_full, which clears MODE_SKIP_UNCHANGED and conditions
  // every pixel), so no pixel can inherit the previous session's ink and the clear is
  // redundant. silentRebootMagic is still armed this early; it is consumed below.
  if (silentRebootMagic == SILENT_REBOOT_MAGIC) {
    LOG_INF("MAIN", "silent reboot: glass keeps the exit frame (boot clear skipped)");
    metalio_vendor_panel_keep_glass_on_boot();
  }
  metalio_vendor_panel_begin();
  // Keep the panel's line-prep threads below the Wi-Fi task (23) on core 0: at their creation
  // priority (24) they outrank the supplicant, so a waveform running while a join is in flight
  // starves the SAE exchange. 22 stays above every app task.
  metalio_vendor_panel_set_priority(22);
  // Permanent per-drive journal hook (r89): records the TPS verdict for every drive so the
  // renderer can tell a drive that landed from one that clocked against dead rails.
  metalio_vendor_panel_set_journal_hook(&panelJournalCStr);
  // r129 (H23): hold the SD log writer for the duration of every waveform. The shim calls this at
  // the entry and exit edges of each drive (including the 0x400 recovery), and the sink only ever
  // DEFERS a drain -- logSetDriveActive(false) on an already-idle writer does nothing, and nothing
  // here can add a write or a delay to the panel path.
#if CP_LOG_HOLD_DURING_DRIVE
  metalio_vendor_panel_set_drive_gate_hook([](int active) { logSetDriveActive(active != 0); });
  LOG_INF("DISP", "log hold during drive: ON (SD drain gated for every waveform)");
#else
  LOG_INF("DISP", "log hold during drive: OFF (SD drain may run during a waveform)");
#endif
  LOG_INF("DISP", "panel feed priority 22 (wifi task 23, both core 0)");
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  blinkN(2);
#endif

#ifdef ENABLE_SERIAL_LOG
  Serial.println("[CP] HalSystem::begin in"); Serial.flush();
#endif
#ifndef CP_SKIP_HAL
  busy();
  // DIAG1, AND IT MUST STAY HERE: HalSystem::begin() -> clearPanic() ->
  // clearLastLogs() wipes the shared RTC log ring on every ordinary boot, and the
  // card is not mounted yet, so the previous drive-mode session's lines have to be
  // copied out of that ring BEFORE this call to be written below. No-op unless a
  // session ran (see UsbMscStorage.cpp).
#if CP_USB_MSC
  usbmsc::preserveRtcRing();
#endif
  HalSystem::begin();
#endif
#ifdef ENABLE_SERIAL_LOG
  Serial.println("[CP] HalSystem::begin out"); Serial.flush();
#endif
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  blinkN(3);
#endif
#ifdef CP_STOP_AFTER_HAL
  // Bisect stop: the two riskiest early calls (expander over I2C, then HalSystem)
  // have run. Prove they return by heartbeat instead of continuing into the rest.
  // Serial only: the old extra haptic buzzes went through a board helper that no
  // longer exists (the motor is driven by mn_haptic_pulse inside HapticFeedback.h),
  // and a bisect stop must not depend on the haptics stack it is meant to isolate.
  for (;;) {
    Serial.println("[CP] past HalSystem::begin - heartbeat");
    Serial.flush();
    delay(500);
  }
#endif
  // checkPanic() clears the watchdog capture marker after a successful SD
  // dump, so retain the boot classification for the later activity route.
  const bool rebootedFromPanic = HalSystem::isRebootFromPanic();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_MAX) ? silentRebootTarget : 0;
  const bool silentRebootLightOn = isSilentReboot && (silentRebootPayload & SILENT_REBOOT_LIGHT_ON) != 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;
  silentRebootPayload = 0;

  busy();
#ifndef CP_SKIP_GPIO_BEGIN
  gpio.begin();
#endif
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  blinkN(4);
#endif
#ifdef ENABLE_SERIAL_LOG
  Serial.println("[CP] step: gpio.begin ok"); Serial.flush();
#endif
  busy();
#ifndef CP_SKIP_POWER_BEGIN
  powerManager.begin();
#endif
  // r97 BATT2: open the battery-statistics record once the gauge path is up.
  // begin() also settles the standby gap left by the previous session before
  // anything samples the gauge.
  batteryStats.begin();
  // r115: the PM build property and its frequency table, once per boot. With PM enabled and no
  // esp_pm_configure() call (CONFIG_PM_DFS_INIT_AUTO is not set in this build, and nothing in this
  // tree calls configure()), esp_pm holds every mode at CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ and never
  // enters automatic light sleep -- the log says so here rather than leaving it to be inferred.
  {
#if defined(CONFIG_PM_ENABLE)
    esp_pm_config_t pmCfg = {};
    const esp_err_t pmErr = esp_pm_get_configuration(&pmCfg);
    LOG_INF("PWR", "PM enabled=1 boot cpu=%d MHz pm_cfg=%s min=%d max=%d light_sleep=%d",
            getCpuFrequencyMhz(), pmErr == ESP_OK ? "ok" : "unavailable",
            pmErr == ESP_OK ? pmCfg.min_freq_mhz : -1, pmErr == ESP_OK ? pmCfg.max_freq_mhz : -1,
            (pmErr == ESP_OK && pmCfg.light_sleep_enable) ? 1 : 0);
#else
    LOG_INF("PWR", "PM enabled=0 boot cpu=%d MHz (no esp_pm: frequency is ours alone)", getCpuFrequencyMhz());
#endif
  }
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  blinkN(5);
#endif
#ifdef ENABLE_SERIAL_LOG
  Serial.println("[CP] step: powerManager.begin ok"); Serial.flush();
#endif

#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  // Before display init on purpose: a discarded wake must not pay the ~6 s panel bring-up.
  discardNonPowerWakes();
#endif

  // Board TX-power ceiling. It can only be set once WiFi has started, so it is
  // applied on every STA/AP start rather than at each place that brings WiFi up.
  if (BoardConfig::ACTIVE.wifiMaxTxPower != 0) {
    const auto applyTxCap = [](arduino_event_id_t, arduino_event_info_t) {
      const esp_err_t err = esp_wifi_set_max_tx_power(static_cast<int8_t>(BoardConfig::ACTIVE.wifiMaxTxPower));
      if (err != ESP_OK) LOG_ERR("WIFI", "TX power cap failed: %d", err);
    };
    WiFi.onEvent(applyTxCap, ARDUINO_EVENT_WIFI_STA_START);
    WiFi.onEvent(applyTxCap, ARDUINO_EVENT_WIFI_AP_START);
  }

  blinkN(6);  // milestone: WiFi event handlers registered (before esp_wifi_init)

  const auto wakeupReason = gpio.getWakeupReason();
  // Sample the wake hold now — a click wake is released within milliseconds of
  // boot — but defer the sleep-or-boot decision until SETTINGS is loaded below:
  // click-to-wake is a setting, and an X4 battery power-off cuts all power, so
  // only SD state survives to the next boot.
  // The Metalio boots after the button is released, so it skips the check.
  const bool wakeHoldVerified = wakeupReason != HalGPIO::WakeupReason::PowerButton || BoardConfig::isMetalioEInk4() || BoardConfig::isMetalioEInk4Plus() ||
                                gpio.verifyPowerButtonWakeup();

  // X4 Pro and X4 Classic both map BTN_UP to GPIO0 — an ESP32-S3 boot strap — so
  // gate recovery on the non-strap Down key (GPIO7) to avoid a stuck-in-recovery loop.
  const auto recoveryButton = (BoardConfig::isX4Pro() || BoardConfig::isX4Classic()) ? MappedInputManager::Button::Down
                                                                                     : MappedInputManager::Button::Up;
  // The Picco has no second app slot to flash into (partitions_picco.csv).
  const bool recoveryFirmwareMode = wakeupReason == HalGPIO::WakeupReason::PowerButton && !BoardConfig::isPaperMono() &&
                                    !BoardConfig::isPicco() && mappedInputManager.isPressed(recoveryButton);

#ifndef CP_SKIP_TILT_CLOCK
  halTiltSensor.begin();
#endif
  blinkN(7);  // milestone: halTiltSensor.begin() returned
#ifdef ENABLE_SERIAL_LOG
  Serial.println("[CP] step: halTiltSensor.begin ok"); Serial.flush();
#endif
#ifndef CP_SKIP_TILT_CLOCK
  halClock.begin();
#endif
  blinkN(8);  // milestone: halClock.begin() returned
#ifdef ENABLE_SERIAL_LOG
  Serial.println("[CP] step: halClock.begin ok"); Serial.flush();
#endif

#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");
#else
  LOG_INF("MAIN", "Device: %s", BoardConfig::ACTIVE.name);
#endif

  // ---------------------------------------------------------------------------
  // r97 SAFETY — an image that has secure boot or flash encryption enabled makes the
  // BOOTLOADER burn the key/encryption eFuses on its FIRST boot. That is irreversible:
  // no reflash, no erase-all, no recovery. This firmware never needs it, nothing in the
  // tree writes an eFuse, and tools/check_efuse_safety.py refuses to build such a
  // configuration (see crosspoint_app/CMakeLists.txt). This check is the second line of
  // defence: if an image built somewhere else, or a config that slipped past the gate,
  // ever runs here, it says so on the console instead of silently arming a fuse.
  // ---------------------------------------------------------------------------
  if (esp_secure_boot_enabled() || esp_flash_encryption_enabled()) {
    LOG_ERR("SAFETY",
            "SECURE BOOT/FLASH-ENC IS ENABLED - eFuse may be burned; this image must not be shipped "
            "(secure_boot=%d flash_enc=%d)",
            esp_secure_boot_enabled() ? 1 : 0, esp_flash_encryption_enabled() ? 1 : 0);
  }

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    // r129 (H34): the failure path already logs the stage/MBR detail from the driver; this line adds
    // the CARD STATE the boot saw, so "no card" and "card that will not mount" are distinguishable
    // from one log paste. Diagnostic only -- it sits before the same return that was always here.
    LOG_ERR("MAIN", "SD card initialization failed (mounted=0 card_bytes=%llu)",
            static_cast<unsigned long long>(Storage.sdTotalBytes()));
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }
  // r129 (H34): one line per boot with the mount result and the card the mount settled on. The
  // readOnly verdict is the marker check below, reported again here so a single line answers
  // "did it mount, what card, and may we write".
  LOG_INF("SD", "mount: ok=1 card_bytes=%llu readOnly=%d",
          static_cast<unsigned long long>(Storage.sdTotalBytes()), Storage.readOnly() ? 1 : 0);
  blinkN(9);  // milestone: Storage.begin() returned true (SD mounted)
  // ---- r109 CARD2/FORMAT1: A MOUNTED CARD IS NOT A CARD WE VOUCH FOR ---------------------------
  // The 10 Nov card mounted fine with its directory tree gone, and the boot then wrote the default
  // settings and an empty library index over the user's data. So the first thing after a successful
  // mount is the marker check: if the volume carries neither our marker nor a legacy
  // settings.json, and its root holds nothing but housekeeping entries, storage goes READ-ONLY for
  // this boot -- no mkdir, no settings, no library index -- until `sd init` says otherwise.
  if (!Storage.checkVolumeMarker()) {
    LOG_ERR("MAIN", "SD mounted but the volume is NOT vouched for: read-only boot (run 'sd init' to lay out a fresh card)");
  }
  // r129 (H34): one line per boot with the mount result, the card the mount settled on and the
  // final write verdict -- after the marker check, so readOnly= here is the answer for this boot.
  LOG_INF("SD", "mount summary: ok=1 card_bytes=%llu readOnly=%d reason=%s",
          static_cast<unsigned long long>(Storage.sdTotalBytes()), Storage.readOnly() ? 1 : 0,
          Storage.readOnly() ? Storage.readOnlyReason() : "none");

#if CP_USB_MSC
  // USB drive mode exits by rebooting (it never remounts the card live), so its
  // exit line was captured in RTC memory while the volume was detached. Append it
  // now, on the first boot that has a filesystem again.
  usbmscjournal::flushDeferred();
  // DIAG1: and the session's own evidence -- op counters, worst op, last LBA and
  // the RTC log lines preserved before HalSystem::begin() -- goes next to it, as
  // 'session summary:' plus '[RTC]' prefixed lines. One dump per session.
  usbmsc::dumpSessionToLog();
#endif

  // UI density, one line per boot, appended straight to the SD log. The capture
  // ring has long evicted the first lines of a boot by the time anyone dumps it,
  // so this is the only way to read the resolved scale back without a console.
  // Format:
  //   [ms] [INF] [UISCALE] scale=1.350 src=board panel=1216x684 headerHeight=113 menuRowHeight=61
  // src: board (BoardProfile::uiScale) | ppi (-DCROSSPOINT_PANEL_PPI/235) |
  //      ppi-area (area-equivalent density, no PPI known) | override (A/B pin).
  {
    const auto& uiMetrics = UITheme::getInstance().getMetrics();
    LOG_INF("UISCALE", "scale=%.3f src=%s panel=%ux%u headerHeight=%d menuRowHeight=%d", uiDensity::scale(),
            uiDensity::sourceName(), static_cast<unsigned>(BoardConfig::ACTIVE.displayWidth),
            static_cast<unsigned>(BoardConfig::ACTIVE.displayHeight), uiMetrics.headerHeight, uiMetrics.menuRowHeight);
    Storage.mkdir("/.crosspoint");
    constexpr const char* kDbgLog = "/.crosspoint/debug.log";
    // The debug log is capped so it cannot grow without bound across boots: past
    // lib/Logging/Logging.cpp's kSdLogMaxBytes it is rolled to debug.log.old. The value is
    // deliberately not repeated here -- it was (as "64 KB") and had already rotted to
    // nothing near the real cap; the constant is the only place it lives. Every other
    // writer to this path calls the same roll.
    logSdRollIfOversized();
    const bool freshLog = !Storage.exists(kDbgLog);
    HalFile uiLog = Storage.open(kDbgLog, O_WRONLY | O_CREAT | O_APPEND);
    if (uiLog) {
      char line[192];
      int n = 0;
      if (freshLog) {
        // Same header logSdFlush() writes on a fresh file, so appending here
        // first does not rob the ring dump of its identifying first line.
        n = snprintf(line, sizeof(line), "--- boot %.48s uptime=%lu ms ---\n", CROSSPOINT_VERSION,
                     static_cast<unsigned long>(millis()));
        if (n > 0) uiLog.write(line, static_cast<size_t>(n));
      }
      // LOG2 -- the boot marker, first line of EVERY session in this file.
      //
      // Why it is needed: on 4 Nov two consecutive `logtail` reads returned windows ending at
      // t=41 s and t=22 s while `ver` reported uptime 151 s, and the stale window nearly
      // produced a wrong conclusion about the panel. A log window's age is only unambiguous
      // when the file says where the session began, so every boot now stamps one line with the
      // reset reason (the IDF ordinal AND its name -- the legend in this file has been off by
      // one before, and a bare number is exactly how that rotted) and the build version.
      //
      // Written straight to the file, not through LOG_INF: the runtime debug gate is applied
      // later in this function (logSetRuntimeDebug), so a gated line here would be silent on a
      // normal boot -- i.e. exactly the boots that need the marker.
      n = snprintf(line, sizeof(line), "[%lu] [INF] [BOOT] boot reason=%d(%s) build=%s\n",
                   static_cast<unsigned long>(millis()), static_cast<int>(esp_reset_reason()),
                   cpResetReasonName(esp_reset_reason()), CROSSPOINT_VERSION);
      if (n > 0) uiLog.write(line, static_cast<size_t>(n));
      n = snprintf(line, sizeof(line),
                   "[%lu] [INF] [UISCALE] scale=%.3f src=%s panel=%ux%u headerHeight=%d menuRowHeight=%d\n",
                   static_cast<unsigned long>(millis()), uiDensity::scale(), uiDensity::sourceName(),
                   static_cast<unsigned>(BoardConfig::ACTIVE.displayWidth),
                   static_cast<unsigned>(BoardConfig::ACTIVE.displayHeight), uiMetrics.headerHeight,
                   uiMetrics.menuRowHeight);
      if (n > 0) uiLog.write(line, static_cast<size_t>(n));
      // PM-P1: the effective dynamic-power-management configuration, one line per boot.
      // Written straight to the file for the same reason as the boot marker above: the
      // runtime debug gate is applied later in this function, and "does this image really
      // carry PM-P1, and at which frequency?" must be answerable from a `logtail` on a
      // normal boot with the gate off. pm = CONFIG_PM_ENABLE compiled in, tickless =
      // FreeRTOS tick suppression, max_freq and light_sleep read back from esp_pm
      // (light_sleep=0 is the expected value in this build: esp_pm_configure() is never
      // called, so automatic light sleep is OFF and the CPU stays at 320 MHz).
      {
#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
        esp_pm_config_t pmCfg = {};
        const esp_err_t pmErr = esp_pm_get_configuration(&pmCfg);
        const bool pmCfgOk = pmErr == ESP_OK;
        n = snprintf(line, sizeof(line),
                     "[%lu] [INF] [PM] pm=%d tickless=%d max_freq=%d MHz light_sleep=%d%s\n",
                     static_cast<unsigned long>(millis()), CONFIG_PM_ENABLE ? 1 : 0,
                     CONFIG_FREERTOS_USE_TICKLESS_IDLE ? 1 : 0,
                     pmCfgOk ? pmCfg.max_freq_mhz : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
                     pmCfgOk && pmCfg.light_sleep_enable ? 1 : 0,
                     pmCfgOk ? "" : " (esp_pm_get_configuration failed)");
#else
        n = snprintf(line, sizeof(line), "[%lu] [INF] [PM] pm=0 tickless=0 max_freq=%d MHz light_sleep=0\n",
                     static_cast<unsigned long>(millis()), CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
#endif
        if (n > 0) uiLog.write(line, static_cast<size_t>(n));
      }
      // Falsifiable follow-up to the line above: did every scaling-table field
      // move exactly as its MetricScale class says, and did the ACTIVE THEME's
      // own menu geometry follow the scale? Silent when clean, so a boot with no
      // "[UISCALE] theme=... not scaled" line is the proof this fix holds.
      const char* audit = UITheme::getInstance().auditUiScale(renderer);
      if (audit != nullptr && audit[0] != '\0') {
        n = snprintf(line, sizeof(line), "[%lu] [WRN] %s\n", static_cast<unsigned long>(millis()), audit);
        if (n > 0) uiLog.write(line, static_cast<size_t>(n));
      }
      // AUDIT2 -- the half of the scale audit that auditUiScale() structurally cannot do.
      //
      // auditUiScale() proves every field in kMetricFields[] moved with the density scale. It
      // says nothing about a screen that never consults a metric and writes a pixel number
      // straight into the draw call: `U-2` (tab band = literal 50 px on the touch path) and
      // `U-3` (toggle = literal 28 px) were both invisible to it and were found on glass.
      // The list is generated at build time (scripts/gen_ui_literals.py) and compiled in, so
      // this is a report of SOURCE, readable from a logtail with no host-side step.
      //
      // Cap on purpose: the full list runs to ~100 entries and debug.log is capped at 128 KB,
      // so the log gets the count, the first few, and the command that prints the rest.
      // READ-ONLY: nothing here draws, scales or gates anything.
      {
        constexpr int kLitLinesShown = 16;
        const int litTotal = uiLiteralAudit::count();
        n = snprintf(line, sizeof(line),
                     "[%lu] [INF] [UILIT] n=%d files=%d unscaled UI literals (integer where a metric "
                     "is expected; U-2/U-3/U-6 class)\n",
                     static_cast<unsigned long>(millis()), litTotal, uiLiteralAudit::fileCount());
        if (n > 0) uiLog.write(line, static_cast<size_t>(n));
        const int shown = litTotal < kLitLinesShown ? litTotal : kLitLinesShown;
        for (int i = 0; i < shown; ++i) {
          n = snprintf(line, sizeof(line), "[%lu] [WRN] [UILIT] %s\n",
                       static_cast<unsigned long>(millis()), uiLiteralAudit::entry(i));
          if (n > 0) uiLog.write(line, static_cast<size_t>(n));
        }
        if (litTotal > shown) {
          n = snprintf(line, sizeof(line),
                       "[%lu] [INF] [UILIT] +%d more not shown; full list: "
                       "python scripts/gen_ui_literals.py --print\n",
                       static_cast<unsigned long>(millis()), litTotal - shown);
          if (n > 0) uiLog.write(line, static_cast<size_t>(n));
        }
      }
      uiLog.flush();
      uiLog.close();
    }
  }
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  // Before the display takes the USB pad: after that this console is silent.
  dumpLogTailToStdout("/.crosspoint/debug.log", 4096);
  // The standby journal from the PREVIOUS session, so a sleep/wake cycle can be verified from the
  // console without pulling the card. sleep.txt is appended by the standby loop itself.
  dumpLogTailToStdout("/.crosspoint/sleep.txt", 2048);

  // Sleep forensics, into the SD log where it survives: everything the sleep path decided happens
  // before the logger exists, so this is the only place it can be read back afterwards.
  //   reset  : esp_reset_reason_t, printed as `reset=<n>(<name>)`. The numbers are
  //            IDF's ordinals, exactly: 0 UNKNOWN, 1 POWERON, 2 EXT pin, 3 SW (esp_restart),
  //            4 PANIC, 5 INT_WDT, 6 TASK_WDT, 7 other WDT, 8 DEEP-SLEEP wake, 9 BROWNOUT,
  //            10 SDIO, 11 USB, 12 JTAG, 13 EFUSE error, 14 PWR_GLITCH, 15 CPU_LOCKUP.
  //            The legend that used to sit here was OFF BY ONE from index 3 upward, and every
  //            value it named past 3 was wrong: it called 3 "power-on" (3 is a software reset),
  //            8 a "watchdog" (8 is the ordinary deep-sleep wake -- so each normal wake was read
  //            as a WDT reset), 9 a "deep-sleep wake" (9 is BROWNOUT) and 12/13 "SW reset"
  //            (12 is JTAG, 13 an eFuse error). That is the mislabelling the name suffix now
  //            makes impossible: the log carries the word, not a number whose meaning lived only
  //            in this comment.
  //   cause  : 0 undefined (cold/reset), 1 all, 4 timer, 5 touchpad, 6 ext1, 7 ext0, 8 gpio
  //   aborted: esp_deep_sleep_start() returned and deepSleep() had to force a restart -- a sleep the
  //            SoC refused, which looks exactly like "the device just reboots instead of sleeping".
  {
    // S31 light-sleep shim: apply the calibration startup bias, then snapshot whatever the previous
    // sleep cycle left in RTC memory. If the firmware died INSIDE a light-sleep slice, phase=200/
    // 500/502 here names the exact SDK step it died in -- but only on the next boot.
    metalio_sleep_init();
    metalio_sleep_capture_reset();
  }
  {
    const auto aborted = freeink::PowerManager::takeAbortedSleepInfo();
    const auto armed = freeink::PowerManager::takeArmedSleepInfo();
    LOG_INF("PWR", "boot: reset=%d cause=%d abortedSleep=%d abortCause=%d abortPin=%d touch=%d armedLevel=%d",
            static_cast<int>(esp_reset_reason()), static_cast<int>(esp_sleep_get_wakeup_cause()),
            aborted.aborted ? 1 : 0, aborted.wakeupCause, aborted.wakePinLevel,
            mappedInputManager.hasTouch() ? 1 : 0, armed.valid ? armed.gpioLevel : -1);

    // Written straight to a file, not through the capture ring: "Print logs to SD" only dumps the
    // RAM ring, which has long since evicted the first lines of the boot -- which is where every
    // sleep decision lives. One line per boot, appended, capped so it stays readable.
    Storage.mkdir("/.crosspoint");
    constexpr const char* kSleepLog = "/.crosspoint/sleep.txt";
    if (Storage.exists(kSleepLog)) {
      std::string prev;
      if (Storage.readFileToString("PWR", kSleepLog, 8192, prev) && prev.size() > 4096) {
        // Roll over rather than grow without bound, but keep the previous generation.
        constexpr const char* kSleepLogOld = "/.crosspoint/sleep.txt.old";
        Storage.remove(kSleepLogOld);
        if (!Storage.rename(kSleepLog, kSleepLogOld)) Storage.remove(kSleepLog);
      }
    }
    HalFile f = Storage.open(kSleepLog, O_WRONLY | O_CREAT | O_APPEND);
    if (f) {
      char line[256];  // reset=<n>(<name>) now carries up to ~50 chars of name
      const int n = snprintf(line, sizeof(line),
                             "reset=%d(%s) cause=%d aborted=%d abortCause=%d abortPin=%d touch=%d armedLevel=%d "
                             "armErr=%d uph=%lu\n",
                             static_cast<int>(esp_reset_reason()), cpResetReasonName(esp_reset_reason()),
                             static_cast<int>(esp_sleep_get_wakeup_cause()),
                             aborted.aborted ? 1 : 0, aborted.wakeupCause, aborted.wakePinLevel,
                             mappedInputManager.hasTouch() ? 1 : 0, armed.valid ? armed.gpioLevel : -1,
                             armed.valid ? armed.armErr : -99,
                             static_cast<unsigned long>(millis()));
      if (n > 0) f.write(line, static_cast<size_t>(n));
      f.flush();
      f.close();
    }
  }
#endif
  // Runtime debug gate. INF/DBG call sites are compiled in (CP_DEBUG_LOG) but stay silent until the
  // Debug page's switch is on; the flag lives on the card so it survives the power cycle each flash
  // needs. Applied here because the flag is only readable once Storage is up.
  logSetRuntimeDebug(debugflag::enabled());

  HalSystem::checkPanic();
  blinkN(10);  // milestone: checkPanic() returned

  APP_STATE.loadFromFile();
  blinkN(11);  // milestone: settings loaded
  const bool isSleepWake = wakeupReason == HalGPIO::WakeupReason::PowerButton;
  const bool isPersistedSleepWake = isSleepWake && !APP_STATE.showBootScreen;

  if (recoveryFirmwareMode) {
    LOG_INF("MAIN", "Recovery firmware mode (%s + POWER held at boot)",
            (BoardConfig::isX4Pro() || BoardConfig::isX4Classic()) ? "DOWN" : "UP");
  }

  // Touch boards default the reader menu to the toolbar overlay instead of the
  // full-screen list. Seeded before the load: fromJson() falls back to the
  // in-memory value only when the file carries no readerMenuStyle key, so a
  // user's saved choice (either style) still wins.
  if (gpio.hasTouch()) {
    SETTINGS.readerMenuStyle = CrossPointSettings::READER_MENU_TOOLBAR;
  }
  SETTINGS.loadFromFile();
  // Step 2.5 item 8: push the saved charge current into the CX25601N. Deliberately
  // here -- after the settings file is read and before anything can plug into the
  // charger path -- and deliberately ONCE: until this call the part runs on its
  // power-on default, and this is what ends that. A missing/unanswering charger logs
  // and returns; it never blocks boot.
  if (BoardConfig::isMetalioEInk4Plus()) {
    chargercurrent::applyChoice(SETTINGS.chargeCurrent);
    // r129 (H31): the CX25601N's VREG/termination, set to the vendor's own init values -- 4400 mV
    // and EN_TERM=1 -- once per boot, right after the charge current. This is a CHARGING behaviour
    // change only: it is a read-modify-write of REG0x04/0x05/0x14 on the charger and touches no
    // panel rail, no expander and no park/resume step. It reads back what the part actually holds
    // and logs a mismatch rather than assuming the write landed.
    chargercurrent::applyVendorVreg();
  }
  // Step 2.5 items 9 and 11, applied once at boot from the saved settings:
  //   item 9  the idle CPU frequency step. Logs `[PWR] idle cpu: set=.. applied=..`;
  //           the frequency itself is only entered at the next idle edge, which is
  //           the existing policy's job (setPowerSaving(true) after 3 s).
  //   item 11 the panel VCOM / black depth. Logs `[PWR] vcom: set=<mv> applied=<mv>`
  //           with a register read-back; the value is re-written by epdPowerOn() at
  //           every panel power-on from here on.
  systemKnobs::applyIdleFreq(SETTINGS.idleCpuFreq);
  if (systemKnobs::PANEL_VCOM_AVAILABLE) {
    systemKnobs::applyPanelVcom(SETTINGS.panelVcomDepth);
  }
  // Push the saved timezone's POSIX rule into the clock (migrating the legacy
  // UTC-offset setting on first boot after the update).
  timezones::applyToClock();
  RECENT_BOOKS.loadFromFile();
  // r163 B6: the vocabulary history is one small JSON file; loading it here keeps the
  // reader menu's Vocabulary row honest (it is hidden when the list is empty).
  VOCAB_HISTORY_STORE.loadFromFile();
  // r163 3.3: the reading-statistics record (per book and lifetime totals).
  READING_STATS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  OPDS_TOKENS.loadFromFile();
  LCP_PASSPHRASES.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);
  pluginevents::refreshSubscriptions();
  blinkN(12);  // milestone: all stores loaded (settings/recent/koreader/OPDS/LCP), theme, plugin subs
  // AUDIO1: arm the UI-sound worker. Reads only the two persisted switches
  // (audioOutEnabled, uiSoundsEnabled), builds its five short waveforms and
  // blocks on a semaphore. It touches no rail, no UART and no I2S, and with
  // both switches off (the defaults) it can never make a sound.
  uisounds::init();
  // Seed the system clock from the battery-backed RTC: on RTC boards it is the
  // only real clock across power-off, and time() is what plugin event
  // timestamps, TLS and file dates read. Placed before trustedtime::init(),
  // whose floor restore vetoes a lagging RTC and restores the floor instead.
  //
  // RTC1: the RTC's calendar is only as good as its backup supply, and power-off
  // on this board deliberately collapses the 3.3 V rail. The PCF8563 says so
  // itself through VL (bit 7 of register 0x02), which HalClock reads on every
  // poll. When VL is set the calendar is the last value the chip knew, not
  // elapsed time, so it is NOT adopted: the persisted TrustedTime floor stands
  // (restored by trustedtime::init() below) and trustedtime::trustedNow() keeps
  // returning 0 until a real sync arrives. Adopting it would make a stale date
  // look like a fact to every date-based check in the firmware.
  struct tm rtcLocal;
  const bool rtcHasTime = halClock.localTime(rtcLocal);
  const bool rtcVl = halClock.voltageLow();
  if (rtcHasTime && !rtcVl) {
    // localTime() renders the RTC's UTC epoch under the TZ rule applied above,
    // so mktime() inverts it exactly (tm_isdst included).
    trustedtime::adopt(static_cast<int64_t>(mktime(&rtcLocal)));
    LOG_INF("CLK", "RTC read: %04d-%02d-%02d %02d:%02d vl=0", rtcLocal.tm_year + 1900, rtcLocal.tm_mon + 1,
            rtcLocal.tm_mday, rtcLocal.tm_hour, rtcLocal.tm_min);
  } else if (rtcHasTime) {
    LOG_ERR("CLK", "RTC read: %04d-%02d-%02d %02d:%02d vl=1 - backup supply lost, clock NOT trusted; not adopted",
            rtcLocal.tm_year + 1900, rtcLocal.tm_mon + 1, rtcLocal.tm_mday, rtcLocal.tm_hour, rtcLocal.tm_min);
  } else if (rtcVl) {
    // VL set and the driver refused to hand out a time at all (its own guard).
    LOG_ERR("CLK", "RTC read: no time available, vl=1 - backup supply lost; persisted floor stands until a sync");
  }
  // Restore the monotonic clock floor before anything reads time() (event
  // timestamps, loan-expiry checks).
  trustedtime::init();
  bleturner::begin(pageturner::host, SETTINGS.ble);
  blinkN(13);  // milestone: bleturner::begin() returned (BLE pager init)

  // Brightness and warmth are always restored. A normal wake starts with the
  // light off unless Restore Light on Wake is enabled; silent maintenance
  // reboots replay the live state captured at restart, so they neither go dark
  // nor light up against the user's wake preference.
  const bool restoreLightOn =
      isSilentReboot ? silentRebootLightOn : (SETTINGS.frontlightOn != 0 && SETTINGS.frontlightRestoreOnWake != 0);
#ifndef CP_SKIP_FRONTLIGHT_BEGIN
  Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth, restoreLightOn);
#endif
  // Milestone 14 is the LAST frontlight-coded marker: from here the app owns the
  // light (restoreLightOn may hold it on), so a continuous glow after 14 means
  // "reached the wake-reason switch", not a hang.
  // CP_SKIP_FRONTLIGHT_BEGIN removes exactly one call between milestone 13 and 14,
  // so "13 then dead" vs "14 reached" isolates Frontlight.begin() as the culprit.
  // The blinks still work when it is skipped: blinkN drives GPIO6/7 directly.
  blinkN(14);  // milestone: Frontlight.begin() returned (or was skipped)
  pulseK(1);   // sub-step: milestone 14 confirmed, entering the wake-reason switch

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      // With Short Power Button Press = Sleep, a single click wakes on any
      // device; otherwise the button must still be held (ghost-wake debounce).
      // Picco: plugging in USB pulses the wake line; power on, as stock does.
      if (!wakeHoldVerified && SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::SLEEP &&
          !(FREEINK_DEVICE_PICCO && gpio.isUsbConnected())) {
        LOG_DBG("MAIN", "Power-button wake not held through verification, sleeping");
        Storage.prepareForDeepSleep();
        powerManager.startDeepSleep(gpio);
      }
      wakePowerReleasePending = true;
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // Most devices return to sleep after a USB-powered cold boot.
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
#if FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC || FREEINK_DEVICE_PAPERMONO || FREEINK_DEVICE_EEGO_A4 || \
    FREEINK_DEVICE_METALIO_EINK4 || FREEINK_DEVICE_METALIO_EINK4_PLUS || FREEINK_DEVICE_PICCO
      // X4 Pro must stay awake so USB Serial/JTAG remains available after leaving
      // USB Drive and reconnecting the cable. Paper Mono has no armable GPIO wake
      // (its button is behind the PMIC). Metalio also needs native USB available
      // after a USB-powered boot, as does the Picco. EEGO A4's post-flash reset reads as
      // POWERON (native-USB), so a flash would otherwise be misclassified as a
      // USB-power cold boot and sleep. Sleeping any of these here would strand
      // the device in a USB-replug boot loop (or sleep right after a flash).
      break;
#else
      Storage.prepareForDeepSleep();
      powerManager.startDeepSleep(gpio);
      break;
#endif
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }
  pulseK(2);  // sub-step: wake-reason switch completed (no deep sleep triggered)

  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  // Only a verified deep-sleep wake may use the one-shot persisted flag.
  // Otherwise a stale flag could suppress the splash on a cold boot.
  const BootResume resume = isSilentReboot         ? BootResume::Silent
                            : isPersistedSleepWake ? BootResume::SplashlessWake
                                                   : BootResume::Splash;
  bool allowFastInitialReaderRefresh = false;
  bool needsWakeRefresh = false;
  pulseK(3);  // sub-step: resume path resolved, about to init the display

  // SUB-STEP 4: PSRAM STORE TEST. Detection is not writability -- on this S31 the boot path
  // has been observed to leave PMA entry 7 unmapped, so external RAM appears in the heap but
  // the first store faults (Store access fault, MCAUSE 0x7, MTVAL 0x50000020) and the board
  // resets. Our S31PmaRegionFix maps the entry, but nothing has ever verified a real store
  // from the application. A display framebuffer is usually the first large PSRAM allocation,
  // so if that store is going to fault, this is where it would.
  // pulseK(4) fires ONLY if the write/read-back succeeds.
  {
    constexpr size_t kProbeBytes = 256 * 1024;
    auto* const probe = static_cast<uint8_t*>(heap_caps_malloc(kProbeBytes, MALLOC_CAP_SPIRAM));
    bool writable = false;
    if (probe != nullptr) {
      writable = true;
      for (size_t i = 0; i < kProbeBytes; i += 4096) {
        probe[i] = static_cast<uint8_t>((i >> 12) & 0xFF);
      }
      for (size_t i = 0; i < kProbeBytes; i += 4096) {
        if (probe[i] != static_cast<uint8_t>((i >> 12) & 0xFF)) {
          writable = false;
          break;
        }
      }
      heap_caps_free(probe);
    }
    if (writable) {
      // BINARY VERDICT, not a count: one long steady warm glow. A 0.25 s pulse among
      // fast pulses is easy to miscount (camera exposure varies mid-burst and the first
      // pulse of a burst can read at 1/3 brightness), and this decides whether the next
      // work is memory or panel-driver. Long glow = PSRAM writable; no glow = it faulted.
      digitalWrite(kCool, LOW);
      // DOUBLE glow = PSRAM writable. Encoded as a PATTERN rather than a duration so it cannot be
      // confused with the 1-6 s beacons that follow immediately after.
      for (int i = 0; i < 2; ++i) {
        digitalWrite(kWarm, HIGH);
        delay(1200);
        digitalWrite(kWarm, LOW);
        delay(700);
      }
      digitalWrite(kCool, HIGH);
      delay(800);
    }
  }

  busy();  // COOL off: if it stays dark now, the death is INSIDE setupDisplayAndFonts
  // BOOT1: the third argument asks setupDisplayAndFonts() to paint the logo as soon as
  // the renderer is up, i.e. before it does the font work -- only on the ordinary boot
  // that is going to show the boot screen. Silent reboots and splashless wakes keep the
  // frame already on the glass, and must not have a logo driven over it.
  setupDisplayAndFonts(resume != BootResume::Splash, resume == BootResume::Splash);
  // No pulse marker here: dpulse(7) inside setupDisplayAndFonts() already marks its completion,
  // and a marker after it would make the sequence non-monotonic (1,2,3,5,6,7,4).
  blinkN(15);  // milestone: display + fonts initialised (panel driver begin returned)

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::SplashlessWake:
      // One-shot flag: re-arm the splash for the next ordinary boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a splashless-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (Storage.exists(SLEEP_FRAME_FILE) && loadSleepFrameBuffer()) {
        if (gpio.deviceIsX3()) {
          // begin() clears the X3 controller RAM, so restore the saved frame as
          // the baseline for the first reader paint without refreshing the panel.
          renderer.cleanupGrayscaleWithFrameBuffer();
          allowFastInitialReaderRefresh = true;
        }
      } else {
        // Clean the retained sleep image as part of the first Home paint.
        needsWakeRefresh = true;
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  // Output polarity is resolved per render by ActivityManager (night mode
  // inverts only the reading surfaces), so nothing to restore here.

  // THE SILENT-REBOOT GLASS CONTRACT, app side (see the call site above, next to
  // metalio_vendor_panel_begin()). The driver did NOT whiten the glass on this boot, so the first
  // paint has to be a WHOLE-PANEL drive rather than a diff against a frame the previous session
  // drew: every screen routed to below is arriving over somebody else's ink, which is exactly what
  // the boot clear used to hide. forceCleanNextRefresh() is the policy's non-cancellable
  // from-white term (a promotion can be parked by a screen; this cannot), so whatever is routed to
  // lands on one GC16-from-white with conditioning on every pixel -- including this boot's Home,
  // which has no activity above it and therefore never reaches goHome()'s arrival arm.
  // Idempotent with ReaderActivity::onEnter()'s own arm, which exists for the in-process returns
  // where no boot happens at all.
  //
  // cleanPaintsAtArm is read HERE, not just before the blocking wait below: a screen can paint
  // while its own onEnter() is building (Home's cover-generation popup does exactly that), and
  // that paint legitimately consumes the arm. Counting from the arm makes the self-check below
  // ask the right question -- "did ANY paint consume it" -- instead of "did the last one".
#if CP_RICKY_REFRESH_POLICY
  uint32_t cleanPaintsAtArm = 0;
#endif
  if (resume == BootResume::Silent) {
    renderer.forceCleanNextRefresh();
#if CP_RICKY_REFRESH_POLICY
    cleanPaintsAtArm = renderer.forcedCleanPaintsConsumed();
#endif
    LOG_INF("MAIN", "silent reboot: first paint is a whole-panel forced clean");
  }

  if (recoveryFirmwareMode) {
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
    // PARKED with the settings entry (see SettingsActivity): an update image is the one failure a
    // USB-only recovery cannot always undo, so this board boots to the normal screen instead.
    LOG_ERR("MAIN", "SD firmware recovery is parked on this board");
    activityManager.goHome();
#else
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
#endif
  } else if (rebootedFromPanic) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_JOIN_NETWORK) {
    // Rebooted on the way *into* File Transfer > Join Network for a fresh heap;
    // resume that flow directly instead of landing on home.
    activityManager.goToJoinNetwork();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_SETTINGS) {
    // Back out of the WiFi rows and the user is where they left off, not on Home.
    activityManager.goToSettings(SettingsSection::Network);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome(HomeMenuItem::NONE, needsWakeRefresh);
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path, allowFastInitialReaderRefresh);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    //
    // SELF-CHECK for THE SILENT-REBOOT GLASS CONTRACT above: the skipped boot clear
    // is only safe because this branch armed a whole-panel forced clean, and that arm
    // is only satisfied if some paint actually CONSUMED it before the glass was
    // considered ready. If nothing did -- a screen that painted nothing, a repaint
    // that resolved elsewhere -- the panel could be holding previous-session ink
    // wherever this frame is white. The counter is the policy's own, so this is
    // evidence, not an assumption; the repair is one more forced clean, paid only in
    // the case that should never happen.
#if CP_RICKY_REFRESH_POLICY
    const uint32_t cleanBefore = cleanPaintsAtArm;
#endif
    activityManager.requestUpdateAndWait();
#if CP_RICKY_REFRESH_POLICY
    if (renderer.forcedCleanPaintsConsumed() == cleanBefore) {
      LOG_ERR("MAIN", "silent reboot: no paint consumed the forced clean -- repainting once from white");
      renderer.forceCleanNextRefresh();
      activityManager.requestUpdateAndWait();
    }
#endif

    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

#if CP_USB_SHELL
  // Runtime toggle: the shell is a capability in the image (CP_USB_SHELL=1) that
  // stays OFF until Debug page -> "USB debug shell" is switched on, persisted as
  // SETTINGS.usbDebugShell (key "usbDebugShell"). Nothing here holds the UART or
  // starts a task when the setting is 0.
  usbshell::UiOps shellOps{};
  shellOps.panel = &shellPanel;
  shellOps.pmic = &shellPmic;
  shellOps.refresh = &shellRefresh;
  shellOps.shot = &shellShot;
  shellOps.activity = &shellActivity;
  shellOps.spine = &shellSpine;
  shellOps.reboot = &shellReboot;
  // E5: shipping, unconditional — the point is that the image that ships can
  // recover touch after a failed probe without a bench build.
  shellOps.touch = &shellTouch;
  // r96 bench commands. Both of these run on the main task and both refuse what
  // they cannot do safely (see their bodies). r97: wired only when their switch is on,
  // so the shipping image has no bench entry points at all.
#if CP_LP_BENCH
  shellOps.lpsleep = &shellLpSleep;
#endif
#if CP_PPA_BENCH
  shellOps.ppabench = &shellPpaBench;
  // PPA1: the renderer's own buffer, both ways — plan, comparison and timing.
  shellOps.ppaframe = &shellPpaFrame;
#endif
#if CP_EXP
  // EXP1: the arm family. Registered only when the substrate is compiled in, so
  // a CP_EXP=OFF image has no `exp` command to type and no strings behind it.
  shellOps.exp = &shellExp;
#endif
  usbshell::installUiOps(shellOps);
  // r91 R2/R7: NEVER start on this path alone. The session flag is restored only when
  // the reset was warm (esp_restart from `reboot`, panic, watchdog, brownout) AND the
  // RTC-domain marker says the session was on before it -- so `reboot` from the shell
  // keeps the shell, while a cold boot (power-on, power-off then on, battery reconnect)
  // does not. The persisted preference is the operator's choice for the menu row, not a
  // licence to open a console at power-on.
  if (SETTINGS.usbDebugShell && usbshell::sessionEnabled()) {
    LOG_INF("SHELL", "usbDebugShell=1 and the session survived a warm reset -> starting the console REPL");
    usbshell::start();
  } else if (SETTINGS.usbDebugShell) {
    LOG_INF("SHELL", "usbDebugShell=1 persisted, session OFF this boot (cold boot/sleep/power-off): not starting");
  }
#endif
  allowSleepAt = millis() + 2000;  printf("[CP] setup() COMPLETE -- entering loop()\n");
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  // r163 B5: start a new gesture epoch HERE, at the last point before loop() can run. A silent
  // restart (heap-defrag ESP.restart()) re-enters setup() with millis() near zero, so any press
  // stamp from before it is meaningless; and a finger still on the key when the new image boots
  // read as a press of "0 ms so far" and was one release away from being taken as a click. The
  // epoch marks such a press INHERITED -- its release is discarded, never classified -- which is
  // the same rule as powerReleasedSinceWake: a hold that began before the edge could be seen has
  // to be released before it counts. On a cold boot this is the same read: a finger resting on
  // the key at boot cannot sleep the device on release either.
  metalioPowerGesture.beginBootMs(millis(), gpio.isPressed(HalGPIO::BTN_POWER));
  if (metalioPowerGesture.pressInherited()) {
    LOG_INF("MAIN",
            "power gesture epoch: key already down at boot (press predates this boot; its release "
            "will be discarded)");
  }
#endif
  fflush(stdout);
}

// B-P1-3 (r101.13b): when the poll taken at a panel drive's edge latched this iteration's input.
// InputManager::update() CLEARS every one-shot event on entry, so the loop's own poll -- taken
// immediately after the drive returns -- used to wipe the press the hook had just latched, and the
// event reached no activity at all ("presses during a drive are eaten"). With the fix on, the hook's
// poll IS that iteration's sample and the loop skips exactly one poll instead of taking a second one.
//
// A timestamp, not a bool: a stale flag would hand the next activity an edge sampled seconds ago and
// skip the fresh poll at the same time -- a phantom tap, the failure mode this has to avoid. Past
// kDriveInputPollMaxAgeMs the loop polls normally and the hook's sample is discarded.
//
// release/acquire: the drive may run on the render task, so the store orders the SDK's (non-atomic)
// edge fields it just wrote before the flag, and the load orders the reads of them after it.
static std::atomic<uint32_t> g_driveInputPollMs{0};
static constexpr uint32_t kDriveInputPollMaxAgeMs = 400;

// B-P2-2: the task that OWNS the input poll model (the loop). The panel shim fires the poll hook
// from inside show(), i.e. on whichever task called it, and on the async paint path that is the
// render task. A hook callback is a plain C function pointer, so the owner is a file-scope handle
// rather than a lambda capture.
static TaskHandle_t g_inputPollOwnerTask = nullptr;

void loop() {
  // TAP1 (r99.1): fire the haptic a UI tap stashed on the PREVIOUS iteration.
  //
  // The pulse is a blocking busy-wait (60-120 ms by profile), so firing it inside
  // UiAppHost::route() — which runs AFTER app.route() has dispatched the action and
  // issued its requestUpdate() — held the main task in the motor loop before the
  // render task could act on that request. Result on glass: the buzz was felt, then
  // the screen changed. Here the previous iteration's frame work has been issued and
  // the trailing loop delay has already elapsed, so the render task has had the whole
  // iteration to compose/drive. Exactly once, coalesced (see serviceDeferredHaptic),
  // and unconditional: no early return above it can strand a pulse on a screen that
  // has gone away.
  UiAppHost::serviceDeferredHaptic();

  // r96.3 (D2): close the resume drive window. This is the one place the summary can be
  // written: it runs on the main task every iteration, so it lands even if the screen that
  // followed the clean never asks for another paint.
  if (g_wakeCapture && static_cast<long>(millis() - g_wakeCaptureDeadlineMs) >= 0) {
    g_wakeCapture = false;
    LOG_INF("WAKE", "resume drives=%lu window=%d ms band=%s", static_cast<unsigned long>(g_wakeDriveN),
            CP_WAKE_CAPTURE_MS, g_wakeDriveN <= 1 ? "single-drive: rail settling or first-pass waveform"
                                                 : "extra drive(s): still something driving after the clean");
  }
#if CP_USB_SHELL
  // THE DRAIN POINT — the whole thread-safety contract rests on this being the
  // only other context that runs shell commands: main task, before
  // activityManager.loop(), so a request is serialized against every gesture,
  // page turn and sleep decision. Placed before the exclusive-storage return
  // below so commands still run in drive mode (the commands themselves refuse
  // what they cannot do there, e.g. `shot` on a detached volume).
  usbshell::drain();
  // `reboot` armed a deadline one drain ago; by now its response is on the wire.
  if (g_shellRebootAtMs != 0 && static_cast<long>(millis() - g_shellRebootAtMs) >= 0) {
    g_shellRebootAtMs = 0;
    usbshell::stop();  // release the console before the reset
    LOG_INF("MAIN", "USB shell: rebooting (1 s warning elapsed)");
    silentRestart();
  }
#if CP_LP_BENCH
  // r96: a `lpsleep`/LP-sleep-row request is armed 0.8 s out so its answer left
  // the wire first. Runs HERE, on the main task, serialized against everything.
  lpcore::pollSleepSlice();
#endif
#endif
  // BENCH1 (`benchdrives`): the scripted transition harness. Main task, before
  // activityManager.loop(), so the one transition it issues per pass is processed by the
  // manager exactly like a gesture would be -- never from the shell task, never from the
  // render task. Idle cost is one compare; it is silent and inert until armed, and it is
  // armed only while debug mode is on.
  benchdrives::loop();
  // Heartbeat: proves the event loop is running and how often. If this stops,
  // the UI is frozen; if it repeats but input is ignored, the input path is dead.
  {
    static uint32_t n = 0;
    if (n < 5 || (n % 200) == 0) {
      printf("[CP] loop tick #%lu t=%lu ms\n", (unsigned long)n, (unsigned long)(millis()));
      fflush(stdout);
    }
    ++n;
  }
  // Power-off confirmed in the dialog: the frame is already on the glass, so run the shutdown
  // before any other work. It never returns.
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  if (metalioPowerOffRequested()) {
    metalioHardPowerOff("power key confirmed in dialog");
  }
#endif
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
  // Applies an NTP reply that arrived after the settings screen stopped waiting for it. No-op when
  // no sync is pending.
  halClock.serviceNtpSync();
  // r97.1 BATT3: keep the battery caches fresh on a TIMER, not on the UI's repaint cadence.
  // Both getters gate their own I2C on BATTERY_POLL_MS (1.5 s), so this is one gauge read and
  // one charger read per 1.5 s of awake time -- measured cause of `ui 96% age=26717 ms` was
  // that nothing called them while the status line sat unpainted. Main-task context, so it
  // cannot interleave with the touch/IMU transfers that share this bus.
  powerManager.pollBattery();
  // r163 E4: the charger control loop (src/system/ChargerPolicy.h). Same main-loop place and
  // same reasoning as the battery poll above: this must not run on the UI thread's paint path
  // or on a part-owned task, and it must be able to interleave with nothing but itself. The
  // tick gates ITSELF (2 s below the cut-off, 10 s at/above it), so calling it every pass is
  // one `millis()` compare on the overwhelming majority of passes. It writes EN_TERM / a
  // recharge kick and nothing else -- UVLO is detected and journaled but NOT acted on, because
  // REG0x18 is one-way in software (docs/engineering/R163_ITEM_E4.md §6). It journals every
  // write, and is compiled out entirely when CP_CHARGER_CONTROL is 0 (ChargerCurrent.h states
  // what that restores).
  chargercontrol::tick(millis());
  // Replays the previous light-sleep trace at 10/30/60 s: USB-JTAG misses the first console output
  // while it enumerates, and that output is the only record of how the last sleep ended. Cheap
  // no-op once all three replays have fired.
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  metalio_sleep_report_reset();
#endif
  // Sample input at both edges of every blocking waveform so a touch that arrives while the panel
  // refreshes is not lost. Registered once, on the first loop iteration.
  {
    static bool pollHookSet = false;
    if (!pollHookSet) {
      pollHookSet = true;
      // B-P2-2: this runs on the loop task by construction (first loop iteration), and that is the
      // task the hook's input sample is allowed to run on.
      g_inputPollOwnerTask = xTaskGetCurrentTaskHandle();
      metalio_vendor_panel_set_poll_hook([] {
        // Called at both edges of every blocking waveform, strictly alternating. Only the ENTRY ->
        // EXIT pair is a blind window; measuring to the previous call instead reported the idle gap
        // between two refreshes as a 36-second "blind window".
        static bool insideRefresh = false;
        static uint32_t entryMs = 0;
        [[maybe_unused]] bool exitEdge = false;  // read only by the once-only off-owner log below
        const uint32_t now = millis();
        if (!insideRefresh) {
          insideRefresh = true;
          entryMs = now;
        } else {
          insideRefresh = false;
          exitEdge = true;
          const uint32_t blind = now - entryMs;
          // Always recorded (not just when long): this is the `panel` command's
          // "last drive ms", the number every timing round has had to guess at.
          usbshell::notePanelDriveMs(blind);
          if (blind > 50) {
            LOG_DBG("TOUCH", "blind window %lums (input polled at both edges)",
                    static_cast<unsigned long>(blind));
          }
        }
        // B-P2-2: the shim fires this hook inside show(), i.e. on whichever task called it. On the
        // async paint path that is the RENDER task, while the loop task is still polling the same
        // InputManager and the same FT6336u: two tasks rewriting one debounce/edge/touch state, and
        // a second I2C reader on the shared bus (the bus mutex makes each transfer atomic, not the
        // state machine consistent). The hook exists to cover a drive that PARKS the loop; an async
        // paint does not park it -- that is the point of displayBufferAsync() -- so a sample there
        // buys nothing and races the real one. The owner is the task that registered the hook.
        if (xTaskGetCurrentTaskHandle() != g_inputPollOwnerTask) {
          static bool notedOffTask = false;
          if (!notedOffTask) {
            notedOffTask = true;
            LOG_INF("TOUCH", "paint poll hook on task '%s' (%s): input sample skipped, async paint "
                             "does not park the loop",
                    pcTaskGetName(nullptr), exitEdge ? "exit" : "entry");
          }
          return;
        }
        mappedInputManager.update();
        // B-P1-3: hand the sample we just took to the loop instead of letting it take one that would
        // clear these edges. Gated by the kill-switch (Settings > Controls, default ON).
        if (SETTINGS.inputDuringRefresh) {
          g_driveInputPollMs.store(now, std::memory_order_release);
        }
      });
    }
  }
  // B-P1-3: skip exactly one poll when a drive edge already sampled this iteration, so the events it
  // latched survive to the activities instead of being cleared here (update() clears on entry).
  {
    const uint32_t drivePollMs = g_driveInputPollMs.exchange(0, std::memory_order_acquire);
    if (drivePollMs == 0 || millis() - drivePollMs > kDriveInputPollMaxAgeMs) {
      mappedInputManager.update();
    }
  }

  // r97 BATT2: integrate awake/sleep/charge time. Throttled internally to one
  // gauge sample per BatteryStats::SAMPLE_MS, so this is a millis() compare on
  // the passes that do nothing.
  batteryStats.update();

  // Bluetooth page turner, before the USB drive's early return: a card handed to USB stops the radio
  // before anything else.
  bleturner::Scene scene{};
  // Home counts only for builds whose radio may start there (the S3 profile, RadioPolicy.h).
  const bool homeShown = activityManager.isForegroundHomeShown();
  const bool remoteContext = activityManager.isRemoteReaderContext();
  scene.where = remoteContext ? bleturner::Where::Reader
                : homeShown   ? bleturner::Where::Home
                              : bleturner::Where::Elsewhere;
  scene.visit = activityManager.visit();
  scene.pageShown = homeShown || remoteContext;
  scene.storageBusy = activityManager.requiresExclusiveStorageLoop();
  scene.wifiOn = WiFi.getMode() != WIFI_MODE_NULL;
  const bool remoteActed = bleturner::tick(scene);

  if (activityManager.requiresExclusiveStorageLoop()) {
    // USB Drive handed the raw SD card to the host. Do not run screenshots,
    // sleep, shortcuts, or normal navigation while its filesystem is detached.
    activityManager.loop();
    if (activityManager.preventAutoSleep()) {
      powerManager.setPowerSaving(false);
      delay(10);
    } else {
      // No host is active, so a slower loop is safe. The activity itself times
      // out the raw-storage handoff rather than entering deep sleep detached.
      powerManager.setPowerSaving(true);
      delay(50);
    }
    return;
  }

  // After the exclusive-storage return: it saves settings to the SD card.
  handleSlideSwitch();
  // A finger on the glass must not also page: the tilt detector is told about
  // the live contact so it can drop a gesture in progress.
  float tiltTouchX = 0.0f, tiltTouchY = 0.0f;
  const bool tiltTouchHeld = gpio.isTouchHeldAt(tiltTouchX, tiltTouchY);
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity(),
                       tiltTouchHeld);

  // Under the render lock, so a step never overlaps the themes' gauge reads.
  //
  // r98.2 gaugefix:: drives the whole BQ27220 capacity load in OUR tree, and is the only thing
  // that touches Data Memory. It replaces BatteryMonitor::loadDesignCapacity() because that
  // machine cannot clear a pre-existing CONFIG UPDATE: it sends EXIT_CFG_UPDATE (0x0091) while the
  // gauge is still sealed, the gauge refuses it, and the machine then SEALS (0x0030) and reports
  // done -- leaving CFGUPDATE latched forever, which is what froze 0x10 and made DM reads
  // incoherent on r98.1. r98.2 unseals first, exits with reinit, polls until CFGUPDATE reads 0,
  // seals, polls until SEC reads sealed, and logs the final 0x3A on every path (see
  // util/BatteryGaugeFix.h). freeink-sdk is untouched.
  //
  // The sequence is bounded by construction; the deadline below is a backstop that forces the
  // exit+seal lane rather than abandoning the gauge mid-sequence.
  static bool gaugeCapacityPending = true;
  static unsigned long gaugeCapacityStartMs = 0;
  if (gaugeCapacityPending) {
    if (gaugeCapacityStartMs == 0) gaugeCapacityStartMs = millis();
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock()) {
      if (millis() - gaugeCapacityStartMs > 20000) {
        // Still running after 20 s: finish synchronously so the gauge cannot be left open.
        LOG_INF("BATT", "gauge sequence overran 20 s; finishing the exit+seal lane now");
        gaugefix::finishNow();
        gaugeCapacityPending = false;
      } else {
        gaugeCapacityPending = gaugefix::tick();
      }
    }
  }

  renderer.setFadingFix(SETTINGS.fadingFix);

  // The ROM console does not depend on Arduino USB CDC's connection state.
  if ((Serial || FREEINK_LOG_TRANSPORT == FREEINK_LOG_TRANSPORT_ROM_PRINTF) && millis() - lastMemPrint >= 10000) {
    const auto heap = HalMemory::getInternalHeap();
    LOG_INF("MEM", "Free: %zu bytes, Total: %zu bytes, Min Free: %zu bytes, MaxAlloc: %zu bytes", heap.freeBytes,
            heap.totalBytes, heap.minFreeBytes, heap.largestBlockBytes);
#ifdef BOARD_HAS_PSRAM
    const auto psram = HalMemory::getPsramHeap();
    LOG_INF("MEM", "PSRAM: Free: %zu bytes, Total: %zu bytes, Min Free: %zu bytes, MaxAlloc: %zu bytes",
            psram.freeBytes, psram.totalBytes, psram.minFreeBytes, psram.largestBlockBytes);
#endif
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%lu\n", static_cast<unsigned long>(bufferSize));
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  // lastActivityTime is file scope so the standby path can reset it; prime it on the first pass
  // instead of relying on static-initialisation timing.
  if (!lastActivityTimePrimed) {
    lastActivityTime = millis();
    lastActivityTimePrimed = true;
  }
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() || halTiltSensor.hadActivity() ||
      remoteActed || activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  // Let wake continue as soon as its hold has been verified. The release can
  // arrive after setup, so consume that one input frame rather than making it
  // a page turn, refresh, or other short power-button action.
  if (wakePowerReleasePending && !gpio.isPressed(HalGPIO::BTN_POWER)) {
    wakePowerReleasePending = false;
    lastActivityTime = millis();  // the wake press/release is swallowed as an input frame
    return;
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  // Consume the second X4 Pro power-button release so it does not also run a
  // configured short-power action after toggling the frontlight.
  if (handleX4ProFrontlightDoubleClick()) {
    return;
  }

  const bool x4ProDoubleClickPwrLight = BoardConfig::isX4Pro() && SETTINGS.doubleClickPwrLight;

#if FREEINK_CAP_TOUCH
  // A single X4 Pro power click becomes Confirm only after the frontlight
  // double-click window expires without a second click.
  mappedInputManager.setPowerConfirmClickFrame(false);
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PWR_CONFIRM && x4ProDoubleClickPwrLight) {
    if (lastX4ProPowerClickAt != 0 && millis() - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
      lastX4ProPowerClickAt = 0;
      mappedInputManager.setPowerConfirmClickFrame(true);
    }
    // A release held too long to be a double-click candidate (but still within
    // the normal Confirm press duration) never reaches handleX4ProFrontlightDoubleClick's
    // click tracking above, so it needs its own Confirm check here.
    if (mappedInputManager.wasReleased(MappedInputManager::Button::Power) &&
        gpio.getPowerButtonHeldTime() > X4PRO_POWER_CLICK_MAX_HOLD_MS &&
        gpio.getPowerButtonHeldTime() <= SETTINGS.getPowerButtonDuration()) {
      mappedInputManager.setPowerConfirmClickFrame(true);
    }
  }
#endif

  // Same deferral for SLEEP: getPowerButtonDuration() drops to 10ms so a quick
  // tap sleeps the device, which otherwise fires on button-down and never lets
  // a second click land. Sleep only once the double-click window has passed.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP && x4ProDoubleClickPwrLight &&
      lastX4ProPowerClickAt != 0 && millis() - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
    lastX4ProPowerClickAt = 0;
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // r91 R1: while a USB debug session is up the device must not power down on its own.
  // Feeding the inactivity clock keeps the user's policy ARMED rather than suspended:
  // switching the shell off resumes the configured timeout from that moment instead of
  // firing instantly on the time spent debugging.
  if (usbShellHoldsAwake()) lastActivityTime = millis();

  // ---- r163 E6: the need table, and the one question the policy asks ---------
  powerNeedTick();
  // A released HARD need means a session ended (a USB transfer, an install, a
  // charge). The inactivity clock starts again from here, so letting go does not
  // sleep the device the instant the last holder does. A SOFT release moves
  // nothing -- by definition.
  if (powerneed::sys().takeActivityRefresh()) lastActivityTime = millis();

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  // Rate limit for the "a need is holding the device awake" line: one per
  // excursion, cleared as soon as the timer is no longer due.
  static bool sleepBlockedLogged = false;
  if (!usbShellHoldsAwake() && sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    // A hard need is the ONLY thing that can suppress this. It is checked here,
    // before enterDeepSleep(), so the park/resume sequence below is reached by
    // exactly the same code as before -- a need can delay the call, never change
    // what the call does.
    if (powerneed::blocksAutoStandby()) {
      if (!sleepBlockedLogged) {
        sleepBlockedLogged = true;
        LOG_INF("SLP", "auto-sleep due but a hard power need is held -- staying awake (see `needs`)");
      }
    } else {
      LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
      enterDeepSleep(true);
      // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
      return;
    }
  } else {
    sleepBlockedLogged = false;
  }

  // A hold that woke the device must be released before it can count as a new
  // in-app long press. Otherwise a user who keeps holding after wake would put
  // the device straight back to sleep once allowSleepAt expires.
  static bool powerReleasedSinceWake = false;
  if (!gpio.isPressed(HalGPIO::BTN_POWER)) powerReleasedSinceWake = true;

  // r46: the Metalio awake UI classifies the power key on EVERY tick, not only after the sleep
  // threshold has fired. The generic branch below only gets control once the press has already
  // crossed getPowerButtonDuration(), so its own long-hold test could never fire on a direct
  // hold -- r44 recorded the same gesture as "held 405ms, sleeping" four times and
  // "held 6394ms, sleeping" once. Here the two thresholds are independent: POWER still down at
  // METALIO_POWER_OFF_HOLD_MS (3000) asks for power off, and the sleep decision moves to the
  // RELEASE edge, where it is decided from the press duration alone (r100.2 -- it used to be
  // nested inside the "dialog was armed" flag, which only a >= 3000 ms hold could set, so a
  // normal tap matched no branch at all and the device could not be put to sleep).
  bool powerHoldOwnedByBoard = false;
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::MetalioEInk4Plus) {
    powerHoldOwnedByBoard = true;  // this board never uses the generic branch below
    const bool metalioPowerDown = gpio.isPressed(HalGPIO::BTN_POWER);
    // r163 B5: feed the classifier the debounced level FIRST, so every branch below reads the
    // same view of one press. `wasTracked` tells the press-edge code whether this tick actually
    // opened a new press (observeLevel is idempotent for an unchanged level).
    const bool wasTracked = metalioPowerGesture.pressDown();
    const unsigned long metalioNowMs = millis();
    metalioPowerGesture.observeLevel(metalioNowMs, metalioPowerDown);
    // IS THE DIALOG UP? Ask the manager, do not remember. The old code kept a bool set at push
    // time and cleared in the dialog's onExit() -- but a queued PowerOffActivity can be
    // DESTROYED BEFORE IT EVER ENTERED (pushActivity and popActivity both call
    // pendingActivity.reset() without onExit, and a competing navigation overwrites
    // pendingAction), so the flag could stay true for the rest of the session.
    //
    // r100.3: a QUEUED push only counts for METALIO_POWER_PUSH_GRACE_MS. Counting it for ever is
    // the shape OPEN_CONCERNS #16 has already been bitten by twice: the transition loop is the
    // only thing that can clear a queued action, and it can return before entering a push (the
    // page-turner gate at the head of the loop, ActivityManager.cpp, defers any transition while
    // bleturner::beforeScreenChange() is false) or have the action overwritten by a competing
    // goHome(). While that lasts, this branch runs on every pass and clears the press tracker, so
    // the bounded retries below -- which require metalioPowerHoldActive -- can never fire: the
    // power key goes quiet for power-off with no log line at all. Past the grace the stale push is
    // simply not "up" any more, so the press is classified again and the push it issues replaces
    // the stale one. In the normal case the push is entered in the same loop pass, so the grace
    // never bites and this is behaviour-preserving.
    const bool metalioDialogEntered = activityManager.enteredActivityNamed("PowerOff");
    const bool metalioDialogQueued = activityManager.pendingPushNamed("PowerOff");
    const uint32_t metalioDialogQueuedMs = metalioDialogQueued ? activityManager.pendingPushAgeMs() : 0;
    const bool metalioDialogUp =
        metalioDialogEntered || (metalioDialogQueued && metalioDialogQueuedMs < METALIO_POWER_PUSH_GRACE_MS);

    if (metalioDialogUp) {
      // While it is up the dialog owns the key: no re-fire every 3 s, and no release-edge sleep
      // for the press that opened it. Stop tracking the press, so the release that closes the
      // dialog cannot later be read as a fresh short press either.
      if (metalioPowerDown) {
        if (!metalioPowerIgnoredLogged) {
          // One line per ignored press. This branch used to be silent, which is exactly why "the
          // key does nothing on this screen" had no evidence attached to it.
          metalioPowerIgnoredLogged = true;
          LOG_INF("MAIN",
                  "power key DOWN ignored: PowerOff owns the key (entered=%d queued=%d queuedAge=%lums "
                  "grace=%lums)",
                  metalioDialogEntered ? 1 : 0, metalioDialogQueued ? 1 : 0,
                  static_cast<unsigned long>(metalioDialogQueuedMs),
                  static_cast<unsigned long>(METALIO_POWER_PUSH_GRACE_MS));
        }
      } else {
        metalioPowerIgnoredLogged = false;  // key is up: the next press logs once more
      }
      metalioPowerHoldActive = false;
      metalioPowerDialogArmedAt = 0;
      metalioDialogPushCount = 0;
      metalioPowerGesture.resetPress(metalioNowMs);
    } else if (metalioPowerDown) {
      // Level is DOWN. observeLevel() already cancelled any pending release stamp: a level that
      // came back within the settle window was a failed expander read, not a release, and the
      // SAME press (with its original DOWN stamp) continues.
      if (!wasTracked) {
        // PRESS EDGE. The clock is the classifier's and it is monotonic for the whole press: it
        // is never restarted while the finger stays down. It is deliberately NOT the input
        // manager's powerButtonPressStart, which is re-stamped by every new BTN_POWER press edge
        // -- an expander read that comes back as "all released" (readButtons() reports 0xFFFF on
        // a failed read) ends the press, and the read after it starts a new one, so a 3 s hold
        // never measures 3 s and the dialog never fires. Every line below prints BOTH clocks, so
        // the log says which of those two shapes this unit has.
        metalioPowerHoldActive = false;  // this press has not opened the dialog
        // r100.4: stamp the DOWN edge for the paint-gap line the dialog logs at its first paint.
        metalioPowerNoteDownEdge(metalioPowerGesture.pressStartMs());
        LOG_INF("MAIN", "power key DOWN (input clock %lums, hold threshold %lums)",
                gpio.getPowerButtonHeldTime(), METALIO_POWER_OFF_HOLD_MS);
      }
      const unsigned long metalioHeldMs = metalioPowerGesture.heldMs(metalioNowMs);
      if (metalioPowerHoldActive) {
        // The push was accepted but the dialog never became the current screen (a queued
        // navigation replaced it). One bounded retry while the key is still down, because the
        // alternative is a press that visibly did nothing.
        if (metalioDialogPushCount > 0 && metalioDialogPushCount < 2 && metalioPowerDialogArmedAt != 0 &&
            millis() - metalioPowerDialogArmedAt > 1500) {
          ++metalioDialogPushCount;
          metalioPowerDialogArmedAt = millis();
          LOG_INF("MAIN", "power decision: dialog RETRY #%u (held=%lums, threshold=%lums)",
                  (unsigned)metalioDialogPushCount, metalioHeldMs, METALIO_POWER_OFF_HOLD_MS);
          activityManager.pushActivity(std::make_unique<PowerOffActivity>(renderer, mappedInputManager));
        }
      } else if (metalioHeldMs >= METALIO_POWER_OFF_HOLD_MS && millis() - metalioLastPushMs >= 500) {
        // Ask first. The dialog paints, and main()'s loop head runs the unpainted shutdown once
        // Confirm has raised the request -- this branch never powers off on its own.
        metalioPowerHoldActive = true;  // the only thing this flag now means: this press opened it
        metalioDialogPushCount = 1;
        metalioPowerDialogArmedAt = millis();
        // Name the screen the decision was taken ON. "Works from Settings, not from Home" is a
        // question about this field, so the line has to carry it.
        const char* metalioFromStack[4] = {nullptr, nullptr, nullptr, nullptr};
        const int metalioStackN = activityManager.activityStackNames(metalioFromStack, 4);
        LOG_INF("MAIN",
                "power decision: DIALOG (held=%lums, threshold=%lums, one per press) screen=%s stack=%d",
                metalioHeldMs, METALIO_POWER_OFF_HOLD_MS, metalioStackN > 0 ? metalioFromStack[0] : "-",
                metalioStackN);
        // r100.4: the tick DIALOG was decided. PowerOffActivity logs `painted at` against the same
        // DOWN stamp, so the gap between the decision and the frame on the glass is one subtraction
        // in the log rather than an argument about how long the hold felt.
        metalioPowerNoteDialogDecision(millis());
        activityManager.pushActivity(std::make_unique<PowerOffActivity>(renderer, mappedInputManager));
        metalioLastPushMs = millis();
        // THE PRESS THAT OPENED THE DIALOG OWNS ITS OWN RELEASE. The push happens while the key is
        // still down (that is what "held 3000 ms" means), so the release that ends this press
        // arrives with PowerOffActivity already the current screen. Which consumer of that edge
        // gets it first is pure timing: the manager's global gesture handlers run before the
        // dialog's loop(), the press may be folded into a remapped front-button predicate
        // (MappedInputManager::mapButton reads SETTINGS.frontButtonBack/Confirm, both user
        // settable), and on touch boards a contact released across the transition is a separate
        // edge again. Any of those closing the dialog is what makes this intermittent -- it
        // depends on where in the transition the finger comes up, not on the gesture.
        //
        // Arming the suppression HERE removes the dependence: the bit lives in MappedInputManager
        // until that release is actually observed, and the first statement of
        // ActivityManager::loop() consumes it, so neither a screen nor a global handler can see the
        // edge, whatever order the passes run in. PowerOffActivity's input arming is the second
        // half (it also covers the touch release, which is not a button).
        if (gpio.isPressed(HalGPIO::BTN_POWER))
          mappedInputManager.swallowNextRelease(MappedInputManager::Button::Power);
      }
    } else {
      // Level is UP. A release is only believed after METALIO_POWER_RELEASE_SETTLE_MS of
      // continuous UP, so one failed expander read cannot chop a real hold in half.
      const bool metalioReleased = metalioPowerGesture.releaseSettled(metalioNowMs);
      if (metalioReleased) {
        // RELEASE EDGE (settled). A short press becomes sleep HERE, and this decision is NOT
        // gated on the dialog having been armed: gating it on a flag that only a >= 3000 ms hold
        // could set is exactly why a normal short press stopped sleeping the device at all.
        // Duration is measured to the FIRST up tick (the real release instant), not to the
        // settled one, so the settle window can never push a press over the sleep/power-off bound.
        //
        // r163 B5: both stamps are read BEFORE poll() consumes the press, because the six guards
        // below are unchanged and two of them are defined in terms of those stamps.
        const unsigned long metalioPressStartMs = metalioPowerGesture.pressStartMs();
        const unsigned long metalioHeldMs = metalioPowerGesture.releaseCandidateMs() - metalioPressStartMs;
        const powergesture::Verdict metalioGesture = metalioPowerGesture.poll(metalioNowMs);
        const unsigned long metalioInputMs = gpio.getPowerButtonHeldTime();
        const bool metalioOpenedDialog = metalioPowerHoldActive;
        const unsigned long powerDuration = SETTINGS.getPowerButtonDuration();
        if (metalioOpenedDialog) {
          LOG_INF("MAIN", "power key UP after %lums (input clock %lums): the dialog owned this press",
                  metalioHeldMs, metalioInputMs);
          // The press that opened the dialog must never also sleep. If the push never became
          // the current screen while the key was down, try once now -- a dialog that appears on
          // release still beats a hold that did nothing.
          if (metalioDialogPushCount > 0 && metalioDialogPushCount < 2) {
            ++metalioDialogPushCount;
            LOG_INF("MAIN", "power decision: dialog RETRY on release #%u (held=%lums)",
                    (unsigned)metalioDialogPushCount, metalioHeldMs);
            activityManager.pushActivity(std::make_unique<PowerOffActivity>(renderer, mappedInputManager));
          }
        } else {
          // allowSleepAt is judged at the moment the PRESS STARTED, not at release: a press that
          // began inside the post-wake/post-boot window is the press that woke the device, however
          // long the finger then stays on the key.
          // r162 PWR4: with the Metalio default now SLEEP, `powerDuration` is 10 ms and this
          // release edge is where a CLICK becomes sleep. Nothing about the guards changed:
          // the press that woke the device is still excluded by powerReleasedSinceWake, a
          // press that started before allowSleepAt still cannot sleep, a press at/over
          // METALIO_POWER_OFF_HOLD_MS was already classified as "the dialog owned it" above,
          // and BTN_DOWN still suppresses the sleep so the screenshot combo survives. The
          // line below now carries shortPwrBtn so a log can tell a 10 ms threshold from a
          // 400 ms one without reading the settings file.
          //
          // r163 B5: the classifier's own verdict is an EXTRA, last condition on the same
          // expression. It can only subtract a sleep the six guards would have allowed, and the
          // one case it subtracts is `Discarded`: a press whose DOWN edge predates this boot
          // epoch (a silent restart landed while the key was held). That press never had a
          // witnessed click, so it must be released rather than read as one.
          const bool metalioSleepAllowed = metalioPressStartMs >= allowSleepAt;
          const bool metalioShouldSleep =
              metalioHeldMs > powerDuration && metalioHeldMs < METALIO_POWER_OFF_HOLD_MS &&
              powerReleasedSinceWake && metalioSleepAllowed && !gpio.isPressed(HalGPIO::BTN_DOWN) &&
              (metalioGesture.kind == powergesture::Kind::Click || metalioGesture.kind == powergesture::Kind::MultiClick);
          LOG_INF("MAIN",
                  "power key UP after %lums (input clock %lums): %s held>%lu=%d held<%lu=%d woke=%d "
                  "allowSleep=%d downKey=%d shortPwrBtn=%u gesture=%s clicks=%u",
                  metalioHeldMs, metalioInputMs, metalioShouldSleep ? "SLEEP" : "no action",
                  static_cast<unsigned long>(powerDuration), metalioHeldMs > powerDuration ? 1 : 0,
                  METALIO_POWER_OFF_HOLD_MS, metalioHeldMs < METALIO_POWER_OFF_HOLD_MS ? 1 : 0,
                  powerReleasedSinceWake ? 1 : 0, metalioSleepAllowed ? 1 : 0,
                  gpio.isPressed(HalGPIO::BTN_DOWN) ? 1 : 0, (unsigned)SETTINGS.shortPwrBtn,
                  powergesture::kindName(metalioGesture.kind), (unsigned)metalioGesture.clicks);
          if (metalioShouldSleep) {
            enterDeepSleep();
            return;  // this board's sleep does not return until the user wakes it
          }
        }
        metalioPowerHoldActive = false;
        metalioPowerDialogArmedAt = 0;
        metalioDialogPushCount = 0;
      }
    }
  }
#endif

  // On the Metalio board the hold above already decided (and took) the action, so the generic
  // press-time branch must not fire a second time for the same gesture.
  const bool x4ProAwaitingClickWindow = x4ProDoubleClickPwrLight &&
                                        SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP &&
                                        gpio.getPowerButtonHeldTime() <= X4PRO_POWER_CLICK_MAX_HOLD_MS;

  if (!powerHoldOwnedByBoard && !x4ProAwaitingClickWindow && powerReleasedSinceWake &&
      millis() >= allowSleepAt && gpio.isPressed(HalGPIO::BTN_POWER) &&
      gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    const unsigned long heldMs = gpio.getPowerButtonHeldTime();
    LOG_DBG("MAIN", "Power button held %lums, sleeping", heldMs);
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

#if FREEINK_DEVICE_PAPERMONO
  // Paper Mono reports the PMIC power button as a one-tick click, so the held
  // path above cannot fire. With the default Ignore action, retain the normal
  // power-button meaning and shut down; explicit alternate bindings still win.
  if ((SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP ||
       SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::IGNORE) &&
      millis() >= allowSleepAt && mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    enterDeepSleep();
    return;
  }
#endif

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (mappedInputManager.homeButtonAction() == HomeButtonAction::ToggleFrontlight) {
    toggleFrontlight();
  }
  if (mappedInputManager.homeButtonAction() == HomeButtonAction::Refresh ||
      (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
       mappedInputManager.wasReleased(MappedInputManager::Button::Power))) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    if (!activityManager.handleForcedRefresh()) {
      RenderLock lock;
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  //
  // r165.6 -- ONE tracker for every battery-pin armer below (BATT6's percentage
  // fallback, BATT5's glyph fallback, and the legacy USB-edge repaint).
  //
  // D1 (r165.3) cleared the pins at the NEXT LOOP PASS, which is the wrong unit:
  // requestUpdate() only sets a flag and the paint happens on the RENDER TASK, so
  // the pass after the arm is usually reached before the task composes -- and the
  // boundary then cleared exactly the pair the requested paint was going to
  // consume, leaving the deferred repaint unpinned (a grey-wipe lane instead of
  // the intended binary DU) or, on the other half of the race, working. The pins
  // belong to the next COMPOSE, so this records the renderer's compose epoch at
  // arm time and reconciles against it: a changed epoch means a compose has
  // happened (spent -- clear any remainder it refused), and an unchanged epoch
  // past oneshotpins::kComposeDeadlineMs means the paint never came (drop it
  // before some unrelated screen inherits it). See src/display/OneShotPaintPins.h.
#if CP_BATT_PERCENT_PATCH || CP_ICON_SAFE_PATCH || !CP_ICON_PARTIAL_REPAINT
  static oneshotpins::ComposeBoundPins batteryPaintPins;
  {
    const oneshotpins::PinState pinState =
        batteryPaintPins.reconcile(renderer.framebufferComposeEpoch(), millis());
    if (pinState == oneshotpins::PinState::Consumed || pinState == oneshotpins::PinState::Dropped) {
      // The render task arms these inside its own paints (InPlaceValueStep), so the
      // clear has to hold the lock it composes under.
      RenderLock lock;
      const bool leftover = renderer.oneShotPinsPending();  // nothing left after a compose took them
      if (leftover) renderer.clearOneShotPins();
      if (leftover && pinState == oneshotpins::PinState::Dropped) {
        LOG_INF("BATT", "paint pins dropped: no compose within %u ms of the request",
                static_cast<unsigned>(oneshotpins::kComposeDeadlineMs));
      } else if (leftover) {
        LOG_INF("BATT", "paint pins cleared: the compose that began refused them (entry gate)");
      }
    }
  }
#endif
#if CP_BATT_PERCENT_PATCH
  // BATT6: the percentage NUMBER's producer, BEFORE the glyph's.
  //
  // It compares the gauge's cached percentage with the value the last paint left in
  // the number's record, and repaints exactly that box -- plus the glyph, whose fill
  // level is the same percentage -- in ONE diff-masked DU drive, licensed by the
  // same canvas==glass fact as the glyph patch (lib/GfxRenderer/GlassEpoch.h) and
  // re-checked inside the RenderLock.
  //
  // Order matters: running it first means a plug AND a percentage step in the same
  // pass cost one drive instead of two (this drive redraws the glyph with the live
  // charging state and settles the glyph producer's edge through
  // cpBatteryIconPatchNoteExternalDraw).
  //
  // It never asks for a repaint itself. A number this path cannot address (the
  // FreeInkUI header band) ARMS one request, and it is consumed HERE, at the loop
  // boundary, outside every paint: asking from inside a paint re-enters the
  // activity manager, and r165.1's on-every-pass requests consumed the armed
  // return-to-Home clean. r165.2 arms at most one per (surface, value), at least
  // 10 s apart, and the decision refuses to arm at all while a clean or a
  // transition is in flight.
  //
  // The reader is declined for the same reason the glyph's fallback is: a page
  // re-render for a number is the flash this path exists to remove, and the reader
  // has an addressable box anyway.
  if (!deepSleepInProgress) {
    cpBatteryPercentPatchOnChange(renderer);
    if (cpBatteryPercentTakeDeferred()) {
      if (activityManager.isReaderActivity()) {
        LOG_INF("BATT", "soc repaint declined on a reader (the next page paint draws the live value)");
      } else {
        // r165.6: the pair belongs to the compose the request below earns, not to
        // this pass -- arm is stamped with the epoch it will advance.
        batteryPaintPins.arm(renderer.framebufferComposeEpoch(), millis());
        renderer.forceBinaryDuOnce();
        renderer.suppressCompletionDriveOnce();
        activityManager.requestUpdate();
      }
    }
  }
#endif

#if CP_ICON_SAFE_PATCH
  // BATT5: ONE producer, one drive, on every screen including the reader.
  //
  // The glyph repaints its OWN rectangle, and the licence to do that is not "a rect
  // was recorded" (the r98 inference that produced the corner-only blank screen) but
  // `framebufferIsGlassFrame()`: the renderer's compose epoch equals its glass epoch,
  // i.e. no paint has composed into the canvas since the last drive the panel
  // ACCEPTED (lib/GfxRenderer/GlassEpoch.h). Under that fact every pixel outside the
  // box already equals the glass, so the panel's framebuffer-vs-glass diff IS the box.
  //
  // The drive is pinned to the diff-masked DU with the FLP2 one-shots 58b4d2cc
  // introduced -- the same pair FrontlightPanelActivity uses for its own 1-bit chrome
  // -- so the vendor shim cannot take `gray8-wipe-GL16(38)` (my_waveform_wipe_gl(3):
  // eight phases that drive every pixel to paper, then a repaint of the mask only =
  // whole panel white with just the icon box redrawn, the blank screen) and there is
  // no row-9 whole-panel follow-up. Both epoch terms are re-checked inside the
  // RenderLock, immediately before the first pixel is touched.
  //
  // NeedsFullRepaint is the ONLY case that falls back, and it means "no addressable
  // rect on this screen" (a battery drawn inside the FreeInkUI header), where a
  // whole-activity repaint is the only producer left. On the READER that fallback is
  // declined: a page re-render for a glyph is precisely the flash this removes, and
  // the reader's next page paint draws the live state anyway. Guard and epoch
  // refusals are Pending and cost nothing -- the next loop iteration retries.
  if (!deepSleepInProgress) {
    if (cpBatteryIconPatchOnEdge(renderer) == CpBatteryIconResult::NeedsFullRepaint) {
      if (activityManager.isReaderActivity()) {
        LOG_INF("BATT",
                "icon repaint declined on a reader: a page re-render for a glyph is the flash this path removes "
                "(the next page paint draws the live state)");
      } else {
        // r165.6 (S2): this armer used to pin untracked, so a skipped paint left a
        // DU-with-no-completion pair for whatever painted next. Same tracker, same rule.
        batteryPaintPins.arm(renderer.framebufferComposeEpoch(), millis());
        renderer.forceBinaryDuOnce();
        renderer.suppressCompletionDriveOnce();
        activityManager.requestUpdate();
      }
    }
  } else {
    // BATT3: this flag must only be true while the device is actually on its way to sleep.
    // Standby on this board RETURNS (main.cpp enterDeepSleep -> metalioStandbySleep), so a
    // flag left set here silences the icon poll for the rest of the session; the resume now
    // clears it, and this line is what makes a future regression of that visible instead of
    // silent.
    static bool sleepFlagLogged = false;
    if (!sleepFlagLogged) {
      sleepFlagLogged = true;
      LOG_INF("BATT", "icon poll suspended: deepSleepInProgress=1 (sleep in progress, no icon repaint)");
    }
  }
#elif CP_ICON_PARTIAL_REPAINT
  // r98 CP_ICON_PARTIAL_REPAINT: the icon repaints its OWN rectangle on the
  // isCharging() edge below, so this whole-activity repaint is now only the
  // FALLBACK for the screens the partial path cannot serve -- ones whose battery
  // glyph is not painted through BaseTheme::drawBatteryOutline() (the fui header is
  // drawn inside freeink-sdk), where cpBatteryIconVisibleOnScreen() is false and
  // there is no rectangle to repaint. Asking for both would spend two drives on one
  // transition.
  //
  // BATT4: the reader used to be excluded here UNCONDITIONALLY (`!isReaderActivity()`),
  // on the assumption that the partial path always covers it. It does not: that path
  // returns NeedsFullRepaint when the last paint recorded no icon rect, and with the
  // reader excluded from this line the transition then had NO producer at all -- the
  // icon could never move on a reader screen whose glyph this tree cannot address.
  // The exclusion is gone; the poll below decides, and this line is the belt-and-braces
  // path for the GPIO edge.
  if (gpio.wasUsbStateChanged() && !cpBatteryIconVisibleOnScreen()) {
    activityManager.requestUpdate();
  }
  // THE READER IS COVERED HERE TOO, which is the case the operator reported: the
  // charging flag (CX25601N REG 0x1E via HalPowerManager::isCharging(), cached for
  // BATTERY_POLL_MS) is EDGE-detected once per loop iteration, so a steady state
  // drives nothing and the 1.5 s poll is never fought. On a transition only the
  // icon's rectangle is repainted, in DU, with the panel's own diff mask limiting
  // the waveform -- and the call is skipped (never forced) while sleeping, while a
  // paint is in flight, or when the icon is not on the visible screen.
  if (!deepSleepInProgress) {
    // BATT4: one owner per transition. Drove/Pending/Idle need nothing more; only a
    // screen this path has no rectangle for asks for the r97 whole-activity repaint,
    // and only on an edge (so it is one full paint per plug/unplug, never periodic).
    if (cpBatteryIconPollAndRepaint(renderer) == CpBatteryIconResult::NeedsFullRepaint) {
      activityManager.requestUpdate();
    }
  } else {
    // BATT3: this flag must only be true while the device is actually on its way to sleep.
    // Standby on this board RETURNS (main.cpp enterDeepSleep -> metalioStandbySleep), so a
    // flag left set here silences the icon poll for the rest of the session; the resume now
    // clears it, and this line is what makes a future regression of that visible instead of
    // silent.
    static bool sleepFlagLogged = false;
    if (!sleepFlagLogged) {
      sleepFlagLogged = true;
      LOG_INF("BATT", "icon poll suspended: deepSleepInProgress=1 (sleep in progress, no icon repaint)");
    }
  }
#else
  // BATT5 (the white flash on plug/unplug). This repaint asked for nothing of its own, so it
  // inherited the screen's mode -- Home asks FAST (HomeActivity.cpp:704 and :851) -- and the
  // vendor shim turns a FAST request over a frame that carries AA grey into the grey-wipe lane:
  // metalio_vendor_panel.cpp:742 `useGray8` -> :846 `greyDrive` -> :1032
  // epd_hl_update_screen_full(MODE_GL16) with my_waveform_wipe_gl(3), whose 8-phase erase prefix
  // drives EVERY pixel to paper before the 30-phase table repaints the identical screen. That is
  // the flash: the panel cleans to white, then repaints, for a glyph that is a few pixels wide.
  //
  // The frame this paint composes is the same screen the glass already holds, so it is pinned to
  // a diff-masked DU for exactly this one paint -- the same FLP2 pairing FrontlightPanelActivity
  // uses for its own 1-bit chrome. `forceBinaryDuOnce()` is read AFTER the policy stage, so
  // neither an armed AUTO nor an explicit FULL/GL16 from the screen's own requestUpdate() can
  // escalate it, and it also exempts the paint from the panel's 24-partial and UI-grey from-white
  // cadences; the panel's own back_fb diff then decides the driven area, which is the glyph's
  // rectangle and nothing else. The row-9 GL16 follow-up is dropped with it: it would be a second
  // whole-panel drive for a frame that is already on the glass.
  //
  // NOT a region patch, and that is the point. The screen is still composed in FULL and the drive
  // still covers every pixel that differs from the glass, so there is no framebuffer that can
  // disagree with the panel about what is on it -- the failure mode that retired
  // CP_ICON_PARTIAL_REPAINT is structurally absent here. The glyph is 1-bit by role
  // (BaseTheme::drawBatteryOutline, ContentRole::Icon), so DU renders it as the grey lane did.
  if (gpio.wasUsbStateChanged() && !activityManager.isReaderActivity()) {
    // r165.6: tracked like the other two armers -- same request-then-compose gap.
    batteryPaintPins.arm(renderer.framebufferComposeEpoch(), millis());
    renderer.forceBinaryDuOnce();
    renderer.suppressCompletionDriveOnce();
    activityManager.requestUpdate();
  }
#endif

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  // r99.5 DRIVE5: the activity can ARM INSIDE loop() -- UsbMscActivity's cable-appear poll does
  // exactly that -- while the two exclusive-storage predicates at the top of this function were
  // read BEFORE the call. A stale decision must not reach the idle tail below, which drops the
  // CPU/PLL frequency (and runs the SD flush) with the gadget already live; that tail is what
  // r99.4 applied 1 ms after "gadget started". Re-read the gate here instead of one iteration
  // later.
  if (activityManager.requiresExclusiveStorageLoop()) return;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  bool skipLoopDelay = false;
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) {
      // Let rendering advance without treating lock contention as idle.
      delay(10);
      return;
    }
    skipLoopDelay = activityManager.skipLoopDelay();
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  // LOG2: drain the SD capture ring from the loop itself, EVERY iteration, before the power branch.
  // It used to sit inside the 3-second-idle branch only, which a live debug session never reaches
  // (the shell holds the device awake, and every read/gesture refreshes lastActivityTime): the ring
  // filled, the card log stopped advancing and `logs` served the same window forever. The function
  // is a cheap size/age test and writes at most once per 5 s, so this costs nothing in the hot path.
  logSdAutoFlush();
  if (skipLoopDelay) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (usbShellHoldsAwake()) {
      // r91 R1: no low-power entry while a debug session is up -- and leave it if the
      // device was already at the reduced clock when the shell came on. delay(10), not
      // the 50 ms idle sleep, so the loop stays as responsive as a reader screen.
      powerManager.setPowerSaving(false);
      lastActivityTime = millis();
      delay(10);
    } else if (!activityManager.preventAutoSleep() &&
               millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS && !pageturner::holdsCpu.load()) {
      // If we've been inactive for a while, increase the delay to save power.
      // r99.5 DRIVE5: and only when no activity is holding the device awake. The predicate is
      // re-read HERE, immediately before the call, not only at the top of the iteration: an
      // activity that armed during activityManager.loop() (USB drive mode by cable-appear) must
      // never have the clock dropped out from under it.
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      // Sleep in short slices and wake the poll as soon as a button contact closes.
      // InputManager commits a press only when two consecutive polls agree, so a
      // press shorter than one 50 ms sleep could land in a single sample and be lost.
      const unsigned long idleStart = millis();
      while (millis() - idleStart < 50) {
        delay(10);
        if (gpio.rawInputActive()) break;
      }
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
