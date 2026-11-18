#include "FrontlightPanelActivity.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/customListIcons.h"
#include "components/icons/listIcons.h"
#include "platform/ExpArms.h"
#include "activities/util/FrontlightPanelRefresh.h"
#include "util/SlideSwitch.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_BRIGHTNESS = 1;
constexpr fui::ActionId ACTION_WARMTH = 2;
constexpr fui::ActionId ACTION_TOGGLE = 3;
constexpr fui::ActionId ACTION_BRIGHTNESS_STEP = 4;
constexpr fui::ActionId ACTION_WARMTH_STEP = 5;
constexpr fui::ActionId ACTION_TILE = 6;  // value = tile index

// iOS-style geometry. The panel is a card hanging from the top of the screen:
// a grabber, full-width slider pills, then a 2-column tile grid. The chrome
// itself is fui::sheet / fui::sliderRow / fui::tileGrid; these constants only
// size the bands, and computePanelBottom() mirrors them.
constexpr int16_t kPanelSideMargin = 16;
constexpr int16_t kGrabberHeight = 5;     // fui::SheetProps default, mirrored here
constexpr int16_t kSliderRowHeight = 56;  // the pill itself (finger-sized)
constexpr int16_t kTileHeight = 84;
constexpr int16_t kTileGap = 16;
constexpr int kTileCols = 2;
// One percent per press, on the -/+ buttons and on the physical Left/Right keys
// alike (both repeat while held), so a level can be set exactly.
constexpr int BRIGHTNESS_STEP = 1;
// The dimmest setting is 1%, not 0: turning the light off is what the lamp
// button next to the slider is for, so a 0% "on" level would only be a second,
// worse way to reach the same place.
constexpr uint8_t MIN_BRIGHTNESS = 1;

uint8_t percentFromPermille(const int16_t permille) {
  int value = (static_cast<int>(permille) * 100 + 500) / 1000;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  return static_cast<uint8_t>(value);
}
}  // namespace

FrontlightPanelActivity::FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FrontlightPanel", renderer, mappedInput), UiAppHost(renderer) {}

void FrontlightPanelActivity::onEnter() {
  Activity::onEnter();

  // A stored 0% predates the 1% floor (or came from the web settings): show it
  // as the floor rather than a level the slider can no longer produce. onExit
  // persists that, which is the intent — 0 is not a brightness any more.
  brightness = std::max(MIN_BRIGHTNESS, Frontlight.brightness());
  warmth = Frontlight.warmth();
  lightOn = Frontlight.isOn();
  lightOnChanged = false;

  // Seed the touch tile's restore mode from the live setting, so toggling off
  // and back on within this session returns to the mode the user had.
  if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) {
    touchModeRestore = SETTINGS.touchReaderControls;
  }

  resetUi();
  app.on(ACTION_BRIGHTNESS, &FrontlightPanelActivity::onBrightnessEvent, this);
  app.on(ACTION_WARMTH, &FrontlightPanelActivity::onWarmthEvent, this);
  app.on(ACTION_TOGGLE, &FrontlightPanelActivity::onToggleEvent, this);
  app.on(ACTION_BRIGHTNESS_STEP, &FrontlightPanelActivity::onBrightnessStepEvent, this);
  app.on(ACTION_WARMTH_STEP, &FrontlightPanelActivity::onWarmthStepEvent, this);
  app.on(ACTION_TILE, &FrontlightPanelActivity::onTileEvent, this);
  app.setScreen(&FrontlightPanelActivity::panelScreen, this);
  // Enter is UNCHANGED by FLP1: no promotion, no suppression -- the
  // activity-transition policy decides the opening paint, exactly as before.
  if (flpanel::plan(flpanel::Event::Enter, exparms::panelPartialRefresh()).requestPaint) requestUpdate();
}

void FrontlightPanelActivity::persistLightSettings() {
  // brightness/warmth are always restored unconditionally on boot (see
  // main.cpp), so they never diverge from SETTINGS at onEnter() — comparing
  // against SETTINGS here only fires on a genuine user change. lightOn has
  // no such guarantee (see lightOnChanged's declaration), so it's gated on
  // the user actually having touched it this session instead.
  const bool changed = SETTINGS.frontlightBrightness != brightness || SETTINGS.frontlightWarmth != warmth ||
                       (lightOnChanged && SETTINGS.frontlightOn != (lightOn ? 1 : 0));
  if (changed) {
    SETTINGS.frontlightBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    if (lightOnChanged) SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
  }
}

void FrontlightPanelActivity::onExit() {
  persistLightSettings();
  Activity::onExit();
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->brightness = std::max(MIN_BRIGHTNESS, percentFromPermille(event.dragPermille));
  Frontlight.setBrightness(self->brightness);
  if (!self->lightOn && !slideSwitchOwnsLight()) {
    self->lightOn = true;
    self->lightOnChanged = true;
    Frontlight.setOn(true);
  }
  // The repaint is NOT requested here, deliberately: the fui sheet invalidates
  // itself on this event and loop()'s tick is the single producer for a drag
  // (see `if (app.invalidated()) requestAdjustRepaint()`), so a second request
  // could drive a second waveform for one percent.
}

void FrontlightPanelActivity::onWarmthEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->warmth = percentFromPermille(event.dragPermille);
  Frontlight.setWarmth(self->warmth);
}

void FrontlightPanelActivity::onToggleEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->toggleLight();
}

void FrontlightPanelActivity::onBrightnessStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustBrightness(event.value);
}

void FrontlightPanelActivity::onWarmthStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustWarmth(event.value);
}

void FrontlightPanelActivity::onTileEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->runTile(event.value);
}

void FrontlightPanelActivity::runTile(const int idx) {
  switch (idx) {
    case 0: {  // Night mode (inverted output polarity, applied to the whole UI)
      SETTINGS.screenInverted = SETTINGS.screenInverted ? 0 : 1;
      SETTINGS.saveToFile();
      // Inversion rewrites every pixel; take the clean waveform so the panel
      // does not keep a ghost of the old polarity. FLP1 does not touch this
      // (plan(NightTile): HALF, unchanged) -- the tile's own reason stands.
      const flpanel::Plan night = flpanel::plan(flpanel::Event::NightTile, exparms::panelPartialRefresh());
      if (night.halfClean) cleanRefreshPending = true;
      if (night.requestPaint) requestUpdate();
      break;
    }
    case 1: {  // Ghost-cleanup refresh of the whole frame
      // The operator asked for a REAL clean and got a home-content inversion:
      // promoting a FULL onto the repaint of the screen underneath cannot show a
      // solid black panel, because the frame being driven is the home screen and
      // GC16's inverted intermediate frames are what read as "a flash" (which is
      // why light shapes -- cover edges, the overlay border, the progress bar --
      // appeared at fixed positions instead of a black panel).
      //
      // Sequence, in this order:
      //   1. dismiss the overlay (close() = finish(): the manager pops us and
      //      queues the home repaint, it does not paint synchronously),
      //   2. fill the WHOLE frame with explicit black and drive it as a
      //      from-white FULL/GC16, so the glass flashes solid black,
      //   3. let the home repaint run through the normal policy -- nothing is
      //      promoted for it (see the note on the double-FULL guard below).
      // close() first: popActivity() applies the activity-transition policy,
      // which may arm a promotion of its own, and that armed mode would replace
      // the FULL asked for below in the producer (GfxRenderer.cpp:1808 /
      // :1798 resolve the promotion AFTER the call site chooses).
      // And all of steps 1 and 2 under the render lock: popActivity() arms its
      // promotion on the renderer's plain (non-atomic) flags, so a paint of this
      // panel landing between close() and the cancel below would consume that
      // promotion on the overlay and add a second FULL before ours.
      LOG_INF("UI", "refresh tile: black fill -> FULL(GC16 from white) -> normal home repaint");
      const bool wasInverted = display.isInverted();
      // Hold the render lock across the fill + drive: the render task paints this
      // activity, and a paint landing between clearScreen() and displayBuffer()
      // would put the panel's own chrome back into the frame we are driving.
      {
        RenderLock lock;
        close();
        renderer.cancelPromotedRefresh();
        // Night mode is a DRIVER-side polarity flip (HalDisplay::setInverted ->
        // FreeInkDisplay inverts the bytes it sends), so with it on a black buffer
        // would flash WHITE. Hold the polarity at normal for this drive only, and
        // restore it before the repaint, so the glass really goes black.
        if (wasInverted) display.setInverted(false);
        // The fill is a literal black (0x00) written straight into the panel
        // buffer: HalDisplay::clearScreen -> FreeInkDisplay::clearScreen memsets the
        // whole buffer, so it is neither the theme background nor a UI colour token,
        // and the bezel insets have no say here (BoardConfig::viewableInsets is
        // consumed only by GfxRenderer::getOrientedViewableTRBL, i.e. layout -- it
        // never clips the buffer), so all 684 rows get the fill.
        renderer.clearScreen(0x00);
        // FULL is a from-white GC16 in the panel shim (metalio_vendor_panel.cpp:
        // epd_hl_update_screen_from_white), i.e. a whole-panel drive that the
        // changed-pixel/diff path cannot short-circuit: the diff-area update is
        // only what the non-full modes use. An explicit FULL also survives the r81
        // policy -- cp_refresh_resolve_mode() keeps CP_RM_FULL ("an explicit FULL
        // survives: it *is* the from-white GC16", RefreshPolicy.h:258-268), so the
        // clean-outranks-promotion rule cannot turn it into something cheaper.
        renderer.displayBuffer(HalDisplay::FULL_REFRESH);
        if (wasInverted) display.setInverted(true);
      }
      LOG_INF("UI", "refresh tile: black FULL driven (polarity force-normal=%d)",
              wasInverted ? 1 : 0);
      // Step 3 needs no call here: the repaint is already queued by the pop.
      // The policy supplies its own drive (the latch is FULL now, so the home
      // request resolves to one HALF/GC16 by rule 3, RefreshPolicy.h:274), which
      // is exactly why no second FULL is armed: two from-white FULLs back to
      // back would be two 0xF7 flashes for one tile press.
      return;
    }
    case 2:  // Cycle the reading orientation
      SETTINGS.orientation = static_cast<uint8_t>((SETTINGS.orientation + 1) % 4);
      SETTINGS.saveToFile();
      // Only the setting changes: turning the renderer cropped the portrait-only
      // screens the panel opens over. The reader reflows on its next loop().
      requestUpdate();
      break;
    case 3:  // Touch reader controls (for reading with the palm on the glass)
      // Toggles the existing Settings -> Controls option, nothing lower-level:
      // that setting only governs the reader's tap/swipe handling, so the
      // panel's own gestures (including the swipe that reopens it) keep
      // working while it is off. Off remembers the mode (Tap/Swipe/Inverted
      // Tap) so toggling back does not stomp the user's choice.
      if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) {
        touchModeRestore = SETTINGS.touchReaderControls;
        SETTINGS.touchReaderControls = CrossPointSettings::TOUCH_READER_OFF;
      } else {
        SETTINGS.touchReaderControls = touchModeRestore;
      }
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    default:
      break;
  }
}

void FrontlightPanelActivity::adjustBrightness(const int delta) {
  int next = static_cast<int>(brightness) + delta;
  if (next < MIN_BRIGHTNESS) next = MIN_BRIGHTNESS;
  if (next > 100) next = 100;
  if (next == brightness) return;
  brightness = static_cast<uint8_t>(next);
  Frontlight.setBrightness(brightness);
  if (!lightOn && !slideSwitchOwnsLight()) {
    lightOn = true;
    lightOnChanged = true;
    Frontlight.setOn(true);
  }
  requestAdjustRepaint();
}

void FrontlightPanelActivity::adjustWarmth(const int delta) {
  int next = static_cast<int>(warmth) + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == warmth) return;
  warmth = static_cast<uint8_t>(next);
  Frontlight.setWarmth(warmth);
  requestAdjustRepaint();
}

void FrontlightPanelActivity::toggleLight() {
  lightOn = !lightOn;
  lightOnChanged = true;
  Frontlight.setOn(lightOn);
  // Same panel-local repaint as an adjustment: the lamp glyph is a small diff,
  // but the row-9 completion would still be a whole-panel second drive.
  requestAdjustRepaint();
}

void FrontlightPanelActivity::requestAdjustRepaint() {
  const flpanel::Plan p = flpanel::plan(flpanel::Event::Adjust, exparms::panelPartialRefresh());
  // The promotion is what makes this the panel's own decision: requestUpdate()
  // only applies the in-screen policy when nothing is armed (RefreshPolicy.h
  // apply() returns early on hasPromotedRefresh()), so an explicit FAST outranks
  // REFRESH_AUTO's measured ratio and any user-chosen FULL/GL16 for menus.
  // render() still asks for FAST itself; this is the term that survives the
  // producer and the latch rule.
  if (p.promoteFast) renderer.promoteNextRefresh(HalDisplay::FAST_REFRESH);
  if (p.suppressCompletion) renderer.suppressCompletionDriveOnce();
  // FLP2: pin this paint to a diff-masked DU whatever the frame underneath
  // carries and whatever ambient term (rule 3 after a reader FULL, the shim's
  // 24-partial clean cadence) would try to escalate it. One-shot, consumed
  // below the entry gate, exactly like the FLP1 terms above.
  if (p.binaryFast) renderer.forceBinaryDuOnce();
  if (p.requestPaint) requestUpdate();
}

void FrontlightPanelActivity::close() { finish(); }

void FrontlightPanelActivity::exitPanel() {
  const flpanel::Plan p = flpanel::plan(flpanel::Event::Exit, exparms::panelPartialRefresh());
  if (!p.cleanOnExit) {
    close();
    return;
  }
  // The proven sequence from runTile()'s ghost-cleanup tile, in this order:
  //   1. close() -- popActivity() applies the activity-transition policy, which
  //      may arm a promotion of its own (RefreshPolicy.h apply()),
  //   2. cancelPromotedRefresh() -- drop that arm, so the promotion below is the
  //      one the paint underneath consumes (cancel also clears the AUTO bit,
  //      which otherwise OUTRANKS the promotion in resolveRefreshRequest()),
  //   3. promoteNextRefresh(FULL) -- arm ONE from-white GC16 for that paint.
  // Under the render lock: the render task paints this activity, and a paint of
  // the panel landing between close() and (3) would consume the FULL on the
  // overlay instead of on the screen underneath.
  // FULL is a from-white GC16 in the panel shim (metalio_vendor_panel.cpp,
  // epd_hl_update_screen_from_white), i.e. a whole-panel drive the diff path
  // cannot short-circuit, and an explicit FULL survives cp_refresh_resolve_mode()
  // (rule 1 keeps CP_RM_FULL: it *is* the strongest clean). The paint it lands on
  // is the destination screen, not a black panel, so nothing flashes in between.
  // A promotion is a FLAG, not a counter: close() being reached twice before the
  // pop lands (a swipe and a release in one loop pass) still arms ONE FULL.
  {
    RenderLock lock;
    close();
    renderer.cancelPromotedRefresh();
    renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
  }
  LOG_INF("UI", "panel exit: one from-white FULL armed for the repaint underneath (flpanel=%d)",
          exparms::panelPartialRefresh() ? 1 : 0);
}

bool FrontlightPanelActivity::handleHomeGesture() {
  exitPanel();
  return true;
}

void FrontlightPanelActivity::loop() {
  const auto touch = routeTouch(mappedInput, false, /*routeHeld=*/true);
  if (touch.routed) {
    // FLP1: the slider's per-drag repaint is one FAST/DU drive with no
    // completion. Today this was a bare requestUpdate(), whose promotion was
    // whatever the in-screen setting asked for -- and the row-9 GL16 completion
    // was appended on top of it either way (two whole-panel drives per percent).
    if (app.invalidated()) requestAdjustRepaint();
    if (touch) {
      if (touch.event.dragPermille >= 0) draggingSlider = true;
      return;
    }
    // Swipe up dismisses the sheet, the way it was pulled down from the top
    // edge. draggingSlider keeps a fast slider flick from closing it.
    if (!draggingSlider && mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Up) {
      exitPanel();
      return;
    }
    // panelBottom > 0 guards the frame the sheet opens in: the release that
    // opened it (a status-bar tap) is still in the input snapshot when the panel
    // runs its first loop(), and panelBottom is only known once render() has
    // measured the layout — so at 0 that release read as "tapped below the
    // sheet" and closed it again before it was ever drawn.
    if (touch.snap.touchReleased && !draggingSlider && panelBottom > 0 && touch.snap.touchY >= panelBottom) {
      exitPanel();
      return;
    }
  }
  if (draggingSlider) {
    if (!touch.snap.touchHeld) {
      draggingSlider = false;
      // arm on: nothing is left to draw, so the lift drives nothing (the exit
      // clean erases the DU ghosting). arm off: today's HALF "clean" on the lift.
      const flpanel::Plan release = flpanel::plan(flpanel::Event::DragRelease, exparms::panelPartialRefresh());
      if (release.halfClean) cleanRefreshPending = true;
      if (release.requestPaint) requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    exitPanel();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!slideSwitchOwnsLight()) toggleLight();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left},
                                       [this] { adjustBrightness(-BRIGHTNESS_STEP); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                       [this] { adjustBrightness(BRIGHTNESS_STEP); });
}

int FrontlightPanelActivity::computePanelBottom() const {
  const auto tokens = uiThemeTokens(uiTarget);
  const int16_t lineHeight = uiTarget.lineHeight(tokens.smallText.font);
  // The header band + the air around it. r77 draws the THEME's titled band here
  // (twice the home status band on Andro), so this mirrors
  // buildPanelScreen()'s spacer + takeTop(getTitledHeaderHeight(), spaceMd) --
  // a stale slim band would let the card's content run past its bottom edge.
  int y = tokens.spaceMd + UITheme::getInstance().getTitledHeaderHeight() + tokens.spaceMd;
  if (Frontlight.present()) {
    // Screen::sliderRow reserves caption + spaceMd + control band, then a
    // spaceMd gap; addSliderRow() adds one more spaceMd of air after each row.
    y += lineHeight + tokens.spaceMd + kSliderRowHeight + 2 * tokens.spaceMd;  // brightness
    if (Frontlight.hasColorTemperature()) {
      y += lineHeight + tokens.spaceMd + kSliderRowHeight + 2 * tokens.spaceMd;  // warmth
    }
    y += tokens.spaceSm;
  }

  // Tiles are touch targets, so a buttons-only board gets no grid and the
  // sheet is exactly the frontlight controls.
  const int tileCount = mappedInput.hasTouch() ? kTileCount : 0;
  y += fui::tileGridHeight(static_cast<uint16_t>(tileCount), kTileCols, kTileHeight, kTileGap);
  // The sheet's grabber band: content margin + grabber + air to the edge.
  // buildPanelScreen() feeds the same theme spacings into SheetProps.
  y += tokens.spaceLg + kGrabberHeight + tokens.spaceLg + tokens.spaceMd;
  return y;
}

void FrontlightPanelActivity::panelScreen(UiScreen& screen, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->buildPanelScreen(screen);
}

void FrontlightPanelActivity::addSliderRow(UiScreen& screen, const char* label, const uint8_t value,
                                           const fui::ActionId sliderAction, const fui::ActionId stepAction,
                                           const bool showToggle) {
  // Live percentage readout. The row draws before this call returns
  // (immediate mode), so borrowing a stack buffer is safe.
  char pct[8];
  snprintf(pct, sizeof(pct), "%u%%", static_cast<unsigned>(value));

  // rowProps is a member (fui::SliderRowProps embeds a 324-byte StyleSet, well
  // past the 256-byte budget a local gets — AGENTS.md). Every field that
  // varies between the two rows is reassigned here; the rest keep their
  // constructed defaults, which already match the panel's card language.
  rowProps.label = label;
  rowProps.value = pct;
  rowProps.sliderValue = value;
  // rowProps is a shared member, so every field a row depends on must be re-stated here: a row that
  // leaves `max` to whatever the previous row set renders a 30% level as a full capsule.
  rowProps.max = 100;
  rowProps.sliderAction = sliderAction;
  rowProps.decrement = stepAction;
  rowProps.increment = stepAction;
  rowProps.decrementValue = -BRIGHTNESS_STEP;
  rowProps.incrementValue = BRIGHTNESS_STEP;
  if (showToggle) {
    // Lamp on/off after the +: the sliders set the level, this kills the light
    // outright. Filled glyph = on, outline = off.
    rowProps.toggleAction = ACTION_TOGGLE;
    rowProps.toggleIcon = fui::bitmapFromIcon(lightOn ? icon_sun_filled_32 : icon_sun_32);
  } else {
    rowProps.toggleAction = fui::NO_ACTION;
    rowProps.toggleIcon = fui::BitmapRef{};
  }
  screen.sliderRow(rowProps, kSliderRowHeight);
  // The wrapper's own trailing gap is one spaceMd; double it so the rows
  // breathe — a control band this tall reads cramped at the list cadence.
  screen.spacer(screen.theme().spaceMd);
}

void FrontlightPanelActivity::buildPanelScreen(UiScreen& screen) {
  const auto& theme = screen.theme();

  // Sheet chrome first: the card body, the 2px rule along its bottom edge, and
  // the grabber on the edge the sheet is dragged from. Screen::sheet() also
  // clamps the content area to the sheet, so every band below lays out inside
  // it. (No header: the panel is a floating card, and its own grabber says
  // what it is.)
  fui::SheetProps sheetProps;
  // A roomy band above the bottom rule: the grabber gets a full spaceLg of
  // air on both sides so the last row of content never crowds the sheet edge.
  sheetProps.grabberMargin = theme.spaceLg;
  sheetProps.grabberInset = static_cast<int16_t>(theme.spaceLg + theme.spaceMd);
  screen.sheet(sheetProps, static_cast<int16_t>(panelBottom));
  screen.insetContent(fui::Insets{0, kPanelSideMargin, 0, kPanelSideMargin});

  // r77: the THEME's titled header, not BaseTheme::drawHeader on a bare status
  // strip. Calling the base directly skipped the active theme's own band, so this
  // panel wore the SDK status cluster on every theme while the rest of the tree
  // wore the theme's titled band (doubled app bar + 6 px rule on Andro, and a
  // theme title face that a non-Latin title can actually be drawn in).
  {
    const auto& metrics = UITheme::getInstance().getMetrics();
    screen.spacer(theme.spaceMd);
    const int16_t bandH = static_cast<int16_t>(UITheme::getInstance().getTitledHeaderHeight());
    screen.takeTop(bandH, theme.spaceMd);
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), bandH}, tr(STR_FRONTLIGHT));
  }

  if (Frontlight.present()) {
    addSliderRow(screen, tr(STR_BRIGHTNESS), brightness, ACTION_BRIGHTNESS, ACTION_BRIGHTNESS_STEP,
                 /*showToggle=*/!slideSwitchOwnsLight());
    if (Frontlight.hasColorTemperature()) {
      addSliderRow(screen, tr(STR_WARMTH), warmth, ACTION_WARMTH, ACTION_WARMTH_STEP, /*showToggle=*/false);
    }
    screen.spacer(theme.spaceSm);
  }

  // Quick-setting tiles. Two columns of finger-sized cards; a tile whose
  // setting is currently on draws filled (StateChecked -> selected style).
  // Touch boards only — the tiles are touch targets.
  if (mappedInput.hasTouch()) {
    static constexpr StrId kOrientNames[4] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                              StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
    // The orientation tile is labelled with just the current mode ("Portrait"):
    // the mode names say what the tile is about on their own.
    const char* orientLabel = I18N.get(kOrientNames[SETTINGS.orientation % 4]);
    // "Touch On" / "Touch Off", from the existing state strings: the label
    // names the current state of the touch-reader-controls setting.
    const bool touchOn = SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF;
    char touchLabel[48];
    snprintf(touchLabel, sizeof(touchLabel), "%s %s", tr(STR_TOUCH_TOGGLE),
             I18N.get(touchOn ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF));

    const char* labels[kTileCount] = {tr(STR_NIGHT_MODE), tr(STR_FORCE_REFRESH), orientLabel, touchLabel};
    const fui::State states[kTileCount] = {SETTINGS.screenInverted ? fui::StateChecked : fui::StateNormal,
                                           fui::StateNormal, fui::StateNormal,
                                           // Filled when touch reader controls are OFF — the non-default,
                                           // attention-worthy state.
                                           touchOn ? fui::StateNormal : fui::StateChecked};

    for (int id = 0; id < kTileCount; ++id) {
      gridItems[id].label = labels[id];
      gridItems[id].value = static_cast<int16_t>(id);
      gridItems[id].state = states[id];
    }
    gridProps.items = gridItems;
    gridProps.count = static_cast<uint16_t>(kTileCount);
    gridProps.action = ACTION_TILE;
    gridProps.tileHeight = kTileHeight;
    gridProps.gap = kTileGap;
    screen.tileGrid(gridProps);
  }
}

void FrontlightPanelActivity::render(RenderLock&&) {
  panelBottom = computePanelBottom();

  // fui::sheet draws the card body, its bottom rule, and the grabber during
  // renderUi(); the battery band at the card's top is part of the screen build.
  renderUi();

  // A tile that rewrote the whole frame (night mode) re-drives every pixel
  // once; ordinary repaints stay on the fast path. HALF: strong enough to
  // flip the whole frame's polarity without the FULL waveform's blackout
  // flash. Any faint residue clears with the panel's dedicated refresh tile
  // or the next scheduled clean refresh.
  //
  // FLP1: for an adjustment this call stays FAST. The promotion armed by
  // requestAdjustRepaint() outranks the AUTO ratio and any user-chosen menu
  // setting in the producer (applyPromotedRefresh / resolveRefreshRequest),
  // and the row-9 completion is suppressed for the same paint, so an
  // adjustment is ONE DU drive and no clean waveform. The single exception is
  // the policy's rule 3 (no HALF after a from-white FULL): the FIRST paint
  // after a FULL is resolved to HALF by design. That paint writes the latch
  // back to FAST (it is the requested mode, not the resolved one), so it
  // happens at most once, and the panel's own paints never leave the latch at
  // FULL, so it cannot land on an adjustment.
  renderer.displayBuffer(cleanRefreshPending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  cleanRefreshPending = false;
}
