#pragma once

#include <cstddef>
#include <cstdint>

// INVARIANT: this include must stay ABOVE the `#if CP_CPU_STATS` below, never inside it.
// CP_CPU_STATS (like every other CP_ switch) is DEFINED by PlatformFeatures.h; inside the
// guard the macro would be seen before that header defines it, an undefined identifier in
// `#if` evaluates to 0, and every hook a translation unit reaches through this header
// would compile as a silent no-op instead of the empty inline it is meant to be. A TU that
// tests a switch must also include PlatformFeatures.h itself, so it does not depend on
// this include order at all (GfxRenderer.cpp, main.cpp and ActivityManager.cpp all do).
#include "PlatformFeatures.h"

// ---------------------------------------------------------------------------
// CP_CPU_STATS — make the machine's own CPU behaviour observable.
//
// WHY THIS EXISTS (docs/engineering/LPCORE_PPA_R96_PLAN.md §1.8, §4.1)
//   Until r96 the tree had no FreeRTOS run-time statistics compiled in at all,
//   so "is core 0 or core 1 idle?" could only be answered by reading the
//   source. This file is the runtime readout for that question, plus the
//   breakdown of what a page render actually spends its time on:
//
//     cpu            per-task run time, idle% per core, actual CPU MHz
//     pm             power state, current MHz, PM locks, light-sleep time
//     bench render   the COMPOSE-vs-DRIVE split of the last page render, with
//                    the compose half split into GLYPH / IMAGE / other
//
// THE QUESTION `bench render` ANSWERS
//   A page turn reported on glass is ~1 s. The panel drive is ~514 ms of that
//   (engineering/EPD_STATIC_IDLE_ARTIFACT.md:533) and is pure physics; the rest
//   is composition — laying out text and blitting glyphs into the 1 bpp
//   framebuffer. Whether that composition is even CPU-bound decides whether any
//   accelerator can help, so it is measured before anything is offloaded.
//
// WHAT IS MEASURED, AND WHERE (no per-pixel instrumentation anywhere)
//   * compose   = time in Activity::render() on the render task, minus the drive
//                 (ActivityManager::renderTaskLoop, src/activities/ActivityManager.cpp)
//   * drive     = time inside GfxRenderer::displayBuffer() — the existing call
//                 boundary, measured from the outside, not from inside epdiy
//   * glyph     = time in the glyph blit path only: GfxRenderer::drawGlyphBitmap()
//                 plus the scaled-glyph pixel loop in renderCharScaled(). One
//                 timer pair per glyph, never per pixel.
//   * image     = time in the 1 bpp / 2 bpp Bitmap blit path only
//                 (GfxRenderer::drawBitmap1Bit() and the non-1-bit body of
//                 drawBitmap()). The JPEG/PNG decode that produces those bitmaps
//                 happens BEFORE the blit and is counted in "layout/other".
//   * layout    = compose − glyph − image. Text measurement, line breaking,
//                 fills/lines/rules, and anything else that is not one of the
//                 three instrumented paths.
//
// SWITCH: CP_CPU_STATS (src/platform/PlatformFeatures.h). With it at 0 every
// hook below is an empty inline, the call sites compile away, and no FreeRTOS
// stats clock exists — the readout commands then report "not built".
// ---------------------------------------------------------------------------
namespace cpustats {

// One page render as the render task measured it.
//
// r97 PHASE 2 — THE LAYOUT SPLIT. `otherUs` used to be one opaque bucket
// (compose - glyph - image = 245.8 ms on a real page turn), which decided
// nothing: a pipelining change is only worth building if that time is real WORK
// in a phase that can be moved off the paint path. The fields below split it at
// call boundaries into the phases a page turn actually has:
//
//   (a) pageLoadUs  Section::loadPage() + Page::deserialize()   — SD read + parse
//   (b) measureUs   the font measure/prewarm scan (createPrewarmScope ..
//                   endScanAndPrewarm, MINUS the status-bar paint inside it)
//   (c) statusUs    every renderStatusBar() call in the window
//   (d) clipUs      drawClippingHighlights()
//   (e) glyphUs / imageUs   the existing blit buckets, unchanged
//   (f) otherUs     compose - (a..e + image): clearScreen, saveProgress, the
//                   overlay snapshot, and whatever is not one of the above
//
// HONEST BOUNDARY: line breaking and justification are NOT in this window.
// extractLine() runs when the section is BUILT (Section::buildSomeMore); by the
// time a page is rendered, TextBlock already carries xposArr and the line's
// x/y, and TextBlock::render only walks them. So (b) is the measure/prewarm
// scan — the only layout-shaped work left in compose — and the line-break cost
// is a CHAPTER BUILD cost, measured by the build's own timer, not here.
struct RenderBreakdown {
  uint32_t seq = 0;         // 1-based render counter since boot
  int64_t totalUs = 0;      // RenderLock acquired -> Activity::render() returned
  int64_t composeUs = 0;    // totalUs - driveUs
  int64_t driveUs = 0;      // all GfxRenderer::displayBuffer() time inside this render
  int64_t glyphUs = 0;      // glyph blit path
  int64_t imageUs = 0;      // bitmap/image blit path
  int64_t pageLoadUs = 0;   // (a) Section::loadPage + deserialise
  int64_t measureUs = 0;    // (b) font measure/prewarm scan, status-bar time excluded
  int64_t statusUs = 0;     // (c) renderStatusBar() paint
  int64_t clipUs = 0;       // (d) clipping highlights
  uint32_t glyphMisses = 0; // (e) hot-group glyph cache misses inside this render
  int64_t otherUs = 0;      // (f) composeUs - (pageLoad + measure + status + clip + glyph + image)
  int driveCalls = 0;       // displayBuffer() calls inside this render (usually 1)
  uint8_t driveMode = 0;    // HalDisplay::RefreshMode actually handed to the panel
  bool valid = false;       // at least one render has completed since boot
};

// --- per-render accounting (render task / main task only) -------------------
#if CP_CPU_STATS
void beginRender();
void endRender(int64_t totalUs);
void noteDriveUs(int64_t us, uint8_t mode);
void noteGlyphUs(int64_t us);
void noteImageUs(int64_t us);
void notePageLoadUs(int64_t us);
void noteMeasureUs(int64_t us);
void noteStatusUs(int64_t us);
void noteClipUs(int64_t us);
void noteGlyphMisses(uint32_t count);
// Running total of renderStatusBar() time inside the current render. The caller
// that times the prewarm window subtracts the delta so (b) and (c) cannot
// double-count the status-bar scan call.
int64_t statusUsInRender();
void noteLightSleepUs(int64_t us);
int64_t nowUs();  // esp_timer_get_time(), the clock the hooks measure with
#else
inline void beginRender() {}
inline void endRender(int64_t) {}
inline void noteDriveUs(int64_t, uint8_t) {}
inline void noteGlyphUs(int64_t) {}
inline void noteImageUs(int64_t) {}
inline void notePageLoadUs(int64_t) {}
inline void noteMeasureUs(int64_t) {}
inline void noteStatusUs(int64_t) {}
inline void noteClipUs(int64_t) {}
inline void noteGlyphMisses(uint32_t) {}
inline int64_t statusUsInRender() { return 0; }
inline void noteLightSleepUs(int64_t) {}
inline int64_t nowUs() { return 0; }
#endif

const RenderBreakdown& lastRender();

// Light-sleep accounting since boot (the HP half of `lpsleep`).
struct SleepTotals {
  int64_t totalUs = 0;
  uint32_t slices = 0;
};
SleepTotals lightSleep();

// Formatters. Each writes a complete, self-describing block (labelled fields,
// one per line) into `out` and returns 0, or a negative errno. They never
// allocate: this is a debug path on a possibly fragmented heap.
int formatCpu(char* out, size_t cap);
int formatPm(char* out, size_t cap);
int formatRender(char* out, size_t cap);

}  // namespace cpustats
