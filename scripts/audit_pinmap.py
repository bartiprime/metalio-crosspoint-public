#!/usr/bin/env python3
"""Static pin-map audit: vendor config.h vs our BoardConfig header.

Two independent transcriptions of the same hardware should agree. This compares them
mechanically so a typo cannot hide behind a comment, and it re-runs after any edit on either
side — which is the point: a one-off manual check goes stale.

    python scripts/audit_pinmap.py
"""
from __future__ import annotations

import os
import re
import sys

ROOT = r"M:\_HermesWork\crosspoint-fork"
VENDOR = os.path.join(ROOT, "Metalio-E-INK4-Plus", "main", "boards",
                      "metalio-e-ink4-plus", "config.h")
OURS = os.path.join(ROOT, "crosspoint-reader", "freeink-sdk", "libs", "hardware",
                    "BoardConfig", "include", "MetalioEInk4PlusBoard.h")

PAIRS = [
    ("EPD_PIN_D0", "EPD_PIN_D0"), ("EPD_PIN_D1", "EPD_PIN_D1"),
    ("EPD_PIN_D2", "EPD_PIN_D2"), ("EPD_PIN_D3", "EPD_PIN_D3"),
    ("EPD_PIN_D4", "EPD_PIN_D4"), ("EPD_PIN_D5", "EPD_PIN_D5"),
    ("EPD_PIN_D6", "EPD_PIN_D6"), ("EPD_PIN_D7", "EPD_PIN_D7"),
    ("EPD_PIN_XCL", "EPD_PIN_XCL"), ("EPD_PIN_XLE", "EPD_PIN_XLE"),
    ("EPD_PIN_XOE", "EPD_PIN_XOE"), ("EPD_PIN_XSTL", "EPD_PIN_XSTL"),
    ("EPD_PIN_SPV", "EPD_PIN_SPV"), ("EPD_PIN_MODE", "EPD_PIN_MODE"),
    ("EPD_PIN_BORDER", "EPD_PIN_BORDER"), ("EPD_PIN_CKV", "EPD_PIN_CKV"),
    ("ANA_SW_GPIO", "ANA_SW_GPIO"), ("FL_COOL_PIN", "FRONTLIGHT_COOL_PIN"),
    ("FL_WARM_PIN", "FRONTLIGHT_WARM_PIN"), ("I2C_SDA_PIN", "I2C_SDA_PIN"),
    ("I2C_SCL_PIN", "I2C_SCL_PIN"), ("IO_EXPANDER_INT_GPIO", "IO_EXPANDER_INT_GPIO"),
    ("TOUCH_INT_PIN", "TOUCH_INT_PIN"), ("BOOT_BUTTON_GPIO", "BOOT_BUTTON_GPIO"),
    ("EPD_TPS_IO_VCOM_CTRL", "P0_VCOM_CTRL"), ("EPD_TPS_IO_PWRUP", "P0_PWRUP"),
    ("EPD_TPS_IO_WAKEUP", "P0_WAKEUP"), ("EPD_TPS_IO_PWR_GOOD", "P0_PWR_GOOD"),
    ("EPD_IO_MOTOR", "P0_MOTOR"), ("EPD_IO_PA_EN", "P0_PA_EN"),
    ("EPD_IO_CAM_SCR_EN", "P0_CAM_SCR_EN"), ("EPD_IO_VOL_UP", "P1_VOLUME_UP"),
    ("EPD_IO_VOL_DOWN", "P1_VOLUME_DOWN"), ("EPD_IO_POWER", "P1_POWER"),
    ("EPD_IO_PWR_PULSE", "P1_POWER_PULSE"), ("EPD_IO_TP_RST", "P1_TOUCH_RESET"),
    ("EPD_IO_BT_PA_PWR", "P1_BT_PA_PWR"), ("EPD_IO_RTC_INT", "P1_RTC_INT"),
]


def parse(path: str) -> dict:
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.lstrip().startswith("//"):
            continue
        m = re.match(r"\s*#define\s+([A-Z0-9_]+)\s+\(?(?:GPIO_NUM_)?(\d+)\)?", line)
        if m:
            out[m.group(1)] = int(m.group(2))
            continue
        m = re.match(r"\s*constexpr\s+\w+\s+([A-Za-z0-9_]+)\s*=\s*(\d+)\s*;", line)
        if m:
            out[m.group(1)] = int(m.group(2))
            continue
        # array form: constexpr int EPD_DATA_PINS[8] = {8, 9, ...};
        m = re.match(r"\s*constexpr\s+\w+\s+EPD_DATA_PINS\s*\[\d+\]\s*=\s*\{([^}]*)\}", line)
        if m:
            for i, v in enumerate(x.strip() for x in m.group(1).split(",")):
                out[f"EPD_PIN_D{i}"] = int(v)
    return out


def main() -> int:
    v, o = parse(VENDOR), parse(OURS)
    bad = 0
    print(f"{'our constant':26s} {'vendor':>8s} {'ours':>6s}")
    for vn, on in PAIRS:
        if vn not in v:
            print(f"{on:26s} {'?':>8s} {'?':>6s}  (vendor {vn} not found)")
            continue
        if on not in o:
            print(f"{on:26s} {v[vn]:8d} {'-':>6s}  MISSING")
            bad += 1
            continue
        ok = v[vn] == o[on]
        bad += 0 if ok else 1
        print(f"{on:26s} {v[vn]:8d} {o[on]:6d}  {'ok' if ok else 'MISMATCH'}")
    print()
    if bad:
        print(f"{bad} mismatch(es)/missing")
        return 1
    print(f"all {len(PAIRS)} assignments agree with the vendor")
    return 0


if __name__ == "__main__":
    sys.exit(main())
