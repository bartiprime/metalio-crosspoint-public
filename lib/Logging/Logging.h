#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
#include <HWCDC.h>
#endif

#include <string>

/*
Define ENABLE_SERIAL_LOG to enable logging
Can be set in platformio.ini build_flags or as a compile definition

Define LOG_LEVEL to control log verbosity:
0 = ERR only
1 = ERR + INF
2 = ERR + INF + DBG
If not defined, defaults to 0

If you have a legitimate need for raw Serial access (e.g., binary data,
special formatting), use the underlying logSerial object directly:
    logSerial.printf("Special case: %d\n", value);
    logSerial.write(binaryData, length);

The logSerial reference (defined below) points to the real Serial object and
won't trigger deprecation warnings.
*/

#ifndef LOG_LEVEL
#define LOG_LEVEL 0
#endif

// CP_DEBUG_LOG compiles the INF/DBG call sites in regardless of LOG_LEVEL, so the debug page's
// toggle can turn them on WITHOUT a rebuild (logPrintf() then decides at runtime). It rides on
// ENABLE_SERIAL_LOG because that is what makes logPrintf exist at all.
#if defined(CP_DEBUG_LOG) && !defined(ENABLE_SERIAL_LOG)
#error "CP_DEBUG_LOG requires ENABLE_SERIAL_LOG (logPrintf is what captures the lines)"
#endif

#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
static HWCDC& logSerial = Serial;
#define LOG_SERIAL_HAS_TX_TIMEOUT 1
#else
static HardwareSerial& logSerial = Serial;
#define LOG_SERIAL_HAS_TX_TIMEOUT 0
#endif

void logPrintf(const char* level, const char* origin, const char* format, ...);

#ifdef ENABLE_SERIAL_LOG
#if LOG_LEVEL >= 0
#define LOG_ERR(origin, format, ...) logPrintf("ERR", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_ERR(origin, format, ...)
#endif

// WRN (r111 PWR9): a warning that is visible without raising LOG_LEVEL. It is compiled in at
// EVERY level (the ERR guard above) because a warning is meant to reach the log in a debug build
// and vanish only when logging itself is compiled out -- see the #else branch, which defines it
// away exactly like LOG_ERR. Two lanes shipped call sites against a name that never existed;
// adding the macro is the fix, spelling it the way the rest of the tree does.
#define LOG_WRN(origin, format, ...) logPrintf("WRN", origin, format "\n", ##__VA_ARGS__)

#if LOG_LEVEL >= 1 || defined(CP_DEBUG_LOG)
#define LOG_INF(origin, format, ...) logPrintf("INF", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_INF(origin, format, ...)
#endif

#if LOG_LEVEL >= 2 || defined(CP_DEBUG_LOG)
#define LOG_DBG(origin, format, ...) logPrintf("DBG", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_DBG(origin, format, ...)
#endif
#else
#define LOG_DBG(origin, format, ...)
#define LOG_ERR(origin, format, ...)
#define LOG_WRN(origin, format, ...)
#define LOG_INF(origin, format, ...)
#endif

std::string getLastLogs();
void clearLastLogs();

// ---- runtime debug gate + SD capture -------------------------------------
// The board has no readable console, so a debug session has to end in a file. With CP_DEBUG_LOG the
// INF/DBG call sites exist in flash, and these functions decide at RUNTIME whether they cost
// anything. ERR always passes and always reaches the crash ring buffer.
void logSetRuntimeDebug(bool on);
bool logGetRuntimeDebug();
// ---- r114d: DRIVE-TIME EXCLUSION --------------------------------------------------------------
// The vendor excludes the log writer from panel refresh (epd_refresh.cc:240-249): a flash write
// during a scan can pause cache/other-core execution and starve the LCD bounce refill, dropping
// rows. logPrintf() only ever appends to the RAM ring, so the thing to exclude is the DRAIN:
// while a drive is active logSdFlush() refuses, and turning the gate off drains the ring right
// away. Nothing is lost beyond what the ring already evicts (it keeps the newest lines and says so
// in the file); ordering is untouched because the ring is append-only.
// Called from the panel driver through metalio_vendor_panel_set_drive_gate_hook.
void logSetDriveActive(bool on);
bool logDriveActive();
// Capture buffer: filled by logPrintf(), drained only from the UI or the idle path so an SD write
// can never land inside a time-critical section.
size_t logSdPending();
bool logSdFlush();
// Caps /.crosspoint/debug.log at 128 KB by rolling it to debug.log.old (same mechanism
// sleep.txt uses). Call before appending to that path from anywhere OTHER than
// logSdFlush(), which already calls it. True = safe to append. If the roll cannot be
// verified the file is KEPT (a cap must never cost the session's history).
bool logSdRollIfOversized();
bool logSdClear();
size_t logSdFileSize();
// ---- r107 P5: the ROLLED generation (debug.log.old) -------------------------------------------
// Rotation already existed; this is the read path for it. `logtail --old` addresses the same
// file, `loginfo` prints both generations' existence/size plus the cap, and `logs clear` removes
// BOTH. logSdMaxBytes() exposes the cap so the shell can print it instead of restating a number.
size_t logSdMaxBytes();
size_t logSdOldFileSize();
bool logSdOldExists();
// ---- r107 P7: `logs clear` -- the ONLY sanctioned log deletion, never implicit ----------------
// Removes /…/debug.log AND /…/debug.log.old after dropping the RAM ring, then reopens debug.log
// with a fresh header (same handle hazard as rotation: nothing holds a handle between flushes,
// and the ring is dropped first so a pending flush cannot recreate the file it just deleted).
bool logSdClearBoth();
void logSdAutoFlush();
// APPEND-ONLY session marker for /.crosspoint/debug.log, used by the USB shell lifecycle.
// Writes one line naming the event and the size it found, then verifies the file did not shrink;
// a shrink is reported as LOG_ERR. There is no truncating open on this path.
void logSdSessionNote(const char* what);
// READ-ONLY view of the RAM capture ring: the bytes logPrintf() has buffered and logSdFlush() has
// NOT yet written to /.crosspoint/debug.log. This is the only place a not-yet-flushed line exists,
// which is why `logtail` (a file tail) cannot show boot-time lines that were never flushed.
// Returns nullptr (and *lenOut = 0) when nothing is buffered. The buffer is NOT NUL-terminated and
// is appended by whichever task logs, so treat it as a snapshot: the tail may grow while reading.
const char* logRingData(size_t* lenOut);
// Read-only copy of the RTC log ring, oldest line first, at most `maxLines`
// entries of `stride` bytes each in `out`. Returns the number of lines copied.
// Exists for the boot-side post-mortem dump: HalSystem::begin() clears the ring
// early in an ordinary boot, so the drive-mode instrument has to see it first.
size_t logRtcRingCopy(char* out, size_t stride, size_t maxLines);
// Validates the RTC log state (magic word + logHead range). Returns true if
// corruption was detected (magic mismatch or logHead out of range), meaning
// logMessages is untrusted garbage. Callers should call clearLastLogs() when
// this returns true so getLastLogs() does not dump corrupt data into crash reports.
bool sanitizeLogHead();

class MySerialImpl : public Print {
 public:
  void begin(unsigned long baud) { logSerial.begin(baud); }

  // Support boolean conversion for compatibility with code like:
  //   if (Serial) or while (!Serial)
  operator bool() const { return logSerial; }

  __attribute__((deprecated("Use LOG_* macro instead"))) size_t printf(const char* format, ...);
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buffer, size_t size) override;
  void flush() override;
  static MySerialImpl instance;
};

#ifdef Serial
#undef Serial
#endif
#define Serial MySerialImpl::instance
