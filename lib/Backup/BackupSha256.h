#pragma once
// ---------------------------------------------------------------------------
// SHA-256 for the BACKUP1 profile envelope.
//
// WHY THIS EXISTS RATHER THAN crosspoint_sha256.h
//   compat/include/crosspoint_sha256.h wraps PSA crypto, which only exists on
//   the device. BACKUP1's envelope check is a pure contract: it has to be
//   exercised by a host test (bad sha -> reject) without an ESP32 and without
//   linking PSA. This is a small, dependency-free implementation of FIPS 180-4,
//   used by both the firmware and the host test. The host test pins it against
//   the published NIST vectors, so a wrong implementation cannot silently agree
//   with itself on both sides.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <string>

namespace backup {

class Sha256 {
 public:
  Sha256() { reset(); }

  void reset();
  void update(const void* data, size_t len);

  // Writes the 32-byte digest. The object is left usable after finish().
  void finish(uint8_t out[32]);

  static std::string hex(const uint8_t* bytes, size_t len);

 private:
  uint32_t h_[8]{};
  uint8_t buf_[64]{};
  size_t used_ = 0;
  uint64_t total_ = 0;
};

// Digest of a UTF-8 std::string, as 64 lowercase hex characters.
std::string sha256Hex(const std::string& data);

}  // namespace backup
