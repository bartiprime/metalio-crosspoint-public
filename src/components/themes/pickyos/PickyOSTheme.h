#pragma once

#include "components/themes/BaseTheme.h"
#include "fontIds.h"

class GfxRenderer;

// PickyOS — the vendor-firmware design language reduced to tokens: outline-first,
// monoline, flat. Black on paper, no gradient, no shadow, no second ink level.
//
// WHAT THE LANGUAGE IS (derived from the vendor's own icon bitmaps, then
// translated into our primitives — no vendor asset is embedded or copied):
//   * one stroke weight for every piece of chrome, at 8–9 % of the nominal size
//     with a 2 px floor on this panel: kRule (2 px, PHYSICAL) is that floor and
//     every rule, frame and chevron in this theme reads it;
//   * round caps / round joins are a property of the icon masks we already draw
//     (64 px monoline Lucide), not something the theme can set;
//   * corners are rounded but TIGHT — ~1x the stroke, so 6–8 design units on a
//     card. This is the single biggest departure from RoundedRaff, whose 18–20
//     unit radii are pills;
//   * solid black only as an accent (the battery fill, an icon), never as a
//     double outline+fill;
//   * low ink: generous insets (24 design), airy row pitch, and no selection
//     fill anywhere on a touch panel.
//
// PASS ONE covers Home, lists and settings. The reader menu, the reader status
// bar, the sleep screen and the control centre inherit BaseTheme unchanged —
// they render, in PickyOS's metrics, and are not restyled.
//
// Design values below are lengths on the 480x800 @ 235 PPI canvas the UI was
// authored on; uiDensity::scale() (1.35 on the Plus panel) converts them once in
// UITheme::applyUiScale(). Values marked PHYSICAL are asset sizes, counts, ink
// widths or sentinels and must never scale — kMetricFields[] (BaseTheme.h)
// carries that classification and the boot [UISCALE] audit enforces it.
namespace PickyOSMetrics {
constexpr ThemeMetrics values = {
    .batteryWidth = 15,
    .batteryHeight = 12,
    // The band starts at the panel edge: the vendor's status strip is flush to
    // the top, with no floating header card above it.
    .topPadding = 0,
    .batteryBarHeight = 20,
    // 52 design -> 70 px. Holds the strip the battery/clock cluster occupies
    // (batteryBarHeight, 27 px) plus a 43 px content row for the title — a
    // compact band, not Classic's 113 px header, but with air above and below
    // the title. The home band is this same height, so the status strip and a
    // titled band read as ONE band shape across screens.
    .headerHeight = 52,
    .verticalSpacing = 10,
    .previewPadding = 12,
    .previewHeightPercent = 30,
    // 24 design -> 32 px: the monoline language spends its budget on margins,
    // not on ink. Every rail in this theme (header, home card, list rows) uses
    // this one number, so titles, cards and rows share an edge.
    .contentSidePadding = 24,
    // 52 design -> 70 px (6.0 mm). A row is a label line plus air; Andro's 92 px
    // rows are a finger-sized card, this is a list line. The pitch is applied
    // through listTouchRowHeight() / listRowFillPitch() below.
    .listRowHeight = 52,
    .listWithSubtitleRowHeight = 78,
    .listRowGap = 0,
    // 0, not a pill: a list row in this language is a band of paper, and a 2 px
    // radius on a 70 px row is ink nobody can see.
    .listRowRadius = 0,
    .listInset = 0,
    .listSidePadding = 24,
    // Underline (fui::SelectionStyle order 0=fill,1=pill,2=underline,3=triangle):
    // the one fill-free selection the SDK can paint for a row. On this touch
    // board uiThemeTokens() paints every row plain anyway (no focus cursor to
    // follow); the underline is what a button device gets.
    .listSelectionStyle = 2,
    .listScrollWidth = 2,  // one rule weight, never a hairline
    .listScrollSide = 0,
    .listTitleBold = false,  // hierarchy is size and space, not weight
    .headerSidePadding = 24,
    // 0 on purpose: this field is Design-scaled and also feeds the SDK's own
    // header divider, which would draw a second rule at a different weight.
    // PickyOS paints its one band rule itself — drawTitledBandRule() below.
    .headerUnderlineSize = 0,
    .headerTitleAlign = 0,  // left: a monoline band reads as a rail, not a caption
    .headerBatterySide = 0,
    .headerShowsClock = true,
    .headerClockCentered = false,
    // 64 design -> 86 px, the list line's pitch at home-menu scale. The home
    // menu's rows SPREAD within the band (getMenuLayout), so this is the floor.
    .menuRowHeight = 64,
    // 0: the spread in getMenuLayout() owns the pitch, and a separated row needs
    // no gap of its own (the 2 px rule between rows is that gap).
    .menuSpacing = 0,
    .tabSpacing = 10,
    .tabBarHeight = 46,
    .coverGridTabBarHeight = 72,
    .tabPillFullSlot = false,
    .scrollBarWidth = 2,
    .scrollBarRightOffset = 8,
    .homeTopPadding = 52,  // == headerHeight: the home band and titled bands agree
    // PHYSICAL: the featured cover's thumb height is the thumbnail cache key and
    // the art is blitted 1:1, so this is an asset size, not a length.
    .homeCoverHeight = 190,
    // PHYSICAL: the featured card band = 190 art + 2 x 2 px frame + 2 x 12 px
    // air. The card is a card, not a hero: 218 design -> 294 px of a 1216 px
    // panel leaves the list the majority of the screen, which is the vendor
    // hierarchy (list first, one card above it).
    .homeCoverTileHeight = 218,
    .homeRecentBooksCount = 1,
    .homeContinueReadingInMenu = false,  // the card owns the book
    .homeMenuTopOffset = 16,
    // 218: the card rect HomeActivity hands drawRecentBookCover() is
    // homeCoverTileHeight; this is the same length stated for the card.
    .homeFeaturedCardHeight = 218,
    .homeFeaturedCoverWidth = 72,   // PHYSICAL (small variant, unused by this theme)
    .homeFeaturedCoverHeight = 120, // PHYSICAL
    // PHYSICAL: the painted cover, 114 x 190 at 0.6 aspect — the width the thumb
    // generator produces for a 190-tall cover, so drawCoverThumbFill() blits it
    // 1:1 with no rescale and no cache invalidation.
    .homeFeaturedCoverLargeWidth = 114,   // PHYSICAL
    .homeFeaturedCoverLargeHeight = 190,  // PHYSICAL
    // PHYSICAL: 2 px, the theme's one stroke weight. Andro frames with 7 px of
    // ink because its frame is a border; here the frame is a drawn line around
    // the art, and the art is what carries the weight.
    .homeFeaturedCoverBorder = 2,
    // DESIGN: a radius is a length -> 8 px on the Plus panel, i.e. ~1.3x the
    // 2 px stroke. Tight rounded square, not a pill.
    .homeFeaturedCoverRadius = 6,
    // PHYSICAL: the progress track is an outline, 8 px tall with a 2 px stroke,
    // leaving a 4 px fill. Ink geometry, never scaled.
    .homeFeaturedProgressBarHeight = 8,
    .homeFeaturedProgressBarStroke = 2,
    .homeScrollIndicatorWidth = 2,  // PHYSICAL: the theme's stroke weight
    // 40 design -> 54 px. Sampled from the 64 px monoline masks at 0.84, so a
    // 4 px authored stroke lands at ~3 px: above the 2 px floor, and small
    // enough that the label, not the glyph, is the row's mass.
    .menuIconSize = 40,
    // PHYSICAL: the theme's ONE stroke weight. Every rule in this theme — band
    // rules, the card frame, separators, the pager outline — reads this number,
    // which is what makes the language monoline instead of merely thin.
    .headerRuleThickness = 2,
    .chevronStroke = 2,      // PHYSICAL
    .chevronBoxWidth = 8,    // PHYSICAL: a tight chevron, not a 10 px arrow
    .chevronBoxHeight = 14,  // PHYSICAL
    .buttonHintsHeight = 40,
    .sideButtonHintsWidth = 30,
    .progressBarHeight = 16,
    .progressBarMarginTop = 1,
    .statusBarHorizontalMargin = 5,
    // Reader status bar: untouched in pass one. These are the BaseMetrics
    // numbers, so a reader page in PickyOS lays out exactly as it does in
    // Classic — the theme changes ink, not the page box.
    .statusBarVerticalMargin = 19,
    .statusBarTextPad = 0,
    .statusBarBottomPad = 0,
    .keyboardKeyHeight = 56,
    .keyboardKeySpacing = 0,
    .keyboardCenteredText = false,
    .keyboardVerticalOffset = -13,
    .keyboardTextFieldWidthPercent = 85,
    .keyboardWidthPercent = 94,
    .popupTopOffsetRatio = 0.075f,
    .popupMarginX = 18,
    .popupMarginY = 14,
    .popupFrameThickness = 2,
    // 0: BaseTheme's rounded popup path FILLS the box black (fillRoundedRect
    // White then Black). A rounded box in this language must be an outline on
    // paper, and that needs a paint path this theme is not allowed to add — so
    // the box stays square, framed at the theme's 2 px weight, which is the
    // same language without a new mechanism.
    .popupCornerRadius = 0,
    .popupTextBold = false,
    .popupTextInverted = true,
    .popupTextBaselineOffsetY = -2,
    .popupProgressBarHeight = 4,
    .popupProgressDrawOutline = false,
    .popupProgressClampPercent = false,
    .popupProgressFillInverted = true,
    .popupProgressOutlineInverted = true,
    // Dialogs: airy, with the selected option drawn as a 2 px OUTLINE
    // (dialogRuleThickness below) instead of a fill.
    .optionPopupItemSpacing = 10,
    .optionPopupInnerPadding = 20,
    .optionPopupSelectionVPadding = 8,
    .optionPopupDialogSideMargin = 20,
    .textFieldHorizontalPadding = 8,
    .textFieldNormalThickness = 2,
    .textFieldCursorThickness = 3,
    .textFieldLineEndOffset = 0,
    // Control-centre tiles and sheets: tight corners (~1x the stroke), not
    // RoundedRaff's 18-unit pills and not the stadium sentinel.
    .controlRadius = 4,
    .sheetRadius = 8,
    .capsuleRadius = 4};
}  // namespace PickyOSMetrics

class PickyOSTheme : public BaseTheme {
 public:
  // --- Home -----------------------------------------------------------------
  // The untitled root band: date left, clock / wifi / battery right, closed by
  // the theme's 2 px rule. Every other band (titled screens, the cover-grid
  // strip) falls through to BaseTheme::drawHeader and gets the shared FreeInkUI
  // header, the shared back target, and drawTitledBandRule() below.
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle = nullptr,
                  bool backButton = true) const override;

  // The featured card: a 1:1 dithered cover inside a HG 2 px hairline frame with
  // a tight 6-unit radius, the title and author in the UI faces, and an outlined
  // progress track under them. No filled box, no inverted label plate.
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> coverStore) const override;

  // Home menu: icon + label rows divided by the theme's 2 px rule, right-edge
  // overflow indicator, no selection ink (touch panel: the finger is the cursor).
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  MenuLayout getMenuLayout(const GfxRenderer& renderer, Rect band, int buttonCount, int selectedIndex) const override;

  // Home thumbnails are generated at the featured cover's height, which is
  // PHYSICAL, so it is also the cache key the draw path asks for.
  int homeCoverThumbHeight(const GfxRenderer& renderer) const override;

  // --- Lists and settings ---------------------------------------------------
  // Every list screen that does not speak for itself wears this theme's rows:
  // the UI body face (listRowFontId stays 0 — the fonts are ours, and the body
  // rung is the one this theme's titles already use), a 44 px icon box sampled
  // from the 64 px masks, and the 70 px themed pitch.
  bool themeListChrome() const override { return true; }
  int listRowIconSize() const override { return 44; }
  int listTouchRowHeight() const override;
  int listRowFillPitch(int bandHeight, int rowCount) const override;

  // Settings lists SCROLL: no pager, no page-tab footer band. A swipe moves the
  // viewport, exactly as r73 shipped, so this theme reserves no band for arrows
  // and paints no chevron footer.
  bool settingsListPager() const override { return false; }

  // --- One rule weight on every band edge -----------------------------------
  // The 2 px rule under a titled band. Declared here, defined in the .cpp: it
  // reads the scaled metrics, and this header must not pull UITheme.h in (it is
  // included BY UITheme.h).
  void drawTitledBandRule(const GfxRenderer& renderer, Rect band) const override;
  // ...and it is the band bottom's ONLY stroke: the shared band suppresses the
  // FreeInkUI frame border and its own underline for a theme that answers true.
  bool paintsOwnBandRule() const override { return true; }

  // Tabs: one 2 px rule under the bar, and the active tab marked by a 2 px
  // underline rather than a dithered pill — fill-free, like every other
  // selection in this theme.
  int tabBarRuleThickness() const override;
  int tabActiveUnderlineThickness() const override;

  // Dialogs: the selected option is a 2 px outline with no fill at all, and the
  // dialog frame is drawn at the same weight (replacing the 2 px box metric with
  // the same number, stated once).
  int dialogRuleThickness() const override;
};
