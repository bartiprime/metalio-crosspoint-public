#pragma once

// PWR9: the power-off dialog frame's COMPOSITION, as a template.
//
// Why a template: the frame's draw sequence (ring, box, one centred line) is what
// test/poweroff_text_frame has to rasterise to prove the words are actually INKED on
// the box they sit in. GfxRenderer.cpp pulls in the board, the panel and the SD font
// stack, so it cannot be built on the host; a stub renderer with the same verbs is
// the only way to exercise the real composition off-device. The geometry and the
// colour pairing therefore live here, in one place, and both the firmware (through
// GfxRenderer/ThemeMetrics) and the host test (through its own small doubles) run
// THIS code -- not a copy of it.
//
// Renderer must provide (all const):
//   getScreenWidth(), getScreenHeight(), getLineHeight(fontId), getTextWidth(fontId, text, style),
//   fillRect(x, y, w, h, black), fillRoundedRect(x, y, w, h, radius, Color),
//   drawCenteredText(fontId, y, text, black, style)
// Metrics must provide: popupMarginX, popupMarginY, popupFrameThickness, popupCornerRadius,
//   popupTextBaselineOffsetY.
//
// The renderer's `black` verbs follow GfxRenderer's own semantics: true paints black ink,
// false paints white ink (GfxRenderer::fillRect -> fillRectImpl<Color::Black/White>).

template <typename Renderer, typename Metrics, typename Style>
void paintPowerOffDialogFrame(const Renderer& renderer, const Metrics& metrics, const int fontId, const char* text,
                              const bool darkInk, const Style style, const int frameThicknessOverride = 0) {
  const int marginX = metrics.popupMarginX;
  const int marginY = metrics.popupMarginY;
  // A theme that states a dialog RULE weight (Andro: 6 px) frames this box at
  // that weight, like OptionPopup and the slider dialogs; 0 keeps
  // popupFrameThickness (every other theme, and the host test).
  const int frameThickness =
      frameThicknessOverride > 0 ? frameThicknessOverride : metrics.popupFrameThickness;
  const int radius = metrics.popupCornerRadius;

  const int lineH = renderer.getLineHeight(fontId);
  const int textWidth = renderer.getTextWidth(fontId, text, style);
  const int w = textWidth + marginX * 2;
  const int h = lineH + marginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;
  const int y = (renderer.getScreenHeight() - h) / 2;

  // Ring and box in opposite inks; darkInk swaps which is which (drawPopup's own shape).
  const Color ring = darkInk ? Color::Black : Color::White;
  const Color box = darkInk ? Color::White : Color::Black;
  bool boxIsBlack = false;
  if (radius > 0) {
    renderer.fillRoundedRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2,
                             radius + frameThickness, ring);
    renderer.fillRoundedRect(x, y, w, h, radius, box);
    boxIsBlack = (box == Color::Black);
  } else {
    renderer.fillRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2,
                      /*black=*/!darkInk);
    renderer.fillRect(x, y, w, h, /*black=*/darkInk);
    boxIsBlack = darkInk;
  }
  // PWR9: THE WORD IS INKED OPPOSITE THE BOX IT SITS ON. It used to be drawn with the
  // caller's `darkInk`, which is the PAGE's ink, not the box's: the flat frame fills
  // its field with the page's own colour (white on a light frame, black on the dark one),
  // so `darkInk` painted white on white / black on black -- the box and its ring went
  // down, the words never did, and the glass showed "border only". The rounded frame
  // (themes with popupCornerRadius > 0) inverts the field, which is why it never showed
  // this. Text ink therefore comes from the box colour the branch above actually
  // painted, never from darkInk twice.
  renderer.drawCenteredText(fontId, y + marginY + metrics.popupTextBaselineOffsetY, text, /*black=*/!boxIsBlack, style);
}
