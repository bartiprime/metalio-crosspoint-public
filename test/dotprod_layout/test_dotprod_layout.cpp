// SIMD1 — the s8 dot-product descriptor, on the HOST.
//
// The point of this file is to make a wrong image2d_t fail here instead of on
// the glass: the r101.21 bench reported `ratio=33.84x` from two kernels that
// returned different values, i.e. at least one of them was reading a different
// set of bytes. So the descriptor walk is checked against the VENDORED ANSI
// kernel itself (dspi_dotprod_s8_ansi.c is compiled into this test), not against
// a second hand-written copy of the same guess.

#include "debug/DotprodBenchMath.h"

#include <gtest/gtest.h>

#include <utility>

extern "C" {
#include "dspi_dotprod.h"
}

namespace {

using namespace dotprodb;

image2d_t toImage(const ImageLayout& l, int8_t* data) {
  image2d_t img;
  img.data = data;
  img.step_x = l.stepX;
  img.step_y = l.stepY;
  img.stride_x = l.strideX;
  img.stride_y = l.strideY;
  img.size_x = l.sizeX;
  img.size_y = l.sizeY;
  return img;
}

// The bench's seed, reproduced verbatim (DotprodBench.cpp smallVal): even
// magnitudes, no zeros, deterministic.
void seedSmall(int8_t* a, int8_t* b, int count, uint32_t start = 0x12345678u) {
  static const int8_t kSet[8] = {-8, -6, -4, -2, 2, 4, 6, 8};
  uint32_t s = start;
  for (int i = 0; i < count; ++i) {
    s = s * 1664525u + 1013904223u;
    a[i] = kSet[(s >> 13) & 7u];
    s = s * 1664525u + 1013904223u;
    b[i] = kSet[(s >> 13) & 7u];
  }
}

constexpr int kShift = 7;

}  // namespace

// ---- descriptor semantics ---------------------------------------------------

TEST(DotprodLayout, DenseLayoutIsRowPitchInElements) {
  const ImageLayout l = denseLayout(64, 32);
  EXPECT_EQ(l.stepX, 1);   // sampling step in X
  EXPECT_EQ(l.stepY, 1);   // sampling step in Y ("usually is 1")
  EXPECT_EQ(l.strideX, 64);  // elements per row, NOT bytes
  EXPECT_EQ(l.strideY, 32);  // rows
  EXPECT_EQ(l.sizeX, 64);
  EXPECT_EQ(l.sizeY, 32);
}

TEST(DotprodLayout, RowAdvanceIsStrideXTimesStepY) {
  // The trap the r101.21 descriptor could have fallen into: the ANSI kernel advances
  // by stride_x * step_y per row (dspi_dotprod_s8_ansi.c: `i_step = stride_x * step_y`),
  // not by stride_y and not by stride_x alone. With step_y = 1 those coincide, which
  // is why a dense layout hides the difference -- assert both.
  const ImageLayout dense = denseLayout(64, 32);
  EXPECT_EQ(elementOffset(dense, 0, 1), 64);   // one row of 64 elements
  EXPECT_EQ(elementOffset(dense, 3, 2), 2 * 64 + 3);

  ImageLayout stepped;
  stepped.stepX = 1;
  stepped.stepY = 2;  // every other row
  stepped.strideX = 64;
  stepped.strideY = 64;
  stepped.sizeX = 64;
  stepped.sizeY = 32;
  EXPECT_EQ(elementOffset(stepped, 0, 1), 128);  // stride_x * step_y
}

TEST(DotprodLayout, RangeChecksUseStepTimesCount) {
  const ImageLayout l = denseLayout(64, 32);
  EXPECT_TRUE(layoutInRange(l, 64, 32));
  EXPECT_FALSE(layoutInRange(l, 65, 32));  // step_x * count_x > stride_x
  EXPECT_FALSE(layoutInRange(l, 64, 33));  // step_y * count_y > stride_y
  EXPECT_FALSE(layoutInRange(l, 0, 32));
  EXPECT_FALSE(layoutInRange(l, 64, 0));

  ImageLayout padded = denseLayout(64, 32);
  padded.strideX = 80;  // 16 elements of padding per row
  EXPECT_TRUE(layoutInRange(padded, 64, 32));
  EXPECT_FALSE(layoutInRange(padded, 81, 32));
}

// ---- arp4 selection ---------------------------------------------------------

TEST(DotprodLayout, Arp4FastPathMirrorsTheKernelGuard) {
  // dspi_dotprod_s8_arp4.S: `or` of both step_x, minus 1, OR'ed with count_x & 15,
  // must be zero before it runs its own body; otherwise it jumps to the ANSI twin.
  EXPECT_TRUE(arp4FastPath(1, 1, 64));
  EXPECT_TRUE(arp4FastPath(1, 1, 16));
  EXPECT_TRUE(arp4FastPath(1, 1, 512));
  EXPECT_FALSE(arp4FastPath(2, 1, 64));   // step_x must be 1
  EXPECT_FALSE(arp4FastPath(1, 2, 64));
  EXPECT_FALSE(arp4FastPath(1, 1, 63));   // count_x must be a multiple of 16
  EXPECT_FALSE(arp4FastPath(1, 1, 65));
  EXPECT_FALSE(arp4FastPath(1, 1, 0));
}

TEST(DotprodLayout, EveryBenchShapeIsEligible) {
  // The shapes the bench times. If one of these stops being eligible the bench
  // would time ANSI against ANSI and print a "1x SIMD" number; keep them honest.
  for (int countX : {64, 128, 256, 512, 16}) {
    EXPECT_TRUE(arp4FastPath(1, 1, countX)) << "countX=" << countX;
    EXPECT_TRUE(layoutInRange(denseLayout(countX, 8), countX, 8)) << "countX=" << countX;
  }
}

// ---- the reference walk, against esp-dsp's own ANSI kernel ------------------

TEST(DotprodLayout, ReferenceWalkMatchesVendoredAnsiOnDenseLayouts) {
  static int8_t a[64 * 32];
  static int8_t b[64 * 32];
  seedSmall(a, b, 64 * 32);

  for (const auto& shape : {std::pair<int, int>{64, 32}, {128, 16}, {16, 64}, {32, 8}}) {
    const int cx = shape.first;
    const int cy = shape.second;
    const ImageLayout l = denseLayout(cx, cy);
    seedSmall(a, b, cx * cy);
    image2d_t ia = toImage(l, a);
    image2d_t ib = toImage(l, b);

    int8_t out = 0;
    ASSERT_EQ(dspi_dotprod_s8_ansi(&ia, &ib, &out, cx, cy, kShift), ESP_OK);
    const int32_t exact = dotRef(l, a, l, b, cx, cy);
    ASSERT_TRUE(int8ResultFits(exact, kShift));
    EXPECT_EQ(static_cast<int>(out), shiftedValue(exact, kShift))
        << "shape " << cx << "x" << cy;
  }
}

TEST(DotprodLayout, ReferenceWalkMatchesVendoredAnsiOnPaddedAndSteppedLayouts) {
  // The library's own unit test uses step_x = step_y = 2 over an 8x8 block
  // (test_dspi_dotprod_s8_ansi.c), so a descriptor helper that ignores step_* or
  // stride_x would pass a dense-only test and fail here.
  static int8_t a[16 * 16];
  static int8_t b[16 * 16];
  seedSmall(a, b, 16 * 16);

  ImageLayout l;
  l.stepX = 2;
  l.stepY = 2;
  l.strideX = 16;
  l.strideY = 16;
  l.sizeX = 16;
  l.sizeY = 16;

  image2d_t ia = toImage(l, a);
  image2d_t ib = toImage(l, b);
  int8_t out = 0;
  ASSERT_EQ(dspi_dotprod_s8_ansi(&ia, &ib, &out, 4, 4, kShift), ESP_OK);
  EXPECT_EQ(static_cast<int>(out), shiftedValue(dotRef(l, a, l, b, 4, 4), kShift));
}

TEST(DotprodLayout, WrongDescriptorIsDetectableOnTheHost) {
  // Negative control. If someone "fixes" the descriptor by using stride_y as the
  // row advance, the reference walk must disagree with a plain contiguous dot, so
  // this test fails loudly rather than silently blessing the change.
  constexpr int W = 8;
  constexpr int H = 4;
  static int8_t a[W * H];
  static int8_t b[W * H];
  seedSmall(a, b, W * H);

  const ImageLayout dense = denseLayout(W, H);
  const int32_t good = dotRef(dense, a, dense, b, W, H);

  // Same bytes, described as step_y = 2: the walk now skips every other row, so
  // it must sum a different set of products.
  ImageLayout wrong = dense;
  wrong.stepY = 2;
  wrong.strideY = 8;
  const int32_t bad = dotRef(wrong, a, wrong, b, W, H / 2);
  EXPECT_NE(good, bad) << "a descriptor bug would have been invisible";
}

// ---- the int8 output gate ---------------------------------------------------

TEST(DotprodLayout, ShiftedValueMatchesTheLibraryRounding) {
  EXPECT_EQ(shiftedValue(0, kShift), 0);
  EXPECT_EQ(shiftedValue(64, kShift), 1);    // +1<<6 then >>7
  EXPECT_EQ(shiftedValue(-64, kShift), 0);   // arithmetic shift, rounded up toward zero
  EXPECT_EQ(shiftedValue(127 * 128, kShift), 127);
  EXPECT_EQ(shiftedValue(-128 * 128, kShift), -128);
}

TEST(DotprodLayout, Int8FitsGateRejectsOutOfRangeSums) {
  constexpr int32_t kmax = 127 * 128;   // 16256
  constexpr int32_t kmin = -128 * 128;  // -16384
  EXPECT_TRUE(int8ResultFits(kmax, kShift));
  EXPECT_TRUE(int8ResultFits(kmin, kShift));
  EXPECT_FALSE(int8ResultFits(kmax + 128, kShift));
  EXPECT_FALSE(int8ResultFits(kmin - 128, kShift));
}

TEST(DotprodLayout, BenchShapesKeepTheirResultInsideInt8) {
  // The bench's seed is deliberately small so that the equality test compares the
  // dot product and not the truncation of an int8 store: 2048 MACs of magnitudes
  // <= 8 accumulate to |sum| <= 16384, which is the int8 window at shift 7.
  static int8_t a[64 * 32];
  static int8_t b[64 * 32];
  seedSmall(a, b, 64 * 32);
  for (const auto& shape : {std::pair<int, int>{64, 32}, {128, 16}, {256, 8}, {512, 4}, {16, 64}}) {
    const int cx = shape.first;
    const int cy = shape.second;
    const ImageLayout l = denseLayout(cx, cy);
    seedSmall(a, b, cx * cy);
    EXPECT_TRUE(int8ResultFits(dotRef(l, a, l, b, cx, cy), kShift))
        << "shape " << cx << "x" << cy;
  }
}
