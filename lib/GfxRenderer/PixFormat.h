#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// CP_RENDER_4BPP — the canvas pixel format, and the only switch in it.
//
// 0 (the old behaviour): one bit per pixel, 152 B/row, 103,968 B for 1216x684.
// 1 (shipping):           four bits per pixel, 608 B/row, 415,872 B. Nibble 0x0
//                         is solid ink, 0xF is paper white, and 0x1..0xE are
//                         real optical levels the panel can drive.
//
// Everything below is expressed so ONE define flips the whole renderer: the
// stride (kPxPerByte), the glyph blitter's addressing (GlyphBitmap.h), the
// framebuffer region helpers, and the panel transfer. The 1 bpp path is kept
// compiling on purpose — an A/B of this round is a rebuild, not a patch stack —
// and the host tests in test/glyph_bitmap exercise it.
//
// Format details that are NOT free to change:
//   * byte = x >> 1, and the EVEN x is the LOW nibble. That is exactly what
//     metalio_vendor_panel.cpp's put4() writes and what the vendor's own 4 bpp
//     framebuffer (epdiy front_fb) holds, so the panel transfer in 4 bpp mode is
//     a row copy rather than a per-pixel re-pack.
//   * polarity matches the 1 bpp buffer: ink is 0, white is 0xF. invertScreen()
//     and the SDK's invertBytes() are byte-wide NOTs, which map 0x0<->0xF,
//     0x5<->0xA, so dark mode stays symmetric with no extra code.
// ---------------------------------------------------------------------------
#ifndef CP_RENDER_4BPP
#define CP_RENDER_4BPP 1
#endif

namespace pixfmt {

#if CP_RENDER_4BPP
constexpr bool kFourBit = true;
constexpr int kBitsPerPixel = 4;
constexpr int kPxPerByte = 2;  // pixels per framebuffer byte (byte = x / 2)
#else
constexpr bool kFourBit = false;
constexpr int kBitsPerPixel = 1;
constexpr int kPxPerByte = 8;
#endif

// Nibble values. Named for what they are, not for a "colour": the panel drives
// these as target levels in the (from,to) LUT.
constexpr uint8_t kInk = 0x0;
constexpr uint8_t kPaper = 0xF;

// Glyph AA coverage levels. These are OPTICAL levels, and the ONLY source that
// may write them: a glyph stem's partial coverage is a genuine intermediate
// grey. Geometry (rules, borders, fills) and images are HALFTONES and stay at
// kInk/kPaper -- a 2x2 dither pattern and a bitmap's 2-bit value already carry
// their grey spatially, so promoting them to a level both looks washed out and
// forces the frame onto the grey waveform.
constexpr uint8_t kLightGray = 0xA;  // ~25% ink (glyph coverage 1)
constexpr uint8_t kDarkGray = 0x5;   // ~67% ink (glyph coverage 2)

// The single gamma shared by every 4-level source: 2-bit glyph levels are
// 0=white .. 3=black (GlyphBitmap.h's contract, the .cpfont converter and the
// TtfEpdFont 25/50/75% quantiser), and 4-level image pixels use the same 0..3
// axis (0=darkest in the image converters, hence the reversal there, not here).
// Keep this table the ONE place a coverage level becomes an optical level:
// a second copy is how the two font paths drift apart.
constexpr uint8_t glyphLevelToNibble(uint8_t level) {
  // 0 white, 1 light, 2 dark, 3 black. Values are spaced 5 apart so the
  // waveform's phase count, not nibble arithmetic, decides the actual grey.
  return level >= 3 ? kInk : level == 2 ? kDarkGray : level == 1 ? kLightGray : 0x0F;
}

// A uniform byte for a horizontal run at an arbitrary x parity: nib*0x11.
constexpr uint8_t nibbleFillByte(uint8_t nib) { return static_cast<uint8_t>(nib | (nib << 4)); }

// 4-level IMAGE pixels are addressed the other way round (0 = darkest, 3 =
// whitest) by Bitmap::readNextRow and by the image converters. Same spacing,
// reversed axis.
//
// DO NOT USE THIS ON THE BW PATH. An image level is a halftone coordinate, not
// an optical level: writing it through paints 0x5/0xA into a dithered picture
// and turns it into a grey wash (the r100 sleep-screen regression). Every BW
// consumer thresholds (< 2 or < 3, per its own tuned cut) and writes ink. Kept
// for a future 16-level image path, which is the queued GRAY16 work.
constexpr uint8_t imageLevelToNibble(uint8_t level) {
  return level >= 3 ? 0x0F : level == 2 ? kLightGray : level == 1 ? kDarkGray : kInk;
}

// ---------------------------------------------------------------------------
// CP_GRAY16 (default 1) — THE GRAY16 ROUTE, and its ONE-DEFINE ROLLBACK.
//
// On:  image pages and the sleep screen may carry SIXTEEN optical levels. The
//      panel side of that is not new — the 16-level tables have always been
//      registered (`my_gl_25_0`, 48 phases, type 5; `my_gc_25_0`, type 2), and
//      the sleep screen already drives GC16-from-white. What was missing was a
//      SOURCE: every image writer thresholded to ink/paper, so a 16-level
//      waveform was fed 2-level data.
// Off: `-DCP_GRAY16=0` (or flip this define). Every image writer falls back to
//      the byte-identical pre-GRAY16 behaviour: 2-bit dither -> ink/paper in the
//      canvas, 2 bpp `.pxc` cache, image pages on the page-turn mode, the sleep
//      cover BMP regenerated 2-bit at its old path. The AA text path is NOT
//      behind this define and is not affected either way.
//
// The nibble convention for a 16-level IMAGE pixel is the same axis as
// imageLevelToNibble: level 0 = darkest, 15 = paper white, and it maps 1:1 onto
// the vendor framebuffer's nibble (0x0 ink .. 0xF paper).
#ifndef CP_GRAY16
#define CP_GRAY16 1
#endif

// PER-SURFACE ROLLBACK (r101.2). CP_GRAY16 is the master switch; these two
// decide which SURFACE keeps 16 levels, so the operator can drop one without a
// rebuild of the other:
//   CP_GRAY16_IMAGES (default = CP_GRAY16) — image pages inside a book.
//   CP_GRAY16_SLEEP  (default = CP_GRAY16) — the sleep screen's cover/bitmap.
// `-DCP_GRAY16_IMAGES=0` leaves the sleep screen at 16 levels, and vice versa;
// `-DCP_GRAY16=0` still disables both (they default to the master).
#ifndef CP_GRAY16_IMAGES
#define CP_GRAY16_IMAGES CP_GRAY16
#endif
#ifndef CP_GRAY16_SLEEP
#define CP_GRAY16_SLEEP CP_GRAY16
#endif

constexpr bool kGray16Images = CP_GRAY16_IMAGES != 0;
constexpr bool kGray16Sleep = CP_GRAY16_SLEEP != 0;

// r164 THUMB1 - the third surface: Home's cover thumbnails. A separate switch because
// it is a separate decision: turning covers back into 1-bit ink
// (`-DCP_GRAY16_COVERS=0`) must not drag the sleep screen or the image pages with it.
// With it off a 2 bpp thumb still DRAWS (GfxRenderer's 2-bit `val < 3` collapse), so
// the rollback degrades the look, never the tile.
#ifndef CP_GRAY16_COVERS
#define CP_GRAY16_COVERS CP_GRAY16
#endif
constexpr bool kGray16Covers = CP_GRAY16_COVERS != 0;

inline uint8_t imageLevel16ToNibble(uint8_t level) { return level > 0x0F ? 0x0F : level; }

// ---------------------------------------------------------------------------
// THE 16-LEVEL QUANTISER — one implementation, two callers.
//
// `lib/GfxRenderer/Bitmap` (the sleep-screen BMP reader) and
// `lib/Epub/.../DitherUtils.h` (the in-book image converters) must quantise to
// the same 16 levels with the same matrix, or a cover and the same picture
// inside a book would render with visibly different texture. The matrix and the
// arithmetic therefore live HERE, in the pixel-format layer, and DitherUtils
// delegates to them.
//
// DITHER DECISION (states the operator asked for): 16-level content IS
// dithered, with a 4x4 ordered matrix whose amplitude spans exactly ONE
// quantisation step (+/-8 of 255, i.e. +/-3 % of the range). It is not
// dither-free true tone. The reason is a measurement on the arithmetic, not a
// preference: 16 levels over 0..255 is a step of 17 codes, and any smooth ramp
// (a sky, a wall, a vignette) that crosses a step boundary in a flat field
// turns that boundary into a visible contour line — the "posterised" look. One
// step of texture removes the contour while staying below the level granularity
// the panel can resolve, so the result reads as tone, not as a dot pattern.
// That is the difference from the 2-level path this replaces, where the matrix
// had to carry the WHOLE luminance (a 50 % grey was half ink, half paper).
// CP_GRAY16=0 removes the dither with the rest of the feature.
constexpr uint8_t kBayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// One 8-bit luminance -> one 16-level optical value (0 ink .. 15 paper).
inline uint8_t ditherLevel16(uint8_t gray, int x, int y) {
  int t = (static_cast<int>(gray) * 240) / 255;  // 0..240, sixteenths of a level
  t += static_cast<int>(kBayer4x4[y & 3][x & 3]) - 8;  // +/- half a level
  if (t < 0) t = 0;
  int level = (t + 8) / 16;  // round to nearest
  if (level > 15) level = 15;
  return static_cast<uint8_t>(level);
}

// Luminance -> 16-level value with NO dithering (a straight nearest-level map).
// Used when the caller explicitly asked for no dithering.
inline uint8_t level16NoDither(uint8_t gray) {
  return static_cast<uint8_t>((static_cast<int>(gray) * 15 + 127) / 255);
}

// ---------------------------------------------------------------------------
// COVER THUMBNAILS (r164 THUMB1) - the 4-level, 2 bpp quantiser and its palette.
//
// WHY 4 LEVELS AND NOT A 1-BIT DITHER. The 1 bpp thumb the device shipped carries its
// grey SPATIALLY: every dot is ink or paper, so a flat field of grey 39 renders as 46 %
// white dots on black (measured mean 9.9, sd 49.3 - R164_THUMB1_INVESTIGATION.md 3.3).
// At 2 bpp each dot may be one of four OPTICAL levels, so the same field is reproduced
// with a dot amplitude of one step instead of the whole range. But a 4-level file is
// only better than a 1-bit one if its quantiser does not do what Atkinson did: DIFFUSE
// ERROR. Atkinson propagates only 6/8 of the error, which compresses the tone response
// at both ends - grey <= 37 never produced a white pixel, grey >= 224 never a black one
// (3.2). Ordered dithering has no state to leak: each pixel is decided from its own
// luminance plus a fixed matrix cell, so the AVERAGE over a matrix period is the input
// tone by construction, and the extremes cannot collapse because the matrix only ever
// moves the threshold by half a step.
//
// LEVEL SPACING IS LINEAR, DELIBERATELY. The panel's 16 levels are linear in
// reflectance and a dithered patch averages them arithmetically, so evenly spaced
// levels are the only choice under which the average is the tone. A perceptual ramp
// would put a non-linear set of numbers into a linear averaging operation and shift
// every midtone.
//
// The four levels are taken from the 16-level axis at 0, 5, 10 and 15, and the palette
// luminance is level*17, which is EXACTLY what Bitmap::readNextRow16() inverts for a
// 2 bpp file:
//
//     t = lum * 240 / 255; t += bayer - 8; level = (t + 8) / 16
//
// gives 80 -> 5, 160 -> 10, 240 -> 15 for every cell of the 4x4 matrix (a +-8 offset
// cannot push 80/160/240 across a 16-wide boundary), and 0 -> 0. So index i lands on
// optical level 5*i on the draw, with no second dither: the file's index IS the level,
// and the only dither in the path is the one below, applied once at generation time.
// test/thumb_greys pins that.
constexpr uint8_t kCoverGrayLevel[4] = {0, 5, 10, 15};   // 16-level axis
constexpr uint8_t kCoverGrayLum[4] = {0, 85, 170, 255};  // == level * 17

// One 8-bit luminance -> one 2 bpp palette index (0..3), ordered-dithered.
// Mirrors ditherLevel16's structure: the luminance is scaled into the same 0..240
// sixteenths space, the 4x4 matrix moves the decision point by half of a FOUR-level
// step (= 40 units) instead of half a sixteenth (= 8), and the result rounds to the
// nearest of 0 / 80 / 160 / 240.
//
// Endpoints are exact: the matrix can only move t by -40..+35, so grey 0 can only
// reach t <= 35 (index 0) and grey 255 only t >= 200 (index 3). Nothing is clipped at
// either end, which is the failure mode this replaces.
inline uint8_t coverDitherIndex(uint8_t gray, int x, int y) {
  int t = (static_cast<int>(gray) * 240) / 255;              // 0..240
  t += (static_cast<int>(kBayer4x4[y & 3][x & 3]) - 8) * 5;  // +- half a 4-level step
  if (t < 0) t = 0;
  if (t > 240) t = 240;
  return static_cast<uint8_t>((t + 40) / 80);  // 0..3, saturates by construction
}

// The level a 2 bpp index draws as. Kept here so the generator, the palette writer and
// the tests cannot disagree about it.
constexpr uint8_t coverIndexToLevel(uint8_t index) { return index > 3 ? kCoverGrayLevel[3] : kCoverGrayLevel[index]; }

}  // namespace pixfmt
