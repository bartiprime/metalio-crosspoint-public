#pragma once

// Read side of the CLX1 index.
//
// Holds one open file handle and a few hundred bytes; nothing that scales with
// the library stays resident. A page of rows is a handful of seeks, which is
// what the fixed 128-byte record stride buys — record k is always at
// recordStart + 128k, so no offset table has to be loaded to find it.

#include <HalStorage.h>

#include <cstdint>
#include <string>

#include "LibraryFormat.h"

namespace library {

enum class SortOrder : uint8_t {
  // "Recent" orders by file modification time (arrival on the card), oldest
  // first in Asc; firstSeen breaks ties for filesystems without timestamps.
  RecentAsc,
  RecentDesc,
  TitleAsc,
  TitleDesc,
  AuthorAsc,
  AuthorDesc,
  // Series groups books by the series their OPF declares and orders within a
  // group by series_index, then by title. Books with no series are a single
  // "Standalone" run at the END of Asc: an empty series is a fact, and it is not
  // a series called "". Nothing is inferred from a title.
  SeriesAsc,
  SeriesDesc,
};

// One book to locate in the index: the complete-path hash (clixPathHash) is
// the identity; fileSize, when nonzero, is a cheap in-record prefilter that
// avoids reading the hash blob for most records. Zero means size unknown and
// every record's hash is checked.
struct BookIdentity {
  uint64_t pathHash;
  uint32_t fileSize;
};

// One variable-length field, sliced out of a blob read by readBlob(). The blob
// is [name][1+author][1+title][1+sourceAuthor][1+series]; `nameLen` comes from
// the record because the name has no length byte of its own. Returns false (and
// leaves `out` empty) when the blob is shorter than the field claims — the card
// changed, or a truncated read.
bool clixBlobField(std::string_view blob, uint8_t nameLen, uint8_t field, std::string_view& out);

class LibraryIndexFile {
 public:
  LibraryIndexFile() = default;
  ~LibraryIndexFile();
  LibraryIndexFile(const LibraryIndexFile&) = delete;
  LibraryIndexFile& operator=(const LibraryIndexFile&) = delete;

  // Open and validate. On failure `validity()` says why, so a rebuild loop
  // caused by a format bug is visible in the log rather than looking like a slow
  // first boot.
  bool open(const char* path);
  // Accept an otherwise valid stale fold so a rebuild can preserve arrival
  // history without exposing stale sort/search keys to the browser.
  bool openForReconciliation(const char* path);
  void close();
  bool isOpen() const { return opened; }
  bool ioFailed() const { return readFailed; }

  ClixValidity validity() const { return lastValidity; }
  const ClixHeader& header() const { return head; }
  uint16_t bookCount() const { return opened ? head.bookCount : 0; }
  bool ranksDegraded() const { return opened && (head.flags & CLIX_FLAG_RANKS_DEGRADED) != 0; }
  bool dedupDegraded() const { return opened && (head.flags & CLIX_FLAG_DEDUP_DEGRADED) != 0; }

  // Record ordinal of the row at display position `row` in `order`. Returns
  // 0xFFFF when out of range, which callers treat as "no such row" rather than
  // indexing anyway.
  uint16_t ordinalForRow(SortOrder order, uint16_t row);

  // Display rows `base` .. `base + count - 1` in `order`, resolved together:
  // ONE read of the stored permutation (32 bytes per 16 rows) for the author,
  // arrival and series orders, and nothing at all in title order. The values are
  // exactly what ordinalForRow() would return for the same rows -- 0xFFFF for an
  // out-of-range entry -- so a pass over the library pays one seek per batch
  // instead of one per row. Returns false (leaving `out` unspecified) only when
  // the permutation read failed.
  bool readOrdinals(SortOrder order, uint16_t base, uint16_t* out, uint16_t count);

  // Display rows (RecentAsc space) of up to MAX_IDENTITY_LOOKUPS books, 0xFFFF
  // for books not in the index. One chunked pass over the record section plus
  // one over the arrival permutation, so cost is bounded by the library, not by
  // `count` — callers batch their lookups instead of calling per book.
  static constexpr size_t MAX_IDENTITY_LOOKUPS = 16;
  bool recentRowsFor(const BookIdentity* books, size_t count, uint16_t* outRows);

  bool readRecord(uint16_t ordinal, ClixRecord& out);
  // `count` records starting at `base`, in ONE read. The record section is
  // 256-byte records over a 512-aligned start, so a batch of 16 is exactly one
  // 4096-byte buffer and never straddles a sector. Search uses this to make one
  // sequential pass instead of a seek per row.
  bool readRecords(uint16_t base, ClixRecord* out, uint16_t count);
  // Persisted complete-path fingerprint used by rebuild reconciliation.
  bool readPathHash(const ClixRecord& record, uint64_t& out);

  // Display basename, exactly as it sits on the card. This is the only string
  // the UI draws, and it is never shortened on disk.
  bool readName(const ClixRecord& record, std::string& out);
  // The author the build settled on, stored right after the name. Reading it
  // rather than re-deriving it from the name is what makes the metadata pass and
  // the spelling harmonisation visible: neither survives a filename that no
  // longer carries "Title - Author".
  bool readAuthor(const ClixRecord& record, std::string& out);
  bool readTitle(const ClixRecord& record, std::string& out);
  // The series the book's own OPF declared, as written. Empty is a fact
  // (standalone), not a gap: nothing derives a series from a title.
  bool readSeries(const ClixRecord& record, std::string& out);
  // Cleaned author spelling before the library-wide spelling vote. Empty is a
  // valid value, so success is independent of `out.empty()`.
  bool readSourceAuthor(const ClixRecord& record, std::string& out);
  // The record's whole name blob (everything past the path hash) in ONE read, so
  // a rebuild that reuses a book's metadata pays one seek instead of four.
  bool readBlob(const ClixRecord& record, std::string& out);

  // Absolute path of the book, rebuilt from its folder record.
  bool readPath(const ClixRecord& record, std::string& out);

 private:
  bool openImpl(const char* path, bool acceptStaleFold);
  bool readAt(uint32_t offset, void* dst, size_t len);
  bool readBlobField(const ClixRecord& record, uint8_t field, std::string& out);
  // Clamp every length in a record that just came off the card. Kept in one
  // place so the single-record and batched readers cannot diverge.
  bool sanitize(ClixRecord& out) const;

  HalFile file;
  ClixHeader head{};
  bool opened = false;
  bool readFailed = false;
  ClixValidity lastValidity = ClixValidity::BadMagic;
};

}  // namespace library
