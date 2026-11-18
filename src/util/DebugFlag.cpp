#include "DebugFlag.h"

#include <HalStorage.h>

namespace {
constexpr const char* kFlagPath = "/.crosspoint/debug.flag";
}  // namespace

namespace debugflag {

bool enabled() {
  HalFile f = Storage.open(kFlagPath);
  if (!f) return false;
  char c = 0;
  const int n = f.read(&c, 1);
  f.close();
  // Absent flag, empty file or anything unexpected means OFF: the default must be silent.
  return n == 1 && (c == '1' || c == 'y' || c == 'Y');
}

void setEnabled(const bool on) {
  Storage.mkdir("/.crosspoint");
  Storage.writeFile(kFlagPath, on ? "1\n" : "0\n");
}

}  // namespace debugflag