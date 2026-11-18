# IMG2 host proof — in-book image tone, old cut vs shipped dither

The dither fix (`applyBayerDither1Bit`, `lib/Epub/Epub/converters/DitherUtils.h:84-85` — threshold
`4 * bayer8x8 + 2` — plus the `< 2` BW cut at
`lib/Epub/Epub/converters/DirectPixelWriter.h:193`) has been in every build since r94 but has never
been judged on the glass. This is the host side: the same arithmetic run over four images with
real numbers, so the device check is a glance rather than a hunt.

**Status: host proof only. `IMG2` is BUILT INTO r101.9 and NOT FLASHED; the tone verdict is still
owed by the operator**, and the three checks at the end of this file are what settles it.

Nothing in the firmware changed for this proof. No build, no flash.

---

## What is reused, and what is new

The proof **includes the shipping header verbatim** — `lib/Epub/Epub/converters/DitherUtils.h`
— rather than re-implementing a copy of the matrix. Only its `#include <GfxRenderer.h>` is
satisfied by a shim (`_scratch/img2-proof/shim/GfxRenderer.h`) declaring the three members
`imageTargetLevels()` needs; that function is not on the path under test. `CP_IMG_DITHER` is
asserted `== 1` at compile time (`static_assert`), so a build that turned the dither off
cannot silently produce this document's numbers.

The two reductions being compared are the ones in `writePixel()`, quoted here so the mapping
from code to table is explicit:

```cpp
// OLD (CP_IMG_DITHER=0 -> quantizeImagePixel() falls through to applyBayerDither4Level)
draw = pixelValue < 3;      // 4-level value: only level 3 stays paper
// SHIPPED (imageTargetLevels()==1 -> applyBayerDither1Bit, threshold 4*bayer8x8+2)
draw = pixelValue < 2;      // 1-bit value: 0 is ink, 3 is paper
```

New files, all under `_scratch/img2-proof/` (git-ignored):

| file | role |
|---|---|
| `gen_inputs.py` | writes the four test images as 8-bit PGM |
| `img2_tone_proof.cpp` | includes the real `DitherUtils.h`; runs both paths; writes two 1-bit BMPs per image; prints coverage |
| `combine.py` | folds each pair into one `*_side_by_side.bmp` (left = old, right = shipped) and emits the table |
| `in/`, `out/` | inputs, and the BMPs to look at |

---

## The four images

| image | why it is in the set | source ink |
|---|---|---|
| `01_flat50` | a flat 50 % grey patch — the exact case that was 100 % black before r92 | 49.8 % |
| `02_gradient` | a linear 255 → 0 ramp: every tone once, uniform histogram, so the numbers are predictable in closed form | 50.0 % |
| `03_photo` | photograph-like: sky ramp, lambert-shaded sphere, cast shadow, film grain — midtone-heavy, 31 % ink | 31.0 % |
| `04_lineart` | text-like page: 2–3 px strokes, a solid heading bar, a 1 px hairline rule, a 244 cream band, 255 paper | 13.7 % |

All are 512 × 384, 8-bit greyscale before quantization.

---

## Coverage table

Black = pixels the panel actually inks. PASS means the shipped path lands within 1.5 points
of the source ink, i.e. the tone survives; the old path is expected to overshoot toward black.

| image | source ink % | OLD (`< 3`) black % | SHIPPED (`< 2` + Bayer) black % | verdict |
|---|---|---|---|---|
| `01_flat50` | 49.8 | **100.0** | **50.0** | PASS |
| `02_gradient` | 50.0 | **76.0** | **50.0** | PASS |
| `03_photo` | 31.0 | **66.0** | **31.0** | PASS |
| `04_lineart` | 13.7 | **14.0** | **13.7** | PASS |

The shipped column reproduces the source ink on every image, to a tenth of a point. The old
column is a 3/4 threshold, so it tracks the *bright fraction* of the histogram, not the mean:
a photo at 31 % ink renders at 66 % black (2.1× too dark), a 50 % grey at 100 %.

### Extremes — does the dither damage strokes or paper?

| image | dark px (< 40) kept black | light px (> 215) kept white | true paper (255) turned black | cream band (235–250) kept white |
|---|---|---|---|---|
| `02_gradient` | 100.0 → 92.2 % | 94.7 → 92.2 % | 0 / 768 → 0 / 768 | 100.0 → 94.9 % |
| `04_lineart` | 100.0 → 95.3 % | 100.0 → 99.6 % | 0 / 156 026 → 0 / 156 026 | 100.0 → 95.0 % |

Read the last two columns literally: **pure paper stays pure paper** — not one of 156 026
white pixels is inked under either path. The cream band loses 5 % of its pixels to dots, and
that is correct arithmetic, not damage: 244/255 is a 4.3 % ink tone and the 8×8 tile delivers
4.7 %. A 12/255 body stroke keeps 95.3 % of its pixels for the same reason — it renders at
95 % coverage because that is what a 95 % ink stroke is. Stroke shape and paper are intact;
what changed is that midtones now have somewhere to go.

---

## Why these numbers differ from the two earlier readings

* **flat 50 % grey: 0.0 % white before, 50.0 % white after.** Identical here, seen from the
  other side: 100.0 % black → 50.0 % black. Level 2 is the `< 3` cut's blind spot — it is
  "light", and the old cut inked it.
* **"bright gradient 87 % black → 47.1 %".** That measurement was on a gradient whose *mean*
  ink was 47.1 %; the shipped column lands on 47.1 % because it reproduces the mean, while the
  old column's 87 % came from that image's brighter tail. A uniform 255 → 0 ramp (mean ink
  50.0 %) gives 76.0 % black instead of 87 %, and that 76.0 % is predictable without running
  the code: the old cut leaves level 3 (adjusted ≥ 192) as paper, which over the 16 dither
  offsets `(bayer4x4 − 8) × 5` averages `64 + mean(dither) = 61.5` of 256 ramp values
  = **24.0 % white → 76.0 % black**. The shipped path's threshold `4·bayer8x8 + 2` averages
  `254 − 4·31.5 = 128` = **50.0 % white**. Both closed forms match the measured table exactly,
  which is an independent check that the harness is running the shipped arithmetic and not a
  lookalike.

---

## Output artifacts

`M:/_HermesWork/crosspoint-fork/crosspoint-reader/_scratch/img2-proof/out/`

| file | contents |
|---|---|
| `01_flat50_side_by_side.bmp` … `04_lineart_side_by_side.bmp` | the glance view: **left = OLD cut, right = SHIPPED**, 8 px white gutter between |
| `<name>_old.bmp`, `<name>_shipped.bmp` | the two panels separately, 1-bit, 512 × 384 |
| `coverage.csv` | raw numbers behind the tables |

The BMPs were read back after writing and their black coverage recomputed from the file
bytes — 100.00 / 50.00, 75.98 / 50.00, 65.98 / 31.04, 14.05 / 13.72 — so the table describes
the files as they sit on disk, not an intermediate.

Rebuild and rerun (host only, clang, ~2 s):

```bash
cd _scratch/img2-proof
python gen_inputs.py
clang++ -std=c++20 -O2 -Ishim -I../.. -I../../lib -I../../lib/GfxRenderer \
        -o img2_tone_proof.exe img2_tone_proof.cpp
for f in in/*.pgm; do ./img2_tone_proof.exe "$f" out; done > coverage.csv
python combine.py
```

---

## What to look at on the glass (three things)

1. **An illustrated page with tone in it** — a photo, a cover, a map. Before IMG2 this read as
   a black plate with the shape barely legible; it should now read as a picture, with the
   left/right comparison in `03_photo_side_by_side.bmp` as the reference for how much detail
   is expected.
2. **Mid-tones present, not crushed** — look for a sky, a wall, a face that is now a texture
   of dots rather than a solid field. In the host table a 31 % ink photo goes 66 % → 31 %
   black; on the device the equivalent is "the shadow side of the sphere is still
   distinguishable from the background".
3. **Speckle visible on midtones, and it stops at the paper** — fine ordered dots inside the
   picture (dither working), and a clean, solid white margin and title band around it. If the
   dots bleed into the page's white text margins, or a bright band goes grey, that is a
   different bug, not this one.

Line-art sanity, if the page has strokes on white: strokes must stay solid and paper must stay
paper — `04_lineart_side_by_side.bmp` is what that looks like on the host (13.7 % vs 13.7 %,
0 of 156 026 white pixels inked).
