#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// FL1 — side-edge drag for the frontlight, as a pure state machine.
//
// THE GESTURE
//   Drag up or down along a side edge of the reading surface: the LEFT band
//   sets brightness, the RIGHT band sets warmth. Up raises, down lowers. The
//   value changes while the finger moves and is committed on the lift.
//
// WHY IT IS A STATE MACHINE AND NOT AN EDGE-SWIPE TEST
//   Every side-edge gesture the firmware already has is classified on RELEASE
//   (InputManager::wasSwipe / fui::edgeSwipe), so by the time one is recognised
//   the finger is gone and nothing can be dragged. This machine claims a
//   contact while it is still down, and the whole collision guard lives in the
//   three tests below -- none of which is a timing window:
//
//   1. WHERE THE CONTACT STARTS. Inside `bandPx` of a side edge, and inside the
//      vertical middle band [guardTopPct, guardBottomPct). The exclusions are
//      the owners of those bands:
//        * top 14%  -- the light-panel down-swipe (wasLightPanelGesture) and
//                      the reader-menu down-swipe (wasMenuGesture), both
//                      classified by fui::edgeSwipe(Top) as sy <= 14% of H;
//        * bottom 14% -- the Home up-swipe (wasHomeGesture) / the
//                      reader-menu up-swipe (wasReaderMenuSwipeUp);
//        * the whole width -- the page-turn tap zones, which are TAPS and are
//                      never claimed here (test 2/3).
//
//   2. HOW FAR IT TRAVELS BEFORE IT IS CLAIMED. `activatePx` is the caller's
//      tap-release slop (InputManager's touchTapReleaseSlopPx = the configured
//      swipe threshold - 1): once the contact has moved that far from its
//      DOWN point the input layer has already latched it as "not a tap"
//      (touchMovedBeyondTapReleaseSlop), so a claimed drag can never also
//      deliver the tap that a page-turn zone acts on. Below that distance
//      nothing is claimed and the contact stays a plain tap/swipe.
//
//   3. WHICH AXIS MOVED. The drag must be VERTICALLY dominant (|dy| > |dx|).
//      A horizontally dominant contact is a back swipe / page sweep: the
//      machine drops it (`Abort`) the moment |dx| reaches `activatePx`, before
//      any value is touched. fui::edgeSwipe(Left/Right) is exactly that same
//      pair of conditions on the other side (adx > ady), so the two are
//      mutually exclusive by construction.
//
//   A tap that starts in the band and lifts: `watching` clears on the lift with
//   no value touched, so the tap zone sees it exactly as before.
//
// No firmware dependency: the machine is pure integer geometry, so the claim can
// be stated (and checked) against the input layer's own numbers without a
// device.
// ---------------------------------------------------------------------------

namespace edgedrag {

enum class Edge : int8_t { None = 0, Left = 1, Right = 2 };

enum class Action : uint8_t {
  None,    // nothing to do this frame
  Engage,  // the contact became a drag; the caller takes ownership of the frame
  Step,    // a new value this frame (repaint one in-place step)
  Commit,  // the finger lifted while dragging: persist
  Abort,   // the contact was dropped (tap, or a horizontal swipe) -- not ours
};

struct Config {
  int screenW = 0;
  int screenH = 0;
  // Half-width of the side band, px. Narrow on purpose: the band is a thumb's
  // reach from the bezel, well inside the SDK's 25%-of-width edge-swipe anchor,
  // and it must not extend into the page-turn tap zone's middle third.
  int bandPx = 44;
  int guardTopPct = 20;     // arm zone: % of height, top
  int guardBottomPct = 80;  // arm zone: % of height, bottom
  int activatePx = 90;      // travel before the drag is claimed (>= tap slop)
  int sweepPx = 380;        // vertical travel for one full value sweep
  // Value band of the edge being watched; the caller fills these from its own
  // target (brightness is 1..100, warmth 0..100) once `State::edge` is known.
  int valueMin = 0;
  int valueMax = 100;
  int currentValue = 0;
};

struct State {
  Edge edge = Edge::None;
  bool watching = false;  // a contact started in a band and is being measured
  bool active = false;    // claimed: the drag owns the value
  int downX = 0;
  int downY = 0;
  int anchorY = 0;    // y at the moment of the claim
  int anchorValue = 0;
  int value = 0;      // the last value reported by Step
};

inline void reset(State& s) {
  s.edge = Edge::None;
  s.watching = false;
  s.active = false;
  s.downX = 0;
  s.downY = 0;
  s.anchorY = 0;
  s.anchorValue = 0;
  s.value = 0;
}

// Which edge, if any, a contact at (x, y) belongs to. Edge::None means the
// contact is outside the arm zone and this gesture does not exist for it.
inline Edge edgeAt(const Config& c, const int x, const int y) {
  if (c.screenW <= 0 || c.screenH <= 0 || c.bandPx <= 0) return Edge::None;
  if (y < c.screenH * c.guardTopPct / 100) return Edge::None;
  if (y >= c.screenH * c.guardBottomPct / 100) return Edge::None;
  if (x < c.bandPx) return Edge::Left;
  if (x >= c.screenW - c.bandPx) return Edge::Right;
  return Edge::None;
}

// One frame. `down` is the press edge, `held` the live contact, (x, y) the
// current contact position in logical screen coordinates. `c.currentValue` is
// the value of the watched edge at the moment of the claim.
inline Action update(State& s, const Config& c, const bool down, const bool held, const int x, const int y) {
  if (!s.active) {
    if (!s.watching) {
      if (!down || !held) return Action::None;
      const Edge e = edgeAt(c, x, y);
      if (e == Edge::None) return Action::None;
      s.watching = true;
      s.edge = e;
      s.downX = x;
      s.downY = y;
      return Action::None;
    }
    if (!held) {  // lifted before it ever became a drag: a tap, not ours
      reset(s);
      return Action::Abort;
    }
    const int dx = x - s.downX;
    const int dy = y - s.downY;
    const int adx = dx < 0 ? -dx : dx;
    const int ady = dy < 0 ? -dy : dy;
    if (ady >= c.activatePx && ady > adx) {
      s.active = true;
      s.anchorY = y;
      s.anchorValue = c.currentValue;
      s.value = c.currentValue;
      return Action::Engage;
    }
    if (adx >= c.activatePx) {  // horizontally dominant: a back/paging swipe
      reset(s);
      return Action::Abort;
    }
    return Action::None;
  }

  if (!held) {  // the lift commits; the value is already the one on the glass
    const int value = s.value;
    const Edge edge = s.edge;
    reset(s);
    // Commit still names WHICH value to persist and the number to persist: the
    // caller writes the settings file here and nowhere else.
    s.value = value;
    s.edge = edge;
    return Action::Commit;
  }

  const int range = c.valueMax - c.valueMin;
  int next = s.anchorValue;
  if (range > 0 && c.sweepPx > 0) next += ((s.anchorY - y) * range) / c.sweepPx;
  if (next < c.valueMin) next = c.valueMin;
  if (next > c.valueMax) next = c.valueMax;
  if (next == s.value) return Action::None;
  s.value = next;
  return Action::Step;
}

}  // namespace edgedrag
