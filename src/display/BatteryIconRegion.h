#pragma once

#include <cstdint>

class GfxRenderer;

// ---------------------------------------------------------------------------
// CP_ICON_PARTIAL_REPAINT — the battery charging glyph repaints itself.
//
// WHAT THIS FIXES (r98): after the r97.1 CX25601N REG 0x1E fix the charging
// bolt is CORRECT, but only appears once the operator navigates away and back.
// The state was right; nothing asked the UI to repaint when it changed. The
// existing producer (main.cpp, `gpio.wasUsbStateChanged()`) is a whole-activity
// repaint -- a full page re-render on the reader -- and it is deliberately
// disabled there, so on the reader the flag was never picked up at all.
//
// HOW THE REGION IS LIMITED: this tree has no sub-area drive. epd_draw_base()
// asserts `area.width == display_width` and every caller drives
// epd_full_screen() with a ROW/COLUMN MASK computed from the diff between the
// framebuffer and the frame the glass holds (highlevel.c:
// epd_difference_image_cropped -> dirty_lines/dirty_columns -> the `[mask]`
// line in render.c). So the smallest drive this renderer supports is "the
// pixels that changed, and nothing else": touch only the icon's rectangle in
// the framebuffer and the mask itself limits the waveform to those rows and
// those 4-pixel column bytes. No new waveform logic, no new epdiy entry point,
// and the unchanged rows are fed zeroed line buffers (render_lcd.c:379-392),
// i.e. no voltage.
//
// WHAT IT DELIBERATELY DOES NOT DO: no from-white clean (FULL_REFRESH), no
// whole-panel GC16, no activity re-render, no change to the read position, the
// entry gate or the band behaviour. If any of those one-shots is armed, or a
// paint is in flight, the repaint is skipped instead of consuming it.
//
// SEE src/display/BatteryIconRegion.h (this file) for the call sites:
//   - BaseTheme::drawBatteryOutline()  records the rect it just painted
//   - ActivityManager::renderTaskLoop() clears the record per paint
//   - main.cpp loop()                  edge-triggers the repaint
// ---------------------------------------------------------------------------

// The render task calls this at the START of every activity paint. A rect only
// counts while it came from the paint that produced what is on the glass now, so
// a screen that does not draw the icon (a menu, a full-screen image, the sleep
// chip) invalidates the record by simply not setting it again. That is the whole
// "is the icon visible?" test, and it costs one store per paint.
void cpBatteryIconBeginPaint();

// BATT2: called by the render task at the END of every activity paint, with the
// answer it already computed for "did this paint reach the glass?" (ActivityManager
// knows it: `drove = !held && !declined`).
//
// WHY THIS EXISTS. cpBatteryIconNotePaint() runs while the frame is COMPOSED, which
// is not the same as the frame being DRIVEN: the entry gate can withhold the first
// drive of a screen (`held=1`) and an activity can decline its own frame
// (`declined=1`). Committing "the glass now shows charging=X" at compose time
// therefore let a paint that never reached the panel consume the edge -- the icon
// stayed stale AND the edge was gone, with no log line to say so. The claim is now
// only committed when the paint actually drove.
void cpBatteryIconEndPaint(bool drove);

// Recorded by BaseTheme::drawBatteryOutline() -- the ONE primitive every theme
// funnels the glyph through (BaseTheme::drawBatteryLeft, AndroTheme's status bar
// and header, Lyra's). `x,y` is the outline's origin and `w,h` its body; the
// terminal nub is drawn at x + w, so the glyph's ink is (w + 1) x h.
void cpBatteryIconNotePaint(int x, int y, int width, int height);

// What one call to cpBatteryIconPollAndRepaint() did. main.cpp acts on
// NeedsFullRepaint and ignores the rest, so a screen the partial path cannot
// address still gets the legacy whole-activity repaint instead of nothing.
enum class CpBatteryIconResult : uint8_t {
  // No edge this call: steady state, or the baseline was just taken.
  Idle = 0,
  // The icon's own rectangle was repainted in DU. The glass is up to date.
  Drove,
  // The edge is real and still owed: a guard (one-shot armed / entry gate / a
  // paint in flight) is holding it. Retried next loop iteration, so the caller
  // must NOT repaint anything.
  Pending,
  // The edge is real, the screen shows a battery, but this path cannot address
  // it (no rect was recorded by the last paint, or the rect was unusable). The
  // edge has been settled here because no retry can help: the ONLY producer
  // left is a whole-activity repaint, which the caller must request.
  NeedsFullRepaint,
};

// True when the last paint of the visible screen drew the icon. main.cpp uses it
// to decide between the partial repaint and the legacy whole-activity repaint.
bool cpBatteryIconVisibleOnScreen();

// Edge-triggered on powerManager.isCharging(): drives only the icon's rectangle
// when the flag changes and the icon is on screen. Returns true when a drive was
// issued. Cheap to call every main-loop iteration (the state is a cached read,
// refreshed at most once per BATTERY_POLL_MS, so this can never fight the 1.5 s
// poll -- it sees whatever the poll last stored and repaints at most twice per
// plug/unplug). Must be called from the main loop task, never from the render
// task, and never while the device is on its way to sleep.
//
// BATT1R: when a guard skips the paint (a one-shot refresh armed, the entry gate
// armed, a paint in flight) the edge stays PENDING and is retried next iteration --
// it is settled by the repaint itself or by any paint that both DRAWS and DRIVES the
// icon (cpBatteryIconEndPaint). Two cases settle immediately instead: no icon on the
// visible screen, and an unusable recorded rect. Consuming the edge before the guards
// is what left the icon showing the old charging state until an unrelated screen
// change happened to repaint it.
//
// BATT3: this is called from main.cpp loop(), which used to gate it on
// `deepSleepInProgress` -- a flag `enterDeepSleep()` sets on the way to standby and
// that the in-place Metalio resume never cleared, so the whole poll was dead after the
// device's first sleep. If the icon goes stale again, grep `[BATT] icon` first: either
// the edge line is missing (the poll is not running) or it is followed by a skip.
//
// BATT4: the return value says what happened, and NeedsFullRepaint is the answer for
// "the screen shows a battery this path cannot address". Before that, main.cpp excluded
// the reader from the whole-activity fallback AND this function settled the edge when no
// rect existed -- so on such a screen there was no producer at all and the icon could
// never move. main.cpp now requests an activity repaint for NeedsFullRepaint, on any
// screen (that is r97's behaviour, restored for exactly this case).
//
// BATT4 also emits an UNCONDITIONAL heartbeat, at most once per 5 s, so "is the poll
// running and what does it believe" is answerable while the debug cable (= the charger)
// is attached and no charging edge can ever fire: `[BATT] icon hb ...`.
CpBatteryIconResult cpBatteryIconPollAndRepaint(GfxRenderer& renderer);
