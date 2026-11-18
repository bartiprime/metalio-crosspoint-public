// PWR9 -- "the power-off screen in TEXT mode draws only the border and no 'Powered off'".
//
// WHAT THIS PROVES, AND HOW. The frame's ring and box were landing and the words were not,
// which is what a border-only glass looks like. The word could only be missing for one reason
// that is visible from here: it was inked in the colour of the field it sits on (white ink on
// the white field of a light frame, black ink on the black field of the DARK one). This test
// paints the REAL composition -- src/activities/util/PowerOffDialogFrame.h, the same header the
// firmware instantiates with GfxRenderer -- onto a 1-bit surface and counts pixels that differ
// from the page background inside the text's own rect. A missing word means zero such pixels.
//
// The rasteriser below is a double, and deliberately a dumb one: fillRect paints the whole rect
// in the ink its `black` argument names (GfxRenderer::fillRect -> fillRectImpl<Color::Black/
// White>), fillRoundedRect degenerates to a rect, and every glyph stamps a solid block inside
// the line band the caller placed it in. So the assertions can only be satisfied by the
// PRODUCTION geometry and colour pairing: nothing here draws a border or a box by hand.
//
// RED BEFORE THE FIX on both flat (popupCornerRadius == 0, the default theme) cases; the two
// rounded cases are green before and after and are kept as the regression guard, because the
// rounded branch fills its field with the opposite of the page and therefore never had this bug.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

// GfxRenderer.h's palette, by value: the composition's rounded branch names Color::Black /
// Color::White. Kept identical to lib/GfxRenderer/GfxRenderer.h so the double cannot quietly
// disagree with the device about which value means ink.
enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

#include "../../src/activities/util/PowerOffDialogFrame.h"

namespace {

constexpr int kScreenW = 800;
constexpr int kScreenH = 480;
constexpr int kLineH = 22;     // NOTOSANS_18-ish line box
constexpr int kAdvance = 11;   // per-glyph advance; a triple-width glyph, like a real bold face
constexpr int kGlyphW = 9;     // inked block inside each advance cell
constexpr int kGlyphH = 15;

// The theme metrics the frame reads. Values are BaseTheme's design metrics for the popup chrome
// (the same numbers PowerOffActivity gets from UITheme::getInstance().getMetrics()).
struct TestMetrics {
  int popupMarginX = 15;
  int popupMarginY = 15;
  int popupFrameThickness = 2;
  int popupCornerRadius = 0;
  int popupTextBaselineOffsetY = -2;
};

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// A 1-BIT SURFACE. 1 = white (paper), 0 = black (ink) -- the same polarity the panel and
// GfxRenderer::fillRect use (fillRect(state=true) writes black).
class OneBitSurface {
 public:
  OneBitSurface(bool pageWhite) : pageWhite_(pageWhite), px_(static_cast<size_t>(kScreenW * kScreenH)) {
    px_.assign(px_.size(), pageWhite ? 1 : 0);
  }

  int getScreenWidth() const { return kScreenW; }
  int getScreenHeight() const { return kScreenH; }
  int getLineHeight(int) const { return kLineH; }
  int getTextWidth(int, const char* text, int) const { return static_cast<int>(std::strlen(text)) * kAdvance; }

  void fillRect(int x, int y, int w, int h, bool black) const { blit(x, y, w, h, black ? 0 : 1); }

  void fillRoundedRect(int x, int y, int w, int h, int, Color color) const {
    fillRect(x, y, w, h, color == Color::Black);
  }

  // The word: one solid block per glyph, inside the line band the caller gave us. The rect
  // handed to drawCenteredText is the text's rect -- that is what the assertions read.
  void drawCenteredText(int, int y, const char* text, bool black, int) const {
    const int width = getTextWidth(0, text, 0);
    const int x = (kScreenW - width) / 2;
    textRect_ = Rect{x, y, width, kLineH};
    const int ink = black ? 0 : 1;
    for (int i = 0; text[i] != '\0'; ++i) {
      blit(x + i * kAdvance, y, kGlyphW, kGlyphH, ink);
    }
  }

  Rect textRect() const { return textRect_; }
  int pageInk() const { return pageWhite_ ? 1 : 0; }

  // Pixels inside a rect that are NOT the page's own colour: the words are inked iff this is
  // non-zero over the text rect.
  int nonBackgroundIn(const Rect& r) const {
    int n = 0;
    for (int yy = r.y; yy < r.y + r.h; ++yy) {
      for (int xx = r.x; xx < r.x + r.w; ++xx) {
        if (xx < 0 || yy < 0 || xx >= kScreenW || yy >= kScreenH) continue;
        if (at(xx, yy) != pageInk()) ++n;
      }
    }
    return n;
  }

 private:
  void blit(int x, int y, int w, int h, int ink) const {
    for (int yy = y; yy < y + h; ++yy) {
      for (int xx = x; xx < x + w; ++xx) {
        if (xx < 0 || yy < 0 || xx >= kScreenW || yy >= kScreenH) continue;
        px_[static_cast<size_t>(yy * kScreenW + xx)] = static_cast<uint8_t>(ink);
      }
    }
  }
  int at(int x, int y) const { return px_[static_cast<size_t>(y * kScreenW + x)]; }

  bool pageWhite_;
  mutable std::vector<uint8_t> px_;
  mutable Rect textRect_;
};

const char* kText = "Powered off";

// Text ink must come from the box the branch painted, never from the page's own `darkInk`.
struct Case {
  const char* name;
  int radius;
  bool darkInk;
};

const Case kCases[] = {
    {"flat_light_default_theme", 0, false},   // RED before: white ink on the white field
    {"flat_dark", 0, true},                   // RED before: black ink on the black field
    {"rounded_light", 6, false},              // guard: already correct
    {"rounded_dark", 6, true},                // guard: already correct
};

void paint(const Case& c, OneBitSurface& surface) {
  TestMetrics metrics;
  metrics.popupCornerRadius = c.radius;
  // fontId 0 stands in for the guarded NOTOSANS_18/UI_12 pick; the double reports one line box.
  paintPowerOffDialogFrame(surface, metrics, 0, kText, c.darkInk, /*style=*/0);
}

class PowerOffTextFrame : public ::testing::Test {};

}  // namespace

TEST_F(PowerOffTextFrame, WordsAreInkedInsideTheirRect) {
  for (const auto& c : kCases) {
    OneBitSurface surface(/*pageWhite=*/!c.darkInk);
    paint(c, surface);
    const auto text = surface.textRect();
    EXPECT_EQ(text.w, static_cast<int>(std::strlen(kText)) * kAdvance);
    // A frame was painted: something on this face is not the page. (The border, still there.)
    EXPECT_GT(surface.nonBackgroundIn(Rect{0, 0, kScreenW, kScreenH}), 0) << c.name;
    // The words: ink inside their own rect. This is the failing assertion before the fix --
    // a white field with white ink, or a black field with black ink, leaves this at 0.
    EXPECT_GT(surface.nonBackgroundIn(text), 0) << c.name << ": the text rect carries no ink (page colour)";
    // ...and enough of it that a stray antialiasing dot could not satisfy the check: the glyph
    // blocks cover kGlyphW*kGlyphH per character, so a real word inks most of its band.
    EXPECT_GE(surface.nonBackgroundIn(text), static_cast<int>(std::strlen(kText)) * kGlyphW * kGlyphH / 2)
        << c.name << ": only a sliver of the text rect is inked";
  }
}
