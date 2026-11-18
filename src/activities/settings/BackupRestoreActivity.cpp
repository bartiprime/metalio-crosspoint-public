#include "BackupRestoreActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <memory>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "backup/BackupService.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr int kRowExport = 0;
constexpr int kRowImport = 1;
constexpr int kRowSnapshot = 2;
}  // namespace

BackupRestoreActivity::BackupRestoreActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("BackupRestore", renderer, mappedInput) {}

void BackupRestoreActivity::onEnter() {
  UiListActivity::onEnter();
  mode = Mode::Menu;
  busy = Busy::None;
  rebuildRows();
}

bool BackupRestoreActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

const char* BackupRestoreActivity::headerTitle() const { return tr(STR_BACKUP_RESTORE); }

void BackupRestoreActivity::rebuildRows() {
  labels_.clear();
  items_.clear();
  if (mode == Mode::Menu) {
    labels_.reserve(3);
    labels_.push_back(tr(STR_BACKUP_EXPORT));
    labels_.push_back(tr(STR_BACKUP_IMPORT));
    labels_.push_back(tr(STR_BACKUP_RESTORE_SNAPSHOT));
    items_.reserve(3);
    for (int i = 0; i < 3; i++) {
      fui::ListItem item;
      item.label = labels_[static_cast<size_t>(i)].c_str();
      item.actionValue = static_cast<int16_t>(i);
      item.enabled = (i != kRowSnapshot) || backupsvc::hasPreRestoreSnapshot();
      items_.push_back(item);
    }
  } else {
    pickerFiles = backupsvc::listBackups();
    labels_.reserve(pickerFiles.size());
    items_.reserve(pickerFiles.size());
    for (size_t i = 0; i < pickerFiles.size(); i++) {
      labels_.push_back(pickerFiles[i]);
      fui::ListItem item;
      item.label = labels_.back().c_str();
      item.actionValue = static_cast<int16_t>(i);
      items_.push_back(item);
    }
  }
}

void BackupRestoreActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + UITheme::getInstance().getTitledHeaderHeight()), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = items_.data();
  props.count = static_cast<uint16_t>(items_.size());
  props.action = ACTION_ROW;
  syncListViewport(screen, props);
  screen.list(props);
}

void BackupRestoreActivity::openPicker() {
  mode = Mode::Picker;
  rebuildRows();
  nav.selected = 0;
  nav.top = 0;
  if (pickerFiles.empty()) {
    showMessage(tr(STR_BACKUP_RESTORE), tr(STR_BACKUP_NONE));
  }
  requestUpdate();
}

void BackupRestoreActivity::backToMenu() {
  mode = Mode::Menu;
  rebuildRows();
  nav.selected = 0;
  nav.top = 0;
  requestUpdate();
}

void BackupRestoreActivity::activateIndex(int index) {
  if (optionPopup.isActive() || busy != Busy::None) return;
  app.clearTapFlash();
  nav.selected = index;
  if (mode == Mode::Menu) {
    if (index == kRowExport) {
      runExport();
    } else if (index == kRowImport) {
      openPicker();
    } else if (index == kRowSnapshot) {
      pendingPreRestore_ = true;
      pendingFile_.clear();
      chooseFile(index);
    }
    return;
  }
  chooseFile(index);
}

void BackupRestoreActivity::chooseFile(int index) {
  if (pendingPreRestore_) {
    pendingFile_ = "";
  } else if (index < static_cast<int>(pickerFiles.size())) {
    pendingFile_ = pickerFiles[static_cast<size_t>(index)];
  } else {
    return;
  }
  const char* options[] = {tr(STR_CANCEL), tr(STR_RESTORE_ACTION)};
  messageTitle_ = tr(STR_BACKUP_RESTORE);
  messageBody_ = pendingPreRestore_ ? tr(STR_BACKUP_RESTORE_SNAPSHOT) : pendingFile_;
  messageBody_ += "\n";
  messageBody_ += tr(STR_BACKUP_CONFIRM);
  optionPopup.showMessage(messageTitle_.c_str(), messageBody_.c_str(), options, 2, 0, [this](int choice) {
    if (choice != 1) return;
    const bool pre = pendingPreRestore_;
    const std::string file = pendingFile_;
    runRestore(file, pre);
  });
  requestUpdate();
}

void BackupRestoreActivity::showMessage(const char* title, const std::string& body) {
  const char* options[] = {tr(STR_OK_BUTTON)};
  messageTitle_ = title;
  messageBody_ = body;
  optionPopup.showMessage(messageTitle_.c_str(), messageBody_.c_str(), options, 1, 0, [this](int) { requestUpdate(); });
}

void BackupRestoreActivity::showFailure(const char* title, const std::string& body) { showMessage(title, body); }

void BackupRestoreActivity::runExport() {
  {
    RenderLock lock(*this);
    busy = Busy::Exporting;
  }
  requestUpdateAndWait();
  const auto result = backupsvc::exportNow();
  busy = Busy::None;
  if (result.ok) {
    showMessage(tr(STR_BACKUP_SAVED), result.fileName);
  } else {
    showMessage(tr(STR_BACKUP_FAILED), result.error);
  }
  rebuildRows();
  requestUpdate();
}

void BackupRestoreActivity::runRestore(const std::string& fileName, bool preRestore) {
  {
    RenderLock lock(*this);
    busy = Busy::Restoring;
  }
  requestUpdateAndWait();

  const auto result = preRestore ? backupsvc::restorePreRestoreSnapshot() : backupsvc::restoreFromFile(fileName);
  busy = Busy::None;

  if (result.ok) {
    LOG_INF("BACKUP1", "restore applied; restarting to settings");
    RenderLock lock;
    silentRestartToSettings();
    return;
  }
  std::string body = backup::rejectText(result.reason);
  if (!result.detail.empty()) {
    body += "\n";
    body += result.detail;
  }
  if (!result.rejectedAs.empty()) {
    body += "\n";
    body += result.rejectedAs;
  }
  showFailure(tr(STR_BACKUP_RESTORE_FAILED), body);
  requestUpdate();
}

void BackupRestoreActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  if (busy != Busy::None) {
    renderer.clearScreen();
    const auto& metrics = UITheme::getInstance().getMetrics();
    GUI.drawHeader(renderer,
                   Rect{0, metrics.topPadding, renderer.getScreenWidth(), UITheme::getInstance().getTitledHeaderHeight()},
                   tr(STR_BACKUP_RESTORE));
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2,
                              busy == Busy::Exporting ? tr(STR_BACKUP_EXPORTING) : tr(STR_BACKUP_RESTORING), true);
    renderer.displayBuffer();
    return;
  }

  UiListActivity::render(std::move(lock));
}
