// Out-of-class definitions for MySerialImpl, which lib/Logging/Logging.h
// declares (and aliases as `#define Serial MySerialImpl::instance`) but never
// defines anywhere in the tree. Any translation unit that includes Logging.h
// and touches `Serial` needs these, so the link fails with
// "undefined reference to MySerialImpl::instance / ::flush" without them.
//
// Forward everything to logSerial, which is the real HWCDC/HardwareSerial
// object captured in Logging.h before the `Serial` macro is redefined.

// CP_LOG_VIA_PRINTF: on the Metalio the USB-Serial/JTAG peripheral belongs to the IDF console
// (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y). Arduino's HWCDC claims it at boot when
// ARDUINO_USB_CDC_ON_BOOT=1, which kills printf and leaves the app completely silent on USB --
// and the frontlight is then the only channel. With CDC off and this define set, every Serial.*
// in the tree reaches the host through stdout instead.

#include <Arduino.h>

#include <cstdarg>
#include <cstdio>

#include "Logging.h"

MySerialImpl MySerialImpl::instance;

#ifdef CP_LOG_VIA_PRINTF

size_t MySerialImpl::write(uint8_t b) {
  fputc(b, stdout);
  return 1;
}

size_t MySerialImpl::write(const uint8_t* buffer, size_t size) {
  const size_t n = fwrite(buffer, 1, size, stdout);
  return n;
}

void MySerialImpl::flush() { fflush(stdout); }

size_t MySerialImpl::printf(const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int n = vfprintf(stdout, format, args);
  va_end(args);
  return n < 0 ? 0 : static_cast<size_t>(n);
}

#else

size_t MySerialImpl::write(uint8_t b) { return logSerial.write(b); }

size_t MySerialImpl::write(const uint8_t* buffer, size_t size) { return logSerial.write(buffer, size); }

void MySerialImpl::flush() { logSerial.flush(); }

size_t MySerialImpl::printf(const char* format, ...) {
  va_list args;
  va_start(args, format);
  const size_t n = logSerial.vprintf(format, args);
  va_end(args);
  return n;
}

#endif
