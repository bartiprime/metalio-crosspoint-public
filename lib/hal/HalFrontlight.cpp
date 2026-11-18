#include "HalFrontlight.h"

#include <Logging.h>

HalFrontlight HalFrontlight::instance;

void HalFrontlight::begin(const uint8_t brightness, const uint8_t warmth, const bool on) {
  // begin() runs the hardware probe (EEGO A4: an I2C ACK from the LM3630A at
  // 0x36) that decides present(), so it must come FIRST. Guarding on present()
  // before begin() would skip the probe forever and hide the light on units
  // that actually have one. begin() is inert on boards without a frontlight.
  manager.begin();
  if (!manager.present()) return;

  lastBrightness = brightness > 100 ? 100 : brightness;
  manager.setColorTemperature(warmth > 100 ? 100 : warmth);
  lit = on;
  manager.setBrightness(lit ? lastBrightness : 0);
  LOG_INF("LIGHT", "Frontlight up: %u%% warm=%u%% %s", lastBrightness, manager.colorTemperature(), lit ? "on" : "off");
}

void HalFrontlight::setBrightness(const uint8_t percent) {
  lastBrightness = percent > 100 ? 100 : percent;
  if (lit) manager.setBrightness(lastBrightness);
}

void HalFrontlight::setWarmth(const uint8_t warmPercent) {
  manager.setColorTemperature(warmPercent > 100 ? 100 : warmPercent);
}

void HalFrontlight::setOn(const bool on) {
  if (on == lit) return;
  lit = on;
  manager.setBrightness(lit ? lastBrightness : 0);
}

void HalFrontlight::reapply() {
  if (!manager.present()) return;
  manager.setBrightness(lit ? lastBrightness : 0);
}

// E4d: park the frontlight at the SOURCE, not at duty 0.
//
// Frontlight.setOn(false) writes duty 0 and leaves everything else running: the
// LEDC channels stay attached to GPIO6/7 (this board's YX6016N cool/warm pair),
// the timer keeps clocking, and on this build the pads are still driven by the
// peripheral for the whole standby. parkPads() detaches both pads and holds them
// LOW, which is the state the driver IC sees as "enable off". The manager's
// brightness/warmth are NOT touched, so releasePads() can restore the exact duty.
//
// The digital hold is unconditional-on-release for the same reason the SDK's
// releaseOnWake() is: a held pad silently ignores ledcAttach()'s drive, and a
// DRAM flag cannot be trusted after a sleep.
bool HalFrontlight::parkPads() {
#if defined(ARDUINO_ARCH_ESP32) && ESP_ARDUINO_VERSION_MAJOR >= 3
  const auto& fl = BoardConfig::ACTIVE.frontlight;
  if (fl.gpio < 0) return false;
  for (const int8_t pin : {fl.gpio, fl.gpioWarm}) {
    if (pin < 0) continue;
    ledcDetach(static_cast<uint8_t>(pin));
    gpio_hold_dis(static_cast<gpio_num_t>(pin));
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    gpio_hold_en(static_cast<gpio_num_t>(pin));
  }
  padsParked_ = true;
  return true;
#else
  return false;  // no LEDC API on this core: fall back to setOn(false)
#endif
}

bool HalFrontlight::releasePads() {
#if defined(ARDUINO_ARCH_ESP32) && ESP_ARDUINO_VERSION_MAJOR >= 3
  const auto& fl = BoardConfig::ACTIVE.frontlight;
  if (fl.gpio < 0) return false;
  bool ok = true;
  for (const int8_t pin : {fl.gpio, fl.gpioWarm}) {
    if (pin < 0) continue;
    gpio_hold_dis(static_cast<gpio_num_t>(pin));  // unconditional, idempotent
    if (!ledcAttach(static_cast<uint8_t>(pin), fl.pwmFrequency, fl.pwmResolutionBits)) ok = false;
  }
  reapply();
  padsParked_ = false;
  return ok;
#else
  return false;
#endif
}

