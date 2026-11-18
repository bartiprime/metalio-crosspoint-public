#include "ScreenshotUtil.h"

#include <Arduino.h>
#include <BitmapHelpers.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <string>

#include "Bitmap.h"  // Required for BmpHeader struct definition
#include "activities/Activity.h"

void ScreenshotUtil::buildFilename(const ScreenshotInfo& info, char* buf, size_t bufSize) {
  const unsigned long ts = millis();

  if (info.readerType == ScreenshotInfo::ReaderType::None || info.title[0] == '\0') {
    snprintf(buf, bufSize, "/screenshots/screenshot-%lu.bmp", ts);
    return;
  }

  char sanitizedTitle[64];
  FsHelpers::sanitizePathComponentForFat32(info.title, sanitizedTitle, sizeof(sanitizedTitle));
  if (sanitizedTitle[0] == '\0') {
    snprintf(buf, bufSize, "/screenshots/screenshot-%lu.bmp", ts);
    return;
  }

  int pct = info.progressPercent;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

  // Display spine index as 1-based for user-facing filenames
  const int chapterNum = info.spineIndex + 1;

  if (info.readerType == ScreenshotInfo::ReaderType::Epub && info.spineIndex >= 0) {
    snprintf(buf, bufSize, "/screenshots/%s/%s_ch%d_p%d_%dpct_%lu.bmp", sanitizedTitle, sanitizedTitle, chapterNum,
             info.currentPage, pct, ts);
  } else {
    snprintf(buf, bufSize, "/screenshots/%s/%s_p%d_%dpct_%lu.bmp", sanitizedTitle, sanitizedTitle, info.currentPage,
             pct, ts);
  }

  // Truncate title if total path exceeds FAT32 limit
  if (strlen(buf) > 255) {
    size_t titleLen = strlen(sanitizedTitle);
    size_t overhead = strlen(buf) - 2 * titleLen;
    if (overhead < 255) {
      size_t maxTitleLen = (255 - overhead) / 2;
      // Walk back to a valid UTF-8 boundary to avoid corrupting multibyte characters
      while (maxTitleLen > 0 && (sanitizedTitle[maxTitleLen] & 0xC0) == 0x80) {
        maxTitleLen--;
      }
      sanitizedTitle[maxTitleLen] = '\0';
      if (info.readerType == ScreenshotInfo::ReaderType::Epub && info.spineIndex >= 0) {
        snprintf(buf, bufSize, "/screenshots/%s/%s_ch%d_p%d_%dpct_%lu.bmp", sanitizedTitle, sanitizedTitle, chapterNum,
                 info.currentPage, pct, ts);
      } else {
        snprintf(buf, bufSize, "/screenshots/%s/%s_p%d_%dpct_%lu.bmp", sanitizedTitle, sanitizedTitle, info.currentPage,
                 pct, ts);
      }
    } else {
      snprintf(buf, bufSize, "/screenshots/screenshot-%lu.bmp", ts);
    }
  }
}

void ScreenshotUtil::takeScreenshot(GfxRenderer& renderer) {
  const uint8_t* fb = renderer.getFrameBuffer();
  if (!fb) {
    LOG_ERR("SCR", "Framebuffer not available");
    return;
  }

  ScreenshotInfo info = activityManager.getScreenshotInfo();
  char filename[256];
  buildFilename(info, filename, sizeof(filename));

  bool saved = saveFramebufferAsBmp(filename, fb, renderer.getDisplayWidth(), renderer.getDisplayHeight(),
                                   renderer.getOrientation());
  if (saved) {
    LOG_DBG("SCR", "Screenshot saved to %s", filename);
  } else {
    LOG_ERR("SCR", "Failed to save screenshot");
    return;
  }

  // Display a border around the screen to indicate a screenshot was taken
  if (renderer.storeBwBuffer()) {
    int marginTop, marginRight, marginBottom, marginLeft;
    renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
    int width = renderer.getScreenWidth() - marginLeft - marginRight - 1;
    int height = renderer.getScreenHeight() - marginTop - marginBottom - 1;
    // Add extra margin to the border to make it more visible
    // Explicit modes on both paints: the border is an edit over the live frame, and the restore puts
    // the pre-screenshot base back wholesale. A bare displayBuffer() here (and a diff-mode restore)
    // is how a screenshot could leave the base partially driven.
    renderer.drawRect(marginLeft + 1, marginTop + 1, width - 2, height - 2, 2, true);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    delay(1000);
    renderer.restoreBwBuffer();
    renderer.displayBuffer(HalDisplay::RefreshMode::FULL_REFRESH);
  }
}

bool ScreenshotUtil::saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width, int height,
                                          const GfxRenderer::Orientation orientation) {
  if (!framebuffer) {
    return false;
  }

  // `framebuffer` is ALWAYS the panel's physical frame (`width` x `height` = the panel's
  // scan geometry, 1216x684 here) whatever the orientation: drawing code maps logical
  // coordinates into it through GfxRenderer::logicalToPhysical(). The BMP, though, is
  // the page the reader SEES, so its dimensions are the LOGICAL ones and every pixel is
  // read back through that same transform.
  //
  // This used to be a fixed 90 deg counter-clockwise rotation, which is only correct for
  // Portrait: a landscape page came out as a 684x1216 file (rotated 90 deg), and
  // PortraitInverted came out mirrored. Verified against logicalToPhysical() for all
  // four orientations -- see the revert-able proof in the commit message.
  const bool logicalPortrait = orientation == GfxRenderer::Orientation::Portrait ||
                               orientation == GfxRenderer::Orientation::PortraitInverted;
  const int phyWidth = logicalPortrait ? height : width;   // BMP width  = logical width
  const int phyHeight = logicalPortrait ? width : height;  // BMP height = logical height

  std::string path(filename);
  size_t last_slash = path.find_last_of('/');
  if (last_slash != std::string::npos) {
    std::string dir = path.substr(0, last_slash);
    if (!Storage.exists(dir.c_str())) {
      if (!Storage.mkdir(dir.c_str())) {
        return false;
      }
    }
  }

  HalFile file;
  if (!Storage.openFileForWrite("SCR", filename, file)) {
    LOG_ERR("SCR", "Failed to save screenshot");
    return false;
  }

  BmpHeader header;

  createBmpHeader(&header, phyWidth, phyHeight, BmpRowOrder::BottomUp);

  bool write_error = false;
  if (file.write(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
    write_error = true;
  }

  if (write_error) {
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filename);
    return false;
  }

  // Row length is derived from the ROTATED width (phyWidth == the display height), so
  // it must never be a constant: kMaxRowSize was hard-coded to 68 -- the row of a
  // 528-px-tall panel -- and every screenshot on this 684-px-tall panel therefore
  // failed with "Row size 88 exceeds buffer capacity" (88 = ceil(684/32)*4).
  const uint32_t rowSizePadded = (phyWidth + 31) / 32 * 4;
  // Buffer bound from the widest row this build can ever emit, i.e. the panel width
  // limit the renderer/framebuffer is compiled against (MAX_WIDTH), not from a magic
  // number. 1216x684 -> phyWidth 684 -> 88 bytes used of 384 available.
#ifndef MAX_WIDTH
#define MAX_WIDTH 3072
#endif
  constexpr size_t kMaxRowSize = static_cast<size_t>((MAX_WIDTH + 31) / 32 * 4);
  if (phyWidth > MAX_WIDTH || rowSizePadded > kMaxRowSize) {
    LOG_ERR("SCR", "Row size %u exceeds buffer capacity %u", rowSizePadded, static_cast<unsigned>(kMaxRowSize));
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filename);
    return false;
  }

  // Rotate on the fly while writing to save memory. BMP rows are bottom-to-top, so
  // outY=0 is the BOTTOM row of the image, i.e. logical row (phyHeight-1-outY).
  uint8_t rowBuffer[kMaxRowSize];
  memset(rowBuffer, 0, rowSizePadded);

  for (int outY = 0; outY < phyHeight; outY++) {
    const int logicalY = phyHeight - 1 - outY;
    for (int outX = 0; outX < phyWidth; outX++) {
      // Logical (outX, logicalY) -> the physical byte the renderer drew it into.
      int srcX = 0;
      int srcY = 0;
      GfxRenderer::logicalToPhysical(orientation, outX, logicalY, static_cast<uint16_t>(width),
                                     static_cast<uint16_t>(height), &srcX, &srcY);
#if defined(CP_RENDER_4BPP) && CP_RENDER_4BPP
      // 4 bpp canvas: two pixels per byte, even x in the low nibble. The BMP is
      // still 1 bpp on purpose (the header says so, and 8 bpp would invent tone
      // the file format here does not carry), so the nibble is thresholded at the
      // half point — a screenshot shows text as if AA were off. Fidelity is the
      // `shot` command's known limitation, not a regression: the old buffer had
      // no grey to lose.
      const int fbIndex = srcY * (width / 2) + (srcX / 2);
      const uint8_t cell = framebuffer[fbIndex];
      const uint8_t level = (srcX & 1) ? (cell >> 4) : (cell & 0x0F);
      const uint8_t pixel = level >= 0x8 ? 1 : 0;
#else
      int fbIndex = srcY * (width / 8) + (srcX / 8);
      uint8_t pixel = (framebuffer[fbIndex] >> (7 - (srcX % 8))) & 0x01;
#endif
      rowBuffer[outX / 8] |= pixel << (7 - (outX % 8));
    }
    if (file.write(rowBuffer, rowSizePadded) != rowSizePadded) {
      write_error = true;
      break;
    }
    memset(rowBuffer, 0, rowSizePadded);  // Clear the buffer for the next row
  }

  // Explicitly close() file before calling Storage.remove()
  file.close();

  if (write_error) {
    Storage.remove(filename);
    return false;
  }

  return true;
}
