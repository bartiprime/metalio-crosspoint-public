# theme_tokens — host checks for a UI theme

`run.sh` builds two small host programs against the theme headers and runs them,
plus a python registration check. **No ESP build, no flash, no device**: any
C++17 compiler and python3 will do.

```
sh tools/theme_tokens/run.sh          # verdicts + the token table on stdout
```

## Why it exists

A theme is a table of numbers plus a few paint overrides. Most ways to break one
are invisible until the glass: a radius that crept to a pill, a stroke under the
2 px ink floor, a hand slot that is 40 px on a 500 px `homeCoverTileHeight`, an
enum value appended in the wrong place (which silently re-themes every saved
device). Those are exactly the things a host program can catch, so they live
here instead of in a flash cycle.

| file | what it does |
|---|---|
| `host_table.cpp` | Prints **every** field of `ThemeMetrics` for four themes straight out of the compiled `constexpr` tables, with each field's `MetricScale` class and its Plus-panel value. The review table in `docs/engineering/R164_PICKYOS_THEME.md` is this output verbatim. |
| `host_verify.cpp` | Asserts the design language: one stroke weight, no radius above 8 design units, no stadium sentinel, no fill selection, the card fits its band, the thumb cache key equals the painted cover height. Prints the derived Home geometry for the Plus panel. |
| `check_theme_registration.py` | Asserts `UI_THEME` values, the `homeThemeValues()` label array, the `setTheme()` switch and `english.yaml` are index-aligned, and that the PSRAM-degraded path relabels the cover-grid **slot** instead of truncating the list. |
| `check_theme_syntax.sh` | `-fsyntax-only` on the theme's `.cpp` against the **real** `BaseTheme.h`, with minimal platform stubs under `synth/`. Catches a wrong `override`, a bad renderer call, a misspelled metric — everything that would otherwise surface at flash time. |
| `stubs/components/UiDensity.h` | Host-only shim: the real header pulls `<BoardConfig.h>` → Arduino/ESP-IDF. `scale() == 1.0` means the tables are read in **design space**, which is what a paper review compares. |
| `synth/` | The platform stubs `check_theme_syntax.sh` compiles against. **Not firmware**: only reachable through that script's `-I`. Every renderer signature is copied from the real header, so if the renderer changes, re-copy. |

## What it cannot prove

Link, size, and anything about ink on e-paper: how a 2 px rule reads at 297 PPI,
whether a sampled icon breaks, ghosting, and every refresh-mode question. Those
are glass judgements — see "How to judge it on glass" in the design doc.

## Adding a theme

1. Add the metrics namespace + the class under `src/components/themes/<name>/`.
2. Append the value at the **end** of `UI_THEME` and to `homeThemeValues()`.
3. Add a `case` in `UITheme::setTheme()` and the label to `english.yaml`.
4. `sh tools/theme_tokens/run.sh` — step 3's alignment check is what catches a
   value appended in the wrong place.
