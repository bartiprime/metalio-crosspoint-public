#include "PageRecordCache.h"

#include <HalStorage.h>
#include <MemReader.h>
#include <Serialization.h>

#include "Page.h"

#include <mutex>

namespace {
// A page record is 1-4 KB in practice. A record that does not fit is not cached at all and the
// streaming read serves it, so the cache's worst case is one bounded buffer.
constexpr uint32_t MAX_RECORD_BYTES = 16 * 1024;

// loadPage() is reached from the render task (renderBook) and from the main task (the clipping
// window and the footnote picker both load the page under the cursor), and both can touch the
// same section. Before this cache existed those two paths shared no state -- a local HalFile per
// read -- so the RAM copy is behind a lock to keep that property. It is a leaf lock: nothing
// taken inside it, so it cannot deadlock against RenderLock.
std::mutex cacheLock;
}  // namespace

void PageRecordCache::invalidate() {
  const std::lock_guard<std::mutex> guard(cacheLock);
  valid_ = false;
  page_ = -1;
  fromBuild_ = false;
  bytes_.clear();
}

// The record is deserialised by the same code whether it came off the card or out of RAM -- only
// the reader differs -- and visibleTextOffset, which is not part of the serialised body, is
// filled in from the LUT the CALLER's read came with (never from a copy taken earlier: a rebuilt
// LUT is exactly how one page index gets a new offset).
std::unique_ptr<Page> PageRecordCache::deserialize(const uint32_t visibleTextOffset) const {
  MemReader reader(bytes_.data(), bytes_.size());
  auto page = Page::deserialize(reader);
  if (page) {
    page->visibleTextOffset = visibleTextOffset;
  }
  return page;
}

std::unique_ptr<Page> PageRecordCache::load(const int page, const bool fromBuild, HalFile& file,
                                            const PageRecordRange& range, const uint32_t restoreTo,
                                            const uint32_t visibleTextOffset) {
  const std::lock_guard<std::mutex> guard(cacheLock);
  if (!range.valid || range.end - range.start > MAX_RECORD_BYTES) {
    valid_ = false;
    return nullptr;
  }

  if (valid_ && page_ == page && fromBuild_ == fromBuild) {
    ++stats_.hits;
    return deserialize(visibleTextOffset);
  }

  // MISS: one read for the whole record. The per-field reads the streaming path does cost one
  // HalFile::read (and one SD round trip) per scalar; the buffer keeps the deserialiser's reads
  // out of the driver entirely.
  valid_ = false;
  bytes_.resize(range.end - range.start);
  // BOTH seeks are checked. An unchecked failure leaves the handle wherever it was: the read below
  // still returns a full-length window and the record deserialises into a page that LOOKS valid but
  // is somebody else's bytes, and a failed restore leaves the caller's cursor (a build's append
  // position) wrong. Either way the answer is "no cache": nullptr, and the caller takes the
  // streaming read, which reports a failure exactly as it always has.
  if (!file.seek(range.start)) {
    bytes_.clear();
    return nullptr;
  }
  const long got = static_cast<long>(file.read(bytes_.data(), bytes_.size()));
  const bool restored = file.seek(restoreTo);
  ++stats_.sdReads;
  if (got != static_cast<long>(bytes_.size()) || !restored) {
    bytes_.clear();
    return nullptr;
  }

  valid_ = true;
  page_ = page;
  fromBuild_ = fromBuild;
  ++stats_.refreshes;
  return deserialize(visibleTextOffset);
}

