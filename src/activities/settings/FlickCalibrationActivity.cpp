#include "FlickCalibrationActivity.h"

#include <GfxRenderer.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr unsigned long kSampleIntervalMs = 10;  // matches the detector's poll gate

const StrId kPrompt[3] = {StrId::STR_FLICK_CAL_LEFT, StrId::STR_FLICK_CAL_RIGHT, StrId::STR_FLICK_CAL_STILL};

const char* rejectText(const flickcal::Reject r) {
  switch (r) {
    case flickcal::Reject::TooFewSamples:
      return tr(STR_FLICK_CAL_REJECT_SAMPLES);
    case flickcal::Reject::WeakFlick:
      return tr(STR_FLICK_CAL_REJECT_WEAK);
    case flickcal::Reject::NoisyStill:
      return tr(STR_FLICK_CAL_REJECT_NOISY);
    case flickcal::Reject::Impact:
      return tr(STR_FLICK_CAL_REJECT_DROP);
    case flickcal::Reject::SameDirection:
      return tr(STR_FLICK_CAL_REJECT_DIRECTION);
    case flickcal::Reject::Imbalance:
    case flickcal::Reject::NoFastOnset:
      return tr(STR_FLICK_CAL_REJECT_SHAPE);
    case flickcal::Reject::None:
      break;
  }
  return "";
}

// Word-wrap and centre text with the renderer's own primitives
// (getTextWidth / drawCenteredText): every other screen draws its body text
// this way and no theme method is involved. Returns the y after the last line.
int drawWrappedCentered(GfxRenderer& r, const int fontId, int y, const int width, const char* text, const int maxLines,
                        const int lineHeight) {
  char line[96] = {0};
  int lines = 0;
  const char* p = text;
  while (*p != '\0' && lines < maxLines) {
    while (*p == ' ') ++p;
    if (*p == '\0') break;
    const char* word = p;
    while (*p != '\0' && *p != ' ') ++p;
    const int wordLen = static_cast<int>(p - word);
    char candidate[96];
    if (line[0] == '\0') {
      snprintf(candidate, sizeof(candidate), "%.*s", wordLen, word);
    } else {
      snprintf(candidate, sizeof(candidate), "%s %.*s", line, wordLen, word);
    }
    if (line[0] != '\0' && r.getTextWidth(fontId, candidate) > width) {
      r.drawCenteredText(fontId, y, line);
      y += lineHeight;
      ++lines;
      snprintf(line, sizeof(line), "%.*s", wordLen, word);
    } else {
      snprintf(line, sizeof(line), "%s", candidate);
    }
  }
  if (line[0] != '\0' && lines < maxLines) {
    r.drawCenteredText(fontId, y, line);
    y += lineHeight;
  }
  return y;
}

}  // namespace

void FlickCalibrationActivity::onEnter() {
  Activity::onEnter();
  stage = LEFT;
  verdict = flickcal::Verdict{};
  for (auto& c : captured) c.reset();
  if (!halTiltSensor.isAvailable()) {
    LOG_ERR("TILT", "calibration: no IMU on this board");
    stage = REJECTED;
    requestUpdate();
    return;
  }
  beginStage(LEFT);
}

void FlickCalibrationActivity::onExit() {
  halTiltSensor.endCapture();
  Activity::onExit();
}

void FlickCalibrationActivity::beginStage(const Stage next) {
  stage = next;
  current.reset();
  lastSampleMs = 0;
  windowStartMs = millis();
  halTiltSensor.beginCapture(SETTINGS.tiltPageTurn);
  LOG_INF("TILT", "calibration: prompt %s", next == LEFT ? "left" : (next == RIGHT ? "right" : "still"));
  requestUpdate();
}

void FlickCalibrationActivity::sampleOnce() {
  const unsigned long now = millis();
  if (lastSampleMs != 0 && (now - lastSampleMs) < kSampleIntervalMs) return;
  lastSampleMs = now;
  float ax, ay, az;
  if (!halTiltSensor.captureSample(ax, ay, az)) return;
  current.add(ax, ay, az, static_cast<uint32_t>(now - windowStartMs),
              static_cast<flick::Orientation>(SETTINGS.orientation));
}

void FlickCalibrationActivity::finishStage() {
  // Too short to mean anything: keep waiting rather than record a broken window.
  const flickcal::Limits limits;
  if (current.samples() < limits.minSamples) {
    LOG_INF("TILT", "calibration: confirm ignored, only %u samples", static_cast<unsigned>(current.samples()));
    return;
  }
  captured[stage] = current;
  LOG_INF("TILT", "cal %s: peak=%.2f g sign=%d rise=%u ms samples=%u noise=%.2f perp=%.2f",
          stage == LEFT ? "left" : (stage == RIGHT ? "right" : "still"), static_cast<double>(fabsf(current.peakG)),
          static_cast<int>(current.onsetSign), static_cast<unsigned>(current.riseMs()),
          static_cast<unsigned>(current.samples()), static_cast<double>(current.noisePeak),
          static_cast<double>(current.peakPerp));

  if (stage == LEFT) {
    beginStage(RIGHT);
    return;
  }
  if (stage == RIGHT) {
    beginStage(STILL);
    return;
  }

  verdict = flickcal::derive(captured[LEFT], captured[RIGHT], captured[STILL]);
  halTiltSensor.endCapture();
  if (verdict.ok) {
    halTiltSensor.saveCalibration(verdict.cal);
    LOG_INF("TILT", "calibration accepted: trigger=%.2f g start=%.2f g nextSign=%d",
            static_cast<double>(verdict.cal.triggerG), static_cast<double>(verdict.cal.startG),
            static_cast<int>(verdict.cal.nextSign));
    stage = DONE;
  } else {
    LOG_ERR("TILT", "calibration rejected: %s", flickcal::rejectName(verdict.reject));
    stage = REJECTED;
  }
  requestUpdate();
}

void FlickCalibrationActivity::loop() {
  if (stage == DONE || stage == REJECTED) {
    int x = 0, y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
      finish();
      return;
    }
    // A rejected capture is retried from the first prompt -- the numbers that
    // made it fail stay on screen until then.
    if (stage == REJECTED && halTiltSensor.isAvailable() &&
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      verdict = flickcal::Verdict{};
      for (auto& c : captured) c.reset();
      beginStage(LEFT);
    }
    return;
  }

  sampleOnce();

  // Back abandons the capture; Confirm ends the window -- the same confirm the
  // rest of the UI uses, so BOOT, the front confirm button and a tap all work.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    halTiltSensor.endCapture();
    finish();
    return;
  }
  int x = 0, y = 0;
  const bool tapped = mappedInput.wasScreenTapped(x, y);
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || tapped) {
    finishStage();
  }
}

void FlickCalibrationActivity::sampleText(char* out, const size_t cap) const {
  // Numbers only, no translated prose: the prompt above it carries the words.
  const uint32_t rise = current.riseMs();
  snprintf(out, cap, "peak %.2f g  rise %u ms  %u samples", static_cast<double>(fabsf(current.peakG)),
           static_cast<unsigned>(rise == 0xFFFFFFFFu ? 0 : rise), static_cast<unsigned>(current.samples()));
}

void FlickCalibrationActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, UITheme::getInstance().getTitledHeaderHeight()},
                 tr(STR_FLICK_CALIBRATE));

  const int bodyTop = metrics.topPadding + UITheme::getInstance().getTitledHeaderHeight() + 24;
  const int textWidth = pageWidth - 48;
  const int lineHeight = renderer.getTextHeight(UI_10_FONT_ID) + 6;

  if (stage == DONE) {
    int y = drawWrappedCentered(renderer, UI_10_FONT_ID, bodyTop, textWidth, tr(STR_FLICK_CAL_OK), 2, lineHeight);

    char left[48], right[48], still[48], trig[48];
    snprintf(left, sizeof(left), "LEFT   peak %.2f g", static_cast<double>(fabsf(captured[LEFT].peakG)));
    snprintf(right, sizeof(right), "RIGHT  peak %.2f g", static_cast<double>(fabsf(captured[RIGHT].peakG)));
    snprintf(still, sizeof(still), "still  max %.2f g", static_cast<double>(captured[STILL].noisePeak));
    snprintf(trig, sizeof(trig), "trigger %.2f g", static_cast<double>(verdict.cal.triggerG));

    y += 16;
    renderer.drawCenteredText(UI_10_FONT_ID, y, left);
    renderer.drawCenteredText(UI_10_FONT_ID, y + lineHeight, right);
    renderer.drawCenteredText(UI_10_FONT_ID, y + lineHeight * 2, still);
    renderer.drawCenteredText(UI_10_FONT_ID, y + lineHeight * 3 + 8, trig, true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (stage == REJECTED) {
    int y = drawWrappedCentered(
        renderer, UI_10_FONT_ID, bodyTop, textWidth,
        halTiltSensor.isAvailable() ? rejectText(verdict.reject) : tr(STR_FLICK_CAL_NO_SENSOR), 4, lineHeight);
    char nums[96];
    snprintf(nums, sizeof(nums), "L %.2f  R %.2f  still %.2f g", static_cast<double>(fabsf(captured[LEFT].peakG)),
             static_cast<double>(fabsf(captured[RIGHT].peakG)), static_cast<double>(captured[STILL].noisePeak));
    y += 12;
    renderer.drawCenteredText(UI_10_FONT_ID, y, nums);
    renderer.drawCenteredText(UI_10_FONT_ID, y + lineHeight, tr(STR_FLICK_CAL_NOTHING_SAVED));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  int y = drawWrappedCentered(renderer, UI_10_FONT_ID, bodyTop, textWidth, I18N.get(kPrompt[stage]), 3, lineHeight);

  // The measurement of the PREVIOUS window stays on screen while the next
  // prompt is up: the operator sees what was actually recorded.
  if (stage > LEFT) {
    char prev[64];
    snprintf(prev, sizeof(prev), "%s  %.2f g / %u ms", stage == RIGHT ? "LEFT" : "RIGHT",
             static_cast<double>(fabsf(captured[stage - 1].peakG)),
             static_cast<unsigned>(captured[stage - 1].riseMs()));
    renderer.drawCenteredText(UI_10_FONT_ID, y + 24, prev);
  }
  char live[64];
  sampleText(live, sizeof(live));
  renderer.drawCenteredText(UI_10_FONT_ID, y + 56, live);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
