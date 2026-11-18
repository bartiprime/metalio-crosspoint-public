#!/usr/bin/env python3
"""Decode the ESP32 partition table read back from the device and decide the flash offset.

Why this exists
---------------
`layout.env` (the write path) and `partitions.csv` (the build) deliberately describe two
different tables, and the audit can only check that `layout.env` matches one of two KNOWN
tables -- it cannot see the device. The pre-flight readback is the only authority, and it
should not be decoded by eye at the bench.

    python scripts/decode_pt.py _scratch/pt_readback.bin          # decode + verdict
    python scripts/decode_pt.py "CrossLink Pixel.bin" --offset 0x8000   # self-test on a dump

Exit codes: 0 = factory layout (proceed), 1 = replacement layout (proceed after editing
layout.env), 2 = unrecognised layout or bad table (STOP).
"""
from __future__ import annotations

import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LAYOUT_ENV = os.path.join(ROOT, "_scratch", "staged", "layout.env")

MAGIC = b"\xaa\x50"
TYPES = {0x00: "app", 0x01: "data"}
APP_SUBTYPES = {0x00: "factory", 0x10: "ota_0", 0x11: "ota_1", 0x20: "test"}
DATA_SUBTYPES = {0x00: "ota", 0x01: "phy", 0x02: "nvs", 0x04: "coredump",
                 0x81: "fat", 0x82: "spiffs", 0x83: "littlefs"}

# The two candidates recorded in the project. (app_off, otadata_off)
FACTORY = (0x20000, 0xF000)
REPLACEMENT = (0x10000, 0xD000)


def decode(blob: bytes) -> list[dict]:
    entries = []
    for i in range(0, len(blob) - 32 + 1, 32):
        e = blob[i:i + 32]
        if e[:2] == b"\xff\xff":
            break
        if e[:2] != MAGIC:
            if i == 0:
                print(f"not a partition table: magic {e[:2].hex()} != aa50 -> STOP")
                raise SystemExit(2)
            break
        ptype, subtype = e[2], e[3]
        offset = int.from_bytes(e[4:8], "little")
        size = int.from_bytes(e[8:12], "little")
        label = e[12:28].split(b"\x00")[0].decode("ascii", "replace")
        flags = int.from_bytes(e[28:32], "little")
        tname = TYPES.get(ptype, f"0x{ptype:02x}")
        sname = (APP_SUBTYPES if ptype == 0 else DATA_SUBTYPES).get(subtype, f"0x{subtype:02x}")
        entries.append({"label": label, "type": tname, "sub": sname,
                        "offset": offset, "size": size, "flags": flags})
    return entries


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("blob", help="file containing the partition table")
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=None,
                    help="read the table from this offset inside the file (e.g. 0x8000)")
    ap.add_argument("--size", type=lambda s: int(s, 0), default=0x1000)
    a = ap.parse_args()

    with open(a.blob, "rb") as fh:
        if a.offset is not None:
            fh.seek(a.offset)
        blob = fh.read(a.size)

    entries = decode(blob)
    print(f"{a.blob}" + (f" @ {a.offset:#x}" if a.offset is not None else ""))
    print(f"{'label':12s} {'type':6s} {'subtype':10s} {'offset':>10s} {'size':>10s}")
    print("-" * 54)
    for e in entries:
        print(f"{e['label']:12s} {e['type']:6s} {e['sub']:10s} "
              f"{e['offset']:#10x} {e['size']:#10x}")

    app = next((e for e in entries if e["sub"] in ("ota_0", "factory")), None)
    ota = next((e for e in entries if e["sub"] == "ota"), None)
    if not app:
        print("\nVERDICT: no ota_0/factory app partition found -> STOP, do not flash")
        return 2
    app_off = app["offset"]
    ota_off = ota["offset"] if ota else None

    configured = {}
    if os.path.isfile(LAYOUT_ENV):
        for line in open(LAYOUT_ENV, encoding="utf-8"):
            if "=" in line and not line.lstrip().startswith("#"):
                k, v = line.strip().split("=", 1)
                configured[k] = v

    print(f"\napp partition '{app['label']}' at {app_off:#x}, size {app['size']:#x}")
    print(f"otadata at {ota_off:#x}" if ota_off is not None else "otadata: not present")

    known = {FACTORY: "FACTORY (verified by a digest-matched flash)",
             REPLACEMENT: "REPLACEMENT (produced 'No bootable app partitions')"}
    layout = known.get((app_off, ota_off))
    if layout is None:
        print("\nVERDICT: unrecognised layout -> STOP. Neither known table applies; "
              "flashing the app would be a guess.")
        return 2

    print(f"\nVERDICT: matches the {layout}")
    want = configured.get("APP_OFF")
    if want is not None:
        if int(want, 0) == app_off:
            print(f"layout.env APP_OFF={want} AGREES with the device -> proceed")
        else:
            print(f"layout.env APP_OFF={want} DISAGREES with the device ({app_off:#x}).\n"
                  f"  Fix before flashing:  APP_OFF={app_off:#x}  OTADATA_OFF={ota_off:#x}  "
                  f"in _scratch/staged/layout.env")
    return 0 if (app_off, ota_off) == FACTORY else 1


if __name__ == "__main__":
    sys.exit(main())
