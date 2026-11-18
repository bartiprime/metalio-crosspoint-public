#pragma once

#include <string>

#include "activities/Activity.h"
#include "components/UiAppHost.h"

class ButtonRemapActivity final : public Activity, private UiAppHost {
 public:
  explicit ButtonRemapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ButtonRemap", renderer, mappedInput), UiAppHost(renderer) {}

  void onEnter() override;
  void loop() override;
  // Back cancels: this screen captures every raw front-button press as a mapping target, so Back has
  // to be taken here before that capture (see loop()).
  bool ownsBackKey() const override { return true; }
  void render(RenderLock&&) override;

 private:
  // Rendering task state.

  // Index of the logical role currently awaiting input.
  uint8_t currentStep = 0;
  // Temporary mapping from logical role -> hardware button index.
  uint8_t tempMapping[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  // Error banner timing (used when reassigning duplicate buttons).
  unsigned long errorUntil = 0;
  std::string errorMessage;
  freeink::ui::ListItem rowItems[4]{};
  // Row pitch the list was built with (see buildScreen). The two hint lines under
  // the list are positioned from it, so a theme-owned row measure cannot put them
  // on top of the last row. Written by buildScreen(), read by render() -- both on
  // the render task.
  int rowPitch_ = 0;

  static void screenTrampoline(UiScreen& screen, void* user);
  void buildScreen(UiScreen& screen);
  // Commit temporary mapping to settings.
  void applyTempMapping();
  // Returns false if a hardware button is already assigned to a different role.
  bool validateUnassigned(uint8_t pressedButton);
  // Labels for UI display.
  static const char* getRoleName(uint8_t roleIndex);
  const char* getHardwareName(uint8_t buttonIndex) const;
};
