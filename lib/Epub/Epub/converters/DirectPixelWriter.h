#pragma once

#include <BitmapHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <stdint.h>

#include <cassert>

// Direct framebuffer writer that eliminates per-pixel overhead from the image
// rendering hot path.  Pre-computes orientation transform as linear coefficients
// and caches render-mode state so the inner loop is: one multiply, one add,
// one shift, and one AND per pixel — no branches, no method calls.
//
// Caller is responsible for ensuring (outX, outY) are within screen bounds.
// ImageBlock::render() already validates this before entering the pixel loop,
// and the JPEG/PNG callbacks pre-clamp destination ranges to screen bounds.
struct DirectPixelWriter {
  uint8_t* fb;
  GfxRenderer::RenderMode mode;
  bool absolute = false;
  uint16_t displayWidthBytes;  // Runtime framebuffer stride (X4: 100, X3: 99)
  // Active write target: for tiled grayscale, fb is the band scratch, originY is
  // the band's top physical row, and clipRows is the band height. Off-band
  // pixels are dropped. With no strip active these collapse to the full frame
  // (originY 0, clipRows panelHeight) so the clip doubles as a bounds guard.
  int originY;
  int clipRows;
  // GRAY16: pixelValue is a 16-level optical value (0 ink .. 15 paper) instead of
  // a 2-bit halftone coordinate, and it is written straight through as a nibble.
  // Only meaningful on the 4 bpp canvas; forced false otherwise in init().
  bool gray16 = false;

  // Orientation is collapsed into a linear transform:
  //   phyX = phyXBase + x * phyXStepX + y * phyXStepY
  //   phyY = phyYBase + x * phyYStepX + y * phyYStepY
  int phyXBase, phyYBase;
  int phyXStepX, phyYStepX;  // per logical-X step
  int phyXStepY, phyYStepY;  // per logical-Y step

  // Row-precomputed: the Y-dependent portion of the physical coords
  int rowPhyXBase, rowPhyYBase;

  void init(GfxRenderer& renderer, bool gray16Images = false) {
    fb = renderer.getWriteTarget();
    originY = renderer.getWriteOriginY();
    clipRows = renderer.getWriteRows();
    mode = renderer.getRenderMode();
    absolute = renderer.grayPlanesAreAbsolute();
    displayWidthBytes = renderer.getDisplayWidthBytes();
    // A 16-level value has nowhere to live on a 1 bpp canvas, so the request is
    // dropped rather than silently thresholded at a different cut than the BW one.
    gray16 = gray16Images && pixfmt::kFourBit &&
             static_cast<const void*>(fb) != nullptr && mode == GfxRenderer::BW;

    const int phyW = renderer.getDisplayWidth();
    const int phyH = renderer.getDisplayHeight();

    switch (renderer.getOrientation()) {
      case GfxRenderer::Portrait:
        // phyX = y, phyY = (phyH-1) - x
        phyXBase = 0;
        phyYBase = phyH - 1;
        phyXStepX = 0;
        phyYStepX = -1;
        phyXStepY = 1;
        phyYStepY = 0;
        break;
      case GfxRenderer::LandscapeClockwise:
        // phyX = (phyW-1) - x, phyY = (phyH-1) - y
        phyXBase = phyW - 1;
        phyYBase = phyH - 1;
        phyXStepX = -1;
        phyYStepX = 0;
        phyXStepY = 0;
        phyYStepY = -1;
        break;
      case GfxRenderer::PortraitInverted:
        // phyX = (phyW-1) - y, phyY = x
        phyXBase = phyW - 1;
        phyYBase = 0;
        phyXStepX = 0;
        phyYStepX = 1;
        phyXStepY = -1;
        phyYStepY = 0;
        break;
      case GfxRenderer::LandscapeCounterClockwise:
        // phyX = x, phyY = y
        phyXBase = 0;
        phyYBase = 0;
        phyXStepX = 1;
        phyYStepX = 0;
        phyXStepY = 0;
        phyYStepY = 1;
        break;
      default:
        // Fallback to LandscapeCounterClockwise (identity transform)
        phyXBase = 0;
        phyYBase = 0;
        phyXStepX = 1;
        phyYStepX = 0;
        phyXStepY = 0;
        phyYStepY = 1;
        break;
    }
  }

  // Call once per row before the column loop.
  // Pre-computes the Y-dependent portion so writePixel() only needs the X part.
  inline void beginRow(int logicalY) {
    rowPhyXBase = phyXBase + logicalY * phyXStepY;
    rowPhyYBase = phyYBase + logicalY * phyYStepY;
  }

  // For the current row (set via beginRow), narrow [colStart, colEnd) to the
  // columns whose pixels fall inside the active strip band. writePixel() would
  // clip the rest anyway, but on a strip pass that is most of a full-page image
  // (only ~one strip-height worth of columns survive in portrait); skipping them
  // here avoids the per-pixel unpack+transform entirely. For full-frame passes
  // (clipRows == panel height) the range is unchanged. xBase is the logical X of
  // column 0; the band test mirrors writePixel(): 0 <= phyY - originY < clipRows.
  inline void bandColRange(int xBase, int width, int& colStart, int& colEnd) const {
    // init() only ever sets phyYStepX to 0, +1, or -1; the +1/-1 solve below
    // relies on that.
    assert(phyYStepX == 0 || phyYStepX == 1 || phyYStepX == -1);
    colStart = 0;
    colEnd = width;
    if (phyYStepX == 0) {
      // phyY is constant across the row: the whole row is in-band or out.
      const int sy = rowPhyYBase - originY;
      if (static_cast<unsigned>(sy) >= static_cast<unsigned>(clipRows)) colEnd = 0;
      return;
    }
    // phyY = rowPhyYBase + logicalX * phyYStepX (phyYStepX is +1 or -1).
    // Solve originY <= phyY <= originY + clipRows - 1 for logicalX.
    const int loY = originY;
    const int hiY = originY + clipRows - 1;
    int xLo, xHi;
    if (phyYStepX > 0) {
      xLo = loY - rowPhyYBase;
      xHi = hiY - rowPhyYBase;
    } else {
      xLo = rowPhyYBase - hiY;
      xHi = rowPhyYBase - loY;
    }
    const int cs = xLo - xBase;
    const int ce = xHi - xBase + 1;  // exclusive
    if (cs > colStart) colStart = cs;
    if (ce < colEnd) colEnd = ce;
    if (colStart < 0) colStart = 0;
    if (colEnd > width) colEnd = width;
    if (colStart > colEnd) colStart = colEnd;
  }

  // Write a single 2-bit dithered pixel value to the framebuffer.
  // Must be called after beginRow() for the current row.
  // No bounds checking — caller guarantees coordinates are valid.
  inline void writePixel(int logicalX, uint8_t pixelValue, bool writeWhiteInBw = false) const {
    // GRAY16: the value IS the optical level (0 ink .. 15 paper) and goes to the
    // canvas as-is. `draw` is therefore always true -- writing paper is what the
    // level means, not "leave the old pixel alone" -- which is the one semantic
    // difference from the 2-level path (there, `draw=false` skipped the pixel so
    // a previously-inked pixel under a white source stayed inked).
    if (gray16) {
      const int phyX = rowPhyXBase + logicalX * phyXStepX;
      const int phyY = rowPhyYBase + logicalX * phyYStepX;
      const int sy = phyY - originY;
      if (static_cast<unsigned>(sy) >= static_cast<unsigned>(clipRows)) return;
      const uint32_t byteIndex = static_cast<uint32_t>(sy) * static_cast<uint32_t>(displayWidthBytes) +
                                 static_cast<uint32_t>(phyX >> 1);
      const uint8_t nib = pixfmt::imageLevel16ToNibble(pixelValue);
      if (phyX & 1)
        fb[byteIndex] = static_cast<uint8_t>((fb[byteIndex] & 0x0F) | (nib << 4));
      else
        fb[byteIndex] = static_cast<uint8_t>((fb[byteIndex] & 0xF0) | nib);
      return;
    }

    // Determine whether to draw based on render mode
    bool draw;
    bool state;
    switch (mode) {
      case GfxRenderer::BW:
        // 1-bpp reduction of a 4-level value. The cut is 2, i.e. the 1|2
        // boundary -- level 2 ("light") and 3 ("white") stay white, 0 and 1
        // draw black. It used to be `< 3`, which is only correct for input that
        // is already 0/3 (a 1-bit BMP expanded to 2 bits, or a 1-bit dither).
        // Against the readers' 4-level dither it was a 3/4 threshold: a flat
        // 50% grey (level 2) rendered 100% black and every midtone in a photo
        // was crushed (r92/IMG2). Because this branch is the ONLY one that
        // runs on a panel without grayscale capability, that cut alone decided
        // the tone of every in-book image on such a panel.
        draw = writeWhiteInBw || pixelValue < 2;
        state = pixelValue < 2;
        break;
      case GfxRenderer::GRAYSCALE_MSB:
      case GfxRenderer::GRAYSCALE_LSB: {
        const auto pixel = grayPlanePixel(pixelValue, mode == GfxRenderer::GRAYSCALE_MSB, absolute);
        draw = pixel.write;
        state = pixel.black;
        break;
      }
      default:
        return;
    }

    if (!draw) return;

    const int phyX = rowPhyXBase + logicalX * phyXStepX;
    const int phyY = rowPhyYBase + logicalX * phyYStepX;

    // Band-local row. The unsigned compare drops both off-band pixels (strip
    // mode) and any out-of-frame row (full-frame mode) in one branch.
    const int sy = phyY - originY;
    if (static_cast<unsigned>(sy) >= static_cast<unsigned>(clipRows)) return;

#if CP_RENDER_4BPP
    if constexpr (pixfmt::kFourBit) {
      // 4 bpp canvas. One byte holds two pixels (even x in the LOW nibble), so
      // the byte index is phyX >> 1 and the write is a nibble splice.
      //
      // BW is a TWO-LEVEL mode on this panel: the cut above (`pixelValue < 2`,
      // r92/IMG2) decides the tone of every in-book image, and the 4 bpp canvas
      // does not widen that to four tones. Writing imageLevelToNibble() here
      // ignored `state` and painted levels 0x0/0x5 for a decoded photo, which is
      // the grey wash the operator sees on dithered pictures; a decoded image
      // level is a halftone coordinate, exactly as in Bitmap::readNextRow.
      const uint32_t byteIndex = static_cast<uint32_t>(sy) * static_cast<uint32_t>(displayWidthBytes) +
                                 static_cast<uint32_t>(phyX >> 1);
      const uint8_t nib = state ? pixfmt::kInk : pixfmt::kPaper;  // both modes stay 2-level
      if (phyX & 1)
        fb[byteIndex] = static_cast<uint8_t>((fb[byteIndex] & 0x0F) | (nib << 4));
      else
        fb[byteIndex] = static_cast<uint8_t>((fb[byteIndex] & 0xF0) | nib);
      return;
    }
#endif

    // Byte index into the framebuffer. This MUST be 32-bit: a 1216x684 1-bpp
    // frame is 103,968 bytes, so a uint16_t index wraps at byte 65,536 (row
    // 431 at a 152-byte stride). Every pixel below that row was written into
    // the top of the buffer instead of its own row. In portrait (phyY = 683-x)
    // that turned the page's leading ~253 columns into a never-written pale
    // stripe -- one third of a 684 px axis -- while their pixels overprinted
    // the trailing third as a ghost. GfxRenderer::drawPixel has always used
    // uint32_t, which is why text was correct and only images broke.
    const uint32_t byteIndex = static_cast<uint32_t>(sy) * static_cast<uint32_t>(displayWidthBytes) +
                               static_cast<uint32_t>(phyX >> 3);
    const uint8_t bitMask = 1 << (7 - (phyX & 7));

    if (state) {
      fb[byteIndex] &= ~bitMask;  // Clear bit (draw black)
    } else {
      fb[byteIndex] |= bitMask;  // Set bit (draw white)
    }
  }
};

// Direct cache writer that eliminates per-pixel overhead from PixelCache::setPixel().
// Pre-computes row pointer so the inner loop is just byte index + bit manipulation.
//
// The cache buffer is a small streaming band (e.g. 16 rows), not the full image,
// so a band-relative row/column that lands outside it would corrupt adjacent
// heap. This writer therefore bounds-checks every access: beginRow() invalidates
// the row when it falls outside the band, and writePixel() drops out-of-range
// columns. This path only runs during the single decode that populates the
// cache, never on the screen render hot path, so the checks are cheap.
struct DirectCacheWriter {
  uint8_t* buffer;
  int bytesPerRow;
  int bandRows;
  int originX;
  // GRAY16: the cache row holds 16-level nibbles (2 px/byte, even x in the HIGH
  // nibble because the row is read back in raster order) instead of 2-bit
  // halftone coordinates. Set from PixelCache::begin so writer and reader can
  // never disagree about the format.
  bool gray16;
  uint8_t* rowPtr;  // Pre-computed for current row; nullptr if row is out of band

  void init(uint8_t* cacheBuffer, int cacheBytesPerRow, int cacheBandRows, int cacheOriginX, bool gray16Cache = false) {
    buffer = cacheBuffer;
    bytesPerRow = cacheBytesPerRow;
    bandRows = cacheBandRows;
    originX = cacheOriginX;
    gray16 = gray16Cache;
    rowPtr = nullptr;
  }

  // Call once per row before the column loop. Drops rows outside the band.
  inline void beginRow(int screenY, int cacheOriginY) {
    const int localRow = screenY - cacheOriginY;
    rowPtr = (static_cast<unsigned>(localRow) < static_cast<unsigned>(bandRows))
                 ? buffer + (size_t)localRow * bytesPerRow
                 : nullptr;
  }

  // Write a 2-bit pixel value. Drops the write if the row is out of band or the
  // column is out of range.
  inline void writePixel(int screenX, uint8_t value) const {
    if (!rowPtr) return;
    const int localX = screenX - originX;
    if (gray16) {
      // 16-level row: byte = localX / 2, EVEN x in the HIGH nibble (raster order,
      // which is how renderRowsFromPxcSlot/ImageBlock read it back).
      const int byteIdx = localX >> 1;
      if (static_cast<unsigned>(byteIdx) >= static_cast<unsigned>(bytesPerRow)) return;
      const uint8_t nib = pixfmt::imageLevel16ToNibble(value);
      if (localX & 1)
        rowPtr[byteIdx] = static_cast<uint8_t>((rowPtr[byteIdx] & 0xF0) | nib);
      else
        rowPtr[byteIdx] = static_cast<uint8_t>((rowPtr[byteIdx] & 0x0F) | (nib << 4));
      return;
    }
    const int byteIdx = localX >> 2;  // localX / 4
    if (static_cast<unsigned>(byteIdx) >= static_cast<unsigned>(bytesPerRow)) return;
    const int bitShift = 6 - (localX & 3) * 2;  // MSB first: pixel 0 at bits 6-7
    rowPtr[byteIdx] = (rowPtr[byteIdx] & ~(0x03 << bitShift)) | ((value & 0x03) << bitShift);
  }
};
