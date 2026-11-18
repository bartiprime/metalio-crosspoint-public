#pragma once

#include <HalStorage.h>

class Print;

class GifToBmpConverter {
  static bool gifFileToBmpStreamInternal(HalFile& gifFile, Print& bmpOut, int targetWidth, int targetHeight,
                                         bool oneBit, bool crop = true, bool originalThresholds = false,
                                         bool coverGreys = false);

 public:
  static bool gifFileToBmpStream(HalFile& gifFile, Print& bmpOut, bool crop = true, bool originalThresholds = false);
  static bool gifFileTo1BitBmpStreamWithSize(HalFile& gifFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // r164 THUMB1: 2 bpp COVER thumbnail - same crop and downsample, quantised to four
  // evenly spaced optical levels with an ordered 4x4 matrix instead of Atkinson.
  static bool gifFileToCoverThumbBmpStreamWithSize(HalFile& gifFile, Print& bmpOut, int targetMaxWidth,
                                                   int targetMaxHeight);
};
