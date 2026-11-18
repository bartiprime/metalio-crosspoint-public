#pragma once

// Panel/PMIC probes behind the USB shell's `panel` and `pmic` commands.
//
// Both are READ-ONLY: no TPS65185 register is ever written here (design doc
// §3.5 — a write from a second context would race the panel sequence and the
// power-manager locks). They run inside usbshell::drain(), i.e. on the main
// task between loop iterations, with the bus mutex held per transfer.

#include <cstddef>

namespace panelprobe {

// int return: 0 ok, negative errno-ish on failure. Always NUL-terminates `out`.
int panelText(char* out, size_t cap);
int pmicText(char* out, size_t cap);

}  // namespace panelprobe
