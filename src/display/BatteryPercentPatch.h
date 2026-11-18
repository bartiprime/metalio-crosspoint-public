#pragma once

#include <cstdint>

#include "display/BatteryIconRegion.h"  // CpBatteryIconResult (shared result enum)

class GfxRenderer;

// ---------------------------------------------------------------------------
// BATT6 — the percentage NUMBER's change producer.
//
// THE HOLE THIS FILLS. The battery glyph repaints itself on the charging edge
// (display/BatteryIconPatch.cpp), and the number had no producer at all: every
// theme reads powerManager.getBatteryPercentage() at PAINT time, so the digits
// moved only when something else repainted the screen. On the glass that reads as
// a percentage frozen at whatever the last paint saw -- measured: 100 % after a
// 7.5 h standby with the pack at ~90 %, refreshed by leaving the screen and
// coming back.
//
// WHAT IT DOES, AND WHAT IT DELIBERATELY DOES NOT. It watches the gauge's cached
// percentage and, when the value the glass is showing differs from it, repaints
// the number's OWN box in place -- erase, redraw through the same primitives the
// paint used, one diff-masked DU drive. It does not clean, it does not re-compose
// the screen, and on a steady percentage it does exactly nothing (the comparison
// is an edge against the value the DROVE paint recorded, never a timer).
//
// THE SAME LICENCE AS THE GLYPH. A region patch is allowed only while the canvas
// IS the frame on the glass (lib/GfxRenderer/GlassEpoch.h: `composed == glass`) and
// only for a box recorded at that epoch, checked again inside the RenderLock. That
// is what makes the blank-frame failure of the retired r98 path structurally
// impossible here rather than merely unlikely.
//
// WHY THE GLYPH IS REPAINTED TOO. The fill level is the same percentage as the
// digits. Repainting only the number would leave a 100 % bar beside "90 %", so when
// the recorded glyph box belongs to the same frame it is erased, redrawn with the
// live percentage and charging state, and driven in the SAME pass: one drive, both
// elements, still only the union of their boxes in the diff mask.
//
// WHICH SURFACES ARE COVERED, AND HOW.
//
// IN PLACE (erase, redraw through the same primitives, one diff-masked DU):
//   * BaseTheme::drawBatteryLeft   -- the reader's status line, and every theme
//                                     status bar that calls it (BaseTheme:1010);
//   * AndroTheme                   -- its status band (:583) and status bar (:902);
//   * PickyOSTheme                 -- its status band (:261).
// These all draw the number through BaseTheme::drawBatteryPercentLabel(), which
// records the box it painted into.
//
// BY A DEFERRED WHOLE-ACTIVITY REPAINT (still diff-masked DU, one request per
// changed value, consumed at the main loop's own boundary):
//   * the FreeInkUI header band (components/TitledHeaderBand.h) -- Home, the
//     library, settings, the catalog family. That number is drawn INSIDE the SDK's
//     battery component, so this path has no box for it and re-deriving the
//     component's reserve arithmetic here would be a second layout that can drift.
//     The band therefore records the VALUE it painted, and a change ARMS one
//     request (cpBatteryPercentTakeDeferred(), polled once per loop pass) which
//     main.cpp pins to DU (forceBinaryDuOnce + suppressCompletionDriveOnce), so the
//     panel still drives only the pixels that differ. At most ONE request per
//     (surface, value) and one per 10 s per surface, and never while a clean or a
//     transition is in flight -- the r165.1 producer asked on every pass and ate
//     the return-to-Home clean.
//
// A screen that draws no percentage (a full-screen image, the sleep chip, the
// number hidden in Settings) records nothing and therefore costs nothing.
// ---------------------------------------------------------------------------
CpBatteryIconResult cpBatteryPercentPatchOnChange(GfxRenderer& renderer);

// True exactly once for each armed request, and only when called from the main
// loop (never from inside a paint). Clears the flag.
bool cpBatteryPercentTakeDeferred();
