# BATCH r101.9 — RESULT (pointer)

**This document lives in the docs repo. The canonical copy is:**

`crosspoint-fork/docs/engineering/BATCH_R1019_RESULT.md`

It is the operator-facing record of what batch r101.9 actually shipped: the final image facts
(`build-r1019/crosspoint_metalio.bin`, 6,715,664 B, sha256 `5cef8ddc…`, stamp `MetalioEinkPlus
r101.9`), the QA1 outcome (0 P0 / 3 P1 fixed in `8154b5fe` / 9 P2 deferred), one row per item with
its commit and its single line of evidence, the **known-unverified** list (the LIB1 host harness
numbers are worker-run and were not independently reproduced; LOCK1's nested take is unproven on
hardware; real-card search and series ordering are unmeasured), and the numbered **morning checklist**
the operator walks once the image is flashed.

**Status it records: BUILT, COMMITTED, NOT FLASHED — nothing in the batch has been seen on glass.**

This repo's `docs/engineering/` is deprecated (`crosspoint-fork/docs/INDEX.md` states the policy):
engineering and planning documents live in `crosspoint-fork/docs/`, and this path holds a pointer so
a session standing in the firmware tree is not left guessing. If the pointer and the canonical file
ever disagree, the canonical file wins.
