#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace imagefile {

// ONE RESOLVER, TWO CONSUMERS (SLEEP1 -> POSCR1).
//
// The sleep screen (SLEEP1) and the power-off frame (POSCR1) both want the same
// thing: "given a path the operator put on the card, give me a BMP on disk that
// Bitmap can parse". A BMP is handed back as-is; PNG/JPEG is converted ONCE into
// an 8-bit greyscale BMP under a per-consumer cache directory, and every later
// open hits that cache. Everything format-agnostic lives here. The sleep screen
// keeps its selection policy (recent-window random over /.sleep + /sleep) and its
// paint (renderBitmapSleepScreen); the power-off screen keeps its own selection
// (/poweroff/poweroff.* first, then a random pick) and its own paint.
//
// CacheSpec carries the WORDS each consumer's log lines use. They are part of the
// log contract, not decoration: the sleep spec reproduces every SLEEP1 line
// verbatim, byte for byte, so this refactor is invisible in a sleep log.
struct CacheSpec {
  const char* dir;          // cache directory, created on first convert
  const char* tag;          // LOG tag: "SLP" / "PWR"
  char prefix;              // first char of every cache file name: 's' / 'p'
  const char* imageLabel;   // "Sleep image" / "Power-off image"
  const char* cacheLabel;   // "sleep-cache" / "poweroff-cache"
  const char* convertLabel; // "sleep image" / "power-off image"
  const char* fallbackNote; // "using default sleep screen" / "using the \"Powered off\" frame"
};

// The sleep screen's spec. `prefix` and `dir` are SLEEP1's own values, so every
// cache entry written before this refactor stays a hit: nothing is converted twice
// and no user-visible sleep behaviour changes.
extern const CacheSpec kSleepImageCache;
// The power-off screen's spec: its own directory, so a /poweroff/ wallpaper can
// never evict the sleep screen's conversion (each cache keeps exactly one entry).
extern const CacheSpec kPowerOffImageCache;

enum class Format : uint8_t { Bmp, Png, Jpeg, Unsupported };

Format formatOf(std::string_view name);

uint32_t fnv1a32(const void* data, size_t length, uint32_t hash = 2166136261u);
// Hash of the first 256 bytes: part of the cache key, so a source edited in place
// (same size, same FS stamp) still misses the cache instead of showing stale art.
uint32_t hashFileHead(HalFile& file);

// Cheap accept/reject before a file is ever handed to a converter. BMP: header
// parse. PNG: signature + IHDR limits. JPEG: segment walk to the first SOFn.
bool isValidImageHeader(HalFile& file, Format format, const char* tag);

// Resolve `srcPath` to a BMP this build can parse. False means the caller must
// fall back; every false return has already logged its own reason.
bool resolveBmpPath(const CacheSpec& spec, const std::string& srcPath, int targetWidth, int targetHeight, bool cropped,
                    std::string& bmpPath);

// Pick one supported, header-valid image from `dirPath`: the preferred exact name
// first (checked by the caller for each preferred candidate), else a uniform random
// entry. Unsupported/corrupt entries are skipped WITH a log line; a directory that
// does not exist is not an error, it just has nothing to pick.
bool pickRandomImageInDir(const CacheSpec& spec, const char* dirPath, std::string& outPath);

}  // namespace imagefile
