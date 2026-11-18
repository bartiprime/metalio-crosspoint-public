#include "SleepActivity.h"

#include <BitmapHelpers.h>
#include <Epub.h>
#include <Epub/converters/DitherUtils.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Memory.h>
#include <PNGdec.h>
#include <PngToBmpConverter.h>
#include <Xtc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "CrossPointSettings.h"
#include "display/RefreshPolicy.h"
#include "CrossPointState.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/ImageFileResolver.h"
#include "images/Logo120.h"
#include "images/MoonIcon.h"

// r101.8: the panel shim's drive counter (main.cpp). Weak, so a host/test build without the
// firmware's main.cpp still links -- the deferred-popup guard below then falls back to committing
// the popup frame, which is the old behaviour.
extern "C" uint32_t cp_panel_drive_seq(void) __attribute__((weak));

namespace {
// Metalio: B/W sleep images use the full 0xF7 waveform instead of HALF (two
// 0xFC partials), matching the clean pass before absolute gray below.
// The guard must include the PLUS board: BoardConfig.h defines
// FREEINK_DEVICE_METALIO_EINK4 as 0 and FREEINK_DEVICE_METALIO_EINK4_PLUS as 1 for this
// device, so a bare _EINK4 test silently selected HALF and the sleep/wake transition showed
// fine horizontal striping instead of one uniform black drive.
#if FREEINK_DEVICE_METALIO_EINK4 || FREEINK_DEVICE_METALIO_EINK4_PLUS
constexpr auto kSleepClean = HalDisplay::FULL_REFRESH;
#else
#if CP_RICKY_SLEEP_PAINT
// r82: RickyOS's sleep screen is a whole-panel GC16 on every board (mode 2 by
// the clean-panel rule, RE_RICKYOS_PIPELINE row 5), so with the switch on the
// board shape stops deciding it. See RE_RICKYOS_SLEEP_WAKE.md §1.
constexpr auto kSleepClean = HalDisplay::FULL_REFRESH;
#else
constexpr auto kSleepClean = HalDisplay::HALF_REFRESH;
#endif
#endif

// The absolute gray paint has no base pass. On Metalio the prior screen shows
// through it after the panel sits unpowered, so clean with 0xF7 first.
void cleanBeforeAbsoluteGray(GfxRenderer& renderer) {
#if FREEINK_DEVICE_METALIO_EINK4 || FREEINK_DEVICE_METALIO_EINK4_PLUS
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
#else
  (void)renderer;
#endif
}

HalDisplay::GrayscaleMode sleepGrayscaleMode(const GfxRenderer& renderer) {
  return renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Direct).supported()
             ? HalDisplay::GrayscaleMode::Direct
             : HalDisplay::GrayscaleMode::Absolute;
}

// Kept separate from normal custom sleep sources so alpha-overlay art does not mix with full-screen wallpapers.
constexpr char TRANSPARENT_SLEEP_ROOT_BMP[] = "/sleep-overlay.bmp";
constexpr char TRANSPARENT_SLEEP_ROOT_PNG[] = "/sleep-overlay.png";
constexpr char TRANSPARENT_SLEEP_DIR[] = "/.sleep-overlay";
constexpr char TRANSPARENT_SLEEP_LEGACY_DIR[] = "/sleep-overlay";
constexpr size_t MAX_SLEEP_FILE_NAME_LEN = 256;
constexpr uint8_t MIN_VISIBLE_ALPHA = 8;

struct BitmapPlacement {
  int x = 0;
  int y = 0;
  float cropX = 0.0f;
  float cropY = 0.0f;
};

struct OverlayBmpInfo {
  int width = 0;
  int height = 0;
  bool topDown = false;
  uint32_t dataOffset = 0;
  uint32_t rowBytes = 0;
};

uint16_t readLE16(HalFile& file) {
  const int c0 = file.read();
  const int c1 = file.read();
  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

uint32_t readLE32(HalFile& file) {
  const int c0 = file.read();
  const int c1 = file.read();
  const int c2 = file.read();
  const int c3 = file.read();
  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  const auto b2 = static_cast<uint8_t>(c2 < 0 ? 0 : c2);
  const auto b3 = static_cast<uint8_t>(c3 < 0 ? 0 : c3);
  return static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) | (static_cast<uint32_t>(b2) << 16) |
         (static_cast<uint32_t>(b3) << 24);
}

BitmapPlacement calculateBitmapPlacement(const int bitmapWidth, const int bitmapHeight, const GfxRenderer& renderer) {
  BitmapPlacement placement;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (bitmapWidth > pageWidth || bitmapHeight > pageHeight) {
    float ratio = static_cast<float>(bitmapWidth) / static_cast<float>(bitmapHeight);
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    if (ratio > screenRatio) {
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        placement.cropX = 1.0f - (screenRatio / ratio);
        ratio = (1.0f - placement.cropX) * static_cast<float>(bitmapWidth) / static_cast<float>(bitmapHeight);
      }
      placement.x = 0;
      placement.y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
    } else {
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        placement.cropY = 1.0f - (ratio / screenRatio);
        ratio = static_cast<float>(bitmapWidth) / ((1.0f - placement.cropY) * static_cast<float>(bitmapHeight));
      }
      placement.x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      placement.y = 0;
    }
  } else {
    placement.x = (pageWidth - bitmapWidth) / 2;
    placement.y = (pageHeight - bitmapHeight) / 2;
  }

  return placement;
}

bool parseOverlayBmpHeader(HalFile& file, OverlayBmpInfo& info, const bool logErrors) {
  if (!file) return false;
  if (!file.seek(0)) return false;

  if (readLE16(file) != 0x4D42) {
    if (logErrors) LOG_ERR("SLP", "Transparent overlay is not a BMP");
    return false;
  }

  file.seekCur(8);
  info.dataOffset = readLE32(file);

  const uint32_t dibSize = readLE32(file);
  if (dibSize < 40) {
    if (logErrors) LOG_ERR("SLP", "Unsupported BMP DIB header: %u", static_cast<unsigned>(dibSize));
    return false;
  }

  info.width = static_cast<int32_t>(readLE32(file));
  const auto rawHeight = static_cast<int32_t>(readLE32(file));
  if (rawHeight == std::numeric_limits<int32_t>::min()) {
    if (logErrors) LOG_ERR("SLP", "Bad transparent overlay dimensions: %dx%d", info.width, rawHeight);
    return false;
  }
  info.topDown = rawHeight < 0;
  info.height = info.topDown ? -rawHeight : rawHeight;

  const uint16_t planes = readLE16(file);
  const uint16_t bpp = readLE16(file);
  const uint32_t compression = readLE32(file);

  // Match Bitmap::parseHeaders(): accept BI_RGB (0) and 32bpp BI_BITFIELDS (3), but keep the same
  // byte-layout assumption as custom sleep BMPs. The renderer below treats pixels as BGRA and does not parse masks.
  if (planes != 1 || bpp != 32 || !(compression == 0 || compression == 3)) {
    if (logErrors) {
      LOG_ERR("SLP", "Transparent overlay must be 32-bit BGRA BMP (planes=%u bpp=%u comp=%u)", planes, bpp,
              static_cast<unsigned>(compression));
    }
    return false;
  }

  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  if (info.width <= 0 || info.height <= 0 || info.width > MAX_IMAGE_WIDTH || info.height > MAX_IMAGE_HEIGHT) {
    if (logErrors) LOG_ERR("SLP", "Bad transparent overlay dimensions: %dx%d", info.width, info.height);
    return false;
  }

  info.rowBytes = static_cast<uint32_t>(info.width) * 4u;
  if (!file.seek(info.dataOffset)) {
    if (logErrors) LOG_ERR("SLP", "Failed to seek transparent overlay pixel data");
    return false;
  }

  return true;
}

enum class TransparentOverlayPass : uint8_t { BW, GrayscaleLsb, GrayscaleMsb };

uint8_t quantizeOverlayLum(const uint8_t lum) {
  // Match Bitmap's native-palette path: 0, 85, 170, 255 map directly to levels 0..3.
  return lum >> 6;
}

bool renderTransparentOverlayPass(HalFile& file, const OverlayBmpInfo& info, const BitmapPlacement& placement,
                                  const GfxRenderer& renderer, uint8_t* row, const TransparentOverlayPass pass) {
  if (!file.seek(info.dataOffset)) {
    LOG_ERR("SLP", "Failed to seek transparent overlay pixel data");
    return false;
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int cropPixX = std::floor(info.width * placement.cropX / 2.0f);
  const int cropPixY = std::floor(info.height * placement.cropY / 2.0f);
  const float croppedWidth = (1.0f - placement.cropX) * static_cast<float>(info.width);
  const float croppedHeight = (1.0f - placement.cropY) * static_cast<float>(info.height);

  float scale = 1.0f;
  if (croppedWidth > 0.0f && croppedHeight > 0.0f) {
    const float widthScale = static_cast<float>(pageWidth) / croppedWidth;
    const float heightScale = static_cast<float>(pageHeight) / croppedHeight;
    scale = std::min(widthScale, heightScale);
    if (scale > 1.0f) scale = 1.0f;
  }
  const bool isScaled = scale < 1.0f;

  for (int bmpY = 0; bmpY < info.height; bmpY++) {
    if (file.read(row, info.rowBytes) != static_cast<int>(info.rowBytes)) {
      LOG_ERR("SLP", "Short read in transparent overlay row %d", bmpY);
      return false;
    }

    int screenY = -cropPixY + (info.topDown ? bmpY : info.height - 1 - bmpY);
    if (isScaled) screenY = std::floor(screenY * scale);
    screenY += placement.y;

    if (screenY >= pageHeight) {
      if (info.topDown) break;
      continue;
    }
    if (screenY < 0) {
      if (!info.topDown) break;
      continue;
    }

    for (int bmpX = cropPixX; bmpX < info.width - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) screenX = std::floor(screenX * scale);
      screenX += placement.x;

      if (screenX >= renderer.getScreenWidth()) break;
      if (screenX < 0) continue;

      const uint8_t* pixel = row + (static_cast<size_t>(bmpX) * 4u);
      const uint8_t alpha = pixel[3];
      if (alpha < MIN_VISIBLE_ALPHA || alpha <= bayer4x4Threshold(screenX, screenY)) continue;

      const uint8_t lum = (77u * pixel[2] + 150u * pixel[1] + 29u * pixel[0]) >> 8;
      const uint8_t level = quantizeOverlayLum(lum);

      switch (pass) {
        case TransparentOverlayPass::BW:
          // Same first pass as custom bitmap sleep: all non-white levels are painted black.
          // Transparent overlay's only difference is that opaque white explicitly erases underlying text.
          renderer.drawPixel(screenX, screenY, level < 3);
          break;
        case TransparentOverlayPass::GrayscaleLsb:
        case TransparentOverlayPass::GrayscaleMsb: {
          const auto planePixel =
              grayPlanePixel(level, pass == TransparentOverlayPass::GrayscaleMsb, renderer.grayPlanesAreAbsolute());
          if (planePixel.write) renderer.drawPixel(screenX, screenY, planePixel.black);
          break;
        }
      }
    }
  }

  return true;
}

// Starts a gray plane under an overlay. Absolute planes keep the B/W base; other gray planes carry only
// the overlay's gray. Cover + Transparent redraws a gray cover (set only for absolute planes) to keep its gray levels.
bool beginOverlayGrayPlane(GfxRenderer& renderer, const Bitmap* grayCover, const GfxRenderer::RenderMode plane,
                           const bool absolute) {
  if (grayCover) {
    if (grayCover->rewindToData() != BmpReaderError::Ok) return false;
    renderer.clearScreen();
  } else if (!absolute) {
    renderer.clearScreen(0x00);
  }
  renderer.setRenderMode(plane);
  if (!grayCover) return true;
  const auto placement = calculateBitmapPlacement(grayCover->getWidth(), grayCover->getHeight(), renderer);
  return renderer.drawBitmap(*grayCover, placement.x, placement.y, renderer.getScreenWidth(),
                             renderer.getScreenHeight(), placement.cropX, placement.cropY);
}

enum class AlphaOverlayResult : uint8_t { Rendered, NotAlphaOverlay, Error };
enum class AlphaScanResult : uint8_t { Useful, NotUseful, Error };

AlphaScanResult scanForUsefulAlpha(HalFile& file, const OverlayBmpInfo& info, uint8_t* row) {
  if (!file.seek(info.dataOffset)) {
    LOG_ERR("SLP", "Failed to seek transparent overlay pixel data");
    return AlphaScanResult::Error;
  }

  bool hasVisiblePixel = false;
  bool hasNonOpaquePixel = false;
  for (int bmpY = 0; bmpY < info.height; bmpY++) {
    if (file.read(row, info.rowBytes) != static_cast<int>(info.rowBytes)) {
      LOG_ERR("SLP", "Short read while checking transparent overlay row %d", bmpY);
      return AlphaScanResult::Error;
    }

    for (int bmpX = 0; bmpX < info.width; bmpX++) {
      const uint8_t alpha = row[static_cast<size_t>(bmpX) * 4u + 3u];
      hasVisiblePixel |= alpha >= MIN_VISIBLE_ALPHA;
      hasNonOpaquePixel |= alpha < 255;
      if (hasVisiblePixel && hasNonOpaquePixel) return AlphaScanResult::Useful;
    }
  }

  return AlphaScanResult::NotUseful;
}

AlphaOverlayResult tryRenderTransparentOverlayBmp(HalFile& file, GfxRenderer& renderer, const char* pathForLog,
                                                  const Bitmap* grayCover, const bool ownDrive) {
  OverlayBmpInfo info;
  if (!parseOverlayBmpHeader(file, info, false)) return AlphaOverlayResult::NotAlphaOverlay;

  const auto placement = calculateBitmapPlacement(info.width, info.height, renderer);
  auto row = makeUniqueNoThrow<uint8_t[]>(info.rowBytes);
  if (!row) {
    LOG_ERR("SLP", "OOM: transparent overlay row (%u bytes)", static_cast<unsigned>(info.rowBytes));
    return AlphaOverlayResult::Error;
  }

  const auto alphaScanResult = scanForUsefulAlpha(file, info, row.get());
  if (alphaScanResult == AlphaScanResult::Error) return AlphaOverlayResult::Error;
  if (alphaScanResult == AlphaScanResult::NotUseful) return AlphaOverlayResult::NotAlphaOverlay;

  LOG_DBG("SLP", "Rendering transparent overlay: %s (%dx%d)", pathForLog, info.width, info.height);

  if (!renderTransparentOverlayPass(file, info, placement, renderer, row.get(), TransparentOverlayPass::BW))
    return AlphaOverlayResult::Error;
  const bool grayCapable = renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported();
  if (ownDrive || !grayCapable) {
    // No grayscale on this panel: the BW pass above IS the sleep image. Driving
    // a HALF gray base and then building LSB/MSB planes from clearScreen(0x00)
    // is what turned a custom overlay into a near-black screen -- the MSB plane
    // was all black and displayGrayBuffer() DU-drives it. Drive the BW frame
    // with the same single clean refresh the default sleep screen uses.
    //
    // r97 C2: with ownDrive the panel DOES have grayscale; the single drive is
    // still what we want, because the operator's complaint is the ghost of the
    // page underneath, and a gray pass is a SECOND drive over the same content.
    renderer.cancelPromotedRefresh();
    renderer.forceCleanNextRefresh();
    renderer.displayBuffer(kSleepClean);
    renderer.setRenderMode(GfxRenderer::BW);
    if (ownDrive) {
      LOG_INF("SLP", "sleep overlay paint: own from-white clean drive (mode=%d)", static_cast<int>(kSleepClean));
    }
    return AlphaOverlayResult::Rendered;
  }
  cleanBeforeAbsoluteGray(renderer);
  const bool absolute = grayCapable;
  if (!renderer.displayGrayscaleBase(sleepGrayscaleMode(renderer))) return AlphaOverlayResult::Error;

  // Absolute planes retain B/W background bits; each visible overlay pixel is rewritten in both passes.
  if (!beginOverlayGrayPlane(renderer, grayCover, GfxRenderer::GRAYSCALE_LSB, absolute) ||
      !renderTransparentOverlayPass(file, info, placement, renderer, row.get(), TransparentOverlayPass::GrayscaleLsb)) {
    renderer.setRenderMode(GfxRenderer::BW);
    // Keep the current display instead of trying another overlay with a
    // framebuffer that now contains an incomplete gray plane.
    return AlphaOverlayResult::Rendered;
  }
  renderer.copyGrayscaleLsbBuffers();

  if (!beginOverlayGrayPlane(renderer, grayCover, GfxRenderer::GRAYSCALE_MSB, absolute) ||
      !renderTransparentOverlayPass(file, info, placement, renderer, row.get(), TransparentOverlayPass::GrayscaleMsb)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return AlphaOverlayResult::Rendered;
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  return AlphaOverlayResult::Rendered;
}

// SLEEP1 -> POSCR1: the cache dir, fnv1a32/hashFileHead, cachePathFor, pruneCache,
// convertToBmp, resolveBmpPath and the PNG/JPEG header checks now live in
// src/images/ImageFileResolver.cpp, shared with the power-off screen. The sleep spec
// (imagefile::kSleepImageCache) carries this file's OWN directory, tag, cache-name
// prefix ('s') and message wording, so cache entries and log lines are byte-identical
// to what SLEEP1 shipped. Selection and painting stay here: selectRandomSleepFile()'s
// recent-window policy and renderBitmapSleepScreen() are untouched.

enum class SleepRecentKind : uint8_t { Standard, Overlay };

// SLEEP1: which formats a counting pass may see. BmpOnly is the first pass over the
// standard wallpaper dirs: BMP is the only format that draws with no convert and no
// cache write, so while any valid BMP exists the PNG/JPEG set is never entered.
enum class SleepImageFilter : uint8_t { BmpOnly, AnySupported };

bool isRecentSleepIndex(const SleepRecentKind recentKind, const uint16_t idx, const uint8_t window) {
  return recentKind == SleepRecentKind::Overlay ? APP_STATE.isRecentOverlaySleep(idx, window)
                                                : APP_STATE.isRecentSleep(idx, window);
}

void pushRecentSleepIndex(const SleepRecentKind recentKind, const uint16_t idx) {
  if (recentKind == SleepRecentKind::Overlay) {
    APP_STATE.pushRecentOverlaySleep(idx);
  } else {
    APP_STATE.pushRecentSleep(idx);
  }
}

bool findNextValidSleepImage(HalFile& dir, const SleepRecentKind recentKind, const SleepImageFilter filter, char* name) {
  for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
    if (dirFile.isDirectory()) continue;

    dirFile.getName(name, MAX_SLEEP_FILE_NAME_LEN);
    if (name[0] == '\0' || name[0] == '.') continue;

    const bool isBmp = FsHelpers::hasBmpExtension(name);
    // The overlay dirs keep their old contract: BMP plus PNG alpha overlays, no JPEG,
    // no conversion. The standard dirs take PNG/JPEG only in the mixed pass, i.e. only
    // when the BMP pass found nothing.
    const bool allowConverted = filter == SleepImageFilter::AnySupported;
    const bool isPng = allowConverted && FsHelpers::hasPngExtension(std::string_view{name});
    const bool isJpg = allowConverted && recentKind == SleepRecentKind::Standard && FsHelpers::hasJpgExtension(std::string_view{name});
    if (!isBmp && !isPng && !isJpg) {
      // A BMP pass deliberately ignores PNG/JPEG without a skip line: the mixed pass
      // that follows will see them, and one "Skipping unsupported" per phone photo per
      // sleep was exactly the noise SLEEP1 removes. r101.11 SLP2: the condition is just
      // `allowConverted` -- this block is only reached when isBmp is FALSE, so the old
      // `isBmp || allowConverted` term was dead and read as if it inverted the rule.
      // Output is unchanged: only the mixed pass logs a reject.
      if (allowConverted) LOG_DBG("SLP", "Skipping unsupported sleep image: %s", name);
      continue;
    }

    // POSCR1: the header checks (BMP parse, PNG IHDR, JPEG SOFn walk) are shared with
    // the power-off screen -- same three tests SLEEP1 shipped, now in one place.
    const bool isValid =
        imagefile::isValidImageHeader(dirFile, isBmp ? imagefile::Format::Bmp : (isPng ? imagefile::Format::Png
                                                                                          : imagefile::Format::Jpeg),
                                      "SLP");
    if (!isValid) {
      LOG_DBG("SLP", "Skipping invalid sleep image: %s", name);
      continue;
    }
    return true;
  }
  return false;
}

bool selectRandomSleepFile(const char* firstDirPath, const char* secondDirPath, const SleepRecentKind recentKind,
                           std::string& selectedPath) {
  auto name = makeUniqueNoThrow<char[]>(MAX_SLEEP_FILE_NAME_LEN);
  if (!name) {
    LOG_ERR("SLP", "OOM: sleep filename buffer");
    return false;
  }

  constexpr size_t DIRECTORY_COUNT = 2;
  const char* dirPaths[DIRECTORY_COUNT] = {firstDirPath, secondDirPath};
  uint16_t directoryCounts[DIRECTORY_COUNT] = {};
  uint16_t fileCount = 0;

  // One pass over both dirs with the given filter, filling directoryCounts.
  const auto countImages = [&](const SleepImageFilter filter) {
    uint16_t total = 0;
    for (size_t i = 0; i < DIRECTORY_COUNT; i++) {
      directoryCounts[i] = 0;
      auto dir = Storage.open(dirPaths[i]);
      if (!dir || !dir.isDirectory()) continue;

      const auto remainingCapacity = static_cast<uint16_t>(UINT16_MAX - total);
      while (directoryCounts[i] < remainingCapacity && findNextValidSleepImage(dir, recentKind, filter, name.get())) {
        directoryCounts[i]++;
      }
      total = static_cast<uint16_t>(total + directoryCounts[i]);
    }
    return total;
  };

  // BMP-first: the standard wallpaper dirs are counted twice at worst -- once for BMPs
  // only, and only if that finds nothing, once for any supported format. Neither the
  // index space nor the recent-window bookkeeping can then mix formats within a pick.
  SleepImageFilter filter = SleepImageFilter::AnySupported;
  if (recentKind == SleepRecentKind::Standard) {
    fileCount = countImages(SleepImageFilter::BmpOnly);
    if (fileCount > 0) {
      filter = SleepImageFilter::BmpOnly;
      LOG_DBG("SLP", "sleep image set: %u BMP(s); BMP preferred", static_cast<unsigned>(fileCount));
    }
  }
  if (fileCount == 0) {
    filter = SleepImageFilter::AnySupported;
    fileCount = countImages(filter);
    if (fileCount > 0) LOG_INF("SLP", "sleep image set: %u image(s), mixed pass (no BMP present)", static_cast<unsigned>(fileCount));
  }

  if (fileCount == 0) return false;

  // Pick a random wallpaper, excluding recently shown ones.
  // Window: up to SLEEP_RECENT_COUNT entries, capped at fileCount-1.
  const uint8_t recentFill =
      recentKind == SleepRecentKind::Overlay ? APP_STATE.recentOverlaySleepFill : APP_STATE.recentSleepFill;
  const uint8_t window = static_cast<uint8_t>(std::min<uint16_t>(recentFill, fileCount - 1));
  auto randomFileIndex = static_cast<uint16_t>(random(fileCount));
  for (uint8_t attempt = 0; attempt < 20 && isRecentSleepIndex(recentKind, randomFileIndex, window); attempt++) {
    randomFileIndex = static_cast<uint16_t>(random(fileCount));
  }

  size_t directoryIndex = 0;
  uint16_t localIndex = randomFileIndex;
  if (randomFileIndex >= directoryCounts[0]) {
    directoryIndex = 1;
    localIndex = static_cast<uint16_t>(randomFileIndex - directoryCounts[0]);
  }

  auto dir = Storage.open(dirPaths[directoryIndex]);
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("SLP", "Failed to open selected sleep image directory: %s", dirPaths[directoryIndex]);
    return false;
  }
  for (uint16_t index = 0; index <= localIndex; ++index) {
    if (!findNextValidSleepImage(dir, recentKind, filter, name.get())) return false;
  }

  const char* selectedDirPath = dirPaths[directoryIndex];
  selectedPath.reserve(strlen(selectedDirPath) + 1 + strlen(name.get()));
  selectedPath = selectedDirPath;
  selectedPath += "/";
  selectedPath += name.get();
  pushRecentSleepIndex(recentKind, randomFileIndex);
  APP_STATE.saveToFile();
  return true;
}

bool drawSleepPopupPreservingFrame(GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int frameThickness = metrics.popupFrameThickness;
  const int popupY = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const int popupHeight = renderer.getLineHeight(UI_12_FONT_ID) + metrics.popupMarginY * 2;
  const int bandTop = std::max(0, popupY - frameThickness);
  const int bandBottom = std::min(renderer.getScreenHeight(), popupY + popupHeight + frameThickness);
  const int bandHeight = bandBottom - bandTop;
  const size_t bandBytes = renderer.getRegionByteSize(0, bandTop, renderer.getScreenWidth(), bandHeight);

  auto savedBand = makeUniqueNoThrow<uint8_t[]>(bandBytes);
  if (!savedBand) {
    LOG_ERR("SLP", "OOM: sleep popup background (%u bytes)", static_cast<unsigned>(bandBytes));
    return false;
  }
  if (!renderer.copyRegionToBuffer(0, bandTop, renderer.getScreenWidth(), bandHeight, savedBand.get(), bandBytes)) {
    LOG_ERR("SLP", "Failed to save sleep popup background");
    return false;
  }

  GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  if (!renderer.copyBufferToRegion(0, bandTop, renderer.getScreenWidth(), bandHeight, savedBand.get(), bandBytes)) {
    LOG_ERR("SLP", "Failed to restore sleep popup background");
    return false;
  }
  return true;
}

void releaseSdFontCachesForDecode(const GfxRenderer& renderer) {
  if (auto* fcm = renderer.getFontCacheManager()) {
    LOG_DBG("SLP", "Free heap before SD font cache release: %d bytes", ESP.getFreeHeap());
    fcm->releaseSdFontCaches();
    LOG_DBG("SLP", "Free heap before sleep image decode: %d bytes", ESP.getFreeHeap());
  }
}

// r101.8: THE "ENTERING SLEEP" POPUP IS NOT A FRAME ON GLASS.
// BaseTheme::drawPopup() commits its own HALF drive, which is right for a popup over a screen that
// STAYS. On every full-screen sleep path that drive is dead: the sleep frame (bitmap, cover, logo,
// blank) is composed from a cleared canvas and driven from white a moment later, wiping whatever the
// popup put down. The operator's `benchdrives` sleep step measured exactly that pair --
// modes=GC16,GC16-from-white ms=630+1174 -- i.e. one redundant whole-panel drive per sleep.
//
// So the popup is drawn with commit=false (no drive) and the popup frame is committed only if
// nothing else reached the glass. The enter-paint debt this activity owes is normally paid by the
// sleep frame itself, but a path that paints nothing (a failed parse/decode that returns early) must
// not leave the press with no visible result at all: that is what this guard pays, in the old mode.
// The probe is the panel's OWN drive counter, so it cannot be fooled by a paint that was held,
// declined or demoted -- it asks whether a drive actually happened.
class DeferredPopupDrive {
 public:
  explicit DeferredPopupDrive(const GfxRenderer& r) : renderer(r), seqBefore(cp_panel_drive_seq ? cp_panel_drive_seq() : 0) {}
  ~DeferredPopupDrive() {
    if (cp_panel_drive_seq != nullptr && cp_panel_drive_seq() != seqBefore) return;  // the sleep frame drove
    LOG_INF("SLP", "sleep frame drove nothing; committing the popup frame");
    // QA2 (r101.9): NOT equalizeBlackMode(). This frame can be the LAST frame of the session -- the
    // path that paints nothing is the path that leaves the popup on glass until wake, so there is no
    // later repaint to pay a debt. EQUALIZE is outside the panel's from-white promotion
    // (metalio_vendor_panel.cpp: gc16Class = FULL || HALF), so a >= 5-optical-level frame would go
    // to the glass as a diff-area drive instead of epd_hl_update_screen_from_white. EQ1's scope is
    // "popups over a screen that stays"; this frame does not stay, it is held for hours. Plain HALF
    // (the pre-EQ1 mode) keeps the from-white promotion that scrubs the old image.
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
  DeferredPopupDrive(const DeferredPopupDrive&) = delete;
  DeferredPopupDrive& operator=(const DeferredPopupDrive&) = delete;

 private:
  const GfxRenderer& renderer;
  const uint32_t seqBefore;
};

// r101.10 SLP1: DRAW AND DRIVE THE POPUP *BEFORE* THE SLEEP FRAME IS DECODED.
//
// r101.8 stopped the popup committing its own drive (commit=false) because the sleep frame replaces
// the whole canvas from white a moment later, so that HALF was a drive nobody ever saw. Dropping the
// drive also dropped the only thing the operator actually reported: for the seconds a JPEG/PNG sleep
// image spends decoding, the glass showed nothing new. The repair is to MOVE the drive, not remove
// it: pulse the box onto the glass first, let the decode run behind it, and let the sleep frame
// replace it. Cost: exactly one extra HALF drive on sleep entry.
//
// The box is deliberately NOT composed into the sleep frame: the frame is held for hours, so the
// popup would freeze "Entering sleep" over the artwork until wake.
//
// Plain HALF_REFRESH, not equalizeBlackMode(): this frame is replaced whole moments later, so EQ1's
// page-wide re-ink (a 48-frame GC16) would buy nothing. The armed one-shot promotion is parked around
// the drive for the same reason BaseTheme::drawPopup parks it -- the sleep frame below still needs its
// own FULL, and a bare displayBuffer() would consume it.
void drawAndDriveSleepPopup(GfxRenderer& renderer) {
  const bool fromReader = APP_STATE.lastSleepFromReader;
  if (fromReader) ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP), false);
  if (fromReader) renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const auto parked = renderer.peekPromotedRefresh();
  renderer.cancelPromotedRefresh();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  renderer.rearmPromotedRefresh(parked);
  LOG_INF("SLP", "sleep popup DRIVEN before the sleep frame (HALF_REFRESH) from_reader=%d", fromReader ? 1 : 0);
}

}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();

  // Only an explicit QUICK_RESUME picks the moon/quick-resume frame. The idle timeout deliberately
  // shares the manual sleep screen: a sleep nobody asked for must not look different from one they
  // did. fromTimeout no longer selects a frame (it still drives the journal/plugin deferral in
  // enterDeepSleep()).
  const bool renderQuickResume = SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME;

  if (renderQuickResume) {
    // Quick Resume keeps the current frame as-is, so the driver's inversion
    // state stays too: a night-mode page sleeps in night polarity, and the
    // moon icon inverts with it at transfer like any other draw.
    return renderLastScreenSleepScreen();
  }

  const bool frameWasInverted = display.isInverted();

  // The remaining sleep screens draw fresh content in normal polarity. This
  // activity draws directly from onEnter (outside ActivityManager's
  // per-render polarity resolution), so clear any inversion left over from a
  // night-mode reader render.
  display.setInverted(false);

  const uint8_t sleepScreen = SETTINGS.sleepScreen;
  // r54 diagnostic: pin down which sleep path paints, and with what capability. A previous log showed
  // a HALF drive over a ~195-line region followed by a whole-panel FULL, and the source has no HALF
  // caller on the default path - so this line plus the [SLP]/[GFX]/[mask] interleave identifies it.
  LOG_INF("SLP", "sleep path: mode=%d absolute_gray_capable=%d inverted=%d from_reader=%d",
          static_cast<int>(sleepScreen),
          static_cast<int>(renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported()),
          static_cast<int>(display.isInverted()), static_cast<int>(APP_STATE.lastSleepFromReader));
  if (sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM ||
      sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_TRANSPARENT) {
    // Transparent mode retains the current framebuffer. Materialize any
    // output-level inversion first so the retained content keeps its visible
    // polarity after the display driver returns to normal.
    if (frameWasInverted) renderer.invertScreen();
    if (APP_STATE.lastSleepFromReader) {
      ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    }
    drawSleepPopupPreservingFrame(renderer);
    if (APP_STATE.lastSleepFromReader) {
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
    }
    releaseSdFontCachesForDecode(renderer);
    if (sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_TRANSPARENT) {
      return renderCoverSleepScreen();
    }
    // ---------------------------------------------------------------------
    // r97 C2: the custom overlay is its OWN image, not a differential edit of
    // whatever was on the glass. Operationally it used to composite onto the
    // live page (transparent pixels kept the old ink) and then drive that
    // composite -- so the previous screen was still underneath it and its
    // residue ghosted through the overlay's white areas. Clearing to WHITE here
    // gives the overlay a clean baseline, and driveSleepOverlayFromWhite()
    // then gives it exactly one full drive. Nothing else goes over this frame.
    // ---------------------------------------------------------------------
    renderer.clearScreen();
    overlayOwnDrive = true;
    renderTransparentCustomSleepScreen();
    overlayOwnDrive = false;
    return;
  }

  // Show popup with reader orientation only when going to sleep from reader
  //
  // r101.8: the popup is drawn WITHOUT its own drive. Every path below replaces the whole frame
  // from a cleared canvas, so the popup's HALF drive was a whole-panel GC16 paid for a frame
  // nothing ever sees (see DeferredPopupDrive); the guard commits it only if this activity ends up
  // painting nothing, which keeps the enter-paint debt honest. The retained-frame paths above
  // (transparent custom / cover+transparent) still commit their popup: there the popup sits on the
  // frame that STAYS on glass.
  // r101.10 SLP1: the popup is DRIVEN here, before the frame below is decoded, so the operator sees
  // "Entering sleep" for the decode interval (see drawAndDriveSleepPopup). The sleep frame then
  // replaces it whole. The DeferredPopupDrive guard stays as the no-frame fallback: it is armed
  // BEFORE this drive, so a popup drive that was held/declined and a frame that painted nothing still
  // commits the popup frame rather than leaving the press with no visible result.
  DeferredPopupDrive popupDrive(renderer);
  drawAndDriveSleepPopup(renderer);

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        return renderCoverSleepScreen();
      } else {
        return renderCustomSleepScreen();
      }
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  // This takes priority over both custom sleep directories.
  HalFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true,
                  renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported() &&
                      display.getController() == HalDisplay::Controller::SSD1677 &&
                      SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap);
      file.close();
      return;
    }
    file.close();
  }

  std::string selectedPath;
  selectRandomSleepFile("/.sleep", "/sleep", SleepRecentKind::Standard, selectedPath);

  if (!selectedPath.empty()) {
    LOG_DBG("SLP", "Randomly loading: %s", selectedPath.c_str());
    delay(100);

    // BMP goes straight in; PNG/JPEG is converted once into the sleep cache and the
    // cached BMP goes in. Either way what is opened below is a BMP and the paint is
    // renderBitmapSleepScreen() -- there is no second sleep renderer.
    if (!FsHelpers::hasBmpExtension(std::string_view{selectedPath})) {
      // The decoder needs heap for a full frame; the SD font caches are the largest
      // thing this activity can hand back, and the custom screen draws no text. The
      // default-screen fallback reloads whatever it needs.
      releaseSdFontCachesForDecode(renderer);
    }
    std::string bmpPath;
    // The crop flag is still SLEEP1's own: the shared resolver only decides *how* the
    // cache key and the conversion are formed, never *what* the sleep screen wants.
    const bool croppedForCover = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
    if (imagefile::resolveBmpPath(imagefile::kSleepImageCache, selectedPath, renderer.getScreenWidth(),
                                  renderer.getScreenHeight(), croppedForCover, bmpPath)) {
      HalFile randFile;
      if (Storage.openFileForRead("SLP", bmpPath, randFile)) {
        Bitmap bitmap(randFile, true,
                      renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported() &&
                          display.getController() == HalDisplay::Controller::SSD1677 &&
                          SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER);
        const auto parseResult = bitmap.parseHeaders();
        if (parseResult == BmpReaderError::Ok) {
          if (bmpPath != selectedPath) {
            LOG_INF("SLP", "Rendering sleep image %s (converted from %s)", bmpPath.c_str(), selectedPath.c_str());
          }
          renderBitmapSleepScreen(bitmap);
          randFile.close();
          return;
        }
        LOG_ERR("SLP", "Sleep image BMP invalid (%s): %s; using default sleep screen", bmpPath.c_str(),
                Bitmap::errorToString(parseResult));
        randFile.close();
      } else {
        LOG_ERR("SLP", "Sleep image BMP did not open: %s; using default sleep screen", bmpPath.c_str());
      }
    }
  }

  renderDefaultSleepScreen();
}

// Sleep screens paint with a single HALF refresh (stock parity): the OEM X4
// firmware's only clean refresh in normal operation is the single-pass 0xD7
// sequence, used once for the sleep image. It never runs the multi-flash GC
// waveform (0xF7) that FULL_REFRESH selects (#2471's blinking complaint).
void SleepActivity::renderDefaultSleepScreen() const {
  LOG_INF("SLP", "paint site: default screen (inv=%d mode=%d)", static_cast<int>(display.isInverted()),
          static_cast<int>(SETTINGS.sleepScreen));
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  // r163 D3: brand art -- `Illustration` (visible content; 1 bpp today, so it carries no
  // level and the tag changes nothing about the drive).
  const GfxRenderer::ScopedContentRole logoRole(renderer, GfxRenderer::ContentRole::Illustration);
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  // A wholesale screen swap cannot be a diff-area drive, and the sleep screen is the last thing
  // on glass before standby: any armed one-shot promotion would downgrade it to a diff of the old
  // page, so cancel it and drive the whole panel.
  //
  // The waveform is kSleepClean, not a literal FULL: on this board the two are the same value
  // (0xF7), and every other sleep paint in this file already goes through it. Spelling FULL out
  // here overrode the per-board choice for the X4/X3/etc. boards, whose sleep screen is
  // deliberately a single HALF pass -- see the function comment above and #2471.
  // r96: nothing may downgrade the sleep screen to a diff-area drive.
  renderer.cancelPromotedRefresh();
  renderer.forceCleanNextRefresh();
  renderer.displayBuffer(kSleepClean);
}

namespace {
// GRAY16: the one-paint opt-in for 16-level bitmap drawing (GfxRenderer::
// setSixteenLevelImages). Scope-local rather than a member so EVERY exit from the
// sleep paint -- the bitmap, the failed-bitmap fallback, the overlay recursion --
// restores the flag, and so no later paint can inherit a 16-level draw by
// accident.
//
// THE DRIVE THIS IMPLIES, and why it is a CLEARING one (r101.2). A 16-level
// frame needs a 16-level waveform, and the panel has exactly two, both 48
// phases (~800 ms): GC16 (type 2, my_gc_25_0) and GL16 (type 5, my_gl_25_0).
// They differ in CONTENT, not length: the vendor GC16 table drives 15->15 as a
// full dark+light oscillation (18 dark / 17 light phases, counted from
// my_waveform_gc.h), i.e. a white target over dirty glass is conditioned back to
// white; my_gl_25_0 holds 15->15 at zero and only gets the single light tick
// gl16_white_tick() adds, so everything the previous screen left behind a white
// pixel STAYS. Driving 16-level content with GL16 is therefore a light diff over
// a panel that was never cleaned -- the ghosting the operator reported on the
// sleep screen and on image pages.
//
// The sleep screen's answer is the from-white GC16 it already wants: FULL ==
// METALIO_PANEL_REFRESH_FULL == epd_hl_update_screen_from_white(MODE_GC16),
// which resets the software baseline to white so EVERY pixel is driven, the
// 15->15 conditioning included. That is the same single drive as kSleepClean on
// this board (~800 ms, no second pass) and it must never be downgraded: the
// 16-level paint is a whole-frame replacement, and r96's rule that nothing may
// turn a sleep paint into a diff-area drive applies to it too.
struct Gray16SleepPaint {
  const GfxRenderer& renderer;
  const bool previous;
  const bool engaged;
  explicit Gray16SleepPaint(const GfxRenderer& r)
      : renderer(r),
        previous(r.sixteenLevelImages()),
        engaged(pixfmt::kGray16Sleep && GfxRenderer::fourBitCanvas()) {
    renderer.setSixteenLevelImages(engaged);
  }
  ~Gray16SleepPaint() { renderer.setSixteenLevelImages(previous); }
  // True when this paint actually wrote 16 levels, i.e. the drive below must be
  // the clearing from-white GC16 rather than the board's ordinary sleep mode.
  bool drivesSixteen() const { return engaged; }
  Gray16SleepPaint(const Gray16SleepPaint&) = delete;
  Gray16SleepPaint& operator=(const Gray16SleepPaint&) = delete;
};

// The one place the sleep screen decides its drive. 16-level content escalates
// to FULL (from-white GC16) because that is the only 16-level mode that
// conditions the pixels it is about to leave white; every other case keeps the
// board's kSleepClean. forceCleanNextRefresh()/cancelPromotedRefresh() stay at
// the call sites: this only names the mode, it never overrides the clean.
inline HalDisplay::RefreshMode sleepDriveMode(const bool sixteen) {
  return sixteen ? HalDisplay::FULL_REFRESH : kSleepClean;
}
}  // namespace

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const bool preserveBackground) const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto placement = calculateBitmapPlacement(bitmap.getWidth(), bitmap.getHeight(), renderer);
  const int x = placement.x;
  const int y = placement.y;
  const float cropX = placement.cropX;
  const float cropY = placement.cropY;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  if (!preserveBackground || overlayOwnDrive) renderer.clearScreen();

  // Gate the grayscale pipeline on what the PANEL can actually do, not on the
  // file: Bitmap::hasGreyscale() is only "bpp > 1", so a CUSTOM bpp>1 sleep BMP
  // used to enter the gray branch on a panel with no gray support, build LSB/MSB
  // planes from clearScreen(0x00) -- BLACK on this board -- and drive the MSB
  // plane with a DU, painting the image and then going near-black. With the
  // capability in the decision such a file falls through to the single
  // kSleepClean FULL the default screen gets, and keeps its picture.
  const bool grayCapable = renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported();
  // r97 C2: an overlay that owns its drive is B/W on purpose -- a gray pass would
  // be a second drive over the same content.
  const bool hasGreyscale = !overlayOwnDrive && grayCapable &&
                            (bitmap.hasGreyscale() || (preserveBackground && overlayGrayCover)) &&
                            (preserveBackground || SETTINGS.sleepScreenCoverFilter ==
                                                       CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER);

  // The flag must cover the draw and nothing else: the drive decision below is
  // made from the canvas, and every non-sleep consumer of drawBitmap keeps 2-bit.
  Gray16SleepPaint gray16Paint(renderer);
  // r163 D3: the sleep-screen cover is visible content -- tagged `Cover`, so the grey
  // admission answers "yes" for it on every path (today's answer already was yes on an
  // AA paint; this makes it the ROLE's answer, not the paint's).
  const GfxRenderer::ScopedContentRole coverRole(renderer, GfxRenderer::ContentRole::Cover);
  if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, preserveBackground)) {
    renderer.cancelPromotedRefresh();  // see the clean drive at the end of this function
    // r96: nothing may downgrade the sleep screen to a diff-area drive.
    renderer.cancelPromotedRefresh();
    renderer.forceCleanNextRefresh();
    renderer.displayBuffer(sleepDriveMode(gray16Paint.drivesSixteen()));
    return;
  }

  if (!preserveBackground &&
      SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  // Cover + Transparent: the B/W cover is the overlay's background; the overlay renderers redraw its gray planes.
  if (overlayCover == &bitmap) {
    if (hasGreyscale && renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported()) {
      overlayGrayCover = &bitmap;
    }
    renderTransparentCustomSleepScreen();
    overlayGrayCover = nullptr;
    return;
  }

  const bool absolute = hasGreyscale && renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported();
  if (absolute) {
    cleanBeforeAbsoluteGray(renderer);
    if (!renderer.displayGrayscaleBase(sleepGrayscaleMode(renderer))) return;
  } else if (hasGreyscale) {
    // OEM grayscale pipeline base. Must stay HALF: the gray nudge LUT is
    // calibrated against the pixel state the single-pass HALF waveform leaves
    // behind. A FULL (GC) base parks pixels in a different charge state and
    // the differential nudge then lands unevenly (blotchy noise in gray areas).
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  } else {
    // r88: the custom-BMP path is the sleep screen, i.e. the last thing on glass
    // before standby, so it needs the same clean drive the default screen gets
    // (renderDefaultSleepScreen drives kSleepClean behind cancelPromotedRefresh).
    // Without the cancel here an armed one-shot promotion -- the activity
    // transition policy of the activity that went to sleep, an AUTO from the last
    // list paint -- replaced kSleepClean with FAST/HALF in the producer
    // (GfxRenderer.cpp:1808), the BMP went down as a diff-area DU with no clean
    // at all, and the page underneath ghosted through it. Cancel first, then
    // drive, exactly like the default screen.
    if (overlayOwnDrive) {
      driveSleepOverlayFromWhite();
    } else {
      // r96: nothing may downgrade the sleep screen to a diff-area drive.
      // r101.2: and the drive a 16-level frame needs is FULL (from-white GC16),
      // not the board's kSleepClean, which is HALF on the X4/X3 families: HALF is
      // a diff-area GC16 that skips same-colour pixels, so it cannot condition the
      // white a 16-level image leaves behind (GC16's 15->15 oscillation is only in
      // the table, and only the from-white path drives every pixel). The forced
      // clean stays armed: FULL is exactly what it asks for, so this cannot
      // downgrade it -- it is the same single drive, at the same 48 phases.
      renderer.cancelPromotedRefresh();
      renderer.forceCleanNextRefresh();
      const HalDisplay::RefreshMode sleepDrive = sleepDriveMode(gray16Paint.drivesSixteen());
      renderer.displayBuffer(sleepDrive);
      LOG_INF("SLP", "sleep bitmap drive mode=%d sixteen=%d levels=%d", static_cast<int>(sleepDrive),
              gray16Paint.drivesSixteen() ? 1 : 0, gray16Paint.drivesSixteen() ? 16 : 4);
    }
  }

  if (hasGreyscale) {
    bool ready = true;
    for (const auto plane : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      if (bitmap.rewindToData() != BmpReaderError::Ok) {
        ready = false;
        break;
      }
      if (preserveBackground) {
        if (!beginOverlayGrayPlane(renderer, overlayGrayCover, plane, absolute)) {
          ready = false;
          break;
        }
      } else {
        renderer.clearScreen(absolute ? 0xFF : 0x00);
        renderer.setRenderMode(plane);
      }
      if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, preserveBackground)) {
        ready = false;
        break;
      }
      if (plane == GfxRenderer::GRAYSCALE_LSB)
        renderer.copyGrayscaleLsbBuffers();
      else
        renderer.copyGrayscaleMsbBuffers();
    }
    if (ready)
      renderer.displayGrayBuffer();
    else
      LOG_ERR("SLP", "Incomplete grayscale image; keeping the current display");
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

bool SleepActivity::renderSleepOverlayFile(HalFile& file, const char* pathForLog) const {
  const auto alphaResult = tryRenderTransparentOverlayBmp(file, renderer, pathForLog, overlayGrayCover, overlayOwnDrive);
  if (alphaResult == AlphaOverlayResult::Rendered) return true;
  if (alphaResult == AlphaOverlayResult::Error) return false;

  Bitmap bitmap(file);
  const auto parseResult = bitmap.parseHeaders();
  if (parseResult != BmpReaderError::Ok) {
    LOG_ERR("SLP", "Invalid sleep overlay BMP %s: %s", pathForLog, Bitmap::errorToString(parseResult));
    return false;
  }

  LOG_DBG("SLP", "Rendering regular BMP sleep overlay: %s (%dx%d)", pathForLog, bitmap.getWidth(), bitmap.getHeight());
  // drawBitmap leaves white pixels untouched; skipping the initial clear makes
  // them transparent while retaining the existing grayscale pipeline.
  renderBitmapSleepScreen(bitmap, true);
  return true;
}

bool SleepActivity::renderTransparentOverlayPng(const std::string& path) const {
  ImageDimensions dimensions;
  if (!PngToFramebufferConverter::getDimensionsStatic(path, dimensions)) return false;

  const auto placement = calculateBitmapPlacement(dimensions.width, dimensions.height, renderer);
  RenderConfig config;
  config.x = placement.x;
  config.y = placement.y;
  config.maxWidth = renderer.getScreenWidth();
  config.maxHeight = renderer.getScreenHeight();
  config.useDithering = false;
  config.sourceCropX = placement.cropX;
  config.sourceCropY = placement.cropY;
  config.useExactDimensions = placement.cropX > 0.0f || placement.cropY > 0.0f;
  config.preserveAlpha = true;

  PngToFramebufferConverter converter;
  LOG_DBG("SLP", "Rendering transparent PNG overlay: %s (%dx%d)", path.c_str(), dimensions.width, dimensions.height);

  if (!converter.decodeToFramebuffer(path, renderer, config)) return false;
  const bool grayCapable = renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported();
  const bool absolute = grayCapable && !overlayOwnDrive;
  if (!absolute) {
    // Same guard as the BMP overlay and the custom-BMP path: with no grayscale
    // capability the decoded BW frame is the sleep image, so drive it with the
    // one clean refresh instead of building black gray planes and DU-driving
    // the MSB plane over it.
    //
    // r97 C2: an overlay that owns its drive takes the same single from-white
    // path even when the panel CAN do gray -- one drive, nothing else over it.
    renderer.cancelPromotedRefresh();
    renderer.forceCleanNextRefresh();
    renderer.displayBuffer(kSleepClean);
    renderer.setRenderMode(GfxRenderer::BW);
    if (overlayOwnDrive) {
      LOG_INF("SLP", "sleep overlay paint: own from-white clean drive (mode=%d)", static_cast<int>(kSleepClean));
    }
    return true;
  }
  // The cover replaces the previous screen, so clean it like the BMP paths do.
  if (overlayCover) cleanBeforeAbsoluteGray(renderer);
  if (!renderer.displayGrayscaleBase(sleepGrayscaleMode(renderer))) return false;

  // Absolute planes retain B/W background bits; each visible overlay pixel is rewritten in both passes.
  if (!beginOverlayGrayPlane(renderer, overlayGrayCover, GfxRenderer::GRAYSCALE_LSB, absolute) ||
      !converter.decodeToFramebuffer(path, renderer, config)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return true;
  }
  renderer.copyGrayscaleLsbBuffers();

  if (!beginOverlayGrayPlane(renderer, overlayGrayCover, GfxRenderer::GRAYSCALE_MSB, absolute) ||
      !converter.decodeToFramebuffer(path, renderer, config)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return true;
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  return true;
}

bool SleepActivity::renderSleepOverlayPath(const std::string& path) const {
  if (FsHelpers::hasPngExtension(path)) {
    return Storage.exists(path.c_str()) && renderTransparentOverlayPng(path);
  }

  HalFile file;
  return Storage.openFileForRead("SLP", path, file) && renderSleepOverlayFile(file, path.c_str());
}

void SleepActivity::renderTransparentCustomSleepScreen() const {
  if (renderSleepOverlayPath(TRANSPARENT_SLEEP_ROOT_BMP)) return;
  if (renderSleepOverlayPath(TRANSPARENT_SLEEP_ROOT_PNG)) return;

  std::string selectedPath;
  selectRandomSleepFile(TRANSPARENT_SLEEP_DIR, TRANSPARENT_SLEEP_LEGACY_DIR, SleepRecentKind::Overlay, selectedPath);

  if (!selectedPath.empty() && renderSleepOverlayPath(selectedPath)) return;

  LOG_ERR("SLP", "No valid transparent sleep overlay found");
  if (overlayCover) {
    // Show the plain cover.
    const Bitmap& cover = *overlayCover;
    overlayCover = nullptr;
    if (cover.rewindToData() == BmpReaderError::Ok) return renderBitmapSleepScreen(cover);
  }
  renderDefaultSleepScreen();
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  bool withOverlay = false;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_TRANSPARENT):
      renderNoCoverSleepScreen = &SleepActivity::renderTransparentCustomSleepScreen;
      withOverlay = true;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  if (APP_STATE.openEpubPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  // SSD absolute images use the new thresholds; other panels retain legacy tuning.
  const bool originalThresholds =
      renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported() &&
      display.getController() == HalDisplay::Controller::SSD1677 &&
      SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;
  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (FsHelpers::hasReflowableBookExtension(APP_STATE.openEpubPath)) {
    // Handle EPUB, TXT, or Markdown file
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      LOG_ERR("SLP", "Failed to load last book");
      return (this->*renderNoCoverSleepScreen)();
    }

    // GRAY16: the cover is written as an 8-bit greyscale BMP so the sleep screen's
    // 16-level drawing path has tone to read. This is the only place the switch
    // reaches the cover cache; with CP_GRAY16=0 (or CP_GRAY16_SLEEP=0) the call is
    // the 2-bit one it has always been, at the path it has always used.
    // kGray16Sleep, not kGray16Images: this is the SLEEP surface, and it must be
    // independent of the image-page switch (the cache path carries the format, so
    // the two settings never read each other's cover).
    const bool gray16Cover = pixfmt::kGray16Sleep && GfxRenderer::fourBitCanvas();
    if (!lastEpub.generateCoverBmp(cropped, originalThresholds, gray16Cover)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped, originalThresholds, gray16Cover);
  } else {
    return (this->*renderNoCoverSleepScreen)();
  }

  HalFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      if (withOverlay) overlayCover = &bitmap;
      renderBitmapSleepScreen(bitmap);
      overlayCover = nullptr;
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderLastScreenSleepScreen() const {
  const auto pageHeight = renderer.getScreenHeight();
  // r163 D3: the moon is a UI glyph, not artwork -- `Icon`, binary by role.
  const GfxRenderer::ScopedContentRole moonRole(renderer, GfxRenderer::ContentRole::Icon);
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  // r96 (part A of the r95 operator findings): the sleep screen is the LAST frame
  // this session puts on the glass — the panel is parked and the rails dropped
  // right after it — so it must be a clean one. This style used to add the moon
  // with a differential FAST update over the page that was being left behind,
  // which is exactly the "paint over residue" shape that ghosts: the reader's page
  // is still under the moon.
  //
  // Now: ONE from-white GC16 and nothing else. cancelPromotedRefresh() first, so
  // an armed AUTO/HALF promotion cannot substitute a diff-area drive for it, then
  // the r93 one-shot forceClean term, then the FULL drive. Cost: ~815 ms on this
  // panel, paid once per sleep, while the device is already on its way down.
  if (gpio.deviceIsX3()) {
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  } else {
    renderer.cancelPromotedRefresh();
    renderer.forceCleanNextRefresh();
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  }
}

// r97 C2 -- ONE definition of "the overlay owns its drive": cancel any armed
// one-shot promotion FIRST (or a diff-area drive replaces the clean one in the
// producer), then the r93 forceClean one-shot, then one kSleepClean pass of the
// frame that was built from white. No second band drive, no gray pass, and the
// rails are dropped by enterDeepSleep() only after this returns.
void SleepActivity::driveSleepOverlayFromWhite() const {
  LOG_INF("SLP", "sleep overlay paint: own from-white clean drive (mode=%d)", static_cast<int>(kSleepClean));
  renderer.cancelPromotedRefresh();
  renderer.forceCleanNextRefresh();
  renderer.displayBuffer(kSleepClean);
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  // r96: nothing may downgrade the sleep screen to a diff-area drive.
  renderer.cancelPromotedRefresh();
  renderer.forceCleanNextRefresh();
  renderer.displayBuffer(kSleepClean);
}
