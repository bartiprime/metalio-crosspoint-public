#pragma once

// Hold-to-repeat cadence for the reader's page-turn buttons (r163 F5).
//
// The rate is anchored to the PRESS TIMESTAMP, not to the last tick: tick k is
// always due at pressAt + kStartMs + k * kIntervalMs, so a pass that runs late
// cannot shift the cadence and the repeats cannot drift. A pass that runs so
// late that the backlog exceeds kMaxLagMs drops the missed ticks instead of
// firing them all at once (a held button must never machine-gun the panel).
//
// Dependency-free on purpose: the timing rules are host-testable without the
// reader, the SDK or a device.
class PageRepeat {
 public:
  static constexpr unsigned long kIntervalMs = 100;  // 10 pages/s
  // Dwell before the first repeat. Long enough that a deliberate single turn is
  // never followed by a stray second one, short enough to feel like a hold.
  static constexpr unsigned long kStartMs = 400;
  // Backlog bound. One interval of lateness is normal (the poll and the interval
  // are the same length); more than three means the loop stalled, so the missed
  // pages are abandoned rather than queued.
  static constexpr unsigned long kMaxLagMs = 300;

  // Release / new gesture: forget everything, including a latched stop.
  void reset() { *this = PageRepeat{}; }

  // Called on the pass that first sees the button held. `pressAtMs` is the press
  // timestamp (millis() - the input layer's held time), never the current time.
  void arm(const int direction, const unsigned long pressAtMs) {
    if (direction == 0) {
      reset();
      return;
    }
    direction_ = direction;
    anchored_ = true;
    stopped_ = false;
    nextDueMs_ = pressAtMs + kStartMs;
  }

  // A step that could not move (page boundary, failed turn): stay stopped for the
  // rest of this press instead of retrying 10 times a second.
  void stop() { stopped_ = true; }

  [[nodiscard]] bool armed() const { return anchored_; }
  [[nodiscard]] bool stopped() const { return stopped_; }
  [[nodiscard]] int direction() const { return direction_; }

  // True at most once per call: the caller paints one page per pass.
  bool due(const unsigned long nowMs) {
    if (!anchored_ || stopped_) return false;
    if (static_cast<long>(nowMs - nextDueMs_) < 0) return false;
    if (nowMs - nextDueMs_ > kMaxLagMs) {
      // Stalled past the backlog bound: drop the missed ticks, resume one
      // interval out, and keep the cadence anchored from there.
      nextDueMs_ = nowMs + kIntervalMs;
    } else {
      nextDueMs_ += kIntervalMs;
    }
    return true;
  }

 private:
  bool anchored_ = false;
  bool stopped_ = false;
  int direction_ = 0;
  unsigned long nextDueMs_ = 0;
};
