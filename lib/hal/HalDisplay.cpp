#include <HalDisplay.h>
#include <HalGPIO.h>

#include "../../src/display/EpdFlashGuard.h"
#include "../../src/display/RefreshPolicy.h"

// Global HalDisplay instance
HalDisplay display;

#define SD_SPI_MISO 7

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

HalDisplay::Controller HalDisplay::getController() const { return BoardConfig::ACTIVE.displayController; }

void HalDisplay::begin(bool seamless) {
  // Set X3-specific panel mode before initializing.
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }

  einkDisplay.begin();

  if (seamless) {
    // Defuse the SDK's X3 _x3InitialFullSyncsRemaining counter (no-op on X4)
    // so the first paint isn't promoted to FULL (~770ms). Skips the wakeup-
    // gated requestResync() below for the same reason.
    einkDisplay.skipInitialResync();
    return;
  }
  // Request resync after specific wakeup events to ensure clean display state.
  const auto wakeupReason = gpio.getWakeupReason();
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
      wakeupReason == HalGPIO::WakeupReason::Other) {
    einkDisplay.requestResync();
  }
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
}

// ---------------------------------------------------------------------------
// CP_EPD_POWERDOWN_AFTER_DRIVE — take the TPS65185 rails down once the drive on
// the glass has finished. Why, and what it costs, is in src/display/RefreshPolicy.h
// beside the switch and in docs/engineering/EPD_STATIC_IDLE_ARTIFACT.md.
//
// The weak reference keeps this file independent of the vendor panel backend: on a
// board whose driver does not export metalio_vendor_panel_off() the helper compiles
// to a call that is never made, exactly like lastPanelDrawError() in GfxRenderer.
// ---------------------------------------------------------------------------
#if CP_EPD_POWERDOWN_AFTER_DRIVE
extern "C" void metalio_vendor_panel_off(void) __attribute__((weak));

static void cp_powerdown_after_drive(bool turnOffScreen) {
  if (metalio_vendor_panel_off == nullptr) return;

  if (turnOffScreen) {
    // turnOffScreen already carried the drive's power-down through the SDK, so
    // the rails are down and a second off would only add a settle wait. The rails
    // are also being LEFT down here, which is exactly when the datasheet section
    // 8.4 tail (PWRUP/VCOM down -> WAKEUP down -> >=100 ms before the next
    // power-up) is required: r84 used to hold it on every drive, so this path
    // needed none of its own. With CP_TPS_FAST_SEQUENCE the per-drive cycle skips
    // the tail, so the paths that leave the rails down hold it instead.
#if CP_TPS_FAST_SEQUENCE
    delay(120);
#endif
    return;
  }

  metalio_vendor_panel_off();
#if !CP_TPS_FAST_SEQUENCE
  // r84 timing: the section-8.4 tail after EVERY drive. epd_board_poweroff() does
  // the two register steps and stops there (epd_board_metalio_eink4_plus.c:889-907),
  // so the wait that keeps the next power-on inside the part's off-time is ours.
  delay(120);
#endif
  // CP_TPS_FAST_SEQUENCE=1: no tail here. RickyOS cycles the rails after every
  // drive with no such wait (its 120 ms lives only in the park function) and
  // neither side programs DWNSEQ, so the part's default down timing applies.
  // The tail is still paid where it matters: park/sleep holds 50 + 120 ms inside
  // tps_enter_sleep() (epd_board_metalio_eink4_plus.c:343-361) and the
  // turnOffScreen branch above holds 120 ms. Rationale and the failure mode of
  // getting this wrong are beside CP_TPS_FAST_SEQUENCE in
  // src/display/RefreshPolicy.h.
}
#else
static inline void cp_powerdown_after_drive(bool) {}
#endif

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::GL16_REFRESH:
      return EInkDisplay::GL16_REFRESH;
    case HalDisplay::EQUALIZE_REFRESH:
      return EInkDisplay::EQUALIZE_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  // EpdFlashGuard: hold off nvs writes for the whole waveform (see EpdFlashGuard.h).
  epdflash::DriveScope flashGuard;
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
  cp_powerdown_after_drive(turnOffScreen);
}

void HalDisplay::displayBufferAsync(HalDisplay::RefreshMode mode) {
  // The guard covers the whole call: on this board the async entry runs the waveform before
  // returning (see the note below), so the hold is correct here. A panel that really defers
  // would need the hold moved to waitRefreshComplete() instead.
  epdflash::DriveScope flashGuard;
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayBufferAsyncNoShadow(convertRefreshMode(mode));
  // No deferral on this board (Epd047Tc2Driver has no displayStart): the SDK's async
  // entry already ran the whole waveform before returning, so the drive is complete
  // here. Where a panel does defer, the power-down belongs to waitRefreshComplete
  // instead and must not be issued while the glass is still being driven.
  if (!einkDisplay.supportsAsyncRefresh()) cp_powerdown_after_drive(false);
}

void HalDisplay::waitRefreshComplete() { einkDisplay.waitRefreshComplete(); }

bool HalDisplay::supportsAsyncRefresh() const { return einkDisplay.supportsAsyncRefresh(); }

HalDisplay::GrayscaleCapabilities HalDisplay::grayscaleCapabilities(GrayscaleMode mode) const {
  // The ED047TC2 is driven as a raw parallel panel with the vendor epdiy waveform set, which has no
  // usable gray LUT. An overlay pass on it renders the frame, then displayGrayBuffer() paints the
  // page black and the B/W restore repaints it: the reader shows the correct page and flips to
  // white-on-black ~0.7 s later, and every page turn pays an extra full-screen waveform.
  // Measured on the device from /.crosspoint/debug.log: without the gray pass a page turn totals
  // ~1.0-1.1 s, with it 1.35-1.82 s (gray_lsb 120 + gray_msb 122 + gray_display 497 ms).
  // Report unsupported here rather than at the call site so every consumer -- reader, sleep screen,
  // XTC, BMP viewer -- follows the hardware. Re-enable only when a gray LUT is proven on the glass.
  if (getController() == BoardConfig::DisplayController::Ed047Tc2) return {};
  return einkDisplay.grayscaleCapabilities(mode);
}

bool HalDisplay::supportsAsyncGrayscaleBase() const { return grayscaleCapabilities().asyncBase; }

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  epdflash::DriveScope flashGuard;  // EpdFlashGuard: same hold as displayBuffer()
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
  cp_powerdown_after_drive(turnOffScreen);
}

void HalDisplay::setInverted(bool inverted) { einkDisplay.setInverted(inverted); }

bool HalDisplay::toggleInverted() { return einkDisplay.toggleInverted(); }

bool HalDisplay::isInverted() const { return einkDisplay.isInverted(); }

void HalDisplay::deepSleep() {
  // Park: the panel's content can no longer be inferred from what we last drove, so
  // drop the refresh-policy latch (a no-op unless CP_RICKY_REFRESH_POLICY is 1).
  cp_refresh_policy_note_park();
  einkDisplay.deepSleep();
}

void HalDisplay::wake() { einkDisplay.wake(); }

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

uint8_t* HalDisplay::lendFrameBufferStorage(uint32_t* sizeOut) { return einkDisplay.lendBuildStorage(sizeOut); }

void HalDisplay::returnFrameBufferStorage() { einkDisplay.returnBuildStorage(); }

bool HalDisplay::displayGrayscaleBase(GrayscaleMode mode, RefreshMode fallback, bool turnOffScreen) {
  epdflash::DriveScope flashGuard;  // EpdFlashGuard: a grayscale base is a waveform too
  if (gpio.deviceIsX3() && fallback == HALF_REFRESH) einkDisplay.requestResync();
  return einkDisplay.displayGrayscaleBase(mode, static_cast<EInkDisplay::RefreshMode>(fallback), turnOffScreen);
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOffScreen) {
  epdflash::DriveScope flashGuard;  // EpdFlashGuard: a grayscale base is a waveform too
  // X3: a HALF fallback means the caller wants a clean base (e.g. the sleep
  // cover, a full-screen swap from arbitrary prior content). Without this, the
  // X3 grayscale base takes its gentle differential happy path and the prior
  // home/reader frame ghosts through the soft aa_pre_bw_mid waveform. Forcing a
  // resync makes displayGrayscaleBase clear first, matching displayBuffer(HALF).
  // The reader's FAST path is deliberately left on the differential path so
  // per-page grayscale stays cheap.
  if (gpio.deviceIsX3() && fallback == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayGrayscaleBase(convertRefreshMode(fallback), turnOffScreen);
}

void HalDisplay::preconditionGrayscale() { einkDisplay.preconditionGrayscale(); }

void HalDisplay::preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  einkDisplay.preconditionGrayscale(x, y, w, h);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) {
  epdflash::DriveScope flashGuard;  // EpdFlashGuard: grayscale display is a waveform too
  einkDisplay.displayGrayBuffer(turnOffScreen);
}

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  einkDisplay.writeGrayscalePlaneStrip(lsbPlane ? EInkDisplay::GRAY_PLANE_LSB : EInkDisplay::GRAY_PLANE_MSB, rows,
                                       yStart, numRows);
}

bool HalDisplay::supportsStripGrayscale() const { return grayscaleCapabilities().stripUploads; }

bool HalDisplay::combinesGrayscaleBase() const { return grayscaleCapabilities().base == GrayscaleBase::Combined; }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
