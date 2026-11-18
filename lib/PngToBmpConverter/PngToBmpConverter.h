#pragma once

#include <HalStorage.h>

class Print;

class PngToBmpConverter {
  static bool pngFileToBmpStreamInternal(HalFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight,
                                         bool oneBit, bool crop = true, bool originalThresholds = false,
                                         bool eightBit = false, int lineArtThreshold = 0,
                                         bool coverGreys = false);

 public:
  // `eightBit`: 8-bit greyscale BMP (raw luminance, undithered) instead of the
  // 2-bit dithered image. Used by GRAY16 for the sleep cover.
  static bool pngFileToBmpStream(HalFile& pngFile, Print& bmpOut, bool crop = true, bool originalThresholds = false,
                                 bool eightBit = false);
  // `eightBit`/`crop` mirror the JPEG overload: 8-bit greyscale output for the
  // 16-level sleep screen, cover (true) vs fit (false).
  static bool pngFileToBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                         bool eightBit = false, bool crop = true);
  // `threshold1bit`: 0 keeps the reader's Atkinson 1-bit dither (the only behaviour
  // before r111 B12).  A non-zero value is a HARD CUT at that grey (BitmapHelpers'
  // threshold1bit): the line-art rule, where each pixel is decided alone and a 1-px
  // stroke cannot be fragmented by diffused error.  `crop` selects cover (true) or
  // fit-with-letterbox (false); a comic needs fit, since a cover-crop of a wide page
  // discards its edges.
  static bool pngFileTo1BitBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth,
                                             int targetMaxHeight, int lineArtThreshold = 0,
                                             bool crop = true);
  // r164 THUMB1: 2 bpp COVER thumbnail - the same crop and downsample as the 1-bit call,
  // quantised to four evenly spaced optical levels with an ordered 4x4 matrix
  // (PixFormat::coverDitherIndex) instead of Atkinson error diffusion.
  static bool pngFileToCoverThumbBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth,
                                                   int targetMaxHeight);
};
