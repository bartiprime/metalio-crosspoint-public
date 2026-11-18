#!/usr/bin/env python3
"""Reactor: when a test result comes back SURVIVOR, build its descendants automatically.

This is the inheritance loop. `test_session.py` records outcomes; the moment an image with a
`-DCP_STEP_MASK` survives, its children (parent mask +/- one action) are exactly the safe base
for every later question — so they get built without anyone asking.

Rules
  * Only reacts to results recorded as `ok` (survived to the heartbeat).
  * Skips while any compiler is running, so it never competes with a batch.
  * Builds `--add-all` children (one per remaining action), capped by --max-children.
  * Never rebuilds: children already staged are skipped, and expanded parents are remembered
    in _scratch/reactor_expanded.json.

Log: _scratch/reactor.log
    python scripts/reactor.py --dry-run
    python scripts/reactor.py --poll 60
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import subprocess
import sys
import time

ROOT = r"M:\_HermesWork\crosspoint-fork"
PROJ = os.path.join(ROOT, "crosspoint-reader")
SCR = os.path.join(PROJ, "scripts")
STATE = os.path.join(ROOT, "_scratch", "test_session.json")
SEEN = os.path.join(ROOT, "_scratch", "reactor_expanded.json")
LOG = os.path.join(ROOT, "_scratch", "reactor.log")
PY = r"M:\hf\penv\Scripts\python.exe"


def log(msg: str) -> None:
    line = f"{time.strftime('%H:%M:%S')}  {msg}"
    print(line, flush=True)
    with open(LOG, "a", encoding="utf-8") as fh:
        fh.write(line + "\n")


def compilers_alive() -> bool:
    try:
        out = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "(Get-Process pio,riscv32-esp-elf-g++,lto-wrapper -ErrorAction SilentlyContinue).Count"],
            capture_output=True, text=True, timeout=60)
        return int((out.stdout or "0").strip() or "0") > 0
    except Exception:
        return True


def load_json(path: str, default):
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, json.JSONDecodeError):
        return default


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--poll", type=int, default=60)
    ap.add_argument("--max-children", type=int, default=13)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    log("=== reactor started ===")
    seen = set(load_json(SEEN, {"expanded": []}).get("expanded", []))

    while True:
        if compilers_alive():
            time.sleep(a.poll)
            continue
        st = load_json(STATE, {})
        results = st.get("results", {})
        todo = [n for n, r in results.items()
                if str(r.get("result", "")).startswith("ok") and n not in seen]
        if not todo:
            time.sleep(a.poll)
            continue

        for parent in todo:
            spec = importlib.util.spec_from_file_location("bd", os.path.join(SCR, "build_descendants.py"))
            bd = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(bd)
            pm = bd.mask_of(parent)
            if pm is None:
                log(f"{parent}: survivor without a mask (EXT/image family) - nothing to inherit")
                seen.add(parent)
                continue
            kids = bd.children(pm, [], [], True)
            staged = [k for k in kids if os.path.isdir(os.path.join(bd.STAGED, f"{parent}__{k}"))]
            fresh = [k for k in kids if k not in staged][: a.max_children]
            log(f"{parent} (mask 0x{pm:04x}) survived -> {len(fresh)} new descendant(s)")
            if a.dry_run:
                seen.add(parent)
                continue
            if fresh:
                rc = subprocess.call([PY, os.path.join(SCR, "build_descendants.py"),
                                      "--base", parent, "--add-all"], cwd=ROOT)
                log(f"{parent}: build_descendants exit {rc}")
            seen.add(parent)
            json.dump({"expanded": sorted(seen)}, open(SEEN, "w", encoding="utf-8"), indent=2)

        time.sleep(a.poll)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        log("=== reactor stopped ===")
