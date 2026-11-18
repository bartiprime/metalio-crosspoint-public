# Documentation index

All paths are relative to `docs/`. New documents must be added here.

## Engineering / fork maintenance

> **The docs repo owns all engineering and planning documents.** Canonical copies live in
> `crosspoint-fork/docs/engineering/` and `crosspoint-fork/docs/planning/`, and
> `crosspoint-fork/docs/INDEX.md` is the map. **This directory is deprecated** — do not add new
> engineering documents here. The rest of `docs/` below is upstream product documentation and *is*
> maintained in this repo.

| Document | What it answers |
|---|---|
| [engineering/FORK_DIVERGENCE_AUDIT.md](engineering/FORK_DIVERGENCE_AUDIT.md) | **Pointer only** — the full audit moved to the docs repo at `crosspoint-fork/docs/engineering/FORK_DIVERGENCE_AUDIT.md`. Read that copy; it covers how far this fork has diverged from upstream CrossPoint Reader, which divergences are necessary vs. suspect, every flaw found (fixed and reported), vendored-tree and submodule provenance, and the merge hazard from the upstream range we have not taken. |
| [engineering/R99_8_BLANK_FRAMES.md](engineering/R99_8_BLANK_FRAMES.md) | **Pointer only** — moved to `crosspoint-fork/docs/engineering/R99_8_BLANK_FRAMES.md`. Every blank/white intermediate frame in the tree, classified into deliberate clears vs gaps, and the r99.8 fix that keeps the boot popup on the glass. |
| [engineering/STANDBY_TIMER_WAKE_CONTRACT.md](engineering/STANDBY_TIMER_WAKE_CONTRACT.md) | What a timer wake is allowed to touch: which peripherals stay parked across a standby slice, where each is parked and restored in `src/main.cpp`, our conformance verdict, and the guard that fires if the park is lifted inside a slice. Read before reopening `LPSLEEP`. |

## Using the reader

| Document | What it answers |
|---|---|
| [activity-manager.md](activity-manager.md) | How screens are pushed, replaced and popped, and who owns the render lock. |
| [focus-reading.md](focus-reading.md) | The focus-reading mode and its UI. |
| [dictionary.md](dictionary.md) | Dictionary lookup: formats, indexes, on-device behaviour. |
| [file-formats.md](file-formats.md) | Which formats are supported and how each is parsed. |
| [hyphenation-trie-format.md](hyphenation-trie-format.md) | The on-disk hyphenation trie layout. |
| [sd-card-fonts.md](sd-card-fonts.md) | Installing and selecting fonts from the SD card. |
| [sd-plugins.md](sd-plugins.md) | Installing and using SD-card plugins. |
| [plugin-events.md](plugin-events.md) | The plugin event catalogue and payload shapes. |
| [i18n.md](i18n.md) | Translations: catalogue layout, generation, adding a language. |
| [translators.md](translators.md) | Guide for translators. |
| [comparison.md](comparison.md) | Feature comparison against other readers. |
| [troubleshooting.md](troubleshooting.md) | Symptom → cause → fix. |
| [fix-bricked-xteink.md](fix-bricked-xteink.md) | Recovering a device that will not boot. |

## Networking and transfer

| Document | What it answers |
|---|---|
| [webserver.md](webserver.md) | The on-device web server and its UI. |
| [webserver-endpoints.md](webserver-endpoints.md) | Every HTTP endpoint, method, parameter and response. |

## Contributing

| Document | What it answers |
|---|---|
| [contributing/](contributing/) | Build, style and review conventions, including touch/UI rules. |
| [freeink-sdk-input-logger.patch](freeink-sdk-input-logger.patch) | Patch carried against the SDK for input logging. |