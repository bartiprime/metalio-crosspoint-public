#pragma once
// ---------------------------------------------------------------------------
// BACKUP1 -- the device side of the full-profile backup/restore.
//
// Everything that needs an SD card, a singleton or ESP-IDF lives here; every
// rule that decides whether a file may be applied lives in lib/Backup
// (BackupProfile), which the host tests exercise. The split is what lets the
// rejection matrix be proven without a flash.
//
// RESTORE ORDER (paranoid, each step before the next):
//   1. read the file with a size cap
//   2. parse JSON; a parse error never reaches step 3
//   3. envelope magic / kind / schema<=1 / sha256 over the payload
//   4. sanitize settings: every numeric value type-checked and clamped, the SD
//      font family must exist in the live registry, the language code must be
//      well formed
//   5. snapshot the CURRENT profile (settings-pre-restore.json + a full
//      pre-restore backup) -- this is the undo path
//   6. write every section to a .tmp file (no live file touched)
//   7. rename temps into place (commit)
//   8. caller restarts to settings so every singleton reloads
//
// A failure in steps 1-6 leaves the live profile untouched, renames the bad
// file to `<name>.rejected` and reports the reason. A failure in step 7 (FAT
// rename) is reported as partially applied; the pre-restore snapshot is the
// recovery path and is never rotated away.
// ---------------------------------------------------------------------------
#include <string>
#include <vector>

#include "BackupProfile.h"

namespace backupsvc {

inline constexpr const char* kBackupsDir = "/.crosspoint/backups";
inline constexpr const char* kPreRestoreFile = "/.crosspoint/backups/pre-restore-last.json";
inline constexpr const char* kPreRestoreSettings = "/.crosspoint/settings-pre-restore.json";

struct ExportResult {
  bool ok = false;
  std::string fileName;  // bare name, e.g. backup-r102-20261109-2005.json
  std::string error;
};

struct RestoreResult {
  bool ok = false;
  bool partial = false;  // commit-stage failure: some live files were replaced
  backup::Reject reason = backup::Reject::None;
  std::string detail;
  std::string rejectedAs;  // `<name>.rejected` when the bad file was kept
};

// Builds and writes one full-profile backup, then applies keep-last-5.
ExportResult exportNow();

// Bare backup file names in /.crosspoint/backups, newest first. Excludes the
// pre-restore snapshot and anything that is not a backup this feature wrote.
std::vector<std::string> listBackups();

// Restores one importable backup by bare file name.
RestoreResult restoreFromFile(const std::string& fileName);

bool hasPreRestoreSnapshot();
RestoreResult restorePreRestoreSnapshot();

// UTC/local ISO8601 stamp for the envelope's "created" field.
std::string isoTimestamp();
// YYYYMMDD-HHMM for the file name.
std::string fileTimestamp();

}  // namespace backupsvc
