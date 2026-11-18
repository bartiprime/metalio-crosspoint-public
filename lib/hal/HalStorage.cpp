#include "HalStorage.h"

#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <Logging.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <cassert>

#define SDCard SDCardManager::getInstance()

HalStorage HalStorage::instance;

HalStorage::HalStorage() {
  // Recursive so the same task can re-enter StorageLock without self-deadlock.
  // openFileForRead/Write take the lock and then assign to a HalFile&
  // out-param; if that out-param already held an Impl, its destructor takes
  // the lock again to close the prior FsFile under serialization (see
  // HalFile::Impl::~Impl below). Priority inheritance still applies to
  // recursive mutexes.
  storageMutex = xSemaphoreCreateRecursiveMutex();
  assert(storageMutex != nullptr);
}

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() {
  const bool ok = SDCard.begin();
  lastMount_.ran = true;
  lastMount_.ok = ok;
  lastMount_.ms = static_cast<unsigned long>(esp_timer_get_time() / 1000LL);
  // r109 SD2(a): the mount-failure probe used to exist only on Serial. `sd` was the health check
  // after every rail experiment and could not say WHY a mount failed; this puts the same numbers
  // in the on-device log, unconditionally, on every failed mount.
  if (!ok) {
    const auto diag = SDCard.lastMountDiag();
    LOG_INF("SD",
            "mount diagnostics: stage=%s err=0x%X mbr read=%d sig=%d p1 type=0x%02X lba=%lu read=%d",
            diag.stage, static_cast<unsigned>(diag.err), diag.mbrRead ? 1 : 0, diag.sig ? 1 : 0,
            static_cast<unsigned>(diag.p1Type), static_cast<unsigned long>(diag.lba), diag.bootRead ? 1 : 0);
  }
  return ok;
}

bool HalStorage::ready() const { return SDCard.ready(); }

// r129 (H34): the capacity the mount actually negotiated. Pass-through only -- the cached value the
// manager already holds, so it cannot trigger a card transaction.
uint64_t HalStorage::sdTotalBytes() const { return SDCard.ready() ? SDCard.sdTotalBytes() : 0; }

// For the rest of the methods, we acquire the mutex to ensure thread safety

class HalStorage::StorageLock {
 public:
  StorageLock() { xSemaphoreTakeRecursive(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGiveRecursive(HalStorage::getInstance().storageMutex); }
};

void HalStorage::prepareForDeepSleep() {
  StorageLock lock;
  SDCard.shutdown();
}

// ---- r109 CARD2 / FORMAT1: never initialise a volume we cannot vouch for ---------------------
//
// WHAT WENT WRONG (10 Nov 2026, R103_GLASS_LOG §INCIDENT): a power cycle with no dwell left the
// card's root directory gone but its volume mountable. The firmware then wrote a DEFAULT
// settings.json and an EMPTY library.idx over the clusters, and the user's books and reading
// positions were destroyed by the recovery action, not by the rail cut.
//
// THE RULE: a mount that succeeds is not proof the volume is OURS. It is proof the FAT is
// readable. Vouching comes from a marker this firmware wrote (/.crosspoint/.volume), or, for a
// pre-r109 install, from the settings file the old firmware wrote -- and NOTHING ELSE.
// r110 TIGHTENED THE PREDICATE: r109 also adopted any volume whose root held something other
// than housekeeping entries, so a camera card (DCIM at the root) was adopted and initialised
// instead of refused. Root content is not provenance; a stray directory cannot vouch for the
// clusters under it. A volume with neither the marker nor a legacy settings.json is BLANK,
// FOREIGN or DAMAGED: we do not mkdir, we do not write settings, we do not write the library
// index -- every write fails for the rest of the boot and we say why. Two explicit verbs can
// say otherwise: `sd init` (lay out a fresh card) and `sd adopt` (this content is mine).
namespace {
constexpr const char* kCrosspointDir = "/.crosspoint";
constexpr const char* kVolumeMarker = "/.crosspoint/.volume";
constexpr const char* kSettingsFile = "/.crosspoint/settings.json";
constexpr const char* kMarkerText = "crosspoint-volume v1\n";
}  // namespace

void HalStorage::setReadOnly(const bool on, const char* why) {
  const StorageLock lock;
  readOnly_ = on;
  readOnlyReason_ = (on && why != nullptr) ? why : "";
  if (!on) readOnlyRefusalLogged_ = false;
}

bool HalStorage::checkVolumeMarker() {
  const StorageLock lock;
  if (!SDCard.ready()) return false;
  if (SDCard.exists(kVolumeMarker)) {
    setReadOnly(false, nullptr);
    LOG_INF("SD", "volume marker present (/.crosspoint/.volume): storage is writable");
    return true;
  }
  // A pre-r109 install has no marker. Its own settings.json is the ONLY other proof that this
  // firmware wrote this volume, so adopt it instead of refusing the user's own card. Root content
  // is NOT proof (r110): a foreign card with DCIM used to be adopted here and then initialised.
  if (SDCard.exists(kSettingsFile)) {
    const bool ok = SDCard.writeFile(kVolumeMarker, kMarkerText);
    setReadOnly(false, nullptr);
    LOG_INF("SD", "volume marker created for a legacy install (settings.json present) ok=%d", ok ? 1 : 0);
    return true;
  }
  LOG_ERR("SD",
          "volume has no marker and no legacy settings.json: refusing to initialise (card may be blank, "
          "foreign or damaged; 'sd init' lays out a fresh card, 'sd adopt' vouches for this one)");
  setReadOnly(true, "volume has no marker: refusing to initialise");
  return false;
}

bool HalStorage::initVolumeLayout() {
  const StorageLock lock;
  if (!SDCard.ready()) return false;
  // Cleared FIRST: this is the one path that is allowed to write to a volume the boot refused.
  setReadOnly(false, nullptr);
  if (!SDCard.exists(kCrosspointDir) && !SDCard.mkdir(kCrosspointDir)) {
    LOG_ERR("SD", "sd init: could not create /.crosspoint");
    return false;
  }
  const bool markerOk = SDCard.writeFile(kVolumeMarker, kMarkerText);
  LOG_INF("SD", "sd init: volume layout created (marker ok=%d) -- nothing that was already on the card was removed",
          markerOk ? 1 : 0);
  return markerOk;
}

// r110: `sd adopt` -- the deliberate "this card is mine" for a card that HAS content but no
// settings.json (a restored backup, a card laid out by another tool). It differs from `sd init`
// in what it does NOT do: no layout is created beyond the marker itself, so the boot's normal
// settings/index creation is the only thing that follows -- and it runs with the volume now
// vouched for, instead of being refused.
//
// The read-only bit is cleared for the duration of the write and restored if the write fails:
// this is the one path besides initVolumeLayout() that may write to a volume the boot refused,
// and it exists only because an operator typed the verb.
bool HalStorage::adoptVolume() {
  const StorageLock lock;
  if (!SDCard.ready()) {
    LOG_ERR("SD", "sd adopt: refused -- the volume is not mounted");
    return false;
  }
  if (SDCard.exists(kVolumeMarker)) {
    setReadOnly(false, nullptr);
    LOG_INF("SD", "sd adopt: the volume already carries the marker (/.crosspoint/.volume); nothing to write");
    return true;
  }
  setReadOnly(false, nullptr);
  if (!SDCard.exists(kCrosspointDir) && !SDCard.mkdir(kCrosspointDir)) {
    LOG_ERR("SD", "sd adopt: could not create /.crosspoint -- marker NOT written, storage stays read-only");
    setReadOnly(true, "sd adopt: /.crosspoint could not be created");
    return false;
  }
  const bool ok = SDCard.writeFile(kVolumeMarker, kMarkerText);
  if (!ok) {
    LOG_ERR("SD", "sd adopt: marker write FAILED -- storage stays read-only for this boot");
    setReadOnly(true, "sd adopt: marker write failed");
    return false;
  }
  LOG_INF("SD", "sd adopt: volume marker written -- this card is now vouched for; writes are allowed");
  return true;
}

#if CP_USB_MSC
FsBlockDeviceInterface* HalStorage::detachForUsbHost() {
  // Same mutex the FS paths take, so the volume cannot be ended under a live
  // read. Nothing acquires it afterwards: the Activity that calls this runs with
  // requiresExclusiveStorageLoop() and every storage user is stopped before it
  // enters.
  StorageLock lock;
  // r99.5 DRIVE5: HAND OVER A MOUNTED, POWERED CARD OR NOTHING.
  // detachFilesystemForRawAccess() refuses an unmounted manager, but until now that refusal was
  // all that stood between the host and a volume parked earlier in the session (a standby trim, a
  // failed remount): the caller saw "could not detach" at best, and a host that did enumerate got
  // a raw device whose FAT it could not read -- the r99.4 run-3 shape (a drive with no label that
  // the PC cannot open). Remount first, so the sectors the MSC layer exports are the ones a fresh
  // mount has just validated; begin() also drives the board's SD power gate.
  if (!SDCard.ready()) {
    LOG_INF("USB", "USB-MSC hand-over: SD volume not mounted -- remounting before the detach");
    if (!SDCard.begin()) {
      LOG_ERR("USB", "USB-MSC hand-over: remount before hand-over failed; refusing to export");
      return nullptr;
    }
  }
  auto* const blockDevice = SDCard.detachFilesystemForRawAccess();
  if (!blockDevice) {
    LOG_ERR("USB", "USB-MSC hand-over requires a mounted SD filesystem");
    return nullptr;
  }
  return blockDevice;
}
#endif

// USB5/B2: beginUsbDrive()/disconnectUsbDriveHost()/endUsbDrive()/usbDriveState()/
// usbDriveHostSuspended() used to live here. They existed only for the SDK's
// Arduino-TinyUSB mass-storage path (FREEINK_CAP_USB_MSC), which this board cannot
// build: that macro is 0 by design, because the SDK implementation needs
// CONFIG_TINYUSB_ENABLED and would take the console's USB peripheral. Their only
// caller was UsbDriveActivity, now deleted. Drive mode on this board is the
// esp_tinyusb/ANA_SW path below (CP_USB_MSC). Do not reintroduce these wrappers
// while the macro is 0 -- none of them can succeed, and the ones that returned a
// bare `false` read as "host suspended"/"unsupported" at the call site.

#define HAL_STORAGE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;               \
  return SDCard.method(__VA_ARGS__);

// r109 CARD2/FORMAT1: a boot that mounted an UNMARKED (blank or foreign) volume is read-only.
// Every write path answers the same way, loudly once and then quietly, instead of silently
// writing defaults over a card whose directory tree may be gone. Reads are untouched.
// r110: the "loudly once" lives in refuseWrite() so HalFile::truncate/rename can use the same
// line as the named helpers, instead of each growing its own copy of the bit.
#define HAL_STORAGE_WRITE_REFUSED() \
  if (readOnly_) return refuseWrite(__func__);

// r110: the one refusal line. First caller per boot says why in the glass log; the rest are
// DEBUG, because a read-only boot retries these paths every few seconds (logSdFlush) and the
// error ring would otherwise be nothing but this sentence.
bool HalStorage::refuseWrite(const char* what) {
  if (!readOnlyRefusalLogged_) {
    LOG_ERR("SD", "write refused (%s): the volume has no marker, storage is read-only this boot", what);
    readOnlyRefusalLogged_ = true;
  } else {
    LOG_DBG("SD", "write refused (%s): still read-only this boot", what);
  }
  return false;
}

#define HAL_STORAGE_WRITE_WRAPPED_CALL(method, ...) \
  HAL_STORAGE_WRITE_REFUSED();                      \
  HalStorage::StorageLock lock;                     \
  return SDCard.method(__VA_ARGS__);

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  HAL_STORAGE_WRAPPED_CALL(listFiles, path, maxFiles);
}

String HalStorage::readFile(const char* path) { HAL_STORAGE_WRAPPED_CALL(readFile, path); }

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_WRAPPED_CALL(readFileToStream, path, out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  HAL_STORAGE_WRAPPED_CALL(readFileToBuffer, path, buffer, bufferSize, maxBytes);
}

// Composed of already-locked HalStorage/HalFile operations; needs no
// StorageLock of its own.
bool HalStorage::readFileToString(const char* moduleName, const std::string& path, size_t cap, std::string& out) {
  out.clear();
  HalFile file;
  if (!openFileForRead(moduleName, path, file)) return false;
  if (file.isDirectory()) return false;
  const size_t size = file.fileSize();
  if (size == 0 || size > cap) return false;
  // string growth is a bare allocation under -fno-exceptions; probe first so
  // a large file on a fragmented heap fails soft instead of abort()ing.
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < size + 512) {
    LOG_ERR(moduleName, "readFileToString OOM: %u bytes for %s", static_cast<unsigned>(size), path.c_str());
    return false;
  }
  out.resize(size);
  return file.read(out.data(), size) == static_cast<int>(size);
}

bool HalStorage::writeFile(const char* path, const String& content) {
  HAL_STORAGE_WRITE_WRAPPED_CALL(writeFile, path, content);
}

bool HalStorage::ensureDirectoryExists(const char* path) { HAL_STORAGE_WRITE_WRAPPED_CALL(ensureDirectoryExists, path); }

class HalFile::Impl {
 public:
  Impl(FsFile&& fsFile) : file(std::move(fsFile)) {}
  // SdFat is not thread-safe; FsFile::close() touches SD/SPI and must run
  // under StorageLock or it races SdSpiCard::m_spiActive across tasks and
  // trips FreeRTOS's xTaskPriorityDisinherit assert. The FsFile member
  // destructor (DESTRUCTOR_CLOSES_FILE=1) will close() again after the lock
  // releases, but close() on an already-closed FsFile is a no-op. See SdFat
  // issue #518 and the HAL note in CLAUDE.md.
  ~Impl() {
    HalStorage::StorageLock lock;
    file.close();
  }
  FsFile file;
};

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  // ---- r110 WRITEGATE: an open can write ------------------------------------------------------------------
  // SdFat writes a DIRECTORY ENTRY at open when O_CREAT is set (FatFileLFN.cpp create()), and
  // O_TRUNC zeroes the file's cluster chain at open. So `open` is a write path, and a read-only
  // boot must refuse every oflag that can create, truncate or write -- not only the ones that
  // name a write helper. The reachable callers on a markerless boot were the boot UISCALE block
  // (main.cpp), sleepJournal on every park, and logSdFlush every <=5 s; the destructive ones are
  // LibraryPartial.cpp's O_RDWR|O_CREAT|O_AT_END and its truncate(0), HalSystem.cpp's
  // /crash_report.txt O_TRUNC, ClippingStore, DictHtmlPages and Dictionary.
  if (readOnly_ && halstorage::isWriteOflag(oflag)) {
    if (!readOnlyRefusalLogged_) {
      LOG_ERR("SD", "open refused (%s, flags 0x%X): write-capable open on a read-only (unmarked) volume", path,
              static_cast<unsigned>(oflag));
      readOnlyRefusalLogged_ = true;
    }
    return HalFile();  // invalid handle: callers see !f and skip the write, exactly like a failure
  }
  return HalFile(std::make_unique<HalFile::Impl>(SDCard.open(path, oflag)));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) { HAL_STORAGE_WRITE_WRAPPED_CALL(mkdir, path, pFlag); }

bool HalStorage::exists(const char* path) { HAL_STORAGE_WRAPPED_CALL(exists, path); }

bool HalStorage::remove(const char* path) { HAL_STORAGE_WRITE_WRAPPED_CALL(remove, path); }
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  HAL_STORAGE_WRITE_WRAPPED_CALL(rename, oldPath, newPath);
}

bool HalStorage::replaceFile(const char* tmpPath, const char* path) {
  HAL_STORAGE_WRITE_WRAPPED_CALL(replaceFile, tmpPath, path);
}

bool HalStorage::rmdir(const char* path) { HAL_STORAGE_WRITE_WRAPPED_CALL(rmdir, path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForRead(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  HAL_STORAGE_WRITE_REFUSED();
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { HAL_STORAGE_WRITE_WRAPPED_CALL(removeDir, path); }

// HalFile implementation
// Allow doing file operations while ensuring thread safety via HalStorage's mutex.
// Please keep the list below in sync with the HalFile.h header

#define HAL_FILE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;            \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

#define HAL_FILE_FORWARD_CALL(method, ...) \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() { HAL_FILE_WRAPPED_CALL(flush, ); }
size_t HalFile::getName(char* name, size_t len) { HAL_FILE_WRAPPED_CALL(getName, name, len); }
size_t HalFile::size() { HAL_FILE_FORWARD_CALL(size, ); }              // already thread-safe, no need to wrap
size_t HalFile::fileSize() { HAL_FILE_FORWARD_CALL(fileSize, ); }      // already thread-safe, no need to wrap
uint64_t HalFile::fileSize64() { HAL_FILE_FORWARD_CALL(fileSize, ); }  // already thread-safe, no need to wrap
uint32_t HalFile::modificationTime() {
  HalStorage::StorageLock lock;
  uint16_t date = 0;
  uint16_t time = 0;
  if (!impl || !impl->file.getModifyDateTime(&date, &time) || date == 0) return 0;
  return (static_cast<uint32_t>(date) << 16) | time;
}
bool HalFile::seek(size_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seek64(uint64_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seekCur(int64_t offset) { HAL_FILE_WRAPPED_CALL(seekCur, offset); }
bool HalFile::seekSet(size_t offset) { HAL_FILE_WRAPPED_CALL(seekSet, offset); }
// r110 WRITEGATE: truncate zeroes a file's cluster chain and rename moves a directory entry --
// both are writes that arrived through a read-opened handle. WebDAVHandler.cpp renames a temp
// over a target, and LibraryPartial truncates a rewritten partial file; on a read-only boot both
// must fail as quietly as the named write helpers do after the first refusal.
bool HalFile::truncate(const uint64_t length) {
  if (HalStorage::getInstance().readOnly()) return HalStorage::getInstance().refuseWrite(__func__);
  HAL_FILE_WRAPPED_CALL(truncate, length);
}
int HalFile::available() const { HAL_FILE_WRAPPED_CALL(available, ); }
size_t HalFile::position() const { HAL_FILE_WRAPPED_CALL(position, ); }
int HalFile::read(void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(read, buf, count); }
int HalFile::read() { HAL_FILE_WRAPPED_CALL(read, ); }
// r109 CARD2: a file opened for write before the volume was ruled unvouched-for must not become a
// second way in. Same answer as openFileForWrite: refuse, and stay quiet after the first refusal.
size_t HalFile::write(const uint8_t* buf, size_t count) {
  if (HalStorage::getInstance().readOnly()) return 0;
  HAL_FILE_WRAPPED_CALL(write, buf, count);
}
size_t HalFile::write(const void* buf, size_t count) {
  if (HalStorage::getInstance().readOnly()) return 0;
  HAL_FILE_WRAPPED_CALL(write, buf, count);
}
size_t HalFile::write(uint8_t b) {
  if (HalStorage::getInstance().readOnly()) return 0;
  HAL_FILE_WRAPPED_CALL(write, b);
}
bool HalFile::rename(const char* newPath) {
  // r110 WRITEGATE: a rename is a directory-entry write (WebDAVHandler renames a temp over its
  // target); it must not be the way around the read-only boot.
  if (HalStorage::getInstance().readOnly()) return HalStorage::getInstance().refuseWrite(__func__);
  HAL_FILE_WRAPPED_CALL(rename, newPath);
}
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }  // already thread-safe, no need to wrap
void HalFile::rewindDirectory() { HAL_FILE_WRAPPED_CALL(rewindDirectory, ); }
bool HalFile::close() { HAL_FILE_WRAPPED_CALL(close, ); }
HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return HalFile(std::make_unique<Impl>(impl->file.openNextFile()));
}
bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }
