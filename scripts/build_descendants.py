#!/usr/bin/env python3
"""Build descendants of a known-surviving image.

The point of this script is inheritance. Once an image survives (COM stays up), every later
question should be asked *from that safe base*, so the whole descendant branch is autonomous:
it boots, it talks, and a human never has to press a button.

A child is the parent's exact mask plus one delta, expressed as a single `-DCP_STEP_MASK`:

    parent mask 0x003f  (survivor)
      + bit 9  ->  0x023f   "parent plus EPD XOE"
      - bit 6  ->  0x001f   "parent minus frontlight COOL"

Both directions matter. `--add` grows capability toward a full boot; `--remove` shrinks toward
the minimal set the board tolerates. Every child inherits the parent's survival, so children are
safe to test unattended.

    python scripts/build_descendants.py --base s_combo_expander --add-all --list
    python scripts/build_descendants.py --base s_combo_expander --add 9 10 11
    python scripts/build_descendants.py --mask 0x003f --add-all
    python scripts/build_descendants.py --base s_combo_expander --remove 1

Lineage is appended to _scratch/staged/LINEAGE.csv so the family tree survives a context reset.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def _mod(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, filename))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


bv = _mod("bv", "build_variants.py")
BASE = bv.BASE
STAGED = bv.STAGED
NAMES = bv.ACTION_NAMES
REAL_BITS = [b for b in range(16) if b != 14]   # bit 14 = opaque whole ensureBooted()
LINEAGE = os.path.join(STAGED, "LINEAGE.csv")


def mask_of(name: str) -> int | None:
    """Read the mask out of an existing variant's recorded flags."""
    entry = bv.VARIANTS.get(name)
    if not entry:
        return None
    for f in entry[0]:
        if f.startswith("CP_STEP_MASK="):
            return int(f.split("=", 1)[1], 0)
    return None


def children(parent_mask: int, add: list[int], remove: list[int], add_all: bool) -> dict[str, tuple[int, str]]:
    out: dict[str, tuple[int, str]] = {}
    if add_all:
        add = [b for b in REAL_BITS if not (parent_mask >> b) & 1]
    for b in add:
        m = parent_mask | (1 << b)
        out[f"d_add{b:02d}"] = (m, f"parent + bit {b} ({NAMES.get(b, '?')})")
    for b in remove:
        m = parent_mask & ~(1 << b)
        out[f"d_sub{b:02d}"] = (m, f"parent - bit {b} ({NAMES.get(b, '?')})")
    return out


def flags_for(mask: int) -> list[str]:
    return BASE + ["CP_BRINGUP_MINIMAL", f"CP_STEP_MASK={mask}"]


def main() -> int:
    ap = argparse.ArgumentParser()
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--base", help="existing variant name whose mask is the parent")
    src.add_argument("--mask", help="parent mask, e.g. 0x003f")
    ap.add_argument("--add", nargs="*", type=int, default=[])
    ap.add_argument("--remove", nargs="*", type=int, default=[])
    ap.add_argument("--add-all", action="store_true", help="one child per remaining bit")
    ap.add_argument("--tag", default="", help="label folded into child names")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    if a.base:
        pm = mask_of(a.base)
        if pm is None:
            print(f"cannot find a CP_STEP_MASK for '{a.base}'")
            print("mask-carrying groups are S, S2, T, BIS and previous descendants")
            return 1
        parent_name = a.base
    else:
        pm = int(a.mask, 0)
        parent_name = f"mask_{pm:04x}"

    kids = children(pm, list(a.add), list(a.remove), a.add_all)
    if not kids:
        print("no children requested (use --add / --remove / --add-all)")
        return 1

    tag = f"_{a.tag}" if a.tag else ""
    print(f"parent {parent_name} mask=0x{pm:04x} -> {len(kids)} child(ren)")
    for k, (m, why) in kids.items():
        print(f"  {parent_name}{tag}__{k:12s} 0x{m:04x}  {why}")
    if a.list:
        return 0

    results = {}
    for k, (m, why) in kids.items():
        name = f"{parent_name}{tag}__{k}"
        print(f"\n=== {name}  mask=0x{m:04x}  {why}", flush=True)
        bv.set_flags(flags_for(m))
        bv.touch_flag_consumers()
        ok = bv.build(name) and bv.stage(name) > 0
        if ok:
            dg = hashlib.sha256(open(os.path.join(STAGED, name, "app_full.bin"), "rb").read()).hexdigest()
            bv.write_manifest([(name, "DESC", " ".join("-D" + f for f in flags_for(m)), why,
                                os.path.getsize(os.path.join(STAGED, name, "app_full.bin")), dg)])
            with open(LINEAGE, "a", encoding="utf-8") as fh:
                fh.write(f"{name},{parent_name},0x{pm:04x},0x{m:04x},{why},{dg[:16]}\n")
        results[name] = ok

    print("\n=== summary ===")
    for k, v in results.items():
        print(f"  {k:34s} {'ok' if v else 'FAILED'}")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
