// r165.6 — the battery one-shot pins are bound to the NEXT COMPOSE, on the host.
//
// Everything below is the REAL header the firmware compiles
// (src/display/OneShotPaintPins.h), driven through a model of the renderer's two
// one-shot booleans plus the compose epoch, and of the ASYNC ordering that the
// r165.3 harness got wrong:
//
//   the armer runs on the MAIN task: pins armed, then requestUpdate(), which only
//   sets a flag (ActivityManager.cpp:1018);
//   the paint runs on the RENDER TASK, when it is notified and takes the lock
//   (renderTaskLoop, ActivityManager.cpp:95) -- one or more loop passes later.
//
// So the harness drives N loop passes with NO compose, then composes -- and the
// reverse -- instead of assuming arm->paint synchronously.
//
// THE PROPERTY: an armed pair is consumed by exactly ONE compose -- the next one --
// or is dropped before it can be observed as stale.
//
//   * a compose one or more passes after the arm still gets the pins (r165.3 dropped
//     them at the first boundary, so the deferred repaint ran unpinned);
//   * a compose that begins and REFUSES them (the entry gate) still spends the
//     intent: the remainder is cleared at the next pass;
//   * no compose at all within kComposeDeadlineMs -> dropped, never applied later;
//   * two armers in one pass are one intent: one compose carries both, no double drive;
//   * a pair armed and consumed inside a paint (InPlaceValueStep and friends) never
//     belongs to this tracker and is never cleared by it.
//
// PHYSICAL PREMISE OF THE BOUND, asserted by the Device: a compose is at least one
// DU drive on this panel (~200 ms) and the loop reconciles on every slice
// (<= 50 ms), so no two composes can both begin inside one pass interval. That is
// what makes "max composes that see a pin = 1" true on the device; the sweep below
// measures it under every placement the schedule allows.
//
// Built by run.sh; standalone like test/batt_percent_change (no test/CMakeLists.txt
// entry -- the header has no dependencies).
#include <cstdint>
#include <cstdio>

#include "display/OneShotPaintPins.h"

namespace {

int checksRun = 0;
int checksFailed = 0;

#define CHECK(condition)                                                \
  do {                                                                  \
    ++checksRun;                                                        \
    if (!(condition)) {                                                 \
      ++checksFailed;                                                   \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);  \
    }                                                                   \
  } while (0)

void section(const char* name) { std::printf("-- %s\n", name); }

// Hardware premise (see the file comment). The sweep fails loudly if a test asks
// for a schedule these two numbers forbid, instead of quietly measuring a bound
// that no device can produce.
constexpr uint32_t kLoopSliceMs = 20;       // <= the 50 ms idle slice in main.cpp's tail
constexpr uint32_t kComposePeriodMs = 200;  // >= one diff-masked DU drive on this panel

// GfxRenderer's two one-shots, the canvas->glass compose epoch
// (GfxRenderer::framebufferComposeEpoch()), and the drive decision that consumes
// them. A compose writes the canvas FIRST -- that is what advances `composed` --
// and only then reaches the drive decision; `held` models the entry gate, which
// returns between the two, so the frame is composed and the pins are untouched.
struct FakeRenderer {
  bool du = false;
  bool sup = false;
  uint32_t composed = 0;
  int composesThatSawPins = 0;
  bool anyCompose = false;
  uint32_t lastComposeAtMs = 0;

  void armPins() {
    du = true;
    sup = true;
  }
  bool pending() const { return du || sup; }
  void clearPins() {
    du = false;
    sup = false;
  }
  void compose(uint32_t nowMs, bool held = false) {
    ++composed;  // the canvas is written: the epoch the armer recorded has moved
    anyCompose = true;
    lastComposeAtMs = nowMs;
    if (held) return;  // the entry gate consumed nothing
    if (pending()) ++composesThatSawPins;
    clearPins();
  }
};

// src/main.cpp as the battery section uses it: one tracker for every armer, the
// reconcile at the top of each pass, the paint on the render task.
struct Device {
  FakeRenderer r;
  oneshotpins::ComposeBoundPins pins;
  uint32_t nowMs = 0;
  int requests = 0;         // requestUpdate() calls
  int consumed = 0;         // passes where a compose had begun since the arm
  int dropped = 0;          // passes where the deadline expired with no compose
  int leftovers = 0;        // of those, how many found the renderer still holding the pair
  bool scheduleImpossible = false;

  // main.cpp BATT6 (percentage fallback) / BATT5 (glyph fallback) / the legacy
  // USB-edge repaint, all three: tracker.arm(epoch, now), then the two pins, then
  // requestUpdate(). Idempotent within a pass, like the real ones.
  void armPercent() {
    pins.arm(r.composed, nowMs);
    r.armPins();
    ++requests;
  }
  void armIcon() {
    pins.arm(r.composed, nowMs);
    r.armPins();
    ++requests;
  }

  // One main-loop pass: the reconcile runs BEFORE any armer, so it never sees the
  // pins a pass is about to arm.
  void pass(const uint32_t advanceMs = kLoopSliceMs) {
    nowMs += advanceMs;
    const oneshotpins::PinState state = pins.reconcile(r.composed, nowMs);
    if (state == oneshotpins::PinState::Consumed || state == oneshotpins::PinState::Dropped) {
      if (state == oneshotpins::PinState::Consumed) {
        ++consumed;
      } else {
        ++dropped;
      }
      if (r.pending()) ++leftovers;  // the pair a compose refused, or that nobody ran
      r.clearPins();
    }
  }

  // A full drive later: the earliest a second compose could physically begin.
  void waitForNextCompose() { nowMs += kComposePeriodMs; }

  // The render task composing. `held` is the entry gate.
  void compose(const bool held = false) {
    if (r.anyCompose && nowMs - r.lastComposeAtMs < kComposePeriodMs) scheduleImpossible = true;
    r.compose(nowMs, held);
  }
};

void testDeferredComposeStillGetsThePins() {
  section("arm -> 5 passes with no compose -> compose: the pins are there, once");
  Device d;
  d.armPercent();
  for (int i = 0; i < 5; ++i) {
    d.pass();
    // THE r165.3 DEFECT, as a property: at no pass before the compose may the pins
    // have been dropped. The old code cleared them at the first boundary.
    CHECK(d.pins.armed());
    CHECK(d.r.pending());
  }
  CHECK(d.r.composesThatSawPins == 0);
  d.compose();
  CHECK(d.r.composesThatSawPins == 1);  // the deferred repaint WAS pinned
  CHECK(!d.r.pending());
  // ...and the pass after it has nothing left to clear and nothing to re-apply.
  d.pass();
  CHECK(d.consumed == 1);
  CHECK(d.leftovers == 0);  // the compose had already consumed them
  CHECK(d.dropped == 0);    // the epoch witness, not the deadline, ended the intent
  CHECK(!d.pins.armed());
  d.waitForNextCompose();
  d.compose();
  CHECK(d.r.composesThatSawPins == 1);
  CHECK(!d.scheduleImpossible);
}

void testAbsentPaintIsDropped() {
  section("arm -> no compose ever -> the deadline drops it, never applied later");
  Device d;
  d.armPercent();
  int passes = 0;
  while (d.dropped == 0 && passes < 100) {
    d.pass();
    ++passes;
  }
  std::printf("   measured: dropped on pass %d (%u ms after the arm)\n", passes,
              static_cast<unsigned>(d.nowMs));
  CHECK(d.dropped == 1);
  CHECK(passes * kLoopSliceMs >= oneshotpins::kComposeDeadlineMs);
  CHECK(!d.r.pending());
  CHECK(!d.pins.armed());
  // The paint that finally runs -- another screen -- is an ordinary paint.
  d.compose();
  CHECK(d.r.composesThatSawPins == 0);
  CHECK(d.leftovers == 1);  // the pair nobody ran, dropped by the deadline
}

void testRefusedComposeLeavesNothingBehind() {
  section("arm -> compose that REFUSES the pins (entry gate): the remainder is cleared");
  Device d;
  d.armPercent();
  d.pass();
  d.compose(/*held=*/true);  // the epoch moves, the gate consumes nothing
  CHECK(d.r.composesThatSawPins == 0);
  CHECK(d.r.pending());  // ...which is exactly the leftover that used to leak
  d.pass();
  CHECK(d.consumed == 1);
  CHECK(d.leftovers == 1);  // the refused pair: cleared, so it cannot reach another paint
  CHECK(d.dropped == 0);    // ended by the epoch, not by the deadline
  CHECK(!d.r.pending());
  d.nowMs += kComposePeriodMs;
  d.compose();
  CHECK(d.r.composesThatSawPins == 0);
  CHECK(!d.scheduleImpossible);
}

void testSkippedIconPaint() {
  section("BATT5's armer: a skipped paint -- dropped when nothing composes, and never in two composes");
  // (a) the queued-navigation skip: the screen is replaced without a paint of its own
  //     and nothing composes inside the window.
  {
    Device d;
    d.armIcon();
    for (int i = 0; i < 5; ++i) d.pass();  // still inside the 250 ms window
    CHECK(d.dropped == 0);
    while (d.dropped == 0 && d.nowMs < 1000) d.pass();
    CHECK(d.dropped == 1);
    CHECK(!d.r.pending());
    d.compose();  // the incoming screen paints
    CHECK(d.r.composesThatSawPins == 0);
  }
  // (b) the same skip, but the incoming screen composes before the next pass. That
  //     compose is the NEXT one, so it is the pair's one legitimate consumer -- the
  //     bound, stated as a count -- and nothing survives it.
  {
    Device d;
    d.armIcon();
    d.pass();
    d.compose();
    CHECK(d.r.composesThatSawPins == 1);
    d.pass();
    CHECK(d.leftovers == 0);  // the compose consumed it; the tracker had nothing to drop
    CHECK(!d.r.pending());
    d.nowMs += kComposePeriodMs;
    d.compose();
    CHECK(d.r.composesThatSawPins == 1);
    CHECK(!d.scheduleImpossible);
  }
}

void testTwoArmersOneCompose() {
  section("two armers in one pass: one compose carries both, no double drive");
  Device d;
  d.armPercent();
  d.armIcon();
  CHECK(d.requests == 2);
  CHECK(d.r.pending());
  d.pass();
  CHECK(d.pins.armed());  // still one intent, waiting for the compose
  d.compose();
  CHECK(d.r.composesThatSawPins == 1);  // ONE drive, not two
  CHECK(!d.r.pending());
  d.pass();
  CHECK(d.consumed == 1);
  CHECK(d.leftovers == 0);
  CHECK(!d.pins.armed());
  d.waitForNextCompose();
  d.compose();
  CHECK(d.r.composesThatSawPins == 1);
  CHECK(!d.scheduleImpossible);
}

void testEachRequestIsItsOwnCompose() {
  section("two requests are two pinned composes, never a doubled one");
  Device d;
  d.armPercent();
  d.compose();
  d.pass();
  d.waitForNextCompose();
  d.armPercent();
  d.compose();
  d.pass();
  CHECK(d.r.composesThatSawPins == 2);
  CHECK(d.consumed == 2);
  CHECK(d.leftovers == 0);
  CHECK(!d.scheduleImpossible);
}

// A pair armed and consumed INSIDE a paint (InPlaceValueStep, FrontlightPanel and the
// reader's own plan) is synchronous with its own drive and never belongs to this
// tracker. The reconcile must not touch it.
void testPaintLocalPinsAreNeverCleared() {
  section("pins armed and consumed inside a paint are not this tracker's");
  Device d;
  d.r.armPins();          // a paint arms them...
  d.compose();            // ...and drives in the same call
  CHECK(d.r.composesThatSawPins == 1);
  d.pass();
  CHECK(d.leftovers == 0);
  CHECK(!d.r.pending());
}

// THE BOUND, measured: one compose observes an armed pair, wherever the compose is
// placed among the passes the schedule allows.
void testAtMostOneComposeObservesThePair() {
  section("at most ONE compose observes an armed pair, under every schedule");
  int worst = 0;
  for (int composeAtPass = 0; composeAtPass <= 8; ++composeAtPass) {
    for (int held = 0; held <= 1; ++held) {
      Device d;
      d.armPercent();
      int seen = 0;
      for (int pass = 0; pass <= 8; ++pass) {
        d.pass();
        if (pass == composeAtPass) {
          const int before = d.r.composesThatSawPins;
          d.compose(held != 0);
          seen += d.r.composesThatSawPins - before;
          if (held != 0) {  // its payback compose, a full drive later
            d.nowMs += kComposePeriodMs;
            const int b2 = d.r.composesThatSawPins;
            d.compose();
            seen += d.r.composesThatSawPins - b2;
          }
        }
      }
      CHECK(!d.scheduleImpossible);
      if (seen > worst) worst = seen;
    }
  }
  std::printf("   measured: worst case %d compose(s) observed the pair\n", worst);
  CHECK(worst == 1);
  std::printf("   measured: max composes that can see a pin = %d\n", worst);
}

}  // namespace

int main() {
  testDeferredComposeStillGetsThePins();
  testAbsentPaintIsDropped();
  testRefusedComposeLeavesNothingBehind();
  testSkippedIconPaint();
  testTwoArmersOneCompose();
  testEachRequestIsItsOwnCompose();
  testPaintLocalPinsAreNeverCleared();
  testAtMostOneComposeObservesThePair();

  std::printf("\n%d/%d checks passed", checksRun - checksFailed, checksRun);
  if (checksFailed != 0) std::printf("  (%d FAILED)", checksFailed);
  std::printf("\n");
  return checksFailed == 0 ? 0 : 1;
}
