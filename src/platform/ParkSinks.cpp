// PARK SINKS — see src/platform/ParkSinks.h for the contract.
//
// Every arm is a RAM bool; a reboot is the reset. The hardware legs are here (not
// in main.cpp) so the register pokes and their read-backs sit next to each other,
// and so the park path in main.cpp stays a sequence of named calls.

#include "platform/ParkSinks.h"

#if CP_PARK_SINKS

#include <cstdio>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
#define PARKSINK_HW 1
#else
#define PARKSINK_HW 0
#endif

#if PARKSINK_HW
#include <esp_err.h>
#include <esp_wifi.h>
#if defined(CONFIG_IDF_TARGET_ESP32S31)
// The two register blocks this file pokes. Both are S31-only:
//   hal/usb_serial_jtag_ll.h — usb_pad_enable / bus clock (the USJ PHY pads)
//   soc/cnnt_io_mux_struct.h — CNNT_PAD_CTRL, the SDMMC block that claims the SD pads
#include <hal/usb_serial_jtag_ll.h>
#include <soc/cnnt_io_mux_struct.h>
#endif
#endif

namespace parksink {
namespace {

// ---- the arms -------------------------------------------------------------
// r106 B0: USB ships DISARMED -- measured on glass, it wedged the CDC endpoint on
// the first park (R103_GLASS_LOG §F3). r118: front and sd now ship DISARMED too.
// With Front disarmed the park calls Frontlight.setOn(false), byte-identical to the
// verified pre-r105 behaviour; with Sd disarmed the SD pads stay claimed, also as
// before r105. The legs and the arms are untouched: `exp park front=1` / `sd=1`
// re-arms either one at runtime, RAM-only, no rebuild. `todayValue` (what r101.21
// did) is "not armed" for all four -- written down here so nobody can flip a
// default and still call the image r101.21.
struct SinkDef {
  const char* name;
  const char* help;
  bool def;
  bool today;
};

const SinkDef kSinks[] = {
    {"usb", "USB-Serial/JTAG PHY pads down in park when VBUS is absent (a cable-attached wake keeps its console); "
            "DEFAULT OFF -- cutting them wedged COM4 on glass (PermissionError 13) and only `exp park usb=1` should re-arm it",
     false, false},
    {"front", "frontlight pads driven LOW and the LEDC channels detached in park; brightness/warmth restored on resume; "
              "DEFAULT OFF since r118 (disarmed = the verified pre-r105 setOn(false) path), `exp park front=1` arms it",
     false, false},
    {"radio", "esp_wifi_deinit() in park (BT controller is not built into this image); re-init on resume; "
              "DEFAULT OFF since the radio-sink demotion -- a sink that can cost the console is not worth a measurement, `exp park radio=1` arms it for one session",
     false, false},
    {"sd", "CNNT SDIO pads released in park (ded_sel=0, pull-downs on); re-claimed before the resume remount; "
           "DEFAULT OFF since r118 (disarmed = the pads stay claimed, as before r105), `exp park sd=1` arms it", false,
     false},
};
static_assert(sizeof(kSinks) / sizeof(kSinks[0]) == static_cast<size_t>(Sink::Count), "one SinkDef per Sink");

// -1 = build default, 0 = forced off, 1 = forced on.
int8_t g_override[static_cast<size_t>(Sink::Count)] = {-1, -1, -1, -1};

// r106: what the hardware legs last did (evidence for the `sd` / `radio` commands).
Verdict g_lastSdRelease = Verdict::Never;
Verdict g_lastSdRestore = Verdict::Never;
bool g_sdReleased = false;
Verdict g_lastRadioCut = Verdict::Never;
bool g_radioDeinited = false;

int idx(const Sink s) { return static_cast<int>(s); }

}  // namespace

bool buildDefault(const Sink s) { return kSinks[idx(s)].def; }

bool active(const Sink s) {
  const int8_t o = g_override[idx(s)];
  return o < 0 ? buildDefault(s) : (o != 0);
}

int8_t overrideValue(const Sink s) { return g_override[idx(s)]; }
void setOverride(const Sink s, const bool on) { g_override[idx(s)] = on ? 1 : 0; }

void clearAll() {
  for (auto& o : g_override) o = -1;
}

const char* name(const Sink s) { return kSinks[idx(s)].name; }
const char* help(const Sink s) { return kSinks[idx(s)].help; }

size_t status(char* out, size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  size_t used = 0;
  out[0] = '\0';
  for (size_t i = 0; i < sizeof(kSinks) / sizeof(kSinks[0]); ++i) {
    const Sink s = static_cast<Sink>(i);
    char line[224];
    // r106 B0: the default token is written as TWO literals on purpose -- `strings` on the
    // shipping image must be able to prove the usb sink ships `default=off` without a device.
    const int n = kSinks[i].def
                      ? snprintf(line, sizeof(line), "exp park %s=%s default=on (%s)", kSinks[i].name,
                                 active(s) ? "on" : "off", kSinks[i].help)
                      : snprintf(line, sizeof(line), "exp park %s=%s default=off (%s)", kSinks[i].name,
                                 active(s) ? "on" : "off", kSinks[i].help);
    if (n <= 0) continue;
    const size_t len = static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n) : sizeof(line) - 1;
    if (used + len + 1 >= cap) break;  // never split a line
    std::memcpy(out + used, line, len);
    used += len;
    out[used++] = '\n';
    out[used] = '\0';
  }
  return used;
}

// r109 EXP2: the four arms WITHOUT the per-sink help sentence. `exp`'s body cap (~1080 B) cut the
// list before `exp park radio=...`/`sd=...` arrived, which is why H1's radio/sd half was UNREAD on
// r108. `exp park` uses this: four short lines, ~150 B, always inside the cap. `exp` and
// `exp park status` keep the full form (help included).
size_t statusCompact(char* out, size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  size_t used = 0;
  out[0] = '\0';
  for (size_t i = 0; i < sizeof(kSinks) / sizeof(kSinks[0]); ++i) {
    const Sink s = static_cast<Sink>(i);
    char buf[64];
    const int n = snprintf(buf, sizeof(buf), "exp park %s=%s default=%s", kSinks[i].name,
                           active(s) ? "on" : "off", kSinks[i].def ? "on" : "off");
    if (n <= 0) continue;
    const size_t len = static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n) : sizeof(buf) - 1;
    if (used + len + 1 >= cap) break;
    std::memcpy(out + used, buf, len);
    used += len;
    out[used++] = '\n';
    out[used] = '\0';
  }
  return used;
}

// ---------------------------------------------------------------------------
// E4c — USB-Serial/JTAG PHY pads
//
// On this board the console IS the USJ peripheral (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG),
// so the sink is the ANALOG front end, not the driver: usb_pad_enable=0 drops the
// PHY bias, the D+ pull-up and the pad drivers, which is the current that is burnt
// with no host attached. The driver, its ISR and the bus clock stay installed on
// purpose — an IDF console write from any stray log line while the clock were gated
// is an untested register access, and a hang in park is a device the operator has to
// power-cycle. The caller gates this on VBUS: with a cable present the pads are the
// one thing that must stay up, or a parked-then-woken device loses its console.
// ---------------------------------------------------------------------------
bool usbPhyDown() {
#if PARKSINK_HW && defined(CONFIG_IDF_TARGET_ESP32S31)
  usb_serial_jtag_ll_phy_enable_pad(false);
  return !usb_serial_jtag_ll_phy_is_pad_enabled();
#else
  return false;
#endif
}

bool usbPhyUp() {
#if PARKSINK_HW && defined(CONFIG_IDF_TARGET_ESP32S31)
  usb_serial_jtag_ll_phy_enable_pad(true);
  return usb_serial_jtag_ll_phy_is_pad_enabled();
#else
  return false;
#endif
}

// ---------------------------------------------------------------------------
// E4f — SDMMC pads
//
// SDCardManager::shutdown() already floats the six pins as plain GPIOs, but on the
// S31 the SDIO pins are still owned by the CNNT dedicated-pad controller
// (sdio_pad_pin_ctrl_ded_sel=1), so the GPIO directions are not what the pads obey
// and the card's VDD net stays back-fed through the pad pull-ups. Clearing ded_sel
// and selecting the pull-downs is the vendor's own pre-sleep step. Restore puts
// ded_sel back so the next sdmmc_host init adopts the pads exactly as on a cold
// boot; the remount that follows is the first card access either way.
// ---------------------------------------------------------------------------
bool sdPadsRelease() {
#if PARKSINK_HW && defined(CONFIG_IDF_TARGET_ESP32S31)
  CNNT_PAD_CTRL.sdio_clk.sdio_clk_fun_wpu = 0;
  CNNT_PAD_CTRL.sdio_clk.sdio_clk_fun_wpd = 1;
  CNNT_PAD_CTRL.sdio_cmd.sdio_cmd_fun_wpu = 0;
  CNNT_PAD_CTRL.sdio_cmd.sdio_cmd_fun_wpd = 1;
  CNNT_PAD_CTRL.sdio_data0.sdio_data0_fun_wpu = 0;
  CNNT_PAD_CTRL.sdio_data0.sdio_data0_fun_wpd = 1;
  CNNT_PAD_CTRL.sdio_data1.sdio_data1_fun_wpu = 0;
  CNNT_PAD_CTRL.sdio_data1.sdio_data1_fun_wpd = 1;
  CNNT_PAD_CTRL.sdio_data2.sdio_data2_fun_wpu = 0;
  CNNT_PAD_CTRL.sdio_data2.sdio_data2_fun_wpd = 1;
  CNNT_PAD_CTRL.sdio_data3.sdio_data3_fun_wpu = 0;
  CNNT_PAD_CTRL.sdio_data3.sdio_data3_fun_wpd = 1;
  CNNT_PAD_CTRL.ctrl.sdio_pad_pin_ctrl_ded_sel = 0;
  const bool ok = CNNT_PAD_CTRL.ctrl.sdio_pad_pin_ctrl_ded_sel == 0;
  g_lastSdRelease = ok ? Verdict::Ok : Verdict::Failed;
  g_sdReleased = ok;
  return ok;
#else
  g_lastSdRelease = Verdict::Failed;
  return false;
#endif
}

bool sdPadsRestore() {
#if PARKSINK_HW && defined(CONFIG_IDF_TARGET_ESP32S31)
  CNNT_PAD_CTRL.sdio_clk.sdio_clk_fun_wpu = 0;
  CNNT_PAD_CTRL.sdio_clk.sdio_clk_fun_wpd = 0;
  CNNT_PAD_CTRL.sdio_cmd.sdio_cmd_fun_wpu = 0;
  CNNT_PAD_CTRL.sdio_cmd.sdio_cmd_fun_wpd = 0;
  CNNT_PAD_CTRL.sdio_data0.sdio_data0_fun_wpu = 0;
  CNNT_PAD_CTRL.sdio_data0.sdio_data0_fun_wpd = 0;
  CNNT_PAD_CTRL.sdio_data1.sdio_data1_fun_wpu = 0;
  CNNT_PAD_CTRL.sdio_data1.sdio_data1_fun_wpd = 0;
  CNNT_PAD_CTRL.sdio_data2.sdio_data2_fun_wpu = 0;
  CNNT_PAD_CTRL.sdio_data2.sdio_data2_fun_wpd = 0;
  CNNT_PAD_CTRL.sdio_data3.sdio_data3_fun_wpu = 0;
  CNNT_PAD_CTRL.sdio_data3.sdio_data3_fun_wpd = 0;
  CNNT_PAD_CTRL.ctrl.sdio_pad_pin_ctrl_ded_sel = 1;
  const bool ok = CNNT_PAD_CTRL.ctrl.sdio_pad_pin_ctrl_ded_sel == 1;
  g_lastSdRestore = ok ? Verdict::Ok : Verdict::Failed;
  if (ok) g_sdReleased = false;
  return ok;
#else
  g_lastSdRestore = Verdict::Failed;
  return false;
#endif
}

// ---------------------------------------------------------------------------
// E4e — radio
//
// BT first, factually: this image has no BT controller at all (CONFIG_BT_ENABLED is
// not set in idf/sdkconfig and FREEINK_CAP_BLE_HID_HOST is 0), so there is nothing
// to disable and a BLE page-turner cannot be paired to it -- the caller logs `bt=0`
// rather than pretending a controller went down. The BT MODULE's supply is the
// expander's P1.4 rail, already cut by the existing PA trim.
//
// WiFi: the sleep path already calls WiFi.disconnect(true)+mode(WIFI_OFF) before the
// park, so the RF is down but esp_wifi_init's driver, buffers and timers are still
// resident. esp_wifi_deinit() frees those; resume re-inits with the SAME buffer
// configuration Arduino's wifiLowLevelInit() uses, because Arduino's own
// `lowLevelInitDone` stays true and the next WiFi.mode()/begin() will call
// esp_wifi_* directly instead of re-initialising. A stack that was never initialised
// is left alone: on battery that is the common case and re-initialising it would
// allocate buffers the session never asked for.
// ---------------------------------------------------------------------------
bool radioDown(bool* wasInited, int* err) {
#if PARKSINK_HW
  if (wasInited != nullptr) *wasInited = false;
  if (err != nullptr) *err = 0;
#if CP_PARK_WIFI_PAUSE  // r117 BUCKET B, default OFF: vendor stops the driver before deinit
  esp_wifi_stop();
#endif
  const esp_err_t e = esp_wifi_deinit();
  if (e == ESP_ERR_WIFI_NOT_INIT) {
    g_lastRadioCut = Verdict::Never;  // nothing to cut, nothing to restore
    return true;
  }
  if (wasInited != nullptr) *wasInited = true;
  if (err != nullptr) *err = static_cast<int>(e);
  const bool ok = e == ESP_OK;
  g_lastRadioCut = ok ? Verdict::Ok : Verdict::Failed;
  if (ok) g_radioDeinited = true;
  return ok;
#else
  if (wasInited != nullptr) *wasInited = false;
  if (err != nullptr) *err = 0;
  g_lastRadioCut = Verdict::Never;
  return true;
#endif
}

bool radioUp(int* err) {
#if PARKSINK_HW
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  // Arduino's wifiLowLevelInit() configuration (static buffers off): match it so the
  // stack the rest of the session sees after a resume is the stack it had before.
  cfg.static_tx_buf_num = 0;
  cfg.dynamic_tx_buf_num = 32;
  cfg.tx_buf_type = 1;
  cfg.cache_tx_buf_num = 4;
  cfg.static_rx_buf_num = 4;
  cfg.dynamic_rx_buf_num = 32;
  const esp_err_t e = esp_wifi_init(&cfg);
  if (err != nullptr) *err = static_cast<int>(e);
  if (e == ESP_OK) g_radioDeinited = false;
  return e == ESP_OK;
#else
  if (err != nullptr) *err = 0;
  return false;
#endif
}

Verdict lastSdRelease() { return g_lastSdRelease; }
Verdict lastSdRestore() { return g_lastSdRestore; }
bool sdPadsReleased() { return g_sdReleased; }
Verdict lastRadioCut() { return g_lastRadioCut; }
bool radioDeinited() { return g_radioDeinited; }

}  // namespace parksink

#endif  // CP_PARK_SINKS
