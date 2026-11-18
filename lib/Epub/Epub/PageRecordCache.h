#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "PageRecordRange.h"

class HalFile;
class Page;

// r163 A13: the page record the reader is showing, held in RAM.
//
// renderBook() loads the page under the reader's cursor on every paint, and the reader repaints
// for reasons that are not page turns -- status-bar updates, menu dismisses, overlays,
// screenshots. Each of those loads used to walk the page record on the card one scalar at a time
// (Serialization::readPod -> HalFile::read): ~340 SD reads for a 40-line page. This keeps the raw
// record bytes of that one page, so a repaint of the same page deserialises from RAM and never
// touches the card.
//
// One page, fixed cost: the record of the page the reader is on, never a chapter or a window.
// Ownership is Section's -- one cache per section, destroyed with it -- so a spine change or a
// re-pagination cannot inherit a stale record; the in-place changes (a build starting,
// committing, being abandoned, or the section file being reloaded) call invalidate().
class PageRecordCache {
 public:
  struct Stats {
    uint32_t hits = 0;       // loads answered from RAM: zero SD reads
    uint32_t refreshes = 0;  // records copied into RAM (one bulk read each)
    uint32_t sdReads = 0;    // HalFile::read calls this cache has issued
  };

  // The bytes a page record deserialises from may have changed: drop the copy.
  void invalidate();

  // Deserialise `page` from `range` inside `file`. The record is copied in with a single read the
  // first time (one read for the whole record, instead of one per field); a repeat load of the
  // same page from the same source is served from RAM with no file access at all.
  //   restoreTo -- cursor to put back afterwards (a build's append position; 0 for a throwaway
  //                read handle).
  //   visibleTextOffset -- page start offset, which is not part of the serialised record body. It
  //                       is stamped onto every result, hits included: the caller's LUT describes
  //                       THIS read, and a rebuilt LUT is how one page index gets a new offset
  //                       while its record bytes stay the same.
  // Returns nullptr when the range is unusable, a seek failed, or the read failed; the caller then
  // takes its streaming read, which reports failures exactly as it always has.
  std::unique_ptr<Page> load(int page, bool fromBuild, HalFile& file, const PageRecordRange& range,
                             uint32_t restoreTo, uint32_t visibleTextOffset);

  const Stats& stats() const { return stats_; }

 private:
  std::unique_ptr<Page> deserialize(uint32_t visibleTextOffset) const;

  bool valid_ = false;
  int page_ = -1;
  // The same page index can be served from the in-progress build's tmp .bin or from the
  // committed file; the two are different bytes and must not be mixed up.
  bool fromBuild_ = false;
  std::vector<uint8_t> bytes_;
  Stats stats_;
};
