#pragma once

#include <esp_err.h>

// USB MSC gadget bring-up (scope C4). Mirrors the vendor's proven ordering for
// this exact SoC (_scratch/vendor-metalio main/boards/common/storage/
// usb_virtual_disk.cc @2f55cb1, the source of the shipping STOCKFW image):
//
//   EnsureMscStorage()            :422-481   storage bound before the gadget
//   RoutePhyToOtg()               :225-241   PHY leg (no-op on S31, logged)
//   tinyusb_driver_install()      :584       HS UTMI, TinyUSB owns the PHY
//   tud_disconnect() + 250 ms     :634-637   clean (re)attach
//   ANA_SW low + 100 ms settle    :638-643   pads move to OTG
//   MountToUsb()                  :646       medium ready before the host sees it
//   tud_connect()                 :660
//
// Descriptor shape is the same one the vendor ships and the same one all three
// third-party images carry (descriptor-verified: 32-byte config, one class-08
// interface, bulk EP 0x01/0x81, 512-byte MPS on the HS descriptor and 64 on the
// FS one, VID:PID 303A:4002).
namespace usbmgadget {

enum class State {
  Idle,            // not started
  Starting,        // route + driver install in progress
  WaitingForHost,  // gadget up, host has not configured us yet
  Connected,       // host enumerated the drive
  Error,           // bring-up failed (caller must reboot to recover)
};

// Installs the gadget and connects it to the port. Requires usbmsc::prepare()
// to have succeeded and the ANA_SW to be free. Idempotent for State::Connected.
esp_err_t start();

// Soft-disconnects and uninstalls the gadget, then routes the port back to the
// console. Never remounts the FS -- the caller reboots.
esp_err_t stop();

State state();
bool hostConnected();
const char* stateName(State state);

}  // namespace usbmgadget
