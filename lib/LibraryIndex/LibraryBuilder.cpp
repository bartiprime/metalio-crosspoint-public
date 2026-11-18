#include "LibraryBuilder.h"

#include <Arduino.h>
#include <BufferedFile.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "LibraryIndexFile.h"
#include "LibraryPartial.h"
#include "LibraryText.h"

namespace library {
namespace {

constexpr char INDEX_PATH[] = "/.crosspoint/library.idx";
constexpr char NEW_PATH[] = "/.crosspoint/library.new";
constexpr char BACKUP_PATH[] = "/.crosspoint/library.bak";
constexpr char STAGE_PATH[] = "/.crosspoint/library.stage";
constexpr char DIRTY_PATH[] = "/.crosspoint/library.dirty";
// LIB1: the framed journal a cancelled or interrupted walk resumes from.
constexpr char PART_PATH[] = "/.crosspoint/library.part";
constexpr char CACHE_DIR[] = "/.crosspoint";
// Sticky fallback when the marker could not be persisted (card unavailable at
// write time): callers must still see the index as stale until a rebuild clears it.
bool dirtyInMemory = false;
constexpr size_t LIBRARY_IO_BUFFER_SIZE = 4096;

// Matches lib/FileIndex's buffer so a name this walk accepts is one the file
// browser could also show.
constexpr size_t NAME_BUF_SIZE = 512;

// One staged entry: the record as far as the filename can fill it, followed by
// the display name. Fixed stride keeps the second pass a seek rather than a scan.
constexpr size_t STAGE_NAME_BYTES = 255;
constexpr size_t STAGE_AUTHOR_BYTES = 128;
constexpr size_t STAGE_SERIES_BYTES = 128;
// A folder path is stored behind one length byte in the folder section.
constexpr size_t FOLDER_PATH_BYTES = 255;
struct StagedEntry {
  ClixRecord record;
  uint64_t pathHash;
  char name[STAGE_NAME_BYTES];
  // Cleaned source spelling from this book. The spelling actually shown
  // is chosen later, across every book by the same person.
  uint8_t authorLen;
  char author[STAGE_AUTHOR_BYTES];
  // The title the book gives itself, kept SEPARATE from `name`. Writing it into
  // the name slot was a defect: readPath rebuilds a book's file path from that
  // slot, so an enriched book resolved to "/Books/Germinal" and could not be
  // opened, and reconciliation hashed a dirent name on one side against a stored
  // title on the other.
  uint8_t titleLen;
  char title[STAGE_NAME_BYTES];
  // The series the book's own OPF declared, exactly as written. Empty for a
  // standalone — never derived from the title, because a wrong guess is worse
  // than a visible Standalone.
  uint8_t seriesLen;
  char series[STAGE_SERIES_BYTES];
};
constexpr size_t STAGE_STRIDE = sizeof(StagedEntry);

// Sort array element. Holding a 12-byte key prefix rather than the whole fold
// keeps this at 14 bytes per book; ties fall back to the ordinal, so the order
// is total and a rebuild cannot shuffle equal-prefix books between runs.
struct SortKey {
  char key[12];
  uint16_t ordinal;
};
static_assert(sizeof(SortKey) == 14, "SortKey must stay small: it is the only per-book resident cost");

constexpr uint8_t MAX_AUTHOR_SPELLINGS = 16;
struct SpellingSlot {
  char text[STAGE_AUTHOR_BYTES];
  uint16_t ordinal;
  uint16_t count;
  uint8_t len;
};
static_assert(sizeof(SpellingSlot) <= 136, "spelling vote scratch grew unexpectedly");

bool sortKeyLess(const SortKey& a, const SortKey& b) {
  const int cmp = memcmp(a.key, b.key, sizeof(a.key));
  if (cmp != 0) return cmp < 0;
  return a.ordinal < b.ordinal;
}

// Let FreeRTOS run the idle task during every long phase, including builds
// without a UI callback and the sort/emit work after the directory walk. The
// counter keeps the delay out of tight per-byte operations while bounding CPU
// work between yields.
void serviceBuilder(uint32_t& workUnits) {
  if ((++workUnits & 0x1Fu) == 0) delay(1);
}

bool recoverInterruptedInstall() {
  if (!Storage.exists(BACKUP_PATH)) return true;
  if (!Storage.exists(INDEX_PATH)) {
    if (Storage.rename(BACKUP_PATH, INDEX_PATH)) {
      LOG_INF("LIBIDX", "restored previous index after interrupted install");
      return true;
    }
    LOG_ERR("LIBIDX", "cannot restore %s; rebuild deferred", BACKUP_PATH);
    return false;
  }

  // Both names exist when power was lost after the new index became live but
  // before backup cleanup. Validate them one at a time (SdFat has one reader)
  // before deciding which copy is stale.
  LibraryIndexFile candidate;
  if (candidate.open(INDEX_PATH)) {
    candidate.close();
    if (Storage.remove(BACKUP_PATH)) return true;
    LOG_ERR("LIBIDX", "cannot remove stale backup; rebuild deferred");
    return false;
  }
  if (candidate.ioFailed()) {
    LOG_ERR("LIBIDX", "cannot read live index during recovery");
    return false;
  }
  candidate.close();

  if (candidate.open(BACKUP_PATH)) {
    candidate.close();
    if (!Storage.remove(INDEX_PATH) || !Storage.rename(BACKUP_PATH, INDEX_PATH)) {
      LOG_ERR("LIBIDX", "validated backup could not replace an invalid live index");
      return false;
    }
    LOG_INF("LIBIDX", "restored previous index after interrupted install");
    return true;
  }
  if (candidate.ioFailed()) {
    LOG_ERR("LIBIDX", "cannot read backup index during recovery");
    return false;
  }
  candidate.close();

  // Neither file validates. The live path will be preserved until a complete
  // new index is ready; the unusable backup only blocks transactional install.
  if (!Storage.remove(BACKUP_PATH)) {
    LOG_ERR("LIBIDX", "invalid stale backup cannot be removed; rebuild deferred");
    return false;
  }
  return true;
}

bool installNewIndex() {
  const bool hadPrevious = Storage.exists(INDEX_PATH);

  // A backup beside a live index is left by a successful install interrupted
  // before cleanup. It is stale now; remove it before reserving that name for
  // the current previous index.
  if (Storage.exists(BACKUP_PATH) && !Storage.remove(BACKUP_PATH)) {
    LOG_ERR("LIBIDX", "cannot remove stale backup; keeping the live index");
    Storage.remove(NEW_PATH);
    return false;
  }

  if (hadPrevious && !Storage.rename(INDEX_PATH, BACKUP_PATH)) {
    LOG_ERR("LIBIDX", "cannot stage previous index for replacement");
    Storage.remove(NEW_PATH);
    return false;
  }

  if (!Storage.rename(NEW_PATH, INDEX_PATH)) {
    LOG_ERR("LIBIDX", "rename %s -> %s failed", NEW_PATH, INDEX_PATH);
    if (hadPrevious && !Storage.rename(BACKUP_PATH, INDEX_PATH)) {
      // recoverInterruptedInstall() retries this on the next rebuild. Do not
      // remove the backup: it is the only complete index left.
      LOG_ERR("LIBIDX", "previous index rollback failed; backup retained at %s", BACKUP_PATH);
    }
    Storage.remove(NEW_PATH);
    return false;
  }

  if (hadPrevious && !Storage.remove(BACKUP_PATH)) {
    // The new live index is already complete. A stale backup is harmless and is
    // removed before the next replacement attempt.
    LOG_ERR("LIBIDX", "new index installed but stale backup cleanup failed");
  }
  return true;
}

void clearLibraryIndexDirty() {
  dirtyInMemory = false;
  if (Storage.exists(DIRTY_PATH) && !Storage.remove(DIRTY_PATH)) {
    LOG_ERR("LIBIDX", "cannot clear dirty marker");
  }
}

// macOS AppleDouble sidecars and hidden entries. The file browser already hides
// these (FileBrowserActivity isMacOSMetadataEntry); the shelf must agree, or a
// card written on a Mac shows every book twice. isLibraryBookFile() applies the
// same rule to whole paths.
bool isHiddenOrSidecar(const char* name) { return name[0] == '.'; }

// D3: does one directory entry become a BOOK RECORD? The walk and the pre-count
// (LibraryPreCounter) both call this, and that is the whole point: two copies of
// this predicate are two numbers, and the bar would then divide by one while the
// walk filled the other.
//   * the name filter is isLibraryBookFile()'s (extensions, no hidden path);
//   * a zero-size file is skipped by the walk as unreadable and never staged, so a
//     dirent that reads 0 bytes is not a book;
//   * a name or folder path past the staging field widths is skipped likewise.
// A predicate about the DIRENT only: nothing here opens a file or reads a byte of
// one, which is what keeps the pre-count cheap enough to run before the walk.
bool countsAsBook(const std::string_view name, const uint32_t size, const size_t folderPathLen) {
  if (name.empty() || size == 0) return false;
  if (name.size() > STAGE_NAME_BYTES || folderPathLen > FOLDER_PATH_BYTES) return false;
  return isLibraryBookFile(name);
}

std::string stemOf(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  return (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
}

// Fold `text` and write the longest complete UTF-8 prefix that fits `cap`.
// One place, so the title/author/series/filename keys cannot disagree about how
// a string is cut — a cut through the middle of a multi-byte character would
// make a name unfindable by its own last letter.
void storeFold(char* dst, const size_t cap, uint8_t& len, const std::string& text) {
  const std::string folded = fold(text);
  const size_t want = std::min(folded.size(), cap);
  len = static_cast<uint8_t>(utf8SafeTruncateBuffer(folded.data(), static_cast<int>(want)));
  if (len > 0) memcpy(dst, folded.data(), len);
}

struct PriorEntry {
  uint64_t pathHash;
  uint32_t fileSize;
  uint16_t firstSeen;
  uint16_t ordinalAndMatched;
};
static_assert(sizeof(PriorEntry) == 16, "prior reconciliation entries must stay at 16 bytes");

constexpr uint16_t PRIOR_MATCHED = 0x8000;
constexpr uint16_t PRIOR_ORDINAL_MASK = 0x7FFF;
static_assert(CLIX_MAX_RECORDS <= PRIOR_MATCHED, "prior ordinal must fit below the matched bit");

uint16_t priorOrdinal(const PriorEntry& entry) { return entry.ordinalAndMatched & PRIOR_ORDINAL_MASK; }

bool priorMatched(const PriorEntry& entry) { return (entry.ordinalAndMatched & PRIOR_MATCHED) != 0; }

void markPriorMatched(PriorEntry& entry) { entry.ordinalAndMatched |= PRIOR_MATCHED; }

bool priorPathLess(const PriorEntry& a, const PriorEntry& b) {
  return a.pathHash < b.pathHash || (a.pathHash == b.pathHash && priorOrdinal(a) < priorOrdinal(b));
}

bool priorSizeLess(const PriorEntry& a, const PriorEntry& b) {
  return a.fileSize < b.fileSize || (a.fileSize == b.fileSize && priorOrdinal(a) < priorOrdinal(b));
}

uint32_t fnv1a32(const char* data, const size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 16777619u;
  }
  return hash;
}

// Sentinel written into a staged record whose book matched no previous path. A
// second pass decides whether it is a rename or genuinely new.
constexpr uint16_t FIRST_SEEN_UNRESOLVED = 0xFFFF;

// State threaded through the recursive walk. Passed by reference rather than
// captured, so the walk stays a plain function and its stack frame stays small.
struct WalkState {
  HalFile stage;
  serialization::BufferedFileWriter* stageOut = nullptr;
  char* nameBuf = nullptr;
  StagedEntry* stagedEntry = nullptr;
  uint16_t books = 0;
  uint16_t folderId = 0;
  uint32_t folderBytes = 0;
  uint16_t nextFirstSeen = 0;
  uint16_t duplicatesDropped = 0;
  uint16_t unreadableSkipped = 0;
  uint64_t* dedupKeys = nullptr;
  uint16_t activeDedupCount = 0;
  bool dedupDegraded = false;
  bool failed = false;
  bool readMetadata = false;
  LibraryIndexFile* previous = nullptr;
  BuildStats* stats = nullptr;
  uint16_t enriched = 0;
  HalFile folders;  // folder section, staged separately then copied in
  // Books the previous index knew. Empty on a first build, in which case every
  // book is new and gets a fresh firstSeen.
  PriorEntry* prior = nullptr;
  uint16_t priorCount = 0;
  // The smart diff, accumulated as the walk goes. `changed` counts books whose
  // path was known but whose size or modification time moved; `unchanged` the
  // ones that needed no OPF read at all.
  uint16_t unchanged = 0;
  uint16_t changed = 0;
  uint32_t generation = 1;
  uint32_t serviceUnits = 0;
};

// Bound to shownTitle when a book told us nothing. A `std::string()` temporary
// in that ternary would copy the title on every book that DID tell us something,
// because the two branches have different value categories.
const std::string kNoTitle;

int findPrior(WalkState& st, const uint64_t pathHash) {
  serviceBuilder(st.serviceUnits);
  if (st.priorCount == 0) return -1;
  PriorEntry* const end = st.prior + st.priorCount;
  PriorEntry* candidate = std::lower_bound(
      st.prior, end, pathHash, [](const PriorEntry& entry, const uint64_t hash) { return entry.pathHash < hash; });
  while (candidate != end && candidate->pathHash == pathHash) {
    if (!priorMatched(*candidate)) return static_cast<int>(candidate - st.prior);
    ++candidate;
  }
  return -1;
}

// Parent basename and depth are gone with the folder-as-author rule they served:
// nothing about a book's surroundings names its author any more.
[[gnu::noinline]] bool stageRecord(WalkState& st, const std::string& name, const uint32_t fileSize,
                                   const uint16_t folderId, const std::string& fullPath,
                                   const uint32_t modificationTime) {
  StagedEntry& entry = *st.stagedEntry;
  memset(&entry, 0, sizeof(entry));
  // The filename is a fallback for the title and nothing else: no parsing, and
  // never an author. Per review on #2885 -- no other reader parses filenames,
  // and a name pulled out of one by pattern is a guess wearing a fact's clothes.
  std::string title = stemOf(name);
  std::string author;
  std::string series;
  uint16_t seriesTenths = 0;
  bool titleFromBook = false;
  bool authorFromBook = false;

  entry.pathHash = clixPathHash(fullPath.data(), fullPath.size());
  const int priorIndex = findPrior(st, entry.pathHash);

  const bool isXtc = FsHelpers::hasXtcExtension(name);
  const bool extractionExpected = st.readMetadata && (FsHelpers::hasEpubExtension(name) || isXtc);
  const uint8_t expectedStatus = extractionExpected ? CLIX_METADATA_EXTRACTED : CLIX_METADATA_NOT_ATTEMPTED;

  // --- the smart diff, decided here and nowhere else -----------------------
  //
  // Identity is the complete path; freshness is size + modification time. Both
  // are properties of the dirent this walk already opened, so the decision costs
  // nothing. A book that matches on all three is UNCHANGED and its OPF is never
  // opened — that is the whole win of an incremental update.
  //
  // mtime is safe despite the dead RTC: the backup battery is gone, so every file
  // the DEVICE writes carries Jan 1 1980 — a constant, which compares equal to
  // itself. What must never be used is wall-clock AGE ("older than N days"),
  // which would be true of every index forever. Staleness is decided by the
  // generation counter, never by a date.
  //
  // A modification time of 0 means the filesystem would not tell us, and an
  // unknown time cannot prove freshness, so such a book is re-read every time.
  bool reuse = false;
  bool changed = false;
  ClixRecord priorRecord{};
  if (priorIndex >= 0) {
    if (!st.previous->readRecord(priorOrdinal(st.prior[priorIndex]), priorRecord)) {
      st.failed = true;
      return false;
    }
    const bool sameIdentity = st.prior[priorIndex].fileSize == fileSize && modificationTime != 0 &&
                              priorRecord.modificationTime == modificationTime;
    // A record whose metadata was never attempted under this mode cannot answer
    // for this walk, and neither can one from a build with the setting flipped.
    reuse = sameIdentity && st.previous->header().metadataEnabled == st.readMetadata &&
            priorRecord.metadataStatus == expectedStatus;
    changed = !reuse;
  }
  // Folds are DERIVED from strings this build already has: a fold-version bump
  // re-folds the stored title/author/series instead of re-opening the book.
  const bool foldsValid = st.previous == nullptr || st.previous->header().foldVersion == CLIX_FOLD_VERSION;

  if (reuse) {
    entry.record = priorRecord;
    std::string blob;
    if (!st.previous->readBlob(priorRecord, blob)) {
      st.failed = true;
      return false;
    }
    std::string_view field;
    // The SOURCE spelling, not the canonical one: the library-wide spelling vote
    // has to be re-run from what each book actually said, or a rebuild would
    // entrench whatever the previous build chose.
    if (clixBlobField(blob, priorRecord.nameLen, CLIX_BLOB_SOURCE_AUTHOR, field)) author.assign(field);
    if (clixBlobField(blob, priorRecord.nameLen, CLIX_BLOB_TITLE, field) && !field.empty()) {
      title.assign(field);
      titleFromBook = true;
    } else {
      title = stemOf(name);
    }
    if (clixBlobField(blob, priorRecord.nameLen, CLIX_BLOB_SERIES, field)) series.assign(field);
    seriesTenths = priorRecord.seriesIndex;
    authorFromBook = !author.empty();
    st.stats->metadataReused++;
    st.unchanged++;
  } else if (priorIndex >= 0) {
    st.changed++;
  }

  // Prefer the reader's existing cache. For an unopened book, loadMetadata()
  // reuses the same EPUB parser but stops before the manifest, so this never
  // builds spine, TOC, CSS, cover, or section caches during the library walk.
  if (!reuse && extractionExpected) {
    st.stats->parsed++;
    std::string bookTitle;
    bool loaded = false;
    if (isXtc) {
      // XTC/XTCH carry title and author in their own header block; load() reads that and the
      // page table header, and writes no cache.
      Xtc xtc(fullPath, CACHE_DIR);
      loaded = xtc.load();
      if (loaded) {
        bookTitle = xtc.getTitle();
        author = xtc.getAuthor();
      }
    } else {
      // The series-aware form, which also reaches the OPF when the reader's
      // metadata cache already has the title: that cache carries no series.
      Epub epub(fullPath, CACHE_DIR);
      Epub::SeriesInfo info;
      loaded = epub.loadMetadata(bookTitle, author, info);
      series = info.name;
      seriesTenths = info.tenths;
      if (!series.empty()) {
        // Calibre writes the series with the same trailing noise an exporter
        // leaves on a name; strip it the same way, but never re-order the words —
        // "The Wheel of Time" is not "Time, The Wheel".
        series = cleanPersonName(series);
      }
    }
    if (loaded) {
      entry.record.metadataStatus = CLIX_METADATA_EXTRACTED;
      if (!bookTitle.empty()) {
        title = std::move(bookTitle);
        titleFromBook = true;
      }
      authorFromBook = !author.empty();
    } else {
      entry.record.metadataStatus = CLIX_METADATA_FAILED;
      series.clear();
      seriesTenths = 0;
    }
    if (!titleFromBook && !authorFromBook) LOG_DBG("LIBIDX", "no metadata for %s", fullPath.c_str());
  }
  if (!series.empty()) st.stats->seriesBooks++;
  // A series index without a series names nothing.
  if (series.empty()) seriesTenths = 0;

  // Exporters write "Unknown" into dc:creator often enough that treating it as
  // a person would put a fictional author at the top of the shelf. fold() already
  // lowercases and trims, so recognising it is the one comparison @Uri-Tauber
  // asked it to cost.
  if (!author.empty() && fold(author) == "unknown") {
    author.clear();
    authorFromBook = false;
  }

  if (titleFromBook || authorFromBook) st.enriched++;

  entry.record.fileSize = fileSize;
  entry.record.modificationTime = modificationTime;
  entry.record.seriesIndex = seriesTenths;

  // Reuse the arrival order this book already had. Without this every rebuild
  // renumbers the whole library in disk-walk order, and "Recently added" silently
  // becomes "whatever order the card enumerates in".
  if (priorIndex >= 0) {
    markPriorMatched(st.prior[priorIndex]);
    entry.record.firstSeen = st.prior[priorIndex].firstSeen;
  } else {
    // Might be a rename rather than a new book; resolved after the walk, when
    // the set of genuinely unmatched previous entries is known.
    entry.record.firstSeen = FIRST_SEEN_UNRESOLVED;
  }
  entry.record.folderId = folderId;
  // In range: walk() skips names longer than STAGE_NAME_BYTES before staging.
  // readPath() rebuilds the file path from this slot, so a clamp here would
  // stage a row that renders but cannot open.
  entry.record.nameLen = static_cast<uint8_t>(name.size());
  // Only stored when the book actually told us something; otherwise the row falls
  // back to the filename and nothing is duplicated.
  const std::string& shownTitle = titleFromBook ? title : kNoTitle;
  entry.titleLen = static_cast<uint8_t>(std::min<size_t>(shownTitle.size(), STAGE_NAME_BYTES));
  if (entry.titleLen > 0) memcpy(entry.title, shownTitle.data(), entry.titleLen);

  if (!reuse || !foldsValid) {
    // The four search keys, folded ONCE at build time. A query then never calls
    // fold() on an author, a title or a filename — at 1,204 books that was the
    // worst case in the old search, which folded every non-matching row.
    storeFold(entry.record.fold, CLIX_FOLD_BYTES, entry.record.foldLen, title);
    storeFold(entry.record.authorFold, CLIX_AUTHOR_FOLD_BYTES, entry.record.authorFoldLen, author);
    storeFold(entry.record.seriesFold, CLIX_SERIES_KEY_BYTES, entry.record.seriesFoldLen, series);
    // The filename WITHOUT its extension: "epub" is in every name and would
    // otherwise match a query for those four letters on the whole library.
    storeFold(entry.record.nameFold, CLIX_NAME_FOLD_BYTES, entry.record.nameFoldLen, stemOf(name));
  }
  if (!reuse || !foldsValid) {
    const std::string key = authorKey(author);
    entry.record.authorKeyLen = static_cast<uint8_t>(std::min(key.size(), CLIX_AUTHOR_KEY_BYTES));
    memcpy(entry.record.authorKey, key.data(), entry.record.authorKeyLen);
  }

  memcpy(entry.name, name.data(), entry.record.nameLen);

  const std::string displayAuthor = cleanPersonName(author);
  entry.authorLen = static_cast<uint8_t>(std::min(displayAuthor.size(), STAGE_AUTHOR_BYTES));
  memcpy(entry.author, displayAuthor.data(), entry.authorLen);

  entry.seriesLen = static_cast<uint8_t>(std::min(series.size(), STAGE_SERIES_BYTES));
  if (entry.seriesLen > 0) memcpy(entry.series, series.data(), entry.seriesLen);

  st.stageOut->write(&entry, STAGE_STRIDE);
  st.books++;
  return true;
}


// One resumable directory frame.
//
// The walk used to be recursion; a stepper has to stop between two books and
// keep its place, so the recursion becomes an explicit stack holding exactly
// what each stack frame held: the path, where the directory's read cursor is,
// the duplicate-key base to restore on return, whether this directory has been
// given a folder id yet, and its depth.
//
// Only ONE directory handle is open at a time, as before: descending closes the
// parent and remembers its cursor, returning reopens and seeks. That matters on
// a C3, where every open SdFat file carries a sector buffer.
struct WalkFrame {
  std::string path;
  uint32_t resumePos = 0;
  uint16_t dedupBase = 0;
  uint16_t folderId = 0;
  uint8_t depth = 0;
  bool folderEmitted = false;
};


// Shared by the offset and write passes so the name-blob layout has one source of
// truth.
uint32_t blobBytesFor(const StagedEntry& entry, const StagedEntry& canonical) {
  return sizeof(entry.pathHash) + entry.record.nameLen + 1u + canonical.authorLen + 1u + entry.titleLen + 1u +
         entry.authorLen + 1u + entry.seriesLen;
}

// Series order: group by the folded series the book declared, then by its
// position in that series, then by title (the record section's own order). Books
// that declare no series carry an all-0xFF key, so the Standalone run sorts last
// in Asc and first in Desc — a real bucket with a visible heading, never a
// series called "".
struct SeriesKey {
  char key[12];
  uint16_t index;
  uint16_t ordinal;
};
static_assert(sizeof(SeriesKey) == 16, "series sort element must stay at 16 bytes");

bool seriesKeyLess(const SeriesKey& a, const SeriesKey& b) {
  const int cmp = memcmp(a.key, b.key, sizeof(a.key));
  if (cmp != 0) return cmp < 0;
  if (a.index != b.index) return a.index < b.index;
  return a.ordinal < b.ordinal;
}

bool emitIndex(const char* folderStagePath, WalkState& st, const uint16_t* order, const uint16_t* resolvedFirstSeen,
               const bool coreSortsAvailable, BuildStats& stats) {
  const uint16_t n = st.books;
  uint32_t serviceUnits = 0;

  auto arrivalOrder = makeUniqueNoThrow<uint16_t[]>(n == 0 ? 1 : n);
  if (!arrivalOrder) {
    LOG_ERR("LIBIDX", "arrival order array alloc failed");
    return false;
  }

  ClixHeader header{};
  memcpy(header.magic, CLIX_MAGIC, sizeof(CLIX_MAGIC));
  header.formatVersion = CLIX_FORMAT_VERSION;
  header.foldVersion = CLIX_FOLD_VERSION;
  header.bookCount = n;
  header.folderCount = st.folderId;
  header.nextFirstSeen = st.nextFirstSeen;
  header.metadataEnabled = st.readMetadata;
  setClixGeneration(header, st.generation);
  // Placeholder only. Degradations are known after the sorts have run.
  header.flags = 0;
  // The blob is the LAST section, so its size affects only selfSize — every
  // section offset is already fixed by the counts. Lay out with a placeholder
  // and correct selfSize once the blob has actually been written, since the
  // author spelling each record ends up carrying is not known until the
  // one-spelling-per-person pass has run.
  layoutSections(header, st.folderBytes, 0);

  HalFile stage;
  HalFile out;
  if (!Storage.openFileForRead("LIBIDX", STAGE_PATH, stage)) return false;
  if (!Storage.openFileForWrite("LIBIDX", NEW_PATH, out)) {
    stage.close();
    return false;
  }
  serialization::BufferedFileWriter outBuffer(out, LIBRARY_IO_BUFFER_SIZE);

  // Returns false rather than spinning. A full card makes write() return 0, and
  // the old loop never advanced past it — the device simply hung mid-rebuild with
  // no message, which is worse than any error.
  bool ioFailed = false;
  // Every final-index write goes through here. A short write on a full card
  // leaves a file that still passes the header check when the header describes
  // what was intended rather than what landed.
  const auto put = [&outBuffer, &ioFailed](const void* data, const size_t len) {
    if (ioFailed) return;
    outBuffer.write(data, len);
  };
  const auto padTo = [&outBuffer, &ioFailed, &serviceUnits](const uint32_t target) {
    if (ioFailed) return;
    static const uint8_t zeros[64] = {0};
    while (outBuffer.position() < target) {
      serviceBuilder(serviceUnits);
      const uint32_t gap = target - static_cast<uint32_t>(outBuffer.position());
      const size_t want = std::min<uint32_t>(gap, sizeof(zeros));
      outBuffer.write(zeros, want);
    }
  };
  const auto readStageAt = [&stage, &ioFailed](const uint64_t offset, void* data, const size_t len) {
    if (ioFailed) return false;
    if (!stage.seekSet(offset) || stage.read(reinterpret_cast<uint8_t*>(data), len) != static_cast<int>(len)) {
      LOG_ERR("LIBIDX", "record stage read failed at %u", static_cast<unsigned>(offset));
      ioFailed = true;
      return false;
    }
    return true;
  };

  // Header placeholder; rewritten below once the sorts have run.
  put(&header, sizeof(header));
  padTo(header.folderStart);

  {
    HalFile folders;
    if (Storage.openFileForRead("LIBIDX", folderStagePath, folders)) {
      uint8_t buf[256];
      uint32_t copied = 0;
      // read() returns int: a -1 error must fail the emit, not wrap into a
      // huge unsigned length.
      int got = 0;
      while ((got = folders.read(buf, sizeof(buf))) > 0) {
        serviceBuilder(serviceUnits);
        put(buf, static_cast<size_t>(got));
        copied += static_cast<uint32_t>(got);
      }
      if (got < 0) ioFailed = true;
      folders.close();
      if (copied != st.folderBytes) {
        LOG_ERR("LIBIDX", "folder stage truncated: read %u of %u bytes", static_cast<unsigned>(copied),
                static_cast<unsigned>(st.folderBytes));
        ioFailed = true;
      }
    } else {
      // Ignoring this would publish an all-zero folder section: selfSize still
      // matches, so the index validates, and readPath() then fails for every
      // book with nothing left to trigger a self-repair.
      LOG_ERR("LIBIDX", "folder stage unreadable: %s", folderStagePath);
      ioFailed = true;
    }
  }
  padTo(header.recordStart);
  if (ioFailed) {
    LOG_ERR("LIBIDX", "emit failed while copying the folder stage");
    outBuffer.flush();
    stage.close();
    out.close();
    Storage.remove(NEW_PATH);
    return false;
  }

  // Author order has to be known BEFORE the records are written because its
  // permutation section is emitted first.
  // The title key array is already gone before this phase. At the 4096-record
  // ceiling this checked, phase-local allocation is 57,344 bytes.
  const bool rankable = coreSortsAvailable;
  if (rankable && n > 1) {
    LOG_DBG("LIBIDX", "author sort alloc: %u bytes, heap %u, max block %u", static_cast<unsigned>(n * sizeof(SortKey)),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }
  auto authorSort = rankable && n > 1 ? makeUniqueNoThrow<SortKey[]>(n) : nullptr;
  if (authorSort) {
    for (uint16_t i = 0; i < n; i++) {
      serviceBuilder(serviceUnits);
      ClixRecord r{};
      if (!readStageAt(static_cast<uint64_t>(order[i]) * STAGE_STRIDE, &r, sizeof(r))) break;
      if (r.authorKeyLen == 0) {
        // 0xFF outranks every folded byte, so unknown authors land at the end.
        memset(authorSort[i].key, 0xFF, sizeof(authorSort[i].key));
      } else {
        memset(authorSort[i].key, 0, sizeof(authorSort[i].key));
        memcpy(authorSort[i].key, r.authorKey, std::min<size_t>(r.authorKeyLen, sizeof(authorSort[i].key)));
      }
      authorSort[i].ordinal = i;
    }
    if (!ioFailed) {
      if (n > 1) {
        delay(1);
        std::sort(authorSort.get(), authorSort.get() + n, sortKeyLess);
        delay(1);
      }
    }
  } else if (n > 1) {
    stats.ranksDegraded = true;
  }

  // --- one spelling per person --------------------------------------------
  //
  // The author KEY already merges "Xun, Lu", "Lu Xun_" and
  // "Lu Xun [Xun, Lu]" into one identity, because its tokens are
  // sorted. The displayed STRING is still whatever each filename happened to
  // carry, so one person appears under several spellings in the same list.
  //
  // Fix: within each key group show the spelling that occurs most often, ties
  // broken by the shortest and then alphabetically. It never invents or reorders
  // a name — it picks one of the strings that actually exist — which is what
  // keeps "Lu Xun" and "Natsume Soseki" safe from a forename/surname rule
  // that would confidently get them backwards.
  //
  // authorSort is already grouped: books by one person are contiguous in it. So
  // this is one walk over the runs, holding only the current run's spellings.
  std::unique_ptr<uint16_t[]> canonicalFrom;
  std::unique_ptr<SpellingSlot[]> spellingScratch;
  if (authorSort && n > 1) {
    canonicalFrom = makeUniqueNoThrow<uint16_t[]>(n);
  }
  if (canonicalFrom) {
    for (uint16_t i = 0; i < n; i++) {
      serviceBuilder(serviceUnits);
      canonicalFrom[i] = i;
    }
    spellingScratch = makeUniqueNoThrow<SpellingSlot[]>(MAX_AUTHOR_SPELLINGS);
  } else if (authorSort && n > 1) {
    LOG_ERR("LIBIDX", "canonical author array alloc failed; author order degraded");
    stats.ranksDegraded = true;
  }
  if (canonicalFrom && !spellingScratch) {
    LOG_ERR("LIBIDX", "author spelling scratch alloc failed; spelling harmonisation skipped");
    stats.ranksDegraded = true;
  }
  if (!ioFailed && canonicalFrom && spellingScratch && authorSort && n > 1) {
    uint16_t runStart = 0;
    while (runStart < n) {
      serviceBuilder(serviceUnits);
      uint16_t runEnd = runStart + 1;
      while (runEnd < n &&
             memcmp(authorSort[runEnd].key, authorSort[runStart].key, sizeof(authorSort[runStart].key)) == 0) {
        serviceBuilder(serviceUnits);
        runEnd++;
      }
      // A run of one has nothing to reconcile, and the unknown-author run (key
      // all 0xFF) must not be collapsed onto one arbitrary empty string.
      const bool unknownRun = static_cast<unsigned char>(authorSort[runStart].key[0]) == 0xFF;
      if (!unknownRun && runEnd - runStart > 1) {
        // Each author read ONCE, then counted in RAM. The first version re-read
        // the whole run for every member of it — k² reads of 768 bytes for a
        // number that k reads can produce — and an author with twenty books cost
        // four hundred SD reads to decide one string.
        //
        // Bounded by DISTINCT spellings rather than by run length, which is the
        // point: one person has two or three spellings on a real card, however
        // many books they wrote, so this holds a handful of short strings instead
        // of one per book.
        uint8_t spellingCount = 0;

        for (uint16_t a = runStart; a < runEnd; a++) {
          serviceBuilder(serviceUnits);
          const uint16_t ord = authorSort[a].ordinal;
          uint8_t len = 0;
          if (!readStageAt(static_cast<uint64_t>(order[ord]) * STAGE_STRIDE + offsetof(StagedEntry, authorLen), &len,
                           sizeof(len)))
            break;
          if (len == 0) continue;

          char buf[STAGE_AUTHOR_BYTES];
          const size_t want = std::min<size_t>(len, sizeof(buf));
          if (!readStageAt(static_cast<uint64_t>(order[ord]) * STAGE_STRIDE + offsetof(StagedEntry, author), buf, want))
            break;
          bool merged = false;
          for (uint8_t i = 0; i < spellingCount; i++) {
            SpellingSlot& sp = spellingScratch[i];
            if (sp.len == want && memcmp(sp.text, buf, want) == 0) {
              sp.count++;
              merged = true;
              break;
            }
          }
          // A hard cap so a card full of near-identical spellings cannot grow this
          // without bound. Sixteen is far past anything real; beyond it the vote
          // simply decides among the first sixteen.
          if (!merged && spellingCount < MAX_AUTHOR_SPELLINGS) {
            SpellingSlot& sp = spellingScratch[spellingCount++];
            memcpy(sp.text, buf, want);
            sp.ordinal = ord;
            sp.count = 1;
            sp.len = static_cast<uint8_t>(want);
          }
        }

        uint16_t bestOrdinal = authorSort[runStart].ordinal;
        int bestScore = -1;
        size_t bestLen = 0;
        const char* bestText = nullptr;
        for (uint8_t i = 0; i < spellingCount; i++) {
          const SpellingSlot& sp = spellingScratch[i];
          const bool better = sp.count > bestScore || (sp.count == bestScore && sp.len < bestLen) ||
                              (sp.count == bestScore && sp.len == bestLen &&
                               (bestText == nullptr || memcmp(sp.text, bestText, sp.len) < 0));
          if (better) {
            bestScore = sp.count;
            bestLen = sp.len;
            bestText = sp.text;
            bestOrdinal = sp.ordinal;
          }
        }
        for (uint16_t a = runStart; a < runEnd; a++) {
          serviceBuilder(serviceUnits);
          canonicalFrom[authorSort[a].ordinal] = bestOrdinal;
        }
      }
      runStart = runEnd;
    }
  }

  // --- re-sort by surname --------------------------------------------------
  //
  // The pass above had to run in authorKey order, because that is what puts one
  // author's books in a single run for the spelling vote. But authorKey sorts a
  // name's WORDS — the property that lets "Victor Hugo" and "Hugo Victor" be
  // recognised as one person — so ordering by it files Herman Melville under B.
  //
  // Now that every book carries its canonical display name, the shelf is ordered
  // by surname, as a library would. Keying off the canonical name rather than the
  // raw one is what keeps a group whole: all of a group's books resolve to the
  // same string, so they cannot split across two places.
  if (!ioFailed && authorSort && canonicalFrom && n > 1) {
    for (uint16_t i = 0; i < n; i++) {
      serviceBuilder(serviceUnits);
      // canonicalFrom holds TITLE-order positions, and the staging file is keyed
      // by walk order — order[] is the map between them. Reading staging with the
      // title position directly fetches an unrelated book, which is what split
      // John Scalzi into two groups and left the shelf in no order at all.
      const uint16_t src = order[canonicalFrom[i]];
      uint8_t authorLen = 0;
      char author[STAGE_AUTHOR_BYTES] = {};
      if (!readStageAt(static_cast<uint64_t>(src) * STAGE_STRIDE + offsetof(StagedEntry, authorLen), &authorLen,
                       sizeof(authorLen)))
        break;
      if (authorLen > 0) {
        if (!readStageAt(static_cast<uint64_t>(src) * STAGE_STRIDE + offsetof(StagedEntry, author), author,
                         std::min<size_t>(authorLen, sizeof(author))))
          break;
      }

      const std::string key = authorLen == 0 ? std::string() : surnameKey(std::string_view(author, authorLen));
      if (key.empty()) {
        // 0xFF outranks every folded byte, so unknown authors stay at the end.
        memset(authorSort[i].key, 0xFF, sizeof(authorSort[i].key));
      } else {
        memset(authorSort[i].key, 0, sizeof(authorSort[i].key));
        memcpy(authorSort[i].key, key.data(), std::min(key.size(), sizeof(authorSort[i].key)));
      }
      authorSort[i].ordinal = i;
    }
    if (!ioFailed) {
      delay(1);
      std::sort(authorSort.get(), authorSort.get() + n, sortKeyLess);
      delay(1);
    }
  }

  // --- arrival order -------------------------------------------------------
  //
  // Primary key is the file's modification time, so the "Recent" shelf reflects
  // when a book actually landed on the card rather than when a rebuild happened
  // to discover it. firstSeen breaks ties (and carries books whose filesystem
  // reports no time): it comes from the PREVIOUS index, so it is no longer a
  // dense sequence in walk order — a rebuild reuses each book's original number
  // and only hands out new ones for books it has never seen. The arrival order
  // has to be SORTED rather than assumed, or "Recent" silently degrades into
  // "the order the card enumerates in" — which is exactly the bug
  // reconciliation exists to prevent.
  if (rankable) {
    for (uint16_t i = 0; i < n; i++) arrivalOrder[i] = i;
    if (n > 1) {
      // Fallible and non-fatal: without the array the sort still runs on
      // firstSeen alone, which is the pre-timestamp behaviour.
      auto mtimes = makeUniqueNoThrow<uint32_t[]>(n);
      if (mtimes) {
        for (uint16_t i = 0; i < n; i++) {
          serviceBuilder(serviceUnits);
          if (!readStageAt(static_cast<uint64_t>(order[i]) * STAGE_STRIDE + offsetof(ClixRecord, modificationTime),
                           &mtimes[i], sizeof(mtimes[i]))) {
            mtimes.reset();
            break;
          }
        }
      } else {
        LOG_ERR("LIBIDX", "OOM: %u-byte mtime array, arrival falls back to firstSeen",
                static_cast<unsigned>(n * sizeof(uint32_t)));
      }
      if (ioFailed) {
        // The shared read helper latched the failure; the emit fails below.
      } else {
        delay(1);
        std::sort(arrivalOrder.get(), arrivalOrder.get() + n,
                  [order, resolvedFirstSeen, mt = mtimes.get()](const uint16_t a, const uint16_t b) {
                    if (mt && mt[a] != mt[b]) return mt[a] < mt[b];
                    const uint16_t aSeen = resolvedFirstSeen[order[a]];
                    const uint16_t bSeen = resolvedFirstSeen[order[b]];
                    return aSeen < bSeen || (aSeen == bSeen && a < b);
                  });
        delay(1);
      }
    }
  } else {
    // Without sorting, preserve walk order by mapping each staging ordinal back
    // to its position in the title-ordered record section.
    for (uint16_t i = 0; i < n; i++) {
      serviceBuilder(serviceUnits);
      arrivalOrder[order[i]] = i;
    }
    LOG_INF("LIBIDX", "%u books: sort allocation unavailable; index kept in walk order", static_cast<unsigned>(n));
    stats.ranksDegraded = true;
  }

  if (ioFailed) {
    LOG_ERR("LIBIDX", "emit failed while reading the record stage");
    outBuffer.flush();
    stage.close();
    out.close();
    Storage.remove(NEW_PATH);
    return false;
  }

  // Records, in title order, with both ranks and the name offset filled in.
  //
  // Name offsets follow the title order used to emit the name blob below.
  // One pair of staging buffers on the heap, reused by both emit loops. As
  // locals they were 768 bytes each, so 1.5 KB of stack inside a function running
  // on a 4 KB task — the kind of margin that survives a test library and fails on
  // someone else's. On the heap the allocation is checked; on the stack an
  // overflow is a silent corruption.
  auto staged = makeUniqueNoThrow<StagedEntry[]>(2);
  if (!staged) {
    LOG_ERR("LIBIDX", "staging buffers alloc failed");
    outBuffer.flush();
    out.close();
    Storage.remove(NEW_PATH);
    return false;
  }
  StagedEntry& entry = staged[0];
  StagedEntry& canonical = staged[1];

  // put()'s rule, applied to the reads. A short read leaves the previous book's
  // bytes in the buffer, so the emit would write a duplicate row — and since the
  // duplicate is internally consistent, written == selfSize still holds and the
  // corrupt index would pass validation.
  const auto fetch = [&readStageAt](const uint16_t stagingIndex, StagedEntry& dest) {
    return readStageAt(static_cast<uint64_t>(stagingIndex) * STAGE_STRIDE, &dest, STAGE_STRIDE);
  };

  uint32_t nameCursor = 0;
  for (uint16_t i = 0; i < n; i++) {
    serviceBuilder(serviceUnits);
    if (!fetch(order[i], entry)) break;
    entry.record.nameOff = nameCursor;
    // The blob holds the path hash, basename, chosen author spelling, title, and
    // source author spelling used by later rebuilds.
    // Keeping them adjacent means no second offset has to live in the record.
    const uint16_t from = canonicalFrom ? canonicalFrom[i] : i;
    if (!fetch(order[from], canonical)) break;
    nameCursor += blobBytesFor(entry, canonical);
    if (resolvedFirstSeen) entry.record.firstSeen = resolvedFirstSeen[order[i]];
    put(&entry.record, sizeof(ClixRecord));
  }
  padTo(header.permStart);

  for (uint16_t k = 0; k < n; k++) {
    serviceBuilder(serviceUnits);
    const uint16_t ordinal = authorSort ? authorSort[k].ordinal : k;
    put(&ordinal, sizeof(ordinal));
  }
  for (uint16_t k = 0; k < n; k++) {
    serviceBuilder(serviceUnits);
    const uint16_t ordinal = arrivalOrder[k];
    put(&ordinal, sizeof(ordinal));
  }

  // Series permutation, written last so the author sort array can be released
  // first: the peak here is one 16-byte element per book, not two.
  authorSort.reset();
  {
    std::unique_ptr<SeriesKey[]> seriesSort;
    if (rankable && n > 0) seriesSort = makeUniqueNoThrow<SeriesKey[]>(n);
    if (seriesSort) {
      for (uint16_t i = 0; i < n; i++) {
        serviceBuilder(serviceUnits);
        ClixRecord r{};
        if (!readStageAt(static_cast<uint64_t>(order[i]) * STAGE_STRIDE, &r, sizeof(r))) break;
        if (r.seriesFoldLen == 0) {
          // 0xFF outranks every folded byte: no series sorts to the end of Asc.
          memset(seriesSort[i].key, 0xFF, sizeof(seriesSort[i].key));
        } else {
          memset(seriesSort[i].key, 0, sizeof(seriesSort[i].key));
          memcpy(seriesSort[i].key, r.seriesFold, std::min<size_t>(r.seriesFoldLen, sizeof(seriesSort[i].key)));
        }
        seriesSort[i].index = r.seriesIndex;
        seriesSort[i].ordinal = i;
      }
      if (!ioFailed && n > 1) {
        delay(1);
        std::sort(seriesSort.get(), seriesSort.get() + n, seriesKeyLess);
        delay(1);
      }
    } else if (rankable && n > 1) {
      LOG_ERR("LIBIDX", "series sort alloc failed; series order degraded to title order");
      stats.ranksDegraded = true;
    }
    for (uint16_t k = 0; k < n; k++) {
      serviceBuilder(serviceUnits);
      const uint16_t ordinal = seriesSort ? seriesSort[k].ordinal : k;
      put(&ordinal, sizeof(ordinal));
    }
  }
  padTo(header.nameStart);

  uint32_t blobWritten = 0;
  for (uint16_t i = 0; i < n; i++) {
    serviceBuilder(serviceUnits);
    if (!fetch(order[i], entry)) break;
    put(&entry.pathHash, sizeof(entry.pathHash));
    put(entry.name, entry.record.nameLen);

    const uint16_t from = canonicalFrom ? canonicalFrom[i] : i;
    if (!fetch(order[from], canonical)) break;
    put(&canonical.authorLen, 1);
    if (canonical.authorLen > 0) put(canonical.author, canonical.authorLen);
    put(&entry.titleLen, 1);
    if (entry.titleLen > 0) put(entry.title, entry.titleLen);
    put(&entry.authorLen, 1);
    if (entry.authorLen > 0) put(entry.author, entry.authorLen);
    put(&entry.seriesLen, 1);
    if (entry.seriesLen > 0) put(entry.series, entry.seriesLen);
    blobWritten += blobBytesFor(entry, canonical);
  }
  header.nameLen = blobWritten;
  header.selfSize = header.nameStart + blobWritten;
  stage.close();

  // Captured HERE, at the end of the data, and not after the header rewrite
  // below: that rewrite seeks back to 0, so asking afterwards reports 64 — the
  // header's own length — and every rebuild looks truncated.
  const uint32_t written = static_cast<uint32_t>(outBuffer.position());
  if (!outBuffer.flush()) ioFailed = true;

  header.flags =
      (stats.ranksDegraded ? CLIX_FLAG_RANKS_DEGRADED : 0) | (stats.dedupDegraded ? CLIX_FLAG_DEDUP_DEGRADED : 0);

  if (!out.seekSet(0)) {
    ioFailed = true;
  } else if (out.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) != sizeof(header))
    ioFailed = true;
  // The file is only as long as it claims if every write landed. A full card
  // fails them silently, and the result passes the header check while carrying
  // zeros — an index that looks valid and is not.
  const bool sizeMatches = written == header.selfSize;
  const bool closed = out.close();

  if (ioFailed || !sizeMatches || !closed) {
    LOG_ERR("LIBIDX", "emit incomplete (I/O %s, close %s, size %u vs %u) — keeping the old index",
            ioFailed ? "failed" : "ok", closed ? "ok" : "failed", static_cast<unsigned>(written),
            static_cast<unsigned>(header.selfSize));
    // Leave the previous index alone. A shelf that is a rebuild out of date is
    // worth incomparably more than none at all, and a full card is exactly when
    // the reader can least afford to lose it.
    if (closed) Storage.remove(NEW_PATH);
    return false;
  }

  // Rename last. The previous index moves to a recoverable backup until the new
  // file owns the live path; a failed rename rolls it back instead of deleting
  // the only usable shelf.
  return installNewIndex();
}

}  // namespace

const char* libraryIndexPath() { return INDEX_PATH; }

bool isLibraryBookFile(const std::string_view path) {
  // The walk skips hidden (dot-prefixed) names, so it never lists a file inside
  // a hidden folder. A lone "." names the folder itself.
  size_t start = 0;
  while (start < path.size()) {
    const size_t slash = path.find('/', start);
    const std::string_view name = path.substr(start, slash - start);
    if (name.size() > 1 && name.front() == '.') return false;
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return FsHelpers::checkFileExtension(path, ".epub") || FsHelpers::checkFileExtension(path, ".txt") ||
         FsHelpers::checkFileExtension(path, ".md") || FsHelpers::hasXtcExtension(path);
}

bool markLibraryIndexDirty() {
  if (Storage.exists(DIRTY_PATH)) return true;
  if (!Storage.exists(CACHE_DIR) && !Storage.mkdir(CACHE_DIR)) {
    LOG_ERR("LIBIDX", "cannot create cache directory for dirty marker");
    dirtyInMemory = true;  // not persisted: stay dirty until the next rebuild
    return true;
  }
  HalFile marker;
  if (!Storage.openFileForWrite("LIBIDX", DIRTY_PATH, marker)) {
    LOG_ERR("LIBIDX", "cannot create dirty marker");
    dirtyInMemory = true;
    return true;
  }
  return true;
}

bool isLibraryIndexDirty() { return dirtyInMemory || Storage.exists(DIRTY_PATH); }


// The stepper.
//
// One blocking call became a machine with one yield point: after a book has been
// both staged AND committed to the journal. Never mid-parse (an EPUB read is the
// expensive part and half of one has no meaning), never mid-write (a torn frame
// costs the whole resume), and never inside the sort/emit phase (that phase is
// seconds, and it is the only place the live index changes).

struct LibraryIndexStepper::Impl {
  enum class Phase : uint8_t { Idle, Scanning, Finalizing, AwaitingCommit, Finished, Failed, Cancelled };

  std::string rootPath;
  bool readMetadata = false;
  // LIB1-L2: stop at the diff and let the reader decide before anything is
  // promoted. Update uses it; Rebuild and Repair force, so they pass false.
  bool confirmBeforeCommit = false;
  Phase phase = Phase::Idle;
  BuildStats stats;
  BuildProgress progress;
  bool cancelRequested = false;
  bool paused = false;
  // True when reconciliation found nothing to do (byte-identical rebuild).
  bool nothingToDo = false;
  uint32_t startMs = 0;
  uint16_t resumedFallback = 0;

  // The arrival number each staged book ends up with. Reconciled in reconcile()
  // and consumed by emitIndex(), so it outlives one step.
  std::unique_ptr<uint16_t[]> resolvedFirstSeen;

  LibraryPartial journal;
  PartIdentity identity;
  uint32_t generation = 1;

  // One directory handle at a time, one staging writer, one folder stage file.
  // These live in WalkState because the staging/folder files are what the walk
  // writes into; opening them anywhere else leaves two copies of the handle.
  std::string folderStagePath;
  std::unique_ptr<serialization::BufferedFileWriter> stageOut;
  WalkState st;

  WalkFrame frames[LIBRARY_MAX_DEPTH + 2];
  int depth = -1;
  HalFile dir;
  bool dirOpen = false;
  uint16_t foldersMatched = 0;
  bool restartRequested = false;
  // The resumed journal already holds a complete walk; nothing left to scan.
  bool skipScan = false;
  // A-P1-5: the walk stopped at CLIX_MAX_RECORDS, so this journal cannot describe the whole
  // card. One-way for the life of the journal: finishScan() must never stamp it complete, or a
  // resume would claim to have finished a walk that was cut short.
  bool recordCapHit = false;

  // Resume tables. Both are sized from the journal header and released before
  // the sort allocations, exactly like the walk-only buffers below.
  std::unique_ptr<uint64_t[]> completedHashes;
  uint16_t completedCount = 0;
  std::unique_ptr<uint32_t[]> folderHashes;
  uint16_t folderHashCount = 0;

  std::unique_ptr<char[]> nameBuf;
  std::unique_ptr<StagedEntry> stagedEntry;
  std::unique_ptr<uint64_t[]> dedupKeys;
  std::unique_ptr<PriorEntry[]> priorList;
  uint16_t priorCount = 0;
  uint16_t nextFirstSeen = 0;
  LibraryIndexFile previous;

  bool begin(const char* root, bool wantMetadata, bool confirm);
  BuildStep step();
  BuildStep stepScan();
  void publishTotal(uint16_t total);
  bool reconcile();
  bool finalize();
  BuildStep finishScan();
  void teardown(bool keepJournal);
  void resetScan();
  bool openStageFiles();
  void releaseWalkAllocations();
  int findCompleted(uint64_t pathHash) const;
  bool emitFolder(WalkFrame& frame);
  void popFrame();
  static bool replayFrame(void* ctx, PartFrameType type, const uint8_t* payload, uint16_t len);
};

bool LibraryIndexStepper::Impl::openStageFiles() {
  if (st.stage) st.stage.close();
  if (st.folders) st.folders.close();
  if (!Storage.openFileForWrite("LIBIDX", STAGE_PATH, st.stage) ||
      !Storage.openFileForWrite("LIBIDX", folderStagePath, st.folders)) {
    LOG_ERR("LIBIDX", "cannot open staging files");
    if (st.stage) st.stage.close();
    if (st.folders) st.folders.close();
    return false;
  }
  if (!stageOut) {
    stageOut = makeUniqueNoThrow<serialization::BufferedFileWriter>(st.stage, LIBRARY_IO_BUFFER_SIZE);
    if (!stageOut) {
      LOG_ERR("LIBIDX", "staging buffer alloc failed");
      return false;
    }
  }
  return true;
}

// Everything the walk collects is thrown away and the scan restarts from the
// root. Used when the card changed while a journal was paused: the journal's
// folder ids no longer describe this tree, and silently renumbering them would
// rebuild every completed row's path from the wrong folder record.
void LibraryIndexStepper::Impl::resetScan() {
  if (dirOpen) {
    if (dir) dir.close();
    dirOpen = false;
  }
  depth = -1;
  foldersMatched = 0;
  completedCount = 0;
  folderHashCount = 0;
  resumedFallback = 0;
  skipScan = false;
  st.books = 0;
  st.folderId = 0;
  st.folderBytes = 0;
  st.activeDedupCount = 0;
  for (int i = 0; i < LIBRARY_MAX_DEPTH + 2; i++) {
    frames[i].path.clear();
    frames[i].resumePos = 0;
    frames[i].folderEmitted = false;
    frames[i].folderId = 0;
    frames[i].dedupBase = 0;
  }
}

bool LibraryIndexStepper::Impl::replayFrame(void* ctx, const PartFrameType type, const uint8_t* payload,
                                            const uint16_t len) {
  auto* self = static_cast<Impl*>(ctx);
  if (type == PartFrameType::Folder) {
    if (len < 1) return false;
    const uint8_t pathLen = payload[0];
    if (static_cast<uint16_t>(pathLen) + 1u != len || self->folderHashCount >= CLIX_MAX_RECORDS) return false;
    if (self->st.folders.write(payload, len) != len) return false;
    self->folderHashes[self->folderHashCount++] =
        fnv1a32(reinterpret_cast<const char*>(payload) + 1, pathLen);
    self->st.folderBytes += len;
    self->st.folderId = self->folderHashCount;
    return true;
  }
  if (type == PartFrameType::Record) {
    if (len != STAGE_STRIDE || self->completedCount >= CLIX_MAX_RECORDS) return false;
    self->stageOut->write(payload, len);
    uint64_t hash = 0;
    memcpy(&hash, payload + offsetof(StagedEntry, pathHash), sizeof(hash));
    self->completedHashes[self->completedCount++] = hash;
    self->st.books = self->completedCount;
    return true;
  }
  return false;
}

bool LibraryIndexStepper::Impl::begin(const char* root, const bool wantMetadata, const bool confirm) {
  stats = BuildStats{};
  progress = BuildProgress{};
  phase = Phase::Idle;
  cancelRequested = false;
  paused = false;
  confirmBeforeCommit = confirm;
  nothingToDo = false;
  resolvedFirstSeen.reset();
  startMs = millis();
  rootPath = (root != nullptr && root[0] != '\0') ? root : "/";
  readMetadata = wantMetadata;
  resumedFallback = 0;
  restartRequested = false;
  resetScan();

  Storage.mkdir(CACHE_DIR);
  if (!recoverInterruptedInstall()) return false;
  Storage.remove(STAGE_PATH);
  folderStagePath = std::string(STAGE_PATH) + ".f";
  Storage.remove(folderStagePath.c_str());

  nameBuf = makeUniqueNoThrow<char[]>(NAME_BUF_SIZE);
  stagedEntry = makeUniqueNoThrow<StagedEntry>();
  dedupKeys = makeUniqueNoThrow<uint64_t[]>(LIBRARY_MAX_DEDUP_KEYS);
  if (!nameBuf || !stagedEntry) {
    return false;
  }
  if (!dedupKeys) {
    LOG_ERR("LIBIDX", "dedup key buffer alloc failed; continuing without duplicate detection");
  }

  // The previous index supplies arrival history and the prior generation, so it
  // has to be read before the journal can be validated.
  if (Storage.exists(INDEX_PATH)) {
    if (previous.openForReconciliation(INDEX_PATH)) {
      nextFirstSeen = previous.header().nextFirstSeen;
      priorCount = previous.bookCount();
      priorList = makeUniqueNoThrow<PriorEntry[]>(priorCount == 0 ? 1 : priorCount);
      if (!priorList) {
        LOG_ERR("LIBIDX", "prior index array alloc failed");
        return false;
      }
      for (uint16_t i = 0; i < priorCount; i++) {
        serviceBuilder(st.serviceUnits);
        ClixRecord r{};
        uint64_t pathHash = 0;
        if (!previous.readRecord(i, r) || !previous.readPathHash(r, pathHash)) {
          LOG_ERR("LIBIDX", "prior index read failed at record %u", static_cast<unsigned>(i));
          return false;
        }
        priorList[i].pathHash = pathHash;
        priorList[i].fileSize = r.fileSize;
        priorList[i].firstSeen = r.firstSeen;
        priorList[i].ordinalAndMatched = i;
      }
      std::sort(priorList.get(), priorList.get() + priorCount, priorPathLess);
    } else if (previous.ioFailed()) {
      LOG_ERR("LIBIDX", "cannot read previous index; rebuild deferred");
      return false;
    }
  }

  // Generation, not a timestamp. A lost RTC makes every file look 46 years old,
  // so age is meaningless here; this counter is the only honest "is this stale".
  const uint32_t baseGeneration = previous.isOpen() ? clixGeneration(previous.header()) : 0;
  generation = baseGeneration + 1;

  identity = PartIdentity{};
  identity.generation = generation;
  identity.baseGeneration = baseGeneration;
  identity.cardId = fnv1a32(rootPath.data(), rootPath.size());
  identity.formatVersion = CLIX_FORMAT_VERSION;
  identity.foldVersion = CLIX_FOLD_VERSION;
  identity.metadataEnabled = readMetadata;
  identity.rootPath = rootPath;

  const PartOpen mode = journal.open(PART_PATH, identity, STAGE_STRIDE);
  if (journal.ioFailed() || !openStageFiles()) {
    teardown(false);
    return false;
  }

  if (mode == PartOpen::Fresh) {
    // A-P1-5: a brand-new journal means a brand-new walk, and the cap is not known for THIS one
    // yet. A RESUMED journal keeps the flag once it is set -- that asymmetry is what stops a
    // resume-at-cap from re-stamping completion on every attempt.
    recordCapHit = false;
  }
  if (mode == PartOpen::Resumed) {
    completedHashes = makeUniqueNoThrow<uint64_t[]>(journal.records() == 0 ? 1 : journal.records());
    folderHashes = makeUniqueNoThrow<uint32_t[]>(journal.folders() == 0 ? 1 : journal.folders());
    bool replayed = completedHashes && folderHashes && journal.replay(&Impl::replayFrame, this);
    if (replayed) {
      std::sort(completedHashes.get(), completedHashes.get() + completedCount);
      resumedFallback = completedCount;
      skipScan = journal.scanComplete() && journal.total() == completedCount && completedCount > 0;
      LOG_INF("LIBIDX", "resuming partial build: %u of %u books, generation %u",
              static_cast<unsigned>(completedCount), static_cast<unsigned>(journal.total()),
              static_cast<unsigned>(generation));
    } else {
      // A journal that replays badly is still only a cache: throw it away and
      // walk the card from the start rather than fail the build.
      LOG_ERR("LIBIDX", "journal replay failed; rebuilding from scratch");
      journal.discard();
      resetScan();
      completedHashes.reset();
      folderHashes.reset();
      restartRequested = false;
      // The writer is bound to the old (truncated) stage file and still holds its
      // buffered bytes; a fresh one is the only safe rebind.
      stageOut.reset();
      journal.open(PART_PATH, identity, STAGE_STRIDE);
      if (!openStageFiles() || journal.ioFailed()) {
        teardown(false);
        return false;
      }
    }
  }

  st.nameBuf = nameBuf.get();
  st.stagedEntry = stagedEntry.get();
  st.dedupKeys = dedupKeys.get();
  st.dedupDegraded = !dedupKeys;
  st.nextFirstSeen = nextFirstSeen;
  st.prior = priorList.get();
  st.priorCount = priorList ? priorCount : 0;
  st.readMetadata = readMetadata;
  st.previous = previous.isOpen() ? &previous : nullptr;
  st.stats = &stats;
  st.generation = generation;
  st.stageOut = stageOut.get();

  // Root frame.
  depth = 0;
  frames[0].path = rootPath;
  frames[0].resumePos = 0;
  frames[0].folderEmitted = false;
  frames[0].dedupBase = 0;
  frames[0].depth = 0;

  progress.books = st.books;
  progress.folders = st.folderId;
  progress.resumed = resumedFallback;
  // D3: a resumed journal whose walk was already COMPLETE knows its own total (the
  // header carries it), so the screen has a denominator without the pre-count --
  // and the pre-count would be a folder walk spent proving a number already on the
  // card. Every other build starts with no denominator and earns one.
  progress.total = skipScan ? static_cast<uint16_t>(completedCount) : 0;
  progress.generation = generation;
  phase = Phase::Scanning;
  return true;
}

// D3: the pre-count's answer, published before the first parse so the bar, the ETA
// and the percent cadence all have a denominator for the whole walk. Three guards,
// each of them load-bearing:
//   * Scanning only -- a finished build's total is the record count, and a number
//     from a folder walk must never replace it;
//   * never overwrite a total that already exists (a scan-complete resume knows its
//     own from the journal);
//   * clamp to CLIX_MAX_RECORDS, because the walk stops there: a card holding more
//     books than the cap would otherwise leave the bar permanently short of 100 %.
void LibraryIndexStepper::Impl::publishTotal(uint16_t total) {
  if (phase != Phase::Scanning || progress.total != 0 || total == 0) return;
  if (total > CLIX_MAX_RECORDS) total = CLIX_MAX_RECORDS;
  progress.total = total;
  LOG_INF("LIBIDX", "pre-count: %u books to visit, published as the walk's total", static_cast<unsigned>(total));
}

int LibraryIndexStepper::Impl::findCompleted(const uint64_t pathHash) const {
  if (completedCount == 0 || !completedHashes) return -1;
  const uint64_t* const first = completedHashes.get();
  const uint64_t* const end = first + completedCount;
  const uint64_t* hit = std::lower_bound(first, end, pathHash);
  return (hit != end && *hit == pathHash) ? static_cast<int>(hit - first) : -1;
}

// A directory gets its folder id on its FIRST book, which is what keeps ids
// dense and in walk order. On a resumed run the k-th directory to yield a book
// must still be journal folder k: the completed records carry those ids, and a
// different tree would rebuild their paths from the wrong folder path.
bool LibraryIndexStepper::Impl::emitFolder(WalkFrame& frame) {
  const uint32_t hash = fnv1a32(frame.path.data(), frame.path.size());
  const uint16_t k = foldersMatched;
  if (k < folderHashCount) {
    if (folderHashes[k] != hash) {
      LOG_INF("LIBIDX", "folder %u changed since the paused build (%s): restarting", static_cast<unsigned>(k),
              frame.path.c_str());
      restartRequested = true;
      return false;
    }
    frame.folderId = k;
    foldersMatched++;
    return true;
  }

  const uint8_t pathLen = static_cast<uint8_t>(frame.path.size());
  if (st.folders.write(&pathLen, 1) != 1 ||
      st.folders.write(reinterpret_cast<const uint8_t*>(frame.path.data()), pathLen) != pathLen) {
    LOG_ERR("LIBIDX", "folder stage write failed: %s", frame.path.c_str());
    st.failed = true;
    return false;
  }
  frame.folderId = st.folderId;
  st.folderBytes += 1u + pathLen;
  st.folderId++;
  foldersMatched++;
  if (!journal.appendFolder(frame.path.data(), pathLen)) {
    st.failed = true;
    return false;
  }
  return true;
}

void LibraryIndexStepper::Impl::popFrame() {
  if (depth < 0) return;
  st.activeDedupCount = frames[depth].dedupBase;
  depth--;
}

BuildStep LibraryIndexStepper::Impl::stepScan() {
  // The ONLY cancellation point: step() is entered between two books, so this
  // can never interrupt a parse or half a frame write.
  if (cancelRequested) {
    teardown(true);
    phase = Phase::Cancelled;
    return BuildStep::Cancelled;
  }

  // The journal's walk was already complete: an interruption landed after the
  // last book and before the promote. Re-walking the card to arrive at the same
  // records would cost minutes and prove nothing.
  if (skipScan) {
    skipScan = false;
    return finishScan();
  }

  while (depth >= 0) {
    WalkFrame& frame = frames[depth];
    if (!dirOpen) {
      dir = Storage.open(frame.path.c_str());
      if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        popFrame();
        continue;
      }
      dirOpen = true;
      if (frame.resumePos == 0) {
        dir.rewindDirectory();
      } else if (!dir.seekSet(frame.resumePos)) {
        LOG_ERR("LIBIDX", "cannot resume directory %s at %u", frame.path.c_str(),
                static_cast<unsigned>(frame.resumePos));
        st.failed = true;
        break;
      }
    }

    HalFile entry = dir.openNextFile();
    if (!entry) {
      dir.close();
      dirOpen = false;
      popFrame();
      continue;
    }
    serviceBuilder(st.serviceUnits);

    frame.resumePos = dir.position();
    st.nameBuf[0] = '\0';
    entry.getName(st.nameBuf, NAME_BUF_SIZE);
    const bool isDir = entry.isDirectory();
    const uint32_t size = isDir ? 0 : static_cast<uint32_t>(entry.fileSize());
    const uint32_t modificationTime = isDir ? 0 : entry.modificationTime();
    entry.close();

    // A-P1-5: the cap used to `break` with st.failed == false, so finishScan() installed the
    // index as if the walk had reached the end of the card: above CLIX_MAX_RECORDS books the
    // shelf was silently short, and marked complete, forever. The cap is now recorded, logged
    // once, and finishScan() refuses to stamp the journal complete -- so an interrupted build
    // resumes instead of claiming to have finished. The flag is ONE-WAY within a journal's life:
    // clearing it on the way out would let a resume-at-cap claim completion again.
    if (st.books >= CLIX_MAX_RECORDS) {
      if (!recordCapHit) {
        LOG_ERR("LIBIDX", "library capped at %u books: the index will be short until the cap is lifted",
                static_cast<unsigned>(CLIX_MAX_RECORDS));
      }
      recordCapHit = true;
      break;
    }
    if (st.nameBuf[0] == '\0' || isHiddenOrSidecar(st.nameBuf)) continue;
    const std::string name(st.nameBuf);

    if (isDir) {
      if (depth + 1 > LIBRARY_MAX_DEPTH) continue;
      dir.close();
      dirOpen = false;
      depth++;
      frames[depth].path = joinLibraryPath(frame.path, name);
      // `frame` is the parent slot and stays valid: depth only moved forward.
      frames[depth].resumePos = 0;
      frames[depth].depth = static_cast<uint8_t>(depth);
      frames[depth].folderEmitted = false;
      frames[depth].folderId = 0;
      frames[depth].dedupBase = st.activeDedupCount;
      continue;
    }
    if (!isLibraryBookFile(name)) continue;

    // D3: THE predicate the pre-count also applies (countsAsBook). Nothing about
    // what gets indexed changes here -- a size-0 entry and an over-long name were
    // the two skips this replaced, and they are the same two skips, now stated
    // once so the bar's denominator and this walk cannot drift apart.
    if (!countsAsBook(name, size, frame.path.size())) {
      st.unreadableSkipped++;
      continue;
    }

    if (!frame.folderEmitted) {
      if (!emitFolder(frame)) {
        if (restartRequested) break;  // handled after the loop
        st.failed = true;
        break;
      }
      frame.folderEmitted = true;
    }

    const std::string fullPath = joinLibraryPath(frame.path, name);
    const uint64_t pathHash = clixPathHash(fullPath.data(), fullPath.size());
    if (findCompleted(pathHash) >= 0) {
      // Already durable in this journal. Skipping by full-path hash also keeps
      // the duplicate-dirent case right: both copies share a path, so both are
      // skipped, exactly as the dedup pass would have dropped the second.
      continue;
    }

    const uint64_t key = (static_cast<uint64_t>(fnv1a32(name.data(), name.size())) << 32) | size;
    if (st.dedupKeys != nullptr) {
      uint64_t* const end = st.dedupKeys + st.activeDedupCount;
      uint64_t* const slot = std::lower_bound(st.dedupKeys + frame.dedupBase, end, key);
      if (slot != end && *slot == key) {
        st.duplicatesDropped++;
        continue;
      }
      if (st.activeDedupCount < LIBRARY_MAX_DEDUP_KEYS) {
        memmove(slot + 1, slot, static_cast<size_t>(end - slot) * sizeof(*slot));
        *slot = key;
        st.activeDedupCount++;
      } else if (!st.dedupDegraded) {
        LOG_INF("LIBIDX", "duplicate detection capped at %u entries in %s",
                static_cast<unsigned>(LIBRARY_MAX_DEDUP_KEYS), frame.path.c_str());
        st.dedupDegraded = true;
      }
    }

    if (!stageRecord(st, name, size, frame.folderId, fullPath, modificationTime)) break;
    // Committed to the journal now, so a stop from here on keeps this book.
    if (!journal.appendRecord(st.stagedEntry, STAGE_STRIDE)) {
      LOG_ERR("LIBIDX", "journal append failed at book %u", static_cast<unsigned>(st.books));
      st.failed = true;
      break;
    }

    progress.books = st.books;
    progress.folders = st.folderId;
    return BuildStep::Progress;
  }

  if (restartRequested) {
    // The card changed under a paused journal. Throw the journal away and walk
    // again from the root; this is the only path that re-reads the same books.
    restartRequested = false;
    journal.discard();
    resetScan();
    completedHashes.reset();
    folderHashes.reset();
    stageOut.reset();
    journal.open(PART_PATH, identity, STAGE_STRIDE);
    if (!openStageFiles() || journal.ioFailed()) {
      st.failed = true;
    } else {
      depth = 0;
      frames[0].path = rootPath;
      frames[0].depth = 0;
      return BuildStep::Progress;
    }
  }

  if (st.failed) {
    teardown(true);  // keep the journal: every book already in it is still good
    phase = Phase::Failed;
    return BuildStep::Failed;
  }

  return finishScan();
}

BuildStep LibraryIndexStepper::Impl::finishScan() {
  // End of the walk. M is known now, so "partial N of M" can be written and a
  // resume at this point would simply finalize.
  const bool stageFlushed = stageOut ? stageOut->flush() : false;
  const bool stageClosed = st.stage.close();
  const bool foldersClosed = st.folders.close();
  if (!stageFlushed || !stageClosed || !foldersClosed) {
    LOG_ERR("LIBIDX", "staging flush failed; keeping the previous index");
    teardown(true);
    phase = Phase::Failed;
    return BuildStep::Failed;
  }
  if (recordCapHit) {
    // A-P1-5: the walk stopped at the cap, so the journal is short by construction. Leaving the
    // header un-stamped is what makes the next run resume rather than install a truncated index
    // as a complete one.
    LOG_ERR("LIBIDX", "not marking the journal complete: walk stopped at the %u-book cap",
            static_cast<unsigned>(CLIX_MAX_RECORDS));
  } else if (!journal.markScanComplete(st.books)) {
    // A-P2-6: the result was ignored, so a failed header write left the pause banner claiming the
    // total is unknown for the rest of the build's life. Say so; the index itself is still good.
    LOG_ERR("LIBIDX", "scan-complete header write failed: the deferred total stays unknown");
  }

  stats.books = st.books;
  stats.folders = st.folderId;
  stats.duplicatesDropped = st.duplicatesDropped;
  stats.unreadableSkipped = st.unreadableSkipped;
  stats.dedupDegraded = st.dedupDegraded;
  stats.unchanged = st.unchanged;
  stats.changed = st.changed;
  stats.enriched = st.enriched;
  progress.total = st.books;
  progress.books = st.books;
  progress.folders = st.folderId;

  // Reconciliation runs here rather than inside finalize(), because the diff has
  // to be FINAL before the counts are shown: added/removed/renamed are only known
  // once every book has been matched against what is left of the previous index.
  if (!reconcile()) {
    teardown(false);
    phase = Phase::Failed;
    return BuildStep::Failed;
  }
  progress.added = static_cast<uint16_t>(stats.added + stats.renamed);
  progress.changed = stats.changed;
  progress.removed = stats.removed;
  progress.unchanged = stats.unchanged;

  if (confirmBeforeCommit && !nothingToDo) {
    phase = Phase::AwaitingCommit;
    return BuildStep::ReadyToCommit;
  }
  phase = Phase::Finalizing;
  return BuildStep::Finalizing;
}

void LibraryIndexStepper::Impl::releaseWalkAllocations() {
  if (dirOpen) {
    if (dir) dir.close();
    dirOpen = false;
  }
  previous.close();
  st.previous = nullptr;
  st.prior = nullptr;
  st.nameBuf = nullptr;
  st.stagedEntry = nullptr;
  st.dedupKeys = nullptr;
  priorList.reset();
  nameBuf.reset();
  stagedEntry.reset();
  dedupKeys.reset();
  completedHashes.reset();
  folderHashes.reset();
}

// Phase one of the commit: decide what the diff means and hand every book its
// arrival number. One pass over the staging file and one over the previous index;
// not a byte of the live index is touched.
bool LibraryIndexStepper::Impl::reconcile() {
  [[maybe_unused]] const uint32_t finalizeStartMs = millis();
  uint32_t serviceUnits = 0;
  nothingToDo = false;

  if (previous.isOpen() && st.books == priorCount && st.unchanged == priorCount && st.changed == 0 &&
      stats.metadataReused == priorCount && st.unreadableSkipped == 0) {
    previous.close();
    Storage.remove(STAGE_PATH);
    Storage.remove(folderStagePath.c_str());
    stats.walkMs = millis() - startMs;
    LOG_INF("LIBIDX", "unchanged: %u reused, %u parsed, no replacement, %ums",
            static_cast<unsigned>(stats.metadataReused), static_cast<unsigned>(stats.parsed),
            static_cast<unsigned>(stats.walkMs));
    clearLibraryIndexDirty();
    journal.discard();
    nothingToDo = true;
    return true;
  }

  resolvedFirstSeen = makeUniqueNoThrow<uint16_t[]>(st.books == 0 ? 1 : st.books);
  if (!resolvedFirstSeen) {
    LOG_ERR("LIBIDX", "firstSeen array alloc failed");
    Storage.remove(STAGE_PATH);
    Storage.remove(folderStagePath.c_str());
    return false;
  }
  if (st.books > 0) {
    if (priorList) std::sort(priorList.get(), priorList.get() + priorCount, priorSizeLess);
    HalFile read;
    if (!Storage.openFileForRead("LIBIDX", STAGE_PATH, read)) {
      LOG_ERR("LIBIDX", "firstSeen reconciliation: cannot reopen the stage");
      Storage.remove(STAGE_PATH);
      Storage.remove(folderStagePath.c_str());
      return false;
    }
    for (uint16_t i = 0; i < st.books; i++) {
      serviceBuilder(serviceUnits);
      ClixRecord r{};
      if (!read.seekSet(static_cast<uint64_t>(i) * STAGE_STRIDE) ||
          read.read(reinterpret_cast<uint8_t*>(&r), sizeof(r)) != static_cast<int>(sizeof(r))) {
        LOG_ERR("LIBIDX", "firstSeen reconciliation: short read at record %u", static_cast<unsigned>(i));
        read.close();
        Storage.remove(STAGE_PATH);
        Storage.remove(folderStagePath.c_str());
        return false;
      }
      if (r.firstSeen != FIRST_SEEN_UNRESOLVED) {
        resolvedFirstSeen[i] = r.firstSeen;
        continue;
      }
      PriorEntry* renamed = nullptr;
      if (priorList) {
        PriorEntry* candidate =
            std::lower_bound(priorList.get(), priorList.get() + priorCount, r.fileSize,
                             [](const PriorEntry& entry, const uint32_t size) { return entry.fileSize < size; });
        while (candidate != priorList.get() + priorCount && candidate->fileSize == r.fileSize) {
          if (!priorMatched(*candidate)) {
            renamed = candidate;
            break;
          }
          ++candidate;
        }
      }
      if (renamed) {
        markPriorMatched(*renamed);
        resolvedFirstSeen[i] = renamed->firstSeen;
        stats.renamed++;
      } else {
        resolvedFirstSeen[i] = st.nextFirstSeen++;
        stats.added++;
      }
    }
    if (!read.close()) {
      LOG_ERR("LIBIDX", "firstSeen reconciliation: stage close failed");
      Storage.remove(STAGE_PATH);
      Storage.remove(folderStagePath.c_str());
      return false;
    }
    for (uint16_t q = 0; q < priorCount; q++) {
      serviceBuilder(serviceUnits);
      if (priorList && !priorMatched(priorList[q])) stats.removed++;
    }
  }

  releaseWalkAllocations();
  LOG_DBG("LIBIDX", "phase reconcile: %ums", static_cast<unsigned>(millis() - finalizeStartMs));
  return true;
}

bool LibraryIndexStepper::Impl::finalize() {
  if (nothingToDo) return true;
  [[maybe_unused]] const uint32_t finalizeStartMs = millis();
  uint32_t serviceUnits = 0;

  auto order = makeUniqueNoThrow<uint16_t[]>(st.books == 0 ? 1 : st.books);
  if (!order) {
    LOG_ERR("LIBIDX", "order array alloc failed (%u books)", static_cast<unsigned>(st.books));
    Storage.remove(STAGE_PATH);
    Storage.remove(folderStagePath.c_str());
    return false;
  }
  for (uint16_t i = 0; i < st.books; i++) {
    serviceBuilder(serviceUnits);
    order[i] = i;
  }

  bool coreSortsAvailable = true;
  if (st.books > 1) {
    auto keys = makeUniqueNoThrow<SortKey[]>(st.books);
    if (keys) {
      HalFile stageRead;
      if (!Storage.openFileForRead("LIBIDX", STAGE_PATH, stageRead)) {
        LOG_ERR("LIBIDX", "title sort: cannot reopen the record stage");
        Storage.remove(STAGE_PATH);
        Storage.remove(folderStagePath.c_str());
        return false;
      }
      for (uint16_t i = 0; i < st.books; i++) {
        serviceBuilder(serviceUnits);
        ClixRecord r{};
        const uint64_t offset = static_cast<uint64_t>(i) * STAGE_STRIDE;
        if (!stageRead.seekSet(offset) ||
            stageRead.read(reinterpret_cast<uint8_t*>(&r), sizeof(r)) != static_cast<int>(sizeof(r))) {
          LOG_ERR("LIBIDX", "title sort: record stage read failed at %u", static_cast<unsigned>(offset));
          stageRead.close();
          Storage.remove(STAGE_PATH);
          Storage.remove(folderStagePath.c_str());
          return false;
        }
        memset(keys[i].key, 0, sizeof(keys[i].key));
        memcpy(keys[i].key, r.fold, std::min<size_t>(r.foldLen, sizeof(keys[i].key)));
        keys[i].ordinal = i;
      }
      if (!stageRead.close()) {
        LOG_ERR("LIBIDX", "title sort: stage close failed");
        Storage.remove(STAGE_PATH);
        Storage.remove(folderStagePath.c_str());
        return false;
      }
      delay(1);
      std::sort(keys.get(), keys.get() + st.books, sortKeyLess);
      delay(1);
      for (uint16_t i = 0; i < st.books; i++) {
        serviceBuilder(serviceUnits);
        order[i] = keys[i].ordinal;
      }
    } else {
      stats.ranksDegraded = true;
      coreSortsAvailable = false;
      LOG_ERR("LIBIDX", "sort skipped: key array alloc failed");
    }
  }

  const bool ok =
      emitIndex(folderStagePath.c_str(), st, order.get(), resolvedFirstSeen.get(), coreSortsAvailable, stats);
  LOG_DBG("LIBIDX", "phase author/orders/emit: %ums", static_cast<unsigned>(millis() - finalizeStartMs));
  resolvedFirstSeen.reset();
  Storage.remove(STAGE_PATH);
  Storage.remove(folderStagePath.c_str());

  stats.walkMs = millis() - startMs;
  stats.indexReplaced = ok;
  LOG_INF("LIBIDX",
          "%s: %u books, %u folders, %u parsed, %u metadata reused, replaced %u, %u dup dropped, %u unreadable, %ums",
          ok ? "built" : "FAILED", static_cast<unsigned>(stats.books), static_cast<unsigned>(stats.folders),
          static_cast<unsigned>(stats.parsed), static_cast<unsigned>(stats.metadataReused),
          static_cast<unsigned>(stats.indexReplaced), static_cast<unsigned>(stats.duplicatesDropped),
          static_cast<unsigned>(stats.unreadableSkipped), static_cast<unsigned>(stats.walkMs));
  if (ok) {
    clearLibraryIndexDirty();
    // The index that journal described is live now; the journal has no further
    // use and would otherwise be resumed into a generation that already shipped.
    journal.discard();
  }
  return ok;
}

void LibraryIndexStepper::Impl::teardown(const bool keepJournal) {
  if (dirOpen) {
    if (dir) dir.close();
    dirOpen = false;
  }
  if (stageOut) {
    stageOut->flush();
    stageOut.reset();
  }
  if (st.stage) st.stage.close();
  if (st.folders) st.folders.close();
  if (!keepJournal) journal.discard();
  journal.close();
  Storage.remove(STAGE_PATH);
  Storage.remove(folderStagePath.c_str());
  previous.close();
}

BuildStep LibraryIndexStepper::Impl::step() {
  switch (phase) {
    case Phase::Scanning:
      return stepScan();
    case Phase::AwaitingCommit:
      // Nothing happens until the caller either commits or cancels. Returning the
      // same step is what lets the screen hold the diff on the glass.
      if (cancelRequested) {
        teardown(true);
        phase = Phase::Cancelled;
        return BuildStep::Cancelled;
      }
      return BuildStep::ReadyToCommit;
    case Phase::Finalizing: {
      const bool ok = finalize();
      phase = ok ? Phase::Finished : Phase::Failed;
      return ok ? BuildStep::Done : BuildStep::Failed;
    }
    case Phase::Finished:
      return BuildStep::Done;
    case Phase::Cancelled:
      return BuildStep::Cancelled;
    case Phase::Idle:
    case Phase::Failed:
      break;
  }
  return BuildStep::Failed;
}

LibraryIndexStepper::LibraryIndexStepper() : impl(new (std::nothrow) Impl()) {
  // Nothrow and not makeUniqueNoThrow(): both give nullptr on OOM (exceptions are
  // off, so a plain new would abort), but this one allocation is a fixed ~800
  // bytes taken before any library-sized work, and keeping it out of the counted
  // sequence leaves the stepper's allocation order identical to the old blocking
  // build's -- which is what the allocation-failure tests pin.
}

LibraryIndexStepper::~LibraryIndexStepper() {
  if (impl && impl->phase != Impl::Phase::Finished && impl->phase != Impl::Phase::Idle) impl->teardown(true);
}

bool LibraryIndexStepper::begin(const char* rootPath, const bool readMetadata, const bool confirmBeforeCommit) {
  if (!impl) return false;
  return impl->begin(rootPath, readMetadata, confirmBeforeCommit);
}

BuildStep LibraryIndexStepper::step() {
  if (!impl) return BuildStep::Failed;
  return impl->step();
}

void LibraryIndexStepper::commit() {
  // Only the gate moves. Anything else (a cancel already requested, a failure)
  // keeps its phase, so a stray Confirm cannot resurrect a stopped build.
  if (impl && impl->phase == Impl::Phase::AwaitingCommit) impl->phase = Impl::Phase::Finalizing;
}

void LibraryIndexStepper::requestCancel() {
  if (impl) impl->cancelRequested = true;
}

const BuildProgress& LibraryIndexStepper::progress() const {
  static const BuildProgress empty;
  return impl ? impl->progress : empty;
}

const BuildStats& LibraryIndexStepper::stats() const {
  static const BuildStats empty;
  return impl ? impl->stats : empty;
}

void LibraryIndexStepper::publishTotal(const uint16_t total) {
  if (impl) impl->publishTotal(total);
}

// --- D3: the pre-count -------------------------------------------------------
//
// The same folder walk the builder does, with the book handling removed: it opens
// directories, reads each dirent's name/size/type, applies countsAsBook() and
// counts. It never opens a book, never parses, never writes. That is what makes it
// affordable as a separate pass over the card before the walk starts.
//
// It is a STEPPER for the same reason the walk is: the screen has to keep drawing
// and keep answering Cancel while it runs. One step() visits at most
// LIBRARY_COUNT_ENTRIES_PER_STEP entries, so a directory with thousands of files
// costs many short steps rather than one long one.
//
// Its frame stack mirrors the builder's: ONE directory handle, and a parent that is
// closed on the way down and re-opened at its remembered position on the way back
// up. Nothing here allocates per entry beyond the path of a directory it descends
// into.
struct LibraryPreCounter::Impl {
  struct Frame {
    std::string path;
    uint32_t resumePos = 0;
  };
  Frame frames[LIBRARY_MAX_DEPTH + 2];
  int depth = -1;
  HalFile dir;
  bool dirOpen = false;
  char nameBuf[NAME_BUF_SIZE] = {};
  uint16_t books = 0;
  // The walk is over (successfully or not). step() is a no-op afterwards.
  bool finished = false;
  bool failedFlag = false;
  uint32_t serviceUnits = 0;

  bool step() {
    if (finished) return false;
    int visited = 0;
    while (depth >= 0 && visited < LIBRARY_COUNT_ENTRIES_PER_STEP) {
      Frame& frame = frames[depth];
      if (!dirOpen) {
        dir = Storage.open(frame.path.c_str());
        if (!dir || !dir.isDirectory()) {
          // The builder treats an unopenable directory as empty and walks on, so
          // the count does too: a directory that cannot be listed contributes no
          // books to either number.
          if (dir) dir.close();
          depth--;
          continue;
        }
        dirOpen = true;
        if (frame.resumePos == 0) {
          dir.rewindDirectory();
        } else if (!dir.seekSet(frame.resumePos)) {
          // The only genuinely unrecoverable case: the count cannot resume where it
          // stopped, so the number it holds is short of the card. Report that the
          // count FAILED rather than publish a denominator the bar would overshoot.
          LOG_ERR("LIBIDX", "pre-count cannot resume %s at %u", frame.path.c_str(),
                  static_cast<unsigned>(frame.resumePos));
          dir.close();
          dirOpen = false;
          depth = -1;
          failedFlag = true;
          finished = true;
          return false;
        }
      }
      HalFile entry = dir.openNextFile();
      if (!entry) {
        dir.close();
        dirOpen = false;
        depth--;
        continue;
      }
      serviceBuilder(serviceUnits);
      frame.resumePos = dir.position();
      nameBuf[0] = '\0';
      entry.getName(nameBuf, NAME_BUF_SIZE);
      const bool isDir = entry.isDirectory();
      const uint32_t size = isDir ? 0u : static_cast<uint32_t>(entry.fileSize());
      entry.close();
      visited++;

      if (nameBuf[0] == '\0' || isHiddenOrSidecar(nameBuf)) continue;
      const std::string_view name(nameBuf);
      if (isDir) {
        // Same cap as the builder: the walk will not descend past this depth, so
        // counting what lies below it would count books nobody will visit.
        if (depth + 1 > LIBRARY_MAX_DEPTH) continue;
        dir.close();
        dirOpen = false;
        depth++;
        frames[depth].path = joinLibraryPath(frame.path, name);
        frames[depth].resumePos = 0;
        continue;
      }
      if (!countsAsBook(name, size, frame.path.size())) continue;
      // CLIX_MAX_RECORDS is where the WALK stops, so counting past it would promise
      // records that will never be visited. Stop the count there as well: the same
      // number, and the card is not read for a figure that cannot be used.
      books++;
      if (books >= CLIX_MAX_RECORDS) {
        dir.close();
        dirOpen = false;
        depth = -1;
        finished = true;
        break;
      }
    }
    if (depth < 0) {
      dirOpen = false;
      finished = true;
    }
    return !finished;
  }
};

LibraryPreCounter::LibraryPreCounter() : impl(new (std::nothrow) Impl()) {}

LibraryPreCounter::~LibraryPreCounter() {
  if (impl && impl->dirOpen && impl->dir) impl->dir.close();
}

void LibraryPreCounter::begin(const char* rootPath) {
  if (!impl) return;
  impl->depth = 0;
  impl->frames[0].path = (rootPath != nullptr && rootPath[0] != '\0') ? rootPath : "/";
  impl->frames[0].resumePos = 0;
  impl->books = 0;
  impl->finished = false;
  impl->failedFlag = false;
  impl->dirOpen = false;
}

bool LibraryPreCounter::step() {
  if (!impl) return false;
  return impl->step();
}

bool LibraryPreCounter::finished() const { return impl == nullptr || impl->finished; }

bool LibraryPreCounter::failed() const { return impl != nullptr && impl->failedFlag; }

uint16_t LibraryPreCounter::count() const { return impl != nullptr ? impl->books : 0; }

bool buildLibraryIndex(const char* rootPath, BuildStats& stats, const bool readMetadata) {
  LibraryIndexStepper stepper;
  if (!stepper.begin(rootPath, readMetadata)) {
    stats = stepper.stats();
    return false;
  }
  BuildStep result = BuildStep::Progress;
  while (result == BuildStep::Progress || result == BuildStep::Finalizing) result = stepper.step();
  stats = stepper.stats();
  return result == BuildStep::Done;
}

bool readLibraryIndexSummary(IndexSummary& out) {
  out = IndexSummary{};
  LibraryIndexFile index;
  if (!index.open(INDEX_PATH)) return false;
  out.valid = true;
  out.books = index.bookCount();
  out.folders = index.header().folderCount;
  out.generation = clixGeneration(index.header());
  out.metadataEnabled = index.header().metadataEnabled != 0;
  out.ranksDegraded = index.ranksDegraded();
  index.close();
  return true;
}

bool readLibraryPartialSummary(PartialSummary& out) {
  out = PartialSummary{};
  return LibraryPartial::peek(PART_PATH, out.records, out.folders, out.total, out.generation);
}

void discardLibraryPartial() {
  if (Storage.exists(PART_PATH)) Storage.remove(PART_PATH);
}

}  // namespace library
