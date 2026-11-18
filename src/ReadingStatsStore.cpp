#include "ReadingStatsStore.h"

#include <Logging.h>

#include <algorithm>

namespace {

// Truncate to at most `maxBytes`, never inside a UTF-8 sequence: a title cut mid-codepoint
// would be stored as bytes no font can draw. Same rule as VocabHistoryStore.
std::string clampUtf8(const std::string& text, const size_t maxBytes) {
  if (text.size() <= maxBytes) return text;
  size_t end = maxBytes;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
  return text.substr(0, end);
}

// Newlines and tabs would break a one-line list row.
std::string oneLine(const std::string& text) {
  std::string out = text;
  for (char& c : out) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  }
  return out;
}

}  // namespace

void ReadingStatsStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : books) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path.c_str();
    obj["title"] = book.title.c_str();
    obj["pages"] = book.tally.pages;
    obj["seconds"] = book.tally.seconds;
  }
  doc["pages"] = overall.pages;
  doc["seconds"] = overall.seconds;
}

bool ReadingStatsStore::fromJson(JsonVariantConst doc) {
  // A missing or malformed member is an empty record, not a failure: the file is a
  // convenience, and losing it must never block a boot.
  books.clear();
  overall = {};
  const uint32_t storedPages = doc["pages"] | 0u;
  const uint32_t storedSeconds = doc["seconds"] | 0u;
  overall.pages = std::min(storedPages, readingstats::kMaxPagesPerBook);
  overall.seconds = std::min(storedSeconds, readingstats::kMaxSecondsPerBook);

  JsonArrayConst arr = doc["books"].as<JsonArrayConst>();
  books.reserve(std::min(arr.size(), static_cast<size_t>(MAX_BOOKS)));
  for (JsonObjectConst obj : arr) {
    if (getCount() >= MAX_BOOKS) break;
    ReadingBookStats book;
    // Read as const char*, never as std::string: the std::string converter drags a per-TU
    // copy of the JSON serializer into flash (see PersistableStore.h).
    book.path = clampUtf8(oneLine(obj["path"] | ""), PATH_MAX_CHARS - 1);
    if (book.path.empty()) continue;
    book.title = clampUtf8(oneLine(obj["title"] | ""), TITLE_MAX_CHARS - 1);
    // A hand-edited or truncated file cannot hand the device counters that will wrap.
    book.tally.pages = std::min<uint32_t>(obj["pages"] | 0u, readingstats::kMaxPagesPerBook);
    book.tally.seconds = std::min<uint32_t>(obj["seconds"] | 0u, readingstats::kMaxSecondsPerBook);
    books.push_back(std::move(book));
  }
  LOG_DBG("RSTATS", "Reading statistics loaded (%d books, %lu s)", getCount(),
          static_cast<unsigned long>(overall.seconds));
  return true;
}

void ReadingStatsStore::record(const std::string& path, const std::string& title, const uint32_t pages,
                               const uint32_t seconds) {
  if (pages == 0 && seconds == 0) return;
  const std::string clampedPath = clampUtf8(path, PATH_MAX_CHARS - 1);
  if (clampedPath.empty()) return;
  const std::string clampedTitle = clampUtf8(oneLine(title), TITLE_MAX_CHARS - 1);

  readingstats::accumulate(overall, pages, seconds);

  auto it =
      std::find_if(books.begin(), books.end(), [&](const ReadingBookStats& book) { return book.path == clampedPath; });
  if (it != books.end()) {
    // Re-read book: accumulate in place, then move to the front without re-allocating the
    // strings (they are already at their clamped length).
    readingstats::accumulate(it->tally, pages, seconds);
    if (it->title.empty() && !clampedTitle.empty()) it->title = clampedTitle;
    std::rotate(books.begin(), it, it + 1);
  } else {
    ReadingBookStats book;
    book.path = clampedPath;
    book.title = clampedTitle;
    readingstats::accumulate(book.tally, pages, seconds);
    books.insert(books.begin(), std::move(book));
    if (static_cast<int>(books.size()) > MAX_BOOKS) {
      books.resize(MAX_BOOKS);
    }
  }
  saveToFile();
}

void ReadingStatsStore::clear() {
  if (books.empty() && overall.pages == 0 && overall.seconds == 0) return;
  books.clear();
  overall = {};
  saveToFile();
}
