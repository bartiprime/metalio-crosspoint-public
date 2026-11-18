#include "PowerOffActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cmath>
#include <string>

#include "CrossPointSettings.h"
#include "HalDisplay.h"
#include "HapticFeedback.h"
#include "MetalioPowerRequest.h"
#include "PowerOffDialogFrame.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/ImageFileResolver.h"

namespace {
// The two labels are literals on purpose: there is no STR_POWER_OFF in the string table, and a
// translated "Cancel" already exists next to them via STR_CANCEL.
constexpr const char* kTitle = "Power off?";

// r101.10 PWR3: did a DRIVEN paint of the power-off frame reach the glass? Read by main.cpp's
// shutdown path immediately before the park, so "park an unpainted panel" cannot happen silently.
// Weak probe (the panel shim's own drive counter, main.cpp): absent on a host build, where the
// handshake below degenerates to "assume it landed" and nothing else changes.
extern "C" uint32_t cp_panel_drive_seq(void) __attribute__((weak));
bool g_powerOffFrameDriven = false;

// ---------------------------------------------------------------------------------------------
// POSCR1: the power-off screen may be an image from /poweroff/, the way the sleep screen reads
// /sleep (and /.sleep). The last painted frame IS what stays on the glass after the rails drop,
// so this paint is the only chance the device gets to draw it.
//
// WHAT "CUSTOM" COSTS AND WHY IT IS THE RICH PATH. The operator chose the 16-level GRAY16 path and
// accepted the shutdown cost. That path is: clear the canvas, draw the bitmap with GfxRenderer's
// 16-level canvas writes (setSixteenLevelImages), then drive the whole panel from white --
// FULL_REFRESH, i.e. METALIO_PANEL_REFRESH_FULL == epd_hl_update_screen_from_white(MODE_GC16) on
// this board. That one from-white GC16 IS the clean + G16 ramp + erase sweep the operator sees;
// with the decode in front of it the shutdown spends ~2.8 s. No 2-bit shortcut is used anywhere: a
// cheap diff-area drive over the frame that will be the only thing on a powered-down device would
// ghost, and nothing can repaint it afterwards.
// ---------------------------------------------------------------------------------------------
constexpr char POWER_OFF_DIR[] = "/poweroff";

// Exact-name candidates, in that order. A poweroff.bmp wins everything: it is the file the docs
// tell the operator to place, and it paints with no convert and no cache write.
constexpr const char* POWER_OFF_PREFERRED[] = {"/poweroff/poweroff.bmp", "/poweroff/poweroff.png",
                                               "/poweroff/poweroff.jpg"};

// Below this the shutdown must not spend seconds decoding a wallpaper: TEXT is painted instead,
// whatever the setting says. Same percentage the status bar shows.
constexpr uint8_t kPowerOffImageMinBatteryPercent = 5;

// GRAY16 scope: the flag covers the draw and nothing else, and every exit from the image paint
// restores it, so no later paint inherits 16-level drawing by accident. Same contract as
// SleepActivity's Gray16SleepPaint -- if that one changes, this one must follow.
struct Gray16ImagePaint {
  explicit Gray16ImagePaint(const GfxRenderer& r)
      : renderer(r), previous(r.sixteenLevelImages()), engaged(pixfmt::kGray16Sleep && GfxRenderer::fourBitCanvas()) {
    renderer.setSixteenLevelImages(engaged);
  }
  ~Gray16ImagePaint() { renderer.setSixteenLevelImages(previous); }
  bool drivesSixteen() const { return engaged; }
  Gray16ImagePaint(const Gray16ImagePaint&) = delete;
  Gray16ImagePaint& operator=(const Gray16ImagePaint&) = delete;

 private:
  const GfxRenderer& renderer;
  const bool previous;
  const bool engaged;
};

// One whole-panel from-white drive, then block until the frame is on the glass. The block is the
// second half of the shutdown handshake: confirmPowerOff() raises the request only after this
// returns.
void driveFrameAndWait(GfxRenderer& renderer) {
  // A wholesale screen replacement, so an armed one-shot promotion (AUTO/HALF from the refresh
  // policy) must not downgrade it to a diff-area drive: cancel, then commit FULL.
  renderer.cancelPromotedRefresh();
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  renderer.waitRefreshComplete();
}

// r101.10 PWR2: THE POST-CONFIRM FRAME IS A DIALOG, NOT A BLANK PAGE.
//
// What shipped until r101.9 was a cleared screen with one centred UI_12 bold line. On glass that
// reads as a mostly-empty page, and the operator reported it as "still missing the powering off
// dialog". The frame now carries the same chrome as every other dialog in the tree
// (BaseTheme::drawPopup: an ink ring around a solid box, the status word inside it), drawn from the
// theme's own scaled popup metrics and a real, larger font id -- no pixel literals and no second
// popup implementation. TEXT/COVER fall through to white-ink, DARK asks for the inverted pair, so a
// black power-off screen shows a white box instead of nothing.
void paintStatusDialogFrame(GfxRenderer& renderer, const bool darkInk) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const BaseTheme& theme = UITheme::getInstance().getTheme();
  // A real font id, one step up from the UI_12 the popup chrome draws its labels in. Guarded: a
  // face that is not in this image must not collapse the last frame of the session, so the UI_12
  // fallback keeps the box (and the words in it) whatever the build carries.
  int fontId = renderer.getLineHeight(NOTOSANS_18_FONT_ID) > 0 ? NOTOSANS_18_FONT_ID : UI_12_FONT_ID;
  // The theme's dialog readout face (Andro: Ember 18), the same one a slider dialog's headline
  // uses, and only when the loaded face covers every codepoint of the string.
  const int themeFace = theme.dialogReadoutFontId();
  if (themeFace != 0 && BaseTheme::textFitsFace(renderer, themeFace, kPowerOffFrameText)) {
    fontId = themeFace;
  }
  // PWR9: the composition itself lives in PowerOffDialogFrame.h, templated on the renderer, so the
  // host test (test/poweroff_text_frame) paints THIS sequence onto a 1-bit surface. It is the same
  // ring/box/word as before; the only change is that the word's ink now comes from the box colour.
  // The frame weight follows the theme's dialog rule, so this box is framed like every other dialog
  // on a theme that states one (Andro: 6 px); 0 keeps popupFrameThickness.
  paintPowerOffDialogFrame(renderer, metrics, fontId, kPowerOffFrameText, darkInk, EpdFontFamily::BOLD,
                           theme.dialogRuleThickness());
}

// The frame every build before POSCR1 shipped, now drawn as a dialog. `darkInk` is the whole
// difference between the TEXT/LIGHT frame and the DARK one.
void paintTextFrame(GfxRenderer& renderer, const bool darkInk) {
  renderer.clearScreen(darkInk ? 0x00 : 0xFF);
  paintStatusDialogFrame(renderer, darkInk);
  driveFrameAndWait(renderer);
  LOG_INF("PWR", "power-off dialog frame DRIVEN (ink=%s font=%d box_chrome=popup)", darkInk ? "light-on-dark" : "dark-on-light",
          renderer.getLineHeight(NOTOSANS_18_FONT_ID) > 0 ? NOTOSANS_18_FONT_ID : UI_12_FONT_ID);
}

// Fit the image inside the panel without cropping (the power-off frame has no cover semantics) and
// centre it. Same geometry as SleepActivity::calculateBitmapPlacement() with crop = 0.
void fitCentered(const int bitmapWidth, const int bitmapHeight, const GfxRenderer& renderer, int& x, int& y) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  if (bitmapWidth <= pageWidth && bitmapHeight <= pageHeight) {
    x = (pageWidth - bitmapWidth) / 2;
    y = (pageHeight - bitmapHeight) / 2;
    return;
  }
  const float ratio = static_cast<float>(bitmapWidth) / static_cast<float>(bitmapHeight);
  const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
  if (ratio > screenRatio) {
    x = 0;
    y = static_cast<int>(std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2));
  } else {
    y = 0;
    x = static_cast<int>(std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2));
  }
}

bool pickPowerOffImage(std::string& srcPath) {
  // 1. The exact name, in the order the docs name the files.
  for (const char* candidate : POWER_OFF_PREFERRED) {
    if (!Storage.exists(candidate)) continue;
    HalFile probe;
    if (!Storage.openFileForRead("PWR", candidate, probe)) continue;
    const bool valid = imagefile::isValidImageHeader(probe, imagefile::formatOf(candidate), "PWR");
    probe.close();
    if (valid) {
      srcPath = candidate;
      LOG_INF("PWR", "power-off image: %s (preferred name)", candidate);
      return true;
    }
    LOG_ERR("PWR", "power-off image %s is not a usable image; ignoring it", candidate);
  }

  // 2. Any supported file in the folder, chosen at random.
  if (!Storage.exists(POWER_OFF_DIR)) {
    LOG_INF("PWR", "power-off image: %s is not on the card; using the \"Powered off\" frame", POWER_OFF_DIR);
    return false;
  }
  if (!imagefile::pickRandomImageInDir(imagefile::kPowerOffImageCache, POWER_OFF_DIR, srcPath)) {
    LOG_INF("PWR", "power-off image: no usable file in %s; using the \"Powered off\" frame", POWER_OFF_DIR);
    return false;
  }
  LOG_INF("PWR", "power-off image: %s (random from %s)", srcPath.c_str(), POWER_OFF_DIR);
  return true;
}

// Returns false when the caller must paint TEXT. Every false return has already logged why, and
// none of them has driven the panel yet: the text frame clears and drives from white, so a partial
// canvas left behind by a failed draw is wiped by the frame that replaces it.
bool paintCustomImageFrame(GfxRenderer& renderer) {
  const uint16_t battery = powerManager.getBatteryPercentage();
  if (battery < kPowerOffImageMinBatteryPercent) {
    LOG_INF("PWR", "power-off image skipped: battery %u%% < %u%%; using the \"Powered off\" frame",
            static_cast<unsigned>(battery), static_cast<unsigned>(kPowerOffImageMinBatteryPercent));
    return false;
  }

  std::string srcPath;
  if (!pickPowerOffImage(srcPath)) return false;

  if (!FsHelpers::hasBmpExtension(std::string_view{srcPath})) {
    // The converter needs heap for a full frame. The SD font caches are the largest thing this
    // activity can hand back, and the image frame draws no text; the text fallback reloads what it
    // needs. The same release the sleep screen performs before a decode.
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
  }

  std::string bmpPath;
  if (!imagefile::resolveBmpPath(imagefile::kPowerOffImageCache, srcPath, renderer.getScreenWidth(),
                                 renderer.getScreenHeight(), /*cropped=*/false, bmpPath)) {
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("PWR", bmpPath, file)) {
    LOG_ERR("PWR", "Power-off image BMP did not open: %s; using the \"Powered off\" frame", bmpPath.c_str());
    return false;
  }

  // The two arguments the sleep screen's bitmap paint passes on this board: dithering on (the
  // 16-level nibble comes from pixfmt::ditherLevel16), originalThresholds off (sleep only turns it
  // on for a gray-CAPABLE panel, and HalDisplay reports ED047TC2 as not gray-capable -- see
  // HalDisplay::grayscaleCapabilities). So a power-off image is toned exactly like a sleep image.
  Bitmap bitmap(file, /*dithering=*/true, /*originalThresholds=*/false);
  const auto parseResult = bitmap.parseHeaders();
  if (parseResult != BmpReaderError::Ok) {
    LOG_ERR("PWR", "Power-off image BMP invalid (%s): %s; using the \"Powered off\" frame", bmpPath.c_str(),
            Bitmap::errorToString(parseResult));
    file.close();
    return false;
  }

  int x = 0;
  int y = 0;
  fitCentered(bitmap.getWidth(), bitmap.getHeight(), renderer, x, y);

  // THE 16-LEVEL PAINT. clearScreen() first: a from-white GC16 assumes every pixel is driven, and
  // the bitmap's own white must end up as paper, not as whatever the dialog left under it.
  {
    Gray16ImagePaint gray16Paint(renderer);
    renderer.clearScreen();
    if (!renderer.drawBitmap(bitmap, x, y, renderer.getScreenWidth(), renderer.getScreenHeight(), 0.0f, 0.0f,
                             /*whiteAsTransparent=*/false)) {
      LOG_ERR("PWR", "Power-off image draw failed: %s; using the \"Powered off\" frame", bmpPath.c_str());
      file.close();
      return false;
    }
    // Nothing may downgrade this frame to a diff-area drive: the one-shot clean is asked for
    // explicitly as well as FULL, exactly like the sleep screen's 16-level paint.
    renderer.forceCleanNextRefresh();
    driveFrameAndWait(renderer);
    LOG_INF("PWR", "power-off image frame: %s (%dx%d at %d,%d) sixteen=%d levels=%d", bmpPath.c_str(),
            bitmap.getWidth(), bitmap.getHeight(), x, y, gray16Paint.drivesSixteen() ? 1 : 0,
            gray16Paint.drivesSixteen() ? 16 : 4);
  }
  file.close();
  return true;
}
}  // namespace

bool metalioPowerOffFrameDriven() { return g_powerOffFrameDriven; }

// r101.21 PWR4: WHICH frame the last metalioPaintPoweredOffFrame() actually put on the glass.
// "image" only when the CUSTOM branch completed its own decode+drive; every failure of that branch
// lands on the TEXT fall-through, which is what this label then reads. main.cpp's auto-off journal
// line prints it, so "the operator's power-off image did not appear" can be confirmed from the
// sleep journal instead of demoed at the device.
static const char* g_powerOffFrameLabel = "none";
const char* metalioPowerOffFrameLabel() { return g_powerOffFrameLabel; }

unsigned long metalioPaintPoweredOffFrame(GfxRenderer& renderer) {
  const unsigned long start = millis();
  // POSCR1: one decision, five outcomes. TEXT (the default) is the frame this build has always
  // painted. DARK/LIGHT/BLANK never touch the card, so each costs one drive and nothing else.
  // CUSTOM tries the image and falls through to TEXT on every failure -- this frame is the last
  // thing the device shows, so "something legible" always wins over "nothing".
  bool painted = false;
  g_powerOffFrameLabel = "text";  // the fall-through below is the default frame
  switch (SETTINGS.powerOffScreen) {
    case CrossPointSettings::POWER_OFF_CUSTOM:
      painted = paintCustomImageFrame(renderer);
      if (painted) g_powerOffFrameLabel = "image";
      break;
    case CrossPointSettings::POWER_OFF_DARK:
      // r101.10 PWR2: the dark frame is the same dialog with the inks swapped, not a bare black
      // panel -- on black a label-less screen is indistinguishable from a dead device.
      paintTextFrame(renderer, /*darkInk=*/true);
      painted = true;
      g_powerOffFrameLabel = "dark";
      break;
    case CrossPointSettings::POWER_OFF_LIGHT:
      // The light counterpart of DARK: the standard dialog frame.
      paintTextFrame(renderer, /*darkInk=*/false);
      painted = true;
      g_powerOffFrameLabel = "light";
      break;
    case CrossPointSettings::POWER_OFF_BLANK:
      renderer.clearScreen();
      driveFrameAndWait(renderer);
      painted = true;
      g_powerOffFrameLabel = "blank";
      break;
    case CrossPointSettings::POWER_OFF_TEXT:
    default:
      break;
  }
  if (!painted) paintTextFrame(renderer, /*darkInk=*/false);
  return millis() - start;
}

void PowerOffActivity::onEnter() {
  Activity::onEnter();

  // r100.3: input is armed by loop() on the first pass that carries no button edge and has the
  // power key up -- the press that opened this dialog must never be able to answer it.
  inputArmed = false;
  shownAtMs = millis();
  g_powerOffFrameDriven = false;  // r101.10 PWR3: a new dialog owes a new frame

  // Cancel first: a stray Confirm on a 4 s hold must not be able to end the device.
  const char* options[] = {I18N.get(StrId::STR_CANCEL), "Power off"};
  confirmPopup.show(kTitle, options, 2, 0, [this](const int idx) {
    if (idx == 1) {
      confirmPowerOff();
      return;
    }
    finish();
  });

  requestUpdate(true);
}

void PowerOffActivity::confirmPowerOff() {
  state = State::SHUTTING_DOWN;
  // Paint through the ordinary render path and block until the frame is on the glass. This is the
  // last paint of the session: the shutdown below it deliberately paints nothing.
  // POSCR1: for POWER_OFF_CUSTOM the frame IS the /poweroff/ image, drawn on the 16-level path
  // inside metalioPoweredOffFrame(); the wait therefore covers the decode, the cache write (first
  // time only) and the from-white GC16. The request below is still raised only after that wait
  // returns -- the handshake is unchanged, and it is what guarantees the image has landed on the
  // glass before the rails drop.
  const uint32_t drivesBefore = (cp_panel_drive_seq != nullptr) ? cp_panel_drive_seq() : 0;
  // A-P1-6: the wait now reports whether a paint actually landed. It is recorded
  // here and NOT acted on -- the hardware check below is still what decides, and
  // neither of them may refuse to power off on a merely slow paint.
  const bool waitDriven = requestUpdateAndWait();
  requested = true;

  // r101.10 PWR3: THE PARK MUST NOT LAND ON AN UNPAINTED PANEL. requestUpdateAndWait() now waits
  // for a DRIVEN paint of the current screen (see ActivityManager), and this second, hardware-side
  // check reads the panel shim's own drive counter, so a wait that returned without a drive is
  // caught here rather than trusted. main.cpp re-paints the frame before parking if this is false.
  g_powerOffFrameDriven = (cp_panel_drive_seq == nullptr) || (cp_panel_drive_seq() != drivesBefore);
  if (g_powerOffFrameDriven) {
    LOG_INF("PWR", "power-off frame verified: a panel drive landed after the wait (drives=%lu)",
            static_cast<unsigned long>(cp_panel_drive_seq ? cp_panel_drive_seq() : 0));
  } else {
    LOG_ERR("PWR", "power-off frame NOT driven after the wait (wait verdict=%d); main.cpp will paint it before parking",
            waitDriven ? 1 : 0);
  }
  // HAPT2 (r101.9): stash the power-off cue -- TWO 100 ms pulses, 100 ms apart -- in the shared
  // deferred haptic mailbox (src/HapticFeedback.h). Nothing is pulsed on this stack: the drain
  // point at the top of loop() fires it, and that point is above the `metalioPowerOffRequested()`
  // check in the same iteration, so the order is pulse, pulse, shutdown. Stashing AFTER the wait
  // is deliberate: the cue is the last thing the operator feels, and it must not be heard before
  // the "Powered off" frame has reached the glass.
  haptic_feedback::deferPowerOffPattern();
  metalioRequestPowerOff();
}

void PowerOffActivity::onExit() {
  Activity::onExit();
  // Either answer closes the dialog; the hold classifier must be allowed to classify again or a
  // key still down would re-open this every 4 s.
  metalioPowerConfirmClosed(true);
}

void PowerOffActivity::loop() {
  // Once the request is up, main's loop() owns the shutdown; do nothing that could repaint.
  if (state == State::SHUTTING_DOWN) return;

  // THE SECOND HALF OF PRESS OWNERSHIP (the first is main's swallowNextRelease at push time).
  // The dialog is pushed at the 3 s mark, while the key is still down, so the release that ends
  // that press arrives with this screen already current. Whether it is seen here depended on how
  // long past the threshold the operator let go -- the intermittency. This gate removes the
  // dependency instead of narrowing it: input is accepted only on a pass where the power key is
  // UP *and* no button edge is present, i.e. strictly after the summoning press is over. Both
  // terms are levels/one-shot edges sampled per pass, so no ordering can deliver a press edge into
  // the dialog. The 3 s cap is the anti-brick term for a stuck key.
  if (!inputArmed) {
    const bool powerDown = mappedInput.isPressed(MappedInputManager::Button::Power);
    const bool anyEdge = mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
                         mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
                         mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                         mappedInput.wasReleased(MappedInputManager::Button::Back) ||
                         mappedInput.wasPressed(MappedInputManager::Button::Power) ||
                         mappedInput.wasReleased(MappedInputManager::Button::Power) ||
                         mappedInput.wasScreenTouchReleased();
    if (powerDown || anyEdge) {
      if (millis() - shownAtMs > 3000) {
        inputArmed = true;
        LOG_INF("PWR", "power-off dialog: arming input with the key still down (3 s cap)");
      } else {
        return;
      }
    } else {
      inputArmed = true;
      LOG_INF("PWR", "power-off dialog: input armed %lu ms after it opened", millis() - shownAtMs);
    }
  }

  if (confirmPopup.handleInput(mappedInput, [this] {
        if (confirmPopup.isActive()) requestUpdate();
      }))
    return;

  // Popup dismissed without a selection (Back button or tap outside): cancel.
  finish();
}

void PowerOffActivity::render(RenderLock&&) {
  // The shutdown request is already up and the final frame is on the glass. OptionPopup fires its
  // requestUpdate() after the select callback, so a second identical FULL drive of this frame would
  // otherwise be queued behind metalioRequestPowerOff(). The waiter notify lives in
  // ActivityManager::renderTaskLoop (it notifies after render() returns, painted or not), so
  // returning early here still satisfies the requestUpdateAndWait() contract.
  if (requested) return;

  if (state == State::CONFIRM) {
    renderer.clearScreen();
    // NOTHING TO SHOW YET (r100.4). render() is reachable before onEnter() has run: the manager
    // publishes the new activity, then calls onEnter(), and the render task can win that race.
    // The popup is shown INSIDE onEnter(), so a paint that arrives first has no dialog to draw --
    // and the code below used to drive that empty page as a whole-panel HALF refresh, i.e. a blank
    // flash (and, once the gate withheld it, a frame counted as painted that never landed). Return
    // without a drive: the paint that onEnter() (and, with the paint debt, the transition) asks for
    // carries the real dialog.
    if (!confirmPopup.isActive()) {
      // r100.5: this paint was consumed building NOTHING -- onEnter() (which creates the popup) has
      // not run yet. Decline it explicitly rather than returning silently:
      //   * the manager reads paintDeclinedFrame and does NOT record this as the dialog's frame, so
      //     the paint it still owes stays owed and the watchdog re-requests it;
      //   * ask for that paint here as well, so the dialog lands on the first pass that has a popup
      //     to draw instead of waiting for the watchdog's first step.
      // This is the only render() in the tree that can compose without driving, and it is why
      // "the dialog is entered but only appears when a key is pressed" was invisible in the log.
      paintDeclinedFrame = true;
      requestUpdate();
      return;
    }

    if (confirmPopup.processRender(renderer, mappedInput)) {
      logFirstPaint();
      return;
    }
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    logFirstPaint();
    return;
  }

  // SHUTTING_DOWN. A wholesale screen replacement, so an armed one-shot promotion (AUTO/HALF from
  // the refresh policy) must not downgrade it to a diff-area drive: the shared painter cancels and
  // commits FULL. POSCR1: that painter is also what turns SETTINGS.powerOffScreen into the frame --
  // text, /poweroff/ image, dark, light or blank -- and it is the same entry point the standby
  // auto-off path in main.cpp uses, which is why it lives outside render().
  metalioPaintPoweredOffFrame(renderer);
}

// The press that summoned this dialog started in main.cpp's classifier; the frame lands here, on
// the render task. Both numbers are read from the same DOWN stamp (MetalioPowerRequest.h), so
//   decision at N ms   -- how long the hold really was before the dialog was decided (the 3000 ms
//                         threshold, measured);
//   painted at M ms    -- from that DOWN edge to the first frame of the dialog on the glass;
//   gap                -- M - N, the paint latency. This is the number that explains "the hold
//                         feels like 5 s": a large gap is time the operator spends holding a key
//                         while the screen still shows the old frame.
// Silent when the dialog was opened by anything other than a classified press (no DOWN stamp) and
// when this paint did not reach the glass (entry gate withheld the drive).
void PowerOffActivity::logFirstPaint() {
  if (firstPaintLogged || renderer.lastPaintAttemptHeld()) return;
  firstPaintLogged = true;
  const unsigned long down = metalioPowerDownEdgeMs();
  const unsigned long decided = metalioPowerDialogDecisionMs();
  if (down == 0) return;
  const unsigned long decisionMs = decided != 0 ? decided - down : 0;
  const unsigned long paintedMs = millis() - down;
  LOG_INF("PWR", "dialog first paint: decision at %lu ms, painted at %lu ms after DOWN (gap %ld ms)",
          decisionMs, paintedMs, static_cast<long>(paintedMs) - static_cast<long>(decisionMs));
}
