#pragma once

#include <I18n.h>

#include <cstddef>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

class GfxRenderer;

class IntervalSelectionActivity final : public Activity, private UiAppHost {
 public:
  explicit IntervalSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* activityName,
                                     StrId titleId, int initialValue, int minValue, int maxValue, int smallStep,
                                     int largeStep, StrId valueFormatId = StrId::STR_NONE_OPT,
                                     bool readerActivity = false, StrId maxBoundaryLabelId = StrId::STR_NONE_OPT);

  void onEnter() override;
  void loop() override;
  // Answers Button::Back in loop(): the manager must not navigate for this screen.
  bool ownsBackKey() const override { return true; }
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return readerActivity; }

 private:
  // The UiAppHost app hosts the shared slider dialog popup (capsule slider,
  // [-] [+] [Confirm] buttons) drawn over the screen underneath.
  static void intervalScreen(UiScreen& screen, void* user);
  static void onSliderEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onOkEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildIntervalScreen(UiScreen& screen);

  StrId titleId;
  StrId valueFormatId;
  StrId maxBoundaryLabelId;
  int value;
  int minValue;
  int maxValue;
  int smallStep;
  int largeStep;
  bool readerActivity;
  ButtonNavigator buttonNavigator;

  // Swallow the swipe/tap fallout of a slider drag so its release can't trigger
  // the back gesture and cancel the dialog.
  bool draggingSlider = false;
  // SLIDE1: a step of the value is waiting to be painted. Set by setValue()/
  // adjustValue() (the drag and the -/+ keys both funnel through them), consumed
  // by render() on the paint that shows the new value, which is then ONE
  // diff-masked DU instead of a FAST paint plus the row-9 GL16 completion drive
  // (a second whole-panel drive per step -- the flash). See
  // activities/util/InPlaceValueStep.h.
  bool stepPaintPending = false;

  void adjustValue(int delta);
  // Absolute value (clamped), from slider drag/tap positions.
  void setValue(int candidate);
  int clampedValue(int candidate) const;
  void formatValue(char* buffer, size_t size, int forValue) const;
  void cancel();
  void confirm();
};
