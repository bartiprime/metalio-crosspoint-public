#!/usr/bin/env python3
"""Rapid-fire test session harness.

Turns "which image is next, what did the board do, what do I flash now" into two commands,
and keeps a durable record so an interrupted session resumes exactly where it stopped.

    python scripts/test_session.py --flash            # flash the next untested image
    python scripts/test_session.py --result dead:3    # it died after a 3 s glow
    python scripts/test_session.py --result ok        # it reached the heartbeat
    python scripts/test_session.py --status           # where are we, what did we learn
    python scripts/test_session.py --next             # just tell me the next image
    python scripts/test_session.py --reset            # start the schedule over

Result syntax
    ok            reached the steady heartbeat (image survived)
    dead:N        warm glow for N seconds, then dark  (N = 0..9)
    dark          never lit at all (did not reach setup)
    skip          not run / no port
    note:<text>   freeform note, keeps the image untested

Everything is stored in _scratch/test_session.json and rendered to TEST_RESULTS.md.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time

ROOT = r"M:\_HermesWork\crosspoint-fork"
STAGED = os.path.join(ROOT, "_scratch", "staged")
STATE = os.path.join(ROOT, "_scratch", "test_session.json")
REPORT = os.path.join(ROOT, "TEST_RESULTS.md")
FLASH = os.path.join(STAGED, "flash_staged.sh")
PORT = os.environ.get("CP_PORT", "COM4")

# Order matters: cheapest decisive information first.
SCHEDULE = (
    # FIRST: is the bus even populated? The vendor's board has SEVEN devices on I2C_NUM_1
    # (0x68, 0x20, 0x38, 0x55, 0x6B, 0x51, 0x19). Fewer means the problem is wiring or power,
    # not sequencing, and every fix image below would be testing the wrong thing.
    ["ext_02_i2c_scan", "diag_battery", "diag_bus_matrix"]
    # Then the audited vendor boot state: the vendor releases gpio_hold as its very first
    # action, holds three power rails HIGH, and never powers the panel during boot.
    + ["fix_vboot_explicit", "fix_vboot_nopower", "fix_vboot_standby",
       "fix_vboot_pwrup", "fix_vboot_deferred",
       "fix_vendor_poweron", "fix_vendor_noupseq"]
    # Flash-current question, only relevant if ext_96 turns out to be fatal.
    + ["diag_nvs_single", "diag_nvs_erase"]
    # Combination matrix: which divergences matter, and which are irrelevant to the fix.
    + ["mx_1110", "mx_1100", "mx_1010", "mx_0110",
       "mx_1111", "mx_1101", "mx_1011", "mx_0111",
       "mx_0000", "mx_1000", "mx_0100", "mx_0010", "mx_0001"]
    # Timing: delete the call, or merely defer it?
    + ["pwr_delay_0000", "pwr_delay_0500", "pwr_delay_1000", "pwr_delay_5000"]
    + [f"s_single_{b:02d}" for b in range(16)]          # which single action kills it
    + ["bis_half_lo", "bis_half_hi", "bis_half_expander", "bis_half_plain_gpio"]
    + [f"bis_q1_0_3", "bis_q2_4_7", "bis_q3_8_11", "bis_q4_12_15"]
    + ["ext_12_eb_control", "ext_11_hold_dis_eb", "ext_01_hands_off", "ext_04_pwrgood"]
    + ["ext_40_cal_1s", "ext_41_cal_3s"]              # validate the reading method
    + ["ext_92_guard_selftest", "ext_93_self_download"]   # validate unattended recovery
    + ["ext_90_guard_full", "ext_91_guard_expander"]      # automated, self-recovering sweep
    + [n for n in ("bis_half_epd", "bis_half_inputs", "bis_half_outputs_low", "bis_half_odd", "bis_half_even")]
    # NVS-traced twins: same actions, but a death reports the exact index and recovers.
    # Only meaningful if ext_96_nvs_probe showed flash writes are survivable.
    + [f"nv_{k}" for k in ("half_lo", "half_hi", "half_expander", "half_plain_gpio",
                           "half_epd", "half_inputs", "half_outputs_low", "half_odd",
                           "half_even", "q1_0_3", "q2_4_7", "q3_8_11", "q4_12_15")]
    # NVS-traced leave-one-out: a LOO image that STILL dies proves there are two killers.
    + [f"nv_loo_{b:02d}" for b in range(14)]
    # Ready-to-flash fixes for the two hypotheses nothing else covered.
    + [f"fix_high_{k}" for k in ("fl_cool", "fl_warm", "ana_sw", "xoe", "mode", "border")]
    + [f"exp_one_{p}_{k}" for p in ("out", "hi")
       for k in ("vcom_ctrl", "pwrup", "wakeup", "motor", "pa_en",
                 "cam_scr_en", "power_pulse", "bt_pa_pwr", "rtc_int")]
    + ["ext_78_dsleep_hold", "ext_79_dsleep_hold_dis", "ext_43_boot_count", "ext_44_deep_sleep"]
    + [f"ext_{n}" for n in ("70_order_exp_fl_epd", "71_order_epd_fl_exp", "72_ana_first",
                            "73_fl_first", "74_disp_parked", "75_ckv_pulse",
                            "76_xoe_then_rails", "77_rails_then_xoe")]
    + ["ctl_shutdown"]
    + ["ext_80_bit0_low", "ext_81_bit1_low", "ext_82_bit2_low", "ext_83_bit3_low",
       "ext_84_bit4_low", "ext_85_bit5_low", "ext_86_bit11_low", "ext_87_bit12_low",
       "ext_88_bit14_low"]
)

# The remaining groups were built but never listed above - 29 images that would otherwise be
# orphaned. Discovered from the variant table so any future group is included automatically.
# Order: extended pins -> timing -> I2C -> real-setup filters (post-fix).
FALLBACK_GROUPS = ("L", "S2", "T", "I", "R")


def _discover_fallback() -> list[str]:
    import importlib.util
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build_variants.py")
    try:
        spec = importlib.util.spec_from_file_location("bv_ts", path)
        bv = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(bv)
    except Exception:                                          # noqa: BLE001
        return []
    out: list[str] = []
    for grp in FALLBACK_GROUPS:
        out += sorted(n for n, v in bv.VARIANTS.items() if v[1] == grp)
    return [n for n in out if n not in SCHEDULE]


SCHEDULE = list(SCHEDULE) + _discover_fallback()


# ---------------------------------------------------------------------------------------------
# Revised flash order -- docs/PARK_AND_FLASH.md section 3, adopted from the external review.
#
# The SCHEDULE above predates that review: it opens with ext_02 and runs the whole FIX family
# third. The review's objection was specific -- FIX removes one action from the FULL vendor
# sequence, so if the fault sits in the shared prefix all 36 images die identically and teach
# nothing. Baseline first, vendor-parity last.
#
# This is a stable re-order by bucket, so no image is added, dropped or duplicated; only the
# sequence changes. `--plan` (scripts/flash_plan.py) renders the result with each image's
# purpose and hash so the order can be read before any hardware is touched.
# ---------------------------------------------------------------------------------------------
def _flash_bucket(name: str) -> int:
    if name == "ext_01_hands_off":
        return 0        # baseline: I2C init + register reads, zero writes
    if name in ("ext_02_i2c_scan", "ext_03_charger", "ext_04_pwrgood"):
        return 1        # bus/rail health gate -- "<7 devices" stops the whole session
    if name in ("ext_12_eb_control", "ext_11_hold_dis_eb"):
        return 2        # the one controlled A/B for vendor divergence #1 (gpio_hold)
    if name.startswith("latch_"):
        return 3        # the three vendor-HIGH rails driven LOW -- only meaningful if 0) survived
    if re.match(r"ext_1[034]_", name):
        return 4        # hold sub-arm
    if re.match(r"ext_2[0-7]_", name):
        return 5        # enable-timing sub-arm (monotone, so searchable in <=3 flashes)
    if re.match(r"ext_3[0-7]_", name):
        return 6        # polarity sub-arm
    if re.match(r"ext_(?:4[0-9]|9[0-9])_", name):
        return 7        # calibration + guard / self-recovery validation
    if name.startswith("diag_"):
        return 8        # battery, bus matrix, single NVS write
    if name.startswith("ext_"):
        return 9        # remaining discrete EXT probes (order/park/pin sweeps ext_5x-8x)
    if re.match(r"s2?_single_", name):
        return 10       # legacy singles: each action alone
    if re.match(r"(s|s2|t|i|r|b|c|v\d)_", name):
        return 11       # remaining legacy battery (LOO, combos, timing, I2C, real-setup)
    if name.startswith("nv_") or name.startswith("bis_"):
        return 12       # NVS-traced twins + bisect permutations around a known failing step
    if name.startswith("mx_") or name.startswith("pwr_delay_"):
        return 13       # divergence matrix + PWRUP deferral
    if name.startswith(("fix_", "exp_", "ctl_")):
        return 14       # vendor parity minus one action -- only informative once step K is known
    return 15


_seen: set[str] = set()
_unique = [n for n in SCHEDULE if not (n in _seen or _seen.add(n))]      # dedupe, keep order
SCHEDULE = [n for _, n in sorted(enumerate(_unique), key=lambda t: (_flash_bucket(t[1]), t[0]))]


def load() -> dict:
    if os.path.exists(STATE):
        try:
            with open(STATE, encoding="utf-8") as fh:
                return json.load(fh)
        except (OSError, json.JSONDecodeError):
            pass
    return {"created": time.strftime("%Y-%m-%d %H:%M:%S"), "results": {}, "order": list(SCHEDULE)}


def save(st: dict) -> None:
    with open(STATE, "w", encoding="utf-8") as fh:
        json.dump(st, fh, indent=2)


def available(name: str) -> bool:
    return os.path.isdir(os.path.join(STAGED, name))


def next_image(st: dict) -> str | None:
    for name in st["order"]:
        if name in st["results"]:
            continue
        if available(name):
            return name
    return None


def unbuilt_ahead(st: dict) -> list[str]:
    """Earlier scheduled images that are not staged yet.

    Without this, `--next` silently falls through to whatever happens to be built, which would
    hand you an old diagnostic image while a vendor-conformance fix sat unbuilt -- and look like
    a normal suggestion rather than a missing build.
    """
    out: list[str] = []
    for name in st["order"]:
        if name in st["results"]:
            continue
        if not available(name):
            out.append(name)
        else:
            break
    return out


def render(st: dict) -> None:
    res = st["results"]
    done = [n for n in st["order"] if n in res]
    dead = [(n, res[n]["result"]) for n in done if not res[n]["result"].startswith(("ok", "skip"))]
    ok = [n for n in done if res[n]["result"].startswith("ok")]

    L = ["# Test results", ""]
    L.append(f"Updated {time.strftime('%Y-%m-%d %H:%M')} · {len(done)} tested · "
             f"{len(ok)} survived · {len(dead)} died")
    L.append("")
    if dead:
        L.append("## Deaths (the signal)")
        L.append("")
        L.append("| Image | Result | Note |")
        L.append("|---|---|---|")
        for n, r in dead:
            L.append(f"| `{n}` | `{r}` | {res[n].get('note', '')} |")
        L.append("")
    if ok:
        L.append("## Survivals (exonerated)")
        L.append("")
        L.append(" " + ", ".join(f"`{n}`" for n in ok))
        L.append("")
    nxt = next_image(st)
    L.append("## Next")
    L.append("")
    L.append(f"`{nxt}`" if nxt else "_schedule complete_")
    L.append("")
    with open(REPORT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(L))


def main() -> int:
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--flash", action="store_true")
    g.add_argument("--result")
    g.add_argument("--status", action="store_true")
    g.add_argument("--next", action="store_true")
    g.add_argument("--reset", action="store_true")
    a = ap.parse_args()

    st = load()
    if a.reset:
        st["results"] = {}
        st["order"] = list(SCHEDULE)
        save(st)
        print("schedule reset")
        return 0

    if a.next or a.flash:
        nxt = next_image(st)
        if not nxt:
            print("schedule complete")
            return 0
        if a.next:
            print(nxt)
            ahead = unbuilt_ahead(st)
            if ahead:
                print(f"!! {len(ahead)} earlier image(s) not built yet: {', '.join(ahead[:4])}",
                      file=sys.stderr)
            return 0
        st["last_flashed"] = nxt
        st["last_flashed_at"] = time.strftime("%Y-%m-%d %H:%M:%S")
        save(st)
        print(f"flashing {nxt} on {PORT}")
        rc = subprocess.call(["bash", FLASH, PORT, os.path.join("_scratch", "staged", nxt)], cwd=ROOT)
        if rc == 0:
            print(f"\nflashed. power-cycle the board, watch the warm glow, then:\n"
                  f"  python crosspoint-reader/scripts/test_session.py --result dead:N   (N = glow seconds)\n"
                  f"  python crosspoint-reader/scripts/test_session.py --result ok")
        return rc

    if a.result:
        raw = a.result.strip()
        target = st.get("last_flashed")
        if not target:
            print("nothing flashed yet - use --flash first, or --result will not record")
            return 1
        if raw.startswith("note:"):
            st["results"].setdefault(target, {"result": "skip", "note": ""})
            st["results"][target]["note"] = raw[5:]
        else:
            st["results"][target] = {"result": raw, "at": time.strftime("%Y-%m-%d %H:%M:%S")}
        save(st)
        render(st)
        nxt = next_image(st)
        print(f"recorded {target} = {raw}")
        print(f"next: {nxt}")
        return 0

    # --status
    res = st["results"]
    done = [n for n in st["order"] if n in res]
    print(f"tested {len(done)} / {len(st['order'])}")
    for n in done:
        mark = "OK  " if res[n]["result"].startswith("ok") else "DEAD"
        print(f"  {mark} {n:26s} {res[n]['result']}")
    print(f"next: {next_image(st)}")
    render(st)
    return 0


if __name__ == "__main__":
    sys.exit(main())
