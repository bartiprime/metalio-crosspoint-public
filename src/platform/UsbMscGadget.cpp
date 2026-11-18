#include "UsbMscGadget.h"

#include <sdkconfig.h>

#include "UsbMscStorage.h"
#include "UsbPortRoute.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(CONFIG_TINYUSB_MSC_ENABLED)
#if __has_include("tinyusb.h")
#define CP_USB_MSC_GADGET_COMPILED 1
#endif
#endif
#ifndef CP_USB_MSC_GADGET_COMPILED
#define CP_USB_MSC_GADGET_COMPILED 0
#endif

#if CP_USB_MSC_GADGET_COMPILED
#include <atomic>

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#endif

namespace {

constexpr const char* TAG = "USBMSC";

// Vendor timings (usb_virtual_disk.cc:63-64). The disconnect wait lets the host
// retire the old device before the port is re-routed; the settle wait lets the
// analog switch and the HS PHY settle before the pull-up appears.
constexpr uint32_t kUsbDisconnectMs = 250;
constexpr uint32_t kUsbTeardownMs = 150;

// State is observable whether or not the gadget was compiled in, so it lives
// outside the compile guard.
std::atomic<int> s_state{static_cast<int>(usbmgadget::State::Idle)};
std::atomic<bool> s_host_configured{false};

void setState(usbmgadget::State s) { s_state.store(static_cast<int>(s)); }

#if CP_USB_MSC_GADGET_COMPILED

bool s_driver_installed = false;

// ---- descriptors (vendor usb_virtual_disk.cc:113-170, byte-identical shape) --
enum { kItfNumMsc = 0, kItfNumTotal };
enum : uint8_t {
  kEdptMscOut = 0x01,
  kEdptMscIn = 0x81,
};

constexpr int kTusbDescTotalLen = TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN;

const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(s_device_desc),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4002,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

// High speed: 512-byte bulk endpoints (the S31 runs the OTG controller at HS).
const uint8_t s_msc_hs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, kItfNumTotal, 0, kTusbDescTotalLen, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(kItfNumMsc, 0, kEdptMscOut, kEdptMscIn, 512),
};

// Full speed fallback: same interface, 64-byte packets.
const uint8_t s_msc_fs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, kItfNumTotal, 0, kTusbDescTotalLen, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(kItfNumMsc, 0, kEdptMscOut, kEdptMscIn, 64),
};

const char* const s_string_desc[] = {
    (const char[]){0x09, 0x04},
    "CrossPoint",
    usbmsc::kVolumeLabel,
    "123456",
    "CrossPoint MSC",
};

void usbEventCb(tinyusb_event_t* event, void* /*arg*/) {
  if (event == nullptr) return;
  switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
      s_host_configured.store(true);
      setState(usbmgadget::State::Connected);
      ESP_LOGW(TAG, "host configured the drive (enumeration complete)");
      break;
    case TINYUSB_EVENT_DETACHED:
      s_host_configured.store(false);
      if (usbmgadget::state() == usbmgadget::State::Connected) {
        setState(usbmgadget::State::WaitingForHost);
      }
      ESP_LOGW(TAG, "host released the drive (detached)");
      break;
#ifdef CONFIG_TINYUSB_SUSPEND_CALLBACK
    case TINYUSB_EVENT_SUSPENDED:
      ESP_LOGI(TAG, "host suspended the bus");
      break;
#endif
#ifdef CONFIG_TINYUSB_RESUME_CALLBACK
    case TINYUSB_EVENT_RESUMED:
      ESP_LOGI(TAG, "host resumed the bus");
      break;
#endif
    default:
      break;
  }
}

#endif  // CP_USB_MSC_GADGET_COMPILED

}  // namespace

namespace usbmgadget {

State state() { return static_cast<State>(s_state.load()); }
bool hostConnected() { return s_host_configured.load(); }

const char* stateName(State s) {
  switch (s) {
    case State::Idle:
      return "idle";
    case State::Starting:
      return "starting";
    case State::WaitingForHost:
      return "waiting-for-host";
    case State::Connected:
      return "connected";
    case State::Error:
      return "error";
  }
  return "?";
}

esp_err_t start() {
  if (!usbmsc::armed()) {
    ESP_LOGE(TAG, "gadget start refused: USB-MSC not armed");
    return ESP_ERR_INVALID_STATE;
  }
#if !CP_USB_MSC_GADGET_COMPILED
  ESP_LOGE(TAG, "gadget start: esp_tinyusb MSC not compiled in");
  return ESP_ERR_NOT_SUPPORTED;
#else
  if (s_driver_installed) return ESP_OK;  // idempotent

  setState(State::Starting);

  // 1. Route the pads to OTG and take the PHY leg (vendor :571). Doing this
  //    before the driver is installed means the host can never see a device
  //    whose medium is not ready yet.
  if (!usbport::routeToOtg("usb drive mode")) {
    setState(State::Error);
    return ESP_ERR_INVALID_STATE;
  }

  // 2. Install the driver: HS UTMI, TinyUSB owns the PHY on this SoC
  //    (vendor :573-591, TINYUSB_CONFIG_HIGH_SPEED + custom task).
  tinyusb_config_t tusb_cfg = TINYUSB_CONFIG_HIGH_SPEED(usbEventCb, nullptr);
  tusb_cfg.task = TINYUSB_TASK_CUSTOM(TINYUSB_DEFAULT_TASK_SIZE, TINYUSB_DEFAULT_TASK_PRIO, 0);
  tusb_cfg.descriptor.device = &s_device_desc;
  tusb_cfg.descriptor.full_speed_config = s_msc_fs_config_desc;
  tusb_cfg.descriptor.high_speed_config = s_msc_hs_config_desc;
  tusb_cfg.descriptor.string = const_cast<const char**>(s_string_desc);
  tusb_cfg.descriptor.string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]);

  esp_err_t err = tinyusb_driver_install(&tusb_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
    usbport::routeToConsole("gadget install failed");
    setState(State::Error);
    return err;
  }
  s_driver_installed = true;

  // 3. Driver installation starts the controller; disconnect again so the host
  //    sees a clean attach only once the medium is ready (vendor :631-637).
  ESP_LOGW(TAG, "soft disconnect, wait %u ms", (unsigned)kUsbDisconnectMs);
  tud_disconnect();
  vTaskDelay(pdMS_TO_TICKS(kUsbDisconnectMs));

  // 4. Hand the sectors to the host (vendor :646 MountToUsb).
  err = usbmsc::exposeToHost();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "expose to host failed: %s", esp_err_to_name(err));
    // FAILED-START UNWIND. This used to uninstall the gadget driver and stop, which left
    // the MEDIUM half-owned: UsbMscStorage's s_hdl still pointed at a live MSC storage
    // object (APP-mounted, with its FATFS mount and its msc driver still installed), so a
    // retry re-entered a gadget whose storage layer had never been told the start failed.
    // Unwind in the vendor's down order (storage first, then the drivers, then the pads) so
    // the next start() sees what a fresh boot sees:
    //   before: driver installed, storage object live (APP mount), pads on OTG, bus disconnected
    //   after : no storage object (s_hdl == nullptr), no msc driver, no gadget driver,
    //           pads back on the console, state Error
    const esp_err_t td = usbmsc::teardown();
    if (td != ESP_OK) {
      // Not fatal: the caller is told the start failed and the port is being handed back.
      // Saying so is what makes a second attempt diagnosable instead of mysterious.
      ESP_LOGW(TAG, "failed-start teardown incomplete: %s", esp_err_to_name(td));
    }
    // teardown() drops the MSC storage handle on BOTH outcomes (UsbMscStorage.cpp
    // teardown(): a failed delete_storage orphans the object but never leaves the pointer
    // set), so this one line is what the next device log shows to prove it: no retry can
    // reach a handle this start left behind. prepare() will build a fresh storage object,
    // or fail honestly if the orphan still holds a LUN.
    ESP_LOGW(TAG, "failed start (%s): MSC storage handle cleared, retry starts clean",
             esp_err_to_name(err));
    // Disconnect before uninstall, the same order stop() uses: the host must retire the
    // LUN while the driver that serves it still exists.
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(kUsbTeardownMs));
    const esp_err_t un = tinyusb_driver_uninstall();
    if (un != ESP_OK) {
      ESP_LOGW(TAG, "tinyusb_driver_uninstall after failed start: %s", esp_err_to_name(un));
    }
    s_driver_installed = false;
    s_host_configured.store(false);
    usbport::routeToConsole("expose failed");
    setState(State::Error);
    return err;
  }

  // 5. Connect. The host enumerates from here.
  setState(State::WaitingForHost);
  tud_connect();
  ESP_LOGW(TAG, "gadget up, waiting for host to configure the drive");
  return ESP_OK;
#endif
}

esp_err_t stop() {
#if !CP_USB_MSC_GADGET_COMPILED
  return ESP_ERR_NOT_SUPPORTED;
#else
  if (!s_driver_installed) {
    usbport::routeToConsole("stop (no gadget)");
    setState(State::Idle);
    return ESP_OK;
  }

  // Vendor teardown order (usb_virtual_disk.cc:483-510): soft disconnect first,
  // give the host time to retire the LUN, then uninstall, then the PHY leg and
  // the switch back to the console.
  ESP_LOGW(TAG, "soft disconnect before teardown");
  tud_disconnect();
  vTaskDelay(pdMS_TO_TICKS(kUsbDisconnectMs));

  const esp_err_t err = tinyusb_driver_uninstall();
  s_driver_installed = false;
  s_host_configured.store(false);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "tinyusb_driver_uninstall: %s", esp_err_to_name(err));
  }

  vTaskDelay(pdMS_TO_TICKS(kUsbTeardownMs));
  usbport::routeToConsole("gadget stopped");
  setState(State::Idle);
  ESP_LOGW(TAG, "gadget stopped, console route restored");
  return err;
#endif
}

}  // namespace usbmgadget
