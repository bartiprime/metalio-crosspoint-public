#pragma once
// Host stub: NVS as one process-wide key/value store, so a test can "reboot" by
// calling BatteryStats::begin() again and see exactly what the device would read.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

inline std::map<std::string, std::vector<uint8_t>>& hostNvs() {
  static std::map<std::string, std::vector<uint8_t>> store;
  return store;
}
inline void hostNvsReset() { hostNvs().clear(); }
inline int& hostNvsWrites() {
  static int writes = 0;
  return writes;
}
inline bool hostNvsHas(const char* key) { return hostNvs().count(key) != 0; }

class Preferences {
 public:
  bool begin(const char* ns, const bool readonly = false) {
    ns_ = ns;
    readonly_ = readonly;
    return true;
  }
  void end() { ns_.clear(); }

  size_t getBytes(const char* key, void* out, const size_t len) const {
    const auto it = hostNvs().find(std::string(ns_) + "/" + key);
    if (it == hostNvs().end()) return 0;
    const size_t n = it->second.size() < len ? it->second.size() : len;
    std::memcpy(out, it->second.data(), n);
    return n;
  }

  size_t putBytes(const char* key, const void* in, const size_t len) {
    if (readonly_) return 0;
    auto& slot = hostNvs()[std::string(ns_) + "/" + key];
    slot.assign(static_cast<const uint8_t*>(in), static_cast<const uint8_t*>(in) + len);
    ++hostNvsWrites();
    return len;
  }

 private:
  std::string ns_;
  bool readonly_ = false;
};
