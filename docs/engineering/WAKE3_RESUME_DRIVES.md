# WAKE3 — who issues resume drives 2 and 3?

Diagnostics-only round (r101.9). **No drive decision, mode, count or timing was
changed.** (Every `file:line` below was re-verified against `8154b5fe` at the end of the batch —
DOCS1 — because the batch inserted lines in `main.cpp`, `GfxRenderer.cpp` and
`metalio_vendor_panel.cpp` after this document was written.) The only edits are two counters/a pointer that nothing branches on and
one extra field on an existing log line.

Symptom under investigation (r101.8, both wakes sampled):

```
[WAKE] resume: one-drive clean armed (promotion FULL + forceClean, one-drive=1)
[WAKE] drive 1 mode=GC16-from-white err=0x0 tps=1 ms=1171 (+158 ms after resume)
[WAKE] drive 2 mode=GC16-from-white err=0x0 tps=1 ms=805  (+964 ms after resume)
[WAKE] drive 3 mode=GC16-from-white err=0x0 tps=1 ms=805  (+966 ms after resume)
[WAKE] resume drives=3 window=4000 ms band=extra drive(s): still something driving after the clean
```

## The three sites

| role | where | what it does |
|---|---|---|
| ARM | `src/main.cpp:2026` `promoteNextRefresh(FULL_REFRESH)` + `:2027` `forceCleanNextRefresh()` | the two one-shots the resume promises are consumed by the **next** `displayBuffer()` |
| ARM state | `src/main.cpp:2045` INFO `[WAKE] arm: target=FULL(3) forcedCleanArmed=..` | reads the arm back through `forcedCleanArmed()` |
| ARM marker | `src/main.cpp:2034` INFO `[WAKE] resume: one-drive clean armed (promotion FULL + forceClean, one-drive=%d)` | the line the operator greps |
| WINDOW OPEN | `src/main.cpp:2050-2055` `g_wakeDriveN = 0; g_wakeCapture = true;` + INFO `resume: drive capture armed window=..` | starts the count |
| **CAPTURE (a)** | `src/main.cpp:844-916` `panelJournalCStr()` — `if (g_wakeCapture) { ++g_wakeDriveN; LOG_INF("WAKE","drive %lu ...") }` at `:901` | **one `[WAKE] drive` row per PANEL JOURNAL LINE**, not per waveform |
| WINDOW CLOSE | `src/main.cpp:4003` INFO `[WAKE] resume drives=%lu window=%d ms band=%s` | prints `g_wakeDriveN`, i.e. the same line count |
| source of the lines | `idf/components/metalio_driver/metalio_vendor_panel.cpp:864` (`[panel] show mode= tps= err= ms=`) and `:932` (`[panel] rails tps= pgood= err=`) — **two lines per successful `show()`** | plus `:903` `[panel] recover 0x400 ...`, `:599` `[panel] pg=`, and `[G16]` / `[G16] erase sweep` from `:697` / `:804` |

The capture sits **below** the renderer, so historically a `[WAKE] drive` row
could not name the paint that issued it.

## Ranked candidate origins

### 1. Line-count artifact: the extra rows are the SAME drive's second journal line (most likely)
One `show()` emits `[panel] show mode=.. ms=..` **and then** `[panel] rails tps=.. pgood=.. err=..`
(`metalio_vendor_panel.cpp:862-867`, `:925-936`). Both reach `panelJournalCStr()`, and the
`rails` line carries neither `mode=` nor `ms=`, so the hook prints the *sticky* globals
(`g_panelDriveMode`, `g_panelDriveMs`, `main.cpp:878-899`) filled in by the preceding `show` line.
That is exactly the observed pair:

* drive 2 and drive 3 are **2 ms apart**, identical `mode`, `err`, `tps` and `ms=805` — two
  lines of one waveform, not two waveforms
* `+964 − +158 = 806 ms ≈ 805` = drive 2's own duration, so drive 1's row was written when a
  drive that had *started before the window armed* finished, and its `ms=1171` is a value parsed
  before `g_wakeCapture` was set (a pre-arm drive, e.g. the wake-prime / sleep-screen paint)

So `drives=3` may be **one real `show` line plus two non-driving rows** — the arming code
promising one and the panel performing one.
**Discriminator:** `src=rails/...` (or `-`, `sweep`, `g16`, `pg`) on the extra rows, and drive 1
and drive 2 sharing the same `p<N>`.

### 2. Row-9 completion drive appended inside the same `displayBuffer()`
`lib/GfxRenderer/GfxRenderer.cpp:2577-2600`: a FAST paint that is not `skipCompletion` issues a
second whole-panel `GL16` drive *after* the first one, from the same paint.
**Discriminator:** two `show` rows with the **same `p<N>`** and the second one `mode=GL16`.

### 3. A second real paint by the render task on the same screen
Home's cover-thumbnail loader re-arms `requestUpdate()` (`HomeActivity.cpp:247`, `:264` (and the
rest of its `requestUpdate()` sites)) and that paint is not suppressed by `CP_WAKE_ONE_DRIVE`; the reader has the same
first-render follow-up shape. Two paints of one screen share `visit=`, so `visit` cannot tell
them apart — the paint counter can.
**Discriminator:** `src=show/pM/R:Home` with `M ≠ N` of drive 1, joined to
`[ACT] paint Home visit=.. p=M`.

### 4. The r100.5 paint-debt re-request or the paint watchdog asking for a frame
`src/activities/ActivityManager.cpp:457-472` (`enterPaintOwed` -> `requestedUpdate = true`) and
`:480-511` (staged watchdog, `[ACT] paint watchdog: no frame for %s yet -- re-requesting`) both
re-arm a paint when no frame reached the glass. On a wake the entry gate can withhold the first
paint, and a held paint drives nothing while `paintedVisit` stays unset — the watchdog then asks
for the frame it thinks is missing, i.e. a legitimate second paint.
**Discriminator:** a `show` row with `src=show/pM/R:<activity>` preceded by
`[ACT] enter paint owed` / `[ACT] paint watchdog: ... re-requesting`, and an earlier
`[ACT] paint <same> ... held=1`.

### 5. A paint issued from `Activity::onEnter()` / the first-render path (main task)
Same events, but composed outside the render task.
**Discriminator:** `src=show/pK/M` (`M`, no activity name). On the resume path this should not
happen when `CP_WAKE_ONE_DRIVE` suppressed the follow-up (`[HOME] wake clean on glass: ...
follow-up paint suppressed`); if it appears, that suppression did not run.

### 6. A driver-side retry or a cadence clean, not a caller at all
`[panel] recover 0x400` re-drive (`metalio_vendor_panel.cpp:901-906`), the 24-partial
`cadenceDue` from-white clean (`:827`), or a `show SKIPPED (no drive)` no-op
(`tElapsed < kNoDriveMsFloor`) — the last one counts as a drive row while driving nothing.
**Discriminator:** `src=rec/...`, `src=skip/...`, or a `show` row whose `p<N>` belongs to a paint
whose `[ACT]`/`[GFX]` trail shows no other reason to drive.

## The instrumentation (one field, no behaviour)

`[WAKE] drive` gained exactly one field, `src=<lineKind>/<paint>`:

```
[WAKE] drive 2 mode=GC16-from-white err=0x0 tps=1 ms=805 (+964 ms after resume) src=rails/p7/R:Home
```

* `lineKind` — `show|skip|rails|rec|pg|boot|g16|sweep|other`, read off the journal line's own
  prefix in `panelJournalCStr()` (`src/main.cpp:856-877`). A non-`show` kind is **not** a second
  waveform. Cost: a handful of `strncmp`s on a line that is already being parsed.
* `paint` — `p<N>/R:<activity>` for a paint composed by ActivityManager's render task, `p<N>/M`
  for any other task. `N` is `displayBuffer()`/`displayBufferAsync()` entries
  (`GfxRenderer.cpp:2355`, `:2620`); the same `N` on two rows means **one** paint entry.
  `R:<activity>` comes from `cp_paint_owner_note()` around `Activity::render()`
  (`ActivityManager.cpp:126-129`), and the same `N` is printed as `p=` on the existing
  `[ACT] paint <name> visit=.. p=N held=.. drove=..` line, so the join is exact rather than
  positional.

Files: `lib/GfxRenderer/GfxRenderer.h:29-51` (declarations + rationale),
`lib/GfxRenderer/GfxRenderer.cpp:2303-2329` (state + `cp_paint_origin_text()`), `:2355`
and `:2622` (the two paint-entry ticks), `src/activities/ActivityManager.cpp:126-129` and
`:148-149` (`R:<activity>` stamp and `p=` on the `[ACT]` line), `src/main.cpp:856-877`
(line kind) and `:901-916` (the one extra field).

Why this cannot add a drive: the counter is incremented at paint **entry**, before the entry
gate can return; `cp_paint_owner_note()`/`_clear()` store and drop a pointer around an existing
`render()` call; `cp_paint_origin_text()` is called from inside the existing `LOG_INF`. Nothing
in `displayBuffer()`, `resolveRefreshRequest()`, the policy or `show()` reads any of it, and no
`#if` was flipped.

Budget note: naming the origin needed exactly one field. A second field would only be needed to
name the *statement* inside an activity (e.g. which `requestUpdate()` site) — that requires a
per-call-site tag and is out of scope here.

## Verification (no build performed)

* The three touched translation units were compiled with the exact r1019 flags out of
  `build-r1019/compile_commands.json`, `-fsyntax-only` only (nothing written into the build
  directory, no link, no flash): `src/main.cpp`, `src/activities/ActivityManager.cpp`,
  `lib/GfxRenderer/GfxRenderer.cpp` — all return 0, no diagnostics.
* `git diff --stat` = 4 files, +109/−4: two counters/one pointer (`cpp:15`), the header
  declarations (`h:25`), the render-task stamp and `p=` field (`ActivityManager:10`), the line
  kind + the one extra field (`main:43`). No `#if` flipped, no `displayBuffer()` call added,
  moved or re-parameterised, no delay changed.

## The experiment

1. Flash the r101.9 image (the orchestrator's `build-r1019`), open a book, press power to
   sleep, wait for standby, press power to wake. Do it **twice**.
2. From the captured serial log take the block from `[WAKE] drive capture armed` to
   `[WAKE] resume drives=`.
3. Read `src=` on each row:

| what the rows say | conclusion |
|---|---|
| `rails`/`-`/`sweep`/`g16` rows, drive 1 and 2 share `p<N>` | candidate 1 — `drives=` over-counts; fix the capture (count `show` lines only), the panel really drove once |
| two `show` rows, same `p<N>`, second `mode=GL16` | candidate 2 — row-9 completion |
| two `show` rows, different `p<N>`, both `R:Home` (or `R:<reader>`) | candidate 3 — the thumbnail/first-render follow-up paint; then look up that `p=` in the `[ACT] paint` line and at the surrounding `[HOME]`/`[ERS]` lines |
| a `show` row with `p<N>/R:..`, preceded by `[ACT] enter paint owed` / `[ACT] paint watchdog: ... re-requesting` (and an earlier `held=1`) | candidate 4 — the paint-debt re-request; not a caller bug |
| a `show` row with `p<N>/M` | candidate 5 — an `onEnter()`/first-render paint; check whether `[HOME] wake clean on glass: ... suppressed` is absent |
| `rec` or `skip` rows only | candidate 5 — driver-side retry / no-op drive |

Useful greps: `grep -E '\[WAKE\]|\[ACT\] paint|\[GFX\] arm' log.txt` — the `[WAKE] drive` rows and
the `[ACT] paint ... p=N` rows share `N`, so `sort`/`join` on `p=` answers "which paint" without
`visit` heuristics.
