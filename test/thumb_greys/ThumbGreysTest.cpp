#include <PixFormat.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

// r164 THUMB1 — the 2 bpp cover-thumbnail quantiser.
//
// Three claims are being pinned here, and they are the three the brief is about:
//
//  1. NO COLLAPSE AT THE EXTREMES. The 1-bit Atkinson ditherer this replaces could
//     not produce a white pixel below grey 37 or a black one above grey 224
//     (R164_THUMB1_INVESTIGATION §3.2) — a flat field there became solid black or
//     solid white regardless of its value. The ordered quantiser must reach both
//     endpoints, and it must do so for the exact grey, not from some threshold.
//
//  2. LEVEL -> LEVEL. The file stores a 2 bpp palette index. Bitmap::readNextRow16
//     turns that index back into a 16-level optical value through the palette, and
//     the arithmetic there re-dithers with the SAME 4x4 matrix. The claim is that
//     index i lands on level 5*i for EVERY matrix cell — i.e. the file's index IS the
//     optical level, and the only dither in the whole path is the one at generation
//     time. If that ever stops being true, the drawn tone is doubly-dithered noise.
//
//  3. A FLAT FIELD REPRODUCES ITS OWN GREY. This is the measured regression: the
//     source cover's flat navy band is grey 39, and the shipped thumb rendered it at
//     mean 9.9 with sd 49.3. Averaged over a matrix period the new quantiser must
//     land within one level of the ideal, for every input grey, monotonically.
namespace {

// Mean optical level (0..15) of a W x H field of `gray`, quantised and then read back
// through the same conversion the draw path uses.
double meanLevel(const int gray, const int w = 16, const int h = 16) {
  int sum = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint8_t index = pixfmt::coverDitherIndex(static_cast<uint8_t>(gray), x, y);
      sum += pixfmt::coverIndexToLevel(index);
    }
  }
  return static_cast<double>(sum) / (w * h);
}

// The 16-level value Bitmap::readNextRow16() derives from palette luminance `lum`.
// Copied arithmetic on purpose: the point of the test is that the two agree, and a
// test that called into Bitmap.cpp would need a file to read.
uint8_t levelFromLum(const uint8_t lum, const int x, const int y) {
  int t = (static_cast<int>(lum) * 240) / 255;
  t += static_cast<int>(pixfmt::kBayer4x4[y & 3][x & 3]) - 8;
  if (t < 0) t = 0;
  int level = (t + 8) / 16;
  if (level > 15) level = 15;
  return static_cast<uint8_t>(level);
}

}  // namespace

TEST(ThumbGreys, PaletteTableMatchesTheSixteenLevelAxis) {
  // The palette luminance and the level must be the same number in two units, or the
  // draw path's inversion lands on a different level than the generator chose.
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(pixfmt::kCoverGrayLum[i], pixfmt::kCoverGrayLevel[i] * 17);
    EXPECT_EQ(pixfmt::kCoverGrayLevel[i], 5 * i) << "levels must be evenly spaced: the "
                                                    "dither AVERAGES them";
  }
}

TEST(ThumbGreys, IndexMapsToItsOwnLevelThroughThePalette) {
  // Claim 2, exhaustively over the matrix: no cell may shift an index off its level.
  for (int i = 0; i < 4; ++i) {
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) {
        EXPECT_EQ(levelFromLum(pixfmt::kCoverGrayLum[i], x, y), pixfmt::coverIndexToLevel(i))
            << "index " << i << " at matrix cell (" << x << "," << y << ")";
      }
    }
  }
}

TEST(ThumbGreys, EndpointsDoNotCollapse) {
  // Claim 1. Every cell of a pure-black field is index 0 and every cell of a pure-white
  // field is index 3 — and the neighbours of those values stay put too, so a black
  // background cannot speckle and white paper cannot grey.
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      EXPECT_EQ(pixfmt::coverDitherIndex(0, x, y), 0);
      EXPECT_EQ(pixfmt::coverDitherIndex(255, x, y), 3);
    }
  }
  // The failure this replaces: grey 30 and grey 37 rendered as ZERO white pixels under
  // Atkinson. Here they must still carry tone (grey 37 is level ~2.2 of 15).
  EXPECT_NEAR(meanLevel(30), 30.0 * 15.0 / 255.0, 0.6);
  EXPECT_NEAR(meanLevel(37), 37.0 * 15.0 / 255.0, 0.6);
  EXPECT_NEAR(meanLevel(225), 225.0 * 15.0 / 255.0, 0.6);
}

TEST(ThumbGreys, FlatFieldReproducesItsOwnGrey) {
  // Claim 3, for every input code. Tolerance is one half-level in each direction: the
  // ordered matrix cannot do better than that, and half a level is below what the
  // panel can resolve (the 16-level path's own step).
  double worst = 0.0;
  int worstGray = -1;
  for (int g = 0; g < 256; ++g) {
    const double ideal = g * 15.0 / 255.0;
    const double got = meanLevel(g);
    const double err = got - ideal;
    if (std::abs(err) > std::abs(worst)) {
      worst = err;
      worstGray = g;
    }
    ASSERT_LT(std::abs(err), 0.55) << "grey " << g << ": ideal " << ideal << " got " << got;
  }
  // Report the worst case in the failure message above rather than silently passing.
  EXPECT_LE(std::abs(worst), 0.55) << "worst at grey " << worstGray;
}

TEST(ThumbGreys, IsMonotonic) {
  // A quantiser that dips where its input rises turns a gradient into banding with a
  // reversed step. The legacy threshold table (0/15, 1/35, 2/90, 3/210) was one cause
  // of the crushed tone, so this is checked rather than assumed.
  double previous = -1.0;
  for (int g = 0; g < 256; ++g) {
    const double now = meanLevel(g);
    EXPECT_GE(now, previous - 1e-9) << "grey " << g << " is darker than grey " << (g - 1);
    previous = now;
  }
}
