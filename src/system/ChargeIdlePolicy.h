#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// r163 3.5 -- CHARGE-AWARE IDLE POLICY, expressed as an E6 need.
//
// WHAT CHANGES. With the policy ON and the pack actually CHARGING on a cable, an
// `ChargerHost` HARD need is held (src/system/PowerNeed.h). The only consequence
// of a hard need is that AUTOMATIC (inactivity) standby does not fire. So:
//
//   on charge, policy ON   the ordinary idle timer does not put the device to
//                          standby; the screen the user left is still there
//   on charge, policy OFF  byte for byte today's behaviour (default)
//   on battery             byte for byte today's behaviour, always
//
// WHAT DELIBERATELY DOES NOT CHANGE:
//   * A MANUAL sleep still sleeps. The need gates the auto-sleep decision only;
//     the power key, the short-press binding and the >2.2 s power-off hold are
//     untouched. (A hard need is read BEFORE enterDeepSleep() is called, never
//     inside it.)
//   * The park/resume sequence, the rail order, the settle constants and the
//     park journal are untouched -- no need is acquired or released inside
//     metalioStandbySleep().
//   * Standby auto-shutdown (standbyAutoOffMinutes) and the battery icon path are
//     untouched.
//
// WHY "CHARGING" AND NOT "CABLE". isCharging() is the pack's current > +5 mA
// verdict (HalPowerManager, CX25601N REG 0x1E). A pack at 100 % on a cable is an
// ordinary idle device again, so the hold releases and the user's own timeout
// applies. A hold that followed the CABLE instead would leave a finished device
// awake for as long as someone left it plugged in, which is the surprising half
// of the trade.
//
// THE DEFAULT IS OFF. The configured sleep timeout is the user's own policy;
// changing its meaning because a cable appeared is the surprise. OFF means this
// whole file is inert, which is also what makes the change safe to ship before
// anyone evaluates it on glass.
// ---------------------------------------------------------------------------

namespace chargepolicy {

// CrossPointSettings::chargeIdlePolicy values. Persisted as a number so the
// generic settings walk and the backup sanitizer both carry it.
enum class Mode : uint8_t {
  FollowTimeout = 0,  // OFF: charging does not change when the device idles to sleep
  KeepAwake = 1,      // ON:  an auto-idle standby is suppressed while charging
};

constexpr Mode modeOf(const uint8_t stored) { return stored == 1 ? Mode::KeepAwake : Mode::FollowTimeout; }

// The whole policy, as arithmetic: should the charging hold be in place?
//
// `charging` is HalPowerManager::isCharging() -- the RAW verdict, not the
// displayed one (the glyph hides the bolt at 100 %; the policy must follow the
// pack, not the glyph).
constexpr bool holdWhileCharging(const uint8_t stored, const bool charging) {
  return modeOf(stored) == Mode::KeepAwake && charging;
}

}  // namespace chargepolicy
