#pragma once

#include "FlickCalibration.h"
#include "activities/Activity.h"

// IMU2: the guided calibration the operator asked for, on the device.
//
// Three prompts, each ended by the reader's existing Confirm (the same key /
// tap every other screen's confirm uses):
//   1. flick the device LEFT, then Confirm
//   2. flick it RIGHT, then Confirm
//   3. hold it still, then Confirm        (the noise floor)
//
// Each window is captured raw (no detector opinion) and MEASURED numbers are
// shown on screen and logged. The derivation and every rejection rule live in
// lib/hal/FlickCalibration.h, which is host-tested; this screen only runs the
// sequence and draws the answer. A refused capture saves nothing.
class FlickCalibrationActivity final : public Activity {
 public:
  explicit FlickCalibrationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FlickCalibration", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // Answers Button::Back in loop(): the manager must not navigate for this screen.
  bool ownsBackKey() const override { return true; }
  // Sampling needs a fast loop (the flick is ~130 ms); this also keeps the CPU
  // out of power saving while the screen is up.
  bool skipLoopDelay() override { return true; }
  void render(RenderLock&&) override;

 private:
  enum Stage : uint8_t { LEFT, RIGHT, STILL, DONE, REJECTED };

  Stage stage = LEFT;
  flickcal::Capture current;
  flickcal::Capture captured[3];
  flickcal::Verdict verdict;

  unsigned long lastSampleMs = 0;
  uint32_t windowStartMs = 0;

  void beginStage(Stage next);
  void finishStage();
  void sampleOnce();
  void sampleText(char* out, size_t cap) const;
};
