#pragma once
// Host stub: the panel guard's only contract with this code is that an NVS commit
// happens inside an NvsWriteScope. Counting constructions proves the write that
// persists a reset went through the guard, and that no write ever happens outside one.
inline int& hostNvsGuardDepth() {
  static int depth = 0;
  return depth;
}
inline int& hostNvsGuardedWrites() {
  static int writes = 0;
  return writes;
}

namespace epdflash {

class NvsWriteScope {
 public:
  explicit NvsWriteScope(const char* reason) {
    (void)reason;
    ++hostNvsGuardDepth();
    ++hostNvsGuardedWrites();
  }
  ~NvsWriteScope() { --hostNvsGuardDepth(); }
  NvsWriteScope(const NvsWriteScope&) = delete;
  NvsWriteScope& operator=(const NvsWriteScope&) = delete;
};

}  // namespace epdflash
