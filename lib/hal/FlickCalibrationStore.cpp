// Persistence for the optional flick calibration (NVS, namespace "cptilt").
//
// Stored in milli-g so the format is integer and exact; the version byte lets a
// future format change be ignored rather than misread. Everything is written
// only by a capture the maths accepted -- HalTiltSensor::saveCalibration() is
// the one caller and it refuses an invalid Calibration.

#include <Logging.h>
#include <Preferences.h>

#include "HalTiltSensor.h"

namespace {

constexpr const char* NVS_NAMESPACE = "cptilt";
constexpr uint8_t kVersion = 1;
constexpr uint16_t kG = 1000;  // milli-g per g

uint16_t toMilliG(float g) {
  if (g < 0.0f) return 0;
  const float mg = g * kG + 0.5f;
  return mg > 65535.0f ? 65535 : static_cast<uint16_t>(mg);
}

float fromMilliG(uint16_t mg) { return static_cast<float>(mg) / kG; }

}  // namespace

namespace tiltcalstore {

bool load(flickcal::Calibration& out) {
  Preferences prefs;
  // Read-write open: a read-only open of a namespace that does not exist yet
  // logs a NOT_FOUND that is not a fault (same reasoning as HybridClock).
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  const uint8_t ver = prefs.getUChar("ver", 0);
  flickcal::Calibration c;
  c.triggerG = fromMilliG(prefs.getUShort("trg", 0));
  c.startG = fromMilliG(prefs.getUShort("strt", 0));
  c.nextSign = static_cast<int8_t>(prefs.getChar("nxt", flick::kDefaultNextSign));
  c.leftPeakG = fromMilliG(prefs.getUShort("lpk", 0));
  c.rightPeakG = fromMilliG(prefs.getUShort("rpk", 0));
  c.noiseG = fromMilliG(prefs.getUShort("nse", 0));
  prefs.end();

  if (ver != kVersion) return false;
  // Anything outside these bounds never came from derive(); ignore it rather
  // than run the detector on a corrupt threshold.
  if (c.triggerG < 0.10f || c.triggerG > 1.20f) return false;
  if (c.startG <= 0.0f || c.startG > c.triggerG) return false;
  if (c.nextSign != 1 && c.nextSign != -1) c.nextSign = flick::kDefaultNextSign;
  c.valid = true;
  out = c;
  return true;
}

bool save(const flickcal::Calibration& cal) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  const bool ok = prefs.putUChar("ver", kVersion) > 0 && prefs.putUShort("trg", toMilliG(cal.triggerG)) > 0 &&
                  prefs.putUShort("strt", toMilliG(cal.startG)) > 0 &&
                  prefs.putChar("nxt", static_cast<int8_t>(cal.nextSign)) > 0 &&
                  prefs.putUShort("lpk", toMilliG(cal.leftPeakG)) > 0 &&
                  prefs.putUShort("rpk", toMilliG(cal.rightPeakG)) > 0 &&
                  prefs.putUShort("nse", toMilliG(cal.noiseG)) > 0;
  prefs.end();
  return ok;
}

void clear() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.clear();
  prefs.end();
}

}  // namespace tiltcalstore
