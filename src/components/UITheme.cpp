#include "UITheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/CoverGridHomeUi.h"
#include "components/UiDensity.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/andro/AndroTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/pickyos/PickyOSTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"

UITheme UITheme::instance;

UITheme::UITheme() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::reload() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

bool UITheme::supportsCoverGrid() { return HalMemory::getPsramHeap().totalBytes > 0; }

bool UITheme::hasCoverGridHome() { return SETTINGS.uiTheme == CrossPointSettings::COVER_GRID && supportsCoverGrid(); }

void UITheme::drawCoverGridHome(CoverGridHomeUi& home) { home.renderUi(); }

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  if (type == CrossPointSettings::COVER_GRID && !supportsCoverGrid()) {
    // Without PSRAM the settings list hides the grid entry (its slot is
    // relabelled ANDRO -- see homeThemeValues()), and a saved COVER_GRID value
    // resolves here to Andro.
    type = CrossPointSettings::ANDRO;
  }

  switch (type) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      LOG_DBG("UI", "Using Classic theme");
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      themeName_ = "Classic";
      break;
    case CrossPointSettings::UI_THEME::COVER_GRID:
    case CrossPointSettings::UI_THEME::LYRA: {
      // The cover home owns its screen-lifetime UI state; other screens retain Lyra styling.
      auto theme = makeUniqueNoThrow<LyraTheme>();
      if (!theme) {
        LOG_ERR("UI", "OOM: Lyra theme");
        return;
      }
      currentTheme = std::move(theme);
      currentMetrics = &LyraMetrics::values;
      themeName_ = (type == CrossPointSettings::UI_THEME::COVER_GRID) ? "CoverGrid" : "Lyra";
      LOG_DBG("UI", "Using Lyra theme");
      break;
    }
    case CrossPointSettings::UI_THEME::ROUNDEDRAFF:
      LOG_DBG("UI", "Using RoundedRaff theme");
      currentTheme = std::make_unique<RoundedRaffTheme>();
      currentMetrics = &RoundedRaffMetrics::values;
      themeName_ = "RoundedRaff";
      break;
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      currentTheme = std::make_unique<Lyra3CoversTheme>();
      currentMetrics = &Lyra3CoversMetrics::values;
      themeName_ = "Lyra3Covers";
      break;
    case CrossPointSettings::UI_THEME::ANDRO:
      LOG_DBG("UI", "Using Andro theme");
      currentTheme = std::make_unique<AndroTheme>();
      currentMetrics = &AndroMetrics::values;
      themeName_ = "Andro";
      break;
    case CrossPointSettings::UI_THEME::PICKYOS:
      LOG_DBG("UI", "Using PickyOS theme");
      currentTheme = std::make_unique<PickyOSTheme>();
      currentMetrics = &PickyOSMetrics::values;
      themeName_ = "PickyOS";
      break;
  }
  metricsValid = false;
}

namespace {

// THE single place the boot-resolved UI density factor touches layout. Every
// absolute pixel of a theme's ThemeMetrics table is a length on the 480x800 @
// 235 PPI design canvas; uiDensity::scale() restores that physical size on a
// denser panel.
//
// The FIELD LIST lives in kMetricFields[] (BaseTheme.h), not here: that table is
// the one place a field is declared Design (a length -> multiplied) or Physical
// (an asset size, count, ratio, enum or sentinel -> left alone), and this pass
// simply walks it. Adding a metric means adding it to the table; the boot audit
// below names any field whose converted value disagrees with its class, so a
// forgotten field surfaces in debug.log instead of in the operator's taps.
int uiScaled(const int value, const float scale) {
  if (value == 0) return 0;
  return static_cast<int>(std::lround(static_cast<float>(value) * scale));
}

int expectedScaled(const int design, const MetricScale kind, const float scale) {
  return kind == MetricScale::Design ? uiScaled(design, scale) : design;
}

void applyUiScale(ThemeMetrics& m) {
  const float s = uiDensity::scale();
  if (s == 1.0f) return;  // design panel: leave every authored value untouched

  // 255 is fui's "full stadium" sentinel (radius = half the control height), not
  // a length; scaling it would overflow the uint8 the token carries.
  const int capsuleSentinel = m.capsuleRadius;

  for (const MetricField& field : kMetricFields) {
    if (field.scale != MetricScale::Design) continue;
    int& value = m.*(field.member);
    value = uiScaled(value, s);
  }

  if (capsuleSentinel == 255) m.capsuleRadius = 255;
}

}  // namespace

const ThemeMetrics& UITheme::getMetrics() const {
  // hasTouch() can flip once touch init completes after static construction, so the
  // cached copy is refreshed when the flag differs instead of copying the struct per call.
  const bool touch = gpio.hasTouch();
  if (!metricsValid || touch != metricsForTouch) {
    adjustedMetrics = *currentMetrics;
    if (touch) {
      adjustedMetrics.buttonHintsHeight = 0;
    }
    applyUiScale(adjustedMetrics);
    metricsForTouch = touch;
    metricsValid = true;
  }
  return adjustedMetrics;
}

// Boot-time falsifiable check for the density scale. Two questions, one line:
//
//  1. Did every ThemeMetrics field move exactly as its MetricScale class says?
//     A Design field that still equals its design-space value, or a Physical
//     field that moved, means the sweep and the table disagree -- the classic
//     "new metric added to the struct, forgotten in the scaling pass".
//  2. Does the ACTIVE THEME's own paint geometry follow the scale? Some surfaces
//     (the home button menu) are laid out by the theme rather than by a shared
//     widget, so a theme that copies pre-scale draw code emits design-space rows
//     while the shared touch grid steps by the scaled ones. The theme reports its
//     menu geometry through getMenuLayout(); if that still returns the design
//     value at scale > 1, the theme is named here.
//
// One line, e.g. "[UISCALE] theme=Lyra field=menuRowHeight not scaled"; "" when
// clean. Runs once at boot and is appended to /.crosspoint/debug.log next to the
// UISCALE line, so a bad theme is readable without a UART.
const char* UITheme::auditUiScale(const GfxRenderer& renderer) {
  static char line[192];
  const float s = uiDensity::scale();
  if (s == 1.0f) return "";  // design panel: nothing converted, nothing to check

  const ThemeMetrics& design = *currentMetrics;
  const ThemeMetrics& scaled = getMetrics();
  const bool hintsBlankedForTouch = gpio.hasTouch();

  char fields[128];
  size_t used = 0;
  fields[0] = '\0';
  const auto note = [&](const char* name) {
    const size_t len = strlen(name);
    if (used + len + 2 >= sizeof(fields)) return;
    if (used) {
      fields[used++] = ',';
      fields[used] = '\0';
    }
    memcpy(fields + used, name, len + 1);
    used += len;
  };

  for (const MetricField& field : kMetricFields) {
    const int designValue = design.*(field.member);
    // getMetrics() blanks buttonHintsHeight on a touch board (the visible
    // controls are on the panel, there is no hint strip to reserve). That is a
    // deliberate hardware override of the design value, not a missed
    // conversion, so the expectation has to carry it too -- otherwise every
    // touch theme reports the field as "not scaled" on every boot.
    const bool blankedForTouch = hintsBlankedForTouch && field.member == &ThemeMetrics::buttonHintsHeight;
    const int want = blankedForTouch ? 0 : expectedScaled(designValue, field.scale, s);
    if (scaled.*(field.member) != want) note(field.name);
  }

  const BaseTheme& theme = getTheme();
  if (!theme.menuRowHeightIsDerived()) {
    const MenuLayout layout = theme.getMenuLayout(renderer, Rect{0, 0, 0, 0}, 1, 0);
    if (layout.rowHeight == design.menuRowHeight) note("menuRowHeight");
    if (layout.step == design.menuRowHeight + design.menuSpacing) note("menuSpacing");
  }

  if (used == 0) return "";
  snprintf(line, sizeof(line), "[UISCALE] theme=%s field=%s not scaled", themeName_, fields);
  return line;
}

// Full drawable content area: screen minus the board's viewable insets (bezel /
// rounded-corner clearance), oriented to the current rotation. The single place
// the inset math lives; every screen derives its content bounds from here (or
// from getScreenSafeArea, which builds on it).
Rect UITheme::getContentArea(const GfxRenderer& renderer) {
  int viTop = 0, viRight = 0, viBottom = 0, viLeft = 0;
  renderer.getOrientedViewableTRBL(&viTop, &viRight, &viBottom, &viLeft);
  return Rect{viLeft, viTop, renderer.getScreenWidth() - viLeft - viRight,
              renderer.getScreenHeight() - viTop - viBottom};
}

// Content area excluding the button hints.
Rect UITheme::getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints, bool hasSideButtonHints) {
  auto orientation = renderer.getOrientation();
  Rect safeArea = getContentArea(renderer);
  const ThemeMetrics metrics = getMetrics();
  switch (orientation) {
    case GfxRenderer::Orientation::Portrait:
      if (hasFrontButtonHints) {
        safeArea.height -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      if (hasFrontButtonHints) {
        safeArea.x += metrics.buttonHintsHeight;
        safeArea.width -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      if (hasFrontButtonHints) {
        safeArea.y += metrics.buttonHintsHeight;
        safeArea.height -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      if (hasFrontButtonHints) {
        safeArea.width -= metrics.buttonHintsHeight;
      }
      break;
  }
  return safeArea;
}

// Header rect for a screen laid out in getScreenSafeArea(): clear of the
// button hints but NOT inset for the bezel, since the header band applies the
// board's viewable insets itself. Built from the SCALED metrics only.
Rect UITheme::getHeaderRect(const GfxRenderer& renderer, bool hasFrontButtonHints, bool hasSideButtonHints) {
  const Rect safe = getScreenSafeArea(renderer, hasFrontButtonHints, hasSideButtonHints);
  const Rect content = getContentArea(renderer);
  const ThemeMetrics metrics = getMetrics();
  return Rect{safe.x - content.x, safe.y - content.y + metrics.topPadding,
              safe.width + renderer.getScreenWidth() - content.width, metrics.headerHeight};
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasImageExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getStatusBarHeight() {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  const auto sb = SETTINGS.statusBarSpec();

  // Layout reservation is hardware-agnostic: pass clockAvailable=true so the
  // reserved height does not depend on whether an RTC is present.
  // A theme may add the rows its own bar chrome needs above the lane (Andro's
  // 6 px top rule + its clear); 0 for every other theme, so their numbers here
  // are unchanged. Whatever this returns is the band every bar consumer -- the
  // reader's page box, the XTC overlay, the settings preview -- reserves, so the
  // bar can never be drawn outside it.
  const int ruleInset = UITheme::getInstance().getTheme().statusBarRuleInset();
  // Air above and below the text lane, in panel pixels. 0 for every theme that
  // does not state it, so only the theme that asked for breathing room pays for
  // it -- and it pays in the ONE reservation every consumer reads, never in a
  // second recipe.
  const int textPad = sb.textLaneVisible(true) ? (2 * std::max(0, metrics.statusBarTextPad)) : 0;
  // r101.9 SBAR1: air below the lane only. Part of the reserved band on purpose --
  // the bar anchors its BOTTOM edge (bandBottom = safe edge) and derives bandTop
  // from this height, so adding the pad here raises bandTop by the pad while
  // bandBottom stays put: the bar lifts, the page box shrinks by exactly the pad,
  // and nothing can be drawn below the safe edge. With no text lane there is no
  // lane to pad, so it costs 0.
  const int bottomPad = sb.textLaneVisible(true) ? std::max(0, metrics.statusBarBottomPad) : 0;
  return (sb.textLaneVisible(true) ? (metrics.statusBarVerticalMargin + ruleInset + textPad + bottomPad) : 0) +
         (sb.showsProgressBar() ? (sb.progressBarHeightPx + metrics.progressBarMarginTop) : 0);
}

int UITheme::getProgressBarHeight() {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  const auto sb = SETTINGS.statusBarSpec();
  return sb.showsProgressBar() ? (sb.progressBarHeightPx + metrics.progressBarMarginTop) : 0;
}

// Centered text implementation that takes the safe area into account
void UITheme::drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black, EpdFontFamily::Style style) {
  const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, text, style)) / 2;
  renderer.drawText(fontId, x, y, text, black, style);
}

void UITheme::drawCenteredWrappedText(const GfxRenderer& renderer, Rect bounds, int fontId, const char* text,
                                      int maxLines, bool black, EpdFontFamily::Style style,
                                      TextVerticalAlignment verticalAlignment) {
  if (!text || *text == '\0' || bounds.width <= 0 || bounds.height <= 0 || maxLines <= 0) return;

  const int lineHeight = renderer.getLineHeight(fontId);
  if (lineHeight <= 0) return;

  const int lineLimit = std::min(maxLines, bounds.height / lineHeight);
  if (lineLimit <= 0) return;

  const auto alignedTop = [&](const int textHeight) {
    switch (verticalAlignment) {
      case TextVerticalAlignment::CENTER:
        return bounds.y + (bounds.height - textHeight) / 2;
      case TextVerticalAlignment::BOTTOM:
        return bounds.y + bounds.height - textHeight;
      case TextVerticalAlignment::TOP:
      default:
        return bounds.y;
    }
  };

  if (renderer.getTextWidth(fontId, text, style) <= bounds.width) {
    drawCenteredText(renderer, bounds, fontId, alignedTop(lineHeight), text, black, style);
    return;
  }

  const auto lines = renderer.wrappedText(fontId, text, bounds.width, lineLimit, style);
  int y = alignedTop(static_cast<int>(lines.size()) * lineHeight);
  for (const auto& line : lines) {
    drawCenteredText(renderer, bounds, fontId, y, line.c_str(), black, style);
    y += lineHeight;
  }
}
