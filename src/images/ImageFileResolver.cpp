#include "images/ImageFileResolver.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Memory.h>
#include <PNGdec.h>
#include <PngToBmpConverter.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace imagefile {

// SLEEP1's own words and directory. Both are the log/on-disk contract of the sleep
// screen, so they are reproduced exactly here: cache file names keep the leading
// 's' and the %08x fields, and every message keeps its SLEEP1 wording.
const CacheSpec kSleepImageCache = {
    "/.crosspoint/sleep-cache", "SLP", 's', "Sleep image", "sleep-cache", "sleep image", "using default sleep screen",
};
const CacheSpec kPowerOffImageCache = {
    "/.crosspoint/poweroff-cache", "PWR", 'p', "Power-off image", "poweroff-cache", "power-off image",
    "using the \"Powered off\" frame",
};

namespace {

constexpr size_t MAX_IMAGE_NAME_LEN = 256;

}  // namespace

Format formatOf(std::string_view name) {
  if (FsHelpers::hasBmpExtension(name)) return Format::Bmp;
  if (FsHelpers::hasPngExtension(name)) return Format::Png;
  if (FsHelpers::hasJpgExtension(name)) return Format::Jpeg;
  return Format::Unsupported;
}

uint32_t fnv1a32(const void* data, const size_t length, uint32_t hash) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

uint32_t hashFileHead(HalFile& file) {
  uint8_t head[256];
  if (!file.seek(0)) return 0;
  const int headBytes = file.read(head, sizeof(head));
  if (headBytes <= 0) return 0;
  return fnv1a32(head, static_cast<size_t>(headBytes));
}

namespace {

uint32_t readBE32(HalFile& file) {
  const int c0 = file.read();
  const int c1 = file.read();
  const int c2 = file.read();
  const int c3 = file.read();
  if (c0 < 0 || c1 < 0 || c2 < 0 || c3 < 0) return 0;
  return (static_cast<uint32_t>(c0) << 24) | (static_cast<uint32_t>(c1) << 16) | (static_cast<uint32_t>(c2) << 8) |
         static_cast<uint32_t>(c3);
}

bool isValidPngHeader(HalFile& file) {
  static constexpr uint8_t PNG_SIGNATURE[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  static constexpr uint32_t MAX_SOURCE_PIXELS = 2048u * 1536u;
  uint8_t signature[8];
  if (!file.seek(0) || file.read(signature, sizeof(signature)) != static_cast<int>(sizeof(signature)) ||
      !std::equal(std::begin(signature), std::end(signature), std::begin(PNG_SIGNATURE))) {
    return false;
  }

  const uint32_t ihdrLength = readBE32(file);
  char chunkType[4];
  if (file.read(reinterpret_cast<uint8_t*>(chunkType), sizeof(chunkType)) != static_cast<int>(sizeof(chunkType)) ||
      ihdrLength != 13 || !std::equal(std::begin(chunkType), std::end(chunkType), "IHDR")) {
    return false;
  }

  const uint32_t width = readBE32(file);
  const uint32_t height = readBE32(file);
  const int bitDepth = file.read();
  const int colorType = file.read();
  const int compression = file.read();
  const int filter = file.read();
  const int interlace = file.read();

  const bool supportedBitDepth =
      bitDepth == 8 || ((colorType == PNG_PIXEL_GRAYSCALE || colorType == PNG_PIXEL_INDEXED) &&
                        (bitDepth == 1 || bitDepth == 2 || bitDepth == 4));
  const bool supportedColorType = colorType == PNG_PIXEL_GRAYSCALE || colorType == PNG_PIXEL_TRUECOLOR ||
                                  colorType == PNG_PIXEL_INDEXED || colorType == PNG_PIXEL_GRAY_ALPHA ||
                                  colorType == PNG_PIXEL_TRUECOLOR_ALPHA;
  return width > 0 && height > 0 && width <= 2048 && height <= 3072 && width * height <= MAX_SOURCE_PIXELS &&
         supportedBitDepth && supportedColorType && compression == 0 && filter == 0 && interlace == 0;
}

// Cheap accept/reject for a JPEG candidate: skip the segment chain to the first
// SOFn and read the frame size, instead of letting the decoder open a 6000x4000 phone
// photo that ends in a failed convert. Same limits the converter itself enforces
// (JpegToBmpConverter: 2048x3072), so a file this accepts will not be rejected later
// for size. If no SOF is reached inside the segment budget the file is assumed usable:
// the converter re-checks dimensions straight after open() and fails fast (and cheap).
bool isValidJpegHeader(HalFile& file, const char* tag) {
  static constexpr uint32_t MAX_SOURCE_PIXELS = 2048u * 1536u;
  static constexpr int MAX_SEGMENTS = 64;
  if (!file.seek(0)) return false;
  if (file.read() != 0xFF || file.read() != 0xD8) return false;  // SOI

  for (int segment = 0; segment < MAX_SEGMENTS; ++segment) {
    if (file.read() != 0xFF) return false;
    int marker = file.read();
    while (marker == 0xFF) marker = file.read();  // fill bytes
    if (marker < 0) return false;
    // Markers without a length field.
    if (marker == 0x01 || marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) continue;
    if (marker == 0xD9 || marker == 0xDA) return false;  // EOI/SOS before any frame header: no image

    const int lenHi = file.read();
    const int lenLo = file.read();
    if (lenHi < 0 || lenLo < 0) return false;
    const int length = (lenHi << 8) | lenLo;
    if (length < 2) return false;

    const bool isSof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
    if (isSof) {
      const int precision = file.read();
      const int hHi = file.read();
      const int hLo = file.read();
      const int wHi = file.read();
      const int wLo = file.read();
      if (precision != 8 || hHi < 0 || hLo < 0 || wHi < 0 || wLo < 0) return false;
      const uint32_t height = static_cast<uint32_t>((hHi << 8) | hLo);
      const uint32_t width = static_cast<uint32_t>((wHi << 8) | wLo);
      return width > 0 && height > 0 && width <= 2048 && height <= 3072 && width * height <= MAX_SOURCE_PIXELS;
    }
    if (!file.seekCur(length - 2)) return false;
  }
  LOG_DBG(tag, "JPEG: no frame header in the first %d segments, deferring to the decoder", MAX_SEGMENTS);
  return true;
}

bool isValidBmpHeader(HalFile& file) {
  Bitmap bitmap(file);
  return bitmap.parseHeaders() == BmpReaderError::Ok;
}

// Cache key inputs are folded into the FILE NAME, so a cache entry validates itself by
// existing: path + source size + the FS write stamp + a hash of the first 256 bytes.
// Nothing here reads a clock -- this board's RTC can be unset, so "age" is not a usable
// staleness rule -- and nothing stats the cached file's contents. A source that changed
// in any of those dimensions simply hashes to a different name and is converted again;
// the previous entry is pruned. `cropped` is in the name for the same reason Epub puts
// the format in the cover name: flipping sleepScreenCoverMode must not reuse a BMP
// shaped for the other mode.
std::string cachePathFor(const CacheSpec& spec, const std::string& srcPath, const uint32_t srcSize,
                         const uint32_t srcMtime, const uint32_t headHash, const bool cropped) {
  char name[64];
  snprintf(name, sizeof(name), "%c%08x_%08x_%08x_%08x%s.bmp", spec.prefix,
           static_cast<unsigned>(fnv1a32(srcPath.data(), srcPath.size())), static_cast<unsigned>(srcSize),
           static_cast<unsigned>(srcMtime), static_cast<unsigned>(headHash), cropped ? "c" : "f");
  std::string path = spec.dir;
  path += "/";
  path += name;
  return path;
}

// One converted image is all a cache needs to hold; drop the rest so a card full of
// changing wallpapers cannot grow the cache directory without bound.
void pruneCache(const CacheSpec& spec, const std::string& keepName) {
  auto dir = Storage.open(spec.dir);
  if (!dir || !dir.isDirectory()) return;

  constexpr size_t MAX_STALE = 8;
  std::string stale[MAX_STALE];
  size_t staleCount = 0;
  char name[MAX_IMAGE_NAME_LEN];
  for (auto entry = dir.openNextFile(); entry && staleCount < MAX_STALE; entry = dir.openNextFile()) {
    if (entry.isDirectory()) continue;
    entry.getName(name, sizeof(name));
    if (name[0] == '\0' || name[0] == '.' || keepName == name) continue;
    stale[staleCount++] = std::string(spec.dir) + "/" + name;
  }
  dir.close();
  for (size_t i = 0; i < staleCount; ++i) Storage.remove(stale[i].c_str());
}

bool convertToBmp(const CacheSpec& spec, const std::string& srcPath, const std::string& outPath,
                  const int targetWidth, const int targetHeight, const bool cropped) {
  HalFile src;
  if (!Storage.openFileForRead(spec.tag, srcPath, src)) {
    LOG_ERR(spec.tag, "%s unreadable: %s; %s", spec.imageLabel, srcPath.c_str(), spec.fallbackNote);
    return false;
  }
  HalFile out;
  if (!Storage.openFileForWrite(spec.tag, outPath, out)) {
    LOG_ERR(spec.tag, "%s: cannot write %s; %s", spec.cacheLabel, outPath.c_str(), spec.fallbackNote);
    src.close();
    return false;
  }

  const uint32_t startMs = millis();
  bool ok = false;
  if (FsHelpers::hasPngExtension(std::string_view{srcPath})) {
    ok = PngToBmpConverter::pngFileToBmpStreamWithSize(src, out, targetWidth, targetHeight, /*eightBit=*/true, cropped);
  } else if (FsHelpers::hasJpgExtension(std::string_view{srcPath})) {
    ok = JpegToBmpConverter::jpegFileToBmpStreamWithSize(src, out, targetWidth, targetHeight, /*eightBit=*/true, cropped);
  } else {
    LOG_ERR(spec.tag, "%s format unsupported: %s; %s", spec.imageLabel, srcPath.c_str(), spec.fallbackNote);
  }
  out.flush();
  out.close();
  src.close();

  const uint32_t elapsedMs = millis() - startMs;
  if (!ok) {
    LOG_ERR(spec.tag, "%s conversion failed: %s (%u ms); %s", spec.imageLabel, srcPath.c_str(), elapsedMs,
            spec.fallbackNote);
    return false;
  }
  LOG_INF(spec.tag, "%s convert: %s -> %s (%dx%d crop=%d) in %u ms", spec.convertLabel, srcPath.c_str(), outPath.c_str(),
          targetWidth, targetHeight, cropped ? 1 : 0, elapsedMs);
  return true;
}

}  // namespace

bool isValidImageHeader(HalFile& file, const Format format, const char* tag) {
  switch (format) {
    case Format::Bmp:
      return isValidBmpHeader(file);
    case Format::Png:
      return isValidPngHeader(file);
    case Format::Jpeg:
      return isValidJpegHeader(file, tag);
    default:
      return false;
  }
}

// Returns the path to a BMP on disk for this image: the source itself when it is a
// BMP, otherwise the cached conversion. Every false return is a caller that must fall
// back; each one has already logged its own reason.
bool resolveBmpPath(const CacheSpec& spec, const std::string& srcPath, const int targetWidth, const int targetHeight,
                    const bool cropped, std::string& bmpPath) {
  if (FsHelpers::hasBmpExtension(std::string_view{srcPath})) {
    bmpPath = srcPath;
    return true;
  }

  HalFile src;
  if (!Storage.openFileForRead(spec.tag, srcPath, src)) {
    LOG_ERR(spec.tag, "%s unreadable: %s; %s", spec.imageLabel, srcPath.c_str(), spec.fallbackNote);
    return false;
  }
  const uint32_t srcSize = static_cast<uint32_t>(src.fileSize());
  const uint32_t srcMtime = src.modificationTime();
  const uint32_t headHash = hashFileHead(src);
  src.close();

  const std::string cachePath = cachePathFor(spec, srcPath, srcSize, srcMtime, headHash, cropped);
  if (Storage.exists(cachePath.c_str())) {
    LOG_DBG(spec.tag, "%s hit: %s", spec.cacheLabel, cachePath.c_str());
    bmpPath = cachePath;
    return true;
  }

  if (!Storage.exists(spec.dir) && !Storage.mkdir(spec.dir)) {
    LOG_ERR(spec.tag, "%s: cannot create %s; %s", spec.cacheLabel, spec.dir, spec.fallbackNote);
    return false;
  }

  // Written to .tmp and renamed: a conversion cut short by a brown-out must never be
  // picked up as this image's cache entry next time.
  const std::string tmpPath = cachePath + ".tmp";
  if (!convertToBmp(spec, srcPath, tmpPath, targetWidth, targetHeight, cropped)) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), cachePath.c_str())) {
    LOG_ERR(spec.tag, "%s: cannot publish %s; %s", spec.cacheLabel, cachePath.c_str(), spec.fallbackNote);
    Storage.remove(tmpPath.c_str());
    return false;
  }
  pruneCache(spec, cachePath.substr(cachePath.rfind('/') + 1));
  bmpPath = cachePath;
  return true;
}

bool pickRandomImageInDir(const CacheSpec& spec, const char* dirPath, std::string& outPath) {
  auto dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    LOG_INF(spec.tag, "%s: %s is not on the card", spec.imageLabel, dirPath);
    return false;
  }

  // Two passes over the same directory: count the entries this build can actually use,
  // then walk to the chosen index. Unsupported/corrupt files are counted out WITH a log
  // line (the operator asked for those, so a silent skip would be a bug they cannot see).
  uint16_t validCount = 0;
  char name[MAX_IMAGE_NAME_LEN];
  for (auto file = dir.openNextFile(); file && validCount < 0xFFFFu; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(name, sizeof(name));
    if (name[0] == '\0' || name[0] == '.') continue;
    const Format format = formatOf(name);
    if (format == Format::Unsupported) {
      LOG_DBG(spec.tag, "Skipping unsupported %s: %s", spec.convertLabel, name);
      continue;
    }
    if (!isValidImageHeader(file, format, spec.tag)) {
      LOG_DBG(spec.tag, "Skipping invalid %s: %s", spec.convertLabel, name);
      continue;
    }
    ++validCount;
  }
  dir.close();
  if (validCount == 0) {
    LOG_INF(spec.tag, "%s: no usable file in %s", spec.imageLabel, dirPath);
    return false;
  }

  const uint16_t chosen = static_cast<uint16_t>(random(validCount));
  dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return false;
  uint16_t seen = 0;
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(name, sizeof(name));
    if (name[0] == '\0' || name[0] == '.') continue;
    const Format format = formatOf(name);
    if (format == Format::Unsupported || !isValidImageHeader(file, format, spec.tag)) continue;
    if (seen++ != chosen) continue;
    outPath.reserve(strlen(dirPath) + 1 + strlen(name));
    outPath = dirPath;
    outPath += "/";
    outPath += name;
    return true;
  }
  return false;
}

}  // namespace imagefile
