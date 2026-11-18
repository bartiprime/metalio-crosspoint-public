#pragma once

#include <Arduino.h>
#include <Imu.h>

#include <cstdint>

#include "FlickCalibration.h"
#include "FlickDetector.h"

// TODO: Move enums into new header and share with CrossPointSettings.h
namespace CrossPointOrientation {
enum Value : uint8_t { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3 };
}

namespace CrossPointTiltPageTurn {
enum Value : uint8_t { TILT_OFF = 0, TILT_NORMAL = 1, TILT_INVERTED = 2 };
}

// The persisted per-unit calibration, if the operator has run
// Settings > Controls > Calibrate flick page turn. Absent means the compiled
// defaults apply -- calibration is optional.
namespace tiltcalstore {
bool load(flickcal::Calibration& out);
bool save(const flickcal::Calibration& cal);
void clear();
}  // namespace tiltcalstore

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;  // Singleton

class HalTiltSensor {
  bool _available = false;
  mutable Imu _sdkImu;

  // Gesture events, consumed by the reader's page-turn detection.
  bool _flickNextEvent = false;
  bool _flickPrevEvent = false;
  bool _hadActivity = false;  // Non-consuming flag for the sleep timer
  bool _isAwake = false;
  unsigned long _wakeMs = 0;  // Timestamp of last wake() for stabilization
  unsigned long _lastPollMs = 0;
  unsigned long _lastSampleMs = 0;

  flick::State _detector;
  flick::Config _config;  // defaults, with the calibration applied when present
  flickcal::Calibration _calibration;

  // Calibration capture: while set, this class is the ONLY sampler, so the
  // calibration screen sees every sample instead of half of them.
  bool _capturing = false;

  // Tuning constants that belong to the sensor, not to the decision logic.
  static constexpr unsigned long POLL_INTERVAL_MS = 10;    // ~100 Hz, the SC7A20H's own ODR
  static constexpr unsigned long WAKE_STABILIZE_MS = 300;  // Ignore readings after wake
  static constexpr unsigned long FIRE_TIMEOUT_MS = 2000;   // an unconsumed event is dropped

  unsigned long _lastFireMs = 0;
  uint32_t _fires = 0;  // monotonic for this boot: the diagnostic's counter

  bool readAccel(float& ax, float& ay, float& az) const;
  void applyConfig();

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // Enables sampling
  bool wake();

  // Puts the sensor into hardware standby (the power contract: no sampling
  // current at all while the feature is off).
  bool deepSleep();

  // True if the active board has an accelerometer this class can read
  bool isAvailable() const { return _available; }

  // Poll the accelerometer and update flick gesture state.
  // touchActive suppresses gestures (a finger is on the glass).
  void update(const uint8_t mode, const uint8_t orientation, const bool inReader, const bool touchActive);

  // Returns true once per flick towards the NEXT page. Consumed on read.
  bool wasFlickNext();

  // Returns true once per flick towards the PREVIOUS page. Consumed on read.
  bool wasFlickPrev();

  // Non-consuming: true if any flick fired since the last call.
  bool hadActivity();

  // Discard any pending events (call when leaving reader or disabling the feature).
  void clearPendingEvents();

  // ---- guided calibration -------------------------------------------
  // Takes exclusive sampling: update() stops polling until endCapture().
  // Wakes the sensor if needed; the caller restores standby with endCapture()
  // when the feature itself is off.
  bool beginCapture(const uint8_t mode);
  bool captureSample(float& ax, float& ay, float& az);
  void endCapture();

  // Persist a capture. Refuses (and logs) anything the maths rejected.
  bool saveCalibration(const flickcal::Calibration& cal);
  void clearCalibration();
  bool calibrated() const { return _calibration.valid; }
  const flickcal::Calibration& calibration() const { return _calibration; }
  const flick::Config& config() const { return _config; }

  // ---- diagnostics (`tilt` shell command) ----------------------------
  struct Diag {
    bool present = false;
    bool awake = false;
    bool capturing = false;
    bool readOk = false;
    float ax = 0, ay = 0, az = 0;
    float latG = 0, perpG = 0;
    float triggerG = 0, startG = 0;
    bool calibrated = false;
    float calLeftPeakG = 0, calRightPeakG = 0, calNoiseG = 0;
    int8_t nextSign = 0;
    float lastPeakG = 0;
    int8_t lastSign = 0;
    uint32_t fires = 0;  // gestures fired since boot
    unsigned long lastFireAgeMs = 0;
  };
  // Reads the sensor once and snapshots the detector state. Safe from the
  // shell task: it only reads I2C and copies scalars.
  Diag diag() const;
};
