#pragma once

#include <ArduinoJson.h>
#include <BlePageTurner.h>
#include <BoardConfig.h>
#include <Epub/ReaderRenderSpec.h>
#include <PersistableStore.h>

#include <cstdint>

#include "ReaderLineSpacingMath.h"
#include "util/HomeButtonInput.h"

class CrossPointSettings : public PersistableStore<CrossPointSettings> {
 private:
  // Private constructor for singleton
  CrossPointSettings() = default;

  friend class PersistableStore<CrossPointSettings>;

 public:
  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    COVER_CUSTOM = 4,
    BLANK = 5,
    QUICK_RESUME = 6,
    TRANSPARENT_CUSTOM = 7,
    COVER_TRANSPARENT = 8,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR {
    BOOK_PROGRESS = 0,
    CHAPTER_PROGRESS = 1,
    HIDE_PROGRESS = 2,
    STATUS_BAR_PROGRESS_BAR_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR_THICKNESS {
    PROGRESS_BAR_THIN = 0,
    PROGRESS_BAR_NORMAL = 1,
    PROGRESS_BAR_THICK = 2,
    STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
  };
  enum STATUS_BAR_TITLE { BOOK_TITLE = 0, CHAPTER_TITLE = 1, HIDE_TITLE = 2, STATUS_BAR_TITLE_COUNT };
  enum XTC_STATUS_BAR_MODE {
    XTC_STATUS_BAR_HIDE = 0,
    XTC_STATUS_BAR_BOTTOM = 1,
    XTC_STATUS_BAR_TOP = 2,
    XTC_STATUS_BAR_MODE_COUNT
  };

  enum STATUS_BAR_CLOCK_MODE {
    STATUS_BAR_CLOCK_HIDE = 0,
    STATUS_BAR_CLOCK_RIGHT = 1,
    STATUS_BAR_CLOCK_LEFT = 2,
    STATUS_BAR_CLOCK_MODE_COUNT
  };

  // Auto follows the timezone's baked DST rule; On/Off override it — the
  // escape hatch for a zone whose law changed before the firmware caught up.
  enum CLOCK_DST_MODE { CLOCK_DST_AUTO = 0, CLOCK_DST_ON = 1, CLOCK_DST_OFF = 2, CLOCK_DST_MODE_COUNT };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Side button layout options
  // Default: Up = Previous, Down = Next. NEXT_NEXT / PREV_PREV assign both
  // buttons to the same direction for one-handed reading.
  enum SIDE_BUTTON_LAYOUT {
    PREV_NEXT = 0,
    NEXT_PREV = 1,
    SIDE_BUTTONS_DISABLED = 2,
    NEXT_NEXT = 3,
    PREV_PREV = 4,
    SIDE_BUTTON_LAYOUT_COUNT
  };

  // Font family options (built-in fonts only; SD card fonts use sdFontFamilyName)
  enum FONT_FAMILY { NOTOSANS = 0, FONT_FAMILY_COUNT };
  static constexpr uint8_t LEGACY_NOTOSERIF = 0;
  // Persisted IDs stay stable even though the built-in menu now starts at zero.
  static constexpr uint8_t STORED_NOTOSANS = 1;
  static constexpr uint8_t LEGACY_OPENDYSLEXIC = 2;
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // Reader font size is a point size, not an enum slot — see fontPointSize.
  // Legacy 1.4-and-earlier files stored a 0..3 SMALL/MEDIUM/LARGE/EXTRA_LARGE
  // slot; fromJson() folds that range up (see LEGACY_FONT_SIZE_MAX).
  static constexpr uint8_t LEGACY_FONT_SIZE_MAX = 3;
  static constexpr uint8_t DEFAULT_FONT_POINT_SIZE = 14;
  // Line height as a percent of the font size, persisted in 1% steps. The
  // older seven-step ladder (100/110/120/130/145/160/175) is migration-only;
  // fromJson() folds a stored ladder slot to the percent it rendered at.
  // The numeric contract lives in ReaderLineSpacingMath.h (host-testable).
  static constexpr uint8_t LINE_SPACING_MIN_PERCENT = lineSpacing::kMinPercent;
  static constexpr uint8_t LINE_SPACING_MAX_PERCENT = lineSpacing::kMaxPercent;
  static constexpr uint8_t LINE_SPACING_DEFAULT_PERCENT = lineSpacing::kDefaultPercent;
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_TIMEOUT_COUNT
  };

  // How a screen that replaces the previous one is drawn. DU (Fast) cannot scrub accumulated grey,
  // so a wholesale repaint on Fast leaves the old screen behind; Full always cleans but costs a
  // from-white blackout. Auto decides per frame from how much of the buffer actually changed.
  // Auto mode switches from DU to GC16 when this much of the frame changed. 5% is a starting point;
  // it is a setting because the right value depends on the panel and on how much residue the user
  // tolerates.
  enum AUTO_THRESHOLD {
    PCT_1 = 0,
    PCT_3 = 1,
    PCT_5 = 2,
    PCT_10 = 3,
    PCT_20 = 4,
    AUTO_THRESHOLD_COUNT
  };

  enum SCREEN_REFRESH {
    REFRESH_FAST = 0,  // DU, 20 phases -- cheapest, never cleans (the historical default)
    REFRESH_HALF = 1,  // GC16 over the diff area -- cleans, no blackout
    REFRESH_GL16 = 2,  // GL16, 48 phases, also drives 15->15 so white areas get pushed clean
    // GC16 from an assumed-white baseline -- flashes (~1.3-1.8 s), always cleans.
    // TODO(review): Full is offered as a transition mode for completeness only. It turns a screen
    // change into a visible blackout, so it most likely belongs on the explicit refresh action and
    // nowhwre else. Revisit once the four debug rows have been compared on the glass.
    REFRESH_FULL = 3,
    REFRESH_AUTO = 4,  // pick Fast/Half per frame from the changed fraction
    SCREEN_REFRESH_COUNT
  };

  // Swipe vs tap discrimination, in native px of travel. 60 px is ~5 mm at 297 PPI, which a normal
  // finger roll exceeds during a press -- that is how a tap on the bottom tab row turns into a tab
  // walk. The service row exists so the value can be found on the hardware instead of guessed.
  enum SWIPE_SENSITIVITY {
    SWIPE_LOW = 0,        // 120 px
    SWIPE_NORMAL = 1,     // 90 px  (default)
    SWIPE_HIGH = 2,       // 60 px  (the old hard-coded value)
    SWIPE_VERY_HIGH = 3,  // 40 px
    SWIPE_SENSITIVITY_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes).
  //
  // PERSISTED BY INDEX (0..5 below are the values r101..r164 wrote), so this
  // enum is APPEND-ONLY: a saved slot means its own page count or it means
  // nothing. CAD1 added the 60-page rung the ladder lacked -- it goes after
  // REFRESH_NEVER as slot 6, because re-slotting 5 would turn every device that
  // saved "Never" into "60 pages" on first boot.
  //
  // CAD2 shows the ladder in the order the operator wants -- 1/5/10/15/30/60/Never,
  // Never last -- WITHOUT moving a value: the pickers index a display-order table
  // (CadenceOrder.h) that maps position <-> slot, so 5 still means Never on the
  // card and always did. Re-slotting instead would have needed a settings
  // migration, and the backup restore path (lib/Backup/BackupProfile.cpp) drops any
  // key that is not a declared setting, so it cannot carry a migration marker.
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_NEVER = 5,
    REFRESH_60 = 6,  // CAD1: the top rung of the 5/10/15/30/60 ladder
    REFRESH_FREQUENCY_COUNT
  };

  // Short power button press actions. Touch boards get PWR_CONFIRM in place of
  // CREATE_CLIPPING; touch boards without a home key also get the reader
  // shortcuts (see SettingsList.h). Persisted by index: append only.
  enum SHORT_PWRBTN {
    IGNORE = 0,
    SLEEP = 1,
    PAGE_TURN = 2,
    FORCE_REFRESH = 3,
    FOOTNOTES = 4,
    PWR_CONFIRM = 5,
    CREATE_CLIPPING = 6,
    PWR_BOOKMARK = 7,
    PWR_SYNC = 8,
    PWR_READER_MENU = 9,
    SHORT_PWRBTN_COUNT
  };

  // Long-press Confirm action while reading an EPUB. Persisted values are stable;
  // SettingsList maps them to the device-specific option list shown to the user.
  enum LONG_PRESS_MENU_FUNCTION {
    LP_MENU_KOSYNC = 0,
    LP_MENU_DISABLED = 1,
    LP_MENU_BOOKMARK = 2,
    LP_MENU_DICTIONARY = 3,
    LP_MENU_READER_MENU = 4,
    LP_MENU_CREATE_CLIPPING = 5,
    LP_MENU_FOOTNOTES = 6,
    LONG_PRESS_MENU_FUNCTION_COUNT
  };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // Page turn button long press behavior
  enum LONG_PRESS_BUTTON_BEHAVIOR {
    OFF = 0,
    CHAPTER_SKIP = 1,
    ORIENTATION_CHANGE = 2,
    LONG_PRESS_BUTTON_BEHAVIOR_COUNT
  };

  // UI Theme
  //
  // APPEND-ONLY: uiTheme is persisted as the ENUM VALUE, and the settings row is
  // a positional list (SettingsList.h homeThemeValues()), so a value here must
  // never be inserted or reordered -- that would silently re-theme every device
  // whose setting was already saved. New themes go on the end.
  enum UI_THEME { CLASSIC = 0, LYRA = 1, LYRA_3_COVERS = 2, ROUNDEDRAFF = 3, COVER_GRID = 4, ANDRO = 5, PICKYOS = 6 };

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };

  // How Select opens the reader menu: the classic full-screen list, or a toolbar
  // overlay (top/bottom bars with Contents / Text / More bottom-sheet panels)
  // painted over the page.
  enum READER_MENU_STYLE { READER_MENU_LIST = 0, READER_MENU_TOOLBAR = 1, READER_MENU_STYLE_COUNT };

  enum TILT_PAGE_TURN { TILT_OFF = 0, TILT_NORMAL = 1, TILT_NVERTED = 2, TILT_PAGE_TURN_COUNT };
  enum VIBRATION { VIBRATION_TOUCH = 0, VIBRATION_TOUCH_PAGE = 1, VIBRATION_OFF = 2 };
  // r88: ON by default where the board actually has a motor. The motor is driven
  // through a bounded, blocking pulse whose off-write is unconditional (see
  // src/HapticFeedback.h), so there is no path that can leave it energised and
  // nothing to gain by shipping it dark; boards without a motor keep OFF, which
  // also keeps the Controls row hidden.
#if FREEINK_CAP_HAPTIC
  uint8_t vibration = VIBRATION_TOUCH;
#else
  uint8_t vibration = VIBRATION_OFF;
#endif
  enum HAPTIC_INTENSITY { HAPTIC_LOW = 0, HAPTIC_MEDIUM = 1, HAPTIC_HIGH = 2 };
  uint8_t hapticIntensity = HAPTIC_HIGH;

  // HAPT3 (r101.9): vibration on the SOFT KEYS, its own axis and its own gate. 0 = OFF
  // (default), which is the pre-HAPT3 behaviour exactly: the soft keys fire nothing that
  // the "vibration" enum above did not already fire, so neither the hand nor the glass
  // changes on an older settings file. 1 = a soft key (the two capacitive page keys under
  // the screen, plus Home/Back in the one case the tap feedback cannot cover them: Haptic
  // Feedback = Off) stashes ONE 120 ms pulse (detail::kSoftKeyPulseMs -- the driver's level-3
  // perceptibility floor; 70 ms went through mn_haptic_pulse_exact() and was measurably silent)
  // in the shared deferred mailbox.
  //
  // Why it is separate from the vibration enum: that enum's middle step ("On Touch + Page
  // Turn") also buzzes every screen tap and swipe turn, so there was no way to buzz the
  // bezel page keys alone. This row is that way, and it is off by default.
  //
  // Persisted through the SettingInfo table under the key "softKeyHaptic" (one TOGGLE row,
  // Settings > Controls, right under Haptic Intensity), so a settings file with no such
  // key loads as 0.
  uint8_t softKeyHaptic = 0;

  // B-P1-3 (r101.13b): does a press that lands DURING a panel refresh survive to the next
  // wasPressed()? 1 (default) = yes. 0 = the pre-r101.13b behaviour.
  //
  // The poll hook (main.cpp) samples input at both edges of every blocking waveform because
  // nothing else can: the main task is parked on the drive. But InputManager::update() CLEARS
  // every one-shot event on entry, so the poll the loop takes immediately after the drive used to
  // wipe the press the hook had just latched -- the event reached no activity at all. With this set,
  // the hook's poll becomes that iteration's sample and the loop skips its own poll (see
  // inputPolledDuringDrive in main.cpp), so the edge survives.
  //
  // Why it is a runtime switch and not just a fix: getting the hand-off wrong fails as DOUBLE
  // DELIVERY, and a phantom second tap in the reader reads as "it turned two pages" -- a symptom
  // indistinguishable from a different bug. Turning this off restores the old polling exactly,
  // without a flash cycle.
  uint8_t inputDuringRefresh = 1;

  // USB debug shell (docs/engineering/USB_DEBUG_CHANNEL.md phase 1). Persisted
  // under the key "usbDebugShell"; the value is 0 = off (default) or 1 = on.
  // It is written and read by hand in toJson()/fromJson() rather than through
  // the SettingInfo table: the row lives on the Debug page, not in Controls, so
  // there is no i18n string or enum list for the generic loop to walk — the same
  // reason shortPwrBtn and the front-button remap are hand-persisted.
  //
  // Compile-time companion: CP_USB_SHELL (src/platform/PlatformFeatures.h)
  // says whether the shell is IN the image at all. This field only decides
  // whether it RUNS. With the capability compiled out the field is inert.
  uint8_t usbDebugShell = 0;

  // TTS1: the audio OUTPUT gate. 0 = OFF, and OFF means the audio path is never
  // brought up: no P1.4 rail access, no UART1 to the BH1098G, no I2S.
  // AUDIO2: reachable from Settings > Controls ("Audio Output") on the Metalio 4
  // Plus. Kept hand-persisted (toJson/fromJson) as well: it folds any non-zero
  // value to 1, and it stays loadable on boards where the row is not emitted.
  uint8_t audioOutEnabled = 0;

  // AUDIO1: short UI feedback clicks (page turn, menu move, select, back,
  // disallowed). Separate from audioOutEnabled on purpose: enabling the audio
  // PATH for a future feature must not make the UI noisy, so a click needs BOTH
  // switches. 0 = OFF, the default. AUDIO2: reachable from Settings > Controls
  // ("UI Sounds"), hidden there while Audio Output is off. Kept hand-persisted
  // (toJson/fromJson), same reasons as the gate above.
  uint8_t uiSoundsEnabled = 0;

  // DLG1 (r101.9) / r101.10: the USB drive-mode EXIT frame. 1 = ON (default since r101.10 -- the
  // operator asked for this dialog and reported it missing while the row defaulted to 0). 0 = the
  // pre-DLG1 exit path exactly: between the last frame of the session and the cold boot the exit
  // performs no compose and no panel drive at all. ON paints one short "card returned" frame before
  // the handoff (UsbMscActivity::paintExitFrame), which costs exactly one whole-panel HALF drive on
  // that path and nothing anywhere else.
  //
  // Persisted through the SettingInfo table under the key "usbDriveExitDialog" (one TOGGLE row,
  // Settings > System). A settings file written before the key existed loads as this default; the
  // one-shot "usbDriveExitDialogMigrated" marker in CrossPointSettings::fromJson() flips a STORED 0
  // (every r101.9 save wrote one) to ON exactly once, then the row follows the operator again.
  uint8_t usbDriveExitDialog = 1;

  // POSCR1 (r101.9): what the reader shows when it is switched OFF. The last frame is the
  // frame the e-paper holds after the rails drop, so this is a display setting, not a
  // shutdown one.
  //
  //   TEXT   (0, default) -- exactly the frame every build before this one shipped: a
  //                          cleared screen with a centred bold "Powered off". Zero visible
  //                          change until the operator picks something else.
  //   CUSTOM (1)          -- an image from /poweroff/: poweroff.bmp/png/jpg if present,
  //                          else a random supported file from that folder. Drawn on the
  //                          16-level GRAY16 path (the sleep screen's), which costs the
  //                          shutdown one from-white GC16 plus the decode, ~2.8 s in total.
  //   DARK   (2)          -- the standard dialog with its inks INVERTED (dark ground, light
  //                          label), not a bare black panel: on black a label-less screen is
  //                          indistinguishable from a dead device. No SD access.
  //   LIGHT  (3)          -- cleared white panel with the label, no SD access.
  //   BLANK  (4)          -- cleared white panel, no label, no SD access.
  //
  // Persisted through the SettingInfo table under the key "powerOffScreen"; an older
  // settings file has no such key and loads as TEXT, i.e. the old glass behaviour.
  enum POWER_OFF_SCREEN_MODE {
    POWER_OFF_TEXT = 0,
    POWER_OFF_CUSTOM = 1,
    POWER_OFF_DARK = 2,
    POWER_OFF_LIGHT = 3,
    POWER_OFF_BLANK = 4,
    POWER_OFF_SCREEN_MODE_COUNT
  };
  uint8_t powerOffScreen = POWER_OFF_TEXT;

  // EQ1 (r101.9): the global MODE_EQUALIZE_BLACK toggle the operator asked for.
  //
  // 0 (default) = OFF, and OFF is the pre-EQ1 request exactly: no call site adds the flag, so
  // every drive asks for the mode it asked for before this row existed (see
  // src/display/EqualizeBlack.h -- the helper returns its argument untouched).
  //
  // 1 = the diff-area GC16 paints -- popups/dialogs (BaseTheme::drawPopup, OptionPopup), the
  // settings-list entry frame (UiListActivity) and the sleep-entry popup -- are requested as
  // EQUALIZE_REFRESH. That is the SAME 48-frame GC16 table: the frame count comes from the
  // waveform table (render.c:178) and the LUT function is picked afterwards (render.c:213), so
  // this is a LUT swap, not a longer waveform. The LUT (lut.c:566-592) drives every pixel
  // whose TARGET is black whatever the panel currently shows, page-wide, which is what
  // "re-ink the black" means here. ED047TC2 only -- every other controller would fall back to
  // its FAST path, so the helper declines there.
  //
  // Deliberately NOT applied to FULL/from-white drives (the sleep frame, the periodic clean,
  // the 16-level erase sweep): those already look every pixel up as 15 -> target, which
  // includes the long 15 -> 0 sweep, so equalizing there buys nothing and would drop the
  // from-white preconditioning. Also NOT applied to the reader's Quick Refresh, which already
  // asks for EQUALIZE unconditionally (that is what it has been since r101.4).
  //
  // Persisted through the SettingInfo table under the key "equalizeBlack" (one TOGGLE row,
  // Settings > Display), so a settings file written by an earlier build has no such key and
  // loads as 0.
  uint8_t equalizeBlack = 0;

  enum TOUCH_READER_CONTROLS { TOUCH_READER_OFF = 0, TOUCH_READER_ON = 1, TOUCH_READER_CONTROLS_COUNT };

  // Per-direction reader page-turn gestures. INVERTED_TAP is tap-only; either
  // direction set to it swaps both directions' shared tap zones (see
  // ReaderUtils::detectTouchPageTurn).
  enum PAGE_TURN_GESTURE {
    TAP_AND_SWIPE = 0,
    TAP_ONLY = 1,
    SWIPE_ONLY = 2,
    INVERTED_TAP = 3,
    PAGE_TURN_GESTURE_DISABLED = 4,
    PAGE_TURN_GESTURE_COUNT
  };

  // How the reader menu opens on touch boards. Persisted under the legacy
  // "tapForReaderMenu" key: 0/1 keep their old Off/Tap meaning.
  enum SHOW_READER_MENU { READER_MENU_OFF = 0, READER_MENU_TAP = 1, READER_MENU_SWIPE_UP = 2, SHOW_READER_MENU_COUNT };

  enum QUICK_RESUME_SLEEP_SCREEN {
    QUICK_RESUME_NEVER = 0,
    QUICK_RESUME_AFTER_TIMEOUT = 1,
    QUICK_RESUME_SLEEP_SCREEN_COUNT
  };

  // Sleep screen settings
  uint8_t sleepScreen = DARK;
  // Night mode: inverted output polarity, applied to every activity per
  // render by ActivityManager. The sleep screen opts out itself.
  uint8_t screenInverted = 0;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Status bar settings
  uint8_t statusBarChapterPageCount = 1;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t statusBarProgressBar = HIDE_PROGRESS;
  uint8_t statusBarProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarBattery = 1;
  uint8_t xtcStatusBarMode = XTC_STATUS_BAR_HIDE;
  // Clock display in status bar (any board whose RTC probe succeeds)
  uint8_t statusBarClock = STATUS_BAR_CLOCK_HIDE;
  // LEGACY, kept for migration only: quarter-hour UTC offset biased by 48
  // (48 = UTC+0). Superseded by clockTimezone; read once by
  // timezones::activeIndex() when clockTimezone is unset.
  uint8_t clockUtcOffsetQ = 48;
  // Clock display format: 0 = 24-hour, 1 = 12-hour
  uint8_t clockFormat = 0;
  // Index into the timezone table (src/util/Timezones.cpp, append-only).
  // 255 = never chosen; falls back to the legacy UTC offset, then UTC.
  uint8_t clockTimezone = 255;
  // CLOCK_DST_MODE: follow the zone's DST rule, or force it on/off.
  uint8_t clockDst = CLOCK_DST_AUTO;
  // Show the clock opposite the battery in every header band that draws one.
  uint8_t clockShowInHeader = 0;
  // Set once an NTP sync succeeds. Used to skip re-syncing on every WiFi connect.
  // Resetting to 0 (e.g. via the web UI) forces a re-sync on next WiFi connect.
  uint8_t clockHasBeenSynced = 0;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t paragraphIndentSpaces = 2;
  static constexpr uint8_t WORD_SPACING_MIN = 50;
  static constexpr uint8_t WORD_SPACING_MAX = 200;
  static constexpr uint8_t WORD_SPACING_STEP = 25;
  uint8_t wordSpacing = 100;                              // percent of the font's space advance
  static constexpr uint8_t CHARACTER_SPACING_OFFSET = 2;  // stored 0..4 maps to -2..+2 px
  uint8_t characterSpacing = CHARACTER_SPACING_OFFSET;
  int8_t getCharacterSpacing() const { return static_cast<int8_t>(characterSpacing - CHARACTER_SPACING_OFFSET); }
  uint8_t textAntiAliasing = 1;
  // r161 MENU AA. Anti-aliasing for MENUS/UI, separate from the reader's Text AA:
  // the two are different lanes on this panel (UI paints are FAST -> the 8-level
  // grey wipe; a reader page turn is its own request), so a user who wants crisp
  // menus and AA text, or the reverse, needs two switches. OFF renders UI text as
  // 1-bit (glyph coverage is thresholded at the blitter, so the frame carries no
  // intermediate level and the FAST paint takes the binary DU lane); ON is the
  // behaviour every build before r161 had, byte for byte.
  uint8_t menuAntiAliasing = 1;
  // Short power button click behaviour.
  //
  // r162 PWR4: on the Metalio board the default is SLEEP, not IGNORE. IGNORE was
  // never inert there: the board decides on the RELEASE edge (main.cpp,
  // `power key UP after …`) and sleeps on any press from getPowerButtonDuration()
  // (400 ms with IGNORE) up to METALIO_POWER_OFF_HOLD_MS (3000 ms) -- so the
  // operator's quick click did nothing and a half-second press slept, which is
  // exactly the "short hold wakes/sleeps at random" complaint. SLEEP drops the
  // threshold to 10 ms, so a click-and-release sleeps, while a hold past 3 s
  // still opens the power-off dialog and every other binding (PAGE_TURN,
  // FOOTNOTES, CREATE_CLIPPING, FORCE_REFRESH) still gets its own 400 ms window
  // when the operator selects it. Other boards keep IGNORE.
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  uint8_t shortPwrBtn = SLEEP;
#else
  uint8_t shortPwrBtn = IGNORE;
#endif
  // X4 Pro: double-click power toggles the frontlight. Disabling frees the
  // power button for shortPwrBtn actions without the double-click wait.
  uint8_t doubleClickPwrLight = 1;
  uint8_t homeButtonTapAction = static_cast<uint8_t>(HomeButtonAction::Home);
  uint8_t homeButtonDoubleTapAction = static_cast<uint8_t>(HomeButtonAction::ToggleFrontlight);
  uint8_t homeButtonLongPressAction = static_cast<uint8_t>(HomeButtonAction::ReaderMenu);
  // ---------------------------------------------------------------------------
  // r97 KEY1: the physical BOOT/AI key (GPIO61 on the Metalio 4 Plus), bound to
  // the SAME three-gesture layer as the home key (util/HomeButtonInput.h) but
  // with its own two rows in Settings > Reading and its own gesture state.
  // Persisted BY KEY STRING ("bootKeyTapAction" / "bootKeyLongPressAction"), so
  // a row added anywhere in the Reading list cannot shift these values.
  //
  // Values are an APPEND-ONLY list of their own, deliberately not
  // HomeButtonAction's: the boot key has no page-turn/bookmark meaning, and
  // reusing that enum's ordinals would make a later action-list edit silently
  // re-point a saved value. MappedInputManager translates these to
  // HomeButtonAction where the two overlap, so the ACTION CODE stays in one
  // place.
  enum BOOT_KEY_ACTION {
    BOOT_KEY_IGNORE = 0,
    BOOT_KEY_FRONTLIGHT = 1,
    BOOT_KEY_SYNC = 2,
    BOOT_KEY_ACTION_COUNT
  };
  // Defaults per the operator's request: tap = frontlight, long = KOReader sync.
  uint8_t bootKeyTapAction = BOOT_KEY_FRONTLIGHT;
  uint8_t bootKeyLongPressAction = BOOT_KEY_SYNC;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  // Reading orientation. On a landscape-native panel (this port's display path is the
  // identity, 1216x684) a PORTRAIT default rotates every reader page 90 degrees; the
  // enum itself marks LANDSCAPE_CCW as "native panel orientation". Set per build.
#if defined(CP_DEFAULT_LANDSCAPE_READING)
  uint8_t orientation = LANDSCAPE_CCW;
#else
  uint8_t orientation = PORTRAIT;
#endif
  // Button layouts.
  // r162 HYG2: `frontButtonLayout` is GONE. It was declared here and referenced
  // nowhere else -- not persisted by toJson, not emitted by SettingsList, not
  // read by MappedInputManager (which uses the four frontButtonBack/Confirm/Left/
  // Right remaps instead), and no migration ever looked at it despite the old
  // "retained for migration only" comment. FRONT_BUTTON_LAYOUT above is kept as
  // the documented four-layout ordering a future row would use.
  uint8_t sideButtonLayout = PREV_NEXT;
  uint8_t frontButtonFollowOrientation = 0;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Reader font settings
  uint8_t fontFamily = NOTOSANS;
  // Point size of the reader font. Only sizes the active family actually ships
  // are selectable; SdCardFontSystem::ensureLoaded() snaps this to the nearest
  // available size (and persists the snap) whenever the family changes.
  uint8_t fontPointSize = DEFAULT_FONT_POINT_SIZE;
  // Line height as a percent of the font size (100..200, 1% steps).
  uint8_t lineSpacingPercent = LINE_SPACING_DEFAULT_PERCENT;
  uint8_t paragraphAlignment = JUSTIFIED;
  // Auto-sleep timeout setting (default 10 minutes). Legacy sleepTimeout enum values are migration-only.
  uint8_t sleepTimeoutMinutes = 10;
  // Step 2.5 item 9: idle CPU frequency step, an index into
  // systemKnobs::IDLE_FREQ_MHZ {80,160,240,320}. 0 = 80 MHz keeps the behaviour
  // every earlier build had (HalPowerManager::LOW_POWER_FREQ), so an upgrade does
  // not change how the device idles. Applied at boot and on change through
  // systemKnobs::applyIdleFreq(); the full-speed restore is untouched.
  uint8_t idleCpuFreq = 0;
  // Step 2.5 item 11: panel black depth, an index into systemKnobs::VCOM_MV
  // {1200,1600,2000} -- the vendor's three levels, taken exactly. 0 = 1200 mV is
  // the value this firmware has always written. Only surfaced on the TPS65185
  // board; still persisted everywhere so a shared settings file stays valid.
  uint8_t panelVcomDepth = 0;
  // Power off while asleep (standby auto-off). 0 = never, the default; see
  // STANDBY_AUTO_OFF_NEVER_MINUTES for the picker's "Never" boundary.
  uint8_t standbyAutoOffMinutes = 0;
  // r163 3.5: the charge-aware idle policy, as a NUMBER (0 = follow the sleep
  // timeout, 1 = do not auto-sleep while charging) because the generic persistence
  // walk and the backup sanitizer both carry uint8_t. 0 is the default and it means
  // this whole feature is inert: the configured timeout keeps its exact meaning.
  // The mechanism is an E6 HARD need (system/ChargeIdlePolicy.h, PowerNeed.h), not a
  // branch in the sleep path -- a manual sleep still sleeps and the park sequence is
  // untouched. Sanitized on restore by lib/Backup/BackupProfile.cpp's kRanged.
  uint8_t chargeIdlePolicy = 0;
  // Charge current for the CX25601N (step 2.5 item 8), as an INDEX into
  // chargercurrent::kChoicesMa = {500, 1000, 1500} mA -- an index and not mA because
  // the generic persistence loop only carries uint8_t. Applied at boot and on change
  // by src/system/ChargerCurrent.cpp; the part quantises to 80 mA steps, so the IC
  // lands on 480 / 960 / 1440 mA. Default 1 A is the vendor board's own default.
  uint8_t chargeCurrent = 1;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  uint8_t swipeSensitivity = SWIPE_NORMAL;
  // Refresh policy. "Menu" covers repaints inside an activity (lists, status changes); "activity"
  // covers entering and leaving an activity. Two knobs because they have different budgets: cursor
  // moves happen constantly, activity changes a few times a minute.
  uint8_t menuTransitionRefresh = REFRESH_AUTO;
  uint8_t activityTransitionRefresh = REFRESH_AUTO;
  uint8_t autoRefreshThreshold = PCT_5;
  // Reader cadences. "Quick" is a GC16 pass over the diff (cleans, no blackout); "full" is the
  // existing refreshFrequency (GC16 from white). Set full to a multiple of quick, or the full clean
  // simply replaces the quick one whenever they coincide.
  uint8_t quickRefreshPages = REFRESH_15;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margin settings
  static constexpr uint8_t SCREEN_MARGIN_MIN = 5;
  static constexpr uint8_t SCREEN_MARGIN_MAX = 40;
  static constexpr uint8_t SCREEN_MARGIN_STEP = 5;
  uint8_t screenMargin = SCREEN_MARGIN_MIN;
  // OPDS download destination folder ("" = SD root). Global; edited from the
  // OPDS server list. Persisted via a category-less SettingInfo::String in
  // SettingsList.h, so it stays out of the on-device Settings screen.
  char opdsDownloadFolder[64] = "";
  // On-disk filename format for OPDS downloads (0=Author-Title default, 1=Title-Author,
  // 2=Title). See OpdsFilenameFormat. Persisted via a category-less SettingInfo::Enum,
  // edited from the OPDS server list; hidden from the on-device Settings screen.
  uint8_t opdsFilenameFormat = 0;
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // Long-press page turn button behavior
  uint8_t longPressButtonBehavior = OFF;
  // Long-press Confirm function in EPUB reader (cycles through LONG_PRESS_MENU_FUNCTION values).
  // Defaults to Disabled so shortcut-based bookmark toggling remains opt-in.
  uint8_t longPressMenuFunction = LP_MENU_DISABLED;
  // UI Theme
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  // This board has PSRAM, so supportsCoverGrid() passes and the cover-grid home (currently-reading
  // hero, cover grid, five-icon bottom tab bar) is available -- it is the intended default for a
  // touch-first reader, and the standard Lyra home draws no tab bar at all.
  uint8_t uiTheme = COVER_GRID;
#else
  uint8_t uiTheme = LYRA;
#endif
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Power button return from footnotes (1 = enabled, 0 = disabled)
  uint8_t pwrBtnFootnoteBack = 1;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 1;
  // Focus Reading - emphasizes the first part of words with bold
  uint8_t focusReadingEnabled = 0;
  uint8_t readerMenuStyle = READER_MENU_LIST;
  // SD card font family name (empty = use built-in fontFamily)
  char sdFontFamilyName[32] = "";
  // Dictionary folder name under /dictionaries (empty = no dictionary)
  char dictionaryName[32] = "";
  // Master switch for all SD-card plugin discovery/execution. New installs keep
  // this disabled; settings migration enables it when pre-existing plugin files
  // are found so a firmware update never silently disables an existing setup.
  uint8_t pluginsEnabled = 0;
  // User hid the Plugin Hub install row; cleared when plugins are turned back on.
  uint8_t pluginHubPromptHidden = 0;
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Append each file's extension to its name in the file browser (0 = stem only,
  // 1 = "book.epub"). Browse Files' view preference, so it is hand-persisted
  // rather than a SettingsList row: this build adds no settings-screen row for it.
  uint8_t showFileExtensions = 0;
  // Show the title and author read from inside each book rather than its
  // filename. Users can disable this to make index rebuilds skip EPUB parsing.
  uint8_t libraryUseMetadata = 1;
  // LIB10 kill-switch. 0 (the default) shows a series drill's books by FILE NAME
  // at the theme's one dense file-name face, wrapped over two lines inside the
  // row height the list already has. 1 goes back to the behaviour this view had
  // before the item: one line, at the list's own face. The face is ONE for the
  // whole drill either way -- a per-row face made the page size oscillate.
  uint8_t libraryFixedRowFace = 0;
  // Remove a book from the Recent Books list when its End-of-Book screen is reached (0 = off, 1 = on)
  uint8_t removeReadBooksFromRecents = 0;
  // Move epub to /Read/ folder on SD card when finished (0 = disabled, 1 = enabled)
  uint8_t moveFinishedToReadFolder = 0;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // Tilt-based page turning (X3 only — requires QMI8658 IMU)
  uint8_t tiltPageTurn = TILT_OFF;
  // Master reader-touch toggle on boards with a touch controller.
  uint8_t touchReaderControls = TOUCH_READER_ON;
  // FL1: side-edge drag for the frontlight (KOReader-style). 0 = OFF (default:
  // the gesture does not exist), 1 = dragging along the LEFT edge sets
  // brightness and along the RIGHT edge warmth. A reader-surface gesture, so it
  // rides touchReaderControls: with touch reader controls off it is off too.
  uint8_t edgeDragFrontlight = 0;
  // Which gestures turn the page in each direction (PAGE_TURN_GESTURE).
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  // Taps turn pages. SWIPE_ONLY (the upstream default) silently discards every tap, which on a
  // device whose primary input is the touchscreen means "tapping the page does nothing" -- the soft
  // keys and hardware buttons still work because they do not go through the tap-zone path at all.
  // NOTE: settings persist to /.crosspoint/settings.json on the SD card, so an existing install keeps
  // its stored value; this default only governs a fresh one.
  uint8_t pageTurnGesture = TAP_AND_SWIPE;
  uint8_t previousPageGesture = TAP_AND_SWIPE;
#else
  uint8_t pageTurnGesture = SWIPE_ONLY;
  uint8_t previousPageGesture = SWIPE_ONLY;
#endif
  // Reader menu open gesture (SHOW_READER_MENU: off / center tap / bottom-edge
  // up-swipe). Only surfaced on home-key boards, where Home is the capacitive
  // key and the bottom edge is free; elsewhere it stays at the Tap default.
  uint8_t showReaderMenu = READER_MENU_TAP;
  // Frontlight quick-panel state. Category-less SettingsList entries persist
  // these without adding them to the regular Settings screen.
  uint8_t frontlightBrightness = 60;
  uint8_t frontlightWarmth = 50;  // 0 = cool .. 100 = warm
  uint8_t frontlightOn = 0;
  // Restore the saved on/off state after a normal boot or wake. Brightness and
  // warmth are always remembered even when this is disabled.
  uint8_t frontlightRestoreOnWake = 1;
  // What the slide switch controls (boards with one): frontlight on/off or
  // night mode. The switch's position is applied at boot and on each change.
  enum SLIDE_SWITCH_ACTION : uint8_t {
    SLIDE_SWITCH_FRONTLIGHT = 0,
    SLIDE_SWITCH_NIGHT_MODE = 1,
    SLIDE_SWITCH_TOUCH_CONTROLS = 2,
  };
  uint8_t slideSwitchAction = SLIDE_SWITCH_FRONTLIGHT;
  // Language setting (Language enum index, default 0 = EN)
  uint8_t language = 0;
  // Keyboard layouts the user can reach, using keyboard_layouts::ALL table bits.
  // 0 means "not configured", resolved to the UI language's layout plus English.
  // Any other value is an explicit choice and is used as-is: the language of the
  // books someone reads is not necessarily the language of their UI.
  // See keyboard_layouts:: for the bit assignment and the defaulting rules.
  uint16_t keyboardLayouts = 0;
  // Bluetooth page turner, saved under the keys lib/BlePageTurner reads and writes (BlePageTurnerJson.h).
  bleturner::Config ble;
  // Quick Resume: keep current content visible with moon icon instead of showing a static sleep screen.
  uint8_t quickResumeSleepScreen = QUICK_RESUME_NEVER;

  static constexpr uint8_t MIN_SLEEP_TIMEOUT_MINUTES = 1;
  static constexpr uint8_t SLEEP_TIMEOUT_NEVER_MINUTES = 31;
  static constexpr uint8_t MAX_SLEEP_TIMEOUT_MINUTES = SLEEP_TIMEOUT_NEVER_MINUTES;

  // Standby auto-off range. 0 = never (default); the picker's top boundary is also
  // "Never", matching how the sleep timeout picker labels its max.
  static constexpr uint8_t MIN_STANDBY_AUTO_OFF_MINUTES = 0;
  static constexpr uint8_t STANDBY_AUTO_OFF_NEVER_MINUTES = 120;
  static constexpr uint8_t MAX_STANDBY_AUTO_OFF_MINUTES = STANDBY_AUTO_OFF_NEVER_MINUTES;

  // Callback to resolve SD card font IDs. Set by SdCardFontSystem::begin().
  // Returns font ID or 0 if not found.
  using SdFontIdResolver = int (*)(void* ctx, const char* familyName, uint8_t fontSize);
  SdFontIdResolver sdFontIdResolver = nullptr;
  void* sdFontResolverCtx = nullptr;

  uint16_t getPowerButtonDuration() const {
    return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? 10 : 400;
  }
  int getReaderFontId() const;

  // Drop the SD font selection and fall back to the built-in family. The reader
  // point size comes back into BUILTIN_READER_POINT_SIZES with it, since that is
  // the only set a built-in family ships — otherwise the settings UI would keep
  // offering a size nothing renders at. Both fields are persisted in one write.
  void clearSdFontFamily();

  // Resolved status-bar composition. Consumers read the spec; only settings
  // editors read the raw fields.
  //
  // Deliberately NOT built under storeMutex: every field it reads is a single
  // byte, so a concurrent settings write can never produce a corrupt value —
  // only a snapshot mixing pre- and post-change fields. That costs at most one
  // e-ink frame drawn with a mixed status bar, which self-corrects on the next
  // refresh. Locking here would instead put a mutex on the render path and
  // stall it behind the SD write inside saveToFile(). Don't add one back.
  struct StatusBarSpec {
    bool showChapterPageCount = false;
    bool showBookProgressPercent = false;
    uint8_t titleMode = HIDE_TITLE;  // STATUS_BAR_TITLE
    bool showBattery = false;
    bool showBatteryPercent = false;
    uint8_t clockMode = STATUS_BAR_CLOCK_HIDE;  // STATUS_BAR_CLOCK_MODE
    bool clock12h = false;
    uint8_t progressBarMode = HIDE_PROGRESS;  // STATUS_BAR_PROGRESS_BAR
    uint8_t progressBarHeightPx = 0;          // (thickness+1)*2; 0 when the bar is hidden
    uint8_t xtcMode = XTC_STATUS_BAR_HIDE;    // XTC_STATUS_BAR_MODE

    bool showsProgressBar() const { return progressBarMode != HIDE_PROGRESS; }
    bool showsTitle() const { return titleMode != HIDE_TITLE; }
    bool showsClock() const { return clockMode != STATUS_BAR_CLOCK_HIDE; }
    // Visibility of the text lane. Clock hardware presence is the caller's
    // concern: pass halClock.isAvailable(), or true for layout reservation.
    bool textLaneVisible(bool clockAvailable) const {
      return showChapterPageCount || showBookProgressPercent || showsTitle() || showBattery ||
             (showsClock() && clockAvailable);
    }
  };
  StatusBarSpec statusBarSpec() const;

  // Resolved text-rendering configuration for the Epub layout engine. The
  // viewport is renderer/orientation-derived, so the caller supplies it —
  // passing it in keeps a spec from ever existing in a half-filled state.
  // Unlocked for the same reason as statusBarSpec(); see the note above.
  ReaderRenderSpec readerRenderSpec(uint16_t viewportWidth, uint16_t viewportHeight) const;

  static const char* getFilePath() { return "/.crosspoint/settings.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  static void validateFrontButtonMapping(CrossPointSettings& settings);
  static uint8_t sleepTimeoutEnumToMinutes(uint8_t legacyValue);

  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
  // Tap/slide discrimination threshold in native px, from swipeSensitivity.
  int getSwipeThresholdPx() const;
  // Percent (1/3/5/10/20) that Auto uses as the DU-vs-GC16 cut-off.
  int getAutoRefreshThresholdPercent() const;
  // Pages between quick (HALF) cleans. NEVER becomes a huge number so the comparison never fires.
  int getQuickRefreshPages() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
