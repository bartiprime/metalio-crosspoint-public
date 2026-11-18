# QA1 — the r101.9 batch audited as ONE change

`git log r101.8-verified..HEAD` (13 commits with code, tag `r101.8-verified` = `424a1740`, code tip
`815ffe55`; HEAD has since advanced to `75aed357`, a docs-only commit — `docs/engineering/IMG2_HOST_PROOF.md`,
+149 lines, no source change, so every finding below applies to the current tip).
Read-only audit: no source edits, no build, no flash. Every claim below is a citation into the tree
at HEAD; where a seam is clean it says so in one line and stops.

**Verdict: 0 × P0, 3 × P1, 9 × P2.** **All three P1s are FIXED in `8154b5fe` (item `QA2`); the nine
P2s are documented and deferred.** This report is otherwise left as written at `75aed357`, which means
the `file:line` digits in the P1 rows describe the tree *before* the fixes; the "fixed at" lines below
are the post-fix ones. Re-verified at `8154b5fe` during DOCS1 (see the citation note at the end). Nothing in the batch can put a wrong frame on the glass by
default, corrupt a settings file, resume a journal into the wrong index, or format-string its way
into undefined behaviour on the shipped path. The P1s are all inside opt-in or new-UI surfaces.

| id | grade | file:line | what is wrong | suggested fix | item |
|---|---|---|---|---|---|
| Q1-1 | **P1 — FIXED in `8154b5fe`, now `SleepActivity.cpp:619` (plain `HALF_REFRESH`)** | `src/activities/boot_sleep/SleepActivity.cpp:616` | `DeferredPopupDrive`'s fallback drive is `equalizeBlackMode(HALF)`. `EQUALIZE_REFRESH` is deliberately outside the panel's from-white promotion (`idf/components/metalio_driver/metalio_vendor_panel.cpp:681-682`: `gc16Class = FULL || HALF`), so on the one sleep path that leaves the popup as the **final frame of the session** a frame carrying ≥5 optical levels no longer gets `epd_hl_update_screen_from_white` — it gets a diff-area drive with the equalize LUT, and nothing can repaint it until wake. This is the one place EQ1 changes what was previously driven, and the header says so (`EqualizeBlack.h:33-38`) but the consequence (night-long residual) is not weighed there. | Leave this call site on plain `HALF_REFRESH` (EQ1's stated scope is "popups/dialogs over a screen that stays"), or pass `FULL_REFRESH` here, since this frame is held for hours and has no repaint. | EQ1 |
| Q1-2 | **P1 — FIXED in `8154b5fe`: one `HALF` per repaint (`LibraryIndexActivity.cpp:348`), `commit=false` draw at `:329`, first-paint `HALF` at `:305` and `:405`** | `src/activities/settings/LibraryIndexActivity.cpp:322` +`:340` | Every progress repaint pays **two** whole-panel drives: `GUI.drawPopup(..., commit=true)` commits a HALF at `BaseTheme.cpp:841`, then `fillPopupProgress()` commits again as FAST (`BaseTheme.cpp:872`). At `PROGRESS_REPAINT_MS = 500` that is two whole-panel drives per repaint for the whole walk (each drives the full 480x800 panel, so the paint cadence is in practice set by the panel, not by the 500 ms constant) — the screen the item exists to make cheap is the most expensive one in the tree. The count line and the Cancel button are even drawn *between* the two drives, so they cannot land in the first. Same defect class at `:283-305` (`paintDiff`) and `:351-405` (`render`): a cleared-canvas, whole-screen replacement is driven with the bare `displayBuffer()` default, which is `FAST_REFRESH` (`GfxRenderer.h:304`), not the HALF a first paint of a replacement frame wants. | Draw the popup with `commit=false` and let the single `fillPopupProgress()` drive carry caption+count+cancel (`paintDiff` already uses this shape); use `HALF_REFRESH` for the first paint of a cleared canvas, FAST only for the in-place progress updates. | LIB1-L1 / LIB1-L2 |
| Q1-3 | **P1 — FIXED in `8154b5fe`: helper `i18nFormatUnsigned()` at `lib/I18n/I18n.h:55` / `I18n.cpp:64`, all 11 sites converted** | `src/activities/library/LibraryListActivity.cpp:151-159`; `src/activities/settings/LibraryIndexActivity.cpp:71,74,284,311,314,317,377,386` | All 11 `snprintf(buf, n, tr(STR_*_FMT), …)` call sites in the tree were new in this batch: a **translated** string is used as the printf *format*. A translator writing `100%` or `% Complete` yields a `%` with no verb → undefined behaviour (missing variadic argument read as a pointer). Secondary: the args are `static_cast<unsigned>` while the formats say `%d` (benign on this ABI, still the wrong verb). | Format the numbers with a fixed `%u`-only literal and wrap via a helper (or keep `_FMT` strings out of `tr()` and localise only the surrounding words); at minimum have `scripts/gen_i18n.py` reject a `_FMT` value whose verb set is not the English one. | LIB1-L1 / LIB1-L2 |
| Q1-4 | P2 | `lib/LibraryIndex/LibraryBuilder.cpp:352,365` | `bool changed` is assigned (`changed = !reuse;`) and never read — the diff counts through `st.changed++` at `:395`. Dead local that reads as if it were the authority. | Delete the variable; keep `st.changed++` in the `else if (priorIndex >= 0)` branch. | LIB1-L2 |
| Q1-5 | P2 | `src/platform/UsbMscGadget.cpp:213` vs `:220` | The two lines contradict each other: `teardown()`'s (new) return value prints *"failed-start teardown incomplete"*, immediately followed by the new line *"MSC storage handle cleared, retry starts clean"*. After USB1 the teardown *is* complete — the only failure left is `delete_storage` orphaning the object. | Word `:213` as *"teardown finished, delete_storage failed: %s"*; keep `:220` as the proof line. | USB1 |
| Q1-6 | P2 | `docs/engineering/DIALOG_REGISTER.md` (canonical copy in `crosspoint-fork/docs/engineering/`), rows A* and C1/C2 | The register's `file:line` citations were not re-verified after this batch moved the lines. Sampled 20 citations: **6 resolve to the claimed statement, 14 do not**. Systematic causes: EQ1 inserted 3 comment lines above `BaseTheme.cpp:841`, so the 31 places that cite "`BaseTheme.cpp:837`" (the popup drive) now point at `cancelPromotedRefresh()`; `OptionPopup.h:164` likewise now points at the parked-refresh line, not the drive (it is `:167`); POSCR1 inserted ~230 lines at the top of `PowerOffActivity.cpp`, so C1's `:42-49` and C2's `:16-30` are inside a comment block. WAKE3's own doc is off too (`WAKE3_RESUME_DRIVES.md:24` cites `main.cpp:2049-2055` for `g_wakeDriveN=0`, which is now a comment two lines below the real block). | Regenerate the line numbers from the tip (a `file:line → first line of the cited statement` check in `scripts/audit_all.py` would make this mechanical), or cite anchors (`BaseTheme::drawPopup`'s `displayBuffer(equalizeBlackMode(` call) instead of line numbers. | DLG2 + regressions from EQ1/POSCR1/LIB1 |
| Q1-7 | P2 | `src/activities/boot_sleep/SleepActivity.cpp:516` | `"sleep image set: %u image(s), mixed pass (no BMP present)"` is logged at INF for the **overlay** dirs too. The overlay path enters `countImages(AnySupported)` directly (`recentKind != Standard` never runs the BMP pass), so the parenthetical claim "no BMP present" is unverified there and can be false while BMPs are exactly what was counted. One line per sleep, so not spam — just a log that lies. | Gate the "(no BMP present)" wording on `recentKind == SleepRecentKind::Standard`, or use a neutral string for the overlay path. | SLEEP1 |
| Q1-8 | P2 | `src/activities/util/PowerOffActivity.cpp:228` (and `:234`, `:239`) | `POWER_OFF_DARK/LIGHT/BLANK/CUSTOM` are drawn without the inversion reset that the sibling sleep path performs (`SleepActivity.cpp:653` `display.setInverted(false)`); `ActivityManager.cpp:103` sets polarity from `SETTINGS.screenInverted` before the render. With Night Mode on, "Dark" ships a white panel, "Light" a black one, and a `/poweroff/` image comes out as a negative. Pre-existing for TEXT (C2), newly *user-visible as a colour choice* because POSCR1 named the modes. | `const bool wasInverted = display.isInverted(); display.setInverted(false);` around the frame, restored afterwards — the exact shape `FrontlightPanelActivity.cpp:192-208` already uses. | POSCR1 |
| Q1-9 | P2 | `test/` (none) | Nothing pins the four new settings keys: `grep -rn 'equalizeBlack\|powerOffScreen\|softKeyHaptic\|usbDriveExitDialog' test/` is empty. The migration is correct by construction (see seam 1) but a future reorder of `SettingsList` or a `clamp` regression would ship silently. | One host test that serialises a JSON blob written by an r101.8 build (no new keys) and asserts the four fields land on `TEXT/0/0/0`, then round-trips. | BUILD2-adjacent (batch as a whole) |
| Q1-10 | P2 | `src/SettingsList.cpp:209` + `src/CrossPointSettings.h:284` | `softKeyHaptic` is declared unconditionally but its SettingInfo row lives inside `#if FREEINK_CAP_HAPTIC`. On a board without the motor the field is never written or read, so a value carried in from another build is silently dropped on the next save. Harmless today (no such board sets it) but it is an asymmetric declaration. | Either move the field inside the same `#if` as the row (it is only read by `HapticFeedback.h`, which is likewise motor-only in practice), or keep it and note in the header that it is intentionally non-persistent off-Metalio. | HAPT3 |
| Q1-11 | P2 | `src/HapticFeedback.h:296,315` | `mn_haptic_pulse_exact()` is referenced unguarded, but declared only under `FREEINK_DEVICE_METALIO_EINK4(_PLUS)` (`:34-51`). On a non-Metalio pio env that is a compile error. **Not a new break**: the same file already calls `mn_haptic_pulse_level()` unguarded at `:151,153,155`, so those envs could not compile before this batch either. Recorded so the next reader does not discover it as a regression. | Wrap the expander branch in the same `#if FREEINK_DEVICE_METALIO_*` the declarations use (both functions, at once). | HAPT2/HAPT3 (pre-existing pattern) |
| Q1-12 | P2 | `src/activities/library/LibraryListActivity.cpp:41` | `constexpr int SERIES_TAB = 3;` is the batch's only new entry in `gen_ui_literals.py` (105 → 106). It is a tab index, not a pixel value, so the tool is over-reporting — but it is the only unscaled number the batch added and it deserves the same scrutiny as `SIDEPADDING=12` next to it. | Leave it (a tab ordinal is not a length); if the tool must stay silent, add it to the tool's allow-list with that reason. | LIB1-L2 |

## Seam verdicts

1. **Settings migration — clean.** Four keys, one row each, no collisions (85 keys in the table, zero
   duplicates; the four appear once: `SettingsList.cpp:52` `powerOffScreen`, `:83` `equalizeBlack`,
   `:209` `softKeyHaptic`, `:296` `usbDriveExitDialog`). Load path is the generic visitor, and a
   missing key resolves to the **struct-initialiser default**, not to zero:
   `src/CrossPointSettings.cpp:180-192` — `const uint8_t fieldDefault = s.*(info.valuePtr);` then
   `uint8_t v = doc[info.key] | fieldDefault;` (ArduinoJson's "value or default"), ENUM clamped to
   `enumLabels().size()` (= `POWER_OFF_SCREEN_MODE_COUNT` = 5, `TEXT`=0 first), TOGGLE clamped to 2.
   So an r101.8 file (no such keys) loads `powerOffScreen=TEXT, equalizeBlack=0, softKeyHaptic=0,
   usbDriveExitDialog=0`; a save round-trips because `toJson()` (`:68-89`) walks the same table by
   `valuePtr`. All 21 new `STR_*` ids referenced by the batch exist in `english.yaml`, and other
   locales fall back to English (`scripts/gen_i18n.py:232`).
2. **Cross-item interaction — one real hit (Q1-1), rest consistent.** The sleep frame proper is a
   from-white `FULL` and is untouched by EQ1 by design; only the fallback popup commit is converted,
   and that is where the from-white promotion is lost. DLG1's exit frame drives its own `HALF`
   directly (never through the helper), POSCR1's four modes drive `FULL` directly, and SLEEP1's
   popups go through `BaseTheme`/`OptionPopup`, both of which EQ1 converts on purpose. No path passes
   a mode the helper then replaces in a way that changes a *previously driven* frame other than Q1-1.
3. **Schema v3 vs the L1 journal — clean, both directions, no P0.** The index side:
   `LibraryFormat.h:230` rejects any `formatVersion != 3` (`UnknownFormatVersion`), and
   `validateHeaderStructure` also re-derives the whole layout (256-byte stride, three permutations,
   `selfSize`) at `:226-245`, so a v2 file cannot open by any route — `openForReconciliation()` uses
   the structural validator and then re-folds only when `foldVersion` differs
   (`:249-252`), which is the v3/v4-fold case, not the v2 case. The journal side:
   `LibraryPartial.cpp:162-169` requires magic, CRC, `PART_SCHEMA`, **`formatVersion`**,
   **`foldVersion`**, metadata mode, **`generation`**, **`baseGeneration`**, `cardId`, **`stride`**
   and the root path to all match, and discards (truncates to a fresh header) on any mismatch — so a
   v2 journal is never resumed into a v3 index, and a v3 journal cannot be promoted onto a v2 index
   (the v2 index will not open, `previous.isOpen()` is false, `baseGeneration` becomes 0 and the
   identity no longer matches → journal discarded). `PART_SCHEMA`/format/fold/stride are redundant by
   design, and that redundancy is what closes the seam.
4. **Draw-audit leftovers — none new.** POSCR1's fallback drives (`paintTextFrame` →
   `driveFrameAndWait`), DLG1's OFF path paints nothing *and says so* (pre-batch behaviour, gated row
   C6), LIB1's progress screen drives (twice — Q1-2), and the library defer banner goes through
   `optionPopup.show()` with `processRender()` called first at
   `LibraryListActivity.cpp:1193` — i.e. the invariant DLG2 wrote down is still honoured. `0
   dead-removed` still holds.
5. **Error paths — clean.** LOCK1: `Lock` is non-copyable/non-movable, the count cannot underflow
   (`~Lock` guards `> 0`), `valid` is always true, and the constructor releases `modeMutex` *before*
   calling `setPowerSaving()` (no recursive take). USB1: `s_hdl/s_dev` are cleared before the driver
   uninstall, and the double-teardown case is caught by `armed()` at the top. SLEEP1's resolver closes
   every `HalFile` explicitly and the rest are RAII (`HalFile::~HalFile` owns a `unique_ptr<Impl>`);
   the `.tmp` file is removed on every false return, and `pickRandomImageInDir`'s early `return true`
   leaves no live handle because the member is destroyed. POSCR1 closes `file` on all four exits.
   LIB1: `resetScan()` clears the frame stack (including `resumePos`, which would otherwise seek the
   root into the middle), `begin()` teardowns on `openStageFiles` failure, and the destructor
   re-teardowns (idempotent) for any phase that is not `Finished/Idle`.
6. **Log discipline — clean on the P0 axis.** Every added `LOG_*` in `src/`, `lib/`, `idf/`, `tools/`
   was re-parsed (verb count vs argument count, `%%`-aware): **0 mismatches**. The `[WAKE] drive`
   line's 8 verbs / 8 args (the diff makes it look like 5 purely because the first arguments are
   context lines). One new tag (`LIBPART`), used consistently in one file; no per-row or per-book INF
   was added (progress logging is per-repaint and gated). The one inaccurate message is Q1-7; the one
   contradictory pair is Q1-5.
7. **Dead code / leftovers — one dead local (Q1-4), one dead-in-practice field (Q1-10).** No unused
   include, no unreferenced helper, no unread `#define`: `statusBarBottomPad` is read in both
   `UITheme.cpp:346` and `AndroTheme.cpp:813`; `MN_*`/`speedLockFirstOwnerDepth_` are read;
   `EqualizeBlack.h`'s helper has 4 call sites and one definition.

## Mechanical

| check | number |
|---|---|
| New compiler warnings vs baseline (`build-check6.log` vs `build-check1.log`) | **+2**, both in `.pio/libdeps/.../Arduino-wolfSSL/src/user_settings.h:297` (`'NO_WOLFSSL_ESP32_CRYPT_RSA_PRI' redefined`). **0 ours.** The 8 baseline warnings (esp_sleep/esp_flash deprecations, one `volatile ++`) are unchanged. |
| App image size | baseline `build-r1018c.log` **0x65f4a0 = 6,681,760 B** → tip `build-check6.log` **0x667840 = 6,715,456 B** = **+33,696 B (+0.50 %)**. (An intermediate r101.9 build, `build-r1019c.log`, was 0x65f510 — level with baseline — so the whole growth arrived with the later items, HAPT2/3 → LIB1-L2; this audit did not build the intermediate points, so the split between them is not measured.) |
| Static RAM delta | **not measurable from the available logs** — the IDF config here prints only the partition size check (`crosspoint_metalio.bin binary size … 54% free`); no `idf.py size` summary was captured for either build, and I did not build. Unverified, not "zero". |
| `gen_ui_literals.py --print` | **105 → 106 literals / 36 files** (diffed against an r101.8 worktree). The one addition is `activities/library/LibraryListActivity.cpp:41 SERIES_TAB=3` (a tab ordinal, not a length) — see Q1-12. No new pixel literal. |
| `grep -rn 'was=' src/` | **0 code occurrences**; 3 comment lines in `src/system/ChargerCurrent.{h,cpp}` that explain why it is gone. CHG2 holds. |
| `MODE_EQUALIZE_BLACK` outside the producer | **0 code occurrences** in `src/` + `lib/`; 7 comment mentions (`ReaderUtils.h:214`, `CrossPointSettings.h:337`, `EqualizeBlack.h:9,22,42`, `SettingsList.cpp:78`, `HalDisplay.h:30`). The single producer remains `HalDisplay.cpp:110`. EQ1 holds. |
| Doc citation sample (20 of 169 in `DIALOG_REGISTER.md` + `WAKE3_RESUME_DRIVES.md`) | **6 / 20 resolve to the claimed statement**, **14 / 20 do not** (Q1-6). Every cited file exists and every line number is in range — the drift is content, not 404s. |

## Notes

* `docs/engineering/DIALOG_REGISTER.md` (this repo) states that this directory is deprecated and the
  canonical docs live in `crosspoint-fork/docs/engineering/`. This file is written here because the
  task named this path; the findings about the register refer to the **canonical** copy.
* Not checked, and worth a follow-up: the LIB1-L2 search/diff behaviour on a real 1,200-book card
  (series ordering with `seriesIndex` ties, `seriesOrderOffset` vs the `Standalone` run at the end),
  and the LOCK1 nested-take path under an actual sleep-entry paint on hardware.

## Citation note (DOCS1, `8154b5fe`)

Every citation in this file was re-checked against the tree at `8154b5fe` and the ones the P1 fixes
moved were re-pointed (see the "FIXED" annotations). Two are unresolvable by construction and are
left as written: the wolfSSL path in the warnings row is a `.pio/libdeps` third-party file that exists
only inside a build directory, and the `test/` row cites nothing on purpose (the audit found no
test). The nine P2 rows are deferred, not fixed — `Q1-4` through `Q1-12` are the list.
