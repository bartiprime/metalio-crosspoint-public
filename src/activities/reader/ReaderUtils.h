#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include <cctype>
#include <string_view>

#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "../../display/RefreshPolicy.h"

namespace ReaderUtils {

// The mode a PAGE TURN requests. CP_RICKY_TURN_MODE swaps our DU request for the
// GL16 family, mirroring RickyOS's page turn (their mode 6, approximated by our
// GL16 because we have no mode-6 entry — see src/display/RefreshPolicy.h).
// Everything that is not a page turn keeps its own request: a list scroll, a row
// highlight or a keystroke stays on DU.
inline HalDisplay::RefreshMode pageTurnRefreshMode() {
#if CP_READER_AA_LANE
  // r161 READER AA LANE (DEFAULT OFF again as of r162): the turn asks for a FAST
  // paint and the FRAME decides the lane in the panel shim (`useGray8`). That is
  // NOT the pre-r161 behaviour and it is why r162 put the default back to 0: FAST
  // on a 2-bit frame selects the grey wipe composite (my_waveform_wipe_gl(3), 38
  // phases) whose first phase pushes the panel to paper -- the page-turn white
  // flash -- and it also feeds the CP_UI_GREY_CLEAN_EVERY counter, so every 5th
  // turn became a from-white clean. Kept only as an A/B switch.
  return HalDisplay::FAST_REFRESH;
#else
  // r162 / pre-r161: a page turn requests GL16, which the panel drives as
  // epd_hl_update_screen(MODE_GL16) -- a diff drive with no wipe prefix. `clean`
  // is true for anything that is not FAST, so `useGray8` is false and neither the
  // grey composite nor s_uiGreyPaintsSinceClean is touched (metalio_vendor_panel.cpp).
  return static_cast<HalDisplay::RefreshMode>(cp_refresh_page_turn_mode(HalDisplay::FAST_REFRESH));
#endif
}

constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long GO_BACK_OR_HOME_MS = GO_HOME_MS;
constexpr unsigned long SKIP_HOLD_MS = 700;
constexpr unsigned long BOOKMARK_HOLD_MS = 400;
constexpr unsigned long BOOKMARK_MESSAGE_DURATION_MS = 2500;

inline bool gestureAllowsSwipe(const uint8_t gesture) {
  return gesture == CrossPointSettings::TAP_AND_SWIPE || gesture == CrossPointSettings::SWIPE_ONLY;
}

inline bool gestureAllowsTap(const uint8_t gesture) {
  return gesture == CrossPointSettings::TAP_AND_SWIPE || gesture == CrossPointSettings::TAP_ONLY ||
         gesture == CrossPointSettings::INVERTED_TAP;
}

inline bool isRtlBookLanguage(std::string_view tag) {
  if (tag.size() < 2 || (tag.size() > 2 && tag[2] != '-' && tag[2] != '_')) return false;
  const auto first = std::tolower(static_cast<unsigned char>(tag[0]));
  const auto second = std::tolower(static_cast<unsigned char>(tag[1]));
  return (first == 'h' && second == 'e') || (first == 'i' && second == 'w') || (first == 'a' && second == 'r') ||
         (first == 'f' && second == 'a');
}

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

struct PageTurnResult {
  bool prev;
  bool next;
  bool fromTilt;
};

inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  const bool usePress = SETTINGS.longPressButtonBehavior == SETTINGS.OFF;
  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasFlickNext();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasFlickPrev();
  const bool swapFront = input.isNavDirectionSwapped();
  const auto prevButton = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextButton = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  const auto pageButtonTriggered = [&](const MappedInputManager::Button button) {
    if (usePress) return input.wasPressed(button);
    return input.wasLongPressed(button, SKIP_HOLD_MS) || input.wasReleased(button);
  };
  const bool prev =
      tiltPrev || (pageButtonTriggered(MappedInputManager::Button::PageBack) || pageButtonTriggered(prevButton));
  const bool powerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                         input.wasReleased(MappedInputManager::Button::Power);
  const bool next = input.homeButtonAction() == HomeButtonAction::NextPage || tiltNext ||
                    pageButtonTriggered(MappedInputManager::Button::PageForward) || powerTurn ||
                    pageButtonTriggered(nextButton);
  return {prev, next, tiltPrev || tiltNext};
}

struct TouchPageTurn {
  bool prev;
  bool next;
  unsigned long heldMs;
};

// r163 F5: the page-turn button held down right now -- -1 = back, +1 = forward,
// 0 = neither (or both). Mirrors detectPageTurn's button set exactly (the
// orientation-swapped front pair plus the dedicated page buttons); the power-key
// turn and tilt are one-shot edges and never repeat.
inline int heldPageTurnDirection(const MappedInputManager& input) {
  const bool swapFront = input.isNavDirectionSwapped();
  const auto prevButton = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextButton = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  const bool prev = input.isPressed(MappedInputManager::Button::PageBack) || input.isPressed(prevButton);
  const bool next = input.isPressed(MappedInputManager::Button::PageForward) || input.isPressed(nextButton);
  if (prev == next) return 0;  // both down, or neither: not a steerable hold
  return next ? 1 : -1;
}

inline TouchPageTurn detectTouchPageTurn(const GfxRenderer& renderer, const MappedInputManager& input,
                                         const bool rtlBook = false) {
  TouchPageTurn result{false, false, 0};
  if (!SETTINGS.touchReaderControls || !input.hasTouch()) {
    return result;
  }

  // A slow swipe never becomes a long-press chapter skip.
  const auto dir = input.wasSwipe();
  if (dir != MappedInputManager::SwipeDir::None) {
    // Instrumentation: proves whether the pipeline produced a swipe at all, and whether the per
    // direction gesture setting accepted it. A silent log with a deliberate swipe means
    // wasSwipe() never fired upstream, not that the reader rejected it.
    LOG_INF("TOUCH", "swipe dir=%d nextGesture=%d prevGesture=%d nextOk=%d prevOk=%d", static_cast<int>(dir),
            SETTINGS.pageTurnGesture, SETTINGS.previousPageGesture,
            static_cast<int>(gestureAllowsSwipe(SETTINGS.pageTurnGesture)),
            static_cast<int>(gestureAllowsSwipe(SETTINGS.previousPageGesture)));
    result.next = dir == (rtlBook ? MappedInputManager::SwipeDir::Right : MappedInputManager::SwipeDir::Left) &&
                  gestureAllowsSwipe(SETTINGS.pageTurnGesture);
    result.prev = dir == (rtlBook ? MappedInputManager::SwipeDir::Left : MappedInputManager::SwipeDir::Right) &&
                  gestureAllowsSwipe(SETTINGS.previousPageGesture);
    return result;
  }

  const bool nextTaps = gestureAllowsTap(SETTINGS.pageTurnGesture);
  const bool prevTaps = gestureAllowsTap(SETTINGS.previousPageGesture);
  if (!nextTaps && !prevTaps) {
    return result;
  }

  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) {
    return result;
  }
  // Instrumentation: a tap that reached the zone logic. If this never appears while tapping, the tap
  // never became a touchReleasedEvent, and the fault is in the touch pipeline rather than the zones.
  LOG_INF("TOUCH", "tap x=%d y=%d nextGesture=%d prevGesture=%d nextTaps=%d prevTaps=%d", x, y,
          SETTINGS.pageTurnGesture, SETTINGS.previousPageGesture, static_cast<int>(nextTaps),
          static_cast<int>(prevTaps));

  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  // The centered reader-menu tap target (isTouchMenuTap below) keeps priority
  // over the page-turn zones.
  if (SETTINGS.showReaderMenu == CrossPointSettings::READER_MENU_TAP && x >= width / 3 && x < width - width / 3 &&
      y >= height / 3 && y < height - height / 3) {
    return result;
  }

  // Give the whole page to the sole tap-enabled direction. When both accept
  // taps, split at the left third. RTL books and Inverted Tap each reverse
  // the shared zones.
  const bool inverted = (SETTINGS.pageTurnGesture == CrossPointSettings::INVERTED_TAP ||
                         SETTINGS.previousPageGesture == CrossPointSettings::INVERTED_TAP) != rtlBook;
  const bool nextZone = inverted ? x < (width * 2) / 3 : x >= width / 3;
  result.next = nextTaps && (!prevTaps || nextZone);
  result.prev = prevTaps && (!nextTaps || !nextZone);
  result.heldMs = gpio.lastTouchHeldMs();
  return result;
}

// Tap in the center third of the screen: the tap path into the reader menu on
// every touch board. detectTouchPageTurn() excludes this centered rectangle,
// so it remains free in tap mode. The Off/Swipe Up
// alternatives are only surfaced on home-key boards (SettingsList), where the
// menu stays reachable through the key's long-press function.
inline bool isTouchMenuTap(const GfxRenderer& renderer, const MappedInputManager& input) {
  if (!input.hasTouch()) return false;
  if (SETTINGS.showReaderMenu != CrossPointSettings::READER_MENU_TAP) return false;
  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) return false;
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int zoneWidth = width / 3;
  const int zoneHeight = height / 3;
  return x >= zoneWidth && x < width - zoneWidth && y >= zoneHeight && y < height - zoneHeight;
}

// Reader menu opens on the menu edge-swipe or a center-third tap. Home-key
// actions are configured separately from screen gestures.
// Menu gestures honor showReaderMenu independently of touchReaderControls,
// which only gates page-turn touch zones in detectTouchPageTurn().
inline bool isTouchMenuGesture(const GfxRenderer& renderer, const MappedInputManager& input) {
  if (!input.hasTouch()) return false;
  if (input.wasMenuGesture()) return true;
  // Bottom-edge up-swipe variant: only selectable on home-key boards, where
  // Home is the capacitive key and the bottom edge is otherwise unused.
  if (SETTINGS.showReaderMenu == CrossPointSettings::READER_MENU_SWIPE_UP && input.wasReaderMenuSwipeUp()) {
    return true;
  }
  return isTouchMenuTap(renderer, input);
}

// One helper, blocking or deferred: the async form starts the refresh and
// returns so the caller can overlap CPU work with the panel's refresh time.
// Async callers must not touch the framebuffer until
// renderer.waitRefreshComplete() and must rebuild the differential baseline
// before the next page turn (the tiled grayscale cleanup does).
inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh,
                                    int& pagesUntilQuickRefresh, bool async = false) {
  // Three-level cadence: a full clean from white every `refreshFrequency` pages, a page-wide
  // black re-ink ("Quick Refresh") every `quickRefreshPages` pages in between, DU for everything
  // else. The full clean resets both counters because it is a superset of the quick one.
  //
  // QUICK REFRESH IS NOT A GC16 DIFF ANY MORE (vendor v2.0.74 item 5+7, upstream 2b2c36a).
  // It used to ask for HALF, which on the ED047TC2 lands in the driver's diff-based GC16
  // (`epd_hl_update_screen`): that re-drives only the rectangle that changed and leaves the rest
  // of the ghost residue in place, and it was measured on the device at 899 ms,
  // indistinguishable from a normal page turn -- there was nothing to regress. It now asks for
  // EQUALIZE_REFRESH (`MODE_EQUALIZE_BLACK`): the same 48-frame GC16 table, but the LUT is rebuilt
  // so that EVERY pixel whose target is black is driven black whatever the panel currently shows,
  // and the drive covers the whole panel instead of the changed rectangle. The page still ends on
  // its new content -- pixels whose target is white keep their normal transition, no black frame
  // is ever shown, and nothing has to repaint after it -- so a future session must not read
  // "Quick Refresh changed behaviour" as a regression. What it does NOT do is push paper to white,
  // so optical ghosting in white areas survives it; the from-white FULL clean is still the only
  // thing that removes that, at `refreshFrequency`.
  //
  // The periodic full clean therefore still asks for FULL, not the quick mode: FULL goes through
  // `epd_hl_update_screen_from_white`, which assumes a white baseline so every pixel is driven and
  // the panel genuinely clears.
  HalDisplay::RefreshMode mode = pageTurnRefreshMode();
  if (pagesUntilFullRefresh <= 1) {
    mode = HalDisplay::FULL_REFRESH;
  } else if (pagesUntilQuickRefresh <= 1) {
    mode = HalDisplay::EQUALIZE_REFRESH;
  }
  if (async) {
    renderer.displayBufferAsync(mode);
  } else {
    renderer.displayBuffer(mode);
  }
  if (pagesUntilFullRefresh <= 1) {
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    pagesUntilQuickRefresh = SETTINGS.getQuickRefreshPages();
  } else {
    pagesUntilFullRefresh--;
    if (pagesUntilQuickRefresh <= 1) {
      pagesUntilQuickRefresh = SETTINGS.getQuickRefreshPages();
    } else {
      pagesUntilQuickRefresh--;
    }
  }
}

// Display the B/W base of a page whose grayscale pass follows. Panels that
// combine the base (Paper Mono) defer the activation so base + gray planes go
// out as one waveform — displaying the base separately makes the gray pass
// re-drive the whole text body (a visible flash). Other panels display
// normally. Same refresh-cadence bookkeeping as displayWithRefreshCycle.
inline void displayBaseWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh,
                                        int& pagesUntilQuickRefresh) {
  if (renderer.grayscaleCapabilities().base != HalDisplay::GrayscaleBase::Combined) {
    displayWithRefreshCycle(renderer, pagesUntilFullRefresh, pagesUntilQuickRefresh);
    return;
  }
  const auto mode = (pagesUntilFullRefresh <= 1) ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH;
  renderer.displayGrayscaleBase(mode);
  if (pagesUntilFullRefresh <= 1) {
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    pagesUntilFullRefresh--;
  }
}

// Grayscale anti-aliasing pass. Renders content twice (LSB + MSB) to build
// the grayscale buffer. Only the content callback is re-rendered — status bars
// and other overlays should be drawn before calling this.
// Kept as a template to avoid std::function overhead; instantiated once per reader type.
template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn) {
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("READER", "Failed to store BW buffer for anti-aliasing");
    // A combined-base panel may still hold a deferred B/W activation; flush it
    // so the page reaches the panel even without its grays.
    if (renderer.grayscaleCapabilities().base == HalDisplay::GrayscaleBase::Combined)
      renderer.cleanupGrayscaleWithFrameBuffer();
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderFn();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderFn();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  renderer.restoreBwBuffer();
}

// Returns true if the back button was consumed (caller should return).
//
// r163: the reading surface now obeys the SAME rule as every other screen -- TAP = one level back,
// HOLD = Home. It was inverted (tap = Home, hold = file browser, and a setting flipped the pair), which
// made the book the second of the two places in the firmware where a single tap left the screen the
// operator was looking at.
//
// TAP: "one level back" from a book is the screen the book was opened FROM. The reader is entered
// with replaceActivity(), so there is no stack entry to pop; ActivityManager recorded that screen at
// goToReader() time (ReaderReturnTarget) and leaveReaderToOrigin() is the single reader of it --
// Library for a book opened from the Library, Home for one opened from Home's recents, else the file
// browser at the book's own folder. r163 hardcoded the last of those for every entry, which is why a
// book opened from the Library took TWO levels of Back (Library -> book -> file browser).
//
// HOLD: not handled here any more. ActivityManager::handleGlobalBackKey() owns the hold for the whole
// firmware (kLongPressBackHomeMs -> goHome) and EpubReaderActivity no longer claims it, so a hold
// reaches Home by the one global rule. The manager also suppresses the release that follows a hold, so
// ANY Back release that arrives here is a tap by construction -- including one whose measured held time
// looks long only because a blocking waveform swallowed the samples in the middle (the panel's blind
// window is why this function used to need its own 1500 ms threshold).
//
// BackNavCallback (an explicit goHome) is gone with the hold: the manager's hold is what goes Home.
inline bool handleBackNavigation(const MappedInputManager& mappedInput, ActivityManager& activityManager,
                                 const char* filePath) {
  // The reading surface deliberately has no left-edge swipe-to-exit path: in
  // swipe page-turn mode a right swipe must page back instead. Home remains
  // available through the hold. Back swipes stay available in menus and other
  // activities; only this reader-surface handler ignores them. Physical Back
  // buttons are unaffected: isPressed() is button-only, and this guard skips just
  // the gesture's own release frame.
  if (mappedInput.wasBackGesture()) {
    return false;
  }

  if (!mappedInput.wasReleased(MappedInputManager::Button::Back)) return false;
  activityManager.leaveReaderToOrigin(filePath);
  return true;
}

}  // namespace ReaderUtils
