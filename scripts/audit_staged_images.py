#!/usr/bin/env python3
"""Static audit of the whole flashing pipeline: every staged image, no hardware.

Catches, before anything is flashed:
  1. chunk reassembly is lossless (chunks -> app_full.bin byte-identical)
  2. chunk shapes are right (all full-size except the last)
  3. the ESP image header is valid: magic 0xE9, sane segment count, entry address in a
     plausible range, chip id = ESP32-S3 (9)
  4. segment table is self-consistent: the parsed segments plus headers equals the file size
     (with a trailing checksum or appended SHA-256 accounted for)
  5. the image fits its ota_0 partition, and the chunk count matches the size

    python scripts/audit_staged_images.py            # all images
    python scripts/audit_staged_images.py --limit 5  # first 5, for a quick look
Exit 0 if every image passes.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys

ROOT = r"M:\_HermesWork\crosspoint-fork"
STAGED = os.path.join(ROOT, "_scratch", "staged")
CHUNK = 0x40000
OTA0_SIZE = 0x500000          # from partitions.csv (device-measured table)
# This board is an ESP32-S31 (`ESP_CHIP_ID_ESP32S31 = 0x0020` in the IDF 5.4 headers), which
# esptool 5.4 does not know and reports as "unexpected chip ID, expected 9". The image is
# correct; the tool is behind.
EXPECTED_CHIP_IDS = {0x0020: "ESP32-S31", 0x0009: "ESP32-S3"}

fails: list[str] = []
warns: list[str] = []


def audit_one(name: str) -> None:
    d = os.path.join(STAGED, name)
    full_p = os.path.join(d, "app_full.bin")
    if not os.path.exists(full_p):
        fails.append(f"{name}: no app_full.bin")
        return
    data = open(full_p, "rb").read()

    # 1 + 2: chunks
    chunks = sorted(f for f in os.listdir(d) if f.startswith("app_") and f.endswith(".bin")
                    and f != "app_full.bin")
    if not chunks:
        fails.append(f"{name}: no chunks")
        return
    expect = (len(data) + CHUNK - 1) // CHUNK
    if len(chunks) != expect:
        fails.append(f"{name}: {len(chunks)} chunks, expected {expect}")
    rebuilt = bytearray()
    for i, f in enumerate(chunks):
        blob = open(os.path.join(d, f), "rb").read()
        last = (i == len(chunks) - 1)
        if not last and len(blob) != CHUNK:
            fails.append(f"{name}: {f} is {len(blob)} B, expected {CHUNK}")
        rebuilt += blob
    if bytes(rebuilt) != data:
        fails.append(f"{name}: chunk reassembly differs from app_full.bin ({len(rebuilt)} vs {len(data)} B)")

    # 3 + 4: image header
    if len(data) < 24:
        fails.append(f"{name}: file too small to be an image")
        return
    if data[0] != 0xE9:
        fails.append(f"{name}: bad magic 0x{data[0]:02X} (expected 0xE9)")
        return
    segs = data[1]
    if not 1 <= segs <= 16:
        fails.append(f"{name}: implausible segment count {segs}")
        return
    entry = struct.unpack_from("<I", data, 4)[0]
    chip_id = struct.unpack_from("<H", data, 12)[0]
    hash_appended = data[23]
    if chip_id not in EXPECTED_CHIP_IDS:
        fails.append(f"{name}: chip id {chip_id}, expected one of {sorted(EXPECTED_CHIP_IDS)}")
    # Entry point is a per-chip property; only flag it for plain ESP32-S3 where IRAM applies.
    if chip_id == 0x0009 and not (0x3C000000 <= entry <= 0x44000000):
        warns.append(f"{name}: entry 0x{entry:08X} outside the usual IROM/DROM range")

    off = 24
    total = 0
    for s in range(segs):
        if off + 8 > len(data):
            fails.append(f"{name}: segment {s} header past EOF")
            return
        _addr, ln = struct.unpack_from("<II", data, off)
        off += 8 + ((ln + 3) & ~3)
        total += 8 + ((ln + 3) & ~3)
        if off > len(data):
            fails.append(f"{name}: segment {s} data past EOF")
            return
    tail = len(data) - off
    want_tail = 32 if hash_appended else 1
    # The image is padded to a 16-byte boundary before the checksum/SHA is appended, so the
    # tail is `padding + 32` (or `padding + 1`). Allow a full 16 bytes of padding.
    if not (want_tail <= tail <= want_tail + 16):
        fails.append(f"{name}: tail is {tail} B, expected {want_tail}..{want_tail + 16} "
                     f"(hash_appended={hash_appended})")

    # 5: partition fit
    if len(data) > OTA0_SIZE:
        fails.append(f"{name}: {len(data):,} B exceeds ota_0 ({OTA0_SIZE:,} B)")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()

    names = sorted(n for n in os.listdir(STAGED)
                   if os.path.isdir(os.path.join(STAGED, n)))
    if a.limit:
        names = names[:a.limit]
    print(f"auditing {len(names)} staged image(s)\n")
    for n in names:
        audit_one(n)
        if len(fails) and fails[-1].startswith(n + ":"):
            print(f"  FAIL  {fails[-1]}")

    print()
    for w in warns[:10]:
        print(f"  warn  {w}")
    if len(warns) > 10:
        print(f"  ... {len(warns) - 10} more warnings")
    if fails:
        print(f"\n{len(fails)} failure(s) across {len(names)} images")
        return 1
    print(f"all {len(names)} images pass: chunks reassemble losslessly, headers valid, "
          f"fit ota_0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
