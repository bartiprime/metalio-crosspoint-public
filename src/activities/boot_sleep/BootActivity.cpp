#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "fontIds.h"
#include "images/Logo120.h"

// The logo is a compiled-in 120x120 array; both the early frame and this screen place
// it with the same maths from the same constant.
static constexpr int kLogoSize = 120;

void BootActivity::paintBase(GfxRenderer& renderer) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  // r163 D3: brand art, `Illustration` (visible content). 1 bpp today, so the role
  // records the admission without changing a single pixel.
  const GfxRenderer::ScopedContentRole logoRole(renderer, GfxRenderer::ContentRole::Illustration);
  renderer.drawImage(Logo120, (pageWidth - kLogoSize) / 2, (pageHeight - kLogoSize) / 2, kLogoSize, kLogoSize);
}

void BootActivity::onEnter() {
  Activity::onEnter();

  paintBase(renderer);

  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
  renderer.displayBuffer();
  // BOOT1 instrument: the second timestamp of the pair whose gap used to be a blank
  // panel (the first is main.cpp's "[PANEL] early boot frame"). This drive is the
  // clean one -- the early frame spent the post-poweron banding.
  LOG_INF("PANEL", "boot screen driven at %lu ms", static_cast<unsigned long>(millis()));
}
