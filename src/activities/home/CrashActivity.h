#pragma once
#include "activities/Activity.h"

class CrashActivity final : public Activity {
  std::string panicMessage;

 public:
  explicit CrashActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Crash", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  // Answers Button::Back in loop(): the manager must not navigate for this screen.
  bool ownsBackKey() const override { return true; }
  void render(RenderLock&&) override;
};
