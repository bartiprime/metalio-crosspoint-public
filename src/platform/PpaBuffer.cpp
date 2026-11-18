#include "PpaBuffer.h"

#include <esp_cache.h>
// esp_cache_get_alignment() is declared in a private header that lives under
// esp_mm's public include dir (same include the existing PpaService.cpp uses).
#include <esp_heap_caps.h>
#include <esp_private/esp_cache_private.h>

#include <cstring>

namespace ppa {

namespace {
// Rounded UP to a whole number of cache lines: the driver checks the buffer's
// start address and its window size, and a partial line at the end would make
// the M2C invalidation touch memory the buffer does not own.
size_t roundUp(const size_t bytes, const size_t align) { return ((bytes + align - 1) / align) * align; }
}  // namespace

size_t Buffer::alignment() {
  static size_t cached = 0;
  if (cached != 0) return cached;
  size_t a = 0;
  if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &a) != ESP_OK) a = 64;
  if (a < 64) a = 64;
  cached = a;
  return cached;
}

bool Buffer::alloc(const size_t bytes, const bool preferPsram) {
  release();
  if (bytes == 0) return false;
  align_ = alignment();
  capacity_ = roundUp(bytes, align_);
  const uint32_t caps = (preferPsram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_8BIT;
  data_ = heap_caps_aligned_alloc(align_, capacity_, caps);
  if (data_ == nullptr && preferPsram) {
    // Internal RAM is the documented fallback: the operation is still proved
    // correct, it just does not exercise the external-memory path.
    data_ = heap_caps_aligned_alloc(align_, capacity_, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    psram_ = false;
  } else {
    psram_ = preferPsram;
  }
  if (data_ == nullptr) {
    capacity_ = 0;
    align_ = 0;
    return false;
  }
  size_ = bytes;
  owned_ = true;
  memset(data_, 0, capacity_);
  return true;
}

void Buffer::adopt(void* ptr, const size_t bytes) {
  release();
  data_ = ptr;
  size_ = bytes;
  capacity_ = bytes;
  align_ = alignment();
  psram_ = true;  // callers adopt the framebuffer, which is in PSRAM by contract
  owned_ = false;
}

void Buffer::release() {
  if (data_ != nullptr && owned_) heap_caps_free(data_);
  data_ = nullptr;
  size_ = 0;
  capacity_ = 0;
  align_ = 0;
  psram_ = false;
  owned_ = false;
}

bool Buffer::syncToDevice() const {
  if (data_ == nullptr || capacity_ == 0) return false;
  // UNALIGNED is safe here because capacity_ is a whole number of cache lines.
  return esp_cache_msync(data_, capacity_, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED) == ESP_OK;
}

bool Buffer::syncFromDevice() const {
  if (data_ == nullptr || capacity_ == 0) return false;
  return esp_cache_msync(data_, capacity_, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED) == ESP_OK;
}

}  // namespace ppa
