#include "Logging.h"

#include <BoardConfig.h>
#include <HalStorage.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>

#include <cstring>

#include <string>

#define MAX_ENTRY_LEN 256
#define MAX_LOG_LINES 16

// Simple ring buffer log, useful for error reporting when we encounter a crash
RTC_NOINIT_ATTR char logMessages[MAX_LOG_LINES][MAX_ENTRY_LEN];
RTC_NOINIT_ATTR size_t logHead = 0;
// Magic word written alongside logHead to detect uninitialized RTC memory.
// RTC_NOINIT_ATTR is not zeroed on cold boot, so logHead may appear in-range
// (0..MAX_LOG_LINES-1) by chance even though logMessages is garbage. The magic
// value is only set by clearLastLogs(), so its absence means the buffer was
// never properly initialized.
RTC_NOINIT_ATTR uint32_t rtcLogMagic;
static constexpr uint32_t LOG_RTC_MAGIC = 0xDEADBEEF;

void addToLogRingBuffer(const char* message) {
  // Add the message to the ring buffer, overwriting old messages if necessary.
  // If the magic is wrong or logHead is out of range (RTC_NOINIT_ATTR garbage
  // on cold boot), clear the entire buffer so subsequent reads are safe.
  if (rtcLogMagic != LOG_RTC_MAGIC || logHead >= MAX_LOG_LINES) {
    memset(logMessages, 0, sizeof(logMessages));
    logHead = 0;
    rtcLogMagic = LOG_RTC_MAGIC;
  }
  strncpy(logMessages[logHead], message, MAX_ENTRY_LEN - 1);
  logMessages[logHead][MAX_ENTRY_LEN - 1] = '\0';
  logHead = (logHead + 1) % MAX_LOG_LINES;
}

// ---------------------------------------------------------------------------
// Runtime debug gate + SD capture buffer
// ---------------------------------------------------------------------------
static bool s_runtimeDebug = false;

void logSetRuntimeDebug(const bool on) { s_runtimeDebug = on; }
bool logGetRuntimeDebug() { return s_runtimeDebug; }

namespace {
constexpr const char* kSdLogPath = "/.crosspoint/debug.log";
// r107 P5/P7: the rolled generation, by name, in one place -- rotation, the read path
// (`logtail --old`), `loginfo` and `logs clear` all address the same file.
constexpr const char* kSdLogOldPath = "/.crosspoint/debug.log.old";
// Cap for the SD debug log. 64 KB is 4 generations of sleep.txt's 16 KB roll: the
// file is the WHOLE boot/session record (sleep.txt is only the standby journal), so
// it earns a bigger window, but it must not grow without bound.
// 128 KB (was 64 KB): an intermittent bug that takes a reproduction round to appear needs more
// than one boot of history, and the 64 KB cap rolled debug.log out from under the kOSync blank-screen
// hunt on 3 Nov 2026 (`logtail 400` returned 21 lines and the event was already gone). With the
// `.old` roll below, the on-card cost is bounded at 2 x this value.
constexpr size_t kSdLogMaxBytes = 128 * 1024;

// Capture buffer size. This used to be 3072 bytes -- about 30 lines -- which one swipe's worth of
// drag samples filled, after which every line was dropped and the dump carried
// "[capture buffer full, lines dropped]" instead of the evidence. It is allocated in PSRAM so a
// long session keeps its whole history without touching internal RAM; if PSRAM refuses, an eighth
// of the size is taken from the heap rather than logging nothing.
// Set CP_CAPTURE_LOG_KB=0 for a release image: nothing is allocated and the flush path compiles out.
#ifndef CP_CAPTURE_LOG_KB
#define CP_CAPTURE_LOG_KB 64
#endif
constexpr size_t kSdBufBytes = static_cast<size_t>(CP_CAPTURE_LOG_KB) * 1024;
// LOG2 -- DRAIN CADENCE. The ring used to be drained only after 3 s of IDLE, and never at all while
// the USB shell held the device awake (loop() took the shell branch instead). A debug session
// therefore filled the ring and then every new line was dropped: `logtail` froze (nothing was ever
// flushed), `logs` returned the identical window (the buffer could not change), and the session
// lost exactly the evidence it existed to collect. Drain from loop() on a bounded cadence instead,
// so "the device is awake" is enough for the card to keep up.
constexpr size_t kSdAutoFlushMinBytes = 1024;
constexpr unsigned long kSdAutoFlushMs = 5000;
// Hard floor: a quiet session that logs one line a minute must still reach the card. Without this
// the byte threshold could hold a 200-byte buffer for an entire session, which is the same "the
// newest logtail line never moves" symptom at a lower volume.
constexpr unsigned long kSdAutoFlushIdleMs = 30000;
char* s_sdBuf = nullptr;
size_t s_sdCap = 0;
size_t s_sdLen = 0;
bool s_sdDropped = false;
// r114d: set by the panel driver for the duration of a waveform scan; logSdFlush() refuses while
// it is true. volatile because it is written from the render task and read from whichever task
// drains the ring.
volatile bool s_driveActive = false;
unsigned long s_sdLastFlushMs = 0;
// r110: true once the debug.log roll has failed in this session, so the "roll failed; keeping N
// bytes" line is an ERROR exactly once and a DEBUG line after that. A read-only boot takes that
// branch on every flush (once per 5 s), which is how r109 filled the error ring with one sentence.
bool s_rollFailureLogged = false;

bool ensureSdBuffer() {
#if CP_CAPTURE_LOG_KB > 0
  if (s_sdBuf != nullptr) return true;
  s_sdBuf = static_cast<char*>(heap_caps_malloc(kSdBufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s_sdBuf != nullptr) {
    s_sdCap = kSdBufBytes;
  } else {
    s_sdCap = kSdBufBytes / 8;
    s_sdBuf = static_cast<char*>(malloc(s_sdCap));
    if (s_sdBuf == nullptr) {
      s_sdCap = 0;
      return false;
    }
  }
  return true;
#else
  return false;
#endif
}

// Buffer only: the caller is logPrintf(), which can run on any task. Writing the card here would put
// an SdFat transaction inside whatever section happened to log.
void sdCapture(const char* line) {
  if (line == nullptr) return;
  const size_t n = strlen(line);
  if (n == 0) return;
  if (!ensureSdBuffer()) return;
  if (n > s_sdCap) {
    // One line can never fit; keep the ring intact rather than half-writing it.
    s_sdDropped = true;
    return;
  }
  if (s_sdLen + n > s_sdCap) {
    // LOG2: KEEP THE NEWEST, DROP THE OLDEST. This used to drop the line being written once the
    // ring was full, so the buffer content never changed again -- `logs` served the identical
    // window on every call while the device was still logging. Evicting the oldest bytes keeps the
    // ring live, and it is why `logs` shows what just happened even when a flush is failing.
    size_t drop = s_sdLen + n - s_sdCap;
    while (drop < s_sdLen && s_sdBuf[drop] != '\n') ++drop;  // never start the ring mid-line
    if (drop < s_sdLen) ++drop;                              // and carry its newline with it
    memmove(s_sdBuf, s_sdBuf + drop, s_sdLen - drop);
    s_sdLen -= drop;
    s_sdDropped = true;
  }
  memcpy(s_sdBuf + s_sdLen, line, n);
  s_sdLen += n;
}
}  // namespace

size_t logSdPending() { return s_sdLen; }

// Read-only accessor for the shell's `logs` command. Deliberately returns the raw buffer instead of
// a copy: the ring is up to 64 KB and the caller (a fixed-size Text frame) only wants the tail.
const char* logRingData(size_t* lenOut) {
  if (lenOut != nullptr) *lenOut = s_sdLen;
  if (s_sdBuf == nullptr || s_sdLen == 0) return nullptr;
  return s_sdBuf;
}

// Rolling cap for /.crosspoint/debug.log.
//
// This file grew to 774 KB (measured live) because NOTHING capped it: every
// logSdFlush(), every usbmscjournal line and every boot's UISCALE block appends.
// sleep.txt has rolled at 16 KB for rounds, so reuse THAT mechanism rather than
// inventing a second one: move the current file to `<path>.old` (removing the
// previous .old first) and start a fresh file. The newest lines are therefore
// never the ones destroyed -- they are the whole content of .old -- which is what
// a postmortem of the run that just hung actually needs.
bool logSdRollIfOversized() {
  if (!Storage.ready()) return false;  // volume parked/unmounted: nothing to size
  HalFile f = Storage.open(kSdLogPath);
  if (!f) return true;  // nothing there (or no card): nothing to roll
  const size_t n = f.size();
  f.close();
  if (n <= kSdLogMaxBytes) return true;
  Storage.remove(kSdLogOldPath);
  if (!Storage.rename(kSdLogPath, kSdLogOldPath)) {
    // LOG2: DO NOT DESTROY THE ONLY COPY TO ENFORCE A CAP. The old fallback was
    // `Storage.remove(kSdLogPath)`, so a rename this code cannot verify (a busy card, a FAT
    // quirk right after a remount) turned the cap into silent deletion -- exactly the shape
    // reported as "debug.log truncated to ~125 B after a sleep re-enable". Keeping a too-large
    // file costs bytes; deleting it costs the session.
    // r110 RATE LIMIT: on a read-only boot (r110 gates rename/remove when the volume is unmarked)
    // this branch is taken by EVERY flush -- once per 5 s, forever -- and r109 turned the error
    // ring into the same sentence repeated. The first failure per session says why in the glass
    // log; the rest go to DEBUG, where a debug session can still see them.
    if (!s_rollFailureLogged) {
      LOG_ERR("LOG", "debug.log roll failed; keeping %u bytes rather than deleting history",
              static_cast<unsigned>(n));
      s_rollFailureLogged = true;
    } else {
      LOG_DBG("LOG", "debug.log roll still failing; keeping %u bytes (first failure already logged)",
              static_cast<unsigned>(n));
    }
    return false;
  }
  return true;
}

bool logSdFlush() {
  if (s_sdLen == 0 || s_sdBuf == nullptr) return false;
  // r114d: no flash write while a waveform is clocking (see logSetDriveActive).
  if (s_driveActive) return false;
  // Parked (deep-sleep trim) or not mounted: keep the lines in RAM instead of failing a write.
  if (!Storage.ready()) return false;
  logSdRollIfOversized();
  Storage.mkdir("/.crosspoint");
  const bool fresh = !Storage.exists(kSdLogPath);
  HalFile f = Storage.open(kSdLogPath, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return false;
  if (fresh) {
    // Self-describing header: a pasted log has to identify the firmware and the run it came from.
#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "crosspoint-unknown"
#endif
    char header[96];
    const int n = snprintf(header, sizeof(header), "--- boot %.48s uptime=%lu ms ---\n", CROSSPOINT_VERSION,
                           static_cast<unsigned long>(millis()));
    if (n > 0) f.write(header, static_cast<size_t>(n));
  }
  const size_t written = f.write(s_sdBuf, s_sdLen);
  if (s_sdDropped) {
    constexpr const char kNote[] = "... [capture ring: oldest lines dropped to make room]\n";
    f.write(kNote, sizeof(kNote) - 1);
  }
  f.flush();
  f.close();
  if (written == 0) return false;
  if (written < s_sdLen) {
    // LOG2: A SHORT WRITE IS NOT A FLUSH. This used to clear the whole ring after a single
    // successful byte, silently losing the rest of the session. Keep the unwritten tail and let
    // the next drain retry it.
    memmove(s_sdBuf, s_sdBuf + written, s_sdLen - written);
    s_sdLen -= written;
    s_sdDropped = false;
    return false;
  }
  s_sdLen = 0;
  s_sdDropped = false;
  return true;
}

// r114d: the drive-time exclusion gate. `on` is held for the length of one waveform scan; when it
// is released the ring is drained immediately, so the lines the drive held back (including the
// `[WAKE] drive N` capture rows and the `[panel]` rows for that drive) reach the card right after
// it, in the order they were logged. Never blocks, never allocates, safe from any task.
void logSetDriveActive(bool on) {
  if (on) {
    s_driveActive = true;
    return;
  }
  s_driveActive = false;
  if (s_sdLen != 0) logSdFlush();
}

bool logDriveActive() { return s_driveActive; }

bool logSdClear() {
  s_sdLen = 0;
  s_sdDropped = false;
  if (!Storage.exists(kSdLogPath)) return true;
  return Storage.remove(kSdLogPath);
}

size_t logSdFileSize() {
  HalFile f = Storage.open(kSdLogPath);
  if (!f) return 0;
  const size_t n = f.size();
  f.close();
  return n;
}

// r107 P5: the rolled generation, exposed. Rotation already existed (logSdRollIfOversized);
// what was missing was a read path and a way to ask "did it roll?".
size_t logSdMaxBytes() { return kSdLogMaxBytes; }

size_t logSdOldFileSize() {
  HalFile f = Storage.open(kSdLogOldPath);
  if (!f) return 0;
  const size_t n = f.size();
  f.close();
  return n;
}

bool logSdOldExists() { return Storage.exists(kSdLogOldPath); }

// r107 P7: `logs clear` -- the ONLY sanctioned log deletion, and it is never implicit.
//
// The handle hazard is the same one rotation has: a delete or a rename against a path that is
// still open is the shape that cost a session on r105. Nothing here holds a handle between
// flushes (logSdFlush() opens, writes, flushes and closes), and this runs on the shell task with
// no flush in flight, so the safe sequence is:
//   drop the RAM ring (so a pending flush cannot recreate debug.log straight after the delete),
//   remove BOTH generations by name, then
//   reopen debug.log with O_CREAT|O_APPEND and write a fresh self-describing header,
// so the reader finds a valid, empty file rather than a missing one.
bool logSdClearBoth() {
  s_sdLen = 0;  // dropped deliberately: the caller asked for both generations gone
  s_sdDropped = false;
  if (!Storage.ready()) return false;
  bool ok = true;
  if (Storage.exists(kSdLogPath) && !Storage.remove(kSdLogPath)) ok = false;
  if (Storage.exists(kSdLogOldPath) && !Storage.remove(kSdLogOldPath)) ok = false;
  // reopen (do not leave the path missing) and stamp the header, so the next append works and
  // the file still identifies the firmware that created it.
  HalFile f = Storage.open(kSdLogPath, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return false;
#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "crosspoint-unknown"
#endif
  char header[128];
  const int n = snprintf(header, sizeof(header), "--- boot %.40s uptime=%lu ms --- (logs clear: both generations removed)\n",
                         CROSSPOINT_VERSION, static_cast<unsigned long>(millis()));
  if (n > 0) f.write(header, static_cast<size_t>(n));
  f.flush();
  f.close();
  return ok;
}

void logSdAutoFlush() {
  // LOG2: called from loop() on EVERY iteration (main task only -- never from a log call, so an SD
  // write can never land inside a time-critical section). The old version ran only inside the
  // 3-second-idle branch, which a debug session never reaches: the shell holds the device awake,
  // reading and logging keep lastActivityTime fresh, and the ring then filled and froze both
  // sinks. Bounded cadence, not "whenever idle": at most one write per 5 s when the buffer holds
  // at least 4 KB, or immediately when a burst reaches 3/4 of the cap.
  if (s_sdCap == 0 || s_sdLen == 0) return;
  if (!Storage.ready()) return;
  const unsigned long now = millis();
  const bool due = s_sdLen >= (s_sdCap * 3) / 4 ||
                   (s_sdLen >= kSdAutoFlushMinBytes && (now - s_sdLastFlushMs) >= kSdAutoFlushMs) ||
                   (now - s_sdLastFlushMs) >= kSdAutoFlushIdleMs;
  if (!due) return;
  logSdFlush();
  s_sdLastFlushMs = now;  // even on failure: a stuck card must not be retried every 10 ms
}

// LOG2: an append-only session marker. Called when the USB debug shell is enabled/disabled, i.e.
// exactly at the moment the operator reported the card log being replaced by a fresh ~125-byte file.
// It makes that moment visible by name and by size in the file itself, and it opens the path with
// O_APPEND only -- there is no truncating open anywhere on this path.
void logSdSessionNote(const char* what) {
  if (!Storage.ready()) return;
  logSdRollIfOversized();
  Storage.mkdir("/.crosspoint");
  const size_t before = logSdFileSize();
  HalFile f = Storage.open(kSdLogPath, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return;
  char line[160];
  const int n = snprintf(line, sizeof(line), "[%lu] [INF] [LOG] %s (debug.log was %u B)\n", millis(),
                         what != nullptr ? what : "?", static_cast<unsigned>(before));
  if (n > 0) f.write(line, static_cast<size_t>(n));
  f.flush();
  f.close();
  const size_t after = logSdFileSize();
  if (before > 0 && after > 0 && after < before) {
    // The invariant this exists to prove: an append must never shrink the file. Say so loudly --
    // an error line in the RTC ring survives even if the card is the thing misbehaving.
    LOG_ERR("LOG", "debug.log SHRANK across '%s': %u -> %u bytes", what, static_cast<unsigned>(before),
            static_cast<unsigned>(after));
  }
}

// Since logging can take a large amount of flash, we want to make the format string as short as possible.
// This logPrintf prepend the timestamp, level and origin to the user-provided message, so that the user only needs to
// provide the format string for the message itself.
void logPrintf(const char* level, const char* origin, const char* format, ...) {
  // RUNTIME GATE, first statement: with CP_DEBUG_LOG the INF/DBG call sites are compiled in, and this
  // is what makes them free when the debug page's switch is off -- the format work is skipped, not
  // just the output. ERR is never gated: it is the crash breadcrumb the ring buffer exists for.
  if (!s_runtimeDebug && level[0] != 'E') return;

  va_list args;
  va_start(args, format);
  char buf[MAX_ENTRY_LEN];
  char* c = buf;
  // add timestamp, level and origin
  {
    unsigned long ms = millis();
    int len = snprintf(c, sizeof(buf), "[%lu] [%s] [%s] ", ms, level, origin);
    // error while writing => return
    if (len < 0) {
      va_end(args);
      return;
    }
    // clamp c to be in buffer range
    c += std::min(len, MAX_ENTRY_LEN);
  }
  // add the user message
  {
    int len = vsnprintf(c, sizeof(buf) - (c - buf), format, args);
    if (len < 0) {
      va_end(args);
      return;
    }
  }
  va_end(args);
#if FREEINK_LOG_TRANSPORT == FREEINK_LOG_TRANSPORT_ROM_PRINTF
  // Sticky's USB serial bridge uses UART0; ROM output also works before Serial0.begin().
  esp_rom_printf("%s", buf);
#else
  // Write even when `logSerial` reports disconnected: after a brief SOF-watchdog flap
  // (common at the 10 MHz low-power clock) HWCDC keeps `connected` false until its next
  // TX interrupt. write() queues without blocking in that state and re-arms that interrupt.
  logSerial.print(buf);
#endif
  addToLogRingBuffer(buf);
  if (s_runtimeDebug) sdCapture(buf);
}

std::string getLastLogs() {
  if (rtcLogMagic != LOG_RTC_MAGIC) {
    return {};
  }
  std::string output;
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    size_t idx = (logHead + i) % MAX_LOG_LINES;
    if (logMessages[idx][0] != '\0') {
      const size_t len = strnlen(logMessages[idx], MAX_ENTRY_LEN);
      output.append(logMessages[idx], len);
    }
  }
  return output;
}

// Checks whether the RTC log state is consistent: rtcLogMagic must equal
// LOG_RTC_MAGIC and logHead must be in 0..MAX_LOG_LINES-1. Returns true if
// corruption is detected, in which case rtcLogMagic is still invalid and
// logMessages may contain garbage. Callers (e.g. HalSystem::begin on the
// panic-reboot path) must call clearLastLogs() after a true result to fully
// reinitialize the ring buffer and stamp the magic before getLastLogs() is used.
size_t logRtcRingCopy(char* out, size_t stride, size_t maxLines) {
  if (out == nullptr || stride == 0 || maxLines == 0) return 0;
  if (rtcLogMagic != LOG_RTC_MAGIC || logHead >= MAX_LOG_LINES) return 0;  // uninitialized RTC memory
  size_t n = 0;
  for (size_t i = 0; i < MAX_LOG_LINES && n < maxLines; i++) {
    const char* src = logMessages[(logHead + i) % MAX_LOG_LINES];
    if (src[0] == '\0') continue;
    char* dst = out + n * stride;
    const size_t len = strnlen(src, MAX_ENTRY_LEN);
    const size_t copy = len < stride - 1 ? len : stride - 1;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
    n++;
  }
  return n;
}

bool sanitizeLogHead() {
  if (rtcLogMagic != LOG_RTC_MAGIC || logHead >= MAX_LOG_LINES) {
    logHead = 0;
    return true;
  }
  return false;
}

void clearLastLogs() {
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    logMessages[i][0] = '\0';
  }
  logHead = 0;
  rtcLogMagic = LOG_RTC_MAGIC;
}
