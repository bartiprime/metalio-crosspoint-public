// HOST STUB for tools/theme_tokens/check_theme_syntax.sh. NOT part of the firmware
// build: it lives under tools/ and is only ever reached through that script's -I.
// Its job is to let a theme .cpp be type-checked on the host against the REAL
// BaseTheme.h; every renderer signature below is copied from the real header.
#pragma once
#include <cstdint>
class HalPowerManager {
 public:
  uint16_t getBatteryPercentage();
  bool isChargingDisplayed();
};
extern HalPowerManager powerManager;
