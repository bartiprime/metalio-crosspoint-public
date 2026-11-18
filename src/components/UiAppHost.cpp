#include <Logging.h>
#include "UiAppHost.h"

#include "UiAppHelpers.h"

namespace fui = freeink::ui;

// TAP1: the mailbox itself lives in haptic_feedback (HapticFeedback.h) so that activities
// and components can stash a pulse without including this header. These two are the
// UiAppHost-facing names for the SAME one-slot queue -- there is exactly one drain point,
// UiAppHost::serviceDeferredHaptic() at the top of main.cpp loop().
void UiAppHost::deferHaptic(const bool longPress) { haptic_feedback::deferTouchAction(longPress); }

void UiAppHost::serviceDeferredHaptic() { haptic_feedback::serviceDeferredTouchAction(); }

UiAppHost::UiAppHost(const GfxRenderer& renderer)
    : uiTarget(makeUiTarget(renderer)), app(uiTarget, uiTarget.deviceContext()), renderer_(renderer) {}

void UiAppHost::resetUi() {
  uiReady = false;
  // Screen entry: nothing has been published for THIS screen yet, so the first tap must wait for
  // the publish generation to advance (tapsAcceptable()).
  genAtEnter = publishGeneration_.load();
  // B-P1-2: publish the tokens from a target built NOW, not from the member `uiTarget`.
  // UiListActivity::fitThemeRowLabels rebinds FONT_BODY on `uiTarget` to the theme's row face while
  // it measures a list, so republishing from it mints every screen's metrics out of the ROW face's
  // line height (rowHeight/headerHeight/minTouchSize 128/86/74 instead of 96/70/58). Rows are then
  // drawn ~34% taller than their own hit rects and taps land one row off. Same defect already fixed
  // downstream in TitledHeaderBand.h and UiTabListActivity.cpp; this is the source.
  applySharedUiTheme(app, makeUiTarget(renderer_));
}

void UiAppHost::renderUi() {
  app.setDevice(uiTarget.deviceContext());
  app.render();
  // app.render() publishes the interaction table; only now can a snapshot resolve against it.
  publishGeneration_.fetch_add(1);
  uiReady = true;
}

UiAppHost::TouchRoute UiAppHost::routeTouch(const MappedInputManager& input, const bool withLongPress,
                                            const bool routeHeld) {
  TouchRoute result;  // named apart from route() — cppcheck flags the shadow
  if (!uiReady) return result;
  result.snap = touchSnapshotFrom(input, withLongPress);
  if (!result.snap.touchPressed && !result.snap.touchReleased && !(routeHeld && result.snap.touchHeld)) {
    return result;
  }
  result.routed = true;
  result.event = route(result.snap);
  // A tap that routes nowhere produces no event, which on e-paper is indistinguishable from a device
  // that ignored the finger. Record both outcomes: a miss means the coordinates are wrong or the
  // region was not published yet, and no line at all means the tap never reached the UI layer.
  if (result.snap.touchReleased) {
    if (result.event) {
      LOG_DBG("UI", "tap %d,%d -> action=%u value=%d", static_cast<int>(result.snap.touchX),
              static_cast<int>(result.snap.touchY), static_cast<unsigned>(result.event.action),
              static_cast<int>(result.event.value));
    } else {
      LOG_DBG("UI", "tap %d,%d -> no hit%s", static_cast<int>(result.snap.touchX),
              static_cast<int>(result.snap.touchY), result.snap.longPress ? " (long-press)" : "");
    }
  }
  return result;
}

fui::ActionEvent UiAppHost::route(const fui::InputSnapshot& snap) {
  if (!uiReady) return {};
  const auto event = app.route(snap);
  // Releases activate controls; held/pressed frames only highlight or drag.
  // Empty/disabled hit regions produce no event and therefore no feedback.
  if (event && (snap.touchReleased || snap.swipeLeft || snap.swipeRight)) {
    // TAP1: STASH the pulse, do not fire it. app.route() above has already run the
    // activity's handler and issued its requestUpdate(); a blocking pulse here would
    // hold the main task (and therefore the render notification's effect) for 60-120 ms,
    // so the motor ran before the panel did. serviceDeferredHaptic() fires it on the
    // next loop iteration, after the frame work. Duration, duty, level and the
    // VIBRATION_OFF check are unchanged -- the check lives in touchAction(), where it
    // always did, so a stashed pulse obeys the setting at the moment it fires.
    deferHaptic(event.longPress);
  }
  return event;
}
