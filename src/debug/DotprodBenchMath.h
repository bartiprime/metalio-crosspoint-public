#pragma once

// SIMD1 — the pure half of the s8 dot-product bench: how an image2d_t is filled
// in, what the library's own walk does with it, when the arp4 kernel is actually
// selected, and whether the int8 output can hold the result.
//
// This header deliberately pulls in NOTHING from esp-dsp: the same code has to
// run on the host (`test/dotprod_layout`) so a wrong descriptor fails in a unit
// test rather than on the glass. DotprodBench.cpp is the only place that turns
// these fields into a real image2d_t.
//
// SEMANTICS, taken from the vendored tree (not guessed):
//   idf/managed_components/espressif__esp-dsp/modules/common/include/dsp_types.h
//     step_x   step of elements by X
//     step_y   step of elements by Y, usually 1
//     stride_x "stride width: size of the elements in X axis * by step_x + padding"
//     stride_y "stride height: size of the elements in Y axis * by step_y + padding"
//   idf/.../modules/dotprod/fixed/dspi_dotprod_s8_ansi.c
//     row advance      i_step = stride_x * step_y      (NOT stride_y, and NOT
//                                                       stride_x alone)
//     element advance  step_x
//     rejects when     step_x * count_x > stride_x  or  step_y * count_y > stride_y
// So the sample at (x, y) lives at data[stride_x * step_y * y + step_x * x], and
// stride_x is counted in ELEMENTS PER ROW, not in bytes and not strided by
// step_x twice.
//
// Since stride_x is the row pitch in elements, a dense row-major W x H int8
// image is step_x=1, step_y=1, stride_x=W, stride_y=H, size_x=W, size_y=H.

#include <cstddef>
#include <cstdint>

namespace dotprodb {

struct ImageLayout {
  int stepX = 1;
  int stepY = 1;
  int strideX = 0;
  int strideY = 0;
  int sizeX = 0;
  int sizeY = 0;
};

// Dense, unpadded, row-major width x height buffer.
inline ImageLayout denseLayout(int width, int height) {
  ImageLayout l;
  l.stepX = 1;
  l.stepY = 1;
  l.strideX = width;  // elements per row
  l.strideY = height; // rows
  l.sizeX = width;
  l.sizeY = height;
  return l;
}

// Element (not byte) offset of the sample at (x, y), by the library's own walk.
inline long elementOffset(const ImageLayout& l, int x, int y) {
  return static_cast<long>(l.strideX) * l.stepY * y + static_cast<long>(l.stepX) * x;
}

// What dspi_dotprod_s8_ansi() will accept. count_* are SAMPLE counts, and both
// images are validated independently, so callers must check both.
inline bool layoutInRange(const ImageLayout& l, int countX, int countY) {
  if (countX <= 0 || countY <= 0) return false;
  if (l.stepX <= 0 || l.stepY <= 0 || l.strideX <= 0 || l.strideY <= 0) return false;
  return l.stepX * countX <= l.strideX && l.stepY * countY <= l.strideY;
}

// The arp4 kernel's own pre-flight (dspi_dotprod_s8_arp4.S: `or` of both step_x,
// minus 1, OR'ed with count_x & 15, must be zero). Anything else silently
// delegates to the ANSI twin -- measuring that call and calling it "SIMD speed"
// would be a fabricated ratio, so the bench only times shapes that pass here.
inline bool arp4FastPath(int stepXIn, int stepXFilter, int countX) {
  // count_x <= 0 would pass the asm guard (0 & 15 == 0) and take the vector
  // path with nothing to do; reject it here so the bench never times it.
  if (countX <= 0) return false;
  return (stepXIn | stepXFilter) == 1 && (countX & 15) == 0;
}

// Exact int32 dot product over the two descriptors, walking exactly like the
// kernel: count_x samples per row at step_x, rows advancing by stride_x*step_y.
inline int32_t dotRef(const ImageLayout& a, const int8_t* ad, const ImageLayout& b,
                      const int8_t* bd, int countX, int countY) {
  int32_t acc = 0;
  for (int y = 0; y < countY; ++y) {
    const long row = static_cast<long>(a.strideX) * a.stepY * y;
    const long frow = static_cast<long>(b.strideX) * b.stepY * y;
    for (int x = 0; x < countX; ++x) {
      acc += static_cast<int32_t>(ad[row + static_cast<long>(a.stepX) * x]) *
             static_cast<int32_t>(bd[frow + static_cast<long>(b.stepX) * x]);
    }
  }
  return acc;
}

// dspi_dotprod_s8 adds 1 << (shift-1), then arithmetic-shifts right by shift and
// stores the int8. If the shifted value does not fit in [-128, 127] the stored
// byte is a truncation artefact, and comparing two implementations on it tests
// the truncation, not the dot product. The library's documented default for
// int8 is shift=7.
inline int32_t shiftedValue(int32_t exactSum, int shift) {
  return (exactSum + (1 << (shift - 1))) >> shift;
}

inline bool int8ResultFits(int32_t exactSum, int shift) {
  const int32_t v = shiftedValue(exactSum, shift);
  return v >= -128 && v <= 127;
}

}  // namespace dotprodb
