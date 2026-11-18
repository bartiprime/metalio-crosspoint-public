#include "CrossPointSettings.h"

#include <BlePageTurnerJson.h>
#include <I18n.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>

#include "I18nKeys.h"
#include "ReaderFontSizes.h"
#include "ReaderLineSpacing.h"
#include "SettingsList.h"
#include "fontIds.h"
#include "util/ParagraphIndentMigration.h"
#include "util/PluginLocations.h"

namespace {

// Stack buffer for "<key>_obf" key construction — avoids a std::string
// allocation per obfuscated setting on every save and load.
constexpr size_t OBF_KEY_BUF = 64;

// Null-terminated copy into a fixed-size settings field.
void copyToField(char* dest, const char* src, const size_t maxLen) {
  strncpy(dest, src, maxLen - 1);
  dest[maxLen - 1] = '\0';
}

}  // namespace

void CrossPointSettings::validateFrontButtonMapping(CrossPointSettings& settings) {
  const uint8_t mapping[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        settings.frontButtonBack = FRONT_HW_BACK;
        settings.frontButtonConfirm = FRONT_HW_CONFIRM;
        settings.frontButtonLeft = FRONT_HW_LEFT;
        settings.frontButtonRight = FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

uint8_t CrossPointSettings::sleepTimeoutEnumToMinutes(const uint8_t legacyValue) {
  switch (legacyValue) {
    case SLEEP_1_MIN:
      return 1;
    case SLEEP_5_MIN:
      return 5;
    case SLEEP_15_MIN:
      return 15;
    case SLEEP_30_MIN:
      return 30;
    case SLEEP_10_MIN:
    default:
      return 10;
  }
}

void CrossPointSettings::toJson(JsonDocument& doc) const {
  const CrossPointSettings& s = *this;

  auto visit = [&](const SettingInfo& info) {
    if (!info.key) return;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) return;

    if (info.stringOffset) {
      const char* strPtr = (const char*)&s + info.stringOffset;
      if (info.obfuscated) {
        char obfKey[OBF_KEY_BUF];
        snprintf(obfKey, sizeof(obfKey), "%s_obf", info.key);
        doc[obfKey] = obfuscation::obfuscateToBase64(strPtr);
      } else {
        doc[info.key] = strPtr;
      }
    } else {
      doc[info.key] = s.*(info.valuePtr);
    }
  };
  forEachBaseSetting(visit);

  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  doc["frontButtonBack"] = frontButtonBack;
  doc["frontButtonConfirm"] = frontButtonConfirm;
  doc["frontButtonLeft"] = frontButtonLeft;
  doc["frontButtonRight"] = frontButtonRight;
  // Font family and size — both use dynamic getter/setters in SettingsList (the
  // option lists depend on the SD font registry), so the generic loop skips them.
  doc["fontFamily"] = STORED_NOTOSANS;
  doc["fontSize"] = fontPointSize;
  // SD card font family name — not in SettingsList, save manually
  if (sdFontFamilyName[0] != '\0') {
    doc["sdFontFamilyName"] = sdFontFamilyName;
  }
  // Dictionary folder name — uses dynamic getter/setter in SettingsList, save manually
  if (dictionaryName[0] != '\0') {
    doc["dictionaryName"] = dictionaryName;
  }
  // The displayed option list varies by board, so this setting uses a dynamic
  // display-index mapping and is skipped by the generic persistence loop.
  doc["longPressMenuFunction"] = longPressMenuFunction;
  doc["shortPwrBtn"] = shortPwrBtn;
  // CAD2: the two cadence rows index a DISPLAY-ordered label list, so the generic
  // loop skips them (it writes by position). The stored byte is still the
  // REFRESH_FREQUENCY index it has always been, under the same keys: a saved
  // "Never" is 5 before and after this change.
  doc["refreshFrequency"] = refreshFrequency;
  doc["quickRefreshPages"] = quickRefreshPages;

  // Language -- managed by LanguageSelectActivity, not in SettingsList.
  // Stored as ISO code string ("EN", "DE", ...) for stability across enum reorders.
  doc["language"] = (language < getLanguageCount()) ? LANGUAGE_CODES[language] : "EN";

  // USB debug shell (0 = off). Hand-persisted: see CrossPointSettings.h.
  doc["usbDebugShell"] = usbDebugShell ? 1 : 0;
  // TTS1 audio output gate (0 = off, the default). Hand-persisted: see the header.
  doc["audioOutEnabled"] = audioOutEnabled ? 1 : 0;
  // AUDIO1 UI clicks (0 = off, the default). Hand-persisted: see the header.
  doc["uiSoundsEnabled"] = uiSoundsEnabled ? 1 : 0;
  // Browse Files view preference (0 = names without their extension). Hand-
  // persisted: it is toggled from the file browser's own actions menu, so it has
  // no SettingsList row and the generic loop skips it.
  doc["showFileExtensions"] = showFileExtensions ? 1 : 0;
  // r101.10: one-shot marker for the usbDriveExitDialog default flip (see fromJson). Written by
  // every save, so the migration there can never fire twice on the same file.
  doc["usbDriveExitDialogMigrated"] = 1;
  // r162 PWR4: one-shot marker for the Metalio shortPwrBtn default flip (see fromJson).
  doc["shortPwrBtnDefaultMigrated"] = 1;
  // r163 B1: one-shot marker for the line-height ladder -> percent move (see fromJson).
  doc["lineSpacingPercentMigrated"] = 1;

  // A uint16_t mask, so it does not fit the uint8_t generic loop. Omitted while
  // unconfigured, so the default keeps following the UI language.
  if (keyboardLayouts != 0) {
    doc["keyboardLayouts"] = keyboardLayouts;
  }

  bleturner::writeJson(ble, doc);
}

bool CrossPointSettings::fromJson(JsonVariantConst doc) {
  CrossPointSettings& s = *this;
  bool needsResave = false;

  auto clamp = [](uint8_t val, uint8_t maxVal, uint8_t def) -> uint8_t { return val < maxVal ? val : def; };

  auto visit = [&](const SettingInfo& info) {
    if (!info.key) return;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) return;

    if (info.stringOffset) {
      // destPtr starts out holding the struct-initializer default; it stays that
      // way unless the document actually carries a value for this key.
      char* destPtr = (char*)&s + info.stringOffset;
      if (info.stringMaxLen == 0) {
        LOG_ERR("CPS", "Misconfigured SettingInfo: stringMaxLen is 0 for key '%s'", info.key);
        destPtr[0] = '\0';
        needsResave = true;
        return;
      }

      bool loaded = false;
      if (info.obfuscated) {
        char obfKey[OBF_KEY_BUF];
        snprintf(obfKey, sizeof(obfKey), "%s_obf", info.key);
        bool ok = false;
        bool tooLong = false;
        const std::string decoded =
            obfuscation::deobfuscateFromBase64(doc[obfKey] | "", info.stringMaxLen - 1, &ok, &tooLong);
        if (tooLong) {
          LOG_ERR("CPS", "Oversized obfuscated value for key '%s'", info.key);
          needsResave = true;
        }
        if (ok && !decoded.empty()) {
          copyToField(destPtr, decoded.c_str(), info.stringMaxLen);
          loaded = true;
        }
      }
      if (!loaded) {
        // Read as const char*, never `| std::string(...)`: ArduinoJson's
        // std::string converter drags a per-TU copy of the serializer into
        // flash. See the note in PersistableStore.h.
        const char* raw = doc[info.key].is<const char*>() ? doc[info.key].as<const char*>() : nullptr;
        if (raw) {
          // Obfuscated field recovered from a legacy plaintext value -> resave.
          if (info.obfuscated && strcmp(raw, destPtr) != 0) needsResave = true;
          copyToField(destPtr, raw, info.stringMaxLen);
        }
      }
    } else {
      const uint8_t fieldDefault = s.*(info.valuePtr);  // struct-initializer default, read before we overwrite it
      uint8_t v = doc[info.key] | fieldDefault;
      if (info.type == SettingType::ENUM) {
        v = clamp(v, (uint8_t)info.enumLabels().size(), fieldDefault);
      } else if (info.type == SettingType::TOGGLE) {
        v = clamp(v, (uint8_t)2, fieldDefault);
      } else if (info.type == SettingType::VALUE) {
        if (v < info.valueRange.min)
          v = info.valueRange.min;
        else if (v > info.valueRange.max)
          v = info.valueRange.max;
      }
      s.*(info.valuePtr) = v;
    }
  };
  forEachBaseSetting(visit);

  // Plugin System did not exist in older settings files. Preserve the behavior
  // of users who already have SD plugins installed, while keeping the new
  // default disabled on clean installs.
  if (doc["pluginsEnabled"].isNull()) {
    pluginsEnabled = PluginLocations::anyPluginInstalledOnDisk() ? 1 : 0;
    needsResave = true;
  }

  const auto indentSpaces = doc["paragraphIndentSpaces"];
  const bool hasSavedWidth = indentSpaces.is<int>();
  const int savedWidth = hasSavedWidth ? indentSpaces.as<int>() : 0;
  paragraphIndentSpaces = migrateParagraphIndentSpaces(hasSavedWidth, savedWidth, extraParagraphSpacing != 0);
  if (!hasSavedWidth || savedWidth < 0 || savedWidth > 5) needsResave = true;

  // Older files stored one combined touch mode under "touchReaderControls":
  // 0=off, 1=tap, 2=swipe, 3=inverted tap. Split it into the master toggle
  // plus the per-direction gesture pair (the generic loop above already folded
  // out-of-range toggle values back to the On default).
  if (doc["pageTurnGesture"].isNull() && doc["previousPageGesture"].isNull() &&
      doc["touchReaderControls"].is<uint8_t>()) {
    const uint8_t mode = doc["touchReaderControls"].as<uint8_t>();
    if (mode >= 1 && mode <= 3) {
      touchReaderControls = TOUCH_READER_ON;
      pageTurnGesture = mode == 1 ? TAP_ONLY : mode == 2 ? SWIPE_ONLY : INVERTED_TAP;
      previousPageGesture = pageTurnGesture;
      needsResave = true;
    }
  }

  if (doc["sleepTimeoutMinutes"].isNull() && !doc["sleepTimeout"].isNull()) {
    const uint8_t legacyValue =
        clamp(doc["sleepTimeout"] | (uint8_t)SLEEP_10_MIN, SLEEP_TIMEOUT_COUNT, (uint8_t)SLEEP_10_MIN);
    sleepTimeoutMinutes = sleepTimeoutEnumToMinutes(legacyValue);
    needsResave = true;
  }
  // r163 B1: line height is a percent now, in 1% steps. Older saves carry a
  // ladder slot instead: "lineHeight" is the seven-step ladder
  // (0..6 = 100/110/120/130/145/160/175 %), and a "lineSpacing" save predates
  // even that (0..3 = Tight/Normal/Wide/Extra Wide, which the ladder then
  // rendered at 110/120/130/145 %). Fold whichever key the file has to the
  // percent it was rendering at, ONCE per file: the marker is stamped by every
  // save, so an operator who then picks 143 % keeps it.
  if (doc["lineSpacingPercentMigrated"].isNull()) {
    int percent = lineSpacingPercent;
    if (doc["lineHeight"].is<uint8_t>()) {
      const int step = doc["lineHeight"].as<uint8_t>();
      percent = lineSpacing::percentFromLadderSlot(step);
      LOG_INF("CPS", "settings migration: lineHeight step %d -> %d%%", step, percent);
    } else if (doc["lineSpacing"].is<uint8_t>()) {
      const int legacy = doc["lineSpacing"].as<uint8_t>();
      percent = lineSpacing::percentFromLegacySlot(legacy);
      LOG_INF("CPS", "settings migration: legacy lineSpacing %d -> %d%%", legacy, percent);
    } else {
      LOG_INF("CPS", "settings migration: line spacing -> %d%% (no ladder value stored)", percent);
    }
    lineSpacingPercent = lineSpacing::clampPercent(percent);
    needsResave = true;
  }
  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  frontButtonBack = clamp(doc["frontButtonBack"] | (uint8_t)FRONT_HW_BACK, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_BACK);
  frontButtonConfirm =
      clamp(doc["frontButtonConfirm"] | (uint8_t)FRONT_HW_CONFIRM, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_CONFIRM);
  frontButtonLeft = clamp(doc["frontButtonLeft"] | (uint8_t)FRONT_HW_LEFT, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_LEFT);
  frontButtonRight =
      clamp(doc["frontButtonRight"] | (uint8_t)FRONT_HW_RIGHT, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_RIGHT);
  validateFrontButtonMapping(s);

  // Reader font size — an actual point size since 1.5. Files written by 1.4 and
  // earlier hold the old SMALL/MEDIUM/LARGE/EXTRA_LARGE slot in 0..3; no font is
  // renderable at those sizes, so the range is unambiguous and folds to the
  // point sizes those slots used to mean. Drop this once 1.4 upgrades are done.
  uint8_t storedFontSize = doc["fontSize"] | DEFAULT_FONT_POINT_SIZE;
  if (storedFontSize <= LEGACY_FONT_SIZE_MAX) {
    storedFontSize = 12 + storedFontSize * 2;  // 0,1,2,3 -> 12,14,16,18
    needsResave = true;
  }
  fontPointSize = storedFontSize;

  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  const uint8_t storedFontFamily = doc["fontFamily"] | STORED_NOTOSANS;
  fontFamily = NOTOSANS;
  // These controls use board-specific display-index mappings and are skipped by
  // the generic settings loop. Persisted action IDs remain stable across boards.
  longPressMenuFunction = clamp(doc["longPressMenuFunction"] | (uint8_t)LP_MENU_DISABLED,
                                (uint8_t)LONG_PRESS_MENU_FUNCTION_COUNT, (uint8_t)LP_MENU_DISABLED);
  shortPwrBtn = clamp(doc["shortPwrBtn"] | (uint8_t)IGNORE, (uint8_t)SHORT_PWRBTN_COUNT, (uint8_t)IGNORE);
  // CAD2: the two cadences are skipped by the loop for the same reason (their rows
  // carry a display-index map), so they are loaded here. Same key, same byte, same
  // bound as the loop applied before -- 0..REFRESH_FREQUENCY_COUNT-1, anything else
  // folding back to the value already in the field, which is what the loop's
  // `fieldDefault` was (the live value, i.e. the compiled-in default on a fresh load).
  {
    const uint8_t storedRefresh = doc["refreshFrequency"] | refreshFrequency;
    refreshFrequency = storedRefresh < REFRESH_FREQUENCY_COUNT ? storedRefresh : refreshFrequency;
    const uint8_t storedQuick = doc["quickRefreshPages"] | quickRefreshPages;
    quickRefreshPages = storedQuick < REFRESH_FREQUENCY_COUNT ? storedQuick : quickRefreshPages;
  }
  // USB debug shell: absent -> 0 (off). Anything non-zero folds to 1 so a hand-
  // edited settings file cannot enable a mode this code does not have.
  usbDebugShell = (doc["usbDebugShell"] | (uint8_t)0) != 0 ? 1 : 0;
  // TTS1: absent -> 0 (off). A device that never enables audio never touches the
  // P1.4 rail, the UART or I2S. Non-zero folds to 1.
  audioOutEnabled = (doc["audioOutEnabled"] | (uint8_t)0) != 0 ? 1 : 0;
  // AUDIO1: absent -> 0 (off), so an existing settings file never starts
  // clicking. Non-zero folds to 1.
  uiSoundsEnabled = (doc["uiSoundsEnabled"] | (uint8_t)0) != 0 ? 1 : 0;
  // Absent -> 0 (off), the pre-r163 behaviour. Fold any non-zero to 1.
  showFileExtensions = (doc["showFileExtensions"] | (uint8_t)0) != 0 ? 1 : 0;

  // r101.10 DLG2 / AUD-P2-4: the USB drive-mode exit frame is ON by default now, and a DEFAULT change alone
  // would not reach the operator -- every r101.9 save wrote "usbDriveExitDialog": 0, so the generic
  // loop above has already loaded that 0. One-shot migration: the marker key is written by the
  // resave below (toJson stamps it on every save), so this fires exactly once per settings file and
  // then the row follows the operator's own choice again -- an explicit OFF after this build is
  // never flipped back, because its save carries the marker.
  //
  // What the migration cannot do, and does not pretend to: tell the r101.9 default of 0 from a 0 the
  // operator chose on r101.9. That one 0 is flipped, once, and logged. The guard below only skips
  // the write when there is nothing to flip -- a legacy file that already says 1 keeps no marker
  // and costs no SD write on boot, and a later save stamps it.
  if (doc["usbDriveExitDialogMigrated"].isNull() && usbDriveExitDialog != 1) {
    usbDriveExitDialog = 1;
    needsResave = true;
    LOG_INF("CPS", "settings migration: usbDriveExitDialog -> 1 (exit frame ON)");
  }

  // r162 PWR4: the Metalio default for a short power press is SLEEP, and a default change alone
  // never reaches an operator who has already saved a settings file -- every earlier build wrote
  // "shortPwrBtn": 0 (IGNORE). On this board IGNORE is not "do nothing": main.cpp's Metalio
  // release-edge branch sleeps on any press from getPowerButtonDuration() (400 ms) up to
  // METALIO_POWER_OFF_HOLD_MS, so a stored 0 already means "sleep, but only after a half-second
  // hold". Folding it to SLEEP is therefore a threshold change, not a new action, and it is done
  // ONCE per file (the marker is stamped by every save, so an operator who picks Ignore afterwards
  // keeps it). Boards without the Metalio input path are untouched: their IGNORE really is inert.
  if (BoardConfig::isMetalioEInk4Plus() && doc["shortPwrBtnDefaultMigrated"].isNull()) {
    if (shortPwrBtn == IGNORE) {
      shortPwrBtn = SLEEP;
      LOG_INF("CPS", "settings migration: shortPwrBtn IGNORE -> SLEEP (click-and-release sleeps)");
    }
    needsResave = true;
  }
  if (BoardConfig::hasHomeKey() && doc["homeButtonLongPressAction"].isNull() &&
      !doc["longPressMenuFunction"].isNull()) {
    static constexpr HomeButtonAction LEGACY[] = {HomeButtonAction::Sync,       HomeButtonAction::Ignore,
                                                  HomeButtonAction::Bookmark,   HomeButtonAction::Dictionary,
                                                  HomeButtonAction::ReaderMenu, HomeButtonAction::CreateClipping};
    if (s.longPressMenuFunction < sizeof(LEGACY) / sizeof(LEGACY[0])) {
      s.homeButtonLongPressAction = static_cast<uint8_t>(LEGACY[s.longPressMenuFunction]);
      needsResave = true;
    }
  }

  // SD card font family name — not in SettingsList, load manually
  const char* sfn = doc["sdFontFamilyName"] | "";
  strncpy(sdFontFamilyName, sfn, sizeof(sdFontFamilyName) - 1);
  sdFontFamilyName[sizeof(sdFontFamilyName) - 1] = '\0';
  if (sdFontFamilyName[0] == '\0') {
    switch (storedFontFamily) {
      case LEGACY_NOTOSERIF:
        copyToField(sdFontFamilyName, "NotoSerif", sizeof(sdFontFamilyName));
        break;
      case LEGACY_OPENDYSLEXIC:
        copyToField(sdFontFamilyName, "OpenDyslexic", sizeof(sdFontFamilyName));
        break;
    }
  }
  if (storedFontFamily != STORED_NOTOSANS) needsResave = true;
  // Dictionary folder name — uses dynamic getter/setter in SettingsList, load manually
  copyToField(dictionaryName, doc["dictionaryName"] | "", sizeof(dictionaryName));
  // Language -- stored as code string for stability across enum reorders.
  if (doc["language"].is<const char*>()) {
    language = static_cast<uint8_t>(I18n::languageFromCode(doc["language"].as<const char*>()));
  }

  // Absent means unconfigured, which is the default.
  if (doc["keyboardLayouts"].is<uint16_t>()) {
    keyboardLayouts = doc["keyboardLayouts"].as<uint16_t>();
  }

  if (!bleturner::readJson(ble, doc)) {
    needsResave = true;
  }

  if (needsResave) {
    LOG_DBG("CPS", "Resaving settings to update format");
    requestResave();
  }

  LOG_DBG("CPS", "Settings loaded from file");

  return true;
}

CrossPointSettings::StatusBarSpec CrossPointSettings::statusBarSpec() const {
  StatusBarSpec spec;
  spec.showChapterPageCount = statusBarChapterPageCount != 0;
  spec.showBookProgressPercent = statusBarBookProgressPercentage != 0;
  spec.titleMode = statusBarTitle;
  spec.showBattery = statusBarBattery != 0;
  spec.showBatteryPercent = hideBatteryPercentage == HIDE_NEVER;
  spec.clockMode = statusBarClock;
  spec.clock12h = clockFormat == 1;
  spec.progressBarMode = statusBarProgressBar;
  spec.progressBarHeightPx =
      statusBarProgressBar != HIDE_PROGRESS ? static_cast<uint8_t>((statusBarProgressBarThickness + 1) * 2) : 0;
  spec.xtcMode = xtcStatusBarMode;
  return spec;
}

ReaderRenderSpec CrossPointSettings::readerRenderSpec(const uint16_t viewportWidth,
                                                      const uint16_t viewportHeight) const {
  ReaderRenderSpec spec;
  spec.fontId = getReaderFontId();
  spec.lineCompression = getReaderLineCompression();
  spec.characterSpacing = getCharacterSpacing();
  spec.wordSpacingPercent = wordSpacing;
  spec.extraParagraphSpacing = extraParagraphSpacing != 0;
  spec.paragraphIndentSpaces = paragraphIndentSpaces;
  spec.paragraphAlignment = paragraphAlignment;
  spec.viewportWidth = viewportWidth;
  spec.viewportHeight = viewportHeight;
  spec.hyphenationEnabled = hyphenationEnabled != 0;
  spec.embeddedStyle = embeddedStyle != 0;
  spec.imageRendering = imageRendering;
  spec.focusReadingEnabled = focusReadingEnabled != 0;
  return spec;
}

float CrossPointSettings::getReaderLineCompression() const {
  // Percent of the font size (v69: line spacing multiplies font size, not advanceY).
  return lineSpacing::multiplier(lineSpacingPercent);
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  if (sleepTimeoutMinutes >= SLEEP_TIMEOUT_NEVER_MINUTES) return 0UL;
  const uint8_t minutes =
      std::clamp(sleepTimeoutMinutes, MIN_SLEEP_TIMEOUT_MINUTES, static_cast<uint8_t>(SLEEP_TIMEOUT_NEVER_MINUTES - 1));
  return static_cast<unsigned long>(minutes) * 60UL * 1000UL;
}

int CrossPointSettings::getAutoRefreshThresholdPercent() const {
  switch (autoRefreshThreshold) {
    case PCT_1:
      return 1;
    case PCT_3:
      return 3;
    case PCT_10:
      return 10;
    case PCT_20:
      return 20;
    case PCT_5:
    default:
      return 5;
  }
}

int CrossPointSettings::getQuickRefreshPages() const {
  switch (quickRefreshPages) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_30:
      return 30;
    case REFRESH_60:
      return 60;
    case REFRESH_NEVER:
      return 1000000;  // effectively off
    case REFRESH_15:
    default:
      return 15;
  }
}

int CrossPointSettings::getSwipeThresholdPx() const {
  switch (swipeSensitivity) {
    case SWIPE_LOW:
      return 120;
    case SWIPE_HIGH:
      return 60;
    case SWIPE_VERY_HIGH:
      return 40;
    case SWIPE_NORMAL:
    default:
      return 90;
  }
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
    case REFRESH_60:
      return 60;
    case REFRESH_NEVER:
      // Effectively disables the periodic full refresh; the page counter
      // counts down from here and never reaches the threshold in practice.
      return std::numeric_limits<int>::max();
  }
}

void CrossPointSettings::clearSdFontFamily() {
  sdFontFamilyName[0] = '\0';
  fontPointSize =
      snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES), fontPointSize);
  saveToFile();
}

int CrossPointSettings::getReaderFontId() const {
  // Check SD card font first
  if (sdFontFamilyName[0] != '\0' && sdFontIdResolver) {
    int id = sdFontIdResolver(sdFontResolverCtx, sdFontFamilyName, fontPointSize);
    if (id != 0) return id;
    // Fall through to built-in if SD font not found
  }

  // A built-in family only exists at BUILTIN_READER_POINT_SIZES, so a size
  // carried over from an SD family may not be one of them. ensureLoaded()
  // normally persists the snap; snap again here (without allocating — this runs
  // in the page render loop) so rendering is correct even before it has run.
  const uint8_t pt =
      snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES), fontPointSize);
  switch (pt) {
    case 12:
      return NOTOSANS_12_FONT_ID;
    case 16:
      return NOTOSANS_16_FONT_ID;
    case 18:
      return NOTOSANS_18_FONT_ID;
    case 14:
    default:
      return NOTOSANS_14_FONT_ID;
  }
}
