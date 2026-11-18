#include "BaseTheme.h"

#include <BoardConfig.h>
#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include <Utf8.h>

#include "I18n.h"
#include "RecentBooksStore.h"
#include "clippings/SelectionGeometry.h"
#include "components/HeaderBackTapTarget.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/bookmark.h"
#include "components/icons/cover.h"
#include "components/icons/headerIcons.h"
#include "components/icons/headerIcons56.h"
#include "display/BatteryIconRegion.h"
#include "display/EqualizeBlack.h"
#include "display/RefreshPolicy.h"
#include "fontIds.h"
#include "components/TitledHeaderBand.h"

int BaseTheme::titledHeaderPx() const {
  const int h = titledHeaderHeight();
  return h > 0 ? h : UITheme::getInstance().getMetrics().headerHeight;
}

// Every codepoint of `text` must be drawable by the face, or the caller keeps
// the shared UI font: NO non-Latin fallback is registered for the Ember ids
// (SdCardFontSystem maps only SMALL/UI_10/UI_12), so an unguarded Ember title
// prints holes in Hebrew, Arabic, Greek or CJK. Same rule r73 applied to
// settings row labels; r77 extends it to every Ember string on Home.
//
// A member (static) rather than a file-local helper because AndroTheme asks the
// same question of the same loaded face for its menu labels, its featured
// title/author and its status line -- guard PER STRING, since a date in C-locale
// abbreviations always fits while a book title often does not.
bool BaseTheme::textFitsFace(const GfxRenderer& renderer, const int fontId, const char* text) {
  if (text == nullptr) return false;
  const auto& faces = renderer.getFontMap();
  const auto it = faces.find(fontId);
  if (it == faces.end()) return false;  // face not loaded: keep the UI font
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text);
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&cursor)) != 0) {
    if (cp >= 0x80 && !it->second.hasCodepoint(cp, EpdFontFamily::REGULAR)) return false;
  }
  return true;
}

void BaseTheme::setCheckboxRow(freeink::ui::ListItem& item, const bool checked) {
  item.value = nullptr;
  item.toggle = true;
  item.toggleChecked = checked;
}

// Internal constants
namespace {
constexpr int homeMenuMargin = 20;
constexpr int homeMarginTop = 30;
constexpr int subtitleY = 738;
constexpr int bookmarkStatusIconHeight = 14;
constexpr int bookmarkStatusIconTopCrop = 2;

}  // namespace

void BaseTheme::drawBookmarkStatusIcon(const GfxRenderer& renderer, const int x, const int y) {
  constexpr int bytesPerRow = bookmarkStatusIconWidth / 8;
  for (int row = 0; row < bookmarkStatusIconHeight; ++row) {
    for (int col = 0; col < bookmarkStatusIconWidth; ++col) {
      const uint8_t byte = BookmarkStatusIcon[(row + bookmarkStatusIconTopCrop) * bytesPerRow + col / 8];
      const uint8_t mask = 1U << (7 - (col % 8));
      renderer.drawPixel(x + col, y + row, (byte & mask) != 0);
    }
  }
}

void BaseTheme::drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight) {
  // r163 D3: a status glyph, so it is binary BY ROLE -- whatever draws it. Every theme
  // funnels the battery outline through here (the same reason the repaint record below
  // lives here), so one tag covers the whole status bar.
  const GfxRenderer::ScopedContentRole role(renderer, GfxRenderer::ContentRole::Icon);
#if CP_ICON_SAFE_PATCH
  // BATT5: record the glyph's geometry ON THE RENDERER, which also owns the epochs that
  // say whether this frame reached the glass (GfxRenderer::BatteryIconRect). Recording
  // here -- the ONE primitive every theme funnels the outline through -- is what gives
  // the patch an exact rectangle instead of a re-derived one.
  renderer.noteBatteryIconRect(x, y, battWidth, rectHeight);
#elif CP_ICON_PARTIAL_REPAINT
  // CP_ICON_PARTIAL_REPAINT: this is the ONE primitive every theme funnels the
  // battery glyph through (BaseTheme::drawBatteryLeft, AndroTheme's status bar and
  // header, Lyra's), so recording here costs one call per paint and gives the
  // repaint path an exact rectangle instead of a re-derived one. The nub is drawn
  // at x + battWidth, so the glyph's ink is (battWidth + 1) wide. The record is
  // cleared at the top of every activity paint, so a screen that never reaches this
  // line has no rect and the repaint is skipped -- see display/BatteryIconRegion.h.
  cpBatteryIconNotePaint(x, y, battWidth, rectHeight);
#endif
  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + rectHeight - 1, x + battWidth - 3, y + rectHeight - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rectHeight - 2);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rectHeight - 2);
  renderer.drawPixel(x + battWidth - 1, y + 3);
  renderer.drawPixel(x + battWidth - 1, y + rectHeight - 4);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rectHeight - 5);
}

// BATT6: the percentage NUMBER, drawn and RECORDED in one place.
//
// Every theme prints the digits through this call, so recording the box here costs
// one store per paint and gives the change producer an exact rectangle -- the same
// argument that put the glyph's record in drawBatteryOutline. The record carries the
// value printed (not the value "intended") because that is what the producer must
// compare the gauge against: if this paint drives, the glass shows exactly `value`.
void BaseTheme::drawBatteryPercentLabel(const GfxRenderer& renderer, int anchorX, int y, int fontId,
                                        const PercentAlign align, const std::string& text, uint16_t value) {
  const int textWidth = renderer.getTextWidth(fontId, text.c_str());
  const bool right = align == PercentAlign::Right;
  const int penX = right ? anchorX - textWidth : anchorX;
#if CP_BATT_PERCENT_PATCH
  GfxRenderer::BatteryPercentRect rec;
  rec.x = static_cast<int16_t>(penX);
  rec.y = static_cast<int16_t>(y);
  rec.w = static_cast<int16_t>(textWidth);
  rec.h = static_cast<int16_t>(renderer.getLineHeight(fontId));
  rec.anchorX = static_cast<int16_t>(anchorX);
  rec.fontId = static_cast<int16_t>(fontId);
  rec.value = static_cast<int16_t>(value);
  rec.align = right ? GfxRenderer::BatteryPercentRect::Right : GfxRenderer::BatteryPercentRect::Left;
  rec.valid = textWidth > 0 && rec.h > 0;
  renderer.noteBatteryPercentRect(rec);
#endif
  renderer.drawText(fontId, penX, y, text.c_str());
}

void BaseTheme::drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY) {
  // r163 D3: icon, binary by role (see drawBatteryOutline).
  const GfxRenderer::ScopedContentRole role(renderer, GfxRenderer::ContentRole::Icon);
  // Draw lightning bolt (white/inverted on black fill for visibility)
  renderer.drawLine(boltX + 4, boltY + 0, boltX + 5, boltY + 0, false);
  renderer.drawLine(boltX + 3, boltY + 1, boltX + 4, boltY + 1, false);
  renderer.drawLine(boltX + 2, boltY + 2, boltX + 5, boltY + 2, false);
  renderer.drawLine(boltX + 3, boltY + 3, boltX + 4, boltY + 3, false);
  renderer.drawLine(boltX + 2, boltY + 4, boltX + 3, boltY + 4, false);
  renderer.drawLine(boltX + 1, boltY + 5, boltX + 4, boltY + 5, false);
  renderer.drawLine(boltX + 2, boltY + 6, boltX + 3, boltY + 6, false);
  renderer.drawLine(boltX + 1, boltY + 7, boltX + 2, boltY + 7, false);
}

void BaseTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  // The bolt means "charging", not "cable attached" (powerManager.isCharging()).
  // r163 E3: and it is the DISPLAYED flag, so a pack sitting full on the cable (the CV
  // tail) draws 100 % with no bolt instead of claiming it is still filling.
  const bool charging = powerManager.isChargingDisplayed();

  const int maxFillWidth = rect.width - 5;
  const int fillHeight = rect.height - 4;
  if (maxFillWidth <= 0 || fillHeight <= 0) {
    return;
  }
  // +1 to round up so we always fill at least one pixel
  int filledWidth = percentage * maxFillWidth / 100 + 1;
  if (filledWidth > maxFillWidth) {
    filledWidth = maxFillWidth;
  }

  // When charging, ensure minimum fill so lightning bolt is fully visible
  constexpr int minFillForBolt = 8;
  if (charging && filledWidth < minFillForBolt) {
    filledWidth = std::min(minFillForBolt, maxFillWidth);
  }

  renderer.fillRect(rect.x + 2, rect.y + 2, filledWidth, fillHeight);

  if (charging) {
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
  }
}

void BaseTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Left aligned: icon on left, percentage on right (reader mode)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  // Centre the icon on the digits' ink rather than at a fixed y: the text line
  // sits a font-dependent distance below rect.y, and that distance scales with
  // uiScale, so a constant drop leaves the glyph high on a denser panel.
  int y = rect.y + 6;
  const auto& fonts = renderer.getFontMap();
  const auto fontIt = fonts.find(SMALL_FONT_ID);
  if (fontIt != fonts.end()) {
    if (const EpdGlyph* glyph = fontIt->second.getGlyph('0', EpdFontFamily::REGULAR)) {
      const int inkTop = renderer.getFontAscenderSize(SMALL_FONT_ID) - static_cast<int>(glyph->top);
      y = rect.y + inkTop + (static_cast<int>(glyph->height) - rect.height) / 2;
    }
  }

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textX = rect.x + batteryPercentSpacing + rect.width;
    drawBatteryPercentLabel(renderer, textX, rect.y, SMALL_FONT_ID, PercentAlign::Left, percentageText, percentage);
  }

  const Rect iconRect{rect.x, y, rect.width, rect.height};
  drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height);
  fillBatteryIcon(renderer, iconRect, percentage);
}

void BaseTheme::drawCoverPlaceholder(const GfxRenderer& renderer, Rect rect) {
  if (rect.width <= 0 || rect.height <= 0) return;
  const int topHeight = rect.height / 3;
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  renderer.fillRect(rect.x, rect.y + topHeight, rect.width, rect.height - topHeight, true);
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
  constexpr int ICON_SIZE = 32;
  if (rect.width >= ICON_SIZE + 4 && topHeight >= ICON_SIZE + 4) {
    const int insetX = std::min(24, (rect.width - ICON_SIZE) / 2);
    const int insetY = std::min(24, (topHeight - ICON_SIZE) / 2);
    renderer.drawIcon(CoverIcon, rect.x + insetX, rect.y + insetY, ICON_SIZE);
  }
}

bool BaseTheme::drawCoverThumbFill(const GfxRenderer& renderer, const Bitmap& bitmap, Rect slot, const int xOffset) {
  if (slot.width <= 0 || slot.height <= 0) return false;
  // xOffset nudges the centered art sideways; the clip stays on the slot.
  const int x = slot.x + (slot.width - bitmap.getWidth()) / 2 + xOffset;
  const int y = slot.y + (slot.height - bitmap.getHeight()) / 2;
  const auto clip = renderer.getClipRect();
  const int left = std::max(slot.x, clip[0]);
  const int top = std::max(slot.y, clip[1]);
  renderer.setClipRect(left, top, std::max(0, std::min(slot.x + slot.width, clip[0] + clip[2]) - left),
                       std::max(0, std::min(slot.y + slot.height, clip[1] + clip[3]) - top));
  // r163 D3: a cover is VISIBLE CONTENT, tagged `Cover`, so the grey lane admits its
  // levels. It is the one element this batch opts into grey explicitly -- and it was
  // already grey on an AA paint, so the tag records the intent rather than changing it.
  const GfxRenderer::ScopedContentRole role(renderer, GfxRenderer::ContentRole::Cover);
  // r164 THUMB1 - the grey plane. Since r164 a Home thumb is a 2 bpp file whose four
  // palette indices are optical levels 0/5/10/15; without this flag drawBitmap takes its
  // `renderMode == BW` branch and collapses all four to ink, which is strictly worse than
  // the 1 bpp halftone it replaces (investigation 3.1: mean 10.6 vs 22.9). The flag is
  // scoped to this one blit and restored either way.
  //
  // WHICH CONFIGURATIONS GET GREY, AND WHY THIS ADDS NO REFRESH CHANGE EITHER WAY:
  //  * menu AA on (the default): the composed Home frame ALREADY carries intermediate
  //    nibbles from the glyphs, so the panel's existing per-frame selection (useGray8,
  //    keyed off any nibble other than 0x0/0xF) is the one it already takes. The cover
  //    adds content to a grey frame, not a drive mode.
  //  * menu AA off: the paint is declared binary and the ONE choke point every explicit
  //    level passes through -- commitPixelPhysical() -- thresholds `level < 8 ? ink :
  //    paper` before the nibble reaches the canvas ("a 16-level cover thumbnail
  //    included", its own comment). So the canvas stays binary, `frameHasGrey` stays
  //    false, the drive stays DU, and the cover is cut at 50 % OF ITS REAL TONE (levels
  //    0/5 -> ink, 10/15 -> paper) instead of at the 2-bit `val < 3` collapse.
  // So no paint changes which waveform it lands on, and nothing in the drive layer is
  // touched. The one thing that DOES change is that a default Home paint no longer
  // composes the cover as solid ink.
  //
  // THE FALLBACK IS DELIBERATE, and it is what keeps a memory squeeze from producing a
  // blank tile: the 16-level read needs a row scratch of (w+1)/2 + rowBytes where the
  // 2-bit path needs (w+3)/4 + rowBytes. If that allocation fails, drawBitmap returns
  // false and we redraw the SAME bitmap through the ordinary 2-bit path, which thresholds
  // to ink - the tile degrades to today's halftone look rather than disappearing. The
  // callers draw a placeholder only if BOTH fail.
  bool drawn = false;
  if (pixfmt::kGray16Covers && GfxRenderer::fourBitCanvas()) {
    const bool previous = renderer.sixteenLevelImages();
    renderer.setSixteenLevelImages(true);
    drawn = renderer.drawBitmap(bitmap, x, y, bitmap.getWidth(), bitmap.getHeight());
    renderer.setSixteenLevelImages(previous);
  }
  if (!drawn) drawn = renderer.drawBitmap(bitmap, x, y, bitmap.getWidth(), bitmap.getHeight());
  renderer.setClipRect(clip[0], clip[1], clip[2], clip[3]);
  return drawn;
}

void BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current, const size_t total) {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  LOG_DBG("UI", "Drawing progress bar: current=%u, total=%u, percent=%d", current, total, percent);
  // Draw outline
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  // Draw filled portion
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  }

  // Draw percentage text centered below bar
  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText.c_str());
}

// Centre hint labels by text height, wrapping to at most two lines.
void BaseTheme::drawHintLabel(const GfxRenderer& renderer, const int fontId, const char* label, const int x,
                              const int boxWidth, const int boxTop, const int boxHeight) {
  constexpr int textPadding = 4;  // keeps a wrapped label off the button's border
  const int maxTextWidth = boxWidth - (textPadding * 2);

  const int textHeight = renderer.getTextHeight(fontId);
  const int textWidth = renderer.getTextWidth(fontId, label);
  if (textWidth <= maxTextWidth) {
    const int textY = boxTop + std::max(1, (boxHeight - textHeight) / 2);
    renderer.drawText(fontId, x + (boxWidth - 1 - textWidth) / 2, textY, label);
    return;
  }

  // Spaced by the glyph height, not getLineHeight() — that returns the font's
  // full advanceY (leading included), which stacks two lines taller than the
  // button and clips the second one.
  constexpr int lineGap = 2;
  const int step = textHeight + lineGap;
  const auto lines = renderer.wrappedText(fontId, label, maxTextWidth, 2);
  const int block = static_cast<int>(lines.size()) * step - lineGap;
  int lineY = boxTop + std::max(1, (boxHeight - block) / 2);
  for (const auto& line : lines) {
    const int lineWidth = renderer.getTextWidth(fontId, line.c_str());
    renderer.drawText(fontId, x + (boxWidth - 1 - lineWidth) / 2, lineY, line.c_str());
    lineY += step;
  }
}

void BaseTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  if (gpio.hasTouch()) {
    return;
  }

  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 106;
  // Band height/thickness from the derived (scaled) metrics, not the authored
  // table: this theme owns the band, and the band is what
  // metrics.buttonHintsHeight reserves in getScreenSafeArea().
  const int buttonHeight = UITheme::getInstance().getMetrics().buttonHintsHeight;
  const int buttonY = buttonHeight;  // Distance from bottom
  // Keyed to the portrait panel width: the 528-wide X3 gets more spacing than
  // the 480-wide boards (X4, X4 Pro, and the other 800x480 panels).
  constexpr int narrowButtonPositions[] = {25, 130, 245, 350};
  constexpr int wideButtonPositions[] = {38, 154, 268, 384};
  const int* buttonPositions = renderer.getScreenWidth() >= 528 ? wideButtonPositions : narrowButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};
  const bool grayscale = renderer.getRenderMode() != GfxRenderer::BW && !renderer.grayPlanesAreAbsolute();

  for (int i = 0; i < 4; i++) {
    // Only draw if the label is non-empty
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[i];
      // Zero gray-plane bits leave the monochrome hint from the base pass intact.
      renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, grayscale);
      if (grayscale) continue;
      renderer.drawRect(x, pageHeight - buttonY, buttonWidth, buttonHeight);
      drawHintLabel(renderer, UI_10_FONT_ID, labels[i], x, buttonWidth, pageHeight - buttonY, buttonHeight);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void BaseTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  if (gpio.hasTouch()) {
    return;
  }

  const int screenWidth = renderer.getScreenWidth();
  // Side hints are only painted on button boards, whose density factor is 1.0 by
  // board profile, but read the derived metrics anyway so a future scaled button
  // board cannot paint a band the layout did not reserve.
  const int buttonWidth = UITheme::getInstance().getMetrics().sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 4;

  if (gpio.hasEdgeSideButtons()) {
    // Edge-button layout (X3, X4 Pro): Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      const int leftX = buttonMargin;
      renderer.drawRect(leftX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = leftX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonMargin - buttonWidth;
      renderer.drawRect(rightX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = rightX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    constexpr int topButtonY = 345;
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonMargin - buttonWidth;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY, x + buttonWidth - 1, topButtonY);
      renderer.drawLine(x, topButtonY, x, topButtonY + buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY, x + buttonWidth - 1, topButtonY + buttonHeight - 1);
    }

    if ((topBtn != nullptr && topBtn[0] != '\0') || (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
      renderer.drawLine(x, topButtonY + buttonHeight, x + buttonWidth - 1, topButtonY + buttonHeight);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY + buttonHeight, x, topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY + buttonHeight, x + buttonWidth - 1,
                        topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x, topButtonY + 2 * buttonHeight - 1, x + buttonWidth - 1, topButtonY + 2 * buttonHeight - 1);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topButtonY + i * buttonHeight;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
        const int textX = x + (buttonWidth - textHeight) / 2;
        const int textY = y + (buttonHeight + textWidth) / 2;
        renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, labels[i]);
      }
    }
  }
}

// Slightly inside the side padding: the battery's boxed glyph and the clock
// digits read wider than text/cover ink on the same line, so flush placement
// looks like it overhangs the content columns.
int BaseTheme::headerStatusInset() { return UITheme::getInstance().getMetrics().headerSidePadding + 4; }

void BaseTheme::applyHeaderStatus(const GfxRenderer& renderer, freeink::ui::HeaderProps& props) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  auto& status = props.status;

  // Status text stays at the fixed small font on every screen: FONT_LABEL is
  // bound to SMALL_FONT_ID by makeUiTarget() (FUI screens) and drawHeader()
  // (passive frames), while the uiScale FONT_SMALL is for list subtitles.
  status.battery.text.font = freeink::ui::GfxRendererTarget::FONT_LABEL;

  status.showBattery = true;
  const uint16_t percentage = powerManager.getBatteryPercentage();
  status.battery.percent = static_cast<uint8_t>(percentage > 100 ? 100 : percentage);
  status.battery.charging = powerManager.isChargingDisplayed();
  // Static label buffers: headers draw on the single render task, and the
  // strings only need to outlive the fui::header() call that consumes them.
  static char percentText[8];
  if (SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS) {
    // BATT6: format the CLAMPED value, the same one the glyph is filled from and the
    // one the label's box/value record carries (components/TitledHeaderBand.h). A
    // gauge read above 100 is not a percentage; the bar has always clamped, and the
    // digits now agree with it instead of printing a number the fill cannot show.
    snprintf(percentText, sizeof(percentText), "%u%%", static_cast<unsigned>(status.battery.percent));
    status.battery.label = percentText;
  }
  status.battery.glyphWidth = static_cast<int16_t>(metrics.batteryWidth);
  status.battery.glyphHeight = static_cast<int16_t>(metrics.batteryHeight);
  status.battery.gap = batteryPercentSpacing;
  status.batteryLeft = metrics.headerBatterySide == 1;
  status.edgeInset = static_cast<int16_t>(headerStatusInset());

  // Header chrome geometry. Status lives on the theme's thin top strip in
  // fixed corners — battery top-right, a corner clock (headerClockCentered =
  // false) top-left, a centered clock top-center — and never repositions.
  // The content row (title, back arrow, trailing action buttons) centers on
  // the region between the strip and the band bottom, so its padding reads
  // as balanced under the status line rather than against the full band.
  // Text hangs low in its line cell by the font's internal leading; the icon
  // buttons drop by that amount to align with the glyphs the user sees.
  // Buttons keep a standard square (a band-8 button dwarfs its 24px icon);
  // the boxes are invisible and minTouchSize pads the tap target.
  constexpr int16_t headerButtonSize = 48;
  // Band geometry comes from the single accessor (getHeaderRect): the status
  // line and everything sized off the band then move together when the board's
  // UI scale changes (headerHeight 84 -> 113 on Plus), instead of re-adding
  // topPadding + headerHeight here and drifting from the callers.
  const Rect band = UITheme::getInstance().getHeaderRect(renderer);
  const int16_t bandHeight = static_cast<int16_t>(band.height);
  const int16_t strip = static_cast<int16_t>(metrics.batteryBarHeight);
  const int titleFontId = uiScaleSpec().titleFontId;
  const int16_t opticalDrop =
      static_cast<int16_t>((renderer.getLineHeight(titleFontId) - renderer.getTextHeight(titleFontId)) / 2);
  props.leadingSize = headerButtonSize;
  props.trailingSize = headerButtonSize;
  props.leadingStyles = freeink::ui::plainStyles(freeink::ui::Paint::solid(freeink::ui::Color::Black));
  props.actionOffsetY = static_cast<int16_t>(strip + (bandHeight - strip - headerButtonSize) / 2 - 4 + opticalDrop);
  // Shift the title's band-centered box down by half the strip: its center
  // lands on the below-strip region's midline with the buttons.
  // headerBand lowers only the band's top edge, which moves a centered title
  // half as far as the buttons; the second half keeps the title on their line.
  props.titleOffsetY = static_cast<int16_t>(strip / 2 + BoardConfig::ACTIVE.headerOffsetY / 2);
  status.stripHeight = strip;
  status.clockCentered = metrics.headerClockCentered;

  // Header clock, opposite the battery, on every screen that draws this band
  // (SETTINGS.clockShowInHeader). Themes whose title layout has no room for
  // the clock's left reserve opt out via headerShowsClock.
  // Status-bar content (operator decision, recorded only - the theme for it is
  // not implemented yet): date on the left, clock on the right, and the
  // progress bar together with its percentage both shown.
  static char clockText[10];
  if (metrics.headerShowsClock && SETTINGS.clockShowInHeader && halClock.isAvailable() &&
      halClock.formatTime(clockText, sizeof(clockText), SETTINGS.clockFormat == 1)) {
    status.clockText = clockText;
  }
}

freeink::ui::Rect BaseTheme::headerBand(const GfxRenderer& renderer, const freeink::ui::Rect rect,
                                        freeink::ui::HeaderProps& props) {
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const auto l = static_cast<int16_t>(left);
  const auto r = static_cast<int16_t>(right);
  const int16_t offset = BoardConfig::ACTIVE.headerOffsetY;
  props.bleed = freeink::ui::Insets{0, r, 0, l};
  // Only the status line sits up in the rounded corners; it alone gets their clearance.
  const int corner = BoardConfig::ACTIVE.cornerRadius;
  props.status.edgeInset = static_cast<int16_t>(props.status.edgeInset + std::max(0, corner - std::min(left, right)));
  return freeink::ui::Rect{static_cast<int16_t>(rect.x + l), static_cast<int16_t>(rect.y + offset),
                           static_cast<int16_t>(rect.width - l - r), static_cast<int16_t>(rect.height - offset)};
}

void BaseTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,
                           const bool backButton) const {
  // Every activity header renders through the FreeInkUI header + battery
  // indicator components, styled by the active theme's tokens (padding,
  // centering, underline). Non-interactive frame: no hit rects registered.
  //
  // The band itself -- bezel fitting, the doubled titled band, the
  // coverage-guarded title face, the leading arrow and the theme's rule -- lives
  // in components/TitledHeaderBand.h, because the screens that compose their own
  // HeaderProps (library list, catalog family) paint the SAME band and must not
  // grow a second recipe for it.
  namespace fui = freeink::ui;
  const auto spec = uiScaleSpec();
  fui::GfxRendererFrame<1> ui(renderer, spec.smallFontId, spec.bodyFontId, spec.titleFontId);
  fui::HeaderProps props;
  titledHeaderBand::drawTitledHeaderBand(*this, ui.frame, renderer, rect, title, subtitle, props, backButton);
}

void BaseTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  constexpr int labelGap = 10;
  // Scaled inset: sub-headers sit above fui lists whose rows are inset by the
  // same (derived) padding, so both must read one value.
  const int sidePad = UITheme::getInstance().getMetrics().contentSidePadding;
  const int contentWidth = std::max(0, rect.width - sidePad * 2);

  int labelWidth = contentWidth;
  if (rightLabel) {
    auto truncatedRightLabel = renderer.truncatedText(SMALL_FONT_ID, rightLabel, contentWidth, EpdFontFamily::REGULAR);
    const int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - sidePad - rightLabelWidth, rect.y + 7,
                      truncatedRightLabel.c_str());
    labelWidth = std::max(0, contentWidth - rightLabelWidth - labelGap);
  }

  if (labelWidth > 0) {
    auto truncatedLabel = renderer.truncatedText(UI_12_FONT_ID, label, labelWidth, EpdFontFamily::REGULAR);
    renderer.drawText(UI_12_FONT_ID, rect.x + sidePad, rect.y, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);
  }
}

// Draw the "Recent Book" cover card on the home screen.
// Still one function on purpose: it is one card drawn in one pass, and the
// cover half is stateful -- `storeCoverBuffer` is called from the middle of the
// paint (the first pass) and the result decides whether the later text blocks and
// the Continue Reading label draw inverted boxes, so the "store" step cannot be
// lifted out without threading that flag through the callers. What a split would
// buy: separate geometry (aspect-fit + fallback), cover paint, and label paint.
void BaseTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const bool hasContinueReading = !recentBooks.empty();
  const bool bookSelected = hasContinueReading && selectorIndex == 0;

  // --- Top "book" card for the current title (selectorIndex == 0) ---
  // When there's no cover image, use fixed size (half screen)
  // When there's cover image, adapt width to image aspect ratio, keep height fixed at 400px
  const int baseHeight = rect.height;  // Fixed height (400px)

  int bookWidth, bookX;
  bool hasCoverImage = false;

  if (hasContinueReading && !recentBooks[0].coverBmpPath.empty()) {
    // Try to get actual image dimensions from BMP header
    const std::string coverBmpPath =
        UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

    HalFile file;
    if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        hasCoverImage = true;
        const int imgWidth = bitmap.getWidth();
        const int imgHeight = bitmap.getHeight();

        // Calculate width based on aspect ratio, maintaining baseHeight
        if (imgWidth > 0 && imgHeight > 0) {
          const float aspectRatio = static_cast<float>(imgWidth) / static_cast<float>(imgHeight);
          bookWidth = static_cast<int>(baseHeight * aspectRatio);

          // Ensure width doesn't exceed reasonable limits (max 90% of screen width)
          const int maxWidth = static_cast<int>(rect.width * 0.9f);
          if (bookWidth > maxWidth) {
            bookWidth = maxWidth;
          }
        } else {
          bookWidth = rect.width / 2;  // Fallback
        }
      }
    }
  }

  if (!hasCoverImage) {
    // No cover: use half screen size
    bookWidth = rect.width / 2;
  }

  bookX = rect.x + (rect.width - bookWidth) / 2;
  const int bookY = rect.y;
  const int bookHeight = baseHeight;

  // Bookmark dimensions (used in multiple places)
  const int bookmarkWidth = bookWidth / 8;
  const int bookmarkHeight = bookHeight / 5;
  const int bookmarkX = bookX + bookWidth - bookmarkWidth - 10;
  const int bookmarkY = bookY + 5;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  {
    // Draw cover image as background if available (inside the box)
    // Only load from SD on first render, then use stored buffer

    if (hasContinueReading && !recentBooks[0].coverBmpPath.empty() && !coverRendered) {
      const std::string coverBmpPath =
          UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

      // First time: load cover from SD and render
      HalFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          LOG_DBG("THEME", "Rendering bmp");

          // The card matches the cover aspect except when width-capped; fill
          // the card 1:1 and crop the overflow rather than rescale the dither.
          drawCoverThumbFill(renderer, bitmap, Rect{bookX, bookY, bookWidth, bookHeight});

          // Draw border around the card
          renderer.drawRect(bookX, bookY, bookWidth, bookHeight);

          // No bookmark ribbon when cover is shown - it would just cover the art

          // Store the buffer with cover image for fast navigation
          coverBufferStored = storeCoverBuffer();
          coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer

          // First render: if selected, draw selection indicators now
          if (bookSelected) {
            LOG_DBG("THEME", "Drawing selection");
            renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
            renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
          }
        }
      }
    }

    if (!bufferRestored && !coverRendered) {
      // No cover image: draw border or fill, plus bookmark as visual flair
      if (bookSelected) {
        renderer.fillRect(bookX, bookY, bookWidth, bookHeight);
      } else {
        renderer.drawRect(bookX, bookY, bookWidth, bookHeight);
      }

      // Draw bookmark ribbon when no cover image (visual decoration)
      if (hasContinueReading) {
        const int notchDepth = bookmarkHeight / 3;
        const int centerX = bookmarkX + bookmarkWidth / 2;

        const int xPoints[5] = {
            bookmarkX,                  // top-left
            bookmarkX + bookmarkWidth,  // top-right
            bookmarkX + bookmarkWidth,  // bottom-right
            centerX,                    // center notch point
            bookmarkX                   // bottom-left
        };
        const int yPoints[5] = {
            bookmarkY,                                // top-left
            bookmarkY,                                // top-right
            bookmarkY + bookmarkHeight,               // bottom-right
            bookmarkY + bookmarkHeight - notchDepth,  // center notch point
            bookmarkY + bookmarkHeight                // bottom-left
        };

        // Draw bookmark ribbon (inverted if selected)
        renderer.fillPolygon(xPoints, yPoints, 5, !bookSelected);
      }
    }

    // If buffer was restored, draw selection indicators if needed
    if (bufferRestored && bookSelected && coverRendered) {
      // Draw selection border (no bookmark inversion needed since cover has no bookmark)
      renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
      renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
    } else if (!coverRendered && !bufferRestored) {
      // Selection border already handled above in the no-cover case
    }
  }

  if (hasContinueReading) {
    const std::string& lastBookTitle = recentBooks[0].title;
    const std::string& lastBookAuthor = recentBooks[0].author;

    // Invert text colors based on selection state:
    // - With cover: selected = white text on black box, unselected = black text on white box
    // - Without cover: selected = white text on black card, unselected = black text on white card

    auto lines = renderer.wrappedText(UI_12_FONT_ID, lastBookTitle.c_str(), bookWidth - 40, 3);

    // Book title text
    int totalTextHeight = renderer.getLineHeight(UI_12_FONT_ID) * static_cast<int>(lines.size());
    if (!lastBookAuthor.empty()) {
      totalTextHeight += renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    }

    // Vertically center the title block within the card
    int titleYStart = bookY + (bookHeight - totalTextHeight) / 2;

    const auto truncatedAuthor = lastBookAuthor.empty()
                                     ? std::string{}
                                     : renderer.truncatedText(UI_10_FONT_ID, lastBookAuthor.c_str(), bookWidth - 40);

    // If cover image was rendered, draw box behind title and author
    if (coverRendered) {
      constexpr int boxPadding = 8;
      // Calculate the max text width for the box
      int maxTextWidth = 0;
      for (const auto& line : lines) {
        const int lineWidth = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
        if (lineWidth > maxTextWidth) {
          maxTextWidth = lineWidth;
        }
      }
      if (!truncatedAuthor.empty()) {
        const int authorWidth = renderer.getTextWidth(UI_10_FONT_ID, truncatedAuthor.c_str());
        if (authorWidth > maxTextWidth) {
          maxTextWidth = authorWidth;
        }
      }

      const int boxWidth = maxTextWidth + boxPadding * 2;
      const int boxHeight = totalTextHeight + boxPadding * 2;
      const int boxX = rect.x + (rect.width - boxWidth) / 2;
      const int boxY = titleYStart - boxPadding;

      // Draw box (inverted when selected: black box instead of white)
      renderer.fillRect(boxX, boxY, boxWidth, boxHeight, bookSelected);
      // Draw border around the box (inverted when selected: white border instead of black)
      renderer.drawRect(boxX, boxY, boxWidth, boxHeight, !bookSelected);
    }

    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, titleYStart, line.c_str(), !bookSelected);
      titleYStart += renderer.getLineHeight(UI_12_FONT_ID);
    }

    if (!truncatedAuthor.empty()) {
      titleYStart += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, titleYStart, truncatedAuthor.c_str(), !bookSelected);
    }

    // "Continue Reading" label at the bottom
    const int continueY = bookY + bookHeight - renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    if (coverRendered) {
      // Draw box behind "Continue Reading" text (inverted when selected: black box instead of white)
      const char* continueText = tr(STR_CONTINUE_READING);
      const int continueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, continueText);
      constexpr int continuePadding = 6;
      const int continueBoxWidth = continueTextWidth + continuePadding * 2;
      const int continueBoxHeight = renderer.getLineHeight(UI_10_FONT_ID) + continuePadding;
      const int continueBoxX = rect.x + (rect.width - continueBoxWidth) / 2;
      const int continueBoxY = continueY - continuePadding / 2;
      renderer.fillRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, bookSelected);
      renderer.drawRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, !bookSelected);
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, continueText, !bookSelected);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, tr(STR_CONTINUE_READING), !bookSelected);
    }
  } else {
    // No book to continue reading
    const int y =
        bookY + (bookHeight - renderer.getLineHeight(UI_12_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_NO_OPEN_BOOK));
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID), tr(STR_START_READING));
  }
}

int BaseTheme::getMenuRowHeight(const GfxRenderer&) const { return UITheme::getInstance().getMetrics().menuRowHeight; }

int BaseTheme::getMenuRowStep(const GfxRenderer& renderer) const {
  // Scaled gap, same table the row height came from. Only one place adds it, so
  // the painted pitch and the tapped pitch cannot drift apart.
  return getMenuRowHeight(renderer) + UITheme::getInstance().getMetrics().menuSpacing;
}

MenuLayout BaseTheme::getMenuLayout(const GfxRenderer& renderer, const Rect band, const int buttonCount,
                                    const int selectedIndex) const {
  (void)selectedIndex;  // only paging themes (RoundedRaff) need the selection here
  MenuLayout layout;
  layout.top = band.y;
  layout.rowHeight = getMenuRowHeight(renderer);
  layout.step = getMenuRowStep(renderer);
  layout.firstIndex = 0;
  layout.rows = buttonCount;
  return layout;
}

void BaseTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  // Geometry comes from getMenuLayout(): the SAME call HomeActivity maps taps
  // with, so a painted tile and its hit band are one rect by construction.
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const MenuLayout layout = getMenuLayout(renderer, rect, buttonCount, selectedIndex);
  const int rowHeight = layout.rowHeight;
  const int sidePad = metrics.contentSidePadding;
  for (int i = layout.firstIndex; i < buttonCount && i < layout.firstIndex + layout.rows; ++i) {
    const int tileY = menuRowTop(layout, i);

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRect(rect.x + sidePad, tileY, rect.width - sidePad * 2, rowHeight);
    } else {
      renderer.drawRect(rect.x + sidePad, tileY, rect.width - sidePad * 2, rowHeight);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = rect.x + (rect.width - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY = tileY + (rowHeight - lineHeight) / 2;  // vertically centered assuming y is top of text
    // Invert text when the tile is selected, to contrast with the filled background
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, selectedIndex != i);
  }
}

Rect BaseTheme::drawPopup(const GfxRenderer& renderer, const char* message, const bool commit) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int marginX = metrics.popupMarginX;
  const int marginY = metrics.popupMarginY;
  const int frameThickness = metrics.popupFrameThickness;
  const EpdFontFamily::Style popupFontFamily = metrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  // Scale y position proportionally to screen height
  const int y = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  // U-7: the box is sized FROM THE STRING and was never clamped to the panel, so any
  // message wider than the screen put its frame and its text past both edges with no
  // ellipsis and no wrap. No shipped string overflows today (the longest is ~43 chars at
  // UI_12 bold = ~516 + 2 x 20 px, which fits 684), so this changes nothing for any
  // current caller: it only turns the never-happens case from "draws off-panel" into
  // "truncated to the widest box that fits, keeping the popup's own side margin".
  int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, popupFontFamily);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int maxTextWidth = renderer.getScreenWidth() - marginX * 4;
  std::string clampedMessage;
  const char* popupText = message;
  if (maxTextWidth > 0 && textWidth > maxTextWidth) {
    clampedMessage = renderer.truncatedText(UI_12_FONT_ID, message, maxTextWidth, popupFontFamily);
    popupText = clampedMessage.c_str();
    textWidth = renderer.getTextWidth(UI_12_FONT_ID, popupText, popupFontFamily);
  }
  const int w = textWidth + marginX * 2;
  const int h = textHeight + marginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  const bool useRoundedPopup = metrics.popupCornerRadius > 0;
  if (useRoundedPopup) {
    renderer.fillRoundedRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2,
                             metrics.popupCornerRadius + frameThickness, Color::White);
    renderer.fillRoundedRect(x, y, w, h, metrics.popupCornerRadius, Color::Black);
  } else {
    renderer.fillRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2, true);
    renderer.fillRect(x, y, w, h, false);
  }

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + marginY + metrics.popupTextBaselineOffsetY;
  renderer.drawText(UI_12_FONT_ID, textX, textY, popupText, metrics.popupTextInverted, popupFontFamily);
  // A popup commits on its own, and a bare displayBuffer() both used a diff-area mode and CONSUMED
  // any one-shot promotion armed for the frame underneath -- stealing the FULL that screen still
  // needs. Park the promotion, paint the box as a HALF (a box over existing content is an edit, not
  // a replacement), then re-arm it for whoever commits next.
  //
  // commit=false is the sleep screen: the box is drawn and NOT driven, because the caller is about
  // to compose a whole replacement frame (a cleared canvas, driven from white) that wipes it. Its
  // drive is the only thing skipped -- the composition, and the promotion, are untouched.
  if (!commit) return Rect{x, y, w, h};
  const auto parked = renderer.peekPromotedRefresh();
  renderer.cancelPromotedRefresh();
  // EQ1 (r101.9): with Settings > Display > Equalize Black ON this popup's HALF becomes
  // EQUALIZE (same 48-frame GC16, LUT-only) so the box AND the text under it are re-inked
  // page-wide. OFF returns HALF unchanged -- see src/display/EqualizeBlack.h.
  renderer.displayBuffer(equalizeBlackMode(HalDisplay::HALF_REFRESH, "popup"));
  renderer.rearmPromotedRefresh(parked);
  return Rect{x, y, w, h};
}

void BaseTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress,
                                  const bool commit) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int barHeight = metrics.popupProgressBarHeight;
  const int barWidth =
      std::max(0, layout.width - metrics.popupMarginX * 2);  // twice the margin in drawPopup to match text width
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - metrics.popupMarginY / 2 - barHeight / 2 - 1;
  if (barWidth <= 0 || barHeight <= 0) {
    if (commit) renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const int scaledProgress = metrics.popupProgressClampPercent ? std::clamp(progress, 0, 100) : progress;
  const int fillWidth = barWidth * scaledProgress / 100;

  if (metrics.popupProgressDrawOutline) {
    renderer.drawRect(barX, barY, barWidth, barHeight, 1, metrics.popupProgressOutlineInverted);
  }
  if (fillWidth > 0) {
    renderer.fillRect(barX, barY, fillWidth, barHeight, metrics.popupProgressFillInverted);
  }

  // commit=false is for a caller that is composing a whole frame (chrome, caption, bar, controls)
  // and will drive it once itself: the bar is painted, the drive is the caller's.
  if (!commit) return;
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BaseTheme::drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                              const int pageCount, std::string title, const int paddingBottom, const int textYOffset,
                              const bool fillMargin, const bool isPageBookmarked, const bool pageCountEstimated) const {
  auto metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const auto sb = SETTINGS.statusBarSpec();
  const bool showStatusBarTextLane = sb.textLaneVisible(halClock.isAvailable());

  // Draw Progress Text
  const auto screenHeight = renderer.getScreenHeight();
  // The status bar's band, from the one height accessor. A board whose panel
  // sits off the band's optical centre would carry a statusBarOffsetY field
  // applied right here; the Plus panel is positionally centred, so the offset
  // is 0 and no BoardConfig field exists -- add one when a board needs it.
  auto textY = screenHeight - UITheme::getInstance().getStatusBarHeight() - orientedMarginBottom - paddingBottom - 4;

  // The side clusters sit in the bottom corners: clear the panel's rounded corners too.
  const int corner = BoardConfig::ACTIVE.cornerRadius;
  const int leftClusterX = metrics.statusBarHorizontalMargin + std::max(orientedMarginLeft, corner) + 1;
  const int rightClusterX =
      renderer.getScreenWidth() - metrics.statusBarHorizontalMargin - std::max(orientedMarginRight, corner);
  int leftClusterWidth = 0;
  int rightClusterWidth = 0;

  if (sb.showBookProgressPercent || sb.showChapterPageCount) {
    // Right aligned text for progress counter
    char progressStr[32];

    // Draw the estimate marker separately so it can use the next UI font size.
    const bool showEstimate = pageCountEstimated && sb.showChapterPageCount;

    if (sb.showBookProgressPercent && sb.showChapterPageCount) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", currentPage, pageCount, bookProgress);
    } else if (sb.showBookProgressPercent) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", currentPage, pageCount);
    }

    int progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    const int estimateWidth = showEstimate ? renderer.getTextWidth(UI_10_FONT_ID, "~") : 0;
    constexpr int estimateGap = 2;
    const int estimateSpacing = showEstimate ? estimateGap : 0;
    const int progressX = rightClusterX - estimateWidth - estimateSpacing - progressTextWidth;
    if (showEstimate) {
      const int estimateY = textY + (renderer.getLineHeight(SMALL_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
      renderer.drawText(UI_10_FONT_ID, progressX, estimateY, "~");
    }
    renderer.drawText(SMALL_FONT_ID, progressX + estimateWidth + estimateSpacing, textY, progressStr);

    rightClusterWidth += estimateWidth + estimateSpacing + progressTextWidth;
  }

  // Draw Progress Bar
  if (sb.showsProgressBar()) {
    const int barMarginLeft = fillMargin ? 0 : orientedMarginLeft;
    const int barMarginRight = fillMargin ? 0 : orientedMarginRight;
    const int progressBarMaxWidth = renderer.getScreenWidth() - barMarginLeft - barMarginRight;
    const int progressBarY = renderer.getScreenHeight() - orientedMarginBottom - sb.progressBarHeightPx -
                             paddingBottom + (fillMargin ? 1 : 0);
    size_t progress;
    if (sb.progressBarMode == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS) {
      progress = static_cast<size_t>(bookProgress);
    } else {
      // Chapter progress
      progress = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) * 100 : 0;
    }
    const int barWidth = progressBarMaxWidth * progress / 100;
    const int barHeight = sb.progressBarHeightPx + (fillMargin ? orientedMarginBottom - 1 : 0);
    renderer.fillRect(barMarginLeft, progressBarY, barWidth, barHeight, true);
  }

  // Draw Battery
  const bool showBatteryPercentage = sb.showBatteryPercent;

  if (sb.showBattery) {
    GUI.drawBatteryLeft(renderer,
                        Rect{leftClusterX + leftClusterWidth, textY, metrics.batteryWidth, metrics.batteryHeight},
                        showBatteryPercentage);
    int batteryWidth = metrics.batteryWidth;

    if (showBatteryPercentage) {
      const uint16_t percentage = powerManager.getBatteryPercentage();
      // width of icon + spacing + text for layout purposes
      batteryWidth +=
          batteryPercentSpacing + renderer.getTextWidth(SMALL_FONT_ID, (std::to_string(percentage) + "%").c_str());
    }

    leftClusterWidth += batteryWidth;
  }

  // Draw Clock (any board whose RTC probe succeeded)
  if (sb.showsClock() && halClock.isAvailable()) {
    char timeBuf[9];
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), sb.clock12h)) {
      int clockTextWidth = renderer.getTextWidth(SMALL_FONT_ID, timeBuf);
      int clockX = 0;
      // Position to the left or right of the progress text (with a small gap)
      if (sb.clockMode == CrossPointSettings::STATUS_BAR_CLOCK_LEFT) {
        clockX = leftClusterX + leftClusterWidth + (leftClusterWidth > 0 ? 10 : 0);
        leftClusterWidth += clockTextWidth + 10;
      } else if (sb.clockMode == CrossPointSettings::STATUS_BAR_CLOCK_RIGHT) {
        clockX = rightClusterX - rightClusterWidth - (rightClusterWidth > 0 ? 10 : 0) - clockTextWidth;
        rightClusterWidth += clockTextWidth + 10;
      }
      renderer.drawText(SMALL_FONT_ID, clockX, textY, timeBuf);
    }
  }

  // Draw Bookmark
  if (showStatusBarTextLane && isPageBookmarked) {
    const int bookmarkGap = leftClusterWidth > 0 ? bookmarkStatusIconGap : 0;
    const int bookmarkX = leftClusterX + leftClusterWidth + bookmarkGap;
    const int bookmarkY = textY + 5;
    drawBookmarkStatusIcon(renderer, bookmarkX, bookmarkY);
    leftClusterWidth += bookmarkStatusIconWidth + bookmarkGap;
  }

  // Draw Title
  if (!title.empty()) {
    textY -= textYOffset;
    // Centered chapter title text
    // Page width minus existing content with 30px padding on each side
    const int rendererableScreenWidth =
        renderer.getScreenWidth() - (metrics.statusBarHorizontalMargin * 2) - orientedMarginLeft - orientedMarginRight;

    const int titleMarginLeft = leftClusterWidth + 30;
    const int titleMarginRight = rightClusterWidth + 30;

    // Attempt to center title on the screen, but if title is too wide then later we will center it within the
    // available space.
    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;

    int titleWidth;
    titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    if (titleWidth > availableTitleSpace) {
      // Not enough space to center on the screen, center it within the remaining space instead
      availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
      titleMarginLeftAdjusted = titleMarginLeft;
    }
    if (titleWidth > availableTitleSpace) {
      title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTitleSpace);
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    }

    renderer.drawText(SMALL_FONT_ID,
                      titleMarginLeftAdjusted + metrics.statusBarHorizontalMargin + orientedMarginLeft +
                          (availableTitleSpace - titleWidth) / 2,
                      textY, title.c_str());
  }
}

void BaseTheme::drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  auto truncatedLabel =
      renderer.truncatedText(SMALL_FONT_ID, label, rect.width - metrics.contentSidePadding * 2, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, rect.y, truncatedLabel.c_str());
}

void BaseTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                              int contentStartX, int contentWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineY = rect.y + rect.height + lineHeight + metrics.verticalSpacing;
  const int thickness = cursorMode ? metrics.textFieldCursorThickness : metrics.textFieldNormalThickness;
  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY,
                      rect.x + contentStartX + contentWidth + metrics.textFieldLineEndOffset, lineY, thickness, true);
  } else {
    const int lineW = textWidth + metrics.textFieldHorizontalPadding * 2;
    const int lineStart = rect.x + (rect.width - lineW) / 2;
    renderer.drawLine(lineStart, lineY, lineStart + lineW + metrics.textFieldLineEndOffset, lineY, thickness, true);
  }
}

void BaseTheme::drawSelectionHandle(const GfxRenderer& renderer, const Rect rect, const bool left) {
  const int radius = rect.width / 2;
  renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, radius, Color::Black);
  renderer.fillRect(left ? rect.x + radius : rect.x, rect.y, radius, radius);
}

void BaseTheme::drawSelectionActions(const GfxRenderer& renderer, const Rect rect, const int count,
                                     const int firstAction) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const char* labels[] = {tr(STR_LOOKUP), tr(STR_CLIP), tr(STR_BOOKMARK_OPTION)};
  for (int i = 0; i < count; ++i) {
    const Rect button = selectionGeometry::button(rect, i, metrics.menuSpacing, count);
    renderer.fillRoundedRect(button.x, button.y, button.width, button.height, metrics.controlRadius, Color::White);
    renderer.drawRoundedRect(button.x, button.y, button.width, button.height, 1, metrics.controlRadius, true);
    UITheme::drawCenteredWrappedText(renderer, button, uiScaleSpec().smallFontId, labels[firstAction + i], 2, true,
                                     EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::CENTER);
  }
}
