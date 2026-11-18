#include "UsbMscStorage.h"

#include <Logging.h>
#include <sdkconfig.h>
#include <strings.h>  // strcasecmp: label comparison is case-insensitive

#include <cstdio>
#include <cstring>

#include <esp_attr.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "UsbMscJournal.h"
#include "UsbPortRoute.h"
#include "esp_log.h"

// The MSC storage object only exists when esp_tinyusb's MSC class was compiled
// in. Both conditions are checked so the file builds and links even in a tree
// where the managed component has not been resolved, degrading to
// ESP_ERR_NOT_SUPPORTED instead of failing the build.
#if defined(CONFIG_TINYUSB_MSC_ENABLED)
#if __has_include("tinyusb_msc.h")
#define CP_USB_MSC_STORAGE_COMPILED 1
#endif
#endif
#ifndef CP_USB_MSC_STORAGE_COMPILED
#define CP_USB_MSC_STORAGE_COMPILED 0
#endif

#if CP_USB_MSC_STORAGE_COMPILED
#include <SdFat.h>  // FsBlockDeviceInterface -- the reader's own 512 B sector path

#include "esp_blockdev.h"
#include "ff.h"  // FATFS: the label a host displays is the FAT volume label
#include "tinyusb_msc.h"
#endif

namespace {

constexpr const char* TAG = "USBMSC";

// Vendor's own mount path for the app-side view of the same volume
// (vendor sdkconfig.defaults.esp32s31:29; usb_virtual_disk.cc:172). The app's
// own SdFat tree never uses it -- it only names the FAT mount esp_tinyusb
// creates while the volume is owned by APP.
char s_basePath[] = "/sdcard";

FsBlockDeviceInterface* s_dev = nullptr;

// ---- DIAG1: drive-mode session instrument (deletable) --------------------
// WHY RTC MEMORY: a drive-mode session is a blackout. The card belongs to the
// host so nothing can be appended to debug.log, the console is taken away by
// the port hand-over, the PSRAM capture ring dies with the cold boot that ends
// the session, and logPrintf's 16-entry RTC ring can only be read back before
// HalSystem::begin() clears it. RTC_NOINIT is the one channel that spans the
// reboot, so every observed fact is written there and turned back into
// debug.log lines by dumpSessionToLog() on the next boot.
//
// This block observes only. The bridge still calls dev->readSectors/writeSectors
// with the same arguments at the same moment and returns the same value; what is
// added around them is one timestamp pair and two counters.
constexpr uint32_t kDiagMagic = 0x44494147;  // "DIAG": a session's counters are live
constexpr uint32_t kSlowUs = 250000;         // 250 ms: the one slow-op line
constexpr uint32_t kStallMs = 3000;          // 3 s inside the driver: suspected hang
constexpr size_t kSnapLines = 16;            // same depth as the shared RTC log ring
constexpr size_t kSnapLen = 128;

struct DiagSession {
  uint32_t magic;       // kDiagMagic from a successful prepare() until the dump
  uint32_t ops_ok;      // sector ops the driver returned success for
  uint32_t ops_err;     // sector ops the driver returned failure for
  uint32_t ops_slow;    // ops over kSlowUs (counted always, logged once)
  uint32_t max_us;      // worst op of the session
  uint32_t last_lba;    // where the last op touched the card
  uint32_t last_count;
  uint32_t stalls;      // in-flight age crossed kStallMs
  uint32_t slowLogged;  // 0/1 latch: exactly one slow line per session
};
RTC_NOINIT_ATTR DiagSession s_diag;

// IN-FLIGHT TIMESTAMP. Non-zero means "a sector call is inside the driver right
// now", and it is the ONLY way a call that never returns can be seen at all:
// bdlRead/bdlWrite log on a returned failure, so silence is indistinguishable
// from a hang without this. lba/count are stored with it, before the timestamp
// is published, so a reader that sees a live timestamp also sees what it is.
RTC_NOINIT_ATTR volatile uint32_t s_inFlightUs;
RTC_NOINIT_ATTR volatile uint32_t s_inFlightLba;
RTC_NOINIT_ATTR volatile uint32_t s_inFlightCount;
RTC_NOINIT_ATTR uint32_t s_stallLogged;  // one stall line per stalled op

// Snapshot of the shared RTC log ring, taken before HalSystem::begin() wipes it.
RTC_NOINIT_ATTR char s_ringSnap[kSnapLines][kSnapLen];
RTC_NOINIT_ATTR uint32_t s_ringCount;

uint32_t nowUs() { return static_cast<uint32_t>(esp_timer_get_time()); }

const char* taskStateName(const eTaskState state) {
  switch (state) {
    case eRunning: return "Running";
    case eReady: return "Ready";
    case eBlocked: return "Blocked";
    case eSuspended: return "Suspended";
    case eDeleted: return "Deleted";
    default: return "Invalid";
  }
}

// Returns the entry timestamp. Call diagOpEnd() with it when the call returns.
inline uint32_t diagOpBegin(const uint32_t lba, const uint32_t count) {
  const uint32_t start = nowUs();
  s_inFlightLba = lba;
  s_inFlightCount = count;
  s_stallLogged = 0;
  s_inFlightUs = start != 0 ? start : 1u;  // 0 is the idle marker
  return start;
}

inline void diagOpEnd(const uint32_t start, const bool ok, const uint32_t lba, const uint32_t count,
                      const bool isRead) {
  const uint32_t us = nowUs() - start;  // wrap-safe within one session
  s_inFlightUs = 0;                     // cleared first: the op is over
  if (ok) {
    ++s_diag.ops_ok;
  } else {
    ++s_diag.ops_err;
  }
  if (us > s_diag.max_us) s_diag.max_us = us;
  s_diag.last_lba = lba;
  s_diag.last_count = count;
  if (us < kSlowUs) return;
  ++s_diag.ops_slow;
  if (s_diag.slowLogged != 0) return;
  s_diag.slowLogged = 1;
  // LOG_ERR, not ESP_LOGW: "ERR" is the only logPrintf level the runtime debug
  // switch never gates (Logging.cpp:201), and the RTC ring is the only output
  // that outlives the session, so this is the one spelling that survives.
  if (isRead) {
    LOG_ERR(TAG, "slow read lba=%u n=%u us=%u", (unsigned)lba, (unsigned)count, (unsigned)us);
  } else {
    LOG_ERR(TAG, "slow write lba=%u n=%u us=%u", (unsigned)lba, (unsigned)count, (unsigned)us);
  }
}

#if CP_USB_MSC_STORAGE_COMPILED
tinyusb_msc_storage_handle_t s_hdl = nullptr;
esp_blockdev_t s_blockdev = {};
esp_blockdev_handle_t s_bdl = nullptr;

// ---- esp_blockdev bridge -------------------------------------------------
// Byte-addressable facade over FsBlockDeviceInterface. esp_tinyusb addresses
// the medium in 512-byte LBAs and asks for read_size/write_size alignment;
// SdFat's interface takes sector indices, so the only work here is the
// /512 conversion plus a range check. No caching: the reader's FsVolume is
// already ended by the time this is live, so there is exactly one owner of the
// sectors and no stale cache can exist.

struct BdlSelf {
  FsBlockDeviceInterface* dev;
};

BdlSelf s_bdl_self = {nullptr};

esp_err_t bdlRead(esp_blockdev_handle_t handle, uint8_t* dst, size_t dst_size, uint64_t src_addr,
                  size_t read_len) {
  auto* self = static_cast<BdlSelf*>(handle->ctx);
  if (self == nullptr || self->dev == nullptr || dst == nullptr) return ESP_ERR_INVALID_ARG;
  if ((src_addr % usbmsc::kSectorBytes) != 0 || (read_len % usbmsc::kSectorBytes) != 0) {
    return ESP_ERR_INVALID_ARG;  // unaligned: would need a read-modify-write
  }
  if (dst_size < read_len) return ESP_ERR_INVALID_SIZE;
  const uint32_t lba = static_cast<uint32_t>(src_addr / usbmsc::kSectorBytes);
  const uint32_t count = static_cast<uint32_t>(read_len / usbmsc::kSectorBytes);
  const uint32_t t0 = diagOpBegin(lba, count);
  const bool ok = self->dev->readSectors(lba, dst, count);
  diagOpEnd(t0, ok, lba, count, true);
  if (!ok) {
    ESP_LOGE(TAG, "read failed lba=%u n=%u", (unsigned)lba, (unsigned)count);
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t bdlWrite(esp_blockdev_handle_t handle, const uint8_t* src, uint64_t dst_addr,
                   size_t write_len) {
  auto* self = static_cast<BdlSelf*>(handle->ctx);
  if (self == nullptr || self->dev == nullptr || src == nullptr) return ESP_ERR_INVALID_ARG;
  if ((dst_addr % usbmsc::kSectorBytes) != 0 || (write_len % usbmsc::kSectorBytes) != 0) {
    return ESP_ERR_INVALID_ARG;
  }
  const uint32_t lba = static_cast<uint32_t>(dst_addr / usbmsc::kSectorBytes);
  const uint32_t count = static_cast<uint32_t>(write_len / usbmsc::kSectorBytes);
  const uint32_t t0 = diagOpBegin(lba, count);
  const bool ok = self->dev->writeSectors(lba, src, count);
  diagOpEnd(t0, ok, lba, count, false);
  if (!ok) {
    ESP_LOGE(TAG, "write failed lba=%u n=%u", (unsigned)lba, (unsigned)count);
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t bdlSync(esp_blockdev_handle_t handle) {
  auto* self = static_cast<BdlSelf*>(handle->ctx);
  if (self == nullptr || self->dev == nullptr) return ESP_ERR_INVALID_ARG;
  if (!self->dev->syncDevice()) return ESP_FAIL;
  return ESP_OK;
}

esp_err_t bdlIoctl(esp_blockdev_handle_t /*handle*/, const uint8_t cmd, void* args) {
  switch (cmd) {
    case ESP_BLOCKDEV_CMD_MARK_DELETED:
    case ESP_BLOCKDEV_CMD_ERASE_CONTENTS:
      // SD cards need no erase before write; reporting success is honest here
      // because the write path never calls this (device_flags.erase_before_write
      // is 0, and storage_blockdev only erases when that flag says so).
      return ESP_OK;
    default:
      return ESP_ERR_NOT_SUPPORTED;
  }
  (void)args;
}

esp_err_t bdlRelease(esp_blockdev_handle_t /*handle*/) {
  // Borrowed device: the block device belongs to SDCardManager/SdFat and is
  // reclaimed by the reboot, never freed here.
  s_bdl = nullptr;
  s_bdl_self.dev = nullptr;
  return ESP_OK;
}

const esp_blockdev_ops_t s_bdl_ops = {
    .read = bdlRead,
    .write = bdlWrite,
    .erase = nullptr,
    .sync = bdlSync,
    .ioctl = bdlIoctl,
    .release = bdlRelease,
};

// ---- FAT volume label ----------------------------------------------------
// The half of the drive name a PC actually shows. It is written through the
// FATFS mount esp_tinyusb created for the APP-owned volume, i.e. AFTER the
// sector bridge is live and BEFORE the mount point flips to USB, so the host
// enumerates a card that already carries the label. Writing it via FATFS (not
// by poking the boot sector) means FAT12/16/32 and the LFN casing are the
// library's problem, and a volume whose filesystem is not FAT comes back as an
// error instead of a mangled sector.
//
// The drive number is allocated at runtime by ff_diskio_get_drive(), so it is
// probed instead of assumed. That is safe here: this is the only FATFS volume
// in the firmware -- the reader's own mount is SdFat, which registers no drive
// -- so the first drive that answers is ours.
int fatDriveNumber() {
  for (int drv = 0; drv < FF_VOLUMES; ++drv) {
    char path[3] = {static_cast<char>('0' + drv), ':', 0};
    TCHAR label[16] = {};
    if (f_getlabel(path, label, nullptr) == FR_OK) return drv;
  }
  return -1;
}

void ensureVolumeLabel() {
  const int drv = fatDriveNumber();
  if (drv < 0) {
    ESP_LOGW(TAG, "volume label: no FATFS volume answered; leaving the card's own label");
    return;
  }

  char path[3] = {static_cast<char>('0' + drv), ':', 0};
  TCHAR current[16] = {};
  const FRESULT got = f_getlabel(path, current, nullptr);
  if (got == FR_OK && strcasecmp(current, usbmsc::kVolumeLabel) == 0) {
    // Idempotent on purpose: an identical rewrite is pure NAND wear on a card
    // that is about to be handed to a host, plus a window to lose.
    ESP_LOGI(TAG, "volume label already \"%s\"", current);
    return;
  }

  char spec[24];  // "<drive>:<label>"
  snprintf(spec, sizeof(spec), "%d:%s", drv, usbmsc::kVolumeLabel);
  const FRESULT set = f_setlabel(spec);
  if (set == FR_OK) {
    ESP_LOGW(TAG, "volume label -> \"%s\" (was \"%s\")", usbmsc::kVolumeLabel,
             got == FR_OK ? current : "?");
  } else {
    // Not fatal: the device name still says it. Report so a card that cannot be
    // relabelled (non-FAT filesystem) is visible in debug.log instead of silent.
    ESP_LOGW(TAG, "volume label unchanged (f_setlabel=%d): volume keeps its own label", set);
  }
}
#endif  // CP_USB_MSC_STORAGE_COMPILED

}  // namespace

namespace usbmsc {

bool compiledIn() { return CP_USB_MSC_STORAGE_COMPILED != 0; }

void setArmed(bool value) { usbport::setArmed(value); }
bool armed() { return usbport::armed(); }

uint32_t sectorCount() {
  if (s_dev != nullptr) return static_cast<uint32_t>(s_dev->sectorCount());
  return 0;
}

esp_err_t readSectors(uint32_t lba, void* dst, uint32_t count) {
  if (!armed()) return ESP_ERR_INVALID_STATE;
  if (s_dev == nullptr || dst == nullptr || count == 0) return ESP_ERR_INVALID_ARG;
  const uint32_t total = sectorCount();
  if (total != 0 && (lba >= total || count > total - lba)) return ESP_ERR_INVALID_SIZE;
  return s_dev->readSectors(lba, static_cast<uint8_t*>(dst), count) ? ESP_OK : ESP_FAIL;
}

esp_err_t writeSectors(uint32_t lba, const void* src, uint32_t count) {
  if (!armed()) return ESP_ERR_INVALID_STATE;
  if (s_dev == nullptr || src == nullptr || count == 0) return ESP_ERR_INVALID_ARG;
  const uint32_t total = sectorCount();
  if (total != 0 && (lba >= total || count > total - lba)) return ESP_ERR_INVALID_SIZE;
  return s_dev->writeSectors(lba, static_cast<const uint8_t*>(src), count) ? ESP_OK : ESP_FAIL;
}

esp_err_t prepare(FsBlockDeviceInterface* blockDevice) {
  if (!armed()) {
    ESP_LOGW(TAG, "prepare refused: USB-MSC not armed");
    return ESP_ERR_INVALID_STATE;
  }
#if !CP_USB_MSC_STORAGE_COMPILED
  ESP_LOGE(TAG, "prepare: esp_tinyusb MSC storage not compiled in");
  return ESP_ERR_NOT_SUPPORTED;
#else
  if (blockDevice == nullptr) return ESP_ERR_INVALID_ARG;
  if (s_hdl != nullptr) return ESP_OK;  // idempotent

  // DIAG1: mark a NEW session's counters zero here, before a single sector can
  // move. A stale total from an earlier session must never read as this one's.
  memset(&s_diag, 0, sizeof(s_diag));
  s_inFlightUs = 0;
  s_inFlightLba = 0;
  s_inFlightCount = 0;
  s_stallLogged = 0;
  s_ringCount = 0;

  s_dev = blockDevice;
  const uint32_t sectors = sectorCount();
  if (sectors == 0) {
    ESP_LOGE(TAG, "prepare: block device reports 0 sectors");
    s_dev = nullptr;
    return ESP_ERR_INVALID_STATE;
  }

  // Vendor driver config (usb_virtual_disk.cc:448-453): automatic remount on
  // host connect/disconnect is OFF -- our hand-over is explicit and one-way.
  tinyusb_msc_driver_config_t driver_cfg = {};
  driver_cfg.user_flags.auto_mount_off = 1;
  driver_cfg.callback = nullptr;
  driver_cfg.callback_arg = nullptr;

  esp_err_t err = tinyusb_msc_install_driver(&driver_cfg);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "msc install driver failed: %s", esp_err_to_name(err));
    s_dev = nullptr;
    return err;
  }

  // Geometry: 512-byte LBA everywhere, no erase-before-write (SD media), and
  // disk_size an exact multiple of the block size -- storage_blockdev rejects
  // the medium otherwise.
  s_bdl_self.dev = blockDevice;
  s_bdl = &s_blockdev;
  s_blockdev.ctx = &s_bdl_self;
  s_blockdev.device_flags = {};
  s_blockdev.geometry.read_size = kSectorBytes;
  s_blockdev.geometry.write_size = kSectorBytes;
  s_blockdev.geometry.erase_size = kSectorBytes;
  s_blockdev.geometry.disk_size = static_cast<uint64_t>(sectors) * kSectorBytes;
  s_blockdev.geometry.recommended_read_size = 4096;
  s_blockdev.geometry.recommended_write_size = 4096;
  s_blockdev.geometry.recommended_erase_size = 4096;
  s_blockdev.ops = &s_bdl_ops;

  // Vendor storage config (usb_virtual_disk.cc:461-469): never format, and
  // start owned by APP so no second mount of the same volume can happen.
  tinyusb_msc_storage_config_t storage_cfg = {};
  storage_cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP;
  storage_cfg.fat_fs.base_path = s_basePath;
  storage_cfg.fat_fs.config.max_files = 5;
  storage_cfg.fat_fs.config.format_if_mount_failed = false;
  storage_cfg.fat_fs.do_not_format = true;
  storage_cfg.medium.blockdev = s_bdl;

  err = tinyusb_msc_new_storage_blockdev(&storage_cfg, &s_hdl);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "msc new storage failed: %s", esp_err_to_name(err));
    s_hdl = nullptr;
    s_dev = nullptr;
    return err;
  }
  ESP_LOGI(TAG, "storage ready (%u sectors x %u B) at %s (APP)", (unsigned)sectors,
           (unsigned)kSectorBytes, s_basePath);

  // DIAG1: only a session that actually got its storage counts as one; a failed
  // prepare leaves the magic clear, so the next boot dumps nothing.
  s_diag.magic = kDiagMagic;

  // Name the drive on the PC while this volume is still ours to write to.
  ensureVolumeLabel();
  return ESP_OK;
#endif
}

esp_err_t exposeToHost() {
  if (!armed()) {
    ESP_LOGW(TAG, "exposeToHost refused: USB-MSC not armed");
    return ESP_ERR_INVALID_STATE;
  }
#if !CP_USB_MSC_STORAGE_COMPILED
  return ESP_ERR_NOT_SUPPORTED;
#else
  if (s_hdl == nullptr) return ESP_ERR_INVALID_STATE;

  tinyusb_msc_mount_point_t mp;
  esp_err_t err = tinyusb_msc_get_storage_mount_point(s_hdl, &mp);
  if (err != ESP_OK) return err;
  if (mp == TINYUSB_MSC_STORAGE_MOUNT_USB) return ESP_OK;  // idempotent

  // One-way by design: no callback, no retry, no rollback. A failure here is
  // answered by the caller rebooting, which is the same answer the vendor gives
  // on exit.
  err = tinyusb_msc_set_storage_mount_point(s_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "expose to host failed: %s", esp_err_to_name(err));
    return err;
  }
  ESP_LOGW(TAG, "SD -> USB host: host owns the sectors until reboot");
  return ESP_OK;
#endif
}

bool exportedToHost() {
#if !CP_USB_MSC_STORAGE_COMPILED
  return false;
#else
  if (s_hdl == nullptr) return false;
  tinyusb_msc_mount_point_t mp;
  if (tinyusb_msc_get_storage_mount_point(s_hdl, &mp) != ESP_OK) return false;
  return mp == TINYUSB_MSC_STORAGE_MOUNT_USB;
#endif
}

esp_err_t teardown() {
  if (!armed()) return ESP_ERR_INVALID_STATE;
#if !CP_USB_MSC_STORAGE_COMPILED
  return ESP_ERR_NOT_SUPPORTED;
#else
  if (s_hdl == nullptr) return ESP_OK;
  // Storage first, then the driver -- the vendor's order on the way down
  // (usb_virtual_disk.cc:530-546). No remount: exit is a cold boot.
  const esp_err_t del = tinyusb_msc_delete_storage(s_hdl);

  // THE HANDLE IS DROPPED WHATEVER delete_storage SAYS. Driver rule
  // (tinyusb_msc.c:1103-1140): every error return is taken BEFORE
  // msc_storage_delete(storage) at :1138, so a failed delete leaves the object
  // alive and still mapped to its LUN, and there is no public call that would
  // free it afterwards. What it also is, from here on, is unusable: in the
  // ESP_ERR_NOT_FOUND case msc_storage_unmount() at :1115-1117 has already torn
  // down the APP volume. Keeping the pointer would make the NEXT prepare() return
  // ESP_OK on it (the idempotence check below, :335) and the next
  // exposeToHost() act on a dead handle -- exactly what a failed start must not
  // leave behind. So the handle is dropped here and the object is orphaned until
  // the reboot that this abort path is written for (the same answer the driver
  // gives for a failed delete: reboot reclaims it).
  if (del != ESP_OK) {
    ESP_LOGW(TAG, "delete storage failed: %s -- s_hdl CLEARED, object orphaned until reboot",
             esp_err_to_name(del));
  }
  s_hdl = nullptr;
  s_dev = nullptr;
  // Safe to ask unconditionally: the driver refuses with ESP_ERR_INVALID_STATE
  // while any storage instance is still mapped (tinyusb_msc.c:817) and frees
  // nothing in that case.
  if (tinyusb_msc_uninstall_driver() != ESP_OK) {
    ESP_LOGW(TAG, "msc uninstall driver failed (a storage instance may still hold it)");
  }
  return del;
#endif
}

// ---- DIAG1 public surface -------------------------------------------------

void diagTick() {
  if (s_diag.magic != kDiagMagic) return;  // no live session: nothing to judge
  const uint32_t start = s_inFlightUs;
  if (start == 0) return;  // nothing is inside the driver
  const uint32_t elapsedMs = (nowUs() - start) / 1000u;
  if (elapsedMs < kStallMs) return;
  if (s_stallLogged != 0) return;  // one line per stalled op, never a stream
  s_stallLogged = 1;
  ++s_diag.stalls;
  // eTaskGetState of the TinyUSB task is the other half of the evidence: a task
  // stuck in the SDMMC wait reads Running, one that never got the callback reads
  // Blocked/Ready, so the two failure shapes are told apart from the log alone.
  const TaskHandle_t tusb = xTaskGetHandle("TinyUSB");
  const eTaskState state = tusb != nullptr ? eTaskGetState(tusb) : eInvalid;
  LOG_ERR(TAG, "STALL op in flight %u ms lba=%u n=%u tusb=%s", (unsigned)elapsedMs, (unsigned)s_inFlightLba,
          (unsigned)s_inFlightCount, taskStateName(state));
}

void preserveRtcRing() {
  if (s_diag.magic != kDiagMagic) return;  // nothing from a session to keep
  // HalSystem::begin() clears the shared ring on every ordinary boot, and the
  // card is not mounted yet, so this is the only moment the session's lines can
  // be read. Copy, do not log: there is nowhere to log to yet.
  s_ringCount = static_cast<uint32_t>(logRtcRingCopy(&s_ringSnap[0][0], kSnapLen, kSnapLines));
}

bool dumpSessionToLog() {
  if (s_diag.magic != kDiagMagic) return false;  // no session since the last dump
  s_diag.magic = 0;                             // consume: exactly one dump per session

  char line[200];
  snprintf(line, sizeof(line),
           "[%lu] [INF] [USBMSC] session summary: ops_ok=%u ops_err=%u slow=%u max_us=%u last_lba=%u "
           "last_count=%u stalls=%u",
           static_cast<unsigned long>(millis()), (unsigned)s_diag.ops_ok, (unsigned)s_diag.ops_err,
           (unsigned)s_diag.ops_slow, (unsigned)s_diag.max_us, (unsigned)s_diag.last_lba,
           (unsigned)s_diag.last_count, (unsigned)s_diag.stalls);
  bool ok = usbmscjournal::writeLine(line);

  // The session's own RTC lines, marked so a pasted log shows at a glance which
  // lines came from before the reboot.
  for (uint32_t i = 0; i < s_ringCount && i < kSnapLines; ++i) {
    if (s_ringSnap[i][0] == '\0') continue;
    char out[kSnapLen + 8];
    snprintf(out, sizeof(out), "[RTC]%s", s_ringSnap[i]);
    if (!usbmscjournal::writeLine(out)) ok = false;
  }
  s_ringCount = 0;
  s_ringSnap[0][0] = '\0';
  return ok;
}

}  // namespace usbmsc
