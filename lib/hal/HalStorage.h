#pragma once

#include <Print.h>
#include <common/FsApiConstants.h>  // for oflag_t
#include <freertos/semphr.h>

#include <memory>
#include <string>
#include <vector>

#include "StorageWriteGate.h"       // r110: halstorage::isWriteOflag -- which opens can write
#include "platform/UsbMscFeature.h"

class HalFile;
class FsBlockDeviceInterface;

class HalStorage {
 public:
  HalStorage();
  bool begin();
  bool ready() const;
  // r129 (H34): card capacity as the driver reports it, for the boot mount line. Read-only pass
  // through to the manager's cached total; returns 0 when no card is mounted.
  uint64_t sdTotalBytes() const;
  // Stop the SD card for deep sleep: unmount, stop the SDMMC host, and release
  // the bus pads (no-op on SPI boards). Call only after all file users have
  // stopped; open HalFiles become invalid. A deep-sleep wake resets the MCU and
  // mounts storage again through begin().
  void prepareForDeepSleep();
  // NOTE: no beginUsbDrive()/endUsbDrive()/usbDriveState() family here. Those wrappers
  // existed only for the SDK's Arduino-TinyUSB path (FREEINK_CAP_USB_MSC), which is 0
  // on this board by design. Drive mode is detachForUsbHost() below (CP_USB_MSC).
#if CP_USB_MSC
  // USB-MSC (esp_tinyusb) hand-over: takes the storage mutex, ends the SdFat
  // volume and returns the raw 512-byte sector device for the gadget to expose.
  // There is NO remount path: the caller cold-boots to get the filesystem back,
  // so every open HalFile is invalid from this point until the reboot.
  FsBlockDeviceInterface* detachForUsbHost();
#endif
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  // Read the entire file at `path` into a String. Returns empty string on failure.
  String readFile(const char* path);
  // Low-memory helpers:
  // Stream the file contents to a `Print` (e.g. `Serial`, or any `Print`-derived object).
  // Returns true on success, false on failure.
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256);
  // Read up to `bufferSize-1` bytes into `buffer`, null-terminating it. Returns bytes read.
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  // Read the whole file at `path` into `out`. Fails (without allocating) on
  // missing, directory, empty, above-`cap`, or short-read files.
  bool readFileToString(const char* moduleName, const std::string& path, size_t cap, std::string& out);
  // Write a string to `path` on the SD card. Overwrites existing file.
  // Returns true on success.
  bool writeFile(const char* path, const String& content);
  // Ensure a directory exists, creating it if necessary. Returns true on success.
  bool ensureDirectoryExists(const char* path);

  // ---- r109 CARD2/FORMAT1: never initialise a volume we cannot vouch for ------------------>
  // A mount can succeed on a BLANK or FOREIGN volume -- that is exactly what the 10 Nov 2026 card
  // looked like after its directory tree was lost, and the firmware answered it by writing the
  // default settings and an empty library index over whatever was there. checkVolumeMarker() runs
  // after a SUCCESSFUL boot mount, refuses to initialise an unmarked volume and puts the storage
  // layer into a no-write state for the rest of the boot. initVolumeLayout() is the ONLY thing that
  // creates a fresh layout, and it is reachable only through the explicit `sd init` shell verb.
  bool checkVolumeMarker();
  bool initVolumeLayout();
  // ---- r110: `sd adopt` -- the operator's statement that a card with content but no
  // settings.json is ours. Writes ONLY the marker (creating /.crosspoint if needed); it never
  // touches settings.json, the library index or any book. Unlike initVolumeLayout() it is meant
  // for a card that already has user content and was restored from a backup.
  bool adoptVolume();
  bool readOnly() const { return readOnly_; }
  const char* readOnlyReason() const { return readOnlyReason_; }
  void setReadOnly(bool on, const char* why);
  // r110: the single "write refused on this read-only volume" answer (LOG_ERR the first time in a
  // boot, LOG_DBG after). Public only because HalFile's write-capable methods live outside this
  // class; it is not a user-facing API.
  bool refuseWrite(const char* what);

  HalFile open(const char* path, const oflag_t oflag = O_RDONLY);
  bool mkdir(const char* path, const bool pFlag = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  // Move a fully written temp file over `path`, replacing any existing file.
  bool replaceFile(const char* tmpPath, const char* path);
  bool rmdir(const char* path);

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForRead(const char* moduleName, const String& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const String& path, HalFile& file);
  bool removeDir(const char* path);

  // ---- r106 `sd` -----------------------------------------------------------------------
  // The result of the LAST mount (Storage.begin()) and when it ran, so the shell can
  // report a mount failure without re-running one. Diagnostic only: nothing reads this
  // on a hot path, and it changes no behaviour.
  struct LastMount {
    bool ran = false;
    bool ok = false;
    unsigned long ms = 0;
  };
  LastMount lastMount() const { return lastMount_; }

  static HalStorage& getInstance() { return instance; }

  class StorageLock;  // private class, used internally

 private:
  static HalStorage instance;

  bool initialized = false;
  bool readOnly_ = false;              // r109 CARD2: unmarked volume -> no writes this boot
  const char* readOnlyReason_ = "";
  bool readOnlyRefusalLogged_ = false;
  LastMount lastMount_;
  SemaphoreHandle_t storageMutex = nullptr;
};

#define Storage HalStorage::getInstance()

class HalFile : public Print {
  friend class HalStorage;
  class Impl;
  std::unique_ptr<Impl> impl;
  explicit HalFile(std::unique_ptr<Impl> impl);

 public:
  HalFile();
  ~HalFile();
  HalFile(HalFile&&);
  HalFile& operator=(HalFile&&);
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush();
  size_t getName(char* name, size_t len);
  size_t size();
  size_t fileSize();
  uint64_t fileSize64();
  uint32_t modificationTime();
  bool seek(size_t pos);
  bool seek64(uint64_t pos);
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset);
  bool truncate(uint64_t length);
  int available() const;
  size_t position() const;
  int read(void* buf, size_t count);
  int read();  // read a single byte
  size_t write(const uint8_t* buf, size_t count) override;
  size_t write(const void* buf, size_t count);
  size_t write(uint8_t b) override;
  bool rename(const char* newPath);
  bool isDirectory() const;
  void rewindDirectory();
  bool close();
  HalFile openNextFile();
  bool isOpen() const;
  operator bool() const;
};

// Downstream code must use Storage instead of SdMan
#ifdef SdMan
#undef SdMan
#endif
