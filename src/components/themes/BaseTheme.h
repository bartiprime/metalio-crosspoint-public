#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Bitmap;
class GfxRenderer;
struct RecentBook;
namespace freeink {
namespace ui {
struct HeaderProps;
struct Rect;
struct BitmapRef;
struct ListItem;
}  // namespace ui
}  // namespace freeink

struct Rect {
  int x;
  int y;
  int width;
  int height;

  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

struct ThemeMetrics {
  int batteryWidth;
  int batteryHeight;

  int topPadding;
  int batteryBarHeight;
  int headerHeight;
  int verticalSpacing;

  int previewPadding;
  int previewHeightPercent;

  int contentSidePadding;
  int listRowHeight;
  int listWithSubtitleRowHeight;
  // FreeInkUI list shape, consumed by uiThemeTokens() for screens rendered
  // through FreeInkApp: the theme supplies geometry and selection style, the
  // uiScale fonts supply the sizes. Plain data by design — the eventual
  // SD-card theme files will provide exactly these values.
  int listRowGap;          // vertical gap between rows
  int listRowRadius;       // row corner radius (RoundedRaff cards, Lyra pill)
  int listInset;           // horizontal inset of the whole list band
  int listSidePadding;     // text inset within a row
  int listSelectionStyle;  // 0=invert fill, 1=light pill, 2=underline, 3=triangle (fui::SelectionStyle order)
  int listScrollWidth;     // scroll indicator thickness
  int listScrollSide;      // 0 = right edge, 1 = left edge
  bool listTitleBold;      // bold row titles (RoundedRaff)
  // FreeInkUI header shape, same contract as the list fields above.
  int headerSidePadding;    // title text inset
  int headerUnderlineSize;  // bottom rule thickness (Lyra), 0 = none
  int headerTitleAlign;     // 0 = left, 1 = center, 2 = right (fui::TextAlign order)
  int headerBatterySide;    // 0 = right edge, 1 = left edge
  // Header clock opt-out for themes whose title layout can't spare the left
  // reserve (RoundedRaff); the user setting still governs the themes that can.
  bool headerShowsClock = true;
  // Clock slot: centered on the band, or on the left after the back arrow.
  bool headerClockCentered = true;
  int menuRowHeight;
  int menuSpacing;

  int tabSpacing;
  int tabBarHeight;
  int coverGridTabBarHeight = 72;
  // Selected-tab pill fills its equal-width slot (legacy RoundedRaff tabs)
  // instead of shrinking to hug the label (legacy Lyra tabs).
  bool tabPillFullSlot = false;

  int scrollBarWidth;
  int scrollBarRightOffset;

  int homeTopPadding;
  int homeCoverHeight;
  int homeCoverTileHeight;
  int homeRecentBooksCount;
  bool homeContinueReadingInMenu;
  int homeMenuTopOffset;
  // Home "featured book" card (the Android-launcher look): one card height,
  // two dithered cover sizes at a 1:1 blit. The cover dimensions are ASSET
  // sizes -- they are the thumbnail cache key and are drawn unscaled -- so
  // they are Physical; the card height is a length on the design canvas.
  int homeFeaturedCardHeight;
  int homeFeaturedCoverWidth;
  int homeFeaturedCoverHeight;
  int homeFeaturedCoverLargeWidth;
  int homeFeaturedCoverLargeHeight;
  // Ink width of the frame drawn around the featured cover. Physical: it is a
  // stroke width, and 4 px at 297 PPI is 0.34 mm -- scaling it with the UI
  // density would thicken the frame past the cover art it is meant to bound.
  int homeFeaturedCoverBorder;
  // Corner radius of that frame. A RADIUS IS A LENGTH, so it is Design and
  // scales with the panel density (9 design -> 12 px on the Plus). The stroke
  // above stays Physical: one is thrown by the arc, the other is ink.
  int homeFeaturedCoverRadius;
  // Featured progress bar: outer height and outline stroke. Both are ink
  // geometry -- exact pixel counts at this panel's 297 PPI -- so they are
  // Physical and never scale. 12 px outer with a 2 px stroke leaves an 8 px
  // fill, which is the smallest bar that still reads as a bar and not a rule.
  int homeFeaturedProgressBarHeight;
  int homeFeaturedProgressBarStroke;
  // Right-edge overflow indicator of the home menu (3 px of ink, never scaled).
  int homeScrollIndicatorWidth;
  // Home icon rows: the glyph box, painted 1:1 inside the row pitch below.
  int menuIconSize;
  // 2 px stroke floor: 1 px is 0.086 mm and lands on a half-driven row after a
  // DU pass, so these three are exact pixel counts, not lengths.
  int headerRuleThickness;
  int chevronStroke;
  int chevronBoxWidth;
  int chevronBoxHeight;

  int buttonHintsHeight;
  int sideButtonHintsWidth;

  int progressBarHeight;
  int progressBarMarginTop;
  int statusBarHorizontalMargin;
  int statusBarVerticalMargin;
  // Air above and below the status bar's text lane, in PANEL pixels. The bar's
  // lane is statusBarVerticalMargin plus whatever a theme's statusBarRuleInset()
  // reserves, and on this panel the Ember 12 line box (30 px) fills it edge to
  // edge -- digits touching the rule above and the glass below, which is what
  // made the cluster read as cramped. A theme that wants breathing room states
  // it here; UITheme::getStatusBarHeight() reserves 2 x this on top, so the bar
  // and the page box still derive from one number. 0 -- the default, and what
  // every other theme keeps -- leaves the bar byte-identical.
  int statusBarTextPad;
  // Air BELOW the bar's text lane only, in PANEL pixels -- the bar's bottom
  // padding. Unlike statusBarTextPad (symmetric), this lifts the whole cluster
  // off the panel's bottom edge and the reader's page box pays for it: the pad is
  // inside the band getStatusBarHeight() reserves, so bandBottom (the safe edge)
  // does not move, bandTop rises by the pad, and the viewport loses exactly the
  // pad. 0 -- the default, and what every other theme keeps -- leaves the bar
  // byte-identical. See AndroTheme::drawStatusBar() for the lane half of it.
  int statusBarBottomPad;
  int keyboardKeyHeight;
  int keyboardKeySpacing;
  bool keyboardCenteredText;
  int keyboardVerticalOffset;
  int keyboardTextFieldWidthPercent;
  int keyboardWidthPercent;

  float popupTopOffsetRatio;
  int popupMarginX;
  int popupMarginY;
  int popupFrameThickness;
  int popupCornerRadius;
  bool popupTextBold;
  bool popupTextInverted;
  int popupTextBaselineOffsetY;
  int popupProgressBarHeight;
  bool popupProgressDrawOutline;
  bool popupProgressClampPercent;
  bool popupProgressFillInverted;
  bool popupProgressOutlineInverted;

  int optionPopupItemSpacing;
  int optionPopupInnerPadding;
  int optionPopupSelectionVPadding;
  int optionPopupDialogSideMargin;

  int textFieldHorizontalPadding;
  int textFieldNormalThickness;
  int textFieldCursorThickness;
  int textFieldLineEndOffset;

  // FreeInkUI control shape (the control center panel), same contract as the
  // list fields above: quick-setting tiles and slider step buttons, the
  // sheet's free-edge corners, and the capsule slider's corners (255 = full
  // stadium, i.e. radius = half the control height).
  int controlRadius;
  int sheetRadius;
  int capsuleRadius;
};

// ---------------------------------------------------------------------------
// HOW A THEME'S METRICS MEET THE PANEL DENSITY
//
// ThemeMetrics is DESIGN SPACE: every number in a theme's table is a length on
// the 480x800 @ 235 PPI canvas the UI was authored on. uiDensity::scale() converts
// design space to the physical panel, and UITheme::applyUiScale() performs that
// conversion in exactly one pass, walking the table below.
//
// The table is the ONLY list of fields that scaling visits: a field that is not
// in it is never scaled, and a field with the wrong class is caught by the boot
// audit (UITheme::auditUiScale) instead of by the operator's finger. Adding a
// metric to ThemeMetrics means adding it here; the audit names any field whose
// scaled copy disagrees with its class.
//
// MetricScale::Physical is the "must stay an exact pixel count" escape hatch and
// is deliberately explicit: asset/bitmap dimensions (they are cache keys and are
// drawn 1:1), counts, percentages, enum indices and sentinels. A theme author
// cannot make a length physical by accident - they have to write it down here.
enum class MetricScale : uint8_t {
  Design,    // a length on the design canvas: multiplied by uiDensity::scale()
  Physical,  // asset size / count / ratio / enum / sentinel: never scaled
};

struct MetricField {
  const char* name;
  int ThemeMetrics::*member;
  MetricScale scale;
};

// Every int field of ThemeMetrics, in declaration order. bool/float fields are
// flags and ratios, never geometry, so they are intentionally absent.
inline constexpr MetricField kMetricFields[] = {
    {"batteryWidth", &ThemeMetrics::batteryWidth, MetricScale::Design},
    {"batteryHeight", &ThemeMetrics::batteryHeight, MetricScale::Design},
    {"topPadding", &ThemeMetrics::topPadding, MetricScale::Design},
    {"batteryBarHeight", &ThemeMetrics::batteryBarHeight, MetricScale::Design},
    {"headerHeight", &ThemeMetrics::headerHeight, MetricScale::Design},
    {"verticalSpacing", &ThemeMetrics::verticalSpacing, MetricScale::Design},
    {"previewPadding", &ThemeMetrics::previewPadding, MetricScale::Design},
    {"previewHeightPercent", &ThemeMetrics::previewHeightPercent, MetricScale::Physical},
    {"contentSidePadding", &ThemeMetrics::contentSidePadding, MetricScale::Design},
    {"listRowHeight", &ThemeMetrics::listRowHeight, MetricScale::Design},
    {"listWithSubtitleRowHeight", &ThemeMetrics::listWithSubtitleRowHeight, MetricScale::Design},
    {"listRowGap", &ThemeMetrics::listRowGap, MetricScale::Design},
    {"listRowRadius", &ThemeMetrics::listRowRadius, MetricScale::Design},
    {"listInset", &ThemeMetrics::listInset, MetricScale::Design},
    {"listSidePadding", &ThemeMetrics::listSidePadding, MetricScale::Design},
    {"listSelectionStyle", &ThemeMetrics::listSelectionStyle, MetricScale::Physical},
    {"listScrollWidth", &ThemeMetrics::listScrollWidth, MetricScale::Design},
    {"listScrollSide", &ThemeMetrics::listScrollSide, MetricScale::Physical},
    {"headerSidePadding", &ThemeMetrics::headerSidePadding, MetricScale::Design},
    {"headerUnderlineSize", &ThemeMetrics::headerUnderlineSize, MetricScale::Design},
    {"headerTitleAlign", &ThemeMetrics::headerTitleAlign, MetricScale::Physical},
    {"headerBatterySide", &ThemeMetrics::headerBatterySide, MetricScale::Physical},
    {"menuRowHeight", &ThemeMetrics::menuRowHeight, MetricScale::Design},
    {"menuSpacing", &ThemeMetrics::menuSpacing, MetricScale::Design},
    {"tabSpacing", &ThemeMetrics::tabSpacing, MetricScale::Design},
    {"tabBarHeight", &ThemeMetrics::tabBarHeight, MetricScale::Design},
    {"coverGridTabBarHeight", &ThemeMetrics::coverGridTabBarHeight, MetricScale::Design},
    {"scrollBarWidth", &ThemeMetrics::scrollBarWidth, MetricScale::Design},
    {"scrollBarRightOffset", &ThemeMetrics::scrollBarRightOffset, MetricScale::Design},
    {"homeTopPadding", &ThemeMetrics::homeTopPadding, MetricScale::Design},
    // Cover bitmaps are dithered 1:1 and their height is part of the thumbnail
    // cache key; scaling would rebuild every cached thumb for no legibility gain.
    {"homeCoverHeight", &ThemeMetrics::homeCoverHeight, MetricScale::Physical},
    {"homeCoverTileHeight", &ThemeMetrics::homeCoverTileHeight, MetricScale::Physical},
    {"homeRecentBooksCount", &ThemeMetrics::homeRecentBooksCount, MetricScale::Physical},
    {"homeMenuTopOffset", &ThemeMetrics::homeMenuTopOffset, MetricScale::Design},
    {"homeFeaturedCardHeight", &ThemeMetrics::homeFeaturedCardHeight, MetricScale::Design},
    // Dithered 1:1 and part of the thumbnail cache key: scaling these would
    // regenerate every cached cover for no legibility gain.
    {"homeFeaturedCoverWidth", &ThemeMetrics::homeFeaturedCoverWidth, MetricScale::Physical},
    {"homeFeaturedCoverHeight", &ThemeMetrics::homeFeaturedCoverHeight, MetricScale::Physical},
    {"homeFeaturedCoverLargeWidth", &ThemeMetrics::homeFeaturedCoverLargeWidth, MetricScale::Physical},
    {"homeFeaturedCoverLargeHeight", &ThemeMetrics::homeFeaturedCoverLargeHeight, MetricScale::Physical},
    // Ink width of the featured cover's frame: a stroke, not a length.
    {"homeFeaturedCoverBorder", &ThemeMetrics::homeFeaturedCoverBorder, MetricScale::Physical},
    // The frame's corner radius is a length (the arc it throws scales with the
    // panel), while the bar's height and stroke are ink counts.
    {"homeFeaturedCoverRadius", &ThemeMetrics::homeFeaturedCoverRadius, MetricScale::Design},
    {"homeFeaturedProgressBarHeight", &ThemeMetrics::homeFeaturedProgressBarHeight, MetricScale::Physical},
    {"homeFeaturedProgressBarStroke", &ThemeMetrics::homeFeaturedProgressBarStroke, MetricScale::Physical},
    // Ink widths, not lengths: 2 px is the stroke floor that survives a DU
    // pass, and the 1-bit icon masks are drawn at their authored pixel size.
    {"homeScrollIndicatorWidth", &ThemeMetrics::homeScrollIndicatorWidth, MetricScale::Physical},
    {"menuIconSize", &ThemeMetrics::menuIconSize, MetricScale::Design},
    {"headerRuleThickness", &ThemeMetrics::headerRuleThickness, MetricScale::Physical},
    {"chevronStroke", &ThemeMetrics::chevronStroke, MetricScale::Physical},
    {"chevronBoxWidth", &ThemeMetrics::chevronBoxWidth, MetricScale::Physical},
    {"chevronBoxHeight", &ThemeMetrics::chevronBoxHeight, MetricScale::Physical},
    {"buttonHintsHeight", &ThemeMetrics::buttonHintsHeight, MetricScale::Design},
    {"sideButtonHintsWidth", &ThemeMetrics::sideButtonHintsWidth, MetricScale::Design},
    {"progressBarHeight", &ThemeMetrics::progressBarHeight, MetricScale::Design},
    {"progressBarMarginTop", &ThemeMetrics::progressBarMarginTop, MetricScale::Design},
    {"statusBarHorizontalMargin", &ThemeMetrics::statusBarHorizontalMargin, MetricScale::Design},
    {"statusBarVerticalMargin", &ThemeMetrics::statusBarVerticalMargin, MetricScale::Design},
    // Ink-free padding, not a length: 2 px is 2 px on any panel (and 0 keeps
    // every other theme's bar exactly where it was).
    {"statusBarTextPad", &ThemeMetrics::statusBarTextPad, MetricScale::Physical},
    // Same class as statusBarTextPad: ink-free padding on the bar's text lane,
    // 5 px is 5 px on any panel (0 keeps every other theme's bar where it was).
    {"statusBarBottomPad", &ThemeMetrics::statusBarBottomPad, MetricScale::Physical},
    {"keyboardKeyHeight", &ThemeMetrics::keyboardKeyHeight, MetricScale::Design},
    {"keyboardKeySpacing", &ThemeMetrics::keyboardKeySpacing, MetricScale::Design},
    {"keyboardVerticalOffset", &ThemeMetrics::keyboardVerticalOffset, MetricScale::Design},
    {"keyboardTextFieldWidthPercent", &ThemeMetrics::keyboardTextFieldWidthPercent, MetricScale::Physical},
    {"keyboardWidthPercent", &ThemeMetrics::keyboardWidthPercent, MetricScale::Physical},
    {"popupMarginX", &ThemeMetrics::popupMarginX, MetricScale::Design},
    {"popupMarginY", &ThemeMetrics::popupMarginY, MetricScale::Design},
    {"popupFrameThickness", &ThemeMetrics::popupFrameThickness, MetricScale::Design},
    {"popupCornerRadius", &ThemeMetrics::popupCornerRadius, MetricScale::Design},
    {"popupTextBaselineOffsetY", &ThemeMetrics::popupTextBaselineOffsetY, MetricScale::Design},
    {"popupProgressBarHeight", &ThemeMetrics::popupProgressBarHeight, MetricScale::Design},
    {"optionPopupItemSpacing", &ThemeMetrics::optionPopupItemSpacing, MetricScale::Design},
    {"optionPopupInnerPadding", &ThemeMetrics::optionPopupInnerPadding, MetricScale::Design},
    {"optionPopupSelectionVPadding", &ThemeMetrics::optionPopupSelectionVPadding, MetricScale::Design},
    {"optionPopupDialogSideMargin", &ThemeMetrics::optionPopupDialogSideMargin, MetricScale::Design},
    {"textFieldHorizontalPadding", &ThemeMetrics::textFieldHorizontalPadding, MetricScale::Design},
    {"textFieldNormalThickness", &ThemeMetrics::textFieldNormalThickness, MetricScale::Design},
    {"textFieldCursorThickness", &ThemeMetrics::textFieldCursorThickness, MetricScale::Design},
    {"textFieldLineEndOffset", &ThemeMetrics::textFieldLineEndOffset, MetricScale::Design},
    {"controlRadius", &ThemeMetrics::controlRadius, MetricScale::Design},
    {"sheetRadius", &ThemeMetrics::sheetRadius, MetricScale::Design},
    // 255 is fui's "full stadium" sentinel (radius = half the height), not a
    // length: applyUiScale() restores it after the pass. Every other value is a
    // design-space corner radius.
    {"capsuleRadius", &ThemeMetrics::capsuleRadius, MetricScale::Design},
};

// The button menu is the ONLY surface a theme lays out itself and then
// hit-tests, so its geometry lives here instead of in each draw override: the
// painter fills this struct and HomeActivity maps taps through the same one.
// Panel pixels - read it from getMenuLayout(), never recompute it.
struct MenuLayout {
  int top = 0;         // y of the first painted row
  int rowHeight = 0;   // painted height of a row
  int step = 0;        // row pitch (rowHeight + gap)
  int firstIndex = 0;  // index of the first painted row (paging themes)
  int rows = 0;        // rows painted in the band this frame
};

enum UIIcon {
  None = 0,
  Folder,
  Text,
  Image,
  Book,
  File,
  Recent,
  Settings,
  Transfer,
  Library,
  Plugins,
  Wifi,
  Hotspot,
  Bookmark,
  Usb,
  Blocks
};

// Default theme implementation (Classic Theme)
// Additional themes can inherit from this and override methods as needed

namespace BaseMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 84,
                                 .verticalSpacing = 10,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 30,
                                 .listWithSubtitleRowHeight = 50,
                                 .listRowGap = 0,
                                 .listRowRadius = 0,
                                 .listInset = 0,
                                 .listSidePadding = 20,
                                 .listSelectionStyle = 0,  // invert fill
                                 .listScrollWidth = 4,
                                 .listScrollSide = 0,
                                 .listTitleBold = false,
                                 .headerSidePadding = 18,
                                 .headerUnderlineSize = 0,
                                 .headerTitleAlign = 1,  // centered
                                 .headerBatterySide = 0,
                                 // Corner clock: a centered clock would collide with the centered title.
                                 .headerClockCentered = false,
                                 .menuRowHeight = 45,
                                 .menuSpacing = 8,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 40,
                                 .homeCoverHeight = 400,
                                 .homeCoverTileHeight = 400,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 10,
                                 // Home featured card / icon rows. Classic does
                                 // not draw these surfaces, but the fields are
                                 // classified in kMetricFields, so they carry the
                                 // authored design values rather than zeros.
                                 .homeFeaturedCardHeight = 186,
                                 .homeFeaturedCoverWidth = 62,
                                 .homeFeaturedCoverHeight = 84,
                                 .homeFeaturedCoverLargeWidth = 88,
                                 .homeFeaturedCoverLargeHeight = 118,
                                 // Same authored shape as Andro; Classic paints
                                 // neither the frame nor the bar.
                                 .homeFeaturedCoverRadius = 9,
                                 .homeFeaturedProgressBarHeight = 12,
                                 .homeFeaturedProgressBarStroke = 2,
                                 .homeScrollIndicatorWidth = 3,
                                 .menuIconSize = 22,
                                 .headerRuleThickness = 2,
                                 .chevronStroke = 2,
                                 .chevronBoxWidth = 10,
                                 .chevronBoxHeight = 16,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 // 0: every theme but Andro keeps its bar's lane
                                 // exactly as tall as it has always been.
                                 .statusBarTextPad = 0,
                                 // 0: no theme but Andro asks for bottom air in
                                 // the bar, so no other bar moves a pixel.
                                 .statusBarBottomPad = 0,
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
                                 .optionPopupItemSpacing = 6,
                                 .optionPopupInnerPadding = 16,
                                 .optionPopupSelectionVPadding = 4,
                                 .optionPopupDialogSideMargin = 20,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0,
                                 .controlRadius = 0,
                                 .sheetRadius = 0,
                                 .capsuleRadius = 0};
}

class BaseTheme {
 public:
  virtual ~BaseTheme() = default;

  // Component drawing methods
  static void setCheckboxRow(freeink::ui::ListItem& item, bool checked);
  static void drawCoverPlaceholder(const GfxRenderer& renderer, Rect rect);
  // Draws a pre-dithered cover thumb 1:1, centered and clipped to fill the
  // slot. Rescaling a dithered bitmap aliases badly, so overflow is cropped.
  static bool drawCoverThumbFill(const GfxRenderer& renderer, const Bitmap& bitmap, Rect slot, int xOffset = 0);
  static void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total);
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect,
                       bool showPercentage = true) const;  // Left aligned (reader mode)
  virtual void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const;
  virtual void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                               const char* btn4) const;
  // Shared by every theme's drawButtonHints(): centres a hint label in its box,
  // wrapping to two lines rather than overflowing when it's too wide to fit.
  static void drawHintLabel(const GfxRenderer& renderer, int fontId, const char* label, int x, int boxWidth, int boxTop,
                            int boxHeight);
  virtual void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const;
  // --- button menu geometry: ONE producer, TWO consumers ---------------------
  // A theme may change the row height, the row pitch, or page the rows; it does
  // that by overriding these accessors, and BOTH drawButtonMenu() (paint) and
  // HomeActivity (touch) read getMenuLayout(). Never derive a menu rect twice:
  // that duplication is exactly how r59's density scale left the painted rows on
  // design pixel counts while the touch grid stepped by the scaled ones.
  virtual int getMenuRowHeight(const GfxRenderer& renderer) const;
  // Row pitch = row height + the theme's gap. Defaults to the scaled metrics.
  virtual int getMenuRowStep(const GfxRenderer& renderer) const;
  // The full band layout for this frame. Painters iterate it; the touch grid
  // maps a tap y to firstIndex + (y - top) / step.
  virtual MenuLayout getMenuLayout(const GfxRenderer& renderer, Rect band, int buttonCount,
                                   int selectedIndex) const;
  // True when getMenuRowHeight() derives from something other than ThemeMetrics
  // (RoundedRaff: the title font's line height). The boot audit
  // (UITheme::auditUiScale) skips the "did this theme scale?" check for those;
  // false means the height MUST come from the scaled metrics.
  virtual bool menuRowHeightIsDerived() const { return false; }
  // Top edge of row `index` as painted, in panel pixels.
  int menuRowTop(const MenuLayout& layout, const int index) const {
    return layout.top + (index - layout.firstIndex) * layout.step;
  }

  // Also draws the wall clock opposite the battery when the user enabled
  // SETTINGS.clockShowInHeader and an RTC is present. On touch boards a
  // tappable back button leads the band (see HeaderBackTapTarget); root
  // screens that own their stack bottom pass backButton = false.
  // The body lives in components/TitledHeaderBand.h: one implementation, shared
  // with every screen that composes its own HeaderProps.
  virtual void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle = nullptr,
                          bool backButton = true) const;
  // Dialogs (OptionPopup, the slider dialogs and every reader confirm that goes
  // through them). Two knobs, both 0 = the legacy SDK treatment, so a theme
  // that does not opt in keeps Classic/Lyra/Lyra3Covers/CoverGrid/RoundedRaff
  // byte-identical.
  //
  // dialogRuleThickness: the theme's rule weight for dialog chrome, in panel
  // pixels. When non-zero the SELECTED option is marked by an outline of that
  // weight and no fill at all, and the dialog's frame is drawn at the same
  // weight -- the fill-free vocabulary this theme already uses for its pager
  // buttons and its active tab. (Andro's board is touch-only: an inverting
  // selection is the affordance r70 removed from its lists.)
  virtual int dialogRuleThickness() const { return 0; }
  // Face for a slider dialog's headline readout; 0 = NOTOSANS_18, the fixed
  // hero face every other theme has always used. Applied only when the face
  // covers every codepoint of the readout (see textFitsFace).
  virtual int dialogReadoutFontId() const { return 0; }

  // Icon box for the READER toolbar sheet -- the Contents/Text/More row and the
  // two scrub chevrons -- in panel pixels. 0 (the default, every theme but
  // Andro) keeps the 24 px glyphs authored at 24. A theme that returns a size
  // here samples the 64 px masks in readerToolbarIcons64.h down to it instead
  // of upscaling 24 px art.
  virtual int readerToolbarIconSize() const { return 0; }

  // Face for a screen's CAPTION / readout line -- the reader menu's chapter-page
  // line, a sheet's page counter -- in a font id, or 0 for the UI small face
  // (uiScaleSpec().smallFontId). A theme whose captions live in its own face
  // says so here rather than every caller reaching for a specific rung of a
  // ladder that means something else (Andro: Ember 12, the face its status bar
  // and dialog readouts already use).
  virtual int captionFontId() const { return 0; }

  // True when every codepoint of `text` is drawable by the face `fontId`, i.e.
  // the face can be used for that string. NO non-Latin fallback is registered
  // for the Ember ids (SdCardFontSystem maps only SMALL/UI_10/UI_12), so an
  // unguarded Ember string prints holes in Hebrew, Arabic, Greek or CJK. Public
  // because every Ember call site -- the titled band, the settings rows, Home's
  // menu labels, featured title/author and status line -- has to ask the same
  // question of the SAME loaded face instead of hardcoding a script list.
  static bool textFitsFace(const GfxRenderer& renderer, int fontId, const char* text);
  // The theme's rule under a header band it just painted, in the band's own
  // rect. Nothing by default: every theme but Andro gets its rule from the
  // header style's bottom border (metrics.headerUnderlineSize). Andro paints its
  // 6 px Physical rule here, which is why it lives in a hook instead of inside
  // drawHeader -- a screen that composes its header through TitledHeaderBand.h
  // must carry the same ink, and must never end up with two rules.
  virtual void drawTitledBandRule(const GfxRenderer& renderer, Rect band) const {
    (void)renderer;
    (void)band;
  }
  // True when drawTitledBandRule() above is the band bottom's ONLY stroke.
  //
  // Three independent mechanisms can ink a header band's bottom edge:
  //   1. the FreeInkUI frame's own bottom border (components/controls/header.h
  //      draws it when the resolved style carries a border AND a width),
  //   2. the header-style underline the shared band adds when
  //      tokens.headerUnderline > 0 (Design-scaled, so Lyra's 3 -> 4 px),
  //   3. drawTitledBandRule() above, painted after the frame.
  // A theme that answers true gets (1) and (2) suppressed by the shared band and
  // carries its rule from (3) alone: one edge, one stroke, and the invariant does
  // not rest on a metric happening to be 0. A theme that answers false keeps the
  // frame border it has always had and leaves the hook empty.
  virtual bool paintsOwnBandRule() const { return false; }
  // Fill the battery/clock status chrome (settings + theme metrics) into
  // header props, so FUI-native screens drawing their own interactive header
  // carry the same band as drawHeader. Status text is styled with the
  // FONT_LABEL slot (bound to the fixed small font by makeUiTarget and
  // drawHeader). The label strings point at internal static buffers refreshed
  // per call (headers draw on the single render task).
  static void applyHeaderStatus(const GfxRenderer& renderer, freeink::ui::HeaderProps& props);
  // The header band every screen draws in: the caller's full-width rect inset
  // by the board's side insets, its top edge lowered by the board's
  // headerOffsetY (bottom edge fixed, so content below keeps its boundary).
  // Sets props.bleed so the band's fill and rule still span `rect`, and widens
  // the status line's edge inset to clear rounded corners. Call after
  // applyHeaderStatus.
  static freeink::ui::Rect headerBand(const GfxRenderer& renderer, freeink::ui::Rect rect,
                                      freeink::ui::HeaderProps& props);
  // Edge inset drawHeader uses for the clock/battery status line (detached
  // layouts hug the corner with a legacy 12px inset instead of the padding).
  static int headerStatusInset();
  virtual void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                             const char* rightLabel = nullptr) const;
  virtual void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                   const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                   bool& bufferRestored, std::function<bool()> storeCoverBuffer) const;
  virtual void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon) const;
  static void drawSelectionHandle(const GfxRenderer& renderer, Rect rect, bool left);
  // C13 (upstream 6b863e48): firstAction skips leading labels so a screen can drop an
  // action that has nowhere to go (Lookup with no dictionary installed) and still draw
  // the actions that remain, in order, with no empty slot.
  static void drawSelectionActions(const GfxRenderer& renderer, Rect rect, int count = 3, int firstAction = 0);
  // commit=false draws the box into the framebuffer and returns WITHOUT its own HALF drive. The
  // caller that will immediately compose and drive a whole replacement frame (the sleep screen)
  // passes false: a popup box over a page that is about to be wiped is not a frame on glass, and
  // committing it costs one whole-panel GC16 drive nobody ever sees (r101.8).
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message, const bool commit = true) const;
  virtual void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress,
                                 const bool commit = true) const;
  virtual void drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage, const int pageCount,
                             std::string title, const int paddingBottom = 0, const int textYOffset = 0,
                             const bool fillMargin = true, const bool isPageBookmarked = false,
                             const bool pageCountEstimated = false) const;
  // Extra rows ABOVE the status bar's text lane that a theme needs for its own
  // chrome (Andro rules the bar's top edge with the 6 px header rule, which does
  // not fit in statusBarVerticalMargin's lane). 0 -- the default, and what every
  // other theme returns -- leaves getStatusBarHeight() byte-identical, so the
  // reader's page box, the XTC overlay and the status-bar preview do not move.
  virtual int statusBarRuleInset() const { return 0; }
  static void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label);
  virtual void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode = false,
                             int contentStartX = 0, int contentWidth = 0) const;
  virtual bool showsFileIcons() const { return false; }
  // Thumb generation height for home covers; 0 means use metrics.homeCoverHeight.
  // Themes with slots wider than 0.6 aspect override this so covers still fill.
  virtual int homeCoverThumbHeight(const GfxRenderer&) const { return 0; }
  // Row height for FreeInkUI lists on a TOUCH board, in panel pixels. 0 -- the
  // default, and what every theme but Andro returns -- means "keep the caller's
  // content-derived measure" (a label line plus the theme's touch padding), so
  // Classic/Lyra/Lyra3Covers/CoverGrid/RoundedRaff rows are byte-for-byte what
  // they were. A theme that returns a height owns the row PITCH outright: the
  // screen uses it as the authoritative row measure, and the SDK still grows an
  // individual row whose own content (wrapped label, subtitle, icon) needs more.
  // This is a virtual, not a ThemeMetrics field, on purpose: a metric field is
  // scaled by kMetricFields[] and audited at boot, and the value a theme wants
  // here is the ALREADY-SCALED one, which the theme derives where it is written.
  virtual int listTouchRowHeight() const { return 0; }

  // Titled-screen status chrome (battery + clock). True -- the default, and
  // what every theme but Andro returns -- keeps the cluster BaseTheme's header
  // fills via applyHeaderStatus(). Andro returns false: its titled bands are the
  // Android app bar it mirrors (back arrow + title + rule), with no clock or
  // battery line. The geometry applyHeaderStatus computes is unchanged and the
  // home band is unaffected (Andro paints its own cluster there), so a theme
  // that hides the cluster moves no other theme's header by a pixel.
  virtual bool headerShowsStatusCluster() const { return true; }

  // Height of a TITLED screen's header band, in PANEL pixels. 0 -- the default,
  // and what every theme but Andro returns -- means "the same band as the home
  // screen", i.e. metrics.headerHeight. Andro returns 2x: its titled settings
  // and file-transfer screens carry the Android app-bar band (twice the home
  // status band), with the title face and the leading arrow scaled to match.
  // Read it through UITheme::getTitledHeaderHeight(), which resolves the 0 case
  // to headerHeight, so the band a screen paints and the content margin it
  // leaves under that band can never disagree.
  virtual int titledHeaderHeight() const { return 0; }
  // Resolved titled-band height: titledHeaderHeight() when the theme asks for
  // one, else metrics.headerHeight. THE single source for both.
  int titledHeaderPx() const;
  // Title face for a titled band on such a theme; 0 = uiScaleSpec().titleFontId
  // (UI_12). Only applied when the loaded face covers every codepoint of the
  // title -- the Ember faces have no fallback registered, so a non-Latin title
  // keeps the UI font rather than printing tofu.
  virtual int titledHeaderFontId() const { return 0; }
  // Square of the leading (back) button on a titled band; 0 = the shared 48 px.
  // A taller band scales it, and the arrow mask is chosen by that size so the
  // glyph is drawn from a mask authored at its size, never upscaled.
  virtual int titledHeaderButtonSize() const { return 0; }

  // True when this theme lays a settings list out as whole pages of its own and
  // therefore wants the pager: a vertical swipe turns the page and a footer of
  // page tabs appears when the rows do not fit. False -- the default, and what
  // every theme but Andro returns -- keeps the r73 behaviour exactly: a swipe
  // scrolls the viewport, the selection stays put, and there is no footer.
  virtual bool settingsListPager() const { return false; }
  // Height, in panel pixels, of the footer band that paging settings list gives
  // up to its page buttons. 0 -- the default, and what every theme but Andro
  // returns -- means metrics.tabBarHeight, i.e. the caption strip the theme
  // already reserves elsewhere, so a theme that does not opt in keeps its band
  // to the pixel. A theme whose page buttons are FINGER-sized tap targets
  // states its own: that band is not a caption strip, it is the control.
  virtual int settingsPagerBandHeight() const { return 0; }

  // Tab-bar rule language, in panel pixels. 0 -- the default, and what every
  // theme but Andro returns -- resolves to the legacy expression the tab bar has
  // always used (max(1, metrics.headerUnderlineSize)), so a theme that does not
  // opt in keeps its exact divider.
  virtual int tabBarRuleThickness() const { return 0; }
  // Thickness of the ACTIVE-tab underline, or 0 (the default) for the legacy
  // treatment: a dithered pill behind the selected label plus a 2 px underline.
  // A theme that returns a weight here says "no fill, no band wash -- the active
  // tab is marked by an underline of this weight", which is the only fill-free
  // indication the FreeInkUI tab bar can paint for one tab without changing the
  // tab hit rects (each tab's slot, not its pill, is what the SDK registers).
  virtual int tabActiveUnderlineThickness() const { return 0; }

  // True when this theme's LIST screens all wear its own list chrome: the
  // titled header band, the row label face, the row icon box and the band-filling
  // row pitch. False -- the default, and what every theme but Andro returns --
  // keeps every list screen at its r72 geometry. This is the THEME half of the
  // opt-in; UiListActivity::usesThemeListChrome() is the single decision point
  // both the settings tree and the non-settings lists read, so there is exactly
  // one code path (and one set of knobs) behind them.
  virtual bool themeListChrome() const { return false; }

  // Settings-row type and icon box, in font ids and panel pixels. 0 -- the
  // default, and what every theme but Andro returns -- keeps the caller's own
  // measure (the theme's bodyText font, the caller's icon size constant). A
  // theme that fills these owns the label font and the row icon box on the
  // settings tree only; the SDK still measures each row from its content, so
  // value, toggle, subtitle and tap geometry are untouched.
  virtual int listRowFontId() const { return 0; }
  virtual int listRowIconSize() const { return 0; }
  // The rungs a themed row face may step DOWN through, largest first, when a
  // label is too long for the row at the theme's preferred size (r99.9, operator
  // request D: no truncated menu label, one face per screen). Empty -- the
  // default -- means "never shrink": a theme that does not supply a ladder keeps
  // exactly the face listRowFontId() names, so Classic/Lyra/RoundedRaff are
  // untouched. The caller picks ONE rung for the whole screen (every label on
  // one line), never a per-row size, so row heights stay uniform.
  virtual const int* listRowFontLadder(int& count) const {
    count = 0;
    return nullptr;
  }
  // The denser variant of that face for screens whose rows are file names rather
  // than menu labels (the file browser), or 0 to keep listRowFontId(). Andro
  // answers with Ember 18, a ~25 % smaller body than the 24 the menu rows use.
  virtual int listRowDenseFontId() const { return 0; }

  // Row pitch for a themed list, in panel pixels, or 0 (the default for every
  // theme but Andro) to leave the caller's measure alone. The theme owns the
  // whole decision -- the minimum row measure, whether the list fits on ONE page
  // at that measure, and how far a fitting list may spread before the cap -- so
  // the painted rows and the tap bands stay one rect: the value returned here is
  // used as ListProps::rowHeight, never as a second layout pass. A theme that
  // returns a pitch also leaves rowGap at 0, so the pitch IS the row.
  virtual int listRowFillPitch(const int bandHeight, const int rowCount) const {
    (void)bandHeight;
    (void)rowCount;
    return 0;
  }

  // r78: the bookmark marker the reader status bar paints beside the battery
  // cluster. Hoisted out of BaseTheme.cpp's anonymous namespace so a theme that
  // overrides drawStatusBar (Andro) marks a bookmarked page with the SAME 16 px
  // art instead of inventing a second glyph.
  static constexpr int bookmarkStatusIconWidth = 16;
  static constexpr int bookmarkStatusIconGap = 4;
  static void drawBookmarkStatusIcon(const GfxRenderer& renderer, int x, int y);
  // Shared constants and helpers for battery drawing (used by all themes)
  static constexpr int batteryPercentSpacing = 4;
  static void drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight);
  // BATT6: the ONE primitive every theme draws the percentage NUMBER through, for
  // the same reason drawBatteryOutline exists for the glyph -- it draws the digits
  // AND records the box they landed in, so the change producer
  // (display/BatteryPercentPatch.cpp) has an exact rectangle instead of a
  // re-derived one. `anchorX` is the pen origin for a LEFT label and the text's
  // right edge for a RIGHT one; `value` is the percentage being printed, recorded
  // as what the glass shows if this paint drives.
  enum class PercentAlign : uint8_t { Left, Right };
  static void drawBatteryPercentLabel(const GfxRenderer& renderer, int anchorX, int y, int fontId, PercentAlign align,
                                      const std::string& text, uint16_t value);
  static void drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY);
};
