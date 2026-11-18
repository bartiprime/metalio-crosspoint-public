#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// BATT6 (r165.2) — "the displayed percentage changed, so repaint the NUMBER".
//
// THE DEFECT THIS EXISTS FOR. BatteryStats tracks lastSoc_ for its own
// accounting, but nothing asked the panel to repaint when the DISPLAYED SOC
// moved: the charging glyph has an edge producer
// (src/display/BatteryIconPatch.cpp) and the number has none. BaseTheme / the
// theme status bars read powerManager.getBatteryPercentage() at PAINT time, so
// the digits moved only when some other event happened to paint the screen.
//
// WHY r165.2 REWROTE r165.1's DECISION. The first version compared the gauge
// against the value a DROVE paint had recorded, and demanded a non-zero epoch
// for that comparison to count. A surface that has no addressable box (the
// FreeInkUI header band, components/TitledHeaderBand.h) records only the
// VALUE -- and a whole-activity repaint of a screen whose pixels did not change
// does not drive, so `drove` stays false and the epoch stays 0 FOREVER. The
// value matched the gauge on every pass and could never be credited, so the
// producer asked for a whole-activity repaint on every pass. On glass:
//
//     12 x  soc paint=0 90%->90% repaint=fallback reason=unaddressable
//      0 x  soc paint=1
//
// Each request re-ran the activity paint; one of them consumed the armed
// return-to-Home clean (the ghosting the operator reported).
//
// THE RULE NOW. One record layout, two kinds of evidence:
//   * addressable box: the VALUE and the EPOCH of the paint that recorded it.
//     Equal value + landed epoch (epoch != 0) => the glass reads it.
//   * unaddressable record: the VALUE only. It is the number the last paint
//     COMPOSED, and a drive that was skipped means the frame already on the
//     glass was composed with that same value. Demanding an epoch from a
//     surface that can never produce one is the loop; the value is the strongest
//     claim that surface can make, and it is enough.
//
// CONVERGENCE. `Requests` records the (surface, value) a request was issued for
// AT ISSUE TIME -- never on a successful paint. A surface that cannot address
// the value therefore asks at most once per distinct value, forever, and a
// value that only the glass can settle costs zero requests. A 10 s DEVICE-WIDE
// floor (not per-surface: alternating surfaces must not each get their own)
// rate limit is the backstop under adversarial alternation.
//
// This header is deliberately free of Arduino / renderer / HAL dependencies so
// the properties are host-tested against this exact code
// (test/batt_percent_change/).
// ---------------------------------------------------------------------------
namespace battpct {

enum class Need : uint8_t {
  None,      // nothing owed: no number on this screen, or the glass reads it
  Patch,     // repaint the number's own box, in place, now
  Pending,   // owed, but a guard, a clean or an epoch holds it: retry next pass
  Deferred,  // the value differs and this path cannot draw it: ask ONCE for a
             // whole-activity repaint, at the next loop boundary
};

// The terms, all POSITIVE evidence.
struct View {
  bool hasRect = false;        // a paint recorded the percentage on this screen
  bool rectPlausible = false;  // ... and that record carries a drawable box
  bool sameWidth = true;       // ... and the new text is exactly as wide as the old
  int16_t shownValue = -1;     // the percentage that record was painted with
  uint32_t rectEpoch = 0;      // compose epoch of the DROVE paint (0 = never drove)
  uint32_t glassEpoch = 0;     // the epoch the panel is holding
  bool canvasIsGlass = false;  // "no composition since the last landed drive"
  bool guardsClear = true;     // no one-shot refresh / entry gate / paint in flight
  bool cleanArmed = false;     // a forced clean is armed or is being consumed
  bool transition = false;     // a screen transition is in flight
};

inline Need need(const View& v, const uint16_t soc) {
  // No record: this screen draws no percentage at all (a full-screen image, the
  // sleep chip, the operator hid the number). Nothing can go stale here, and in
  // particular this must not turn every percent step into a whole-screen repaint.
  if (!v.hasRect) return Need::None;

  const bool sameValue = v.shownValue == static_cast<int16_t>(soc);

  // An ADDRESSABLE box that a LANDED paint recorded with this value: the glass
  // reads it. (epoch 0 means the frame the box belongs to never reached the
  // panel, so its value is not a claim about the glass -- see below.)
  if (v.rectPlausible && v.rectEpoch != 0 && sameValue) return Need::None;

  // An UNADDRESSABLE record whose value already IS the gauge's value: the last
  // paint composed this number and either drove it or was skipped as identical
  // to the frame on the glass. There is no epoch for this surface to earn, so
  // this is the whole of its evidence -- and it is sufficient. This single line
  // is what ends the r165.1 loop.
  if (!v.rectPlausible && sameValue) return Need::None;

  // A clean or a transition is in flight: its own repaint draws the live value,
  // so asking for another one is at best redundant and at worst (observed) eats
  // the armed wake clean. Wait for it.
  if (v.cleanArmed || v.transition) return Need::Pending;

  // Addressable, and the text is the same width: repaint the box in place, but
  // only while the canvas IS the frame on the glass and the box was recorded at
  // that frame. Anything else waits for the paint that will make it true.
  if (v.rectPlausible && v.sameWidth) {
    if (!v.guardsClear) return Need::Pending;
    if (!v.canvasIsGlass) return Need::Pending;
    if (v.rectEpoch == 0 || v.rectEpoch != v.glassEpoch) return Need::Pending;
    return Need::Patch;
  }

  // Recorded, but not this path's to draw: no box at all, a box that cannot be
  // on this panel, or a label whose WIDTH changes (a digit count changed, so the
  // cluster must be re-laid out). One request per value, never one per pass.
  return Need::Deferred;
}

// The producer's ONLY memory: which (surface, value) it has already ASKED for.
//
// Updated at ISSUE, not on a successful paint. That is the difference between
// "one request per distinct value" (bounded, converges) and "one request per
// pass until the glass agrees" (the r165.1 loop: the glass could never agree on
// a surface with no epoch to give).
class Requests {
 public:
  // The backstop under adversarial alternation (90 <-> 91 every pass), and the
  // floor between two real requests on one surface.
  static constexpr uint32_t kMinIntervalMs = 10000;

  // True exactly once per (surface, value), and at most once per kMinIntervalMs
  // ON THE WHOLE DEVICE -- the interval is deliberately not per-surface.
  bool take(const uint32_t surface, const uint16_t soc, const uint32_t nowMs) {
    if (surface != surface_) {
      // A different surface is a different obligation, so forget the VALUE it
      // asked for. The interval is NOT forgotten: a screen that alternates with
      // another every pass (90 <-> 91 across two surfaces) must not reset the
      // floor on each switch, or the backstop degenerates to one request per
      // pass -- which is the rate this class exists to bound.
      surface_ = surface;
      hasTaken_ = false;
    }
    if (hasTaken_ && value_ == soc) return false;
    if (timeSet_ && (nowMs - lastMs_) < kMinIntervalMs) return false;
    hasTaken_ = true;
    value_ = soc;
    lastMs_ = nowMs;
    timeSet_ = true;
    ++issued_;
    return true;
  }
  uint32_t issued() const { return issued_; }

 private:
  bool hasTaken_ = false;
  uint32_t surface_ = 0;
  uint16_t value_ = 0;
  bool timeSet_ = false;
  uint32_t lastMs_ = 0;
  uint32_t issued_ = 0;
};

}  // namespace battpct
