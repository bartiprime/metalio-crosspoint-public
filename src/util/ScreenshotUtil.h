#pragma once
#include <GfxRenderer.h>

#include <cstddef>

#include "ScreenshotInfo.h"

class ScreenshotUtil {
 public:
  static void takeScreenshot(GfxRenderer& renderer);
  // `width`/`height` are the PANEL's physical frame dimensions; `orientation` is the
  // orientation the page was drawn in, because `framebuffer` is always the physical
  // frame and the BMP has to be the LOGICAL page.
  static bool saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width, int height,
                                   GfxRenderer::Orientation orientation);

 private:
  static void buildFilename(const ScreenshotInfo& info, char* buf, size_t bufSize);
};
