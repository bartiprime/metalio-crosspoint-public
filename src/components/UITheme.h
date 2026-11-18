#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"

class CoverGridHomeUi;

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  enum class TextVerticalAlignment { TOP, CENTER, BOTTOM };

  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const;
  // The active theme's AUTHORED table, i.e. design space (480x800 canvas). Read
  // it for asset dimensions and cache keys; anything that lands on the glass
  // must use getMetrics(), which is the same table converted for this panel.
  const ThemeMetrics& getDesignMetrics() const { return *currentMetrics; }
  const char* themeName() const { return themeName_; }
  // Boot-time falsifiability check for the density scale, run once after the
  // theme and fonts are up. Returns "" when every scaling-table field moved as
  // its MetricScale says and the active theme's own paint geometry followed the
  // scale; otherwise ONE line naming the theme and the offending fields, e.g.
  //   [UISCALE] theme=Lyra field=menuRowHeight not scaled
  // Callers append it to /.crosspoint/debug.log next to the UISCALE boot line.
  const char* auditUiScale(const GfxRenderer& renderer);
  const BaseTheme& getTheme() const { return currentTheme ? *currentTheme : fallbackTheme; }
  // Height of a TITLED screen's header band, in panel pixels: the active
  // theme's own app-bar height when it asks for one (Andro: 2x the home band),
  // else metrics.headerHeight. This is the number BOTH the band a titled
  // settings screen paints through drawHeader and the content margin it leaves
  // under that band must use -- two separate expressions are how r73's band and
  // its content origin drifted apart.
  int getTitledHeaderHeight() const { return getTheme().titledHeaderPx(); }
  // The drawable content area: full screen minus the board's viewable insets
  // (bezel / rounded-corner clearance from BoardConfig::ACTIVE.viewableInsets,
  // oriented to the current rotation). THE single place activities should get
  // their content bounds from so every screen clears the bezel identically.
  // Zero-inset on rectangular panels, so no visible change there.
  static Rect getContentArea(const GfxRenderer& renderer);
  Rect getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints = false,
                         bool hasSideButtonHints = false);
  // The header band: the strip across the top of a screen that carries the
  // status line (battery glyph, percentage, clock) and the title below it.
  // Measured from the SCALED metrics (getMetrics(): topPadding + headerHeight),
  // so its height -- and every offset derived from it -- follows the board's UI
  // density scale. THE single source for anything positioning status-bar
  // content: a caller that would otherwise re-add topPadding and headerHeight
  // takes the band from here instead.
  Rect getHeaderRect(const GfxRenderer& renderer, bool hasFrontButtonHints = false,
                     bool hasSideButtonHints = false);
  static void drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  // Wraps only overflowing text, then aligns the complete line block within bounds.
  static void drawCenteredWrappedText(const GfxRenderer& renderer, Rect bounds, int fontId, const char* text,
                                      int maxLines, bool black = true,
                                      EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                                      TextVerticalAlignment verticalAlignment = TextVerticalAlignment::CENTER);
  static bool supportsCoverGrid();
  static bool hasCoverGridHome();
  static void drawCoverGridHome(CoverGridHomeUi& home);
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight();
  static int getProgressBarHeight();

 private:
  BaseTheme fallbackTheme;
  const ThemeMetrics* currentMetrics = &BaseMetrics::values;
  const char* themeName_ = "Classic";
  std::unique_ptr<BaseTheme> currentTheme;
  mutable ThemeMetrics adjustedMetrics;
  mutable bool metricsValid = false;
  mutable bool metricsForTouch = false;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
