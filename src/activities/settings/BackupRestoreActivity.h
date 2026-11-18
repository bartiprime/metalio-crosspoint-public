#pragma once
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// BACKUP1 -- Settings > System > Backup & Restore.
//
// One sub-page with three verbs on the mode screen:
//   Export full profile now
//   Import from file ...        (turns the same list into a picker over
//                                /.crosspoint/backups/)
//   Restore pre-restore snapshot
//
// The rows are the ordinary settings list rows (theme list chrome, so the row
// pitch and touch target come from the theme exactly as every other settings
// page). Nothing here writes a file itself: backupsvc owns the restore chain
// and returns a verdict, which this screen reports on the glass.
class BackupRestoreActivity final : public UiListActivity {
 public:
  explicit BackupRestoreActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void render(RenderLock&& lock) override;

 private:
  enum class Mode : uint8_t { Menu, Picker };
  enum class Busy : uint8_t { None, Exporting, Restoring };

  int listCount() const override { return static_cast<int>(items_.size()); }
  void buildScreen(UiScreen& screen) override;
  bool usesThemeListChrome() const override { return true; }
  bool usesListFooterPager() const override { return true; }
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;

  void rebuildRows();
  void openPicker();
  void backToMenu();
  void chooseFile(int index);
  void runExport();
  void runRestore(const std::string& fileName, bool preRestore);
  void showMessage(const char* title, const std::string& body);
  void showFailure(const char* title, const std::string& body);

  Mode mode = Mode::Menu;
  Busy busy = Busy::None;
  std::vector<std::string> pickerFiles;
  // Labels/values live here so the pointers handed to the list stay valid for
  // the frame; refreshed only by rebuildRows(), never during a render.
  std::vector<std::string> labels_;
  std::vector<freeink::ui::ListItem> items_;
  std::string messageTitle_;
  std::string messageBody_;

  // The row being confirmed, captured when the dialog opens (the picker can be
  // refreshed by a stray tick while the dialog is up).
  std::string pendingFile_;
  bool pendingPreRestore_ = false;

  OptionPopup optionPopup;
};
