#pragma once

#include <FrontlightManager.h>

// Thin firmware HAL over the SDK frontlight manager. It is inert on boards
// without a frontlight, so callers do not need board-specific conditionals.
class HalFrontlight {
 public:
  static HalFrontlight& getInstance() { return instance; }

  void begin(uint8_t brightness, uint8_t warmth, bool on);

  bool present() const { return manager.present(); }
  bool hasColorTemperature() const { return manager.hasColorTemperature(); }

  void setBrightness(uint8_t percent);
  void setWarmth(uint8_t warmPercent);
  void setOn(bool on);

  // ---- E4d standby park (HARDWARE_POWER_PLAN.md §4) -------------------------
  // parkPads() drives both PWM pads LOW and hands the pads back from the LEDC
  // peripheral (detached + digital hold), WITHOUT touching the manager's brightness
  // or warmth. Returns false when the board has no LEDC frontlight (nothing was
  // touched). releasePads() re-attaches the same channels and re-drives the duty
  // from the live state, so the user's brightness/colour come back byte-for-byte.
  bool parkPads();
  bool releasePads();

  // Re-drive the PWM from the current state, even when nothing changed. Needed on
  // the resume path: setOn() is transition-edged and would be a no-op after a
  // park/release cycle.
  void reapply();

  uint8_t brightness() const { return lastBrightness; }
  uint8_t warmth() const { return manager.colorTemperature(); }
  bool isOn() const { return lit; }
  // r106 `light`: whether parkPads() currently holds both pads LOW and detached, so the
  // shell can show "re-applied" vs "released" instead of guessing from brightness.
  bool padsParked() const { return padsParked_; }

 private:
  HalFrontlight() = default;

  FrontlightManager manager;
  // The SDK represents off as brightness 0. Keep the selected brightness so
  // toggling back on restores it.
  uint8_t lastBrightness = 60;
  bool lit = false;
  bool padsParked_ = false;

  static HalFrontlight instance;
};

#define Frontlight HalFrontlight::getInstance()
