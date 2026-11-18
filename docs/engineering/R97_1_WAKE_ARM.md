# r97.1 — the wake arm: where the second drive comes from

Diagnostics-only round for the wake "band". No drive decision was changed:
`skipCompletion`, `CP_RICKY_COMPLETION_DRIVE`, the row-8/row-9 latch and the
completion drive itself are exactly r97.

## The arm, end to end

| step | where | what it does |
|---|---|---|
| ARM | `src/main.cpp:1483` `renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH)` | cancellable promotion: any call site that wants its own waveform can drop it (`cancelPromotedRefresh()`) |
| ARM | `src/main.cpp:1484` `renderer.forceCleanNextRefresh()` | NOT cancellable one-shot term the policy reads at paint time |
| ARM marker | `src/main.cpp:1491` INFO `[WAKE] resume: one-drive clean armed (...)` | the line the operator greps |
| ARM state (new) | `src/main.cpp:1502` INFO `[WAKE] arm: target=FULL(0) forcedCleanArmed=1 ...` | the arm's state read back through `forcedCleanArmed()` |
| CONSUME | `lib/GfxRenderer/GfxRenderer.cpp:1977-1979` | `const bool forceClean = g_forceCleanPaint; g_forceCleanPaint = false;` — consumed *before* the drive, so a skipped/failed paint cannot leave it armed |
| CONSUME log | `GfxRenderer.cpp:1987` `forced clean consumed by this paint (cleanPaints=..)` | pre-existing marker |
| CONSUME log (new) | `GfxRenderer.cpp:2000` INFO `[GFX] arm: consumed by this paint -- later paints in this wake see force=0` | states the consequence explicitly |
| RESET | `GfxRenderer.cpp:1914-1920` `cp_refresh_policy_note_park()` | park/deinit drops the latch AND `g_forceCleanPaint = false` |
| other armer | `src/activities/ActivityManager.cpp:511` `goHome()` | arms the same term for non-Sleep/non-Boot transitions; Sleep is excluded at `:505-512`, which is why resume's own arm at main.cpp:1483 is the one that matters |

## Why the observed paint logged `force=0 full=0 clean=0 previous=1`

Read from the code, not inferred:

1. **The arm was consumed, not lost.** `force=0` means `g_forceCleanPaint` was
   already false at that paint (`GfxRenderer.cpp:1977`). It is cleared in exactly
   two places: the consumption at `:1978` and the park at `:1919`. `clean=0`
   rules out the park (park also writes `CP_LATCH_CLEAN` + invalid, which would
   print `clean=1`), so an earlier paint in the same wake took it. That paint is
   Home's first frame — its `[GFX] arm armed=1` line (`GfxRenderer.cpp:1982`) is now the proof.
2. **`previous=1` is the latch after a forced clean.** `GfxRenderer.cpp:2060`
   writes `CP_LATCH_FULL` only when `requested == FULL_REFRESH`:
   ```cpp
   g_lastAppliedMode = (requested == HalDisplay::FULL_REFRESH) ? CP_LATCH_FULL : CP_LATCH_FAST;
   ```
   `forceClean` overrides `refreshMode` (`:2004`), never `requested`. Home asks
   for `HalDisplay::FAST_REFRESH` unconditionally on the wake frame
   (`src/activities/home/HomeActivity.cpp:591`, list path `:679`), so the
   from-white clean that landed was recorded as **FAST**. The next paint
   therefore sees `previous=1`, and row 8 (which promotes on
   `last_applied == CP_LATCH_FULL`) cannot fire.
3. **The second paint is Home's own repaint.** With `CP_WAKE_ONE_DRIVE=1`,
   Home suppresses only the *synchronous* first-render follow-up
   (`HomeActivity.cpp:604-621` grid, `:682-699` list) and only when
   `lastPaintForcedClean()` is true. The cover-thumbnail loader re-arms a paint
   from inside its own loop — `requestUpdate()` at `HomeActivity.cpp:234`,
   `:251`, and the deferred `loadRecentCovers()` branch at `:623-630` / `:700-706`
   — and that paint is not suppressed by anything. It resolves to FAST (the
   forced clean is gone, the promotion was cleared by `cancelPromotedRefresh()`
   at `:622` / `:699`), and row 9 then appends its GL16 whole-panel completion
   (`GfxRenderer.cpp:2077-2098`).

So the operator's wake is **three whole-panel drives**, not two: the armed
from-white clean, Home's cover-load repaint as a DU, and row 9's GL16
completion of that DU. The band is an intermediate frame *between* drives, which
is why the settled Home screen is clean.

## What the log will settle next

`[WAKE] arm:` (one line) → `[GFX] arm armed=1` + `[GFX] arm: consumed` (the
clean) → `[GFX] arm armed=0 latch=1` (Home's repaint) → `row9 completion drive
appended`. Counting `[GFX] arm armed=0` lines inside the capture window
(`CP_WAKE_CAPTURE_MS`) gives the number of extra drives directly.

## Conclusion on "ONE from-white drive"

Yes — the wake should be one from-white drive, but the completion drive is not
what makes it three. Deleting row 9 would leave Home's DU on the glass with no
completion, i.e. an unfinished fast transition over a freshly cleaned panel,
and would not remove the second drive anyway. The behaviour change the evidence
supports (NOT made in this round) is to defer Home's cover-load repaint while
the wake window is open, or to key `CP_LATCH_FULL` on the mode that was
*applied* rather than the mode that was *requested*. Both need on-glass A/B.
