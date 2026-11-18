#include "BackupProfile.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "BackupSha256.h"

namespace backup {
namespace {

// Inclusive clamp range for one persisted settings key. Values outside are
// clamped, never silently replaced by a default: the note in SanitizeResult
// tells the operator which field moved.
struct IntRule {
  const char* key;
  int lo;
  int hi;
};

// Toggles. These are the numeric settings whose only legal values are 0 and 1.
constexpr IntRule kToggles[] = {
    {"equalizeBlack", 0, 1},
    {"fadingFix", 0, 1},
    {"frontlightRestoreOnWake", 0, 1},
    {"screenInverted", 0, 1},
    {"embeddedStyle", 0, 1},
    {"focusReadingEnabled", 0, 1},
    {"hyphenationEnabled", 0, 1},
    {"extraParagraphSpacing", 0, 1},
    {"textAntiAliasing", 0, 1},
    {"menuAntiAliasing", 0, 1},
    {"softKeyHaptic", 0, 1},
    {"inputDuringRefresh", 0, 1},
    {"edgeDragFrontlight", 0, 1},
    {"audioOutEnabled", 0, 1},
    {"uiSoundsEnabled", 0, 1},
    {"frontButtonFollowOrientation", 0, 1},
    {"doubleClickPwrLight", 0, 1},
    {"pwrBtnFootnoteBack", 0, 1},
    {"pluginsEnabled", 0, 1},
    {"pluginHubPromptHidden", 0, 1},
    {"showHiddenFiles", 0, 1},
    {"showFileExtensions", 0, 1},
    {"usbDriveExitDialog", 0, 1},
    {"libraryUseMetadata", 0, 1},
    {"libraryFixedRowFace", 0, 1},
    {"removeReadBooksFromRecents", 0, 1},
    {"moveFinishedToReadFolder", 0, 1},
    {"frontlightOn", 0, 1},
    {"statusBarChapterPageCount", 0, 1},
    {"statusBarBookProgressPercentage", 0, 1},
    {"statusBarBattery", 0, 1},
    {"clockShowHeader", 0, 1},
    {"clockHasBeenSynced", 0, 1},
    {"clockFormat", 0, 1},
    {"usbDebugShell", 0, 1},
    {"usbDriveExitDialogMigrated", 0, 1},
    {"lineSpacingPercentMigrated", 0, 1},
};

// Enumerated / ranged numeric settings. Upper bounds are the LAST legal index
// (count - 1), read off the enums in CrossPointSettings.h.
constexpr IntRule kRanged[] = {
    {"fontFamily", 0, 2},                 // 0 built-in, 1 legacy NotoSerif, 2 legacy OpenDyslexic
    {"fontSize", 6, 48},                  // point size; the reader snaps to what the face ships
    {"uiTheme", 0, 5},                    // CLASSIC .. ANDRO
    {"sleepScreen", 0, 8},                // SLEEP_SCREEN_MODE
    // r162 FIX5: keys that were persisted but had NO rule here. A restore writes
    // the sanitized object as the WHOLE settings file, so an absent key is not
    // "keep what I have" -- it loads as the field's compiled-in default. Each
    // bound below is (enum count - 1) from CrossPointSettings.h.
    {"powerOffScreen", 0, 4},             // POWER_OFF_SCREEN_MODE_COUNT - 1
    {"sideButtonLayout", 0, 4},           // SIDE_BUTTON_LAYOUT_COUNT - 1
    {"homeButtonTapAction", 0, 11},       // HomeButtonAction::Count - 1
    {"homeButtonDoubleTapAction", 0, 11},
    {"homeButtonLongPressAction", 0, 11},
    {"bootKeyTapAction", 0, 2},           // BOOT_KEY_ACTION_COUNT - 1
    {"bootKeyLongPressAction", 0, 2},
    {"sleepScreenCoverMode", 0, 1},
    {"sleepScreenCoverFilter", 0, 2},
    {"quickResumeSleepScreen", 0, 1},
    {"hideBatteryPercentage", 0, 2},
    {"autoRefreshThreshold", 0, 4},
    {"menuTransitionRefresh", 0, 4},
    {"activityTransitionRefresh", 0, 4},
    {"swipeSensitivity", 0, 3},
    {"slideSwitchAction", 0, 2},
    {"lineHeight", 0, 6},           // PRE-r163 ladder slot; fromJson() folds it to a percent
    {"lineSpacingPercent", 100, 200},  // LINE_SPACING_MIN_PERCENT..MAX_PERCENT, 1% steps
    // r162 FIX5: these three were 0..5 in r161 and every restore therefore wrote
    // word spacing 5 % and a 5 px margin. The bounds below are the fields' OWN
    // declared ranges (CrossPointSettings.h WORD_SPACING_MIN/MAX, the 5-slot
    // character-spacing enum, SCREEN_MARGIN_MIN/MAX), spelled as literals because
    // this become-Shared library must not include the firmware settings header.
    {"wordSpacing", 50, 200},      // WORD_SPACING_MIN..MAX, percent of the space advance
    {"characterSpacing", 0, 4},    // 5 enum slots, stored 0..4 = -2..+2 px offset
    {"screenMargin", 5, 40},       // SCREEN_MARGIN_MIN..MAX, px
    {"paragraphAlignment", 0, 4},
    {"paragraphIndentSpaces", 0, 5},
    {"orientation", 0, 3},
    {"imageRendering", 0, 2},
    // CAD1: 0..6 -- slot 6 is 60 pages, appended after Never so every slot an
    // older file or backup carries still means its own page count.
    {"refreshFrequency", 0, 6},
    {"quickRefreshPages", 0, 6},
    {"readerMenuStyle", 0, 1},
    {"vibration", 0, 2},
    {"hapticIntensity", 0, 2},
    {"touchReaderControls", 0, 1},
    {"pageTurnGesture", 0, 3},
    {"previousPageGesture", 0, 3},
    {"tapForReaderMenu", 0, 2},
    {"longPressButtonBehavior", 0, 2},
    {"tiltPageTurn", 0, 2},
    {"sleepTimeoutMinutes", 1, 31},
    {"standbyAutoOffMinutes", 0, 120},
    {"chargeIdlePolicy", 0, 1},  // r163 3.5: 0 off (default) / 1 keep awake while charging
    {"chargeCurrent", 0, 255},
    {"frontlightBrightness", 0, 100},
    {"frontlightWarmth", 0, 100},
    {"statusBarProgressBar", 0, 2},
    {"statusBarProgressBarThickness", 0, 2},
    {"statusBarTitle", 0, 2},
    {"statusBarClock", 0, 2},
    {"xtcStatusBarMode", 0, 2},
    {"clockUtcOffsetQ", 0, 104},
    {"clockTimezone", 0, 255},
    {"clockDst", 0, 2},
    {"shortPwrBtn", 0, 9},
    {"longPressMenuFunction", 0, 6},
    {"frontButtonBack", 0, 3},
    {"frontButtonConfirm", 0, 3},
    {"frontButtonLeft", 0, 3},
    {"frontButtonRight", 0, 3},
    {"opdsFilenameFormat", 0, 255},
    {"idleCpuFreq", 0, 255},
    {"panelVcomDepth", 0, 255},
    {"keyboardLayouts", 0, 65535},
};

// Persisted strings. CrossPointSettings caps the two char[32] fields at 31
// characters; the settings loader truncates the same way, so a restore does
// too. `language` is an ISO code string and is checked for shape instead.
// r162 FIX5: opdsDownloadFolder (char[64]) was missing from this list entirely,
// so a restore silently dropped it and the OPDS destination fell back to the SD
// root. Its limit is the loader's own truncation, stringMaxLen - 1 = 63.
struct StringRule {
  const char* key;
  size_t maxLen;
};

constexpr StringRule kStrings[] = {
    {"sdFontFamilyName", 31},
    {"dictionaryName", 31},
    {"opdsDownloadFolder", 63},
};

bool inTable(const IntRule* table, size_t n, const char* key, const IntRule*& rule) {
  for (size_t i = 0; i < n; i++) {
    if (std::strcmp(table[i].key, key) == 0) {
      rule = &table[i];
      return true;
    }
  }
  return false;
}

bool isUpperCode(const char* s) {
  if (s == nullptr) return false;
  const size_t n = std::strlen(s);
  if (n < 2 || n > 4) return false;
  for (size_t i = 0; i < n; i++) {
    if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9'))) return false;
  }
  return true;
}

void noteClamp(SanitizeResult& r, const char* key, long long from, int to) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%s: %lld -> %d", key, from, to);
  r.notes.emplace_back(buf);
}

}  // namespace

const char* rejectToken(Reject r) {
  switch (r) {
    case Reject::None:
      return "ok";
    case Reject::NotJson:
      return "not-json";
    case Reject::NotEnvelope:
      return "not-envelope";
    case Reject::BadKind:
      return "bad-kind";
    case Reject::SchemaTooNew:
      return "schema-too-new";
    case Reject::BadSha:
      return "bad-sha256";
    case Reject::BadPayload:
      return "bad-payload";
    case Reject::MissingSettings:
      return "missing-settings";
    case Reject::InvalidField:
      return "invalid-field";
    case Reject::UnknownFont:
      return "unknown-font";
    case Reject::TooLarge:
      return "too-large";
  }
  return "unknown";
}

const char* rejectText(Reject r) {
  switch (r) {
    case Reject::None:
      return "OK";
    case Reject::NotJson:
      return "Not a readable backup file";
    case Reject::NotEnvelope:
      return "Not a CrossPoint backup";
    case Reject::BadKind:
      return "Not a full-profile backup";
    case Reject::SchemaTooNew:
      return "Backup made by a newer firmware";
    case Reject::BadSha:
      return "Backup is damaged (checksum mismatch)";
    case Reject::BadPayload:
      return "Backup has no payload";
    case Reject::MissingSettings:
      return "Backup has no settings section";
    case Reject::InvalidField:
      return "Backup contains an invalid setting";
    case Reject::UnknownFont:
      return "Font family is not installed on this device";
    case Reject::TooLarge:
      return "Backup file is too large";
  }
  return "Unknown failure";
}

std::string buildEnvelope(JsonVariantConst payload, const char* app, const char* created) {
  if (!payload.is<JsonObjectConst>()) return std::string();

  JsonDocument doc;
  JsonObject meta = doc[kMagic].to<JsonObject>();
  meta["schema"] = kSchema;
  meta["app"] = app != nullptr ? app : kApp;
  meta["device"] = kDevice;
  meta["created"] = created != nullptr ? created : "";
  meta["kind"] = kKindFullProfile;
  JsonObject outPayload = doc["payload"].to<JsonObject>();
  outPayload.set(payload);

  std::string canonical;
  serializeJson(doc["payload"], canonical);
  if (canonical.empty()) return std::string();
  meta["sha256"] = sha256Hex(canonical);

  std::string text;
  serializeJson(doc, text);
  return text;
}

Reject parseEnvelope(JsonDocument& doc) {
  if (!doc.is<JsonObjectConst>()) return Reject::NotJson;
  JsonObjectConst meta = doc[kMagic].as<JsonObjectConst>();
  if (meta.isNull()) return Reject::NotEnvelope;

  const int schema = meta["schema"] | 0;
  if (schema < 1) return Reject::NotEnvelope;
  if (schema > kSchema) return Reject::SchemaTooNew;

  const char* kind = meta["kind"] | "";
  if (std::strcmp(kind, kKindFullProfile) != 0) return Reject::BadKind;

  const char* sha = meta["sha256"] | "";
  if (std::strlen(sha) != 64) return Reject::BadSha;

  JsonVariantConst payload = doc["payload"];
  if (!payload.is<JsonObjectConst>()) return Reject::BadPayload;

  std::string canonical;
  serializeJson(payload, canonical);
  if (canonical.empty()) return Reject::BadPayload;
  if (sha256Hex(canonical) != std::string(sha)) return Reject::BadSha;
  return Reject::None;
}

SanitizeResult sanitizeSettings(JsonObjectConst in, const std::vector<std::string>& fontFamilies,
                                JsonDocument& out) {
  SanitizeResult r;
  out.clear();
  if (in.isNull()) {
    r.reason = Reject::MissingSettings;
    r.detail = "settings section is missing";
    return r;
  }

  JsonObject dst = out.to<JsonObject>();
  for (JsonPairConst kv : in) {
    const char* key = kv.key().c_str();
    JsonVariantConst v = kv.value();

    // sdFontFamilyName is the one value with an external requirement: the
    // family must actually be installed, or the reader would render nothing.
    if (std::strcmp(key, "sdFontFamilyName") == 0) {
      if (!v.is<const char*>()) {
        r.reason = Reject::InvalidField;
        r.detail = "sdFontFamilyName is not a string";
        return r;
      }
      const char* name = v.as<const char*>();
      if (name[0] != '\0') {
        const bool found = std::find(fontFamilies.begin(), fontFamilies.end(), std::string(name)) != fontFamilies.end();
        if (!found) {
          r.reason = Reject::UnknownFont;
          r.detail = std::string("font family '") + name + "' is not installed";
          return r;
        }
      }
      dst[key] = name;
      continue;
    }

    if (std::strcmp(key, "language") == 0) {
      if (!v.is<const char*>()) {
        r.reason = Reject::InvalidField;
        r.detail = "language is not a string";
        return r;
      }
      const char* code = v.as<const char*>();
      if (!isUpperCode(code)) {
        r.reason = Reject::InvalidField;
        r.detail = std::string("language code '") + code + "' is malformed";
        return r;
      }
      dst[key] = code;
      continue;
    }

    bool knownString = false;
    size_t stringMaxLen = 0;
    for (const auto& s : kStrings) {
      if (std::strcmp(key, s.key) == 0) {
        knownString = true;
        stringMaxLen = s.maxLen;
        break;
      }
    }
    if (knownString) {
      if (!v.is<const char*>()) {
        r.reason = Reject::InvalidField;
        r.detail = std::string(key) + " is not a string";
        return r;
      }
      const char* text = v.as<const char*>();
      if (std::strlen(text) > stringMaxLen) {
        r.reason = Reject::InvalidField;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s is longer than %u characters", key, static_cast<unsigned>(stringMaxLen));
        r.detail = buf;
        return r;
      }
      dst[key] = text;
      continue;
    }

    const IntRule* rule = nullptr;
    if (inTable(kToggles, sizeof(kToggles) / sizeof(kToggles[0]), key, rule) ||
        inTable(kRanged, sizeof(kRanged) / sizeof(kRanged[0]), key, rule)) {
      if (!v.is<long long>()) {
        r.reason = Reject::InvalidField;
        r.detail = std::string(key) + " is not a number";
        return r;
      }
      long long value = v.as<long long>();
      const long long lo = rule->lo;
      const long long hi = rule->hi;
      if (value < lo || value > hi) {
        const long long clamped = value < lo ? lo : hi;
        noteClamp(r, key, value, static_cast<int>(clamped));
        value = clamped;
      }
      dst[key] = static_cast<int>(value);
      continue;
    }

    // Unknown to this firmware: drop it. A restore must not re-introduce a
    // key whose meaning this build does not implement.
    if (!v.is<const char*>() && !v.is<long long>()) {
      r.droppedKeys++;
      continue;
    }
    r.droppedKeys++;
  }
  return r;
}

std::string backupStampOf(const std::string& name) {
  if (!isBackupFileName(name)) return std::string();
  const std::string stem = name.substr(0, name.size() - 5);  // drop ".json"
  return stem.substr(stem.size() - 13);
}

bool isBackupFileName(const std::string& name) {
  if (name.empty() || name.size() > 96) return false;
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return false;
  if (name.find("..") != std::string::npos) return false;
  if (name.rfind("backup-", 0) != 0) return false;
  if (name.size() <= 20) return false;
  if (name.compare(name.size() - 5, 5, ".json") != 0) return false;

  // Stem must end in "-YYYYMMDD-HHMM": 13 digits/one dash, preceded by '-'.
  const std::string stem = name.substr(0, name.size() - 5);  // backup-<app>-<stamp>
  if (stem.size() < 20) return false;
  const size_t stampAt = stem.size() - 13;
  if (stampAt == 0 || stem[stampAt - 1] != '-') return false;
  for (size_t i = stampAt; i < stem.size(); i++) {
    if (i == stampAt + 8) {
      if (stem[i] != '-') return false;
    } else if (!std::isdigit(static_cast<unsigned char>(stem[i]))) {
      return false;
    }
  }
  return true;
}

std::vector<std::string> rotationPlan(const std::vector<std::string>& names, int keep) {
  std::vector<std::string> sorted;
  sorted.reserve(names.size());
  for (const auto& n : names) {
    if (backupStampOf(n).empty()) continue;  // never delete a file this feature did not write
    sorted.push_back(n);
  }
  std::sort(sorted.begin(), sorted.end(), [](const std::string& a, const std::string& b) {
    return backupStampOf(a) < backupStampOf(b);  // oldest first
  });
  std::vector<std::string> doomed;
  const size_t keepCount = keep < 0 ? 0 : static_cast<size_t>(keep);
  const size_t total = sorted.size();
  const size_t deleteCount = total > keepCount ? total - keepCount : 0;
  for (size_t i = 0; i < deleteCount; i++) doomed.push_back(sorted[i]);
  return doomed;
}

bool isBookCacheDirName(const std::string& name) {
  size_t start = 0;
  if (name.rfind("epub_", 0) == 0) {
    start = 5;
  } else if (name.rfind("xtc_", 0) == 0) {
    start = 4;
  } else {
    return false;
  }
  if (name.size() <= start) return false;
  for (size_t i = start; i < name.size(); i++) {
    if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
  }
  return true;
}

bool hexEncodeBytes(const uint8_t* data, size_t len, std::string& out) {
  out.clear();
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    char pair[3];
    std::snprintf(pair, sizeof(pair), "%02x", static_cast<unsigned>(data[i]));
    out.push_back(pair[0]);
    out.push_back(pair[1]);
  }
  return true;
}

bool hexDecodeBytes(const std::string& hex, std::vector<uint8_t>& out) {
  out.clear();
  if (hex.size() % 2 != 0) return false;
  out.reserve(hex.size() / 2);
  auto nibble = [](char c, int& v) {
    if (c >= '0' && c <= '9') { v = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
    return false;
  };
  for (size_t i = 0; i < hex.size(); i += 2) {
    int hi = 0, lo = 0;
    if (!nibble(hex[i], hi) || !nibble(hex[i + 1], lo)) return false;
    out.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

namespace {

// Clamp a UTF-8 string to `maxBytes` characters, never cutting a sequence in half:
// a half codepoint would be stored, listed and compared as bytes no font can draw.
std::string clampUtf8ForBackup(const char* text, const size_t maxBytes) {
  if (text == nullptr) return std::string();
  const size_t len = std::strlen(text);
  if (len <= maxBytes) return std::string(text, len);
  size_t end = maxBytes;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
  return std::string(text, end);
}

}  // namespace

VocabSanitizeResult sanitizeVocabHistory(JsonObjectConst in, JsonDocument& out) {
  VocabSanitizeResult r;
  out.clear();
  JsonArray dst = out["words"].to<JsonArray>();
  if (in.isNull()) return r;

  JsonArrayConst src = in["words"].as<JsonArrayConst>();
  for (JsonObjectConst entry : src) {
    if (r.kept >= kMaxVocabEntries) {
      r.dropped++;
      continue;
    }
    const char* rawWord = entry["w"] | "";
    const char* rawBook = entry["b"] | "";
    const std::string word = clampUtf8ForBackup(rawWord, kMaxVocabWordChars - 1);
    if (word.empty()) {
      r.dropped++;
      continue;
    }
    const std::string book = clampUtf8ForBackup(rawBook, kMaxVocabBookChars - 1);
    JsonObject o = dst.add<JsonObject>();
    o["w"] = word.c_str();
    o["b"] = book.c_str();
    r.kept++;
  }
  // Anything past the cap is counted too, so the restore log names the real loss.
  const size_t total = src.size();
  if (total > r.kept + r.dropped) r.dropped = total - r.kept;
  return r;
}

namespace {

// A counter from a file that may have been hand-edited: negative, absent and absurd all
// land on a legal value. `clamped` is set when the value moved, so the restore log can say
// so instead of quietly repairing it.
uint32_t clampCounter(const long raw, const uint32_t max, bool& clamped) {
  if (raw < 0) {
    clamped = true;
    return 0;
  }
  if (static_cast<unsigned long>(raw) > max) {
    clamped = true;
    return max;
  }
  return static_cast<uint32_t>(raw);
}

}  // namespace

ReadingStatsSanitizeResult sanitizeReadingStats(JsonObjectConst in, JsonDocument& out) {
  ReadingStatsSanitizeResult r;
  out.clear();
  JsonArray dst = out["books"].to<JsonArray>();
  // Totals are written even for an absent section, so the staged document is always a
  // complete store-shaped file rather than half a record.
  bool pagesClamped = false;
  bool secondsClamped = false;
  out["pages"] = clampCounter(in["pages"] | 0L, kMaxReadingStatPages, pagesClamped);
  out["seconds"] = clampCounter(in["seconds"] | 0L, kMaxReadingStatSeconds, secondsClamped);
  if (in.isNull()) return r;
  if (pagesClamped) r.clamped++;
  if (secondsClamped) r.clamped++;

  JsonArrayConst src = in["books"].as<JsonArrayConst>();
  for (JsonObjectConst entry : src) {
    if (r.kept >= kMaxReadingStatBooks) {
      r.dropped++;
      continue;
    }
    const std::string path = clampUtf8ForBackup(entry["path"] | "", kMaxReadingStatPathChars - 1);
    if (path.empty()) {
      r.dropped++;
      continue;
    }
    const std::string title = clampUtf8ForBackup(entry["title"] | "", kMaxReadingStatTitleChars - 1);
    bool clamped = false;
    const uint32_t pages = clampCounter(entry["pages"] | 0L, kMaxReadingStatPages, clamped);
    const uint32_t seconds = clampCounter(entry["seconds"] | 0L, kMaxReadingStatSeconds, clamped);
    if (clamped) r.clamped++;
    JsonObject o = dst.add<JsonObject>();
    o["path"] = path.c_str();
    o["title"] = title.c_str();
    o["pages"] = pages;
    o["seconds"] = seconds;
    r.kept++;
  }
  // Anything past the cap is counted too, so the restore log names the real loss.
  const size_t total = src.size();
  if (total > r.kept + r.dropped) r.dropped = total - r.kept;
  return r;
}

}  // namespace backup
