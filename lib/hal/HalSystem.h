#pragma once

#include <cstdint>
#include <string>

namespace HalSystem {
struct StackFrame {
  uint32_t sp;
  uint32_t spp[8];
};

void begin();

// Dump panic info to SD card if necessary
void checkPanic();
void clearPanic();

std::string getPanicInfo(bool full = false);
bool isRebootFromPanic();

// r163 3.11: the reset cause this boot came from, as one short line ("PANIC (exception/abort)",
// "TASK_WDT", "BROWNOUT", ...). esp_reset_reason() -- not a stored record -- because it is the
// one thing that survives every reset class including the ones where no panic hook runs (a
// lockup or a hardware watchdog), and the crash screen needs exactly that when the panic
// message is empty. Never NULL.
const char* resetReasonText();
}  // namespace HalSystem
