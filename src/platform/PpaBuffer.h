#pragma once

#include <cstddef>
#include <cstdint>

#include "PlatformFeatures.h"

// ---------------------------------------------------------------------------
// CP_PPA_BENCH — the two obligations of using the PPA on this board, solved once.
//
// WHY A WRAPPER
//   `ppa_do_scale_rotate_mirror()` refuses a buffer that is not aligned to what
//   `esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, ...)` reports
//   (the driver derives ext_mem_align at ppa_core.c:94 and enforces it per
//   transaction in ppa_check_buffer_alignment(), ppa_core.c:610-647), and its DMA
//   reads and writes bypass the CPU, so every buffer needs an explicit C2M sync
//   before the PPA reads it and an M2C sync before the CPU reads the result.
//   Both were done inline for three 1 KiB buffers inside PpaService::selfTest();
//   this type is that knowledge, written once, so no call site re-derives it.
//
// THE STRIDE FINDING (plan §4.4) — why there is no partial-rectangle bench
//   Our framebuffer is 152 bytes per line (BoardConfig.h:2228-2231). 152 is not
//   a multiple of 32, 64 or 128, so the PPA's row stride for the live framebuffer
//   is not cache-line legal. A full-frame operation is fine (the stride simply is
//   152 and the destination is a separate aligned buffer); a sub-rectangle
//   operation whose row stride the driver will reject is not. r96 therefore
//   states the finding and does not attempt a sub-rect op on the framebuffer.
//
// SWITCH: CP_PPA_BENCH. With it at 0 this header's class exists but nothing
// allocates, and ppa::benchFullFrame() reports "not built".
// ---------------------------------------------------------------------------
namespace ppa {

// One DMA-visible buffer: aligned allocation, size rounded UP to the alignment,
// and explicit cache maintenance on both sides.
class Buffer {
 public:
  Buffer() = default;
  ~Buffer() { release(); }
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  // `preferPsram` selects MALLOC_CAP_SPIRAM (the interesting case: the PPA's
  // external-memory path) or internal RAM (the fallback that still proves the
  // operation is correct). Returns false and stays unusable on failure.
  bool alloc(size_t bytes, bool preferPsram);

  // Adopt an existing, already-qualifying region (the live framebuffer). No
  // ownership: release() will not free it. Used by `ppabench fb`.
  void adopt(void* ptr, size_t bytes);

  void release();

  void* data() const { return data_; }
  size_t size() const { return size_; }       // requested bytes
  size_t capacity() const { return capacity_; }  // rounded to the alignment
  size_t align() const { return align_; }
  bool ok() const { return data_ != nullptr; }
  bool inPsram() const { return psram_; }
  bool owned() const { return owned_; }

  // C2M: CPU finished writing, make it visible to the PPA's DMA.
  bool syncToDevice() const;
  // M2C: the PPA finished writing, discard stale CPU cache lines.
  bool syncFromDevice() const;

  // The cache-line alignment this SoC's PPA demands, cached after first query.
  static size_t alignment();

 private:
  void* data_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0;
  size_t align_ = 0;
  bool psram_ = false;
  bool owned_ = false;
};

}  // namespace ppa
