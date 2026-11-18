#pragma once

#include <HalGPIO.h>

#include "util/HomeButtonInput.h"
#include "util/RemoteButtonInput.h"

class GfxRenderer;
namespace freeink {
namespace ui {
enum class ScreenEdge : uint8_t;
}
}  // namespace freeink

class MappedInputManager {
 public:
  enum class Button {
    Back,
    Confirm,
    Left,
    Right,
    Up,
    Down,
    Power,
    PageBack,
    PageForward,
    NavNext,
    NavPrevious,
    ScreenLeft,
    ScreenRight,
    ScreenUp,
    ScreenDown
  };
  enum class SwipeDir { None, Left, Right, Up, Down };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  MappedInputManager(HalGPIO& gpio, const GfxRenderer& renderer) : gpio(gpio), renderer(renderer) {}

  // Blocking transfer loops pump physical input themselves. Defer configured
  // Home-key actions so the next main-loop pass can dispatch them, while the
  // current action remains available for immediate Home cancellation.
  void update(bool deferHomeButtonAction = false) const;
#if FREEINK_CAP_TOUCH
  // X4 Pro delays a single power click until its frontlight double-click window
  // expires. The main loop supplies that one-frame event here.
  void setPowerConfirmClickFrame(const bool clicked) { powerConfirmClickFrame = clicked; }
#endif
  bool injectRemoteClick(RemoteButton button);
  bool hasRemoteClick() const { return remoteInput.button != RemoteButton::None; }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  // One-shot threshold event while the button is down; consumes its release.
  // r163: `notBeforeMs` (0 = ungated) is the r162 allowSleepAt gate, applied to a HOLD. The held time
  // is the SDK's millis()-buttonPressStart, and buttonPressStart is only stamped when the WHOLE button
  // state goes 0 -> nonzero (InputManager::applyStateChange) -- so a press that began before the current
  // screen existed, or whose press-down sample was swallowed by a blocking drive or the panel bus guard,
  // reports a held time measured from an EARLIER contact. If the implied press start predates the moment
  // the caller watched the press go down, this is not a hold and must never be classified as one: it is a
  // tap. Pass the millis() you observed the press edge at.
  bool wasLongPressed(Button button, unsigned long thresholdMs, unsigned long notBeforeMs = 0) const;
  bool consumeSuppressedRelease() const;
  // r100.3: drop the release edge of a press this screen has already acted on. The bit stays armed
  // until that release is actually observed, and the first statement of ActivityManager::loop()
  // consumes it -- so neither the global Back/gesture path nor the current activity can see the
  // edge, and no timing window can let it through. Used by the power-off dialog: the press that
  // opened it belongs to the dialog end-to-end.
  void swallowNextRelease(Button button) const { suppressNextRelease(button); }
  bool isPressed(Button button) const;
  bool hasTouch() const;
  bool wasScreenTapped(int& x, int& y) const;
  // Immediate press edge for drag handles, without the row-selection dwell.
  bool wasScreenTouchPressed(int& x, int& y) const;
  bool wasScreenTouchDown(int& x, int& y) const;
  // One-shot long-press from the SDK touch classifier, fired WHILE the finger
  // is still down (stationary contact held past the SDK threshold). Consuming
  // it suppresses the remainder of the contact — its continued hold and its
  // release edge — so the ensuing finger lift can't also tap-dismiss the popup
  // the long-press opened. The SDK owns that latch and self-clears it once the
  // contact ends.
  bool wasScreenLongPress(int& x, int& y) const;
  bool isScreenTouchHeld(int& x, int& y) const;
  // Raw release edge, also true when the contact ended in a swipe or drag-off
  // (which wasScreenTapped never reports). InputSnapshot builders forward it
  // off-target so FreeInkUI routing clears its pressed-element state.
  bool wasScreenTouchReleased() const;

  // Combined touch interaction for a band of equal rows with caller-supplied
  // geometry — the shared hit-test for lists the theme helpers above do not
  // cover (custom row heights, option prompts, menus). Down = a held
  // tap-candidate is on a row (update the selection highlight); Tap = a tap
  // released on one (activate). rowHeight limits the hit to the top rowHeight
  // px of each step (0 = the full step, no gap band).
  enum class RowTouch : uint8_t { None, Down, Tap };
  RowTouch rowTouch(int& row, int top, int rowStep, int rowCount, int xStart = 0, int xEnd = INT32_MAX,
                    int rowHeight = 0) const;
  // Horizontal variant for side-by-side button pairs (confirmation prompts).
  RowTouch colTouch(int& col, int left, int colStep, int colCount, int yStart, int yEnd, int colWidth = 0) const;

  SwipeDir wasSwipe(int* startX = nullptr, int* startY = nullptr) const;
  // Back = left-to-right swipe anchored at the left edge. Public so swipe-mode
  // page turns (reader) can exclude it from a plain SwipeDir::Right.
  bool wasBackGesture() const;
  // The header back CONTROL (the arrow in the titled band) is a touch target
  // with two different meanings, and the touch pipeline only reports its
  // release (HalGPIO::wasTouchTap): a hold on it produces no event of its own,
  // so a hold could never reach Home. The press/hold timer therefore lives
  // here, at the one place every screen's back control is decoded, instead of
  // in each activity. One-shot per contact; a fired hold consumes the rest of
  // the contact so its lift cannot also land as a tap. Tap stays the ordinary
  // Button::Back path (= one level).
  // r163: `notBeforeMs` as above -- the on-screen control's contact must have STARTED on this screen,
  // otherwise a finger that was already down when the screen changed cannot become a hold.
  bool wasBackControlHold(unsigned long thresholdMs, unsigned long notBeforeMs = 0) const;
  // Home-key boards use a short Home-key tap to exit; their bottom-edge swipe
  // is intentionally unused. Other boards retain the bottom-edge Home gesture.
  // The reader menu remains on its existing top-edge gesture and middle tap.
  bool wasHomeGesture() const;
  // Configured one-frame action, independent of the gesture that triggered it.
  HomeButtonAction homeButtonAction() const { return homeAction; }
  // Swallow touch edges latched before a screen transition (see ActivityManager).
  void discardPendingTouch() const { gpio.discardPendingTouchEvents(); }
  void applySwipeSensitivity() const;

  void resetHomeButtonInput() const {
    homeButtonInput.reset();
    bootKeyInput.reset();
    deferredHomeAction = HomeButtonAction::Ignore;
  }
  bool wasMenuGesture() const;
  // Bottom-edge up-swipe as the reader-menu gesture (SHOW_READER_MENU's Swipe
  // Up option). Only meaningful on home-key boards, where Home lives on the
  // key and the bottom edge is free; elsewhere the same swipe is the Home
  // gesture and this returns false.
  bool wasReaderMenuSwipeUp() const;
  // Top-edge down-swipe opens the light panel when the active board actually
  // has a frontlight. ActivityManager consumes it before activity input.
  bool wasLightPanelGesture() const;
  unsigned long getHeldTime() const;
  const GfxRenderer& getRenderer() const { return renderer; }
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Maps four screen-direction labels onto the two physical front-button roles
  // using the same live-orientation transform as ScreenLeft/Right/Up/Down.
  Labels mapDirectionalLabels(const char* back, const char* confirm, const char* left, const char* right,
                              const char* up, const char* down) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

  // True when the control axis is flipped relative to the physical buttons: always on touch boards,
  // or when button-only boards opt in, while the screen is currently INVERTED / LANDSCAPE_CCW.
  [[nodiscard]] bool isNavDirectionSwapped() const;

 private:
  HalGPIO& gpio;
  // Logical-to-physical button mapping depends on what the user is actually looking at: when the
  // screen is rendered rotated, the directional buttons must flip to match. The renderer is the only
  // authority on the *live* orientation (the reader rotates it and restores portrait on exit), so we
  // read it here instead of CrossPointSettings.orientation, which is just the persisted reader
  // preference and stays "rotated" even while portrait UI like home/settings is on screen.
  const GfxRenderer& renderer;

  Button mapScreenDirection(Button button) const;
  Labels mapFrontLabels(const char* back, const char* confirm, const char* left, const char* right) const;
  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  // SDK edge classification (fui::edgeSwipe) + the shared decode/held-time
  // bookkeeping; the wrappers below give each edge its board meaning.
  bool wasEdgeSwipe(freeink::ui::ScreenEdge edge) const;
  bool wasTopEdgeDownSwipe() const;
  bool wasBottomEdgeUpSwipe() const;
  // Fetch the pending swipe (if any) and map both endpoints to logical screen coords
  bool decodeSwipe(int& sx, int& sy, int& ex, int& ey) const;
#if FREEINK_CAP_TOUCH
  bool wasPowerConfirmClick() const;
#endif
  void rememberTouchHeldTime() const;
  void suppressNextRelease(Button button) const;

  mutable RemoteButtonInput remoteInput;
  mutable HomeButtonInput homeButtonInput;
  // r97 KEY1: the boot/AI key's own gesture state (see update()); kept apart
  // from the home key's so pressing one cannot consume the other's second tap.
  mutable HomeButtonInput bootKeyInput;
  mutable HomeButtonAction bootKeyAction = HomeButtonAction::Ignore;
  mutable HomeButtonAction homeAction = HomeButtonAction::Ignore;
  mutable HomeButtonAction deferredHomeAction = HomeButtonAction::Ignore;
  mutable bool touchHeldOverrideValid = false;
  mutable unsigned long touchHeldOverrideMs = 0;
  mutable unsigned long touchHeldOverrideAt = 0;
  mutable uint16_t longPressFiredButtons = 0;
  mutable uint16_t suppressedReleaseButtons = 0;
  // Armed when a hold on the header back control has already fired for the
  // current contact; cleared when the contact ends so the next one can fire.
  mutable bool backControlHoldFired = false;
#if FREEINK_CAP_TOUCH
  bool powerConfirmClickFrame = false;
#endif
};
