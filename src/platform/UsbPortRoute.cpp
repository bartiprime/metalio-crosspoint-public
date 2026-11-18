#include "UsbPortRoute.h"

#include <sdkconfig.h>

#include <atomic>

#include "UsbSerialJtagHandoff.h"
#include "driver/gpio.h"
#include "esp_log.h"

#if CONFIG_IDF_TARGET_ESP32S31
// metalio_driver (components/metalio_driver/display) -- the vendor EPD board
// support for this panel, compiled verbatim from the vendor tree.
#include "epd_board_metalio_eink4_plus.h"
#endif

namespace {

constexpr const char* TAG = "USBPORT";

// ANA_SW on the single USB port (vendor config.h:101, our copy of the same
// board file lives in components/metalio_driver/config.h:101).
constexpr int kAnaSwGpio = 36;

std::atomic<bool> s_armed{false};
std::atomic<int> s_applied{static_cast<int>(usbport::Route::Unknown)};

}  // namespace

namespace usbport {

void setArmed(bool value) {
  const bool prev = s_armed.exchange(value, std::memory_order_relaxed);
  if (prev == value) return;
  ESP_LOGW(TAG, "USB-MSC runtime flag %s", value ? "ARMED" : "disarmed");
}

bool armed() { return s_armed.load(std::memory_order_relaxed); }

Route current() { return static_cast<Route>(s_applied.load(std::memory_order_relaxed)); }

bool consoleIsDefault() {
#if CONFIG_IDF_TARGET_ESP32S31
  // gpio_get_level() on an output returns the latched level, so this reads the
  // switch state without changing the pin's direction or value.
  return gpio_get_level(static_cast<gpio_num_t>(kAnaSwGpio)) != 0;
#else
  // No analog switch on other targets we build for; the console is whatever the
  // target's own mux says.
  return true;
#endif
}

bool routeToConsole(const char* why) {
  const bool already = (current() == Route::Console) && consoleIsDefault();
  if (already) return true;

  // Vendor teardown order (usb_virtual_disk.cc:500-507): PHY/USJ leg first,
  // then the analog switch. On S31 the USJ/UTMI peripherals are separate, so the
  // PHY leg only logs; the switch is what actually re-routes the pads.
  usbSerialJtagRestoreAfterOtg();

#if CONFIG_IDF_TARGET_ESP32S31
  epd_board_metalio_eink4_plus_ana_sw_set(true);
#endif

  s_applied.store(static_cast<int>(Route::Console), std::memory_order_relaxed);
  ESP_LOGI(TAG, "route -> console (ANA_SW(GPIO%d)=1)%s%s", kAnaSwGpio, why ? ": " : "",
           why ? why : "");
  return true;
}

bool routeToOtg(const char* why) {
  if (!armed()) {
    // Single choke point: an unarmed build can never move the switch off the
    // console, whatever a future call site does.
    ESP_LOGW(TAG, "route -> OTG refused: USB-MSC not armed%s%s", why ? ": " : "",
             why ? why : "");
    return false;
  }

  const bool already = (current() == Route::Otg) && !consoleIsDefault();
  if (already) return true;

  // Vendor start order (usb_virtual_disk.cc:571 RoutePhyToOtg, then :639
  // ANA_SW low, then :643 settle): the PHY leg runs before the switch.
  usbSerialJtagReleaseForOtg();

#if CONFIG_IDF_TARGET_ESP32S31
  epd_board_metalio_eink4_plus_ana_sw_set(false);
#endif

  s_applied.store(static_cast<int>(Route::Otg), std::memory_order_relaxed);
  ESP_LOGW(TAG, "route -> OTG (ANA_SW(GPIO%d)=0), settle %u ms%s%s", kAnaSwGpio,
           static_cast<unsigned>(kRouteSettleMs), why ? ": " : "", why ? why : "");
  return true;
}

ConsoleRouteGuard::ConsoleRouteGuard(const char* why) : why_(why), engaged_(true) {}

ConsoleRouteGuard::~ConsoleRouteGuard() {
  if (engaged_) routeToConsole(why_);
}

}  // namespace usbport
