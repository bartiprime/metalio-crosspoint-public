#include "BackupSha256.h"

#include <cstdio>
#include <cstring>

namespace backup {
namespace {

constexpr uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

void Sha256::reset() {
  h_[0] = 0x6a09e667u;
  h_[1] = 0xbb67ae85u;
  h_[2] = 0x3c6ef372u;
  h_[3] = 0xa54ff53au;
  h_[4] = 0x510e527fu;
  h_[5] = 0x9b05688cu;
  h_[6] = 0x1f83d9abu;
  h_[7] = 0x5be0cd19u;
  used_ = 0;
  total_ = 0;
}

namespace {
void compressBlock(uint32_t h[8], const uint8_t* block) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; i++) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
    const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = S0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}
}  // namespace

void Sha256::update(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  total_ += len;
  if (used_ != 0) {
    const size_t need = 64 - used_;
    const size_t take = len < need ? len : need;
    std::memcpy(buf_ + used_, p, take);
    used_ += take;
    p += take;
    len -= take;
    if (used_ == 64) {
      compressBlock(h_, buf_);
      used_ = 0;
    }
  }
  while (len >= 64) {
    compressBlock(h_, p);
    p += 64;
    len -= 64;
  }
  if (len != 0) {
    std::memcpy(buf_, p, len);
    used_ = len;
  }
}

void Sha256::finish(uint8_t out[32]) {
  const uint64_t totalBits = total_ * 8;
  // Append 0x80, pad with zeros, then the 64-bit big-endian length.
  buf_[used_++] = 0x80;
  if (used_ > 56) {
    std::memset(buf_ + used_, 0, 64 - used_);
    compressBlock(h_, buf_);
    used_ = 0;
  }
  std::memset(buf_ + used_, 0, 56 - used_);
  for (int i = 0; i < 8; i++) {
    buf_[56 + i] = static_cast<uint8_t>((totalBits >> (56 - i * 8)) & 0xff);
  }
  compressBlock(h_, buf_);
  used_ = 0;
  for (int i = 0; i < 8; i++) {
    out[i * 4 + 0] = static_cast<uint8_t>(h_[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
  }
}

std::string Sha256::hex(const uint8_t* bytes, size_t len) {
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    char pair[3];
    std::snprintf(pair, sizeof(pair), "%02x", static_cast<unsigned>(bytes[i]));
    out.push_back(pair[0]);
    out.push_back(pair[1]);
  }
  return out;
}

std::string sha256Hex(const std::string& data) {
  Sha256 h;
  h.update(data.data(), data.size());
  uint8_t digest[32];
  h.finish(digest);
  return Sha256::hex(digest, sizeof(digest));
}

}  // namespace backup
