# UI standards — standing rules (pointer)

**This document lives in the docs repo. The canonical copy is:**

`crosspoint-fork/docs/engineering/UI_STANDARDS.md`

Code in this repo cites it by that path — `src/activities/settings/BatteryStatsActivity.cpp:187`,
`:227`, `:236`, `:273` and `BatteryStatsActivity.h:76` all say "UI-STD-1 / UI-STD-2
(docs/engineering/UI_STANDARDS.md)" — so this stub exists only to make a docs-repo citation
resolvable from this tree. **It adds no rules of its own; if the rule and this stub ever disagree, the
canonical file wins.**

**The two rules, one line each (so a reader standing here is not left guessing):**

- **`UI-STD-1`** — every interactive element's hit target must be at least **1.5 × `theme.minTouchSize`**
  (44 px SDK default → **>= 66 px**; Andro's 54 px → **>= 81 px**). Pad `minTouchSize`, never the drawn
  glyph. No exceptions: a control below the floor is a layout bug. Any report on a screen must quote the
  hit-rect in pixels **and** the multiple.
- **`UI-STD-2`** — a control that appears to do nothing is a **defect**, not a placeholder. If the action
  is a no-op, do not draw the control, or draw it disabled with a reason.

**Policy (stated once, here and in `crosspoint-fork/docs/INDEX.md`):** the docs repo owns **all**
engineering and planning documents. This repo's `docs/engineering/` is **deprecated** — it holds
leftovers that predate the split. The rest of this repo's `docs/` is upstream CrossPoint product
documentation, which this repo does own and should keep.

Do not add new engineering documents here. Add them to `crosspoint-fork/docs/engineering/` and list them
in `crosspoint-fork/docs/INDEX.md`. If a stale link sent you here, follow the path above.
