#pragma once
#include "UiDensity.h"
#include "fontIds.h"

// FreeInkUI font slots. Row heights, header height, and touch sizes are not
// chosen here: FreeInkApp derives all metric tokens from the body font's line
// height (themeTokensForLineHeight), and UITheme::getMetrics() applies the
// boot-resolved uiDensity factor to the theme's pixel metrics. There is no
// user-facing UI-scale setting (see SCOPE_U1_UI_MENU_SCALE.md section 10): the
// factor comes from the board profile and the panel, never from settings.
//
// FONT LADDER. Only three UI sizes are baked (fontIds.h:8-10):
//   SMALL_FONT_ID 8 pt  <  UI_10 10 pt  <  UI_12 12 pt
// The body and title slots already sit on the top rung, so no factor up to the
// 1.6 clamp can step them further: the physical size gain on a dense panel is
// carried entirely by ThemeMetrics. A real Large tier needs a 14 pt UI font
// baked into flash -- a deliberate later decision, since the 6 MB factory slot
// has only 473,632 B of headroom. The alternatives are worse: stepping the
// subtitle slot onto the body rung erases the label/subtitle hierarchy, and the
// baked NotoSans reader subsets do not cover the scripts the Ubuntu UI fonts do
// (the SD CJK fallback is size-matched to the Ubuntu sizes).
//
// The one step today's assets allow is armed below and off by default so it can
// be A/B'd on device without new code.
namespace uiFontTiers {

// Step the subtitle/small slot up to the body rung once the panel factor
// reaches kStepScale. OFF: at that point subtitle text is the same size as the
// row label above it, which reads as one flat two-line block.
inline constexpr bool kStepSmallToBody = false;
inline constexpr float kStepScale = 1.30f;

}  // namespace uiFontTiers

struct UIScaleSpec {
  int smallFontId;
  int bodyFontId;
  int titleFontId;
};

inline UIScaleSpec uiScaleSpec() {
  UIScaleSpec spec{};
  spec.smallFontId = UI_10_FONT_ID;
  spec.bodyFontId = UI_12_FONT_ID;
  // Titles use the UI font, not a reader font: fui headers draw book and
  // directory titles, and the built-in Ubuntu UI fonts cover Hebrew (plus the
  // size-matched SD CJK fallback) where the NotoSans reader subsets do not.
  // Same font develop's drawHeader used, so script coverage matches develop.
  spec.titleFontId = UI_12_FONT_ID;
  if (uiFontTiers::kStepSmallToBody && uiDensity::scale() >= uiFontTiers::kStepScale) {
    spec.smallFontId = UI_12_FONT_ID;
  }
  return spec;
}
