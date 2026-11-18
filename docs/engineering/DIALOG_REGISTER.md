# DIALOG REGISTER — dialog liveness (pointer)

**This document lives in the docs repo. The canonical copy is:**

`crosspoint-fork/docs/engineering/DIALOG_REGISTER.md`

It holds the register of **every** dialog/popup in `src/` (draw site, drive site, drive mode, what it
covers, how long that takes, verdict), the liveness contract, the per-path verdicts and the
"how to add a dialog correctly" checklist. **It adds no rules of its own; if the register and this stub
ever disagree, the canonical file wins.**

**The contract, in one paragraph (so a reader standing here is not left guessing):**

> Any operation that can leave the glass unchanged for longer than a blink must say so **on the glass** —
> a named dialog, **actually driven** (`displayBuffer()` returned with the box in it). No path may ask the
> operator to infer liveness from a delayed repaint. `BaseTheme::drawPopup()` commits its own `HALF`;
> `commit=false` is legal only when the caller is about to drive a whole replacement frame from a cleared
> canvas (the sleep screen), and such a path owes a `DeferredPopupDrive`-style guard so a frame that
> paints nothing still pays the enter-paint debt. A message with no drive of its own goes into a drive the
> path already performs — `src/display/StatusFrame.h`.

**Policy (stated once, here and in `crosspoint-fork/docs/INDEX.md`):** the docs repo owns **all**
engineering and planning documents. This repo's `docs/engineering/` is **deprecated**. Do not add new
engineering documents here — add them to `crosspoint-fork/docs/engineering/` and list them in
`crosspoint-fork/docs/INDEX.md`. If a stale link sent you here, follow the path above.
