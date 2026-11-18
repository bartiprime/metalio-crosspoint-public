#pragma once

#include <Epub.h>
#include <Epub/PageLink.h>
#include <Logging.h>

#include <optional>
#include <vector>

#include "ProgressFile.h"

namespace EpubReaderUtils {

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(const Epub& epub, int spineIndex, int pageNumber, int pageCount,
                         std::optional<uint32_t> visibleTextOffset = std::nullopt) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  progressrecord::Record record;
  record.spineIndex = static_cast<uint16_t>(spineIndex);
  record.pageNumber = static_cast<uint16_t>(pageNumber);
  record.pageCount = static_cast<uint16_t>(pageCount);
  record.visibleTextOffset = visibleTextOffset.value_or(0);
  record.hasVisibleTextOffset = visibleTextOffset.has_value();
  return ProgressFile::write(epub.getCachePath(), record);
}

inline const PageLink* linkAtPoint(const std::vector<PageLink>& links, const int x, const int y, const int marginLeft,
                                   const int marginTop) {
  // Finger slop, plus a floor on the target width: a note marker is often a single superscript
  // digit only a few pixels wide. The box is never grown vertically beyond its own line, so
  // taps on the lines above and below still reach the page-turn zones.
  constexpr int TOUCH_SLOP = 6;
  constexpr int MIN_TOUCH_WIDTH = 28;
  const int pageX = x - marginLeft;
  const int pageY = y - marginTop;
  for (const auto& link : links) {
    if (link.contains(pageX, pageY, TOUCH_SLOP, MIN_TOUCH_WIDTH)) {
      return &link;
    }
  }
  return nullptr;
}

}  // namespace EpubReaderUtils
