#pragma once

// EpdFlashGuard — hold off nvs writes while a panel waveform is driving.
//
// WHY (adopted from the OEM firmware, CloudZao/Metalio-E-INK4-Plus @e703fd3,
// `main/display/epd_flash_guard.h` + `epd_nvs_guard.cc`; see
// docs/planning/VENDOR_DEVICE_DELTA_2026-11-03.md §2 and §"Recommended sequence" #2):
// a waveform needs uninterrupted cache/PSRAM access. On this SoC the flash cache is
// shared with the PSRAM path the epdiy line pipeline bursts through, so a flash write
// (NVS commit = erase + program, plus an NVS page copy) that lands inside a drive can
// stall the fetch path long enough to underrun the line queue — and an underrun is not
// a slow drive, it is a drive that never covered the panel (EPD_DRAW_EMPTY_LINE_QUEUE,
// and the 0x400 recovery path in metalio_vendor_panel.cpp). Whether it is the *cause*
// of a particular artifact is unproven; what is cheap and certain is making the two
// mutually exclusive.
//
// SCOPE CHOSEN — writes only, and only from our own call sites.
//   * It guards the nvs WRITE entry points (set/commit) reached from this tree, not
//     every nvs_* call: reads (nvs_get_*) are a cache/PSRAM read and cannot erase or
//     program flash, so gating them would add exactly the latency the guard is meant to
//     avoid nowhere. `nvs_open(READWRITE)` is not a write either.
//   * The hook is a helper called at the write site (a scope object), not a --wrap on
//     the nvs_* symbols. A link-level wrap would have to apply to the whole component's
//     link line and would also capture the drivers' own internal NVS users (esp_wifi
//     stores its calibration and its stored config in NVS from inside the prebuilt
//     IDF libraries) — i.e. it would promise coverage this tree cannot audit, and an
//     interception that silently misses half its targets is worse than one that states
//     its boundary. The one raw nvs writer in this tree is DeviceSecret.cpp; the
//     settings/state stores are file-based, so today the covered set is complete for
//     our code.
//
// RECURSION, AND WHY THIS CANNOT DEADLOCK.
//   * The mutex is RECURSIVE. A save initiated from inside the refresh path (same task,
//     already holding the drive) does not hang: it takes the lock again, completes, and
//     is counted and logged once as the one case the guard cannot defer.
//   * Verified by reading, not assumed: no nvs write is reachable from the panel or
//     refresh call stack today. The drive path is
//     HalDisplay::displayBuffer -> EInkDisplay::displayBuffer ->
//     metalio_vendor_panel_show -> {g_poll_hook (input poll), g_journal_hook (log
//     append), cp_panel_note (log line)}, and the only nvs writer in the tree
//     (deviceSecret()) is called from CrossPointWebServer and BookKey, neither of which
//     is reachable from there. So the nested branch is a safety net, not a hot path.
//   * Lock order: the flash guard is the OUTERMOST lock a writer takes (DeviceSecret
//     takes its own std::mutex first, then this one, and neither the drive path nor the
//     logging path ever takes DeviceSecret's mutex), so there is no inversion to fix.
//   * A writer that waits is bounded by kMaxWriteWaitMs (2 s, well past the longest
//     observed waveform ~0.8 s) so a lock held by a wedged task cannot block a save
//     forever. A timed-out wait writes ANYWAY and logs an ERR: a save is never dropped.
//
// COST ON THE COMMON PATH: one uncontended try_lock (~tens of ns) per write, and one
// uncontended lock/unlock per drive. A write with no drive running does not wait.
//
// Header-only on purpose: the component globs src/ at configure time, and a new .cpp
// would need a full reconfigure to be compiled at all.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include <Logging.h>

#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif

namespace epdflash {

namespace detail {

// One recursive mutex for the whole device: there is one panel and one flash device, so
// per-drive or per-writer partitioning would only violate the exclusion it exists for.
inline std::recursive_timed_mutex& mutex() {
  static std::recursive_timed_mutex m;
  return m;
}

// Depth of the drive hold. Read without the mutex by a writer that has not locked yet, so
// it is atomic; it is only ever modified while the mutex is held.
inline std::atomic<int>& depth() {
  static std::atomic<int> d{0};
  return d;
}

// Diagnostics (never load-bearing): how often a write had to wait, how long the worst one
// was, and how often a write ran from inside a drive.
inline uint32_t& blockedWrites() {
  static uint32_t n = 0;
  return n;
}
inline uint32_t& nestedWrites() {
  static uint32_t n = 0;
  return n;
}
inline int64_t& worstWaitMs() {
  static int64_t ms = 0;
  return ms;
}

constexpr int kMaxWriteWaitMs = 2000;

inline int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
// PM-P1: one process-wide esp_pm lock for panel drives. Held ONLY by the outermost
// DriveScope (see below), i.e. exactly for the duration of a waveform, never across an
// idle wait.
//
// WHY APB_FREQ_MAX: the ED047TC2 is driven by the vendor epdiy stack through LCD_CAM with
// timing derived from the APB/PLL path (EpdBus/FreeInkDisplay's own pm locks only cover
// the SPI bus and the light-sleep decision). A frequency switch under a running waveform
// would change the line timing mid-drive. ESP_PM_APB_FREQ_MAX also keeps the APB mode
// above PM_MODE_APB_MAX, so it excludes light sleep for the drive as well.
//
// It is created on the first drive, not at boot: esp_pm is initialised by esp_system, and
// a drive never runs before that. Creation failure (e.g. PM compiled out) leaves the
// handle null and every call below a no-op -- the same idiom as
// freeink-sdk FreeInkDisplay.cpp displayNoLightSleepLock().
inline esp_pm_lock_handle_t& panelPmLock() {
  static esp_pm_lock_handle_t lock = nullptr;
  static bool initialised = false;
  if (!initialised) {
    initialised = true;
    if (esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "epd-drive", &lock) != ESP_OK) {
      lock = nullptr;
    }
  }
  return lock;
}

inline void panelPmAcquire() {
  const esp_pm_lock_handle_t lock = panelPmLock();
  if (lock != nullptr) esp_pm_lock_acquire(lock);
}

inline void panelPmRelease() {
  const esp_pm_lock_handle_t lock = panelPmLock();
  if (lock != nullptr) esp_pm_lock_release(lock);
}
#endif  // CONFIG_PM_ENABLE

}  // namespace detail

// True while a waveform is driving (any task).
inline bool driveActive() { return detail::depth().load(std::memory_order_relaxed) > 0; }

// Stats snapshot, for a shell command or a log line. Cheap, and safe from any task.
struct Stats {
  bool drive = false;
  uint32_t blocked = 0;
  uint32_t nested = 0;
  int64_t worstWaitMs = 0;
};
inline Stats stats() {
  Stats s;
  s.drive = driveActive();
  s.blocked = detail::blockedWrites();
  s.nested = detail::nestedWrites();
  s.worstWaitMs = detail::worstWaitMs();
  return s;
}

// ---------------------------------------------------------------- drive side ----
// Construct at the TOP of a panel drive and let it fall out of scope after the waveform
// returns. Recursive so a driver that re-enters a HalDisplay entry point cannot deadlock
// against itself.
class DriveScope {
 public:
  DriveScope() {
    detail::mutex().lock();
#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
    // Only the OUTERMOST drive takes the PM lock: a nested entry point (see the recursion
    // note above) must not acquire it twice, because esp_pm locks are not per-task counted
    // here and a mismatched release would be an error.
    if (detail::depth().fetch_add(1, std::memory_order_relaxed) == 0) {
      detail::panelPmAcquire();
    }
#else
    detail::depth().fetch_add(1, std::memory_order_relaxed);
#endif
  }
  ~DriveScope() {
#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
    if (detail::depth().fetch_sub(1, std::memory_order_relaxed) == 1) {
      detail::panelPmRelease();
    }
#else
    detail::depth().fetch_sub(1, std::memory_order_relaxed);
#endif
    detail::mutex().unlock();
  }
  DriveScope(const DriveScope&) = delete;
  DriveScope& operator=(const DriveScope&) = delete;
};

// ---------------------------------------------------------------- write side ----
// Construct around the WHOLE write (set + commit), so the pair is never split by a drive
// starting between them.
class NvsWriteScope {
 public:
  explicit NvsWriteScope(const char* what) : what_(what != nullptr ? what : "nvs") {
    if (detail::mutex().try_lock()) {
      held_ = true;
      if (detail::depth().load(std::memory_order_relaxed) > 0) {
        // Same task, already inside a drive: a write issued FROM the refresh path. The
        // recursive lock makes it safe instead of a hang; it is counted and said out loud
        // once because it is the case the guard cannot hold off.
        const uint32_t n = ++detail::nestedWrites();
        if (n == 1) {
          LOG_ERR("GUARD",
                  "nvs write '%s' issued from INSIDE a panel drive: recursion keeps it safe, but a "
                  "flash program is landing during the waveform (further ones are only counted)",
                  what_);
        }
      }
      return;
    }

    // Someone else holds the guard. A drive is its only long holder, so wait for it to
    // finish. This is the intended behaviour: a save is DELAYED, never dropped.
    const int64_t t0 = detail::nowMs();
    held_ = detail::mutex().try_lock_for(std::chrono::milliseconds(detail::kMaxWriteWaitMs));
    const int64_t waitedMs = detail::nowMs() - t0;
    ++detail::blockedWrites();
    if (waitedMs > detail::worstWaitMs()) detail::worstWaitMs() = waitedMs;

    if (held_) {
      LOG_INF("GUARD", "nvs write '%s' deferred %ld ms by a panel drive", what_,
              static_cast<long>(waitedMs));
    } else {
      // Nothing is ever dropped: the write goes ahead unlocked and says so. A failure to
      // take the lock is a diagnostic event, not a reason to lose a settings save.
      LOG_ERR("GUARD",
              "nvs write '%s' waited %ld ms for the panel guard and timed out: writing anyway "
              "(NOT dropped, exclusion lost for this write)",
              what_, static_cast<long>(waitedMs));
    }
  }

  ~NvsWriteScope() {
    if (held_) detail::mutex().unlock();
  }

  bool held() const { return held_; }

  NvsWriteScope(const NvsWriteScope&) = delete;
  NvsWriteScope& operator=(const NvsWriteScope&) = delete;

 private:
  const char* what_ = "nvs";
  bool held_ = false;
};

}  // namespace epdflash
