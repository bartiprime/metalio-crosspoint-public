#pragma once

#include <HalStorage.h>

#include "JpegHwPath.h"

class Print;
class ZipFile;

// Defined in JpegHwDecode.h (which needs the ESP driver headers); only the type is
// needed to declare jpegCompareHwVsSw here.
enum class JpegHwStatus : uint8_t;

class JpegToBmpConverter {
  // JPEG1: everything a call needs, in one struct. A zero-initialised spec is the
  // old default behaviour; the debug fields are only ever used by the on-device
  // comparison (`jpeg compare`) and by the first-use gate.
  struct Spec {
    int targetWidth = 0;
    int targetHeight = 0;
    bool oneBit = false;
    bool crop = true;
    bool originalThresholds = false;
    bool eightBit = false;
    bool coverGreys = false;
    // Diagnostics: force the software decoder, and/or grade the software luma
    // against a hardware raster row by row (see consumeSourceRow).
    bool forceSoftware = false;
    const uint8_t* expectLuma = nullptr;
    uint32_t expectStride = 0;  // pixels
    int expectBpp = 1;
    JpegLumaDiff* lumaDiff = nullptr;
  };

  static bool jpegFileToBmpStreamInternal(HalFile& jpegFile, Print& bmpOut, Spec spec);

 public:
  // `eightBit` writes an 8-bit greyscale BMP (256-entry palette, raw luminance,
  // undithered) instead of the default 2-bit dithered image. GRAY16 uses it for
  // the sleep cover, where the 16-level quantisation happens at DRAW time
  // (Bitmap::readNextRow16) and therefore needs the tone still on disk.
  static bool jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop = true, bool originalThresholds = false,
                                  bool eightBit = false);
  // Convert with custom target size (thumbnails, and the custom sleep images, which
  // want the renderer's own canvas size rather than the portrait cover sizing).
  // `eightBit`: 8-bit greyscale output, same reason as above -- the sleep screen
  // draws 16 levels from the file's luminance. `crop`: cover (true) vs fit (false).
  static bool jpegFileToBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                          bool eightBit = false, bool crop = true);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                              int targetMaxHeight);
  // r164 THUMB1: convert to a 2 bpp COVER thumbnail - same box downsample and same crop
  // as the 1-bit call above, but quantised to four evenly spaced optical levels with an
  // ordered 4x4 matrix (PixFormat::coverDitherIndex) instead of Atkinson error
  // diffusion. Drawn on Home's grey plane the file's index IS the optical level, so a
  // flat field reproduces its own grey instead of collapsing to black.
  static bool jpegFileToCoverThumbBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                    int targetMaxHeight);

  // JPEG1 on-device comparison. Decodes the file with the hardware codec AND with
  // JPEGDEC, and compares the two decoders' Y planes pixel by pixel (not the BMP
  // bytes: both paths share the scaler and the ditherer, so a BMP difference would
  // describe thresholds, not decoders).
  //   returns false  -> the file is not hardware-eligible; `why` names the reason
  //                     and nothing was decoded twice.
  //   returns true   -> `diff` holds the per-pixel statistics, frameWidth/frameHeight
  //                     the compared grid, hwStatus the driver's own result.
  static bool jpegCompareHwVsSw(HalFile& jpegFile, JpegLumaDiff* diff, JpegProbe* probeOut, JpegHwReason* why,
                                int* frameWidth, int* frameHeight, JpegHwStatus* hwStatus);
};
