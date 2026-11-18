#pragma once

#include <stdbool.h>

// USB port routing for the Metalio E-INK4 Plus (ESP32-S31).
//
// The board has ONE physical USB port, time-shared between two independent
// controllers by a GPIO36 analog switch (ANA_SW):
//     HIGH -> USB-Serial/JTAG (the console, VID:PID 303A:1001)
//     LOW  -> USB-OTG / High-Speed UTMI (TinyUSB mass storage)
//
// Authority for the polarity and the ordering is the shipping vendor firmware
// for this exact SoC, `_scratch/vendor-metalio` @2f55cb1:
//   * main/boards/metalio-e-ink4-plus/config.h:101      ANA_SW_GPIO = GPIO_NUM_36
//   * .../display/epd_board_metalio_eink4_plus.c:597    boot leaves it HIGH ("非虚拟 U 盘")
//   * main/boards/common/storage/usb_virtual_disk.cc:225-258  PHY leg (no-op on S31)
//   * .../usb_virtual_disk.cc:637-660                   OTG: PHY leg, then ANA_SW low, settle, attach
//   * .../usb_virtual_disk.cc:500-507                   console: PHY leg, then ANA_SW high
//
// GROUNDWORK ONLY (scope C4, stage 1): nothing calls routeToOtg() today, and it
// refuses to move the switch unless the runtime flag has been armed. A user who
// never enters drive mode therefore cannot leave the console route.
namespace usbport {

enum class Route : unsigned char {
  Unknown,  // this module has not driven the switch yet
  Console,  // ANA_SW HIGH -- USB-Serial/JTAG owns the pads
  Otg,      // ANA_SW LOW  -- USB-OTG owns the pads
};

// The single runtime gate for the whole USB-MSC feature. Default false; round 2
// arms it from the "Connect as USB drive" action and disarms on exit. Every
// hardware-touching entry point below is refused while it is false.
void setArmed(bool armed);
bool armed();

Route current();

// Reads GPIO36 back (output latch) without driving it. True == console route.
bool consoleIsDefault();

// Idempotent. Always allowed: this is the safe direction.
bool routeToConsole(const char* why);

// Idempotent. Refused (returns false, no hardware write) unless armed().
// Mirrors the vendor sequence: PHY/USJ leg first, then ANA_SW low.
bool routeToOtg(const char* why);

// Settle time the vendor waits between throwing the switch and letting the host
// enumerate (usb_virtual_disk.cc:64, kUsbRouteSettleMs).
constexpr unsigned kRouteSettleMs = 100;

// RAII: restores the console route on destruction, so an early return inside a
// drive-mode attempt cannot leave the switch thrown.
class ConsoleRouteGuard {
 public:
  explicit ConsoleRouteGuard(const char* why);
  ~ConsoleRouteGuard();
  ConsoleRouteGuard(const ConsoleRouteGuard&) = delete;
  ConsoleRouteGuard& operator=(const ConsoleRouteGuard&) = delete;

  // Give up ownership: the caller promises it handles the restore itself.
  void release() { engaged_ = false; }

 private:
  const char* why_;
  bool engaged_;
};

}  // namespace usbport
