#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// BATT5 (CP_ICON_SAFE_PATCH) — the positive "this canvas IS the frame on the
// glass" fact, and the gate that decides whether a battery-icon region patch may
// be driven into it.
//
// WHY A FACT AND NOT AN INFERENCE
//   The retired r98 self-repaint (CP_ICON_PARTIAL_REPAINT, see
//   src/display/BatteryIconRegion.cpp) licensed a region patch from a RECORDED
//   RECT. A recorded rect says what a paint DREW, not what the panel shows: a
//   paint the entry gate held, a paint an activity declined, a paint whose drive
//   failed, and a grey-plane pass that restores the BW canvas all leave the
//   canvas describing a frame the glass has never had. Driving a region of that
//   canvas is the corner-only blank screen that retired that path.
//
//   So the licence here is an equality between two counters:
//     composed  bumped by EVERY canvas mutation (GfxRenderer::noteFramebufferWrite(),
//               called at every write sink -- grep the name)
//     glass     assigned from `composed` at the one place the code knows a drive
//               LANDED (result == 0 after the rail verdict; the driver re-seeds its
//               differential base, back_fb = front_fb, only on success:
//               idf/components/metalio_driver/display/highlevel.c:136)
//
//   Nothing else ever writes `glass`, and every path that can lose a drive
//   INVALIDATES the canvas instead (noteDriveFailed() advances `composed`), so the
//   equality reads false until a later drive actually lands.
//
// THE DIRECTION THAT MATTERS is a false EQUAL: it would let a patch run over a
// canvas that is not on the glass. It is excluded by construction, not by a
// convention -- `glass` is only ever assigned from `composed`, in the same
// statement that accepts the driver's success, so equality means literally "no
// mutation has been recorded since the last accepted drive". A mutation that is
// not recorded is the one way to break that, which is why the bump sits at every
// write sink in the renderer (and why the three call sites outside it that write
// through getFrameBuffer() call noteFramebufferWrite() themselves).
//
// A stale `glass` (a drive that happened below the renderer, e.g. a direct
// display.displayBuffer() from a path that does not report back) can only make
// the equality FALSE: harmless, it falls back to the whole-activity repaint.
//
// NO ARDUINO / ESP-IDF DEPENDENCY, on purpose: the arithmetic and the gate are
// unit-tested on the host in test/batt_icon_glass_epoch/.
// ---------------------------------------------------------------------------
namespace cpglass {

struct Epochs {
  uint32_t composed = 0;      // every canvas mutation, ever
  uint32_t glass = 0;         // `composed` at the last drive the driver accepted
  bool glassKnown = false;    // false until a drive has landed since boot / since the
                              // last failed drive: nothing has ever been put on the
                              // panel, so 0 == 0 must NOT read as "equal".

  // A canvas mutation was recorded: the canvas may no longer be the frame the
  // panel is showing.
  void noteCanvasWrite() { ++composed; }

  // A drive was ACCEPTED by the driver (draw error 0 and the rail verdict did not
  // disagree): the driver re-seeded back_fb from the canvas, so the two now hold
  // the same bytes and the panel is showing them.
  void noteDriveLanded() {
    glass = composed;
    glassKnown = true;
  }

  // A drive was attempted and did NOT land. The panel may be half-driven and the
  // canvas is known-not-on-glass. Clearing `glassKnown` and advancing `composed`
  // (rather than leaving the counters alone) is what makes this fail CLOSED: a
  // failed drive over an unchanged canvas would otherwise still read as "equal".
  void noteDriveFailed() {
    ++composed;
    glassKnown = false;
  }

  bool canvasIsGlass() const { return glassKnown && composed == glass; }
};

// Why the region patch was refused. None == licensed.
enum class Refusal : uint8_t {
  None = 0,                // licensed: patch and drive
  NoRect,                  // the last paint drew no battery glyph: nothing to patch
  BadRect,                 // the recorded rect is not plausible on this geometry
  CanvasNotGlass,          // the canvas has been composed since the last landed drive
  RectNotFromGlassFrame,   // the rect was recorded by a paint the glass never took
  GuardArmed,              // a one-shot refresh, the entry gate, or a paint in flight
};

// Every field is POSITIVE evidence; the gate is a conjunction of all of them.
struct Licence {
  bool canvasIsGlass = false;
  bool hasRect = false;
  bool rectPlausible = false;
  uint32_t rectEpoch = 0;   // `composed` when the rect was recorded
  uint32_t glassEpoch = 0;  // `glass`
  bool guardsClear = false;
};

inline bool licensed(const Licence& l) {
  return l.canvasIsGlass && l.hasRect && l.rectPlausible && l.rectEpoch == l.glassEpoch && l.guardsClear;
}

// The first failing term, in the order a reader would check them, for the log.
inline Refusal refusal(const Licence& l) {
  if (!l.hasRect) return Refusal::NoRect;
  if (!l.rectPlausible) return Refusal::BadRect;
  if (!l.canvasIsGlass) return Refusal::CanvasNotGlass;
  if (l.rectEpoch != l.glassEpoch) return Refusal::RectNotFromGlassFrame;
  if (!l.guardsClear) return Refusal::GuardArmed;
  return Refusal::None;
}

// `true` when the screen cannot be served by a rect patch at all, i.e. the only
// producer left is a whole-activity repaint. Everything else is a temporary
// refusal the next loop iteration retries.
inline bool needsWholeActivityRepaint(const Refusal r) {
  return r == Refusal::NoRect || r == Refusal::BadRect;
}

inline const char* refusalName(const Refusal r) {
  switch (r) {
    case Refusal::None: return "licensed";
    case Refusal::NoRect: return "no-rect";
    case Refusal::BadRect: return "bad-rect";
    case Refusal::CanvasNotGlass: return "canvas-not-glass";
    case Refusal::RectNotFromGlassFrame: return "rect-not-from-glass-frame";
    case Refusal::GuardArmed: return "guard-armed";
  }
  return "?";
}

}  // namespace cpglass
