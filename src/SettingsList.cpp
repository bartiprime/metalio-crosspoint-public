#include "SettingsList.h"

#include <array>
#include <utility>

#include "util/SystemKnobs.h"

void forEachBaseSetting(SettingVisitor visit, void* context) {
  // Load and save walk this list, so every condition here must be fixed before
  // settings load. Runtime probes belong in getSettingsList().

  // Enum settings are persisted as numeric values. Assign these labels by enum
  // value so a reordered menu or enum cannot silently swap their behavior.
  static constexpr auto SLEEP_SCREEN_VALUES = [] {
    std::array<StrId, CrossPointSettings::SLEEP_SCREEN_MODE_COUNT> values{};
    values[CrossPointSettings::DARK] = StrId::STR_DARK;
    values[CrossPointSettings::LIGHT] = StrId::STR_LIGHT;
    values[CrossPointSettings::CUSTOM] = StrId::STR_CUSTOM;
    values[CrossPointSettings::COVER] = StrId::STR_COVER;
    values[CrossPointSettings::COVER_CUSTOM] = StrId::STR_COVER_CUSTOM;
    values[CrossPointSettings::BLANK] = StrId::STR_NONE_OPT;
    values[CrossPointSettings::QUICK_RESUME] = StrId::STR_QUICK_RESUME;
    values[CrossPointSettings::TRANSPARENT_CUSTOM] = StrId::STR_TRANSPARENT;
    values[CrossPointSettings::COVER_TRANSPARENT] = StrId::STR_COVER_TRANSPARENT;
    return values;
  }();
  static constexpr auto STATUS_BAR_CLOCK_VALUES = [] {
    std::array<StrId, CrossPointSettings::STATUS_BAR_CLOCK_MODE_COUNT> values{};
    values[CrossPointSettings::STATUS_BAR_CLOCK_HIDE] = StrId::STR_HIDE;
    values[CrossPointSettings::STATUS_BAR_CLOCK_RIGHT] = StrId::STR_DIR_RIGHT;
    values[CrossPointSettings::STATUS_BAR_CLOCK_LEFT] = StrId::STR_DIR_LEFT;
    return values;
  }();

  const auto emit = [visit, context](auto&& info) { visit(context, info); };
  // --- Display ---
  emit(SettingInfo::StaticEnum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen, SLEEP_SCREEN_VALUES,
                               "sleepScreen", StrId::STR_CAT_DISPLAY));
  // POSCR1 (r101.9): what is on the glass when the reader is OFF. TEXT is index 0 and the
  // initialiser, so a settings file written by any earlier build (no "powerOffScreen" key)
  // loads as TEXT and the power-off frame is the one it always was. The description is the
  // caveat the operator asked for: an image on a powered-off device reads as "still on".
  static constexpr auto POWER_OFF_SCREEN_VALUES = [] {
    std::array<StrId, CrossPointSettings::POWER_OFF_SCREEN_MODE_COUNT> values{};
    values[CrossPointSettings::POWER_OFF_TEXT] = StrId::STR_POWER_OFF_OPT_TEXT;
    values[CrossPointSettings::POWER_OFF_CUSTOM] = StrId::STR_POWER_OFF_OPT_IMAGE;
    values[CrossPointSettings::POWER_OFF_DARK] = StrId::STR_DARK;
    values[CrossPointSettings::POWER_OFF_LIGHT] = StrId::STR_LIGHT;
    values[CrossPointSettings::POWER_OFF_BLANK] = StrId::STR_NONE_OPT;
    return values;
  }();
  emit(SettingInfo::StaticEnum(StrId::STR_POWER_OFF_SCREEN, &CrossPointSettings::powerOffScreen,
                               POWER_OFF_SCREEN_VALUES, "powerOffScreen", StrId::STR_CAT_DISPLAY)
           .withDescription(StrId::STR_POWER_OFF_SCREEN_DESC));
  emit(SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                         {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                         {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                         "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY));
  // r162 HYG2: the "Quick Resume after timeout" row is GONE. It was never read:
  // the sleep path asks `sleepScreen == QUICK_RESUME` (SleepActivity / main.cpp
  // isQuickResumeSleep) and nothing else, while this field was only ever WRITTEN
  // -- by this screen's own mirroring. The field stays for load compatibility
  // with older settings files and backups; it no longer changes any behaviour.
  emit(SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                         {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                         StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_AUTO_THRESHOLD, &CrossPointSettings::autoRefreshThreshold,
                         {StrId::STR_PCT_1, StrId::STR_PCT_3, StrId::STR_PCT_5, StrId::STR_PCT_10,
                          StrId::STR_PCT_20},
                         "autoRefreshThreshold", StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_MENU_REFRESH, &CrossPointSettings::menuTransitionRefresh,
                         {StrId::STR_REFRESH_FAST, StrId::STR_REFRESH_HALF, StrId::STR_REFRESH_GL16,
                          StrId::STR_REFRESH_FULL, StrId::STR_REFRESH_AUTO},
                         "menuTransitionRefresh", StrId::STR_CAT_DISPLAY));
  emit(SettingInfo::Enum(StrId::STR_ACTIVITY_REFRESH, &CrossPointSettings::activityTransitionRefresh,
                         {StrId::STR_REFRESH_FAST, StrId::STR_REFRESH_HALF, StrId::STR_REFRESH_GL16,
                          StrId::STR_REFRESH_FULL, StrId::STR_REFRESH_AUTO},
                         "activityTransitionRefresh", StrId::STR_CAT_DISPLAY));
  // r161 MENU AA: menus/UI text renders 1-bit when this is OFF (see
  // CrossPointSettings.h "MENU AA"), so a UI paint carries no intermediate level
  // and the panel takes the binary DU lane instead of the 8-level grey wipe.
  // Default 1 = every build before r161, unchanged.
  emit(SettingInfo::Toggle(StrId::STR_MENU_AA, &CrossPointSettings::menuAntiAliasing, "menuAntiAliasing",
                           StrId::STR_CAT_DISPLAY));
  // EQ1 (r101.9): the operator's global MODE_EQUALIZE_BLACK toggle. Default 0 keeps every drive
  // asking for exactly the mode it asked for before this row existed (see
  // src/display/EqualizeBlack.h, whose first line is the OFF early return). ON converts only the
  // HALF (diff-area GC16) paints -- popups/dialogs, the settings-list entry frame, the
  // sleep-entry popup -- to EQUALIZE_REFRESH: same 48-frame GC16 waveform, LUT-only difference.
  emit(SettingInfo::Toggle(StrId::STR_EQUALIZE_BLACK, &CrossPointSettings::equalizeBlack, "equalizeBlack",
                           StrId::STR_CAT_DISPLAY)
           .withDescription(StrId::STR_EQUALIZE_BLACK_DESC));
  emit(SettingInfo::Enum(StrId::STR_SWIPE_SENSITIVITY, &CrossPointSettings::swipeSensitivity,
                         {StrId::STR_SENS_LOW, StrId::STR_SENS_NORMAL, StrId::STR_SENS_HIGH, StrId::STR_SENS_VHIGH},
                         "swipeSensitivity", StrId::STR_CAT_CONTROLS));
  emit(SettingInfo::Enum(StrId::STR_UI_THEME, &CrossPointSettings::uiTheme, homeThemeValues(), "uiTheme",
                         StrId::STR_CAT_DISPLAY));
  if (!BoardConfig::hasTouch())
    emit(SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                             StrId::STR_CAT_DISPLAY));
#if FREEINK_CAP_FRONTLIGHT
  emit(SettingInfo::Toggle(StrId::STR_RESTORE_LIGHT_ON_WAKE, &CrossPointSettings::frontlightRestoreOnWake,
                           "frontlightRestoreOnWake", StrId::STR_CAT_DISPLAY));
#endif
#if FREEINK_DEVICE_PICCO
  emit(SettingInfo::Enum(StrId::STR_SLIDE_SWITCH, &CrossPointSettings::slideSwitchAction,
                         {StrId::STR_FRONTLIGHT, StrId::STR_NIGHT_MODE, StrId::STR_TOUCH_READER_CONTROLS},
                         "slideSwitchAction", StrId::STR_CAT_DISPLAY));
#endif
  // Night mode = inverted output polarity everywhere (ActivityManager
  // applies it to every activity), so it lives in the Display category.
  emit(SettingInfo::Toggle(StrId::STR_NIGHT_MODE, &CrossPointSettings::screenInverted, "screenInverted",
                           StrId::STR_CAT_DISPLAY));

  // --- Reader ---
  // Built-in font-family entry. Replaced per-call with a registry-aware
  // version when SD fonts are installed.
  emit(SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily, {StrId::STR_NOTO_SANS}, "fontFamily",
                         StrId::STR_CAT_READER)
           .withTextSettings());
  // Placeholder: the selectable sizes depend on the active font family, so
  // UI callers replace this entry with buildFontSizeSetting(). It only
  // fixes the setting's position in the Reader category.
  emit(SettingInfo::Enum(StrId::STR_FONT_SIZE, nullptr, {}, "fontSize", StrId::STR_CAT_READER).withTextSettings());
  // "lineSpacingPercent": fromJson() folds an older ladder save ("lineHeight",
  // or the even older "lineSpacing") to the percent that ladder step rendered at.
  emit(SettingInfo::Value(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacingPercent,
                          {CrossPointSettings::LINE_SPACING_MIN_PERCENT, CrossPointSettings::LINE_SPACING_MAX_PERCENT, 1},
                          "lineSpacingPercent", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Value(StrId::STR_WORD_SPACING, &CrossPointSettings::wordSpacing,
                          {CrossPointSettings::WORD_SPACING_MIN, CrossPointSettings::WORD_SPACING_MAX,
                           CrossPointSettings::WORD_SPACING_STEP},
                          "wordSpacing", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(StrId::STR_CHARACTER_SPACING, &CrossPointSettings::characterSpacing,
                         {StrId::STR_SPACING_MINUS_2, StrId::STR_SPACING_MINUS_1, StrId::STR_SPACING_ZERO,
                          StrId::STR_SPACING_PLUS_1, StrId::STR_SPACING_PLUS_2},
                         "characterSpacing", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin,
                          {CrossPointSettings::SCREEN_MARGIN_MIN, CrossPointSettings::SCREEN_MARGIN_MAX,
                           CrossPointSettings::SCREEN_MARGIN_STEP},
                          "screenMargin", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                         {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                          StrId::STR_BOOK_S_STYLE},
                         "paragraphAlignment", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_FOCUS_READING, &CrossPointSettings::focusReadingEnabled, "focusReadingEnabled",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(
      StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
      {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW},
      "orientation", StrId::STR_CAT_READER));
  emit(SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                           "extraParagraphSpacing", StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Value(StrId::STR_PARAGRAPH_INDENTATION, &CrossPointSettings::paragraphIndentSpaces, {0, 5, 1},
                          "paragraphIndentSpaces", StrId::STR_CAT_READER)
           .withTextSettings());
  // r162 HYG4 — WHAT "TEXT AA = OFF" ACTUALLY DOES ON THIS PANEL, written down so
  // nobody re-concludes "the toggle is inert" from reading the reader's
  // dual-plane branch:
  //   * ON  = glyph coverage is blitted into the 4 bpp canvas and reaches the glass
  //           as up to 4 levels per glyph pixel (the GL16/grey lane).
  //   * OFF = the glyph blitter thresholds coverage, so the FRAME carries only ink
  //           and paper. The setting is applied per paint, at the blitter, and it
  //           therefore binarises the WHOLE frame that paint produces: body text,
  //           headings, UI text drawn into the same frame, AND in-book images and
  //           sleep/cover renders that share the canvas. It is not a text-only,
  //           glyph-only filter.
  //   * The `needsTextGrayscale` / displayGray() branch that used to be the only
  //     reader of this field is the DUAL-PLANE overlay path, which this driver
  //     reports unsupported on purpose (that path is the black flip). It is
  //     expected to be dead on this board; its deadness says nothing about whether
  //     the toggle works.
  emit(SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                           StrId::STR_CAT_READER)
           .withTextSettings());
  emit(SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                         {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                         "imageRendering", StrId::STR_CAT_READER));
  // r99.9 (operator request A3): the two refresh cadences are READING decisions,
  // not panel settings -- they moved out of Display into Reader, directly under
  // Images, and were renamed with the move ("Full Refresh" / "Quick Refresh").
  // The rename is in the shared string table, so the reader's own More panel
  // and this screen print the same words.
  // CAD1: 5/10/15/30/60 -- the ladder the operator asked for on BOTH rows. The
  // 1-page slot and Never stay: they are values a device may already have saved,
  // and this enum is persisted by index, so dropping either would silently
  // re-point a saved slot at a different page count. REFRESH_60 is slot 6 for
  // the same reason; see REFRESH_FREQUENCY in CrossPointSettings.h.
  // CAD2: the SLOTS are unchanged; the ORDER the picker shows them in is
  // 1/5/10/15/30/60/Never (CadenceOrder.h), so Never sits at the bottom without
  // moving a single stored value. Both rows go through one builder.
  emit(buildCadenceSetting(StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency, "refreshFrequency"));
  emit(buildCadenceSetting(StrId::STR_QUICK_REFRESH_PAGES, &CrossPointSettings::quickRefreshPages,
                           "quickRefreshPages"));
  emit(SettingInfo::Enum(StrId::STR_READER_MENU_STYLE, &CrossPointSettings::readerMenuStyle,
                         {StrId::STR_MENU_STYLE_LIST, StrId::STR_MENU_STYLE_TOOLBAR}, "readerMenuStyle",
                         StrId::STR_CAT_READER));
  // r97 KEY1: the physical boot/AI key (Metalio 4 Plus GPIO61). Read path: the same
  // three-gesture layer the home key uses, but two rows of its own, in Reading,
  // persisted by KEY STRING. Emitted only on a board that HAS the key, so the
  // web API and the load/save walk stay honest about what the hardware can do.
  if (BoardConfig::isMetalioEInk4Plus()) {
    emit(SettingInfo::StaticEnum(StrId::STR_BOOT_KEY_TAP, &CrossPointSettings::bootKeyTapAction,
                                 boot_key::ACTION_LABELS, boot_key::TAP_KEY, StrId::STR_CAT_READER)
             .withDescription(StrId::STR_BOOT_KEY_CANNOT_WAKE));
    emit(SettingInfo::StaticEnum(StrId::STR_BOOT_KEY_LONG_PRESS, &CrossPointSettings::bootKeyLongPressAction,
                                 boot_key::ACTION_LABELS, boot_key::LONG_PRESS_KEY, StrId::STR_CAT_READER)
             .withDescription(StrId::STR_BOOT_KEY_CANNOT_WAKE));
  }
  // --- Controls ---
#if FREEINK_CAP_HAPTIC
  emit(SettingInfo::Enum(StrId::STR_VIBRATION, &CrossPointSettings::vibration,
                         {StrId::STR_VIBRATION_TOUCH, StrId::STR_VIBRATION_TOUCH_PAGE, StrId::STR_STATE_OFF},
                         "vibration", StrId::STR_CAT_CONTROLS));
  emit(SettingInfo::Enum(StrId::STR_HAPTIC_INTENSITY, &CrossPointSettings::hapticIntensity,
                         {StrId::STR_HAPTIC_LOW, StrId::STR_HAPTIC_MEDIUM, StrId::STR_HAPTIC_HIGH}, "hapticIntensity",
                         StrId::STR_CAT_CONTROLS));
  // HAPT3 (r101.9): the soft-key gate, default 0. In Controls and inside the
  // FREEINK_CAP_HAPTIC block on purpose -- that is where the motor's own two rows live, and
  // the block keeps the row off boards that have no motor rather than present-but-inert.
  // OFF is the only gate, so turning it back off returns the soft keys to exactly the
  // pre-HAPT3 behaviour. Described rather than bare because the row names a pulse the
  // operator will otherwise never see in a settings file.
  emit(SettingInfo::Toggle(StrId::STR_SOFT_KEY_HAPTIC, &CrossPointSettings::softKeyHaptic, "softKeyHaptic",
                           StrId::STR_CAT_CONTROLS)
           .withDescription(StrId::STR_SOFT_KEY_HAPTIC_DESC));
#endif
  // AUDIO2: the two sound switches, right after the motor's rows because they are
  // the same kind of feedback decision (Controls, not Display: neither one changes
  // what the panel draws). Both default OFF and both were shell-only before this
  // row existed, so the operator could not reach either feature.
  //
  //   * Audio Output is TTS1's master gate. OFF means the audio path is never
  //     brought up -- no P1.4 rail, no UART1 to the BH1098G, no I2S. Turning it
  //     ON only arms the path: nothing plays, and the rail still stays low until
  //     something actually asks for a sound (TTS1's contract, unchanged here).
  //   * UI Sounds is AUDIO1's own switch and needs the master as well. The row is
  //     HIDDEN, not disabled, while the master is off (see getSettingsList): the
  //     list has no disabled-row state and hiding is what this screen already
  //     does for probe-gated rows, and because every toggle rebuilds the list the
  //     row appears the moment the master goes ON.
  //
  // Metalio 4 Plus only: src/audio drives that board's vendor audio chain on fixed
  // GPIO38-44 and TCA P1.4/P0.4. On a board without it both rows would be
  // present-but-inert, which is the defect class this audit removes.
  if (BoardConfig::isMetalioEInk4Plus()) {
    emit(SettingInfo::Toggle(StrId::STR_AUDIO_OUTPUT, &CrossPointSettings::audioOutEnabled, "audioOutEnabled",
                             StrId::STR_CAT_CONTROLS)
             .withDescription(StrId::STR_AUDIO_OUTPUT_DESC));
    emit(SettingInfo::Toggle(StrId::STR_UI_SOUNDS, &CrossPointSettings::uiSoundsEnabled, "uiSoundsEnabled",
                             StrId::STR_CAT_CONTROLS)
             .withDescription(StrId::STR_UI_SOUNDS_DESC));
  }
  // B-P1-3 (r101.13b): the input-during-refresh kill-switch, default ON (new behaviour). OFF is the
  // pre-r101.13b polling exactly, so a bad hand-off is a switch instead of a flash cycle. Lives in
  // Controls because it is an input-timing decision, and outside the haptic block because every
  // board polls input at its drive edges.
  emit(SettingInfo::Toggle(StrId::STR_INPUT_DURING_REFRESH, &CrossPointSettings::inputDuringRefresh,
                           "inputDuringRefresh", StrId::STR_CAT_CONTROLS));
  emit(SettingInfo::Enum(
      StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
      {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV, StrId::STR_DISABLED, StrId::STR_NEXT_NEXT, StrId::STR_PREV_PREV},
      "sideButtonLayout", StrId::STR_CAT_CONTROLS));
  // The reader menu style stays available on button boards (the toolbar
  // chrome is button-navigable); only the touch controls are hidden.
  if (BoardConfig::hasTouch()) {
    emit(SettingInfo::Toggle(StrId::STR_TOUCH_READER_CONTROLS, &CrossPointSettings::touchReaderControls,
                             "touchReaderControls", StrId::STR_CAT_CONTROLS));
    // FL1: side-edge frontlight drag. Default OFF, so the shipped reading surface
    // has no new dead zone unless the operator asks for it; ON is a reader gesture
    // that rides the switch above (off = off). Described because the bands it
    // avoids (top/bottom edge, page-turn zones) are not visible anywhere else.
#if FREEINK_CAP_FRONTLIGHT
    emit(SettingInfo::Toggle(StrId::STR_EDGE_DRAG_FRONTLIGHT, &CrossPointSettings::edgeDragFrontlight,
                             "edgeDragFrontlight", StrId::STR_CAT_CONTROLS)
             .withDescription(StrId::STR_EDGE_DRAG_FRONTLIGHT_DESC));
#endif
    emit(SettingInfo::Enum(StrId::STR_NEXT_PAGE_GESTURE, &CrossPointSettings::pageTurnGesture,
                           {StrId::STR_TAP_AND_SWIPE, StrId::STR_TAP_ONLY, StrId::STR_SWIPE_ONLY,
                            StrId::STR_INVERTED_TAP, StrId::STR_DISABLED},
                           "pageTurnGesture", StrId::STR_CAT_CONTROLS));
    emit(SettingInfo::Enum(StrId::STR_PREV_PAGE_GESTURE, &CrossPointSettings::previousPageGesture,
                           {StrId::STR_TAP_AND_SWIPE, StrId::STR_TAP_ONLY, StrId::STR_SWIPE_ONLY,
                            StrId::STR_INVERTED_TAP, StrId::STR_DISABLED},
                           "previousPageGesture", StrId::STR_CAT_CONTROLS));
  }
  // The reader-menu gesture choice only makes sense where the menu stays
  // reachable without the tap and the bottom edge is free (the capacitive
  // Home key); everywhere else the bottom-edge up-swipe is Home and the
  // center tap is the primary path, so the setting stays at its Tap default.
  // Persisted under the legacy "tapForReaderMenu" key: old saves map
  // 0 = Off, 1 = Tap.
  if (BoardConfig::hasHomeKey())
    emit(SettingInfo::Enum(StrId::STR_SHOW_READER_MENU, &CrossPointSettings::showReaderMenu,
                           {StrId::STR_STATE_OFF, StrId::STR_STATE_TAP, StrId::STR_STATE_SWIPE_UP}, "tapForReaderMenu",
                           StrId::STR_CAT_CONTROLS));
  if (!BoardConfig::hasTouch())
    emit(SettingInfo::Toggle(StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION, &CrossPointSettings::frontButtonFollowOrientation,
                             "frontButtonFollowOrientation", StrId::STR_CAT_CONTROLS));
  emit(SettingInfo::Enum(StrId::STR_LONG_PRESS_BEHAVIOR, &CrossPointSettings::longPressButtonBehavior,
                         {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                          StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION},
                         "longPressButtonBehavior", StrId::STR_CAT_CONTROLS));
  emit(buildLongPressMenuSetting());
  // Double-click power frontlight shortcut only exists on the X4 Pro.
  if (BoardConfig::isX4Pro())
    emit(SettingInfo::Toggle(StrId::STR_DBL_CLICK_PWR_LIGHT, &CrossPointSettings::doubleClickPwrLight,
                             "doubleClickPwrLight", StrId::STR_CAT_CONTROLS));
  emit(buildShortPowerButtonSetting());
  // Always persisted; getSettingsList() hides it when the IMU probe fails.
  emit(SettingInfo::Enum(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn,
                         {StrId::STR_STATE_OFF, StrId::STR_NORMAL, StrId::STR_INVERTED}, "tiltPageTurn",
                         StrId::STR_CAT_CONTROLS));
  // r162 HYG2: the "Power button returns from footnotes" row is GONE. It never
  // gated anything: the reader's footnote return is unconditional (footnoteDepth
  // > 0 -> restoreSavedPosition(), EpubReaderActivity::loop) and the toggle had
  // no second reader anywhere in the tree. A switch that does nothing is the
  // defect class this audit exists to remove, so the row is removed rather than
  // left as a promise the firmware does not keep. The field stays only so an
  // older settings.json / backup that carries the key still loads without a
  // resave; this build no longer persists it. Wiring it is a reader change and
  // is tracked separately.
  //
  // r163: the same rule, followed through. "Back short -> file browser" is gone
  // too, and this time the field and the backup rule went with it: the r163 tap
  // rewrite made Back's short press mean one level back, everywhere, with no
  // reader of a per-board override. A restore of an older backup that still
  // carries the key drops it and counts it in SanitizeResult::droppedKeys
  // (BackupProfile: an unknown numeric key is dropped, not an error).

  // Persisted here; set from the Connectivity -> Plugins popup.
  emit(SettingInfo::Toggle(StrId::STR_PLUGINS, &CrossPointSettings::pluginsEnabled, "pluginsEnabled").withWebHidden());
  emit(SettingInfo::Toggle(StrId::STR_PLUGIN_HUB, &CrossPointSettings::pluginHubPromptHidden, "pluginHubPromptHidden")
           .withWebHidden());

  // --- System ---
  emit(SettingInfo::Value(
      StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeoutMinutes,
      {CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1},
      "sleepTimeoutMinutes", StrId::STR_CAT_SYSTEM));
  emit(SettingInfo::Value(
      StrId::STR_STANDBY_AUTO_OFF, &CrossPointSettings::standbyAutoOffMinutes,
      {CrossPointSettings::MIN_STANDBY_AUTO_OFF_MINUTES, CrossPointSettings::MAX_STANDBY_AUTO_OFF_MINUTES, 5},
      "standbyAutoOffMinutes", StrId::STR_CAT_SYSTEM));
  // r163 3.5: charging-aware idle policy. OFF by default -- the configured sleep
  // timeout is the user's own policy and a cable appearing must not silently change
  // it. ON holds an E6 HARD need while the pack is charging, so the ORDINARY idle
  // timer does not park the device; a manual sleep still sleeps. Metalio only: that
  // is the board whose standby is the light-sleep park this concerns.
  if (BoardConfig::isMetalioEInk4Plus()) {
    emit(SettingInfo::Toggle(StrId::STR_CHARGE_KEEP_AWAKE, &CrossPointSettings::chargeIdlePolicy, "chargeIdlePolicy",
                             StrId::STR_CAT_SYSTEM));
  }
  // Step 2.5 item 8: charge current for the CX25601N. Only on a board that HAS that
  // charger at 0x6B, so the web API and the load/save walk stay honest about what the
  // hardware can do. The three values are the vendor's own UI set
  // (settings_charging_tab.cc:7); 1.5 A is the top of the range we surface and the
  // limit itself comes from chargercurrent::applyMa(), not from this list.
  if (BoardConfig::isMetalioEInk4Plus()) {
    emit(SettingInfo::Enum(StrId::STR_CHARGE_CURRENT, &CrossPointSettings::chargeCurrent,
                           {StrId::STR_CHARGE_CURRENT_500, StrId::STR_CHARGE_CURRENT_1A,
                            StrId::STR_CHARGE_CURRENT_1_5A},
                           "chargeCurrent", StrId::STR_CAT_SYSTEM));
  }
  emit(SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles, "showHiddenFiles",
                           StrId::STR_CAT_SYSTEM));
  // DLG1 (r101.9): gate for the USB drive-mode EXIT frame. Default 0 keeps the exit path
  // paint-free, which is what every shipped build has done; the row is the only gate, so
  // turning it off again returns the path to exactly that behaviour. Described rather than
  // bare because the row is about a frame the operator will otherwise never see in a
  // settings file.
  emit(SettingInfo::Toggle(StrId::STR_USB_EXIT_DIALOG, &CrossPointSettings::usbDriveExitDialog,
                           "usbDriveExitDialog", StrId::STR_CAT_SYSTEM)
           .withDescription(StrId::STR_USB_EXIT_DIALOG_DESC));
  emit(SettingInfo::Toggle(StrId::STR_LIBRARY_USE_METADATA, &CrossPointSettings::libraryUseMetadata,
                           "libraryUseMetadata", StrId::STR_CAT_SYSTEM));
  // LIB10 kill-switch: OFF is the shipped behaviour (the series drill shrinks each
  // book's file name so the whole name fits the row it already has). ON forces the
  // fixed single-line face the drill used before this item. A plain label and a
  // plain description -- no format verbs, nothing to interpolate (FMT1).
  emit(SettingInfo::Toggle(StrId::STR_LIBRARY_FIXED_ROW_FACE, &CrossPointSettings::libraryFixedRowFace,
                           "libraryFixedRowFace", StrId::STR_CAT_SYSTEM)
           .withDescription(StrId::STR_LIBRARY_FIXED_ROW_FACE_DESC));
  emit(SettingInfo::Toggle(StrId::STR_REMOVE_READ_FROM_RECENTS, &CrossPointSettings::removeReadBooksFromRecents,
                           "removeReadBooksFromRecents", StrId::STR_CAT_SYSTEM));
  emit(SettingInfo::Toggle(StrId::STR_MOVE_FINISHED_TO_READ, &CrossPointSettings::moveFinishedToReadFolder,
                           "moveFinishedToReadFolder", StrId::STR_CAT_SYSTEM));

  // Step 2.5 item 9 -- idle CPU frequency, 4 steps. The vendor's own setting
  // (settings_power_tab.cc:26-31) offers 160/240/320; 80 MHz is the operator's
  // added step and is the shipped idle frequency, so it is index 0 and the
  // default. Values come from systemKnobs so the row, the boot apply and the
  // apply-on-change cannot disagree.
  static constexpr StrId IDLE_FREQ_LABELS[systemKnobs::IDLE_FREQ_COUNT] = {
      StrId::STR_IDLE_FREQ_80, StrId::STR_IDLE_FREQ_160, StrId::STR_IDLE_FREQ_240, StrId::STR_IDLE_FREQ_320};
  emit(SettingInfo::StaticEnum(StrId::STR_IDLE_CPU_FREQ, &CrossPointSettings::idleCpuFreq, IDLE_FREQ_LABELS,
                               "idleCpuFreq", StrId::STR_CAT_SYSTEM));

  // Step 2.5 item 11 -- panel black depth, the vendor's three VCOM levels taken
  // exactly (epd_refresh.cc:58 kBlackDepthMv). Only this board has the TPS65185,
  // so the row is absent everywhere else rather than present-but-inert; the key
  // is still persisted on every board so a settings file stays portable.
  if (systemKnobs::PANEL_VCOM_AVAILABLE) {
    static constexpr StrId VCOM_LABELS[systemKnobs::VCOM_COUNT] = {StrId::STR_DEFAULT_VALUE,
                                                                  StrId::STR_PANEL_VCOM_MEDIUM,
                                                                  StrId::STR_PANEL_VCOM_STRONG};
    // r163 D9: the row now carries the panel-lifetime warning under its title (the picker's
    // description line), because a deeper black is bought with bias stress on the glass.
    emit(SettingInfo::StaticEnum(StrId::STR_PANEL_VCOM, &CrossPointSettings::panelVcomDepth, VCOM_LABELS,
                                 "panelVcomDepth", StrId::STR_CAT_SYSTEM)
             .withDescription(StrId::STR_PANEL_VCOM_WARN));
  }

  // OPDS download folder: persisted + web-exposed, but category-less so it
  // is hidden from the on-device Settings screen (edited via OPDS UI).
  emit(SettingInfo::String(StrId::STR_OPDS_DOWNLOAD_FOLDER, &SETTINGS.opdsDownloadFolder[0],
                           sizeof(SETTINGS.opdsDownloadFolder), "opdsDownloadFolder"));
  // OPDS download filename format: persisted + web-exposed, category-less so it
  // is hidden from the on-device Settings screen (cycled from the OPDS UI).
  emit(SettingInfo::Enum(StrId::STR_OPDS_FILENAME_FORMAT, &CrossPointSettings::opdsFilenameFormat,
                         {StrId::STR_FMT_AUTHOR_TITLE, StrId::STR_FMT_TITLE_AUTHOR, StrId::STR_FMT_TITLE},
                         "opdsFilenameFormat"));

  // Frontlight quick-panel state: persisted and web-exposed, but hidden
  // from the on-device Settings screen because the swipe panel owns it.
  emit(SettingInfo::Value(StrId::STR_BRIGHTNESS, &CrossPointSettings::frontlightBrightness, {0, 100, 5},
                          "frontlightBrightness"));
#if FREEINK_CAP_WARMLIGHT
  emit(SettingInfo::Value(StrId::STR_WARMTH, &CrossPointSettings::frontlightWarmth, {0, 100, 5}, "frontlightWarmth"));
#endif
  emit(SettingInfo::Toggle(StrId::STR_FRONTLIGHT, &CrossPointSettings::frontlightOn, "frontlightOn"));

  // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
  emit(SettingInfo::DynamicString(
      StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
      [](const std::string& v) {
        KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
        KOREADER_STORE.saveToFile();
      },
      "koUsername", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicString(
           StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
           [](const std::string& v) {
             KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
             KOREADER_STORE.saveToFile();
           },
           "koPassword", StrId::STR_KOREADER_SYNC)
           .withObfuscated());
  emit(SettingInfo::DynamicString(
      StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
      [](const std::string& v) {
        KOREADER_STORE.setServerUrl(v);
        KOREADER_STORE.saveToFile();
      },
      "koServerUrl", StrId::STR_KOREADER_SYNC));
  // NAME1: the kosync device name. Also editable on the device (Settings >
  // CrossPoint Sync > Device name, same key); this row is what the web settings
  // API exposes, and an empty value means the compile-time default.
  emit(SettingInfo::DynamicString(
      StrId::STR_DEVICE_NAME, [] { return KOREADER_STORE.getDeviceName(); },
      [](const std::string& v) {
        KOREADER_STORE.setDeviceName(v);
        KOREADER_STORE.saveToFile();
      },
      "koDeviceName", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicString(
      StrId::STR_KOREADER_CUSTOM_HEADER_1_NAME, [] { return KOREADER_STORE.getCustomHeaders()[0].name; },
      [](const std::string& v) {
        KOREADER_STORE.setCustomHeader(0, v, KOREADER_STORE.getCustomHeaders()[0].value);
        KOREADER_STORE.saveToFile();
      },
      "koHeader1Name", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicString(
           StrId::STR_KOREADER_CUSTOM_HEADER_1_VALUE, [] { return KOREADER_STORE.getCustomHeaders()[0].value; },
           [](const std::string& v) {
             KOREADER_STORE.setCustomHeader(0, KOREADER_STORE.getCustomHeaders()[0].name, v);
             KOREADER_STORE.saveToFile();
           },
           "koHeader1Value", StrId::STR_KOREADER_SYNC)
           .withObfuscated());
  emit(SettingInfo::DynamicString(
      StrId::STR_KOREADER_CUSTOM_HEADER_2_NAME, [] { return KOREADER_STORE.getCustomHeaders()[1].name; },
      [](const std::string& v) {
        KOREADER_STORE.setCustomHeader(1, v, KOREADER_STORE.getCustomHeaders()[1].value);
        KOREADER_STORE.saveToFile();
      },
      "koHeader2Name", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicString(
           StrId::STR_KOREADER_CUSTOM_HEADER_2_VALUE, [] { return KOREADER_STORE.getCustomHeaders()[1].value; },
           [](const std::string& v) {
             KOREADER_STORE.setCustomHeader(1, KOREADER_STORE.getCustomHeaders()[1].name, v);
             KOREADER_STORE.saveToFile();
           },
           "koHeader2Value", StrId::STR_KOREADER_SYNC)
           .withObfuscated());
  emit(SettingInfo::DynamicEnum(
      StrId::STR_SERVER_TYPE, {StrId::STR_CROSSPOINT, StrId::STR_KOSYNC_SERVER, StrId::STR_OTHER},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getServerType()); },
      [](uint8_t v) {
        KOREADER_STORE.setServerType(static_cast<KOReaderServerType>(v));
        KOREADER_STORE.saveToFile();
      },
      "koServerType", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicEnum(
      StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
      [](uint8_t v) {
        KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
        KOREADER_STORE.saveToFile();
      },
      "koMatchMethod", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicEnum(
      StrId::STR_SEND_METADATA, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getSendMetadata()); },
      [](uint8_t v) {
        KOREADER_STORE.setSendMetadata(v != 0);
        KOREADER_STORE.saveToFile();
      },
      "koSendMetadata", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicEnum(
      StrId::STR_SYNC_BEHAVIOR, {StrId::STR_ASK_EVERY_TIME, StrId::STR_SMART_SYNC},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getSyncBehavior()); },
      [](uint8_t v) {
        KOREADER_STORE.setSyncBehavior(static_cast<KOReaderSyncBehavior>(v));
        KOREADER_STORE.saveToFile();
      },
      "koSyncBehavior", StrId::STR_KOREADER_SYNC));
  emit(SettingInfo::DynamicEnum(
      StrId::STR_SYNC_CLIPPINGS, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getSyncClippings()); },
      [](uint8_t v) {
        if (KOREADER_STORE.getSyncClippings() == (v != 0)) return;
        KOREADER_STORE.setSyncClippings(v != 0);
        KOREADER_STORE.saveToFile();
      },
      "koSyncClippings", StrId::STR_KOREADER_SYNC));
  // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
  emit(SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount,
                           "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookProgressPercentage,
                           "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                         {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarProgressBar",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                         {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                         "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                         {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTitle",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                           StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_XTC_STATUS_BAR, &CrossPointSettings::xtcStatusBarMode,
                         {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP}, "xtcStatusBarMode",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  // Clock entries (persistence + web settings; the device UI is
  // ClockSettingsActivity under System settings).
  emit(SettingInfo::StaticEnum(StrId::STR_CLOCK, &CrossPointSettings::statusBarClock, STATUS_BAR_CLOCK_VALUES,
                               "statusBarClock", StrId::STR_CUSTOMISE_STATUS_BAR));
  // LEGACY: retired quarter-hour UTC offset (biased by 48). Persisted so
  // timezones::activeIndex() can migrate it into clockTimezone.
  emit(SettingInfo::Value(StrId::STR_CLOCK, &CrossPointSettings::clockUtcOffsetQ, {0, 104, 1}, "clockUtcOffsetQ",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat,
                         {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H}, "clockFormat",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  // Index into the append-only table in src/util/Timezones.cpp; 255 = unset.
  emit(SettingInfo::Value(StrId::STR_TIMEZONE, &CrossPointSettings::clockTimezone, {0, 255, 1}, "clockTimezone",
                          StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Enum(StrId::STR_CLOCK_DST, &CrossPointSettings::clockDst,
                         {StrId::STR_CLOCK_DST_AUTO, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, "clockDst",
                         StrId::STR_CUSTOMISE_STATUS_BAR));
  emit(SettingInfo::Toggle(StrId::STR_CLOCK_IN_HEADER, &CrossPointSettings::clockShowInHeader, "clockShowHeader",
                           StrId::STR_CUSTOMISE_STATUS_BAR));
  // Persistence flag for NTP debounce. Resetting from the web UI forces a re-sync
  // on next WiFi connect, which is useful when crossing time zones.
  emit(SettingInfo::Toggle(StrId::STR_CLOCK_SYNCED, &CrossPointSettings::clockHasBeenSynced, "clockHasBeenSynced",
                           StrId::STR_CUSTOMISE_STATUS_BAR));
  if (BoardConfig::hasHomeKey()) {
    for (unsigned i = 0; i < 3; ++i) {
      emit(SettingInfo::StaticEnum(home_button::GESTURE_LABELS[i], home_button::FIELDS[i], home_button::ACTION_LABELS,
                                   home_button::KEYS[i], StrId::STR_CAT_CONTROLS));
    }
  }
}

std::vector<SettingInfo> getBaseSettingsList(const size_t extraCapacity) {
  // Count first so the one contiguous block is sized exactly.
  size_t count = 0;
  forEachBaseSetting([](void* context, SettingInfo&) { ++*static_cast<size_t*>(context); }, &count);
  std::vector<SettingInfo> v;
  v.reserve(count + extraCapacity);
  forEachBaseSetting(
      [](void* context, SettingInfo& info) {
        static_cast<std::vector<SettingInfo>*>(context)->push_back(std::move(info));
      },
      &v);
  return v;
}
