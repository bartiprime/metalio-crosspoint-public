#pragma once

// ---- r110 WRITEGATE: one predicate, every write-capable open flag --------------------------
// WHY: r109 made the storage layer refuse writes when the volume was not vouched for, but it
// gated the NAMED write helpers (writeFile, mkdir, remove, ...) and left `HalStorage::open()`
// alone. SdFat's open() with O_CREAT writes a directory entry AT OPEN (FatFileLFN.cpp, create),
// and O_TRUNC zeroes the file's cluster chain at open, so a single `open(path, O_RDWR|O_CREAT)`
// on a markerless boot could lay down a directory entry on a volume the boot had just refused
// to initialise. `HalFile::truncate()` and `HalFile::rename()` were open the same way.
//
// THE RULE: no flag combination that can write may reach the card while storage is read-only.
// Read-only opens (O_RDONLY, with or without O_AT_END) MUST still work -- the whole point of a
// read-only boot is that the operator can still browse the card they were not allowed to write.
//
// SdFat picks between two encodings of these flags (SdFatConfig.h USE_FCNTL_H): the system
// fcntl.h on ESP32/ARM, and its own 1/2/4/8/0x10/0x20 set elsewhere. The predicate is written
// in terms of the NAMES, so it is correct under either; test/storage_write_gate compiles it
// against both encodings and asserts the same verdicts.

#include <common/FsApiConstants.h>

namespace halstorage {

// Pure: true when this oflag can create, truncate, or write through the handle.
// `acc != O_RDONLY` covers O_WRONLY, O_RDWR and the (nonsense but not impossible) both-bits
// encoding, under either flag set, because O_RDONLY is 0 in both.
inline constexpr bool isWriteOflag(const oflag_t oflag) {
  const oflag_t acc = static_cast<oflag_t>(oflag & O_ACCMODE);
  return acc != O_RDONLY || (oflag & O_CREAT) != 0 || (oflag & O_TRUNC) != 0 || (oflag & O_APPEND) != 0;
}

}  // namespace halstorage
