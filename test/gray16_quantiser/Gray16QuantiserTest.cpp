#include <gtest/gtest.h>
#include <PixFormat.h>

#include <array>
#include <cstdint>
#include <set>

// GRAY16 (item 14/15) — the quantiser end of the path: luminance -> 16-level
// optical value -> canvas nibble. These are the functions BOTH surfaces use
// (the in-book image converters via DitherUtils::applyBayerDither16Level, and the
// sleep screen via Bitmap::readNextRow16), so a regression here is a regression
// on both. The panel end (nibble -> waveform table) is proven on the device by
// `[panel] show mode=... ms=...` and cannot be exercised here.
namespace {

// A perceptual ramp: every code 0..255, in a 64x4 patch so the 4x4 matrix is
// exercised across all of its cells for each value.
std::array<int, 16> histogramOf(const bool dither, const int base) {
  std::array<int, 16> hist{};
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 64; ++x) {
      const auto gray = static_cast<uint8_t>((x * 4 + base) & 0xFF);
      const uint8_t level = dither ? pixfmt::ditherLevel16(gray, x, y) : pixfmt::level16NoDither(gray);
      hist[level]++;
    }
  }
  return hist;
}

uint8_t roundTripNibble(const uint8_t level) { return pixfmt::imageLevel16ToNibble(level); }

}  // namespace

TEST(Gray16Quantiser, ProducesAllSixteenLevels) {
  std::set<uint8_t> seen;
  for (int g = 0; g < 256; ++g) seen.insert(pixfmt::ditherLevel16(static_cast<uint8_t>(g), 0, 0));
  // Ordered dither moves at most half a level, so 16 source levels is the floor.
  EXPECT_GE(seen.size(), 16u);
  EXPECT_EQ(*seen.begin(), 0);
  EXPECT_EQ(*seen.rbegin(), 15);
}

TEST(Gray16Quantiser, EndpointsAreExact) {
  // Pure ink and pure paper must never be dithered away from the endpoints, or a
  // black rule or a white page would gain a visible texture.
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      EXPECT_EQ(pixfmt::ditherLevel16(0, x, y), 0);
      EXPECT_EQ(pixfmt::ditherLevel16(255, x, y), 15);
    }
  }
}

TEST(Gray16Quantiser, FlatToneStaysWithinOneLevel) {
  // The whole argument for dithering 16 levels rather than 4: a flat mid grey must
  // not become a visible dot pattern. Every cell of a flat 128 must land on one of
  // the two adjacent levels, never further.
  std::set<uint8_t> levels;
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) levels.insert(pixfmt::ditherLevel16(128, x, y));
  ASSERT_FALSE(levels.empty());
  EXPECT_LE(*levels.rbegin() - *levels.begin(), 1)
      << "flat tone spread over more than one level: the dither amplitude is wrong";
}

TEST(Gray16Quantiser, RampIsMonotonicAndContiguous) {
  // A ramp drawn left-to-right must produce every level in order: a skipped level
  // is a contour line in a gradient, which is what GRAY16 exists to remove.
  int previous = -1;
  for (int g = 0; g < 256; g += 16) {
    const int level = pixfmt::level16NoDither(static_cast<uint8_t>(g));
    EXPECT_GE(level, previous);
    EXPECT_LE(level - previous, 1);
    previous = level;
  }
  // 240 is the last sample of the stride and rounds to 14 (240/255 * 15 = 14.1);
  // only paper itself reaches 15.
  EXPECT_EQ(previous, 14);
  EXPECT_EQ(pixfmt::level16NoDither(255), 15);
}

TEST(Gray16Quantiser, DitherDoesNotShiftTheMean) {
  // The 4x4 matrix is symmetric, so the average level over a patch must equal the
  // undithered quantisation of that patch's average luminance. If it drifts, a
  // photographic page is systematically lighter or darker than the source.
  const auto withDither = histogramOf(true, 0);
  int ditheredSum = 0, ditheredCount = 0;
  for (int i = 0; i < 16; ++i) {
    ditheredSum += i * withDither[i];
    ditheredCount += withDither[i];
  }
  const auto plain = histogramOf(false, 0);
  int plainSum = 0, plainCount = 0;
  for (int i = 0; i < 16; ++i) {
    plainSum += i * plain[i];
    plainCount += plain[i];
  }
  ASSERT_GT(ditheredCount, 0);
  ASSERT_GT(plainCount, 0);
  const double ditheredMean = static_cast<double>(ditheredSum) / ditheredCount;
  const double plainMean = static_cast<double>(plainSum) / plainCount;
  EXPECT_NEAR(ditheredMean, plainMean, 0.5) << "the 16-level dither shifts the mean tone";
}

TEST(Gray16Quantiser, LevelMapsToOneToOneNibble) {
  // The canvas encoder must be the identity on 0..15 — that is the contract the
  // panel's nibble (0x0 ink .. 0xF paper) and my_gl_25_0/my_gc_25_0 are built on.
  for (int level = 0; level < 16; ++level) {
    EXPECT_EQ(roundTripNibble(static_cast<uint8_t>(level)), level);
  }
  // Out of range clamps to paper rather than wrapping into the dark end.
  EXPECT_EQ(roundTripNibble(16), 15);
  EXPECT_EQ(roundTripNibble(255), 15);
}

TEST(Gray16Quantiser, RollbackDefineIsTheOnlySwitch) {
  // CP_GRAY16=0 must be expressible from the build system, and the constant that
  // every writer reads must follow it. A build with -DCP_GRAY16=0 is what the
  // operator's rollback rebuild produces.
  EXPECT_EQ(pixfmt::kGray16Images, CP_GRAY16 != 0);
}

TEST(Gray16Quantiser, PerSurfaceSwitchesFollowTheMaster) {
  // r101.2: the master switch still governs both surfaces by default, and each
  // surface has its own define so one can be dropped without rebuilding the other.
  // These are compile-time constants, so this test is a guard on the DEFAULT
  // wiring: with no -D overrides both surfaces must equal CP_GRAY16.
  EXPECT_EQ(pixfmt::kGray16Images, pixfmt::kGray16Sleep);
  EXPECT_EQ(pixfmt::kGray16Images, CP_GRAY16_IMAGES != 0);
  EXPECT_EQ(pixfmt::kGray16Sleep, CP_GRAY16_SLEEP != 0);
}
