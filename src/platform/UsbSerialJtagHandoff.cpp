#include "UsbSerialJtagHandoff.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <sdkconfig.h>
#endif

#if defined(ARDUINO_ARCH_ESP32) && CONFIG_IDF_TARGET_ESP32S3 && FREEINK_CAP_USB_MSC

#include <Arduino.h>
#include <esp_err.h>
#include <esp_intr_alloc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <hal/clk_gate_ll.h>
#include <hal/usb_serial_jtag_ll.h>
#include <soc/periph_defs.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/soc.h>
#include <soc/usb_serial_jtag_reg.h>
#include <soc/usb_serial_jtag_struct.h>

extern "C" esp_err_t deinit_usb_hal();

namespace {
void IRAM_ATTR onUsbSerialJtagReset(void* context) {
  BaseType_t higherPriorityTaskWoken = pdFALSE;
  const uint32_t interruptStatus = usb_serial_jtag_ll_get_intsts_mask();
  usb_serial_jtag_ll_clr_intsts_mask(interruptStatus);
  if (interruptStatus & USB_SERIAL_JTAG_INTR_BUS_RESET) {
    xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(context), &higherPriorityTaskWoken);
  }
  if (higherPriorityTaskWoken == pdTRUE) portYIELD_FROM_ISR();
}
}  // namespace

void handoffUsbOtgToSerialJtag() {
  // Serial/JTAG was active before USB Drive claimed the shared PHY. Release
  // its old ISR first so the temporary BUS_RESET listener below can own it.
  Serial.end();

  // Match Arduino's usb_switch_to_cdc_jtag(): stop USB-OTG, select the
  // hardware Serial/JTAG PHY, and force a host-visible disconnect/reconnect.
  (void)deinit_usb_hal();
  periph_ll_reset(PERIPH_USB_MODULE);
  periph_ll_disable_clk_set_rst(PERIPH_USB_MODULE);
  CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                      RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL | USB_SERIAL_JTAG_USB_PAD_ENABLE);

  pinMode(USB_INT_PHY0_DM_GPIO_NUM, OUTPUT_OPEN_DRAIN);
  pinMode(USB_INT_PHY0_DP_GPIO_NUM, OUTPUT_OPEN_DRAIN);
  digitalWrite(USB_INT_PHY0_DM_GPIO_NUM, LOW);
  digitalWrite(USB_INT_PHY0_DP_GPIO_NUM, LOW);

  const usb_serial_jtag_pull_override_vals_t pullConfig = {
      .dp_pu = 1,
      .dm_pu = 0,
      .dp_pd = 0,
      .dm_pd = 0,
  };
  usb_serial_jtag_ll_phy_enable_pull_override(&pullConfig);
  usb_serial_jtag_ll_phy_disable_pull_override();
  usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
  usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
  usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_BUS_RESET);

  SemaphoreHandle_t resetSemaphore = xSemaphoreCreateBinary();
  intr_handle_t resetInterrupt = nullptr;
  if (!resetSemaphore || esp_intr_alloc(ETS_USB_SERIAL_JTAG_INTR_SOURCE, 0, onUsbSerialJtagReset, resetSemaphore,
                                        &resetInterrupt) != ESP_OK) {
    if (resetSemaphore) vSemaphoreDelete(resetSemaphore);
    resetSemaphore = nullptr;
  }

  SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  if (resetSemaphore) {
    (void)xSemaphoreTake(resetSemaphore, pdMS_TO_TICKS(1000));
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    esp_intr_free(resetInterrupt);
    vSemaphoreDelete(resetSemaphore);
  } else {
    // Preserve a visible disconnect even if the temporary listener could not
    // be allocated; the subsequent normal reboot initializes Serial/JTAG.
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    delay(20);
  }
}

#else

void handoffUsbOtgToSerialJtag() {}

#endif

// ---------------------------------------------------------------------------
// Scope C4 groundwork. Deliberately outside the FREEINK_CAP_USB_MSC guard above:
// that macro gates the legacy Arduino-USBMSC path, whereas these two legs
// belong to the esp_tinyusb-based route helper (src/platform/UsbPortRoute.cpp).
// Nothing calls them unless the USB-MSC runtime flag is armed.
// ---------------------------------------------------------------------------
#if defined(ARDUINO_ARCH_ESP32) && CONFIG_IDF_TARGET_ESP32S31

#include <esp_log.h>

namespace {
constexpr const char* kUsjTag = "USJ";
}  // namespace

void usbSerialJtagReleaseForOtg() {
  // S31 has no PHY mux to flip: HS UTMI (what TinyUSB drives) and USB-Serial/JTAG
  // are separate controllers with separate PHYs, and TinyUSB installs its own PHY
  // in tinyusb_driver_install(). The vendor proves this by leaving the whole leg
  // as a log line on S31 (usb_virtual_disk.cc:225-228). The pads are moved by the
  // board's ANA_SW analog switch, which the caller (usbport::routeToOtg) throws
  // immediately after this returns -- that ordering is the vendor's
  // (usb_virtual_disk.cc:571 then :639).
  ESP_LOGW(kUsjTag, "S31: USJ untouched (separate from UTMI), OTG PHY is TinyUSB-managed");
}

void usbSerialJtagRestoreAfterOtg() {
  // Symmetric no-op. The console comes back two ways: usbport::routeToConsole()
  // throws ANA_SW high straight away, and the reboot that ends a drive session
  // re-runs epd_board_init() -> ana_sw_set(true)
  // (components/metalio_driver/display/epd_board_metalio_eink4_plus.c:627) and
  // re-initialises the IDF console, exactly as the vendor reboots its storage
  // hand-off path. There is no PHY state to undo on this target.
  ESP_LOGI(kUsjTag, "S31: USJ untouched (separate from UTMI), console route is switch-only");
}

#else

void usbSerialJtagReleaseForOtg() {}
void usbSerialJtagRestoreAfterOtg() {}

#endif

