# The global Back key (r99.7)

One key, two gestures, one implementation: **tap = one level back**, **hold = Home**. The cover-glass
soft key, the physical Back button and the header back arrow all arrive as `Button::Back`, so all three
obey the same rule on every screen.

## Where the rule lives

`ActivityManager::handleGlobalBackKey()` (`src/activities/ActivityManager.cpp`) runs at the top of
`loop()`, before the activity's own `loop()`, in this order:

1. `pendingAction != None` -> nothing (the press belongs to the screen that is leaving).
2. HOLD (`wasLongPressed(Back, 1200, notBefore)` or `wasBackControlHold(1200, visitStart)`) ->
   `handleLongPressBack()`, else `goHome()`. Checked first so a hold is never also a tap. r163: only
   for a press/contact whose DOWN was observed on this screen visit (see the r163 section below).
3. TAP (`wasReleased(Back)`, armed by a press that began on **this** screen visit) -> `popActivity()`
   (empty stack lands on Home) unless the activity declared `ownsBackKey()`.

While a navigation is already queued (`pendingAction != None`) the rule stays out of the way for that
pass: the press belongs to the screen that is leaving, and acting on it would pop the screen underneath.
That pass is the only window in which a Back press is dropped rather than misapplied; normally a
transition completes inside the same pass, so the window is microseconds. The function returns whether
IT navigated, and the caller skips the rest of the pass only then -- a queued transition must still run
(it is what keeps a deferred transition, e.g. one held for the BLE teardown, from starving).

`Activity::ownsBackKey()` (default **false**) is the only per-screen input the rule needs: a screen that
returns true answers Back itself and the manager never touches it, so nothing can double-fire. Screens
that return false -- including every screen written later -- get the generic pop.

## Press that outlives its screen

A press is latched with the screen visit it started on (`backPressLatched` / `backPressVisit`), and every
transition increments `screenVisit`. A release delivered after a navigation is therefore ignored: one
press is one action. No transition path has to clear anything.

## Per-activity impact

| Activity | Own Back handler | Change |
|---|---|---|
| `UiListActivity` (+ `UiTabListActivity`) | `handleButtons()` -> `finish()`; subclasses override `onBackButton()` / `handleCustomInput()` | **unchanged** (declares ownership on the base, as do all its subclasses) |
| `FileBrowserActivity` | up one folder / root on 1 s hold | **unchanged** (owns; its own hold is the 1000-1200 ms window, above that the global hold is Home, exactly as in r88) |
| `LibraryListActivity` | tab/shelf back, next-release lock | **unchanged** |
| `CatalogActivity`, `OpdsBookBrowserActivity`, `PluginCatalogActivity`, `PluginHubInstallActivity` | leave list, cancel download | **unchanged** |
| `TextSettingsActivity`, `DebugActivity`, `FontDownloadActivity` | via `UiListActivity` base | **unchanged** |
| `ReaderActivity` -> `EpubReaderActivity`, `XtcReaderActivity` | **tap = leave the book for the file browser, hold = Home** (r163; was tap = Home, hold = file browser) | **changed in r163**: the reader no longer claims the hold (`handleLongPressBack()` is gone), so the 1200 ms global hold is what goes Home, and `handleBackNavigation()` answers only the tap |
| `EpubReaderBookmarks/ChapterSelection/ClippingList/Menu/FootnoteSelect/PercentSelection`, `XtcReaderChapterSelection` | own Back | **unchanged** (each declares ownership) |
| `DictionaryDefinitionActivity` | header long-press -> lookup; Back -> close | **unchanged** |
| `KOReaderSyncActivity` | cancel sync | **unchanged** |
| `WifiSelectionActivity` | cancel connect | **unchanged** |
| `CrossPointWebServerActivity` | leave screen, stop server | **unchanged** |
| `CalibreConnectActivity`, `ClockSyncActivity`, `KOReaderAuthActivity`, `OtaUpdateActivity`, `SdFirmwareUpdateActivity`, `ClearCacheActivity`, `QrDisplayActivity` | cancel/finish | **unchanged** |
| `CrashActivity` | Back -> reboot/exit | **unchanged** |
| `KeyboardEntryActivity`, `IntervalSelectionActivity`, `ConfirmationActivity`, `PowerOffActivity`, `ClipSelectionActivity` | cancel with a result | **unchanged** (a bare pop would report a non-cancelled result) |
| `BmpViewerActivity`, `DetailsActivity`, `FrontlightPanelActivity` | finish | **unchanged** |
| `UsbMscActivity` | exit drive mode (r99.5) | **exempt**: `requiresExclusiveStorageLoop()` returns before the global rule runs, so drive mode never sees it; it also declares ownership. |
| `HomeActivity` | deliberately nothing | **unchanged**: Home is the bottom of the stack, a tap there is ignored by rule |
| `ButtonRemapActivity` | none (only Up = reset, Down = cancel); it also CAPTURES every raw front-button press as a mapping target | **had to add a handler**: declares ownership and Back now leaves without saving, before the capture (otherwise Back would be assigned to a role and then leave on its release) |
| `FullScreenMessageActivity` | none, no `loop()` at all | **now inherits**: tap = pop / Home |
| `DictionaryWordSelectActivity` | none | **had to add a handler**: its result feeds the reader (`!isCancelled` reopens the lookup), so it declares ownership and Back now returns `isCancelled = true` |
| `BootActivity`, `SleepActivity` | n/a | **exempt** (no user input in these loops) |

## On glass

1. Settings -> a sub-list (e.g. Display): tap Back = one level, hold = Home.
2. Library: tap Back = one level, hold = Home.
3. Reader (EPUB): tap Back = out of the book (file browser), 1.2 s hold = Home. The old inversion
   (tap = Home, 1.5 s hold = browser) is gone; there is no per-screen threshold any more.
4. Home: tap Back = nothing happens; hold = nothing happens.
5. Settings -> Remap front buttons: tap Back = leaves without saving (previously dead).
6. USB drive mode: Back still exits exactly as r99.5 made it exit (armed = reboot to Home, refused = screen exit).

## r163: the tap/hold split is now gated on a press this screen observed

One defect and one inversion were fixed here.

**Settings > System, one tap -> main screen.** `SettingsActivity` (every settings section) has no
`onBackButton()` override, so it never answers a tap on its own; the manager's HOLD branch is what
navigated. It ran before the `ownsBackKey()` check, and it was timed from `HalGPIO`'s raw
`buttonPressStart`, which `InputManager::applyStateChange()` only stamps when the whole button state
goes 0 -> nonzero. A press that began before the current screen existed -- or whose press-down sample
was swallowed by a blocking waveform or by the panel's expander bus guard -- therefore reported a held
time measured from an EARLIER contact, already past 1200 ms, and a tap became Home.

The fix is the shape `main.cpp`'s `allowSleepAt` gate already uses on the power key: a press that
cannot be timed is never a hold.

* `ActivityManager::handleGlobalBackKey()` latches the visit the press-down was observed on
  (`backPressSeenMs`), and only asks for a hold when `backPressLatched && backPressVisit == screenVisit`
  -- the same per-visit rule the tap already used.
* `MappedInputManager::wasLongPressed()` / `wasBackControlHold()` take an optional `notBeforeMs`: the
  implied press start (`millis() - held`) must not predate the moment the caller saw the press go down.
  The on-screen back control's contact must likewise have STARTED on this screen, which closes the
  same leak for a finger already resting on the header arrow when the screen changes.

**The reader is no longer inverted.** Tap = leave the book for the file browser (the reader is entered
with `replaceActivity()`, so `goToFileBrowser()` is the one-level expression); hold = Home, by the one
global 1200 ms rule. `EpubReaderActivity::handleLongPressBack()` is gone, so the manager's hold reaches
Home, and because a fired hold suppresses its own release, every Back release the reader sees is a tap.

**The settings root pops.** `SettingsListActivity::onBackButton()` calls `finish()` instead of
`onGoHome()`. At the root the stack is empty (`goToSettings()` replaces), so the pop path lands on Home
exactly as before; the difference is that no TAP in the firmware means "main screen" from inside a menu
tree.
