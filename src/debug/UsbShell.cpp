// CP_USB_SHELL — an esp_console REPL on the console port the logs already use.
//
// Design: docs/engineering/USB_DEBUG_CHANNEL.md (§2.1 option C, §3.1-3.2, §4).
//
// WHAT IT IS NOT
//   * It does not move the console off USB-Serial/JTAG and does not open a
//     second CDC. The board has ONE connector time-shared by GPIO36 (ANA_SW),
//     so a composite cannot coexist with this console. Phase 2 is the OTG
//     composite; this command table and this mailbox carry over unchanged.
//   * The shell task never touches UI state. See UsbShell.h for the contract.
//
// THE COST, ACCEPTED UP FRONT
//   The prompt and command output interleave with ESP_LOG / logPrintf spam —
//   they share one pipe. That is why every response is framed between
//   <<<BEGIN id cmd=name>>> and <<<END id rc= ms=>>> markers: the frame is
//   parseable out of the noise (tools/cpd.py does exactly that), and this is
//   the only structural mitigation phase 1 has.

#include "debug/UsbShell.h"

#include "platform/PlatformFeatures.h"

#if CP_USB_SHELL

#include <Arduino.h>
#include <BoardConfig.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalTiltSensor.h>
#include <SDCardManager.h>
#include <WiFi.h>
#include <Wire.h>
#include <Logging.h>
#include <esp_attr.h>
#include <esp_console.h>
#include <esp_err.h>  // TCH5a: esp_err_to_name on the live touch-id read failure
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

// r91 lifecycle: the persisted half of the effective state (SETTINGS.usbDebugShell).
#include "CrossPointSettings.h"
#include <PersistableStore.h>
#include "JpegToBmpConverter.h"    // JPEG1 `jpeg compare`: the two-decoder comparison
#include "JpegHwDecode.h"          // JPEG1: hardware decoder arm state + counters
#include "JpegHwPath.h"            // JPEG1: reason names and the raster geometry

// r96 bench readouts. CpuStats is the FreeRTOS run-time-stats reader; LpCoreService
// and PpaService own the two accelerator benches.
#include "platform/CpuStats.h"
#include "platform/LpCoreService.h"
#include "platform/PpaService.h"
#include "platform/StandbyTrims.h"
#include "audio/BtAudioOutput.h"  // TTS1 `audio`: the BH1098G output path
#include "audio/UiSounds.h"      // AUDIO1 `uisound`: the short UI feedback clicks
#include "platform/HybridClock.h"  // Build A item C: stored drift factor + the park mark `batt delta` reads
#include "platform/ParkSinks.h"    // r106 `sd` / `radio`: what the park sinks last did
// r128: SdPowerCycle.h is not included -- r104 has no 150/50 ms dwell around a card power cycle.
#include "activities/RenderLock.h"
#include "debug/BenchDrives.h"  // BENCH1: the `benchdrives` harness this file only ARMS
#include "util/BatteryGaugeFix.h"  // r98.2: validated DM reads for the captions below
#include "system/ChargerCurrent.h"  // step 2.5 item 8: the charger's own ICHG field
#include "system/ChargeIdlePolicy.h"  // r163 3.5 + E6 `needs`: the charge policy's mode
#include "system/PowerNeed.h"         // r163 E6: the need table `needs` reports
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
#include "epd_board_metalio_eink4_plus.h"  // epd_board_..._pa_rail_set: the standby trim's own call
#endif
#include "metalio_touch.h"  // TCH5a: live FT6336U chip id / firmware read for the `ver` line

namespace {

// ---------------------------------------------------------------------------
// Output text. Fixed buffer, no allocation: a command must not fail on a
// fragmented heap — that is usually the thing being debugged.
// ---------------------------------------------------------------------------
struct Text {
  char buf[1280];
  size_t len = 0;

  void clear() {
    len = 0;
    buf[0] = '\0';
  }
  void put(const char* s) {
    if (s == nullptr) return;
    const size_t n = strlen(s);
    append(s, n);
  }
  void append(const char* s, size_t n) {
    if (len + n >= sizeof(buf)) {
      // Truncate rather than overflow, and say so — a silently short answer is
      // what makes a debug channel untrustworthy.
      static const char kTrunc[] = "...[truncated]\n";
      const size_t room = sizeof(buf) - len - 1;
      const size_t keep = room > sizeof(kTrunc) ? room - sizeof(kTrunc) : 0;
      if (keep) {
        memcpy(buf + len, s, keep);
        len += keep;
      }
      if (sizeof(buf) - len > sizeof(kTrunc)) {
        memcpy(buf + len, kTrunc, sizeof(kTrunc) - 1);
        len += sizeof(kTrunc) - 1;
      }
      buf[len] = '\0';
      return;
    }
    memcpy(buf + len, s, n);
    len += n;
    buf[len] = '\0';
  }
  void outf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    append(tmp, static_cast<size_t>(n) < sizeof(tmp) ? static_cast<size_t>(n) : sizeof(tmp) - 1);
  }
};

// ---------------------------------------------------------------------------
// Request mailbox. Depth 4 (the design says 8; 4 is what a one-command-at-a-time
// pipe can actually fill, and every slot is a static 800-byte buffer).
// ---------------------------------------------------------------------------
enum class Kind : uint8_t { None, Panel, Pmic, Refresh, Shot, Activity, Spine, Reboot, LpSleep, PpaBench, PpaFrame, Exp, Touch };

// (r97: kindName(Kind) lived here, defined and never called -- it was absent from the ELF.
// The slot carries its request text in `arg`, so a name table was never needed.)

enum class SlotState : uint8_t { Idle, Queued, InFlight, Done };

struct Slot {
  SlotState state = SlotState::Idle;
  Kind kind = Kind::None;
  char arg[112] = {};
  uint32_t id = 0;
  int rc = 0;
  bool abandoned = false;
  TaskHandle_t caller = nullptr;
  char out[768] = {};
};

constexpr int kSlots = 4;
Slot s_slots[kSlots];
Slot* s_ring[kSlots] = {};
int s_head = 0;
int s_tail = 0;
int s_n = 0;
SemaphoreHandle_t s_mboxMutex = nullptr;

usbshell::UiOps s_ops{};

char s_lastPanelLine[160] = {};
volatile unsigned long s_lastDriveMs = 0;
// r96.1 D3: when that drive ended, so "just finished?" can be answered as an AGE.
// 0 = no drive noted since boot (see lastPanelDriveAgeMs()).
volatile unsigned long s_lastDriveAtMs = 0;
volatile bool s_running = false;
esp_console_repl_t* s_repl = nullptr;

// ---------------------------------------------------------------------------
// r91 LIFECYCLE (docs/engineering/USB_DEBUG_CHANNEL.md, "r91 lifecycle rules")
//
// The EFFECTIVE state is  persisted && s_session.  s_session is volatile, starts
// false, and is restored after a WARM reset only (R7) -- see sessionInit().
// ---------------------------------------------------------------------------
volatile bool s_session = false;
bool s_sessionInitDone = false;

// Survives a warm reset in the RTC domain and dies on a true power cycle (and on a
// brownout that takes the RTC domain with it). The magic + one bit is the whole
// marker: nothing else is trusted after a reset.
RTC_NOINIT_ATTR uint32_t s_rtcMagic;
RTC_NOINIT_ATTR uint32_t s_rtcSession;
constexpr uint32_t kRtcMagic = 0x43505239u;  // "CPR9"

// R5 ("toggle off on USB disconnect") was DROPPED by the operator as not worth the
// unreliability. Consequence, documented in USB_DEBUG_CHANNEL.md: an unplugged device
// with the shell enabled stays awake (R1) until the operator turns it off, sleeps,
// powers off, enters drive mode, or cold-boots.

const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    // 11 on this SoC. NOT an exotic vendor code: esp_reset_reason_t continues past
    // SDIO, and on ESP32-S31 reset_reason.c maps RESET_REASON_CORE_USB_UART and
    // RESET_REASON_CORE_USB_JTAG to it. The USB-Serial/JTAG peripheral itself resets
    // the chip on the host's DTR/RTS line transitions, so ANY pyserial open/close of
    // COM4 that moves those lines reboots the board with this reason -- which is why
    // the operator saw the uptime go backwards around probes.
    case ESP_RST_USB: return "usb (USB-Serial/JTAG line reset)";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "unknown";
  }
}

// R7: which reset reasons may KEEP the session. Everything that is not a power-on is
// treated as warm, because those resets happen mid-session and the operator asked for
// "reboot from the shell and keep debugging". Deliberate exceptions, both conservative:
//   * ESP_RST_POWERON / ESP_RST_EXT / ESP_RST_DEEPSLEEP -> cold: session OFF (R2).
//   * ESP_RST_JTAG / ESP_RST_EFUSE / ESP_RST_UNKNOWN -> OFF: after a flash, an efuse
//     fault or an unexplained reset the image may be different, so the operator
//     re-toggles.
//   * ESP_RST_BROWNOUT / ESP_RST_PWR_GLITCH -> warm, but the RTC marker is checked too:
//     if the brownout took the RTC domain the magic is gone and the session is OFF.
//   * ESP_RST_USB -> warm. This is the one r91 missed: the USB-Serial/JTAG peripheral
//     resets the chip whenever the host moves DTR/RTS (an ordinary pyserial open or
//     close of COM4), the RTC domain survives it, and treating it as cold meant the
//     session was thrown away by the very act of connecting -- the tool then talked to
//     a device whose shell had just been switched off. Cold boot is unaffected: a
//     power-on still reports ESP_RST_POWERON, and a real power cycle also loses the
//     RTC_NOINIT marker.
//   * ESP_RST_CPU_LOCKUP -> warm: a double exception is a crash, like a panic.
bool warmReset(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_SW:
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
    case ESP_RST_PWR_GLITCH:
    case ESP_RST_CPU_LOCKUP:
    case ESP_RST_USB:
      return true;
    default:
      return false;
  }
}

void noteSession(bool on) {
  s_rtcMagic = kRtcMagic;
  s_rtcSession = on ? 1u : 0u;
}

// Runs once, on the first lifecycle query (setup(), or the first drain).
void sessionInit() {
  if (s_sessionInitDone) return;
  s_sessionInitDone = true;
  const esp_reset_reason_t rr = esp_reset_reason();
  const bool markerValid = (s_rtcMagic == kRtcMagic);
  const bool keep = warmReset(rr) && markerValid && (s_rtcSession & 1u) != 0;
  s_session = keep;
  ESP_LOGI("shell", "lifecycle: reset=%s marker=%d persisted=%d session=%d", resetReasonName(rr),
           markerValid ? 1 : 0, static_cast<int>(SETTINGS.usbDebugShell), s_session ? 1 : 0);
  noteSession(s_session);
}

// R5: dropped by the operator -- see the note above. Deliberately no host-presence
// heuristic lives here: an idle host and an unplugged host cannot be told apart
// reliably enough to tear down a session on.

// Default 2 s for state reads; 30 s for the ones that drive the panel or the SD
// card (a FULL refresh is ~1.8 s on this panel, a shot writes 48 KB).
constexpr uint32_t kFastTimeoutMs = 2000;
constexpr uint32_t kSlowTimeoutMs = 30000;

void mboxInit() {
  if (s_mboxMutex == nullptr) s_mboxMutex = xSemaphoreCreateMutex();
}

// Posts a request and blocks for the answer. Returns 0, or -EBUSY on a full
// mailbox, or -ETIMEDOUT when the main loop did not answer in time.
int post(Kind kind, const char* arg, uint32_t timeoutMs, Text& out, int* rcOut) {
  mboxInit();
  if (s_mboxMutex == nullptr) return -ENOMEM;

  Slot* slot = nullptr;
  xSemaphoreTake(s_mboxMutex, portMAX_DELAY);
  if (s_n >= kSlots) {
    xSemaphoreGive(s_mboxMutex);
    return -EBUSY;
  }
  for (int i = 0; i < kSlots; ++i) {
    if (s_slots[i].state == SlotState::Idle) {
      slot = &s_slots[i];
      break;
    }
  }
  if (slot == nullptr) {
    xSemaphoreGive(s_mboxMutex);
    return -EBUSY;
  }
  slot->state = SlotState::Queued;
  slot->kind = kind;
  slot->rc = 0;
  slot->abandoned = false;
  slot->out[0] = '\0';
  slot->caller = xTaskGetCurrentTaskHandle();
  if (arg != nullptr) {
    strncpy(slot->arg, arg, sizeof(slot->arg) - 1);
    slot->arg[sizeof(slot->arg) - 1] = '\0';
  } else {
    slot->arg[0] = '\0';
  }
  s_ring[s_tail] = slot;
  s_tail = (s_tail + 1) % kSlots;
  ++s_n;
  xSemaphoreGive(s_mboxMutex);

  // Consume any stale notification before waiting, so a previous request's
  // completion cannot satisfy this one.
  ulTaskNotifyTake(pdTRUE, 0);
  if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeoutMs)) == 0) {
    xSemaphoreTake(s_mboxMutex, portMAX_DELAY);
    if (slot->state != SlotState::Done) {
      // Mark it abandoned: the main loop must not answer into a stack buffer
      // that no longer exists. drain() skips an abandoned slot.
      slot->abandoned = true;
      slot->caller = nullptr;
    }
    xSemaphoreGive(s_mboxMutex);
    return -ETIMEDOUT;
  }

  xSemaphoreTake(s_mboxMutex, portMAX_DELAY);
  out.put(slot->out);
  *rcOut = slot->rc;
  slot->state = SlotState::Idle;
  slot->caller = nullptr;
  xSemaphoreGive(s_mboxMutex);
  return 0;
}

int execute(Slot& s) {
  switch (s.kind) {
    case Kind::Panel:
      return s_ops.panel ? s_ops.panel(s.out, sizeof(s.out)) : -ENODEV;
    case Kind::Pmic:
      return s_ops.pmic ? s_ops.pmic(s.out, sizeof(s.out)) : -ENODEV;
    case Kind::Refresh:
      return s_ops.refresh ? s_ops.refresh(s.arg, s.out, sizeof(s.out)) : -ENODEV;
    case Kind::Shot:
      return s_ops.shot ? s_ops.shot(s.arg, s.out, sizeof(s.out)) : -ENODEV;
    case Kind::Activity:
      return s_ops.activity ? s_ops.activity(s.out, sizeof(s.out)) : -ENODEV;
    case Kind::Spine:
      return s_ops.spine ? s_ops.spine(s.out, sizeof(s.out)) : -ENODEV;
    case Kind::Reboot:
      if (s_ops.reboot) s_ops.reboot(s.out, sizeof(s.out));
      return s_ops.reboot ? 0 : -ENODEV;
    case Kind::LpSleep:
      return s_ops.lpsleep ? s_ops.lpsleep(s.arg, s.out, sizeof(s.out)) : -ENODEV;
    case Kind::PpaBench:
      return s_ops.ppabench ? s_ops.ppabench(s.arg, s.out, sizeof(s.out)) : -ENODEV;
    case Kind::PpaFrame:
      // PPA1: plan / comparison / timing over the live canvas. Main task: the
      // canvas is read under RenderLock.
      return s_ops.ppaframe ? s_ops.ppaframe(s.arg, s.out, sizeof(s.out)) : -ENODEV;
    case Kind::Exp:
      // EXP1: the whole `exp` family, on the main task. The arg is the sub-command
      // line ("status", "row9 off", "vid 60", "simd 200").
      return s_ops.exp ? s_ops.exp(s.arg, s.out, sizeof(s.out)) : -ENODEV;
    case Kind::Touch:
      // E5: `touch status|recover`. Main task like every other rail-touching
      // command: it drives the TCA expander and the shared I2C bus.
      return s_ops.touch ? s_ops.touch(s.arg, s.out, sizeof(s.out)) : -ENODEV;
    default:
      return -EINVAL;
  }
}

// ---------------------------------------------------------------------------
// Frames. One per command execution; see the header for the wire format.
// ---------------------------------------------------------------------------
struct Frame {
  uint32_t id;
  const char* name;
  int64_t t0;
};

void frameBegin(const Frame& f) {
  printf("<<<BEGIN %lu cmd=%s\n", static_cast<unsigned long>(f.id), f.name);
  fflush(stdout);
}

void frameEnd(const Frame& f, int rc, const Text& body) {
  if (body.len) fwrite(body.buf, 1, body.len, stdout);
  if (body.len && body.buf[body.len - 1] != '\n') fputc('\n', stdout);
  const int64_t ms = (esp_timer_get_time() - f.t0) / 1000;
  printf("<<<END %lu rc=%d ms=%lld heap=%u\n", static_cast<unsigned long>(f.id), rc,
         static_cast<long long>(ms), static_cast<unsigned>(esp_get_free_heap_size()));
  fflush(stdout);
}

// ---------------------------------------------------------------------------
// logtail: last n lines of /.crosspoint/debug.log, read backwards from the end
// in 512-byte chunks. Never reads the whole file (it grows to 16 KB and then
// rolls, but a tail should not depend on that).
// ---------------------------------------------------------------------------
int cmdLogtail(int argc, char** argv, Text& out) {
  int want = 20;
  int offset = 0;
  // r107 P5: `logtail <n> [offset] [--old]`. `--old` reads the ROLLED generation
  // (/.crosspoint/debug.log.old), which rotation already produces at 128 KB; until r107 the file
  // existed on the card but no read path could address it. Same nmax/offset semantics either way.
  bool readOld = false;
  int nums = 0;
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == nullptr) continue;
    if (strcmp(argv[i], "--old") == 0) {
      readOld = true;
      continue;
    }
    if (nums == 0) {
      const long v = strtol(argv[i], nullptr, 10);
      if (v <= 0 || v > 400) {
        out.put("logtail: n must be 1..400\n");
        return -EINVAL;
      }
      want = static_cast<int>(v);
      ++nums;
      continue;
    }
    if (nums == 1) {
      const long o = strtol(argv[i], nullptr, 10);
      if (o < 0 || o > 5000) {
        out.put("logtail: offset must be 0..5000 (lines back from the end)\n");
        return -EINVAL;
      }
      offset = static_cast<int>(o);
      ++nums;
      continue;
    }
    out.put("logtail: usage: logtail <n 1..400> [offset 0..5000] [--old]\n");
    return -EINVAL;
  }
  // r106: the optional second argument is a LINE OFFSET FROM THE END. `logtail 20 400` returns
  // 20 lines that START 400 lines before the end. On r105 a 400-line request was answered
  // with the OLDEST ~10 lines because the frame body is capped at ~1 KB -- so the park
  // region was unreachable (`loggrep` now answers that better; this offset is the
  // no-pattern fallback). Offset counts BACK from the newest line.
  const char* kPath = readOld ? "/.crosspoint/debug.log.old" : "/.crosspoint/debug.log";
  HalFile f;
  if (!Storage.openFileForRead("SHELL", kPath, f)) {
    out.outf("logtail: cannot open %s (no card, or the volume is handed to USB)\n", kPath);
    return -EIO;
  }
  const size_t size = f.size();
  if (size == 0) {
    out.outf("%s is empty\n", kPath);
    f.close();
    return 0;
  }

  constexpr size_t kChunk = 512;
  char chunk[kChunk];
  // Collect backwards until `want + offset + 1` newlines are behind us, then reverse.
  const int need = want + offset;
  size_t pos = size;
  int newlines = 0;
  size_t tailStart = 0;
  while (pos > 0 && newlines <= need) {
    const size_t take = (pos > kChunk) ? kChunk : pos;
    pos -= take;
    if (!f.seek(pos)) break;
    const int got = f.read(chunk, take);
    if (got <= 0) break;
    for (int i = got - 1; i >= 0; --i) {
      if (chunk[i] == '\n') {
        ++newlines;
        if (newlines == need + 1) {
          tailStart = pos + static_cast<size_t>(i) + 1;
          break;
        }
      }
    }
  }
  f.close();

  out.outf("== %s (last %d of %u bytes, from offset %u, %d line(s) back)\n", kPath, want,
           static_cast<unsigned>(size), static_cast<unsigned>(tailStart), offset);
  HalFile g;
  if (!Storage.openFileForRead("SHELL", kPath, g)) {
    out.put("logtail: reopen failed\n");
    return -EIO;
  }
  if (!g.seek(tailStart)) {
    out.put("logtail: seek failed\n");
    g.close();
    return -EIO;
  }
  // Skip `offset` whole lines forward, so the first line printed is the one the caller asked for.
  if (offset > 0) {
    size_t bodyAt = tailStart;
    int skipped = 0;
    bool found = false;
    while (!found) {
      const size_t at = g.position();
      const int n = g.read(chunk, sizeof(chunk));
      if (n <= 0) break;
      for (int i = 0; i < n; ++i) {
        if (chunk[i] != '\n') continue;
        if (++skipped >= offset) {
          bodyAt = at + static_cast<size_t>(i) + 1;
          found = true;
          break;
        }
      }
    }
    if (!found || !g.seek(bodyAt)) {
      out.outf("logtail: offset %d is past the end (%u bytes)\n", offset, static_cast<unsigned>(size));
      g.close();
      return -EINVAL;
    }
  }
  // r106: the body budget is the frame's, not a fixed 640 B, so 12 lines of real log
  // (e.g. one page turn's [PANEL] show) fit; a long answer is still cut on a line
  // boundary and STILL says how many lines were left out.
  char line[256];
  constexpr size_t kBodyBudget = 1080;
  size_t budget = kBodyBudget;
  int shown = 0;
  int leftOut = 0;
  while (g.available() > 0) {
    const int n = g.read(line, sizeof(line) - 1);
    if (n <= 0) break;
    line[n] = '\0';
    const size_t take = strlen(line);
    if (shown >= want) break;
    if (take > budget) {
      ++leftOut;
      break;
    }
    out.append(line, take);
    budget -= take;
    ++shown;
  }
  // Do not silently stop: count the requested lines the frame cap kept out.
  while (g.available() > 0 && shown + leftOut < want) {
    const int n = g.read(line, sizeof(line) - 1);
    if (n <= 0) break;
    ++leftOut;
  }
  if (leftOut > 0) {
    out.outf("logtail: %d of %d requested line(s) NOT shown (frame cap ~%u B) -- halve n or use `loggrep`\n",
             leftOut, want, static_cast<unsigned>(kBodyBudget));
  }
  g.close();
  return 0;
}

// ---------------------------------------------------------------------------
// logs: the RAM capture ring, NOT the card.
//
// `logtail` reads /.crosspoint/debug.log, which is only written when logSdFlush() runs (the idle
// path, the Debug page, `loglevel`, the LP-core phases). A line logged at boot therefore exists
// in exactly one place -- the CP_CAPTURE_LOG_KB ring that logSdFlush() drains -- and if the ring
// is flushed or never flushed, logtail cannot show it. This dumps that ring before anything else
// touches it, which is what makes a boot-time investigation possible at all.
//
// READ-ONLY and non-destructive: nothing is cleared, and reading does NOT flush, so `logs` and
// `logtail` can be used in either order. Newest-last, same reading direction as logtail, because
// that is how the ring itself is written.
//
// HONEST LIMITATION: the ring only receives lines logged while the runtime debug gate was on
// (plus ERR, which is never gated). main.cpp applies that gate part-way through setup(), because
// the flag lives on the card, so lines logged BEFORE that point are in neither the ring nor the
// file. An empty answer here is therefore a fact about the gate, not about the log path.
// ---------------------------------------------------------------------------
int cmdLogs(int argc, char** argv, Text& out) {
  int want = 20;
  // r107 P7: `logs clear` — the ONLY sanctioned log deletion in the image, and it is never
  // implicit: no code path calls this, a human has to type it. It removes BOTH generations
  // (debug.log and the rolled debug.log.old), says how many bytes each held, and reports the
  // free space afterwards. The RAM ring is dropped first inside logSdClearBoth(), so a pending
  // auto-flush cannot recreate the file seconds after the delete, and debug.log is reopened with
  // a fresh header so the next append has a valid, empty file to land in.
  if (argc >= 2 && argv[1] != nullptr && strcmp(argv[1], "clear") == 0) {
    if (!Storage.ready()) {
      out.put("logs clear: REFUSED — the SD is not mounted (no card, or the volume is handed to USB)\n");
      return -EIO;
    }
    const size_t curB = logSdFileSize();
    const size_t oldB = logSdOldFileSize();
    const bool ok = logSdClearBoth();
    const size_t newB = logSdFileSize();
    auto& sd = SDCardManager::getInstance();
    const uint64_t total = sd.sdTotalBytes();
    const uint64_t used = sd.sdUsedBytes();
    out.outf("logs clear: /.crosspoint/debug.log %u B -> removed; /.crosspoint/debug.log.old %u B -> removed\n",
             static_cast<unsigned>(curB), static_cast<unsigned>(oldB));
    out.outf("logs clear: %u B freed; debug.log reopened with a fresh header (%u B)%s\n",
             static_cast<unsigned>(curB + oldB), static_cast<unsigned>(newB), ok ? "" : " — one remove FAILED, see above");
    if (total != 0) {
      out.outf("logs clear: free space after = %llu MB of %llu MB\n",
               static_cast<unsigned long long>((total - (used > total ? total : used)) / (1024ULL * 1024ULL)),
               static_cast<unsigned long long>(total / (1024ULL * 1024ULL)));
    } else {
      out.put("logs clear: free space unknown (the driver caches no capacity for this volume)\n");
    }
    return ok ? 0 : -EIO;
  }
  if (argc >= 2) {
    const long v = strtol(argv[1], nullptr, 10);
    if (v <= 0 || v > 400) {
      out.put("logs: n must be 1..400\n");
      return -EINVAL;
    }
    want = static_cast<int>(v);
  }
  size_t len = 0;
  const char* buf = logRingData(&len);
  if (buf == nullptr || len == 0) {
    out.outf("logs: RAM ring is empty (nothing buffered since the debug gate came on, or it was just flushed)\n");
    return 0;
  }

  // Walk backwards for the start of the last `want` lines. Each element is a complete line (every
  // logPrintf() line ends in '\n'), so the byte at `len` is not consulted -- the final '\n' just
  // terminates the newest line.
  size_t start = 0;
  int seen = 0;
  for (size_t i = len; i > 0; --i) {
    if (buf[i - 1] != '\n' || i == len) continue;
    if (++seen == want) {
      start = i;
      break;
    }
  }

  out.outf("== RAM ring (last %d of %u bytes, from offset %u) ==\n", want, static_cast<unsigned>(len),
           static_cast<unsigned>(start));
  // Line-at-a-time so a cut is always on a line boundary AND always announced: a silently short
  // dump is what makes a debug channel untrustworthy (same rule logtail's body cap follows).
  const char* p = buf + start;
  const char* const end = buf + len;
  size_t budget = 900;
  int shown = 0;
  while (p < end) {
    const char* nl = static_cast<const char*>(memchr(p, '\n', static_cast<size_t>(end - p)));
    const size_t n = (nl != nullptr) ? static_cast<size_t>(nl - p) + 1 : static_cast<size_t>(end - p);
    if (n > budget) break;
    out.append(p, n);
    budget -= n;
    p += n;
    ++shown;
  }
  if (p < end) {
    out.outf("logs: %d line(s) shown, %u byte(s) still in the ring -- ask for a smaller n\n", shown,
             static_cast<unsigned>(end - p));
  } else {
    out.outf("logs: %d line(s), ring fully shown\n", shown);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// r107 P5 `loginfo`: BOTH generations on one screen — "did it roll?" in one command.
//
// Rotation already existed (logSdRollIfOversized, 128 KB cap: debug.log -> debug.log.old, the
// previous .old is dropped, a fresh file starts). What was missing on r106 was any way to SEE
// that it happened: `logtail` only ever read debug.log, so a roll looked exactly like the card
// log shrinking. This prints existence + size for both files and the cap, and names the read
// path for the rolled one.
// ---------------------------------------------------------------------------
int cmdLoginfo(int argc, char** argv, Text& out) {
  (void)argc;
  (void)argv;
  if (!Storage.ready()) {
    out.put("loginfo: SD is not mounted — neither generation is readable right now (`sd` says why)\n");
    return -EIO;
  }
  const size_t curB = logSdFileSize();
  const size_t oldB = logSdOldFileSize();
  const bool oldHas = logSdOldExists();
  const size_t cap = logSdMaxBytes();
  out.outf("loginfo: cap=%u B per generation, 2 generations kept on the card\n", static_cast<unsigned>(cap));
  out.outf("  /.crosspoint/debug.log      %-13s %u B\n", curB == 0 ? "empty/absent" : "present",
           static_cast<unsigned>(curB));
  out.outf("  /.crosspoint/debug.log.old  %-13s %u B%s\n", oldHas ? "present" : "absent",
           static_cast<unsigned>(oldB),
           (oldHas && oldB > cap) ? " (over cap: a roll that could not be verified kept the file, by design)" : "");
  out.outf("  rolled? %s\n", oldHas ? "YES — the previous generation is intact; read it with `logtail <n> --old`"
                                    : "no — debug.log has not crossed the cap since the last `logs clear`");
  out.outf("  next roll at %u B; the newest lines are never the ones dropped\n", static_cast<unsigned>(cap));
  return 0;
}

// ---------------------------------------------------------------------------
// r163 E6 `needs` -- the declarative power-need table, as the policy sees it.
//
// This is the surface a LEAK has to be visible on: a need acquired and never
// released blocks automatic standby forever, and the log line fires once, so the
// question "what is holding this device awake RIGHT NOW, and for how long" is this
// command. The age is the diagnosis: a `charger-host` aged a few seconds is a
// charging pack, an `usb-virtual-disk` aged six hours is a session nobody ended.
//
// It reads counters written by the main loop from the shell task. Every field is a
// scalar and a torn read can only misreport an age by one iteration, which is why
// no lock is taken: a lock here would let a debug command stall the policy.
// ---------------------------------------------------------------------------
int cmdNeeds(int argc, char** argv, Text& out) {
  (void)argc;
  (void)argv;
  const unsigned long now = millis();
  const powerneed::Report r = powerneed::sys().report(now);
  out.put("power needs (E6): HARD blocks AUTOMATIC standby only (a manual sleep still sleeps); SOFT is\n"
          "                  visible and inert -- it never blocks standby and never refreshes the idle clock\n");
  for (uint8_t i = 0; i < powerneed::kNeedCount; ++i) {
    const powerneed::Status& s = r.status[i];
    out.outf("  %-18s %-4s count=%u %s", s.name, powerneed::holdName(s.hold), static_cast<unsigned>(s.count),
             s.count == 0 ? "released" : "");
    if (s.count != 0) out.outf("held=%lu ms", s.heldMs);
    const unsigned long budget = powerneed::leakBudgetMs(static_cast<powerneed::Need>(i));
    if (budget != 0)
      out.outf(" budget=%lu ms", budget);
    else
      out.put(" budget=none (re-evaluated from a hardware predicate every loop)");
    if (s.leaked) out.put("  <-- LEAK");
    out.outf("  acq=%lu rel=%lu\n", static_cast<unsigned long>(s.acquiredTotal),
             static_cast<unsigned long>(s.releasedTotal));
  }
  out.outf("auto-standby: %s | idle-clock refresh pending: %d | double releases: %lu | leaks ever: %lu\n",
           r.blocksAutoStandby ? "BLOCKED by a hard need" : "allowed", r.awaitingActivityRefresh ? 1 : 0,
           static_cast<unsigned long>(r.doubleReleases), static_cast<unsigned long>(r.leakedEver));
  if (r.leakedNow != 0)
    out.outf("  %lu need(s) are past their budget RIGHT NOW -- if the device will not auto-sleep, this is why\n",
             static_cast<unsigned long>(r.leakedNow));
  out.outf("charge policy: %s (chargeIdlePolicy=%u) charging=%d -- a MANUAL sleep is never affected\n",
           chargepolicy::modeOf(SETTINGS.chargeIdlePolicy) == chargepolicy::Mode::KeepAwake
               ? "keep awake while charging"
               : "follow the sleep timeout",
           static_cast<unsigned>(SETTINGS.chargeIdlePolicy), powerManager.isCharging() ? 1 : 0);
  return 0;
}

// ---------------------------------------------------------------------------
// r106 `loggrep`: search the CARD log on-device and print only the matching lines.
// r107 P1: the scan buffers are OFF the `console_repl` task stack (heap, freed on every return).
//
// WHY: on r105 every log hunt was blind -- `logtail` answers with the OLDEST ~10 of the
// lines it was asked for (the frame is capped at ~1 KB), and the park region sits
// hundreds of lines back. Reading the file in the shell and printing only matches with
// their line numbers makes "where is `[PWR] park mark`" a single command.
//
// BOUNDED BY CONSTRUCTION: the file is streamed in 512-byte chunks (no allocation, no
// whole-file read), at most 40 matches are printed, and the answer says when it
// truncated. `before` is the number of context lines kept before each match (0..3).
// Case-sensitive on purpose: log lines are grepped by their exact tag prefixes ([PWR],
// [PANEL], epd_eink4p), and a case-folding option would only hide a wrong tag.
// ---------------------------------------------------------------------------
int cmdLoggrep(int argc, char** argv, Text& out) {
  if (argc < 2 || argv[1] == nullptr || argv[1][0] == '\0') {
    out.put("usage: loggrep <pattern> [before 0..3] [--old]   (case-sensitive, card log only; --old greps debug.log.old)\n");
    return -EINVAL;
  }
  const char* pat = argv[1];
  int before = 0;
  // ---- r109 LOG7: `--old` used to be ACCEPTED AND DISCARDED ----------------------------------
  // `loggrep park --old` grepped the CURRENT file and the header said `debug.log`, so it looked
  // like a successful search of the previous generation. After a 128 KB roll the park/resume marks
  // are only in debug.log.old, and the tool built to find them was silently reading the wrong file.
  // Now every trailing token is either the before-context count or `--old`; anything else is an
  // error rather than a no-op.
  bool readOld = false;
  bool beforeSet = false;
  for (int i = 2; i < argc; ++i) {
    const char* tok = argv[i];
    if (tok == nullptr || tok[0] == '\0') continue;
    if (strcmp(tok, "--old") == 0) {
      readOld = true;
      continue;
    }
    if (!beforeSet) {
      // r110: a token that is not a NUMBER is not before=0. `strtol("foo")` returns 0 and r109
      // accepted it, so `loggrep park foo` looked like a successful 0-context grep instead of the
      // typo it is. The whole token must be the number.
      char* end = nullptr;
      const long b = strtol(tok, &end, 10);
      if (end == tok || end == nullptr || *end != '\0') {
        out.outf("loggrep: unknown option '%s' (want [before 0..3] [--old])\n", tok);
        return -EINVAL;
      }
      if (b < 0 || b > 3) {
        out.put("loggrep: before must be 0..3\n");
        return -EINVAL;
      }
      before = static_cast<int>(b);
      beforeSet = true;
      continue;
    }
    out.outf("loggrep: unknown option '%s' (want [before 0..3] [--old])\n", tok);
    return -EINVAL;
  }

  const char* const kPath = readOld ? "/.crosspoint/debug.log.old" : "/.crosspoint/debug.log";
  HalFile f;
  if (!Storage.openFileForRead("SHELL", kPath, f)) {
    out.outf("loggrep: cannot open %s (no card, or the volume is handed to USB)\n", kPath);
    return -EIO;
  }
  const size_t size = f.size();
  if (size == 0) {
    out.outf("%s is empty -- nothing to grep (the RAM ring is `logs`)\n", kPath);
    f.close();
    return 0;
  }

  constexpr int kMaxShown = 40;
  // r107 P1: a per-call scan cap. 512 KB is four 128 KB generations: a normal grep over the
  // whole card log (or over debug.log.old after a roll) completes inside it, and no call can
  // walk an unbounded file. Hitting it is printed, never silent.
  constexpr size_t kMaxScanBytes = 512u * 1024u;
  // ---- r107 P1: THE REPL STACK IS THE BUDGET, NOT THE HEAP ----------------------------------
  // On r106 these buffers lived on the `console_repl` task stack -- 512 (chunk) + 256 (line) +
  // 3 x 192 (context) plus the frame body -- and the FIRST scan overflowed that task's stack by
  // 18 B and panic-rebooted the device (R103_GLASS_LOG §G1: SP 0x2f069110 vs bound 0x2f069128,
  // task "console_repl", 'Stack protection fault'). A shell verb may not assume a large stack
  // because it is "only reading". Everything sizeable is therefore ONE heap block here, freed by
  // an RAII holder on every return path (including both error returns), so the shell stack holds
  // only scalars whatever the log size is.
  struct ScanBuf {
    char chunk[512];
    char line[256];
    char ctx[3][192];
    int ctxNo[3];
  };
  struct ScanHolder {
    ScanBuf* p;
    ~ScanHolder() { free(p); }
  };
  ScanHolder holder{static_cast<ScanBuf*>(malloc(sizeof(ScanBuf)))};
  if (holder.p == nullptr) {
    f.close();
    out.put("loggrep: REFUSED -- no heap for the 1.4 KB scan buffers (never on the shell stack)\n");
    return -ENOMEM;
  }
  ScanBuf& s = *holder.p;
  int ctxCount = 0;
  size_t len = 0;
  size_t scanned = 0;
  int lineNo = 0;
  int matches = 0;
  int shown = 0;
  bool capped = false;
  bool scanCapped = false;

  out.outf("grep %s in %s (%u bytes)\n", pat, kPath, static_cast<unsigned>(size));
  while (matches < 5000) {
    if (scanned >= kMaxScanBytes) {
      scanCapped = true;
      break;
    }
    const size_t roomLeft = kMaxScanBytes - scanned;
    const size_t ask = roomLeft < sizeof(s.chunk) ? roomLeft : sizeof(s.chunk);
    const int n = f.read(s.chunk, ask);
    if (n <= 0) break;
    scanned += static_cast<size_t>(n);
    for (int i = 0; i < n; ++i) {
      const char c = s.chunk[i];
      if (c != '\n') {
        if (len + 1 < sizeof(s.line)) s.line[len++] = c;
        continue;
      }
      s.line[len] = '\0';
      len = 0;
      ++lineNo;
      if (strstr(s.line, pat) != nullptr) {
        ++matches;
        if (shown < kMaxShown && out.len < 980) {
          for (int k = 0; k < ctxCount; ++k) {
            out.outf("%d| %s\n", s.ctxNo[k], s.ctx[k]);
          }
          out.outf("%d| %s\n", lineNo, s.line);
          ++shown;
        } else {
          capped = true;
        }
      }
      // Push THIS line into the before-context ring (it is context for the next match).
      if (before > 0) {
        if (ctxCount < before) {
          snprintf(s.ctx[ctxCount], sizeof(s.ctx[0]), "%s", s.line);
          s.ctxNo[ctxCount] = lineNo;
          ++ctxCount;
        } else {
          for (int k = 1; k < before; ++k) {
            memcpy(s.ctx[k - 1], s.ctx[k], sizeof(s.ctx[0]));
            s.ctxNo[k - 1] = s.ctxNo[k];
          }
          snprintf(s.ctx[before - 1], sizeof(s.ctx[0]), "%s", s.line);
          s.ctxNo[before - 1] = lineNo;
        }
      }
    }
  }
  f.close();
  out.outf("loggrep: %d match(es) in %d line(s); %d shown%s\n", matches, lineNo, shown,
           capped ? " -- OUTPUT CAP (40 matches / ~1 KB); narrow the pattern or raise before= for context"
                  : "");
  if (scanCapped) {
    out.outf("loggrep: SCAN CAP -- stopped after %u KB of %u bytes; `logtail <n> <offset>` reads the rest\n",
             static_cast<unsigned>(kMaxScanBytes / 1024u), static_cast<unsigned>(size));
  }
  return 0;
}


// ---------------------------------------------------------------------------
// loglevel. HONEST LIMITATION, stated instead of faked:
//   The application logger (lib/Logging) is NOT ESP_LOG. It has exactly one
//   runtime gate — logSetRuntimeDebug(bool), which is what the Debug page's
//   "Debug mode" switch drives — plus ERR, which is never gated. There is no
//   per-level runtime filter to set. What this command can do:
//     none/err/warn/info : gate the app's INF/DBG off, and set the IDF esp_log
//                          level for the vendor/IDF lines that use ESP_LOG*.
//     debug              : gate the app's INF/DBG on, IDF level DEBUG.
//   INF/DBG call sites only exist in flash because this image is built with
//   CP_DEBUG_LOG=1 (CMakeLists). On an image built without it, "debug" can
//   only turn the IDF level up, and this command says so.
//   The raw `printf` lines main.cpp emits ([CP] loop tick ...) are not
//   filtered by anything, by design — they are the proof the loop is alive.
// ---------------------------------------------------------------------------
int cmdLoglevel(int argc, char** argv, Text& out) {
  if (argc < 2) {
    out.outf("loglevel: current app-debug=%s idf=%s\n", logGetRuntimeDebug() ? "on" : "off",
             esp_log_level_get("*") >= ESP_LOG_DEBUG   ? "debug"
             : esp_log_level_get("*") >= ESP_LOG_INFO  ? "info"
             : esp_log_level_get("*") >= ESP_LOG_WARN  ? "warn"
             : esp_log_level_get("*") >= ESP_LOG_ERROR ? "err"
                                                       : "none");
    out.put("usage: loglevel <none|err|warn|info|debug>\n");
    return -EINVAL;
  }
  const char* lv = argv[1];
  esp_log_level_t idf = ESP_LOG_INFO;
  bool appDebug = false;
  if (strcmp(lv, "none") == 0) {
    idf = ESP_LOG_NONE;
  } else if (strcmp(lv, "err") == 0 || strcmp(lv, "error") == 0) {
    idf = ESP_LOG_ERROR;
  } else if (strcmp(lv, "warn") == 0) {
    idf = ESP_LOG_WARN;
  } else if (strcmp(lv, "info") == 0) {
    idf = ESP_LOG_INFO;
  } else if (strcmp(lv, "debug") == 0) {
    idf = ESP_LOG_DEBUG;
    appDebug = true;
  } else {
    out.outf("loglevel: unknown level '%s'\n", lv);
    return -EINVAL;
  }
  esp_log_level_set("*", idf);
  logSetRuntimeDebug(appDebug);
  out.outf("loglevel: idf=*:%d app-inf/dbg=%s\n", static_cast<int>(idf), appDebug ? "on" : "off");
  if (appDebug) {
    out.put("note: app INF/DBG only exist if this image was built with CP_DEBUG_LOG=1; "
            "if no app lines appear, it was not. ERR always prints.\n");
  }
  return 0;
}

// ---------------------------------------------------------------------------
// r97 `batt` — the BATT1 discriminator. SHIPPING, deliberately NOT behind a
// bench switch (operator, 1 Nov 2026).
//
// THE DEFECT IT EXISTS FOR: the operator measured ~1 A from the charger at 87 %
// and the reader showed no charging bolt. Three causes produce that same picture
// and need three different fixes, so the UI cannot separate them:
//
//   1. STALE / DEAD READ  — the gauge stopped answering; every readout keeps
//                           returning its last cached value (or `false`).
//   2. INVERTED SIGN      — current is flowing, but the sign convention the code
//                           assumes is the wrong way round for this gauge.
//   3. WRONG STATUS PATH  — the derived flag comes from a charger register that
//                           this charger IC does not implement, so the read
//                           "succeeds" and reports zero.
//
// So this command prints, in ONE frame: the gauge's own answer (freshly read,
// not the cache), the value the UI is actually showing and how old it is, the
// SIGNED current with the convention NAMED, the derived flag with the exact
// function chain and register bits it came from, and the raw status words.
//
// SIGN CONVENTION (from the driver, not guessed): BatteryMonitor.cpp reads
// BQ27220 Current() (command 0x0C) as a signed 16-bit little-endian value and
// treats `> 0` as charging — "current flowing INTO the battery (> 0) means
// charging" (readGaugeCharging()). TI's BQ27xxx convention is the same:
// positive Current() = charge. So on this device +mA is charge, -mA discharge.
//
// Everything here is a READ: no register is written, no policy is changed.
// ---------------------------------------------------------------------------
namespace {

// Same wire protocol the SDK uses (BatteryMonitor.cpp readReg16/readReg8): write
// the command code, repeated start, read N bytes little-endian. No bus-free
// delay is needed for a read; the 66 us in writeReg16() is for Control() writes.
// BUS INVARIANT: ONE shared I2C bus, the same wire the TCA9555 expander lives on;
// the expander's failed read is what caused the power-key glitch. The three
// helpers below are one of four accessor sets on that bus (here, HalPowerManager,
// ChargerCurrent, and MetalioEInk4PlusBoard.h, where the canonical note lives) and
// they take no bus guard, so a shell read can interleave with expander/EPD
// traffic. Never "fix" a failed read by freeing the bus, and do not fold these
// sets together without a measurement round.
bool battRead16(const uint8_t addr, const uint8_t reg, uint16_t& out) {
  if (addr == 0) return false;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) return false;
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  out = static_cast<uint16_t>(lo) | static_cast<uint16_t>(hi << 8);
  return true;
}

bool battRead8(const uint8_t addr, const uint8_t reg, uint8_t& out) {
  if (addr == 0) return false;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) return false;
  out = Wire.read();
  return true;
}

// Two consecutive bytes in one transaction (status + fault on the charger).
bool battRead2(const uint8_t addr, const uint8_t reg, uint8_t& a, uint8_t& b) {
  if (addr == 0) return false;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) return false;
  a = Wire.read();
  b = Wire.read();
  return true;
}

}  // namespace

int cmdBatt(int argc, char** argv, Text& out) {
  // ---- Build A item C: the drift factor is STORED, not compiled in -----------------------------
  //   batt drift=<ppm>   store it (NVS, survives reboot) and print the correction it implies
  //   batt drift         print the stored value
  // It starts at 0 -- uncorrected -- because no measurement has been entered on this unit yet.
  // The value comes from §8.2 of HARDWARE_POWER_PLAN (device epoch delta vs the HOST wall clock
  // across one 8 h park); it is not a constant this tree knows.
  if (argc >= 2 && argv != nullptr && argv[1] != nullptr) {
    // ---- r106 B4: `batt mark` — the IDLE mark ----------------------------------------------
    // The park mark brackets a PARK. A9 needed to bracket an AWAKE hour and there was no
    // instrument for it, so this takes the SAME mark the park path takes (gauge 0x10 through
    // the same validated double-read the wake recorder uses, HybridClock epoch + uptime),
    // in RAM only: no NVS, no busy record, nothing persisted. `batt delta` then brackets the
    // awake interval, and its mA figure stays PROVISIONAL because the denominator is the
    // device clock (divide charge by the HOST wall clock).
    if (strcmp(argv[1], "mark") == 0) {
      const auto& g = BoardConfig::ACTIVE.batteryGauge;
      if (g.gaugeAddr == 0) {
        out.put("batt mark: no I2C gauge on this board profile -- nothing to mark\n");
        return -ENOSYS;
      }
      const HalPowerManager::CapCheck c = HalPowerManager::readRemainingCapacityChecked(g.gaugeAddr);
      hybridclock::markIdle(c.mah, c.ok, "batt mark");
      const hybridclock::Mark& m = hybridclock::parkMark();
      out.outf("batt mark: coulomb=%s%u mAh epoch=%lld uptime=%lu ms rtc=%llu us (IDLE mark, RAM only -- `batt delta` now brackets an AWAKE span; the mA figure is PROVISIONAL)\n",
               c.ok ? "" : "FAIL ", static_cast<unsigned>(c.mah), static_cast<long long>(m.epoch), m.uptimeMs,
               static_cast<unsigned long long>(m.rtcUs));
      if (!c.ok) out.outf("           gauge 0x10 did not validate: %s\n", c.why);
      return 0;
    }
    if (strcmp(argv[1], "drift") == 0) {
      char label[32];
      out.outf("drift    ppm=%s [stored in NVS cpclk/ppm; applies to the PARKED span only]\n",
               hybridclock::driftLabel(label, sizeof(label)));
      out.put("         set with `batt drift=<ppm>`; +ppm = the RTC runs fast. Measure it, do not guess.\n");
      return 0;
    }
    static constexpr const char* kPrefix = "drift=";
    if (strncmp(argv[1], kPrefix, 6) == 0) {
      const int ppm = atoi(argv[1] + 6);
      const int32_t stored = hybridclock::setDriftPpm(static_cast<int32_t>(ppm));
      out.outf("drift    stored ppm=%ld -> a 1000.000 s raw park is corrected to %.3f s\n",
               static_cast<long>(stored), 1000.0 + static_cast<double>(stored) / 1000.0);
      out.put("         This is the ONLY correction applied: the parked span (RTC ticks). The epoch is not\n"
              "         re-scaled here, because gettimeofday advances on the same ticks.\n");
      return 0;
    }
  }
  // BQ27220 standard command codes, repeated here rather than taken from the
  // SDK's internal constants: this is the file that has to be readable while the
  // gauge is lying, so the register numbers stay visible.
  constexpr uint8_t kGaugeStatus = 0x0A;   // BatteryStatus(), bit0 = DSG (1 = discharging)
  constexpr uint8_t kGaugeCurrent = 0x0C;  // Current(), signed mA, + = into the battery
  // 0x2C StateOfCharge() is read through HalPowerManager::readSocChecked() -- a 1-BYTE command,
  // twice. Do not re-add a raw 16-bit read here: that is what printed `65380%`.
  constexpr uint8_t kChargerReg0B = 0x0B;  // BQ25896-family REG0B, [4:3] = CHRG_STAT
  constexpr uint8_t kChargerReg00 = 0x00;  // status + fault

  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  if (g.gaugeAddr == 0) {
    out.put("batt: no I2C gauge on this board profile (batteryGauge.gaugeAddr == 0)\n");
    return 0;
  }

  const HalPowerManager::BatteryDiag d = powerManager.batteryDiag();
  const unsigned long now = d.uptimeMs;

  // One fresh read of every source, on this task, right now.
  uint16_t curRaw = 0, statRaw = 0;
  const bool curOk = battRead16(g.gaugeAddr, kGaugeCurrent, curRaw);
  const bool statOk = battRead16(g.gaugeAddr, kGaugeStatus, statRaw);
  // r98.3: the SoC is taken through the SAME gate the UI uses (HalPowerManager::readSocChecked),
  // not a second copy of the rules. 0x2C is a ONE-BYTE command; the r98.2 line here read it as a
  // u16 and printed `65380% [0x2C ok]` -- 0xFF64, whose high half is 0x2D (0xFF), a byte that is
  // not part of StateOfCharge. That printed a broken read and a good one identically. `sc.raw`
  // is the reconstructed 16-bit word so the old failure is still visible as a number, with the
  // reason it was refused next to it.
  const HalPowerManager::SocCheck sc = HalPowerManager::readSocChecked(g.gaugeAddr);
  const int16_t mA = static_cast<int16_t>(curRaw);
  uint8_t reg0B = 0, reg00 = 0, reg01 = 0;
  const bool reg0BOk = battRead8(g.chargerAddr, kChargerReg0B, reg0B);
  const bool reg00Ok = battRead2(g.chargerAddr, kChargerReg00, reg00, reg01);
  const uint8_t chrgStat = static_cast<uint8_t>((reg0B >> 3) & 0x03);
  const bool chargerCharging = chrgStat == 0x01 || chrgStat == 0x02;

  // What the UI is being told, and how old that value is.
  const bool uiFlag = powerManager.isCharging();
  // r163 E3: the flag the ICON is drawn from -- raw charging AND SoC < 100, so a full
  // pack on the cable shows no bolt. Printed separately because "the record says
  // charging" and "the glass shows a bolt" are now two different answers on purpose.
  const bool uiFlagShown = powerManager.isChargingDisplayed();
  const uint16_t uiSoc = powerManager.getBatteryPercentage();
  const unsigned long socAge = d.socEverRead ? (now - d.socLastOkMs) : 0xFFFFFFFFul;
  const unsigned long flagAge = d.chargingLastPollMs ? (now - d.chargingLastPollMs) : 0xFFFFFFFFul;

  out.outf("batt: gauge 0x%02X chg 0x%02X%s, fresh read (not the UI cache)\n", g.gaugeAddr, g.chargerAddr,
           g.chargerAddr == 0 ? " (BQ25896 map disabled -- CX25601N read directly)" : "");
  // r98.3: a correct reading and a rejected one must never look the same. The accepted value is
  // the 1-byte 0x2C read; a refused one prints the raw word that was refused AND the test it
  // failed, so `soc 65380%` can no longer appear next to a plain "ok".
  if (sc.ok) {
    out.outf("soc      %u%% [0x2C ok: two 1-byte reads agree (0x%02X), word 0x%04X] | ui %u%% age=",
             static_cast<unsigned>(sc.soc), static_cast<unsigned>(sc.soc1), static_cast<unsigned>(sc.raw),
             static_cast<unsigned>(uiSoc));
  } else {
    out.outf("soc      0x%04X REJECTED (%s); last good ", static_cast<unsigned>(sc.raw),
             sc.why[0] != '\0' ? sc.why : "no reason recorded");
    if (d.socEverRead) {
      out.outf("%u%%%s", static_cast<unsigned>(d.socCached), d.socStale ? " (STALE, ui reports 0)" : "");
    } else {
      out.put("none (no accepted reading since boot)");
    }
    out.outf("; ui %u%% age=", static_cast<unsigned>(uiSoc));
  }
  if (socAge == 0xFFFFFFFFul) {
    out.put("never");
  } else {
    out.outf("%lu ms fails=%lu", socAge, static_cast<unsigned long>(d.socFails));
  }
  out.outf(" rejects=%lu  (getBatteryPercentage)\n", static_cast<unsigned long>(d.socRejects));
  // r163 E1: how the shown number was produced. The average is pre-quantisation, so a
  // steady 53 % reads "avg 53 -> shown 50" -- the quantiser down, not nearest.
  out.outf("shown    %u%% <- 60-sample avg %u%% (%u/60 samples) quantise-down 5%%%s%s\n",
           static_cast<unsigned>(uiSoc), static_cast<unsigned>(d.socAverage),
           static_cast<unsigned>(d.socSamples),
           d.socSettling ? " | post-wake settle (value held)" : "",
           d.socRecalPending ? " | one-shot recalibration armed" : "");
  if (sc.spare != 0) {
    // The old value, spelled out: this is what a 2-byte read at 0x2C returns.
    out.outf("         note 0x2D=0x%02X is NOT part of StateOfCharge, so a 16-bit read at 0x2C gives "
             "0x%04X = %u; r98.2 printed that as \"%%\" and its clamp made it 100%%\n",
             static_cast<unsigned>(sc.spare), static_cast<unsigned>(sc.raw), static_cast<unsigned>(sc.raw));
  }

  if (curOk) {
    out.outf("current  %+d mA [0x0C raw 0x%04X ok]\n", static_cast<int>(mA), static_cast<unsigned>(curRaw));
  } else {
    out.put("current  READ FAILED [0x0C]\n");
  }
  out.put("  sign: + = INTO the battery (charging), - = OUT (discharging)\n");
  out.put("  readGaugeCharging() treats Current() > 0 as charging\n");

  // Step 2.5 item 8: the charger's OWN current limit, from REG0x02/0x03 -- not the
  // settings file. This is what the IC will actually do, which is the number the
  // setting has to be checked against; 500/1000/1500 quantise to 480/960/1440.
  {
    uint8_t ichgCode = 0;
    uint16_t ichgMa = 0;
    if (chargercurrent::readCode(ichgCode, &ichgMa)) {
      out.outf("ichg     %u mA (code %u, 80 mA steps) from %s | row=%umA\n", static_cast<unsigned>(ichgMa),
               static_cast<unsigned>(ichgCode), "REG0x02[7:6]+REG0x03[3:0]",
               static_cast<unsigned>(chargercurrent::maForChoice(SETTINGS.chargeCurrent)));
    } else {
      out.put("ichg     READ FAILED [0x02/0x03] -> charge current cannot be confirmed\n");
    }
  }

  out.outf("flag     %d %s <- HalPowerManager::isCharging() age=", uiFlag ? 1 : 0,
           uiFlag ? "CHARGING" : "not charging");
  if (flagAge == 0xFFFFFFFFul) {
    out.put("never");
  } else {
    out.outf("%lu ms", flagAge);
  }
  out.outf("  cache %lu ms | drawn=%d (%s)\n", static_cast<unsigned long>(HalPowerManager::BATTERY_POLL_MS),
           uiFlagShown ? 1 : 0,
           uiFlagShown ? "bolt shown" : (uiFlag ? "hidden: SoC 100%" : "no bolt"));
  // r163 E2: the 100 % evidence, next to the 100 % claim. `hold` counts only while 0x08 is
  // at/above 4300 mV WITHOUT a gap; `confirmed` is that hold completed, released at 4280 mV.
  if (d.gaugeMvOk) {
    out.outf("full     %u mV | %s%s held=%lu ms\n", static_cast<unsigned>(d.gaugeMv),
             d.fullConfirmed ? "CONFIRMED" : "not confirmed",
             d.fullHolding ? " (holding >=4300)" : "",
             static_cast<unsigned long>(d.fullHeldMs));
  } else {
    out.put("full     0x08 READ FAILED -> the confirmation was reset (a failed read is not a\n"
            "         measurement; see src/util/BatteryGaugeState.h)\n");
  }
  if (g.chargerAddr == 0) {
    // r98.1 D5: NOT "no charger". The charger is a CX25601N at 0x6B whose status is REG0x1E, so
    // the SDK's BQ25896 map (REG0x0B) is switched off rather than allowed to answer with the wrong
    // register. See the `cx` line below for the real status.
    out.put("  1 chargerAddr=0: BQ25896 REG0x0B map DISABLED on this board (0x6B is a CX25601N; "
            "its REG0x1E is the `cx` line below)\n");
  } else if (reg0BOk) {
    out.outf("  1 chg 0x0B=0x%02X CHRG_STAT[4:3]=%u %s\n", static_cast<unsigned>(reg0B),
             static_cast<unsigned>(chrgStat), chargerCharging ? "charging (01/10)" : "NOT charging (00/11)");
  } else {
    out.put("  1 chg 0x0B READ FAILED -> step 2 decides\n");
  }
  if (curOk) {
    out.outf("  2 gauge fallback if 1 failed: Current()>0 = %s\n", mA > 0 ? "true" : "false");
  } else {
    out.put("  2 gauge fallback READ FAILED -> flag reported as 0\n");
  }

  out.outf("raw      gauge 0x0A=0x%04X %s DSG=%u (1=discharging, rest undecoded)\n",
           static_cast<unsigned>(statRaw), statOk ? "ok" : "READ FAILED", static_cast<unsigned>(statRaw & 0x01));
  out.outf("         chg 0x00=0x%02X fault=0x%02X %s | 0x0B=0x%02X %s\n", static_cast<unsigned>(reg00),
           static_cast<unsigned>(reg01), reg00Ok ? "ok" : "READ FAILED", static_cast<unsigned>(reg0B),
           reg0BOk ? "ok" : "READ FAILED");

  // ---- r98.1 D1/D2: the numbers that make a SoC provable ----------------------------------------
  // Before this block `batt` could not distinguish a correct SoC from a wrong one: it printed 0x2C
  // and 0x0A and nothing to compare them against. These are the values that decide whether the
  // percentage means anything:
  //   0x3C DesignCapacity      the denominator the gauge believes the pack has (3000 = TI default,
  //                            2000 = the pack we actually fitted; r98.1 writes it at boot)
  //   0x12 FullChargeCapacity  what the gauge learned from charge cycles; a learning cycle moves it
  //                            at most 256 mAh, so one learned against a 3000 mAh default stays far
  //                            above a 2000 mAh pack
  //   0x3A OperationStatus     CFGUPDATE bit 10 + SEC[1:0]; all ones means nothing is answering
  //   DM 0x929D / 0x929F       Learned FCC / Design Capacity as stored in Data Memory -- the values
  //                            the load machine rewrites, read back through MAC (0x3E select, 0x40
  //                            data), which is the only way to see what the checksummed write did
  {
    uint16_t design = 0, fcc = 0, opStat = 0, remaining = 0;
    constexpr uint8_t kDesignCapacity = 0x3C;
    constexpr uint8_t kFullChargeCapacity = 0x12;
    constexpr uint8_t kOperationStatus = 0x3A;
    constexpr uint8_t kRemainingCapacity = 0x10;
    const bool dOk = battRead16(g.gaugeAddr, kDesignCapacity, design);
    const bool fOk = battRead16(g.gaugeAddr, kFullChargeCapacity, fcc);
    const bool oOk = battRead16(g.gaugeAddr, kOperationStatus, opStat);
    const bool rOk = battRead16(g.gaugeAddr, kRemainingCapacity, remaining);
    out.outf("capacity 0x3C=%u mAh %s | 0x12=%u mAh %s  (0x3C must be 2000 = the fitted pack; 3000 is TI's default) -> denominator %s\n",
             static_cast<unsigned>(design), dOk ? "ok" : "READ FAILED", static_cast<unsigned>(fcc),
             fOk ? "ok" : "READ FAILED",
             (dOk && design == static_cast<unsigned>(BoardConfig::ACTIVE.batteryGauge.designCapacityMah))
                 ? "OK (matches the profile)"
                 : "WRONG (differs from the profile; every % is scaled)");
    // r98.1: 0x10 RemainingCapacity is the gauge's own coulomb count, integrated in hardware. It is
    // the ONLY instrument that survives light sleep -- the CPU is asleep, so nothing samples current
    // and the 1 mAh LSB sets the measurement floor of any standby A/B. Delta over a standby is the
    // charge that left the pack, with no software integration and no MCU awake time involved.
    out.outf("remaining 0x10=%u mAh %s (coulomb count; survives light sleep, 1 mAh LSB | 0x10/0x3C=%s)\n",
             static_cast<unsigned>(remaining), rOk ? "ok" : "READ FAILED",
             (rOk && dOk && design != 0) ? "see next line" : "-");
    if (rOk) {
      // Static so two `batt` invocations can be differenced without an external log.
      static uint16_t s_prevRemaining = 0;
      static unsigned long s_prevMs = 0;
      if (s_prevMs != 0) {
        out.outf("           since last `batt`: %+d mAh over %lu ms (%lu bad 1-mAh-LSB steps)\n",
                 static_cast<int>(remaining) - static_cast<int>(s_prevRemaining), (now - s_prevMs),
                 static_cast<unsigned long>(llabs(static_cast<long>(remaining) - static_cast<long>(s_prevRemaining))));
      }
      s_prevRemaining = remaining;
      s_prevMs = now;
    }

    // ---- Build A item C: the park/resume pair, answered from one place (`batt delta`) ----------
    // Charge from the gauge; elapsed from a clock that NAMES ITSELF (RTC ticks across the park,
    // corrected by the stored ppm factor; esp_timer while awake). No number here is offered as a
    // true current: the denominator is the clock under test, so the operator divides the charge by
    // the HOST wall clock. Two lines only -- `batt` shares a fixed 1280-byte body with everything
    // else it prints, and this must not push the gauge-verification fields out of it.
    {
      const hybridclock::Mark& pm = hybridclock::parkMark();
      if (!pm.valid) {
        out.put("delta    no park mark this session (set at the first standby park; a fresh boot has none)\n");
      } else {
        const long dMah = static_cast<long>(remaining) - static_cast<long>(pm.coulombMah);
        const uint64_t elUs = hybridclock::sinceMarkUs();
        const unsigned long elMs = static_cast<unsigned long>(elUs / 1000ULL);
        long centiMa = 0;
        if (elMs != 0) centiMa = static_cast<long>((static_cast<long long>(dMah) * 360000000LL) / static_cast<long long>(elMs));
        char drift[32];
        hybridclock::driftLabel(drift, sizeof(drift));
        const hybridclock::Segment& seg = hybridclock::lastSegment();
        out.outf("delta    0x10 %u -> %u = %+ld mAh over %lu.%02lu s device time (%s%ld.%02ld mA PROVISIONAL; divide charge by the HOST wall clock)\n",
                 static_cast<unsigned>(pm.coulombMah), static_cast<unsigned>(remaining), dMah, elMs / 1000,
                 (elMs % 1000) / 10, centiMa < 0 ? "-" : "", labs(centiMa) / 100, labs(centiMa) % 100);
        if (seg.valid) {
          out.outf("         drift %s | parked %lu ms (RTC raw) -> %lu ms corrected | mark epoch=%lld uptime=%lu ms rtc=%llu us%s\n",
                   drift, static_cast<unsigned long>(seg.parkedRawUs / 1000ULL),
                   static_cast<unsigned long>(seg.parkedCorrectedUs / 1000ULL),
                   static_cast<long long>(pm.epoch), pm.uptimeMs, static_cast<unsigned long long>(pm.rtcUs),
                   pm.coulombOk ? "" : " (0x10 FAILED at park)");
        } else {
          out.outf("         drift %s | no resume half (esp_timer only) | mark epoch=%lld uptime=%lu ms rtc=%llu us\n", drift,
                   static_cast<long long>(pm.epoch), pm.uptimeMs, static_cast<unsigned long long>(pm.rtcUs));
        }
      }
    }
    if (oOk) {
      const bool cfg = (opStat & 0x0400) != 0;
      const unsigned sec = (opStat >> 1) & 3;
      out.outf("opstat   0x3A=0x%04X CFGUPDATE=%u SEC=%u %s\n", static_cast<unsigned>(opStat),
               cfg ? 1u : 0u, sec,
               opStat == 0xFFFF ? "<- ALL ONES: nothing answering, do not trust 0x2C" : "");
      // r98.2: the verdict, not just the bits. In CONFIG UPDATE the gauge suspends Impedance
      // Track, so 0x10 stops moving and DM reads come back from whichever block the MAC pointer
      // happens to point at. This is the line that says whether the numbers below mean anything.
      if (cfg) {
        out.put("         -> GAUGE IS IN CONFIG UPDATE: Impedance Track suspended, so 0x10 is frozen and DM\n"
                "            reads are not coherent. r98.2 clears this at every boot (unseal, EXIT 0x0091,\n"
                "            poll 0x3A, re-seal); if it persists, look for the `gauge FINAL` log line.\n");
      } else if (sec != 3) {
        out.outf("         -> CFGUPDATE is clear but SEC=%u is not sealed: a gauge left unsealed stays writable\n"
                 "            and will not survive a seal-only recovery. Reboot to run the r98.2 seal lane.\n",
                 sec);
      } else {
        out.put("         -> CFGUPDATE clear and sealed: Impedance Track running, 0x10 is a live count\n");
      }
    } else {
      out.put("opstat   0x3A READ FAILED -> 0x2C not trustworthy\n");
    }
    // Data Memory, big-endian words 0x929D (Learned FCC) / 0x929F (Design Capacity). Read-only
    // here; the machine that WRITES them is gaugefix:: in util/BatteryGaugeFix.cpp.
    //
    // r98.2: these are VALIDATED reads, not raw ones. r98.1 printed whatever came back, so the
    // same address showed 2000 and then 3280 -- the second value was the block the gauge's MAC
    // pointer had advanced to, and nothing in the output said so. A word is now read twice (each
    // read re-selects the block) and is only reported when both reads and both block checksums
    // agree; it must also agree with its command-register mirror. Anything else prints INVALID
    // and no number, so a plausible-looking value cannot be mistaken for a measurement.
    const auto dmShow = [&](const uint16_t dmAddr, const char* label, const uint16_t mirror, const char* mirrorName,
                            const bool haveMirror) {
      gaugefix::DmWord w;
      gaugefix::readDmWord(dmAddr, w);
      if (w.verdict == gaugefix::Verdict::Ok) {
        out.outf("dm %s 0x%04X=%u sum=0x%04X [valid: %s]", label, static_cast<unsigned>(dmAddr),
                 static_cast<unsigned>(w.value), static_cast<unsigned>(w.sum), w.why);
        if (haveMirror) {
          out.outf(" | %s=%u %s", mirrorName, static_cast<unsigned>(mirror),
                   w.value == mirror ? "agrees" : "DIFFERS (reinit/learning pending, or a stale mirror)");
        }
        out.put("\n");
      } else if (w.verdict == gaugefix::Verdict::Invalid) {
        out.outf("dm %s 0x%04X=INVALID [%s: %u vs %u, sum 0x%04X vs 0x%04X]\n", label,
                 static_cast<unsigned>(dmAddr), w.why, static_cast<unsigned>(w.value),
                 static_cast<unsigned>(w.value2), static_cast<unsigned>(w.sum), static_cast<unsigned>(w.sum2));
      } else {
        out.outf("dm %s 0x%04X=unavailable [%s]", label, static_cast<unsigned>(dmAddr), w.why);
        if (haveMirror) out.outf(" | %s=%u %s", mirrorName, static_cast<unsigned>(mirror),
                                w.value == mirror ? "(agrees)" : "(validated from the command register)");
        out.put("\n");
      }
    };
    dmShow(0x929D, "learnedFCC", fcc, "0x12", fOk);
    dmShow(0x929F, "designCap ", design, "0x3C", dOk);
    // r98.1 left a standing implication that writing DM 0x929D should move 0x12. It should not and
    // does not: 0x12 FullChargeCapacity is a gauge-computed register. The gauge copies DM Learned
    // FCC into it at reinitialisation (EXIT_CFG_UPDATE_REINIT / POR), and after that only a full
    // charge/discharge LEARNING cycle moves it (at most 256 mAh per cycle, TRM 1.1.3). So:
    //   * 0x12 == learnedFCC after a successful r98.2 exit  -> the reinit ran, writes are live;
    //   * 0x12 != learnedFCC                                 -> either the reinit has not happened
    //     yet, or the gauge has not completed a learning cycle. Nothing is broken and nothing
    //     needs re-writing; it converges on the next full cycle.
    if (fOk && dOk) {
      out.outf("         -> 0x12 is a gauge-computed register, not a DM mirror: it is loaded from DM 0x929D at\n"
               "            gauge reinit and only moves otherwise at the end of a full learning cycle. %s\n",
               fcc == design ? "It already matches 0x3C here."
                : fcc > static_cast<unsigned>(design + design / 4)
                    ? "It is still far above the pack: expect it to converge after one full cycle."
                    : "It is within a learning cycle's worth of the pack rating.");
    }
    out.outf("soc gate 0x%04X %s rejected=%lu stale=%d | age ", static_cast<unsigned>(sc.raw),
             sc.ok ? "accepted" : "refused", static_cast<unsigned long>(d.socRejects), d.socStale ? 1 : 0);
    if (socAge == 0xFFFFFFFFul) {
      out.put("never\n");
    } else {
      out.outf("%lu ms (>%lu ms = UI reports 0)\n", socAge,
               static_cast<unsigned long>(HalPowerManager::BATTERY_STALE_MS));
    }
  }

  // r97.1 BATT3: the register the charge flag NOW reads. 0x0B above is the BQ25896
  // map; this board's part is a CX25601N at the same address, where charge status
  // is REG0x1E CHG_STAT[4:3] (0 terminated, 1 CC, 2 CV, 3 top-off). Printed even
  // when the probe failed, so the fallback is visible instead of implied.
  const HalPowerManager::ChargerDiag cd = powerManager.chargerDiag();
  {
    if (cd.cxIdentified) {
      if (cd.statusRead) {
        out.outf("cx       0x%02X REG0x1E=0x%02X CHG_STAT[4:3]=%d %s%s | PART0x38=0x%02X %s\n",
                 static_cast<unsigned>(cd.chargerAddr), static_cast<unsigned>(cd.reg1E), cd.chgStat,
                 cd.charging ? "CHARGING" : "not charging",
                 cd.charging ? "" : " (0 = terminated/idle)",
                 static_cast<unsigned>(cd.partInfo), cd.partRead ? "ok" : "READ FAILED");
        out.outf("        <- isCharging() source is CX25601N (ui flag %d %s)\n", uiFlag ? 1 : 0,
                 (cd.charging == uiFlag) ? "agrees" : "DISAGREES (cache window or read failure)");
      } else {
        out.put("cx       0x6B REG0x1E READ FAILED -> isCharging() fell back to BatteryMonitor\n");
      }
    } else {
      out.put("cx       not a CX25601N on this board/bus -> isCharging() uses BatteryMonitor\n");
    }
  }
  // r97.2 BATT4: the verdict is DERIVED from what this command just read, not assumed. The old
  // line stated the pre-CX25601N failure ("+mA with flag 0 and CHG_STAT 00/11 -> sign or wrong
  // register") unconditionally, so a device correctly reporting CHARGING through the CX25601N
  // still printed a diagnosis of the BQ25896 register map -- reading as broken while it was fine.
  {
    const char* source = "none";
    int observed = -1;  // -1 unknown, 0 not charging, 1 charging
    const char* detail = "";
    if (cd.cxIdentified && cd.statusRead) {
      source = "CX25601N REG0x1E CHG_STAT[4:3]";
      observed = cd.charging ? 1 : 0;
      detail = cd.charging ? "(CC/CV/top-off)" : "(terminated or idle)";
    } else if (reg0BOk) {
      source = "chg REG0x0B CHRG_STAT[4:3]";
      observed = chargerCharging ? 1 : 0;
      detail = chargerCharging ? "(01/10)" : "(00/11)";
    } else if (curOk) {
      source = "gauge Current() sign";
      observed = mA > 0 ? 1 : 0;
      detail = "";
    }
    if (observed < 0) {
      out.put("verdict  no charging source readable: every status register and the gauge failed (see READ FAILED above)\n");
    } else {
      out.outf("verdict  source %s -> %s %s | ui flag %d %s\n", source, observed ? "CHARGING" : "not charging",
               detail, uiFlag ? 1 : 0, (observed == (uiFlag ? 1 : 0)) ? "agrees" : "DISAGREES (poll cache or a failed read)");
      if (curOk && observed == 1 && mA <= 0) {
        out.outf("         note: charging but Current()=%+d mA -- gauge sign/offset, or not yet settled\n",
                 static_cast<int>(mA));
      }
      if (curOk && observed == 0 && mA > 0) {
        out.outf("         note: not charging but Current()=%+d mA -- input-limited path, or gauge sign suspect\n",
                 static_cast<int>(mA));
      }
    }
    out.put("         an age near the poll window above, or a READ FAILED, is stale evidence -- not a verdict\n");
  }
  return 0;
}

// ---------------------------------------------------------------------------
// r98.1 `trim` — the STANDBY A/B ARMS, at runtime.
//
// Why a command and not just a define: CP_CUT_PA_RAIL_ON_STANDBY and
// CP_SD_PARK_ON_STANDBY are compile-time arms, so measuring one costs a flash and
// you cannot interleave them on one battery state. These subcommands set a RAM
// override for the NEXT standby; the standby path itself is unchanged and reads the
// same predicate either way (src/platform/StandbyTrims.h).
//
//   trim status           arms, their compiled defaults, and the P1.4 shadow bit
//   trim pa on|off        arm the P1.4 (BT/PA) rail cut for the next standby
//   trim pa cut|restore   drive P1.4 NOW through the same driver call the standby
//                         uses -- bench verification that the rail really moves
//   trim cam on|off       arm the P0.5 (CAM_SCR camera/screen) rail cut, independently
//   trim cam restore     re-assert P0.5 NOW; the CUT half is RETIRED (B15a) -- it killed
//                        the device and destroyed a card on r105
//   trim sd on|off        arm the SD park (unmount + remount on resume)
//
// Refusals, all of them deliberate: a trim changed while the device is parked or
// while a panel drive holds the render lock would leave the hardware in a state
// that is neither arm. The override never survives a reboot, so the image's build
// default is always what a cold boot returns to.
// ---------------------------------------------------------------------------
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
int cmdTrim(int argc, char** argv, Text& out) {
  const auto board = BoardConfig::ACTIVE.board;
  if (board != BoardConfig::Board::MetalioEInk4Plus) {
    out.put("trim: this board has no standby trims (Metalio Plus only)\n");
    return -ENOSYS;
  }
  if (standbytrim::parked()) {
    out.put("trim: REFUSED -- the device is in standby (panel + rails parked). Wake it first.\n");
    return -EBUSY;
  }
  if (RenderLock::peek()) {
    out.put("trim: REFUSED -- a panel drive/render holds the lock; a trim moved mid-drive is neither arm.\n");
    return -EBUSY;
  }

  const char* sub = (argc >= 2) ? argv[1] : "status";

  const auto printArm = [&](const standbytrim::Trim t) {
    const int8_t o = standbytrim::overrideValue(t);
    out.outf("  %-8s compiled=%s effective=%s source=%s\n", standbytrim::name(t),
             standbytrim::buildDefault(t) ? "ON " : "OFF", standbytrim::active(t) ? "ON " : "OFF",
             o < 0 ? "build default" : (o ? "runtime override ON" : "runtime override OFF"));
  };

  if (strcmp(sub, "status") == 0) {
    out.outf("trim: %s (image built with %s=%s %s=%s %s=%s)\n", CROSSPOINT_VERSION,
             standbytrim::macroName(standbytrim::Trim::PaRail),
             standbytrim::buildDefault(standbytrim::Trim::PaRail) ? "1" : "0",
             standbytrim::macroName(standbytrim::Trim::CamRail),
             standbytrim::buildDefault(standbytrim::Trim::CamRail) ? "1" : "0",
             standbytrim::macroName(standbytrim::Trim::SdPark),
             standbytrim::buildDefault(standbytrim::Trim::SdPark) ? "1" : "0");
    printArm(standbytrim::Trim::PaRail);
    printArm(standbytrim::Trim::CamRail);
    printArm(standbytrim::Trim::SdPark);
    out.outf("  parked=%d  (1 = mid-standby, trims locked)\n", standbytrim::parked() ? 1 : 0);
    out.put("  pa-rail  drives TCA P1.4 at standby entry (pa_rail_set(false)) and re-asserts it on\n"
            "           resume BEFORE the panel is re-powered (80 ms settle);\n"
            "  cam-rail drives TCA P0.5 at standby entry (cam_rail_set(false), BEFORE P1.4 — the\n"
            "           vendor's P5-then-P14 order) and re-asserts it only once P1.4 is back;\n"
            "           default OFF: `trim cam on` arms it for this session only, a cold boot clears it.\n");
    return 0;
  }

  if (argc < 3) {
    out.put("trim: usage: trim status | trim pa on|off|cut|restore | trim cam on|off|restore"
            " | trim sd on|off\n");
    return -EINVAL;
  }
  const char* val = argv[2];

  if (strcmp(sub, "pa") == 0) {
    if (strcmp(val, "cut") == 0 || strcmp(val, "restore") == 0) {
      const bool on = strcmp(val, "restore") == 0;
      const bool ok = epd_board_metalio_eink4_plus_pa_rail_set(on);
      out.outf("trim: P1.4 %s now -> %s\n", on ? "re-assert" : "cut", ok ? "done" : "FAILED (TCA write)");
      char line[160];
      snprintf(line, sizeof(line), "trim: P1.4 %s %s (immediate, bench)", on ? "re-assert" : "cut",
               ok ? "ok" : "FAILED");
      standbytrim::journal(line);
      return ok ? 0 : -EIO;
    }
    const bool on = strcmp(val, "on") == 0;
    if (!on && strcmp(val, "off") != 0) {
      out.put("trim: pa takes on|off|cut|restore\n");
      return -EINVAL;
    }
    standbytrim::setOverride(standbytrim::Trim::PaRail, on);
    char line[192];
    snprintf(line, sizeof(line),
             "trim: pa-rail arm -> %s (runtime override; image default %s) -- applies to the NEXT standby",
             on ? "ON" : "OFF", standbytrim::buildDefault(standbytrim::Trim::PaRail) ? "ON" : "OFF");
    standbytrim::journal(line);
    out.outf("%s\n", line);
    printArm(standbytrim::Trim::PaRail);
    return 0;
  }

  if (strcmp(sub, "cam") == 0) {
    // ---- r107 P4 / B15a: THE IMMEDIATE `cut` VERB IS GONE ------------------------------------
    // R103_GLASS_LOG G2: the r105 `trim cam cut` was the first P0.5 cut ever made with a card
    // mounted. The SD bus was transacting against a rail that had just been removed, which
    // produced sdmmc 0x109/0x107/0x108 and `RESET REASON: 4 (PANIC)` and destroyed a card.
    // The r107 refusal could not make the verb safe: it could still be typed while awake against
    // an unmounted card and kill the device. B15a retires the cut half outright. The ONE
    // sanctioned P0.5 cut is the standby arm (`trim cam on` -> park), which unmounts the card
    // first and cuts from inside the parked path. `restore` stays: re-asserting a rail cannot
    // race a mounted card.
    if (strcmp(val, "cut") == 0) {
      out.put("trim cam cut: RETIRED (B15a) -- the P0.5 cut is the standby arm only: `trim cam on` then park\n");
      return -ENOSYS;
    }
    if (strcmp(val, "restore") == 0) {
      const bool ok = epd_board_metalio_eink4_plus_cam_rail_set(true);
      out.outf("trim: P0.5 re-assert now -> %s\n", ok ? "done" : "FAILED (TCA write)");
      char line[160];
      snprintf(line, sizeof(line), "trim: P0.5 re-assert %s (immediate, bench)", ok ? "ok" : "FAILED");
      standbytrim::journal(line);
      return ok ? 0 : -EIO;
    }
    const bool on = strcmp(val, "on") == 0;
    if (!on && strcmp(val, "off") != 0) {
      out.put("trim: cam takes on|off|restore (the immediate cut is retired)\n");
      return -EINVAL;
    }
    standbytrim::setOverride(standbytrim::Trim::CamRail, on);
    char line[192];
    snprintf(line, sizeof(line),
             "trim: cam-rail arm -> %s (runtime override; image default %s) -- applies to the NEXT standby",
             on ? "ON" : "OFF", standbytrim::buildDefault(standbytrim::Trim::CamRail) ? "ON" : "OFF");
    standbytrim::journal(line);
    out.outf("%s\n", line);
    printArm(standbytrim::Trim::CamRail);
    return 0;
  }

  if (strcmp(sub, "sd") == 0) {
    const bool on = strcmp(val, "on") == 0;
    if (!on && strcmp(val, "off") != 0) {
      out.put("trim: sd takes on|off\n");
      return -EINVAL;
    }
    standbytrim::setOverride(standbytrim::Trim::SdPark, on);
    char line[192];
    snprintf(line, sizeof(line),
             "trim: sd-park arm -> %s (runtime override; image default %s) -- applies to the NEXT standby",
             on ? "ON" : "OFF", standbytrim::buildDefault(standbytrim::Trim::SdPark) ? "ON" : "OFF");
    standbytrim::journal(line);
    out.outf("%s\n", line);
    printArm(standbytrim::Trim::SdPark);
    return 0;
  }

  out.put("trim: usage: trim status | trim pa on|off|cut|restore | trim cam on|off|restore"
          " | trim sd on|off\n");
  return -EINVAL;
}
#else
int cmdTrim(int argc, char** argv, Text& out) {
  (void)argc;
  (void)argv;
  out.put("trim: not built for this board (standby trims are Metalio Plus only)\n");
  return -ENOSYS;
}
#endif

// ---------------------------------------------------------------------------
// TTS1 — `audio`: the BH1098G output path.
//
// The chain on this board is ESP I2S DOUT (GPIO44) -> BH1098G (the DAC *and*
// the I2S clock master) -> NS4150 amp (TCA P0.4) -> speaker, with the module
// gated by UART1 (GPIO39 TX / GPIO38 RX, 115200) and powered from the shared
// P1.4 rail. Every pin comes from the vendor board config; see
// src/audio/BtAudioOutput.h for the file:line.
//
// Everything below is bounded: a rail settle of 700 ms, a 700 ms gap between
// AT+RX=2 and AT+MODE=1, a 1000 ms mode-ack deadline, 60 ms per 32 ms DMA
// chunk. Worst case `audio on` is ~2.4 s on the SHELL task, never in the boot
// path or on the UI task. A chip that does not answer faults the session; it is
// not retried, and the rail goes back down.
//
//   audio                       state, gate, rail/amp/I2S, handshake, counters, why
//   audio enable|disable        persist the gate (disable also powers the path down)
//   audio on|off                bring the path up now / power it down now
//   audio tone [hz] [ms]        1 kHz / 300 ms default; lazy power-on
// ---------------------------------------------------------------------------
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
int cmdAudio(int argc, char** argv, Text& out) {
  const char* sub = (argc >= 2) ? argv[1] : "status";
  const auto st = btaudio::status();

  if (strcmp(sub, "status") == 0 || strcmp(sub, "-s") == 0) {
    out.put("audio: BH1098G output path (I2S DOUT GPIO44 -> module DAC -> NS4150 P0.4 -> speaker)\n");
    out.outf("  state        %s\n", btaudio::stateName(st.state));
    out.outf("  gate         %s  (settings key audioOutEnabled; 0 = this build never touches P1.4)\n",
             st.gate ? "ON" : "OFF");
    out.outf("  rail P1.4    %s   amp P0.4 %s   i2s TX %s   handshake %s\n",
             st.railOn ? "HIGH" : "low", st.ampOn ? "on" : "off", st.i2sUp ? "up" : "down",
             st.handshakeOk ? "mode 1 acked" : "no ack");
    out.outf("  power-ons    %u this session   tones played %u   dma timeouts %u   rate %u Hz\n",
             unsigned(st.powerOns), unsigned(st.tones), unsigned(st.writeTimeouts),
             unsigned(st.lastRate));
    out.outf("  last reason  %s\n", st.reason);
    if (st.state == btaudio::State::Faulted) {
      out.put("  NOTE the session is faulted: audio stays off until `audio off` then `audio on`"
              " (or a reboot). Nothing retries on its own.\n");
    }
    out.put("  idle cost    rail P1.4 low and amp off whenever state is off/disabled\n");
    return 0;
  }

  if (strcmp(sub, "enable") == 0 || strcmp(sub, "disable") == 0) {
    const bool on = strcmp(sub, "enable") == 0;
    const uint32_t savesBefore = persiststore::saveCount();
    btaudio::setGate(on);
    // AUDIO3: the setter no longer persists; this command owns the setting, so it
    // writes once. The delta is printed so the count is evidence, not a claim.
    SETTINGS.saveToFile();
    out.outf("audio: gate %s, persisted (audioOutEnabled=%d) in %u settings write(s)\n", on ? "ON" : "OFF",
             on ? 1 : 0, unsigned(persiststore::saveCount() - savesBefore));
    if (!on) out.put("audio: path powered down (amp off, I2S released, P1.4 low)\n");
    return 0;
  }

  if (strcmp(sub, "on") == 0) {
    if (!btaudio::gate()) {
      out.put("audio: refused -- the persisted gate is OFF. Run `audio enable` first.\n");
      return -EPERM;
    }
    const bool ok = btaudio::powerOn();
    const auto s2 = btaudio::status();
    out.outf("audio: power-on %s (%.1f s budget: 700 ms rail settle, 700 ms RX->MODE, <=1000 ms ack)\n",
             ok ? "OK" : "FAILED", 2.4f);
    out.outf("  state %s  rail %s  amp %s  i2s %s  handshake %s\n", btaudio::stateName(s2.state),
             s2.railOn ? "HIGH" : "low", s2.ampOn ? "on" : "off", s2.i2sUp ? "up" : "down",
             s2.handshakeOk ? "acked" : "none");
    out.outf("  reason %s\n", s2.reason);
    if (!ok) out.put("audio: left OFF (rail low). Not retried this session -- `audio on` once more is safe.\n");
    return ok ? 0 : -EIO;
  }

  if (strcmp(sub, "off") == 0) {
    btaudio::powerOff();
    const auto s2 = btaudio::status();
    out.outf("audio: powered down -- rail %s, amp %s, i2s %s (%s)\n", s2.railOn ? "HIGH" : "low",
             s2.ampOn ? "on" : "off", s2.i2sUp ? "up" : "down", s2.reason);
    return 0;
  }

  if (strcmp(sub, "tone") == 0) {
    uint16_t hz = 1000;
    uint16_t ms = 300;
    if (argc >= 3) {
      const int v = atoi(argv[2]);
      if (v >= 50 && v <= 4000) hz = static_cast<uint16_t>(v);
    }
    if (argc >= 4) {
      const int v = atoi(argv[3]);
      if (v >= 20 && v <= 1000) ms = static_cast<uint16_t>(v);
    }
    const bool ok = btaudio::playTone(hz, ms);
    const auto s2 = btaudio::status();
    out.outf("audio: tone %u Hz %u ms -> %s\n", unsigned(hz), unsigned(ms), ok ? "written to I2S" : "FAILED");
    out.outf("  state %s  handshake %s  dma timeouts %u\n", btaudio::stateName(s2.state),
             s2.handshakeOk ? "acked" : "none", unsigned(s2.writeTimeouts));
    out.outf("  reason %s\n", s2.reason);
    if (ok) {
      out.put("  heard it? -> the chain works end to end. Silence here with handshake=acked and\n"
              "  zero dma timeouts means the chip is clocking but not driving the amp/speaker.\n");
    }
    return ok ? 0 : -EIO;
  }

  out.put("audio: usage: audio | audio enable|disable | audio on|off | audio tone [hz] [ms]\n");
  return -EINVAL;
}
#else
int cmdAudio(int argc, char** argv, Text& out) {
  (void)argc;
  (void)argv;
  out.put("audio: not built for this board (the BH1098G path is Metalio Plus only)\n");
  return -ENOSYS;
}
#endif

// ---------------------------------------------------------------------------
// AUDIO1 — `uisound`: the short UI feedback clicks.
//
// The clicks reuse the `audio` path above (same rail, same mode-1 handshake,
// same I2S), so `uisound` never drives hardware itself: it flips the persisted
// switch and, for `test`, calls the same playPcm() the worker calls.
//
//   uisound                     both gates, worker state, counters, event cost
//   uisound on|off              persist SETTINGS.uiSoundsEnabled (needs `audio enable` too)
//   uisound test [name]         play one wave now: click|move|select|back|error
//   uisound policy              the event -> sound map and the drop-if-busy rule
// ---------------------------------------------------------------------------
int cmdUiSound(int argc, char** argv, Text& out) {
  const char* sub = (argc >= 2) ? argv[1] : "status";
  const auto st = uisounds::stats();

  if (strcmp(sub, "status") == 0) {
    out.put("uisound: short UI clicks on the TTS1 output path (async worker, no paint cost)\n");
    out.outf("  master gate  %s  (audioOutEnabled)   ui switch %s  (uiSoundsEnabled)\n", st.master ? "ON" : "OFF",
             st.ui ? "ON" : "OFF");
    out.outf("  armed %d   pathOk %d   worker %s\n", int(st.armed), int(st.pathOk),
             st.workerState == 2 ? "playing" : (st.workerState == 1 ? "idle" : "not created"));
    out.outf("  played %u   queued %u   dropped-busy %u   gated %u   play-failures %u\n",
             unsigned(st.started), unsigned(st.queued), unsigned(st.dropped), unsigned(st.skipped),
             unsigned(st.failed));
    out.outf("  last event cost %u us   last wave %u frames   (%s)\n", unsigned(st.lastCostUs),
             unsigned(st.lastFrames), st.reason);
    out.outf("  trace %s   settings writes this boot %u\n", st.traceOn ? "ON" : "OFF", unsigned(st.saves));
    if (!st.master) out.put("  NOTE both switches must be ON: run `audio enable` then `uisound on`.\n");
    return 0;
  }

  if (strcmp(sub, "on") == 0 || strcmp(sub, "off") == 0) {
    const bool on = strcmp(sub, "on") == 0;
    const uint32_t savesBefore = persiststore::saveCount();
    uisounds::setEnabled(on);
    // AUDIO3: same split as `audio enable` -- the setter moves the gate, this
    // command owns (and writes) the setting, exactly once.
    SETTINGS.saveToFile();
    out.outf("uisound: ui switch %s, persisted (uiSoundsEnabled=%d) in %u settings write(s)\n", on ? "ON" : "OFF",
             on ? 1 : 0, unsigned(persiststore::saveCount() - savesBefore));
    if (on && !btaudio::gate()) out.put("uisound: no clicks yet -- audioOutEnabled is still OFF (`audio enable`)\n");
    return 0;
  }

  if (strcmp(sub, "policy") == 0) {
    out.put("uisound: event -> sound (all five distinct)\n");
    for (int i = 0; i < uisounds::kEventCount; i++) {
      const auto e = static_cast<uisounds::Event>(i);
      out.outf("  %-9s -> %s\n", uisounds::eventName(e), uisounds::soundName(uisounds::soundFor(e)));
    }
    out.put("  `uisound calls` prints a per-call-site ledger (tag x event x outcome) and the\n"
            "  last 32 calls; `uisound log on` traces every event to the serial log at most\n"
            "  once per 200 ms.\n");
    out.put("uisound: a click needs audioOutEnabled AND uiSoundsEnabled AND a healthy path.\n"
            "  one sound plays at a time with ONE queued behind it; anything after that is\n"
            "  DROPPED (counted as dropped-busy), never queued. onEvent() never blocks.\n");
    return 0;
  }

  if (strcmp(sub, "calls") == 0) {
    // AUDIO3: WHICH surface dropped what. One row per (tag, event) that was ever
    // asked for; the outcome columns are the ledger's four counters.
    out.put("uisound: per-call-site ledger (tag/event: started queued dropped skipped)\n");
    const auto& led = uisounds::ledger();
    int rows = 0;
    for (int t = 0; t < uisounds::kTagCount; t++) {  // row 0 = "?" (an untagged caller)
      const auto tag = static_cast<uisounds::Tag>(t);
      for (int e = 0; e < uisounds::kEventCount; e++) {
        const auto ev = static_cast<uisounds::Event>(e);
        if (led.eventsFor(tag, ev) == 0) continue;
        out.outf("  %-10s %-9s %6u %6u %7u %7u\n", uisounds::tagName(tag), uisounds::eventName(ev),
                 unsigned(led.at(tag, ev, uisounds::Policy::Outcome::Started)),
                 unsigned(led.at(tag, ev, uisounds::Policy::Outcome::Queued)),
                 unsigned(led.at(tag, ev, uisounds::Policy::Outcome::DroppedBusy)),
                 unsigned(led.at(tag, ev, uisounds::Policy::Outcome::Skipped)));
        rows++;
      }
    }
    if (rows == 0) out.put("  (no events yet this boot)\n");
    static uisounds::TraceEntry tr[32];
    const int n = uisounds::trace(tr, 32);
    if (n > 0) {
      out.outf("  last %d calls, oldest first:\n", n);
      for (int i = 0; i < n; i++) {
        out.outf("    %6ums  %-10s %-9s %s\n", unsigned(tr[i].ageMs),
                 uisounds::tagName(static_cast<uisounds::Tag>(tr[i].tag)),
                 uisounds::eventName(static_cast<uisounds::Event>(tr[i].event)),
                 uisounds::Ledger::outcomeName(tr[i].outcome));
      }
    }
    return 0;
  }

  if (strcmp(sub, "log") == 0) {
    // AUDIO3: per-event trace to the serial log, rate-limited to one line per
    // 200 ms (a burst is reported as a suppressed COUNT on the next line).
    const bool on = (argc >= 3) ? (strcmp(argv[2], "on") == 0) : !uisounds::traceOn();
    uisounds::setTrace(on);
    out.outf("uisound: trace %s\n", on ? "ON -- one line per event, at most one per 200 ms" : "OFF");
    return 0;
  }

  if (strcmp(sub, "test") == 0) {
    uisounds::Sound s = uisounds::Sound::Click;
    const char* name = (argc >= 3) ? argv[2] : "click";
    bool known = true;
    if (strcmp(name, "click") == 0) s = uisounds::Sound::Click;
    else if (strcmp(name, "move") == 0) s = uisounds::Sound::Move;
    else if (strcmp(name, "select") == 0) s = uisounds::Sound::Select;
    else if (strcmp(name, "back") == 0) s = uisounds::Sound::Back;
    else if (strcmp(name, "error") == 0) s = uisounds::Sound::Error;
    else known = false;
    if (!known) {
      out.put("uisound: test takes click|move|select|back|error\n");
      return -EINVAL;
    }
    const bool ok = uisounds::playNow(s);
    out.outf("uisound: %s -> %s%s\n", name, ok ? "played" : "FAILED", ok ? "" : " (see `audio` and `uisound`)");
    return ok ? 0 : -EIO;
  }

  out.put("uisound: usage: uisound | uisound on|off | uisound test [click|move|select|back|error] | uisound policy"
          " | uisound calls | uisound log [on|off]\n");
  return -EINVAL;
}

// ---------------------------------------------------------------------------
// IMU2 `tilt` -- the flick detector's own instrument.
//
// THE QUESTION IT ANSWERS: "is it even seeing me?" The gesture is a ~130 ms
// lateral acceleration peak, so a person cannot tell by watching whether the
// sensor is alive, whether the axis is the one they think, or whether the
// threshold they are arguing about is the compiled one or a calibration. This
// prints, in ONE frame: whether the part identified at boot, the current raw
// sample in g and in the content frame, the ACTIVE thresholds and where they
// came from, the stored calibration, and how many gestures fired this boot.
//
// READ-ONLY except `tilt clear`, which deletes the stored calibration and puts
// the compiled defaults back. One I2C read, on the shell task (the same access
// pattern `batt` uses for the gauge).
// ---------------------------------------------------------------------------
int cmdTilt(int argc, char** argv, Text& out) {
  if (argc >= 2 && argv[1] != nullptr && strcmp(argv[1], "clear") == 0) {
    halTiltSensor.clearCalibration();
    out.outf("tilt: calibration erased; compiled defaults restored (trigger=%.2f g next=%+d)\n",
             halTiltSensor.config().triggerG, static_cast<int>(halTiltSensor.config().nextSign));
    return 0;
  }
  const bool wantCal = (argc >= 2 && argv[1] != nullptr && strcmp(argv[1], "cal") == 0);
  const HalTiltSensor::Diag d = halTiltSensor.diag();
  static const char* kModes[] = {"OFF", "NORMAL", "INVERTED"};

  out.outf("tilt: flick page turn (a short lateral flick, not a held angle)\n");
  out.outf("  setting:     %s\n", SETTINGS.tiltPageTurn < 3 ? kModes[SETTINGS.tiltPageTurn] : "?");
  out.outf("  sensor:      %s (%s, %s)\n", d.present ? "PRESENT" : "ABSENT",
           d.awake ? "sampling" : "standby", d.capturing ? "capture running" : "not capturing");
  if (!d.present) {
    out.put("               begin() did not identify a part: WHO_AM_I/version failed on this\n"
            "               board's IMU address. Nothing can fire until that is fixed.\n");
  } else if (wantCal) {
    // the calibration block below is the answer this variant was asked for
  } else if (!d.readOk) {
    out.put("  raw:         read failed (no new sample, or I2C error)\n");
  } else {
    out.outf("  raw:         ax=%+.3f ay=%+.3f az=%+.3f g   |a|=%.3f\n", d.ax, d.ay, d.az,
             sqrtf(d.ax * d.ax + d.ay * d.ay + d.az * d.az));
    out.outf("  content x:   lat=%+.3f g  perp=%.3f g   (device frame is already mount-corrected)\n", d.latG,
             d.perpG);
  }
  out.outf("  thresholds:  trigger=%.2f g start=%.2f g nextSign=%+d  [%s]\n", d.triggerG, d.startG,
           static_cast<int>(d.nextSign), d.calibrated ? "CALIBRATED" : "compiled defaults");
  if (d.calibrated) {
    out.outf("  calibration: left=%.2f g right=%.2f g noise=%.2f g (stored in NVS cptilt)\n", d.calLeftPeakG,
             d.calRightPeakG, d.calNoiseG);
  } else {
    out.put("  calibration: none -- compiled defaults; run Settings > Controls > Calibrate flick page turn\n");
  }
  out.outf("  fires:       %u this boot\n", static_cast<unsigned>(d.fires));
  if (d.fires == 0) {
    out.put("               none yet -- flick the device sideways while reading\n");
  } else if (d.lastFireAgeMs != 0xFFFFFFFFul) {
    out.outf("               last: peak=%.2f g sign=%+d, %lu ms ago\n", d.lastPeakG, static_cast<int>(d.lastSign),
             d.lastFireAgeMs);
  } else {
    out.outf("               last: peak=%.2f g sign=%+d\n", d.lastPeakG, static_cast<int>(d.lastSign));
  }
  if (wantCal) {
    out.outf("  nextSign %+d means: onset %s = NEXT page; the mode setting flips it\n", static_cast<int>(d.nextSign),
             d.nextSign > 0 ? "+x" : "-x");
  } else {
    out.put("  `tilt cal` = calibration detail; `tilt clear` = erase the stored calibration\n");
  }
  return 0;
}

// ---------------------------------------------------------------------------
// ver
// ---------------------------------------------------------------------------
int cmdVer(int argc, char** argv, Text& out) {
  (void)argc;
  (void)argv;
  const int64_t up = esp_timer_get_time();
  const unsigned upS = static_cast<unsigned>(up / 1000000);
  out.outf("firmware: %s\n", CROSSPOINT_VERSION);
  out.outf("idf:      %s (target esp32s31, riscv)\n", esp_get_idf_version());
  out.outf("uptime:   %ud %02u:%02u:%02u (%llu ms)\n", upS / 86400, (upS / 3600) % 24, (upS / 60) % 60,
           upS % 60, static_cast<unsigned long long>(up / 1000));
  out.outf("heap int: free=%u minfree=%u largest=%u total=%u\n",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL)));
  out.outf("heap psr: free=%u largest=%u total=%u\n", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
           static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)));
  out.outf("shell:    %s (CP_USB_SHELL=1, session=%s)\n", s_running ? "running" : "stopped",
           s_session ? "on" : "off");
  // TCH5a: the FT6336U chip-id / firmware registers. The boot line that carries
  // them is emitted before the log sink exists, so this line is the only way to
  // ask "did the vendor V04 controller update apply?" without log archaeology.
  // This is a LIVE I2C read (under the shared I2C1 bus lock, same mutex the
  // touch feed task takes), not the init-time cache: re-init paths rebuild the
  // cache, and a cached flag once made this line say "no chip id" on a
  // controller that had answered seconds earlier. Two reads, only when `ver`
  // runs. A failure is always named -- an empty or optimistic line is worse than
  // no line, because it would read as "the V04 update did not apply".
  const metalio_touch_id_live_t touchId = metalio_touch_read_id_live();
  if (touchId.ok) {
    out.outf("touch:    chip=0x%02X fw=0x%02X (%s, live read)\n", touchId.chip, touchId.fw,
             touchId.chip == METALIO_TOUCH_FT6336_CHIP_ID ? "FT6336U" : "unknown part");
  } else if (touchId.chip_err != 0) {
    out.outf("touch:    live read failed (0xA3: %s 0x%X)\n", esp_err_to_name((esp_err_t)touchId.chip_err),
             static_cast<unsigned>(touchId.chip_err));
  } else {
    out.outf("touch:    live read failed (0xA6: %s 0x%X)\n", esp_err_to_name((esp_err_t)touchId.fw_err),
             static_cast<unsigned>(touchId.fw_err));
  }
  out.outf("panel:    %s\n", s_lastPanelLine[0] ? s_lastPanelLine : "(no drive since boot)");
  return 0;
}


// ---------------------------------------------------------------------------
// r96 `cpu` — per-task run time, idle% per core, actual MHz.
// LOCAL: reads uxTaskGetSystemState() and two counters, touches no UI state.
//
// r97: the whole bench block below is compiled ONLY when its switch is on. Before
// r97 these handlers existed unconditionally and answered "not built (...=0)", which
// meant the SHIPPING image still carried the command names and the not-built strings.
// HEAD is the shipping image now, so the commands do not exist at all with the
// switches at 0 -- and `help` does not advertise them.
// ---------------------------------------------------------------------------
#if CP_CPU_STATS
int cmdCpu(int argc, char** argv, Text& out) {
  (void)argc;
  (void)argv;
  char body[1150];
  cpustats::formatCpu(body, sizeof(body));
  out.put(body);
  return 0;
}

// ---------------------------------------------------------------------------
// r96 `pm` — compile-time PM state, MHz, locks, light-sleep time.
// LOCAL: reads HalPowerManager fields guarded by its own mutex.
// ---------------------------------------------------------------------------
int cmdPm(int argc, char** argv, Text& out) {
  (void)argc;
  (void)argv;
  char body[900];
  cpustats::formatPm(body, sizeof(body));
  out.put(body);
  return 0;
}

// ---------------------------------------------------------------------------
// r96 `bench render` — the COMPOSE-vs-DRIVE split of the LAST page render.
// LOCAL. It reads a snapshot; it does not repaint anything, so running it never
// costs a drive and never disturbs the page on the glass.
// ---------------------------------------------------------------------------
int cmdBench(int argc, char** argv, Text& out) {
  if (argc < 2 || strcmp(argv[1], "render") != 0) {
    out.put("usage: bench render\n");
    out.put("       reports the compose/drive/glyph/image/layout split of the most recent page render.\n");
    out.put("       Turn the page you want measured, then run it.\n");
    return -EINVAL;
  }
  char body[1000];
  cpustats::formatRender(body, sizeof(body));
  out.put(body);
  return 0;
}
#endif  // CP_CPU_STATS

// ---------------------------------------------------------------------------
// BENCH1 `benchdrives [runs]` — the DRIVE2 instrument.
//
// LOCAL: it only ARMS a state machine and returns, so the reply is on the wire before the first
// transition paints. The sequence itself runs on the main task (main.cpp loop() ->
// benchdrives::loop()), one transition per pass, through the same ActivityManager entry points a
// gesture uses. Nothing here measures: the per-step ledger is read from the panel shim's existing
// `[panel] show mode=.. ms=..` journal line, so this command cannot change a drive.
//
// The ledger arrives in the LOG STREAM, not in this reply — a run is ~20 s, far longer than a
// command frame is meant to live. Read it with the same tail that shows `[PANEL]`/`[MAP]` lines,
// or grep for `[BENCH]`.
// ---------------------------------------------------------------------------
int cmdBenchDrives(int argc, char** argv, Text& out) {
  int runs = 3;
  if (argc >= 2) {
    const long v = strtol(argv[1], nullptr, 10);
    if (v < 1 || v > 5) {
      out.put("benchdrives: runs must be 1..5 (default 3; one e-ink transition varies, so 3)\n");
      return -EINVAL;
    }
    runs = static_cast<int>(v);
  }
  if (benchdrives::running()) {
    char st[24];
    benchdrives::statusText(st, sizeof(st));
    out.outf("benchdrives: already running (%s) -- wait for the aggregate line\n", st);
    return -EBUSY;
  }
  if (!benchdrives::start(static_cast<uint8_t>(runs))) {
    out.put("benchdrives: refused -- debug mode is OFF (Debug > Debug mode), or the device is not ready\n");
    return -EPERM;
  }
  out.outf("benchdrives: armed %d run(s) x 15 scripted steps; starts in %d ms and ends on Home\n", runs,
           800);
  out.put("             one line per step in the log: [BENCH] drive2 run R/N: step=.. drives=.. modes=.. ms=.. EXPECT<=1\n");
  out.put("             then: [BENCH] drive2 aggregate: total drives= expected= over= worst=\n");
  out.put("             needs no input; do not touch the device until the 'drive2 done' line\n");
  return 0;
}

#if CP_LP_BENCH
// ---------------------------------------------------------------------------
// r96 `lpbench [n]` — LP core iteration rate + completion time.
// LOCAL: it starts and stops the LP core and nothing else in the firmware starts
// it, so there is no shared state to serialize against.
// ---------------------------------------------------------------------------
int cmdLpBench(int argc, char** argv, Text& out) {
  int runs = 8;
  if (argc >= 2) {
    const long v = strtol(argv[1], nullptr, 10);
    if (v < 1 || v > 32) {
      out.put("lpbench: n must be 1..32\n");
      return -EINVAL;
    }
    runs = static_cast<int>(v);
  }
  const lpcore::Bench b = lpcore::bench(runs);
  if (!b.built) {
    out.outf("lpbench: %s\n", b.benchBin ? "not built" : "no bench program in this build");
    return 0;
  }
  out.outf("lpbench: %s\n", b.pass ? "pass" : "FAIL");
  out.outf("runs:       %d of %d requested\n", b.runs, runs);
  out.outf("hb delta:   min=%lu mean=%lu max=%lu (per %lu ms sample gap)\n",
           static_cast<unsigned long>(b.deltaMin), static_cast<unsigned long>(b.deltaMean),
           static_cast<unsigned long>(b.deltaMax), static_cast<unsigned long>(b.sampleGapMs));
  out.outf("rate:       mean=%.0f/s min=%.0f/s max=%.0f/s\n", b.rateMean, b.rateMin, b.rateMax);
  out.outf("spread:     %.1f%% of mean\n",
           b.rateMean > 0 ? (b.rateMax - b.rateMin) * 100.0 / b.rateMean : 0.0);
  if (b.doneMs >= 0) {
    out.outf("done:       %lld ms for the bounded r87 program (400000 iterations)\n",
             static_cast<long long>(b.doneMs));
  } else {
    out.outf("done:       not observed inside 2000 ms (the bounded r87 program had not finished)\n");
  }
  out.outf("images:     bench=%lu B core=%lu B (LP RAM reserved: %d B)\n", static_cast<unsigned long>(b.benchBin),
           static_cast<unsigned long>(b.coreBin), CONFIG_ULP_COPROC_RESERVE_MEM);
  return 0;
}

// ---------------------------------------------------------------------------
// r99 `lpsentry [sec]` — the WAKE-SOURCE INSTRUMENT (docs/LP_CORE_CAPABILITY.md
// §7 step 3). Runs the sentry ULP program in RECORD-ONLY mode for `sec` seconds
// with the HP core awake, then reports what it saw.
//
// What it answers, and why each field is here:
//   * `wakes` — how many times the LP core woke on its OWN timer versus on LP_IO
//     (GPIO2). A timer-dominated count with a live hb is the evidence that the LP
//     core executes independently of the HP core; an io-dominated one says GPIO2
//     is what brings it back, which is exactly what the pre-filter acts on.
//   * `pressed` — debounced power-key assertions the LP side would have raised an
//     HP wake for. Compared against the HP-side journal it prices the filter.
//   * `level5`/`edges5` — the FT6336U INT line. GPIO5 is inside the LP IO window
//     (SOC_RTCIO_PIN_COUNT=8), so the LP core CAN see a touch. It cannot read the
//     touch: coordinates come over I2C, which LP does not own. This command is how
//     that distinction gets measured instead of argued.
//   * `hb` — one per LP wakeup. A frozen hb with a set magic means the program
//     started and the LP core then stopped executing.
//
// gpio2 is taken into the RTC-IO function for the run and given back before the
// answer is printed, so nothing after this command sees a reconfigured pad.
// ---------------------------------------------------------------------------
int cmdLpSentry(int argc, char** argv, Text& out) {
  uint32_t sec = 10;
  if (argc >= 2) {
    const long v = strtol(argv[1], nullptr, 10);
    if (v < 1 || v > 60) {
      out.put("lpsentry: sec must be 1..60\n");
      return -EINVAL;
    }
    sec = static_cast<uint32_t>(v);
  }
  if (!lpcore::sentryBegin(/*arm=*/false)) {
    out.put("lpsentry: not built — needs CONFIG_ULP_COPROC_ENABLED + CP_LP_CORE in a bench image\n");
    return -ENOSYS;
  }
  const lpcore::LpSentry a = lpcore::sentryRead();
  const int64_t t0 = esp_timer_get_time();
  vTaskDelay(pdMS_TO_TICKS(sec * 1000));
  const lpcore::LpSentry b = lpcore::sentryRead();
  lpcore::sentryEnd();
  const int64_t elapsedUs = esp_timer_get_time() - t0;
  const uint32_t dh = b.hb - a.hb;

  out.outf("lpsentry: %s (%lu s, HP awake, record-only)\n", b.alive ? "ran" : "FAIL", static_cast<unsigned long>(sec));
  out.outf("  hb:        +%lu over %lldus -> %.1f/s   magic=0x%08lx ver=%lu\n", static_cast<unsigned long>(dh),
           static_cast<long long>(elapsedUs), elapsedUs > 0 ? dh * 1e6 / elapsedUs : 0.0,
           static_cast<unsigned long>(b.magic), static_cast<unsigned long>(b.ver));
  out.outf("  wakes:     timer=%lu io=%lu other=%lu  last_cause=0x%lx\n",
           static_cast<unsigned long>(b.wakeTimer), static_cast<unsigned long>(b.wakeIo),
           static_cast<unsigned long>(b.wakeOther), static_cast<unsigned long>(b.lastCause));
  out.outf("  gpio2:     level=%lu edges=%lu low=%lu  pressed=%lu  (expander INT / power key)\n",
           static_cast<unsigned long>(b.level2), static_cast<unsigned long>(b.edges2),
           static_cast<unsigned long>(b.low2), static_cast<unsigned long>(b.pressed));
  out.outf("  gpio5:     level=%lu edges=%lu  (FT6336U INT — LP sees the line, not the data)\n",
           static_cast<unsigned long>(b.level5), static_cast<unsigned long>(b.edges5));
  out.outf("  woke:      %lu (0 = record-only)   image=%lu B\n", static_cast<unsigned long>(b.woke),
           static_cast<unsigned long>(b.binSize));
  return b.alive ? 0 : -EIO;
}
#endif  // CP_LP_BENCH

// ---------------------------------------------------------------------------
// fbrow <y0> [y1] [map] -- what the CURRENT canvas actually holds for those rows.
//
// WHY IT EXISTS. A header rule, a popup frame and a dialog border are three
// different widgets on three code paths, and all three read to the operator as
// "not a solid line". fillRect(...,true) provably writes whole rows of nibble
// 0x0 (GfxRenderer.cpp fillRectImpl<Color::Black>, 4 bpp branch), so the open
// question is whether the CANVAS holds one solid run or whether the panel drive
// breaks it up. This command answers the canvas half from the bytes themselves,
// which is what makes the next change falsifiable instead of another guess.
//
// READ-ONLY: no paint, no drive, no panel register. It takes the render lock
// with Try and refuses rather than blocking the shell task behind a waveform.
//
// SPACE. Rows are LOGICAL rows of the Portrait frame the Settings chrome and
// every dialog are drawn in (684 wide x 1216 tall on this panel). Portrait maps
// logical (x,y) -> physical (phyX=y, phyY=panelHeight-1-x), so one logical row
// is a COLUMN of the physical buffer: it is walked here and reported in logical
// x order (x=0 is the left edge on the glass).
//
// Every row gets a VERDICT, because the three failure classes need different
// fixes and must not be confusable:
//   SOLID   every pixel is nibble 0x0 -- the stroke the code claims to draw
//   PARTIAL ink plus paper in one row -- broken/gapped or edge-only stroke
//   DITHER  dark pixels but not one pure 0x0 -- a halftone, never a fill
//   GREY    an optical level (0x1..0xE) is present -- glyph AA or a grey wash
//   BLANK   no dark pixel at all -- nothing was drawn here
//      (PARTIAL/GREY also report runs, and `map` gives a coarse ASCII profile:
//       '#' 8/8 ink, '+' some ink, '.' none, 'g' a non-binary level.)
// ---------------------------------------------------------------------------
int cmdFbrow(int argc, char** argv, Text& out) {
  if (argc < 2) {
    out.put("fbrow: usage: fbrow <y0> [y1] [map]   (logical rows, Portrait)\n");
    return -EINVAL;
  }
  // Optional third token "map": a coarse ink profile, one char per 8 px.
  bool wantMap = false;
  for (int i = 2; i < argc; ++i) {
    if (argv[i] != nullptr && strcmp(argv[i], "map") == 0) wantMap = true;
  }
  const int pw = display.getDisplayWidth();        // physical columns (1216)
  const int ph = display.getDisplayHeight();       // physical rows (684)
  const int stride = display.getDisplayWidthBytes();
  if (pw <= 0 || ph <= 0 || stride <= 0) {
    out.put("fbrow: panel geometry not ready\n");
    return -ENODEV;
  }
  const bool fourBit = (stride * 2 == pw);
  const int logicalW = ph;   // Portrait: logical width  == physical rows
  const int logicalH = pw;   // Portrait: logical height == physical columns

  long y0 = strtol(argv[1], nullptr, 10);
  long y1 = y0;
  for (int i = 2; i < argc; ++i) {
    if (argv[i] != nullptr && argv[i][0] >= '0' && argv[i][0] <= '9') {
      y1 = strtol(argv[i], nullptr, 10);
      break;
    }
  }
  if (y0 > y1) {
    const long t = y0;
    y0 = y1;
    y1 = t;
  }
  if (y0 < 0) y0 = 0;
  if (y1 > logicalH - 1) y1 = logicalH - 1;
  if (y0 > y1) {
    out.outf("fbrow: y out of range (0..%d)\n", logicalH - 1);
    return -EINVAL;
  }
  // The map costs 86 chars/row, the counts line ~66; both share one 1280 B Text.
  const int kMaxRows = wantMap ? 8 : 16;
  const bool clipped = (y1 - y0 + 1) > kMaxRows;
  if (clipped) y1 = y0 + kMaxRows - 1;

  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) {
    out.put("fbrow: render lock busy (a frame or a drive is in progress) -- retry\n");
    return -EBUSY;
  }
  const uint8_t* fb = display.getFrameBuffer();
  if (fb == nullptr) {
    out.put("fbrow: framebuffer not allocated (lent to a chapter build?)\n");
    return -ENODEV;
  }

  out.outf("fbrow: %dx%d phys, %dx%d logical Portrait, %s, rows %ld..%ld\n", pw, ph, logicalW, logicalH,
           fourBit ? "4bpp ink=0x0" : "1bpp ink=1", y0, y1);
  int solidRows = 0;
  for (long y = y0; y <= y1; ++y) {
    int dark = 0;
    int solid = 0;
    int grey = 0;
    int runs = 0;
    int runLen = 0;
    int prev = -1;
    char runTxt[112];
    size_t rt = 0;
    runTxt[0] = '\0';
    char map[96];   // 684 logical px / 8 = 86 cells, +NUL
    int mapLen = 0;
    map[0] = '\0';
    int cellInk = 0;    // pure ink pixels in this 8 px cell
    int cellDark = 0;   // dark pixels in this 8 px cell
    int cellGrey = 0;   // non-binary pixels in this 8 px cell
    for (int x = 0; x < logicalW; ++x) {
      const int phyX = static_cast<int>(y);
      const int phyY = ph - 1 - x;
      uint8_t nib;
      if (fourBit) {
        const uint8_t b = fb[static_cast<size_t>(phyY) * stride + (phyX >> 1)];
        nib = (phyX & 1) ? static_cast<uint8_t>(b >> 4) : static_cast<uint8_t>(b & 0x0F);
      } else {
        const uint8_t b = fb[static_cast<size_t>(phyY) * stride + (phyX >> 3)];
        nib = ((b >> (7 - (phyX & 7))) & 1) ? 0x0F : 0x00;
      }
      const bool isDark = nib <= 7;
      const bool isInk = nib == 0x00;
      const bool isGrey = (nib != 0x00 && nib != 0x0F);
      if (isDark) ++dark;
      if (isInk) ++solid;
      else if (isGrey) ++grey;
      if (isInk) ++cellInk;
      if (isDark) ++cellDark;
      if (isGrey) ++cellGrey;
      if (nib == prev) {
        ++runLen;
      } else {
        if (prev >= 0) {
          ++runs;
          if (runs <= 6) {
            rt += static_cast<size_t>(snprintf(runTxt + rt, sizeof(runTxt) - rt, "%s%dx%X", runs > 1 ? " " : "",
                                               runLen, prev));
          }
        }
        prev = nib;
        runLen = 1;
      }
      if (wantMap && ((x & 7) == 7 || x == logicalW - 1)) {
        const int cell = (x & 7) + 1;
        if (cellGrey > 0) map[mapLen++] = 'g';
        else if (cellInk == cell) map[mapLen++] = '#';
        else if (cellDark > 0) map[mapLen++] = '+';
        else map[mapLen++] = '.';
        cellInk = cellDark = cellGrey = 0;
      }
    }
    if (prev >= 0) {
      ++runs;
      if (runs <= 6) {
        rt += static_cast<size_t>(snprintf(runTxt + rt, sizeof(runTxt) - rt, "%s%dx%X", runs > 1 ? " " : "", runLen, prev));
      }
    }
    if (runs > 6) {
      snprintf(runTxt + rt, sizeof(runTxt) - rt, " +%d", runs - 6);
    }
    if (wantMap) map[mapLen] = '\0';
    const char* verdict = solid == logicalW ? "SOLID"
                          : dark == 0      ? "BLANK"
                          : grey > 0       ? "GREY"
                          : solid == 0     ? "DITHER"
                                           : "PARTIAL";
    if (solid == logicalW) ++solidRows;
    out.outf("y=%4ld %-7s w=%3d dark=%3d solid=%3d grey=%3d runs=%2d %s\n", y, verdict, logicalW, dark, solid,
             grey, runs, runTxt);
    if (wantMap) out.outf("     |%s|\n", map);
  }
  out.outf("fbrow: %d/%ld rows SOLID across the full width\n", solidRows, y1 - y0 + 1);
  if (clipped) out.outf("fbrow: range capped at %d rows -- call again with y0=%ld\n", kMaxRows, y1 + 1);
  return 0;
}

// ---------------------------------------------------------------------------
// r106 B0..B4 read-only diagnostics — added so the SHIPPING shell can run the whole
// r106 test list without another flash round. ALL of these are read-only commands that
// run on the shell task: no pollers, no hot-path I2C, nothing persisted, no bench guard.
//
//   ioexp   TCA9555: input port0/port1, OUTPUT port0/port1 (the B1 registers), latches,
//           the B1 fallback shadow, and which bits are named on this board.
//   clock   esp_timer/uptime vs the RTC counter vs the PCF8563 calendar (+VL) and ppm.
//   sd      mount state, capacity/used, last mount result, SD trim + last pad release/restore.
//   light   brightness/warmth/duty and whether the park pads are released or re-applied.
//   radio   Wi-Fi mode/connected/SSID, whether esp_wifi_deinit is applied since the last
//           park, and the fact that this image has no BT controller.
// ---------------------------------------------------------------------------
int cmdIoexp(int argc, char** argv, Text& out) {
  (void)argc;
  (void)argv;
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  epd_ioexp_dump_t d{};
  const bool ok = epd_board_metalio_eink4_plus_ioexp_dump(&d);
  if (!ok) {
    out.put("ioexp: TCA9555 not ready (expander init failed) -- nothing to read\n");
    return -EIO;
  }
  out.put("ioexp: TCA9555 four port bytes -- 0x02/0x03 are the B1 read-modify-write targets\n");
  char in0[16];
  char in1[16];
  if (d.in0_ok) {
    snprintf(in0, sizeof(in0), "0x%02X", static_cast<unsigned>(d.in0));
  } else {
    snprintf(in0, sizeof(in0), "READ FAILED");
  }
  if (d.in1_ok) {
    snprintf(in1, sizeof(in1), "0x%02X", static_cast<unsigned>(d.in1));
  } else {
    snprintf(in1, sizeof(in1), "READ FAILED");
  }
  out.outf("  input  port0=%s port1=%s   (reg 0x00/0x01: PIN LEVELS, never an output byte)\n", in0, in1);
  out.outf("  output port0=0x%02X%s port1=0x%02X%s   (reg 0x02/0x03: the OUTPUT latches)\n", d.out0,
           d.out0_ok ? "" : " (READ FAILED)", d.out1, d.out1_ok ? "" : " (READ FAILED)");
  out.outf("  latch  port0=0x%02X (p0_output_value) port1=0x%02X (s_p1_out)  shadow0=0x%02X%s\n", d.latch0,
           d.latch1, d.shadow0, d.shadow0_valid ? "" : " (none yet: no successful read/write)");
  out.put("  named  P0.5=CAM_SCR P1.4=BT/PA_PWR P1.2=TP_RST P1.6=RTC_ALARM P0.7=PWR_GOOD(input)\n");
  out.outf("  state  CAM_SCR(P0.5)=%d BT_PA(P1.4)=%d TP_RST(P1.2)=%d RTC_ALARM(P1.6)=%d PWR_GOOD(P0.7)=%d\n",
           (d.out0 & 0x20) ? 1 : 0, (d.out1 & 0x10) ? 1 : 0, (d.out1 & 0x04) ? 1 : 0, (d.out1 & 0x40) ? 1 : 0,
           (d.in0 & 0x80) ? 1 : 0);
  return 0;
#else
  out.put("ioexp: this board has no TCA9555 expander\n");
  return -ENOSYS;
#endif
}

int cmdClock(int argc, char** argv, Text& out) {
  (void)argc;
  (void)argv;
  const uint64_t esp = esp_timer_get_time();
  const uint64_t rtc = hybridclock::rtcUs();
  out.put("clock: the three time sources this tree reads, side by side (no I2C is written here)\n");
  out.outf("  esp_timer   %llu us (%lu ms)   <- XTAL; NOT clocked in light sleep (the IDF advances it by the RTC length)\n",
           static_cast<unsigned long long>(esp), static_cast<unsigned long>(esp / 1000ULL));
  out.outf("  rtc counter %llu us (%lu ms)   <- internal RC; the ONLY clock that keeps counting while parked\n",
           static_cast<unsigned long long>(rtc), static_cast<unsigned long>(rtc / 1000ULL));
  struct tm tmv{};
  char cal[48] = "unavailable";
  if (halClock.localTime(tmv)) {
    snprintf(cal, sizeof(cal), "%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  }
  out.outf("  pcf8563     %s  (local, TZ applied) VL=%s%s\n", cal,
           halClock.voltageLow() ? "SET (calendar is a stale value, not elapsed time)" : "clear",
           halClock.voltageLowKnown() ? "" : " [not read yet this boot]");
  const time_t epoch = time(nullptr);
  out.outf("  epoch       %lld s (gettimeofday/time(); RC-backed -- NOT the calendar)\n",
           static_cast<long long>(epoch));
  char drift[32];
  out.outf("  stored ppm  %s (applied to the PARKED span only; `batt drift=<ppm>` sets it, NVS)\n",
           hybridclock::driftLabel(drift, sizeof(drift)));
  {
    const hybridclock::Mark& pm = hybridclock::parkMark();
    const hybridclock::Segment& seg = hybridclock::lastSegment();
    if (pm.valid) {
      out.outf("  last mark   epoch=%lld uptime=%lu ms rtc=%llu us valid=%d\n", static_cast<long long>(pm.epoch),
               pm.uptimeMs, static_cast<unsigned long long>(pm.rtcUs), seg.valid ? 1 : 0);
    } else {
      out.put("  last mark   none this session\n");
    }
  }
  return 0;
}

int cmdSd(int argc, char** argv, Text& out) {
  const char* sub = (argc >= 2 && argv[1] != nullptr) ? argv[1] : "";
  // ---- r109 CARD3: umount/mount are DESTRUCTIVE DIAGNOSTICS, not routine read-outs ------------
  // Both verbs power-cycle the card (both edges of the OEM's timed sequence), and on 10 Nov 2026
  // this pair sat in front of the sequence that came back with a lost directory tree. They are
  // therefore gated on an explicit token: an operator typing `sd umount` at a prompt is not the
  // same statement as an operator who has been told what it costs.
  const bool sdUmount = strcmp(sub, "umount") == 0;
  const bool sdMount = strcmp(sub, "mount") == 0;
  if (sdUmount || sdMount) {
    const bool confirmed = argc >= 3 && argv[2] != nullptr && strcmp(argv[2], "confirm") == 0;
    if (!confirmed) {
      out.outf("sd %s: this power-cycles the card; add 'confirm' if you mean it\n", sub);
      return -EINVAL;
    }
  }
  // ---- r107 P2: `sd umount` / `sd mount` -----------------------------------------------------
  // WHY: a rail bench needs a SAFE unmount before any rail cut. On r105 `trim cam cut` ran with
  // a card mounted, the SD bus was still transacting when P0.5 went away, and the result was an
  // sdmmc abort followed by a panic (R103_GLASS_LOG §G2). Until r107 there was no way to take
  // the card down from the shell at all. These two verbs are the SAME storage pair the standby
  // park and the resume use -- Storage.prepareForDeepSleep() (+ the CNNT pad release) to go down,
  // pad restore + Storage.begin() (with its one retry) to come back -- with NO park entered and
  // no trim armed, so they work on the shipping image without a bench guard.
  //
  // Nothing here can run while the device is mid-standby: the rails are already cut then, and a
  // mount would adopt pads under a dead rail.
  if (sdUmount || sdMount) {
    const bool up = sdMount;
    if (standbytrim::parked()) {
      out.outf("sd %s: REFUSED -- the device is in standby (rails parked). Wake it first.\n", sub);
      return -EBUSY;
    }
    auto& sd0 = SDCardManager::getInstance();
    out.outf("sd %s: BEFORE mounted=%d (Storage.ready) pads=%s\n", sub, Storage.ready() ? 1 : 0,
             parksink::sdPadsReleased() ? "RELEASED (held parked)" : "claimed");
    if (!up) {
      if (!Storage.ready()) {
        out.outf("sd umount: already unmounted (mounted=0) -- nothing to take down%s\n",
                 parksink::sdPadsReleased() ? "" : "; releasing the pads anyway");
      } else {
        // Drain the log ring FIRST: after the unmount there is no file to write to, and the
        // lines have to be on the card before the volume goes away.
        logSdFlush();
        Storage.prepareForDeepSleep();
      }
      const bool padsOk = parksink::sdPadsRelease();
      const unsigned long freeMb = static_cast<unsigned long>((sd0.sdTotalBytes()) / (1024ULL * 1024ULL));
      out.outf("sd umount: AFTER mounted=%d (Storage.ready) pads=%s%s\n", Storage.ready() ? 1 : 0,
               parksink::sdPadsReleased() ? "RELEASED (ded_sel=0, pull-downs on)" : "still claimed",
               padsOk ? "" : "  -- pad release FAILED (ded_sel read-back), see `sd`");
      out.outf("sd umount: volume is down; capacity cached before the unmount was %lu MB. `logtail`/`loggrep`\n"
               "           will now report 'cannot open' and log lines stay in the RAM ring (`logs`).\n",
               freeMb);
      return padsOk ? 0 : -EIO;
    }
    // mount: re-claim the CNNT pads BEFORE the mount that first touches them (same order as the
    // resume path: ded_sel must read 1 before sdmmc_host_init adopts the pads).
    const bool padsBack = parksink::sdPadsRestore();
    const unsigned long t0 = millis();
    bool ok = Storage.begin();
    if (!ok) {
      delay(20);
      ok = Storage.begin();  // one retry: the mount power-cycles the card and can miss once
    }
    const HalStorage::LastMount lm = Storage.lastMount();
    // r109 CARD2: same rule as the boot -- a successful remount does not vouch for the volume.
    if (ok) {
      const bool vouched = Storage.checkVolumeMarker();
      out.outf("sd mount: volume marker %s\n", vouched ? "present -- storage is writable"
                                                       : "MISSING -- storage is read-only this boot (use `sd init`/`sd adopt`)");
    }
    out.outf("sd mount: AFTER mounted=%d pads=%s mount=%s in %lu ms\n", Storage.ready() ? 1 : 0,
             parksink::sdPadsReleased() ? "RELEASED (held parked)" : "claimed", ok ? "OK" : "FAILED",
             millis() - t0);
    out.outf("sd mount: pads re-claimed ok=%d; last mount %s at t=%lu ms%s\n", padsBack ? 1 : 0,
             lm.ok ? "OK" : "FAILED", lm.ms, padsBack ? "" : " (pad restore FAILED, ded_sel still 0)");
    return ok && padsBack ? 0 : -EIO;
  }
  // ---- r109 CARD2/FORMAT1: `sd init` -- the ONLY path that creates a fresh volume layout ------
  // WHY: a mount that succeeds on a BLANK or FOREIGN volume used to be answered by writing the
  // default settings and an empty library index over whatever was there. On 10 Nov 2026 that is
  // how the user's books and reading positions were destroyed. The boot now refuses (read-only
  // boot) and says so; this verb is the operator's explicit "yes, initialise this card".
  if (strcmp(sub, "init") == 0) {
    auto& sd = SDCardManager::getInstance();
    if (!Storage.ready()) {
      if (!sd.begin()) {
        out.put("sd init: REFUSED -- the card is not mounted, so there is nothing to initialise\n");
        return -EIO;
      }
    }
    const bool ok = Storage.initVolumeLayout();
    out.outf("sd init: %s\n", ok ? "volume layout created: /.crosspoint/.volume + /.crosspoint (no book data was touched)"
                                 : "FAILED to create the volume layout, see the log");
    out.outf("sd init: marker=%d/.crosspoint/.volume  (a volume without it is refused on the next boot)\n",
             Storage.exists("/.crosspoint/.volume") ? 1 : 0);
    return ok ? 0 : -EIO;
  }
  // ---- r110: `sd adopt` -- vouch for a card that HAS content but no settings.json ------------
  // WHY: r110 refuses to adopt a volume just because its root holds entries (r109 did, so a camera
  // card with DCIM was adopted and then initialised). The legitimately restored card -- books back
  // from a backup, no settings.json, no marker -- is therefore refused at boot, and the user needs
  // a way to say "this content is mine" WITHOUT laying out a fresh card (`sd init` is the creator,
  // this is the voucher). Writes ONE file: /.crosspoint/.volume. Nothing else is touched.
  if (strcmp(sub, "adopt") == 0) {
    auto& sd = SDCardManager::getInstance();
    if (!Storage.ready()) {
      if (!sd.begin()) {
        out.put("sd adopt: REFUSED -- the card is not mounted, so there is nothing to adopt\n");
        return -EIO;
      }
    }
    if (Storage.exists("/.crosspoint/settings.json")) {
      out.put("sd adopt: this card already has /.crosspoint/settings.json -- it is a legacy install and\n"
              "  the boot adopts it by itself; nothing was written\n");
      return 0;
    }
    const bool ok = Storage.adoptVolume();
    out.outf("sd adopt: %s\n", ok ? "marker written -- this volume is vouched for, writes are allowed now"
                                   : "FAILED -- no marker was written, storage stays read-only");
    out.outf("sd adopt: marker=%d /.crosspoint/.volume (a volume without it is refused on the next boot)\n",
             Storage.exists("/.crosspoint/.volume") ? 1 : 0);
    out.put("sd adopt: settings.json / library.idx are created by the NEXT boot, not here\n");
    return ok ? 0 : -EIO;
  }
  const bool ready = Storage.ready();
  auto& sd = SDCardManager::getInstance();
  const uint64_t total = sd.sdTotalBytes();
  const uint64_t used = sd.sdUsedBytes();
  out.outf("sd: mounted=%d (Storage.ready)  card=%s\n", ready ? 1 : 0,
           total == 0 ? "capacity unknown (not mounted this boot?)" : "present");
  // r109 CARD2: whether this boot may write at all. A mounted card is not a vouched-for card.
  out.outf("    storage writes: %s\n",
           Storage.readOnly() ? "REFUSED -- read-only boot: the volume has no marker (run `sd init` on a fresh card, `sd adopt` on your own)"
                              : "allowed (the volume is marked or adopted)");
  if (total != 0) {
    // r109 SD2(b): a card whose FAT scan did not answer must not be reported as `used=0 MB`. On the
    // 10 Nov card `sd` printed `used=0 MB` with content restored, and `sd` is the health check after
    // every rail experiment -- a number nobody computed is worse than no number.
    if (sd.sdUsedBytesKnown()) {
      out.outf("    capacity=%llu MB used=%llu MB free=%llu MB\n",
               static_cast<unsigned long long>(total / (1024ULL * 1024ULL)),
               static_cast<unsigned long long>(used / (1024ULL * 1024ULL)),
               static_cast<unsigned long long>((total - (used > total ? total : used)) / (1024ULL * 1024ULL)));
    } else {
      out.outf("    capacity=%llu MB used=<unknown: not computed> free=<unknown: not computed>\n",
               static_cast<unsigned long long>(total / (1024ULL * 1024ULL)));
      out.put("      (freeClusterCount() did not answer on this mount, so no used/free figure exists;\n"
              "       read it again after `sd mount confirm` rather than treating 0 as an empty card)\n");
    }
  } else {
    out.put("    capacity = 0: the volume is not mounted, so no size is cached\n");
  }
  const HalStorage::LastMount lm = Storage.lastMount();
  if (lm.ran) {
    out.outf("    last mount %s at t=%lu ms%s\n", lm.ok ? "OK" : "FAILED", lm.ms,
             lm.ok ? "" : "  (no esp_err_t is kept by HalStorage; the driver error is in the journal)");
  } else {
    out.put("    last mount not recorded this boot\n");
  }
  // r109 SD2(c): the one home the intermittent `sdmmc 0x107` has. The driver logs it to Serial
  // only; this line carries the last failing code and when it happened into the glass log.
  const int lastSdErr = sd.lastSdmmcErrorCode();
  if (lastSdErr != 0) {
    out.outf("    last sdmmc error: 0x%X (%s) at t=%lu ms%s\n", lastSdErr, esp_err_to_name(lastSdErr),
             sd.lastSdmmcErrorMs(),
             lastSdErr == ESP_ERR_TIMEOUT ? "  -- 0x107=ESP_ERR_TIMEOUT (the SD2 watch item)" : "");
  } else {
    out.put("    last sdmmc error: none since boot\n");
  }
  out.outf("    trim sd-park arm=%s (default=%s)%s  parked=%d\n",
           standbytrim::active(standbytrim::Trim::SdPark) ? "ON" : "OFF",
           standbytrim::buildDefault(standbytrim::Trim::SdPark) ? "ON" : "OFF",
           standbytrim::overrideValue(standbytrim::Trim::SdPark) < 0 ? " [build default]" : " [runtime override]",
           standbytrim::parked() ? 1 : 0);
  const auto verdictText = [](const parksink::Verdict v) {
    return v == parksink::Verdict::Never ? "never-run"
           : v == parksink::Verdict::Ok  ? "ok"
                                          : "FAILED";
  };
  out.outf("    pads   last release=%s last restore=%s currently=%s (CNNT ded_sel, E4f)\n",
           verdictText(parksink::lastSdRelease()), verdictText(parksink::lastSdRestore()),
           parksink::sdPadsReleased() ? "RELEASED (held parked)" : "claimed");
  return 0;
}

int cmdLight(int argc, char** argv, Text& out) {
  (void)argc;
  (void)argv;
  const auto& fl = BoardConfig::ACTIVE.frontlight;
  out.outf("light: present=%d on=%d brightness=%u%% warmth=%u%%\n", Frontlight.present() ? 1 : 0,
           Frontlight.isOn() ? 1 : 0, static_cast<unsigned>(Frontlight.brightness()),
           static_cast<unsigned>(Frontlight.warmth()));
  out.outf("       pads=%s (E4d park)\n",
           Frontlight.padsParked() ? "PARKED: LOW + LEDC detached (no duty is being driven)"
                                   : "re-applied: LEDC attached, duty driven from brightness/warmth");
#if defined(ARDUINO_ARCH_ESP32) && ESP_ARDUINO_VERSION_MAJOR >= 3
  if (fl.gpio >= 0 && !Frontlight.padsParked()) {
    out.outf("       duty cool=%u warm=%u (raw ledcRead, %u-bit)\n", static_cast<unsigned>(ledcRead(fl.gpio)),
             static_cast<unsigned>(ledcRead(fl.gpioWarm)), static_cast<unsigned>(fl.pwmResolutionBits));
  } else if (fl.gpio >= 0) {
    out.put("       duty cool=detached warm=detached (the channels are not attached while parked)\n");
  } else {
    out.put("       this board profile has no LEDC frontlight pins\n");
  }
#else
  out.put("       no LEDC API on this core, so no duty can be read\n");
#endif
  return 0;
}

int cmdRadio(int argc, char** argv, Text& out) {
  (void)argc;
  (void)argv;
  const wifi_mode_t mode = WiFi.getMode();
  const char* modeName = mode == WIFI_OFF ? "OFF" : mode == WIFI_STA ? "STA" : mode == WIFI_AP ? "AP" : "other";
  out.outf("radio: wifi mode=%d (%s) connected=%d ssid='%s'\n", static_cast<int>(mode), modeName,
           WiFi.isConnected() ? 1 : 0, WiFi.SSID().c_str());
  const auto verdictText = [](const parksink::Verdict v) {
    return v == parksink::Verdict::Never ? "never-run (nothing was initialised)"
           : v == parksink::Verdict::Ok  ? "ok"
                                          : "FAILED";
  };
  out.outf("       esp_wifi_deinit since the last park: %s | last attempt: %s\n",
           parksink::radioDeinited() ? "APPLIED (stack is down until the next resume re-inits it)"
                                     : "not applied (stack inited or already re-inited)",
           verdictText(parksink::lastRadioCut()));
  out.put("       bt: NO CONTROLLER IN THIS IMAGE (CONFIG_BT_ENABLED unset, FREEINK_CAP_BLE_HID_HOST=0);\n"
          "           the P1.4 BT/PA rail is the PA trim's, and `bt=0` in the park lines is correct\n");
  return 0;
}

// ---------------------------------------------------------------------------
// JPEG1 `jpeg` — the hardware JPEG decoder, drivable on the device.
//
//   jpeg                    status: arm, first-use verdict, counters, budget, PSRAM
//   jpeg on | off           arm / disarm the hardware path (RAM only)
//   jpeg gate               re-arm the first-use comparison (clears the verdict)
//   jpeg compare <path>     decode one file BOTH ways and print the per-pixel verdict
//
// This is the only way to answer "are the covers still right on this unit?"
// without a host: the host cannot run the codec, and the first-use gate only
// grades the first eligible image of a boot. `compare` forces the comparison on
// any file at any time, including files the selector refuses (then it says why and
// decodes nothing twice).
//
// The comparison is on DECODED LUMINANCE, not on BMP bytes — see
// JpegToBmpConverter::jpegCompareHwVsSw for why.
// ---------------------------------------------------------------------------
int cmdJpeg(int argc, char** argv, Text& out) {
  const char* sub = (argc >= 2 && argv[1] != nullptr) ? argv[1] : "status";

  if (strcmp(sub, "on") == 0 || strcmp(sub, "off") == 0) {
    const bool on = (strcmp(sub, "on") == 0);
    jpeghw::setArmed(on);
    out.outf("jpeg: hardware path %s (RAM only — a reboot restores the compiled default)\n",
             on ? "ARMED" : "DISARMED");
    return 0;
  }

  if (strcmp(sub, "gate") == 0) {
    jpeghw::resetGate();
    out.put("jpeg: first-use comparison re-armed — the next hardware-eligible image is decoded both ways\n"
            "      and the picture itself comes from the software decoder; `jpeg` then shows the verdict\n");
    return 0;
  }

  if (strcmp(sub, "compare") == 0) {
    if (argc < 3 || argv[2] == nullptr || argv[2][0] == '\0') {
      out.put("usage: jpeg compare <path-on-card>\n");
      return -EINVAL;
    }
    const char* path = argv[2];
    HalFile f;
    if (!Storage.openFileForRead("SHELL", path, f)) {
      out.outf("jpeg compare: cannot open %s (no card, or the volume is handed to USB)\n", path);
      return -EIO;
    }
    const size_t bytes = f.size();
    JpegLumaDiff diff;
    JpegProbe probe;
    JpegHwReason why = JpegHwReason::NotProbed;
    int fw = 0;
    int fh = 0;
    JpegHwStatus hwStatus = JpegHwStatus::BadStream;
    const bool ok = JpegToBmpConverter::jpegCompareHwVsSw(f, &diff, &probe, &why, &fw, &fh, &hwStatus);
    f.close();

    out.outf("jpeg compare %s (%u B)\n", path, (unsigned)bytes);
    if (!probe.headerOk) {
      out.put("  header: unreadable — no SOF marker; nothing decoded\n  verdict: SKIPPED (header unreadable)\n");
      return 0;
    }
    out.outf("  header: %dx%d, %d component(s), %s, sampling %dx%d/%dx%d%s\n", probe.width, probe.height,
             probe.components, jpegCodingName(probe.coding), probe.sampH0, probe.sampV0, probe.sampH1,
             probe.sampV1, probe.restartInterval ? ", restart intervals" : "");
    if (!ok) {
      out.outf("  verdict: SKIPPED — %s (software path, nothing decoded twice)\n", jpegHwReasonName(why));
      if (why == JpegHwReason::DecodeFailed) out.outf("  driver: %s\n", jpegHwStatusName(hwStatus));
      return 0;
    }

    const double ident = 100.0 * static_cast<double>(diff.pixels - diff.differing) / static_cast<double>(diff.pixels);
    out.outf("  compared: %dx%d luma (hardware raster stride %u px, %d B/px)\n", fw, fh, jpegHwStride(probe),
             jpegHwLumaBytesPerPixel(probe));
    out.outf("  pixels %llu: identical %.1f%%, differing %.1f%%, mean|d| %.2f, max|d| %u\n",
             (unsigned long long)diff.pixels, ident, 100.0 - ident, diff.meanAbs(), diff.maxAbs);
    out.outf("  |d|>=3: %llu   |d|>=5: %llu\n", (unsigned long long)diff.over2, (unsigned long long)diff.over4);
    out.outf("  verdict: %s\n", diff.matches() ? "MATCH (IDCT rounding only — the gate accepts this)"
                                              : "MISMATCH — the hardware picture is NOT the software picture");
    if (!diff.matches()) {
      out.put("  action: leave the hardware path off (`jpeg off`); the picture on screen is the software one,\n"
              "          because the first-use gate refuses to paint from a decoder that disagrees\n");
    }
    return 0;
  }

  // ---- status (default) ----------------------------------------------------
  const jpeghw::Counters c = jpeghw::counters();
  const JpegHwLimits lim;
  out.outf("jpeg: hardware path %s (RAM only; compiled default ON)%s\n", c.armed ? "ARMED" : "DISARMED",
           c.armed ? "" : " — every image uses JPEGDEC");
  if (!c.gateDone) {
    out.put("gate: PENDING — the next hardware-eligible image is decoded both ways; the picture comes from\n"
            "      the software decoder and the verdict lands in the log + here\n");
  } else if (c.gatePassed) {
    out.outf("gate: PASSED on %u px (max|d| %u, mean %.2f) — hardware in use this boot\n", c.gatePixels, c.gateMaxAbs,
             c.gateMeanX100 / 100.0);
  } else {
    out.outf("gate: FAILED on %u px (max|d| %u) — hardware switched off for this session\n", c.gatePixels,
             c.gateMaxAbs);
  }
  out.outf("budget: %u B stream+raster, %u B headroom, largest free PSRAM %u B\n", (unsigned)lim.maxHwBytes,
           (unsigned)lim.psramHeadroom, (unsigned)jpeghwLargestFreePsram());
  out.outf("files: hardware %u, software %u\n", c.hwFiles, c.swFiles);
  int shown = 0;
  for (int i = 0; i < jpeghw::kReasonCount; i++) {
    if (c.perReason[i] == 0) continue;
    out.outf("  sw %ux %s\n", c.perReason[i], jpegHwReasonName(static_cast<JpegHwReason>(i)));
    if (++shown >= 5) break;
  }
  out.put("hint: `jpeg compare <path>` decodes one file both ways; `jpeg off` reverts to JPEGDEC only\n");
  return 0;
}

// ---------------------------------------------------------------------------
// Command table. `risk` is printed by `help` — a command that can stop the
// world has to say so before the operator types it.
// ---------------------------------------------------------------------------
struct CmdDef {
  const char* name;
  const char* args;
  const char* help;
  Kind kind;      // Kind::None = answered locally on the shell task
  bool slow;      // 30 s mailbox timeout instead of 2 s
  int (*local)(int argc, char** argv, Text& out);  // used when kind == None
};

const CmdDef kCmds[] = {
    {"help", "", "list commands and their risk", Kind::None, false, nullptr},
    {"ver", "", "version, uptime, heap/PSRAM, last panel line (safe)", Kind::None, false, cmdVer},
    // IMU2 `tilt` -- the flick gesture's instrument. Shipping, not behind a bench
    // switch: "is it even seeing me" is a question about the image the operator runs.
    {"tilt", "[cal | clear]", "flick page turn: sensor present/standby/capturing, the raw sample in g and the content-frame lateral component, the ACTIVE trigger/start thresholds and whether they came from a calibration, the stored calibration, and fires this boot (READ-ONLY); `tilt cal` adds the mechanism; `tilt clear` erases the stored calibration and restores the compiled defaults", Kind::None, false, cmdTilt},
    // r97 `batt` — SHIPPING in every image, NOT behind a bench switch. The bug it
    // diagnoses (no charging bolt at 87 % with ~1 A measured) happens on the image
    // the operator actually runs, so a diagnostic that only exists in a bench build
    // would be useless for it. Operator agreed, 1 Nov 2026.
    {"batt", "drift | drift=<ppm> | mark", "battery: soc%, signed mA (+=charge, - =discharge) with the convention named, derived charge flag + its source, raw gauge/charger regs, read age, the park-mark coulomb delta (charge vs device time, host-clock caveat spelled out); `batt drift=<ppm>` stores the RTC drift factor applied to the parked span; `batt mark` takes an IDLE mark (RAM only) so the next `batt delta` brackets an AWAKE interval", Kind::None, false, cmdBatt},
    {"trim", "status | pa on|off|cut|restore | cam on|off|restore | sd on|off", "standby A/B arms at runtime: set the P1.4 rail cut, the P0.5 rail cut or the SD park for the next standby, or drive either rail now (P0.5 re-assert only -- the immediate `cam cut` is RETIRED); prints the compiled defaults; the P0.5 cut is the standby arm (`trim cam on` -> park, which unmounts first)", Kind::None, false, cmdTrim},
    // TTS1 — the speaker path. OFF by default and gated by a persisted setting:
    // `audio on` refuses until `audio enable`. Everything it does is bounded and
    // runs on this shell task, never in the boot path.
    {"audio", "enable|disable | on|off | tone [hz] [ms]", "BH1098G audio OUTPUT path: state (gate, P1.4 rail, P0.4 amp, I2S TX, mode-1 handshake, counters, last reason); `audio enable|disable` persists the gate (disable powers the path down); `audio on|off` raises/lowers it now (700 ms rail settle + AT handshake, <=1000 ms ack, one attempt, faults the session on failure); `audio tone [hz] [ms]` plays a 1 kHz / 300 ms test tone (<=1 s) (DRIVES THE RAIL + THE SPEAKER)", Kind::None, false, cmdAudio},
    // AUDIO1 — the UI feedback clicks. Same rail, second switch, OFF by default.
    {"uisound", "on|off | test [name] | policy | calls | log [on|off]", "short UI clicks (page turn, menu move, select, back, disallowed) on the `audio` path: prints both switches (audioOutEnabled + uiSoundsEnabled), the worker state, played/queued/dropped-busy/gated counters and the measured cost of the last event-path call; `uisound on|off` persists the ui switch (both switches must be ON for a click); `uisound test <name>` plays one wave now; `uisound policy` prints the event->sound map; `uisound calls` prints the per-call-site ledger (tag x event x started/queued/dropped/skipped) plus the last 32 calls; `uisound log on|off` traces every event to the log at most once per 200 ms", Kind::None, false, cmdUiSound},
    // JPEG1 — SHIPPING, not behind a bench switch: the first-use gate can switch the
    // hardware decoder off on the image the operator actually runs, and `compare` is
    // the only way to check a cover without a host machine.
    {"jpeg", "status | on | off | gate | compare <path>", "hardware JPEG decoder: arm state, the first-use comparison verdict, file counters by fallback reason, the PSRAM budget (READ-ONLY); `jpeg on|off` arms/disarms the hardware path (RAM only); `jpeg gate` re-arms the first-use comparison; `jpeg compare <path>` decodes one file with BOTH decoders and prints the per-pixel luma verdict (identical %, mean/max |d|, MATCH/MISMATCH) -- costs up to 4 MB of PSRAM while it runs", Kind::None, false, cmdJpeg},
    // E5 (HARDWARE_POWER_PLAN §5.4) — SHIPPING, not behind a bench switch: the
    // whole point is that the shipped image can recover touch after a failed probe.
    // `slow` because three attempts spend ~1.6 s in rail settles + reset delays.
    {"touch", "status | recover", "FT6336U capability: current latch/attempts, or run the probe ladder now (rail P5 cycle + TP_RST pulse after a failed first probe); RAM-only, nothing persisted (drives the expander)", Kind::Touch, true, nullptr},
    // r163 E6: the need table. Bridge rather than bench: a leak is a shipped-image defect.
    {"needs", "", "the declarative power-need table: every need, hard/soft, count, how long it has been held, the leak budget, acquisitions vs releases, whether automatic standby is blocked, and the charge policy -- the answer to \"what is holding this device awake\" (READ-ONLY)",
     Kind::None, false, cmdNeeds},
    {"logtail", "[n] [offset] [--old]", "last n lines of /.crosspoint/debug.log read from the end; optional offset = lines back from the end to start at; `--old` reads the ROLLED generation /.crosspoint/debug.log.old (same semantics) (safe)", Kind::None, false,
     cmdLogtail},
    // r107 P5: rotation already existed (128 KB cap, debug.log -> debug.log.old); this is the
    // read/report path for it, so "did it roll?" is one command instead of an inference from a
    // file that looks smaller than it was.
    {"loginfo", "", "BOTH card-log generations: existence + size of debug.log and debug.log.old, the per-generation cap, and whether a roll has happened (READ-ONLY)", Kind::None,
     false, cmdLoginfo},
    // r106: on-device grep. The card log is streamed and only matches are printed, with line
    // numbers, so a park/drive line is reachable in ONE command instead of a clipped logtail.
    {"loggrep", "<pattern> [before 0..3] [--old]", "search the card log on-device: matching lines with line numbers + total match count (READ-ONLY, 40-match/1 KB cap, says when it truncated); `--old` greps debug.log.old and the header names the file it read",
     Kind::None, false, cmdLoggrep},
    // batch 1A: `logs` reads the RAM ring that logSdFlush() drains. logtail can only show what a
    // flush already wrote, so a boot-time line was unreachable until now.
    {"logs", "[n] | clear", "`logs [n]` = last n lines of the RAM log ring, NOT yet on the card (READ-ONLY, newest last); `logs clear` DELETES BOTH card-log generations (debug.log + debug.log.old), reports bytes removed per file and the free space after -- the only sanctioned log deletion, never implicit", Kind::None,
     false, cmdLogs},
    // ---- r106 read-only diagnostics (no bench guard: the shipping shell must be able to run
    // the whole r106 test list without another flash round) -------------------------------
    {"ioexp", "", "TCA9555 input+OUTPUT port bytes (0x02/0x03), the latches, the B1 fallback shadow and the named bits (READ-ONLY, four short I2C reads)",
     Kind::None, false, cmdIoexp},
    {"clock", "", "esp_timer/uptime vs the RTC counter vs the PCF8563 calendar (+VL flag) and the stored ppm (READ-ONLY)", Kind::None,
     false, cmdClock},
    {"sd", "[init | adopt | umount confirm | mount confirm]", "`sd` = mount state, capacity/used, last mount result, last sdmmc error, SD trim arm and the last CNNT pad release/restore (READ-ONLY); `sd init` = create the volume layout (/.crosspoint/.volume) on an EXPLICITLY chosen blank card -- the boot refuses to initialise an unmarked volume; `sd adopt` = write ONLY the marker on a card that already holds your content but no settings.json (a restored backup); `sd umount confirm` = unmount + release the CNNT pads WITHOUT entering park (drains the log ring first; POWER-CYCLES the card, so it needs the token); `sd mount confirm` = re-claim the pads + Storage.begin() with the one retry, then report the mount time", Kind::None,
     false, cmdSd},
    {"light", "", "frontlight brightness/warmth/LEDC duty and whether the park pads are released or re-applied (READ-ONLY)", Kind::None,
     false, cmdLight},
    {"radio", "", "Wi-Fi mode/connected/SSID, whether esp_wifi_deinit is applied since the last park, and that this image has no BT controller (READ-ONLY)",
     Kind::None, false, cmdRadio},
    // r100.3 fbrow: read the canvas, not the glass. Answers "is the ink actually
    // there and solid in the buffer" for a row range, so a non-solid line can be
    // blamed on the draw or on the drive with evidence instead of a hypothesis.
    {"fbrow", "<y0> [y1] [map]", "canvas ink per logical row: SOLID/PARTIAL/DITHER/GREY/BLANK + counts, runs, ASCII map (READ-ONLY)",
     Kind::None, false, cmdFbrow},
    {"loglevel", "<none|err|warn|info|debug>", "runtime log verbosity; 'none' quiets the spam (safe)", Kind::None,
     false, cmdLoglevel},
    {"panel", "", "panel state: last resolved mode, TPS verdict, PWR_GOOD, drive ms (safe, read-only)",
     Kind::Panel, false, nullptr},
    {"pmic", "", "TPS65185 registers, hex + names (READ-ONLY; no register is written)", Kind::Pmic, false, nullptr},
    {"refresh", "[full|half|fast]", "repaint through RefreshPolicy, same path as a gesture (drives the panel)",
     Kind::Refresh, true, nullptr},
    {"shot", "[path]", "write the CURRENT framebuffer to a 1-bpp BMP on SD (writes ~48 KB)", Kind::Shot, true,
     nullptr},
    {"activity", "", "activity stack names, top first (safe)", Kind::Activity, false, nullptr},
    {"spine", "", "spine index, page, progress of the open book (safe)", Kind::Spine, false, nullptr},
    {"reboot", "", "restart after a 1 s warning; the response goes out first (STOPS THE WORLD)", Kind::Reboot,
     false, nullptr},
    // ---- r96 bench commands (CP_CPU_STATS / CP_LP_BENCH / CP_PPA_BENCH) ----
    // r97: each entry exists only when its switch is on, so the SHIPPING image has no
    // bench command names -- not even a "not built" stub to explain them.
#if CP_CPU_STATS
    {"cpu", "", "per-task run time, idle% per core, actual MHz (safe, read-only)", Kind::None, false, cmdCpu},
    {"pm", "", "power state, current MHz, PM locks, light-sleep time (safe, read-only)", Kind::None, false, cmdPm},
    {"bench", "render", "compose vs drive split of the LAST page render (safe, no drive)", Kind::None, false,
     cmdBench},
#endif
    // BENCH1: not behind a bench switch on purpose -- the harness is gated by the EXISTING debug
    // switch at runtime (it refuses when Debug mode is off), the same gate the Debug row obeys.
    {"benchdrives", "[runs]", "BENCH1: script the transition map (default 3x) and print a per-step drive ledger (drives the panel)",
     Kind::None, false, cmdBenchDrives},
#if CP_EXP
    // EXP1 (r101.21): the runtime experiment arms. One family, one entry point:
    //   exp status | clear | row9 on|off | flpanel on|off | vid [n] | simd [n]
    //       | park [usb=0|1 front=0|1 radio=0|1 sd=0|1]
    // `park` is Build A step 2 (E4c-E4f): the four standby-park current sinks, each armed
    // by default and individually forced off for one session so one image can attribute
    // them in separate coulomb windows. Every arm is RAM-only and defaults to the behaviour
    // r101.20 shipped with, so this command cannot change what the image does until a name
    // and `on`/`off` are typed at it. `slow` because `exp vid 60` holds the main task for
    // ~6 s and `exp simd` for the length of its loop.
    {"exp", "status | clear | row9 on|off | vid [n] | simd [n] | park | park [usb=0|1 ...]",
     "EXP1: experiment arms + Build A park sinks (all RAM-only, all at their build default); `status` prints one line per arm and per sink; `park` alone prints ONLY the four park sink lines (usb/front/radio/sd, arms + defaults -- short enough for the body cap); `park usb=0` forces one sink off for this session; vid drives n short-DU frames, simd times s8 dotprod vs its ANSI twin",
     Kind::Exp, true, nullptr},
#endif
#if CP_LP_BENCH
    {"lpbench", "[n]", "LP core iteration rate + completion time, n runs (starts the LP core)", Kind::None, true,
     cmdLpBench},
    {"lpsleep", "[sec]", "REFUSED in this build (CP_LPSLEEP_ALLOW_HP_SLEEP=0); use lpbench", Kind::LpSleep, true,
     nullptr},
    {"lpsentry", "[sec]", "LP core wake-source recorder: which line wakes it, for sec seconds (safe, no HP sleep)",
     Kind::None, true, cmdLpSentry},
#endif
#if CP_PPA_BENCH
    {"ppabench", "[op] [w] [h] [src]", "PPA full-frame us/MB/s vs a software reference (characterises the PPA)",
     Kind::PpaBench, true, nullptr},
    // PPA1: the decision AND the measurement, on the renderer's own canvas.
    // `plan` is pure arithmetic; `cmp`/`time` run the CPU transform against the
    // widen->PPA->narrow sandwich over the live buffer and report byte-exactness
    // plus the per-stage microseconds. `slow` because a 2.5 MB PSRAM allocation
    // and a full-frame SRM call are not a 2 s operation on a cold heap.
    {"ppaframe", "[plan|cmp|time] [copy|rot180|mirror] [n]",
     "PPA1: may the SRM engine serve the live canvas, and what would the widening sandwich cost? `plan` = the "
     "pure decision + the GRAY8 buffer a packed canvas would need; `cmp` = run the CPU transform AND "
     "widen->PPA->narrow over the same canvas, report byte-identical/differing bytes and each stage's us; "
     "`time` = the same, repeated (default 3), reporting the min of each stage. Costs up to ~2.5 MB PSRAM; "
     "drives nothing",
     Kind::PpaFrame, true, nullptr},
#endif
};
constexpr int kCmdCount = static_cast<int>(sizeof(kCmds) / sizeof(kCmds[0]));

const CmdDef* findCmd(const char* name) {
  for (int i = 0; i < kCmdCount; ++i) {
    if (strcmp(kCmds[i].name, name) == 0) return &kCmds[i];
  }
  return nullptr;
}

int cmdHelp(Text& out) {
  out.put("commands (all responses are framed <<<BEGIN id cmd=name ... <<<END id rc= ms=>):\n");
  for (int i = 0; i < kCmdCount; ++i) {
    out.outf("  %-9s %-22s %s\n", kCmds[i].name, kCmds[i].args, kCmds[i].help);
  }
  out.put("ids: append '--id <x>' (or '-i <x>'); the host driver uses this to match a reply.\n");
  out.put("note: this console is shared with the log stream; anything outside the markers is telemetry.\n");
  out.put("      a 'refresh'/'shot' runs on the main task via the mailbox, so it obeys RenderLock and\n");
  out.put("      RefreshPolicy exactly like a gesture does.\n");
  return 0;
}

// The single esp_console entry point. esp_console has already split the line
// into argv; argv[0] is the command name (linenoise eats the prompt).
int shellDispatch(int argc, char** argv) {
  if (argc == 0 || argv == nullptr || argv[0] == nullptr) return 0;

  uint32_t id = 0;
  bool haveId = false;
  // Pull the optional id out of argv (accepted anywhere).
  char* rest[8];
  int restN = 0;
  for (int i = 0; i < argc; ++i) {
    const char* a = argv[i];
    if ((strcmp(a, "--id") == 0 || strcmp(a, "-i") == 0) && i + 1 < argc) {
      id = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 0));
      haveId = true;
      continue;
    }
    if (strncmp(a, "--id=", 5) == 0) {
      id = static_cast<uint32_t>(strtoul(a + 5, nullptr, 0));
      haveId = true;
      continue;
    }
    if (restN < static_cast<int>(sizeof(rest) / sizeof(rest[0]))) rest[restN++] = const_cast<char*>(a);
  }
  if (restN == 0) return 0;
  if (!haveId) {
    static uint32_t s_nextId = 1;
    id = s_nextId++;
  }

  const CmdDef* def = findCmd(rest[0]);
  if (def == nullptr) {
    // Unknown command: still framed, so the host is not left waiting for an END.
    Text body;
    body.outf("unknown command '%s' — try 'help'\n", rest[0]);
    Frame f{id, rest[0], esp_timer_get_time()};
    frameBegin(f);
    frameEnd(f, -2, body);
    return 0;
  }

  Frame f{id, def->name, esp_timer_get_time()};
  frameBegin(f);
  Text body;
  int rc = 0;

  if (def->kind == Kind::None) {
    if (strcmp(def->name, "help") == 0) {
      rc = cmdHelp(body);
    } else if (def->local != nullptr) {
      rc = def->local(restN, rest, body);
    }
  } else {
    // r96: join every argument after the command name, so a mailbox command that
    // takes several (`ppabench rot90 1216 684 fb`) arrives intact. The slot's arg
    // buffer is 112 B, which is more than max_cmdline_length allows anyway.
    char joined[112];
    joined[0] = '\0';
    for (int i = 1; i < restN; ++i) {
      const size_t used = strlen(joined);
      if (used + 1 >= sizeof(joined)) break;
      snprintf(joined + used, sizeof(joined) - used, "%s%s", i > 1 ? " " : "", rest[i]);
    }
    const char* arg = (restN >= 2) ? joined : nullptr;
    rc = post(def->kind, arg, def->slow ? kSlowTimeoutMs : kFastTimeoutMs, body, &rc);
    if (rc == -ETIMEDOUT) {
      body.outf("%s: no answer from the main loop within %u ms (UI busy in a refresh?) — EBUSY\n", def->name,
                static_cast<unsigned>(def->slow ? kSlowTimeoutMs : kFastTimeoutMs));
    } else if (rc == -EBUSY) {
      body.outf("%s: mailbox full — EBUSY\n", def->name);
    }
  }
  frameEnd(f, rc, body);
  return 0;
}

}  // namespace

// ===========================================================================
// Public API
// ===========================================================================
namespace usbshell {

bool available() { return true; }

bool running() { return s_running; }

// ----- r91 lifecycle -------------------------------------------------------
bool sessionEnabled() {
  sessionInit();
  return s_session;
}

bool keepsDeviceAwake() {
  sessionInit();
  return s_session && s_running;
}

bool enableSession() {
  sessionInit();
  s_session = true;
  noteSession(true);
  // LOG2: append (never truncate) a marker to the on-card log at the exact moment the shell comes
  // back, so an append that did not append is visible afterwards as a missing line + a shrunk size.
  logSdSessionNote("USB debug shell session ENABLED");
  ESP_LOGI("shell", "session ENABLED (persisted=%d): holding the device awake (R1)",
           static_cast<int>(SETTINGS.usbDebugShell));
  return start();
}

void disableSession() {
  sessionInit();
  s_session = false;
  noteSession(false);
  logSdSessionNote("USB debug shell session DISABLED");
  LOG_INF("SHELL", "session DISABLED by the operator: power policy restored");
  stop();
}

void clearSession(const char* why) {
  sessionInit();
  if (!s_session && !s_running) return;
  s_session = false;
  noteSession(false);
  logSdSessionNote("USB debug shell session cleared (sleep/power-off)");
  // INFO, not DBG: this is the line that explains why a shell the operator left on is
  // no longer there after a sleep or a power-off.
  LOG_INF("SHELL", "session cleared (%s): REPL %s; the console reverts to log-only", why ? why : "?",
          s_running ? "stopping" : "already stopped");
  if (s_running) stop();
}

void deactivateAndPersist(const char* why) {
  sessionInit();
  const bool wasPersisted = SETTINGS.usbDebugShell != 0;
  SETTINGS.usbDebugShell = 0;
  // Only touch the card when the value actually changed, and only from the main task
  // (callers are loop()/onEnter(), never the shell task).
  if (wasPersisted) SETTINGS.saveToFile();
  LOG_INF("SHELL", "shell deactivated and toggled OFF (%s)", why ? why : "?");
  clearSession(why);
}

bool start() {
  if (s_running) return true;
  mboxInit();

  esp_console_repl_config_t replConfig = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  // Short prompt: it shares the pipe with the log stream and lands mid-line.
  replConfig.prompt = "cp>";
  replConfig.max_cmdline_length = 192;
  replConfig.max_cmdline_args = 8;
  replConfig.max_history_len = 8;
  replConfig.task_stack_size = 5120;
  replConfig.task_priority = 3;
  replConfig.task_core_id = 0;

  esp_console_dev_usb_serial_jtag_config_t devConfig = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

  // new_repl_* calls esp_console_init() internally, so the command table has to
  // be registered after it and before the task starts consuming input.
  esp_err_t err = esp_console_new_repl_usb_serial_jtag(&devConfig, &replConfig, &s_repl);
  if (err != ESP_OK || s_repl == nullptr) {
    ESP_LOGE("shell", "new_repl failed: %s", esp_err_to_name(err));
    s_repl = nullptr;
    return false;
  }

  for (int i = 0; i < kCmdCount; ++i) {
    esp_console_cmd_t cmd = {};
    cmd.command = kCmds[i].name;
    cmd.help = kCmds[i].help;
    cmd.hint = kCmds[i].args;
    cmd.func = &shellDispatch;
    const esp_err_t r = esp_console_cmd_register(&cmd);
    if (r != ESP_OK) ESP_LOGW("shell", "register %s: %s", kCmds[i].name, esp_err_to_name(r));
  }

  err = esp_console_start_repl(s_repl);
  if (err != ESP_OK) {
    ESP_LOGE("shell", "start_repl failed: %s", esp_err_to_name(err));
    esp_console_stop_repl(s_repl);
    s_repl = nullptr;
    return false;
  }
  s_running = true;
  // Deliberately INFO, not DBG: CONFIG_LOG_MAXIMUM_LEVEL=3 compiles ESP_LOGD out.
  ESP_LOGI("shell", "REPL up on the USB-Serial/JTAG console (shared with logs). Type 'help'.");
  printf("\ncp> shell ready — 'help' lists commands. Responses are framed <<<BEGIN/<<<END.\n");
  fflush(stdout);
  return true;
}

bool stop() {
  if (!s_running || s_repl == nullptr) {
    s_running = false;
    return true;
  }
  // esp_console_stop_repl(): sets DEINIT, signals the eventfd, waits for the REPL
  // task to return, frees linenoise history; the del() path then frees the
  // command table and puts the USB-Serial/JTAG reader back to non-blocking
  // writes, so plain log output keeps working — that is the whole point of
  // using it instead of killing the task.
  const esp_err_t err = esp_console_stop_repl(s_repl);
  s_repl = nullptr;
  s_running = false;
  // Any request still queued dies with the console: abandon them so a stale
  // pointer cannot be answered into freed memory.
  if (s_mboxMutex) {
    xSemaphoreTake(s_mboxMutex, portMAX_DELAY);
    for (int i = 0; i < kSlots; ++i) {
      s_slots[i].state = SlotState::Idle;
      s_slots[i].caller = nullptr;
      s_slots[i].abandoned = true;
    }
    s_n = 0;
    s_head = s_tail = 0;
    xSemaphoreGive(s_mboxMutex);
  }
  if (err != ESP_OK) {
    ESP_LOGW("shell", "stop_repl: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI("shell", "REPL stopped; console back to plain log output");
  return true;
}

void drain() {
  if (s_mboxMutex == nullptr) return;
  Slot* slot = nullptr;
  xSemaphoreTake(s_mboxMutex, portMAX_DELAY);
  if (s_n > 0) {
    slot = s_ring[s_head];
    s_head = (s_head + 1) % kSlots;
    --s_n;
    slot->state = SlotState::InFlight;
  }
  xSemaphoreGive(s_mboxMutex);
  if (slot == nullptr) return;

  if (slot->abandoned) {
    xSemaphoreTake(s_mboxMutex, portMAX_DELAY);
    slot->state = SlotState::Idle;
    xSemaphoreGive(s_mboxMutex);
    return;
  }

  const int rc = execute(*slot);

  xSemaphoreTake(s_mboxMutex, portMAX_DELAY);
  slot->rc = rc;
  slot->state = SlotState::Done;
  TaskHandle_t caller = slot->caller;
  xSemaphoreGive(s_mboxMutex);
  if (caller != nullptr) xTaskNotifyGive(caller);
}

void notePanelJournal(const char* line) {
  if (line == nullptr) return;
  strncpy(s_lastPanelLine, line, sizeof(s_lastPanelLine) - 1);
  s_lastPanelLine[sizeof(s_lastPanelLine) - 1] = '\0';
}

// r96.1 D3: stamps the DURATION (the paint log's number, unchanged) and the END TIME.
// The two must be set together: a duration without an end time is what let an age
// check silently read 549 ms of waveform as "549 ms ago" forever.
void notePanelDriveMs(unsigned long ms) {
  s_lastDriveMs = ms;
  s_lastDriveAtMs = millis() ? millis() : 1u;  // 0 means "never", so never store it
}

const char* lastPanelJournal() { return s_lastPanelLine; }

unsigned long lastPanelDriveMs() { return s_lastDriveMs; }

unsigned long lastPanelDriveAgeMs() {
  const unsigned long at = s_lastDriveAtMs;
  if (at == 0) return ULONG_MAX;  // nothing driven this boot: age is unknowable, not zero
  // Unsigned subtraction is wrap-correct across the millis() rollover (~49.7 days).
  return millis() - at;
}

void installUiOps(const UiOps& ops) { s_ops = ops; }

}  // namespace usbshell

#else  // !CP_USB_SHELL

// Compiled-out build: every entry point exists, none of them do anything, and
// no esp_console symbol is referenced anywhere in the image.
namespace usbshell {
bool available() { return false; }
bool running() { return false; }
bool sessionEnabled() { return false; }
bool keepsDeviceAwake() { return false; }
bool enableSession() { return false; }
void disableSession() {}
void clearSession(const char*) {}
void deactivateAndPersist(const char*) {}
bool start() { return false; }
bool stop() { return true; }
void drain() {}
void notePanelJournal(const char*) {}
void notePanelDriveMs(unsigned long) {}
const char* lastPanelJournal() { return ""; }
unsigned long lastPanelDriveMs() { return 0; }
// r96.1 D3: no drive was ever noted in a build without the shell, so the age is
// unknowable (ULONG_MAX), not zero -- zero would read as "a drive just finished".
unsigned long lastPanelDriveAgeMs() { return ULONG_MAX; }
void installUiOps(const UiOps&) {}
}  // namespace usbshell

#endif  // CP_USB_SHELL
