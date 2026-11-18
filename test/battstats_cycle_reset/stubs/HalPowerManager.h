#pragma once
// Host stub for the two things BatteryStats.cpp asks of the power manager: the gauge
// percentage and the charger IC's own status. Both are settable so a test can drive a
// charge cycle sample by sample. millis() rides along here because on the device it
// arrives with the same HAL headers.
#include <cstdint>

inline uint32_t& hostMillis() {
  static uint32_t now = 0;
  return now;
}
inline uint32_t millis() { return hostMillis(); }

class HalPowerManager {
 public:
  uint16_t getBatteryPercentage() const { return soc_; }
  bool chargingForStats() const { return charging_; }
  // BATT6: the post-wake settle/recalibration gate BatteryStats::update() consults
  // before it samples. Settable so a test can hold it open and prove the sample is
  // deferred; false by default, i.e. every pre-BATT6 expectation is unchanged.
  bool socAwaitingPostWakeSample() const { return postWakeWait_; }
  void hostSet(const uint16_t soc, const bool charging) {
    soc_ = soc;
    charging_ = charging;
  }
  void hostSetPostWake(const bool waiting) { postWakeWait_ = waiting; }

 private:
  uint16_t soc_ = 80;
  bool charging_ = false;
  bool postWakeWait_ = false;
};

inline HalPowerManager powerManager;
