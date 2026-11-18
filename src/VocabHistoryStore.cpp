#include "VocabHistoryStore.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace {

// ASCII lower for the duplicate test only. A word that differs in case is the same word
// to a reader ("Apple" after "apple"), and the list should move it to the front rather
// than show it twice.
char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool sameWord(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (asciiLower(a[i]) != asciiLower(b[i])) return false;
  }
  return true;
}

// Truncate to at most `maxBytes`, never inside a UTF-8 sequence. A word cut mid-codepoint
// would be stored as bytes no font can render, and its length would then be wrong for
// every later comparison.
std::string clampUtf8(const std::string& text, const size_t maxBytes) {
  if (text.size() <= maxBytes) return text;
  size_t end = maxBytes;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
  return text.substr(0, end);
}

// Newlines and tabs would break a one-line list row; collapse them to spaces.
std::string oneLine(const std::string& text) {
  std::string out = text;
  for (char& c : out) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  }
  return out;
}

}  // namespace

void VocabHistoryStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["words"].to<JsonArray>();
  for (const auto& entry : entries) {
    JsonObject obj = arr.add<JsonObject>();
    obj["w"] = entry.word;
    obj["b"] = entry.book;
  }
}

bool VocabHistoryStore::fromJson(JsonVariantConst doc) {
  // A missing or malformed 'words' key is an empty list, not a failure: the file is a
  // convenience, and losing it must never block a boot.
  entries.clear();
  JsonArrayConst arr = doc["words"].as<JsonArrayConst>();
  entries.reserve(std::min(arr.size(), static_cast<size_t>(MAX_ENTRIES)));
  for (JsonObjectConst obj : arr) {
    if (getCount() >= MAX_ENTRIES) break;
    VocabEntry entry;
    // Read as const char*, never as std::string: the std::string converter drags a
    // per-TU copy of the JSON serializer into flash (see PersistableStore.h).
    entry.word = oneLine(obj["w"] | "");
    entry.book = oneLine(obj["b"] | "");
    if (entry.word.empty()) continue;
    entries.push_back(std::move(entry));
  }
  LOG_DBG("VOCAB", "Vocabulary history loaded (%d entries)", getCount());
  return true;
}

void VocabHistoryStore::addLookup(const std::string& word, const std::string& book) {
  const std::string clampedWord = clampUtf8(oneLine(word), WORD_MAX_CHARS - 1);
  if (clampedWord.empty()) return;
  const std::string clampedBook = clampUtf8(oneLine(book), BOOK_MAX_CHARS - 1);

  auto it = std::find_if(entries.begin(), entries.end(), [&](const VocabEntry& entry) {
    return sameWord(entry.word, clampedWord) && entry.book == clampedBook;
  });
  if (it != entries.end()) {
    // Same word from the same book: move it to the front without re-allocating the
    // strings (they are already at their clamped length).
    std::rotate(entries.begin(), it, it + 1);
    if (entries.front().book.empty() && !clampedBook.empty()) entries.front().book = clampedBook;
  } else {
    entries.insert(entries.begin(), {clampedWord, clampedBook});
    if (static_cast<int>(entries.size()) > MAX_ENTRIES) {
      entries.resize(MAX_ENTRIES);
    }
  }
  saveToFile();
}
