#pragma once

// ---------------------------------------------------------------------------
// CP_USB_SHELL — phase 1 of docs/engineering/USB_DEBUG_CHANNEL.md: an
// esp_console REPL on the USB-Serial/JTAG console the logs already use.
//
// THREADING CONTRACT (design doc §3.2) — the reason this file exists:
//   * The shell task owns the byte pipe and NOTHING else. It parses, and either
//     answers locally (help/ver/logtail/loglevel) or posts a request and blocks
//     for the answer.
//   * The main loop is the only context that may touch activityManager, the
//     renderer, RenderLock, the panel driver's state or Storage while a volume
//     is host-mounted. usbshell::drain() is called from exactly one place —
//     the top of loop(), before activityManager.loop() — so every command is
//     serialized against every gesture, page turn and sleep decision for free.
//   * A command that stops the world (reboot) only sets a deadline; loop()
//     acts on it one iteration later, after the response has gone out.
//   * A command that wedges must never hold the pipe: the shell task waits with
//     a timeout (2 s default, 30 s for commands that declare themselves slow)
//     and returns EBUSY instead of spinning.
//
// OUTPUT FRAMING (machine driving). Every command prints
//     <<<BEGIN <id> cmd=<name>
//     ...payload...
//     <<<END <id> rc=<n> ms=<n>
// so a host can parse a response out of interleaved log spam. <id> comes from
// "--id <x>" / "-i <x>" in the command line, else an incrementing counter.
// Anything outside the markers is device telemetry, not this command's output.
// ---------------------------------------------------------------------------

#include <cstddef>

namespace usbshell {

// True when CP_USB_SHELL compiled the shell into this image.
bool available();

// True when the REPL task is running.
bool running();

// ----- r91 LIFECYCLE (docs/engineering/USB_DEBUG_CHANNEL.md, "r91 lifecycle rules") -----
//
// The EFFECTIVE state is  persisted && session:
//   * persisted = SETTINGS.usbDebugShell, survives a power cycle (a preference);
//   * session   = a volatile flag that starts FALSE on every boot (R2) and is
//                 cleared by a manual sleep (R3) and by a confirmed power-off (R4).
// R3/R4 clear the SESSION only; the persisted preference stays as the user left
// it, so the menu row reads "ON (inactive)" until the shell is re-toggled.
// Clearing the session also stops the REPL, so the console reverts to log-only.
bool sessionEnabled();

// THE ONE PREDICATE for power policy (R1). True only while the shell is both
// session-enabled and actually running. Every automatic power-down path asks
// this and nothing else -- do not test SETTINGS.usbDebugShell anywhere else.
bool keepsDeviceAwake();

// Enable/disable the session. enableSession() sets the flag and starts the REPL,
// returning whether it came up; disableSession() clears the flag and stops it.
// Both are safe from loop() and from the Debug page (main task).
bool enableSession();
void disableSession();

// R2/R3/R4/R7: drop the session and, if the REPL is up, stop it. `why` is logged.
// Session flag only; the persisted preference is untouched, so the Debug row reads
// "ON (inactive)" and one tap brings the shell back.
void clearSession(const char* why);

// R6: drop the session AND clear the persisted preference, so the toggle reads OFF
// and nothing re-enables the shell when drive mode exits.
void deactivateAndPersist(const char* why);

// Start/stop the REPL. Idempotent; both are safe to call from loop() (main
// task) and from the Debug page's toggle. stop() deinitialises the console,
// frees linenoise's history and its command table, and puts the USB-Serial/JTAG
// reader back into its non-blocking mode, so plain log output keeps working.
bool start();
bool stop();

// The one drain point. Call at the top of loop(), before activityManager.loop().
// Executes at most one queued UI request and answers its caller.
void drain();

// Forensics fed from main.cpp's existing hooks:
//  * notePanelJournal(): every line the vendor panel shim emits per show()
//    ("[panel] show mode=... tps=... err=... ms=..."), i.e. the last RESOLVED
//    mode and the drive's own timing.
//  * notePanelDriveMs(): the entry->exit duration of the last blocking waveform,
//    measured at the poll hook main.cpp already installs.
//
// r96.1 D3: the duration and the AGE of that drive are two different numbers, and
// they were conflated. `lastPanelDriveMs()` is a DURATION (~350-990 ms for one of our
// drives); anything asking "did a drive just finish?" needs the age since it ended.
// `lastPanelDriveAgeMs()` is that age, stamped at the same instant the duration is
// noted. Both are reported; a caller that wants a duration must not ask for an age.
void notePanelJournal(const char* line);
void notePanelDriveMs(unsigned long ms);
const char* lastPanelJournal();
unsigned long lastPanelDriveMs();
// Milliseconds since the last blocking waveform ENDED, or ULONG_MAX when no drive has
// been noted this boot (age is then unknowable -- callers must treat it as "long ago",
// never as 0, which would read as "a drive just finished").
unsigned long lastPanelDriveAgeMs();

// UI-side implementations, installed once by main.cpp. Every one of these runs
// inside drain(), i.e. on the main task, never on the shell task. Each returns
// 0 on success or a negative errno (-ENODEV, -EINVAL, -EBUSY, -EIO) and writes
// its human-readable result into out (always NUL-terminated).
struct UiOps {
  int (*panel)(char* out, size_t cap);
  int (*pmic)(char* out, size_t cap);
  int (*refresh)(const char* mode, char* out, size_t cap);
  int (*shot)(const char* path, char* out, size_t cap);
  int (*activity)(char* out, size_t cap);
  int (*spine)(char* out, size_t cap);
  // Only arms the reboot; loop() runs it ~1 s later so the response is out first.
  void (*reboot)(char* out, size_t cap);
  // r96. Both run on the main task. `args` is the command line after the command
  // name, space-joined (so `ppabench rot90 1216 684 fb` arrives whole).
  //   lpsleep  arms ONE bounded light-sleep slice and answers BEFORE the device
  //            sleeps; loop() executes it (lpcore::pollSleepSlice). r96.2: the LP
  //            core is stopped for the slice and the RTC timer is re-armed every
  //            <=5 s, so no single sleep call can be unbounded.
  //   ppabench runs one full-frame PPA operation; it takes RenderLock itself for
  //            the `fb` source, which is why it cannot run on the shell task.
  int (*lpsleep)(const char* args, char* out, size_t cap);
  int (*ppabench)(const char* args, char* out, size_t cap);
  // PPA1: `ppaframe [plan|cmp|time] [op] [n]`. Main task like ppabench, because
  // the plan and the comparison are both about the LIVE canvas, which is read
  // under RenderLock.
  int (*ppaframe)(const char* args, char* out, size_t cap);
  // r101.21 EXP1: `exp ...` — the runtime experiment arms. Main task for the same
  // reason as the two above: `exp vid` drives the panel (`exp status` only reads
  // RAM, but one command family with one entry point cannot disagree with itself).
  // With CP_EXP=OFF this field and its command do not exist.
  int (*exp)(const char* args, char* out, size_t cap);
  // E5 (HARDWARE_POWER_PLAN §5.4): `touch status|recover`. Main task because the
  // ladder drives the TCA expander (rail P5 + TP_RST) and the shared I2C bus.
  // Always present in the shipping image — recovery must not need a bench build.
  int (*touch)(const char* args, char* out, size_t cap);
};

void installUiOps(const UiOps& ops);

}  // namespace usbshell
