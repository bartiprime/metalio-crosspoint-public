#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// r163 E6 -- POWER NEEDS, declared by whoever holds them, counted, and visible.
//
// WHAT PROBLEM THIS REPLACES. Until this file, "may the device auto-sleep?" was
// answered by asking ONE forwarder (`activityManager.preventAutoSleep()`) and a
// set of board-shaped booleans, each read at the moment of the decision. Two
// consequences, both observed:
//   * a hold that was never released was INVISIBLE. The device simply never
//     auto-slept again, and nothing anywhere said why.
//   * every new reason to stay awake needed a new method on a class, and the
//     decision site grew a condition per reason.
//
// THE MODEL (the vendor's: power_policy, `power_req.cc`). A module ACQUIRES a
// need while it needs power and RELEASES it when it is done. The decision site
// asks ONE question -- `blocksAutoStandby()` -- and never asks why.
//
//   HARD  blocks AUTOMATIC (inactivity) standby. Nothing else: it never blocks a
//         manual sleep, and it cannot reorder the park (it is read before
//         enterDeepSleep() is called, never inside it).
//         Releasing the LAST hold of a hard need REFRESHES the inactivity clock,
//         so a session that ends does not sleep the instant it lets go.
//   SOFT  visible, counted, and deliberately INERT: it does not refresh the
//         inactivity clock and it does not prevent automatic standby. It exists
//         so "this is happening right now" can be seen (and so a leak in it is
//         visible) without the fact changing when the device sleeps.
//
// WHY A LEAK IS THE INTERESTING FAILURE. A need acquired and never released
// blocks standby forever. So every hold carries an age, and a hold older than
// its need's budget is ANNOUNCED once per excursion (poll() + the caller's
// takeLeakAnnouncement()) and stays visible in the debug surface with its age.
//
// THIS HEADER OWNS NO I/O AND NO TASK. It is a counter table and a clock the
// caller passes in, which is why the algebra below is host-tested
// (test/power_need). The one instance is `sys()`.
// ---------------------------------------------------------------------------

namespace powerneed {

enum class Need : uint8_t {
  // The SD card is exported to a USB host (drive mode). The panel must not park
  // out from under a transfer. Vendor: `UsbVirtualDisk`, HARD, and it blocks the
  // INACTIVITY standby only -- the operator can still sleep by hand.
  UsbVirtualDisk = 0,
  // "Not while the device is merely idle." The generic hard hold for a session
  // that must finish (a firmware install, a calibration sweep). Vendor:
  // `StandbyInhibit`, HARD, inactivity standby only.
  StandbyInhibit = 1,
  // On charge: the charge-aware idle policy (item 3.5) holds this while the pack
  // is charging on a cable. See src/system/ChargeIdlePolicy.h.
  ChargerHost = 2,
  // A foreground network session (web server, Calibre, a Wi-Fi picker). Vendor:
  // `ForegroundNetwork`, SOFT -- it does NOT keep the device awake and does not
  // feed the inactivity clock. It is here to be SEEN (and to make a session that
  // never ends visible as a leak) without changing when the device sleeps.
  ForegroundNetwork = 3,
};

constexpr uint8_t kNeedCount = 4;

enum class Hold : uint8_t { Soft = 0, Hard = 1 };

constexpr Hold holdOf(const Need n) {
  switch (n) {
    case Need::UsbVirtualDisk:
    case Need::StandbyInhibit:
    case Need::ChargerHost:
      return Hold::Hard;
    case Need::ForegroundNetwork:
      return Hold::Soft;
  }
  return Hold::Hard;  // an unlisted need fails closed: block, and be visible.
}

// Age at which a held need is called a LEAK, in ms. 0 = not age-monitored: the
// hold's owner re-evaluates it from a hardware predicate on every loop
// iteration, so it cannot be leaked by a missed release -- a stuck one shows as
// an old age in the debug surface instead of as an announce.
constexpr unsigned long leakBudgetMs(const Need n) {
  switch (n) {
    case Need::UsbVirtualDisk:
      return 6UL * 60UL * 60UL * 1000UL;  // a real drive session can be hours long
    case Need::StandbyInhibit:
      return 30UL * 60UL * 1000UL;
    case Need::ForegroundNetwork:
      return 15UL * 60UL * 1000UL;
    case Need::ChargerHost:
      return 0UL;
  }
  return 0UL;
}

inline const char* nameOf(const Need n) {
  switch (n) {
    case Need::UsbVirtualDisk:
      return "usb-virtual-disk";
    case Need::StandbyInhibit:
      return "standby-inhibit";
    case Need::ChargerHost:
      return "charger-host";
    case Need::ForegroundNetwork:
      return "foreground-network";
  }
  return "?";
}

inline const char* holdName(const Hold h) { return h == Hold::Hard ? "hard" : "soft"; }

// One need, as the debug surface reports it.
struct Status {
  const char* name = "";
  Hold hold = Hold::Hard;
  uint8_t count = 0;
  unsigned long heldMs = 0;  // 0 when not held
  bool leaked = false;       // held longer than the budget (budget > 0 only)
  uint32_t acquiredTotal = 0;
  uint32_t releasedTotal = 0;
};

struct Report {
  Status status[kNeedCount];
  bool blocksAutoStandby = false;
  bool awaitingActivityRefresh = false;
  uint32_t doubleReleases = 0;  // releases with nothing held -- always a bug
  uint32_t leakedNow = 0;       // needs currently past their budget
  uint32_t leakedEver = 0;      // excursions announced since boot
};

class Tracker {
 public:
  // One hold. Nested acquires are legal and counted; the LAST release is what
  // ends the hold (and, for a hard need, what refreshes the inactivity clock).
  void acquire(const Need n, const unsigned long nowMs) {
    Slot& s = slot(n);
    if (s.count == 0) {
      s.firstMs = nowMs;
      s.announced = false;
    }
    if (s.count < 255) ++s.count;
    ++s.acquiredTotal;
  }

  // Returns false when nothing was held: a DOUBLE RELEASE, which is counted and
  // never allowed to wrap the count. The caller names it in the log.
  //
  // `refreshIdle` is false only for a BACKSTOP release (the RAII guard's destructor). A hold that
  // ended because its scope died was not an orderly end of work, and granting it a fresh
  // inactivity window would let a leaked hold postpone auto-standby instead of merely stopping
  // blocking it.
  bool release(const Need n, const unsigned long nowMs, const bool refreshIdle = true) {
    (void)nowMs;
    Slot& s = slot(n);
    if (s.count == 0) {
      ++doubleReleases_;
      return false;
    }
    --s.count;
    ++s.releasedTotal;
    if (s.count == 0) {
      s.announced = false;
      if (holdOf(n) == Hold::Hard && refreshIdle) activityRefreshPending_ = true;
    }
    return true;
  }

  // Declarative form: make the hold match `wanted`. Used where the owner is a
  // hardware predicate re-read every loop iteration (the charge policy), so the
  // hold cannot be leaked by a missed release path.
  void set(const Need n, const bool wanted, const unsigned long nowMs) {
    if (wanted) {
      if (slot(n).count == 0) acquire(n, nowMs);
    } else if (slot(n).count != 0) {
      release(n, nowMs);
    }
  }

  bool held(const Need n) const { return slot(n).count != 0; }
  uint8_t count(const Need n) const { return slot(n).count; }
  uint32_t doubleReleases() const { return doubleReleases_; }

  // THE ONE QUESTION the power policy asks.
  bool blocksAutoStandby() const {
    for (uint8_t i = 0; i < kNeedCount; ++i) {
      const Need n = static_cast<Need>(i);
      if (holdOf(n) == Hold::Hard && slots_[i].count != 0) return true;
    }
    return false;
  }

  bool anyHeld() const {
    for (uint8_t i = 0; i < kNeedCount; ++i)
      if (slots_[i].count != 0) return true;
    return false;
  }

  // True once after the last hold of a HARD need was released. The caller moves
  // the inactivity clock; this class does not know what a clock is.
  bool takeActivityRefresh() {
    const bool p = activityRefreshPending_;
    activityRefreshPending_ = false;
    return p;
  }
  bool awaitingActivityRefresh() const { return activityRefreshPending_; }

  // Age/leak pass. Call once per awake loop iteration. Sets the announce bit for
  // every need that has JUST crossed its budget (once per excursion).
  void poll(const unsigned long nowMs) {
    for (uint8_t i = 0; i < kNeedCount; ++i) {
      const Need n = static_cast<Need>(i);
      Slot& s = slots_[i];
      const unsigned long budget = leakBudgetMs(n);
      if (budget == 0 || s.count == 0 || s.announced) continue;
      if (nowMs - s.firstMs < budget) continue;
      s.announced = true;
      ++leakAnnouncements_;
      announceMask_ |= static_cast<uint8_t>(1u << i);
    }
  }

  // True once per leaked excursion. The caller logs the line (this header has no
  // logger, and a host test must not need one).
  bool takeLeakAnnouncement(const Need n) {
    const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(n));
    if ((announceMask_ & bit) == 0) return false;
    announceMask_ &= static_cast<uint8_t>(~bit);
    return true;
  }
  uint8_t pendingAnnounceMask() const { return announceMask_; }

  // A standby owns the wall clock for its whole duration (millis() advances
  // across it), so the age of every hold must be rebased at the resume or a long
  // park would announce a leak that never happened. Counts and budgets are
  // untouched.
  void rebase(const unsigned long nowMs) {
    for (uint8_t i = 0; i < kNeedCount; ++i) {
      if (slots_[i].count != 0) slots_[i].firstMs = nowMs;
    }
  }

  // Test hook: forget every hold and every counter (a cold boot).
  void reset() { *this = Tracker(); }

  Report report(const unsigned long nowMs) const {
    Report r;
    for (uint8_t i = 0; i < kNeedCount; ++i) {
      const Need n = static_cast<Need>(i);
      const Slot& s = slots_[i];
      Status& st = r.status[i];
      st.name = nameOf(n);
      st.hold = holdOf(n);
      st.count = s.count;
      st.acquiredTotal = s.acquiredTotal;
      st.releasedTotal = s.releasedTotal;
      if (s.count != 0) {
        st.heldMs = nowMs - s.firstMs;
        const unsigned long budget = leakBudgetMs(n);
        const bool past = budget != 0 && st.heldMs >= budget;
        st.leaked = past || s.announced;
        if (st.leaked) ++r.leakedNow;
      }
    }
    r.blocksAutoStandby = blocksAutoStandby();
    r.awaitingActivityRefresh = activityRefreshPending_;
    r.doubleReleases = doubleReleases_;
    r.leakedEver = leakAnnouncements_;
    return r;
  }

 private:
  struct Slot {
    uint8_t count = 0;
    unsigned long firstMs = 0;
    bool announced = false;
    uint32_t acquiredTotal = 0;
    uint32_t releasedTotal = 0;
  };

  Slot& slot(const Need n) { return slots_[static_cast<uint8_t>(n) % kNeedCount]; }
  const Slot& slot(const Need n) const { return slots_[static_cast<uint8_t>(n) % kNeedCount]; }

  Slot slots_[kNeedCount];
  bool activityRefreshPending_ = false;
  uint8_t announceMask_ = 0;
  uint32_t doubleReleases_ = 0;
  uint32_t leakAnnouncements_ = 0;
};

// The one instance. Header-only on purpose: one definition, no .cpp to wire into
// the build, and the host test exercises exactly this object.
inline Tracker& sys() {
  static Tracker t;
  return t;
}

// A hold as an object, for the owners that have a clear scope (an activity's
// session). Same shape as HalPowerManager::FullSpeedHold: acquire once, release
// on scope exit, and release() is idempotent so a backstop release is free.
// Takes the clock from the caller so this header stays free of Arduino.
class ScopedNeed {
 public:
  constexpr explicit ScopedNeed(const Need n) : need_(n) {}
  ~ScopedNeed() { backstopRelease(); }

  ScopedNeed(const ScopedNeed&) = delete;
  ScopedNeed& operator=(const ScopedNeed&) = delete;

  void acquire(const unsigned long nowMs) {
    if (held_) return;
    sys().acquire(need_, nowMs);
    held_ = true;
  }

  // Returns false when it was not held (already released): a backstop release
  // after a normal one must not be counted as a double release.
  bool release(const unsigned long nowMs) {
    if (!held_) return false;
    held_ = false;
    return sys().release(need_, nowMs);
  }

  // The destructor's release. Same bookkeeping, but it does NOT refresh the inactivity clock: a
  // hold that only ended because its scope died must not earn the fresh idle window an orderly
  // release earns.
  bool backstopRelease() {
    if (!held_) return false;
    held_ = false;
    return sys().release(need_, 0, /*refreshIdle=*/false);
  }

  bool held() const { return held_; }

 private:
  Need need_;
  bool held_ = false;
};

inline void acquire(const Need n, const unsigned long nowMs) { sys().acquire(n, nowMs); }
inline bool release(const Need n, const unsigned long nowMs) { return sys().release(n, nowMs); }
inline void set(const Need n, const bool wanted, const unsigned long nowMs) { sys().set(n, wanted, nowMs); }
inline bool held(const Need n) { return sys().held(n); }
inline bool blocksAutoStandby() { return sys().blocksAutoStandby(); }

}  // namespace powerneed
