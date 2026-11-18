#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

class FsBlockDeviceInterface;

// USB mass storage storage layer (scope C4).
//
// The SD card is handed to the host ONCE; it is taken back by REBOOTING, not by
// a mount-point dance. Exit-by-cold-boot is the operator's chosen UX and it
// removes the vendor's MountToApp/MountToUsb retry/rollback machinery
// (usb_virtual_disk.cc:300-357, :512-556) from the critical path: a reboot
// re-establishes the FS, the UI, the sleep timers and the console route from a
// known-good state.
//
// Ownership chain (each step is checked, none can silently overlap):
//   SDCardManager::detachFilesystemForRawAccess()   -> FsVolume ended
//   usbmsc::prepare(blockDevice)                    -> MSC storage bound to it
//   usbmsc::exposeToHost()                          -> host owns the sectors
//   ... session ...
//   ESP.restart()                                   -> everything re-mounts
//
// The card is reached through SdFat's FsBlockDeviceInterface -- the same
// 512-byte sector path the reader uses -- bridged to esp_blockdev so
// esp_tinyusb's MSC can drive it. That leaves the board's proven SDMMC bring-up
// (pin map, DAT3 pull-up, power-gate retries in SdmmcBlockDevice.cpp) untouched
// instead of re-initialising a raw sdmmc_card_t behind its back, which is what
// the vendor's InitRawCardForMsc had to do (their stack is VFS/FAT-based).
//
// Everything is refused unless the runtime flag is armed (usbport::armed()).
namespace usbmsc {

// MSC LBA unit: the SD card and the SCSI layer both use 512-byte sectors, so
// there is no translation layer between them.
constexpr uint32_t kSectorBytes = 512;

// The name the drive carries on the PC. A host labels a removable volume from
// the FAT VOLUME LABEL, not from a USB descriptor, and the USB product string
// only names the DEVICE -- so ONE spelling lives here and feeds both, which is
// what keeps the two views of the drive from disagreeing.
constexpr const char* kVolumeLabel = "Crosspoint";

// True when esp_tinyusb's MSC storage support was actually compiled in
// (CONFIG_TINYUSB_MSC_ENABLED and the component's headers present).
bool compiledIn();

// The single runtime gate; forwarded to usbport so there is exactly one flag.
void setArmed(bool armed);
bool armed();

// Binds MSC storage to an ALREADY-DETACHED block device and installs the MSC
// driver. Idempotent. The caller must hold HalStorage's storage mutex and must
// not use the FS afterwards -- the volume is gone until the reboot.
// Mount point starts at APP so nothing else can mount the same volume.
esp_err_t prepare(FsBlockDeviceInterface* blockDevice);

// Hands the sectors to the host. Single-shot by design: no switch-back.
esp_err_t exposeToHost();
bool exportedToHost();

// Drops storage + driver without remounting. Only for the abort path where the
// gadget never came up; the normal exit is a cold boot.
esp_err_t teardown();

// 512-byte sector access to the bound device (0 sectors when nothing is bound).
// Drives the esp_blockdev callbacks below and the round-2 self-check.
esp_err_t readSectors(uint32_t lba, void* dst, uint32_t count);
esp_err_t writeSectors(uint32_t lba, const void* src, uint32_t count);
uint32_t sectorCount();

// ---- DIAG1: drive-mode session instrument (deletable) --------------------
// A drive-mode session is a blackout -- the card is the host's, the console is
// gone, and the exit is a cold boot -- so the only evidence that survives is
// what these functions keep in RTC_NOINIT memory. They observe; they never
// change what the bridge does.
//
// Boot order matters and is forced: preserveRtcRing() MUST be called before
// HalSystem::begin() (which clears the shared RTC log ring), and
// dumpSessionToLog() is called after Storage.begin() next to
// usbmscjournal::flushDeferred(). Both no-op unless a session actually ran.
void preserveRtcRing();
bool dumpSessionToLog();
// 1 s tick from the drive-mode activity: reports an op that has been inside the
// driver for longer than 3 s. Does nothing when no session is live.
void diagTick();

}  // namespace usbmsc
