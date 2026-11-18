// r165.6 FAILING FIRST — the r165.5 one-shot pins, measured against the ASYNC ordering.
//
// This probe compiles the PRE-r165.6 header (extracted from git by failing_first_asynch.sh,
// src/display/OneShotPaintPins.h at 4cafdc3f) and drives it through the same race-controlled
// fake the new harness uses: the arm+request happen on the main task, the paint happens on the
// render task zero or more loop passes later.
//
// It asserts the DEFECT, and exits 1 when it is present -- the shape
// test/batt_percent_change/failing_first.sh already uses.
//
//   A. arm, then 5 loop passes with no compose, then the compose (the deferred repaint the
//      request earned): r165.5's boundary clear ran at the first of those passes, so the
//      compose that finally ran saw NO pins -- the exact "deferred repaint runs unpinned"
//      defect. Expected on r165.5: 0. Wanted: 1.
//   B. BATT5's armer was untracked: a skipped paint left the pair set and the next screen's
//      compose inherited it (DU with no completion drive, on a screen that never asked).
//      Expected on r165.5: 1 stray pinned compose. Wanted: 0.
#include <cstdint>
#include <cstdio>

#include "display/OneShotPaintPins.h"

namespace {

constexpr uint32_t kLoopSliceMs = 20;

struct FakeRenderer {
  bool du = false;
  bool sup = false;
  uint32_t composed = 0;
  int composesThatSawPins = 0;

  void armPins() {
    du = true;
    sup = true;
  }
  bool pending() const { return du || sup; }
  void clearPins() {
    du = false;
    sup = false;
  }
  void compose() {
    ++composed;
    if (pending()) ++composesThatSawPins;
    clearPins();
  }
};

// The r165.5 loop, verbatim in behaviour: the arming pass, then a boundary clear at the top of
// EVERY following pass (main.cpp:5746 as it shipped).
struct OldDevice {
  FakeRenderer r;
  oneshotpins::PassBoundPins pins;
  uint32_t nowMs = 0;

  void armAndRequest() {
    pins.arm();
    r.armPins();
  }
  void pass() {
    nowMs += kLoopSliceMs;
    if (pins.expire(r.pending())) r.clearPins();
  }
};

}  // namespace

int main() {
  std::printf("r165.5 one-shot pins under the async ordering\n");

  int defects = 0;

  // A: the deferred repaint.
  {
    OldDevice d;
    d.armAndRequest();
    for (int i = 0; i < 5; ++i) d.pass();  // the render task has not composed yet
    d.r.compose();                         // ...and now it does
    std::printf("  A deferred compose saw pins: %d  (defect wants 0 -- the pair was cleared "
                "before the paint)\n",
                d.r.composesThatSawPins);
    if (d.r.composesThatSawPins == 0) ++defects;
  }

  // B: BATT5's untracked armer, with the requested paint skipped.
  {
    OldDevice d;
    d.r.armPins();  // the icon armer: pins + requestUpdate(), no tracker at all
    d.pass();       // skipped paint, and the tracked boundary knows nothing about this pair
    d.r.compose();  // the next screen paints, on a pair nobody asked it to spend
    std::printf("  B a skipped icon paint leaked into the next compose: %d stray pinned "
                "compose(s)  (defect wants 1)\n",
                d.r.composesThatSawPins);
    if (d.r.composesThatSawPins == 1) ++defects;
  }

  std::printf("defects reproduced: %d/2\n", defects);
  return defects == 2 ? 1 : 0;  // 1 == the defect is present, which is what the script asserts
}
