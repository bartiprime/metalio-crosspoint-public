#include "SettingsActivity.h"

#include <BlePageTurner.h>
#include <LibraryBuilder.h>
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "AboutActivity.h"
#include "audio/BtAudioOutput.h"
#include "audio/UiSounds.h"
#include "BackupRestoreActivity.h"
#include "BatteryStatsActivity.h"
#include "BleSettingsActivity.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "ClockSettingsActivity.h"
#include "CrossPointSettings.h"
#include "FlickCalibrationActivity.h"
#include "FontDownloadActivity.h"
#include "HomeButtonSettingsActivity.h"
#include "KOReaderSettingsActivity.h"
#include "KeyboardLayoutsActivity.h"
#include "LanguageSelectActivity.h"
#include "LibraryIndexActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "OptionSelectActivity.h"
#include "ReadingStatsActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "SilentRestart.h"
#include "StatusBarSettingsActivity.h"
#include "TextSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "system/ChargerCurrent.h"
#include "util/PluginEvents.h"
#include "util/SystemKnobs.h"

namespace fui = freeink::ui;

namespace {
// Shared-list filtering for this board; false drops the row from the device UI
// (it stays in the shared list for the web settings API).
SettingsSection sectionFor(const SettingInfo& setting);

bool showOnDevice(const SettingInfo& setting) {
  if (sectionFor(setting) == SettingsSection::End || home_button::isSetting(setting.valuePtr)) return false;
  // The sunlight fading fix is a grayscale-waveform compensation that does
  // not apply on the X4 Pro / X4 Classic (plain OTP waveform, same panels).
  if (setting.valuePtr == &CrossPointSettings::fadingFix && (BoardConfig::isX4Pro() || BoardConfig::isX4Classic())) {
    return false;
  }
  // Merged into the Text Settings screen.
  if (setting.category == StrId::STR_CAT_READER && setting.inTextSettings) return false;
  return true;
}

// End = not a row here: other categories (KOReader Sync, Status Bar, ...)
// only group the web UI; their rows live on their own sub-screens.
SettingsSection sectionFor(const SettingInfo& setting) {
  if (setting.valuePtr == &CrossPointSettings::libraryUseMetadata ||
      setting.valuePtr == &CrossPointSettings::removeReadBooksFromRecents ||
      setting.valuePtr == &CrossPointSettings::moveFinishedToReadFolder) {
    return SettingsSection::Library;
  }
  if (setting.valuePtr == &CrossPointSettings::sleepTimeoutMinutes ||
      setting.valuePtr == &CrossPointSettings::standbyAutoOffMinutes) {
    return SettingsSection::Display;
  }
  if (setting.category == StrId::STR_CAT_DISPLAY) return SettingsSection::Display;
  if (setting.category == StrId::STR_CAT_READER) return SettingsSection::Reader;
  if (setting.category == StrId::STR_CAT_CONTROLS) return SettingsSection::Controls;
  if (setting.category == StrId::STR_CAT_SYSTEM) return SettingsSection::General;
  return SettingsSection::End;
}

// Device-only action rows added per section, beyond the shared list.
constexpr size_t EXTRA_ROWS_PER_SECTION = 6;
}  // namespace

void SettingsActivity::rebuildSettings() {
  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  // Rescan /dictionaries on every rebuild: cheap (one directory listing) and
  // picks up dictionaries copied to the SD card since the last visit.
  std::vector<DictionaryEntry> dictionaries;
  DictionaryRegistry::discover(dictionaries);

  auto all = getSettingsList(&sdFontSystem.registry(), &dictionaries);

  const size_t count = std::count_if(all.begin(), all.end(), [this](const SettingInfo& setting) {
    return sectionFor(setting) == section && showOnDevice(setting);
  });
  settings.clear();
  settings.reserve(count + EXTRA_ROWS_PER_SECTION);

  const auto addAction = [this](SettingsSection target, StrId name, SettingAction action) {
    if (target == section) settings.push_back(SettingInfo::Action(name, action));
  };

  // Language leads so a user stuck in an unfamiliar language finds it first.
  addAction(SettingsSection::General, StrId::STR_LANGUAGE, SettingAction::Language);
  addAction(SettingsSection::General, StrId::STR_KEYBOARD_LAYOUTS, SettingAction::KeyboardLayouts);
  addAction(SettingsSection::Reader, StrId::STR_TEXT_SETTINGS, SettingAction::TextSettings);
  addAction(SettingsSection::Reader, StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts);
  if (BoardConfig::hasHomeKey()) {
    addAction(SettingsSection::Controls, StrId::STR_HOME_BUTTON, SettingAction::HomeButton);
  }
  if (!BoardConfig::hasTouch()) {
    addAction(SettingsSection::Controls, StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons);
  }
  // IMU2: the guided calibration for the flick gesture. Offered only where the
  // accelerometer identified itself -- the screen would have nothing to sample.
  if (halTiltSensor.isAvailable()) {
    addAction(SettingsSection::Controls, StrId::STR_FLICK_CALIBRATE, SettingAction::TiltCalibration);
  }

  for (auto& setting : all) {
    if (sectionFor(setting) != section || !showOnDevice(setting)) continue;
    settings.push_back(std::move(setting));
  }

  // Clock configuration only exists where the RTC probe found hardware.
  if (halClock.isAvailable()) {
    addAction(SettingsSection::General, StrId::STR_CLOCK, SettingAction::ClockSettings);
  }
  addAction(SettingsSection::Reader, StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar);
  addAction(SettingsSection::Reader, StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache);
  // r163 3.3: what the reader measured -- session time, pages and pages/hour, per book
  // and overall. A read-only sub-page, so it sits beside the other Reader destinations.
  addAction(SettingsSection::Reader, StrId::STR_READING_STATISTICS, SettingAction::ReadingStats);
  // LIB1: the library index gets its own row with status and three verbs, because
  // "refresh the shelf" is not a boolean and the reader needs to see how far the
  // last walk got before deciding to resume it.
  addAction(SettingsSection::Reader, StrId::STR_LIBRARY_INDEX, SettingAction::LibraryIndex);

  addAction(SettingsSection::Network, StrId::STR_WIFI_NETWORKS, SettingAction::Network);
  if (bleturner::status().compiledIn) {
    addAction(SettingsSection::Network, StrId::STR_BT_PAGE_TURNER, SettingAction::Bluetooth);
  }
  addAction(SettingsSection::Network, StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync);
  addAction(SettingsSection::Network, StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser);
  addAction(SettingsSection::Network, StrId::STR_PLUGINS, SettingAction::Plugins);

  // OTA fetches this board's own release asset (see OtaUpdater); boards whose
  // asset isn't published yet just report no update available. The Picco's
  // stock layout has one app slot (partitions_picco.csv), so it cannot update
  // itself either way.
  if (!BoardConfig::isPicco()) {
    addAction(SettingsSection::General, StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates);
    addAction(SettingsSection::General, StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate);
  }
  // r99.9 (operator request A2): Battery statistics first, About directly
  // below it -- the two device-record rows sit together at the end of System.
  // BACKUP1 (r102): the full-profile backup/restore page. System, because it
  // is a record about this device's whole setup, not a reading preference.
  addAction(SettingsSection::General, StrId::STR_BACKUP_RESTORE, SettingAction::BackupRestore);
  // r97 BATT2: the measured battery statistics page. General (System), because it
  // is a record about the device, not a reading preference.
  addAction(SettingsSection::General, StrId::STR_BATTERY_STATISTICS, SettingAction::BatteryStats);
  addAction(SettingsSection::General, StrId::STR_ABOUT, SettingAction::About);
  rebuildRowItems();
}

SettingsActivity::SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const SettingsSection section)
    : UiListActivity("Settings", renderer, mappedInput), section(section) {}

void SettingsActivity::onEnter() {
  UiListActivity::onEnter();

  // r162 HYG2: the quickResumeSleepScreen mirror that used to run here is gone
  // with its row (SettingsList.cpp). `sleepScreen == QUICK_RESUME` is the only
  // thing the sleep path consults, so there was nothing to keep in sync.

  rebuildSettings();
}

// Rebuilds rowValues_/rowItems_ (label + actionValue) for settings.
// Structural — call only when the setting list changes, never from
// buildScreen(), which only refreshes rowValues_ content and rowItems_[].value
// pointers in place.
void SettingsActivity::rebuildRowItems() {
  rowValues_.assign(settings.size(), std::string());
  rowItems_.clear();
  rowItems_.reserve(settings.size());
  for (size_t i = 0; i < settings.size(); i++) {
    fui::ListItem item;
    item.label = I18N.get(settings[i].nameId);
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
}

const char* SettingsActivity::headerTitle() const {
  return I18N.get(SETTINGS_SECTION_TITLES[static_cast<size_t>(section)]);
}

void SettingsActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  // Most rows repaint a different surface (popup, sub-activity, new value);
  // a lingering tap flash would gray an unrelated element.
  app.clearTapFlash();
  nav.selected = index;
  toggleSelectedSetting();
  requestUpdate();
}

void SettingsActivity::applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr) {
  // Step 2.5 item 8: the charge-current row takes effect NOW, not on the next boot --
  // the operator picks 1.5 A to charge faster and expects the charger to do it while
  // the cable is still in. applyChoice() logs the read-back either way.
  if (valuePtr == &CrossPointSettings::chargeCurrent) {
    chargercurrent::applyChoice(SETTINGS.chargeCurrent);
    return;
  }
  // Step 2.5 items 9 and 11 also live in this screen, so a change takes effect
  // here and not on the next reboot. Both delegate to systemKnobs, which owns the
  // value tables, the vendor bounds and the one log line per apply.
  if (valuePtr == &CrossPointSettings::idleCpuFreq) {
    // Safe on this screen: the settings UI is idle by definition, and
    // HalPowerManager applies the new step immediately if the device is already
    // at the idle frequency. The full-speed restore path is not touched.
    systemKnobs::applyIdleFreq(SETTINGS.idleCpuFreq);
    return;
  }
  if (valuePtr == &CrossPointSettings::panelVcomDepth) {
    // VCOM is STORE-ONLY here (r163 D9): the driver keeps the level and writes it once per
    // panel power-on, from epd_board_poweron() -> tps_set_vcom(). Nothing is written to the
    // panel from this handler, because a bias change mid-scan is a hardware risk, not a
    // glitch. Ask for a FULL drive: the new level reaches the glass at that power-on, and a
    // diff-based update would neither show it nor scrub the old level's residue.
    systemKnobs::applyPanelVcom(SETTINGS.panelVcomDepth);
    requestFullRefresh();
    return;
  }
  // AUDIO2/AUDIO3: the two sound rows (Controls). The audio modules cache their switch
  // as an atomic at init(), and btaudio::setGate() / uisounds::setEnabled() are the
  // only writers that also move that cached copy -- writing the settings byte from
  // the generic toggle path alone would leave the audio path on its boot-time value
  // until the next restart, i.e. a row that appears to do nothing. Neither call powers
  // the rail: TTS1 brings P1.4 up on the first sound that actually plays, not here.
  //
  // PERSISTENCE IS THE CALLER'S (AUDIO3, the S2 fix): every path into this function
  // -- the in-place step at :350, the popup at :410, the toggle at :646 -- has already
  // called SETTINGS.saveToFile() exactly once. The setters used to save again, so ONE
  // toggle tap wrote the settings file TWICE and the second write ran on the input
  // path behind the popup's own write. The setters no longer save; the count is now
  // one, and `uisound` prints the running total (persiststore::saveCount()) so the
  // claim is readable from the device instead of argued from the call graph.
  if (valuePtr == &CrossPointSettings::audioOutEnabled) {
    btaudio::setGate(SETTINGS.audioOutEnabled != 0);
    return;
  }
  if (valuePtr == &CrossPointSettings::uiSoundsEnabled) {
    uisounds::setEnabled(SETTINGS.uiSoundsEnabled != 0);
    return;
  }
  // Theme changes take effect immediately, on this screen — reload the theme
  // and re-derive the app's tokens so the very next repaint is in the new look.
  if (valuePtr != &CrossPointSettings::uiTheme) {
    return;
  }
  UITheme::getInstance().reload();
  // Re-derive the shared tokens for the new look; the gate stays closed until
  // the repaint that rebuilds the interaction table in the new layout.
  resetUi();
}

bool SettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

settingsrow::RowFacts SettingsActivity::settingRowFacts(const SettingInfo& setting) {
  settingsrow::RowFacts facts;
  facts.isAction = setting.type == SettingType::ACTION;
  facts.isToggle = setting.type == SettingType::TOGGLE;
  facts.isEnum = setting.type == SettingType::ENUM;
  facts.isValue = setting.type == SettingType::VALUE;
  facts.hasDescription = setting.description != StrId::STR_NONE_OPT;
  if (facts.isEnum) {
    // Dynamic rows (font family, font size, dictionary, long-press menu, short
    // power click) carry their options as strings; the rest as StrId.
    const auto labels = setting.enumLabels();
    facts.optionCount = setting.enumStringValues.empty() ? labels.size() : setting.enumStringValues.size();
    // The same OFF/ON test the list has always used to decide a checkbox. It is
    // here, not in buildScreen(), so a two-label enum that is NOT a boolean
    // (FIT/CROP, LIST/TOOLBAR) reads as a choice and opens the list.
    facts.isOffOnPair = setting.enumStringValues.empty() && labels.size() == 2 &&
                        labels[0] == StrId::STR_STATE_OFF && labels[1] == StrId::STR_STATE_ON;
  }
  return facts;
}

std::vector<std::string> SettingsActivity::optionLabels(const SettingInfo& setting) const {
  // Exactly what the row offers right now: a dynamic row's list is rebuilt in
  // rebuildSettings() on every entry, so this is the same set the row's value
  // text was drawn from.
  if (!setting.enumStringValues.empty()) return setting.enumStringValues;
  std::vector<std::string> out;
  const auto labels = setting.enumLabels();
  out.reserve(labels.size());
  for (const StrId id : labels) out.push_back(I18N.get(id));
  return out;
}

void SettingsActivity::openOptionSelector(const SettingInfo& setting) {
  auto labels = optionLabels(setting);
  if (labels.size() < 2) return;  // nothing to choose; the row stays as it is

  const int current = setting.valuePtr != nullptr
                          ? static_cast<int>(SETTINGS.*(setting.valuePtr))
                          : (setting.valueGetter ? static_cast<int>(setting.valueGetter()) : 0);

  auto activity = makeUniqueNoThrow<OptionSelectActivity>(renderer, mappedInput, setting.nameId, std::move(labels),
                                                         current);
  if (!activity) {
    LOG_ERR("SET", "OOM: option selector");
    return;
  }
  // Copied before the push: rebuildSettings() replaces every SettingInfo, so the
  // reference `setting` is dead by the time the result comes back.
  const auto valuePtr = setting.valuePtr;
  const auto setter = setting.valueSetter;
  startActivityForResult(std::move(activity), [this, valuePtr, setter](const ActivityResult& result) {
    // Back / a tap outside: the row is untouched and nothing is persisted.
    if (result.isCancelled) return;
    const auto* chosen = std::get_if<OptionIndexResult>(&result.data);
    if (chosen == nullptr) return;
    if (valuePtr != nullptr) {
      SETTINGS.*valuePtr = static_cast<uint8_t>(chosen->index);
    } else if (setter) {
      // Dynamic row: index -> stored value is the row's own setter, i.e. the
      // exact function cycling used -- for the font family that includes the
      // built-in/SD-name split (FontFamilyRow.h).
      setter(static_cast<uint8_t>(chosen->index));
    } else {
      return;
    }
    // Same write, same slot, same save call as the in-place step it replaces.
    SETTINGS.saveToFile();
    // Re-derives the row's value text (and, for the dynamic rows, the option
    // list) WITHOUT touching nav: the settings list keeps the scroll position
    // the operator left it at -- nothing here resets the nav, and this screen's
    // onEnter() (the only caller of activeNav().reset()) is not re-run on a pop.
    rebuildSettings();
    applyUiSettingChange(valuePtr);
  });
}

void SettingsActivity::toggleSelectedSetting() {
  mappedInput.resetHomeButtonInput();
  const int selectedSetting = nav.selected;
  if (selectedSetting < 0 || selectedSetting >= static_cast<int>(settings.size())) {
    return;
  }

  const auto& setting = settings[selectedSetting];
  // r162 HYG2: sleepScreen no longer needs a companion mirror update -- the row
  // that consumed it (quickResumeSleepScreen) is gone. See SettingsList.cpp.

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }

  if (setting.nameId == StrId::STR_STANDBY_AUTO_OFF) {
    openStandbyAutoOffPicker();
    return;
  }

  // SET3: one decision, shared with buildScreen (SettingsRowKind.h).
  const auto kind = settingKind(setting);

  if (kind == settingsrow::Kind::Inert) {
    return;
  } else if (kind == settingsrow::Kind::Checkbox) {
    // A boolean: the OFF/ON enum pairs that draw as checkboxes land here too.
    if (setting.valuePtr != nullptr) {
      const bool currentValue = SETTINGS.*(setting.valuePtr);
      SETTINGS.*(setting.valuePtr) = !currentValue;
    } else if (setting.valueGetter && setting.valueSetter) {
      setting.valueSetter(setting.valueGetter() == 0 ? 1 : 0);
    } else {
      return;
    }
  } else if (kind == settingsrow::Kind::Selector) {
    // The discrete rows: open the option list instead of walking the value one
    // step. Nothing is written here -- the list hands back the chosen index.
    openOptionSelector(setting);
    return;
  } else if (kind == settingsrow::Kind::Popup) {
    // A discrete row that carries a description line keeps the modal popup,
    // which is the only surface on this screen with room for that line (r97 KEY1).
    const auto enumLabels = setting.enumLabels();
    if (setting.valuePtr != nullptr) {
      const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
      const auto valuePtr = setting.valuePtr;
      auto onSelect = [this, valuePtr](int idx) {
        SETTINGS.*valuePtr = static_cast<uint8_t>(idx);
        SETTINGS.saveToFile();
        rebuildSettings();
        applyUiSettingChange(valuePtr);
      };
      std::vector<const char*> pages;
      pages.reserve(enumLabels.size());
      for (const StrId id : enumLabels) pages.push_back(I18N.get(id));
      optionPopup.show(I18N.get(setting.nameId), I18N.get(setting.description), pages.data(),
                       static_cast<int>(pages.size()), currentValue, std::move(onSelect));
      requestUpdate();
      return;
    }
    if (setting.valueGetter && setting.valueSetter) {
      const auto valueSetter = setting.valueSetter;
      auto onSelect = [this, valueSetter](int idx) {
        valueSetter(static_cast<uint8_t>(idx));
        SETTINGS.saveToFile();
        rebuildSettings();
      };
      if (!setting.enumStringValues.empty()) {
        optionPopup.show(setting.nameId, setting.enumStringValues, setting.valueGetter(), std::move(onSelect));
      } else {
        std::vector<const char*> pages;
        pages.reserve(enumLabels.size());
        for (const StrId id : enumLabels) pages.push_back(I18N.get(id));
        optionPopup.show(I18N.get(setting.nameId), I18N.get(setting.description), pages.data(),
                         static_cast<int>(pages.size()), setting.valueGetter(), std::move(onSelect));
      }
      requestUpdate();
      return;
    }
    return;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    // Fewer than two options: there is nothing to choose, so the row only
    // normalises its stored byte (0) the way the old wrap-around did.
    const auto enumLabels = setting.enumLabels();
    if (enumLabels.empty()) return;
    SETTINGS.*(setting.valuePtr) = (SETTINGS.*(setting.valuePtr) + 1) % static_cast<uint8_t>(enumLabels.size());
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumLabels().size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    if (totalValues == 0) return;
    setting.valueSetter((setting.valueGetter() + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const uint8_t before = SETTINGS.*(setting.valuePtr);
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
    // SLIDE1: same field, same range, same wrap -- only the repaint changes. The
    // step's paint is ONE diff-masked DU over the row (no row-9 completion drive,
    // no clean), the way the frontlight panel's brightness drag already behaved.
    // Armed only when the byte actually moved, so a clamped step that paints
    // nothing cannot leave the one-shots for the next screen's drive.
    if (SETTINGS.*(setting.valuePtr) != before) armValueStepRepaint();
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::HomeButton: {
        // Activities must outlive this call and are owned by the activity stack.
        auto activity = makeUniqueNoThrow<HomeButtonSettingsActivity>(renderer, mappedInput);
        if (!activity) {
          LOG_ERR("SET", "OOM: Home button settings");
          return;
        }
        startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
        return;
      }
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ClockSettings:
        if (auto activity = makeUniqueNoThrow<ClockSettingsActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(activity), resultHandler);
        } else {
          LOG_ERR("SETTINGS", "OOM: ClockSettingsActivity");
        }
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network: {
        auto activity = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, false);
        if (!activity) {
          LOG_ERR("SETTINGS", "OOM: WifiSelectionActivity");
          return;
        }
        startActivityForResult(std::move(activity), [](const ActivityResult&) {
          SETTINGS.saveToFile();
          // Every other WiFi consumer hands the radio to a session it owns;
          // these rows only save credentials, so nothing here would ever
          // release the driver's heap. The scan alone brings it up, so tear
          // down whether or not the user joined a network.
          if (WiFi.getMode() == WIFI_MODE_NULL) return;
          WiFi.disconnect(false);
          delay(30);
          // Unlike the onExit() teardowns, this runs from the loop task with
          // no lock held; the restart popup paints straight to the panel.
          RenderLock lock;
          silentRestartToSettings();
        });
        break;
      }
      case SettingAction::Bluetooth: {
        auto activity = makeUniqueNoThrow<BleSettingsActivity>(renderer, mappedInput);
        if (!activity) {
          LOG_ERR("SETTINGS", "OOM: BleSettingsActivity");
          return;
        }
        startActivityForResult(std::move(activity), resultHandler);
        break;
      }
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::TiltCalibration:
        startActivityForResult(std::make_unique<FlickCalibrationActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { requestUpdate(); });
        break;
      case SettingAction::LibraryIndex: {
        auto activity = makeUniqueNoThrow<LibraryIndexActivity>(renderer, mappedInput);
        if (!activity) {
          LOG_ERR("SET", "OOM: LibraryIndexActivity");
          return;
        }
        startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
        break;
      }
      case SettingAction::CheckForUpdates:
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SdFirmwareUpdate:
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
        // PARKED for this board while the port is being stabilised: a bad update image is the one
        // failure mode a USB-only recovery cannot always undo. The activity stays compiled; only the
        // entry point is closed.
        LOG_ERR("SETTINGS", "SD firmware update is parked on this board");
#else
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
#endif
        break;
      case SettingAction::DownloadFonts:
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettings();
                               });
        break;
      case SettingAction::TextSettings:
        startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                      TextSettingsActivity::Tab::Family),
                               [this](const ActivityResult&) {
                                 // TextSettingsActivity saves on each change; no save needed here.
                                 rebuildSettings();
                               });
        break;
      case SettingAction::Language:
        // Row labels are translated once in rebuildRowItems() and don't
        // re-run on Pop (see ActivityManager::loop()), so a language switch
        // needs an explicit rebuild here rather than the generic resultHandler.
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettings();
                               });
        break;
      case SettingAction::Plugins: {
        static constexpr StrId OPTIONS[] = {StrId::STR_ENABLED, StrId::STR_DISABLED, StrId::STR_CANCEL};
        optionPopup.show(StrId::STR_PLUGINS, OPTIONS, 3, SETTINGS.pluginsEnabled ? 0 : 1, [this](const int idx) {
          if (idx == 2) return;
          SETTINGS.pluginsEnabled = idx == 0 ? 1 : 0;
          // Turning plugins on brings back a dismissed Plugin Hub install row.
          if (SETTINGS.pluginsEnabled) SETTINGS.pluginHubPromptHidden = 0;
          SETTINGS.saveToFile();
          // Applies to event delivery now, not after a reboot.
          pluginevents::refreshSubscriptions();
          rebuildSettings();
        });
        requestUpdate();
        return;
      }
      case SettingAction::KeyboardLayouts:
        if (auto activity = makeUniqueNoThrow<KeyboardLayoutsActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(activity), nullptr);
        } else {
          LOG_ERR("SETTINGS", "OOM: KeyboardLayoutsActivity");
        }
        break;
      case SettingAction::About:
        if (auto activity = makeUniqueNoThrow<AboutActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(activity), nullptr);
        } else {
          LOG_ERR("SETTINGS", "OOM: AboutActivity");
        }
        break;
      case SettingAction::BackupRestore:
        if (auto activity = makeUniqueNoThrow<BackupRestoreActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(activity), resultHandler);
        } else {
          LOG_ERR("SETTINGS", "OOM: BackupRestoreActivity");
        }
        break;
      case SettingAction::ReadingStats:
        if (auto activity = makeUniqueNoThrow<ReadingStatsActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(activity), resultHandler);
        } else {
          LOG_ERR("SETTINGS", "OOM: ReadingStatsActivity");
        }
        break;
      case SettingAction::BatteryStats:
        // r97 BATT2. A sub-page (resultHandler refreshes this list on return).
        if (auto activity = makeUniqueNoThrow<BatteryStatsActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(activity), resultHandler);
        } else {
          LOG_ERR("SETTINGS", "OOM: BatteryStatsActivity");
        }
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  SETTINGS.saveToFile();
  const auto valuePtr = setting.valuePtr;  // rebuildSettings() replaces `setting`
  rebuildSettings();
  applyUiSettingChange(valuePtr);
  // Rows can appear/disappear with a change (e.g. the footnote-back toggle).
  nav.selected = std::min(selectedSetting, static_cast<int>(settings.size()) - 1);
}

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
          CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
          StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, StrId::STR_SLEEP_NEVER),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::openStandbyAutoOffPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "StandbyAutoOffInterval", StrId::STR_STANDBY_AUTO_OFF,
          SETTINGS.standbyAutoOffMinutes, CrossPointSettings::MIN_STANDBY_AUTO_OFF_MINUTES,
          CrossPointSettings::MAX_STANDBY_AUTO_OFF_MINUTES, 5, 30, StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false,
          StrId::STR_SLEEP_NEVER),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.standbyAutoOffMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

std::string SettingsActivity::settingValueText(const SettingInfo& setting) {
  if (setting.action == SettingAction::HomeButton) return tr(STR_CONFIGURE);
  if (setting.action == SettingAction::LibraryIndex) {
    // The status column, not a date: the RTC backup is lost on this board, so
    // every file the device writes says Jan 1 1980 and an age-based "last
    // indexed" line would read the same forever. Counts are honest; a date is not.
    library::IndexSummary summary;
    library::PartialSummary part;
    const bool havePartial = library::readLibraryPartialSummary(part);
    char text[64];
    if (library::readLibraryIndexSummary(summary)) {
      if (havePartial) {
        std::snprintf(text, sizeof(text), "%u books, %u paused", static_cast<unsigned>(summary.books),
                      static_cast<unsigned>(part.records));
      } else {
        std::snprintf(text, sizeof(text), "%u books", static_cast<unsigned>(summary.books));
      }
    } else if (havePartial) {
      std::snprintf(text, sizeof(text), "%u paused", static_cast<unsigned>(part.records));
    } else {
      std::snprintf(text, sizeof(text), "%s", tr(STR_LIBRARY_INDEX_NONE));
    }
    return std::string(text);
  }
  if (setting.action == SettingAction::Plugins) return SETTINGS.pluginsEnabled ? tr(STR_ENABLED) : tr(STR_DISABLED);
  if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    // Guard like the valueGetter branch below: a corrupt/migrated settings
    // byte must not index past the enum table.
    const uint8_t value = SETTINGS.*(setting.valuePtr);
    const auto enumLabels = setting.enumLabels();
    if (value >= enumLabels.size()) return "";
    return I18N.get(enumLabels[value]);
  }
  if (setting.type == SettingType::ENUM && setting.valueGetter) {
    const uint8_t value = setting.valueGetter();
    if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
      return setting.enumStringValues[value];
    }
    const auto enumLabels = setting.enumLabels();
    if (value < enumLabels.size()) {
      return I18N.get(enumLabels[value]);
    }
    return "";
  }
  if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
      if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
        return tr(STR_SLEEP_NEVER);
      }
      char valueBuffer[32];
      snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
               static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
      return valueBuffer;
    }
    if (setting.nameId == StrId::STR_STANDBY_AUTO_OFF) {
      if (SETTINGS.standbyAutoOffMinutes == 0 ||
          SETTINGS.standbyAutoOffMinutes >= CrossPointSettings::STANDBY_AUTO_OFF_NEVER_MINUTES) {
        return tr(STR_SLEEP_NEVER);
      }
      char valueBuffer[32];
      snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
               static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
      return valueBuffer;
    }
    if (setting.nameId == StrId::STR_LINE_SPACING) {
      return std::to_string(SETTINGS.*(setting.valuePtr)) + "%";
    }
    return std::to_string(SETTINGS.*(setting.valuePtr));
  }
  return "";
}

void SettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + UITheme::getInstance().getTitledHeaderHeight()), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems_ (label/actionValue) was built by rebuildRowItems(); only the
  // live value text needs refreshing here, by assigning into the existing
  // rowValues_ strings (no vector growth).
  for (size_t i = 0; i < settings.size(); i++) {
    const auto& setting = settings[i];
    // SET3: the checkbox decision is the activation decision (SettingsRowKind.h),
    // so a row can never draw as a switch while a tap opens a list.
    const bool checkbox = settingKind(setting) == settingsrow::Kind::Checkbox;
    if (checkbox && (setting.valuePtr || setting.valueGetter)) {
      const bool checked = setting.valuePtr ? SETTINGS.*(setting.valuePtr) != 0 : setting.valueGetter() != 0;
      rowValues_[i].clear();
      GUI.setCheckboxRow(rowItems_[i], checked);
    } else {
      rowValues_[i] = settingValueText(setting);
      rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
    }
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Touch uses the standard list sizing, as in Bluetooth; buttons keep compact labels.
  // maxLines also marks smallText as explicit so the list does not replace it with bodyText.
  props.labelText = mappedInput.hasTouch() ? screen.theme().bodyText : screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void SettingsActivity::drawFooter() {
  const int selected = nav.selected;
  const bool picker = selected >= 0 && selected < static_cast<int>(settings.size()) &&
                      (settingsrow::opensAnotherSurface(settingKind(settings[selected])) ||
                       settings[selected].nameId == StrId::STR_TIME_TO_SLEEP ||
                       settings[selected].nameId == StrId::STR_STANDBY_AUTO_OFF);
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), picker ? tr(STR_SELECT) : tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SettingsActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
