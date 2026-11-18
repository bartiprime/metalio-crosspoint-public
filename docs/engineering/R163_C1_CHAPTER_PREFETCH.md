# R163 C1 — cross-chapter preparation (layout only)

**Status: implemented, host-tested, NOT built for the device and NOT flashed.** One commit; revert with
`git revert <sha>`.

Implements item C1 of the r163 plan against the trace in `R163_CHAPTER_LOAD_TRACE.md` and the vendor design in
`UPSTREAM_VENDOR_SWEEP_2026_11_16.md` §"The prefetch answer". Images are **not** implemented (see §7, which is a
report only).

---

## 1. What is built, and what it deliberately is not

**Not a task.** A chapter build holds the `Section` the render task paints, rasterises into the font cache, and
`ChapterHtmlSlimParser`'s image probes can borrow the framebuffer. The handoff contract's rule 1
(`src/activities/reader/ReaderHandoff.h:15-18`) forbids all three to a worker, and the C2 work proved it. So the
prepare is **idle work on the loop task**, under the same Try-acquired `DisplayLockGuard` and the same no-input tail
that already carries `backgroundBuildTick()` and `idlePrewarmTick()`.

**Not "prefetch the chapter".** The trace (§1f) shows `renderBook()` blocks only until the *target page* exists:
`section->pageCount > target`, and a chunk is `BUILD_PAGES_PER_CHUNK = 8`. What the turn actually pays is everything
before that page: the spine entry, the zip inflate, the parser prefix, and the first layout chunk (§1d). So one
attempt lays out `kPagesPerAttempt = 4` pages — the target page plus slack — and stops.

**What a prepare does** (`EpubReaderActivity::chapterPrefetchTick()`,
`src/activities/reader/EpubReaderActivity.cpp:664`):

1. `Section(epub, currentSpineIndex + 1, renderer, Section::Cache::Prefetch)` — a section for the *next* spine,
   owning `sections/<n>.pre.bin` (see §2).
2. `startBuild(spec, nullptr, /*shedFontCaches=*/false)` — the inflate and the parser prefix. The promoted
   `html/<n>.html` is layout-independent and stays cached even if the prepare is discarded.
3. `buildSomeMore(4, &buttonContactClosed)` — one layout chunk, interruptible by a press.
4. If the chapter did not fit: `suspendBuild()` — commits the pages as a partial at `.pre.bin` and **frees the
   parser**. A held artefact costs no heap until it is adopted.

The artefact is `{Target{spine, layoutSignature}, pages, bytes, ready}` — a file plus five scalars.

## 2. The temp-path collision, and how it is avoided

The trace's hard blocker (§2): `Section`'s cache path and its `.part` temp both derive from `spineIndex` alone
(`Section.cpp:115`, `:78`), so two `Section`s on one spine fight over `sections/<n>.bin.part`.

Two independent guarantees, both required:

- **Different spine.** The prepared section is always `currentSpineIndex + 1`, never the live spine. A prepare that
  would name the live spine cannot happen: `nextAction()` only ever returns `Prepare` for `live.spineIndex + 1`.
- **Different path.** `Section::Cache { Live, Prefetch }` is now a constructor parameter, and the purpose is part of
  the string, not a flag beside it:

  | purpose | cache file | build temp |
  |---|---|---|
  | `Cache::Live` | `.../sections/<n>.bin` | `.../sections/<n>.bin.part` |
  | `Cache::Prefetch` | `.../sections/<n>.pre.bin` | `.../sections/<n>.pre.bin.part` |

  `binTmpPath()` is `filePath + ".part"`, so parameterising `filePath` parameterises both. `clearCache()` and
  `abandonBuild()` also follow `filePath`, so every destructive path in a prefetch section can only reach its own
  file.

**The live `Section` is never touched.** The prepared artefact is a *different object* on a *different spine*, and
the only two things ever done to it are:

- **discard** — `abandonBuild()` + `clearCache()`: two deletes of files it alone owns, no writes;
- **adopt** — one `Storage.rename(prepared, live)`. It exists only if the live slot is empty (§5).

## 3. The gates, as implemented

`chapterprefetch::mayPrepare()` (`src/activities/reader/ChapterPrefetch.h:112`) is the AND of all of them, in one
function so a host test can flip each one:

| # | gate | read from |
|---|---|---|
| 1 | display guard acquired by **Try** (`DisplayLockGuard`) — paint drained, no compose running | `DisplayLockGuard::owns()` |
| 2 | framebuffer present | `renderer.hasFrameBuffer()` |
| 3 | no overlay / toolbar / panel open | `overlay == Overlay::None` |
| 4 | no deferred chrome refresh owning the framebuffer | `overlayRefreshPending` |
| 5 | framebuffer not stale (a build step lent it and it came back blank) | `pageBufferStale` |
| 6 | live section exists | `section != nullptr` |
| 7 | `!section->isBuilding()` | `Section::isBuilding()` |
| 8 | live chapter is not at its partial watermark (about to start its own build) | `isPartial()` + `PARTIAL_REBUILD_START_MARGIN` |
| 9 | `!FontCacheManager::isScanning()` | `renderer.getFontCacheManager()` |
| 10 | no queued / held turn | `pendingManualTurn`, `pendingPageJump`, `pendingAnchor`, `pendingPercentJump`, `pendingOffsetJump` |
| 11 | no button or touch down | `buttonContactClosed()`, `anyButtonPressed()` |
| 12 | the BLE page turner is not holding the heap | `bleturner::holdsHeap()` |
| 13 | heap: **free ≥ 64 KiB** and **max block ≥ 32 KiB** | `ESP.getFreeHeap()`, `ESP.getMaxAllocHeap()` |
| 14 | layout known (the first paint has resolved the viewport) | `buildViewportWidth/Height` |
| 15 | a next spine exists | `epub->getSpineItemsCount()` |
| 16 | the next spine has **no** `sections/<n>.bin` | `Storage.exists` |
| 17 | the next spine's HTML is **≤ 96 KiB** | `epub->getCumulativeSpineItemSize` |

Gates 16-17 are the two card facts, and both **refuse the spine** (`State::refusedSpine`) rather than re-probing every
idle frame; the refusal expires by itself because it names a spine, not a chapter.

Also debounce-free by construction: the tick runs on the no-input tail, which is already ≥ 400 ms after the last paint
in the prewarm's terms, and it is **one attempt per chapter** — after a success the artefact is `ready` and the tick
does no card work at all.

### Budgets — bytes and work per idle frame

| bound | value | why |
|---|---|---|
| pages per attempt | **4** | one is all the target page needs; four keeps the record ceiling small and leaves slack |
| page-record bytes | **64 KiB** (`4 × kPageRecordMaxBytes`) | a record is bounded at 16 KiB (`Section.cpp:104`); typical is 1-4 KiB |
| spine HTML inflate | **96 KiB** | below the reader's own "this will make you wait" threshold (`BUILD_POPUP_BYTE_THRESHOLD`); above it the inflate is a stall, not an idle frame |
| heap | 64 KiB free / 32 KiB block | twice the idle build's floors (`EpubReaderActivity.h:204-205`), so the live path can still start its own build behind us |
| frames | 1 per chapter | measured state, not a timer |

**Gates 13-17 and the 96 KiB cap are reasoned from the tree, not measured.** They are deliberately on the
conservative side of existing constants; §6 says which of them glass should settle.

## 4. Invalidation

Not a list of navigation sites — there are ~30 `section.reset()` sites (trace §2) and a list would silently leak one.
The rule is derived from four facts, re-read every tick (`chapterprefetch::nextAction()`):

| fact | changes it | result |
|---|---|---|
| `SessionGeneration` (`ReaderHandoff.h:50`) | a new `Section`, a new spine, or a new layout signature, observed by `renderBook()` | **Discard** |
| target spine still `live.spineIndex + 1` | chapter jump, TOC to another chapter, footnote target, search jump, sync apply, book close | **Discard** |
| layout signature unchanged | font / size / spacing / alignment / hyphenation / viewport change | **Discard** |
| the artefact is `ready` | — | an unready artefact is dropped rather than left |

So: chapter jump, TOC (to anywhere else), footnote, search, book close, font or text-settings change, and sleep all
discard — the first four through the generation and/or the spine fact, the font change through the signature, sleep
and exit through an explicit `discardPreparedChapter()` in `prepareForSleep()` / `onExit()`.

A **TOC jump onto the prepared chapter itself, at its top, with the same layout** is the one non-turn navigation that
keeps and adopts the artefact. That is not an oversight: the artefact is valid for exactly that open, and refusing it
there would throw away work that the open then has to redo. Every other target invalidates.

Discard is **free**: `abandonBuild()` removes the two files it owns and writes nothing. The promoted `html/<n>.html`
stays — it is layout-independent, and it is the expensive half of a cold chapter.

## 5. Adoption, and the fallback

On the render task, inside `renderBook()`'s `if (!section)` block and **before** the live `Section` is constructed
(`EpubReaderActivity.cpp:2080`):

```
adoptPreparedChapter()   // renames sections/<n>.pre.bin -> sections/<n>.bin, if it may
section = new Section(...); section->loadSectionFile(renderSpec);
```

`canAdopt()` requires: ready, target spine == the spine being opened, same layout signature, and a **forward** turn
that lands on page 0 — `nextPageNumber == 0` and none of `pendingPageJump`, `pendingAnchor`, `pendingOffsetJump`,
`pendingPercentJump`. A backward chapter turn asks for the chapter's last page (`pageTurn` sets
`pendingPageJump = UINT16_MAX`, `EpubReaderActivity.cpp:1665`), a sync/bookmark jump asks for a text offset the
prepared pages may not contain, and a percent jump rebuilds the chapter outright: a prepared *first page* answers
none of those, so those artefacts are dropped instead.

`adoptPreparedChapter()` additionally refuses when `sections/<n>.bin` already exists — adoption must never replace a
warm chapter with a first page, so it can only make an open better, never worse.

After the rename the live section finds a **partial** where a cold open would have found nothing, and takes the paths
the reader already has for that state:

- `loadSectionFile()` validates version, spec and the watermark trailer as it does for any partial (so a stale or
  foreign file is cleared and rebuilt — the same code that handles a suspended build);
- the open's build loop stops as soon as `pageCount > target` — 4 pages already exist, so **no layout chunk is laid
  out on the turn**, and the inflate was paid at idle time;
- the loop/background ticks extend the partial exactly as they do today.

**The fallback is literally today's code.** If adoption declines or the rename fails, nothing has changed except one
file left on the card, which the next prepare clears. Worst case is today's behaviour.

Residual cost on the open, stated plainly: `renderBook()` still calls `startBuild()` in that branch (parser creation,
CSS hydration, TOC anchors), because 4 pages is below `target + PARTIAL_REBUILD_START_MARGIN` and so the reader does
not take the "partial covers the target, defer" branch. What is skipped is the inflate, the parser prefix and the
blocking layout chunk. Making the open skip `startBuild()` too would need ≥ 16 pages prepared, i.e. four times the
budget, which is not what item C1 asks for.

## 6. Host tests, and what needs glass

`test/chapter_prefetch/ChapterPrefetchTest.cpp` — **26 tests, all passing** (built standalone against the repo's
pinned googletest; the repo's own `test/CMakeLists.txt` gains `add_subdirectory(chapter_prefetch)`):

- **gates** — the idle frame passes; each of the 14 gate fields alone blocks; the heap floors are inclusive;
- **budgets** — the record ceiling equals `4 × 16 KiB`; `withinBudget` boundaries; the 96 KiB spine-cap boundary;
- **lifecycle** — prepares when everything holds; does not prepare past the last chapter or before the first layout;
  a refused spine is not retried and expires when the reader moves;
- **holding** — a ready artefact survives 30 page turns in the same chapter (the generation does not move);
- **invalidation** — discard on session change, on a chapter jump (caught twice over), on a layout change, and when
  the next chapter stops being the target;
- **adoption** — adopted on a forward turn into a valid target; refused on a backward turn, a changed layout, a
  different chapter, and an unready artefact; the armed generation is *not* required to match (the chapter turn is
  what bumps it);
- **wiring, over the reader source** — the prepared section is constructed with `Cache::Prefetch` on `next`; the tick
  sits on the loop's idle tail after the build tick and acquires the display through `DisplayLockGuard`; adoption
  runs before the live `Section` is constructed; `prepareForSleep()` discards.

`Section.h` is also compiled on the host against the parser test's stubs (a `-fsyntax-only` check), because the ESP
build is out of scope for this item.

**What no host test can answer, and gold should:**

1. **Wall-clock of one attempt.** The trace marks the first `buildSomeMore` UNKNOWN (its §5.2). The attempt is one
   `startBuild` (inflate ≤ 96 KiB) plus one chunk of ≤ 4 pages; if either is > ~200 ms on a slow card the idle frame
   is too expensive and `kPagesPerAttempt` and/or `kMaxSpineBytes` must come down. The `[ERS] "Prepared chapter n: …,
   %lums"` line reports it.
2. **The heap peak.** 64 KiB / 32 KiB is twice the build's floors, not a measured peak. The `[ERS]` log plus the
   existing `Heap before section build` line is the measurement.
3. **Whether the BT radio is ever released for a prepare.** It is not: `bleturner::holdsHeap()` blocks the gate
   outright. With a page turner connected there will be no prefetch at all — verify that is acceptable in the field.
4. **The first-paint difference on a real chapter turn**, which is the whole point: cold vs adopted open, from the
   existing `[ERS]` timings.

## 7. F7 — image prefetch radius: assessment only, **not built**

**Are images already in the layout path? Yes, three ways.**

1. **During layout** every `<img>` costs an early-aborted zip inflate to read its dimensions
   (`ChapterHtmlSlimParser.cpp:1718-1736`, 1 KiB chunks, 1-3 reads), with a `FrameBufferLoan` retry when the inflate
   window does not fit, and a full extraction *with the indexing popup* as the last resort. So a prepared chapter
   already pays its image probes — that is part of why the loan-count guard in the tick exists.
2. **At render** `ImageBlock` decodes the source into a `.pxc` pixel cache keyed by
   `img_<spine>_<counter>` + the book's own extension (`ImageBlock.cpp:41-46`, `:1715` in the parser), and re-reads a
   whole `.pxc` off the card on every decode pass (~100 ms for a full-page image, `ImageBlock.cpp:105`).
   `Page::hasImagesNeedingDecode()` (`Page.h:188`) is the "this page will stall on a decode" predicate.
3. **In policy** an image page already has its own arrival contract: `CP_IMG_CLEAN_ON_ARRIVAL` arms a from-white
   GC16 (~815 ms) on the first paint of an image page, and the placeholder drive on a FAST waveform covers the
   decode (`PlatformFeatures.h:176-212`).

**What a radius would change, and why it is not in this build.**

- A `.pxc` cannot be produced without **decoding into a framebuffer** (`ImageToFramebufferDecoder`), which is the one
  resource the idle gates exist to protect: our own gate 5/loan counter, and rule 1 of the handoff contract. Doing it
  from the loop task under the lock would hold the framebuffer for a whole decode and blank the page the operator is
  reading.
- The win would also land where the cost is *not*: the trace's first-paint cost is dominated by inflate + layout, and
  the image page's own delay is already covered by the placeholder drive + arrival clean. The measurable delta of a
  radius is one `.pxc` read (~100 ms) on the page it warms, on books that are image-heavy.
- Radius interacts with invalidation: `.pxc` is keyed by image index and by the *rendered* size, so a layout change
  invalidates it exactly like a section file — the same 4-fact rule would have to be extended, and a `.pxc` written by
  a discarded prepare is dead weight on the card with no cleanup path today.

**Recommendation:** leave it unbuilt. If it is ever wanted, do it as a separate item with its own budget, its own
gate (`hasImagesNeedingDecode()` on the candidate page), and a decode path that does not need the live framebuffer.
The 16-level greyscale panel is irrelevant to this decision — nothing here dithers or quantises.
