#include "components/themes/pickyos/PickyOSTheme.h"

#include <Bitmap.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
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
#include "components/UiDensity.h"
#include "components/icons/cover.h"
#include "components/icons/menuIcons64.h"
#include "components/icons/wifi.h"
#include "fontIds.h"
#include "util/BookProgress.h"

// PickyOS — outline-first, monoline, flat. Pass one: Home, lists, settings.
//
// THE RULE OF THIS FILE: every stroke it paints is metrics.headerRuleThickness
// (2 px, MetricScale::Physical). There is no second weight, no fill used as a
// border, and no gradient — a theme that needs a new primitive to say something
// is a scope change, and this one does not.
//
// Nothing here touches a refresh mode: the paint calls are the same ones every
// other theme already makes, so a PickyOS frame drives the panel exactly as an
// Andro or Classic frame of the same content would.

namespace {

// --- type ladder -----------------------------------------------------------
// UI faces only. This theme owns no typeface: the furniture is the 10 pt UI
// face and the 12 pt UI face, both of which carry the scripts the reader's
// baked fonts do not (the SD CJK fallback maps SMALL/UI_10/UI_12).
constexpr int kStatusFont = UI_10_FONT_ID;   // date, clock, battery %, author
constexpr int kLabelFont = UI_12_FONT_ID;    // menu rows, card title, list rows
constexpr int kRuleLineGap = 12;             // hairline gap inside a row
constexpr int kCardTextGap = 24;             // framed cover -> text column
constexpr int kMenuIconLabelGap = 18;        // icon box -> label
constexpr int kMenuRowCapacity = 5;          // rows the home band will paint
constexpr int kMenuBottomInset = 24;         // air kept under the last row
constexpr int kFeaturedTextGap = 12;         // author line -> progress track
constexpr int kSeam = 1;                     // art runs 1 px under the frame

// Mask source for the 64x64 monoline icon sets (menuIcons64.h).
constexpr int kIconMaskSize = 64;

// The icon masks are authored for the Portrait orientation every theme forces,
// so a mask point (row, col) lands at (x + size - 1 - row, y + col). `size` need
// not be 64: the sample is nearest-neighbour from the authored grid, which is
// what keeps a 54 px menu glyph from being a 1.9x upscale of a 32 px mask.
void drawMaskAtSize(const GfxRenderer& renderer, const uint8_t* bitmap, const int x, const int y, const int size) {
  if (bitmap == nullptr || size <= 0) return;
  constexpr int maskRowBytes = kIconMaskSize / 8;
  for (int row = 0; row < size; ++row) {
    const int srcRow = row * kIconMaskSize / size;
    for (int col = 0; col < size; ++col) {
      const int srcCol = col * kIconMaskSize / size;
      const uint8_t byte = bitmap[srcRow * maskRowBytes + (srcCol >> 3)];
      if (((byte >> (7 - (srcCol & 7))) & 1) == 0) {
        renderer.drawPixel(x + (size - 1 - row), y + col, true);
      }
    }
  }
}

const uint8_t* iconFor(const UIIcon icon) {
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

// Sagitta of the corner arc: how far a rounded rect's edge is inset at
// `rowInCorner` rows into the corner. r - sqrt(r^2 - (r - 1 - row)^2).
int cornerInset(const int radius, const int rowInCorner) {
  if (radius <= 0) return 0;
  const float r = static_cast<float>(radius);
  const float dy = r - 1.0f - static_cast<float>(rowInCorner);
  const float sq = r * r - dy * dy;
  if (sq <= 0.0f) return radius;
  return radius - static_cast<int>(std::lround(std::sqrt(sq)));
}

// A rounded-rect STROKE of `thickness`: one thickness-wide bar per row, inset by
// the corner arc, plus the two straight runs. The corner is what makes this a
// card instead of a box, and at 2 px it is the theme's rule weight bent around
// the corner — same ink, same weight, one language.
void drawRoundedStroke(const GfxRenderer& renderer, const Rect& rect, const int thickness, const int radius) {
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

// Clear the page background outside the rounded rect of `radius`: the blitted
// art is square and the frame is round, so without this the art's four corners
// would show outside the stroke.
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

// A cover slot with no art: paper with the frame's own outline and one glyph at
// the theme weight. Never BaseTheme's placeholder, which fills the lower two
// thirds of the slot solid black — a filled block is the one thing this language
// does not do, and on this panel it is also the shape most likely to ghost.
void drawEmptyCover(const GfxRenderer& renderer, const Rect& rect) {
  if (rect.width <= 0 || rect.height <= 0) return;
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  constexpr int kGlyph = 32;  // the cover mask is authored 32x32
  if (rect.width >= kGlyph + 8 && rect.height >= kGlyph + 8) {
    renderer.drawIcon(CoverIcon, rect.x + (rect.width - kGlyph) / 2, rect.y + (rect.height - kGlyph) / 2, kGlyph);
  }
}

// Saved percentage of the featured book, or -1. loadBookProgress() parses the
// book's metadata, so it is memoised on the path: one lookup per featured book,
// never one per frame. Render task only.
int featuredProgress(const std::string& path) {
  static std::string cachedPath;
  static int cachedProgress = -1;
  if (cachedPath != path) {
    cachedPath = path;
    cachedProgress = loadBookProgress(path);
  }
  return cachedProgress;
}

// A chevron: two strokes at the theme weight, meeting at a point. The only
// "there is a door here" mark this theme draws.
void drawChevron(const GfxRenderer& renderer, const int rightX, const int centreY, const int boxW, const int boxH,
                 const int stroke) {
  const int halfH = boxH / 2;
  const int tipX = rightX;
  const int tailX = rightX - boxW;
  renderer.drawLine(tailX, centreY - halfH, tipX, centreY, stroke, true);
  renderer.drawLine(tailX, centreY + halfH, tipX, centreY, stroke, true);
}

}  // namespace

// ---------------------------------------------------------------------------
// Home status band
// ---------------------------------------------------------------------------
void PickyOSTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,
                              const bool backButton) const {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  // Home is the only untitled ROOT band at least one header row tall. Every
  // other band — a titled screen, the cover-grid status strip — is the shared
  // FreeInkUI one, and carries the shared back target and this theme's rule
  // through drawTitledBandRule().
  const bool homeBand = title == nullptr && !backButton && rect.height >= metrics.headerHeight;
  if (!homeBand) {
    BaseTheme::drawHeader(renderer, rect, title, subtitle, backButton);
    return;
  }

  HeaderBackTapTarget::clear();  // Home owns its stack bottom: no back target

  const int rule = metrics.headerRuleThickness;
  const int sidePad = metrics.contentSidePadding;
  const int rightEdge = rect.x + rect.width - sidePad;
  const int lineHeight = renderer.getLineHeight(kStatusFont);
  // The status line is centred in the band ABOVE the rule, so the 2 px stroke
  // never crowds the digits it closes off.
  const int lineY = rect.y + (rect.height - rule - lineHeight) / 2;
  const int centreY = lineY + lineHeight / 2;

  // Date left. No RTC -> no date and no clock: a stale clock on a 1-bit panel
  // reads as fact, and printing yesterday's date is worse than nothing.
  if (halClock.isAvailable()) {
    struct tm now {};
    if (halClock.localTime(now)) {
      char dateText[24];
      if (strftime(dateText, sizeof(dateText), "%a %d %b", &now) > 0) {
        renderer.drawText(kStatusFont, rect.x + sidePad, lineY, dateText);
      }
    }
  }

  // Right cluster, laid out from the edge inwards: battery glyph + percentage,
  // then Wi-Fi (connected only), then the clock (RTC only). Every member shares
  // the line's optical centre, so the group reads as one line of type.
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const bool showPercentage = SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;

  std::string percentText;
  int percentWidth = 0;
  if (showPercentage) {
    percentText = std::to_string(percentage) + "%";
    percentWidth = renderer.getTextWidth(kStatusFont, percentText.c_str());
  }

  const int batteryWidth = metrics.batteryWidth;
  const int batteryHeight = metrics.batteryHeight;
  const int batteryX = rightEdge - (batteryWidth + (showPercentage ? batteryPercentSpacing + percentWidth : 0));
  const int batteryY = centreY - (batteryHeight - 1) / 2;
  BaseTheme::drawBatteryOutline(renderer, batteryX, batteryY, batteryWidth, batteryHeight);
  fillBatteryIcon(renderer, Rect{batteryX, batteryY, batteryWidth, batteryHeight}, percentage);
  if (showPercentage) {
    // BATT6: shared primitive (records the box for the change producer).
    BaseTheme::drawBatteryPercentLabel(renderer, rightEdge, lineY, kStatusFont, PercentAlign::Right, percentText,
                                       percentage);
  }

  constexpr int iconSize = 32;  // the header Wi-Fi mask is authored 32x32
  constexpr int clusterGap = 12;
  int cursorX = batteryX - clusterGap;
  if (WiFi.status() == WL_CONNECTED) {
    renderer.drawIcon(WifiIcon, cursorX - iconSize, centreY - iconSize / 2, iconSize);
    cursorX -= iconSize + clusterGap;
  }
  if (metrics.headerShowsClock && SETTINGS.clockShowInHeader && halClock.isAvailable()) {
    char clockText[10];
    if (halClock.formatTime(clockText, sizeof(clockText), SETTINGS.clockFormat == 1)) {
      const int clockWidth = renderer.getTextWidth(kStatusFont, clockText);
      renderer.drawText(kStatusFont, cursorX - clockWidth, lineY, clockText);
    }
  }

  // The band's one rule, flush with its bottom edge: the closing stroke of the
  // status strip, at the theme weight.
  if (rule > 0 && rect.width > 0 && rect.height > rule) {
    renderer.fillRect(rect.x, rect.y + rect.height - rule, rect.width, rule, true);
  }
}

// ---------------------------------------------------------------------------
// The rule under a titled band — the same stroke, stated once
// ---------------------------------------------------------------------------
void PickyOSTheme::drawTitledBandRule(const GfxRenderer& renderer, Rect band) const {
  const int rule = UITheme::getInstance().getMetrics().headerRuleThickness;
  if (rule <= 0 || band.width <= 0 || band.height <= rule) return;
  renderer.fillRect(band.x, band.y + band.height - rule, band.width, rule, true);
}

int PickyOSTheme::tabBarRuleThickness() const { return UITheme::getInstance().getMetrics().headerRuleThickness; }

int PickyOSTheme::tabActiveUnderlineThickness() const {
  return UITheme::getInstance().getMetrics().headerRuleThickness;
}

int PickyOSTheme::dialogRuleThickness() const { return UITheme::getInstance().getMetrics().headerRuleThickness; }

// ---------------------------------------------------------------------------
// Featured card
// ---------------------------------------------------------------------------
void PickyOSTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                       const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                       bool& bufferRestored, std::function<bool()> coverStore) const {
  (void)selectorIndex;    // the card is a single tap target: no per-tile selection
  (void)bufferRestored;   // a restored buffer already carries this card
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  const int sidePad = metrics.contentSidePadding;
  const int coverWidth = metrics.homeFeaturedCoverLargeWidth;
  const int coverHeight = metrics.homeFeaturedCoverLargeHeight;
  const int border = metrics.homeFeaturedCoverBorder;
  const int radius = metrics.homeFeaturedCoverRadius;

  const int coverX = rect.x + sidePad + border;  // the FRAME sits on the content rail
  const int coverY = rect.y + (rect.height - (coverHeight + 2 * border)) / 2 + border;
  const int frameX = coverX - border;
  const int frameY = coverY - border;
  const int frameW = coverWidth + 2 * border;
  const int frameH = coverHeight + 2 * border;
  const int textX = frameX + frameW + kCardTextGap;
  const int textWidth = rect.x + rect.width - sidePad - textX;

  const bool hasBook = !recentBooks.empty();

  if (!coverRendered && hasBook) {
    bool drewCover = false;
    if (!recentBooks[0].coverBmpPath.empty()) {
      const std::string coverPath = UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, metrics.homeCoverHeight);
      HalFile file;
      if (Storage.openFileForRead("HOME", coverPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          // Already dithered at this height: blit 1:1, crop the overflow. The
          // slot is grown a pixel on every side so the art runs UNDER the
          // stroke instead of stopping short of it.
          drewCover = drawCoverThumbFill(renderer, bitmap, Rect{coverX - kSeam, coverY - kSeam, coverWidth + 2 * kSeam,
                                                               coverHeight + 2 * kSeam});
        }
        file.close();
      }
    }
    if (!drewCover) {
      // No art, no shadow, no gradient: flat ink placeholder in the same slot.
      drawEmptyCover(renderer, Rect{coverX, coverY, coverWidth, coverHeight});
    }
    maskRoundedCorners(renderer, Rect{coverX - kSeam, coverY - kSeam, coverWidth + 2 * kSeam, coverHeight + 2 * kSeam},
                       std::max(0, radius - border) + kSeam);
    drawRoundedStroke(renderer, Rect{frameX, frameY, frameW, frameH}, border, radius);

    coverBufferStored = coverStore();
    coverRendered = coverBufferStored;
  }

  if (!hasBook) {
    // Nothing to feature: the card still exists — an empty framed slot and two
    // lines — so Home never opens with a hole where the featured book will be.
    const Rect artSlot{coverX - kSeam, coverY - kSeam, coverWidth + 2 * kSeam, coverHeight + 2 * kSeam};
    drawEmptyCover(renderer, Rect{coverX, coverY, coverWidth, coverHeight});
    maskRoundedCorners(renderer, artSlot, std::max(0, radius - border) + kSeam);
    drawRoundedStroke(renderer, Rect{frameX, frameY, frameW, frameH}, border, radius);
    const int lineHeight = renderer.getLineHeight(kLabelFont);
    const int blockTop = rect.y + (rect.height - lineHeight * 2) / 2;
    renderer.drawText(kLabelFont, textX, blockTop, tr(STR_NO_OPEN_BOOK));
    renderer.drawText(kStatusFont, textX, blockTop + lineHeight, tr(STR_START_READING));
    return;
  }

  const RecentBook& book = recentBooks[0];
  const int titleLineHeight = renderer.getLineHeight(kLabelFont);
  const int authorLineHeight = renderer.getLineHeight(kStatusFont);
  const int barHeight = metrics.homeFeaturedProgressBarHeight;
  const int barStroke = metrics.homeFeaturedProgressBarStroke;

  // wrappedText() fills the last allowed line with an ellipsis, so a long title
  // ends on line three rather than being cut mid-glyph.
  const auto titleLines = renderer.wrappedText(kLabelFont, book.title.c_str(), textWidth, 3);
  const int lineCount = static_cast<int>(titleLines.size());
  const int progress = featuredProgress(book.path);
  const bool hasProgress = progress >= 0;
  const int pct = std::clamp(progress, 0, 100);
  const bool hasAuthor = !book.author.empty();

  // Block height in whole line boxes: title lines, the author's line, the gap,
  // and the track. Centred against the FRAMED cover — the rect the eye reads as
  // the card — not against the band, which also holds the card's air.
  const int blockHeight =
      lineCount * titleLineHeight + (hasAuthor ? authorLineHeight : 0) + kFeaturedTextGap + barHeight;
  int y = frameY + (frameH - blockHeight) / 2;

  for (const auto& line : titleLines) {
    renderer.drawText(kLabelFont, textX, y, line.c_str());
    y += titleLineHeight;
  }

  if (hasAuthor) {
    std::string pctText;
    int pctWidth = 0;
    if (hasProgress) {
      pctText = std::to_string(pct) + "%";
      pctWidth = renderer.getTextWidth(kStatusFont, pctText.c_str());
    }
    // The author is truncated FIRST, so the two can never collide; the label
    // rides the author's line box, right-aligned on the track's own column.
    const int authorMaxWidth = pctWidth > 0 ? std::max(0, textWidth - pctWidth - kRuleLineGap) : textWidth;
    const auto author = renderer.truncatedText(kStatusFont, book.author.c_str(), authorMaxWidth);
    renderer.drawText(kStatusFont, textX, y, author.c_str());
    if (pctWidth > 0) {
      renderer.drawText(kStatusFont, textX + textWidth - pctWidth, y, pctText.c_str());
    }
    y += authorLineHeight;
  }

  // The track: an OUTLINE at the theme's stroke weight, then the fill inside it.
  // Never a hairline — at 3 % a hairline reads as an accidental hyphen, and the
  // outline is what says "bar" when the fill is empty.
  const int trackY = y + kFeaturedTextGap;
  renderer.fillRect(textX, trackY, textWidth, barStroke, true);
  renderer.fillRect(textX, trackY + barHeight - barStroke, textWidth, barStroke, true);
  renderer.fillRect(textX, trackY, barStroke, barHeight, true);
  renderer.fillRect(textX + textWidth - barStroke, trackY, barStroke, barHeight, true);
  const int innerWidth = textWidth - 2 * barStroke;
  if (hasProgress && pct > 0 && innerWidth > 0) {
    const int filled = std::max(1, innerWidth * pct / 100);
    renderer.fillRect(textX + barStroke, trackY + barStroke, std::min(filled, innerWidth), barHeight - 2 * barStroke,
                      true);
  }
}

int PickyOSTheme::homeCoverThumbHeight(const GfxRenderer& renderer) const {
  (void)renderer;
  return UITheme::getInstance().getMetrics().homeFeaturedCoverLargeHeight;
}

// ---------------------------------------------------------------------------
// Home menu
// ---------------------------------------------------------------------------
MenuLayout PickyOSTheme::getMenuLayout(const GfxRenderer& renderer, Rect band, int buttonCount,
                                       int selectedIndex) const {
  MenuLayout layout = BaseTheme::getMenuLayout(renderer, band, buttonCount, selectedIndex);
  // Capacity, not content: the band holds five rows, so a sixth entry is refused
  // here rather than silently re-flowing the home screen.
  layout.rows = std::min(buttonCount, kMenuRowCapacity);

  // The menu SPREADS over the band it has. At the authored pitch a five-row menu
  // would end 470 px above the bottom of a 1216 px panel; here the pitch is the
  // band divided by the rows, clamped so a row never grows past 1.5x its own
  // height (that cap is what stops four items scattering down the screen) and
  // never shrinks below the authored row. The block is then centred in the space
  // that is left. The touch grid consumes this same struct, so taps follow the
  // paint.
  if (layout.rows > 0) {
    const int usable = renderer.getScreenHeight() - band.y - kMenuBottomInset;
    const int maxPitch = layout.rowHeight + layout.rowHeight / 2;
    if (usable > 0) {
      // min(step, cap): the authored step is the floor, unless a theme ever sets
      // menuSpacing above the cap, in which case clamp() would be given lo > hi.
      const int floor = std::min(layout.step, maxPitch);
      layout.step = std::clamp(usable / layout.rows, floor, maxPitch);
      const int blockHeight = (layout.rows - 1) * layout.step + layout.rowHeight;
      layout.top = band.y + std::max(0, (usable - blockHeight) / 2);
    }
  }
  return layout;
}

void PickyOSTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                  const std::function<std::string(int index)>& buttonLabel,
                                  const std::function<UIIcon(int index)>& rowIcon) const {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  // The SAME layout HomeActivity maps taps with: paint and hit band are one
  // struct, so a row's icon cannot drift from where the finger lands.
  const MenuLayout layout = getMenuLayout(renderer, rect, buttonCount, selectedIndex);
  const int sidePad = metrics.contentSidePadding;
  const int iconSize = metrics.menuIconSize;
  const int iconX = rect.x + sidePad;
  const int labelX = iconX + iconSize + kMenuIconLabelGap;
  const int rule = metrics.headerRuleThickness;

  const int lastPainted = layout.firstIndex + layout.rows;
  // The right-edge indicator is the only truthful affordance about list length,
  // drawn only when the band cannot show every row. The chevrons step out of its
  // way rather than sharing the edge with it.
  const bool showIndicator = layout.rows < buttonCount;
  const int chevronRight = rect.x + rect.width - sidePad - (showIndicator ? metrics.chevronBoxWidth + 10 : 0);

  for (int i = layout.firstIndex; i < buttonCount && i < lastPainted; ++i) {
    const int rowTop = menuRowTop(layout, i);
    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    const int lineHeight = renderer.getLineHeight(kLabelFont);
    const int labelY = rowTop + (layout.rowHeight - lineHeight) / 2;
    const int iconY = labelY + (lineHeight - iconSize) / 2;

    drawMaskAtSize(renderer, rowIcon != nullptr ? iconFor(rowIcon(i)) : nullptr, iconX, iconY, iconSize);
    renderer.drawText(kLabelFont, labelX, labelY, label);
    if (metrics.chevronBoxWidth > 0) {
      drawChevron(renderer, chevronRight, labelY + lineHeight / 2, metrics.chevronBoxWidth, metrics.chevronBoxHeight,
                  metrics.chevronStroke);
    }

    // The row divider: the theme's one stroke, drawn in the air between this row
    // and the next, from the content rail to the content rail. Only when the
    // spread actually left air — at a compact pitch the rule would sit on the
    // next row's label box.
    const int gap = layout.step - layout.rowHeight;
    if (i + 1 < buttonCount && i + 1 < lastPainted && rule > 0 && gap > rule + 2) {
      const int ruleY = rowTop + layout.rowHeight + (gap - rule) / 2;
      renderer.fillRect(rect.x + sidePad, ruleY, rect.width - 2 * sidePad, rule, true);
    }
  }

  if (showIndicator) {
    const int barWidth = metrics.homeScrollIndicatorWidth;
    const int bandHeight = layout.rows * layout.step - (layout.step - layout.rowHeight);
    const int thumbHeight = std::max(layout.rowHeight / 4, bandHeight * layout.rows / buttonCount);
    const int maxFirst = std::max(1, buttonCount - layout.rows);
    const int thumbY = layout.top + (bandHeight - thumbHeight) * layout.firstIndex / maxFirst;
    renderer.fillRect(rect.x + rect.width - sidePad - barWidth, thumbY, barWidth, thumbHeight, true);
  }
}

// ---------------------------------------------------------------------------
// List / settings pitch
// ---------------------------------------------------------------------------
int PickyOSTheme::listTouchRowHeight() const { return uiDensity::px(PickyOSMetrics::values.listRowHeight); }

// The themed list pitch. One page whenever the list fits at the comfortable
// measure; compact rows only when the list genuinely cannot fit, and then the
// viewport scrolls (this theme has no pager). clamp(band / rows, min, max) is
// the whole rule, and it is the ONE number written into ListProps::rowHeight —
// paint, tap band and page size are one value by construction.
int PickyOSTheme::listRowFillPitch(const int bandHeight, const int rowCount) const {
  if (bandHeight <= 0 || rowCount <= 0) return 0;
  const int minPitch = uiDensity::px(PickyOSMetrics::values.listRowHeight);
  // 74 design -> 100 px: a row may grow into a short band, but a five-row list
  // does not become five 200 px cards.
  const int maxPitch = uiDensity::px(74);
  if (rowCount * minPitch <= bandHeight) {
    return std::min(bandHeight / rowCount, maxPitch);
  }
  return minPitch;
}
