#include "AboutActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalFrontlight.h>
#include <HalTiltSensor.h>
#include <MetalioEInk4PlusBoard.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <sdkconfig.h>

#include <cstdio>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "display/PanelIdentity.h"

namespace fui = freeink::ui;

namespace {
enum MenuItem {
  ITEM_DEVICE = 0,
  ITEM_FIRMWARE,
  ITEM_BUILD,
  ITEM_CHIP,
  ITEM_FLASH,
  ITEM_PSRAM,
  ITEM_DISPLAY,
  ITEM_PMIC,
  ITEM_RESOLUTION,
  ITEM_TOUCH,
  ITEM_FRONTLIGHT,
  ITEM_RTC,
  ITEM_IMU,
  ITEM_GAUGE,
  ITEM_MAC,
};

// Deliberately hardcoded English, exempt from the tr() rule: support reads
// these screenshots across every device language, so the labels must be
// identical on every unit.
// Order is the enum above, one label per ITEM_* -- keep the two lists in step.
const char* const menuNames[AboutActivity::ITEM_COUNT] = {
    "Device", "Firmware", "Build", "Chip", "Flash", "PSRAM", "Display Controller", "Panel PMIC",
    "Resolution", "Touch", "Frontlight", "RTC", "Tilt Sensor (IMU)", "Fuel Gauge", "MAC Address",
};

// Chip part numbers, not user prose - deliberately untranslated.
const char* displayControllerName(const BoardConfig::DisplayController c) {
  switch (c) {
    case BoardConfig::DisplayController::SSD1677:
      return "SSD1677";
    case BoardConfig::DisplayController::UC8253:
      return "UC8253";
    case BoardConfig::DisplayController::ED2208:
      return "ED2208";
    case BoardConfig::DisplayController::LgfxEpd:
      return "LovyanGFX EPD";
    case BoardConfig::DisplayController::IT8951:
      return "IT8951";
    case BoardConfig::DisplayController::UC8279:
      return "UC8279";
    case BoardConfig::DisplayController::UC8179:
      return "UC8179";
    case BoardConfig::DisplayController::UC8279C:
      return "UC8279C";
    case BoardConfig::DisplayController::Ed047Tc2:
      // The Plus links the 1216x684 variant symbol (display/displays.c:67), not the
      // 960x540 base part, so the row names the variant it actually drives.
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
      return CP_PANEL_CONTROLLER_NAME;
#else
      return "ED047TC2";
#endif
  }
  return "?";
}

const char* touchControllerName(const BoardConfig::TouchController c) {
  switch (c) {
    case BoardConfig::TouchController::None:
      return nullptr;
    case BoardConfig::TouchController::Chsc6x:
      return "CHSC6X";
    case BoardConfig::TouchController::Gt911:
      return "GT911";
    case BoardConfig::TouchController::Ft5x06:
      return "FT5x06";
    case BoardConfig::TouchController::Ft6336u:
      return "FT6336U";
    case BoardConfig::TouchController::Gslx680:
      return "GSLX680";
  }
  return nullptr;
}

// The part names below are switches on the board profile's own enum, so a row can
// never claim a part the profile does not declare: a board with no gauge/IMU/RTC
// prints "none" instead of a stale name.
const char* gaugePartName(const BoardConfig::GaugeType t) {
  switch (t) {
    case BoardConfig::GaugeType::Bq27220:
      return "BQ27220";
    case BoardConfig::GaugeType::Cw2017:
      return "CW2017";
    case BoardConfig::GaugeType::Axp2101:
      return "AXP2101";
  }
  return "unknown";
}

const char* rtcPartName(const BoardConfig::RtcType t) {
  switch (t) {
    case BoardConfig::RtcType::None:
      return nullptr;
    case BoardConfig::RtcType::Pcf8563:
      return "PCF8563";
    case BoardConfig::RtcType::Ds3231:
      return "DS3231";
    case BoardConfig::RtcType::Rx8130:
      return "RX8130";
    case BoardConfig::RtcType::Pcf85063:
      return "PCF85063";
  }
  return "unknown";
}

const char* imuPartName(const BoardConfig::ImuType t) {
  switch (t) {
    case BoardConfig::ImuType::None:
      return nullptr;
    case BoardConfig::ImuType::Lsm6ds3:
      return "LSM6DS3";
    case BoardConfig::ImuType::Qmi8658:
      return "QMI8658";
    case BoardConfig::ImuType::Sc7a20h:
      return "SC7A20H";
  }
  return "unknown";
}

// esp_chip_info() is the runtime truth (model, cores, silicon revision); the
// CONFIG_IDF_TARGET token is the build's own name for the same part and is what
// the logs and the OTA tooling match, so the row carries both.
const char* chipModelName(const esp_chip_model_t model) {
  switch (model) {
#if defined(CHIP_ESP32S31)
    case CHIP_ESP32S31:
      return "ESP32-S31";
#endif
#if defined(CHIP_ESP32S3)
    case CHIP_ESP32S3:
      return "ESP32-S3";
#endif
#if defined(CHIP_ESP32S2)
    case CHIP_ESP32S2:
      return "ESP32-S2";
#endif
#if defined(CHIP_ESP32)
    case CHIP_ESP32:
      return "ESP32";
#endif
    default:
      return CONFIG_IDF_TARGET;
  }
}
}  // namespace

AboutActivity::AboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("About", renderer, mappedInput) {}

const char* AboutActivity::headerTitle() const { return "About"; }

void AboutActivity::onEnter() {
  UiListActivity::onEnter();
  for (int i = 0; i < ITEM_COUNT; i++) {
    rowItems_[i].label = menuNames[i];
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }

  char buf[64];
  // Hardware and firmware information is fixed after boot; fill it once here.
  // BoardConfig::ACTIVE reflects RUNTIME detection: selectDevice() picked the
  // profile and applyXteinkDisplayController() may have promoted the display
  // controller to the panel actually found on the bus.
  rowValues_[ITEM_DEVICE] = BoardConfig::ACTIVE.name;
  rowValues_[ITEM_FIRMWARE] = CROSSPOINT_VERSION;
  // Build stamp: the compiler's own date/time. Nothing else knows when this image
  // was built, and a guessed one would be worse than the toolchain's.
  rowValues_[ITEM_BUILD] = __DATE__ " " __TIME__;

  esp_chip_info_t chip{};
  esp_chip_info(&chip);
  snprintf(buf, sizeof(buf), "%s %uc rev %u.%u", chipModelName(chip.model), static_cast<unsigned>(chip.cores),
           static_cast<unsigned>(chip.revision / 100), static_cast<unsigned>(chip.revision % 100));
  rowValues_[ITEM_CHIP] = buf;

  // Flash and PSRAM come from the driver and the heap, never from a board constant:
  // a part with a different size then reports what it really has.
  // esp_flash_get_size() reports through an out-parameter and can fail; on failure say
  // so rather than print a 0 MB row that reads like a broken part.
  uint32_t flashBytes = 0;
  const bool haveFlash = esp_flash_get_size(nullptr, &flashBytes) == ESP_OK && flashBytes > 0;
  if (haveFlash) {
    snprintf(buf, sizeof(buf), "%u MB", static_cast<unsigned>(flashBytes / (1024u * 1024u)));
    rowValues_[ITEM_FLASH] = buf;
  } else {
    rowValues_[ITEM_FLASH] = "unknown";
  }
  const size_t psramBytes = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  if (psramBytes > 0) {
    snprintf(buf, sizeof(buf), "%u MB", static_cast<unsigned>(psramBytes / (1024u * 1024u)));
    rowValues_[ITEM_PSRAM] = buf;
  } else {
    rowValues_[ITEM_PSRAM] = "none";
  }

  // Controller plus the vendor glass it is driving. The vendor part has no symbol
  // in any header (see display/PanelIdentity.h for why it lives there).
  // r96 D2 (operator: "some of them are too long, so can we wrap them in multiple
  // lines instead? e.g. Display controller is cutoff to Display Con...").
  //
  // The VALUE is drawn on ONE line by the shared list chrome, and making it wrap
  // means changing that chrome for every list on every screen (row height, pitch,
  // paging) - a wide diff, so it was NOT done here. What was done is the operator's
  // own fallback: shorten the SOURCE so the row stops clipping.
  //   Display  before "ED047TC2_1216 E0470A14-AF" (24 ch)  after "ED047TC2_1216" (13 ch)
  //   PMIC     before "TPS65185 0x68/TCA9555"      (20 ch)  after "TPS65185 0x68"  (13 ch)
  // The dropped detail is not lost: both full strings are logged, and the vendor
  // glass part is still in PanelIdentity.h next to the controller symbol.
  rowValues_[ITEM_DISPLAY] = displayControllerName(BoardConfig::ACTIVE.displayController);
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  LOG_INF("TEST", "about display: controller=%s vendor_glass=%s",
          displayControllerName(BoardConfig::ACTIVE.displayController), CP_PANEL_VENDOR_PART);
  // The panel rails come from the TPS65185 through the expander, not from a GPIO.
  // The addresses are the board header's own constants, so this row cannot drift
  // from the bus the driver actually talks to.
  snprintf(buf, sizeof(buf), "%s 0x%02X", CP_PANEL_PMIC_NAME,
           static_cast<unsigned>(freeink::metalio4plus::TPS65185_ADDR));
  rowValues_[ITEM_PMIC] = buf;
  LOG_INF("TEST", "about pmic: %s 0x%02X via expander %s 0x%02X", CP_PANEL_PMIC_NAME,
          static_cast<unsigned>(freeink::metalio4plus::TPS65185_ADDR), CP_PANEL_PMIC_EXPANDER,
          static_cast<unsigned>(freeink::metalio4plus::EXPANDER_ADDR));
#else
  // BoardConfig has no PMIC field: a board with panel rails has nothing to read
  // here, and naming a part would be a guess.
  rowValues_[ITEM_PMIC] = "unknown";
#endif

  snprintf(buf, sizeof(buf), "%ux%u", static_cast<unsigned>(BoardConfig::ACTIVE.displayWidth),
           static_cast<unsigned>(BoardConfig::ACTIVE.displayHeight));
  rowValues_[ITEM_RESOLUTION] = buf;

  const char* touch = touchControllerName(BoardConfig::ACTIVE.touch.controller);
  if (touch != nullptr) {
    if (BoardConfig::ACTIVE.touch.irq != BoardConfig::PIN_UNASSIGNED) {
      snprintf(buf, sizeof(buf), "%s 0x%02X GPIO%d", touch, static_cast<unsigned>(BoardConfig::ACTIVE.touch.i2cAddress),
               static_cast<int>(BoardConfig::ACTIVE.touch.irq));
    } else {
      snprintf(buf, sizeof(buf), "%s 0x%02X", touch, static_cast<unsigned>(BoardConfig::ACTIVE.touch.i2cAddress));
    }
    rowValues_[ITEM_TOUCH] = buf;
  } else {
    rowValues_[ITEM_TOUCH] = "No";
  }

  rowValues_[ITEM_FRONTLIGHT] = Frontlight.present() ? "Yes" : "No";

  // Part plus address, not a bare Yes/No: "which RTC is on the bus at 0x51" is the
  // question a support screenshot has to answer.
  const char* rtc = rtcPartName(BoardConfig::ACTIVE.sensors.rtcType);
  if (rtc != nullptr && BoardConfig::ACTIVE.sensors.rtcAddr != 0 && halClock.isAvailable()) {
    snprintf(buf, sizeof(buf), "%s 0x%02X", rtc, static_cast<unsigned>(BoardConfig::ACTIVE.sensors.rtcAddr));
    rowValues_[ITEM_RTC] = buf;
  } else {
    rowValues_[ITEM_RTC] = "none";
  }

  const char* imu = imuPartName(BoardConfig::ACTIVE.sensors.imuType);
  if (imu != nullptr && BoardConfig::ACTIVE.sensors.imuAddr != 0 && halTiltSensor.isAvailable()) {
    snprintf(buf, sizeof(buf), "%s 0x%02X", imu, static_cast<unsigned>(BoardConfig::ACTIVE.sensors.imuAddr));
    rowValues_[ITEM_IMU] = buf;
  } else {
    rowValues_[ITEM_IMU] = "none";
  }

  // gaugeAddr != 0 is the profile's own "there is an I2C gauge" test
  // (BatteryGaugeConfig, BoardConfig.h); 0 means the board reads an ADC pin.
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    snprintf(buf, sizeof(buf), "%s 0x%02X", gaugePartName(BoardConfig::ACTIVE.batteryGauge.gaugeType),
             static_cast<unsigned>(BoardConfig::ACTIVE.batteryGauge.gaugeAddr));
    rowValues_[ITEM_GAUGE] = buf;
  } else {
    rowValues_[ITEM_GAUGE] = "none (ADC)";
  }

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  rowValues_[ITEM_MAC] = buf;
}

void AboutActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + UITheme::getInstance().getTitledHeaderHeight()), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  for (int i = 0; i < ITEM_COUNT; i++) {
    rowItems_[i].value = rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = ITEM_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}
