#pragma once

// ---------------------------------------------------------------------------
// EOB1 (r111) — the end-of-book panel's geometry and repaint decision, pure
// and host-testable.
//
// THE BUG (operator report: "too small and too plain")
//   `ReaderActivity::render()`'s `if (isAtEndOfBook())` branch called
//   `renderer.clearScreen()` unconditionally, on ENTRY and on every repaint.
//   Two consequences, both measured in this tree before the fix:
//     1. A cleared canvas makes the framebuffer-vs-glass diff the WHOLE panel
//        (the `[mask] dirty_lines=684/684` line from the vendor driver), so the
//        policy has nothing partial to work with: every appearance, including
//        each selection tick, is a whole-panel drive. Same mechanism as
//        `planning/LIB5B_PROGRESS_REPAINT.md` §"the diagnose was wrong".
//     2. Nothing pinned the drive BELOW the policy, so an AUTO request measured
//        over that cleared frame (ratio ~100 %) resolved to HALF — the 48-phase
//        GC16-over-the-diff — and a FAST resolution would have appended the
//        row-9 completion (a second whole-panel GL16, GfxRenderer.cpp row 9).
//
// WHAT THIS TABLE ENCODES (the FLP2 three-term shape, one-for-one)
//   Enter  -> one deliberate from-white GC16 for a frame that really does
//             replace the page (clear + full chrome), pinned by an explicit
//             FULL promotion so the transition setting cannot downgrade it.
//   Select -> NO clear, no chrome: the two rows whose state changed are repainted
//             into the retained canvas, so the diff mask is two row bands.
//             `promoteFast` + `suppressCompletion` + `binaryFast` are pins
//             applied AFTER the policy, exactly as FLP2 does, so (i) the resolved
//             mode is DU whatever the menu-transition setting or the reader's
//             inherited FULL latch says, (ii) the row-9 completion does not
//             append a second whole-panel drive, and (iii) the vendor shim
//             cannot spend its 24-partial clean cadence on the tick.
//   Back   -> nothing to paint: the reader repaints the page it returns to.
//
// This header has no firmware dependency on purpose: test/eob_panel compiles it
// against the real src/display/RefreshPolicy.h, so "selection resolves to ONE
// DU drive" is proven through the same two-stage producer + policy chain the
// renderer runs, not asserted about a copy of it.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace eobpanel {

// Mirrors of the policy's mode codes (src/display/RefreshPolicy.h), so the host
// test can name a drive without pulling the firmware HAL into the header.
inline constexpr int kFull = 0;  // from-white GC16
inline constexpr int kHalf = 1;  // GC16 over the diff
inline constexpr int kFast = 2;  // DU

// MAX_SUGGESTIONS (3) + the trailing Home row. Fixed capacity: the panel never
// allocates and a row rect exists for every row the list can offer.
inline constexpr int kMaxRows = 4;

struct Box {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};
constexpr int bottom(const Box& b) { return b.y + b.h; }
constexpr int right(const Box& b) { return b.x + b.w; }

// Everything the live panel reads from UITheme / GfxRenderer, handed in as
// plain data: the header stays a table, the activity stays the only thing that
// touches the device.
struct Tokens {
  Box safe;              // UITheme::getScreenSafeArea(renderer, true, false)
  int bodyLine = 0;      // renderer.getLineHeight(UI_12_FONT_ID)  (51 on the Plus)
  int smallLine = 0;     // renderer.getLineHeight(UI_10_FONT_ID)  (51)
  int minTouchSize = 0;  // max(44, bodyLine + 14) — the SDK's touch floor
  int verticalSpacing = 0;
  int sidePadding = 0;
  int rowCount = 0;
};

struct Layout {
  Box safe;
  Box header;
  Box subtitle;
  Box band;                  // the list band: rows start here, in this width
  Box hints;
  Box rows[kMaxRows];
  int rowCount = 0;
  int rowHeight = 0;
  int rowGap = 0;
  int sidePadding = 0;  // text inset inside a row (theme token)
};

// 8 px between rows: the plan's gap. Fixed rather than derived so two
// appearances on the same panel can never disagree about where a row edge is.
inline constexpr int kRowGap = 8;

// Row height: 1.6 × the touch floor. On the Plus that is ceil(1.6 × 65) = 104 px,
// against the 67 px row the theme-derived list painted (line height 51 + 2 × 8
// padding), which the operator read as "too small". 96 px is the floor so a
// theme with a small touch minimum still gets a roomy row; 120 px is the ceiling
// so the four rows plus gaps stay inside the band on every orientation.
inline constexpr int rowHeightFor(const int minTouchSize) {
  const int want = (minTouchSize * 16 + 9) / 10;  // ceil(1.6 ×)
  if (want < 96) return 96;
  if (want > 120) return 120;
  return want;
}

// Band-aware clamp: a landscape band is ~440 px tall (the Plus's 684-row side
// minus chrome), which fits four 104 px rows plus gaps only by shrinking them.
// The clamp never goes below the touch floor — a band too short for that is a
// layout bug the host test catches, not something to paper over.
inline constexpr int fitRowHeight(const int want, const int bandHeight, const int rows) {
  if (rows <= 0) return want;
  const int spare = bandHeight - (rows - 1) * kRowGap;
  if (spare <= 0) return want;
  const int fit = spare / rows;
  if (fit >= want) return want;
  return fit;
}

inline constexpr Layout layout(const Tokens& t) {
  Layout l;
  l.safe = t.safe;
  const int rows = t.rowCount < 0 ? 0 : (t.rowCount > kMaxRows ? kMaxRows : t.rowCount);
  l.rowCount = rows;
  l.header = Box{t.safe.x, t.safe.y + t.safe.h / 8, t.safe.w, t.bodyLine};
  l.subtitle = Box{t.safe.x, bottom(l.header) + t.verticalSpacing, t.safe.w, t.smallLine};
  const int bandTop = bottom(l.subtitle) + t.verticalSpacing * 2;
  const int bandBottom = bottom(t.safe) - t.verticalSpacing;
  l.band = Box{t.safe.x, bandTop, t.safe.w, bandBottom > bandTop ? bandBottom - bandTop : 0};
  // The hint strip is the safe area's bottom edge; GUI.drawButtonHints paints it.
  l.hints = Box{t.safe.x, bandBottom, t.safe.w, t.verticalSpacing};
  l.rowGap = kRowGap;
  l.sidePadding = t.sidePadding;
  l.rowHeight = fitRowHeight(rowHeightFor(t.minTouchSize), l.band.h, rows);
  // Rows start one pitch apart from a band-centred top — the same cursor the
  // FreeInkUI list walks when it lays rows out from its content rect, so a
  // rect computed here is the rect the list painted (which is what makes a
  // two-row repaint's dirty mask exactly those two rows).
  const int total = rows * l.rowHeight + (rows > 0 ? (rows - 1) * l.rowGap : 0);
  // Centre the block in the band: a 440 px list under a 903 px band glued to
  // the top reads as the "nearly empty page" the operator reported, and an
  // over-tall block starts at the band top and is clamped by fitRowHeight().
  int y = l.band.y;
  if (total > 0 && total < l.band.h) y += (l.band.h - total) / 2;
  for (int i = 0; i < rows; ++i) {
    l.rows[i] = Box{l.band.x, y, l.band.w, l.rowHeight};
    y += l.rowHeight + l.rowGap;
  }
  return l;
}

enum class Event : uint8_t {
  Enter,   // the panel appears: the reader's page is underneath
  Select,  // the selection moved; two rows changed
  Back,    // leaving for the page it came from
};

struct Plan {
  bool clearScreen = false;         // wipe the canvas before drawing
  bool paintChrome = false;         // title, subtitle, list, hints
  bool paintRows = false;           // only the changed rows
  bool promoteFast = false;         // pin the resolved mode to DU
  bool promoteFull = false;         // arm ONE from-white GC16
  bool suppressCompletion = false;  // suppress the row-9 second whole-panel drive
  bool binaryFast = false;          // 1-bit chrome: take the DU lane
  int rowFirst = -1;                // changed window (paintRows); -1 = none
  int rowLast = -1;
  // How many panel drives one appearance of this plan implies. The entered
  // frame is one drive; a selection is one drive; Back drives nothing.
  constexpr int drives() const { return (paintChrome || paintRows) ? 1 : 0; }
};

constexpr Plan plan(const Event e, int rowFirst = -1, int rowLast = -1) {
  Plan p;
  switch (e) {
    case Event::Enter:
      // The one time the panel legitimately replaces the page: clear, draw the
      // whole chrome, and pay exactly one from-white GC16 for it.
      p.clearScreen = true;
      p.paintChrome = true;
      p.promoteFull = true;
      p.suppressCompletion = true;
      return p;

    case Event::Select:
      // No clear: the canvas already carries the panel, so the diff is the two
      // rows whose paint changed. Pins make that one DU drive under the policy.
      p.paintRows = true;
      p.promoteFast = true;
      p.suppressCompletion = true;
      p.binaryFast = true;
      p.rowFirst = rowFirst;
      p.rowLast = rowLast;
      return p;

    case Event::Back:
      return p;  // the reader's own repaint of the page handles it
  }
  return p;
}

// --- the two layers below, mirrored so the host test can state the claim ----

// The renderer's pin, applied AFTER cp_refresh_resolve_mode() in
// GfxRenderer::displayBuffer() (`if (binaryDuForced) refreshMode = FAST_REFRESH;`).
// FLP2's forcedDriveMode(), unchanged: `resolved` is the policy's output, which
// on a FAST request can still be HALF because the latch says the previous paint
// was a from-white FULL (rule 3, RefreshPolicy.h:415-417).
constexpr int forcedDriveMode(int resolved, const bool binaryFast) {
  return binaryFast ? kFast : resolved;
}

// The row-9 completion condition, verbatim from GfxRenderer::displayBuffer():
// a FAST drive appends a second whole-panel GL16 unless the one-shot was armed.
constexpr bool completionRuns(int applied, const bool row9Arm, const bool suppressed) {
  return applied == kFast && row9Arm && !suppressed;
}

// Drives one appearance puts on the glass: the paint itself plus the completion
// when it runs. "Exactly one per appearance" is this returning 1.
constexpr int drivesPerAppearance(const Plan& p, const int applied, const bool row9Arm) {
  return p.drives() + (p.drives() != 0 && completionRuns(applied, row9Arm, p.suppressCompletion) ? 1 : 0);
}

}  // namespace eobpanel
