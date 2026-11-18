#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// r165.6 -- a one-shot pin pair bound to the NEXT COMPOSE.
//
// WHAT THE PINS ARE. The battery producers (src/main.cpp: the BATT6 percentage
// fallback, the BATT5 glyph fallback and the legacy USB-edge repaint) pin the
// next paint to a diff-masked binary DU --
// renderer.forceBinaryDuOnce() + renderer.suppressCompletionDriveOnce() --
// and then ask for that paint with activityManager.requestUpdate(), which
// returns VOID.
//
// WHY A LOOP PASS WAS THE WRONG UNIT (r165.3's D1 fix, which this replaces).
// requestUpdate(false) only sets `requestedUpdate` (ActivityManager.cpp:1018);
// the paint happens on the RENDER TASK, asynchronously, when that task is
// notified and takes the RenderLock (renderTaskLoop, ActivityManager.cpp:95).
// The main loop keeps running in between -- delay(10) / up to a 50 ms idle
// slice in main.cpp's tail -- so the "next loop boundary" is reached LONG
// before the render task composes. D1 cleared the pins there, i.e. it cleared
// exactly the pair the requested paint was going to consume: the deferred
// repaint ran unpinned, inherited the screen's mode, and a grey-wipe style
// drive could happen where a binary DU was intended. Whether it worked was a
// race against the render task's scheduling.
//
// THE RULE NOW. A pin belongs to the next COMPOSE, however many loop passes
// away it is, and the renderer's compose epoch is the witness that it happened:
//
//   * `composed` is bumped by every canvas mutation (GfxRenderer::
//     noteFramebufferWrite() -> GlassEpoch::noteCanvasWrite()), so a paint that
//     STARTS composing advances it before it ever reads the pins;
//   * the armer records that epoch at arm time;
//   * every loop pass asks reconcile(): a changed epoch means a compose has
//     happened since the arm, so the intent is spent -- either that compose
//     consumed the pins (normal, nothing left to clear) or it refused them (the
//     entry gate holds the drive above the one-shot consumption), in which case
//     the remainder is dropped so no LATER paint can inherit it;
//   * an unchanged epoch past kComposeDeadlineMs means the paint never came at
//     all (skipped for a queued navigation, declined, or lost), and a pair that
//     nobody consumed must not poison an unrelated paint.
//
// INVARIANT: an armed pair is consumed by exactly one compose -- the next one --
// or is dropped before any compose can observe it as stale. Max composes that
// can see a pin: ONE. The latter half holds because a compose on this panel is
// at least one DU drive (~200 ms) while the loop reconciles every slice
// (<= 50 ms): a compose that begins advances the epoch, and the reconcile that
// runs before the next compose can begin clears whatever it left behind.
//
// Kept free of renderer / HAL dependencies so it is host-tested against this
// exact code (test/oneshot_paint_pins/).
// ---------------------------------------------------------------------------
namespace oneshotpins {

// How long an armed pair may wait for a compose to BEGIN.
//
// A paint's compose is the first thing the render task does once it holds the
// RenderLock, so on a healthy path the epoch advances within a pass of the
// request. What can delay it is the tail of the paint already running ahead of
// the request -- a DU drive is ~0.2 s on this panel, HALF/GL16 longer -- plus
// notification and lock latency. 250 ms covers one full DU drive ahead of the
// request plus a couple of 50 ms idle slices, and is far shorter than the
// multi-second window a skipped or declined request would otherwise leave a
// stale pair set for.
constexpr uint32_t kComposeDeadlineMs = 250;

enum class PinState : uint8_t {
  Idle,      // nothing armed
  Pending,   // armed, no compose yet, deadline not reached: still the next compose's
  Consumed,  // a compose began since the arm; the intent is spent -- drop any remainder
  Dropped,   // the deadline elapsed with no compose: the paint never came
};

class ComposeBoundPins {
 public:
  // `composeEpoch` is GfxRenderer::framebufferComposeEpoch() sampled BEFORE the
  // request is issued, `nowMs` the arming timestamp. Idempotent within a pass:
  // two armers arming in one pass both mean "the next compose is ours".
  void arm(const uint32_t composeEpoch, const uint32_t nowMs) {
    armed_ = true;
    epoch_ = composeEpoch;
    armedMs_ = nowMs;
  }

  // Once per loop pass, before any armer. The returned state is the caller's
  // instruction: Consumed and Dropped both mean "clear whatever the renderer
  // still holds" (clearing a pair a compose consumed is a no-op).
  PinState reconcile(const uint32_t composeEpochNow, const uint32_t nowMs) {
    if (!armed_) return PinState::Idle;
    if (composeEpochNow != epoch_) {
      armed_ = false;
      return PinState::Consumed;
    }
    if (static_cast<uint32_t>(nowMs - armedMs_) >= kComposeDeadlineMs) {
      armed_ = false;
      return PinState::Dropped;
    }
    return PinState::Pending;
  }

  bool armed() const { return armed_; }
  uint32_t armedEpoch() const { return epoch_; }

 private:
  bool armed_ = false;
  uint32_t epoch_ = 0;
  uint32_t armedMs_ = 0;
};

}  // namespace oneshotpins
