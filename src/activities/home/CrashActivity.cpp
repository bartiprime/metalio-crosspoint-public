#include "CrashActivity.h"

#include <GfxRenderer.h>
#include <HalSystem.h>
#include <I18n.h>

#include "components/UITheme.h"
#include "fontIds.h"

void CrashActivity::onEnter() {
  Activity::onEnter();

  panicMessage = HalSystem::getPanicInfo(false);
  if (panicMessage.empty()) {
    panicMessage = tr(STR_CRASH_NO_REASON);
  }

  requestUpdateAndWait();
}

void CrashActivity::loop() {
  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}

void CrashActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto contentWidth = pageWidth - 2 * metrics.contentSidePadding;
  const auto x = metrics.contentSidePadding;
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  // Crash report is a dead end, not a pushed screen: no back button.
  // ANDRO4: still a titled band, so it takes the theme's titled height (Andro: the
  // doubled app bar) rather than the home status strip; the body below follows from
  // the same value. Other themes resolve it to metrics.headerHeight, unchanged.
  const int bandHeight = UITheme::getInstance().getTitledHeaderHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, bandHeight}, tr(STR_CRASH_TITLE), nullptr, false);

  int y = metrics.topPadding + bandHeight + metrics.verticalSpacing;

  auto descLines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_CRASH_DESCRIPTION), contentWidth, 10);
  for (const auto& line : descLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  y += metrics.verticalSpacing * 2;

  // r163 3.11: the reset CAUSE first, then the panic text. On a lockup or a hardware
  // watchdog the panic hook never runs, so getPanicInfo(false) is empty and this screen
  // used to say "no reason was recorded" while esp_reset_reason() knew the cause exactly.
  // Both lines come from HalSystem, so the screen and crash_report.txt agree.
  renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_CRASH_RESET));
  y += lineHeight + metrics.verticalSpacing;
  renderer.drawText(UI_10_FONT_ID, x, y, HalSystem::resetReasonText());
  y += lineHeight + metrics.verticalSpacing * 2;

  renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_CRASH_REASON));
  y += lineHeight + metrics.verticalSpacing;

  auto panicLines = renderer.wrappedText(UI_10_FONT_ID, panicMessage.c_str(), contentWidth, 5);
  for (const auto& line : panicLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
