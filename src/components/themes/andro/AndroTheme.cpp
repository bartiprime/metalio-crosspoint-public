#include "components/themes/andro/AndroTheme.h"

#include <Bitmap.h>

#include <BoardConfig.h>

#include <EpdFontFamily.h>

#include <GfxRenderer.h>

#include <HalClock.h>

#include <HalPowerManager.h>

#include <HalStorage.h>

#include <Logging.h>

#include <WiFi.h>

#include <algorithm>

#include <cmath>

#include <cstring>

#include <ctime>

#include <string>

#include <vector>

#include "CrossPointSettings.h"

#include "I18n.h"

#include "RecentBooksStore.h"

#include "components/HeaderBackTapTarget.h"

#include "components/UITheme.h"

#include "components/icons/menuIcons64.h"

#include "components/icons/wifi.h"

#include "fontIds.h"

#include "util/BookProgress.h"

namespace {

// The Andro type ladder: real Amazon Ember faces from fontIds.h, baked at

// 12/14/18/20/22/24 and used at their physical pixel sizes on the 1216 px

// panel -- nothing here is scaled by uiDensity. Measured line boxes (the

// advanceY the generated headers carry): 12 -> 30 px, 14 -> 35 px, 18 -> 45,

// 20 -> 50, 22 -> 55, 24 -> 60.

// Header/status 14 (date, clock, battery %), author and progress % 14, menu row

// labels 24 (60 px line box centred in the 115 px row), featured title 24 bold

// (60 px, 3-line cap = 180 px inside the 342 px featured band).

constexpr int kHeaderFont = AMAZONEMBER_14_FONT_ID;

constexpr int kMenuLabelFont = AMAZONEMBER_24_FONT_ID;

constexpr int kFeaturedTitleFont = AMAZONEMBER_24_FONT_ID;

constexpr int kFeaturedAuthorFont = AMAZONEMBER_14_FONT_ID;

// 223 - (27 side padding + 172 cover + 24 gap): the featured card's text column

// starts where the framed cover ends and runs to the 657 px content edge (434

// wide).

constexpr int featuredTextGap = 24;

// Gap between the icon box and its label.

constexpr int menuIconLabelGap = 18;

// Rows the layout will paint, whatever the band could hold. The home menu

// paints at most 5 entries (4 base + the library slot); capacity, not content.

constexpr int menuRowCapacity = 5;

// Space kept clear under the last row when a short menu spreads over its band.

constexpr int menuBottomInset = 24;

// Gap between the author's line box and the progress bar's outline.

constexpr int featuredBarGap = 12;

// Gap between the author's text and the right-aligned percentage label.

constexpr int progressLabelGap = 12;

// The home-menu masks (menuIcons64.h) are authored 64x64, so the 61 px icon is

// drawn ~1:1. kIconMaskSize is the mask's stride source: 64 / 8 = 8 bytes a row.

constexpr int kIconMaskSize = 64;

// The art slot is grown by this much on every side, so the bitmap's ink runs

// UNDER the frame's inner edge instead of stopping a pixel short of it: a seam

// between art and stroke is the one thing a hairline frame cannot hide.

constexpr int kCoverSeam = 1;

// The home-menu masks are 64x64 (components/icons/menuIcons64.h), generated

// by freeink-sdk's gen_icons.py from the same Lucide SVGs the old 32 px set

// came from, so the 61 px menu icon is drawn ~1:1 instead of a 1.9x

// nearest-neighbour upscale. The generator pre-rotates them 90 CCW like every

// other drawIcon() mask, so drawIconAtSize()'s (size-1-row, col) mapping still

// lands them orientation-correct.

const uint8_t* iconForRow(const UIIcon icon) {

  switch (icon) {

    case UIIcon::Folder:

      return icon_folder_64.bits;

    case UIIcon::Book:

      return icon_book_64.bits;

    case UIIcon::Recent:

      return icon_recent_64.bits;

    case UIIcon::Settings:

      return icon_settings_64.bits;

    case UIIcon::Transfer:

      return icon_transfer_64.bits;

    case UIIcon::Library:

      return icon_library_64.bits;

    case UIIcon::Plugins:

    case UIIcon::Blocks:

      return icon_blocks_64.bits;

    case UIIcon::Wifi:

      return icon_wifi_64.bits;

    case UIIcon::Hotspot:

      return icon_hotspot_64.bits;

    case UIIcon::Bookmark:

      return icon_bookmark_64.bits;

    default:

      return nullptr;

  }

}

// The face a string is drawn in: the theme's Ember face when the LOADED font

// covers every codepoint of this string, else the nearest baked UI rung. Ember

// is Latin+Cyrillic only and no non-Latin fallback is registered for its ids

// (SdCardFontSystem maps only SMALL/UI_10/UI_12), so an unguarded string in

// Hebrew, Arabic, Greek, CJK or Thai prints holes. Guarded PER STRING, not per

// screen: the status line is digits plus C-locale abbreviations and almost

// always passes, while a book title often does not.

int emberFaceOr(const GfxRenderer& renderer, const int emberFace, const int uiFace, const char* text) {

  return BaseTheme::textFitsFace(renderer, emberFace, text) ? emberFace : uiFace;

}

// Y of a font's capital-band ink top, relative to that line's line-box top.

// The renderer draws ink at lineTop + ascender - glyph->top, so the 'H' gives

// the band every status/title line is optically centred on. Same measurement

// BaseTheme::drawBatteryLeft() uses to centre the battery on a digit's ink.

int capInkTop(const GfxRenderer& renderer, const int fontId) {

  const auto& fonts = renderer.getFontMap();

  const auto it = fonts.find(fontId);

  if (it != fonts.end()) {

    if (const EpdGlyph* glyph = it->second.getGlyph('H', EpdFontFamily::REGULAR)) {

      return renderer.getFontAscenderSize(fontId) - static_cast<int>(glyph->top);

    }

  }

  // Face not registered yet: assume a 3/5 cap ratio rather than returning a

  // value that would put the battery off the band.

  return renderer.getFontAscenderSize(fontId) - renderer.getLineHeight(fontId) * 3 / 5;

}

// 1 bpp UI icon masks drawn at an arbitrary square size. GfxRenderer::drawIcon()

// indexes the mask with rowBytes = (size + 7) / 8, so it can only draw a mask at

// its authored size; the menu asks for 61 px out of a 64 px mask, where that

// stride would read 8-byte rows out of an 8-byte-row mask but sample only 61 of

// the 64 columns. Nearest-neighbour sampling keeps the mask intact at any size

// (and stays ~1:1 for 61 of 64), and `inkIsBlack` paints white-on-black without

// a second copy of the loop.

void drawIconAtSize(const GfxRenderer& renderer, const uint8_t* bitmap, const int x, const int y, const int size,

                    const bool inkIsBlack) {

  if (bitmap == nullptr || size <= 0) return;

  constexpr int maskRowBytes = kIconMaskSize / 8;

  for (int row = 0; row < size; ++row) {

    const int srcRow = row * kIconMaskSize / size;

    for (int col = 0; col < size; ++col) {

      const int srcCol = col * kIconMaskSize / size;

      const uint8_t byte = bitmap[srcRow * maskRowBytes + (srcCol >> 3)];

      if (((byte >> (7 - (srcCol & 7))) & 1) == 0) {

        // Same transpose as drawIcon(): icons are authored for the Portrait

        // orientation every theme forces.

        renderer.drawPixel(x + (size - 1 - row), y + col, inkIsBlack);

      }

    }

  }

}

// How far a rounded rect's edge is inset at `rowInCorner` rows into its corner

// arc: the sagitta of the circle, i.e. r - sqrt(r^2 - (r - 1 - row)^2).

int cornerInset(const int radius, const int rowInCorner) {

  if (radius <= 0) return 0;

  const float r = static_cast<float>(radius);

  const float dy = r - 1.0f - static_cast<float>(rowInCorner);

  const float sq = r * r - dy * dy;

  if (sq <= 0.0f) return radius;

  return radius - static_cast<int>(std::lround(std::sqrt(sq)));

}

// A rounded rectangle STROKE of `thickness`, drawn as one thickness-wide bar per

// row (inset by the corner arc) plus the two straight runs. The renderer has no

// rounded-rect primitive, and four filled bars cannot follow an arc, so the

// per-row form is what makes the corner read as a card rather than a box.

void drawRoundedFrame(const GfxRenderer& renderer, const Rect& rect, const int thickness, const int radius) {

  if (rect.width <= 0 || rect.height <= 0 || thickness <= 0) return;

  const int r = std::min({radius, rect.width / 2, rect.height / 2});

  for (int row = 0; row < rect.height; ++row) {

    int inset = 0;

    if (row < r) {

      inset = cornerInset(r, row);

    } else if (row >= rect.height - r) {

      inset = cornerInset(r, rect.height - 1 - row);

    }

    const int y = rect.y + row;

    renderer.fillRect(rect.x + inset, y, thickness, 1, true);

    renderer.fillRect(rect.x + rect.width - inset - thickness, y, thickness, 1, true);

  }

  const int straightWidth = rect.width - 2 * r;

  if (straightWidth > 0) {

    renderer.fillRect(rect.x + r, rect.y, straightWidth, thickness, true);

    renderer.fillRect(rect.x + r, rect.y + rect.height - thickness, straightWidth, thickness, true);

  }

}

// Paint the page background back over everything inside `rect` that falls

// outside the rounded rect of `radius`: the bitmap is square and the frame is

// round, so without this the art's four corners would show through outside the

// stroke. Only the corner bands are touched -- straight rows inset by 0.

void maskRoundedCorners(const GfxRenderer& renderer, const Rect& rect, const int radius) {

  if (rect.width <= 0 || rect.height <= 0 || radius <= 0) return;

  const int r = std::min({radius, rect.width / 2, rect.height / 2});

  for (int row = 0; row < rect.height; ++row) {

    int inset = 0;

    if (row < r) {

      inset = cornerInset(r, row);

    } else if (row >= rect.height - r) {

      inset = cornerInset(r, rect.height - 1 - row);

    }

    if (inset <= 0) continue;

    renderer.fillRect(rect.x, rect.y + row, inset, 1, false);

    renderer.fillRect(rect.x + rect.width - inset, rect.y + row, inset, 1, false);

  }

}

// Saved percentage of the featured book, or -1. loadBookProgress() parses the

// book's metadata, so it is memoised on the path: one lookup per featured book,

// never one per frame (same contract CoverGridHomeUi documents). Render task

// only, like the static label buffers in BaseTheme::applyHeaderStatus.

int featuredBookProgress(const std::string& path) {

  static std::string cachedPath;

  static int cachedProgress = -1;

  if (cachedPath != path) {

    cachedPath = path;

    cachedProgress = loadBookProgress(path);

  }

  return cachedProgress;

}

// ONE band-rule recipe, shared by the home band and every titled band.
//
// The home band has always drawn its app-bar rule 4 px CLEAR of the band's
// bottom edge (ink 55..60, band ends at 65 on the Plus panel). Every titled
// band drew the same 6 px of ink at its very LAST row instead, flush with the
// band's bottom edge. That placement is the ONLY difference between the rule
// the operator reads as one crisp line (home) and the one he reads as a
// banded / bevelled multi-row artefact (every titled band, i.e. every screen
// but home), so the titled band now uses home's geometry. Home is unchanged by
// construction: bottom-minus-4 is the expression it already used.
//
// Full ink, no coverage, no dither: fillRect(..., true) fills the rect with the
// ink nibble (pixfmt::kInk, 0x0) row by row, so a rule is a solid stroke and
// never a halftone or an optical level. A stroke's weight is Physical
// (kMetricFields[]), so UI density never scales it.
constexpr int kBandRuleClear = 4;

void drawBandRule(const GfxRenderer& renderer, const Rect& band) {
  const int thickness = UITheme::getInstance().getMetrics().headerRuleThickness;
  if (thickness <= 0 || band.width <= 0 || band.height < thickness + kBandRuleClear) return;
  const int y = band.y + band.height - thickness - kBandRuleClear;
  renderer.fillRect(band.x, y, band.width, thickness, true);
}

}  // namespace

void AndroTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,

                            const bool backButton) const {

  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  // Home is the only untitled ROOT band at least one header row tall (titled

  // screens pass a title, the cover-grid status strip is shorter). Everything

  // else keeps the shared FreeInkUI header -- including HeaderBackTapTarget,

  // which only a titled band registers.

  const bool homeBand = title == nullptr && !backButton && rect.height >= metrics.headerHeight;

  if (!homeBand) {

    // Every non-home band is the shared one: the titled band (or the shorter

    // cover-grid status strip) comes from BaseTheme::drawHeader, and the 6 px

    // app-bar rule from drawTitledBandRule() below -- which the shared band

    // helper calls for the SAME rect. r73 painted that rule HERE, so every screen

    // that composed its own header (library, catalog family) missed it entirely.

    BaseTheme::drawHeader(renderer, rect, title, subtitle, backButton);

    return;

  }

  HeaderBackTapTarget::clear();  // Home owns its stack bottom: no back target

  const int sidePad = metrics.contentSidePadding;

  const int rightEdge = rect.x + rect.width - sidePad;

  const int lineHeight = renderer.getLineHeight(kHeaderFont);  // 35 px at Ember 14

  const int lineY = rect.y + (metrics.headerHeight - lineHeight) / 2;

  // ONE optical centre line for the whole status row. The 14 pt cap band ('H'

  // ink: lineY + 9 .. lineY + 29) is what the eye reads as the line's middle --

  // centring the battery on the BAND, or on the line box, leaves it visibly low,

  // which is exactly the misalignment the operator saw.

  const int capTop = lineY + capInkTop(renderer, kHeaderFont);

  const int baseline = lineY + renderer.getFontAscenderSize(kHeaderFont);

  const int statusCentreY = (capTop + baseline) / 2;

  // Date left. No RTC -> no date and no clock: a stale clock on a 1-bit panel

  // reads as fact, and printing yesterday's date is worse than nothing.

  if (halClock.isAvailable()) {

    struct tm now {};

    if (halClock.localTime(now)) {

      char dateText[24];

      if (strftime(dateText, sizeof(dateText), "%a %d %b", &now) > 0) {

        // Per-string guard: the date is digits plus C-locale abbreviations, so it

        // almost always passes -- but if the loaded Ember face cannot draw it the

        // UI face prints it instead of tofu.

        renderer.drawText(emberFaceOr(renderer, kHeaderFont, UI_10_FONT_ID, dateText), rect.x + sidePad,

                          lineY, dateText);

      }

    }

  }

  // Right cluster, laid out from the edge inwards: battery glyph + percentage,

  // then Wi-Fi (connected only), then the clock (RTC only). Every member is

  // placed against statusCentreY, so the row reads as one line of type.

  const uint16_t percentage = powerManager.getBatteryPercentage();

  const bool showPercentage = SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;

  std::string percentText;

  int percentWidth = 0;

  int percentFace = kHeaderFont;

  if (showPercentage) {

    percentText = std::to_string(percentage) + "%";

    percentFace = emberFaceOr(renderer, kHeaderFont, UI_10_FONT_ID, percentText.c_str());

    percentWidth = renderer.getTextWidth(percentFace, percentText.c_str());

  }

  const int batteryWidth = metrics.batteryWidth;

  const int batteryHeight = metrics.batteryHeight;

  const int batteryX = rightEdge - (batteryWidth + (showPercentage ? batteryPercentSpacing + percentWidth : 0));

  // Battery ink spans y .. y + height - 1, so (height - 1) / 2 is its exact

  // half: the outline's centre lands on statusCentreY, not half a pixel under.

  const int batteryY = statusCentreY - (batteryHeight - 1) / 2;

  // drawBatteryLeft() would re-centre the glyph on the SMALL font's digit ink and

  // print the percentage in that same tiny face; the status line is Ember 14, so

  // the outline, the fill and the number are placed here instead.

  BaseTheme::drawBatteryOutline(renderer, batteryX, batteryY, batteryWidth, batteryHeight);

  fillBatteryIcon(renderer, Rect{batteryX, batteryY, batteryWidth, batteryHeight}, percentage);

  if (showPercentage) {

    // BATT6: through the shared primitive, so the number's box is recorded and the
    // change producer can repaint it in place. Right-aligned on the band's edge.
    BaseTheme::drawBatteryPercentLabel(renderer, rightEdge, lineY, percentFace, PercentAlign::Right, percentText,
                                       percentage);

  }

  constexpr int iconSize = 32;  // the 1 bpp header/menu masks are authored 32x32

  constexpr int clusterGap = 12;

  int cursorX = batteryX - clusterGap;

  if (WiFi.status() == WL_CONNECTED) {

    renderer.drawIcon(WifiIcon, cursorX - iconSize, statusCentreY - iconSize / 2, iconSize);

    cursorX -= iconSize + clusterGap;

  }

  if (metrics.headerShowsClock && SETTINGS.clockShowInHeader && halClock.isAvailable()) {

    char clockText[10];

    if (halClock.formatTime(clockText, sizeof(clockText), SETTINGS.clockFormat == 1)) {

      const int clockFace = emberFaceOr(renderer, kHeaderFont, UI_10_FONT_ID, clockText);

      const int clockWidth = renderer.getTextWidth(clockFace, clockText);

      renderer.drawText(clockFace, cursorX - clockWidth, lineY, clockText);

    }

  }

  // 6 px rule, 4 px clear of the band's bottom edge (ink 55..60, band ends at 65

  // on the Plus panel). It is metrics.headerRuleThickness: 6 px of ink,

  // registered as MetricScale::Physical in kMetricFields[] so UI density never

  // scales a stroke. Bottom-anchored so it follows the band the caller computed

  // instead of re-deriving the header height here.

  // 6 px rule, 4 px clear of the band's bottom edge (ink 55..60, band ends at 65
  // on the Plus panel) -- the ONE band-rule recipe, shared with every titled
  // band so both read as the same single stroke of ink.
  drawBandRule(renderer, rect);

}



// r77: the 6 px app-bar rule under a band this theme painted, in the band's own

// rect. Same ink as the home band's rule -- metrics.headerRuleThickness (6 px,

// MetricScale::Physical) -- so both bands read at one weight, and the SDK's own

// divider weight (.headerUnderlineSize) deliberately stays 0: that field is

// Design-scaled, so 6 there would paint 8 px (6 x 1.35).

// Geometry is flush with the band's bottom edge, unlike home's 4 px inset: a

// titled band's title line and back arrow both end above it, so an inset rule

// would sit ON the arrow; at the bottom edge the 6 px land inside the band and

// the screen's content margin starts below them.

// Called by the shared band helper (components/TitledHeaderBand.h) for EVERY

// band drawn through it, which is why a screen that composes its own header

// (library list, catalog family) now carries the rule too.

void AndroTheme::drawTitledBandRule(const GfxRenderer& renderer, Rect band) const {
  // The ONE stroke this edge carries for this theme: the shared recipe's ink,
  // weight and geometry (see drawBandRule), reached through a hook so every
  // titled band -- including the screens that compose their own header -- gets
  // it from the same call. The frame's own bottom border and the header-
  // underline border are suppressed in TitledHeaderBand.h because this theme
  // answers paintsOwnBandRule(); moving this stroke 4 px (flush -> clear) was
  // tried first and changed nothing on the glass, so placement is not the
  // variable the operator's complaint tracks.
  drawBandRule(renderer, band);
}



// r77 tab chrome: ONE 6 px rule (the header rule's weight) instead of the SDK's

// 1 px band divider, and a 6 px underline instead of a dithered pill.

int AndroTheme::tabBarRuleThickness() const {

  return UITheme::getInstance().getMetrics().headerRuleThickness;

}



int AndroTheme::tabActiveUnderlineThickness() const {

  return UITheme::getInstance().getMetrics().headerRuleThickness;

}

// r78: dialog rule weight. The same 6 px Physical rule the header band, the tab
// bar and the active tab already use. Non-zero is what tells OptionPopup to mark
// its selected option by OUTLINE instead of the SDK's inverting/dithered fill --
// the fill affordance r70 removed from this theme's lists, on a board with no
// keyboard and nothing but a finger.
int AndroTheme::dialogRuleThickness() const {

  return UITheme::getInstance().getMetrics().headerRuleThickness;

}

// r93 RDR1: the rows the text lane needs ABOVE statusBarVerticalMargin's lane. This
// used to be the bar's 6 px top rule plus a 4 px clear; the rule is gone (operator,
// r91/r93: no divider across the bottom of a page in the Andro theme), so what is
// left is the 4 px clear alone — and it is still needed, not leftover. The base bar
// lets its line box hang 4 px above statusBarVerticalMargin's lane (`textY = ... - 4`)
// and Ember 12's box is 4 px taller than that lane, so without the 4 the digits would
// be pushed below the safe bottom edge again (r78's bug). What the 6 px bought — the
// rule's room — is now reclaimed by the page box: UITheme::getStatusBarHeight() drops
// from 40 to 34 px for this theme, and the reader page grows by exactly the rule.
int AndroTheme::statusBarRuleInset() const {

  const int rule = UITheme::getInstance().getMetrics().headerRuleThickness;
  // r96 D5 (operator, r95 on-glass session): "it is gone, but lets add back the
  // 2px padding which I think was removed". RDR1 (r93) took this from 10 to 4 when
  // the rule was deleted; the 4 is still needed as the line-box clear (the base bar
  // hangs 4 px above statusBarVerticalMargin's lane and Ember 12's box is 4 px
  // taller than it), so the 2 px the operator wants back is 4 -> 6, not 10.
  //
  // MEASURED EFFECT, in px: statusBarRuleInset 4 -> 6, so
  // UITheme::getStatusBarHeight() 34 -> 36 and the reader's page box loses exactly
  // 2 px at the bottom. The status text does not move; the gap between the last
  // body line and the text lane grows from 4 to 6 px. The rule itself stays gone --
  // this is padding, not the divider.
  return rule > 0 ? 6 : 0;

}

// r78: the reader's bottom status bar in this theme's own language. Same items as
// the base bar (progress/percent, clock, battery, bookmark, chapter title) but on
// this theme's weights and positions: Ember 12 for the cluster, Ember 14 for the
// chapter title, and ONE 6 px rule at the band's top edge -- the mirror of the
// header band's rule -- instead of the hairline weights the base bar implies.
//
// REFRESH MODES ARE DELIBERATELY UNTOUCHED. Nothing here selects, promotes or
// cancels a refresh: a page turn is a full-content change, so the reader keeps
// driving exactly the modes it drove before this override existed. This hook
// changes ink and geometry only.
void AndroTheme::drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                               const int pageCount, std::string title, const int paddingBottom, const int textYOffset,
                               const bool fillMargin, const bool isPageBookmarked, const bool pageCountEstimated) const {

  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  const auto sb = SETTINGS.statusBarSpec();

  const bool textLane = sb.textLaneVisible(halClock.isAvailable());



  int marginTop = 0, marginRight = 0, marginBottom = 0, marginLeft = 0;

  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);



  const int screenW = renderer.getScreenWidth();

  const int barH = UITheme::getInstance().getStatusBarHeight();

  const int ruleThickness = metrics.headerRuleThickness;

  // r79 GEOMETRY: anchor the bar to its BOTTOM edge and derive everything from
  // it. The reserved band is statusBarVerticalMargin (19 design -> 26 px) plus
  // statusBarRuleInset() (4 px line-box clear since r93; the rule is gone) plus the
  // text pad, and the panel's
  // safe bottom is screenHeight - marginBottom - paddingBottom, so the band is
  // exactly [bandBottom - barH, bandBottom] and NOTHING is placed below
  // bandBottom. r78 anchored to the TOP instead: it read the base bar's
  // text-baseline expression as a BAND TOP and then put a 6 px rule plus a 30 px
  // line box BELOW it, so the cluster ended 10 px past the safe edge and the
  // bottom of every digit was clipped by the panel edge. That was the bar that
  // drew off-screen.
  const int bandBottom = renderer.getScreenHeight() - marginBottom - paddingBottom;

  const int bandTop = bandBottom - barH;

  // r93 RDR1 — THE RULE IS GONE FROM THIS BAR. The operator asked for it in r91: the
  // Andro look does not want a divider across the bottom of a page, and it is the
  // reader's bar this applies to, not the header band (the Home header rule is
  // deliberately KEPT — see the header comment; do not "fix" that one too).
  //
  // Watch the knock-on, handled in the same change: statusBarRuleInset() no longer
  // reserves the rule's 6 px, so UITheme::getStatusBarHeight() drops 40 -> 34 and the
  // reader's page box grows by exactly the rule's height. The band is now just the
  // text lane, and the text does NOT move: before, laneTop was bandTop + 6 inside a
  // 40 px band; now it is bandTop inside a 34 px band, and bandTop itself is 6 px
  // lower, so the lane is the same 34 px in the same place on the glass.
  const int ruleInset = 0;

  // Cluster face: Ember 12. Every item shares ONE optical centre line inside the
  // band, exactly like the home status band. The lane runs from the rule's lower
  // edge to the band's bottom (or to the progress bar's top when that is
  // painted), and the 30 px line box is centred in it: with the reservation
  // above, the lane is exactly 30 px on this panel, so the box fills it edge to
  // edge and the descender row still lands on the glass.
  const int face = AMAZONEMBER_12_FONT_ID;

  const int lineH = renderer.getLineHeight(face);

  const int progressReserve = sb.showsProgressBar() ? std::max(1, ruleThickness) : 0;

  const int laneTop = bandTop + ruleInset;

  // r101.9 SBAR1: the band is `pad` taller than the lane it holds (getStatusBarHeight()
  // reserves the bar's bottom padding), so the LANE must not grow with it. If it did,
  // the cluster would just re-centre in a 41 px lane and drift up only (41 - 30)/2 -
  // (36 - 30)/2 = 2 px instead of the 5 the operator asked for. Take the pad off the
  // lane's BOTTOM edge instead: laneH stays 36 px, laneTop (= bandTop) rose by 5, so
  // the line box and every glyph hung off textY lift exactly 5 px, and the pad is the
  // air left below them. Same gate as the reservation, so the two can never disagree.
  const int barBottomPad = sb.textLaneVisible(true) ? std::max(0, metrics.statusBarBottomPad) : 0;

  const int laneBottom = bandBottom - progressReserve - barBottomPad;

  const int laneH = laneBottom - laneTop;

  int clusterY = laneTop + (laneH > lineH ? (laneH - lineH) / 2 : 0);

  // Belt and braces: even if a panel's font ladder or an odd margin makes the
  // lane shorter than one line box, the box is clamped inside the band instead
  // of hanging off it.
  clusterY = std::min(std::max(clusterY, bandTop), std::max(bandTop, bandBottom - lineH));

  // textYOffset is the base bar's "shift the whole bar down" contract (the auto
  // page-turn path buys extra bottom margin for it). It is applied as a shift and
  // CLAMPED into the band, so no caller can push this bar off the panel.
  const int textY = std::min(std::max(clusterY - textYOffset, bandTop), std::max(bandTop, bandBottom - lineH));



  const int corner = BoardConfig::ACTIVE.cornerRadius;

  const int leftX = metrics.statusBarHorizontalMargin + std::max(marginLeft, corner) + 1;

  const int rightX = screenW - metrics.statusBarHorizontalMargin - std::max(marginRight, corner);

  int leftClusterWidth = 0;

  int rightClusterWidth = 0;



  // Progress counter, right cluster.
  if (sb.showBookProgressPercent || sb.showChapterPageCount) {

    char progressStr[32];

    if (sb.showBookProgressPercent && sb.showChapterPageCount) {

      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", currentPage, pageCount, bookProgress);

    } else if (sb.showBookProgressPercent) {

      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);

    } else {

      snprintf(progressStr, sizeof(progressStr), "%d/%d", currentPage, pageCount);

    }

    const bool showEstimate = pageCountEstimated && sb.showChapterPageCount;

    const int progressW = renderer.getTextWidth(face, progressStr);

    const int estimateW = showEstimate ? renderer.getTextWidth(face, "~") : 0;

    const int estimateGap = showEstimate ? 2 : 0;

    const int progressX = rightX - estimateW - estimateGap - progressW;

    if (showEstimate) renderer.drawText(face, progressX, textY, "~");

    renderer.drawText(face, progressX + estimateW + estimateGap, textY, progressStr);

    rightClusterWidth += estimateW + estimateGap + progressW;

  }



  // Battery, left cluster: the shared outline + fill art (same glyph every theme
  // paints), with the percentage in Ember rather than the base bar's SMALL face.
  if (sb.showBattery) {

    const uint16_t percentage = powerManager.getBatteryPercentage();

    const int iconY = textY + std::max(0, (lineH - metrics.batteryHeight) / 2);

    drawBatteryOutline(renderer, leftX, iconY, metrics.batteryWidth, metrics.batteryHeight);

    fillBatteryIcon(renderer, Rect{leftX, iconY, metrics.batteryWidth, metrics.batteryHeight}, percentage);

    int batteryWidth = metrics.batteryWidth;

    if (sb.showBatteryPercent) {

      const std::string pct = std::to_string(percentage) + "%";

      // BATT6: shared primitive (records the box for the change producer).
      drawBatteryPercentLabel(renderer, leftX + batteryWidth + batteryPercentSpacing, textY, face, PercentAlign::Left,
                              pct, percentage);

      batteryWidth += batteryPercentSpacing + renderer.getTextWidth(face, pct.c_str());

    }

    leftClusterWidth += batteryWidth;

  }



  // Clock (any board whose RTC probe succeeded).
  if (sb.showsClock() && halClock.isAvailable()) {

    char timeBuf[9];

    if (halClock.formatTime(timeBuf, sizeof(timeBuf), sb.clock12h)) {

      const int clockW = renderer.getTextWidth(face, timeBuf);

      int clockX = 0;

      if (sb.clockMode == CrossPointSettings::STATUS_BAR_CLOCK_LEFT) {

        clockX = leftX + leftClusterWidth + (leftClusterWidth > 0 ? 10 : 0);

        leftClusterWidth += clockW + 10;

      } else if (sb.clockMode == CrossPointSettings::STATUS_BAR_CLOCK_RIGHT) {

        clockX = rightX - rightClusterWidth - (rightClusterWidth > 0 ? 10 : 0) - clockW;

        rightClusterWidth += clockW + 10;

      }

      renderer.drawText(face, clockX, textY, timeBuf);

    }

  }



  // Bookmark marker: the shared 16 px art (see BaseTheme::drawBookmarkStatusIcon).
  if (textLane && isPageBookmarked) {

    const int bookmarkGap = leftClusterWidth > 0 ? bookmarkStatusIconGap : 0;

    const int bookmarkX = leftX + leftClusterWidth + bookmarkGap;

    drawBookmarkStatusIcon(renderer, bookmarkX, textY + 1);

    leftClusterWidth += bookmarkStatusIconWidth + bookmarkGap;

  }



  // Optional progress BAR (the status-bar mode that replaces the page counter
  // with a full-width bar). Drawn at the theme's rule weight and flush to the
  // band's bottom edge, so it is the same ink as the rule at the top.
  if (sb.showsProgressBar()) {

    size_t progress = 0;

    if (sb.progressBarMode == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS) {

      progress = static_cast<size_t>(bookProgress);

    } else {

      progress = (pageCount > 0) ? static_cast<size_t>((static_cast<float>(currentPage) / pageCount) * 100) : 0;

    }

    const int barX = fillMargin ? 0 : marginLeft;

    const int barW = screenW - barX - (fillMargin ? 0 : marginRight);

    const int barH = std::max(1, ruleThickness);

    const int barY = renderer.getScreenHeight() - marginBottom - paddingBottom - barH;

    renderer.fillRect(barX, barY, barW * static_cast<int>(progress) / 100, barH, true);

  }



  // Chapter title, centred between the clusters. Ember 14 -- the theme's title
  // step -- and only when the face can draw it (no non-Latin fallback is
  // registered for the Ember ids, so a Hebrew/Arabic/Greek/CJK title keeps the
  // UI face rather than printing holes).
  if (!title.empty()) {

    const int titleFace = textFitsFace(renderer, AMAZONEMBER_14_FONT_ID, title.c_str()) ? AMAZONEMBER_14_FONT_ID : face;

    // Centred on the cluster's line box, then CLAMPED into the band: Ember 14's
    // line box is 35 px against the cluster's 30, so the naive centre would put
    // 2 px of it below the safe bottom edge -- the same class of error as the
    // cluster's own r78 overflow. The clamp only ever moves the title's line box
    // (its ink starts a cap height below the top), never its baseline out of the
    // band.
    const int titleLineH = renderer.getLineHeight(titleFace);

    int titleY = textY - std::max(0, (titleLineH - lineH) / 2);

    titleY = std::min(std::max(titleY, bandTop), std::max(bandTop, bandBottom - titleLineH));

    const int usableW = screenW - metrics.statusBarHorizontalMargin * 2 - marginLeft - marginRight;

    const int titleMarginLeft = leftClusterWidth + 30;

    const int titleMarginRight = rightClusterWidth + 30;

    int marginAdjusted = std::max(titleMarginLeft, titleMarginRight);

    int availableSpace = usableW - 2 * marginAdjusted;

    int titleW = renderer.getTextWidth(titleFace, title.c_str());

    if (titleW > availableSpace) {

      availableSpace = usableW - titleMarginLeft - titleMarginRight;

      marginAdjusted = titleMarginLeft;

    }

    if (titleW > availableSpace) {

      title = renderer.truncatedText(titleFace, title.c_str(), availableSpace);

      titleW = renderer.getTextWidth(titleFace, title.c_str());

    }

    if (availableSpace > 0) {

      renderer.drawText(titleFace,
                        marginAdjusted + metrics.statusBarHorizontalMargin + marginLeft + (availableSpace - titleW) / 2,
                        titleY, title.c_str());

    }

  }

}

void AndroTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,

                                     const int selectorIndex, bool& coverRendered, bool& coverBufferStored,

                                     bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {

  (void)selectorIndex;  // the featured block is a single tap target: no per-tile selection

  (void)bufferRestored;

  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  const int sidePad = metrics.contentSidePadding;

  const int coverWidth = metrics.homeFeaturedCoverLargeWidth;

  const int coverHeight = metrics.homeFeaturedCoverLargeHeight;

  // The frame is drawn OUTSIDE the 1:1 blit, so the slot the bitmap fills and

  // the ink around it are separate rects: the art never loses a pixel column to

  // its own border.

  const int borderThickness = metrics.homeFeaturedCoverBorder;

  const int frameRadius = metrics.homeFeaturedCoverRadius;

  const int coverX = rect.x + sidePad;

  // Centered on the framed cover (bitmap + 2 x border), so the band's top and

  // bottom margins stay equal at 23 px.

  const int coverY = rect.y + (rect.height - (coverHeight + 2 * borderThickness)) / 2 + borderThickness;

  // Text column: 263..657 on the Plus panel, i.e. derived from the same card

  // rect HomeActivity hit-tests, never from a second geometry recipe.

  const int textX = coverX + coverWidth + featuredTextGap;

  const int textWidth = rect.x + rect.width - sidePad - textX;

  const int frameX = coverX - borderThickness;

  const int frameY = coverY - borderThickness;

  const int frameW = coverWidth + 2 * borderThickness;

  const int frameH = coverHeight + 2 * borderThickness;

  // Art slot: one pixel larger than the bitmap on every side so the ink runs

  // UNDER the stroke. The corner mask then works on a radius grown by the same

  // pixel, which puts its boundary inside the 7 px stroke: white can never show

  // between the art and the frame, and no art survives outside it.

  const Rect artSlot{coverX - kCoverSeam, coverY - kCoverSeam, coverWidth + 2 * kCoverSeam,

                     coverHeight + 2 * kCoverSeam};

  const int maskRadius = std::max(0, frameRadius - borderThickness) + kCoverSeam;

  const bool hasBook = !recentBooks.empty();

  if (!coverRendered && hasBook) {

    bool drewCover = false;

    if (!recentBooks[0].coverBmpPath.empty()) {

      const std::string coverPath = UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, metrics.homeCoverHeight);

      HalFile file;

      if (Storage.openFileForRead("HOME", coverPath, file)) {

        Bitmap bitmap(file);

        if (bitmap.parseHeaders() == BmpReaderError::Ok) {

          // Already dithered at this height: blit 1:1 and crop the overflow.

          drewCover = drawCoverThumbFill(renderer, bitmap, artSlot);

        }

        file.close();

      }

    }

    if (!drewCover) {

      // No frame, no shadow: the placeholder is flat ink inside the same slot.

      drawCoverPlaceholder(renderer, Rect{coverX, coverY, coverWidth, coverHeight});

    }

    // Square art, round frame: clear the four corners to the page background

    // BEFORE the stroke, so the mask's own edge is buried under the frame's ink.

    maskRoundedCorners(renderer, artSlot, maskRadius);

    // The frame is metrics.homeFeaturedCoverBorder (7 px at 297 PPI, Physical) --

    // a stroke, not a length, so it never scales with the UI density.

    drawRoundedFrame(renderer, Rect{frameX, frameY, frameW, frameH}, borderThickness, frameRadius);

    coverBufferStored = storeCoverBuffer();

    coverRendered = coverBufferStored;

  }

  if (!hasBook) {

    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);

    const int blockTop = rect.y + (rect.height - lineHeight * 2) / 2;

    renderer.drawText(UI_12_FONT_ID, coverX, blockTop, tr(STR_NO_OPEN_BOOK), true, EpdFontFamily::BOLD);

    renderer.drawText(UI_10_FONT_ID, coverX, blockTop + lineHeight, tr(STR_START_READING));

    return;

  }

  const RecentBook& book = recentBooks[0];

  // Ember 24 bold: 60 px line box, so the 3-line cap is 180 px and the whole

  // block (title + author + bar) is 242 px inside the 296 px framed cover.

  // Per-string Ember coverage guard (r77). Each face is chosen BEFORE it is

  // measured, so the line heights, the cap band, the wrap and the centring all

  // use the face the string is actually drawn in.

  const int titleFace = emberFaceOr(renderer, kFeaturedTitleFont, UI_12_FONT_ID, book.title.c_str());

  const int authorFace = emberFaceOr(renderer, kFeaturedAuthorFont, UI_10_FONT_ID, book.author.c_str());

  const int titleLineHeight = renderer.getLineHeight(titleFace);

  const int authorLineHeight = renderer.getLineHeight(authorFace);

  const int barHeight = metrics.homeFeaturedProgressBarHeight;

  const int barStroke = metrics.homeFeaturedProgressBarStroke;

  const int titleCapTop = capInkTop(renderer, titleFace);

  // wrappedText() fills the last allowed line with truncatedText(), so a long

  // title ellipsises on line 3 instead of being cut mid-glyph.

  const auto titleLines =

      renderer.wrappedText(titleFace, book.title.c_str(), textWidth, 3, EpdFontFamily::BOLD);

  const int lineCount = static_cast<int>(titleLines.size());

  const int progress = featuredBookProgress(book.path);

  // -1 means "this book has no saved position", which is not 0 %: the track is

  // drawn either way, but only a real reading position fills it or prints a

  // number.

  const bool hasProgress = progress >= 0;

  const int pct = std::clamp(progress, 0, 100);

  const bool hasAuthor = !book.author.empty();

  // Block geometry measured in INK, not line boxes, so what gets centred is the

  // visible mass: the title's cap band (lineCount - 1 lines apart) + half a line

  // box + the author's cap band + the gap + the bar's 12 px outline.

  const int authorTop = hasAuthor ? lineCount * titleLineHeight + authorLineHeight / 2 : -1;

  const int barTop = (hasAuthor ? authorTop + authorLineHeight : lineCount * titleLineHeight) + featuredBarGap;

  const int blockInkTop = titleCapTop;

  const int blockInkHeight = barTop + barHeight - blockInkTop;

  // Centred against the FRAMED cover -- the rect the eye reads as the card -- not

  // against the band: the band also carries the card's 23 px margins, and

  // centring there left the text riding high with dead space beside the art's

  // lower third.

  int y = frameY + (frameH - blockInkHeight) / 2 - blockInkTop;

  for (const auto& line : titleLines) {

    renderer.drawText(titleFace, textX, y, line.c_str(), true, EpdFontFamily::BOLD);

    y += titleLineHeight;

  }

  if (hasAuthor) {

    // Regular face, no grey tier and no synthetic bold: hierarchy is size and

    // weight only (real bold/regular faces, no second grayscale pass).

    y += authorLineHeight / 2;

    std::string pctText;

    int pctWidth = 0;

    if (hasProgress) {

      pctText = std::to_string(pct) + "%";

      pctWidth = renderer.getTextWidth(authorFace, pctText.c_str());

    }

    // The label rides the author's line box, right-aligned on the same column

    // edge as the bar, so it shares that line's baseline exactly. The author is

    // truncated first, so the two can never collide.

    const int authorMaxWidth = pctWidth > 0 ? std::max(0, textWidth - pctWidth - progressLabelGap) : textWidth;

    const auto author = renderer.truncatedText(authorFace, book.author.c_str(), authorMaxWidth);

    renderer.drawText(authorFace, textX, y, author.c_str(), true, EpdFontFamily::REGULAR);

    if (pctWidth > 0) {

      renderer.drawText(authorFace, textX + textWidth - pctWidth, y, pctText.c_str(), true,

                        EpdFontFamily::REGULAR);

    }

    y += authorLineHeight;

  }

  // The progress element is a BAR, not a hairline: at 3 % the old 11 px rule read

  // as an accidental hyphen. The outline is what supplies the context -- an empty

  // track is still unmistakably a bar -- and the fill reports the percentage.

  const int trackY = y + featuredBarGap;

  renderer.fillRect(textX, trackY, textWidth, barStroke, true);                                  // top

  renderer.fillRect(textX, trackY + barHeight - barStroke, textWidth, barStroke, true);          // bottom

  renderer.fillRect(textX, trackY, barStroke, barHeight, true);                                  // left

  renderer.fillRect(textX + textWidth - barStroke, trackY, barStroke, barHeight, true);          // right

  const int innerWidth = textWidth - 2 * barStroke;

  if (hasProgress && pct > 0 && innerWidth > 0) {

    // Inset by the stroke on all four sides so fill and outline never collide;

    // at least one pixel, so 1 % is visible ink rather than an empty track.

    const int filled = std::max(1, innerWidth * pct / 100);

    renderer.fillRect(textX + barStroke, trackY + barStroke, std::min(filled, innerWidth),

                      barHeight - 2 * barStroke, true);

  }

}

MenuLayout AndroTheme::getMenuLayout(const GfxRenderer& renderer, Rect band, int buttonCount,

                                    int selectedIndex) const {

  MenuLayout layout = BaseTheme::getMenuLayout(renderer, band, buttonCount, selectedIndex);

  // Capacity, not content: the band can hold 5 rows, so a sixth menu entry is

  // refused here rather than silently re-flowing the home screen.

  layout.rows = std::min(buttonCount, menuRowCapacity);

  // A four-row menu on a five-row pitch left ~290 px (24 % of the screen) blank

  // under the last row. Spread the rows the band actually paints over the height

  // it actually has: the pitch is derived from the row count and stays inside the

  // five-row step .. rowHeight + rowHeight/2 range, so a short menu gets airy rows

  // rather than four items scattered down the panel, and the block is centred in

  // whatever is left. The touch grid consumes this same struct, so taps follow.

  if (layout.rows > 0 && layout.rows < menuRowCapacity) {

    const int usable = renderer.getScreenHeight() - band.y - menuBottomInset;

    const int maxPitch = layout.rowHeight + layout.rowHeight / 2;

    if (usable > 0) {

      layout.step = std::clamp(usable / layout.rows, layout.step, maxPitch);

      const int blockHeight = (layout.rows - 1) * layout.step + layout.rowHeight;

      layout.top = band.y + std::max(0, (usable - blockHeight) / 2);

    }

  }

  return layout;

}

void AndroTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,

                                const std::function<std::string(int index)>& buttonLabel,

                                const std::function<UIIcon(int index)>& rowIcon) const {

  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  // The SAME layout HomeActivity maps taps with: paint and hit band are one

  // struct, so a row's icon row cannot drift from where the finger lands.

  const MenuLayout layout = getMenuLayout(renderer, rect, buttonCount, selectedIndex);

  const int sidePad = metrics.contentSidePadding;

  const int iconSize = metrics.menuIconSize;

  const int iconX = rect.x + sidePad;

  const int labelX = iconX + iconSize + menuIconLabelGap;

  // Per-ROW guard (r77): a menu label in a script Ember does not carry falls

  // back to UI_12 for that row only, and the icon is centred on that row's own

  // line box. The band still holds the same number of rows at the theme's

  // pitch, so the row and its tap band do not move.

  for (int i = layout.firstIndex; i < buttonCount && i < layout.firstIndex + layout.rows; ++i) {

    const int rowTop = menuRowTop(layout, i);

    std::string labelStr = buttonLabel(i);

    const char* label = labelStr.c_str();

    const int labelFace = emberFaceOr(renderer, kMenuLabelFont, UI_12_FONT_ID, label);

    const int labelLineHeight = renderer.getLineHeight(labelFace);

    const int labelY = rowTop + (layout.rowHeight - labelLineHeight) / 2;

    // The icon is centred on the LABEL's line box, not on the row box: at 24 pt

    // the two centres differ, and centring on the row makes the glyph read as

    // sitting high against the word it belongs to. 61 px icon on a 60 px line

    // box -> iconY = labelY, so the art spans 27..88 of the 115 px row.

    const int iconY = labelY + (labelLineHeight - iconSize) / 2;

    const uint8_t* icon = rowIcon != nullptr ? iconForRow(rowIcon(i)) : nullptr;

    // No selection ink. On a touch panel there is no focus cursor to move and

    // nothing to preview with the arrow keys: the eye and the finger both work

    // on the whole row, so an inverted row was ink no one asked for -- and a

    // hard black rectangle is the most ghost-prone shape on this panel. Every

    // row draws normal ink; selectedIndex is still consumed by getMenuLayout()

    // above, which is what HomeActivity's tap grid maps against.

    if (icon != nullptr) drawIconAtSize(renderer, icon, iconX, iconY, iconSize, true);

    renderer.drawText(labelFace, labelX, labelY, label, true);

  }

  // Right-edge indicator: the only truthful affordance about list length, drawn

  // only when the band cannot show every row.

  if (layout.rows < buttonCount) {

    const int barWidth = metrics.homeScrollIndicatorWidth;

    const int bandHeight = layout.rows * layout.step - (layout.step - layout.rowHeight);

    const int thumbHeight = std::max(layout.rowHeight / 4, bandHeight * layout.rows / buttonCount);

    const int maxFirst = std::max(1, buttonCount - layout.rows);

    const int thumbY = layout.top + (bandHeight - thumbHeight) * layout.firstIndex / maxFirst;

    renderer.fillRect(rect.x + rect.width - sidePad - barWidth, thumbY, barWidth, thumbHeight, true);

  }

}

int AndroTheme::homeCoverThumbHeight(const GfxRenderer& renderer) const {

  (void)renderer;

  // PHYSICAL: the generated thumb is drawn 1:1 and its height is the cache key.

  return UITheme::getInstance().getMetrics().homeFeaturedCoverLargeHeight;

}

