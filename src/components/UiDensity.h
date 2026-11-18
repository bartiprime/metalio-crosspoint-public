#pragma once

#include <BoardConfig.h>

#include <cmath>

// Automatic, board-derived UI scale.
//
// The inherited UI was laid out on the 480x800 @ 235 PPI panel of the non-Plus
// devices, so every absolute value in ThemeMetrics is a pixel count from that
// canvas. On a denser panel the same pixel count is physically smaller: a 12 px
// glyph is 1.03 mm at 297 PPI but 1.30 mm at 235 PPI, i.e. 26 % smaller on the
// glass. This header resolves ONE scalar that restores the design's physical
// chrome size, and UITheme::getMetrics() applies it at a single point.
//
// It is NOT a user-facing setting (see
// docs/planning/SCOPE_U1_UI_MENU_SCALE.md section 10): the factor is derived
// from the board profile and the panel, never from settings.
//
// Why one scalar is enough: the panel is wider AND taller in pixels than the
// design canvas, and the width ratio is always the smaller of the two
// (684/480 = 1.425 < 1216/800 = 1.520), so a factor that fits the width can
// never overflow the height. Two axis factors are unnecessary.
//
// A/B hook: -DCROSSPOINT_UI_SCALE_OVERRIDE=1.15f pins the factor on device
// without a code change. 0 (the default) means "resolve at boot".
//
// BoardProfile carries no physical diagonal and no PPI field, so a board that
// left uiScale at its 1.0 default falls back to pixel density: an explicit
// -DCROSSPOINT_PANEL_PPI when the builder knows it, otherwise the area-
// equivalent ratio sqrt(panelArea / designArea). Every shipping board states
// uiScale explicitly except the 800x480 ones, whose area ratio is exactly 1.0,
// so the fallback resolves to 1.0 there and changes nothing.
#ifndef CROSSPOINT_UI_SCALE_OVERRIDE
#define CROSSPOINT_UI_SCALE_OVERRIDE 0.0f
#endif

#ifndef CROSSPOINT_PANEL_PPI
#define CROSSPOINT_PANEL_PPI 0.0f
#endif

namespace uiDensity {

// The design canvas the inherited metrics were authored against.
inline constexpr int kDesignCanvasWidth = 480;
inline constexpr int kDesignCanvasHeight = 800;
inline constexpr float kDesignCanvasPpi = 235.0f;

// Nothing may shrink below the authored design size, and 1.6 is the point where
// paddings alone would start crowding the 684 px content width on a phone-sized
// panel. The physical-parity factor for the 297 PPI Plus panel is 297/235 =
// 1.264; its board profile states 1.35 (95 % width fill) and fits.
inline constexpr float kMinScale = 1.0f;
inline constexpr float kMaxScale = 1.6f;

struct Resolved {
  float scale;
  const char* source;  // "override" | "board" | "ppi" | "ppi-area"
};

inline Resolved clamped(const float value, const char* source) {
  float s = value;
  if (!(s > kMinScale)) s = kMinScale;  // also catches NaN
  if (s > kMaxScale) s = kMaxScale;
  return Resolved{s, source};
}

inline Resolved compute() {
  if (CROSSPOINT_UI_SCALE_OVERRIDE > 0.0f) return clamped(CROSSPOINT_UI_SCALE_OVERRIDE, "override");

  // 1) The per-board value. BoardProfile::uiScale sat in the struct since the
  //    board table was written but had no consumer, so it is the intended
  //    source and the only one an operator has already hand-tuned.
  const float board = BoardConfig::ACTIVE.uiScale;
  if (board > 1.0f) return clamped(board, "board");

  // 2) Derive from the panel. displayWidth/Height are the panel-native scan
  //    frame; the product is orientation independent.
  const float area = static_cast<float>(BoardConfig::ACTIVE.displayWidth) *
                     static_cast<float>(BoardConfig::ACTIVE.displayHeight);
  const float designArea = static_cast<float>(kDesignCanvasWidth) * static_cast<float>(kDesignCanvasHeight);
  if (CROSSPOINT_PANEL_PPI > 0.0f) return clamped(CROSSPOINT_PANEL_PPI / kDesignCanvasPpi, "ppi");
  return clamped(std::sqrt(area / designArea), "ppi-area");
}

// Resolved once per boot; function-local static so no init-order hazard.
inline const Resolved& resolved() {
  static const Resolved r = compute();
  return r;
}

inline float scale() { return resolved().scale; }
inline const char* sourceName() { return resolved().source; }

// DESIGN SPACE -> PANEL SPACE. Every absolute pixel in a theme's ThemeMetrics
// table is a length on the 480x800 design canvas; this converts one such length
// to the physical panel. UITheme::applyUiScale() is the single pass that walks a
// whole metrics table through it.
//
// Paint code must NOT call this per field: read
// UITheme::getInstance().getMetrics() instead, which is already converted, so a
// painted rect and the rect the same widget hit-tests with can never disagree.
// Use px() only for a design-space constant that has no metrics-table entry
// (a lone gap in a theme's draw code).
inline int px(const int designPx) {
  if (designPx == 0) return 0;
  return static_cast<int>(std::lround(static_cast<float>(designPx) * scale()));
}

}  // namespace uiDensity
