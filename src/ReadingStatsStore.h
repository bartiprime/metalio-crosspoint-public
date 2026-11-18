#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

#include "util/ReadingStats.h"

// r163 3.3 -- the reading statistics this card has accumulated, per book and overall.
//
// A store of its own (/.crosspoint/reading_stats.json) rather than fields of
// CrossPointSettings, for the reason VocabHistoryStore gives: it is a list that grows, and
// settings.json is parsed on the boot path where size matters. It follows RecentBooksStore
// and VocabHistoryStore exactly -- same base class, same "write on change, tolerate a
// missing file" contract.
//
// WHO FEEDS IT, AND WHEN. The reader's own session accounting (ReaderSession, whose dwell
// clock already measured "time on this page before the reader advanced") supplies the
// delta; ReaderActivity::flushReaderSession() hands it here at reader exit and at sleep.
// So a page turn is a counter increment in RAM, and the SD write happens once per reader
// session, not once per page -- the same coalescing shape as ProgressSaves.
//
// THE BOUND: kMaxBooks (12) entries, most recently read first, plus a lifetime total that
// is NOT the sum of the entries on screen -- evicting a book past the twelfth must not
// rewrite the "All books" block downwards, so the totals are stored and accumulated
// separately. Per-entry pages and seconds saturate at readingstats::kMaxPagesPerBook /
// kMaxSecondsPerBook (also the restore clamp in lib/Backup/BackupProfile.h). Paths are
// clamped to PATH_MAX_CHARS-1 and titles to TITLE_MAX_CHARS-1 bytes, so the worst case on
// disk is 12 * (~180 + ~64 + ~40) bytes ~= 3.5 KB and a typical file is well under 1 KB.
// The whole list lives in RAM as std::string pairs, so a fixed count is what keeps the
// footprint predictable on a device that hands out heap in single-digit KB.
struct ReadingBookStats {
  std::string path;
  std::string title;
  readingstats::Tally tally;
};

class ReadingStatsStore : public PersistableStore<ReadingStatsStore> {
 public:
  static constexpr int MAX_BOOKS = readingstats::kMaxBooks;
  static constexpr size_t TITLE_MAX_CHARS = 64;
  static constexpr size_t PATH_MAX_CHARS = 180;

 private:
  std::vector<ReadingBookStats> books;
  // Lifetime totals. Kept even when an entry is evicted past MAX_BOOKS.
  readingstats::Tally overall;

  ReadingStatsStore() = default;
  ~ReadingStatsStore() = default;

  friend class PersistableStore<ReadingStatsStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/reading_stats.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Add one reader session's contribution for `path`: accumulated into the entry if the
  // book is already known (and moved to the front), otherwise a new front entry with the
  // oldest beyond MAX_BOOKS dropped. Always adds to the lifetime totals. Persists on
  // change; a no-op contribution (0 pages and 0 seconds) writes nothing.
  void record(const std::string& path, const std::string& title, uint32_t pages, uint32_t seconds);

  void clear();

  const std::vector<ReadingBookStats>& getBooks() const { return books; }
  readingstats::Tally getOverall() const { return overall; }
  int getCount() const { return static_cast<int>(books.size()); }
  bool empty() const { return books.empty(); }
};

#define READING_STATS ReadingStatsStore::getInstance()
