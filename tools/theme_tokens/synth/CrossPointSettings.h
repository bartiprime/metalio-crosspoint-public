// HOST STUB for tools/theme_tokens/check_theme_syntax.sh. NOT part of the firmware
// build: it lives under tools/ and is only ever reached through that script's -I.
// Its job is to let a theme .cpp be type-checked on the host against the REAL
// BaseTheme.h; every renderer signature below is copied from the real header.
#pragma once
#include <cstdint>
class CrossPointSettings {
 public:
  enum UI_THEME { CLASSIC = 0, LYRA = 1, LYRA_3_COVERS = 2, ROUNDEDRAFF = 3, COVER_GRID = 4, ANDRO = 5, PICKYOS = 6 };
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_ALWAYS = 2 };
  uint8_t uiTheme = LYRA;
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  uint8_t clockFormat = 0;
  bool clockShowInHeader = true;
};
extern CrossPointSettings SETTINGS;
