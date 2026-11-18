#pragma once

// E5 (docs/engineering/HARDWARE_POWER_PLAN.md §5.4) — the DECISION half of the
// touch probe retry ladder, with no hardware in it.
//
// Why this is a separate, hardware-free type: "how many attempts, when may
// another one run, when is the capability abandoned" is the part that can be
// wrong without any board attached, so it is the part a host test can cover.
// The caller owns the two hardware actions (probe, and the recovery pair
// "rail P5 off 200 ms -> on -> settle -> TP_RST pulse") and this type only
// says when they are due.
//
// SEMANTICS (must stay true, the park work depends on them):
//   * attempt 1 is a plain probe and NOTHING else — no delay, no rail, no reset.
//     That is the happy path, and it must cost exactly what it costs today.
//   * every LATER attempt is preceded by the recovery pair (needsRecovery()).
//   * a successful probe latches immediately and ends the ladder: recordAttempt()
//     returns false, so the caller cannot probe a fourth time after success.
//   * after kMaxAttempts failures the ladder is exhausted and stays so; the
//     capability is reported absent for this boot/session only. NOTHING here is
//     persisted — there is no NVS/settings writer anywhere in the ladder.

#include <cstdint>

namespace touchladder {

class Ladder {
 public:
  // rail cycle + reset + probe, up to three probes in total.
  static constexpr int kMaxAttempts = 3;

  // True when the NEXT probe must be preceded by the recovery pair. False
  // before the first attempt (attempt 1 is the plain probe, exactly as today),
  // false once a probe has succeeded, and false after the ladder is exhausted
  // (there is no next probe to prepare for).
  bool needsRecovery() const { return attempts_ > 0 && !ok_ && attempts_ < kMaxAttempts; }

  // Records the outcome of one probe. Returns true when another probe is
  // allowed (this one failed and attempts remain); false on success or when
  // the ladder is exhausted.
  bool recordAttempt(bool probe_ok) {
    ++attempts_;
    ok_ = probe_ok;
    if (probe_ok) return false;
    return attempts_ < kMaxAttempts;
  }

  int attempts() const { return attempts_; }
  bool ok() const { return ok_; }
  bool exhausted() const { return !ok_ && attempts_ >= kMaxAttempts; }
  // True while nothing has been tried yet — the caller's "first attempt" fast
  // path (no recovery, no logging beyond the one result line).
  bool untouched() const { return attempts_ == 0; }

 private:
  int attempts_ = 0;
  bool ok_ = false;
};

}  // namespace touchladder
