#include "HalTiltSensor.h"

#include <Logging.h>

HalTiltSensor halTiltSensor;  // Singleton instance

namespace {

flick::Orientation toDetectorOrientation(uint8_t orientation) {
  switch (orientation) {
    case CrossPointOrientation::LANDSCAPE_CW:
      return flick::Orientation::LandscapeCw;
    case CrossPointOrientation::INVERTED:
      return flick::Orientation::Inverted;
    case CrossPointOrientation::LANDSCAPE_CCW:
      return flick::Orientation::LandscapeCcw;
    case CrossPointOrientation::PORTRAIT:
    default:
      return flick::Orientation::Portrait;
  }
}

flick::Mode toDetectorMode(uint8_t mode) {
  switch (mode) {
    case CrossPointTiltPageTurn::TILT_NORMAL:
      return flick::Mode::Normal;
    case CrossPointTiltPageTurn::TILT_INVERTED:
      return flick::Mode::Inverted;
    case CrossPointTiltPageTurn::TILT_OFF:
    default:
      return flick::Mode::Off;
  }
}

}  // namespace

bool HalTiltSensor::readAccel(float& ax, float& ay, float& az) const {
  Imu::Sample sample;
  if (!_sdkImu.read(sample)) return false;
  ax = sample.ax;
  ay = sample.ay;
  az = sample.az;
  return true;
}

void HalTiltSensor::applyConfig() {
  _config = flick::Config{};
  if (!_calibration.valid) return;
  _config.triggerG = _calibration.triggerG;
  _config.startG = _calibration.startG;
  _config.nextSign = _calibration.nextSign;
}

void HalTiltSensor::begin() {
  if (!_sdkImu.begin()) {
    LOG_ERR("GYR", "SDK IMU not found");
    return;
  }
  // begin() leaves the sensors sampling; stand them by until the flick
  // detector actually wakes them, so a disabled IMU doesn't drain the battery.
  if (!_sdkImu.sleep()) {
    LOG_ERR("GYR", "IMU standby failed");
  }
  // The gesture is read from the accelerometer, so any part the driver can
  // read works -- including the accelerometer-only SC7A20H. hasGyroscope() is
  // deliberately not the gate.
  _available = _sdkImu.present();
  if (!_available) {
    LOG_ERR("GYR", "IMU not present");
    return;
  }
  _detector = flick::State{};
  if (tiltcalstore::load(_calibration)) {
    LOG_INF("GYR", "SDK IMU ready (flick); calibration trigger=%.2f g start=%.2f g nextSign=%d", _calibration.triggerG,
            _calibration.startG, static_cast<int>(_calibration.nextSign));
  } else {
    _calibration = flickcal::Calibration{};
    LOG_INF("GYR", "SDK IMU ready (flick); compiled defaults trigger=%.2f g start=%.2f g nextSign=%d",
            _config.triggerG, _config.startG, static_cast<int>(_config.nextSign));
  }
  applyConfig();
}

bool HalTiltSensor::wake() {
  if (!_available) {
    return false;
  }

  if (!_sdkImu.wake()) {
    LOG_ERR("GYR", "IMU wake failed");
    return false;
  }

  _lastPollMs = millis();
  _lastSampleMs = 0;
  _wakeMs = millis();
  _detector = flick::State{};  // re-learn the posture from the current hold
  _isAwake = true;
  return true;
}

bool HalTiltSensor::deepSleep() {
  if (!_available) {
    return false;
  }

  if (!_sdkImu.sleep()) {
    LOG_ERR("GYR", "IMU sleep failed");
    return false;
  }

  clearPendingEvents();
  _detector = flick::State{};
  _isAwake = false;
  _capturing = false;
  return true;
}

void HalTiltSensor::update(const uint8_t mode, const uint8_t orientation, const bool inReader, const bool touchActive) {
  if (!_available) {
    return;
  }

  // The calibration screen samples the sensor itself while it is up.
  if (_capturing) {
    return;
  }

  // State machine: wake up or sleep based on the enabled flag
  if ((mode != CrossPointTiltPageTurn::TILT_OFF) && !_isAwake) {
    _isAwake = wake();
    return;
  } else if ((mode == CrossPointTiltPageTurn::TILT_OFF) && _isAwake) {
    _isAwake = !deepSleep();
    return;
  }

  // If disabled, skip the rest of the polling logic and avoid unnecessary I2C traffic in non-reader activities
  if ((mode == CrossPointTiltPageTurn::TILT_OFF) || !inReader) {
    return;
  }

  const unsigned long now = millis();
  // Stabilization: discard readings during the sensor's startup transient
  if ((now - _wakeMs) < WAKE_STABILIZE_MS) {
    return;
  }

  if ((now - _lastPollMs) < POLL_INTERVAL_MS) {
    return;
  }
  _lastPollMs = now;

  float ax, ay, az;
  if (!readAccel(ax, ay, az)) {
    return;
  }

  // The decision logic owns no clock: hand it the real sample spacing so the
  // filter, baseline and window timers stay correct if a poll is late.
  const uint32_t dt = _lastSampleMs == 0 ? POLL_INTERVAL_MS : static_cast<uint32_t>(now - _lastSampleMs);
  _lastSampleMs = now;

  const flick::Event event = flick::step(_detector, _config, ax, ay, az, dt, touchActive,
                                         toDetectorOrientation(orientation), toDetectorMode(mode));

  if (event == flick::Event::None) {
    return;
  }

  _hadActivity = true;
  _lastFireMs = now;
  ++_fires;
  if (event == flick::Event::Next) {
    _flickNextEvent = true;
  } else {
    _flickPrevEvent = true;
  }
  LOG_INF("TILT", "%s peak=%.2f g sign=%d lat=%.2f ax=%.2f ay=%.2f az=%.2f",
          event == flick::Event::Next ? "flick next" : "flick prev", _detector.lastPeakG,
          static_cast<int>(_detector.lastSign), _detector.latG, ax, ay, az);
}

bool HalTiltSensor::wasFlickNext() {
  // An event nobody asked for within a couple of seconds belongs to a screen
  // the operator already left; drop it instead of turning a page later.
  if (_flickNextEvent && (millis() - _lastFireMs) > FIRE_TIMEOUT_MS) {
    _flickNextEvent = false;
  }
  const bool val = _flickNextEvent;
  _flickNextEvent = false;
  return val;
}

bool HalTiltSensor::wasFlickPrev() {
  if (_flickPrevEvent && (millis() - _lastFireMs) > FIRE_TIMEOUT_MS) {
    _flickPrevEvent = false;
  }
  const bool val = _flickPrevEvent;
  _flickPrevEvent = false;
  return val;
}

bool HalTiltSensor::hadActivity() {
  const bool val = _hadActivity;
  _hadActivity = false;
  return val;
}

void HalTiltSensor::clearPendingEvents() {
  _flickNextEvent = false;
  _flickPrevEvent = false;
  _hadActivity = false;
  // The detector state is preserved so a held posture doesn't retrigger on the
  // next poll.
}

// ---- guided calibration -------------------------------------------------

bool HalTiltSensor::beginCapture(const uint8_t mode) {
  if (!_available) return false;
  if (!_isAwake) {
    // Sampling is exclusive while the capture runs, so the feature being off
    // must not keep the part asleep -- but it is still put back to standby by
    // endCapture() + the next update().
    if (!wake()) return false;
    _wakeMs = 0;  // no stabilization hold: the screen is already showing the prompt
  }
  (void)mode;
  _capturing = true;
  _detector = flick::State{};
  _lastSampleMs = 0;
  return true;
}

bool HalTiltSensor::captureSample(float& ax, float& ay, float& az) {
  if (!_capturing) return false;
  if (!readAccel(ax, ay, az)) return false;
  return true;
}

void HalTiltSensor::endCapture() {
  _capturing = false;
  _detector = flick::State{};
  _lastSampleMs = 0;
  _lastPollMs = millis();
}

bool HalTiltSensor::saveCalibration(const flickcal::Calibration& cal) {
  if (!cal.valid) {
    LOG_ERR("TILT", "calibration refused: not valid");
    return false;
  }
  if (!tiltcalstore::save(cal)) {
    LOG_ERR("TILT", "calibration NOT persisted (NVS write failed); RAM only this session");
  }
  _calibration = cal;
  applyConfig();
  LOG_INF("TILT", "calibration saved trigger=%.2f g start=%.2f g nextSign=%d left=%.2f right=%.2f noise=%.2f",
          cal.triggerG, cal.startG, static_cast<int>(cal.nextSign), cal.leftPeakG, cal.rightPeakG, cal.noiseG);
  return true;
}

void HalTiltSensor::clearCalibration() {
  tiltcalstore::clear();
  _calibration = flickcal::Calibration{};
  applyConfig();
  LOG_INF("TILT", "calibration cleared; compiled defaults trigger=%.2f g nextSign=%d", _config.triggerG,
          static_cast<int>(_config.nextSign));
}

HalTiltSensor::Diag HalTiltSensor::diag() const {
  Diag d;
  d.present = _available;
  d.awake = _isAwake;
  d.capturing = _capturing;
  if (_available && !_capturing) {
    float ax, ay, az;
    if (readAccel(ax, ay, az)) {
      d.readOk = true;
      d.ax = ax;
      d.ay = ay;
      d.az = az;
    }
  }
  d.latG = _detector.latG;
  d.perpG = _detector.perpG;
  d.triggerG = _config.triggerG;
  d.startG = _config.startG;
  d.nextSign = _config.nextSign;
  d.calibrated = _calibration.valid;
  d.calLeftPeakG = _calibration.leftPeakG;
  d.calRightPeakG = _calibration.rightPeakG;
  d.calNoiseG = _calibration.noiseG;
  d.lastPeakG = _detector.lastPeakG;
  d.lastSign = _detector.lastSign;
  d.fires = _fires;
  d.lastFireAgeMs = _lastFireMs == 0 ? 0xFFFFFFFFul : (millis() - _lastFireMs);
  return d;
}
