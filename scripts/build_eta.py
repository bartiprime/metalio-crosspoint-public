#!/usr/bin/env python3
"""Live build progress + ETA for PlatformIO builds.

PlatformIO prints one ``Compiling`` line per translation unit, so counting them
against a known full-build total gives a usable completion percentage. The total
is taken from previous successful logs (the largest count ever observed), because
incremental builds compile fewer files.

Usage:
    python scripts/build_eta.py _scratch/build62.log
    python scripts/build_eta.py _scratch/build62.log --watch 20

Prints a single status line each sample::

    [####------]  43%  812/2813 units   elapsed 05:12   eta 06:51   tl 12:03

Exit codes: 0 = build finished OK, 2 = build FAILED, 3 = still running (--watch
without completion), 1 = usage error.
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import sys
import time

SUCCESS_RE = re.compile(r"\bSUCCESS\b")
FAILED_RE = re.compile(r"\b(FAILED|: error:)\b")
UNIT_RE = re.compile(r"^Compiling\b", re.MULTILINE)


def known_total(log_glob: str, current: int) -> int:
    """Largest Compiling count seen in any previous log, floored at 500."""
    best = 0
    for path in glob.glob(log_glob):
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                n = len(UNIT_RE.findall(fh.read()))
            best = max(best, n)
        except OSError:
            continue
    return max(best, current, 500)


def read_state(path: str) -> tuple[int, str]:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return 0, ""
    n = len(UNIT_RE.findall(text))
    tail = text[-4000:]
    if FAILED_RE.search(tail):
        return n, "failed"
    if SUCCESS_RE.search(tail):
        return n, "ok"
    return n, "running"


def bar(frac: float, width: int = 10) -> str:
    frac = max(0.0, min(1.0, frac))
    filled = int(round(frac * width))
    return "[" + "#" * filled + "-" * (width - filled) + "]"


def hms(seconds: float) -> str:
    seconds = max(0, int(seconds))
    return f"{seconds // 60:02d}:{seconds % 60:02d}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log", help="pio build log path")
    ap.add_argument("--watch", type=int, default=0,
                    help="sample every N seconds until the build ends")
    args = ap.parse_args()

    if not os.path.exists(args.log):
        print(f"no such log yet: {args.log}")
        return 1

    started = time.time()
    unit_total = 0
    last_n, last_t = 0, None
    rate = 0.0

    while True:
        n, state = read_state(args.log)
        now = time.time()
        unit_total = known_total(os.path.join(os.path.dirname(args.log), "build*.log"), n)
        if n != last_n:
            if last_t is not None and n > last_n:
                inst_rate = (n - last_n) / max(now - last_t, 1e-6)
                rate = inst_rate if rate == 0 else 0.6 * rate + 0.4 * inst_rate
            last_n, last_t = n, now

        frac = min(n / unit_total, 0.995) if unit_total else 0.0
        elapsed = now - started
        eta = (unit_total - n) / rate if rate > 0 else float("inf")
        eta_s = "  ?:??" if eta == float("inf") else hms(eta)
        tl = "  ?:??" if eta == float("inf") else hms(elapsed + eta)
        print(f"{bar(frac)} {frac * 100:3.0f}%  {n}/{unit_total} units"
              f"   elapsed {hms(elapsed)}   eta {eta_s}   tl {tl}"
              f"   [{state}]", flush=True)

        if state == "ok":
            return 0
        if state == "failed":
            return 2
        if not args.watch:
            return 3
        time.sleep(args.watch)


if __name__ == "__main__":
    sys.exit(main())
