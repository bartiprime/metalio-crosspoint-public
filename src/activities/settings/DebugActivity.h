#pragma once

#include <string>

#include "activities/UiListActivity.h"
// EXP1: brings the CP_EXP default in BEFORE this header decides whether the three
// r101.21 arm rows exist, so the enum, the label table and the switch cannot
// disagree about the row count.
#include "platform/ExpArms.h"

// Diagnostics page: the one place to turn logging on, dump it to the SD card, and read it back.
//
// Labels are deliberately hardcoded English, on the same exemption as AboutActivity: this is a
// bring-up/support screen, and its wording must be identical on every unit regardless of the UI
// language, because it is read off a photo or a pasted log.
//
// Two surfaces on purpose (see docs/BUILD_ONE_REPORT.md §18):
//   * "View recent log" shows the in-RAM crash ring -- 16 lines, dies with the power cycle. A tail,
//     not a history; the row says so.
//   * "Print logs to SD" appends the RAM capture buffer to /.crosspoint/debug.log, which is the
//     history that survives a power cycle and can be copied off the card.
class DebugActivity final : public UiListActivity {
 public:
  explicit DebugActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Five diagnostic actions + two log toggles + two read-outs + the four refresh probes +
  // the LP rows + the USB shell toggle + the two trim arms. The self-tests that remain
  // exist so a peripheral can be exercised from the UI and proved in
  // /.crosspoint/debug.log, instead of being guessed at from source.
  // r90: +1 for the runtime toggle of the USB debug shell (CP_USB_SHELL).
  // r96: +2 for the LP-core rows (CP_LP_BENCH); the PPA rows were pruned in r99.1.
  // r98.4: +2 for the standby-trim arm toggles (PA-rail cut, SD park).
  // r101.19: +1 -> 17, one APPENDED row: the P0.5 CAM/SCR rail arm, a trim of its own.
  // BENCH1: 15 -> 16, one APPENDED row: the `benchdrives` harness (the same arm the USB shell
  // command provides) so the instrument has a one-press trigger on the device itself.
  // DBG1 (r99.1): 25 -> 15. Ten rows removed because they duplicated `ver`/`cpu`
  // output or their topic is closed (PPA: all-six-no). See the enum for the list.
#if CP_EXP
  // r101.21 EXP1: 17 -> 20, the three arm rows APPENDED (row9 / short-DU / s8 dotprod).
  // r163 3.10: +1 for the TPS65185 fault trace (APPENDED after the EXP block; read-only,
  // one tap takes ONE sample and the row shows the verdict).
  static constexpr int ITEM_COUNT = 21;
#else
  static constexpr int ITEM_COUNT = 18;
#endif
  // Cap matches the crash ring's MAX_LOG_LINES, so the view can never claim more history than exists.
  static constexpr int MAX_VIEW_LINES = 16;

  int listCount() const override { return viewMode_ ? viewCount_ : ITEM_COUNT; }
  void buildScreen(UiScreen& screen) override;

  // r73 settings chrome: theme-owned row type/icons + a band-filling pitch.
  bool usesThemeListChrome() const override { return true; }
  bool usesListFooterPager() const override { return true; }
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  void refreshValues();
  // Automatic A/B waveform probe. One row tap runs a fixed sequence of 11 frames: a clean FULL
  // reference, then five BEFORE/AFTER pairs driven with the mode under test, so residue from the
  // previous frame accumulates the way it does in real page turns. One frame per loop() pass -- the
  // render task owns the ~650 ms displayBuffer(), so the input loop is never wedged.
  void beginRefreshProbe(uint8_t mode);
  void advanceRefreshProbe();  // schedule the next frame; called from loop() only
  void resetRefreshProbe();
  const char* probeModeName() const;
  // 1 reference + 5 cycles x (AFTER, BEFORE) = 11 frames. Step 0 is the reference; odd steps are
  // AFTER, even steps (>= 2) are BEFORE.
  static constexpr uint8_t kProbeLastStep = 10;
  static constexpr uint8_t kProbeCycles = 5;
  void drawProbeFrame(uint8_t step);
  int drawProbeParagraph(const char* text, int x, int y, int width, int font, int lineH, int bottom);
  uint8_t probePhase_ = 0;  // 0 = idle, 1 = sequence running, 2 = sequence done (last frame held)
  uint8_t probeStep_ = 0;   // 0 = reference, odd = AFTER, even = BEFORE
  uint8_t probeMode_ = 0;   // the waveform under test
  uint8_t probeFrameMode_ = 0;  // the waveform used for the frame on the panel right now
  volatile bool probeFrameDrawn_ = false;   // render task -> input loop: this step reached the panel
  volatile unsigned long probeStepMs_ = 0;  // millis() when that frame was displayed
  void openLogView();

  bool viewMode_ = false;
  int viewCount_ = 0;
  std::string actionNote_;  // result of the last action, shown beside its row
  // r86 accelerator row: the short outcome of the last run, or empty to fall
  // back to the service's own state name. Detail goes to the debug log.
  std::string lpNote_;
  // r96 bench rows: the short outcome of the last run. Empty = fall back to the
  // service's own state (or "-" when nothing has run yet).
  std::string lpBenchNote_;
  std::string lpSleepNote_;
  // BENCH1: refusal text for the drive-bench row. Empty means "show the harness's own state", so a
  // successful press leaves the row live rather than stuck on a stale "armed".
  std::string benchNote_;
  // r163 3.10: the short verdict of the last TPS65185 fault sample ("none",
  // "VCOM,VGL int1", "read failed (258)"). Empty means "not sampled yet".
  std::string tpsNote_;
  // r98.4: refusal text for the two trim rows (indexed by standbytrim::Trim). Only
  // filled when the arm was refused; empty means "show the arm state".
  std::string trimNote_[3];
#if CP_EXP
  // r101.21 EXP1: per-row action notes for the three arm rows (row9 / vid / simd).
  // Same contract as benchNote_: a refusal replaces the value, because a toggle that
  // reads "ON" and did nothing looks broken rather than refused.
  std::string expNote_[3];
#endif
  std::string rowValues_[ITEM_COUNT];
  std::string viewLines_[MAX_VIEW_LINES];
  freeink::ui::ListItem rowItems_[ITEM_COUNT]{};
  freeink::ui::ListItem viewItems_[MAX_VIEW_LINES]{};
};