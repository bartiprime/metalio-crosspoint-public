#include "JpegToBmpConverter.h"

#include <HalDisplay.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Logging.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>

#include "BitmapHelpers.h"
#include "JpegHwDecode.h"
#include "JpegHwPath.h"
#include "PixFormat.h"

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
// 8-bit output is now a PER-CALL choice (the `eightBit` argument, used by the
// GRAY16 sleep cover) rather than a global build switch; this constant is kept
// only because the tuning comments below refer to the old A/B.
// Dithering method selection (only one should be true, or all false for simple quantization):
constexpr bool USE_ATKINSON = true;          // Atkinson dithering (cleaner than F-S, less error diffusion)
constexpr bool USE_FLOYD_STEINBERG = false;  // Floyd-Steinberg error diffusion (can cause "worm" artifacts)
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering (good for downsampling)
// Pre-resize to target display size (CRITICAL: avoids dithering artifacts from post-downsampling)
constexpr bool USE_PRESCALE = true;  // true: scale image to target size before dithering
// ============================================================================

inline void write16(Print& out, const uint16_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

inline void write32(Print& out, const uint32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

inline void write32Signed(Print& out, const int32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

// Helper function: Write BMP header with 8-bit grayscale (256 levels)
void writeBmpHeader8bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 3) / 4 * 4;  // 8 bits per pixel, padded
  const int imageSize = bytesPerRow * height;
  const uint32_t paletteSize = 256 * 4;  // 256 colors * 4 bytes (BGRA)
  const uint32_t fileSize = 14 + 40 + paletteSize + imageSize;

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);                      // Reserved
  write32(bmpOut, 14 + 40 + paletteSize);  // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 8);              // Bits per pixel (8 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 256);   // colorsUsed
  write32(bmpOut, 256);   // colorsImportant

  // Color Palette (256 grayscale entries x 4 bytes = 1024 bytes)
  for (int i = 0; i < 256; i++) {
    bmpOut.write(static_cast<uint8_t>(i));  // Blue
    bmpOut.write(static_cast<uint8_t>(i));  // Green
    bmpOut.write(static_cast<uint8_t>(i));  // Red
    bmpOut.write(static_cast<uint8_t>(0));  // Reserved
  }
}

// Helper function: Write BMP header with 1-bit color depth (black and white)
static void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 31) / 32 * 4;  // 1 bit per pixel, round up to 4-byte boundary
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;  // 14 (file header) + 40 (DIB header) + 8 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 62);        // Offset to pixel data (14 + 40 + 8)

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 1);              // Bits per pixel (1 bit)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 2);     // colorsUsed
  write32(bmpOut, 2);     // colorsImportant

  // Color Palette (2 colors x 4 bytes = 8 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  // Note: In 1-bit BMP, palette index 0 = black, 1 = white
  uint8_t palette[8] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0xFF, 0xFF, 0xFF, 0x00   // Color 1: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

// Helper function: Write BMP header with 2-bit color depth
static void writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;  // 2 bits per pixel, round up
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;  // 14 (file header) + 40 (DIB header) + 16 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 70);        // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 2);              // Bits per pixel (2 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 4);     // colorsUsed
  write32(bmpOut, 4);     // colorsImportant

  // Color Palette (4 colors x 4 bytes = 16 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  // r164 THUMB1: the four entries come from PixFormat's cover table (0, 85, 170, 255 =
  // optical levels 0, 5, 10, 15 of the 16-level axis). The bytes are what they always
  // were; naming the table is what stops the palette and the quantiser drifting apart
  // (Bitmap::readNextRow16 inverts THIS palette).
  for (const uint8_t lum : pixfmt::kCoverGrayLum) {
    bmpOut.write(lum);  // Blue
    bmpOut.write(lum);  // Green
    bmpOut.write(lum);  // Red
    bmpOut.write(static_cast<uint8_t>(0));  // Reserved
  }
}

namespace {

// Max MCU height supported by any JPEG (4:2:0 chroma = 16 rows, 4:4:4 = 8 rows)
constexpr int MAX_MCU_HEIGHT = 16;
constexpr size_t JPEG_DECODER_SIZE = 20 * 1024;
constexpr size_t MIN_FREE_HEAP = JPEG_DECODER_SIZE + 32 * 1024;
constexpr uint32_t FP_ONE = 1UL << 16;

// Static file pointer for JPEGDEC open callback.
// Safe in single-threaded embedded context; never accessed concurrently.
static HalFile* s_jpegFile = nullptr;
static uint8_t s_jpegIoSinceYield = 0;

static void yieldToIdle() { vTaskDelay(1); }

static void yieldDuringJpegIo() {
  if (++s_jpegIoSinceYield < 4) return;
  s_jpegIoSinceYield = 0;
  yieldToIdle();
}

void* bmpJpegOpen(const char* /*filename*/, int32_t* size) {
  if (!s_jpegFile || !*s_jpegFile) return nullptr;
  s_jpegIoSinceYield = 0;
  s_jpegFile->seek(0);
  *size = static_cast<int32_t>(s_jpegFile->size());
  yieldDuringJpegIo();
  return s_jpegFile;
}

void bmpJpegClose(void* /*handle*/) {
  // Caller owns the file — do not close it here
}

int32_t bmpJpegRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
  auto* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return 0;
  int32_t n = f->read(pBuf, len);
  if (n < 0) n = 0;
  pFile->iPos += n;
  yieldDuringJpegIo();
  return n;
}

int32_t bmpJpegSeek(JPEGFILE* pFile, int32_t pos) {
  auto* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f || !f->seek(pos)) return -1;
  pFile->iPos = pos;
  yieldDuringJpegIo();
  return pos;
}

// Context passed to the JPEGDEC draw callback via setUserPointer()
struct BmpConvertCtx {
  Print* bmpOut;
  int srcWidth;
  int srcHeight;
  int outWidth;
  int outHeight;
  bool oneBit;
  // r164 THUMB1: a COVER thumbnail - 2 bpp, but quantised with PixFormat's ordered
  // 4-level matrix (pixfmt::coverDitherIndex) instead of the legacy Atkinson-on-4-levels
  // ditherer. The legacy thresholds (0/15, 1/35, 2/90, 3/210) plus 6/8 error diffusion
  // are what crushed flat tone on the 1-bit path; a cover must reproduce its own grey.
  bool coverGreys;
  // GRAY16: write 8-bit greyscale rows (256-entry palette, raw luminance) instead
  // of a dithered 2-bit image. The consumer (Bitmap::readNextRow16 -> the 4 bpp
  // canvas) does the 16-level quantisation, so the tone must survive to disk.
  bool eightBit;
  int bytesPerRow;
  bool needsScaling;
  uint32_t scaleX_fp;  // source pixels per output pixel, 16.16 fixed-point
  uint32_t scaleY_fp;
  bool smoothUpscale;
  uint32_t smoothScaleX_fp;
  uint32_t smoothScaleY_fp;

  // Accumulates one MCU row (up to MAX_MCU_HEIGHT source rows × srcWidth pixels)
  // Filled column-by-column as JPEGDEC callbacks arrive for the same MCU row
  std::unique_ptr<uint8_t[]> mcuBuf;

  // Y-axis area averaging accumulators (needsScaling only)
  int currentOutY;
  uint32_t nextOutY_srcStart;  // 16.16 fixed-point boundary for the next output row
  std::unique_ptr<uint32_t[]> rowAccum;
  std::unique_ptr<uint32_t[]> rowCount;

  int smoothNextOutY;
  int smoothPrevY;
  std::unique_ptr<uint8_t[]> smoothRows;
  uint8_t* smoothPrevRow;
  uint8_t* smoothCurrRow;
  uint8_t* smoothOutRow;

  std::unique_ptr<uint8_t[]> bmpRow;

  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<FloydSteinbergDitherer> fsDitherer;
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitDitherer;

  uint8_t rowsSinceYield;
  uint8_t blocksSinceYield;
  bool error;

  // JPEG1 diagnostic tap: when set, every decoded source row (the decoder's Y
  // plane, pre-scaling) is compared against an expected raster. Unused in normal
  // operation; the on-device `jpeg compare` and the first-use gate fill these.
  const uint8_t* expectLuma;
  uint32_t expectStride;  // pixels
  int expectBpp;
  JpegLumaDiff* lumaDiff;
};

static void yieldDuringDecode(BmpConvertCtx* ctx) {
  if (++ctx->rowsSinceYield < 8) return;
  ctx->rowsSinceYield = 0;
  yieldToIdle();
}

static void yieldDuringDecodeBlock(BmpConvertCtx* ctx) {
  if (++ctx->blocksSinceYield < 16) return;
  ctx->blocksSinceYield = 0;
  yieldToIdle();
}

// Write a fully-assembled output row (grayscale bytes, length outWidth) to BMP
static void writeOutputRow(BmpConvertCtx* ctx, const uint8_t* srcRow, int outY) {
  memset(ctx->bmpRow.get(), 0, ctx->bytesPerRow);

  if (ctx->eightBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      ctx->bmpRow[x] = adjustPixel(srcRow[x]);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(srcRow[x], x)
                                                    : quantize1bit(srcRow[x], x, outY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel(srcRow[x]);
      uint8_t twoBit;
      if (ctx->coverGreys) {
        twoBit = pixfmt::coverDitherIndex(gray, x, outY);
      } else if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, outY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  ctx->bmpOut->write(ctx->bmpRow.get(), ctx->bytesPerRow);
  yieldDuringDecode(ctx);
}

// Matches the progressive-JPEG smoothing used by JpegToFramebufferConverter, but stays
// local because cover generation streams dithered BMP rows instead of framebuffer pixels.
static uint32_t interpolationStep(const int srcSize, const int outSize) {
  if (srcSize <= 1 || outSize <= 1) return 0;
  return (static_cast<uint32_t>(srcSize - 1) << 16) / static_cast<uint32_t>(outSize - 1);
}

static uint32_t interpolatedSourceFp(const int outIndex, const int outSize, const int srcSize, const uint32_t step) {
  if (srcSize <= 1 || outSize <= 1) return 0;
  if (outIndex >= outSize - 1) return static_cast<uint32_t>(srcSize - 1) << 16;
  return static_cast<uint32_t>(outIndex) * step;
}

static void scaleRowLinear(BmpConvertCtx* ctx, const uint8_t* srcRow, uint8_t* dstRow) {
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    const uint32_t srcX_fp = interpolatedSourceFp(outX, ctx->outWidth, ctx->srcWidth, ctx->smoothScaleX_fp);
    const int x0 = srcX_fp >> 16;
    const int x1 = (x0 + 1 < ctx->srcWidth) ? (x0 + 1) : x0;
    const uint32_t fx = srcX_fp & (FP_ONE - 1);
    dstRow[outX] = static_cast<uint8_t>((srcRow[x0] * (FP_ONE - fx) + srcRow[x1] * fx) >> 16);
  }
}

static void writeBlendedRow(BmpConvertCtx* ctx, const uint8_t* row0, const uint8_t* row1, const uint32_t fy,
                            const int outY) {
  const uint32_t invFy = FP_ONE - fy;
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    ctx->smoothOutRow[outX] = static_cast<uint8_t>((row0[outX] * invFy + row1[outX] * fy) >> 16);
  }
  writeOutputRow(ctx, ctx->smoothOutRow, outY);
}

static void processSmoothSourceRow(BmpConvertCtx* ctx, const uint8_t* srcRow, const int srcY) {
  scaleRowLinear(ctx, srcRow, ctx->smoothCurrRow);

  if (ctx->smoothPrevY < 0) {
    uint8_t* tmp = ctx->smoothPrevRow;
    ctx->smoothPrevRow = ctx->smoothCurrRow;
    ctx->smoothCurrRow = tmp;
    ctx->smoothPrevY = srcY;
    if (ctx->srcHeight <= 1) {
      while (ctx->smoothNextOutY < ctx->outHeight) {
        writeOutputRow(ctx, ctx->smoothPrevRow, ctx->smoothNextOutY);
        ctx->smoothNextOutY++;
      }
      return;
    }
    return;
  }

  while (ctx->smoothNextOutY < ctx->outHeight) {
    const uint32_t srcY_fp =
        interpolatedSourceFp(ctx->smoothNextOutY, ctx->outHeight, ctx->srcHeight, ctx->smoothScaleY_fp);
    const int y0 = srcY_fp >> 16;
    const int y1 = (y0 + 1 < ctx->srcHeight) ? (y0 + 1) : y0;
    if (y1 > srcY) break;

    const uint8_t* row0 = (y0 == srcY) ? ctx->smoothCurrRow : ctx->smoothPrevRow;
    const uint8_t* row1 = (y1 == srcY) ? ctx->smoothCurrRow : ctx->smoothPrevRow;
    writeBlendedRow(ctx, row0, row1, srcY_fp & (FP_ONE - 1), ctx->smoothNextOutY);
    ctx->smoothNextOutY++;
  }

  uint8_t* tmp = ctx->smoothPrevRow;
  ctx->smoothPrevRow = ctx->smoothCurrRow;
  ctx->smoothCurrRow = tmp;
  ctx->smoothPrevY = srcY;
}

static void finishSmoothUpscale(BmpConvertCtx* ctx) {
  if (ctx->smoothPrevY < 0) {
    LOG_ERR("JPG", "No progressive rows decoded for smoothing");
    ctx->error = true;
    return;
  }

  while (ctx->smoothNextOutY < ctx->outHeight) {
    writeOutputRow(ctx, ctx->smoothPrevRow, ctx->smoothNextOutY);
    ctx->smoothNextOutY++;
  }
}

// Flush one scaled output row from Y-axis accumulators and advance currentOutY
static void flushScaledRow(BmpConvertCtx* ctx) {
  memset(ctx->bmpRow.get(), 0, ctx->bytesPerRow);

  if (ctx->eightBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      ctx->bmpRow[x] = adjustPixel(gray);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(gray, x)
                                                    : quantize1bit(gray, x, ctx->currentOutY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel((ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0);
      uint8_t twoBit;
      if (ctx->coverGreys) {
        twoBit = pixfmt::coverDitherIndex(gray, x, ctx->currentOutY);
      } else if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, ctx->currentOutY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  ctx->bmpOut->write(ctx->bmpRow.get(), ctx->bytesPerRow);
  ctx->currentOutY++;
  yieldDuringDecode(ctx);
}

// One complete source row of the DECODED IMAGE (not of the output): this is the
// single point where the two decoders meet. The software path calls it from the
// JPEGDEC MCU callback, one MCU row at a time; the hardware path calls it over the
// driver's raster. Everything downstream — the area average, the progressive
// smoothing, the palette quantisation, the BMP row packing — is therefore the same
// code for both, so a difference in the BMP can only come from a difference in the
// luma this function is handed.
static void consumeSourceRow(BmpConvertCtx* ctx, const uint8_t* srcRow, int y) {
  // JPEG1 diagnostic: grade the decoder's own Y row against the expected raster
  // BEFORE any scaling or dithering, so the numbers describe decoders, not
  // thresholds.
  if (ctx->lumaDiff != nullptr && ctx->expectLuma != nullptr && y >= 0 && y < ctx->srcHeight) {
    const uint8_t* want = ctx->expectLuma + static_cast<size_t>(y) * ctx->expectStride * static_cast<size_t>(ctx->expectBpp);
    ctx->lumaDiff->addRow(srcRow, want, ctx->srcWidth, 1, ctx->expectBpp);
  }

  if (ctx->smoothUpscale) {
    processSmoothSourceRow(ctx, srcRow, y);
    return;
  }
  if (!ctx->needsScaling) {
    // 1:1 — outWidth == srcWidth, write directly
    writeOutputRow(ctx, srcRow, y);
    return;
  }

  // Fixed-point area averaging on X axis
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    const int srcXStart = (static_cast<uint32_t>(outX) * ctx->scaleX_fp) >> 16;
    const int srcXEnd = (static_cast<uint32_t>(outX + 1) * ctx->scaleX_fp) >> 16;
    int sum = 0;
    int count = 0;
    for (int srcX = srcXStart; srcX < srcXEnd && srcX < ctx->srcWidth; srcX++) {
      sum += srcRow[srcX];
      count++;
    }
    if (count == 0 && srcXStart < ctx->srcWidth) {
      sum = srcRow[srcXStart];
      count = 1;
    }
    ctx->rowAccum[outX] += sum;
    ctx->rowCount[outX] += count;
  }

  // Flush output row(s) whose Y boundary we've crossed
  const uint32_t srcY_fp = static_cast<uint32_t>(y + 1) << 16;
  while (srcY_fp >= ctx->nextOutY_srcStart && ctx->currentOutY < ctx->outHeight) {
    flushScaledRow(ctx);
    ctx->nextOutY_srcStart = static_cast<uint32_t>(ctx->currentOutY + 1) * ctx->scaleY_fp;
    if (srcY_fp >= ctx->nextOutY_srcStart) continue;
    memset(ctx->rowAccum.get(), 0, ctx->outWidth * sizeof(uint32_t));
    memset(ctx->rowCount.get(), 0, ctx->outWidth * sizeof(uint32_t));
  }
}

// JPEGDEC draw callback — receives one MCU-width × MCU-height block at a time,
// in left-to-right, top-to-bottom order (baseline JPEG).
// Accumulates columns into mcuBuf; once the last column arrives (completing the MCU
// row), hands each completed source row to consumeSourceRow().
int bmpDrawCallback(JPEGDRAW* pDraw) {
  auto* ctx = reinterpret_cast<BmpConvertCtx*>(pDraw->pUser);
  if (!ctx || ctx->error) return 0;
  yieldDuringDecodeBlock(ctx);

  const uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;
  const int blockX = pDraw->x;
  const int blockY = pDraw->y;

  // Guard against unexpected callback geometry so we never index past row buffers.
  if (blockX < 0 || blockY < 0 || blockX >= ctx->srcWidth || blockY >= ctx->srcHeight) {
    LOG_ERR("JPG", "Unexpected JPEG block origin (%d,%d) for decode grid %dx%d", blockX, blockY, ctx->srcWidth,
            ctx->srcHeight);
    ctx->error = true;
    return 0;
  }

  // Copy block pixels into MCU row buffer
  for (int r = 0; r < blockH && r < MAX_MCU_HEIGHT; r++) {
    const int copyW = (blockX + validW <= ctx->srcWidth) ? validW : (ctx->srcWidth - blockX);
    if (copyW <= 0) continue;
    memcpy(ctx->mcuBuf.get() + r * ctx->srcWidth + blockX, pixels + r * stride, copyW);
  }

  // Wait for the last MCU column before processing any rows
  if (blockX + validW < ctx->srcWidth) return 1;

  // Process each complete source row in this MCU row
  const int endRow = blockY + blockH;

  for (int y = blockY; y < endRow && y < ctx->srcHeight; y++) {
    consumeSourceRow(ctx, ctx->mcuBuf.get() + (y - blockY) * ctx->srcWidth, y);
    if (ctx->error) break;
  }

  return ctx->error ? 0 : 1;
}

}  // namespace

// Internal implementation with configurable target size and bit depth
bool JpegToBmpConverter::jpegFileToBmpStreamInternal(HalFile& jpegFile, Print& bmpOut, Spec spec) {
  const int targetWidth = spec.targetWidth;
  const int targetHeight = spec.targetHeight;
  const bool oneBit = spec.oneBit;
  const bool crop = spec.crop;
  const bool originalThresholds = spec.originalThresholds;
  const bool eightBit = spec.eightBit;
  const bool coverGreys = spec.coverGreys;

  LOG_DBG("JPG", "Converting JPEG to %s BMP (target: %dx%d)",
          oneBit ? "1-bit" : eightBit ? "8-bit greyscale" : coverGreys ? "2-bit cover greys" : "2-bit", targetWidth,
          targetHeight);

  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%u free, need %u)", ESP.getFreeHeap(), MIN_FREE_HEAP);
    return false;
  }

  s_jpegFile = &jpegFile;

  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;

  // ---- JPEG1: choose a decoder BEFORE any pixels are promised ----------------
  // The probe reads at most 16 KB (markers up to SOS) plus the file tail, so the
  // decision is made for EVERY file — including the ones the hardware cannot take,
  // which are then named in the log instead of silently decoded twice.
  JpegProbe probe;
  JpegHwReason hwWhy = JpegHwReason::NotProbed;
  JpegDecodePath selected = JpegDecodePath::Software;
  const bool probed = jpegProbeFile(jpegFile, &probe);
  if (spec.forceSoftware || !jpeghw::armed()) {
    hwWhy = JpegHwReason::Disabled;
  } else if (probed) {
    JpegHwLimits limits;
    limits.maxDimW = MAX_IMAGE_WIDTH;
    limits.maxDimH = MAX_IMAGE_HEIGHT;
    selected = jpegSelectPath(probe, jpegFile.size(), jpeghwLargestFreePsram(), limits, &hwWhy);
  }

  JpegHwFrame hwFrame;
  if (selected == JpegDecodePath::Hardware) {
    int driverErr = 0;
    const JpegHwStatus st = jpegHwDecodeFile(jpegFile, probe, jpegFile.size(), &hwFrame, &driverErr);
    if (st != JpegHwStatus::Ok) {
      LOG_WRN("JPG", "JPEG1: hardware decode failed (%s, err=%d) — software path", jpegHwStatusName(st), driverErr);
      selected = JpegDecodePath::Software;
      hwWhy = JpegHwReason::DecodeFailed;
    }
  }

  // FIRST USE AFTER BOOT: grade the hardware against the software decoder on this
  // very file, and produce the picture from the SOFTWARE decode. So the first
  // cover/thumbnail after a flash is bit-identical to what shipped before, and a
  // driver that disagrees (wrong plane, wrong stride, anything) switches the
  // hardware off for the rest of the session instead of painting a wrong image.
  const bool gradeHw = (selected == JpegDecodePath::Hardware) && jpeghw::gatePending();
  const bool useHw = (selected == JpegDecodePath::Hardware) && !gradeHw;
  JpegLumaDiff gateDiff;

  if (selected == JpegDecodePath::Software && hwWhy != JpegHwReason::Disabled) {
    LOG_DBG("JPG", "JPEG1: software path (%s)", jpegHwReasonName(hwWhy));
  }

  // ---- the software decoder, opened only when it will actually decode --------
  std::unique_ptr<JPEGDEC> jpeg;
  int srcWidth = 0;
  int srcHeight = 0;
  bool progressiveDecode = false;
  int decodedSrcWidth = 0;
  int decodedSrcHeight = 0;

  if (useHw) {
    // Geometry from the probe: the raster is already decoded at native size.
    srcWidth = probe.width;
    srcHeight = probe.height;
    decodedSrcWidth = srcWidth;
    decodedSrcHeight = srcHeight;
  } else {
    jpeg = makeUniqueNoThrow<JPEGDEC>();
    if (!jpeg) {
      LOG_ERR("JPG", "OOM: JPEG decoder");
      return false;
    }
    const int openRc = jpeg->open("", bmpJpegOpen, bmpJpegClose, bmpJpegRead, bmpJpegSeek, bmpDrawCallback);
    if (openRc != 1) {
      LOG_ERR("JPG", "JPEG open failed (err=%d)", jpeg->getLastError());
      return false;
    }
    srcWidth = jpeg->getWidth();
    srcHeight = jpeg->getHeight();
    progressiveDecode = (jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE);
    // JPEGDEC forces progressive streams to JPEG_SCALE_EIGHTH in DecodeJPEG,
    // so callback coordinates and MCU buffering must use the reduced decode grid.
    decodedSrcWidth = progressiveDecode ? ((srcWidth + 7) >> 3) : srcWidth;
    decodedSrcHeight = progressiveDecode ? ((srcHeight + 7) >> 3) : srcHeight;

    LOG_DBG("JPG", "JPEG dimensions: %dx%d", srcWidth, srcHeight);
    if (progressiveDecode) {
      LOG_DBG("JPG", "Progressive JPEG decode uses 1/8 source: %dx%d", decodedSrcWidth, decodedSrcHeight);
    }
  }

  const ScopedCleanup cleanup{[&jpeg]() {
    if (jpeg) jpeg->close();
  }};

  if (srcWidth <= 0 || srcHeight <= 0 || srcWidth > MAX_IMAGE_WIDTH || srcHeight > MAX_IMAGE_HEIGHT) {
    LOG_DBG("JPG", "Image too large or invalid (%dx%d), max supported: %dx%d", srcWidth, srcHeight, MAX_IMAGE_WIDTH,
            MAX_IMAGE_HEIGHT);
    return false;
  }

  // Calculate output dimensions (pre-scale to fit display exactly)
  int outWidth = srcWidth;
  int outHeight = srcHeight;
  if (targetWidth <= 0 || targetHeight <= 0) {
    // Without an explicit target, keep decoder-native dimensions.
    outWidth = decodedSrcWidth;
    outHeight = decodedSrcHeight;
  }

  const int scaleSrcWidth = decodedSrcWidth;
  const int scaleSrcHeight = decodedSrcHeight;

  uint32_t scaleX_fp = 65536;  // 1.0 in 16.16 fixed point
  uint32_t scaleY_fp = 65536;
  bool needsScaling = false;

  if (targetWidth > 0 && targetHeight > 0 && (srcWidth != targetWidth || srcHeight != targetHeight)) {
    const float scaleToFitWidth = static_cast<float>(targetWidth) / srcWidth;
    const float scaleToFitHeight = static_cast<float>(targetHeight) / srcHeight;
    float scale = 1.0f;
    if (crop) {
      scale = (scaleToFitWidth > scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    } else {
      scale = (scaleToFitWidth < scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    }

    outWidth = static_cast<int>(srcWidth * scale);
    outHeight = static_cast<int>(srcHeight * scale);
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;

    LOG_DBG("JPG", "Scaling source %dx%d (decode grid %dx%d) -> %dx%d (target %dx%d)", srcWidth, srcHeight,
            scaleSrcWidth, scaleSrcHeight, outWidth, outHeight, targetWidth, targetHeight);
  }

  if (scaleSrcWidth != outWidth || scaleSrcHeight != outHeight) {
    scaleX_fp = (static_cast<uint32_t>(scaleSrcWidth) << 16) / outWidth;
    scaleY_fp = (static_cast<uint32_t>(scaleSrcHeight) << 16) / outHeight;
    needsScaling = true;
  }

  const bool smoothUpscale =
      progressiveDecode && needsScaling && scaleSrcWidth <= outWidth && scaleSrcHeight <= outHeight;

  // Write BMP header with output dimensions
  int bytesPerRow;
  if (eightBit && !oneBit) {
    writeBmpHeader8bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 3) / 4 * 4;
  } else if (oneBit) {
    writeBmpHeader1bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 31) / 32 * 4;
  } else {
    writeBmpHeader2bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
  }

  BmpConvertCtx ctx = {};
  ctx.bmpOut = &bmpOut;
  ctx.srcWidth = scaleSrcWidth;
  ctx.srcHeight = scaleSrcHeight;
  ctx.outWidth = outWidth;
  ctx.outHeight = outHeight;
  ctx.oneBit = oneBit;
  ctx.coverGreys = coverGreys && !oneBit && !eightBit;
  ctx.eightBit = eightBit && !oneBit;
  ctx.bytesPerRow = bytesPerRow;
  ctx.needsScaling = needsScaling;
  ctx.scaleX_fp = scaleX_fp;
  ctx.scaleY_fp = scaleY_fp;
  ctx.smoothUpscale = smoothUpscale;
  ctx.smoothScaleX_fp = interpolationStep(ctx.srcWidth, outWidth);
  ctx.smoothScaleY_fp = interpolationStep(ctx.srcHeight, outHeight);
  ctx.smoothNextOutY = 0;
  ctx.smoothPrevY = -1;
  ctx.rowsSinceYield = 0;
  ctx.blocksSinceYield = 0;
  ctx.error = false;
  // JPEG1: the gate grades the hardware raster; an external comparison supplies
  // its own expected plane through the spec.
  if (gradeHw) {
    ctx.expectLuma = hwFrame.row(0);
    ctx.expectStride = hwFrame.stride();
    ctx.expectBpp = hwFrame.bpp();
    ctx.lumaDiff = &gateDiff;
  } else {
    ctx.expectLuma = spec.expectLuma;
    ctx.expectStride = spec.expectStride;
    ctx.expectBpp = spec.expectBpp;
    ctx.lumaDiff = spec.lumaDiff;
  }

  // MCU row buffer: MAX_MCU_HEIGHT rows × decoded srcWidth columns of grayscale
  ctx.mcuBuf = makeUniqueNoThrow<uint8_t[]>(MAX_MCU_HEIGHT * ctx.srcWidth);
  if (!ctx.mcuBuf) {
    LOG_ERR("JPG", "OOM: MCU buffer (%d bytes)", MAX_MCU_HEIGHT * ctx.srcWidth);
    return false;
  }
  memset(ctx.mcuBuf.get(), 0, MAX_MCU_HEIGHT * ctx.srcWidth);

  ctx.bmpRow = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
  if (!ctx.bmpRow) {
    LOG_ERR("JPG", "OOM: BMP row buffer");
    return false;
  }

  if (smoothUpscale) {
    // One contiguous allocation avoids three heap blocks while keeping smoothing line-buffered.
    const size_t smoothRowsBytes = static_cast<size_t>(outWidth) * 3;
    ctx.smoothRows = makeUniqueNoThrow<uint8_t[]>(smoothRowsBytes);
    if (!ctx.smoothRows) {
      LOG_ERR("JPG", "OOM: progressive smoothing buffers");
      return false;
    }
    ctx.smoothPrevRow = ctx.smoothRows.get();
    ctx.smoothCurrRow = ctx.smoothPrevRow + outWidth;
    ctx.smoothOutRow = ctx.smoothCurrRow + outWidth;
    LOG_DBG("JPG", "Progressive smoothing: %dx%d -> %dx%d, buffers=%u bytes", ctx.srcWidth, ctx.srcHeight, outWidth,
            outHeight, static_cast<unsigned>(smoothRowsBytes));
  } else if (needsScaling) {
    ctx.rowAccum = makeUniqueNoThrow<uint32_t[]>(outWidth);
    ctx.rowCount = makeUniqueNoThrow<uint32_t[]>(outWidth);
    if (!ctx.rowAccum || !ctx.rowCount) {
      LOG_ERR("JPG", "OOM: scaling buffers");
      return false;
    }
    ctx.nextOutY_srcStart = scaleY_fp;
  }

  if (oneBit) {
    ctx.atkinson1BitDitherer = makeUniqueNoThrow<Atkinson1BitDitherer>(outWidth);
    if (!ctx.atkinson1BitDitherer || !ctx.atkinson1BitDitherer->isValid()) {
      LOG_ERR("JPG", "OOM: Atkinson1BitDitherer");
      return false;
    }
  } else if (!eightBit && !coverGreys) {
    // r164 THUMB1: a cover has no error to diffuse (per-pixel ordered quantisation), so
    // its three error rows are not allocated at all.
    if (USE_ATKINSON) {
      ctx.atkinsonDitherer = makeUniqueNoThrow<AtkinsonDitherer>(outWidth, originalThresholds);
      if (!ctx.atkinsonDitherer || !ctx.atkinsonDitherer->isValid()) {
        LOG_ERR("JPG", "OOM: AtkinsonDitherer");
        return false;
      }
    } else if (USE_FLOYD_STEINBERG) {
      ctx.fsDitherer = makeUniqueNoThrow<FloydSteinbergDitherer>(outWidth, originalThresholds);
      if (!ctx.fsDitherer || !ctx.fsDitherer->isValid()) {
        LOG_ERR("JPG", "OOM: FloydSteinbergDitherer");
        return false;
      }
    }
  }

  // ---- decode ---------------------------------------------------------------
  // Same consumption path for both decoders: the hardware one walks its raster row
  // by row, the software one drives the same function from the MCU callback.
  bool decodeOk = true;
  if (useHw) {
    for (int y = 0; y < ctx.srcHeight && !ctx.error; y++) {
      consumeSourceRow(&ctx, hwFrame.row(y), y);
    }
    if (ctx.smoothUpscale && !ctx.error) {
      finishSmoothUpscale(&ctx);
    }
    decodeOk = !ctx.error;
    if (!decodeOk) LOG_ERR("JPG", "JPEG1: hardware raster walk failed");
  } else {
    jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
    jpeg->setUserPointer(&ctx);

    const int rc = jpeg->decode(0, 0, 0);

    if (rc == 1 && ctx.smoothUpscale && !ctx.error) {
      finishSmoothUpscale(&ctx);
    }

    if (rc != 1 || ctx.error) {
      LOG_ERR("JPG", "JPEG decode failed (rc=%d, err=%d)", rc, jpeg->getLastError());
      decodeOk = false;
    }
  }

  // Grade the hardware once per boot, on a real file, before any image depends on
  // it. A failure disables the hardware for the session (jpeghw::noteGateResult).
  if (gradeHw && decodeOk) {
    jpeghw::noteGateResult(gateDiff.matches(), gateDiff);
  }

  if (decodeOk) {
    jpeghw::noteDecision(selected, hwWhy);
  }

  if (!decodeOk) {
    return false;
  }

  LOG_DBG("JPG", "Successfully converted JPEG to BMP");
  return true;
}

// Core function: Convert JPEG file to 2-bit BMP (uses default target size)
bool JpegToBmpConverter::jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop, bool originalThresholds,
                                             bool eightBit) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  Spec spec;
  spec.targetWidth = display.getDisplayHeight();
  spec.targetHeight = display.getDisplayWidth();
  spec.crop = crop;
  spec.originalThresholds = originalThresholds;
  spec.eightBit = eightBit;
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, spec);
}

// Convert with custom target size (for thumbnails, and the custom sleep images)
bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight, bool eightBit, bool crop) {
  Spec spec;
  spec.targetWidth = targetMaxWidth;
  spec.targetHeight = targetMaxHeight;
  spec.eightBit = eightBit;
  spec.crop = crop;
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, spec);
}

// Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                         int targetMaxHeight) {
  Spec spec;
  spec.targetWidth = targetMaxWidth;
  spec.targetHeight = targetMaxHeight;
  spec.oneBit = true;
  spec.crop = true;
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, spec);
}

// r164 THUMB1: the Home cover thumbnail. Same sizing and crop as the 1-bit call above
// (width = height * 0.6, cover crop) - changing the composition is a separate,
// taste-driven decision (R164_THUMB1_INVESTIGATION 5 step 3). Only the bit depth and
// the quantiser differ, so the 2 bpp file is the SAME picture as the old one, at four
// optical levels instead of two.
bool JpegToBmpConverter::jpegFileToCoverThumbBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                               int targetMaxHeight) {
  Spec spec;
  spec.targetWidth = targetMaxWidth;
  spec.targetHeight = targetMaxHeight;
  spec.crop = true;
  spec.coverGreys = true;
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, spec);
}

// ---------------------------------------------------------------------------
// JPEG1 on-device comparison.
//
// Both decoders decode the SAME file. The hardware one produces a PSRAM raster;
// the software one is then run with the raster as its expected plane, so every
// decoded Y row is compared against the hardware's Y row as it arrives (see
// consumeSourceRow). The BMP the software decode writes goes to a null sink —
// nothing on the card is touched, no thumbnail cache is involved, and the format
// of any file this ever writes is unchanged.
//
// The comparison is on LUMA, not on BMP bytes. A byte-for-byte BMP comparison
// would be the wrong test: both decoders feed the same scaler, quantiser and
// ditherer, so any BMP difference is a consequence of a luma difference plus
// threshold noise — the luma numbers are the ones that say "same picture".
// ---------------------------------------------------------------------------
namespace {
class NullPrint : public Print {
 public:
  size_t write(uint8_t) override { return 1; }
  size_t write(const uint8_t*, size_t n) override { return n; }
};
}  // namespace

bool JpegToBmpConverter::jpegCompareHwVsSw(HalFile& jpegFile, JpegLumaDiff* diff, JpegProbe* probeOut,
                                           JpegHwReason* why, int* frameWidth, int* frameHeight,
                                           JpegHwStatus* hwStatus) {
  if (diff) *diff = JpegLumaDiff{};
  if (frameWidth) *frameWidth = 0;
  if (frameHeight) *frameHeight = 0;
  if (hwStatus) *hwStatus = JpegHwStatus::BadStream;
  if (why) *why = JpegHwReason::NotProbed;

  JpegProbe probe;
  if (!jpegProbeFile(jpegFile, &probe)) {
    if (why) *why = JpegHwReason::NotProbed;
    return false;
  }
  if (probeOut) *probeOut = probe;

  JpegHwLimits limits;
  limits.maxDimW = 2048;
  limits.maxDimH = 3072;
  // The comparison must be possible even when the runtime arm is off — that is how
  // an operator checks the hardware before turning it on.
  JpegHwReason sel = JpegHwReason::Eligible;
  const JpegDecodePath path = jpegSelectPath(probe, jpegFile.size(), jpeghwLargestFreePsram(), limits, &sel);
  if (why) *why = sel;
  if (path != JpegDecodePath::Hardware) return false;

  JpegHwFrame frame;
  int driverErr = 0;
  const JpegHwStatus st = jpegHwDecodeFile(jpegFile, probe, jpegFile.size(), &frame, &driverErr);
  if (hwStatus) *hwStatus = st;
  if (st != JpegHwStatus::Ok) {
    if (why) *why = JpegHwReason::DecodeFailed;
    return false;
  }

  NullPrint sink;
  Spec spec;
  spec.forceSoftware = true;
  spec.expectLuma = frame.row(0);
  spec.expectStride = frame.stride();
  spec.expectBpp = frame.bpp();
  spec.lumaDiff = diff;
  // Native size: the tap compares decoder output before any scaling, so a target
  // size would only add work.
  spec.targetWidth = 0;
  spec.targetHeight = 0;
  spec.eightBit = true;  // 8-bit grey rows: cheapest downstream of the tap

  const bool ok = jpegFileToBmpStreamInternal(jpegFile, sink, spec);
  if (frameWidth) *frameWidth = frame.width();
  if (frameHeight) *frameHeight = frame.height();
  if (!ok && why) *why = JpegHwReason::DecodeFailed;
  return ok;
}
