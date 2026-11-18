#!/usr/bin/env python3
"""Build the FIX family: ready-to-flash candidates for the hypotheses nothing else covers.

Two blind spots in the existing 180 images:

  1. Every plain-pin variant drives FRONTLIGHT/ANA_SW/EPD **LOW**. Nothing ever drove them HIGH.
     If sourcing current into a pin (not sinking) is what collapses the rail, that is invisible
     in the current set.
  2. The expander is only ever configured in one shot: CONFIG0 flips all nine pins to outputs in
     a single write. Nothing enables them ONE AT A TIME. If contention on a single externally
     driven pin is the mechanism, the whole-write image dies and every single-pin image survives
     - which is a completely different fix from anything we would otherwise try.

Every image here is also NVS-traced, so a death reports the exact action and the next boot
recovers instead of looping.

    python scripts/build_fix.py --list
    python scripts/build_fix.py --only fix_high_fl_cool
    python scripts/build_fix.py
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

PLAIN_PINS = {6: "fl_cool", 7: "fl_warm", 8: "ana_sw", 9: "xoe", 10: "mode", 11: "border"}

# Expander bits that ensureBooted() turns into outputs (the nine in kOutputs).
EXP_BITS = {0: "vcom_ctrl", 1: "pwrup", 2: "wakeup", 3: "motor", 4: "pa_en",
            5: "cam_scr_en", 11: "power_pulse", 12: "bt_pa_pwr", 14: "rtc_int"}

FIX: dict[str, tuple[int, dict[str, str], str]] = {}

# --- (c) FAITHFUL vendor power-on: the highest-value candidate --------------
# ensureBooted() raises PWRUP without ever programming the TPS65185 power-up sequencer, so the
# PMIC runs its default rail order. The vendor programs UPSEQ0=0xE4 / UPSEQ1=0x55 first, waits
# for PWR_GOOD, and only then writes ENABLE. Built first because it is a real behavioural
# divergence from shipped firmware, not a hypothesis about pin polarity.
# The vendor NEVER powers the panel at boot: epd_poweron() is only called from refresh paths.
# Our ensureBooted() raises PWRUP + VCOM during boot, which is an action shipped firmware does
# not perform until a refresh is needed. This triad isolates that single divergence.
# --- diagnostics that come FIRST in build order: they gate the interpretation of everything
# below. If the pack cannot survive boost inrush, or the bus is not populated, then no
# sequencing hypothesis is testable.
FIX["diag_battery"] = (113, {}, "read BQ27220 voltage/SOC/current + charger status; beacon the pack level")
FIX["diag_bus_matrix"] = (114, {}, "per-address probe x3: absent vs flaky vs present")
FIX["diag_nvs_single"] = (115, {}, "exactly ONE NVS write - separates the act from sustained current")
FIX["diag_nvs_erase"] = (116, {}, "full nvs_flash_erase - heaviest flash current, no trace involved")

FIX["fix_vboot_explicit"] = (110, {}, "explicit vendor power-up latches: CAM_SCR/TP_RST/BT_PA HIGH, no PWRUP")
FIX["fix_vboot_nopower"] = (107, {}, "vendor boot state: holds released, latches preserved, NO panel power")
FIX["fix_vboot_standby"] = (108, {}, "vendor boot state + PMIC UPSEQ programmed in STANDBY, still no PWRUP")
FIX["fix_vboot_pwrup"] = (109, {}, "vendor boot state + PWRUP at boot (tests powering the panel early)")
FIX["fix_vboot_deferred"] = (112, {}, "vendor boot state + PWRUP after 2 s: never vs not-yet")
FIX["ctl_shutdown"] = (111, {}, "POSITIVE CONTROL: real 15x100ms shutdown pulse; board should power off")

FIX["fix_vendor_poweron"] = (105, {}, "faithful vendor power-on: UPSEQ programmed, PWR_GOOD polled, ENABLE after PG")
FIX["fix_vendor_noupseq"] = (106, {}, "vendor power-on but WITHOUT the UPSEQ writes (isolates the sequencer)")

# --- (a) the opposite polarity: drive the pin HIGH instead of LOW ----------
for bit, short in PLAIN_PINS.items():
    FIX[f"fix_high_{short}"] = (100, {"CP_FIX_BIT": str(bit)},
                               f"everything except bit {bit}, then drive it HIGH instead of LOW")

# --- (b) enable exactly one expander pin as an output ----------------------
for bit, short in EXP_BITS.items():
    FIX[f"exp_one_out_{short}"] = (101, {"CP_PIN_BIT": str(bit)},
                                   f"bit {bit} ({short}) enabled as an output alone, never driven high")
    FIX[f"exp_one_hi_{short}"] = (102, {"CP_PIN_BIT": str(bit)},
                                  f"bit {bit} ({short}) enabled as an output and driven HIGH alone")


def flags_for(name: str) -> list[str]:
    mode, extra, _ = FIX[name]
    fl = BASE + ["CP_BRINGUP_MINIMAL", "CP_BRINGUP_EXT", f"CP_EXT_MODE={mode}"]
    fl += [f"{k}={v}" for k, v in extra.items()]
    return fl


def _apply_flags(name: str) -> None:
    """Header mode when available: recompiles 2 TUs. Legacy otherwise: full rebuild."""
    fl = flags_for(name)
    if bv.header_mode_available():
        bv.set_header(fl)
        bv.touch_header()
    else:
        bv.set_flags(fl)
        bv.touch_flag_consumers()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    if a.list:
        for n, (mode, extra, why) in FIX.items():
            print(f"{n:26s} mode={mode:<4d} {' '.join(f'-D{k}={v}' for k, v in extra.items()):22s} {why}")
        print(f"\ntotal: {len(FIX)}")
        return 0

    names = a.only or list(FIX)
    results, seen = {}, {}
    for name in names:
        if name not in FIX:
            print("unknown:", name)
            return 1
        mode, extra, why = FIX[name]
        print(f"\n=== {name}  (mode {mode})  {why}", flush=True)
        _apply_flags(name)
        ok = bv.build(name) and bv.stage(name) > 0
        if ok:
            dg = hashlib.sha256(open(os.path.join(STAGED, name, "app_full.bin"), "rb").read()).hexdigest()
            if dg in seen:
                print(f"[{name}] !! WARNING identical to {seen[dg]} - flags did not take effect")
            seen[dg] = name
            print(f"[{name}] sha256={dg[:16]}", flush=True)
            bv.write_manifest([(name, "FIX", " ".join("-D" + f for f in flags_for(name)), why,
                                os.path.getsize(os.path.join(STAGED, name, "app_full.bin")), dg)])
        results[name] = ok

    print("\n=== summary ===")
    for k, v in results.items():
        print(f"  {k:28s} {'ok' if v else 'FAILED'}")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
