# R96 round — audit tables and live findings

This file is the durable record the r96 commits point at (`docs/engineering/R96_ROUND.md` §6-A and
§6-D1). It did not exist in the tree when r96 was cut; r96.1 creates it and records the two r96.1
defect rounds plus the backlog rows they came from.

Everything below is a **file:line** claim against the tree plus, where stated, lines from the
device's own log (`/.crosspoint/debug.log`, read over the USB shell with `logtail`). Where a claim
needs the operator's eyes, it says so.

---

## r96.1 — D1: the image-page clean fired on the wrong edge

### Operator report

> "not fully fixed. navigating away from an image does the clean. navigating to a page with an
> image does no clean."

### Cause: the mechanism was compiled out

`src/activities/reader/EpubReaderActivity.cpp` used `#if CP_IMG_CLEAN_ON_ARRIVAL` but did **not**
include `src/platform/PlatformFeatures.h`, and `#if UNDEFINED` is 0 (C++ [cpp.cond]/1 — unlike
`#ifdef`, an undefined identifier in `#if` evaluates to 0, it is not an error). The whole arm block
compiled to nothing.

This is provable from the artifact that was on the glass, not just from the source:

```
$ grep -a -c 'forcing a from-white clean on arrival' M:/tmp/r96-final.bin
0
$ grep -a -o '.\{0,50\}image page.\{0,70\}' M:/tmp/r96-final.bin
Could not start absolute image page; displaying B/W
UC8279 image page: absolute quality waveform      <- the only two "image page" strings
```

The r96 build carries both other `[ERS]` image-page strings and not the arm's, so the block was not
in the image. The same grep on the r96.1 artifact returns 1.

### The actual paint order for an arriving image page (r96, this board)

Board: Metalio E-Ink 4 Plus, `BoardConfig::DisplayController::Ed047Tc2`. That matters: the
absolute-grayscale branch is gated on `display.getController() == Controller::UC8279`
(`EpubReaderActivity.cpp:2080-2084`), so `absoluteImageGrayscale == false`, and
`HalDisplay::grayscaleCapabilities()` returns `{}` for this controller
(`lib/hal/HalDisplay.cpp:150`), so `needsAnyGrayscale`, `tiledGrayscale`, `combinedGrayscaleBase`
and `overlapRefresh` are all false.

| step | site | mode | arrival? |
|---|---|---|---|
| 1 | `EpubReaderActivity.cpp:2129-2134` placeholder drive — **only when `hasImagesNeedingDecode()`** | `displayBuffer(FAST_REFRESH)` → DU, plus the row-9 GL16 completion (`GfxRenderer.cpp:2028`) | yes (placeholder frame) |
| 2 | `EpubReaderActivity.cpp:2165-2166` image branch, arm site (r96: compiled out) | — | — |
| 3 | `EpubReaderActivity.cpp:2168` image branch, the page's own drive | `cleanImageBasePending ? HALF_REFRESH : pageTurnRefreshMode()` → GL16 (`RefreshPolicy.h:291`, `CP_RICKY_TURN_MODE=1`) | **yes — the real arrival** |
| 4 | `EpubReaderActivity.cpp:2169` (r96 only) | sets `pagesUntilFullRefresh = 1` | departure, next page |
| 5 | `ReaderUtils.h:214-219` next page's `displayWithRefreshCycle` — `pagesUntilFullRefresh <= 1` | `FULL_REFRESH` → GC16-from-white | **departure clean — the wrong edge** |

Steps 4→5 are the clean the operator saw on leaving an image page. Log, two independent round
trips, spine 1 = a full-page 670x1072 jpeg (`[IMG] Rendering image at 7,7 ... img_1_0.jpg`):

```
[28145] ... Loading file: EPUB/front.xhtml, index: 1        <- the image page
[26041] [GFX] paintdec req=3 res=3 force=0 ... prev=1 end=3   <- arrival: GL16, force=0
[27023] [PANEL] show mode=GL16 tps=1 err=0x0 ms=809
[27028] [GFX] paint req=3 applied=3 full=0 clean=0 previous=1 force=0 ms=987
...
[30105] ... Loading file: EPUB/front.xhtml, index: 1
[30376] [GFX] paintdec req=3 res=3 force=0 ... prev=2 end=3   <- still GL16 on arrival
[31347] [PANEL] show mode=GL16 ... ms=800
[31352] [GFX] paint req=3 applied=3 full=0 ... force=0 ms=976
...
[39642] ... Loading file: EPUB/intro.xhtml, index: 2         <- the page AFTER it
[39703] [GFX] paintdec req=0 res=0 force=0 ... prev=1 end=0   <- FULL, from-white
[40690] [PANEL] show mode=GC16-from-white tps=1 err=0x0 ms=813
[40695] [GFX] paint req=0 applied=0 full=1 clean=0 ... force=0 ms=992
```

`force=0` on every paint in the whole log is the second half of the proof: with the arm compiled
in, the arrival paint is `force=1`, `end=0`.

### Fix

1. `EpubReaderActivity.cpp` now includes `../../platform/PlatformFeatures.h`. The arm block exists.
2. The image branch no longer leaves `pagesUntilFullRefresh = 1` behind when the arrival clean is
   available: it restarts `pagesUntilFullRefresh`/`pagesUntilQuickRefresh` from the settings, which
   is exactly what `displayWithRefreshCycle()` does after its own FULL (`ReaderUtils.h:225-234`).
   A departure from an image page is therefore a normal page-turn drive. With
   `CP_IMG_CLEAN_ON_ARRIVAL=0` the pre-r96 behaviour is kept verbatim.

### Which edge inside the arrival, and why

Two drives can exist on an arrival (step 1 + step 3) when `hasImagesNeedingDecode()` is true:

* **(a)** arm before the placeholder drive, so the placeholder frame is the clean;
* **(b)** leave the placeholder drive alone and let the second paint — the frame that actually
  contains the image — be the clean.

**(b) was chosen.** Under (a) the operator pays 815 ms to clean from white *onto a page of empty
placeholder boxes*, and the picture still arrives on a differential drive — the opposite of "the
image page arrives on clean glass". Under (b) the from-white drive sits underneath the image, which
is the frame being complained about. And on this device (b) usually costs nothing extra: the `.pxc`
image cache means `hasImagesNeedingDecode()` is already false on any later visit, and the log above
shows a **single** paint on the arrival (no placeholder drive), so the arm-then-display order has
nothing to sequence against.

### Guard and stale-flag discipline

* One clean per (spine, page): `cleanImageSpine_`/`cleanImagePage_` are written at the arm site,
  immediately before the one `displayBuffer()` that consumes the flag, so a repaint of the same
  page (status bar, clipping highlight, a menu over it) pays nothing.
* An arm cannot survive a page turn: `g_forceCleanPaint` is cleared by the very next paint
  (`GfxRenderer.cpp:1948-1949` blocking, `:2068-2069` async) **and** by a panel park
  (`cp_refresh_policy_note_park`, `GfxRenderer.cpp:1898-1904`), which is the same boundary the r93
  contract uses and which every power-down drive path reaches through
  `HalDisplay::deepSleep()` → `cp_refresh_policy_note_park()` (`lib/hal/HalDisplay.cpp:171-176`).

---

## r96.1 — D2: waking from sleep still bands

### Operator report

> "still feels weird when i wake from sleep. i get the banding mid-transition black screen flash,
> its supposed to be a full flash, full clean."

### Every drive on the resume path (r96)

`metalioStandbySleep()` resume, `src/main.cpp:1379-1421`:

| step | site | mode | drives? |
|---|---|---|---|
| 1 | `main.cpp:1381` `gpio.resumeAfterSleep()` | — | no |
| 2 | `main.cpp:1390` `epd_board_..._bus_restore()` | — | no (re-opens the parallel bus + `epd_lcd_reinit`) |
| 3 | `main.cpp:1391` `display.wake()` → `FreeInkDisplay.cpp:1196` → `Epd047Tc2Driver.cpp:1668` → `metalio_vendor_panel_begin()` | — | **no**: `metalio_vendor_panel.cpp:135` `if (g_begun) return true;`, and `g_begun` is never cleared for an in-place standby. Despite the driver's comment, the cold path's `epd_poweron()` + `epd_clear()` do **not** run on wake. |
| 4 | `main.cpp:1396` `metalio_vendor_panel_repower()` (TPS off/on) | — | no |
| 5 | `main.cpp:1394` `delay(KWAKE_SETTLE_MS)` = 250 ms (`main.cpp:1047`) | — | no |
| 6 | `main.cpp:1402` `metalioWakePrimeDrive()` — **`#if CP_RICKY_WAKE_PRIME`** | `displayBuffer(FULL_REFRESH)` | **not compiled**: `RefreshPolicy.h:206` is `#define CP_RICKY_WAKE_PRIME 0` |
| 7 | `main.cpp:1407` `promoteNextRefresh(FULL_REFRESH)` | — | no (arms step 8) |
| 8 | arriving activity's single `render()` → one `displayBuffer()` (`ActivityManager.cpp:96-116`) | resolved `FULL_REFRESH`: park cleared the latch (`HalDisplay::deepSleep` → `cp_refresh_policy_note_park`), row 5 gives clean precedence, and the promotion asks for it | **yes — the only drive** |
| 9 | Home only: the `firstRenderDone` follow-up (`HomeActivity.cpp:600-610` grid / `:670-681` list) | `displayBuffer(FAST_REFRESH)` after `cancelPromotedRefresh()` → DU **plus** the row-9 `GL16_REFRESH` completion (`GfxRenderer.cpp:2028-2035`) | **yes, when the resume lands on Home** |

### What this round established, and what it changed

1. **r96's own note is wrong.** `6-A` says the wake is "a promoted FULL + wake prime". The prime is
   compiled out (`CP_RICKY_WAKE_PRIME=0`), so the wake has been a *single* drive, and the
   `goHome()` exclusion in `ActivityManager.cpp:461-464` ("already preceded by a promoted FULL +
   wake prime") justifies itself with a drive that does not exist. The exclusion is still correct —
   it is the *promotion* that must not be doubled — but the comment was fixed here rather than left
   as a false premise.
2. **The residual band is not an app-level second paint on the reader path.** Steps 1-8 issue one
   whole-panel drive. On the panel shim's own terms a band on a whole-panel drive is
   `EPD_DRAW_EMPTY_LINE_QUEUE` (0x400): `render.c:303` logs "line buffer underrun occurred!", and
   `metalio_vendor_panel.cpp:428-433` says it in the operator's own language — "err=0x400 here means
   the line queue underran and the drive never covered the panel, **which is what banded refresh
   looks like on glass**". The shim then recovers by `epd_clear_checked()` + a from-white retry
   (`:446-466`), which is itself a second visible drive. A starved first post-poweron frame is
   exactly the case the W22 measurement (`docs/R55_R57_WAKE.md`) says bands "whichever waveform it
   runs".
3. **Changed here:** `main.cpp:1407-1417` now arms `forceCleanNextRefresh()` *as well as* the
   promotion. The promotion is cancellable — `HomeActivity.cpp:608/674`, `ClockSyncActivity.cpp:27`,
   `DebugActivity.cpp:540`, `FontDownloadActivity.cpp:81` all call `cancelPromotedRefresh()` before
   their drive — and a cancelled promotion turns the wake paint into `pageTurnRefreshMode()`:
   GL16, whose erase/saturate phases are an inverted intermediate frame, i.e. a black flash
   mid-transition. The one-shot clean is a policy *term* (`GfxRenderer.cpp:1948`), not a promotion,
   so it cannot be cancelled; both are consumed by the same first `displayBuffer()`, so this cannot
   add a drive.
4. **Home's follow-up is a real third drive** (step 9) over the frame the clean just laid down. The
   DU is usually a no-op diff, but the row-9 completion drive runs unconditionally after a FAST
   paint (`GfxRenderer.cpp:2028`), so the panel takes a 48-phase GL16 over unchanged content —
   precisely a flash of nothing. It is **not** changed here: the follow-up is also what refreshes
   thumb paths and triggers `loadRecentCovers()`, so suppressing it can leave blank cover tiles
   until the next touch. That needs the operator's eyes before it is worth the risk.

### Left open on purpose (needs glass, not a guess)

The band on the reader path is a *panel* behaviour: the first whole-panel drive after
`epd_poweron()` starves the line queue. Options, all with a visible cost or a memory cost:

* **(A) Thicker line queue.** `render.c:441-445` sets `queue_len = max(queue_len, 128)` (~2-3 ms of
  buffering). A first frame that is tens of ms slow would need several times that in internal RAM,
  across `NUM_RENDER_THREADS` queues. Measure first.
* **(B) Re-enable the throwaway prime** (`CP_RICKY_WAKE_PRIME=1`): the band lands on the sleep
  frame, so the arriving frame is the second drive on warm rails — but the operator sees the sleep
  image flash *and* the page flash. Two flashes, one of them banded. Not "a full flash, full clean".
* **(C) Prime with a frame that cannot be seen changing.** Impossible on a bistable panel: a
  diff-driven prime of unchanged content is a no-op (`highlevel.c:228`, `diff_area.height == 0`), so
  no waveform runs and nothing is eaten; a from-white prime takes the glass to white first, so a
  starved row keeps the sleep image against a white field — the band again, just earlier.
* **(D) Remove the recovery's visible clear.** `metalio_vendor_panel.cpp:446-466` is the vendor's
  own EMPTY_LINE_QUEUE branch and predates this round; changing it changes what a *failed* drive
  leaves on glass everywhere, not just at wake.

Recommendation: (A) after a measurement, with the wake drive's `[panel] show mode=… err=0x… ms=…`
line captured from `/.crosspoint/sleep.txt` on a real wake. If that line reads `err=0x0` and the
band is still there, the band is the GC16 waveform itself and (B) is the only remaining lever.

---

## r96.1 — D3: `lpsleep` refused forever (duration used as an age)

`src/debug/UsbShell.cpp:954` stored the drive's **duration** (`~350-990 ms` on this panel) and
`LpCoreService.cpp:351-354` compared that duration to `1000` as if it were "ms since the drive
finished". Every ordinary drive is shorter than a second, so the guard refused every slice — and a
drive *slower* than a second would have let a slice start immediately, the opposite of the intent.

Fix: `notePanelDriveMs()` stamps the end time in the same call (`s_lastDriveAtMs`), and
`lastPanelDriveAgeMs()` returns `millis() - s_lastDriveAtMs` (unsigned subtraction, so the ~49.7 day
`millis()` rollover is correct) or `ULONG_MAX` when nothing has been driven this boot — "never" must
not read as "just now". `LpCoreService` compares the **age**.

Callers of the old accessor, checked one by one:

| caller | what it wants | verdict |
|---|---|---|
| `LpCoreService.cpp:351` | age | **was the bug**, now uses `lastPanelDriveAgeMs()` |
| `src/debug/PanelProbe.cpp:96` | duration, printed as `last_drive: %lu ms (entry->exit of the last blocking waveform)` | correct as written, left alone |
| `src/main.cpp:3075` | writes the duration from the poll hook | producer, correct |

---

## r96.2 — three defects from r96.1 on glass

Round scope: D1 (image-page clean fires on neither edge), D2 (wake still bands), D3 (`lpsleep 60`
hangs the device). No flash this round — the operator took the device to download mode, so every
claim below is a **code** claim plus the log strings that will prove it on the next boot. Where the
device was needed to close something, it says so.

### D1 — the arrival clean fired on neither edge

**What was on the glass was not what was in the tree.** `cb5871c0` is an ancestor of HEAD and
`EpubReaderActivity.cpp:72` does include `PlatformFeatures.h`, so the arm exists in the source. The
artifact the operator flashed (`M:/tmp/r96.1-final.bin`, sha256 `5eee9118…`, built 20:43) predates
that commit by seven minutes and has since been overwritten on disk — it cannot be grepped any more,
which is itself a process finding: **a firmware image must not be built before the commit it claims
to contain**. The rebuilt `r96.1-final.bin` (`a1a65808…`, 20:58) does contain the arm string.

**Why "neither direction" was still possible even with the arm compiled in.** r96.1 wrote the
`(spine, page)` guard *at the arm site* and then drove:

```
if (cleanImageSpine_ != spine || cleanImagePage_ != page) {
    renderer.forceCleanNextRefresh();
    cleanImageSpine_ = spine;      // committed BEFORE the paint
    cleanImagePage_ = page;
}
renderer.displayBuffer(...);        // may or may not take the flag
```

If that one paint did not consume the flag (a promotion resolved the request, the drive was skipped,
another paint ran in between), the pair was already recorded — so the arrival clean could never fire
for that page again, and the r96.1 change had simultaneously removed `pagesUntilFullRefresh = 1`
(the departure clean). That is exactly "neither direction cleans", and nothing in the code could
report it.

**Fix (`EpubReaderActivity.cpp:2160-2205`, `GfxRenderer.{h,cpp}`).** The arm records INTENT
(`cleanArmedSpine_/cleanArmedPage_/cleanArmCounter_`); the pair is committed **only if**
`renderer.forcedCleanPaintsConsumed()` moved across the drive. If it did not, the guard stays unset,
the next paint re-arms, and an `[ERS] cleanNOTconsumed` line says so. Three log points now bracket
the handoff:

```
[ERS] armImageClean spine=1 page=7 force=1 counter=0: forcing a from-white clean on arrival
[GFX] forced clean consumed by this paint (cleanPaints=1)
[ERS] cleanConsumed spine=1 page=7 force=1 counter=1
[GFX] paint req=.. applied=3 full=1 clean=.. previous=.. force=1 result=0x0 ...
```

`force=1 applied=3` (FULL) on the arrival paint, once per (spine, page), and `force=0` on the
departure paint is the whole acceptance test. **Not yet proven on glass.**

### D2 — waking still bands

**Established from code, since the device left before a wake drive line could be read.** r96.1 made
the wake paint an uncancellable from-white FULL, and the resume issues exactly one drive. But the
paint is not the only drive in the transition. Two more follow it *over the same content*:

| # | drive | file:line | why it exists |
|---|---|---|---|
| 0 | (nothing) | `metalio_vendor_panel.cpp:135` | `metalio_vendor_panel_begin()` early-returns on `g_begun`, so `epd_clear()` does not run |
| 1 | wake paint, from-white GC16 | `main.cpp:1417-1420` → `GfxRenderer.cpp:1955` | the clean |
| 2 | **Home first-render follow-up**, DU | `HomeActivity.cpp:604-615` (grid), `:684-695` (list) | repaint after thumbnails generate |
| 3 | **row-9 completion**, whole-panel GL16 | `GfxRenderer.cpp:2064-2078` | ends any `FAST_REFRESH` paint |

Drive 3 is unconditional after a FAST paint, so drive 2 buys a **second full-screen waveform on
pixels that have not changed**. On a bistable panel that is a visible band one second after the clean.

**Fix, behind `CP_WAKE_ONE_DRIVE` (default 1).** A paint that consumes a forced clean sets
`g_paintsSinceClean = 1`; the next paint then skips row 9 (`[GFX] row9 completion skipped: previous
paint was a from-white clean over the same content`), and Home suppresses its first-render follow-up
when the frame it just drew was that clean (`[HOME] wake clean on glass: first-render follow-up paint
suppressed…`). Covers load on the next natural repaint; nothing is left on the glass that needs a
second drive. `CP_WAKE_ONE_DRIVE=0` restores the r96.1 sequence verbatim.

**PENDING, and stated as pending:** the band's *panel-level* signature was never read this round. The
next wake capture must answer one question — inside `/.crosspoint/sleep.txt`, is the wake drive
`err=0x0` or `err=0x400`?

* `err=0x400` (`EPD_DRAW_EMPTY_LINE_QUEUE`, `render.c`) → the line queue starved and the recovery
  path at `metalio_vendor_panel.cpp:445-467` added the second visible drive. The queue is already 128
  lines (`idf/components/metalio_driver/display/render.c:440`), so the next step is 256 behind a
  measurement, not a guess.
* `err=0x0` → the GC16 itself bands and only a thicker queue or a changed waveform helps; that is a
  bigger decision and needs the numbers.

### D3 — `lpsleep 60` hung the device

**What happened.** Armed, answered, never came back; the shell was silent for >2 minutes; the only
recovery was closing the serial port (which resets the board). After the reboot `logtail 3000` had
**no `lpsleep` line at all** — not even the `armed` line.

**That last fact is explained, and it is a defect in its own right.** `/.crosspoint/debug.log` is not
a live log: `logSdFlush()` dumps a RAM ring on demand (`main.cpp:655-657`, `lib/Logging/Logging.cpp:104`).
Every `LOG_INF` in the slice path lived in RAM only, so a hang or an untidy reset loses all of it.

**What could hang it (prime suspect, not proven).** The r96.1 slice armed `EXT1 + timer`, started the
LP bench program with `cp_lp_bench_arm(1)` (`LpCoreService.cpp:177` → `ulp/main_bench.c:87-99` calls
`ulp_lp_core_wakeup_main_processor()` on an edge) and then slept with the LP core **live**. That is
the one configuration standby never uses: `metalioStandbyLoop()` (`main.cpp:1203-1330`) sleeps with
the LP core parked and has done so for hours on this device. A ULP wake request with no matching
enabled `ESP_SLEEP_WAKEUP_ULP` source is the only difference between the proven path and the one that
stranded the board.

**Fix — the command now cannot use an unproven sleep path, and cannot lose its own record.**

| change | evidence | file |
|---|---|---|
| LP program sampled, **STOPPED**, slice, sampled again | slice runs the exact configuration standby proves | `LpCoreService.cpp:405-420, 500-510` |
| every HP sleep call armed for **≤ 5 s** (`kHpSleepChunkMs`), timer re-armed per chunk, total = `sec` | **the bound: 5 s + settle**, not 60 s | `LpCoreService.cpp:430-470` |
| no timer ⇒ **no sleep** (abort before `esp_sleep_light_sleep`) | the only failure that cannot be recovered from is taken out of the code path | `LpCoreService.cpp:437-448` |
| `logSdFlush()` after the sample, after every arm, after every chunk wake, after the result | the arm record now reaches the card before the sleep | `LpCoreService.cpp:411, 445, 465, 517` |
| slice clamped to 5..60 s (was 5..120) | shorter exposure, same measurement | `LpCoreService.cpp:308-311`, `main.cpp:896-900` |
| `CP_LPSLEEP_ALLOW_HP_SLEEP=0` refuses with a stated reason | the hard off switch | `PlatformFeatures.h`, `LpCoreService.cpp:346-352` |

Honest limit: this target has no `rtc_wdt` API (`esp_hw_support/include/rtc_wdt.h` is
`#if CONFIG_IDF_TARGET_ESP32 || ESP32S2` only), so a *hardware* watchdog cannot be armed. The bound
is therefore structural — the same timer + EXT1 pair standby has been running for hours, with the LP
core out of the loop — plus the 5 s per-call cap. If the next run still does not return, `logtail 40`
now carries the phase it died in, and the switch above disables the command.

## r96.2 verification (offline — the device was in download mode)

```
build:   idf.py -B ../build-r90c build          EXIT=0
errors:  grep -c 'error:' <build log>           0
stamp:   'MetalioEinkPlus r96.2'                see freeze block
         'MetalioEinkPlus r96 ' / ' r96.1 '     0 hits (substring trap: exact-string counts)
```

## r96.3 — three fixes, after r96.2 was tested on glass

Operator, verbatim, on D1:

> "it only happens when you first open the book. then when you flip to page with image it cleans,
> but if i flip to text after the image page it doesnt clean. flipping back to image page also
> doesnt clean. then i exit out of book, go back to book and it cleans and same behaviour as
> mentioned."

### D1 — the clean fired once per BOOK, not on every arrival

**Cause, and it is the design, not a bug in it.** r96–r96.2 armed the arrival clean when
`(spine, page)` differed from a pair that meant *"the page I last cleaned"*
(`EpubReaderActivity.h:42-43`). Nothing released that pair when the reader LEFT the page, so for the
lifetime of the activity a page that had been cleaned once could never be cleaned again. Flipping
image → text → image cleaned exactly once; only a re-entry into the book (a NEW
`EpubReaderActivity`, members back to `-1`) cleaned again — which is the "then i exit out of book,
go back to book and it cleans" the operator described.

**Fix: model the arrival, not the history.** `displayedSpine_/displayedPage_` is now *the page that
is on the glass*, written in exactly one place — after the base drive, `EpubReaderActivity.cpp`
(`const auto tDisplay = millis();` + the `displayed spine=.. page=..` DEBUG line) — and shared by
every page kind, image or not. The image branch arms when the arriving page differs from that
record (`samePageOnGlass` / `armArrivalClean`, `EpubReaderActivity.cpp:2186-2190`). Therefore:

| event | record before | armed? | why |
|---|---|---|---|
| first page of a book, has images | `-1/-1` | **yes** | new activity ⇒ nothing displayed yet |
| image → text | image page | no | this page is text |
| text → image | text page | **yes** | the record moved to the text page |
| repaint of the page you are on (status bar, highlight, menu over it, idle prewarm) | same page | **no** | the record did not move |
| image → image (next page) | previous image page | **yes** | different page |
| leaving the book (to Home) | last page | no drive | a departure is not an arrival; Home is not a page, so the next entry is one |

The r96.2 retry is kept but scoped: if the arm was made and the paint did not consume the flag,
`displayedCleanPending_` keeps the SAME page outstanding, so the next paint of that page re-arms;
it is retired the moment the reader moves to another page (`displayedCleanPending_ && !(…)`). There
is no per-book guard left to get stuck.

### D2 — the wake band: the drive SUCCEEDS, so it is counted instead of argued about

Measured on the device this round, on the r96.2 image:

```
[panel] show mode=GC16-from-white tps=1 err=0x0 ms=765
```

`err=0` — not `0x400`. The line-queue-underrun theory is **refuted**: the drive reports that it
clocked the whole panel. r96.2 had already removed the row-9 completion drive and Home's follow-up,
so two possibilities remain and they need different fixes: a drive sequence that is still more than
one drive, or a single from-white GC16 that bands on rails that have just come back from SLEEP.

The resume now measures instead of asserting: `[WAKE] drive N mode=.. err=0x.. tps=.. ms=.. (+N ms
after resume)` for every drive inside a `CP_WAKE_CAPTURE_MS` (4000 ms) window opened at
"resume: one-drive clean armed", and a summary at the close:

```
[WAKE] resume drives=N window=4000 ms band=single-drive: rail settling or first-pass waveform
```

`drives=1` ⇒ rail settling / first-pass waveform. `drives>1` ⇒ something still drives after the
clean. The counting is fed by the panel shim's own `[panel] show …` line (parsed in
`panelJournalCStr`), so the [WAKE] numbers cannot disagree with the drive.

**The distinguishing experiment, behind a switch, default OFF:** `CP_WAKE_SETTLE_EXTRA_MS`
(`PlatformFeatures.h`, default 0) adds dead time on top of `KWAKE_SETTLE_MS` (250 ms) between
`metalio_vendor_panel_repower()` and the clean drive, and logs
`[WAKE] rail-settle test: extra N ms …`. It adds a delay, not a drive — no extra flash, and with the
switch at 0 the image is byte-for-byte the r96.2 sequence. 800–1500 ms is the suggested setting: if
the band goes, it is settling; if it does not, it is the waveform's first pass and no delay will help.

One more candidate was checked and eliminated on the way: `CP_RICKY_WAKE_PRIME` is 0
(`RefreshPolicy.h:206`), so no throwaway prime drive runs at resume.

### D3 — `lpsleep`: the cheap hypothesis is REFUTED, and the command is now refused by default

**Hypothesis tested first, as instructed: does `lpsleep` bypass the vendor handoff?** No.

| | site | entry call |
|---|---|---|
| standby (proven for hours) | `main.cpp:1280` (`metalioStandbyLoop`) | `metalio_sleep_light_sleep(0)` |
| `lpsleep` | `LpCoreService.cpp:476` (`pollSleepSlice`) | `metalio_sleep_light_sleep(0)` |

Both make the SAME call, and `metalio_sleep_light_sleep()`
(`idf/components/metalio_driver/sleep/metalio_sleep.c:216-233`) is the only place `s_restore_armed`
is set — the gate that lets `__wrap_rtc_clk_mpll_configure` re-apply the 12/12 bias and the MSPI
divider, and the only path into `__wrap_pmu_sleep_start`. There is exactly one
`esp_light_sleep_start()` call in the sleep shim (`metalio_sleep.c:221`) and it is inside that
wrapper. So the PSRAM/MPLL handoff is NOT skipped; no call site reaches the SDK directly.

**What the device actually did, twice, this round (r96.2 image, no flashing):**

| run | `lpsleep` answer | what followed | record on the card |
|---|---|---|---|
| A (a previous boot, already in `debug.log`) | armed | 12 × `chunk back slept=5000ms cause=timer … err=0`, then `lpsleep: pass sec=60 slept=60000ms asleep=60049471us cause=timer` | complete |
| B (`lpsleep 15`) | armed, answered | shell silent for >10 s, twice; `ver` never answered; only closing the port (which resets the board) brought it back | **none** — not the per-chunk `armed`, not `chunk back` |

Run B leaving nothing at all matters: the first flushed record in the sequence is the per-chunk
`armed` line, so the device can die BEFORE it — i.e. in `startBench()`/`stopBench()` or the INT-line
idle wait — not only inside sleep. The failure is therefore not deterministic, and "the USB link
dies across the slice" is a candidate for run A's silence, not a proven mechanism. Nothing in this
round proves a cause.

**What is proven is that the paths are not identical**, which is the re-enable condition: standby
calls `halClock.disableAlarm()`, `epd_board_metalio_eink4_plus_ioexp_mask_for_sleep()` and
`ioexp_enable_power_wake_pin()` so that ONLY the power key can assert INT
(`main.cpp:1209-1268`); this bench calls `releaseAccelIntLine()` and nothing else. Until that is
matched and a slice has been observed to leave the shell answering, the command is refused:

```
#ifndef CP_LPSLEEP_ALLOW_HP_SLEEP
#define CP_LPSLEEP_ALLOW_HP_SLEEP 0
#endif
```

Refusal text (`LpCoreService.cpp`, `requestSleepSlice`): *"the HP light-sleep slice is disabled in
this build (CP_LPSLEEP_ALLOW_HP_SLEEP=0): its entry path is standby's, but a slice has never left
the USB shell answering; use `lpbench` (LP core, HP awake)"*. The shell's help row says REFUSED.

## r96.3 verification

```
build:   idf.py -B ../build-r90c build          EXIT=0
errors:  grep -c 'error:' <build log>           0
stamp:   'MetalioEinkPlus r96.3'                3 hits
         bare 'r96.2' / bare 'r96'              0 hits each (standalone-string counts)
```

Artifact strings checked in the frozen image (the habit r96.1 added): `forcing a from-white clean on
arrival` ≥ 1, `armImageClean`, `cleanConsumed`, `resume drives=`, `[WAKE]`, `rail-settle test`.

## r96.1 verification (offline — the device was handed to the operator for a flash)

```
build:   idf.py -B ../build-r90c build          EXIT=0
errors:  grep -c 'error:' <build log>           0
stamp:   'MetalioEinkPlus r96.1'                 3 hits
         bare 'MetalioEinkPlus r96' (not .1)     0 hits   (grep -o 'MetalioEinkPlus r96[^.0-9]')
size:    6,693,216 bytes
frozen:  M:/tmp/r96.1-final.bin
sha256:  a1a65808ca04824167ad6d20333895a66f3c13addb0403b9901da506d3d5083d
         (rebuilt from the committed tree; the first build in this round differed only by
          __LINE__ shifts from comment edits, so the frozen image is the committed one)
```

Also verified in the artifact: `forcing a from-white clean on arrival` is present (1 hit), i.e. the
D1 arm is compiled in — the check that r96 failed.

## Backlog

| id | item | state |
|---|---|---|
| D1 | image page: one from-white clean on EVERY ARRIVAL (the page arriving differs from the page on the glass) | **r96.3** — the per-book `(spine,page)` guard is gone; `displayedSpine_/displayedPage_` moves at the single post-drive commit site, so image→text→image cleans on both arrivals and a repaint of the current page does not. **Operator check: flip image ↔ text several times in one book — every arrival at an image page flashes once, no repaint does** |
| D1b | the image-page clean "fired on neither edge" on the flashed r96.1 | **explained r96.2** — the flashed image (sha256 `5eee9118…`, 20:43) predates commit `cb5871c0` (20:50) and no longer exists on disk. Process rule added: never build a final image before the commit it claims to contain, and always grep the artifact for the round's new log strings |
| D1c | the arrival clean could fire once per book and never again (r96.2 on glass) | **fixed r96.3** (see D1) |
| W1 | wake from standby bands mid-transition | **r96.3, unproven on glass** — the line-queue theory is DEAD (`[panel] show mode=GC16-from-white tps=1 err=0x0` on the device, r96.2): the drive succeeds and the glass still bands. `[WAKE] resume drives=N` now counts the transition from the shim's own lines; `CP_WAKE_SETTLE_EXTRA_MS` (default 0) is the rail-settle experiment. **Operator check: after a wake, `logtail 40` — report the `[WAKE] resume drives=` line and whether drives=1** |
| W2 | Home's first-render follow-up adds a DU + GL16 completion on top of a wake clean | **fixed r96.2** (`CP_WAKE_ONE_DRIVE`, `HomeActivity.cpp:604-615/684-695` + `GfxRenderer.cpp:2064-2078`). Covers load on the next natural repaint** |
| L1 | `lpsleep` refused because a drive *duration* was read as an *age* | **fixed r96.1** |
| L2 | `lpsleep 60` armed, answered, and never returned; the arm record was lost | **r96.3: REFUSED by default** (`CP_LPSLEEP_ALLOW_HP_SLEEP=0`). The "different sleep entry path" hypothesis is refuted — both standby and `lpsleep` call `metalio_sleep_light_sleep()`, the only setter of `s_restore_armed`. Two live runs this round: one completed (pass line on the card), one died before the first flushed record and left none. Not reproducible ⇒ not a fix. Re-enable only when the bench matches standby's extend/INT masking AND a slice leaves the shell answering |
| L3 | the `lpsleep` entry path is not identical to standby's | **open** — standby masks the IO expander to the power key + `halClock.disableAlarm()`; the bench calls `releaseAccelIntLine()` only (`main.cpp:1209-1268` vs `LpCoreService.cpp:436-443`). Cheapest next step: make the bench perform the same extend/INT masking, then re-test with the shell watching |
