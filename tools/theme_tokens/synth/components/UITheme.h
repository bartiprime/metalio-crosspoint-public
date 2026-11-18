// HOST STUB for tools/theme_tokens/check_theme_syntax.sh. NOT part of the firmware
// build: it lives under tools/ and is only ever reached through that script's -I.
// Its job is to let a theme .cpp be type-checked on the host against the REAL
// BaseTheme.h; every renderer signature below is copied from the real header.
#pragma once
#include <string>
#include "components/themes/BaseTheme.h"
class GfxRenderer;
class UITheme {
 public:
  static UITheme& getInstance();
  const ThemeMetrics& getMetrics() const;
  const BaseTheme& getTheme() const;
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
};
#define GUI UITheme::getInstance().getTheme()
