#include "BackupService.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "VocabHistoryStore.h"
#include "SdCardFontSystem.h"
#include <PersistableStore.h>
#include "WifiCredentialStore.h"
#include "activities/reader/ProgressFile.h"

namespace backupsvc {
namespace {

constexpr const char* TAG = "BACKUP1";
constexpr size_t kMaxProgressBlob = 64;  // real files are 4/6/10 bytes

void logReject(const RestoreResult& r, const std::string& name) {
  LOG_ERR(TAG, "restore %s rejected: %s (%s)", name.c_str(), backup::rejectToken(r.reason), r.detail.c_str());
}

std::string baseNameOnly(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

void collectFontFamilies(std::vector<std::string>& out) {
  out.clear();
  const auto& families = sdFontSystem.registry().getFamilies();
  out.reserve(families.size());
  for (const auto& f : families) out.push_back(f.name);
}

// Reads the whole file into `out`, enforcing the cap without allocating it
// first (a corrupt 40 MB file must fail, not OOM).
bool readBackupText(const std::string& path, std::string& out, backup::Reject& reason) {
  out.clear();
  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) {
    reason = backup::Reject::NotJson;
    return false;
  }
  const size_t size = file.fileSize();
  if (size == 0 || size > backup::kMaxBackupBytes) {
    reason = size == 0 ? backup::Reject::NotJson : backup::Reject::TooLarge;
    return false;
  }
  out.resize(size);
  const int read = file.read(out.data(), size);
  if (read != static_cast<int>(size)) {
    out.clear();
    reason = backup::Reject::NotJson;
    return false;
  }
  return true;
}

// Builds the payload document from the live singletons. `progressLimit` caps
// the number of per-book blobs so one card with tens of thousands of cached
// books cannot produce a file larger than the parser can take.
void buildPayload(JsonDocument& payload, int progressLimit) {
  JsonObject p = payload.to<JsonObject>();

  {
    JsonDocument settings;
    SETTINGS.toJson(settings);
    p["settings"].set(settings);
  }
  {
    JsonDocument recent;
    RECENT_BOOKS.toJson(recent);
    JsonObject reading = p["reading"].to<JsonObject>();
    reading["recent"].set(recent);
    // r163 B6: the vocabulary history rides in the reading section (bounded on the way
    // back in by backup::sanitizeVocabHistory, see BackupProfile.h).
    {
      JsonDocument vocab;
      VOCAB_HISTORY_STORE.toJson(vocab);
      reading["vocab"].set(vocab);
    }
    // r163 3.3: the reading statistics ride here too (bounded on the way back in by
    // backup::sanitizeReadingStats). An older firmware ignores the member.
    {
      JsonDocument stats;
      READING_STATS.toJson(stats);
      reading["readingStats"].set(stats);
    }
    JsonArray progress = reading["progress"].to<JsonArray>();
    int added = 0;
    HalFile root = Storage.open("/.crosspoint");
    if (root && root.isDirectory()) {
      char name[128];
      for (HalFile f = root.openNextFile(); f && added < progressLimit; f = root.openNextFile()) {
        if (!f.isDirectory()) {
          f.close();
          continue;
        }
        f.getName(name, sizeof(name));
        f.close();
        const std::string dirName(name);
        if (!backup::isBookCacheDirName(dirName)) continue;
        const std::string rel = "/.crosspoint/" + dirName + "/progress.bin";
        HalFile pf;
        if (!Storage.openFileForRead(TAG, rel, pf)) continue;
        const size_t size = pf.fileSize();
        if (size == 0 || size > kMaxProgressBlob) {
          pf.close();
          continue;
        }
        std::vector<uint8_t> bytes(size);
        const int got = pf.read(bytes.data(), size);
        pf.close();
        if (got != static_cast<int>(size)) continue;
        std::string hex;
        if (!backup::hexEncodeBytes(bytes.data(), bytes.size(), hex)) continue;
        JsonObject entry = progress.add<JsonObject>();
        entry["dir"] = dirName;
        entry["bytes"] = hex;
        added++;
      }
    }
    if (root) root.close();
    LOG_INF(TAG, "export: %d reading positions", added);
  }
  {
    JsonObject creds = p["credentials"].to<JsonObject>();
    JsonDocument wifi;
    WIFI_STORE.toJson(wifi);
    creds["wifi"].set(wifi);
    JsonDocument koreader;
    KOREADER_STORE.toJson(koreader);
    creds["koreader"].set(koreader);
  }
}

std::string makeBackupFileName() {
  return std::string("backup-") + backup::kApp + "-" + fileTimestamp() + ".json";
}

bool writeTextAtomic(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".tmp";
  if (!Storage.writeFile(tmp.c_str(), String(text.c_str()))) return false;
  return Storage.replaceFile(tmp.c_str(), path.c_str());
}

bool writeDocAtomic(const std::string& path, const JsonDocument& doc) {
  const std::string tmp = path + ".tmp";
  if (!PersistableStoreBase::writeDocToFile(tmp.c_str(), doc)) return false;
  return Storage.replaceFile(tmp.c_str(), path.c_str());
}

void noteSanitize(const backup::SanitizeResult& s) {
  for (const auto& n : s.notes) LOG_INF(TAG, "sanitize clamp: %s", n.c_str());
  if (s.droppedKeys > 0) LOG_INF(TAG, "sanitize dropped %d unknown key(s)", s.droppedKeys);
}

// Snapshots the live profile before a restore. Two artefacts on purpose:
//  - settings-pre-restore.json, the operator-named undo file
//  - a full envelope under backups/pre-restore-last.json, so the UI's
//    "Restore pre-restore snapshot" row goes through the SAME validated chain
//    as any other import.
bool snapshotCurrentProfile(std::string& error) {
  JsonDocument payload;
  buildPayload(payload, static_cast<int>(backup::kMaxProgressEntries));
  const std::string text = backup::buildEnvelope(payload.as<JsonVariantConst>(), backup::kApp, isoTimestamp().c_str());
  if (text.empty()) {
    error = "could not serialize the current profile";
    return false;
  }
  // EXISTS-GUARD FIRST. HalStorage::mkdir's return-on-existing is unconfirmed and this
  // codebase's convention is to check exists() before mkdir (OpdsBookBrowserActivity.cpp:489
  // says so explicitly). On the glass the second export failed with "could not create the
  // backups directory" precisely because the directory already existed from the first one.
  // Belt and braces: even if mkdir reports false, accept it when the directory is now there.
  if (!Storage.exists(kBackupsDir)) {
    Storage.mkdir(kBackupsDir, true);
    if (!Storage.exists(kBackupsDir)) {
      error = "could not create the backups directory";
      return false;
    }
  }
  if (!writeTextAtomic(kPreRestoreFile, text)) {
    error = "could not write the pre-restore snapshot";
    return false;
  }
  // Plain copy of the settings file for the operator's own inspection. Best
  // effort: the validated envelope above is the restore path, this is a
  // readable artefact and a failed copy must not block a restore.
  JsonDocument settings;
  SETTINGS.toJson(settings);
  if (!PersistableStoreBase::writeDocToFile(kPreRestoreSettings, settings)) {
    LOG_ERR(TAG, "pre-restore settings copy failed (envelope snapshot is intact)");
  }
  return true;
}

void keepLastFive() {
  std::vector<std::string> names;
  for (const auto& n : Storage.listFiles(kBackupsDir, 200)) {
    const std::string name = baseNameOnly(n.c_str());
    if (backup::isBackupFileName(name)) names.push_back(name);
  }
  const auto doomed = backup::rotationPlan(names, backup::kKeepBackups);
  for (const auto& n : doomed) {
    const std::string path = std::string(kBackupsDir) + "/" + n;
    LOG_INF(TAG, "rotation: removing %s", n.c_str());
    if (!Storage.remove(path.c_str())) LOG_ERR(TAG, "rotation: could not remove %s", n.c_str());
  }
}

// Removes every temp file this restore created. Called on any pre-commit
// failure so a half-written profile is not mistaken for a real one.
struct TempSet {
  std::vector<std::string> paths;
  ~TempSet() {
    for (const auto& p : paths) Storage.remove(p.c_str());
  }
};

bool validateCredentials(JsonVariantConst credsVariant, std::string& detail) {
  if (credsVariant.isNull()) return true;
  if (!credsVariant.is<JsonObjectConst>()) {
    detail = "credentials section is not an object";
    return false;
  }
  JsonObjectConst creds = credsVariant.as<JsonObjectConst>();
  JsonObjectConst wifi = creds["wifi"].as<JsonObjectConst>();
  if (!wifi.isNull()) {
    JsonArrayConst arr = wifi["credentials"].as<JsonArrayConst>();
    if (!wifi["credentials"].isNull() && arr.isNull()) {
      detail = "wifi credentials are not an array";
      return false;
    }
  }
  JsonObjectConst koreader = creds["koreader"].as<JsonObjectConst>();
  if (!koreader.isNull() && !koreader["username"].isNull() && !koreader["username"].is<const char*>()) {
    detail = "koreader username is not a string";
    return false;
  }
  return true;
}

}  // namespace

std::string isoTimestamp() {
  const time_t now = time(nullptr);
  struct tm tmBuf {};
  localtime_r(&now, &tmBuf);
  char iso[32] = {0};
  strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S%z", &tmBuf);
  return std::string(iso);
}

std::string fileTimestamp() {
  const time_t now = time(nullptr);
  struct tm tmBuf {};
  localtime_r(&now, &tmBuf);
  char stamp[16] = {0};
  strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M", &tmBuf);
  return std::string(stamp);
}

ExportResult exportNow() {
  ExportResult r;
  if (!Storage.exists(kBackupsDir)) {
    Storage.mkdir(kBackupsDir, true);
    if (!Storage.exists(kBackupsDir)) {
      r.error = "could not create the backups directory";
      return r;
    }
  }
  JsonDocument payload;
  buildPayload(payload, static_cast<int>(backup::kMaxProgressEntries));
  const std::string text = backup::buildEnvelope(payload.as<JsonVariantConst>(), backup::kApp, isoTimestamp().c_str());
  if (text.empty()) {
    r.error = "could not serialize the profile";
    return r;
  }
  if (text.size() > backup::kMaxBackupBytes) {
    r.error = "profile is larger than the backup size cap";
    return r;
  }
  r.fileName = makeBackupFileName();
  const std::string path = std::string(kBackupsDir) + "/" + r.fileName;
  if (!writeTextAtomic(path, text)) {
    r.error = "could not write the backup file";
    return r;
  }
  r.ok = true;
  LOG_INF(TAG, "exported %s (%u bytes)", r.fileName.c_str(), static_cast<unsigned>(text.size()));
  keepLastFive();
  return r;
}

std::vector<std::string> listBackups() {
  std::vector<std::string> names;
  for (const auto& n : Storage.listFiles(kBackupsDir, 200)) {
    const std::string name = baseNameOnly(n.c_str());
    if (backup::isBackupFileName(name)) names.push_back(name);
  }
  std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
    return backup::backupStampOf(a) > backup::backupStampOf(b);  // newest first
  });
  return names;
}

bool hasPreRestoreSnapshot() { return Storage.exists(kPreRestoreFile); }

RestoreResult restoreFromFile(const std::string& fileName) {
  RestoreResult r;
  if (!backup::isBackupFileName(fileName)) {
    r.reason = backup::Reject::NotEnvelope;
    r.detail = "not a backup file name";
    return r;
  }
  const std::string path = std::string(kBackupsDir) + "/" + fileName;

  // 1-2. read + parse
  std::string text;
  if (!readBackupText(path, text, r.reason)) {
    r.detail = "could not read the backup";
    logReject(r, fileName);
    r.rejectedAs = fileName + ".rejected";
    Storage.rename(path.c_str(), (path + ".rejected").c_str());
    return r;
  }
  JsonDocument doc;
  const auto parseError = deserializeJson(doc, text);
  if (parseError) {
    r.reason = backup::Reject::NotJson;
    r.detail = parseError.c_str();
    logReject(r, fileName);
    r.rejectedAs = fileName + ".rejected";
    Storage.rename(path.c_str(), (path + ".rejected").c_str());
    return r;
  }

  // 3. envelope integrity
  const auto envelope = backup::parseEnvelope(doc);
  if (envelope != backup::Reject::None) {
    r.reason = envelope;
    r.detail = backup::rejectText(envelope);
    logReject(r, fileName);
    r.rejectedAs = fileName + ".rejected";
    Storage.rename(path.c_str(), (path + ".rejected").c_str());
    return r;
  }

  JsonVariantConst payload = doc["payload"];
  JsonObjectConst settingsIn = payload["settings"].as<JsonObjectConst>();

  // 4. settings validation + clamping against the live font registry.
  std::vector<std::string> families;
  sdFontSystem.refreshIfDirty();
  collectFontFamilies(families);
  JsonDocument sanitized;
  const auto sane = backup::sanitizeSettings(settingsIn, families, sanitized);
  if (sane.reason != backup::Reject::None) {
    r.reason = sane.reason;
    r.detail = sane.detail;
    logReject(r, fileName);
    r.rejectedAs = fileName + ".rejected";
    Storage.rename(path.c_str(), (path + ".rejected").c_str());
    return r;
  }
  noteSanitize(sane);

  // A dictionary folder that is not on this card is cleared, not rejected: the
  // name is harmless, the dictionary itself is not part of a profile backup.
  {
    const char* dict = sanitized["dictionaryName"] | "";
    if (dict[0] != '\0') {
      const std::string dir = std::string("/dictionaries/") + dict;
      if (!Storage.exists(dir.c_str())) {
        LOG_INF(TAG, "dictionary '%s' not installed; clearing the selection", dict);
        sanitized["dictionaryName"] = "";
      }
    }
  }

  std::string credError;
  if (!validateCredentials(payload["credentials"], credError)) {
    r.reason = backup::Reject::InvalidField;
    r.detail = credError;
    logReject(r, fileName);
    r.rejectedAs = fileName + ".rejected";
    Storage.rename(path.c_str(), (path + ".rejected").c_str());
    return r;
  }

  JsonObjectConst creds = payload["credentials"].as<JsonObjectConst>();
  JsonObjectConst recent = payload["reading"]["recent"].as<JsonObjectConst>();
  // r163 B6: the vocabulary history is optional -- a backup from before this build has
  // no `vocab` member, and that restores as "keep what is on the card" rather than an
  // empty list.
  JsonObjectConst vocabIn = payload["reading"]["vocab"].as<JsonObjectConst>();
  // r163 3.3: the reading statistics are optional in the same way -- a backup from
  // before this build has no `readingStats` member and restores as "keep what is here".
  JsonObjectConst statsIn = payload["reading"]["readingStats"].as<JsonObjectConst>();
  if (!payload["reading"].isNull() && !payload["reading"].is<JsonObjectConst>()) {
    r.reason = backup::Reject::InvalidField;
    r.detail = "reading section is not an object";
    logReject(r, fileName);
    r.rejectedAs = fileName + ".rejected";
    Storage.rename(path.c_str(), (path + ".rejected").c_str());
    return r;
  }

  // 5. snapshot the CURRENT profile (undo path) before anything changes.
  std::string snapError;
  if (!snapshotCurrentProfile(snapError)) {
    r.reason = backup::Reject::InvalidField;
    r.detail = snapError;
    LOG_ERR(TAG, "restore aborted before touching live files: %s", snapError.c_str());
    return r;  // not the import's fault: do NOT mark the file rejected
  }

  // 6. write every section to a temp file. Live files remain untouched here.
  TempSet temps;
  const std::string settingsTmp = std::string("/.crosspoint/settings.json.tmp");
  temps.paths.push_back(settingsTmp);
  if (!PersistableStoreBase::writeDocToFile(settingsTmp.c_str(), sanitized)) {
    r.reason = backup::Reject::InvalidField;
    r.detail = "could not stage settings";
    return r;
  }

  const std::string wifiSrc = std::string(kBackupsDir) + "/wifi.json.tmp";
  const std::string koreaderSrc = std::string(kBackupsDir) + "/koreader.json.tmp";
  const std::string recentSrc = std::string(kBackupsDir) + "/recent.json.tmp";
  bool haveWifi = false, haveKoreader = false, haveRecent = false;
  if (JsonObjectConst w = creds["wifi"].as<JsonObjectConst>(); !w.isNull()) {
    JsonDocument stage;
    stage.set(w);
    if (!PersistableStoreBase::writeDocToFile(wifiSrc.c_str(), stage)) {
      r.reason = backup::Reject::InvalidField;
      r.detail = "could not stage wifi credentials";
      return r;
    }
    temps.paths.push_back(wifiSrc);
    haveWifi = true;
  }
  if (JsonObjectConst k = creds["koreader"].as<JsonObjectConst>(); !k.isNull()) {
    JsonDocument stage;
    stage.set(k);
    if (!PersistableStoreBase::writeDocToFile(koreaderSrc.c_str(), stage)) {
      r.reason = backup::Reject::InvalidField;
      r.detail = "could not stage koreader credentials";
      return r;
    }
    temps.paths.push_back(koreaderSrc);
    haveKoreader = true;
  }
  if (!recent.isNull()) {
    JsonDocument stage;
    stage.set(recent);
    if (!PersistableStoreBase::writeDocToFile(recentSrc.c_str(), stage)) {
      r.reason = backup::Reject::InvalidField;
      r.detail = "could not stage recent books";
      return r;
    }
    temps.paths.push_back(recentSrc);
    haveRecent = true;
  }

  // r163 B6: the vocabulary history, bounded by backup::sanitizeVocabHistory so the file
  // we install is one the firmware could have written itself.
  const std::string vocabSrc = std::string(kBackupsDir) + "/vocab.json.tmp";
  bool haveVocab = false;
  size_t vocabDropped = 0;
  if (!vocabIn.isNull()) {
    JsonDocument stage;
    const backup::VocabSanitizeResult v = backup::sanitizeVocabHistory(vocabIn, stage);
    vocabDropped = v.dropped;
    if (!PersistableStoreBase::writeDocToFile(vocabSrc.c_str(), stage)) {
      r.reason = backup::Reject::InvalidField;
      r.detail = "could not stage vocabulary history";
      return r;
    }
    temps.paths.push_back(vocabSrc);
    haveVocab = true;
  }

  // r163 3.3: the reading statistics, bounded by backup::sanitizeReadingStats so the
  // file we install is one the firmware could have written itself.
  const std::string statsSrc = std::string(kBackupsDir) + "/reading_stats.json.tmp";
  bool haveStats = false;
  backup::ReadingStatsSanitizeResult statsResult;
  if (!statsIn.isNull()) {
    JsonDocument stage;
    statsResult = backup::sanitizeReadingStats(statsIn, stage);
    if (!PersistableStoreBase::writeDocToFile(statsSrc.c_str(), stage)) {
      r.reason = backup::Reject::InvalidField;
      r.detail = "could not stage reading statistics";
      return r;
    }
    temps.paths.push_back(statsSrc);
    haveStats = true;
  }

  // Per-book progress: only into cache directories that already exist. A
  // missing directory means the book's cache was cleared; creating a bare
  // cache dir would leave the reader looking at a half-built cache, whereas
  // skipping it simply leaves no saved position for that book.
  JsonArrayConst progress = payload["reading"]["progress"].as<JsonArrayConst>();
  struct ProgressWrite {
    std::string dirPath;
    std::vector<uint8_t> bytes;
  };
  std::vector<ProgressWrite> progressWrites;
  int skipped = 0;
  for (JsonObjectConst entry : progress) {
    const char* dir = entry["dir"] | "";
    const char* hex = entry["bytes"] | "";
    if (!backup::isBookCacheDirName(dir)) {
      skipped++;
      continue;
    }
    std::vector<uint8_t> bytes;
    if (!backup::hexDecodeBytes(hex, bytes) || bytes.empty() || bytes.size() > kMaxProgressBlob) {
      skipped++;
      continue;
    }
    const std::string cacheDir = std::string("/.crosspoint/") + dir;
    if (!Storage.exists(cacheDir.c_str())) {
      skipped++;
      continue;
    }
    progressWrites.push_back({cacheDir, std::move(bytes)});
  }
  if (skipped > 0) LOG_INF(TAG, "restore: %d reading position(s) skipped (no cache dir)", skipped);

  // 7. commit. The order puts settings first and progress last, so a
  // commit-stage failure still leaves the undo snapshot reachable.
  if (!Storage.replaceFile(settingsTmp.c_str(), "/.crosspoint/settings.json")) {
    r.reason = backup::Reject::InvalidField;
    r.detail = "could not install settings";
    return r;  // temps cleaned; live settings still the old file
  }
  temps.paths.erase(temps.paths.begin());
  bool commitOk = true;
  if (haveWifi && !Storage.replaceFile(wifiSrc.c_str(), "/.crosspoint/wifi.json")) commitOk = false;
  if (haveKoreader && !Storage.replaceFile(koreaderSrc.c_str(), "/.crosspoint/koreader.json")) commitOk = false;
  if (haveRecent && !Storage.replaceFile(recentSrc.c_str(), "/.crosspoint/recent.json")) commitOk = false;
  if (haveVocab && !Storage.replaceFile(vocabSrc.c_str(), VocabHistoryStore::getFilePath())) commitOk = false;
  if (haveStats && !Storage.replaceFile(statsSrc.c_str(), ReadingStatsStore::getFilePath())) commitOk = false;
  if (haveStats && (statsResult.dropped > 0 || statsResult.clamped > 0)) {
    LOG_INF(TAG, "restore: reading statistics %u book(s) dropped (cap or damaged), %u value(s) clamped",
            static_cast<unsigned>(statsResult.dropped), static_cast<unsigned>(statsResult.clamped));
  }
  if (haveVocab && vocabDropped > 0) {
    LOG_INF(TAG, "restore: %u vocabulary entr%s dropped (cap or damaged)",
            static_cast<unsigned>(vocabDropped), vocabDropped == 1 ? "y" : "ies");
  }

  int restoredProgress = 0;
  for (const auto& w : progressWrites) {
    if (ProgressFile::writeAtomic(w.dirPath, w.bytes.data(), w.bytes.size())) restoredProgress++;
  }
  LOG_INF(TAG, "restore committed: %d reading position(s), commitOk=%d", restoredProgress, commitOk ? 1 : 0);

  if (!commitOk) {
    r.partial = true;
    r.detail = "some profile files could not be replaced; use Restore pre-restore snapshot";
    return r;
  }
  r.ok = true;
  return r;
}

RestoreResult restorePreRestoreSnapshot() {
  RestoreResult r;
  if (!hasPreRestoreSnapshot()) {
    r.reason = backup::Reject::NotEnvelope;
    r.detail = "no pre-restore snapshot exists";
    return r;
  }
  std::string text;
  if (!readBackupText(kPreRestoreFile, text, r.reason)) {
    r.detail = "could not read the pre-restore snapshot";
    return r;
  }
  JsonDocument doc;
  const auto err = deserializeJson(doc, text);
  if (err) {
    r.reason = backup::Reject::NotJson;
    r.detail = err.c_str();
    return r;
  }
  const auto envelope = backup::parseEnvelope(doc);
  if (envelope != backup::Reject::None) {
    r.reason = envelope;
    r.detail = backup::rejectText(envelope);
    return r;
  }

  // Reuse the import path by staging the snapshot under a valid backup name,
  // then importing it. The staged copy is removed whether the import works or
  // not; the snapshot itself is never renamed.
  const std::string stagedName = std::string("backup-") + backup::kApp + "-" + fileTimestamp() + ".json";
  const std::string stagedPath = std::string(kBackupsDir) + "/" + stagedName;
  if (!writeTextAtomic(stagedPath, text)) {
    r.reason = backup::Reject::InvalidField;
    r.detail = "could not stage the snapshot for restore";
    return r;
  }
  r = restoreFromFile(stagedName);
  Storage.remove(stagedPath.c_str());
  return r;
}

}  // namespace backupsvc
