#pragma once
#include "activities/Activity.h"

class BootActivity final : public Activity {
 public:
  explicit BootActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Boot", renderer, mappedInput) {}
  void onEnter() override;
  // Boot is a frame, not a screen: nothing may navigate away from it (Back included).
  bool ownsBackKey() const override { return true; }

  // The boot screen's base layer -- white panel plus the centred logo, and nothing
  // else -- as a free function so the BOOT1 early frame in main.cpp (painted before
  // the SD font scan finishes) and onEnter() cannot drift apart. The caller owns the
  // drive: this draws into the framebuffer and never calls displayBuffer().
  static void paintBase(GfxRenderer& renderer);
};
