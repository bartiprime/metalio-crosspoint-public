#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "Serialization.h"

// r163 A13: a read-only view over bytes that are already in RAM.
//
// The page-record deserialisers (Page, PageLine, PageImage, PageHorizontalRule, PageDropCap,
// PageBorderBox, TextBlock, ImageBlock) are templated on the reader they read from. On a cold
// read the reader is a HalFile, so every scalar field in the record is one SD read -- ~340 of
// them for a 40-line page. When the same page is loaded again (which happens on every repaint,
// not just on page turns) Section serves the record from a RAM copy and passes this reader
// instead, so the deserialise costs zero SD reads.
//
// The API is exactly the subset of HalFile those deserialisers use, and read() keeps HalFile's
// signature and its "return the bytes actually read" contract, so a record that is truncated in
// RAM fails in the same place, and with the same error, as a truncated record on the card.
class MemReader {
 public:
  MemReader() = default;
  MemReader(const uint8_t* data, const size_t size) : data_(data), size_(size) {}

  // Never reads past the end of the buffer: returns the bytes actually copied, 0 once exhausted.
  int read(void* buf, size_t count) {
    if (!buf || pos_ >= size_) {
      return 0;
    }
    const size_t n = (count < size_ - pos_) ? count : size_ - pos_;
    std::memcpy(buf, data_ + pos_, n);
    pos_ += n;
    return static_cast<int>(n);
  }
  int read() { return pos_ < size_ ? data_[pos_++] : -1; }
  bool seek(const size_t pos) {
    if (pos > size_) {
      return false;
    }
    pos_ = pos;
    return true;
  }
  size_t position() const { return pos_; }
  size_t size() const { return size_; }
  int available() const { return static_cast<int>(size_ - pos_); }
  bool isOpen() const { return data_ != nullptr; }
  explicit operator bool() const { return data_ != nullptr; }

 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t pos_ = 0;
};

namespace serialization {
template <typename T>
inline void readPod(MemReader& reader, T& value) {
  reader.read(&value, sizeof(T));
}

inline void readString(MemReader& reader, std::string& s) {
  uint32_t len = 0;
  readPod(reader, len);
  // The length is untrusted (it comes off the card). Clamp it to what the buffer actually holds
  // so a corrupt record cannot ask for a multi-megabyte allocation; a short read leaves the tail
  // zeroed, which the caller's own validation rejects.
  const auto available = static_cast<uint32_t>(reader.size() - reader.position());
  if (len > available) {
    len = available;
  }
  s.resize(len);
  if (len > 0) {
    reader.read(s.data(), len);
  }
}
}  // namespace serialization
