#include "UsbMscJournal.h"

#include <HalStorage.h>
#include <Logging.h>
#include <esp_attr.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kLogPath = "/.crosspoint/debug.log";
constexpr size_t kDeferredLen = 200;

// RTC memory survives ESP.restart() (SW reset) but not power loss, which is the
// same contract SilentRestart relies on. The magic word is what tells a real
// pending line from uninitialized garbage on a cold boot.
RTC_NOINIT_ATTR char s_deferred[kDeferredLen];
RTC_NOINIT_ATTR uint32_t s_deferredMagic;
constexpr uint32_t kDeferredMagic = 0x4D53434A;  // "MSCJ"

}  // namespace

namespace usbmscjournal {

bool writeLine(const char* line) {
  if (line == nullptr || line[0] == '\0') return false;
  Storage.mkdir("/.crosspoint");
  // C1: debug.log had no cap and grew to 774 KB. Roll it (debug.log -> .old) before
  // appending, using the same mechanism sleep.txt uses.
  logSdRollIfOversized();
  HalFile f = Storage.open(kLogPath, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return false;
  const size_t len = strlen(line);
  const bool ok = f.write(reinterpret_cast<const uint8_t*>(line), len) == len && f.write('\n') == 1;
  f.flush();
  f.close();
  return ok;
}

void deferLine(const char* line) {
  if (line == nullptr || line[0] == '\0') return;
  strncpy(s_deferred, line, kDeferredLen - 1);
  s_deferred[kDeferredLen - 1] = '\0';
  s_deferredMagic = kDeferredMagic;
}

bool flushDeferred() {
  if (s_deferredMagic != kDeferredMagic) return false;  // cold boot / nothing pending
  s_deferredMagic = 0;
  if (s_deferred[0] == '\0') return false;
  const bool ok = writeLine(s_deferred);
  s_deferred[0] = '\0';
  return ok;
}

}  // namespace usbmscjournal
