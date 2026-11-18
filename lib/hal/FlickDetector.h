#pragma once

// x-axis flick page turn: the whole decision chain (posture tracker, lateral
// peak window, onset direction, refractory) with no I2C, no millis() and no
// allocation, so it can be replayed sample by sample on the host
// (test/imu_flick). HalTiltSensor feeds it accelerometer samples and asks
// "did the reader just flick, and in which direction".
//
// The gesture is a short lateral flick, not a held angle. A flick is a
// transient: the hand accelerates the device sideways for ~60-150 ms and stops
// it. So the signal is the dynamic (rest-relative) component of the
// acceleration along the content's horizontal axis, its peak, and how fast
// that peak arrives -- and a gesture only counts if the signal has come back
// to rest before the window closes. An angle held for any length of time, a
// slow roll into a new posture, a pick-up or a set-down all fail at least one
// of those tests.
//
// The board's SC7A20H is an accelerometer only: units are g, and everything is
// expressed in the device frame Imu::read() normalises to (+X = screen right,
// +Y = screen up, +Z = out of the screen). The mount correction
// (SensorsConfig::imuSwapXY/FlipX/FlipY) is applied there, so this file only
// has to know where the content's horizontal axis points for each orientation.

#include <cmath>
#include <cstdint>

namespace flick {

// Matches CrossPointOrientation -- the direction the content's top edge faces.
enum class Orientation : uint8_t { Portrait = 0, LandscapeCw = 1, Inverted = 2, LandscapeCcw = 3 };

// Matches CrossPointTiltPageTurn.
enum class Mode : uint8_t { Off = 0, Normal = 1, Inverted = 2 };

enum class Event : uint8_t { None, Next, Prev };

// THE DIRECTION CONSTANT. Sign of the lateral acceleration that means NEXT.
// A flick to the right starts by pushing the device right, and the
// accelerometer reports the APPLIED force, so the onset lobe is positive on
// +X. Flip this (or let calibration derive it) if a unit turns pages the
// wrong way for the same motion.
inline constexpr int8_t kDefaultNextSign = +1;

// Which accelerometer component is the content's horizontal axis, and with
// what sign. right = up x out for a right-handed screen frame.
struct Axis {
  uint8_t index;  // 0 = x, 1 = y, 2 = z
  float sign;
};

inline Axis screenRight(Orientation o) {
  switch (o) {
    case Orientation::Inverted:
      return {0, -1.0f};  // screen up = -Y
    case Orientation::LandscapeCw:
      return {1, -1.0f};  // screen up = +X
    case Orientation::LandscapeCcw:
      return {1, +1.0f};  // screen up = -X
    case Orientation::Portrait:
    default:
      return {0, +1.0f};  // screen up = +Y
  }
}

// Defaults are deliberately set so that a deliberate flick clears them by a
// wide margin and ordinary handling does not come close. Calibration replaces
// triggerG/startG and nextSign per unit; everything else is structural.
struct Config {
  // |lateral| that opens a candidate window: any real sideways motion.
  float startG = 0.15f;
  // Peak |lateral| a flick must reach. A deliberate flick is 0.6-1.2 g.
  float triggerG = 0.40f;
  // |lateral| considered back at rest. A flick returns here in < 200 ms.
  float releaseG = 0.07f;
  // The peak must arrive this soon after the window opened: this is what
  // separates a flick (fast onset, ~60-120 ms) from a roll (slow ramp).
  uint32_t riseMs = 120;
  // ... and the signal must fall back to rest this soon AFTER the peak. A
  // flick stops dead (60-120 ms). A posture change decays only as fast as the
  // baseline tracker follows it (~250 ms tau, so 300-450 ms), which is what
  // keeps a fast 15-25 deg tilt from paging.
  uint32_t decayMs = 200;
  // Longest a candidate window may stay open. A signal still moving after
  // this is handling, not a flick.
  uint32_t windowMs = 300;
  // The signal must sit inside the release band this long before the window
  // is judged. A flick's own two lobes cross zero in the middle; without a
  // settle time that zero would close the window early and the second lobe
  // would be read as a flick in the opposite direction.
  uint32_t settleMs = 40;
  // One gesture, one page: nothing new may fire for this long after a fire or
  // a disturbance.
  uint32_t refractoryMs = 500;
  // Posture tracker on each axis. Fast enough to follow a slow roll into a new
  // resting angle (a 20 deg/s roll leaves < 0.04 g behind) and slow enough not
  // to eat the flick's own 130 ms lobe.
  float baselineTauMs = 250.0f;
  // Noise smoothing on the raw sample. 8 ms keeps a 60 ms lobe's peak.
  float filterTauMs = 8.0f;
  // The non-lateral dynamic component must stay below perpRatio * |peak|: a
  // flick is sideways, a pick-up is mostly vertical.
  float perpRatio = 1.0f;
  // | |a| - 1 | above this is an impact (a drop, a knock on a table).
  float impactG = 0.95f;
  // A sample gap this long breaks continuity; restart from scratch.
  uint32_t resetGapMs = 200;
  // Direction mapping: the onset sign that means NEXT. Calibration replaces
  // this per unit when it has one; the Mode setting flips it.
  int8_t nextSign = kDefaultNextSign;
};

struct State {
  bool primed = false;
  float fx = 0, fy = 0, fz = 0;  // low-passed acceleration, g
  float bx = 0, by = 0, bz = 0;  // posture baseline (slow), g

  bool open = false;        // a candidate window is collecting a peak
  uint32_t openMs = 0;      // window age
  uint32_t restMs = 0;      // consecutive time inside the release band
  float onsetSign = 0;      // sign of the first sample above startG = travel direction
  float peakG = 0;          // signed lateral peak inside the window
  uint32_t peakMs = 0;      // window age when |peakG| was last seen
  float peakPerp = 0;       // largest non-lateral dynamic component

  bool armed = true;    // a new window may open
  uint32_t holdMs = 0;  // refractory countdown

  // Diagnostics (the `tilt` shell command and the TILT log line).
  float latG = 0;        // last lateral dynamic component
  float perpG = 0;       // last non-lateral dynamic component
  float lastPeakG = 0;   // |peak| of the last fired gesture
  int8_t lastSign = 0;   // onset sign of the last fired gesture
  uint32_t fires = 0;
};

// One sample in, at most one event out. dtMs is the time since the previous
// sample; the caller owns the clock so traces are exact.
inline Event step(State& s, const Config& c, float ax, float ay, float az, uint32_t dtMs, bool touchActive,
                  Orientation o, Mode m) {
  if (m == Mode::Off) {
    s = State{};
    return Event::None;
  }
  if (dtMs == 0) return Event::None;
  if (dtMs > c.resetGapMs) {
    const bool armed = s.armed;
    const uint32_t fires = s.fires;
    const float lastPeak = s.lastPeakG;
    const int8_t lastSign = s.lastSign;
    s = State{};
    s.armed = armed;
    s.fires = fires;
    s.lastPeakG = lastPeak;
    s.lastSign = lastSign;
  }

  // --- 1. smoothing and posture baseline --------------------------------
  if (!s.primed) {
    s.fx = ax;
    s.fy = ay;
    s.fz = az;
    s.bx = ax;
    s.by = ay;
    s.bz = az;
    s.primed = true;
  } else {
    const float a = 1.0f - expf(-static_cast<float>(dtMs) / c.filterTauMs);
    s.fx += a * (ax - s.fx);
    s.fy += a * (ay - s.fy);
    s.fz += a * (az - s.fz);
    const float b = 1.0f - expf(-static_cast<float>(dtMs) / c.baselineTauMs);
    s.bx += b * (s.fx - s.bx);
    s.by += b * (s.fy - s.by);
    s.bz += b * (s.fz - s.bz);
  }

  // --- 2. dynamic component, split along / across the flick axis ---------
  const float dx = s.fx - s.bx, dy = s.fy - s.by, dz = s.fz - s.bz;
  const Axis axis = screenRight(o);
  const float comp[3] = {dx, dy, dz};
  const float lat = comp[axis.index] * axis.sign;
  const float dynMag = sqrtf(dx * dx + dy * dy + dz * dz);
  const float perp2 = dynMag * dynMag - lat * lat;
  const float perp = perp2 > 0.0f ? sqrtf(perp2) : 0.0f;
  s.latG = lat;
  s.perpG = perp;

  const float mag = sqrtf(s.fx * s.fx + s.fy * s.fy + s.fz * s.fz);
  const bool impact = fabsf(mag - 1.0f) > c.impactG;

  if (s.holdMs > 0) s.holdMs = (dtMs >= s.holdMs) ? 0 : s.holdMs - dtMs;

  // --- 3. anything that cannot be a gesture -----------------------------
  // A finger on the glass, or an impact: drop the window and make the device
  // come back to rest before a new one may open.
  if (touchActive || impact) {
    s.open = false;
    s.armed = false;
    s.holdMs = c.refractoryMs;
    return Event::None;
  }

  // --- 4. candidate window ----------------------------------------------
  const float alat = fabsf(lat);
  if (!s.open) {
    if (s.armed && s.holdMs == 0 && alat >= c.startG) {
      s.open = true;
      s.openMs = 0;
      s.restMs = 0;
      s.onsetSign = lat > 0.0f ? 1.0f : -1.0f;
      s.peakG = lat;
      s.peakMs = 0;
      s.peakPerp = perp;
    }
  } else {
    s.openMs += dtMs;
    if (alat > fabsf(s.peakG)) {
      s.peakG = lat;
      s.peakMs = s.openMs;
    }
    if (perp > s.peakPerp) s.peakPerp = perp;
    // Rest has to PERSIST: a flick's two lobes cross zero in the middle.
    if (alat <= c.releaseG) {
      s.restMs += dtMs;
    } else {
      s.restMs = 0;
    }

    // FIRE ONLY ON THE WAY BACK TO REST. A flick is a transient: the signal
    // must have settled inside the release band before the window closes.
    // Motion that is still there at windowMs is the device being carried.
    const bool settled = s.restMs >= c.settleMs;
    const bool expired = s.openMs >= c.windowMs;
    if (settled || expired) {
      s.open = false;
      if (settled) {
        const float peakAbs = fabsf(s.peakG);
        const bool strong = peakAbs >= c.triggerG;
        const bool fast = s.peakMs <= c.riseMs;
        const bool stopped = (s.openMs - s.peakMs) <= c.decayMs;
        const bool axial = s.peakPerp <= c.perpRatio * peakAbs;
        if (strong && fast && stopped && axial) {
          s.armed = false;
          s.holdMs = c.refractoryMs;
          s.lastPeakG = peakAbs;
          s.lastSign = s.onsetSign > 0.0f ? 1 : -1;
          ++s.fires;
          // Direction comes from the ONSET lobe (the direction the device was
          // pushed), not from whichever lobe peaked higher: the stop is often
          // sharper than the start, which would invert half the flicks.
          const int8_t nextSign = m == Mode::Inverted ? static_cast<int8_t>(-c.nextSign) : c.nextSign;
          return (s.lastSign == nextSign) ? Event::Next : Event::Prev;
        }
      } else {
        // Sustained lateral motion: handling, not a gesture.
        s.armed = false;
        s.holdMs = c.refractoryMs;
      }
    }
  }

  // --- 5. re-arm only from rest -----------------------------------------
  if (!s.armed && s.holdMs == 0 && alat <= c.releaseG) {
    s.armed = true;
  }
  return Event::None;
}

}  // namespace flick
