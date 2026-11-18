#pragma once

// Edge-stamped power-key gesture classifier.
//
// WHY THIS EXISTS
// ---------------
// Before this, the awake-UI power key was timed by four loose statics in main.cpp
// (a "press seen" bool, a start stamp, a release stamp and a hold flag) and a
// decision written inline in the loop. That worked, but each rule was only
// checkable by reading the loop, and the state could not be reset as a unit.
//
// The classifier keeps the SAME semantics and moves them behind one object:
//   * the DOWN edge is stamped once and is never re-stamped while the key stays
//     down (a failed expander read that reports "all released" must not restart
//     the clock -- that is what made a 3 s hold measure 400 ms);
//   * a release is believed only after `releaseSettleMs` of continuous UP, and
//     the duration is measured to the FIRST UP tick, so the settle window can
//     never push a press over a classification bound;
//   * classification happens on the (settled) release edge only:
//       - duration >= holdMs                  -> Hold
//       - duration <  holdMs, first click     -> Click
//       - duration <  holdMs, within the      -> MultiClick (clicks = n)
//         multi-click window of the previous
//         click's release
//   * a press inherited across a restart is DISCARDED, never classified.
//
// SOFT RESTARTS
// -------------
// A silent restart (heap-defrag ESP.restart()) re-enters setup() with millis()
// back near zero, so a stamp taken before the restart is meaningless afterwards
// -- and a finger that is still on the key when the new image boots would look
// like a press of "0 ms so far" and was one release away from being read as a
// click. `beginBootMs()` / `beginBoot()` therefore start a new EPOCH: everything
// is cleared, and if the key is already down it is remembered as INHERITED. An
// inherited press produces no click, no multi-click and no hold: its release is
// reported once as `Kind::Discarded` and the next press classifies normally.
// That is the same shape as main.cpp's `powerReleasedSinceWake` rule -- a hold
// that began before we could see its edge must be released before it counts.
//
// The classifier stores no state in RTC memory, so nothing is inherited from a
// previous boot by accident; the epoch exists to make the correct behaviour
// explicit and testable rather than to survive anything.
//
// Header-only and free of Arduino/ESP dependencies: the boundaries above are
// covered by test/power_gesture_classifier on the host.

#include <cstdint>

namespace powergesture {

enum class Kind : uint8_t {
  None = 0,     // nothing to report yet
  Click,        // one short press, released
  MultiClick,   // short press released inside the multi-click window
  Hold,         // press reached holdMs
  Discarded,    // press began before this epoch (soft restart): never classified
};

inline const char* kindName(const Kind kind) {
  switch (kind) {
    case Kind::Click: return "click";
    case Kind::MultiClick: return "multi-click";
    case Kind::Hold: return "hold";
    case Kind::Discarded: return "discarded";
    case Kind::None: break;
  }
  return "none";
}

struct Verdict {
  Kind kind = Kind::None;
  // DOWN edge -> first UP edge, in ms. 0 for None.
  unsigned long pressMs = 0;
  // 1 for Click, n for MultiClick, 0 otherwise.
  uint8_t clicks = 0;
  // Tick at which the DOWN edge was stamped (useful for a journal line).
  unsigned long pressStartMs = 0;
};

struct Config {
  // A press at or after this duration is a Hold. Matches the caller's
  // `heldMs >= holdMs` test: exactly holdMs is a Hold, holdMs-1 is not.
  unsigned long holdMs = 2200;
  // A release is only believed after this much continuous UP. Measured from the
  // first UP tick, so it never stretches the press duration.
  unsigned long releaseSettleMs = 100;
  // Two clicks whose releases are at most this far apart are one MultiClick
  // burst. Exactly this far apart still counts (inclusive).
  unsigned long multiClickMs = 500;
  // A burst is capped so a stuck key cannot overflow `clicks`.
  uint8_t maxClicks = 8;
};

class Classifier {
 public:
  explicit Classifier(const Config& config = Config()) : cfg_(config) {}

  const Config& config() const { return cfg_; }

  // Start a new epoch at boot. `pressed` is the level already read for the key
  // when this is called: a key that is down here began before this epoch was
  // observable and is therefore inherited (see the header note).
  void beginBootMs(const unsigned long nowMs, const bool pressed) { reset(nowMs, pressed); }

  // Explicit restart of one press, e.g. on entry into standby: a press that
  // sleep owned must never be re-classified. Not an epoch boundary -- the
  // multi-click burst is kept, so a click before sleeping still frames the
  // first click after waking.
  void resetPress([[maybe_unused]] const unsigned long nowMs) {
    pressDown_ = false;
    pressInherited_ = false;
    pressStartMs_ = 0;
    releaseCandidateMs_ = 0;
  }

  // Full clear, including the multi-click history.
  void reset(const unsigned long nowMs, const bool pressed) {
    lastClickReleaseMs_ = 0;
    haveLastClick_ = false;
    resetPress(nowMs);
    if (pressed) {
      // No edge was observed for this press: it predates the epoch.
      pressDown_ = true;
      pressInherited_ = true;
      pressStartMs_ = nowMs;
    }
  }

  bool pressDown() const { return pressDown_; }
  bool pressInherited() const { return pressInherited_; }
  unsigned long pressStartMs() const { return pressStartMs_; }
  // Tick of the FIRST UP edge of the current press, or 0 while the key is down.
  // The press duration is `releaseCandidateMs() - pressStartMs()`: measured to
  // the real release instant, never to the settled one.
  unsigned long releaseCandidateMs() const { return releaseCandidateMs_; }

  // Press duration so far. Zero while the key is up -- including the settle
  // window after the release tick, so a held may never keep climbing once the
  // finger is off -- and zero for an inherited press, which has no edge to
  // measure from.
  unsigned long heldMs(const unsigned long nowMs) const {
    if (!pressDown_ || pressInherited_ || releaseCandidateMs_ != 0) return 0;
    return nowMs - pressStartMs_;
  }

  // Feed a debounced level. Idempotent for a level that has not changed.
  void observeLevel(const unsigned long nowMs, const bool down) {
    if (down) {
      // A level that came back inside the settle window was a failed read, not
      // a new press: cancel the pending release and keep the same press clock.
      releaseCandidateMs_ = 0;
      if (!pressDown_) {
        pressDown_ = true;
        pressInherited_ = false;
        pressStartMs_ = nowMs;
      }
      return;
    }
    if (!pressDown_) return;
    // First UP tick: stamp it once. The press stays "down" for classification
    // purposes until the settle window proves the release was real.
    if (releaseCandidateMs_ == 0) releaseCandidateMs_ = nowMs;
  }

  // True once the key has been continuously up for releaseSettleMs, i.e. the
  // release is believed (but not yet consumed by poll()).
  bool releaseSettled(const unsigned long nowMs) const {
    return pressDown_ && releaseCandidateMs_ != 0 && nowMs - releaseCandidateMs_ >= cfg_.releaseSettleMs;
  }

  // Consume a settled release. Returns Kind::None while the press is still down
  // or the release has not settled. On a verdict, `pressMs` is measured to the
  // FIRST UP tick.
  Verdict poll(const unsigned long nowMs) {
    Verdict verdict;
    if (!pressDown_) return verdict;
    if (releaseCandidateMs_ == 0) return verdict;
    if (nowMs - releaseCandidateMs_ < cfg_.releaseSettleMs) return verdict;

    const unsigned long releaseMs = releaseCandidateMs_;
    const unsigned long duration = releaseMs - pressStartMs_;
    const bool inherited = pressInherited_;

    pressDown_ = false;
    pressInherited_ = false;
    pressStartMs_ = 0;
    releaseCandidateMs_ = 0;

    if (inherited) {
      // Began before this epoch: no click, no hold, and it does not frame a
      // burst either.
      verdict.kind = Kind::Discarded;
      verdict.pressMs = duration;
      return verdict;
    }

    verdict.pressMs = duration;
    if (duration >= cfg_.holdMs) {
      verdict.kind = Kind::Hold;
      haveLastClick_ = false;  // a hold ends any burst
      return verdict;
    }

    if (haveLastClick_ && releaseMs - lastClickReleaseMs_ <= cfg_.multiClickMs) {
      verdict.kind = Kind::MultiClick;
      clicks_ = static_cast<uint8_t>(clicks_ < cfg_.maxClicks ? clicks_ + 1 : cfg_.maxClicks);
    } else {
      verdict.kind = Kind::Click;
      clicks_ = 1;
    }
    verdict.clicks = clicks_;
    haveLastClick_ = true;
    lastClickReleaseMs_ = releaseMs;
    return verdict;
  }

 private:
  Config cfg_;
  bool pressDown_ = false;
  bool pressInherited_ = false;
  unsigned long pressStartMs_ = 0;
  unsigned long releaseCandidateMs_ = 0;
  bool haveLastClick_ = false;
  unsigned long lastClickReleaseMs_ = 0;
  uint8_t clicks_ = 0;
};

}  // namespace powergesture
