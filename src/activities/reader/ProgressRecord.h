#pragma once

#include <cstddef>
#include <cstdint>

// r163 C2/C4: the progress record as everything that touches it agrees on it -- the loop task's
// synchronous save, the background writer, and the pending-snapshot store. Pure on purpose: the
// coalescing rules in ProgressSaves.h are host-tested, so they cannot depend on HalStorage.

namespace progressrecord {

struct Record {
  uint16_t spineIndex = 0;
  uint16_t pageNumber = 0;
  uint16_t pageCount = 0;
  uint32_t visibleTextOffset = 0;
  bool hasVisibleTextOffset = false;

  bool operator==(const Record& other) const {
    return spineIndex == other.spineIndex && pageNumber == other.pageNumber && pageCount == other.pageCount &&
           visibleTextOffset == other.visibleTextOffset && hasVisibleTextOffset == other.hasVisibleTextOffset;
  }
  bool operator!=(const Record& other) const { return !(*this == other); }
};

// The bytes progress.bin holds: 6, or 10 when the visible text offset is carried. The loader
// accepts 4, 6 and 10.
inline size_t encode(const Record& record, uint8_t out[10]) {
  out[0] = record.spineIndex & 0xFF;
  out[1] = (record.spineIndex >> 8) & 0xFF;
  out[2] = record.pageNumber & 0xFF;
  out[3] = (record.pageNumber >> 8) & 0xFF;
  out[4] = record.pageCount & 0xFF;
  out[5] = (record.pageCount >> 8) & 0xFF;
  if (!record.hasVisibleTextOffset) return 6;
  out[6] = record.visibleTextOffset & 0xFF;
  out[7] = (record.visibleTextOffset >> 8) & 0xFF;
  out[8] = (record.visibleTextOffset >> 16) & 0xFF;
  out[9] = (record.visibleTextOffset >> 24) & 0xFF;
  return 10;
}

// Decode the bytes progress.bin may hold. `len` is 4 (legacy, spine+page only), 6 or 10.
inline bool decode(const uint8_t* data, const size_t len, Record& out) {
  if (len != 4 && len != 6 && len != 10) return false;
  out.spineIndex = static_cast<uint16_t>(data[0] + (data[1] << 8));
  out.pageNumber = static_cast<uint16_t>(data[2] + (data[3] << 8));
  out.pageCount = len >= 6 ? static_cast<uint16_t>(data[4] + (data[5] << 8)) : 0;
  if (len == 10) {
    out.visibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                            (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
    out.hasVisibleTextOffset = true;
  } else {
    out.visibleTextOffset = 0;
    out.hasVisibleTextOffset = false;
  }
  return true;
}

}  // namespace progressrecord
