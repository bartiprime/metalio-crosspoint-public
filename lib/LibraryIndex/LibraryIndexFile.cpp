#include "LibraryIndexFile.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "LibraryText.h"

namespace library {

LibraryIndexFile::~LibraryIndexFile() { close(); }

bool LibraryIndexFile::open(const char* path) { return openImpl(path, false); }

bool LibraryIndexFile::openForReconciliation(const char* path) { return openImpl(path, true); }

bool LibraryIndexFile::openImpl(const char* path, const bool acceptStaleFold) {
  close();
  readFailed = false;
  if (!Storage.openFileForRead("LIBIDX", path, file)) {
    readFailed = true;
    return false;
  }

  if (file.read(&head, sizeof(head)) != static_cast<int>(sizeof(head))) {
    readFailed = true;
    lastValidity = ClixValidity::SizeMismatch;
    file.close();
    return false;
  }

  lastValidity =
      acceptStaleFold ? validateHeaderStructure(head, file.fileSize64()) : validateHeader(head, file.fileSize64());
  if (lastValidity != ClixValidity::Ok) {
    LOG_INF("LIBIDX", "index rejected: %s", clixValidityName(lastValidity));
    file.close();
    return false;
  }
  opened = true;
  return true;
}

void LibraryIndexFile::close() {
  if (file.isOpen()) file.close();
  opened = false;
}

bool LibraryIndexFile::readAt(const uint32_t offset, void* dst, const size_t len) {
  if (!opened) return false;
  // Every offset handed to this function comes from the header, and the header
  // was validated against the real file size, so a short read means the card
  // changed under us rather than a bad computation.
  if (!file.seekSet(offset) || file.read(dst, len) != static_cast<int>(len)) {
    readFailed = true;
    return false;
  }
  return true;
}

uint16_t LibraryIndexFile::ordinalForRow(const SortOrder order, const uint16_t row) {
  constexpr uint16_t NONE = 0xFFFF;
  if (!opened || row >= head.bookCount) return NONE;

  switch (order) {
    case SortOrder::TitleAsc:
      // The record section IS in title order, so this costs no storage and no
      // read at all.
      return row;
    case SortOrder::TitleDesc:
      return static_cast<uint16_t>(head.bookCount - 1 - row);
    case SortOrder::AuthorAsc:
    case SortOrder::AuthorDesc: {
      const uint16_t k = order == SortOrder::AuthorAsc ? row : static_cast<uint16_t>(head.bookCount - 1 - row);
      uint16_t ordinal = NONE;
      return readAt(authorOrderOffset(head, k), &ordinal, sizeof(ordinal)) && ordinal < head.bookCount ? ordinal : NONE;
    }
    case SortOrder::RecentAsc:
    case SortOrder::RecentDesc: {
      // arrivalOrder runs oldest first, so both directions share one on-disk
      // permutation.
      const uint16_t k = order == SortOrder::RecentAsc ? row : static_cast<uint16_t>(head.bookCount - 1 - row);
      uint16_t ordinal = NONE;
      return readAt(arrivalOrderOffset(head, k), &ordinal, sizeof(ordinal)) && ordinal < head.bookCount ? ordinal
                                                                                                        : NONE;
    }
    case SortOrder::SeriesAsc:
    case SortOrder::SeriesDesc: {
      const uint16_t k = order == SortOrder::SeriesAsc ? row : static_cast<uint16_t>(head.bookCount - 1 - row);
      uint16_t ordinal = NONE;
      return readAt(seriesOrderOffset(head, k), &ordinal, sizeof(ordinal)) && ordinal < head.bookCount ? ordinal
                                                                                                      : NONE;
    }
  }
  return NONE;
}

bool LibraryIndexFile::readOrdinals(const SortOrder order, const uint16_t base, uint16_t* out, const uint16_t count) {
  constexpr uint16_t NONE = 0xFFFF;
  if (!opened || out == nullptr || count == 0 || base >= head.bookCount) return false;
  const uint16_t n = std::min<uint16_t>(count, static_cast<uint16_t>(head.bookCount - base));

  // Title order IS the record section, so these need no read at all -- the same
  // arithmetic ordinalForRow() does one row at a time.
  if (order == SortOrder::TitleAsc) {
    for (uint16_t i = 0; i < n; i++) out[i] = static_cast<uint16_t>(base + i);
    return true;
  }
  if (order == SortOrder::TitleDesc) {
    for (uint16_t i = 0; i < n; i++) out[i] = static_cast<uint16_t>(head.bookCount - 1 - (base + i));
    return true;
  }

  // The permutations are stored oldest/lowest first, so a descending sort walks
  // its index k the other way: one ascending run of the perm serves both
  // directions, and the run is reversed into row order afterwards.
  const bool descending = order == SortOrder::RecentDesc || order == SortOrder::AuthorDesc ||
                          order == SortOrder::SeriesDesc;
  const uint16_t kFirst = descending ? static_cast<uint16_t>(head.bookCount - 1 - base) : base;
  const uint16_t kBase = descending ? static_cast<uint16_t>(kFirst - (n - 1)) : kFirst;

  uint32_t offset = 0;
  switch (order) {
    case SortOrder::AuthorAsc:
    case SortOrder::AuthorDesc:
      offset = authorOrderOffset(head, kBase);
      break;
    case SortOrder::RecentAsc:
    case SortOrder::RecentDesc:
      offset = arrivalOrderOffset(head, kBase);
      break;
    default:
      offset = seriesOrderOffset(head, kBase);
      break;
  }
  if (!readAt(offset, out, static_cast<size_t>(n) * sizeof(uint16_t))) return false;

  if (descending) std::reverse(out, out + n);
  // Same rejection ordinalForRow() applies: an ordinal outside the record
  // section is "no such row", never an index into the card.
  for (uint16_t i = 0; i < n; i++) {
    if (out[i] >= head.bookCount) out[i] = NONE;
  }
  return true;
}

bool LibraryIndexFile::recentRowsFor(const BookIdentity* books, const size_t count, uint16_t* outRows) {
  constexpr uint16_t NONE = 0xFFFF;
  for (size_t i = 0; i < count; i++) outRows[i] = NONE;
  if (!opened || count == 0 || count > MAX_IDENTITY_LOOKUPS || head.bookCount == 0) return opened;

  constexpr size_t CHUNK_RECORDS = 16;  // 4096 bytes, the aligned-tile size
  auto chunk = makeUniqueNoThrow<uint8_t[]>(CHUNK_RECORDS * sizeof(ClixRecord));
  if (!chunk) {
    LOG_ERR("LIBIDX", "OOM: %u-byte lookup chunk", static_cast<unsigned>(CHUNK_RECORDS * sizeof(ClixRecord)));
    return false;
  }

  // Pass 1: record section, matching sizes in the chunk and confirming the few
  // size hits against the stored path hash.
  uint16_t ordinals[MAX_IDENTITY_LOOKUPS];
  for (size_t i = 0; i < count; i++) ordinals[i] = NONE;
  size_t unresolved = count;
  for (uint16_t base = 0; base < head.bookCount && unresolved > 0; base += CHUNK_RECORDS) {
    const uint16_t batch = std::min<uint16_t>(CHUNK_RECORDS, head.bookCount - base);
    if (!readAt(recordOffset(head, base), chunk.get(), batch * sizeof(ClixRecord))) return false;
    for (uint16_t r = 0; r < batch && unresolved > 0; r++) {
      // memcpy, not a cast: the chunk buffer has no alignment guarantee for the
      // record's 32-bit fields.
      ClixRecord record;
      memcpy(&record, chunk.get() + r * sizeof(ClixRecord), sizeof(ClixRecord));
      uint64_t hash = 0;
      bool hashRead = false;
      for (size_t i = 0; i < count; i++) {
        if (ordinals[i] != NONE) continue;
        // Size 0 means the caller could not stat the file (the index handle
        // may be the only reader the card allows); the hash alone decides.
        if (books[i].fileSize != 0 && books[i].fileSize != record.fileSize) continue;
        if (!hashRead) {
          if (record.nameOff > head.nameLen) break;  // unvalidated record; skip it
          if (!readPathHash(record, hash)) return false;
          hashRead = true;
        }
        if (books[i].pathHash == hash) {
          ordinals[i] = base + r;
          unresolved--;
        }
      }
    }
  }

  // Pass 2: arrival permutation, translating matched ordinals to ascending
  // rows. Stops as soon as every match has been placed -- the books in the
  // store are usually near the front of this permutation, and there is nothing
  // to learn from the rest of it.
  const size_t matched = count - unresolved;
  size_t translated = 0;
  for (uint16_t base = 0; base < head.bookCount && translated < matched; base += CHUNK_RECORDS * 2) {
    const uint16_t batch = std::min<uint16_t>(CHUNK_RECORDS * 2, head.bookCount - base);
    if (!readAt(arrivalOrderOffset(head, base), chunk.get(), batch * sizeof(uint16_t))) return false;
    for (uint16_t k = 0; k < batch; k++) {
      uint16_t ordinal;
      memcpy(&ordinal, chunk.get() + k * sizeof(uint16_t), sizeof(uint16_t));
      for (size_t i = 0; i < count; i++) {
        if (ordinals[i] != NONE && outRows[i] == NONE && ordinals[i] == ordinal) {
          outRows[i] = base + k;
          translated++;
        }
      }
    }
  }
  return true;
}

bool LibraryIndexFile::readRecord(const uint16_t ordinal, ClixRecord& out) {
  if (!opened || ordinal >= head.bookCount) return false;
  if (!readAt(recordOffset(head, ordinal), &out, sizeof(out))) return false;
  return sanitize(out);
}

bool LibraryIndexFile::readRecords(const uint16_t base, ClixRecord* out, const uint16_t count) {
  if (!opened || out == nullptr || count == 0) return false;
  if (base >= head.bookCount) return false;
  const uint16_t n = std::min<uint16_t>(count, static_cast<uint16_t>(head.bookCount - base));
  if (!readAt(recordOffset(head, base), out, static_cast<size_t>(n) * sizeof(ClixRecord))) return false;
  for (uint16_t i = 0; i < n; i++) {
    if (!sanitize(out[i])) return false;
  }
  return true;
}

bool LibraryIndexFile::sanitize(ClixRecord& out) const {
  // Clamp here, at the single point every record enters the program. These
  // lengths come off an SD card that the user can write to and that can rot: a
  // foldLen of 255 against a 96-byte field sends a string_view 159 bytes past the
  // end of the record, and callers build views from them without looking. Fixing
  // it at each call site would mean fixing it again at the next one.
  out.foldLen = static_cast<uint8_t>(std::min<size_t>(out.foldLen, CLIX_FOLD_BYTES));
  out.authorFoldLen = static_cast<uint8_t>(std::min<size_t>(out.authorFoldLen, CLIX_AUTHOR_FOLD_BYTES));
  out.seriesFoldLen = static_cast<uint8_t>(std::min<size_t>(out.seriesFoldLen, CLIX_SERIES_KEY_BYTES));
  out.nameFoldLen = static_cast<uint8_t>(std::min<size_t>(out.nameFoldLen, CLIX_NAME_FOLD_BYTES));
  out.authorKeyLen = static_cast<uint8_t>(std::min<size_t>(out.authorKeyLen, CLIX_AUTHOR_KEY_BYTES));
  if (out.metadataStatus > CLIX_METADATA_FAILED) return false;
  // nameOff is u32 and every reader adds a length to it before comparing against
  // the section size. A forged value near the top of the range wraps that sum and
  // passes the bounds check it was supposed to fail, so it is rejected here
  // instead — the one place that can, before any arithmetic touches it.
  if (out.nameOff > head.nameLen) {
    out.nameLen = 0;
    out.nameOff = 0;
  }
  return true;
}

bool LibraryIndexFile::readName(const ClixRecord& record, std::string& out) {
  out.clear();
  if (!opened || record.nameLen == 0) return false;
  if (record.nameOff > head.nameLen || sizeof(uint64_t) > head.nameLen - record.nameOff ||
      record.nameLen > head.nameLen - record.nameOff - sizeof(uint64_t))
    return false;
  out.resize(record.nameLen);
  return readAt(head.nameStart + record.nameOff + sizeof(uint64_t), out.data(), record.nameLen);
}

bool LibraryIndexFile::readPathHash(const ClixRecord& record, uint64_t& out) {
  out = 0;
  if (!opened) return false;
  if (record.nameOff > head.nameLen || sizeof(out) > head.nameLen - record.nameOff) {
    readFailed = true;
    return false;
  }
  return readAt(head.nameStart + record.nameOff, &out, sizeof(out));
}

bool LibraryIndexFile::readBlobField(const ClixRecord& record, const uint8_t field, std::string& out) {
  out.clear();
  if (!opened || record.nameLen == 0) return false;
  if (record.nameOff > head.nameLen || sizeof(uint64_t) > head.nameLen - record.nameOff ||
      record.nameLen > head.nameLen - record.nameOff - sizeof(uint64_t))
    return false;

  uint32_t at = record.nameOff + sizeof(uint64_t) + record.nameLen;
  for (uint8_t i = 0; i <= field; i++) {
    if (at >= head.nameLen) return false;
    uint8_t len = 0;
    if (!readAt(head.nameStart + at, &len, sizeof(len))) return false;
    ++at;
    if (len > head.nameLen - at) return false;
    if (i == field) {
      out.resize(len);
      return len == 0 || readAt(head.nameStart + at, out.data(), len);
    }
    at += len;
  }
  return false;
}

bool LibraryIndexFile::readAuthor(const ClixRecord& record, std::string& out) {
  return readBlobField(record, CLIX_BLOB_AUTHOR, out) && !out.empty();
}

// The book's own title, after the name and the author. Absent (length 0) for a
// book that never told us one, in which case the caller shows the filename.
bool LibraryIndexFile::readTitle(const ClixRecord& record, std::string& out) {
  return readBlobField(record, CLIX_BLOB_TITLE, out) && !out.empty();
}

bool LibraryIndexFile::readSeries(const ClixRecord& record, std::string& out) {
  return readBlobField(record, CLIX_BLOB_SERIES, out) && !out.empty();
}

bool LibraryIndexFile::readSourceAuthor(const ClixRecord& record, std::string& out) {
  return readBlobField(record, CLIX_BLOB_SOURCE_AUTHOR, out);
}

bool LibraryIndexFile::readBlob(const ClixRecord& record, std::string& out) {
  out.clear();
  if (!opened || record.nameLen == 0) return false;
  if (record.nameOff > head.nameLen || sizeof(uint64_t) > head.nameLen - record.nameOff) return false;
  const uint32_t start = record.nameOff + sizeof(uint64_t);
  uint32_t want = static_cast<uint32_t>(record.nameLen) + 4u * 256u;
  const uint32_t remaining = head.nameLen - start;
  if (want > remaining) want = remaining;
  out.resize(want);
  return want == 0 || readAt(head.nameStart + start, out.data(), want);
}

bool clixBlobField(const std::string_view blob, const uint8_t nameLen, const uint8_t field, std::string_view& out) {
  out = {};
  size_t at = nameLen;
  if (at > blob.size()) return false;
  for (uint8_t i = 0; i <= field; i++) {
    if (at >= blob.size()) return false;
    const size_t len = static_cast<uint8_t>(blob[at]);
    ++at;
    if (len > blob.size() - at) return false;
    if (i == field) {
      out = blob.substr(at, len);
      return true;
    }
    at += len;
  }
  return false;
}

bool LibraryIndexFile::readPath(const ClixRecord& record, std::string& out) {
  out.clear();
  if (!opened || record.folderId >= head.folderCount) return false;

  // Folder records are variable length, so reaching folder n means walking the
  // n preceding length bytes. At one seek per folder this is only done when a
  // book is opened or its details are shown, never while paging.
  uint32_t offset = head.folderStart;
  const uint32_t folderEnd = head.folderStart + head.folderLen;
  for (uint16_t i = 0; i <= record.folderId; i++) {
    if (offset >= folderEnd) return false;
    uint8_t pathLen = 0;
    if (!readAt(offset, &pathLen, sizeof(pathLen)) || pathLen == 0) return false;
    if (pathLen > folderEnd - offset - 1u) return false;
    if (i == record.folderId) {
      std::string dir(pathLen, '\0');
      if (!readAt(offset + 1, dir.data(), pathLen)) return false;
      std::string name;
      if (!readName(record, name)) return false;
      out = joinLibraryPath(dir, name);
      return true;
    }
    offset += 1u + pathLen;
    if (offset >= folderEnd) return false;
  }
  return false;
}

}  // namespace library
