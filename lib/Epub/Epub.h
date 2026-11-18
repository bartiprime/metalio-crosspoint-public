#pragma once

#include <ContentProtection.h>
#include <Print.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class ZipFile;

class Epub {
 public:
  struct SyncMetadata {
    std::string isbn;
    std::string asin;
    std::string series;
    std::optional<float> seriesIndex;
  };

  // Series a book declares in its own OPF, in the forms Calibre and EPUB 3 use.
  struct SeriesInfo {
    std::string name;     // empty = a standalone, which is a fact, not a guess
    uint16_t tenths = 0;  // series_index in tenths; 0 = unnumbered
    bool numbered = false;
  };

 private:
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;
  // Optional encrypted-entry accessor. Entries are decoded in memory and stay
  // encrypted at rest. Null when the accessor is not needed or unavailable.
  std::unique_ptr<freeink::content::ContentDecryptor> decryptor;
  // User-presentable reason the encrypted-entry accessor could not be opened.
  std::string protectionError;

  bool findContentOpfFile(std::string* contentOpfFile, ZipFile* sharedZip = nullptr) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true,
                       bool metadataOnly = false, ZipFile* sharedZip = nullptr, SeriesInfo* series = nullptr);
  bool generateThumbBmpForCover(int height, const std::string& coverImageHref) const;
  bool openProtection();
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  void discoverCssFilesFromZip();
  CssParser::ParseResult parseCssFiles(CssParser::CacheStatus existingCacheStatus) const;

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir);
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  bool loadMetadata(std::string& title, std::string& author);
  bool loadSyncMetadata(SyncMetadata& metadata);
  // Metadata for the library index: title, author AND the series. Separate from
  // the two-argument form because it must reach the OPF even when the reader's
  // metadata cache already holds the title — that cache has no series field.
  bool loadMetadata(std::string& title, std::string& author, SeriesInfo& series);
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  // Empty unless the encrypted-entry accessor failed to open.
  const std::string& getProtectionError() const { return protectionError; }
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  // `gray16`: produce the GRAY16 sleep cover — an 8-bit greyscale BMP at its own
  // cache path (`..._g16.bmp`) so the 16-level drawing path has real tone to read
  // and a 2-bit file from another build is never mistaken for it.
  std::string getCoverBmpPath(bool cropped = false, bool originalThresholds = false, bool gray16 = false) const;
  bool generateCoverBmp(bool cropped = false, bool originalThresholds = false, bool gray16 = false) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  bool generateThumbBmp(int height) const;
  // Locate the cover without building spine, TOC, or reading caches.
  bool generateThumbBmpFromSource(int height);
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize,
                                bool allowEarlyStop = false) const;
  // Extract an item to a file on SD. On failure the partial file is removed.
  bool extractItemToFile(const std::string& itemHref, const std::string& destPath) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  // First spine index whose cumulative size reaches `bytes`, or -1 past the end.
  int getSpineIndexForSize(size_t bytes) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;
};
