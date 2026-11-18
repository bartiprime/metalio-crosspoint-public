#!/usr/bin/env python3
"""Build every pre-planned bring-up variant and stage each for instant flashing.

Stages:
  _scratch/staged/<name>/app_full.bin + app_NN.bin chunks (256 KB, stride 0x40000)

Groups
  A  core:       ladder diagnostic + three candidate fixes + two real-setup builds
  B  bit tests:  drive exactly ONE TCA9555 output bit high, nothing else
  C  order/combos: config-only, reversed order, no-polarity, safe+boost

Run:
    M:/hf/penv/Scripts/python crosspoint-reader/scripts/build_variants.py --list
    ... --group A
    ... --only v1_ladder b_pwrup
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time

ROOT = r"M:\_HermesWork\crosspoint-fork"
PROJ = os.path.join(ROOT, "crosspoint-reader")
INI = os.path.join(PROJ, "platformio.ini")
BUILD = os.path.join(PROJ, ".pio", "build", "metalio_eink4_plus", "firmware.bin")
STAGED = os.path.join(ROOT, "_scratch", "staged")
CHUNK = 256 * 1024

BASE = ["CP_CONSOLE_ONLY"]

# name -> (flags, group, purpose)
VARIANTS: dict[str, tuple[list[str], str, str]] = {
    # ---------------- group A: core ----------------
    "v0_anchor": (BASE + ["CP_BRINGUP_MINIMAL"], "A",
                  "no I2C at all; heartbeat only. Known to survive - reflash to prove the board is alive"),
    "v1_ladder": (BASE + ["CP_BRINGUP_MINIMAL", "CP_I2C_LADDER"], "A",
                  "6-step diagnostic: reads -> OUTPUT0 -> CONFIG0 -> POLARITY0 -> rails -> vendor fix"),
    "v2_fixA": (BASE + ["CP_BRINGUP_MINIMAL", "CP_SAFE_EXPANDER_REST"], "A",
                  "ensureBooted keeps CAM_SCR/TP_RST/BT_PA HIGH at rest"),
    "v3_fixB": (BASE + ["CP_BRINGUP_MINIMAL", "CP_SAFE_EXPANDER_REST", "CP_VENDOR_POWER_ORDER"], "A",
                  "v2 + vendor power order (wakeup -> 50 ms -> pwrup+vcom)"),
    "v4_real_norails": (BASE + ["CP_SKIP_RAILS"], "A",
                  "REAL setup() with no expander/rail calls"),
    "v5_real_fix": (BASE + ["CP_SAFE_EXPANDER_REST"], "A",
                  "REAL setup() + fixA - candidate release path"),
    "v6_low_ladder": (BASE + ["CP_BRINGUP_MINIMAL", "CP_LOW_LADDER"], "A",
                  "INVERSE ladder: drive each of the 6 low bits LOW one at a time - finds which pin must never be driven low"),

    # ---------------- group B: single-bit isolation ----------------
    # Each drives exactly one TCA9555 output bit high (bit made an output, all else
    # left as inputs). Answers: does THIS rail bit alone power the board off?
    "b_vcom":    (BASE + ["CP_BRINGUP_MINIMAL", "CP_BIT_TEST=0"], "B", "P0.0 VCOM_CTRL high only"),
    "b_pwrup":   (BASE + ["CP_BRINGUP_MINIMAL", "CP_BIT_TEST=1"], "B", "P0.1 PWRUP high only"),
    "b_wakeup":  (BASE + ["CP_BRINGUP_MINIMAL", "CP_BIT_TEST=2"], "B", "P0.2 WAKEUP high only"),
    "b_motor":   (BASE + ["CP_BRINGUP_MINIMAL", "CP_BIT_TEST=3"], "B", "P0.3 MOTOR high only"),
    "b_pa_en":   (BASE + ["CP_BRINGUP_MINIMAL", "CP_BIT_TEST=4"], "B", "P0.4 PA_EN high only"),
    "b_cam_scr": (BASE + ["CP_BRINGUP_MINIMAL", "CP_BIT_TEST=5"], "B", "P0.5 CAM_SCR_EN high only"),
    "b_ppulse":  (BASE + ["CP_BRINGUP_MINIMAL", "CP_BIT_TEST=11"], "B", "P1.3 POWER_PULSE high only"),
    "b_tp_rst":  (BASE + ["CP_BRINGUP_MINIMAL", "CP_BIT_TEST=12"], "B", "P1.4 TOUCH_RESET high only"),
    "b_bt_pa":   (BASE + ["CP_BRINGUP_MINIMAL", "CP_BIT_TEST=14"], "B", "P1.6 BT_PA_PWR high only"),

    # ---------------- group C: order / combos ----------------
    "c_cfg_only": (BASE + ["CP_BRINGUP_MINIMAL", "CP_CFG_ONLY"], "C",
                   "flip directions to outputs WITHOUT writing OUTPUT0 first (reg resets to 0xFF)"),
    "c_cfg_first": (BASE + ["CP_BRINGUP_MINIMAL", "CP_CFG_BEFORE_OUT"], "C",
                   "CONFIG0 before OUTPUT0 - tests whether write ORDER causes a glitch"),
    "c_safe3": (BASE + ["CP_BRINGUP_MINIMAL", "CP_SAFE3_ONLY"], "C",
                   "vendor-safe OUTPUT0 + CONFIG0 only, no polarity, no rails"),
    "c_pol_skip": (BASE + ["CP_BRINGUP_MINIMAL", "CP_SAFE_EXPANDER_REST", "CP_POL_SKIP"], "C",
                   "v2 without the polarity write"),

    # ---------------- group R: real setup() with subsystem filters ----------------
    # These exist for the stage AFTER the killer pin is fixed: they run the real
    # CrossPoint setup() while skipping the rail/expander init entirely, so each
    # subsystem (HalSystem, gpio, powerManager, tilt/clock) can be bisected without
    # waiting for a new build.
    "r_stop_after_rails": (BASE + ["CP_STOP_AFTER_RAILS"], "R",
                  "real setup(), stops right after holdPowerRails() - control for the rail stage"),
    "r_skip_hal": (BASE + ["CP_SKIP_RAILS", "CP_SKIP_HAL"], "R",
                  "no expander/rails, and skips HalSystem::begin()"),
    "r_skip_gpio": (BASE + ["CP_SKIP_RAILS", "CP_SKIP_GPIO_BEGIN"], "R",
                  "no expander/rails, and skips gpio.begin()"),
    "r_skip_power": (BASE + ["CP_SKIP_RAILS", "CP_SKIP_POWER_BEGIN"], "R",
                  "no expander/rails, and skips powerManager.begin()"),
    "r_skip_tilt": (BASE + ["CP_SKIP_RAILS", "CP_SKIP_TILT_CLOCK"], "R",
                  "no expander/rails, and skips tilt sensor + clock init"),
    "r_norails_all": (BASE + ["CP_SKIP_RAILS", "CP_SKIP_HAL", "CP_SKIP_GPIO_BEGIN",
                              "CP_SKIP_POWER_BEGIN", "CP_SKIP_TILT_CLOCK"], "R",
                  "real setup() with every early subsystem skipped - how far does CrossPoint get"),

    # ---------------- group I: I2C bus variants ----------------
    "i_100k": (BASE + ["CP_SAFE_EXPANDER_REST", "CP_I2C_100K"], "I",
                  "real setup + fixA, I2C at 100 kHz instead of the configured rate"),
    "i_recovery": (BASE + ["CP_SAFE_EXPANDER_REST", "CP_I2C_RECOVERY"], "I",
                  "real setup + fixA, with a 9-clock I2C bus-recovery prelude before Wire.begin()"),
}



# ---------------------------------------------------------------------------
# Group S: mask-driven action battery (50 images)
# ---------------------------------------------------------------------------
# Bit -> action (see the CP_STEP_MASK executor in src/main.cpp)
ACTION_NAMES = {
    0: "expander OUTPUT0 = safe3",
    1: "expander CONFIG0 = 9 outputs",
    2: "expander POLARITY0 clear",
    3: "GPIO2 INPUT_PULLUP (expander INT)",
    4: "GPIO5 INPUT (touch INT)",
    5: "GPIO61 INPUT_PULLUP (boot key)",
    6: "GPIO6 frontlight COOL out LOW",
    7: "GPIO7 frontlight WARM out LOW",
    8: "GPIO36 ANA_SW out LOW",
    9: "GPIO18 XOE out LOW",
    10: "GPIO19 MODE out LOW",
    11: "GPIO37 BORDER out LOW",
    12: "holdPowerRails() latch loop",
    13: "TPS power-on wakeup/pwrup/vcom",
    14: "ensureBooted() FULL (original)",
    15: "EPD XOE+MODE+BORDER all LOW",
}


def _add_s_group() -> None:
    """Populate VARIANTS with the 50 mask images. Deterministic numbering."""
    flags_for = lambda mask: BASE + ["CP_BRINGUP_MINIMAL", f"CP_STEP_MASK={mask}"]

    def add(name: str, mask: int, why: str) -> None:
        VARIANTS[name] = (flags_for(mask), "S", f"mask 0x{mask:04x} :: {why}")

    # 1-16: every single action in isolation
    for bit in range(16):
        add(f"s_single_{bit:02d}", 1 << bit, ACTION_NAMES[bit])

    # 17-30: cumulative prefixes 0..13 (excludes the FULL ensureBooted bit 14)
    for k in range(14):
        mask = (1 << (k + 1)) - 1
        add(f"s_prefix_{k:02d}", mask,
            f"actions 0..{k} cumulatively (last: {ACTION_NAMES[k]})")

    # 31-44: leave-one-out from bits 0..13 -> the omission that SURVIVES is the culprit
    base = (1 << 14) - 1  # bits 0..13
    for bit in range(14):
        mask = base & ~(1 << bit)
        add(f"s_loo_{bit:02d}", mask,
            f"all of 0..13 EXCEPT bit {bit} ({ACTION_NAMES[bit]}) - survives => bit {bit} is required for the fault")

    # 45-50: targeted combinations
    add("s_combo_frontlight", (1 << 6) | (1 << 7), "both frontlight pins driven low")
    add("s_combo_epd_trio", (1 << 9) | (1 << 10) | (1 << 11), "XOE+MODE+BORDER together")
    add("s_combo_ints", (1 << 3) | (1 << 4) | (1 << 5), "the three INT/key pin modes")
    add("s_combo_fl_ana", (1 << 6) | (1 << 7) | (1 << 8), "frontlight + ANA_SW")
    add("s_combo_expander", (1 << 0) | (1 << 1) | (1 << 2), "all three expander register writes")
    add("s_combo_gpio_all", sum(1 << b for b in range(3, 12)),
        "every plain-GPIO action (3..11), no expander")

_add_s_group()

# ---------------------------------------------------------------------------
# Group S2 / T: extended actions (16-23) and timing variants
# ---------------------------------------------------------------------------
EXT_NAMES = {
    16: "GPIO16 XCL low",
    17: "GPIO17 XLE low",
    18: "GPIO35 SPV low",
    19: "GPIO40 CKV low (LCD_PCLK pad)",
    20: "GPIO4 XSTL low",
    21: "EPD clock pins 16/17/35/40/4 all LOW",
    22: "chargeEnable pin write",
    23: "EVERY plain GPIO low",
}


def _add_s2_group() -> None:
    def add(name: str, mask: int, why: str, group: str = "S2", extra: list[str] | None = None) -> None:
        flags = BASE + ["CP_BRINGUP_MINIMAL", f"CP_STEP_MASK={mask}"] + (extra or [])
        VARIANTS[name] = (flags, group, f"mask 0x{mask:06x} :: {why}")

    # every extended action alone
    for bit in sorted(EXT_NAMES):
        add(f"s2_single_{bit:02d}", 1 << bit, EXT_NAMES[bit])

    # cumulative over the extended tier only
    acc = 0
    for bit in sorted(EXT_NAMES):
        acc |= 1 << bit
        add(f"s2_prefix_{bit:02d}", acc,
            f"extended actions 16..{bit} cumulatively (last: {EXT_NAMES[bit]})")

    # targeted
    add("s2_combo_epd_clocks", sum(1 << b for b in (16, 17, 18, 19, 20)),
        "the five EPD clock/control pins driven low together")
    add("s2_combo_all_gpio",
        sum(1 << b for b in list(range(3, 12)) + [16, 17, 18, 19, 20]),
        "every plain GPIO the driver touches, expander untouched")

    # timing: same mask, different settle time -> instant fault vs brownout
    gpio_mask = sum(1 << b for b in list(range(3, 12)) + [16, 17, 18, 19, 20])
    add("t_settle_50", gpio_mask, "all GPIO, 50 ms pause after each action", "T",
        ["CP_STEP_SETTLE_MS=50"])
    add("t_settle_500", gpio_mask, "all GPIO, 500 ms pause after each action", "T",
        ["CP_STEP_SETTLE_MS=500"])
    add("t_ana_500", 1 << 8, "ANA_SW low alone, 500 ms settle", "T",
        ["CP_STEP_SETTLE_MS=500"])


_add_s2_group()


# ---------------------------------------------------------------------------
# Group L: the three rails the vendor holds HIGH, driven LOW
#
# The gap this fills, precisely. `ensureBooted()` does `latch &= ~kOutputs`, clearing all nine
# expander output bits. The evidence that "exonerated the expander" covers only two of the three
# possible directions:
#   * six pins driven LOW cumulatively    -> v6_low_ladder, survived
#   * CAM_SCR / TP_RST / BT_PA driven HIGH -> b_cam_scr / b_tp_rst / b_bt_pa, survived
# The S battery's bit 0 writes those three HIGH as well (`expander OUTPUT0 = safe3`), which is the
# VENDOR's at-rest state, not ours. So the state the port actually drives -- those three LOW --
# had never been run, not even one pin at a time. These five images close that.
# ---------------------------------------------------------------------------
L_NAMES = {
    24: "P0.5 CAM_SCR_EN (screen rail) low",
    25: "P1.2 TOUCH_RESET low (low = touch unpowered)",
    26: "P1.4 BT_PA_PWR low",
    27: "CAM_SCR + TP_RST + BT_PA all low -- the untested state",
    28: "all NINE expander outputs low -- ensureBooted's latch state",
}


def _add_l_group() -> None:
    def add(name: str, bit: int, why: str) -> None:
        flags = BASE + ["CP_BRINGUP_MINIMAL", f"CP_STEP_MASK={1 << bit}"]
        VARIANTS[name] = (flags, "L", f"mask 0x{1 << bit:07x} :: {why}")

    add("latch_low3", 27, L_NAMES[27] + " (decisive single image)")
    add("latch_low9", 28, L_NAMES[28])
    add("latch_low_cam_scr", 24, L_NAMES[24])
    add("latch_low_tp_rst", 25, L_NAMES[25])
    add("latch_low_bt_pa", 26, L_NAMES[26])


_add_l_group()


def set_flags(flags: list[str]) -> None:
    """Replace the -DCP_* lines INSIDE [env:metalio_eink4_plus] only.

    Bug fixed 2026-10-24: the first version anchored on the first occurrence of
    -DENABLE_SERIAL_LOG in the file, which belongs to a DIFFERENT env. Variant flags
    were therefore never applied to our env and every build produced the same binary.
    Always bound the edit to the real env block.
    """
    with open(INI, "r", encoding="utf-8", newline="") as fh:
        text = fh.read()
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(nl)

    start = next(i for i, ln in enumerate(lines) if ln.strip() == "[env:metalio_eink4_plus]")
    end = start + 1
    while end < len(lines) and not lines[end].lstrip().startswith("["):
        end += 1

    block = [ln for ln in lines[start:end] if not ln.strip().startswith("-DCP_")]
    anchor = next(i for i, ln in enumerate(block) if ln.strip() == "-DENABLE_SERIAL_LOG")
    inject = [f"  -D{f}  ; variant flag (build_variants.py)" for f in flags]
    block[anchor + 1:anchor + 1] = inject

    out = lines[:start] + block + lines[end:]
    with open(INI, "w", encoding="utf-8", newline="") as fh:
        fh.write(nl.join(out))

    # hard verification: the flags must be inside our block now
    check = open(INI, "r", encoding="utf-8", newline="").read().split(nl)
    s2 = next(i for i, ln in enumerate(check) if ln.strip() == "[env:metalio_eink4_plus]")
    e2 = s2 + 1
    while e2 < len(check) and not check[e2].lstrip().startswith("["):
        e2 += 1
    present = [f for f in flags if any(f"-D{f}" in ln for ln in check[s2:e2])]
    if len(present) != len(flags):
        missing = sorted(set(flags) - set(present))
        raise RuntimeError(f"flags not injected into env block: {missing}")


def touch_flag_consumers() -> list[str]:
    """Force recompilation of every TU that reads -DCP_* flags.

    PlatformIO does NOT necessarily rebuild when only the -D flags change: observed
    on 2026-10-24, two variants produced byte-identical binaries because objects were
    reused. Touching the consumers makes the rebuild real (costs ~10-15 min each).
    """
    touched = []
    for rel in ("src/main.cpp",
                "src/system/BootBeacon.cpp",
                "src/system/SerialProxyImpl.cpp",
                "freeink-sdk/libs/hardware/BoardConfig/include/MetalioEInk4PlusBoard.h"):
        fp = os.path.join(PROJ, rel)
        if os.path.exists(fp):
            os.utime(fp, None)
            touched.append(rel)
    return touched


def build(name: str) -> bool:
    log = os.path.join(ROOT, "_scratch", f"build_{name}.log")
    env = dict(os.environ)
    env.update({
        "PLATFORMIO_CORE_DIR": "M:/cpc",
        "TEMP": os.path.join(ROOT, "_scratch"),
        "TMP": os.path.join(ROOT, "_scratch"),
        "PATH": r"C:\Program Files\Git\cmd;M:\hf\penv\Scripts;" + env.get("PATH", ""),
    })
    env.pop("MSYSTEM", None)
    env.pop("MSYS", None)
    t0 = time.time()
    with open(log, "w", encoding="utf-8") as fh:
        rc = subprocess.call(["pio", "run", "-e", "metalio_eink4_plus"], cwd=PROJ,
                             env=env, stdout=fh, stderr=subprocess.STDOUT, shell=True)
    ok = rc == 0 and os.path.exists(BUILD)
    print(f"[{name}] {'OK' if ok else 'FAILED'}  {(time.time()-t0)/60:.1f} min", flush=True)
    return ok


def stage(name: str) -> int:
    d = os.path.join(STAGED, name)
    os.makedirs(d, exist_ok=True)
    for f in os.listdir(d):
        os.remove(os.path.join(d, f))
    data = open(BUILD, "rb").read()
    shutil.copyfile(BUILD, os.path.join(d, "app_full.bin"))
    n = (len(data) + CHUNK - 1) // CHUNK
    for i in range(n):
        open(os.path.join(d, f"app_{i:02d}.bin"), "wb").write(data[i*CHUNK:(i+1)*CHUNK])
    print(f"[{name}] staged {len(data):,} B / {n} chunks", flush=True)
    return len(data)


MANIFEST = os.path.join(STAGED, "MANIFEST.csv")


def write_manifest(rows: list[tuple]) -> None:
    import csv
    new = not os.path.exists(MANIFEST)
    with open(MANIFEST, "a", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        if new:
            w.writerow(["variant", "group", "flags", "purpose", "bytes", "sha256"])
        for r in rows:
            w.writerow(r)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--group", default=None)
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    if a.list:
        for k, (f, g, why) in sorted(VARIANTS.items(), key=lambda kv: (kv[1][1], kv[0])):
            print(f"{g}  {k:16s} {' '.join('-D'+x for x in f)}")
            print(f"      {why}")
        return 0

    names = a.only or [k for k, v in VARIANTS.items() if a.group in (None, v[1])]
    os.makedirs(STAGED, exist_ok=True)
    res = {}
    seen: dict[str, str] = {}
    for name in names:
        flags, group, why = VARIANTS[name]
        print(f"\n=== {group} {name}: {why}", flush=True)
        set_flags(flags)
        touch_flag_consumers()
        ok = build(name) and stage(name) > 0
        digest = ""
        if ok:
            import hashlib
            digest = hashlib.sha256(open(os.path.join(STAGED, name, "app_full.bin"), "rb").read()).hexdigest()
            prev = seen.get(digest)
            if prev:
                print(f"[{name}] !! WARNING: identical binary to {prev} ({digest}) - "
                      f"flags did NOT take effect", flush=True)
            seen[digest] = name
            print(f"[{name}] sha256={digest[:16]}", flush=True)
        write_manifest([(name, group, " ".join("-D" + f for f in flags), why,
                         os.path.getsize(os.path.join(STAGED, name, "app_full.bin")) if ok else 0,
                         digest)])
        res[name] = ok
    print("\n=== summary ===")
    for k, v in res.items():
        print(f"  {k:18s} {'ok' if v else 'FAILED'}")
    return 0 if all(res.values()) else 1


if __name__ == "__main__":
    sys.exit(main())


# ---------------------------------------------------------------------------
# HEADER MODE — the same variant selection, without global invalidation
# ---------------------------------------------------------------------------
# Writing -D flags into platformio.ini makes PlatformIO rebuild all ~401 TUs per variant, and
# touch_flag_consumers() also touches the BoardConfig header, which is included almost everywhere.
# Measured 2026-10-24: 401/401 objects rebuilt per image, ~8.4 min, 24 cores saturated.
#
# Only main.cpp and BringupExt.cpp actually read the per-variant macros. Emitting them into a
# private header means a variant build recompiles exactly two TUs plus the link, and leaves
# platformio.ini untouched.
HEADER = os.path.join(PROJ, "src", "variant_config.h")

_KEEP_IN_INI = {"CP_CONSOLE_ONLY"}   # constant for every variant; stays where it is


def header_mode_available() -> bool:
    """True only for a real generated header. A reverted marker file must NOT count, or the
    builders would silently switch back to header mode after a failed verification."""
    if not os.path.exists(HEADER):
        return False
    try:
        head = open(HEADER, "r", encoding="utf-8").read(200)
    except OSError:
        return False
    return "GENERATED per variant" in head


def set_header(flags: list[str]) -> None:
    """Write the per-variant macros into src/variant_config.h (idempotent, content-stable)."""
    body = ["// GENERATED per variant by build_variants.set_header() - do not edit by hand.",
            "// Included ONLY by main.cpp and BringupExt.cpp so a variant change recompiles two TUs",
            "// instead of the whole project.",
            "#pragma once", ""]
    for f in flags:
        if f in _KEEP_IN_INI:
            continue
        if "=" in f:
            k, v = f.split("=", 1)
            body.append(f"#ifndef {k}\n#define {k} {v}\n#endif")
        else:
            body.append(f"#ifndef {f}\n#define {f} 1\n#endif")
    text = "\n".join(body) + "\n"
    cur = None
    if os.path.exists(HEADER):
        cur = open(HEADER, "r", encoding="utf-8").read()
    if cur != text:
        with open(HEADER, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
    os.utime(HEADER, None)


def touch_header() -> list[str]:
    """Only the variant header needs a fresh mtime; its two consumers follow from the .d files."""
    if os.path.exists(HEADER):
        os.utime(HEADER, None)
        return ["src/variant_config.h"]
    return []


def strip_variant_flags_from_ini() -> list[str]:
    """Remove every -DCP_* except the constants, so build_flags stops changing between variants."""
    with open(INI, "r", encoding="utf-8", newline="") as fh:
        text = fh.read()
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(nl)
    start = next(i for i, ln in enumerate(lines) if ln.strip() == "[env:metalio_eink4_plus]")
    end = start + 1
    while end < len(lines) and not lines[end].lstrip().startswith("["):
        end += 1
    removed = [ln.strip() for ln in lines[start:end]
               if ln.strip().startswith("-DCP_")
               and not any(k in ln for k in _KEEP_IN_INI)]
    block = [ln for ln in lines[start:end]
             if not (ln.strip().startswith("-DCP_") and not any(k in ln for k in _KEEP_IN_INI))]
    with open(INI, "w", encoding="utf-8", newline="") as fh:
        fh.write(nl.join(lines[:start] + block + lines[end:]))
    return removed
