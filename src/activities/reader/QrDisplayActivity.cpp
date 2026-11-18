#include "QrDisplayActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

void QrDisplayActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void QrDisplayActivity::onExit() { Activity::onExit(); }

void QrDisplayActivity::loop() {
  int x = 0;
  int y = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    finish();
    return;
  }
}

void QrDisplayActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // ANDRO4: the THEME's titled band (Andro: the doubled app bar with the Ember
  // title and the 56 px arrow), not the home status strip -- this screen is
  // reached from Calibre and the web server, both of which already wear it.
  // Every other theme resolves this to metrics.headerHeight, unchanged.
  const int bandHeight = UITheme::getInstance().getTitledHeaderHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, bandHeight}, tr(STR_DISPLAY_QR), nullptr);

  // The QR frame is inset by the theme's content padding, not a bare 20 px, so it
  // lines up with the text column on every panel (Andro: 27 px).
  const int inset = metrics.contentSidePadding;
  const int availableWidth = pageWidth - 2 * inset;
  const int availableHeight =
      pageHeight - metrics.topPadding - bandHeight - metrics.verticalSpacing * 2 - metrics.buttonHintsHeight;
  const int startY = metrics.topPadding + bandHeight + metrics.verticalSpacing;

  const Rect qrBounds(inset, startY, availableWidth, availableHeight);
  QrUtils::drawQrCode(renderer, qrBounds, textPayload);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
