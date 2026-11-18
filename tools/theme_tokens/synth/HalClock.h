// HOST STUB for tools/theme_tokens/check_theme_syntax.sh. NOT part of the firmware
// build: it lives under tools/ and is only ever reached through that script's -I.
// Its job is to let a theme .cpp be type-checked on the host against the REAL
// BaseTheme.h; every renderer signature below is copied from the real header.
#pragma once
#include <cstddef>
#include <ctime>
class HalClock {
 public:
  bool isAvailable() const;
  bool localTime(struct tm& out) const;
  bool formatTime(char* buf, size_t len, bool h24) const;
};
extern HalClock halClock;
