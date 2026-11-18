#!/usr/bin/env python3
"""Unattended flash -> boot -> capture -> diagnose loop.

This is the automation you asked for, with its honest limits built in:

  * It flashes, waits for the port, captures the boot log, parses a verdict, and moves on to
    the next image. Fully hands-free for images that keep USB alive.
  * When a flash produces NO port (the board cut its own power), no firmware and no script can
    restore it -- that is the physical failure. It records the image as `dead` and, after
    `--max-dead` consecutive no-port outcomes, stops rather than burning through the queue.

    python scripts/auto_flash.py --images ext_90_guard_full ext_92_guard_selftest
    python scripts/auto_flash.py --schedule          # walk test_session.py's order
    python scripts/auto_flash.py --images ... --dry-run

Everything it learns is written back through test_session.py, so a session interrupted at any
point resumes where it left off.
"""
from __future__ import annotations

import argparse
import importlib.util
import os
import re
import subprocess
import sys

ROOT = r"M:\_HermesWork\crosspoint-fork"
PROJ = os.path.join(ROOT, "crosspoint-reader")
STAGED = os.path.join(ROOT, "_scratch", "staged")
PY = r"M:\hf\penv\Scripts\python.exe"
PORT = os.environ.get("CP_PORT", "COM4")


def load(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(name, os.path.join(PROJ, "scripts", filename))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def flash(image: str) -> bool:
    target = os.path.join("_scratch", "staged", image)
    print(f"\n=== flashing {image}")
    rc = subprocess.call(["bash", os.path.join(STAGED, "flash_staged.sh"), PORT, target], cwd=ROOT)
    return rc == 0


def capture(image: str, seconds: float) -> tuple[int, bool]:
    """Return (capture exit code, reached_heartbeat)."""
    cmd = [PY, os.path.join(PROJ, "scripts", "capture_log.py"),
           "--image", image, "--port", PORT, "--seconds", str(seconds), "--wait", "25"]
    out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    text = (out.stdout or "") + (out.stderr or "")
    # print only the verdict line to keep the transcript small
    for line in text.splitlines():
        if line.startswith(("VERDICT", "NO PORT", "---")):
            print("   " + line)
    alive = "SURVIVOR" in text
    dead_no_port = "NO PORT" in text
    return (2 if dead_no_port else (0 if alive else 1)), alive


def main() -> int:
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--images", nargs="+")
    g.add_argument("--schedule", action="store_true")
    ap.add_argument("--seconds", type=float, default=40)
    ap.add_argument("--max-dead", type=int, default=3,
                    help="stop after this many consecutive no-port flashes")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    session = load("ts", "test_session.py")

    if a.schedule:
        st = session.load()
        images = [n for n in st["order"] if n not in st["results"] and session.available(n)]
    else:
        images = a.images

    print(f"{len(images)} image(s): {', '.join(images[:6])}"
          f"{' ...' if len(images) > 6 else ''}")
    if a.dry_run:
        return 0

    st = session.load()
    consecutive_dead = 0
    for img in images:
        if not session.available(img):
            print(f"!! not staged: {img} - skipping")
            continue
        if not flash(img):
            print(f"!! flash failed for {img}")
            continue
        code, alive = capture(img, a.seconds)
        st = session.load()
        st.setdefault("results", {})
        st["results"][img] = {"result": "ok" if alive else ("dead:0" if code == 2 else "dead:hung"),
                              "at": subprocess.run(["date"], capture_output=True).stdout.decode().strip()}
        st["last_flashed"] = img
        session.save(st)
        session.render(st)

        if code == 2:
            consecutive_dead += 1
            print(f"   no port - needs a physical power cycle ({consecutive_dead}/{a.max_dead})")
            if consecutive_dead >= a.max_dead:
                print("\nstopping: the board is not enumerating; hand it back to a human")
                break
        else:
            consecutive_dead = 0

    print("\n=== session state ===")
    subprocess.call([PY, os.path.join(PROJ, "scripts", "test_session.py"), "--status"], cwd=ROOT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
