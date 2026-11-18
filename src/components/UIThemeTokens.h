#pragma once
#include <BoardConfig.h>
#include <FreeInkUIGfxRenderer.h>

#include "UITheme.h"

// Merges the active UITheme's shape (row gaps, radii, insets, selection
// style) with the uiScale-derived sizes into FreeInkUI theme tokens: the
// theme says what lists look like, the scale says how big they are.
// Everything read here is plain data from ThemeMetrics — the same values an
// SD-card theme file will eventually supply.
//
// INVARIANT (r101.11 BAND1): `target` must carry the CANONICAL uiScale binding,
// i.e. be a makeUiTarget() target with FONT_BODY = uiScaleSpec().bodyFontId.
// Every derived size (rowHeight, headerHeight, minTouchSize = body line height +
// 14) is measured from that slot, so a target whose FONT_BODY a list screen has
// temporarily rebound to the theme's row face yields a DIFFERENT, larger set --
// the r101.10 tab band that doubled from 72 to 117 px once a page had drawn.
inline freeink::ui::ThemeTokens uiThemeTokens(const freeink::ui::GfxRendererTarget& target) {
  namespace fui = freeink::ui;
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  fui::ThemeTokens tokens = fui::themeTokensForLineHeight(target.lineHeight(fui::GfxRendererTarget::FONT_BODY));
  // Button-device lists retain the theme's preferred density. The SDK still
  // grows individual rows for wrapping, subtitles and icons.
  if (!BoardConfig::hasTouch()) tokens.listMinRowHeight = static_cast<int16_t>(metrics.listRowHeight);
  tokens.listRowGap = static_cast<int16_t>(metrics.listRowGap);
  tokens.listRowRadius = static_cast<uint8_t>(metrics.listRowRadius);
  tokens.listInset = static_cast<int16_t>(metrics.listInset);
  tokens.listSidePadding = static_cast<int16_t>(metrics.listSidePadding);
  tokens.listSelectionStyle = static_cast<fui::SelectionStyle>(metrics.listSelectionStyle);
  tokens.listScrollWidth = static_cast<int16_t>(metrics.listScrollWidth);
  tokens.listScrollSide = static_cast<uint8_t>(metrics.listScrollSide);
  // No extra scroll-track inset: the bezel is already compensated once at the
  // frame level (GfxRendererTarget::deviceContext() feeds the board's
  // viewable insets into the fui safe area, and every list band derives from
  // safeRect()). Adding the same inset here doubled the compensation and
  // floated the track a full bezel-width inside the visible edge.
  tokens.listScrollInset = 0;
  // Screen::header()/status() band height. Without this the SDK's
  // line-height-derived default applies and fui-drawn headers (OPDS) come out
  // a different height than every GUI.drawHeader band.
  tokens.headerHeight = static_cast<int16_t>(metrics.headerHeight);
  tokens.headerSidePadding = static_cast<int16_t>(metrics.headerSidePadding);
  tokens.headerUnderline = static_cast<uint8_t>(metrics.headerUnderlineSize);
  tokens.headerTitleAlign = static_cast<fui::TextAlign>(metrics.headerTitleAlign);
  // Control-panel shape (sheet corners, tiles, step buttons, capsule slider),
  // so the control center follows the theme like every list and header does.
  tokens.controlRadius = static_cast<uint8_t>(metrics.controlRadius);
  tokens.sheetRadius = static_cast<uint8_t>(metrics.sheetRadius);
  tokens.capsuleRadius = static_cast<uint8_t>(metrics.capsuleRadius);
  tokens.bodyText.bold = metrics.listTitleBold;
  // Touch-only: a list has no focus cursor to move, so StateSelected is not a
  // navigation affordance -- it only ever painted a black row bar on Settings,
  // the library and the file browser, the most ghost-prone shape on this panel.
  // Paint StateSelected exactly like a normal row instead.
  //
  // The paints are explicit AND explicitlySet on purpose. resolveListProps()
  // does `theme_.listRow.unset() ? defaultListRowStyles() : theme_.listRow`, and
  // StyleSet::unset() is true whenever every paint is PaintKind::None -- so a
  // "blank" StyleSet would be discarded and defaultListRowStyles() (black
  // selected fill, white ink) re-applied on top. componentStyle()->apply() only
  // overwrites fields a screen explicitly set, so it cannot resurrect the bar
  // either. StateActive is left alone: the brief invert on tap is wanted.
  if (BoardConfig::hasTouch()) {
    fui::StyleSet rows = tokens.listRow;
    rows.explicitlySet = true;
    rows.normal.background = fui::Paint::solid(fui::Color::White);
    rows.normal.foreground = fui::Paint::solid(fui::Color::Black);
    rows.selected = rows.normal;
    tokens.listRow = rows;
  }
  return tokens;
}
