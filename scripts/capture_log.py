#!/usr/bin/env python3
"""Capture the serial log from a variant image and summarise it.

This is the piece that lets a diagnosis happen without a human reading the screen: attach to
the port, collect everything the firmware prints, and reduce it to the few lines that matter
(reset reason, guard verdict, last action reached).

    python scripts/capture_log.py --image ext_90_guard_full --seconds 60
    python scripts/capture_log.py --image ext_92_guard_selftest --port COM4 --wait 45

Exit status
    0  log captured and the image reported reaching the heartbeat (survivor)
    1  log captured, image never reported the heartbeat (died or hung)
    2  no port ever appeared (the board cut power / USB never enumerated)

Logs: _scratch/logs/<image>_<timestamp>.log
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import time

ROOT = r"M:\_HermesWork\crosspoint-fork"
LOGDIR = os.path.join(ROOT, "_scratch", "logs")

KEEP = re.compile(r"(rst:0x|boot:|CP-GUARD|CP-EXT|CP-ANCHOR|CP-BOOT|Guru Meditation|"
                  r"panic|abort\(\)| watchdog|Stack canary|invalid header|"
                  r"No bootable app|ets |entry 0x|Internal error|assert)", re.I)
ALIVE = re.compile(r"CP-GUARD\] sweep complete|CP-ANCHOR\] setup reached", re.I)


def wait_port(name: str, limit: float) -> str | None:
    try:
        from serial.tools import list_ports
    except ImportError:
        return name
    end = time.time() + limit
    while time.time() < end:
        for p in list_ports.comports():
            if p.device.upper() == name.upper():
                return p.device
        time.sleep(0.5)
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--port", default=os.environ.get("CP_PORT", "COM4"))
    ap.add_argument("--seconds", type=float, default=45)
    ap.add_argument("--wait", type=float, default=30, help="seconds to wait for the port")
    ap.add_argument("--baud", type=int, default=115200)
    a = ap.parse_args()

    import serial

    os.makedirs(LOGDIR, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    path = os.path.join(LOGDIR, f"{a.image}_{stamp}.log")

    port = wait_port(a.port, a.wait)
    if not port:
        print(f"NO PORT ({a.port} never appeared) - the board is not enumerating USB")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(f"# {a.image}: no port after {a.wait}s\n")
        return 2

    lines: list[str] = []
    alive = False
    try:
        with serial.Serial(port, a.baud, timeout=1) as ser:
            ser.reset_input_buffer()
            end = time.time() + a.seconds
            while time.time() < end:
                raw = ser.readline()
                if not raw:
                    continue
                txt = raw.decode("utf-8", "replace").rstrip("\r\n")
                if not txt:
                    continue
                lines.append(txt)
                if ALIVE.search(txt):
                    alive = True
                if KEEP.search(txt) or len(lines) <= 5:
                    print(txt)
    except Exception as exc:                                  # noqa: BLE001
        print(f"serial error: {exc}")

    with open(path, "w", encoding="utf-8") as fh:
        fh.write(f"# image={a.image} port={port} captured={stamp}\n")
        fh.write("\n".join(lines) + "\n")

    print(f"\n--- {len(lines)} lines -> {path}")
    if not lines:
        print("VERDICT: silent (no output at all)")
        return 1
    print("VERDICT:", "SURVIVOR (reached heartbeat)" if alive else "DIED / HUNG (no heartbeat)")
    return 0 if alive else 1


if __name__ == "__main__":
    sys.exit(main())
