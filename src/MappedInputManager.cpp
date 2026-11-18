#include "MappedInputManager.h"

#include <BoardConfig.h>
#include <FreeInkUICore.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "HapticFeedback.h"
#include "HomeButtonSettings.h"
#include "components/HeaderBackTapTarget.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
// B-P2-5: WHERE A COVER-GLASS KEY COMES FROM.
//
// The board whose cover glass carries the three keys (HOME / PREV / NEXT) decodes them out of the
// touch report and merges them into the button mask as BTN_BACK / BTN_LEFT / BTN_RIGHT. There is no
// physical pin behind those bits on such a board (BoardConfig keeps input.left/right/back
// unassigned), so on it a BTN_LEFT/BTN_RIGHT edge IS a page key, and nothing else can produce one.
//
// This predicate is deliberately narrow: controller is the FT6336u AND all three bits have no pin.
// On any board with a real button on those pins the answer is false and the existing paths --
// the cst816s wasCapacitivePagePressed() branch, and the plain front-button mapping -- keep doing
// exactly what they did before.
bool coverGlassKeysAreButtonBits() {
  const auto& b = BoardConfig::ACTIVE;
  return b.touch.controller == BoardConfig::TouchController::Ft6336u && b.input.back < 0 && b.input.left < 0 &&
         b.input.right < 0;
}

}  // namespace

void MappedInputManager::update(const bool deferHomeButtonAction) const {
  applySwipeSensitivity();
  remoteInput.reset();
  gpio.update();
  // The off-screen capacitive PAGE KEYS are the one input the vibration option separates: a press
  // of one is a page turn, so it stashes a PAGE-TURN pulse. Whether it buzzes is decided when the
  // pulse fires (VIBRATION_TOUCH stays silent, VIBRATION_TOUCH_PAGE buzzes, OFF never buzzes).
  // Deferred, not pulsed here: this runs on the main task ahead of the frame the turn acknowledges.
  // r101.10 HAPT5: THE SOFT-KEY SOURCE ON THIS BOARD.
  //
  // gpio.wasCapacitivePagePressed() is the cst816s path (it returns cstVirtualButtons, which only
  // pollCst816s() sets). This board is FT6336u, so that mask is permanently 0 -- neither HAPT3 hook
  // could ever fire, and the second one also needs wasHomeKeyTapped(), which is absent here too.
  // The cover-glass keys are decoded from the touch stream into InputManager's vkButtons and merged
  // into the readable button state as BTN_BACK / BTN_LEFT / BTN_RIGHT (InputManager.cpp:476), so the
  // press edge is taken from THERE.
  //
  // r101.11 HAPT6: NO VIBRATION GATE. The soft-key row exists precisely FOR the
  // Vibration = Off case (the key has no other feedback), so requiring
  // vibration != VIBRATION_OFF silenced the one setting that needs it.
  // softKeyPattern() gates the cue itself.
  //
  // B-P2-5 (third attempt at this path) -- A PAGE KEY IS A PAGE TURN, AND THE CLASS FOLLOWS THE KEY.
  // The branch above existed but answered ALL THREE cover-glass keys with the SOFT-KEY cue, so the
  // two page keys buzzed like menu keys and the page-turn class (Vibration = On Touch + Page Turn)
  // never applied to the one input that IS a page turn. Worse, deferPageTurnAction() stayed behind
  // wasCapacitivePagePressed() -- permanently 0 here -- so it was unreachable on this board.
  //
  // What decides the class is the KEY, not the board's haptic row:
  //   BTN_LEFT / BTN_RIGHT  -> deferPageTurnAction()   (page-turn class; the option decides)
  //   BTN_BACK              -> deferSoftKeyAction()    (a soft key; the row decides, as before)
  // Both are still one shot per contact, and both are gated inside HapticFeedback at fire time, so a
  // setting changed mid-frame is honoured.
  const bool coverKeys = coverGlassKeysAreButtonBits() && gpio.hasTouch() && !gpio.wasCapacitivePagePressed();
  const bool coverPageKeyEdge =
      coverKeys && (gpio.wasPressed(HalGPIO::BTN_LEFT) || gpio.wasPressed(HalGPIO::BTN_RIGHT));
  // The BACK key is the soft-key row's business (it is a menu key, not a page turn), so its edge keeps
  // that row's gate. The page keys do not: deferPageTurnAction() decides from the vibration option.
  const bool coverBackKeyEdge = coverKeys && (SETTINGS.softKeyHaptic != 0) && gpio.wasPressed(HalGPIO::BTN_BACK);
  if (gpio.wasCapacitivePagePressed()) {
    // HAPT3 (r101.9): the soft-key row REPLACES the page-key cue when it is on -- one 70 ms
    // pulse instead of the page-turn pulse. This is the only way to buzz the two bezel page
    // keys without also buzzing every screen tap and swipe turn, which is what Vibration =
    // "On Touch + Page Turn" does. With the row off the code is the pre-HAPT3 call exactly, so
    // nothing is stashed that was not stashed before.
    if (SETTINGS.softKeyHaptic != 0) {
      haptic_feedback::deferSoftKeyAction();
    } else {
      haptic_feedback::deferPageTurnAction();
    }
  } else if (coverPageKeyEdge) {
    haptic_feedback::deferPageTurnAction();
    LOG_DBG("HAPTIC", "cover-glass PAGE key press -> page-turn cue stashed (vkButtons path)");
  } else if (coverBackKeyEdge) {
    haptic_feedback::deferSoftKeyAction();
    LOG_DBG("HAPTIC", "cover-glass BACK key press -> soft-key cue stashed (vkButtons path)");
  }
  homeAction = HomeButtonAction::Ignore;
  if (gpio.hasHomeKey()) {
    homeAction = homeButtonInput.update(millis(), gpio.wasHomeKeyTapped(), gpio.wasHomeKeyLongPressed(),
                                        wasSwipe() != SwipeDir::None, gpio.wasHomeKeyPressed(),
                                        static_cast<HomeButtonAction>(SETTINGS.homeButtonTapAction),
                                        static_cast<HomeButtonAction>(SETTINGS.homeButtonDoubleTapAction),
                                        static_cast<HomeButtonAction>(SETTINGS.homeButtonLongPressAction));
    if (gpio.wasHomeKeyLongPressed() && homeAction != HomeButtonAction::Ignore) {
      haptic_feedback::deferTouchAction(true);
    } else if (homeAction != HomeButtonAction::Ignore) {
      // The key's gesture resolved to an action: that is a touch-driven action, not a page turn.
      // HAPT3 (r101.9): the soft-key row is NOT wired on top of this, because the tap feedback
      // above already buzzes the home/back key under every Haptic Feedback setting. It only
      // covers the one case that feedback cannot reach -- Vibration = Off, where the tap path
      // stashes a pulse that is then deliberately thrown away. There, and only there, the
      // soft-key row's 70 ms cue is what the key fires.
      if (SETTINGS.softKeyHaptic != 0 && SETTINGS.vibration == CrossPointSettings::VIBRATION_OFF) {
        haptic_feedback::deferSoftKeyAction();
      } else {
        haptic_feedback::deferTouchAction();
      }
    }
  }
  // -------------------------------------------------------------------------
  // r97 KEY1: the physical boot/AI key (GPIO61 on the Metalio 4 Plus) drives the
  // SAME three-gesture layer with its own settings and its own state, because
  // on this board the capacitive home key already owns the layer: they are two
  // independent contacts and must not share an edge history.
  //
  // Two traps, both real:
  //  * doubleTap is pinned to Ignore, so HomeButtonInput returns the tap
  //    IMMEDIATELY on release instead of holding it for DOUBLE_TAP_MS=350 while
  //    it waits for a second contact the boot key does not have;
  //  * the tap edge comes from HalGPIO::wasBootKeyTapped(), which only sets on
  //    RELEASE. A press-edge tap would toggle the frontlight on the way down of
  //    a long press and then also sync on the way up.
  // A screen swipe is deliberately not passed in: it means nothing to a
  // physical key and would only reset an unrelated gesture.
  // -------------------------------------------------------------------------
  if (gpio.hasBootKey()) {
    bootKeyAction = bootKeyInput.update(
        millis(), gpio.wasBootKeyTapped(), gpio.wasBootKeyLongPressed(), false, gpio.wasBootKeyPressed(),
        boot_key::toHomeAction(SETTINGS.bootKeyTapAction), HomeButtonAction::Ignore,
        boot_key::toHomeAction(SETTINGS.bootKeyLongPressAction));
    if (bootKeyAction != HomeButtonAction::Ignore) {
      if (gpio.wasBootKeyLongPressed()) {
        haptic_feedback::deferTouchAction(true);
      } else {
        haptic_feedback::deferTouchAction();
      }
      // The home key wins a same-frame collision; both are one-shot, so the
      // loser is simply dropped rather than queued.
      if (homeAction == HomeButtonAction::Ignore) homeAction = bootKeyAction;
    }
  }
  // Touch boards without a home key: a short power click can carry a reader
  // shortcut (see SHORT_PWRBTN).
  if (homeAction == HomeButtonAction::Ignore && mapButton(Button::Power, &HalGPIO::wasReleased)) {
    switch (SETTINGS.shortPwrBtn) {
      case CrossPointSettings::PWR_BOOKMARK:
        homeAction = HomeButtonAction::Bookmark;
        break;
      case CrossPointSettings::PWR_SYNC:
        homeAction = HomeButtonAction::Sync;
        break;
      case CrossPointSettings::PWR_READER_MENU:
        homeAction = HomeButtonAction::ReaderMenu;
        break;
      default:
        break;
    }
  }
  if (deferHomeButtonAction) {
    // Keep the first action observed during a synchronous transfer. Home must
    // still be visible now so the transfer can cancel and unwind promptly.
    if (homeAction != HomeButtonAction::Ignore && deferredHomeAction == HomeButtonAction::Ignore) {
      deferredHomeAction = homeAction;
    }
  } else if (deferredHomeAction != HomeButtonAction::Ignore) {
    homeAction = deferredHomeAction;
    deferredHomeAction = HomeButtonAction::Ignore;
  }
  for (uint8_t value = 0; value <= static_cast<uint8_t>(Button::ScreenDown); ++value) {
    if (!isPressed(static_cast<Button>(value))) longPressFiredButtons &= ~(1u << value);
  }
}

bool MappedInputManager::isNavDirectionSwapped() const {
  // Touch boards always follow the rendered orientation; button-only boards keep the user toggle.
  // Home and settings render in portrait, so neither path swaps them.
  const auto orientation = renderer.getOrientation();
  return (gpio.hasTouch() || SETTINGS.frontButtonFollowOrientation) &&
         (orientation == GfxRenderer::PortraitInverted || orientation == GfxRenderer::LandscapeCounterClockwise);
}

MappedInputManager::Button MappedInputManager::mapScreenDirection(const Button button) const {
  // Rows follow GfxRenderer::Orientation's declared order.
  static constexpr Button directions[][4] = {
      {Button::Left, Button::Right, Button::Up, Button::Down},
      {Button::Down, Button::Up, Button::Left, Button::Right},
      {Button::Right, Button::Left, Button::Down, Button::Up},
      {Button::Up, Button::Down, Button::Right, Button::Left},
  };

  uint8_t direction = 0;
  switch (button) {
    case Button::ScreenLeft:
      direction = 0;
      break;
    case Button::ScreenRight:
      direction = 1;
      break;
    case Button::ScreenUp:
      direction = 2;
      break;
    case Button::ScreenDown:
      direction = 3;
      break;
    default:
      return button;
  }

  const uint8_t orientation =
      SETTINGS.frontButtonFollowOrientation ? static_cast<uint8_t>(renderer.getOrientation()) : 0;
  return directions[orientation][direction];
}

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = SETTINGS.sideButtonLayout;

  switch (button) {
    case Button::Back:
      // Logical Back maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonBack);
    case Button::Confirm:
      // Logical Confirm maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonConfirm);
    case Button::Left:
      // Logical Left maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonLeft);
    case Button::Right:
      // Logical Right maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonRight);
    case Button::Up:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_UP);
    case Button::Down:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_DOWN);
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return (gpio.*fn)(isNavDirectionSwapped() ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP);
        case CrossPointSettings::NEXT_PREV:
          return (gpio.*fn)(isNavDirectionSwapped() ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN);
        case CrossPointSettings::PREV_PREV:
          return (gpio.*fn)(HalGPIO::BTN_UP) || (gpio.*fn)(HalGPIO::BTN_DOWN);
        case CrossPointSettings::NEXT_NEXT:
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return (gpio.*fn)(isNavDirectionSwapped() ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN);
        case CrossPointSettings::NEXT_PREV:
          return (gpio.*fn)(isNavDirectionSwapped() ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP);
        case CrossPointSettings::NEXT_NEXT:
          return (gpio.*fn)(HalGPIO::BTN_UP) || (gpio.*fn)(HalGPIO::BTN_DOWN);
        case CrossPointSettings::PREV_PREV:
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::NavNext:
      // Logical "next item" navigation: side Down + front Right, with the control axis flipped in
      // INVERTED / LANDSCAPE_CCW under the live orientation policy, matching the rotated hint labels.
      return isNavDirectionSwapped() ? (mapButton(Button::Up, fn) || mapButton(Button::Left, fn))
                                     : (mapButton(Button::Down, fn) || mapButton(Button::Right, fn));
    case Button::NavPrevious:
      // Logical "previous item" navigation: side Up + front Left, axis-flipped in the same orientations.
      return isNavDirectionSwapped() ? (mapButton(Button::Down, fn) || mapButton(Button::Right, fn))
                                     : (mapButton(Button::Up, fn) || mapButton(Button::Left, fn));
    case Button::ScreenLeft:
    case Button::ScreenRight:
    case Button::ScreenUp:
    case Button::ScreenDown:
      return mapButton(mapScreenDirection(button), fn);
  }

  return false;
}

namespace {
constexpr unsigned long TOUCH_DOWN_SELECT_DELAY_MS = 90;
constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;
}  // namespace

bool MappedInputManager::hasTouch() const { return gpio.hasTouch(); }

void MappedInputManager::rememberTouchHeldTime() const {
  touchHeldOverrideValid = true;
  touchHeldOverrideMs = gpio.lastTouchHeldMs();
  touchHeldOverrideAt = millis();
}

void MappedInputManager::applySwipeSensitivity() const {
  // Cheap and idempotent: the setting can change while the Controls screen is open, so the value is
  // compared against the last one applied rather than pushed from a change handler.
  static int lastPx = -1;
  const int px = SETTINGS.getSwipeThresholdPx();
  if (px == lastPx) return;
  lastPx = px;
  gpio.setSwipeThreshold(px);
}

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  int tapX = 0;
  int tapY = 0;
  renderer.tapToLogical(nx, ny, tapX, tapY);
  // A tap on the header back button is Button::Back (wasBackGesture), not a
  // screen tap: screens that route every tap (the keyboard's key router)
  // would otherwise swallow it before their Back check.
  if (HeaderBackTapTarget::contains(tapX, tapY)) {
    LOG_DBG("UITGT", "tap %d,%d swallowed as Back (rect %d,%d %dx%d)", tapX, tapY, HeaderBackTapTarget::x,
            HeaderBackTapTarget::y, HeaderBackTapTarget::w, HeaderBackTapTarget::h);
    return false;
  }
  x = tapX;
  y = tapY;
  rememberTouchHeldTime();
  return true;
}

bool MappedInputManager::wasScreenTouchPressed(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchDown(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenTouchDown(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  unsigned long heldMs = 0;
  if (!gpio.isTouchTapCandidate(nx, ny, heldMs)) return false;
  if (heldMs < TOUCH_DOWN_SELECT_DELAY_MS) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenLongPress(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchLongPress(nx, ny)) return false;
  // Consuming the long-press implies acting on it: suppress the rest of the
  // contact so the finger lift can't also tap whatever the action opened.
  gpio.suppressTouchContact();
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::isScreenTouchHeld(int& x, int& y) const {
  // Live contact position while the finger is down (no tap-slop gate) — drag tracking.
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.isTouchHeldAt(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenTouchReleased() const { return gpio.wasTouchReleased(); }

MappedInputManager::RowTouch MappedInputManager::rowTouch(int& row, const int top, const int rowStep,
                                                          const int rowCount, const int xStart, const int xEnd,
                                                          const int rowHeight) const {
  if (rowStep <= 0 || rowCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (x < xStart || x >= xEnd || y < top) return false;
    const int r = (y - top) / rowStep;
    if (r >= rowCount) return false;
    if (rowHeight > 0 && (y - top) % rowStep >= rowHeight) return false;
    row = r;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

MappedInputManager::RowTouch MappedInputManager::colTouch(int& col, const int left, const int colStep,
                                                          const int colCount, const int yStart, const int yEnd,
                                                          const int colWidth) const {
  if (colStep <= 0 || colCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (y < yStart || y >= yEnd || x < left) return false;
    const int c = (x - left) / colStep;
    if (c >= colCount) return false;
    if (colWidth > 0 && (x - left) % colStep >= colWidth) return false;
    col = c;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

bool MappedInputManager::decodeSwipe(int& sx, int& sy, int& ex, int& ey) const {
  float nxs = 0.0f;
  float nys = 0.0f;
  float nxe = 0.0f;
  float nye = 0.0f;
  if (!gpio.wasSwipe(nxs, nys, nxe, nye)) return false;
  renderer.tapToLogical(nxs, nys, sx, sy);
  renderer.tapToLogical(nxe, nye, ex, ey);
  return true;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe(int* startX, int* startY) const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return SwipeDir::None;
  if (startX) *startX = sx;
  if (startY) *startY = sy;
  switch (fui::swipeDirection(sx, sy, ex, ey)) {
    case fui::SwipeDir::Left:
      return SwipeDir::Left;
    case fui::SwipeDir::Right:
      return SwipeDir::Right;
    case fui::SwipeDir::Up:
      return SwipeDir::Up;
    case fui::SwipeDir::Down:
      return SwipeDir::Down;
    default:
      return SwipeDir::None;
  }
}

// Edge classification (which swipe counts as an edge gesture) lives in the
// SDK; only the MEANING of each edge — back, menu, home, light panel, and the
// home-key remap — is decided here.
bool MappedInputManager::wasEdgeSwipe(const freeink::ui::ScreenEdge edge) const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const bool hit = fui::edgeSwipe(edge, sx, sy, ex, ey, renderer.getScreenWidth(), renderer.getScreenHeight());
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasBackGesture() const {
  // Tap on the header back button (rect recorded by BaseTheme::drawHeader;
  // empty on screens without one). Folded into Button::Back alongside the
  // swipe so every activity's existing Back handling picks it up.
  float nx = 0.0f;
  float ny = 0.0f;
  if (gpio.wasTouchTap(nx, ny)) {
    int tapX = 0;
    int tapY = 0;
    renderer.tapToLogical(nx, ny, tapX, tapY);
    if (HeaderBackTapTarget::contains(tapX, tapY)) {
      LOG_DBG("ACT", "back source: header rect tap @%d,%d", tapX, tapY);
      rememberTouchHeldTime();
      return true;
    }
  }
  // Back = left-to-right swipe starting near the left edge. Edge-anchored so that
  // mid-screen horizontal swipes stay available to activities that consume
  // SwipeDir::Left/Right (e.g. percent selection, image viewer).
  const bool edge = wasEdgeSwipe(fui::ScreenEdge::Left);
  if (edge) LOG_DBG("ACT", "back source: left-edge swipe");
  return edge;
}

bool MappedInputManager::wasBackControlHold(const unsigned long thresholdMs, const unsigned long notBeforeMs) const {
  // The only events the touch SDK reports for the back control are its release
  // (wasTouchTap) and, separately, a long-press classifier. A hold on the small
  // arrow cannot rely on either: the release arrives only when the finger
  // lifts (so a hold would still read as one tap = one level, never Home), and
  // the long-press event is what a screen's own hold actions consume. So the
  // press/hold timer is timed HERE, from the contact's own down sample.
  float nx = 0.0f;
  float ny = 0.0f;
  unsigned long heldMs = 0;
  if (!gpio.isTouchTapCandidate(nx, ny, heldMs)) {
    // No qualifying contact (lifted, or dragged past the tap slop): re-arm for
    // the next touch.
    backControlHoldFired = false;
    return false;
  }
  if (backControlHoldFired) return false;  // one shot per contact
  int x = 0;
  int y = 0;
  renderer.tapToLogical(nx, ny, x, y);
  // Timed from the DOWN point, so a hold only counts when it started on the
  // control: dragging onto the arrow must not turn into Home.
  if (!HeaderBackTapTarget::contains(x, y)) return false;
  if (heldMs < thresholdMs) return false;
  // r163: r88 timed this from the contact's own down sample but never asked WHICH screen that down
  // belonged to. A finger already resting on the arrow when the screen changes would then hold to Home
  // from a screen it never touched. Same gate as the physical key: the contact must have STARTED here.
  if (notBeforeMs != 0 && millis() - heldMs < notBeforeMs) return false;
  backControlHoldFired = true;
  // The lift must not also arrive as a tap (which would go back one level on
  // top of going Home).
  gpio.suppressTouchContact();
  LOG_DBG("UITGT", "back control held %lu ms -> Home (rect %d,%d %dx%d)", heldMs, HeaderBackTapTarget::x,
          HeaderBackTapTarget::y, HeaderBackTapTarget::w, HeaderBackTapTarget::h);
  return true;
}

bool MappedInputManager::wasTopEdgeDownSwipe() const { return wasEdgeSwipe(fui::ScreenEdge::Top); }

bool MappedInputManager::wasBottomEdgeUpSwipe() const { return wasEdgeSwipe(fui::ScreenEdge::Bottom); }

bool MappedInputManager::wasMenuGesture() const { return wasTopEdgeDownSwipe(); }

bool MappedInputManager::wasReaderMenuSwipeUp() const { return gpio.hasHomeKey() && wasBottomEdgeUpSwipe(); }

bool MappedInputManager::wasHomeGesture() const {
  return gpio.hasHomeKey() ? homeAction == HomeButtonAction::Home : wasBottomEdgeUpSwipe();
}

bool MappedInputManager::wasLightPanelGesture() const {
  // On lightless boards the same edge remains available to the reader menu.
  return Frontlight.present() && wasTopEdgeDownSwipe();
}

#if FREEINK_CAP_TOUCH
bool MappedInputManager::wasPowerConfirmClick() const {
  if (!gpio.hasTouch() || SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::PWR_CONFIRM) return false;
  // Wait out the X4 Pro's frontlight double-click window before treating its
  // first release as Confirm. With the shortcut disabled, and on other touch
  // boards, the release counts directly.
  if (BoardConfig::isX4Pro() && SETTINGS.doubleClickPwrLight) return powerConfirmClickFrame;
  return gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.getPowerButtonHeldTime() <= SETTINGS.getPowerButtonDuration();
}
#endif

bool MappedInputManager::injectRemoteClick(const RemoteButton button) {
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || isPressed(Button::Confirm) || isPressed(Button::Back))
    return false;
  return remoteInput.click(button);
}

bool MappedInputManager::wasPressed(const Button button) const {
  if (button == Button::Confirm && remoteInput.button == RemoteButton::Confirm) return true;
  if (button == Button::Back && remoteInput.button == RemoteButton::Back) return true;
  if ((button == Button::NavNext || button == Button::Right) && remoteInput.button == RemoteButton::Next) return true;
  if ((button == Button::NavPrevious || button == Button::Left) && remoteInput.button == RemoteButton::Previous)
    return true;
  if (button == Button::Confirm && homeAction == HomeButtonAction::Confirm) return true;
  if (button == Button::Back && wasBackGesture()) return true;
#if FREEINK_CAP_TOUCH
  if (button == Button::Confirm && wasPowerConfirmClick()) return true;
#endif
  return mapButton(button, &HalGPIO::wasPressed);
}

bool MappedInputManager::wasReleased(const Button button) const {
  if (button == Button::Confirm && remoteInput.button == RemoteButton::Confirm) return true;
  if (button == Button::Back && remoteInput.button == RemoteButton::Back) return true;
  if ((button == Button::NavNext || button == Button::Right) && remoteInput.button == RemoteButton::Next) return true;
  if ((button == Button::NavPrevious || button == Button::Left) && remoteInput.button == RemoteButton::Previous)
    return true;
  if (button == Button::Confirm && homeAction == HomeButtonAction::Confirm) return true;
  if (button == Button::Back && wasBackGesture()) return true;
#if FREEINK_CAP_TOUCH
  if (button == Button::Confirm && wasPowerConfirmClick()) return true;
#endif
  return mapButton(button, &HalGPIO::wasReleased);
}

bool MappedInputManager::wasLongPressed(const Button button, const unsigned long thresholdMs,
                                       const unsigned long notBeforeMs) const {
  if (!isPressed(button)) return false;
  const uint16_t bit = 1u << static_cast<uint8_t>(button);
  const unsigned long heldMs = getHeldTime();
  if ((longPressFiredButtons & bit) != 0 || heldMs < thresholdMs) return false;
  // r163: a press that cannot be timed is a tap, never a hold (see the header). getHeldTime() is
  // millis()-buttonPressStart, so millis()-heldMs reconstructs the press start the SDK believes in;
  // if that predates the moment the caller saw the press go down, the measurement belongs to an
  // earlier contact and must not be spent on a Home navigation. Judged at the press's own start --
  // the same shape as main.cpp's allowSleepAt gate on the power key.
  if (notBeforeMs != 0 && millis() - heldMs < notBeforeMs) return false;
  longPressFiredButtons |= bit;
  suppressNextRelease(button);
  // A cover-glass PAGE key held down: the raw contact bits, not the logical mapping. The two page
  // keys are the only bits the cover glass can set to LEFT/RIGHT, and this is reached only where the
  // press edge for THIS button already took the page-turn class, so a hold can only ever upgrade that
  // same contact's pulse.
  const bool coverPageKeyHeld = coverGlassKeysAreButtonBits() &&
                                (gpio.isPressed(HalGPIO::BTN_LEFT) || gpio.isPressed(HalGPIO::BTN_RIGHT));
  if (mapButton(button, &HalGPIO::isCapacitivePagePressed) || coverPageKeyHeld) {
    // A hold on a PAGE KEY: page-turn class, so the option decides at fire time (the press-edge
    // pulse for the same contact was stashed by update(); this one is the hold recognised ~0.5 s
    // later, which is a separate event, not a duplicate).
    //
    // B-P2-6: the gate used to be wasCapacitivePagePressed() alone -- the cst816s source, which is
    // permanently 0 on this board, so a cover-glass page key HELD never buzzed at all. The second
    // half of the test is the same source update() now uses for the press edge.
    //
    // Still ONE cue per contact: deferPageTurnAction(true) against an already-stashed page-turn pulse
    // only upgrades its longPress flag in the same slot; it does not queue a second pattern.
    if (coverGlassKeysAreButtonBits() || SETTINGS.softKeyHaptic == 0) haptic_feedback::deferPageTurnAction(true);
  }
  return true;
}

void MappedInputManager::suppressNextRelease(const Button button) const {
  suppressedReleaseButtons |= 1u << static_cast<uint8_t>(button);
}

bool MappedInputManager::consumeSuppressedRelease() const {
  uint16_t released = 0;
  for (uint8_t value = 0; value <= static_cast<uint8_t>(Button::ScreenDown); ++value) {
    const uint16_t bit = 1u << value;
    if ((suppressedReleaseButtons & bit) != 0 && mapButton(static_cast<Button>(value), &HalGPIO::wasReleased)) {
      released |= bit;
    }
  }
  suppressedReleaseButtons &= ~released;
  return released != 0;
}

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

unsigned long MappedInputManager::getHeldTime() const {
  // A mapped action has its own meaning, independent of the contact duration.
  if (hasRemoteClick() || homeAction != HomeButtonAction::Ignore) return 0;
  if (!gpio.wasAnyPressed() && !gpio.wasAnyReleased() && touchHeldOverrideValid &&
      millis() - touchHeldOverrideAt <= TOUCH_HELD_OVERRIDE_WINDOW_MS) {
    return touchHeldOverrideMs;
  }
  touchHeldOverrideValid = false;
  return gpio.getHeldTime();
}

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Swap previous/next labels to match the page turn direction swap in INVERTED and LANDSCAPE_CCW.
  const bool swapLabels = isNavDirectionSwapped();
  const char* leftLabel = swapLabels ? next : previous;
  const char* rightLabel = swapLabels ? previous : next;

  return mapFrontLabels(back, confirm, leftLabel, rightLabel);
}

MappedInputManager::Labels MappedInputManager::mapDirectionalLabels(const char* back, const char* confirm,
                                                                    const char* left, const char* right, const char* up,
                                                                    const char* down) const {
  const auto labelForButton = [&](const Button rawButton) {
    if (mapScreenDirection(Button::ScreenLeft) == rawButton) return left;
    if (mapScreenDirection(Button::ScreenRight) == rawButton) return right;
    if (mapScreenDirection(Button::ScreenUp) == rawButton) return up;
    if (mapScreenDirection(Button::ScreenDown) == rawButton) return down;
    return "";
  };
  return mapFrontLabels(back, confirm, labelForButton(Button::Left), labelForButton(Button::Right));
}

MappedInputManager::Labels MappedInputManager::mapFrontLabels(const char* back, const char* confirm, const char* left,
                                                              const char* right) const {
  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return left;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return right;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}
