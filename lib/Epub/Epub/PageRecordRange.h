#pragma once
#include <cstdint>

// r163 A13: the byte range one page record occupies in a section .bin.
//
// Records are written back to back before the LUTs, so a record starts at its own LUT entry and
// ends at the next page's entry. The last page on disk ends at the page LUT itself (lutOffset),
// which commitBuildFile() writes immediately after the final record; the last page laid out by a
// build that has not committed yet ends at the build's write cursor, because the LUTs do not
// exist until it does.
//
// Anything that is not a non-empty, bounded range is refused, and Section falls back to the
// per-field streaming read that predates the cache -- so a malformed or truncated file degrades
// to the old path instead of being misread.
struct PageRecordRange {
  uint32_t start = 0;
  uint32_t end = 0;
  bool valid = false;
};

inline PageRecordRange pageRecordRange(const uint32_t pagePos, const uint32_t nextPagePos, const bool lastPage,
                                       const uint32_t pageBlockEnd, const uint32_t headerSize,
                                       const uint32_t maxBytes) {
  PageRecordRange range;
  // A record never lives inside the header.
  if (pagePos < headerSize) {
    return range;
  }
  const uint32_t end = lastPage ? pageBlockEnd : nextPagePos;
  if (end <= pagePos || end - pagePos > maxBytes) {
    return range;
  }
  range.start = pagePos;
  range.end = end;
  range.valid = true;
  return range;
}
