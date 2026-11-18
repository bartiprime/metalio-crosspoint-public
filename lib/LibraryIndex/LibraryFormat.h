#pragma once

// On-disk layout of the Library index (`CLX1`), plus the pure validation and
// offset arithmetic around it.
//
// Deliberately free of HalStorage and Arduino so the whole layout is
// host-testable (test/library_format). File I/O lives in LibraryIndexFile.
//
// Layout, in order, every section starting on a 512-byte boundary:
//
//   header        64 bytes of struct, padded to 512
//   folders       F variable-length records; the id of a folder IS its ordinal
//   records       N x exactly 256 bytes, in folded-title order
//   permutations  authorOrder[N], arrivalOrder[N], seriesOrder[N], all u16
//   names         path hash, filename, display author, title, source author and
//                 series blobs
//
// The fixed 256-byte record stride is the load-bearing choice: record k lives at
// recordStart + 256k, so paging is O(1) in every sort order with no offset
// table; 16 records fill a 4096-byte buffer exactly, so a streaming scan never
// straddles a record; and since recordStart is 512-aligned and 256 divides 512,
// every chunk read is aligned by construction rather than by remembering to.
//
// LIB1-L2 doubled the stride from 128 to 256. The old record was full to the
// byte, and search needs the four keys it looks at — title, author, series and
// filename — resident in the record rather than behind a blob seek per row.
// 256 also keeps the sector-tiling property (two records per 512-byte sector)
// that 160 or 192 would have broken. The cost is a wider index: 1,204 books go
// from ~250 KiB to ~380 KiB.

#include <cstddef>
#include <cstdint>

namespace library {

inline constexpr char CLIX_MAGIC[4] = {'C', 'L', 'X', '1'};
// Bumping this is the whole migration: an index from an older version fails
// validation and is rebuilt. No previous development format is accepted.
//
// 3 (LIB1-L2) adds series, series_index and the folded author/filename/series
// search keys, and a third permutation (series order). An index written by 2
// cannot be reconciled into a 3: there is no series to carry over and the record
// stride changed, so it is rejected outright and the first boot after the update
// rebuilds once. Nothing half-migrated is ever readable.
inline constexpr uint8_t CLIX_FORMAT_VERSION = 3;

// Bump when the fold or a permutation's sort key changes.
// Forces fold and ranks to be rebuilt while firstSeen values are preserved, so
// arrival history survives.
inline constexpr uint8_t CLIX_FOLD_VERSION = 5;

inline constexpr uint32_t CLIX_ALIGN = 512;
// Title fold: the search haystack and the title sort key.
inline constexpr size_t CLIX_FOLD_BYTES = 96;
// AUTHOR fold, already case- and diacritic-folded at build time so a query
// never folds an author per non-matching row. 40 bytes is past every real name;
// a longer one is truncated on the same UTF-8 boundary rule as the title.
inline constexpr size_t CLIX_AUTHOR_FOLD_BYTES = 40;
// SERIES fold: the group/sort key. Deliberately short — a series is a grouping
// label, and two series that agree for 24 folded bytes are the same shelf.
inline constexpr size_t CLIX_SERIES_KEY_BYTES = 24;
// FILENAME fold. Bulk dumps carry books whose OPF says nothing useful, and the
// filename is then the only thing the reader can type. Stored so the search
// needs no blob read per row.
inline constexpr size_t CLIX_NAME_FOLD_BYTES = 56;
inline constexpr size_t CLIX_AUTHOR_KEY_BYTES = 12;

// A 2000-book card already produces a 429 KiB index. This hard bound keeps every
// record count and permutation ordinal representable by uint16_t.
inline constexpr uint16_t CLIX_MAX_RECORDS = 4096;

// Complete-path fingerprint stored in front of every record's name blob.
// Shared by the builder's reconciliation and the browser's recent-book lookup,
// which must agree byte-for-byte on the hash of the same path.
inline uint64_t clixPathHash(const char* data, const size_t len) {
  uint64_t hash = 14695981039346656037ULL;  // FNV-1a 64
  for (size_t i = 0; i < len; i++) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

enum ClixFlags : uint8_t {
  CLIX_FLAG_RANKS_DEGRADED = 1 << 0,
  CLIX_FLAG_DEDUP_DEGRADED = 1 << 1,
};

enum ClixMetadataStatus : uint8_t {
  CLIX_METADATA_NOT_ATTEMPTED = 0,
  CLIX_METADATA_EXTRACTED = 1,
  CLIX_METADATA_FAILED = 2,
};

#pragma pack(push, 1)

struct ClixHeader {
  char magic[4];
  uint8_t formatVersion;
  uint8_t foldVersion;
  uint8_t flags;
  uint8_t metadataEnabled;
  uint16_t bookCount;
  uint16_t folderCount;
  uint16_t nextFirstSeen;
  uint16_t padding1;
  uint32_t folderStart;
  uint32_t folderLen;
  uint32_t recordStart;
  uint32_t permStart;
  uint32_t nameStart;
  uint32_t nameLen;
  // Expected total file size. Comparing it with the real size is a free
  // truncation guard: a build interrupted by a power cut cannot pass.
  uint32_t selfSize;
  uint8_t reserved[20];
};
static_assert(sizeof(ClixHeader) == 64, "ClixHeader must be exactly 64 bytes");

// Monotonic rebuild generation, carved out of the header's reserved tail.
//
// It exists because the RTC backup is lost on this board: every file the device
// writes carries Jan 1 1980, so "index older than N days" is true for every
// index ever built and would rebuild forever. A counter cannot lie that way.
//
// Not validated on load. An index written before this field existed reads as
// generation 0, which simply means the next build is generation 1.
inline uint32_t clixGeneration(const ClixHeader& h) {
  uint32_t value = 0;
  for (size_t i = 0; i < sizeof(value); i++) value |= static_cast<uint32_t>(h.reserved[i]) << (8 * i);
  return value;
}

inline void setClixGeneration(ClixHeader& h, const uint32_t value) {
  for (size_t i = 0; i < sizeof(value); i++) h.reserved[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
}


// Which variable-length field to read out of a record's name blob, in blob
// order. Named rather than passed as a bare 0..4: the order is on-disk state.
enum ClixBlobField : uint8_t {
  CLIX_BLOB_AUTHOR = 0,        // display spelling chosen by the spelling vote
  CLIX_BLOB_TITLE = 1,         // the book's own title; empty when it gave none
  CLIX_BLOB_SOURCE_AUTHOR = 2,  // raw spelling, kept for the next spelling vote
  CLIX_BLOB_SERIES = 3,        // display series name; empty for a standalone
};

struct ClixRecord {
  uint32_t nameOff;   // from nameStart, into the per-record name blob
  uint32_t fileSize;  // captured while the dirent was open; part of the identity
  uint32_t modificationTime;
  uint16_t firstSeen;
  uint16_t folderId;
  // Calibre's series_index in TENTHS, so 1.5 (a novella between two novels)
  // survives. 0 means "no position": either a standalone or an unnumbered book.
  uint16_t seriesIndex;
  uint8_t nameLen;
  uint8_t foldLen;        // title fold, the search haystack and title sort key
  uint8_t authorFoldLen;  // folded display author, for search
  uint8_t seriesFoldLen;  // folded series, for search and the series permutation
  uint8_t nameFoldLen;    // folded filename, for search
  uint8_t authorKeyLen;
  uint8_t metadataStatus;
  uint8_t padding0;
  char fold[CLIX_FOLD_BYTES];
  char authorFold[CLIX_AUTHOR_FOLD_BYTES];
  char seriesFold[CLIX_SERIES_KEY_BYTES];
  char nameFold[CLIX_NAME_FOLD_BYTES];
  char authorKey[CLIX_AUTHOR_KEY_BYTES];
  uint8_t reserved[2];
};
static_assert(sizeof(ClixRecord) == 256, "ClixRecord must be exactly 256 bytes");
static_assert(CLIX_ALIGN % sizeof(ClixRecord) == 0, "records must tile a 512-byte sector");

struct ClixFolderHeader {
  uint8_t pathLen;  // 1..255; the path bytes follow, with no trailing '/'
};
static_assert(sizeof(ClixFolderHeader) == 1, "ClixFolderHeader must be 1 byte");

#pragma pack(pop)

inline uint32_t alignUp(const uint32_t value) { return (value + CLIX_ALIGN - 1) / CLIX_ALIGN * CLIX_ALIGN; }

// Fill in every offset and the expected file size from the counts alone, so the
// writer and the reader can never disagree about where a section starts.
inline void layoutSections(ClixHeader& h, const uint32_t folderBytes, const uint32_t nameBytes) {
  h.folderStart = CLIX_ALIGN;
  h.folderLen = folderBytes;
  h.recordStart = alignUp(h.folderStart + folderBytes);
  h.permStart = alignUp(h.recordStart + static_cast<uint32_t>(h.bookCount) * sizeof(ClixRecord));
  // Three permutations: author, arrival, series.
  h.nameStart = alignUp(h.permStart + static_cast<uint32_t>(h.bookCount) * 3u * sizeof(uint16_t));
  h.nameLen = nameBytes;
  h.selfSize = h.nameStart + nameBytes;
}

inline uint32_t recordOffset(const ClixHeader& h, const uint16_t ordinal) {
  return h.recordStart + static_cast<uint32_t>(ordinal) * sizeof(ClixRecord);
}
inline uint32_t authorOrderOffset(const ClixHeader& h, const uint16_t k) {
  return h.permStart + static_cast<uint32_t>(k) * sizeof(uint16_t);
}
inline uint32_t arrivalOrderOffset(const ClixHeader& h, const uint16_t k) {
  return h.permStart + (static_cast<uint32_t>(h.bookCount) + k) * sizeof(uint16_t);
}
inline uint32_t seriesOrderOffset(const ClixHeader& h, const uint16_t k) {
  return h.permStart + (2u * static_cast<uint32_t>(h.bookCount) + k) * sizeof(uint16_t);
}

// Why a loaded index was rejected. Reported rather than swallowed so a rebuild
// loop caused by a format bug shows up in the log instead of looking like a slow
// first boot.
enum class ClixValidity : uint8_t {
  Ok,
  BadMagic,
  UnknownFormatVersion,
  StaleFoldVersion,
  SizeMismatch,  // truncated, or a build interrupted before the final rename
  CountOutOfRange,
  SectionsInconsistent,
};

// Validate a header against the real file size. Cheap enough to run on the one
// sector already read, and strict enough that nothing downstream has to
// re-check bounds.
inline ClixValidity validateHeaderStructure(const ClixHeader& h, const uint64_t actualFileSize) {
  for (size_t i = 0; i < sizeof(CLIX_MAGIC); i++) {
    if (h.magic[i] != CLIX_MAGIC[i]) return ClixValidity::BadMagic;
  }
  if (h.formatVersion != CLIX_FORMAT_VERSION) return ClixValidity::UnknownFormatVersion;
  if (h.bookCount > CLIX_MAX_RECORDS) return ClixValidity::CountOutOfRange;
  if (h.metadataEnabled > 1) return ClixValidity::SectionsInconsistent;
  if (actualFileSize != h.selfSize) return ClixValidity::SizeMismatch;

  // Both lengths are attacker-controlled bytes. Capped against the real file
  // size they cannot wrap the 32-bit section sums below, so the layout
  // comparison stays sound instead of re-deriving the same wrapped values.
  if (h.folderLen > actualFileSize || h.nameLen > actualFileSize) return ClixValidity::SectionsInconsistent;

  ClixHeader expected = h;
  layoutSections(expected, h.folderLen, h.nameLen);
  if (expected.folderStart != h.folderStart || expected.recordStart != h.recordStart ||
      expected.permStart != h.permStart || expected.nameStart != h.nameStart || expected.selfSize != h.selfSize) {
    return ClixValidity::SectionsInconsistent;
  }
  return ClixValidity::Ok;
}

inline ClixValidity validateHeader(const ClixHeader& h, const uint64_t actualFileSize) {
  const ClixValidity structure = validateHeaderStructure(h, actualFileSize);
  if (structure != ClixValidity::Ok) return structure;
  return h.foldVersion == CLIX_FOLD_VERSION ? ClixValidity::Ok : ClixValidity::StaleFoldVersion;
}

const char* clixValidityName(ClixValidity v);

}  // namespace library
