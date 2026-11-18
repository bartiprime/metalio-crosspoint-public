#pragma once

// Builds the CLX1 index by walking the SD card once.
//
// The walk derives filename fallbacks and can read title and author metadata from
// EPUBs. Fresh metadata is reused from the previous index.
//
// Shape of the build, and why:
//
//   * ONE walk. Records go straight into a staging file in discovery order, so
//     nothing proportional to the library stays resident. Phase-local sort arrays
//     are fallible and released before the next large allocation.
//   * Duplicate directory entries are dropped. A damaged FAT can hand the same
//     file out twice — measured on a real card: 6 of 75 entries were duplicate
//     dirents resolving to one inode — and without this the shelf shows phantom
//     books that cannot be opened.
//   * Unreadable entries are skipped, never fatal. The same card had 7 entries
//     whose names enumerate but whose contents cannot be opened.
//   * Install is write-then-rename, so an interrupted build leaves the previous
//     index untouched rather than a half-written one.
//   * The walk is a STEPPER (LIB1): it yields between books, so the screen can
//     show progress and a Cancel can stop it without losing what it already did.
//     Every committed book also lands in `/.crosspoint/library.part`, a framed
//     journal the next Update resumes from. Nothing is promoted until the last
//     book is in, so the old index answers for the whole build.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "LibraryFormat.h"

namespace library {

// Directory levels below the scan root that are walked. The measured corpus is
// two deep (genre/author/book); the cap exists because a corrupted FAT can
// contain a directory that contains itself — also measured on the same card —
// and an uncapped walk would never return.
inline constexpr int LIBRARY_MAX_DEPTH = 5;

// Duplicate identities remembered while one directory is enumerated. The
// fixed, fallible allocation is 8 KiB at this cap; unlike std::vector it cannot
// grow into abort() when a damaged or unusually flat directory is scanned.
inline constexpr uint16_t LIBRARY_MAX_DEDUP_KEYS = 1024;

struct BuildStats {
  uint16_t books = 0;
  uint16_t folders = 0;
  uint16_t duplicatesDropped = 0;
  uint16_t unreadableSkipped = 0;
  uint32_t walkMs = 0;
  // The smart diff. Keys are full path (identity) + size + modification time
  // (freshness); see LibraryBuilder.cpp for why mtime is safe here despite a dead
  // RTC. Their sum over a rebuild with no card changes should be:
  // unchanged == books, everything else zero.
  uint16_t unchanged = 0;  // same path, same size, same mtime: no OPF read at all
  uint16_t changed = 0;    // same path, size or mtime differs: THAT book re-parsed
  uint16_t added = 0;      // matched nothing, not even by size
  uint16_t renamed = 0;    // matched a leftover entry by size alone (counted in `added` on the line)
  uint16_t removed = 0;    // previous entry no book claimed
  uint16_t enriched = 0;   // took its title or author from the book rather than the filename
  uint16_t parsed = 0;     // EPUB metadata reads performed by this build
  uint16_t metadataReused = 0;
  uint16_t seriesBooks = 0;  // books whose own OPF named a series
  bool indexReplaced = false;
  bool ranksDegraded = false;
  bool dedupDegraded = false;
};

// What the progress screen draws. `total` is 0 while no denominator exists: nothing
// can know how many books a card holds without walking it. D3 changed WHEN one
// exists, not what 0 means -- a pre-count (LibraryPreCounter) walks the folders
// before the first parse and the stepper publishes its answer through
// publishTotal(), so 0 now means "no denominator: the pre-count failed, has not
// finished, or this build is not walking" rather than "the walk has not finished".
// Every reader still has to render the bar empty and the ETA absent for that case.
struct BuildProgress {
  uint16_t books = 0;    // records committed so far, resumed ones included
  uint16_t folders = 0;
  uint16_t total = 0;    // 0 = unknown
  uint16_t resumed = 0;  // carried over from the journal when the run began
  uint32_t generation = 1;
  // The diff summary. Final only once `total` is known (the walk is complete) and
  // the previous index has been reconciled; before that it is a running count.
  uint16_t added = 0;
  uint16_t changed = 0;
  uint16_t removed = 0;
  uint16_t unchanged = 0;
};

enum class BuildStep : uint8_t {
  Progress,       // one book committed; the caller may paint and poll input
  Finalizing,     // the walk is done; the next step() sorts and promotes
  ReadyToCommit,  // confirmBeforeCommit only: counts are final, nothing promoted
  Done,
  Failed,
  Cancelled,
};

// Drives one library build. The contract the UI relies on:
//
//   * step() only ever stops BETWEEN two books (or between a book and the
//     finalize pass). It never returns mid-parse and never mid-frame-write, so
//     a cancel costs at most the book it was already reading.
//   * requestCancel() is honoured at the next book boundary. Everything already
//     committed is on the card and the next begin() resumes from there.
//   * Nothing here touches the live index until finalize, which writes
//     `library.new` and renames it over `library.idx` in one step.
class LibraryIndexStepper {
 public:
  LibraryIndexStepper();
  ~LibraryIndexStepper();
  LibraryIndexStepper(const LibraryIndexStepper&) = delete;
  LibraryIndexStepper& operator=(const LibraryIndexStepper&) = delete;

  // Recover any interrupted install, load the previous index for reconciliation,
  // and open (or validate away) the journal. Blocking, but bounded: it is one
  // record read per prior book plus one frame read per journal record.
  //
  // `confirmBeforeCommit`: stop at BuildStep::ReadyToCommit once the counts are
  // final and before the live index is replaced, so the caller can show the diff
  // and let the reader decide. commit() then continues; requestCancel() at that
  // point leaves the journal intact and the served index untouched.
  bool begin(const char* rootPath, bool readMetadata = false, bool confirmBeforeCommit = false);
  BuildStep step();
  // Release the ReadyToCommit gate. Ignored unless that step was reported.
  void commit();
  void requestCancel();
  const BuildProgress& progress() const;
  const BuildStats& stats() const;

  // D3: the denominator, handed over by the pre-count before the first parse.
  // Ignored unless the walk is scanning and still has no total, so it cannot
  // overwrite the number finishScan() derives from the records themselves, and it
  // is clamped to the record cap so the bar can never be driven past 100 %.
  void publishTotal(uint16_t total);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

// D3: the denominator, counted BEFORE anything is parsed.
//
// The walk can only learn how many books a card holds by walking it, so `total`
// used to arrive at finishScan() -- i.e. after the bar, the ETA and the percent
// cadence had already been useless for the whole build. This is the cheap
// pre-pass: one folder walk that COUNTS the dirents the walk will turn into
// records, opening no book, so the screen has a denominator from its first frame.
//
// Three properties, each of which the screen depends on:
//   * it is a COUNTER, NOT A FILTER. It decides nothing about what gets indexed;
//     both it and the walk call countsAsBook()/isLibraryBookFile(), so the number
//     it publishes is the number of records the walk will visit or the two have
//     drifted and the bar is lying.
//   * it is bounded per call: step() visits at most
//     LIBRARY_COUNT_ENTRIES_PER_STEP directory entries and returns, so the caller
//     keeps its input polling (and its Cancel) at the same cadence the walk uses.
//   * a failure is not fatal: failed() says the card could not be enumerated, the
//     caller publishes nothing, and the build proceeds exactly as it did before
//     D3 (total 0, empty bar, per-book cadence).
// The answer may be slightly stale (a file added or removed mid-walk); the bar
// clamps to 100 % because publishTotal() clamps and the walk's own finishScan()
// total replaces it once the records are the truth.
inline constexpr int LIBRARY_COUNT_ENTRIES_PER_STEP = 24;

class LibraryPreCounter {
 public:
  LibraryPreCounter();
  ~LibraryPreCounter();
  LibraryPreCounter(const LibraryPreCounter&) = delete;
  LibraryPreCounter& operator=(const LibraryPreCounter&) = delete;

  // Start counting under `rootPath` (the same root the stepper is given). Cheap:
  // nothing is opened until the first step().
  void begin(const char* rootPath);
  // Visit up to LIBRARY_COUNT_ENTRIES_PER_STEP entries. Returns false once the
  // count is over -- finished() or failed() -- and true while there is more.
  bool step();
  // The walk is over: count() is the answer if failed() is false.
  bool finished() const;
  bool failed() const;
  uint16_t count() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

// Walk `rootPath`, write `/.crosspoint/library.idx`, and report what happened.
// This is the blocking convenience wrapper the callers that cannot paint (the
// home screen's cover grid) still use: it drives the stepper straight through.
bool buildLibraryIndex(const char* rootPath, BuildStats& stats, bool readMetadata = false);

// Live index path, shared by the builder and activity.
const char* libraryIndexPath();

// A successful book transfer marks the retained index stale. The next Library
// entry rebuilds it through the normal reconciliation path.
bool markLibraryIndexDirty();
bool isLibraryIndexDirty();

// Whether `path` (a full path or a bare name) passes the walk's name filter: a
// book extension, and no hidden (dot-prefixed) folder or file name.
bool isLibraryBookFile(std::string_view path);

// What Settings > Reader > Library index reports. Generations, never dates: the
// RTC backup is lost on this board, so every timestamp the device writes says
// Jan 1 1980 and an age-based "last indexed" line would be a lie.
struct IndexSummary {
  bool valid = false;
  uint16_t books = 0;
  uint16_t folders = 0;
  uint32_t generation = 0;
  bool metadataEnabled = false;
  bool ranksDegraded = false;
};

struct PartialSummary {
  bool present = false;
  uint16_t records = 0;
  uint16_t folders = 0;
  uint16_t total = 0;  // 0 = the paused walk never saw the end of the card
  uint32_t generation = 0;
};

bool readLibraryIndexSummary(IndexSummary& out);
bool readLibraryPartialSummary(PartialSummary& out);

// Throw away the journal. "Rebuild" and "Repair" begin with this: resuming a
// prefix is exactly what the reader just asked not to do.
void discardLibraryPartial();

}  // namespace library
