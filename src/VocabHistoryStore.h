#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

// r163 B6: the words this card has looked up, most recent first.
//
// The list is deliberately a store of its own (/.crosspoint/vocab.json) rather than a
// field of CrossPointSettings: it is a growing list, not a setting, and settings.json is
// parsed on the boot path where size matters. It follows RecentBooksStore exactly --
// same base class, same JSON shapes, same "write on change, tolerate a missing file"
// contract -- so it needs no new persistence machinery.
//
// BOUND: 40 entries. Two reasons. The screen is a look-back aid, not an archive, so past
// a screenful of pages the oldest words are not what anyone scrolls to; and the whole
// list is held in RAM as std::string pairs, so a fixed count is what keeps its footprint
// predictable on a device that hands out heap in single-digit KB. Each entry is also
// clamped: word to WORD_MAX_CHARS-1 bytes, book title to BOOK_MAX_CHARS-1. Worst case
// on disk is therefore 40 * (~48 + ~64 + JSON overhead) ~= 6 KB, and a typical entry is
// 20-30 bytes. Duplicates (same word, same book, case-insensitive) move to the front
// instead of being added again, so re-lookups cost no space.
//
// The SAME bound is enforced on the restore path by backup::sanitizeVocabHistory
// (lib/Backup/BackupProfile.h, which spells the numbers out because that library must
// not include firmware headers). Change one and the other must change with it.
struct VocabEntry {
  std::string word;
  std::string book;
};

class VocabHistoryStore : public PersistableStore<VocabHistoryStore> {
 public:
  static constexpr int MAX_ENTRIES = 40;
  static constexpr size_t WORD_MAX_CHARS = 48;
  static constexpr size_t BOOK_MAX_CHARS = 64;

 private:
  std::vector<VocabEntry> entries;

  VocabHistoryStore() = default;
  ~VocabHistoryStore() = default;

  friend class PersistableStore<VocabHistoryStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/vocab.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Record one successful lookup. Moves an existing (word, book) pair to the front,
  // otherwise inserts at the front and drops the oldest entry past MAX_ENTRIES.
  // Persists on change. An empty word is ignored.
  void addLookup(const std::string& word, const std::string& book);

  const std::vector<VocabEntry>& getEntries() const { return entries; }
  int getCount() const { return static_cast<int>(entries.size()); }
};

// r163 fix: this was `#define VOCAB_HISTORY`, an object-like macro that rewrote every occurrence of
// the token -- including `EpubReaderMenuActivity::MenuAction::VOCAB_HISTORY`, which then expanded to
// `MenuAction::VocabHistoryStore::getInstance()` and failed to compile. The store accessor is named
// distinctly so it can never eat a same-named enum value.
#define VOCAB_HISTORY_STORE VocabHistoryStore::getInstance()
