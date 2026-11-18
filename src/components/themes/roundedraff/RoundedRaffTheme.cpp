#include "RoundedRaffTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UiDensity.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"

namespace {
constexpr int kCoverRadius = 18;
constexpr int kMenuRadius = 30;
constexpr int kBottomRadius = 15;
constexpr int kRowRadius = 20;
constexpr int kInteractiveInsetX = 20;
constexpr int kSelectableRowGap = 6;
constexpr int kTitleFontId = UI_12_FONT_ID;  // Requested main title size: 12px
constexpr int kGuideFontId = SMALL_FONT_ID;  // Closest available to requested 6px

void drawScrollBar(const GfxRenderer& renderer, Rect rect, int itemCount, int pageStartIndex, int pageItems) {
  if (itemCount <= 0 || pageItems <= 0 || itemCount <= pageItems) {
    return;
  }

  // Scaled metrics: the authored tables are DESIGN space (480x800 @ 235 PPI) and
  // r59 applies the density factor in UITheme::getMetrics(), so painting from
  // RoundedRaffMetrics::values here would draw a design-pixel scroll bar on a
  // denser panel while the rows around it are scaled.
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const int barW = metrics.scrollBarWidth;
  const int barX = rect.x + rect.width - metrics.scrollBarRightOffset - barW;
  const int barY = rect.y;
  const int barH = rect.height;

  const int thumbH = std::max(uiDensity::px(10), (barH * pageItems) / itemCount);
  const int maxStart = std::max(1, itemCount - pageItems);
  const int maxTravel = std::max(1, barH - thumbH);
  const int clampedStart = std::clamp(pageStartIndex, 0, maxStart);
  const int thumbY = barY + (clampedStart * maxTravel) / maxStart;

  renderer.fillRect(barX, thumbY, barW, thumbH);
}

}  // namespace
int coverWidth = 0;

void RoundedRaffTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,
                                  const bool backButton) const {
  // Home screen header is custom-rendered in drawRecentBookCover.
  if (title == nullptr) {
    return;
  }
  BaseTheme::drawHeader(renderer, rect, title, subtitle, backButton);
}

void RoundedRaffTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                           bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  // Scaled metrics for the tile inset: contentSidePadding is a Design length, so
  // the drawn card must use the converted value or it would sit at the design
  // margin while the menu rows below it are scaled. homeCoverHeight stays on the
  // DESIGN table on purpose: it is MetricScale::Physical (a bitmap drawn 1:1 and
  // part of the thumbnail cache key), so it is exempt from the density sweep.
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const int tileWidth = rect.width - 2 * metrics.contentSidePadding;
  const int tileHeight = rect.height;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();
  if (coverWidth == 0) {
    coverWidth = RoundedRaffMetrics::values.homeCoverHeight * 0.6;  // Physical: 1:1 bitmap height
  }
  const int imgY = tileY + (tileHeight - RoundedRaffMetrics::values.homeCoverHeight) / 2;
  const int tileX = rect.x + metrics.contentSidePadding;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    RecentBook book = recentBooks[0];
    if (!coverRendered) {
      std::string coverPath = book.coverBmpPath;
      bool hasCover = true;
      if (coverPath.empty()) {
        hasCover = false;
      } else {
        const std::string coverBmpPath =
            UITheme::getCoverThumbPath(coverPath, RoundedRaffMetrics::values.homeCoverHeight);

        // First time: load cover from SD and render
        HalFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            coverWidth = bitmap.getWidth();
            // Narrow covers come out taller than the slot; fill 1:1 and crop
            // vertically instead of rescaling the dither.
            drawCoverThumbFill(renderer, bitmap,
                               Rect{tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                                    RoundedRaffMetrics::values.homeCoverHeight});
            renderer.maskRoundedRectOutsideCorners(tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                                                   RoundedRaffMetrics::values.homeCoverHeight, kCoverRadius,
                                                   Color::LightGray);
          } else {
            hasCover = false;
          }
          file.close();
        }
      }

      // Draw either way
      renderer.drawRoundedRect(tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                               RoundedRaffMetrics::values.homeCoverHeight, 1, kCoverRadius, true);

      if (!hasCover) {
        // Render empty cover
        renderer.fillRect(tileX + (tileWidth - coverWidth) / 2, imgY + (RoundedRaffMetrics::values.homeCoverHeight / 3),
                          coverWidth, 2 * RoundedRaffMetrics::values.homeCoverHeight / 3, true);
        renderer.drawIcon(CoverIcon, tileX + (tileWidth - coverWidth) / 2 + 24, imgY + 24, 32);
        renderer.maskRoundedRectOutsideCorners(tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                                               RoundedRaffMetrics::values.homeCoverHeight, kCoverRadius,
                                               Color::LightGray);
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    renderer.fillRoundedRect(tileX, tileY, tileWidth, imgY - tileY, kRowRadius, true, true, false, false,
                             Color::LightGray);
    renderer.fillRectDither(tileX, imgY, (tileWidth - coverWidth) / 2, RoundedRaffMetrics::values.homeCoverHeight,
                            Color::LightGray);
    renderer.fillRectDither(tileX + (tileWidth + coverWidth) / 2, imgY, (tileWidth - coverWidth) / 2,
                            RoundedRaffMetrics::values.homeCoverHeight, Color::LightGray);
    renderer.fillRoundedRect(tileX, imgY + RoundedRaffMetrics::values.homeCoverHeight, tileWidth,
                             tileHeight - (imgY - tileY + RoundedRaffMetrics::values.homeCoverHeight), kRowRadius,
                             false, false, true, true, Color::LightGray);
  } else {
    renderer.fillRoundedRect(tileX, tileY, tileWidth, tileHeight, kRowRadius, Color::LightGray);
    renderer.drawCenteredText(kTitleFontId, rect.y + rect.height / 2 - renderer.getLineHeight(kTitleFontId) / 2,
                              tr(STR_NO_OPEN_BOOK));
  }
}

int RoundedRaffTheme::getMenuRowHeight(const GfxRenderer& renderer) const {
  return renderer.getLineHeight(kTitleFontId) + 20;  // 10px top + 10px bottom
}

int RoundedRaffTheme::getMenuRowStep(const GfxRenderer& renderer) const {
  // The card gap is this theme's own design-space constant, not a metrics field.
  return getMenuRowHeight(renderer) + kSelectableRowGap;
}

MenuLayout RoundedRaffTheme::getMenuLayout(const GfxRenderer& renderer, const Rect band, const int buttonCount,
                                           const int selectedIndex) const {
  MenuLayout layout;
  layout.top = band.y;
  layout.rowHeight = getMenuRowHeight(renderer);
  layout.step = getMenuRowStep(renderer);
  // This theme pages the card list: the page holding the selection is the one
  // painted, so the touch grid pages with the same arithmetic.
  const int pageItems = std::max(1, band.height / layout.step);
  const int safeSelected = std::max(0, selectedIndex);
  layout.firstIndex = (safeSelected / pageItems) * pageItems;
  const int remaining = std::max(0, buttonCount - layout.firstIndex);
  layout.rows = std::min(pageItems, remaining);
  return layout;
}

void RoundedRaffTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                      const std::function<std::string(int index)>& buttonLabel,
                                      const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rowIcon;
  // One geometry source: HomeActivity hit-tests getMenuLayout(), this paints it.
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const int sidePadding = metrics.contentSidePadding;
  const int rowX = rect.x + sidePadding;
  const MenuLayout layout = getMenuLayout(renderer, rect, buttonCount, selectedIndex);
  const int rowHeight = layout.rowHeight;
  const int pageItems = std::max(1, rect.height / layout.step);  // for the scroll bar only
  const int textLineHeight = renderer.getLineHeight(kTitleFontId);
  const int menuMaxWidth = std::max(0, rect.width - sidePadding * 2);

  for (int i = layout.firstIndex; i < buttonCount && i < layout.firstIndex + layout.rows; ++i) {
    const std::string label = buttonLabel(i);
    const int rowY = menuRowTop(layout, i);
    const int kRowPaddingX = uiDensity::px(40);  // 20px L/R on the design canvas
    const int maxLabelWidth = std::max(0, menuMaxWidth - kRowPaddingX);
    const std::string truncatedLabel =
        renderer.truncatedText(kTitleFontId, label.c_str(), maxLabelWidth, EpdFontFamily::BOLD);
    const int rowWidth = std::min(
        menuMaxWidth, renderer.getTextWidth(kTitleFontId, truncatedLabel.c_str(), EpdFontFamily::BOLD) + kRowPaddingX);
    const bool isSelected = selectedIndex == i;
    renderer.fillRoundedRect(rowX, rowY, rowWidth, rowHeight, uiDensity::px(kMenuRadius),
                             isSelected ? Color::Black : Color::White);
    const int textY = rowY + (rowHeight - textLineHeight) / 2;
    const int textX = rowX + uiDensity::px(kInteractiveInsetX);
    if (selectedIndex == i) {
      renderer.drawText(kTitleFontId, textX, textY, truncatedLabel.c_str(), false, EpdFontFamily::BOLD);
    } else {
      renderer.drawText(kTitleFontId, textX, textY, truncatedLabel.c_str(), true, EpdFontFamily::BOLD);
    }
  }

  drawScrollBar(renderer, rect, buttonCount, layout.firstIndex, pageItems);
}

void RoundedRaffTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                                     int contentStartX, int contentWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineY = rect.y + rect.height + lineHeight + metrics.verticalSpacing;
  const int thickness = cursorMode ? 3 : 2;

  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY, rect.x + contentStartX + contentWidth - 1, lineY, thickness, true);
    return;
  }

  constexpr int hPadding = 8;
  const int lineW = textWidth + hPadding * 2;
  const int lineStart = rect.x + (rect.width - lineW) / 2;
  renderer.drawLine(lineStart, lineY, lineStart + lineW - 1, lineY, thickness, true);
}

void RoundedRaffTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                       const char* btn4) const {
  if (gpio.hasTouch()) {
    return;
  }

  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  // Hint band geometry is design space: convert it with uiDensity::px() so the
  // guides keep their physical size on a denser panel (nothing here reads the
  // scaled table because this bar also exists on non-touch boards, where the
  // scale is resolved from the same factor).
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const int sidePadding = uiDensity::px(20);
  const int groupGap = uiDensity::px(10);
  const int bottomMargin = uiDensity::px(10);
  const int hintHeight = metrics.buttonHintsHeight - uiDensity::px(10);  // 30px total guide height on the design canvas
  const int groupWidth = (pageWidth - sidePadding * 2 - groupGap) / 2;
  const int hintY = pageHeight - hintHeight - bottomMargin;
  const int textY = hintY + (hintHeight - renderer.getLineHeight(kGuideFontId)) / 2;

  if (renderer.getRenderMode() != GfxRenderer::BW && !renderer.grayPlanesAreAbsolute()) {
    renderer.fillRect(sidePadding, hintY, groupWidth, hintHeight, true);
    renderer.fillRect(sidePadding + groupWidth + groupGap, hintY, groupWidth, hintHeight, true);
    renderer.setOrientation(origOrientation);
    return;
  }

  const bool backDisabled = (btn1 == nullptr || btn1[0] == '\0');
  const int leftGroupX = sidePadding;
  const int rightGroupX = leftGroupX + groupWidth + groupGap;
  const std::string backLabel = backDisabled ? "" : std::string(btn1);
  // Callers should provide the button labels. If a label is not specified, it should render empty.
  const std::string selectText = (btn2 && btn2[0] != '\0') ? std::string(btn2) : "";
  const std::string upText = (btn3 && btn3[0] != '\0') ? std::string(btn3) : "";
  const std::string downText = (btn4 && btn4[0] != '\0') ? std::string(btn4) : "";

  // Ensure button hints always "win" visually even if other elements accidentally render into this area.
  renderer.fillRect(leftGroupX, hintY, groupWidth, hintHeight, false);
  renderer.fillRect(rightGroupX, hintY, groupWidth, hintHeight, false);

  renderer.drawRoundedRect(leftGroupX, hintY, groupWidth, hintHeight, 2, uiDensity::px(kBottomRadius), true);
  const int selectWidth = renderer.getTextWidth(kGuideFontId, selectText.c_str(), EpdFontFamily::REGULAR);
  const int downWidth = renderer.getTextWidth(kGuideFontId, downText.c_str(), EpdFontFamily::REGULAR);
  const int innerEdgePadding = uiDensity::px(16);  // design-space text inset

  const int backX = leftGroupX + innerEdgePadding;
  const int selectX = leftGroupX + groupWidth - innerEdgePadding - selectWidth;
  const int upX = rightGroupX + innerEdgePadding;
  const int downX = rightGroupX + groupWidth - innerEdgePadding - downWidth;

  if (!backDisabled) {
    renderer.drawText(kGuideFontId, backX, textY, backLabel.c_str(), true, EpdFontFamily::REGULAR);
  }
  renderer.drawText(kGuideFontId, selectX, textY, selectText.c_str(), true, EpdFontFamily::REGULAR);

  renderer.drawRoundedRect(rightGroupX, hintY, groupWidth, hintHeight, 2, uiDensity::px(kBottomRadius), true);

  renderer.drawText(kGuideFontId, upX, textY, upText.c_str(), true, EpdFontFamily::REGULAR);
  renderer.drawText(kGuideFontId, downX, textY, downText.c_str(), true, EpdFontFamily::REGULAR);

  renderer.setOrientation(origOrientation);
}
