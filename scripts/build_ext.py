#!/usr/bin/env python3
"""Build the EXT variant family (bring-up extension scenarios).

Each EXT image is the anchor build plus ONE constant, `-DCP_EXT_MODE=<n>`, which selects a
scenario in `src/system/BringupExt.cpp`. Optional `-DCP_EXT_PRE_DELAY_MS=<ms>` settles the
rails before anything is touched (distinguishes instant faults from brownouts).

Re-uses the flag rewrite / hash guard / staging machinery from build_variants.py.

    python scripts/build_ext.py --list
    python scripts/build_ext.py --only ext_01_hands_off
    python scripts/build_ext.py                # everything
"""
from __future__ import annotations

import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("bv", os.path.join(HERE, "build_variants.py"))
bv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bv)

BASE = bv.BASE
STAGED = bv.STAGED

# name -> (mode, description, extra flags)
EXT: dict[str, tuple[int, str, list[str]]] = {
    # --- hands-off / observability -------------------------------------------
    "ext_01_hands_off":        (1, "I2C init + register reads only, zero writes", []),
    "ext_02_i2c_scan":         (2, "scan the whole I2C bus, beacon the device count", []),
    "ext_03_charger":          (3, "read CX25601N charger status/fault", []),
    "ext_04_pwrgood":          (4, "read PWR_GOOD before and after enabling rail bits", []),
    "ext_05_stress":           (5, "100 read-modify-write cycles on the expander", []),
    # --- gpio_hold family ----------------------------------------------------
    "ext_10_hold_dis_all":     (10, "release EVERY gpio hold, then heartbeat", []),
    "ext_11_hold_dis_eb":      (11, "release holds, then the real ensureBooted()", []),
    "ext_12_eb_control":       (12, "control: real ensureBooted() with holds untouched", []),
    "ext_13_charge_off":       (13, "force charger-enable to its inactive level", []),
    "ext_14_charge_held":      (14, "hold charger-enable asserted across a restart", []),
    # --- ensureBooted timing / context --------------------------------------
    "ext_20_eb_500ms":         (20, "ensureBooted() after 500 ms settle", []),
    "ext_21_eb_2000ms":        (21, "ensureBooted() after 2000 ms settle", []),
    "ext_22_eb_twice":         (22, "call ensureBooted() twice", []),
    "ext_23_eb_100k":          (23, "I2C at 100 kHz, then ensureBooted()", []),
    "ext_24_eb_busrec":        (24, "9-clock bus recovery, then ensureBooted()", []),
    "ext_25_vendor_on":        (25, "vendor power-on only (wakeup->50ms->pwrup+vcom)", []),
    "ext_26_vendor_slow":      (26, "vendor power-on with 300 ms settling", []),
    "ext_27_vendor_reverse":   (27, "reversed order: boost before wakeup", []),
    # --- single-pin polarity probes -----------------------------------------
    "ext_30_ana_high":         (30, "ANA_SW driven HIGH", []),
    "ext_31_epd_high":         (31, "EPD XOE/MODE/BORDER driven HIGH", []),
    "ext_32_fl_both_high":     (32, "both frontlight channels HIGH", []),
    "ext_33_fl_strobe":        (33, "frontlight strobed 10x at 20 ms", []),
    "ext_34_all_gpio_high":    (34, "every plain GPIO driven HIGH", []),
    "ext_35_power_pulse":      (35, "single POWER_PULSE (risk probe, 120 ms)", []),
    "ext_36_wakeup_low":       (36, "WAKEUP driven LOW while running", []),
    "ext_37_polarity_inv":     (37, "expander polarity inverted for inputs", []),
    # --- calibration / persistence / restart --------------------------------
    "ext_40_cal_1s":           (40, "calibration: 1 s glow then esp_restart", []),
    "ext_41_cal_3s":           (41, "calibration: 3 s glow then esp_restart", []),
    "ext_42_cal_5s":           (42, "calibration: 5 s glow then esp_restart", []),
    "ext_43_boot_count":       (43, "RTC_NOINIT boot counter, beacons the count", []),
    "ext_44_deep_sleep":       (44, "deep sleep 5 s then continue", []),
    "ext_45_restart_after_eb": (45, "ensureBooted() then restart (reboot signature)", []),
    # --- post-boot panel precursors ----------------------------------------
    "ext_50_panel_full":       (50, "ensureBooted + vendor panel power-on", []),
    "ext_51_panel_staggered":  (51, "ensureBooted + staggered panel power-on", []),
    "ext_52_panel_on_off":     (52, "panel rails on, then off after 1 s", []),
    "ext_53_panel_parked":     (53, "park EPD pins then ensureBooted", []),
    # --- pre-delay dimension (settle before touching anything) --------------
    "ext_60_hands_off_pd500":  (1, "hands-off, 500 ms pre-delay", ["CP_EXT_PRE_DELAY_MS=500"]),
    "ext_61_hands_off_pd2000": (1, "hands-off, 2000 ms pre-delay", ["CP_EXT_PRE_DELAY_MS=2000"]),
    "ext_62_eb_pd500":         (12, "ensureBooted with 500 ms pre-delay", ["CP_EXT_PRE_DELAY_MS=500"]),
    "ext_63_eb_pd2000":        (12, "ensureBooted with 2000 ms pre-delay", ["CP_EXT_PRE_DELAY_MS=2000"]),
    "ext_64_hold_pd500":       (11, "hold_dis + ensureBooted, 500 ms pre-delay", ["CP_EXT_PRE_DELAY_MS=500"]),
    "ext_65_hold_pd2000":      (11, "hold_dis + ensureBooted, 2000 ms pre-delay", ["CP_EXT_PRE_DELAY_MS=2000"]),
    "ext_66_scan_pd500":       (2, "I2C scan with 500 ms pre-delay", ["CP_EXT_PRE_DELAY_MS=500"]),
    "ext_67_scan_pd2000":      (2, "I2C scan with 2000 ms pre-delay", ["CP_EXT_PRE_DELAY_MS=2000"]),
    "ext_68_vendor_pd500":     (25, "vendor power-on with 500 ms pre-delay", ["CP_EXT_PRE_DELAY_MS=500"]),
    "ext_69_vendor_pd2000":    (25, "vendor power-on with 2000 ms pre-delay", ["CP_EXT_PRE_DELAY_MS=2000"]),
    # --- ordering permutations & display precursors -------------------------
    "ext_70_order_exp_fl_epd": (70, "order: expander -> frontlight -> EPD", []),
    "ext_71_order_epd_fl_exp": (71, "order: EPD -> frontlight -> expander", []),
    "ext_72_ana_first":        (72, "ANA_SW first (200 ms), then ensureBooted", []),
    "ext_73_fl_first":         (73, "frontlight first (200 ms), then ensureBooted", []),
    "ext_74_disp_parked":      (74, "display bus pins parked, then ensureBooted", []),
    "ext_75_ckv_pulse":        (75, "CKV pulsed 100x at ~1 us", []),
    "ext_76_xoe_then_rails":   (76, "output-enable asserted, then rails", []),
    "ext_77_rails_then_xoe":   (77, "rails, then XOE high / border low", []),
    "ext_78_dsleep_hold":      (78, "latch all GPIO into deep-sleep holds", []),
    "ext_79_dsleep_hold_dis":  (79, "release deep-sleep holds, then ensureBooted", []),
    # --- single expander bit LOW (complement of the b_* HIGH family) ---------
    "ext_80_bit0_low":         (80, "bit 0 (VCOM_CTRL) LOW, others untouched", []),
    "ext_81_bit1_low":         (81, "bit 1 (PWRUP) LOW, others untouched", []),
    "ext_82_bit2_low":         (82, "bit 2 (WAKEUP) LOW, others untouched", []),
    "ext_83_bit3_low":         (83, "bit 3 (MOTOR) LOW, others untouched", []),
    "ext_84_bit4_low":         (84, "bit 4 (PA_EN) LOW, others untouched", []),
    "ext_85_bit5_low":         (85, "bit 5 (CAM_SCR_EN) LOW, others untouched", []),
    "ext_86_bit11_low":        (86, "bit 11 (POWER_PULSE) LOW, others untouched", []),
    "ext_87_bit12_low":        (87, "bit 12 (BT_PA_PWR) LOW, others untouched", []),
    "ext_88_bit14_low":        (88, "bit 14 (RTC_INT) LOW, others untouched", []),
    # --- crash-recovery automation (see run_ext_after.py + capture_log.py) ---
    "ext_90_guard_full":       (90, "guarded sweep: skips the action that killed the last boot", []),
    "ext_91_guard_expander":   (91, "guarded sweep, expander + rail actions only", []),
    "ext_92_guard_selftest":   (92, "prove the guard: arm index 6, restart, come back knowing", []),
    "ext_93_self_download":    (93, "ROM download mode on request - no BOOT button", []),
    "ext_94_guard_every":      (94, "guarded sweep over ALL actions incl. extended panel pins", []),
    "ext_95_nvs_guard_full":   (95, "guarded sweep whose memory survives a REAL power-off", []),
    "ext_96_nvs_probe":        (96, "40 NVS writes only: is flash-writing itself survivable?", []),
    "ext_97_nvs_guard_exp":    (97, "NVS guard, expander + rail subset", []),
    "ext_98_nvs_read":         (98, "read the trace a previous death left behind", []),
    "pm_blackbox":             (130, "read charger/PMIC state + NVS blame, BEACON both, then NVS-traced sweep", []),
    "pwr_panel_clean":         (131, "safe3 + correct CONFIG0 (0xE3C0), then raise PWRUP+VCOM: clean test of divergence #3", []),
}


# Built FIRST. These four decide whether the rest of the campaign can run unattended at all,
# so they must not wait behind 60 diagnostic images.
PRIORITY = [
    "ext_96_nvs_probe",        # prerequisite: is a flash write alone survivable?
    "ext_92_guard_selftest",   # does RTC_NOINIT survive a restart?
    "ext_93_self_download",    # can the board enter download mode without the BOOT button?
    "ext_43_boot_count",       # does an RTC counter survive an actual death?
    "ext_95_nvs_guard_full",   # NVS trace: works even if power is removed
    "ext_98_nvs_read",         # cheapest readout of what a death recorded
    "ext_90_guard_full",       # self-recovering sweep: one flash replaces 14 S singles
    "ext_94_guard_every",      # same, including the extended panel pins
    "ext_91_guard_expander",   # expander-only guarded sweep
]


def ordered(names: list[str]) -> list[str]:
    head = [n for n in PRIORITY if n in names]
    return head + [n for n in names if n not in head]


def flags_for(name: str) -> list[str]:
    mode, _why, extra = EXT[name]
    return BASE + ["CP_BRINGUP_MINIMAL", "CP_BRINGUP_EXT", f"CP_EXT_MODE={mode}"] + extra


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
        for name, (mode, why, extra) in sorted(EXT.items()):
            print(f"{name:26s} mode={mode:<3d} {' '.join(extra):26s} {why}")
        print(f"\ntotal: {len(EXT)}")
        return 0

    names = ordered(a.only) if a.only else ordered(sorted(EXT))
    results, seen = {}, {}
    for name in names:
        if name not in EXT:
            print("unknown:", name)
            return 1
        mode, why, extra = EXT[name]
        print(f"\n=== {name}  (mode {mode})  {why}", flush=True)
        _apply_flags(name)
        ok = bv.build(name) and bv.stage(name) > 0
        if ok:
            import hashlib
            dg = hashlib.sha256(open(os.path.join(STAGED, name, "app_full.bin"), "rb").read()).hexdigest()
            if dg in seen:
                print(f"[{name}] !! WARNING identical to {seen[dg]} - flags did not take effect")
            seen[dg] = name
            print(f"[{name}] sha256={dg[:16]}", flush=True)
            bv.write_manifest([(name, "EXT", " ".join("-D" + f for f in flags_for(name)), why,
                                os.path.getsize(os.path.join(STAGED, name, "app_full.bin")), dg)])
        results[name] = ok

    print("\n=== summary ===")
    for k, v in results.items():
        print(f"  {k:28s} {'ok' if v else 'FAILED'}")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
