// HOST STUB for tools/theme_tokens/check_theme_syntax.sh. NOT part of the firmware
// build: it lives under tools/ and is only ever reached through that script's -I.
// Its job is to let a theme .cpp be type-checked on the host against the REAL
// BaseTheme.h; every renderer signature below is copied from the real header.
#pragma once
#include <cstdint>
#include "HalStorage.h"
enum class BmpReaderError { Ok, BadMagic, BadHeader };
class Bitmap {
 public:
  explicit Bitmap(HalFile& f);
  BmpReaderError parseHeaders();
  int getWidth() const;
  int getHeight() const;
};
