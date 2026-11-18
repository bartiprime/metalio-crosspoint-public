#pragma once

// Releases the ESP32-S3 USB-OTG PHY and reconnects the hardware USB
// Serial/JTAG peripheral before a normal application restart. No-op on targets
// that do not enable the USB Drive capability (FREEINK_CAP_USB_MSC).
void handoffUsbOtgToSerialJtag();

// ---------------------------------------------------------------------------
// Scope C4 (USB mass storage) groundwork -- the two legs of the port hand-off.
//
// These are independent of FREEINK_CAP_USB_MSC: that macro still gates the old
// Arduino-USBMSC implementation, which stays compiled out. The pair below is
// called only from usbport::routeToOtg()/routeToConsole(), which is itself
// gated behind the USB-MSC runtime flag (usbport::armed()).
//
// On ESP32-S31 the USB-OTG (HS UTMI) and the USB-Serial/JTAG controllers are
// separate peripherals with separate PHYs -- the vendor's own S31 branch of
// RoutePhyToOtg/RoutePhyToUsj only logs
// (_scratch/vendor-metalio main/boards/common/storage/usb_virtual_disk.cc:225-258,
// strings "PHY: S31 UTMI (TinyUSB managed)" / "PHY: S31 USJ untouched" verified
// present in the shipping STOCKFW.bin). The pads move when the board's ANA_SW
// analog switch is thrown, so on this board the "release" is the switch and
// these functions document/verify that rather than poking a non-existent mux.
// ---------------------------------------------------------------------------
void usbSerialJtagReleaseForOtg();
void usbSerialJtagRestoreAfterOtg();
