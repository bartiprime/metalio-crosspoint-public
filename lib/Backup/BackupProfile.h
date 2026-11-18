#pragma once
// ---------------------------------------------------------------------------
// BACKUP1 -- the versioned full-profile envelope, as pure code.
//
// This file is the CONTRACT, not the I/O. It parses, validates, clamps and
// plans rotation; it never touches an SD card, a singleton or an ESP32 API.
// That is deliberate: every safety rule the restore path depends on -- bad
// sha256, schema from the future, an out-of-range enum, a font family that
// is not installed, truncated JSON -- is exercised by a host test
// (test/backup_profile), so the device is not the first place those rules
// run.
//
// ENVELOPE (schema 1):
//   {
//     "crosspoint_backup": {
//       "schema": 1, "app": "r102", "device": "metalio-eink4-plus",
//       "created": "<ISO8601>", "sha256": "<of payload>", "kind": "full-profile"
//     },
//     "payload": { "settings": {...}, "reading": {...}, "credentials": {...} }
//   }
//
// The sha is over the canonical serialization of the `payload` value. The
// restore path recomputes it from the parsed payload, so a single flipped byte
// in any section rejects the whole file -- there is no partially-applied
// profile.
// ---------------------------------------------------------------------------
#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace backup {

inline constexpr int kSchema = 1;
inline constexpr const char* kApp = "r102";
inline constexpr const char* kDevice = "metalio-eink4-plus";
inline constexpr const char* kMagic = "crosspoint_backup";
inline constexpr const char* kKindFullProfile = "full-profile";
inline constexpr int kKeepBackups = 5;
// A profile is JSON text. This cap is generous for a card with thousands of
// books (one 10-byte progress blob per book) and still small enough that a
// hostile or corrupt file cannot exhaust the parse heap.
inline constexpr size_t kMaxBackupBytes = 256u * 1024u;
inline constexpr size_t kMaxProgressEntries = 8000;

enum class Reject : uint8_t {
  None = 0,
  NotJson,        // deserializeJson failed / not an object
  NotEnvelope,    // no crosspoint_backup object
  BadKind,        // kind is not full-profile
  SchemaTooNew,   // schema > kSchema, written by a newer firmware
  BadSha,         // payload hash mismatch
  BadPayload,     // payload missing / not an object
  MissingSettings,
  InvalidField,   // wrong type or a string that cannot be represented
  UnknownFont,    // sdFontFamilyName is not an installed family
  TooLarge,       // beyond kMaxBackupBytes
};

const char* rejectToken(Reject r);
const char* rejectText(Reject r);  // short human sentence, used in the UI/log

std::string sha256Hex(const std::string& data);

// Builds the whole envelope file text. `payload` is copied in; `created`
// is the ISO8601 stamp (caller formats it). Returns the JSON text, or "" if
// serialization failed (payload not serializable / empty).
std::string buildEnvelope(JsonVariantConst payload, const char* app, const char* created);

// Validates magic/kind/schema/sha on a parsed document. On Reject::None
// doc["payload"] is the payload object. Never mutates the document.
Reject parseEnvelope(JsonDocument& doc);

// ---- settings validation --------------------------------------------------
// Copies `in` into `out`, applying every rule below. Unknown keys are dropped
// (a restore never re-introduces a key this firmware does not know), numeric
// values are clamped, and `sdFontFamilyName` must name one of `fontFamilies`
// (the registry's live discovery) or the whole restore is rejected.
//
// Clamped values are reported in `notes` so the restore can log exactly what
// it changed instead of silently "fixing" a file.
struct SanitizeResult {
  Reject reason = Reject::None;
  std::string detail;
  std::vector<std::string> notes;
  int droppedKeys = 0;
};

SanitizeResult sanitizeSettings(JsonObjectConst in, const std::vector<std::string>& fontFamilies, JsonDocument& out);

// ---- rotation -------------------------------------------------------------
// Backup file names are `backup-<app>-YYYYMMDD-HHMM.json`. Lexicographic order
// equals chronological order, so rotation needs no filesystem timestamps:
// return every name beyond the newest `keep`, oldest first, for deletion.
std::vector<std::string> rotationPlan(const std::vector<std::string>& names, int keep = kKeepBackups);

// True for a bare backup file name (no path separators) that this feature
// wrote. Import only accepts these, so a stray file cannot be imported.
bool isBackupFileName(const std::string& name);
// Extracts the sortable key (YYYYMMDD-HHMM) from a backup file name; empty if it
// does not match the expected shape.
std::string backupStampOf(const std::string& name);

// ---- reading-state helpers ------------------------------------------------
// Book cache directories are `epub_<decimal>` / `xtc_<decimal>` (std::hash).
// The restore only writes into names that pass this check, so a payload cannot
// escape /.crosspoint with `../`.
bool isBookCacheDirName(const std::string& name);

// Progress bytes travel as lowercase hex (10 bytes -> 20 chars), not as a
// byte array: it is compact and unambiguous. Invalid hex fails the restore.
bool hexEncodeBytes(const uint8_t* data, size_t len, std::string& out);
bool hexDecodeBytes(const std::string& hex, std::vector<uint8_t>& out);

// ---- r163 B6: the vocabulary history --------------------------------------
// The history travels in the reading section as `reading.vocab`, the same shape
// the store itself writes ({ "words": [ {"w": ..., "b": ...}, ... ] }).
//
// WHY IT IS REGISTERED HERE: sanitizeSettings() drops any settings key this build
// does not know, so a persisted list that a restore can re-introduce has to be
// validated somewhere. VocabHistoryStore keeps its data in its own file rather than
// in settings.json (it is a list, and settings.json is parsed on the boot path), so
// this is the one place its bound is enforced on the way back in.
//
// THE BOUND: at most kMaxVocabEntries entries, each word clamped to
// kMaxVocabWordChars-1 bytes and each book title to kMaxVocabBookChars-1, with the
// same rule cost the store applies -- the firmware never writes more than that, so a
// backup file cannot either, and a hostile or corrupt file cannot hand the device a
// ten-thousand-entry list to hold in RAM.
inline constexpr size_t kMaxVocabEntries = 40;
inline constexpr size_t kMaxVocabWordChars = 48;
inline constexpr size_t kMaxVocabBookChars = 64;

struct VocabSanitizeResult {
  size_t kept = 0;
  size_t dropped = 0;  // entries past the cap, empty, or the wrong shape
};

// Copies `in` into `out` as a store-shaped document, clamped and truncated to the
// bounds above. Never rejects: the history is a convenience, so a damaged entry is
// dropped and the restore continues.
VocabSanitizeResult sanitizeVocabHistory(JsonObjectConst in, JsonDocument& out);

// ---- r163 3.3: the reading statistics -------------------------------------
// They travel in the reading section as `reading.readingStats`, the same shape the
// store itself writes ({ "books": [ {"path","title","pages","seconds"}, ... ],
// "pages": N, "seconds": N }). Registered for the same reason as the vocabulary
// history: the records live in their own file, so a restore is the one place their
// bounds are enforced on the way back in.
//
// THE BOUND, mirroring src/util/ReadingStats.h (whose constants these must match --
// that library cannot include firmware headers, so the numbers are spelled out):
// at most kMaxReadingStatBooks books, each with pages clamped to kMaxReadingStatPages
// and seconds to kMaxReadingStatSeconds, and path/title clamped to
// kMaxReadingStatPathChars-1 / kMaxReadingStatTitleChars-1 bytes. Books past the cap
// are dropped, never silently merged: the lifetime totals ("pages"/"seconds") are
// separate members and are clamped in place. An ABSENT readingStats member is left
// alone -- an older backup restores as "keep what is on the card", exactly like
// `vocab`.
inline constexpr size_t kMaxReadingStatBooks = 12;
inline constexpr uint32_t kMaxReadingStatPages = 10000000u;
inline constexpr uint32_t kMaxReadingStatSeconds = 100000000u;
inline constexpr size_t kMaxReadingStatTitleChars = 64;
inline constexpr size_t kMaxReadingStatPathChars = 180;

struct ReadingStatsSanitizeResult {
  size_t kept = 0;
  size_t dropped = 0;  // past the cap, empty path, or the wrong shape
  size_t clamped = 0;  // counters that were out of range and were clamped
};

// Copies `in` into `out` as a store-shaped document, clamped to the bounds above.
// Never rejects: the record is a convenience, so a damaged entry is dropped and the
// restore continues.
ReadingStatsSanitizeResult sanitizeReadingStats(JsonObjectConst in, JsonDocument& out);

}  // namespace backup
