#pragma once

#include <Arduino.h>
#include <atomic>
#include <cstdint>

#include <BoardConfig.h>
#include <HalHaptics.h>
#include <Logging.h>

#include "CrossPointSettings.h"

// The Metalio Plus hangs its vibration motor off the TCA9555 expander at P0.3,
// not a GPIO, so HalHaptics' LEDC path cannot drive it (BoardConfig has no haptic
// gpio for this board -- BoardConfig.h:2199-2202). The vendor driver's own pulse
// is the same one the panel shim already exposes:
//   idf/components/metalio_driver/metalio_periph.c:257  mn_haptic_pulse(ms)
//     -> epd_board_metalio_eink4_plus_motor_set(true) / delay(ms) / (false)
// and the hardware facts behind it are the vendor's:
//   Metalio-E-INK4-Plus/main/boards/metalio-e-ink4-plus/config.h:68  EPD_IO_MOTOR 3 (P03, active high)
//   Metalio-E-INK4-Plus/main/boards/metalio-e-ink4-plus/config.h:72  VIBRATION_MOTOR_PULSE_MS 50
//   Metalio-E-INK4-Plus/main/boards/metalio-e-ink4-plus/input/metalio_vibe.cc:16-17
//     strength = soft-PWM duty {0, 14000, 18000, 20000} us over a 20 ms period
//   (r90 note: that table is the vendor's, and it only expresses itself in their
//   CONTINUOUS mode. Copied onto their 50 ms pulse it is imperceptible -- see
//   metalio_periph.c for the measurement and the fix.)
// Weak, so every board whose shim does not export it links with a null pointer
// and keeps the SDK path (same pattern as GfxRenderer's metalio panel hooks).
// EXCEPT on the Metalio boards: there the symbol is a STRONG reference. A weak
// one does not keep the definition alive -- with -ffunction-sections +
// --gc-sections the linker dropped mn_haptic_pulse out of the image (checked on
// the r88 ELF: nm showed no mn_haptic_pulse), the call site became `jalr zero`
// and every tap pulsed nothing. The strong reference is what keeps it.
#if FREEINK_DEVICE_METALIO_EINK4 || FREEINK_DEVICE_METALIO_EINK4_PLUS
extern "C" void mn_haptic_pulse(int ms);
// Strength-carrying variant (r89). The vendor drives this motor with a soft-PWM
// duty, not a fixed width, so duration alone cannot express a setting:
//   Metalio-E-INK4-Plus/main/boards/metalio-e-ink4-plus/input/metalio_vibe.cc:16-17
//     VIBE_PWM_PERIOD_US 20000 (50 Hz), k_duty_on_us[4] = {0, 14000, 18000, 20000}
//   => level 1 = 70 % duty, level 2 = 90 %, level 3 = 100 % (held on).
// Implemented in idf/components/metalio_driver/metalio_periph.c. Declared here
// rather than in the shim header because the shim header is a vendored file.
extern "C" void mn_haptic_pulse_level(int ms, int level);
// HAPT2/HAPT3 (r101.9): exact-duration pulse. The level profiles above carry a per-level
// duration FLOOR (60 / 90 / 120 ms) so a short tap stays perceptible, which would silently
// stretch a pattern that NAMES its duration (the power-off cue is 2 x 100 ms, the soft-key
// cue 70 ms). This one takes `ms` literally on the level-3 envelope (bit held high, 100 %
// duty) and applies no floor. Implemented next to mn_haptic_pulse_level() in
// idf/components/metalio_driver/metalio_periph.c.
extern "C" void mn_haptic_pulse_exact(int ms);
#else
extern "C" void mn_haptic_pulse(int ms) __attribute__((weak));
#endif

// Call only after the application accepts an interactive touch action.
// Sampling a contact or classifying a gesture must remain silent.
namespace haptic_feedback {
namespace detail {
// Vendor pulse widths. 50 ms is the vendor's short pulse (config.h:72), which on
// this motor is the tactile "click". A hold answers with a DOUBLE pulse so it is
// distinguishable without looking at the screen; the gap keeps the two apart on
// a pager motor that needs ~30 ms to spin up or down.
constexpr int kTapPulseMs = 50;
constexpr int kHoldPulseMs = 30;
constexpr int kHoldGapMs = 60;
// HAPT2 (r101.9): the power-off cue -- TWO pulses of 100 ms with a 100 ms gap. Settled with
// the operator on 2 Nov 2026; the 2 x 1 s spec drafted the same day was withdrawn because a
// 1 s pulse is far too long on this motor. Both pulses go through the exact-duration entry
// point, so 100 ms means 100 ms at every intensity.
constexpr int kPowerOffPulseMs = 100;
constexpr int kPowerOffGapMs = 100;
constexpr int kPowerOffPulses = 2;
// HAPT3 (r101.9) / r101.10: the soft-key cue -- ONE pulse, longer than the 50 ms tap so it is not
// mistaken for it, and single so it reads as neither the hold's double nor the power-off's slow
// double. 120 ms is the driver's own PERCEPTIBILITY FLOOR at level 3 (HapticFeedback.cpp:326 floors
// every level-3 pulse at 120 ms): the old 70 ms went through mn_haptic_pulse_exact(), which applies
// no floor by design, so the cue was measurably below what this motor can register as a click.
constexpr int kSoftKeyPulseMs = 120;
// r163 3.14: the ERROR cue -- THREE short pulses, i.e. the only pattern in this file that
// repeats more than twice. That is what makes it tell apart from the hold's double
// (2 x 30 ms / 60 ms gap) and the power-off's slow double (2 x 100 ms / 100 ms gap) on a
// pager motor that cannot express a fifth distinct SHAPE. 60 ms is the level-3
// perceptibility floor from HapticFeedback's own profile (HapticFeedback.cpp floors every
// level-3 pulse at 120 ms; this pattern instead goes through the exact-duration entry
// point, so 60 ms is 60 ms and the three ticks stay separate at every intensity).
constexpr int kErrorPulseMs = 60;
constexpr int kErrorGapMs = 90;
constexpr int kErrorPulses = 3;
}  // namespace detail

// Whether the pulse reaches real hardware. The expander motor is reachable on
// the Plus; everywhere else HalHaptics decides.
inline bool expanderMotor() {
#if FREEINK_CAP_HAPTIC && !CROSSPOINT_EMULATED
#if FREEINK_DEVICE_METALIO_EINK4 || FREEINK_DEVICE_METALIO_EINK4_PLUS
  // Strong symbol: it is in the image (see the declaration above).
  return !BoardConfig::hasHaptics();
#else
  return !BoardConfig::hasHaptics() && mn_haptic_pulse != nullptr;
#endif
#else
  return false;
#endif
}

namespace detail {
// THE SETTING, evaluated at FIRE time (never at arm time): a stashed pulse obeys
// the option as it stands when it fires, so flipping the row in Controls takes
// effect on the next action with no reboot.
//   VIBRATION_TOUCH       "On Touch"              -> actions that ARE a touch:
//                                                      taps, key presses, a
//                                                      recognised hold.
//   VIBRATION_TOUCH_PAGE  "On Touch + Page Turn"  -> the above AND page turns
//                                                      (bezel page keys, screen
//                                                      tap/swipe page turns).
//   VIBRATION_OFF         "Off"                   -> nothing, ever.
// A page turn is the one class the option separates, so it is the only thing
// pulseAllowed() discriminates -- it does not second-guess anything else.
inline bool pulseAllowed(const bool pageTurnOnly) {
  const uint8_t mode = SETTINGS.vibration;
  return mode == CrossPointSettings::VIBRATION_TOUCH_PAGE ||
         (mode == CrossPointSettings::VIBRATION_TOUCH && !pageTurnOnly);
}

// The strength a pulse is driven with, read from SETTINGS at FIRE time (never at arm time),
// so the Controls row applies without a restart. Three call sites need it now (tap/hold,
// power-off logging, soft key), so it lives here instead of being spelled out in each.
inline int hapticLevel() {
  return SETTINGS.hapticIntensity == CrossPointSettings::HAPTIC_LOW      ? 1
         : SETTINGS.hapticIntensity == CrossPointSettings::HAPTIC_MEDIUM ? 2
                                                                         : 3;
}
}  // namespace detail

inline void touchAction(bool longPress = false, bool pageTurnOnly = false) {
  const bool enabled = detail::pulseAllowed(pageTurnOnly);
  const bool expander = expanderMotor();
  const int level = detail::hapticLevel();
  // One line per decision, both ways, so a device session can prove WHICH pulse
  // fired (and on which motor path) and why one did not. LOG_DBG is compiled in
  // under CP_DEBUG_LOG and gated at RUNTIME by the Debug page toggle, so it costs
  // nothing when the debug switch is off.
  LOG_DBG("HAPTIC", "%s path=%s mode=%u page=%d long=%d level=%d", enabled ? "fire" : "skip",
          expander ? "expander" : "hal", static_cast<unsigned>(SETTINGS.vibration), pageTurnOnly ? 1 : 0,
          longPress ? 1 : 0, level);
  if (!enabled) return;
  // Expander motor first: its off-state is unconditional (the pulse helper writes
  // the motor bit low after the delay, and also returns early if the expander is
  // dead), so there is no path that leaves it energised.
  if (expander) {
    // Strength pairs a DUTY with a DURATION per level (r90). r89 carried only
    // the vendor's duty over the vendor's fixed 50 ms pulse and every level felt
    // identical: 50 ms is shorter than this motor's ~30 ms spin-up, and the 2 ms
    // off window at level 2 is not longer than a TCA9555 write. So the level now
    // selects a whole profile in metalio_periph.c (see the long note there):
    //   HAPTIC_LOW    -> level 1: 50 % duty (12 ms on / 12 ms off), >= 60 ms
    //   HAPTIC_MEDIUM -> level 2: 79 % duty (19 ms on /  5 ms off), >= 90 ms
    //   HAPTIC_HIGH   -> level 3: 100 %, bit held high,                    >= 120 ms
    // The `ms` passed below is a duration FLOOR; the profile may hold it longer.
    // Live: the level is read from SETTINGS on every pulse, so the Controls row
    // applies without a restart.
    if (longPress) {
      mn_haptic_pulse_level(detail::kHoldPulseMs, level);
      delay(detail::kHoldGapMs);
      mn_haptic_pulse_level(detail::kHoldPulseMs, level);
    } else {
      mn_haptic_pulse_level(detail::kTapPulseMs, level);
    }
    return;
  }
  // Other boards: HalHaptics owns the motor. It is still the only pulse path
  // there, so the policy above is applied before this point for every board.
  if (longPress) {
    HalHaptics::longPress(enabled, SETTINGS.hapticIntensity);
  } else {
    HalHaptics::feedback(enabled, true, SETTINGS.hapticIntensity);
  }
}

// TAP1: the DEFERRED pulse mailbox.
//
// touchAction() above BLOCKS. On this board mn_haptic_pulse_level() busy-waits the whole
// profile (>=60 ms LOW, >=90 ms MEDIUM, >=120 ms HIGH), and every call site of it runs on
// the MAIN task before the frame that tap acknowledged has been given to the render task --
// so on glass the buzz was felt and only then did the panel change. r99.1 fixed the central
// UiAppHost::route() path by stashing the pulse and firing it at the top of the next loop()
// iteration; this is that same mailbox, lifted into haptic_feedback so the remaining call
// sites (popup, home grid, reader page turns, keyboard, dictionary) can use it without
// pulling UiAppHost.h into every activity. UiAppHost's two statics now forward here, so
// there is still exactly ONE slot and ONE drain point (main.cpp loop()).
//
// One slot on purpose: a burst of taps before servicing coalesces into one buzz instead of
// a backlog. Process-wide (function-local statics of inline functions) on purpose too: the
// pulse must still fire if the activity that acknowledged the tap was replaced or destroyed
// before it is serviced.
//
// Order of the pair: longPress then pending-release, so a reader that sees pending==true is
// guaranteed to see the matching longPress bit. The third bit -- page-turn-only -- rides the
// same slot so the option has something to discriminate at fire time; the slot is not split
// into two queues because a burst of taps must still coalesce into one buzz.
namespace detail {
inline std::atomic<bool>& deferredPending() {
  static std::atomic<bool> v{false};
  return v;
}
inline std::atomic<bool>& deferredLongPress() {
  static std::atomic<bool> v{false};
  return v;
}
// True when the stashed action is a PAGE TURN and nothing else. Cleared by every
// deferTouchAction(): a real touch action outranks a page turn that landed in the same frame,
// so the pair still fires once and it is the touch that justifies it.
inline std::atomic<bool>& deferredPageTurnOnly() {
  static std::atomic<bool> v{false};
  return v;
}
// HAPT2/HAPT3 (r101.9): which PATTERN the one slot carries. kKindTouchOrPage = 0 keeps the
// pre-r101.9 meaning exactly (a touch or a page turn, discriminated by the two bits above), so
// every existing arm site is untouched apart from storing the tag. The two new kinds are
// added, not queued: still ONE slot, ONE drain point (loop() -> serviceDeferredTouchAction()),
// and a burst of them still coalesces into one pattern.
// r163 3.14: kKindError is the fourth class. It is a STEP FAILURE (a page turn the app
// accepted and could not perform), not a touch: it fires whichever vibration mode is set,
// and only VIBRATION_OFF silences it -- the same contract as the power-off cue.
enum DeferredKind : uint8_t { kKindTouchOrPage = 0, kKindPowerOff = 1, kKindSoftKey = 2, kKindError = 3 };
inline std::atomic<uint8_t>& deferredKind() {
  static std::atomic<uint8_t> v{kKindTouchOrPage};
  return v;
}
}  // namespace detail

inline void deferTouchAction(bool longPress = false) {
  // HAPT4 (r101.10): A STASHED POWER-OFF PATTERN IS NOT DISPLACEABLE. PowerOffActivity's confirm
  // callback stashes the 2 x 100 ms cue and OptionPopup then calls this for the same tap -- which
  // overwrote the one slot with kKindTouchOrPage, so on glass the operator got the 50 ms tap pulse
  // and never the power-off double (the confirm-BUTTON path has no such call, which is what made
  // this look like a hardware flake). The power-off cue is the last pulse of the session; a touch
  // that lands beside it must not swallow either pulse.
  if (detail::deferredPending().load(std::memory_order_acquire) &&
      detail::deferredKind().load(std::memory_order_relaxed) == detail::kKindPowerOff) {
    return;
  }
  detail::deferredKind().store(detail::kKindTouchOrPage, std::memory_order_relaxed);
  detail::deferredPageTurnOnly().store(false, std::memory_order_relaxed);
  detail::deferredLongPress().store(longPress, std::memory_order_relaxed);
  detail::deferredPending().store(true, std::memory_order_release);
}

// A PAGE TURN (bezel page key press, screen tap/swipe turn, manual queued turn). Same mailbox,
// same one drain point; only the class differs, so VIBRATION_TOUCH can stay silent on it while
// VIBRATION_TOUCH_PAGE buzzes. If a touch action is already stashed for this frame the slot keeps
// the TOUCH class: one pulse fires anyway, and a real touch must never be silenced because a page
// turn landed beside it.
inline void deferPageTurnAction(bool longPress = false) {
  if (detail::deferredPending().load(std::memory_order_acquire)) {
    if (longPress) detail::deferredLongPress().store(true, std::memory_order_relaxed);
    return;
  }
  detail::deferredKind().store(detail::kKindTouchOrPage, std::memory_order_relaxed);
  detail::deferredLongPress().store(longPress, std::memory_order_relaxed);
  detail::deferredPageTurnOnly().store(true, std::memory_order_relaxed);
  detail::deferredPending().store(true, std::memory_order_release);
}

// HAPT2 (r101.9): the POWER-OFF cue. TWO pulses of 100 ms, 100 ms apart.
//
// Why it is in this mailbox and not a delay() in the shutdown: metalioHardPowerOff() paints
// nothing and collapses the rail, and the whole r99.1/`TAP1` contract is that a pulse never
// runs on the stack that decided it. So the dialog STASHES the pattern here
// (PowerOffActivity::confirmPowerOff, after the last frame is on the glass) and the existing
// drain point at the top of loop() fires it -- which is above the `metalioPowerOffRequested()`
// check in the same iteration, so the order is: pulse, pulse, then the shutdown. No new
// delay(), no second queue, no change to metalioHardPowerOff().
//
// A power-off outranks anything else in the slot: it is the last pulse of the session, and a
// tap that landed beside the confirmation must not swallow it.
inline void deferPowerOffPattern() {
  detail::deferredLongPress().store(false, std::memory_order_relaxed);
  detail::deferredPageTurnOnly().store(false, std::memory_order_relaxed);
  detail::deferredKind().store(detail::kKindPowerOff, std::memory_order_relaxed);
  detail::deferredPending().store(true, std::memory_order_release);
}

// HAPT3 (r101.9): a SOFT KEY (the page keys under the screen; Home/Back reach it only where
// the tap feedback would be silent -- see MappedInputManager). Gated by SETTINGS.softKeyHaptic
// at BOTH ends: here, so the slot is not spent when the row is off, and again in
// softKeyPattern() at fire time, so a mid-frame flip of the row is honoured.
// Priority is the same shape as the page-turn rule: a real touch already stashed for this
// frame keeps the slot (one pulse fires either way), a power-off is never displaced.
inline void deferSoftKeyAction() {
  if (SETTINGS.softKeyHaptic == 0) return;
  if (detail::deferredPending().load(std::memory_order_acquire)) {
    const uint8_t kind = detail::deferredKind().load(std::memory_order_relaxed);
    if (kind == detail::kKindPowerOff) return;
    if (kind == detail::kKindTouchOrPage && !detail::deferredPageTurnOnly().load(std::memory_order_relaxed)) return;
  }
  detail::deferredLongPress().store(false, std::memory_order_relaxed);
  detail::deferredPageTurnOnly().store(false, std::memory_order_relaxed);
  detail::deferredKind().store(detail::kKindSoftKey, std::memory_order_relaxed);
  detail::deferredPending().store(true, std::memory_order_release);
}

// r163 3.14: the ERROR cue. Stashed by the reader when a page turn it accepted did not
// move the page (pageTurn()/skipPages() returned false), which is the one failure the
// operator otherwise learns only by looking away from the book and back.
//
// Priority: a stashed power-off is never displaced (it is the last pulse of the session);
// anything else in the slot is replaced, because an error is the more specific answer to
// "what just happened" than a tap pulse. Still ONE slot and ONE drain point.
inline void deferErrorAction() {
  if (detail::deferredPending().load(std::memory_order_acquire) &&
      detail::deferredKind().load(std::memory_order_relaxed) == detail::kKindPowerOff) {
    return;
  }
  detail::deferredLongPress().store(false, std::memory_order_relaxed);
  detail::deferredPageTurnOnly().store(false, std::memory_order_relaxed);
  detail::deferredKind().store(detail::kKindError, std::memory_order_relaxed);
  detail::deferredPending().store(true, std::memory_order_release);
}

// Fires the power-off pattern: kPowerOffPulses pulses of kPowerOffPulseMs, gap kPowerOffGapMs.
// Both pulses use mn_haptic_pulse_exact() so the duration is the number in the name (the level
// profiles would floor 100 ms to 120 at HIGH intensity). Called from the drain point only --
// never from a render path, never from the shutdown.
inline void powerOffPattern() {
  const bool expander = expanderMotor();
  // Same contract as every other pulse: VIBRATION_OFF means nothing, ever. Default on this
  // board is VIBRATION_TOUCH, so the cue fires out of the box.
  const bool enabled = SETTINGS.vibration != CrossPointSettings::VIBRATION_OFF;
  LOG_INF("HAPTIC", "%s power-off pattern (2 x %d ms, gap %d ms) path=%s mode=%u level=%d", enabled ? "fire" : "skip",
          detail::kPowerOffPulseMs, detail::kPowerOffGapMs, expander ? "expander" : "hal",
          static_cast<unsigned>(SETTINGS.vibration), detail::hapticLevel());
  if (!enabled) return;
  for (int i = 0; i < detail::kPowerOffPulses; ++i) {
    // The gap is the point of the pattern; nothing else provides it. Still at the drain point,
    // i.e. outside metalioHardPowerOff().
    if (i > 0) delay(detail::kPowerOffGapMs);
    // r101.10 HAPT4: one visible line PER PULSE, so "did both pulses actually happen" is answerable
    // from the log alone instead of from the glass.
    LOG_INF("HAPTIC", "power-off pulse %d/%d (%d ms)", i + 1, detail::kPowerOffPulses, detail::kPowerOffPulseMs);
    if (expander) {
      mn_haptic_pulse_exact(detail::kPowerOffPulseMs);
    } else {
      // Other boards: HalHaptics owns the motor and takes no duration, so the pulse width is
      // its own; only the count and the gap are expressed here.
      HalHaptics::feedback(true, true, SETTINGS.hapticIntensity);
    }
  }
}

// HAPT3: the soft-key cue -- ONE pulse of kSoftKeyPulseMs, exact. The toggle is re-read here,
// so the row as it stands when the pulse actually fires is what decides.
inline void softKeyPattern() {
  const bool expander = expanderMotor();
  const bool enabled = SETTINGS.softKeyHaptic != 0;
  // r101.10 HAPT5: LOG_INF, not LOG_DBG -- the operator judges the soft key on glass and this line
  // is how "the pattern fired at all" is proven without it.
  LOG_INF("HAPTIC", "%s soft-key pattern (1 x %d ms) path=%s softKey=%u mode=%u", enabled ? "fire" : "skip",
          detail::kSoftKeyPulseMs, expander ? "expander" : "hal", static_cast<unsigned>(SETTINGS.softKeyHaptic),
          static_cast<unsigned>(SETTINGS.vibration));
  if (!enabled) return;
  if (expander) {
    mn_haptic_pulse_exact(detail::kSoftKeyPulseMs);
  } else {
    HalHaptics::feedback(true, true, SETTINGS.hapticIntensity);
  }
}

// r163 3.14: fires the error cue -- kErrorPulses exact pulses of kErrorPulseMs. Read the
// vibration setting here, at FIRE time, like every other pattern.
inline void errorPattern() {
  const bool expander = expanderMotor();
  const bool enabled = SETTINGS.vibration != CrossPointSettings::VIBRATION_OFF;
  LOG_INF("HAPTIC", "%s error pattern (%d x %d ms, gap %d ms) path=%s mode=%u", enabled ? "fire" : "skip",
          detail::kErrorPulses, detail::kErrorPulseMs, detail::kErrorGapMs, expander ? "expander" : "hal",
          static_cast<unsigned>(SETTINGS.vibration));
  if (!enabled) return;
  for (int i = 0; i < detail::kErrorPulses; ++i) {
    if (i > 0) delay(detail::kErrorGapMs);
    if (expander) {
      mn_haptic_pulse_exact(detail::kErrorPulseMs);
    } else {
      HalHaptics::feedback(true, true, SETTINGS.hapticIntensity);
    }
  }
}

// Fires the stashed pattern at most once, then clears it. The vibration setting is read inside
// touchAction()/powerOffPattern(), and the soft-key toggle inside softKeyPattern(), so a stashed
// pulse obeys the settings as they stand when it actually fires.
inline void serviceDeferredTouchAction() {
  if (!detail::deferredPending().exchange(false, std::memory_order_acquire)) return;
  const uint8_t kind = detail::deferredKind().load(std::memory_order_relaxed);
  if (kind == detail::kKindPowerOff) {
    powerOffPattern();
    return;
  }
  if (kind == detail::kKindSoftKey) {
    softKeyPattern();
    return;
  }
  if (kind == detail::kKindError) {
    errorPattern();
    return;
  }
  touchAction(detail::deferredLongPress().load(std::memory_order_relaxed),
              detail::deferredPageTurnOnly().load(std::memory_order_relaxed));
}
}  // namespace haptic_feedback
