#pragma once

#include <GfxRenderer.h>
#include <PixFormat.h>
#include <stdint.h>

// CP_IMG_DITHER (default 1): quantize EPUB image luminance for the target the
// framebuffer can actually represent.
//
// Background (r92 / IMG2): the converters always produced a 4-LEVEL dithered
// value (0..3), but on a panel with no grayscale capability the only writer
// that ever runs is DirectPixelWriter's BW branch, which reduced that value
// with `pixelValue < 3` -- a 3/4 threshold. A flat 50% grey therefore came out
// 100% black and a photographic cover lost every midtone. 1-bit targets now get
// a true 1-bit ordered dither (0 or 3); 4-level values are kept for panels that
// really do upload gray planes.
#ifndef CP_IMG_DITHER
#define CP_IMG_DITHER 1
#endif

// 4x4 Bayer matrix for ordered dithering
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// The SAME 4x4 matrix in 8-bit alpha units: bayer4x4 scaled by 16, so the cells
// span 0..240 instead of 0..15 (15 * 16 = 240). `((y & 3) << 2) | (x & 3)` is
// row-major, i.e. exactly bayer4x4[y & 3][x & 3] -- verified entry by entry, so
// this is one table at two scales, not two tables. Used by the alpha cut-outs
// (sleep overlay BMP, PNG with transparency); an alpha is 0..255, and cutting a
// 4-level dither table at 0..15 would erase every semi-transparent pixel.
inline uint8_t bayer4x4Threshold(const int x, const int y) {
  return static_cast<uint8_t>(bayer4x4[y & 3][x & 3] << 4);
}

// 8x8 Bayer matrix (0..63) for a true 1-bit ordered dither: 64 evenly spaced
// thresholds across 0..255 is what a photograph needs. The 4x4 matrix only
// spans 16 steps and is meant for the 4-level (2 bpp) case.
inline const uint8_t bayer8x8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21},
};

// Apply Bayer dithering and quantize to 4 levels (0-3)
// Stateless - works correctly with any pixel processing order
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y) {
  int bayer = bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 5;  // Scale to +/-40 (half of quantization step 85)

  int adjusted = gray + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  if (adjusted < 64) return 0;
  if (adjusted < 128) return 1;
  if (adjusted < 192) return 2;
  return 3;
}

// 16-level ordered dither (GRAY16). The arithmetic and the matrix live in
// PixFormat.h (pixfmt::ditherLevel16) because the sleep-screen BMP reader needs
// the SAME quantiser; this is the name the image converters use.
//
// Why dither into 16 and not "true greys, no dither": 16 levels over 0..255 is a
// step of 17 codes, and a photographic gradient that spans a step in a flat field
// (sky, wall, vignette) turns into a visible contour line. Ordered dither across
// one step removes the contour for a texture amplitude of one level, i.e. ~6% of
// the range, invisible at reading distance on this panel's contrast.
inline uint8_t applyBayerDither16Level(uint8_t gray, int x, int y) { return pixfmt::ditherLevel16(gray, x, y); }

// 1-bit ordered dither. Returns 0 (black) or 3 (white) -- the same 2-bit
// convention a 1-bit BMP expands to, so DirectPixelWriter's BW branch, the gray
// planes and the on-disk pixel cache all read it correctly.
//
// Threshold = (bayer + 0.5) * 255 / 64, i.e. 4*bayer + 2 spanning 2..254: a
// matrix CENTRED on the range, which is why a flat 50% grey yields ~50% white.
// A matrix that is not centred, or a cut applied to an already-quantized
// value, biases the whole image -- the class of bug this replaces.
inline uint8_t applyBayerDither1Bit(uint8_t gray, int x, int y) {
  const int threshold = (static_cast<int>(bayer8x8[y & 7][x & 7]) << 2) + 2;
  return gray < threshold ? 0 : 3;
}

// Single entry point for the EPUB image converters: pick the quantization the
// render target can represent. `levels` is the number of OPTICAL levels the drive
// can put on the glass and the canvas can hold:
//   1  — 1 bpp canvas / no grayscale capability: a true 1-bit ordered dither.
//   4  — the 4 bpp canvas's 2-bit image levels (pre-GRAY16 behaviour).
//   16 — GRAY16: a 4 bpp canvas driven by a 16-level waveform table.
// `oneBitTarget` used to be a bool here; it is spelled 1 so no call site can
// accidentally ask for a 1-bit dither by passing `false`.
inline uint8_t quantizeImagePixel(uint8_t gray, int x, int y, bool useDithering, uint8_t levels) {
#if CP_IMG_DITHER
  if (levels <= 1 && useDithering) return applyBayerDither1Bit(gray, x, y);
  if (levels >= 16) return useDithering ? applyBayerDither16Level(gray, x, y) : pixfmt::level16NoDither(gray);
#endif
  if (useDithering) return applyBayerDither4Level(gray, x, y);
  const uint8_t v = gray / 85;
  return v > 3 ? 3 : v;
}

// THE ROUTING SIGNAL, in one place, for every image decode: how many optical
// levels may a pixel of this decode carry?
//
//   * 1 bpp canvas, or a panel with neither overlay nor absolute gray planes:
//     ONE level (a true 1-bit dither). Unchanged from before GRAY16.
//   * 1 bpp canvas with gray planes: FOUR (the old 2-bit path).
//   * 4 bpp canvas: FOUR — the pre-GRAY16 behaviour — unless the caller asked
//     for sixteen AND CP_GRAY16 is on, in which case SIXTEEN.
//
// `wantSixteen` is the CALLER's request (an image page, the sleep screen), never
// a guess made here: this function only answers whether the hardware and the
// build can honour it. A refused request degrades to 4 (4 bpp canvas) or 1
// (1 bpp canvas), which is exactly the pre-GRAY16 look — never to a different
// threshold than the one that path used.
inline uint8_t imageTargetLevels(GfxRenderer& renderer, const bool wantSixteen) {
  const bool grayPlanes =
      renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Overlay).supported() ||
      renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
  if (!GfxRenderer::fourBitCanvas()) return grayPlanes ? 4 : 1;
  return (wantSixteen && pixfmt::kGray16Images) ? 16 : 4;
}
