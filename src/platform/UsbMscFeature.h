#pragma once

// CP_USB_MSC: 1 when this board compiles the esp_tinyusb-backed USB mass-storage
// drive mode (scope C4). Set by idf/components/crosspoint_app/CMakeLists.txt for
// the Metalio E-INK4 Plus only; every other board keeps 0 and links none of it.
//
// This is deliberately NOT FREEINK_CAP_USB_MSC. That macro belongs to the SDK's
// Arduino-TinyUSB implementation (freeink-sdk libs/hardware/UsbMassStorage),
// which needs Arduino's CONFIG_TINYUSB_ENABLED and therefore the USB peripheral
// this board's console lives on. This feature uses the esp_tinyusb managed
// component's MSC class instead, with its own descriptor set and its own
// ANA_SW-driven port hand-over.
#ifndef CP_USB_MSC
#define CP_USB_MSC 0
#endif
