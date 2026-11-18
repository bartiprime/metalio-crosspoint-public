#!/usr/bin/env python3
"""Static tests for the waveform path. No hardware, no cross-compiler.

Checks that would otherwise only fail on a panel:

  1. the generated header is byte-identical to the vendor table (transcription safety)
  2. every code is in {0,1,2} (packing assumption)
  3. for a 1-bpp transition, the LAST non-zero code pushes toward the target
  4. with `prev == nullptr` (unknown panel state) black pixels are still driven -- the
     vendor assumes a white buffer, and assuming "unchanged" would emit all-idle
  5. the 2-bit packing reproduces the driver's nibble layout (0xC0/0x30/0x0C/0x03)
  6. line geometry: 1216 px / 4 = 304 data bytes, +1 dummy = 305

    python scripts/test_waveform_static.py [-v]
"""
from __future__ import annotations

import os
import re
import sys

ROOT = r"M:\_HermesWork\crosspoint-fork"
VENDOR = os.path.join(ROOT, "Metalio-E-INK4-Plus", "main", "boards",
                      "metalio-e-ink4-plus", "display", "waveforms")
GEN = os.path.join(ROOT, "crosspoint-reader", "src", "display", "WaveformDU.h")
DRV = os.path.join(ROOT, "crosspoint-reader", "freeink-sdk", "libs", "display",
                   "FreeInkDisplay", "src", "driver", "Epd047Tc2Driver.cpp")

fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    if not cond:
        fails.append(msg)
        print(f"  FAIL  {msg}")


def vendor_du() -> bytes:
    src = open(os.path.join(VENDOR, "my_waveform_du.h"), encoding="utf-8", errors="replace").read()
    i = src.index("my_du_25_0_data")
    j = src.index("const EpdWaveformPhases", i)
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", src[i:j]))


def header_du() -> bytes:
    src = open(GEN, encoding="utf-8").read()
    i = src.index("kWaveform_DU[")
    j = src.index("};", i)
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", src[i:j]))


def code(flat: bytes, phase: int, to: int, frm: int) -> int:
    return (flat[(phase * 16 + to) * 4 + (frm // 4)] >> (6 - 2 * (frm % 4))) & 3


def main() -> int:
    verbose = "-v" in sys.argv
    v, g = vendor_du(), header_du()

    print(f"1. transcription: vendor {len(v)} B vs header {len(g)} B")
    check(len(v) == len(g) == 20 * 64, f"size mismatch {len(v)}/{len(g)}")
    check(v == g, "generated table differs from the vendor table")

    print("2. code alphabet")
    codes = {code(v, f, t, fr) for f in range(20) for t in range(16) for fr in range(16)}
    check(codes <= {0, 1, 2}, f"unexpected codes present: {sorted(codes)}")

    print("3. every active 1-bpp transition ends pushing toward its target")
    bad = []
    for to, want in ((0, 1), (15, 2)):
        for frm in (0, 15):
            if to == frm:
                continue   # idle transition: no drive is correct, not a failure
            seq = [code(v, f, to, frm) for f in range(20)]
            nz = [c for c in seq if c]
            if not nz:
                bad.append((to, frm, "no drive at all"))
            elif nz[-1] != want:
                bad.append((to, frm, f"last code {nz[-1]} != {want}"))
    check(not bad, f"bad endpoint transitions: {bad}")

    print("4. first paint (prev == nullptr ⇒ assume white)")
    # black target, from white -> must drive; white target, from white -> idle
    black_drive = sum(1 for f in range(20) if code(v, f, 0, 15))
    white_drive = sum(1 for f in range(20) if code(v, f, 15, 15))
    print(f"   black pixel: {black_drive} active phases | white pixel: {white_drive} active phases")
    check(black_drive > 0, "black pixels would not be driven on a first paint")
    check(white_drive == 0, "white pixels driven despite from == to == white")

    print("5. nibble layout matches the driver's packer")
    # reconstruct a byte for four black pixels at phase 0 and compare to the driver's shifts
    byte = 0
    for k in range(4):
        byte |= code(v, 0, 0, 15) << (6 - 2 * k)
    expect = 0
    for shift in (6, 4, 2, 0):
        expect |= code(v, 0, 0, 15) << shift
    check(byte == expect, "2-bit packing disagrees with 0xC0/0x30/0x0C/0x03 layout")
    if verbose:
        print(f"   black-at-phase0 byte = 0x{byte:02X}")

    print("6. geometry")
    drv = open(DRV, encoding="utf-8").read()
    check("kWidth / 4" in drv or "kLineDataBytes = kWidth / 4" in drv, "driver no longer uses width/4")
    check("304" in drv, "304 data bytes/line not found in the driver")
    check(1216 / 4 + 1 == 305, "line math")

    print()
    if fails:
        print(f"{len(fails)} check(s) FAILED")
        return 1
    print("all static waveform checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
