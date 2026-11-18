#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>

// ---------------------------------------------------------------------------
// SLIDE1 — ONE in-place value step is ONE diff-masked DU, everywhere.
//
// THE BUG (operator report)
//   The frontlight panel's brightness/warmth drag repaints smoothly; every
//   Settings control that steps a value -- the Time to sleep / Power off while
//   asleep slider dialog, and any list row whose activation steps a number --
//   cleaned the whole panel once per step. The dialogs and the list each
//   already asked for FAST, so the flash was not their requested mode. Two
//   terms BELOW the request turned one step into a whole-panel drive, and they
//   are exactly the two the frontlight panel had to defeat in FLP1/FLP2
//   (src/activities/util/FrontlightPanelRefresh.h):
//     1. The row-9 completion (CP_RICKY_COMPLETION_DRIVE, GfxRenderer.cpp:2752):
//        every FAST paint is finished by a SECOND whole-panel GL16 drive. That
//        second drive is a flash by construction, and it is what a step showed.
//     2. The vendor shim's waveform choice (idf/components/metalio_driver/
//        metalio_vendor_panel.cpp): `useGray8 = !clean && src4bpp &&
//        frameHasGrey` sends a frame carrying ANY grey -- a dialog's dimmed
//        backdrop, the reader page under a percent dialog -- through the
//        38-phase grey-wipe table over every pixel; the 24-partial clean
//        cadence (:719-726) promotes any FAST paint to a from-white GC16.
//        A list's request is also downgraded to HALF by cp_refresh_resolve_mode
//        rule 3 (RefreshPolicy.h) when the reader underneath left the latch at
//        FULL.
//
// WHAT THIS HEADER IS
//   The port of FLP1/FLP2's Adjust arm, not a new policy: the same three
//   one-shots the frontlight panel arms for its own adjustment, reachable from
//   any surface that steps a value in place. Each is consumed by the next paint
//   that reaches the drive decision (below the entry gate), so a surface must
//   arm it in its own RENDER path, immediately before the displayBuffer() it
//   wants to be the step -- never from an input handler that may not paint.
//
// Nothing here changes what is stored, the range, or when it is saved.
// ---------------------------------------------------------------------------

namespace inplacestep {

// Self-owned mode (a popup dialog that already calls displayBuffer(FAST) or
// displayBuffer() itself, but does not pin the result): all three terms.
//   * promoteNextRefresh(FAST) -- an explicit FAST outranks REFRESH_AUTO's
//     measured ratio and the inherited latch rule above.
//   * suppressCompletionDriveOnce() -- no row-9 GL16 second drive for this
//     paint.
//   * forceBinaryDuOnce() -- pins the resolved mode to DU after the policy and
//     tells the shim this paint is binary, so neither the grey-wipe lane nor
//     the clean cadence can take it.
inline void armOwnedPaint(GfxRenderer& renderer) {
  renderer.promoteNextRefresh(HalDisplay::FAST_REFRESH);
  renderer.suppressCompletionDriveOnce();
  renderer.forceBinaryDuOnce();
}

// List-owned mode (UiListActivity::render): the list cancels promotions and
// pins its own paint (HALF on an entry/page turn, FAST otherwise), so a
// promotion here would be dropped, and arming one on a paint that turns out to
// be HALF or skipped would leak it onto an unrelated drive. The two terms the
// list cannot say for itself are the ones below the policy stage -- no
// completion, and the binary/DU lane -- and the caller arms them only on the
// FAST paints it actually commits.
inline void armListPaint(GfxRenderer& renderer) {
  renderer.suppressCompletionDriveOnce();
  renderer.forceBinaryDuOnce();
}

}  // namespace inplacestep
