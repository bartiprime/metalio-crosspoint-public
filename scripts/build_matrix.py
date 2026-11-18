#!/usr/bin/env python3
"""Divergence matrix + PWRUP timing sweep.

Four documented divergences exist between our bring-up and shipped firmware:

    H = gpio_hold released first
    L = CAM_SCR / TP_RST / BT_PA latched HIGH at power-up
    S = PMIC UPSEQ0/1 programmed
    P = PWRUP raised during boot

Single-divergence images tell us which one is fatal. The full combination matrix additionally tells
us which divergences are **irrelevant** — and that matters, because the shipping patch should not
carry unrelated changes just because the board happened to survive with all of them applied.

The timing sweep answers a different question: if raising PWRUP at boot is wrong, is it wrong
*always*, or only while the rails are still settling? That decides between deleting the call and
deferring it.

    python scripts/build_matrix.py --list
    python scripts/build_matrix.py
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

MATRIX: dict[str, tuple[dict[str, str], str]] = {}

# --- 16 combinations: H L S P -------------------------------------------------
for h in (1, 0):
    for l in (1, 0):
        for s_ in (1, 0):
            for p_ in (1, 0):
                name = f"mx_{h}{l}{s_}{p_}"
                desc = " + ".join(x for x, on in
                                  (("holds", h), ("latches", l), ("upseq", s_), ("pwrup", p_)) if on) or "nothing"
                MATRIX[name] = ({"CP_MX_H": str(h), "CP_MX_L": str(l),
                                 "CP_MX_S": str(s_), "CP_MX_P": str(p_)}, desc)

# --- 6 timing points: full vendor state, PWRUP deferred -----------------------
for ms in (0, 250, 500, 1000, 2000, 5000):
    MATRIX[f"pwr_delay_{ms:04d}"] = ({"CP_PWRUP_DELAY_MS": str(ms)},
                                     f"holds+latches+upseq, PWRUP after {ms} ms")


def flags_for(name: str) -> list[str]:
    extra, _ = MATRIX[name]
    mode = "121" if name.startswith("pwr_delay_") else "120"
    return (BASE + ["CP_BRINGUP_MINIMAL", "CP_BRINGUP_EXT", f"CP_EXT_MODE={mode}"]
            + [f"{k}={v}" for k, v in extra.items()])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    if a.list:
        for n, (extra, why) in MATRIX.items():
            print(f"{n:20s} {' '.join(f'-D{k}={v}' for k, v in extra.items()):40s} {why}")
        print(f"\ntotal: {len(MATRIX)}")
        return 0

    names = a.only or list(MATRIX)
    results, seen = {}, {}
    for name in names:
        if name not in MATRIX:
            print("unknown:", name)
            return 1
        _, why = MATRIX[name]
        print(f"\n=== {name}  {why}", flush=True)
        fl = flags_for(name)
        if bv.header_mode_available():
            bv.set_header(fl)
            bv.touch_header()
        else:
            bv.set_flags(fl)
            bv.touch_flag_consumers()
        ok = bv.build(name) and bv.stage(name) > 0
        if ok:
            dg = hashlib.sha256(open(os.path.join(STAGED, name, "app_full.bin"), "rb").read()).hexdigest()
            if dg in seen:
                print(f"[{name}] !! WARNING identical to {seen[dg]} - flags did not take effect")
            seen[dg] = name
            print(f"[{name}] sha256={dg[:16]}", flush=True)
            bv.write_manifest([(name, "MX", " ".join("-D" + f for f in fl), why,
                                os.path.getsize(os.path.join(STAGED, name, "app_full.bin")), dg)])
        results[name] = ok

    print("\n=== summary ===")
    for k, v in results.items():
        print(f"  {k:24s} {'ok' if v else 'FAILED'}")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
