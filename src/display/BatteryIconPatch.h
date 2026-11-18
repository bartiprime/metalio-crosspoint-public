#pragma once

#include <cstdint>

#include "display/BatteryIconRegion.h"  // CpBatteryIconResult (shared result enum)

class GfxRenderer;

// ---------------------------------------------------------------------------
// CP_ICON_SAFE_PATCH — the GUARDED replacement for CP_ICON_PARTIAL_REPAINT.
//
// The rect bookkeeping does NOT live here: it lives on the renderer
// (GfxRenderer::BatteryIconRect) next to the compose/glass epochs, so the rect and
// the fact that licenses using it are recorded by the same object at the same
// instant. See lib/GfxRenderer/GlassEpoch.h for the proof and
// src/display/RefreshPolicy.h for the switch.
//
// ONE entry point, called from main.cpp's loop after the power/sleep guards:
//
//   Idle              no charging edge
//   Drove             the icon's rectangle was patched and driven as a diff-masked
//                     DU; the glass now shows the new state
//   Pending           the edge is owed but a guard or an epoch mismatch is holding
//                     it. Retried next loop iteration -- the caller must do nothing.
//   NeedsFullRepaint  this screen has no addressable rect. The edge has been
//                     SETTLED (the glass is believed to show the new state, because
//                     nothing this subsystem can draw is stale) and the caller may
//                     request one whole-activity repaint.
//
// The reader is NOT excluded: when the epochs are equal a rect patch is safe there
// and costs no page re-render. What the reader must not get is the
// NeedsFullRepaint fallback -- that IS a page re-render -- so main.cpp declines it
// on a reader and leaves the edge to the next page paint, which draws the icon
// with the live state anyway.
CpBatteryIconResult cpBatteryIconPatchOnEdge(GfxRenderer& renderer);

// BATT6: the percentage producer (display/BatteryPercentPatch.cpp) repaints the
// glyph too when it redraws the number -- the fill level IS that percentage -- so
// it tells this subsystem what it put on the glass. Without this the glyph's own
// producer would still believe the glass shows the previous charging state and
// spend a second drive on an edge that already landed.
//
// ONLY the state the glass is believed to show changes here: no drive, no request,
// no counter that says an edge was seen.
void cpBatteryIconPatchNoteExternalDraw(bool charging);
