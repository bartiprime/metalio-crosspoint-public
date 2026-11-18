#pragma once

// ---------------------------------------------------------------------------
// FLP1 (r101.21b) — the frontlight panel's repaint decision table, pure and
// host-testable.
//
// THE BUG (operator report)
//   In the swipe-down frontlight panel, every brightness/warmth adjustment
//   flashed the whole panel. Two independent causes, both below the activity:
//     1. A drag's `requestUpdate()` runs the ordinary in-screen policy: with the
//        shipped REFRESH_AUTO default the measured ratio can resolve to HALF
//        (GC16 over the diff -- a grey flash over the row), and any user who
//        picked REFRESH_FULL/GL16 for menus gets exactly what they asked for on
//        a slider (full-panel repaints per percent).
//     2. Even when it resolved to FAST, `CP_RICKY_COMPLETION_DRIVE` appended the
//        row-9 GL16 completion drive: a SECOND whole-panel waveform per
//        adjustment. One fast paint was two drives, and the second one is a
//        flash by construction (GfxRenderer.cpp, row 9).
//   So "ask for FAST" alone is not the fix; the completion has to go with it.
//
// WHAT THIS TABLE ENCODES
//   Adjust    -> ONE explicit FAST/DU drive (the promotion outranks the setting
//                and the AUTO ratio) and the row-9 completion suppressed. No
//                clean waveform of any kind.
//   Exit      -> ONE from-white FULL for the repaint of the screen underneath
//                (armed as a promotion while the panel closes, under the render
//                lock). A promotion is a FLAG, not a counter, so re-entering the
//                arm on a second close() cannot produce a second FULL: that is
//                the whole "exactly once per exit" property.
//   Enter     -> untouched: a bare requestUpdate(), i.e. the activity-transition
//                policy decides the opening paint.
//   NightTile -> untouched: HALF for its own reason (it rewrote every pixel).
//
// The arm (`exparms::panelPartialRefresh()`, `exp flpanel`) selects between this
// table and the pre-fix behaviour; `arm == false` returns the bare
// requestUpdate() shape for every event, i.e. exactly what r101.20 did.
//
// This header has no firmware dependency on purpose: test/frontlight_panel/
// compiles it against the real src/display/RefreshPolicy.h so "adjust resolves
// to FAST" and "exit resolves to FULL" are proven through the SAME two-stage
// producer + policy chain the renderer runs (GfxRenderer::resolveRefreshRequest
// then cp_refresh_resolve_mode), not asserted about a copy of it.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace flpanel {

enum class Event : uint8_t {
  Adjust,       // brightness/warmth drag OR +/- key step: the content changed
  DragRelease,  // the finger lifted, nothing left to draw
  Enter,        // the panel is opening
  Exit,         // the panel is closing (back, home gesture, swipe up, tap below)
  NightTile,    // the inversion tile rewrote every pixel
};

struct Plan {
  bool requestPaint = false;        // ask the manager for a repaint
  bool promoteFast = false;         // arm an explicit FAST promotion for that paint
  bool suppressCompletion = false;  // suppress the row-9 GL16 completion for it
  bool halfClean = false;           // render() takes HALF (cleanRefreshPending)
  bool cleanOnExit = false;         // arm ONE from-white FULL for the pop's repaint
  // FLP2: "this paint is binary content and MUST leave as a diff-masked DU".
  // Two things below FLP1 were still able to turn one adjustment into a
  // whole-panel drive, and neither is the call site's mode:
  //   * the frame's content. The panel never clears the framebuffer
  //     (FrontlightPanelActivity.cpp render() -> renderUi()), so the frame it
  //     drives is the screen underneath plus a card, and the vendor shim picks
  //     its waveform from that frame: metalio_vendor_panel.cpp:644
  //     `useGray8 = !clean && src4bpp && frameHasGrey` -> the 38-phase
  //     erase-then-grey table driven over EVERY pixel (:859, _full clears
  //     MODE_SKIP_UNCHANGED). The panel's own chrome is 1-bit, so this term
  //     says: for THIS paint, take the DU lane whatever the frame contains.
  //   * the inherited mode. Two ambient terms can still escalate a paint the
  //     panel asked to be FAST: cp_refresh_resolve_mode()'s rule 3 turns a
  //     FAST request into HALF when the reader that was underneath left the
  //     latch at FULL (RefreshPolicy.h:415-417), and the shim's 24-partial
  //     clean cadence (:719-726) promotes any FAST paint to a from-white GC16
  //     clean. `binaryFast` is therefore consumed in GfxRenderer.cpp BELOW the
  //     entry gate as a one-shot that (a) pins the resolved mode to FAST and
  //     (b) arms the shim's own binary flag for that one drive.
  bool binaryFast = false;
};

constexpr Plan plan(const Event e, const bool arm) {
  Plan p;
  switch (e) {
    case Event::Adjust:
      // arm: the panel owns its own repaint mode instead of inheriting the
      // global menu/transition setting, and it is one binary drive.
      p.promoteFast = arm;
      p.suppressCompletion = arm;
      p.binaryFast = arm;
      p.requestPaint = true;
      return p;

    case Event::DragRelease:
      // Today a lift arms a HALF "clean" over the diff. The exit clean replaces
      // it, so with the arm on the lift drives nothing at all.
      p.halfClean = !arm;
      p.requestPaint = !arm;
      return p;

    case Event::Exit:
      // The pop queues the repaint itself; the clean is a promotion armed on it.
      p.cleanOnExit = arm;
      return p;

    case Event::NightTile:
      // UNCHANGED. The tile inverts every pixel; HALF is what flips the frame
      // without the FULL blackout, and the exit clean is not involved.
      p.halfClean = true;
      p.requestPaint = true;
      return p;

    case Event::Enter:
      // UNCHANGED. No promotion: the activity-transition policy decides.
      p.requestPaint = true;
      return p;
  }
  return p;
}

// --- FLP2: the two terms the panel's forced-binary paint must defeat ---------
//
// Pure mirrors of the two layers below FLP1, so the host test can state the
// claim without a copy drifting silently. Each cites the line it mirrors.

// The renderer's pin, applied AFTER cp_refresh_resolve_mode() in
// GfxRenderer::displayBuffer() (mirrors `if (binaryDuForced) refreshMode =
// FAST_REFRESH;`). `resolved` is the policy's output: on a FAST request it can
// be HALF because the reader underneath left the latch at FULL (rule 3) or
// because the panel was never painted (rule 1).
constexpr int forcedDriveMode(int resolved, bool binaryForced) {
  return binaryForced ? 2 /* CP_RM_FAST */ : resolved;
}

// Whether the vendor shim would take the grey lane for this paint. Mirrors
// metalio_vendor_panel.cpp:644 + :728 + :859:
//   useGray8  = !clean && src4bpp && frameHasGrey
//   greyDrive = useGray8 && !cadenceDue
// `clean` is false for a FAST request, `src4bpp` is true on this build, so a
// forced-binary paint is the only thing that can keep a grey frame on DU.
constexpr bool shimTakesGreyWipe(bool fastRequest, bool frameHasGrey, bool binaryForced) {
  return fastRequest && frameHasGrey && !binaryForced;
}

// The shim's re-condition cadence (metalio_vendor_panel.cpp:719-726): every 24
// partial drives with >= 60 s since the last clean, ANY FAST paint becomes a
// from-white GC16 whole-panel clean. It is ambient state accumulated by the
// reader's earlier paints, not by the panel, which is why a forced-binary paint
// skips it too.
constexpr bool shimTakesCadenceClean(bool fastRequest, bool binaryForced, bool cadenceDue) {
  return fastRequest && cadenceDue && !binaryForced;
}

// How many from-white FULLs one exit can put on the glass. A promotion is a
// single flag consumed by the next paint, so any number of close() calls before
// that paint still yields ONE drive. Returned as a constant here so the test can
// state the invariant without re-implementing the flag.
constexpr int fromWhiteDrivesPerExit(const bool arm) { return arm ? 1 : 0; }

}  // namespace flpanel
