#include "CpuStats.h"

#include "PlatformFeatures.h"

// Needed in BOTH configurations: the stubs below report "not built" with snprintf.
#include <cstdio>

#if CP_CPU_STATS

#include <Arduino.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <sdkconfig.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace cpustats {

namespace {

// ---------------------------------------------------------------------------
// Per-render recorder. Written only from the task that runs Activity::render()
// (ActivityManagerRender) and from GfxRenderer, which is called from inside
// that same render — so no lock is needed and none is taken. Reading it from
// the shell task is a torn read at worst, which is why `valid` and `seq` are
// written LAST in endRender().
// ---------------------------------------------------------------------------
struct Recorder {
  bool active = false;
  int64_t glyphUs = 0;
  int64_t imageUs = 0;
  int64_t driveUs = 0;
  int64_t pageLoadUs = 0;
  int64_t measureUs = 0;
  int64_t statusUs = 0;
  int64_t clipUs = 0;
  uint32_t glyphMisses = 0;
  int driveCalls = 0;
  uint8_t mode = 0;
};

Recorder s_rec;
RenderBreakdown s_last;

int64_t s_sleepUs = 0;
uint32_t s_sleepSlices = 0;

// Append helpers: every formatter below writes into a caller buffer, never the heap.
struct Out {
  char* buf;
  size_t cap;
  size_t len = 0;

  void put(const char* s) {
    if (s == nullptr || len + 1 >= cap) return;
    const size_t n = strnlen(s, cap - len - 1);
    memcpy(buf + len, s, n);
    len += n;
    buf[len] = '\0';
  }
  void f(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
    if (n > 0) len += (static_cast<size_t>(n) < cap - len) ? static_cast<size_t>(n) : cap - len - 1;
  }
};

// 12.3-friendly percentages: one decimal, computed from integers so a tiny
// denominator cannot produce NaN.
void pct(char* dst, size_t cap, const int64_t part, const int64_t whole) {
  if (whole <= 0) {
    snprintf(dst, cap, "  n/a");
    return;
  }
  const int64_t tenths = (part * 1000) / whole;
  snprintf(dst, cap, "%3lld.%lld%%", static_cast<long long>(tenths / 10), static_cast<long long>(tenths % 10));
}

// TaskStatus_t carries xCoreID only when configTASKLIST_INCLUDE_COREID is on,
// which this build sets (CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID=y). Kept as a
// helper so a configuration without it degrades to "core unknown" instead of
// failing to compile — the per-task numbers are still correct.
inline int taskCore(const TaskStatus_t& t) {
#if defined(configTASKLIST_INCLUDE_COREID)
  return static_cast<int>(t.xCoreID);
#else
  (void)t;
  return -1;
#endif
}

const char* modeName(const uint8_t m) {
  // Values are HalDisplay::RefreshMode; named here so the log line needs no header.
  switch (m) {
    case 0:
      return "FULL";
    case 1:
      return "HALF";
    case 2:
      return "FAST";
    case 3:
      return "GL16";
    default:
      return "?";
  }
}

}  // namespace

int64_t nowUs() { return esp_timer_get_time(); }

void beginRender() {
  s_rec.active = true;
  s_rec.glyphUs = 0;
  s_rec.imageUs = 0;
  s_rec.driveUs = 0;
  s_rec.pageLoadUs = 0;
  s_rec.measureUs = 0;
  s_rec.statusUs = 0;
  s_rec.clipUs = 0;
  s_rec.glyphMisses = 0;
  s_rec.driveCalls = 0;
  s_rec.mode = 0;
}

void endRender(const int64_t totalUs) {
  RenderBreakdown b;
  b.seq = s_last.seq + 1;
  b.totalUs = totalUs;
  b.driveUs = s_rec.driveUs;
  b.composeUs = totalUs - s_rec.driveUs;
  if (b.composeUs < 0) b.composeUs = 0;  // a drive outside the measured window
  b.glyphUs = s_rec.glyphUs;
  b.imageUs = s_rec.imageUs;
  b.pageLoadUs = s_rec.pageLoadUs;
  b.measureUs = s_rec.measureUs;
  b.statusUs = s_rec.statusUs;
  b.clipUs = s_rec.clipUs;
  b.glyphMisses = s_rec.glyphMisses;
  b.otherUs = b.composeUs - b.pageLoadUs - b.measureUs - b.statusUs - b.clipUs - b.glyphUs - b.imageUs;
  if (b.otherUs < 0) b.otherUs = 0;  // overlapping instrumentation: report 0, not a lie in the negative
  b.driveCalls = s_rec.driveCalls;
  b.driveMode = s_rec.mode;
  b.valid = true;
  s_rec.active = false;
  s_last = b;  // last write: a reader can only ever see a complete breakdown
}

void noteDriveUs(const int64_t us, const uint8_t mode) {
  if (!s_rec.active) return;  // a drive outside a render (shot, wake prime) is not page latency
  s_rec.driveUs += us;
  s_rec.driveCalls++;
  s_rec.mode = mode;
}

void noteGlyphUs(const int64_t us) {
  if (s_rec.active) s_rec.glyphUs += us;
}

void noteImageUs(const int64_t us) {
  if (s_rec.active) s_rec.imageUs += us;
}

// r97 Phase 2 hooks. Same rule as the r96 ones: only inside a measured render,
// and only ever called from the render task, so no lock is taken.
void notePageLoadUs(const int64_t us) {
  if (s_rec.active && us > 0) s_rec.pageLoadUs += us;
}

void noteMeasureUs(const int64_t us) {
  if (s_rec.active && us > 0) s_rec.measureUs += us;
}

void noteStatusUs(const int64_t us) {
  if (s_rec.active && us > 0) s_rec.statusUs += us;
}

void noteClipUs(const int64_t us) {
  if (s_rec.active && us > 0) s_rec.clipUs += us;
}

void noteGlyphMisses(const uint32_t count) {
  if (s_rec.active) s_rec.glyphMisses += count;
}

int64_t statusUsInRender() { return s_rec.active ? s_rec.statusUs : 0; }

void noteLightSleepUs(const int64_t us) {
  if (us <= 0) return;
  s_sleepUs += us;
  ++s_sleepSlices;
  LOG_INF("TEST", "lightsleep slice %lu: %lld us (total %lld us)", static_cast<unsigned long>(s_sleepSlices),
          static_cast<long long>(us), static_cast<long long>(s_sleepUs));
}

const RenderBreakdown& lastRender() { return s_last; }

SleepTotals lightSleep() {
  SleepTotals t;
  t.totalUs = s_sleepUs;
  t.slices = s_sleepSlices;
  return t;
}

// ---------------------------------------------------------------------------
// `bench render` — the COMPOSE-vs-DRIVE split of the last page render.
// ---------------------------------------------------------------------------
int formatRender(char* out, size_t cap) {
  Out o{out, cap};
  const RenderBreakdown& b = s_last;
  if (!b.valid) {
    o.put("render: no page render measured yet since boot\n");
    o.put("        turn a page (or repaint any screen), then run this again\n");
    return 0;
  }
  char c1[16], c2[16], c3[16], c4[16];
  pct(c1, sizeof(c1), b.totalUs - b.driveUs, b.totalUs);  // compose as a share of the whole paint
  pct(c2, sizeof(c2), b.driveUs, b.totalUs);
  pct(c3, sizeof(c3), b.glyphUs, b.composeUs);
  pct(c4, sizeof(c4), b.imageUs, b.composeUs);
  char c5[16];
  pct(c5, sizeof(c5), b.otherUs, b.composeUs);

  char c6[16], c7[16], c8[16], c9[16];
  pct(c6, sizeof(c6), b.pageLoadUs, b.composeUs);
  pct(c7, sizeof(c7), b.measureUs, b.composeUs);
  pct(c8, sizeof(c8), b.statusUs, b.composeUs);
  pct(c9, sizeof(c9), b.clipUs, b.composeUs);

  o.f("render #%lu mode=%s drives=%d\n", static_cast<unsigned long>(b.seq), modeName(b.driveMode), b.driveCalls);
  o.f("  total   %8.1f ms   (RenderLock -> render() returned)\n", static_cast<double>(b.totalUs) / 1000.0);
  o.f("  compose %8.1f ms   %s of total   (total - drive)\n", static_cast<double>(b.composeUs) / 1000.0, c1);
  o.f("  drive   %8.1f ms   %s of total   (GfxRenderer::displayBuffer)\n",
      static_cast<double>(b.driveUs) / 1000.0, c2);
  o.put("  compose split, by call boundary (shares are of compose):\n");
  o.f("  (a) page load/deserialise %7.1f ms  %s  Section::loadPage\n",
      static_cast<double>(b.pageLoadUs) / 1000.0, c6);
  o.f("  (b) font measure/prewarm  %7.1f ms  %s  scan, status-bar time excluded\n",
      static_cast<double>(b.measureUs) / 1000.0, c7);
  o.f("  (c) status-bar paint      %7.1f ms  %s  renderStatusBar()\n",
      static_cast<double>(b.statusUs) / 1000.0, c8);
  o.f("  (d) clipping highlights   %7.1f ms  %s  drawClippingHighlights()\n",
      static_cast<double>(b.clipUs) / 1000.0, c9);
  o.f("  (e) glyph blit            %7.1f ms  %s  misses=%lu\n", static_cast<double>(b.glyphUs) / 1000.0, c3,
      static_cast<unsigned long>(b.glyphMisses));
  o.f("  (e) image blit            %7.1f ms  %s  bitmap/image blit\n", static_cast<double>(b.imageUs) / 1000.0, c4);
  o.f("  (f) other                 %7.1f ms  %s  compose - a..e\n", static_cast<double>(b.otherUs) / 1000.0, c5);
  o.put("read: line-break/justify is NOT in compose -- it runs at chapter build (extractLine); (b) is\n");
  o.put("      the measure/prewarm scan, the only layout-shaped work left here. drive-bound if compose\n");
  o.put("      is a small share of total; pipelining pays only if (a)+(b)+(e) are real work.\n");
  return 0;
}

// ---------------------------------------------------------------------------
// `cpu` — per-task run time, idle% per core, actual MHz.
//
// Source: uxTaskGetSystemState() (needs CONFIG_FREERTOS_USE_TRACE_FACILITY) with
// each task's ulRunTimeCounter (needs CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS).
// Percentages are shares of the SUM of all task counters, i.e. of both cores'
// scheduled time — so a fully busy single core reads 50 %, and idle0+idle1 on an
// idle machine read ~100 % together.
//
// RESOLUTION AND WRAP: the run-time-stats clock is the 1 MHz esp_timer, a 32-bit
// counter, so it wraps every ~4290 s of uptime. Percentages stay correct across
// the wrap only because every task counter wraps at the same instant; the
// absolute "clock" figure printed below is the wrap-prone one.
// ---------------------------------------------------------------------------
int formatCpu(char* out, size_t cap) {
  Out o{out, cap};
#if !defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) || !defined(CONFIG_FREERTOS_USE_TRACE_FACILITY)
  o.put("cpu: not compiled in — CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS /\n");
  o.put("     CONFIG_FREERTOS_USE_TRACE_FACILITY are not set in this build's sdkconfig.\n");
  o.put("     They are in the tracked idf/sdkconfig.defaults (r96); regenerate sdkconfig to pick them up.\n");
  return 0;
#else
  const UBaseType_t n = uxTaskGetNumberOfTasks();
  if (n == 0 || n > 64) {
    o.f("cpu: implausible task count %u\n", static_cast<unsigned>(n));
    return -EIO;
  }
  auto* arr = static_cast<TaskStatus_t*>(
      heap_caps_malloc(sizeof(TaskStatus_t) * static_cast<size_t>(n + 4), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (arr == nullptr) {
    o.f("cpu: cannot allocate %u task records (internal heap)\n", static_cast<unsigned>(n));
    return -ENOMEM;
  }
  uint32_t total = 0;
  const UBaseType_t got = uxTaskGetSystemState(arr, n + 4, &total);
  if (got == 0) {
    heap_caps_free(arr);
    o.put("cpu: uxTaskGetSystemState returned 0 tasks\n");
    return -EIO;
  }

  // Per-core totals, and the idle share (the idle task is named "IDLE" on both
  // cores in this kernel — configIDLE_TASK_NAME — so the core id is what tells
  // them apart, NOT the name).
  int64_t perCore[2] = {0, 0};
  int64_t idleCore[2] = {0, 0};
  for (UBaseType_t i = 0; i < got; ++i) {
    const int core = taskCore(arr[i]);
    const int64_t c = static_cast<int64_t>(arr[i].ulRunTimeCounter);
    if (core == 0 || core == 1) perCore[core] += c;
    if (strncmp(arr[i].pcTaskName, "IDLE", 4) == 0 && (core == 0 || core == 1)) idleCore[core] += c;
  }

  char tmp[16];
  o.f("cpu: %u tasks, runtime stats ON (esp_timer 1 MHz), counter total %lu\n", static_cast<unsigned>(got),
      static_cast<unsigned long>(total));
  for (int core = 0; core < 2; ++core) {
    const int64_t busy = perCore[core];
    pct(tmp, sizeof(tmp), idleCore[core], busy);
    o.f("core%d: idle=%s of that core's scheduled time\n", core, tmp);
  }
  o.f("mhz=%d  (getCpuFrequencyMhz; 80 after 3 s idle, 320 otherwise)\n", static_cast<int>(getCpuFrequencyMhz()));

  // Top consumers, widest first. 10 rows keeps the frame under the shell's
  // 1280-byte body budget with the header above.
  constexpr int kRows = 10;
  int64_t topCounter[kRows] = {};
  int topIdx[kRows] = {};
  for (UBaseType_t i = 0; i < got; ++i) {
    const int64_t c = static_cast<int64_t>(arr[i].ulRunTimeCounter);
    for (int k = 0; k < kRows; ++k) {
      if (c > topCounter[k]) {
        for (int j = kRows - 1; j > k; --j) {
          topCounter[j] = topCounter[j - 1];
          topIdx[j] = topIdx[j - 1];
        }
        topCounter[k] = c;
        topIdx[k] = static_cast<int>(i);
        break;
      }
    }
  }
  o.put("  task                core      cpu%       ms\n");
  for (int k = 0; k < kRows; ++k) {
    if (topIdx[k] == 0 && topCounter[k] == 0) continue;
    const TaskStatus_t& t = arr[topIdx[k]];
    if (t.ulRunTimeCounter == 0) continue;
    pct(tmp, sizeof(tmp), topCounter[k], total);
    o.f("  %-18.18s %4d  %9s %8.1f\n", t.pcTaskName, taskCore(t), tmp,
        static_cast<double>(topCounter[k]) / 1000.0);
  }
  heap_caps_free(arr);
  return 0;
#endif
}

// ---------------------------------------------------------------------------
// `pm` — the power policy as the firmware actually holds it.
// ---------------------------------------------------------------------------
int formatPm(char* out, size_t cap) {
  Out o{out, cap};
#if defined(CONFIG_PM_ENABLE)
  o.put("pm: PM_ENABLE=1 (compile time) — IDF dynamic power management is ON\n");
#else
  o.put("pm: PM_ENABLE=0 (compile time) — no IDF PM, no DFS, no tickless idle\n");
#endif
#if defined(CONFIG_FREERTOS_USE_TICKLESS_IDLE)
  o.put("    tickless idle=CONFIGURED\n");
#else
  o.put("    tickless idle=not configured (it depends on PM_ENABLE, which is off)\n");
#endif
  o.f("    mhz=%d  normal=%d  low-power=%d\n", static_cast<int>(getCpuFrequencyMhz()),
      powerManager.normalFreqMhz(), powerManager.idleCpuFreqMhz());
  o.f("    state=%s  normal-speed lock=%s\n",
      powerManager.lowPowerActive() ? "low-power(idle step)" : "normal(full speed)",
      powerManager.normalSpeedLockHeld() ? "HELD" : "free");
  const HalPowerManager::Transition tr = powerManager.lastTransition();
  if (tr.atMs == 0) {
    o.put("    last change: none since boot\n");
  } else {
    o.f("    last change: t=%lu ms -> %s (requested through setPowerSaving(%d); the caller is\n",
        static_cast<unsigned long>(tr.atMs), tr.toLowPower ? "low-power (idle step)" : "normal (full speed)",
        tr.toLowPower ? 1 : 0);
    o.put("                 main.cpp's idle policy, not a PM governor)\n");
  }
  const SleepTotals s = lightSleep();
  o.f("    light sleep since boot: %lld us in %lu slices\n", static_cast<long long>(s.totalUs),
      static_cast<unsigned long>(s.slices));
  o.put("not here, by design: current draw (out of scope for r96 — no mA is measured).\n");
  return 0;
}

}  // namespace cpustats

#else  // !CP_CPU_STATS — the switch is off: every entry point answers honestly.

namespace cpustats {

const RenderBreakdown& lastRender() {
  static const RenderBreakdown kNone;
  return kNone;
}

SleepTotals lightSleep() { return SleepTotals{}; }

int formatCpu(char* out, size_t cap) {
  snprintf(out, cap, "cpu: not built (CP_CPU_STATS=0 in src/platform/PlatformFeatures.h)\n");
  return 0;
}

int formatPm(char* out, size_t cap) {
  snprintf(out, cap, "pm: not built (CP_CPU_STATS=0 in src/platform/PlatformFeatures.h)\n");
  return 0;
}

int formatRender(char* out, size_t cap) {
  snprintf(out, cap, "render: not built (CP_CPU_STATS=0 in src/platform/PlatformFeatures.h)\n");
  return 0;
}

}  // namespace cpustats

#endif  // CP_CPU_STATS
