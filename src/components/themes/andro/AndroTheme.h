#pragma once

#include "components/UiDensity.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

class GfxRenderer;

// Andro — the opt-in "Android launcher" home: a status band with a 6 px rule, a
// featured-book card (dithered cover inside a rounded frame + bold wrapped
// title + regular author + progress bar), and icon rows with no boxes.
// Classic, Lyra, Lyra3Covers, CoverGrid and RoundedRaff are untouched: this
// class only ever ADDS an override, and every geometry it paints comes from the
// scaled metrics / getMenuLayout() Home already hit-tests.
//
// Design values below are lengths on the 480x800 @ 235 PPI canvas the UI was
// authored on; uiDensity::scale() (1.35 on the Plus panel) converts them once in
// UITheme::applyUiScale(). Values marked PHYSICAL are asset sizes, counts, ink
// widths or sentinels and must never scale -- kMetricFields[] (BaseTheme.h)
// carries that classification and the boot audit enforces it.
namespace AndroMetrics {
constexpr ThemeMetrics values = {
    .batteryWidth = 15,
    .batteryHeight = 12,
    // The home band starts at the panel edge (Android status bar sits at 0).
    .topPadding = 0,
    .batteryBarHeight = 20,
    // The 14 pt status line's content row: 40 -> 54 px. r68's 84 -> 113 left the
        // rule 50 px below the text's baseline; 54 px holds the 35 px Ember line box
        // with ~9 px of air above and below, so the rule lands about 17 px under the
        // baseline instead of 50.
        .headerHeight = 40,  // -> 54
        .verticalSpacing = 10,
        .previewPadding = 12,
        .previewHeightPercent = 30,
        .contentSidePadding = 20,  // -> 27; content width 630
        .listRowHeight = 68,       // -> 92 (7.9 mm): settings rows, round 2
    .listWithSubtitleRowHeight = 50,
    .listRowGap = 0,
    .listRowRadius = 0,
    .listInset = 0,
    .listSidePadding = 20,
    .listSelectionStyle = 0,  // invert fill
    .listScrollWidth = 2,     // -> 3: same ink as homeScrollIndicatorWidth
    .listScrollSide = 0,
    .listTitleBold = false,
    .headerSidePadding = 18,
    // Stays 0: this field is Design-scaled, so 6 here would paint 8 px and the
    // SDK divider it also feeds (tab lists) would thicken with it. Andro draws
    // its own rules instead -- home from drawHeader's band code, titled bands
    // from the same metrics.headerRuleThickness (6 px Physical) in r73.
    .headerUnderlineSize = 0,
    .headerTitleAlign = 0,     // left
    .headerBatterySide = 0,
    .headerClockCentered = false,
    .menuRowHeight = 85,  // -> 115
    // -> 11: the pitch the five-row capacity uses. A home menu that paints
    // fewer rows than menuRowCapacity SPREADS them over the band instead (see
    // getMenuLayout in AndroTheme.cpp), so this is the floor, not the on-screen
    // pitch of a four-row menu.
    .menuSpacing = 8,
    .tabSpacing = 8,
    .tabBarHeight = 48,
    .coverGridTabBarHeight = 72,
    .scrollBarWidth = 4,
    .scrollBarRightOffset = 5,
    // 48 -> 65 px: header content row 54 + 6 px rule ending at 60 + 4 px clear.
    // The featured band starts here, so this metric is the ONLY knob that moves
    // the whole home layout -- shortening headerHeight alone would leave the
    // rule floating at the bottom of a dead white band.
    .homeTopPadding = 48,  // -> 65
    // PHYSICAL: the featured cover's thumb height is a cache key, drawn 1:1.
    // 288 is the same value as homeFeaturedCoverLargeHeight below: the featured
    // card is the only home cover Andro paints, and the generated thumbnail is
    // keyed on its height, so the two must never drift apart.
    .homeCoverHeight = 288,
    // PHYSICAL: the featured card band. 342 = 288 cover + 2 x 7 px border +
    // 2 x 20 px breathing room; 65 + 342 + 26 (homeMenuTopOffset) puts the menu
    // band's top at y=433, so the tightened header hands its recovered 61 px to
    // the menu band rather than to the card. (The border went 4 -> 7 px in r71,
    // so the breathing room is 20 px, not the 23 px this comment used to claim;
    // the band itself did not move.)
    .homeCoverTileHeight = 342,
    .homeRecentBooksCount = 1,
    .homeContinueReadingInMenu = false,  // the featured card owns the book
    .homeMenuTopOffset = 19,             // -> 26 gap under the card
    // 185, not 186: 185 x 1.35 = 249.75 -> 250, the authored card height.
    // 186 would scale to 251 and push the menu band off its 402 px anchor.
    .homeFeaturedCardHeight = 185,
    .homeFeaturedCoverWidth = 84,          // PHYSICAL (small variant)
    .homeFeaturedCoverHeight = 113,        // PHYSICAL
    // The painted cover: 172x288. 288 = 160 x 1.8 (linear +80 %); the width is
    // NOT scaled with it -- the frame is sized to the ART. Cover thumbs are
    // generated at width = height * 0.6 = 172.8 -> 172 with crop, so a 288-tall
    // thumb is always >=172 wide and BaseTheme::drawCoverThumbFill() centres it
    // 1:1 and clips the overflow: at a 172-wide slot the art fills the frame
    // exactly for every book aspect, with no regeneration and no cache-key
    // change.
    // The height alone is the thumbnail cache key (see homeCoverThumbHeight()),
    // so changing it orphans the old thumbs in /.crosspoint/covers rather than
    // mixing sizes; the draw path asks for the same height it blits.
    .homeFeaturedCoverLargeWidth = 172,    // PHYSICAL (the painted variant)
    .homeFeaturedCoverLargeHeight = 288,   // PHYSICAL
    // PHYSICAL: the frame around the painted cover, drawn OUTSIDE the bitmap so
    // the art keeps its full 172x288. 7 px of solid ink at 297 PPI = 0.60 mm:
    // the frame hugs the 172 px art, and at 4 px the rule read as a hairline
    // around the artwork. A stroke width is not a length, so it never scales
    // with the UI density.
    .homeFeaturedCoverBorder = 7,
    // -> 12 px. A 12 px radius on a 172 px-wide cover (7 %) reads as a
    // book/e-reader card at reading distance and still leaves a 5 px inner
    // radius (12 - 7) for the corner mask. DESIGN: a radius is a length, so it
    // follows the panel density while the 7 px stroke above stays Physical ink.
    .homeFeaturedCoverRadius = 9,
    // PHYSICAL: 12 px outer bar with a 2 px outline, so the fill is 8 px tall.
    // The outline is what makes a 3 % fill read as a bar instead of a stray
    // dash; neither number is a length, so neither scales.
    .homeFeaturedProgressBarHeight = 12,
    .homeFeaturedProgressBarStroke = 2,
    .homeScrollIndicatorWidth = 3,         // PHYSICAL
    // -> 61 px. Ember 24 labels carry a 60 px line box, and the icon is
    // centred on that box (drawButtonMenu), so the icon may grow to the label's
    // own line height and no further: 45 design = 60.75 -> 61 px fits the 115 px
    // row with 27 px above and 28 below. Drawn from the 64 px mask
    // (menuIcons64.h), so this is ~1:1 rather than a 1.9x upscale of 32 px.
    .menuIconSize = 45,
    // PHYSICAL: 6 px of ink under the header band (3x the old 2 px rule). A rule
    // is a stroke width, not a length -- scaling it with UI density would thicken
    // it past the band it divides. kMetricFields[] marks it Physical.
    .headerRuleThickness = 6,
    .chevronStroke = 2,                    // PHYSICAL (round 2)
    .chevronBoxWidth = 10,                 // PHYSICAL (round 2)
    .chevronBoxHeight = 16,                // PHYSICAL (round 2)
    .buttonHintsHeight = 40,
    .sideButtonHintsWidth = 30,
    .progressBarHeight = 16,
    .progressBarMarginTop = 1,
    .statusBarHorizontalMargin = 5,
    .statusBarVerticalMargin = 19,
    // PHYSICAL: 2 px of air above and below the cluster's line box. The lane is
    // statusBarVerticalMargin (26 px) + statusBarRuleInset (4 px clear only since r93;
    // the 6 px rule this bar used to head is gone) = 30 px and Ember 12's box is
    // exactly 30 px, so the box fills the lane edge to edge. UITheme::getStatusBarHeight()
    // reserves 2 x 2 = 4 px more, which is the whole cost: the reader page box goes
    // from -10 px to -14 px, and since r93 the rule's 6 px are reclaimed on top.
    .statusBarTextPad = 2,
    // r101.9 SBAR1 (operator, 5 Nov): "increase the bottom padding of the bottom
    // status bar, probably by 5 px". PHYSICAL 5, not a design length: 5 px is the
    // ask and 5 px is what this panel gets (a design 4 would also round to 5 here
    // via lround(4 x 1.35) = 5, but would drift to 4 px on a 1.0-density panel).
    // It is the bar's BOTTOM air only, so it lifts the whole cluster -- digits,
    // battery glyph, clock, bookmark -- 5 px off the panel edge, and
    // UITheme::getStatusBarHeight() pays for it out of the reader's page box.
    .statusBarBottomPad = 5,
    .keyboardKeyHeight = 56,
    .keyboardKeySpacing = 0,
    .keyboardCenteredText = false,
    .keyboardVerticalOffset = -13,
    .keyboardTextFieldWidthPercent = 85,
    .keyboardWidthPercent = 94,
    .popupTopOffsetRatio = 0.075f,
    .popupMarginX = 15,
    .popupMarginY = 15,
    .popupFrameThickness = 2,
    .popupCornerRadius = 0,
    .popupTextBold = true,
    .popupTextInverted = true,
    .popupTextBaselineOffsetY = -2,
    .popupProgressBarHeight = 4,
    .popupProgressDrawOutline = false,
    .popupProgressClampPercent = false,
    .popupProgressFillInverted = true,
    .popupProgressOutlineInverted = true,
    // Dialog footprint. The power-off confirmation is the one dialog every user
    // meets, and at the old numbers it was a small card: 513 x ~175 px on a
    // 684 x 1216 panel. These three double the card's padding and its button
    // rows and open the seam between the two options (Confirm / Cancel) to
    // 20 design -> 27 px, so an accidental press is much harder -- the dialog
    // comes out 513 x ~330 px, i.e. ~1.9x its old area. Width stays capped at
    // 3/4 of the panel inside OptionPopup, which is why the height carries the
    // growth. Every other theme keeps the BaseTheme table (6 / 16 / 4 / 20).
    .optionPopupItemSpacing = 20,
    .optionPopupInnerPadding = 32,
    .optionPopupSelectionVPadding = 14,
    .optionPopupDialogSideMargin = 20,
    .textFieldHorizontalPadding = 6,
    .textFieldNormalThickness = 1,
    .textFieldCursorThickness = 3,
    .textFieldLineEndOffset = 0,
    .controlRadius = 0,
    .sheetRadius = 0,
    .capsuleRadius = 0};
}  // namespace AndroMetrics

class AndroTheme : public BaseTheme {
 public:
  // S2: date left, clock + Wi-Fi + battery right, 6 px rule at the band's
  // bottom. Every status element -- date, clock, percentage, battery, Wi-Fi --
  // shares ONE optical centre line (the 14 pt cap band), never the band's
  // middle. Only the untitled root band (Home) takes this path; every titled
  // screen keeps BaseTheme::drawHeader, so HeaderBackTapTarget and the shared
  // FreeInkUI header are unchanged.
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle = nullptr,
                  bool backButton = true) const override;

  // S5: settings-style list rows. metrics.listRowHeight is 68 design; on this
  // panel that is 92 px (7.9 mm), and it is the SAME number the UITheme scaling
  // pass produces for the button-device path (UIThemeTokens.h). Reading it
  // through uiDensity::px keeps the two in lockstep on any panel instead of
  // restating 92 as a literal that would go stale if the board's scale moved.
  int listTouchRowHeight() const override { return uiDensity::px(AndroMetrics::values.listRowHeight); }

  // r73/r74 settings chrome. Titled bands get the app-bar look of the Android
  // settings screen: no clock/battery line (the cluster BaseTheme would fill is
  // dropped), the home band keeps its own cluster -- this override never reaches
  // that path. The band is TWICE the home status band -- 2 x 40 design -> 108 px
  // -- with an Ember 24 bold title and a 72 px back button holding the 56 px
  // arrow mask. Titled screens that still pass the home band's 54 px (every
  // non-settings title in this theme) keep the shared 48 px arrow and UI_12.
  bool headerShowsStatusCluster() const override { return false; }
  int titledHeaderHeight() const override { return 2 * uiDensity::px(AndroMetrics::values.headerHeight); }
  int titledHeaderFontId() const override { return AMAZONEMBER_24_FONT_ID; }
  int titledHeaderButtonSize() const override { return 72; }
  // Settings lists are paged by this theme's pitch, so they get the pager: a
  // vertical swipe turns a page and a bottom tab bar appears when the rows
  // overflow (r74).
  bool settingsListPager() const override { return true; }
  // r76: that footer is a pair of tap targets, not a caption strip. 68 design ->
  // 92 px (7.9 mm), +40 % on the 65 px metrics.tabBarHeight this band used to
  // borrow. It stays a knob of its own so the subheader / reader-menu bands that
  // read tabBarHeight keep their 65 px.
  int settingsPagerBandHeight() const override { return uiDensity::px(68); }
  // Row labels are Ember 24 -- the top of the baked ladder (12/14/18/20/22/24;
  // there is no 26) -- and the row icon box is 56 px sampled DOWN from the
  // 64x64 row masks (settingsRowIcons64.h), never upscaled from the 32 px list
  // icons. 56 is PHYSICAL: an asset sampling box (56/64 of the mask), exactly
  // like the home menu's 61-of-64. The row pitch around it is what follows the
  // panel scale.
  int listRowFontId() const override { return AMAZONEMBER_24_FONT_ID; }
  int listRowIconSize() const override { return 56; }
  // r99.9 (operator request D): the row face is now a LADDER, not one size. The
  // shared list path walks it top down and takes the largest rung at which EVERY
  // label on the screen still fits its row on one line -- uniform per screen, so
  // the row pitch (and therefore the page count and the dirty-mask geometry) do
  // not change with it, only the ink size. A label too long even for Ember 18
  // still wraps (ListProps::labelText.maxLines), and only that row grows.
  const int* listRowFontLadder(int& count) const override {
    static constexpr int LADDER[] = {AMAZONEMBER_24_FONT_ID, AMAZONEMBER_22_FONT_ID, AMAZONEMBER_20_FONT_ID,
                                     AMAZONEMBER_18_FONT_ID};
    count = static_cast<int>(sizeof(LADDER) / sizeof(LADDER[0]));
    return LADDER;
  }
  // The file browser's rows are file names, not menu labels: 24 -> 18 is the
  // rung ~25 % down this face (the ladder has no 17; 18 is the nearest, and the
  // next rung is 20 at only 17 %).
  int listRowDenseFontId() const override { return AMAZONEMBER_18_FONT_ID; }
  // r77: every list screen on this theme wears the same chrome the settings
  // tree got in r73-r76 -- the doubled titled band and the Ember row face read
  // through the SAME UiListActivity path (usesThemeListChrome()). The knobs
  // above are the theme's half of that decision; this is the switch.
  bool themeListChrome() const override { return true; }
  // r77: the tab bar's rule language. ONE 6 px rule at the header rule's weight
  // (Physical) replaces the 1 px band divider, and the active tab is marked by
  // an underline of that same weight instead of a dithered pill -- no fill
  // anywhere, which is the only vocabulary this theme paints.
  // Defined in the .cpp: both read the scaled metrics, and this header must not
  // pull UITheme.h in (it is included BY UITheme.h).
  int tabBarRuleThickness() const override;
  int tabActiveUnderlineThickness() const override;
  // r77: the 6 px app-bar rule, lifted out of drawHeader so a screen that
  // composes its own header (library, catalog) gets the same ink from the same
  // place instead of a hand-drawn copy.
  void drawTitledBandRule(const GfxRenderer& renderer, Rect band) const override;
  // The 6 px rule above is the ONLY ink on a titled band's bottom edge: the
  // shared band suppresses the FreeInkUI frame's own bottom border and the
  // header-underline border for a theme that answers true here (see BaseTheme.h).
  bool paintsOwnBandRule() const override { return true; }

  // r78: dialog chrome. The selected option is marked by an OUTLINE at the
  // 6 px Physical rule weight with no fill at all -- the same fill-free
  // vocabulary as the pager buttons and the active tab, and the reason an
  // inverting selection (the affordance r70 removed from this theme's lists)
  // is gone from OptionPopup. The dialog's own frame is drawn at that weight
  // too, replacing metrics.popupFrameThickness's 2 px box.
  int dialogRuleThickness() const override;
  // r78: the slider readout is Ember 18 instead of NOTOSANS_18 (same box, the
  // theme face), guarded by textFitsFace so a readout in a script Ember does
  // not carry keeps NotoSans rather than printing holes.
  int dialogReadoutFontId() const override { return AMAZONEMBER_18_FONT_ID; }
  // r162: captions are Ember 12 -- the face this theme's status bar and its
  // dialog readouts already use -- so the reader menu's chapter/page line and a
  // sheet's page counter read as the same family as every other small run of
  // text in the theme instead of dropping to the 10 pt UI face.
  int captionFontId() const override { return AMAZONEMBER_12_FONT_ID; }
  // r78: the reader toolbar's glyph box. Andro paints the sheets' icons at
  // 40 px sampled DOWN from the 64 px masks (readerToolbarIcons64.h); every
  // other theme keeps the 24 px art it has always drawn.
  int readerToolbarIconSize() const override { return 40; }

  // r78: the reader's bottom status bar in this theme's own language -- Ember
  // at the theme's own weights and positions, and no hairlines. The REFRESH
  // modes are untouched: a page turn is a full-content change, so this
  // override only changes ink and geometry.
  void drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage, const int pageCount,
                     std::string title, const int paddingBottom = 0, const int textYOffset = 0,
                     const bool fillMargin = true, const bool isPageBookmarked = false,
                     const bool pageCountEstimated = false) const override;
  // r79 (revised r93): the rows ABOVE the text lane that the lane itself needs. It is
  // the 4 px line-box clear and nothing else now — the 6 px rule that used to sit on
  // top of it was removed in r93 (operator request via RDR1: no divider across the
  // bottom of a reader page in the Andro theme), and its reservation was reclaimed by
  // the page box in the same change. The 4 px stays because the Ember 12 box is 4 px
  // taller than statusBarVerticalMargin's lane and the base bar lets it hang 4 px above.
  // UITheme::getStatusBarHeight() adds this, so every consumer — the reader's page box,
  // the XTC overlay, the status-bar settings preview — reserves exactly what the bar
  // paints: statusBarVerticalMargin + 4 + 2 x statusBarTextPad.
  int statusBarRuleInset() const override;

  // Row pitch for a themed list, in panel pixels, returned as ListProps::rowHeight.
  //
  // THE RULE (r79) -- one page whenever the list fits, and never a spread that
  // swallows the band:
  //   1. minPitch = metrics.listRowHeight (68 design -> 92 px) is the minimum
  //      comfortable row; every themed row is at least this tall.
  //   2. If rowCount * minPitch <= bandHeight the WHOLE list fits on ONE page.
  //      The rows may then grow into the room that is left -- but only up to
  //      maxPitch = 111 design (150 px). r74-r76 asked only "band / rows" and
  //      then spread the remainder as inter-row air, so a 3-row menu came out
  //      364 px per row and filled the panel; a short list must NOT fill the
  //      band. Returns clamp(band / rows, minPitch, maxPitch).
  //   3. Otherwise the content genuinely cannot fit at the minimum measure, so
  //      a pager is the honest answer and the rows take the COMPACT measure --
  //      exactly minPitch -- so each page holds as many rows as it can.
  // The caller writes this into rowHeight and keeps rowGap at 0, so paint, tap
  // band and page size are one number.
  //
  // On the Plus panel (684x1216, a 1092 px band under the 108 px titled band)
  // this yields:
  //   3 rows  -> 1 page,  pitch 150 px (3 x 150 = 450, not a filled screen)
  //   7 rows  -> 1 page,  pitch 150 px (7 x 150 = 1050 <= 1092)
  //   10 rows -> 1 page,  pitch 109 px (10 x 109 = 1090 <= 1092)
  //   17 rows -> 2 pages, pitch 92 px (compact): 10 rows, then 7
  int listRowFillPitch(const int bandHeight, const int rowCount) const override {
    if (bandHeight <= 0 || rowCount <= 0) return 0;
    const int minPitch = uiDensity::px(AndroMetrics::values.listRowHeight);
    const int maxPitch = uiDensity::px(111);  // 111 design -> 150 px
    // Does everything fit on one page at the minimum measure? Ask that FIRST:
    // the r74-r76 pitch answered "band / rows" before asking, and the answer it
    // gave for a short list was a spread across the whole panel.
    if (rowCount * minPitch <= bandHeight) {
      const int pitch = bandHeight / rowCount;
      return pitch > maxPitch ? maxPitch : pitch;
    }
    // Cannot fit: compact rows, so the pager holds as many as the band allows.
    return minPitch;
  }

  // S3: featured card — dithered cover 1:1 at the large size inside a rounded
  // 7 px frame the art fills (corners masked to the page), text column at
  // x=223 / width 434, bold wrapped title (3 lines, ellipsis), regular author,
  // outlined progress bar under it. The text block is centred against the
  // FRAMED cover rect, not top-aligned in the band.
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                         const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                         std::function<bool()> storeCoverBuffer) const override;

  // S4: icon + sentence-case label rows, no box. This is a touch-only board:
  // there is no focus cursor to move, so no row is ever drawn inverted -- every
  // entry paints the same normal ink (see drawButtonMenu). Right-edge indicator
  // when the rows overflow the band.
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                    const std::function<std::string(int index)>& buttonLabel,
                    const std::function<UIIcon(int index)>& rowIcon) const override;

  // Capacity is 5 rows. A SHORTER menu (the shipped home has 4) spreads its
  // rows over the band -- pitch derived from the rows actually painted, capped
  // at rowHeight + rowHeight/2 -- instead of leaving a quarter of the screen
  // blank under the last row.
  MenuLayout getMenuLayout(const GfxRenderer& renderer, Rect band, int buttonCount, int selectedIndex) const override;

  // Home thumbnails are generated at the featured cover's height (PHYSICAL, so
  // it is also the cache key the draw path asks for).
  int homeCoverThumbHeight(const GfxRenderer& renderer) const override;

  // S5/S6 (settings lists): 92 px rows when Andro is the active theme, and a
  // 2 px chevron on rows that drill in. Both are read by the shared list path
  // through these hooks, so a theme that does not override them is untouched.
};