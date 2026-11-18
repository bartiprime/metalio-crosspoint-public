#pragma once

#include <BoardConfig.h>
#include <HalDisplay.h>
#include <Logging.h>

#include "CrossPointSettings.h"

// EQ1 (r101.9) -- the operator's global MODE_EQUALIZE_BLACK toggle.
//
// WHAT IT DOES. Pass a refresh mode through this helper instead of calling
// renderer.displayBuffer(mode) directly, and the mode comes back as
// HalDisplay::EQUALIZE_REFRESH when -- and only when -- all three of these hold:
//   1. SETTINGS.equalizeBlack is 1 (default 0 = OFF);
//   2. the caller asked for HALF_REFRESH, i.e. the diff-area GC16 paint;
//   3. this board is the ED047TC2, the only controller whose driver implements the mode
//      (FreeInkDisplay.cpp:85-91 -- every other driver sees an unknown mode and falls into
//      its else branch, i.e. it would be demoted to FAST. Declining here keeps a HALF a HALF).
//
// WHY IT IS FREE. EQUALIZE_REFRESH is the same 48-frame GC16 waveform table as HALF:
// metalio_vendor_panel.cpp:826 drives it through epd_hl_update_screen() with
// MODE_GC16 | MODE_EQUALIZE_BLACK, and render.c takes the frame count from the waveform
// table at :178 BEFORE find_lut_functions() picks a LUT builder at :213. The flag changes
// only which builder is picked (lut.c:616 / :625 -> build_equalize_black_*), never
// frame_count. Nor does it cost scanning time: a masked-out row is still zero-filled and
// committed to the line queue (output_lcd/render_lcd.c:380-405), so the LCD engine clocks
// the same number of frames either way.
//
// WHAT IT IS NOT. Not an erase: pixels whose target is white keep their normal transition,
// so the frame on the glass is still the frame in the buffer. No repaint is needed after it
// and no black frame is ever shown.
//
// THE ONE BEHAVIOURAL DIFFERENCE beyond the LUT, and it is why this is scoped to HALF: the
// panel shim promotes a HALF to a from-white GC16 when the frame carries >= 5 optical levels
// (metalio_vendor_panel.cpp:678-680, forceFromWhite). EQUALIZE is not in that gc16 class, so
// such a frame stays a diff-area drive with the equalize LUT instead of going from white --
// same 48 frames, no blackout. That is the frame a popup wants anyway; a caller that NEEDS the
// from-white clean must ask for FULL, and this helper never touches FULL.
//
// OFF PATH. When the setting is 0 this returns `mode` unchanged on the first line, so the
// value handed to displayBuffer() is bit-for-bit what the call site passed before this
// header existed -- MODE_EQUALIZE_BLACK is never OR-ed in anywhere outside the driver.

inline HalDisplay::RefreshMode equalizeBlackMode(const HalDisplay::RefreshMode mode, const char* origin) {
  if (SETTINGS.equalizeBlack == 0) return mode;
  if (mode != HalDisplay::HALF_REFRESH) return mode;
  if (BoardConfig::ACTIVE.displayController != BoardConfig::DisplayController::Ed047Tc2) return mode;
  // Once per paint: this helper is called once per drive, never per row.
  LOG_INF("DISP", "equalize-black: %s HALF -> EQUALIZE (whole-panel black re-ink, same 48-frame GC16)",
          origin != nullptr ? origin : "paint");
  return HalDisplay::EQUALIZE_REFRESH;
}
