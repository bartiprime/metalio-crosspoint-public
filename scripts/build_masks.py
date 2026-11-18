#!/usr/bin/env python3
"""Build the BISECT family: masks designed for rapid narrowing, not for singletons.

The S group already holds every single action (16), cumulative prefixes (14) and
leave-one-out (14). What it lacks is the shape you actually want during a live session:
images that cut the search space in HALF.

    halves   -> which half contains the killer
    quarters -> which quarter
    pairs    -> the adjacent pair
    settle   -> is the fault instantaneous or load-dependent

Each image is the same anchor build plus `-DCP_STEP_MASK=<mask>`; no new source is needed.

    python scripts/build_masks.py --list
    python scripts/build_masks.py --only bis_half_lo
    python scripts/build_masks.py
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("bv", os.path.join(HERE, "build_variants.py"))
bv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bv)

BASE = bv.BASE
STAGED = bv.STAGED
NAMES = bv.ACTION_NAMES

# bit 14 is "ensureBooted() FULL" -- an opaque whole, useless inside a bisection mask.
REAL_BITS = [b for b in range(16) if b != 14]


def m(bits: list[int]) -> int:
    return sum(1 << b for b in bits)


BISECT: dict[str, tuple[int, str]] = {}

# --- halves: 8 images ------------------------------------------------------
BISECT["bis_half_lo"] = (m(REAL_BITS[:8]), "actions 0-7 only (lower half)")
BISECT["bis_half_hi"] = (m(REAL_BITS[8:]), "actions 8-15 only (upper half)")
BISECT["bis_half_expander"] = (m([0, 1, 2, 12, 13]), "expander writes + rail latch + TPS power-on")
BISECT["bis_half_plain_gpio"] = (m(list(range(3, 12))), "every plain-GPIO action (3..11)")
BISECT["bis_half_epd"] = (m([9, 10, 11, 15]), "EPD trio, both forms")
BISECT["bis_half_inputs"] = (m([3, 4, 5]), "the three input-pin modes")
BISECT["bis_half_outputs_low"] = (m([6, 7, 8, 9, 10, 11]), "every pin driven LOW")
BISECT["bis_half_odd"] = (m([b for b in REAL_BITS if b % 2]), "odd-numbered actions")
BISECT["bis_half_even"] = (m([b for b in REAL_BITS if b % 2 == 0]), "even-numbered actions")

# --- quarters: 4 images ----------------------------------------------------
BISECT["bis_q1_0_3"] = (m([0, 1, 2, 3]), "actions 0-3")
BISECT["bis_q2_4_7"] = (m([4, 5, 6, 7]), "actions 4-7")
BISECT["bis_q3_8_11"] = (m([8, 9, 10, 11]), "actions 8-11")
BISECT["bis_q4_12_15"] = (m([12, 13, 15]), "actions 12-15")

# --- two tracks over the same 13 masks ------------------------------------
# 13 plain + 13 NVS-traced = 26 images, the same count as the previously planned set.
# Plain is the guarantee (works even if all power is cut); NVS-traced is the upgrade that
# reports the exact action and recovers, and only pays off if ext_96_nvs_probe passes.
CORE = {k[len("bis_"):]: v for k, v in BISECT.items() if k.startswith(("bis_half_", "bis_q"))}
PLAIN = {f"bis_{k}": v for k, v in CORE.items()}
NVS = {f"nv_{k}": v for k, v in CORE.items()}

# --- leave-one-out, NVS-traced: 14 images ---------------------------------
# s_loo_* already omits one action from the first fourteen, but those images are silent and
# unrecoverable. These are the same masks with the NVS trace: if a LOO image still dies, there
# are TWO killers, and the trace says which one fired second.
LOO_BASE = (1 << 14) - 1
LOO_NVS = {f"nv_loo_{b:02d}": (LOO_BASE & ~(1 << b),
           f"all of 0..13 except bit {b} ({NAMES[b]}), NVS-traced")
           for b in range(14)}

ALL = {**PLAIN, **NVS, **LOO_NVS}
BISECT = ALL


def flags_for(name: str) -> list[str]:
    mask, _ = BISECT[name]
    if name.startswith("nv_"):
        # mode 99 runs the mask with the NVS trace; the mask travels in CP_NVS_MASK so it
        # cannot interact with CP_STEP_MASK's own executor in src/main.cpp.
        return BASE + ["CP_BRINGUP_MINIMAL", "CP_BRINGUP_EXT", "CP_EXT_MODE=99",
                       f"CP_NVS_MASK={mask}"]
    return BASE + ["CP_BRINGUP_MINIMAL", f"CP_STEP_MASK={mask}"]


def _apply_flags(name: str) -> None:
    """Header mode when available: recompiles 2 TUs. Legacy otherwise: full rebuild."""
    fl = flags_for(name)
    if bv.header_mode_available():
        bv.set_header(fl)
        bv.touch_header()
    else:
        bv.set_flags(fl)
        bv.touch_flag_consumers()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    if a.list:
        for n, (mask, why) in BISECT.items():
            print(f"{n:24s} 0x{mask:04x}  {why}")
        print(f"\ntotal: {len(BISECT)}")
        return 0

    names = a.only or list(BISECT)
    results, seen = {}, {}
    for name in names:
        if name not in BISECT:
            print("unknown:", name)
            return 1
        mask, why = BISECT[name]
        print(f"\n=== {name}  mask=0x{mask:04x}  {why}", flush=True)
        _apply_flags(name)
        ok = bv.build(name) and bv.stage(name) > 0
        if ok:
            dg = hashlib.sha256(open(os.path.join(STAGED, name, "app_full.bin"), "rb").read()).hexdigest()
            if dg in seen:
                print(f"[{name}] !! WARNING identical to {seen[dg]} - flags did not take effect")
            seen[dg] = name
            print(f"[{name}] sha256={dg[:16]}", flush=True)
            bv.write_manifest([(name, "BIS", " ".join("-D" + f for f in flags_for(name)), why,
                                os.path.getsize(os.path.join(STAGED, name, "app_full.bin")), dg)])
        results[name] = ok

    print("\n=== summary ===")
    for k, v in results.items():
        print(f"  {k:26s} {'ok' if v else 'FAILED'}")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
