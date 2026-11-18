#pragma once
#include "activities/Activity.h"
#include "components/OptionPopup.h"

// >4 s POWER hold: a modal Cancel / Power off pair, and on Confirm one last paint of
// "Powering off..." through the ordinary render path. The shutdown itself paints nothing: by the
// time loop() acts on the request this frame is already on the glass.
class PowerOffActivity : public Activity {
 public:
  PowerOffActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PowerOff", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // Answers Button::Back in loop(): the manager must not navigate for this screen.
  bool ownsBackKey() const override { return true; }
  void render(RenderLock&& lock) override;

 private:
  enum class State { CONFIRM, SHUTTING_DOWN };

  void confirmPowerOff();
  // r100.4: logs the DOWN -> decision -> first-paint timings once, for the first frame of the
  // dialog that actually reached the glass (see MetalioPowerRequest.h).
  void logFirstPaint();

  OptionPopup confirmPopup;
  State state = State::CONFIRM;
  bool requested = false;
  bool firstPaintLogged = false;
  // r100.3: the press that opened this dialog owns its own release, so the dialog stays deaf
  // until that press is over. Level- and edge-based (never a deadline race): see loop().
  bool inputArmed = false;
  unsigned long shownAtMs = 0;
};
