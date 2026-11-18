# R97 round — audit tables and live findings

This file is the durable record the r97 commits point at. It follows the r96.1
format: every claim is a **file:line** statement against the tree plus, where
stated, a line from the device's own log (`/.crosspoint/debug.log`, read over the
USB shell with `logtail`). Where a claim needs the operator's eyes, it says so.

Phases 1–2 (BATT1 `batt` diagnostic, the efuse/bench gates, the shipping freeze
discipline) are recorded in their own commit messages; this file starts with the
overnight Phase 4 features. The measurement-gated phases (3 / 3c / 5) were moved
to after the morning flash and are NOT covered here.

---

## Phase 4 — three features (KEY1, C2, BATT2)

### KEY1 — the BOOT/AI key (GPIO61) is now a remappable gesture key

**Hardware, verified against the vendor repo**: `BOOT_BUTTON_GPIO = 61`
(`freeink-sdk/libs/hardware/BoardConfig/include/MetalioEInk4PlusBoard.h:107`),
`INPUT_PULLUP`, so pressed = LOW. It is **not** a strapping pin — the strap is
GPIO0, which is also the I2C SDA line — and it is **not** RTCIO (only GPIO0–7
are), which is why it can never be a deep-sleep wake source.

Before this commit the pin was only ever configured: `pinMode()` at
`MetalioEInk4PlusBoard.h:378`, no `BTN_` index, and the SDK's Metalio branch of
`InputManager` mapped only the expander volume keys and the power key.

**Read path** (`lib/hal/HalGPIO.cpp`, `updateBootKey()`):

| step | detail |
|---|---|
| level | `digitalRead(BOOT_BUTTON_GPIO) == LOW` |
| debounce | 30 ms — the expander keys' order, so a press reaches the gesture layer within ~1 frame instead of the ~3 a cached poll would add |
| press | `wasBootKeyPressed()` one-shot on the debounced falling edge |
| release | `wasBootKeyTapped()` one-shot **only if** the long threshold was not reached — the tap is emitted on the way UP |
| hold | `wasBootKeyLongPressed()` one-shot while still down, at 700 ms (the same number the SDK home key uses) |

Both traps named in the brief are handled where the brief says they belong:

* **double-tap = Ignore.** `MappedInputManager::update()` passes
  `HomeButtonAction::Ignore` as the layer's doubleTap action, which is the
  branch in `util/HomeButtonInput.h:43` that returns the single tap
  IMMEDIATELY instead of holding it `DOUBLE_TAP_MS = 350` waiting for a second
  contact the key does not have.
* **tap on release.** `HalGPIO` only raises the tap on the release edge, so a
  long press cannot toggle the frontlight on the way down and then sync on the
  way up.

The boot key feeds its **own** `HomeButtonInput` instance
(`src/MappedInputManager.h`, `bootKeyInput`), not the home key's: on this board
`BoardConfig::hasHomeKey()` is true (the cover-glass HOME key is a touch contact
decoded outside the panel rect), so sharing one instance would let either
contact consume the other's pending edge. The resulting action is folded into
the same `homeButtonAction()` the reader and `main.cpp` already dispatch, so the
action code is not duplicated: `boot_key::toHomeAction()` maps the persisted
value onto `HomeButtonAction::ToggleFrontlight` / `Sync` / `Ignore`.

**Settings > Reading — two rows, persisted by KEY STRING:**

| row label | JSON key | options | default |
|---|---|---|---|
| `Boot key tap` | `bootKeyTapAction` | Ignore · Toggle Frontlight · CrossPoint Sync | **Toggle Frontlight** |
| `Boot key long press` | `bootKeyLongPressAction` | Ignore · Toggle Frontlight · CrossPoint Sync | **CrossPoint Sync** |

Values are their own append-only enum (`CrossPointSettings::BOOT_KEY_ACTION`), so
a later edit of the action list cannot silently re-point a saved number; the key
strings are what persist, so a row added anywhere in Reading cannot shift them.

The picker for both rows carries a description line via the new
`SettingInfo::withDescription()` (shown as the dialog's subject):
*"The boot key cannot wake the reader from sleep - only the power key does that."*

Strings: `STR_BOOT_KEY_TAP`, `STR_BOOT_KEY_LONG_PRESS`, `STR_BOOT_KEY_CANNOT_WAKE`
were added to `lib/I18n/translations/english.yaml` only. Every other language
falls back to the English text (the generator fills missing keys from English and
warns), so no translation is silently missing a row — it is visibly English.

**Known limitation, by design:** the two boot-key rows exist only on a board that
has the key (`BoardConfig::isMetalioEInk4Plus()`), so the web settings API does
not advertise a control the hardware cannot perform.

**Not verified on glass:** that the AI key's electrical contact is clean enough
for a single tap (the vendor uses it as push-to-talk, so a hold is its normal
gesture) and that 30 ms is the right debounce for this specific switch. The
`[SLP]`/`[BATT]` logs cannot answer either.

---

### C2 — the custom sleep overlay is its own drive, not an inversion

**Operator report**: a custom `.bmp` sleep screen painted over the live content
without a clean drive, leaving ghosting.

**Cause.** `TRANSPARENT_CUSTOM` is the mode that paints over live content by
design: `SleepActivity::onEnter()` kept the framebuffer (`if (frameWasInverted)
renderer.invertScreen();` at `SleepActivity.cpp:618`, then the popup-preserving
draw), and `renderTransparentOverlayPass()` draws only the overlay's opaque
pixels, so every transparent pixel kept the ink that was already on the glass.
The drive that followed carried that composite — the previous page was still
underneath it, and its residue ghosted through the overlay's white areas. On
this panel `grayscaleCapabilities()` is supported, so the path then went on to
`cleanBeforeAbsoluteGray()` + `displayGrayscaleBase()` + two plane passes, i.e.
several drives over the same content, none of them from white.

**Fix** — one frame, one drive, from white:

1. `onEnter()` clears the framebuffer to WHITE before the overlay is painted
   (`SleepActivity.cpp`, the `TRANSPARENT_CUSTOM` branch) and sets
   `overlayOwnDrive`, which scopes every rule below to that mode only.
2. `renderBitmapSleepScreen()` treats an own-drive overlay as B/W
   (`hasGreyscale` is forced false) so there is no gray base and no LSB/MSB
   plane pass.
3. `driveSleepOverlayFromWhite()` is the single definition of the drive:
   `cancelPromotedRefresh()` → `forceCleanNextRefresh()` → one `kSleepClean`
   (`FULL`, 0xF7). `kSleepClean` on this board IS `FULL_REFRESH`.
4. The PNG overlay path takes the same branch.

Rails: `enterDeepSleep()` paints sleep synchronously before any rail is dropped,
so the drive completes first — unchanged by this commit.

**One log line names the mode** so the morning capture can confirm it:

```
SLP  sleep overlay paint: own from-white clean drive (mode=3)
```

(`mode` is `kSleepClean`'s `HalDisplay::REFRESH_MODE`, 3 = FULL on this board.)

**Not verified on glass:** whether a B/W-only overlay is acceptable for art that
used to get gray. This is the deliberate cost of "one drive, nothing else over
the same content"; if the operator wants gray back, the honest way is a separate
two-drive mode, not a second pass inside this one.

---

### BATT2 — a measured battery statistics page

`Settings > System > Battery statistics` → `BatteryStatsActivity`, a Settings
sub-page (list chrome, Andro typography through the theme), not a dialog.

Header: `Battery statistics (NN%)`. Four blocks — **Awake since last charge**,
**Sleeping since last charge**, **During last charge**, **Since last charge** —
each with Time / Used (Charged for the charge block) / Per hour / Remaining.
Footer: a real touch button, `Reset statistics`, behind a confirmation dialog.

**Measured, not extrapolated.** `src/util/BatteryStats.cpp` samples the gauge
every 30 s (`SAMPLE_MS`) and integrates the real SoC delta per state, so "per
hour" is `delta / elapsed` over a window we actually observed. Nothing comes
from cell capacity: `designCapacityMah = 0` and `loadDesignCapacity()` is never
called, so the page carries percentages and elapsed time only.

**Awake vs sleeping, separately.** This board resumes IN PLACE (its framebuffer
survives standby, see `main.cpp`'s `frameDumpNeeded`), so `millis()` jumps
forward instead of restarting. Standby is therefore detected two ways:

* a sampling gap past `SLEEP_DETECT_MS` (90 s) books all but one period as
  sleep — `BATT  stats: standby gap <N>s counted as sleep`;
* a cold start books the wall-clock gap left in the record
  (`absorbGap()`, ≥ `SLEEP_GAP_MIN_MS` = 120 s) — `BATT  stats: wake gap …`.

The first SoC delta after a gap is charged to SLEEP (`pendingSleepDelta`),
because that is the state the battery spent the interval in.

**Charge sessions.** Start edge opens a new "during last charge" block; stop
edge freezes it and restarts the since-last-charge window from that SoC.
**Every charge decision reads through ONE function**:

```
HalPowerManager::chargingForStats()      // HalPowerManager.cpp
```

Today it forwards `isCharging()`, whose flag is suspect (the operator measured
~1 A while the UI showed not-charging; BATT1's `batt` exists to diagnose it).
Fixing that rule is a change in this function and nowhere else — the record,
its edges and the page all follow it.

**Remaining** is derived from the CURRENT state's rate only, and prints `--`
until the window is worth `MIN_RATE_WINDOW_MS` (10 min) or the rate rounds to
nothing.

**Versioned record.** One NVS blob — namespace `battstat`, key `rec` — holding

```
magic 'BST1' | version | size-as-written | awake/sleep/charge ms + deltas |
soc at last charge end | last wall-clock epoch | charging | pendingSleepDelta |
FNV-1a checksum
```

Any of magic / version / size / checksum mismatching DISCARDS the record instead
of reinterpreting it, so a future layout change cannot resurrect old bytes as
new fields. Writes are throttled to `PERSIST_MS` (5 min) plus one on every state
edge and on the way into sleep.

**Not verified on glass:** the page's typography/pagination, and whether the
charge edges fire at all given the suspect charging flag — which is exactly what
the morning `batt` capture is for.

---

## Phase 4 build evidence

```
build:   idf.py -B ../build-r90c build                        EXIT=0
errors:  grep -c 'error:' <build log>                          0
stamp:   'MetalioEinkPlus r97'                                 3 hits (unchanged this round)
```

Artifact strings in the SHIPPING binary (`build-r90c/crosspoint_metalio.bin`),
one per feature, `grep -a -c`:

| string | hits |
|---|---|
| `Boot key tap` | 1 |
| `Boot key long press` | 1 |
| `sleep overlay paint: own from-white clean drive` | 1 |
| `Battery statistics` | 1 |

Frozen artifact (rebuilt from the committed tree):

```
size:    6,647,392 bytes
path:    M:/tmp/r97-p4-shipping.bin
sha256:  2ae180c5074c74e66669e0d0ffe29bfeb20247fb97edf1fd92311d0b9dac7fd5
```

BATT1's `batt` readout and the efuse gate line (`SECURE BOOT/FLASH-ENC`) are
still present, and the bench-only strings still count the same 2 hits as the
Phase 2 shipping image — i.e. the bench commands remain compiled OUT
(`bench: not built in this configuration`).

## Phase 4 — what needs the operator's eyes

1. **Boot key, tap only**: single tap toggles the frontlight; a 300 ms hold
   does NOT toggle it on the way down (that is the release-edge trap).
2. **Boot key, long**: hold ~700 ms opens KOReader sync in the reader; in no
   other screen does it do anything (there is no book to sync).
3. **Boot key rows** in Settings > Reading show the two defaults and the
   picker shows the "cannot wake" description.
4. **Custom overlay sleep**: paint a transparent overlay over a page with heavy
   black text — no ghost of the page under the overlay's white areas, and
   `logtail` shows exactly one `sleep overlay paint: own from-white clean drive`.
5. **Battery statistics**: after some hours, does "Awake/Sleeping since last
   charge" grow in the right proportion, and does plugging in create a
   "During last charge" block at all?

---

## Phase 4b — four self-contained items (RTC1, DRIVEUI1, NAME1, MAP1)

None of these touches the refresh policy, the render path, the power-off rail
sequence or the version stamp. Order: RTC1, DRIVEUI1, NAME1, MAP1.

### RTC1 — the clock says when it cannot be trusted

**The gap.** `lib/hal/HalClock.cpp` had no reference to the PCF8563's `VL`
(voltage-low) flag, and the SDK driver refuses a read with `VL` set
(`freeink-sdk/libs/hardware/Rtc/src/Rtc.cpp:150`:
`if (raw[0] & PCF8563_VL_FLAG) return false;`). That refusal is right, but it
collapses **"the chip is present and its calendar is a lie"** and **"there is no
chip"** into the same silent `false`: at boot with a dead backup supply,
`main.cpp` logged nothing at all and the clock simply did not seed.

**What it does now.**

| where | what |
|---|---|
| `HalClock::readVoltageLow()` (`HalClock.cpp:40`) | direct `Wire` read of register `0x02` bit 7, the same transaction the SDK uses |
| `HalClock::begin()` (`HalClock.cpp:30-38`) | one read before anything asks for a time |
| `HalClock::localTime()` poll (`HalClock.cpp:181-184`) | **every** clock read, before the time itself, so a 1→0 clear (the chip clears `VL` when a valid time is written) is seen too |
| `HalClock::noteVoltageLow()` (`HalClock.cpp:56`) | one log line per transition: `vl=1` is `LOG_ERR`, `vl=0` after a set is `LOG_INF` |
| `main.cpp:2875-2892` | the boot line, `[CLK] RTC read: YYYY-MM-DD HH:MM vl=0/1` |

`VL` is chip-specific, so the read is gated on `BoardConfig::RtcType::Pcf8563`;
on any other chip type the accessors report "not known" and nothing changes.
The RTC **write** path and the power-off rail sequence are untouched.

**Decision when `vl=1`: keep the calendar out of the clock, put the persisted
TrustedTime floor in charge — and say so.** Three candidates were on the table:

* *present it but flag it in the log* — rejected: the time is then still fed to
  `trustedtime::adopt()`, which persists it as the monotonic floor, so a stale
  date would become the lowest time the device ever trusts again
  (`TrustedTime` only ever raises the floor). A warning line does not undo a
  persisted lie.
* *refuse to apply it* — this is what shipped, and it is the *least* surprising
  behaviour because `lib/TrustedTime` is already built for exactly this case: it
  keeps a persisted floor in NVS, `init()` restores it when the RTC is behind it,
  and `trustedNow()` returns 0 when nothing is trustworthy (its callers fail
  closed). Skipping `adopt()` therefore does not leave the device without a
  clock, it leaves it on the floor it already had.
* *fall back to the floor* is the same statement as the previous bullet; the
  code makes it explicit: `main.cpp:2879` adopts only when
  `rtcHasTime && !rtcVl`, and the untrusted case logs
  `RTC read: … vl=1 - backup supply lost, clock NOT trusted; not adopted`.

So: **vl=1 ⇒ the calendar is not adopted, the persisted floor stands, the next
Wi-Fi sync overwrites both the system clock and the chip's calendar (clearing
`VL`), and a `LOG_ERR` line names the state.** `vl=0` on a later read logs the
clear, which is the evidence that the sync actually fixed it.

**Not verified on glass:** whether this board's backup supply is exhausted often
enough to see `vl=1` in the field. `logtail` after a long power-off settles it —
either the flag appears or the backup supply is fine. The RTC2 row (keep the
clock alive across power-off) stays filed and blocked on the schematic.

### DRIVEUI1 — the drive screen reads like the rest of the firmware

Paint only. `startSession()`, the R6 session-clearing contract, the journal, the
gadget handover and the exit-by-reboot path are unchanged; only
`buildDriveScreen()` (`UsbMscActivity.cpp:353-470`) was restyled.

Layout, top to bottom: **header band** (`GUI.drawHeader`, unchanged) → **state
stamp** → **phase in the title face** → **explanation in the body face** →
**eject-instruction band, live sessions only** → **EXIT/START button** (bottom,
unchanged).

The stamp is the item's real content, and it is driven by the *same* predicate
the session uses (`cardFree = !sessionStarted`, `:373`):

| state | stamp | treatment |
|---|---|---|
| nothing exported (neutral) | `SAFE TO UNPLUG` | filled black, white ink |
| session started / failed | `CARD IN USE - DO NOT UNPLUG` | outlined black |

The eject band (`kUnplugWarning`, unchanged string) is now painted only while a
session is live. Before this commit the screen told the operator not to touch
the cable *in every phase*, including the neutral one where nothing was
exported — that contradiction is what made the screen read as provisional.
Typography comes from `screen.theme().titleText/bodyText/smallText`, i.e. the
r68–r70 scale through `uiThemeTokens()`, instead of one small face for four
lines.

### NAME1 — the kosync device name is a settings field

`DEVICE_NAME` was a `constexpr` in the sync client, so every unit advertised
`MetalioEinkPlus`.

| piece | where |
|---|---|
| storage | `KOReaderCredentialStore::deviceName`, JSON key **`deviceName`** (`KOReaderCredentialStore.cpp:26` write, `:51` read) |
| on-device edit | Settings > CrossPoint Sync > **Device name** (`KOReaderSettingsActivity.cpp:91-106`), using the **existing** `KeyboardEntryActivity` that username/password/server-URL already use |
| row value | the stored name, or `Default: MetalioEinkPlus` when empty (`:197-203`) |
| web API | same key, `koDeviceName` (`SettingsList.cpp:289-296`), category `STR_KOREADER_SYNC` → web-only, no dead on-device row |
| **consumed at** | `KOReaderSyncClient.cpp:289` — `doc["device"] = advertisedDeviceName()`, the `device` field of every `PUT /syncs/progress` payload |
| fallback | `KOReaderSyncClient::DEFAULT_DEVICE_NAME` (`KOReaderSyncClient.h:79`) when the field is empty |

No config-version bump: a missing `deviceName` key and an empty value are the
same state (the default), so an upgraded config is not rewritten. The setter
trims surrounding whitespace and caps at `MAX_DEVICE_NAME_CHARS = 40`
(`KOReaderCredentialStore.h:95`) — the name is displayed in another client's
dialog and travels as a JSON string.

`STR_DEVICE_NAME` was added to `english.yaml` only; every other language falls
back to the English text, visibly.

### MAP1 — the non-list screen/transition map

Enumerated for refresh-mode tuning. "Not a list" means: not
`UiListActivity`/`UiTabListActivity` chrome. Every row names what the call site
**requests**, what the policy **resolves** it to, whether a **clean** is issued,
and how many whole-panel **drives** the transition performs.

**How to read the columns.**

* *resolves*: `GfxRenderer::displayBuffer()` runs the producer
  (`GfxRenderer.cpp:1963`) then the policy (`:1993-2013`), whose rules are
  `display/RefreshPolicy.h:272-296`: clean panel → HALF (or FULL if asked),
  whole-frame request → HALF/FULL, never a fast drive directly after a FULL.
* *drives*: `1` = one GC16/FULL-family drive; `1–2` = one DU/GL16 drive
  normally, **two** when the resolved mode is FAST, because pipeline row 9
  appends a GL16 completion drive (`GfxRenderer.cpp:2078-2100`). The completion
  is skipped when the previous paint was already a from-white clean
  (`:2080-2083`), which is the r96.2 wake fix. `2–3` = the grayscale paths,
  which add a base and two plane passes.
* *clean*: a from-white FULL was **armed** (`forceCleanNextRefresh()`) or
  requested explicitly for this transition. "—" means no clean of any kind.
  A HALF is a diff-area GC16, not a clean of the whole panel.

| # | screen / transition | paints at | requests | resolves to | clean | drives |
|---|---|---|---|---|---|---|
| 1 | Boot splash | `BootActivity.cpp:20` | FAST | HALF (panel is clean by latch) | — (panel already clean) | 1 |
| 2 | cold boot / resume → Home, first frame | `HomeActivity.cpp:591`, `:679` | FULL when `!firstRenderDone && cleanInitialRefresh` | FULL | yes (explicit FULL) | 1 |
| 3 | Home → reader (book open) | `EpubReaderActivity.cpp` first paint via `ReaderUtils.h:203` | GL16 (page-turn mode) | GL16 | — | 1 |
| 4 | reader text page turn | `ReaderUtils.h:203-224` | GL16 | GL16 | — | 1 |
| 5 | reader periodic full clean | `ReaderUtils.h:215-216` | FULL | FULL | yes | 1 |
| 6 | reader periodic quick clean | `ReaderUtils.h:217-218` | HALF | HALF | diff-area only | 1 |
| 7 | reader **image-page arrival** | arm `EpubReaderActivity.cpp:2236`, paint `:2248` | FULL (forced) | FULL | yes (r96 arrival clean) | 1 (+2 if gray) |
| 8 | reader image-placeholder pass (decode) | `EpubReaderActivity.cpp:2169` | FAST | FAST | — | 1–2 |
| 9 | reader grayscale base + planes | `EpubReaderActivity.cpp:2288`, `:2295` | HALF / FAST base, then planes | HALF / FAST | — | 1–2 |
| 10 | reader overlay pushed (menu bar, progress, toolbar) | `EpubReaderActivity.cpp:2760-2763` | FAST | FAST | — | 1–2 |
| 11 | reader overlay dismissed → page repaint | `EpubReaderActivity.cpp:2863` | FAST | FAST | — | 1–2 |
| 12 | reader menu action over the page (frontlight) | `EpubReaderActivity.cpp:3349` | FAST | FAST | — | 1–2 |
| 13 | reader manual refresh | `ReaderActivity.cpp:287` | FULL (promoted) | FULL | yes | 1 |
| 14 | reader end-of-book page | `ReaderActivity.cpp:268` | FAST (default) | FAST, or HALF after a FULL | — | 1–2 |
| 15 | reader empty / out-of-bounds / page-load error frame | `EpubReaderActivity.cpp:1879`, `:1890`, `:1923` | FAST (default) | FAST, or HALF after a FULL | — | 1–2 |
| 16 | reader → Home (stack empty / popped) | `ActivityManager.cpp:233` arm, `:235` log | FULL (forced) | FULL | yes | 1 |
| 17 | any non-Home/Sleep/Boot screen → Home | `ActivityManager.cpp:508-514` arm | FULL (forced) | FULL | yes (r93 one-shot) | 1 |
| 18 | pop → parent screen (stack intact) | `ActivityManager.cpp:241-260` | `activityTransitionRefresh` promotion | setting-dependent | — | 1–2 |
| 19 | push → any screen | `ActivityManager.cpp:534` + the screen's own paint | `activityTransitionRefresh` promotion | setting-dependent | — | 1–2 |
| 20 | wake → Home / reader | `main.cpp:1483-1494` | FULL (promoted **and** forced) | FULL | yes (r96.2 one-drive) | 1 |
| 21 | sleep overlay (default, bitmap, transparent-custom) | `SleepActivity.cpp:412-413`, `:754`, `:790`, `:836`, `:924`, `:1079-1081`, `:1092`, `:1100` | FULL + forced | FULL | yes (r97 C2 own drive) | 1 (2–3 on gray paths, `:421-422`, `:812-819`, `:933-934`) |
| 22 | Wi-Fi state change (connecting / connected / failed / save prompt) | `WifiSelectionActivity.cpp:981` promote, `:988` paint | FULL | FULL | yes (explicit FULL) | 1 |
| 23 | Wi-Fi finishing → parent | `WifiSelectionActivity.cpp:1207-1211` | FULL armed for the **incoming** frame | FULL | yes | 1 |
| 24 | keyboard entry | `KeyboardEntryActivity.cpp:1021` | FAST (default) | FAST or HALF | — | 1–2 |
| 25 | confirmation dialog | `ConfirmationActivity.cpp:42` | FAST | FAST or HALF | — | 1–2 |
| 26 | frontlight panel open / drag | `FrontlightPanelActivity.cpp:207` / `:490` | FULL / FAST or HALF | FULL / FAST or HALF | yes (open) / — (drag) | 1 / 1–2 |
| 27 | USB drive mode screen | `UsbMscActivity.cpp:329` | FAST (default) | FAST or HALF | — | 1–2 |
| 28 | clock sync (syncing → result) | `ClockSyncActivity.cpp:181` / `:176` | FAST / FULL (armed at entry) | FAST / FULL | yes on the result frame | 1–2 + 1 |
| 29 | power off | `PowerOffActivity.cpp:27` / `:87` | FULL / HALF | FULL / HALF | yes | 1 + 1 |
| 30 | other non-list screens: web server, Calibre, QR, OTA, SD update, crash, details, interval, dictionary (2), footnote, clip selection, BmpViewer | `CrossPointWebServerActivity.cpp:485`, `CalibreConnectActivity.cpp:236`, `QrDisplayActivity.cpp:46`, `OtaUpdateActivity.cpp:172`, `SdFirmwareUpdateActivity.cpp:267`, `CrashActivity.cpp:63`, `DetailsActivity.cpp:244`, `IntervalSelectionActivity.cpp:212`, `DictionaryDefinitionActivity.cpp:721`, `DictionaryWordSelectActivity.cpp:155`, `EpubReaderFootnoteSelectActivity.cpp:137`, `ClipSelectionActivity.cpp:793`, `BmpViewerActivity.cpp:131-300` | FAST (default) | FAST or HALF | — | 1–2 |

**What the table says outright.**

* **18 of the 30 transitions issue no clean at all** (rows 1, 3, 4, 8, 9, 10,
  11, 12, 14, 15, 18, 19, 24, 25, 26 drag, 27, 28 syncing, 30). Nine of those
  are the *reader's* own interactive path and are deliberate (a clean per tap
  would be an 815 ms flash); the rest are ordinary screen chrome.
* **15 of them can pay two whole-panel drives** — every paint that resolves to
  FAST. That is by design (row 9 finishes the fast transition) and it is the
  single biggest cost in this table: ~2× the flash for any UI repaint that is
  not preceded by a FULL.
* **Three can reach three drives**: the image-page arrival and the two
  grayscale sleep paths, where a base plus two plane passes follow the clean.
* **Only Home arrival, the wake, the sleep overlay, the image-page arrival and
  the Wi-Fi state frames clean on arrival** — the set r93/r96/r96.2 built.

**Logging added so this can be confirmed on device** (no behaviour change):

```
[MAP] enter <To> from <From> action=push|replace|pop cleanArmed=0|1
[GFX] row9 completion drive appended (mode=3 ms=..): second whole-panel drive for this transition
```

`[MAP]` is emitted once per transition from `ActivityManager.cpp:235` (pop to
Home), `:259` (pop to a parent) and `:309` (push/replace), **after** `onEnter()`
or the result handler, so an arm made there is what the arriving paint consumes.
`cleanArmed` is `GfxRenderer::forcedCleanArmed()` (`GfxRenderer.cpp:1930`), a
read-only view of the r93 one-shot. The completion line pairs with the existing
`row9 completion skipped` (`GfxRenderer.cpp:2080`), so counting
`enter … / paint … / row9 …` in `debug.log` settles the *drives* column for any
row without another build.

**UNKNOWN by reading, and the line that settles it**: the `resolves to` column of
rows 18–19 depends on the user's *Activity transition* setting
(`SETTINGS.activityTransitionRefresh`) and on the latch value at that moment —
`[GFX] paintdec req=… promo=… valid=… prev=… end=…` prints every input, so one
pass over `logtail` fills them in. Nothing else in the table is UNKNOWN: every
other row's mode is written at the call site.

## Phase 4b build evidence

```
build:   idf.py -B ../build-r90c build                        EXIT=0
errors:  grep -c 'error:' <build log>                          0
stamp:   'MetalioEinkPlus r97'                                 3 hits (unchanged: no version bump)
         'r970'                                                0 hits
```

Artifact strings in the shipping binary, one per item (`grep -a -c`):

| string | hits |
|---|---|
| `vl=1` (RTC1 clock format string) | 3 |
| `RTC read: %04d-%02d-%02d %02d:%02d vl` | 2 |
| `voltage-low flag set` | 1 |
| `SAFE TO UNPLUG` (DRIVEUI1) | 1 |
| `CARD IN USE - DO NOT UNPLUG` | 1 |
| `Device name` (NAME1 settings row) | 1 |
| `deviceName` / `koDeviceName` (keys) | 1 / 1 |
| `enter %s from %s action=%s cleanArmed=%d` (MAP1) | 1 |
| `row9 completion drive appended` | 1 |

Still present, unchanged: `batt` (12), `SECURE BOOT` (efuse gate, 1),
`Boot key tap` (1), `Battery statistics` (1),
`sleep overlay paint: own from-white clean drive` (1).
Still absent: `bench render` (0 — bench commands remain compiled out).

Frozen artifact (rebuilt from the committed tree):

```
size:    6,650,912 bytes
path:    M:/tmp/r97-p4b-shipping.bin
sha256:  f97e4b3582845716d237f0b317bb91ea75e075ece80306d562cffc853193445a
```

## Phase 4b — what needs the operator's eyes

1. **Drive screen, neutral**: it opens on a filled black `SAFE TO UNPLUG` stamp,
   the phase in the large face, and the START button; no eject warning band.
2. **Drive screen, live**: after START the stamp becomes an outlined
   `CARD IN USE - DO NOT UNPLUG` and the eject band appears below the status.
3. **Device name**: Settings > CrossPoint Sync > Device name opens the normal
   keyboard prefilled with the stored name; type one, save, and another client's
   "Progress pulled from …" shows it. Clearing the field returns the row to
   `Default: MetalioEinkPlus`.
4. **Clock**: `logtail` after a long power-off should show
   `[CLK] RTC read: … vl=` with a value. `vl=1` must be accompanied by the
   `NOT trusted; not adopted` error and no `[CLK] RTC set` line until a Wi-Fi
   sync.
5. **MAP1 spot checks**: one `[MAP] enter` per screen change, and a
   `row9 completion drive appended` after every `paint … applied=2`.
