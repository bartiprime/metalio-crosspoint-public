#pragma once
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "util/BatteryStats.h"

// r97 BATT2 -- the battery statistics page (Settings > System > Battery
// statistics). A Settings SUB-PAGE, drawn with the same list chrome every other
// settings screen uses, not a dialog.
//
// KOReader's shape: a header that names the current charge, four blocks (awake /
// sleeping / during last charge / all-time) each carrying measured time, the
// measured change, the measured change per hour and the estimate that follows
// from it. Every number comes from BatteryStats, which integrates gauge reads and
// real elapsed time; nothing here is derived from cell capacity.
//
// HISTORY vs ESTIMATE. The three state blocks are the RECORD: measured time and
// measured change for the cycle that began at the last confirmed unplug, so the
// page reads "since last charge" with no intervention (BatteryStats zeroes the
// record on the same gated charge -> discharge edge the estimate window restarts
// on). The last block's rate and time-left are an ESTIMATE computed from
// BatteryStats' rolling discharge window, so they describe the current session
// instead of dividing one cycle's total by that same total's clock.
//
// Footer: a real button (not a row) that asks before wiping the record. It is now
// the MANUAL OVERRIDE -- the record clears itself at every charge cycle -- and it
// still says so on the glass when it has.
class BatteryStatsActivity final : public UiListActivity {
 public:
  explicit BatteryStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  // 4 blocks x 4 metrics.
  static constexpr int ITEM_COUNT = 16;

  void onEnter() override;
  void loop() override;
  // The confirm dialog is drawn over the live page; without this override the
  // OptionPopup is active but never painted (see the .cpp).
  void render(RenderLock&&) override;

 private:
  struct Block {
    StrId name;
    const BatteryStats::Section* section;
    bool charging;  // "Charged"/"until full" wording instead of "Used"/"left"
  };

  // Footer reset button. Derived from the base's protected ACTION_USER, so it
  // cannot collide with ACTION_ROW or the pager id; declared here because action
  // ids are protected members of UiListActivity and only visible from inside the
  // derived class.
  static constexpr freeink::ui::ActionId ACTION_RESET = ACTION_USER;

  int listCount() const override { return ITEM_COUNT; }
  void buildScreen(UiScreen& screen) override;
  bool usesThemeListChrome() const override { return true; }
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;
  static void resetTrampoline(const freeink::ui::ActionEvent& event, void* user);

  void refreshRows();
  void confirmReset();

  freeink::ui::ListItem rowItems_[ITEM_COUNT]{};
  BatteryStats::Snapshot snap_{};

  // Row text lives here so the pointers handed to the list stay valid across the
  // frame (the render pass must not allocate).
  static constexpr int VALUE_LEN = 24;
  char values_[ITEM_COUNT][VALUE_LEN] = {};
  // mutable: headerTitle() is const but fills this buffer on each call.
  mutable char title_[48] = {0};

  OptionPopup optionPopup;

  // UI-STD-2: a control that appears to do nothing is a defect, so the clear
  // announces itself on the glass for a few seconds (the footer label becomes the
  // localized "Done", and every value on the page is a fresh-record zero). 0 = no
  // notice pending.
  static constexpr uint32_t kClearedNoticeMs = 4000;
  uint32_t clearedUntilMs_ = 0;
};
