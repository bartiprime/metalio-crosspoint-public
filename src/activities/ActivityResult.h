#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

struct WifiResult {
  bool connected = false;
  std::string ssid;
  std::string ip;
};

struct KeyboardResult {
  std::string text;
};

struct MenuResult {
  int action = -1;
  uint8_t orientation = 0;
  uint8_t pageTurnOption = 0;
};

struct ChapterResult {
  int spineIndex = 0;
  std::string anchor;
};

struct PercentResult {
  int percent = 0;
};

struct IntervalResult {
  uint32_t value = 0;
};

struct PageResult {
  uint32_t page = 0;
};

// SET3 (r164): the option a selector list was closed with. Index into the option
// list the selector was handed, never a stored value -- the caller owns the
// index -> settings mapping (see FontFamilyRow.h for the dynamic row).
struct OptionIndexResult {
  int index = 0;
};

struct ClippingResult {
  enum class Action : uint8_t { Clip, Lookup, Bookmark };
  Action action = Action::Clip;
  std::string text;
  uint16_t startPageOffset = 0;
  uint16_t endPageOffset = 0;
  uint16_t startWordIndex = 0;
  uint16_t endWordIndex = 0;
  uint16_t wordCount = 0;
  uint32_t startOffset = UINT32_MAX;
  uint32_t endOffset = UINT32_MAX;
  // C14 (upstream aaf50e44): true when the cursor (not the anchor) was on the FIRST word,
  // i.e. the selection was being extended leftwards. Restoring the selection has to put
  // the cursor back on that same end, or the next word picked inside the restored range
  // collapses the selection instead of extending it.
  bool cursorAtStart = false;
};

struct ProgressChangeResult {
  int spineIndex = 0;
  int page = 0;
  int totalPages = 0;
  std::string xpath;
  float percentage = 0.0f;
  bool hasSavedProgress = false;
  // Exact visible-codepoint offset within spineIndex, when the source (a bookmark) has one.
  // Preferred over xpath/percentage on resolution: it is immune to re-pagination.
  bool hasVisibleTextOffset = false;
  uint32_t visibleTextOffset = 0;
};

enum class NetworkMode;

struct NetworkModeResult {
  NetworkMode mode;
};

struct FootnoteResult {
  std::string href;
};

struct FilePathResult {
  std::string path;
};

using ResultVariant =
    std::variant<std::monostate, WifiResult, KeyboardResult, MenuResult, ChapterResult, PercentResult, IntervalResult,
                 PageResult, ClippingResult, ProgressChangeResult, NetworkModeResult, FootnoteResult, FilePathResult,
                 OptionIndexResult>;

struct ActivityResult {
  bool isCancelled = false;
  ResultVariant data;

  explicit ActivityResult() = default;

  template <typename ResultType>
    requires std::is_constructible_v<ResultVariant, ResultType&&>
  // cppcheck-suppress noExplicitConstructor
  ActivityResult(ResultType&& result) : data{std::forward<ResultType>(result)} {}
};

using ActivityResultHandler = std::function<void(const ActivityResult&)>;
