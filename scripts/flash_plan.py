#!/usr/bin/env python3
"""Render the flash plan: what to flash, in what order, and what each result means.

Why this exists
---------------
`test_session.py` holds the order and `MANIFEST.csv` holds each image's purpose and sha256, but
nothing joined them. On the morning of a hardware session that meant reading a 170-name schedule
and a 216-row CSV side by side and hoping no image was mis-identified. This prints one card per
image: sequence number, name, its recorded purpose, group, and the first 12 hex of its sha256 so
the flashed bytes can be checked against what was audited.

    python scripts/flash_plan.py              # first 20
    python scripts/flash_plan.py -n 5
    python scripts/flash_plan.py --all
    python scripts/flash_plan.py --gaps       # images scheduled but absent from MANIFEST.csv
"""
from __future__ import annotations

import argparse
import csv
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
STAGED = os.path.join(ROOT, "_scratch", "staged")
MANIFEST = os.path.join(STAGED, "MANIFEST.csv")


def load_schedule() -> list[str]:
    spec = importlib.util.spec_from_file_location("ts_plan", os.path.join(HERE, "test_session.py"))
    if spec is None or spec.loader is None:
        raise SystemExit("cannot load test_session.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return list(mod.SCHEDULE)


def load_manifest() -> dict[str, dict[str, str]]:
    out: dict[str, dict[str, str]] = {}
    if not os.path.isfile(MANIFEST):
        return out
    with open(MANIFEST, newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            name = (row.get("variant") or "").strip()
            if name:
                out[name] = row
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=20, help="how many to show (default 20)")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--gaps", action="store_true", help="list scheduled images missing from MANIFEST.csv")
    a = ap.parse_args()

    schedule = load_schedule()
    manifest = load_manifest()
    staged = {d for d in os.listdir(STAGED) if os.path.isdir(os.path.join(STAGED, d))}

    if a.gaps:
        gaps = [n for n in schedule if n not in manifest]
        print(f"{len(gaps)} scheduled image(s) absent from MANIFEST.csv "
              f"({len(manifest)} rows total):")
        for n in gaps:
            print(f"  {n}{'' if n in staged else '   [NOT STAGED]'}")
        return 0

    show = schedule if a.all else schedule[: a.n]
    print(f"flash plan -- {len(schedule)} images scheduled, "
          f"{sum(1 for n in schedule if n in staged)} staged, "
          f"{sum(1 for n in schedule if n not in manifest)} missing from MANIFEST\n")
    print(f"{'#':>3} {'image':26s} {'sha256':12s} {'flags':34s} purpose")
    print("-" * 118)
    for i, name in enumerate(show, 1):
        row = manifest.get(name, {})
        sha = (row.get("sha256") or "")[:12] or "-"
        flags = (row.get("flags") or "").strip()[:34] or "-"
        purpose = (row.get("purpose") or "").strip()
        if not purpose:
            purpose = "(no manifest row)" if name not in manifest else ""
        missing = "" if name in staged else "  !! NOT STAGED"
        print(f"{i:3d} {name:26s} {sha:12s} {flags:34s} {purpose}{missing}")
    if not a.all and len(schedule) > a.n:
        print(f"... {len(schedule) - a.n} more (use --all)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
