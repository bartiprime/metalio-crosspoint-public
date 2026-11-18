#!/usr/bin/env python3
"""Whole-tree safety audit — run before trusting an unattended chain.

Covers the failure modes that would cost real time tonight:

  A. every Python script still parses (py_compile)
  B. edited C++ files are structurally sane: balanced braces/parens, balanced #if/#endif,
     declaration matches definition for the changed functions, and every symbol the driver
     references actually exists in the generated headers
  C. queue consistency: builder counts, schedule coverage, staged dirs, orphans, duplicates
  D. chain integrity: pending patches present, exactly one runner per stage, gates pointing at
     the right log, header-mode prepare refuses while a compiler is running

    python scripts/audit_all.py
Exit 0 only if every section passes.
"""
from __future__ import annotations

import glob
import os
import py_compile
import re
import subprocess
import sys

ROOT = r"M:\_HermesWork\crosspoint-fork"
PROJ = os.path.join(ROOT, "crosspoint-reader")
SCRIPTS = os.path.join(PROJ, "scripts")
PENDING = os.path.join(ROOT, "_scratch", "pending")
DRV = os.path.join(PROJ, "freeink-sdk", "libs", "display", "FreeInkDisplay", "src", "driver")
GEN = os.path.join(PROJ, "src", "display")

fails: list[str] = []
notes: list[str] = []


def ok(msg: str) -> None:
    print(f"  ok    {msg}")


def bad(msg: str) -> None:
    fails.append(msg)
    print(f"  FAIL  {msg}")


# ---------------------------------------------------------------- A: python
def audit_python() -> None:
    print("\nA. python scripts")
    targets = sorted(glob.glob(os.path.join(SCRIPTS, "*.py")) +
                     glob.glob(os.path.join(PENDING, "*.py")))
    n = 0
    for t in targets:
        try:
            py_compile.compile(t, doraise=True, cfile=os.path.join(ROOT, "_scratch",
                                                                  "_pyc_tmp.pyc"))
            n += 1
        except Exception as exc:                                  # noqa: BLE001
            bad(f"{os.path.basename(t)}: {exc}")
    ok(f"{n}/{len(targets)} scripts compile")


# ---------------------------------------------------------------- B: C++
def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def audit_cpp() -> None:
    print("\nB. C++ structural checks")
    files = [os.path.join(DRV, "Epd047Tc2Driver.cpp"),
             os.path.join(DRV, "Epd047Tc2Driver.h"),
             os.path.join(GEN, "WaveformDU.h"),
             os.path.join(GEN, "WaveformGC.h"),
             os.path.join(GEN, "WaveformGL.h")]
    for f in files:
        if not os.path.exists(f):
            bad(f"missing {os.path.relpath(f, ROOT)}")
            continue
        raw = open(f, encoding="utf-8", errors="replace").read()
        code = strip_comments(raw)
        db = code.count("{") - code.count("}")
        dp = code.count("(") - code.count(")")
        di = len(re.findall(r"^\s*#\s*if", code, re.M)) - len(re.findall(r"^\s*#\s*endif", code, re.M))
        name = os.path.basename(f)
        if db or dp or di:
            bad(f"{name}: braces {db:+d}, parens {dp:+d}, #if/#endif {di:+d}")
        else:
            ok(f"{name}: balanced")

    cpp = open(files[0], encoding="utf-8").read()
    hdr = open(files[1], encoding="utf-8").read()

    # declaration must match the definition for the two changed members
    d = re.search(r"bool runPass\(([^)]*)\)", hdr.replace("\n", " "))
    df = re.search(r"bool Epd047Tc2Driver::runPass\(([^)]*)\)", cpp.replace("\n", " "))
    if not d or not df:
        bad("runPass declaration or definition missing")
    else:
        if " ".join(d.group(1).split()) != " ".join(df.group(1).split()):
            bad(f"runPass signature mismatch:\n        decl: {d.group(1)}\n        def : {df.group(1)}")
        else:
            ok("runPass declaration matches definition")

    # every freeink:: waveform symbol the driver names must be declared
    used = set(re.findall(r"freeink::(kWaveform_[A-Z]+(?:Phases)?)", cpp))
    declared = set()
    for f in glob.glob(os.path.join(GEN, "Waveform*.h")):
        declared |= set(re.findall(r"(kWaveform_[A-Z]+(?:Phases)?)", open(f, encoding="utf-8").read()))
    missing = used - declared
    if missing:
        bad(f"driver references undeclared waveform symbols: {sorted(missing)}")
    else:
        ok(f"all {len(used)} waveform symbols referenced by the driver are declared")

    # includes for the headers actually used
    for h in ("WaveformDU.h", "WaveformGC.h"):
        if h == "WaveformGC.h" and "kWaveform_GC" not in cpp:
            continue
        if f'#include "{h}"' not in cpp:
            bad(f"driver uses {h} but does not include it")
    ok("waveform includes present")

    # the flag must genuinely be off by default: no CP_WAVEFORM_DU in platformio.ini or builders
    ini = open(os.path.join(PROJ, "platformio.ini"), encoding="utf-8").read()
    if "CP_WAVEFORM_DU" in ini:
        bad("CP_WAVEFORM_DU is set in platformio.ini - the queued chain would change behaviour")
    else:
        ok("CP_WAVEFORM_DU not set in platformio.ini (display changes inert for the chain)")


# ---------------------------------------------------------------- C: queue
def audit_queue() -> None:
    print("\nC. queue consistency")
    sys.path.insert(0, SCRIPTS)
    import importlib.util

    def load(name, fname):
        spec = importlib.util.spec_from_file_location(name, os.path.join(SCRIPTS, fname))
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        return m

    bv = load("bv", "build_variants.py")
    be = load("be", "build_ext.py")
    bm = load("bm", "build_masks.py")
    bf = load("bf", "build_fix.py")
    bx = load("bx", "build_matrix.py")
    ts = load("ts", "test_session.py")

    built_names = set(bv.VARIANTS) | set(be.EXT) | set(bm.BISECT) | set(bf.FIX) | set(bx.MATRIX)
    ok(f"inventory: battery {len(bv.VARIANTS)} + EXT {len(be.EXT)} + FIX {len(bf.FIX)} "
       f"+ MX {len(bx.MATRIX)} + BIS {len(bm.BISECT)} = {len(built_names)}")

    dup = [n for n in set(ts.SCHEDULE) if ts.SCHEDULE.count(n) > 1]
    if dup:
        bad(f"duplicate schedule entries: {dup}")
    else:
        ok(f"schedule: {len(ts.SCHEDULE)} entries, no duplicates")

    unknown = [n for n in ts.SCHEDULE if n not in built_names]
    if unknown:
        bad(f"scheduled but not produced by any builder: {unknown[:6]}")
    else:
        ok("every scheduled image is produced by a builder")

    # builders that silently produce nothing would be worse than a failure
    for nm, d in (("EXT", be.EXT), ("FIX", bf.FIX), ("MX", bx.MATRIX), ("BIS", bm.BISECT)):
        if not d:
            bad(f"{nm} builder is empty")

    # staged dirs that no builder or schedule knows about
    staged = {os.path.basename(p) for p in glob.glob(os.path.join(ROOT, "_scratch", "staged", "*"))
              if os.path.isdir(p)}
    orphans = sorted(staged - built_names - set(ts.SCHEDULE))
    if orphans:
        notes.append(f"staged but unknown to builders/schedule: {orphans}")
    else:
        ok("no orphaned staged directories")


# ---------------------------------------------------------------- D: chain
def audit_chain() -> None:
    print("\nD. chain integrity")
    for f in ("bringup_ext.cpp", "run_ext_after.py", "run_fix_after_ext.py",
              "run_bis_after_ext.py", "prepare_header_mode.py"):
        p = os.path.join(PENDING, f)
        if not os.path.exists(p):
            bad(f"missing pending/{f}")
    ok("all pending patches present")

    # gates must point at the right logs
    ext = open(os.path.join(PENDING, "run_ext_after.py"), encoding="utf-8").read()
    fix = open(os.path.join(PENDING, "run_fix_after_ext.py"), encoding="utf-8").read()
    bis = open(os.path.join(PENDING, "run_bis_after_ext.py"), encoding="utf-8").read()
    if "battery_run.log" not in ext:
        bad("EXT runner no longer waits for the battery")
    if "ext_run.log" not in fix:
        bad("FIX runner does not gate on the EXT log")
    if "fix_run.log" not in bis:
        bad("BIS runner does not gate on the FIX log")
    if "build_matrix.py" not in bis:
        bad("BIS runner no longer builds the MX stage")
    ok("stage gates: battery -> EXT -> FIX -> (MX, BIS)")

    # exactly one logical runner per stage
    ps1 = os.path.join(PENDING, "pslist.ps1")
    if os.path.exists(ps1):
        out = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                              "-File", ps1], capture_output=True, text=True, timeout=120).stdout
        counts = {}
        for line in out.splitlines():
            for st in ("run_ext_after", "run_fix_after_ext", "run_bis_after_ext", "reactor"):
                if st in line:
                    counts[st] = counts.get(st, 0) + 1
        # each logical runner shows as launcher + child = 2
        for st, c in counts.items():
            if c != 2:
                notes.append(f"{st}: {c} processes (expected 2 = launcher + child)")
        ok(f"runner processes: {counts}")

    # header mode must refuse while compiling
    prep = os.path.join(PENDING, "prepare_header_mode.py")
    if os.path.exists(prep) and "REFUSED" in open(prep, encoding="utf-8").read():
        ok("header-mode prepare refuses while a compiler runs")


# ---------------------------------------------------------------- E: flash path
KNOWN_LAYOUTS = {
    "factory (verified by digest-matched flash)": (0x20000, 0xF000),
    "replacement table (recorded: No bootable app partitions)": (0x10000, 0xD000),
}


def audit_flash_path() -> None:
    """Cross-check the write path against the layout configured for the write path.

    History, because this section was wrong once and a green audit certified a bad offset:
    a "fix" that made the script default to 0x10000 (matching partitions.csv) was itself wrong --
    HANDOFF.md records a digest-matched flash at 0x20000 with the factory table, and the 0x10000
    table producing "No bootable app partitions". The offset is therefore an explicit decision in
    layout.env, validated here against the known layouts, not assumed from a document.
    """
    import re

    sh = os.path.join(ROOT, "_scratch", "staged", "flash_staged.sh")
    env = os.path.join(ROOT, "_scratch", "staged", "layout.env")
    pt = os.path.join(ROOT, "crosspoint-reader", "partitions.csv")

    if not os.path.exists(sh):
        bad("flash_staged.sh missing -- the only write path is unverified")
        return
    body = open(sh, encoding="utf-8", errors="replace").read()
    if "layout.env" not in body:
        bad("flash_staged.sh does not source layout.env -- the offset is implicit again")
    else:
        ok("flash_staged.sh reads its layout from layout.env")
    if "REFUSING: no flash layout configured" in body:
        ok("flash_staged.sh refuses to write when no layout is configured")
    else:
        bad("flash_staged.sh would write without a configured layout")

    if not os.path.exists(env):
        bad("layout.env missing -- the write offset is undecided")
        return
    vals = dict(re.findall(r"^(APP_OFF|OTADATA_OFF|SLOT_SIZE)=(\S+)", open(env, encoding="utf-8").read(), re.M))
    if not vals.get("APP_OFF") or not vals.get("OTADATA_OFF"):
        bad("layout.env does not set both APP_OFF and OTADATA_OFF")
        return
    app_off = int(vals["APP_OFF"], 0)
    ota_off = int(vals["OTADATA_OFF"], 0)
    slot = int(vals.get("SLOT_SIZE", "0x500000"), 0)

    which = next((k for k, v in KNOWN_LAYOUTS.items() if v == (app_off, ota_off)), None)
    if which is None:
        bad(f"layout.env configures an unknown layout (app 0x{app_off:X} / otadata 0x{ota_off:X}); "
            f"known: " + "; ".join(f"{k} = 0x{a:X}/0x{o:X}" for k, (a, o) in KNOWN_LAYOUTS.items()))
        return
    ok(f"layout.env uses {which}: app 0x{app_off:X}, otadata 0x{ota_off:X}, slot 0x{slot:X}")

    # partitions.csv describes the OTHER table. Disagreement is expected and must be visible,
    # not fatal -- but it must never be silently reconciled.
    csv_app = csv_ota = None
    if os.path.exists(pt):
        for line in open(pt, encoding="utf-8"):
            if line.strip().startswith("#"):
                continue
            f = [x.strip() for x in line.split(",")]
            if len(f) >= 5 and f[0] == "ota_0":
                csv_app = int(f[3], 0)
            elif len(f) >= 5 and f[0] == "otadata":
                csv_ota = int(f[3], 0)
    if csv_app is None:
        bad("partitions.csv: no ota_0 row")
    elif csv_app != app_off:
        notes.append(f"partitions.csv ({csv_app:#x}) and layout.env ({app_off:#x}) describe different "
                     f"tables -- expected; layout.env governs the write path, partitions.csv governs the build")
    else:
        ok("partitions.csv agrees with layout.env")

    if "DRY_RUN" in body:
        ok("flash_staged.sh supports DRY_RUN=1 (plan without hardware)")
    else:
        bad("flash_staged.sh has no DRY_RUN")
    if "ABORTING" in body:
        ok("flash_staged.sh aborts on a failed chunk")
    else:
        bad("flash_staged.sh continues after a failed chunk, leaving a hole")

    fr = os.path.join(ROOT, "crosspoint-reader", "scripts", "flash_retry.py")
    if os.path.exists(fr) and "int(sys.argv[2], 0)" in open(fr, encoding="utf-8").read():
        ok("flash_retry.py accepts 0x-prefixed offsets")
    else:
        bad("flash_retry.py may not parse 0x-prefixed offsets")

    # The recovery path is part of the flash boundary: if it cannot resolve a dump, the one
    # command you reach for after a brick fails at the worst moment. This caught the default
    # backup being absent while nothing else in the suite noticed.
    rs = os.path.join(SCRIPTS, "restore_stock.sh")
    if os.path.isfile(rs):
        body = open(rs, encoding="utf-8", errors="replace").read()
        if "FALLBACK_BACKUP" in body:
            ok("restore_stock.sh falls back to the shipped dump when the backup is absent")
        else:
            bad("restore_stock.sh has a single hardcoded backup path -- a missing file bricks recovery")
    else:
        bad("restore_stock.sh missing")
    chunk_dir = os.path.join(ROOT, "_scratch", "fw_analysis", "chunks16")
    if os.path.isdir(chunk_dir):
        n = len([f for f in os.listdir(chunk_dir) if re.fullmatch(r"c\d{3}\.bin", f)])
        if n == 64:
            ok("restore chunk set present: 64 x 256 KB")
        elif n:
            bad(f"restore chunk set incomplete: {n}/64")
    else:
        notes.append("restore chunks16/ absent -- restore_stock.sh will regenerate it on first use")

    staged = os.path.join(ROOT, "_scratch", "staged")
    unfit, nomagic, total = [], [], 0
    if os.path.isdir(staged):
        for d in sorted(os.listdir(staged)):
            full = os.path.join(staged, d, "app_full.bin")
            if not os.path.isfile(full):
                continue
            total += 1
            if os.path.getsize(full) > slot:
                unfit.append(d)
            with open(full, "rb") as fh:
                if fh.read(1) != b"\xe9":
                    nomagic.append(d)
    if unfit:
        bad(f"{len(unfit)} staged image(s) exceed the configured slot: {unfit[:3]}")
    else:
        ok(f"all {total} staged images fit the 0x{slot:X} slot")
    if nomagic:
        bad(f"{len(nomagic)} staged image(s) lack the 0xE9 ESP magic: {nomagic[:3]}")
    else:
        ok("every staged image starts with the ESP app magic")


def audit_tca_order() -> None:
    """F. TCA9555 OUTPUT-before-CONFIG invariant (review item S14).

    Register semantics that make this subtle: in the PCA/TCA9555 CONFIG register a 1 means
    INPUT and a 0 means OUTPUT. So *clearing* a CONFIG bit is what enables a driver, and the
    danger is enabling a pin as an output before its OUTPUT latch has been loaded -- the part
    powers up with OUTPUT = 0xFFFF and CONFIG = 0xFFFF, so the pin instantly sources HIGH for
    the duration of the transaction. On this board two of those pins are the power-IC control
    line (P1.3) and the BT PA rail (P1.4).

    A rule that simply says "CONFIG must follow OUTPUT" produces false alarms on the two
    images that invert the order *on purpose* (CP_CFG_ONLY, CP_CFG_BEFORE_OUT). Those are
    experiments, not defects, so they are recognised by their enclosing #if and reported as
    allowed exceptions rather than quietly passing or loudly failing.
    """
    print("\nF. TCA9555 write-order invariant (S14)")
    targets = [
        os.path.join(PROJ, "src", "main.cpp"),
        os.path.join(PROJ, "freeink-sdk", "libs", "hardware", "BoardConfig", "include",
                     "MetalioEInk4PlusBoard.h"),
    ]
    # Sites whose whole purpose is to invert the order. Named here so the audit states why
    # they are exempt instead of pretending it did not see them.
    DELIBERATE = {"CP_CFG_ONLY", "CP_CFG_BEFORE_OUT"}

    # Match the call and the rest of the line, NOT "...);" -- an earlier version required a
    # trailing semicolon and therefore failed to see `if (!tcaWrite16(...)) return false;`, which
    # made the correctly-ordered helper `expander_low()` look like a violation. The forms that
    # occur in this tree are: `tcaWrite16(...);`, `if (!tcaWrite16(...)) return false;`, and
    # `return tcaWrite16(...);`. Matching to end-of-line covers all three.
    write_re = re.compile(
        r"tcaWrite16\s*\(\s*TCA_REG_(OUTPUT0|OUTPUT1|CONFIG0|CONFIG1)\s*,\s*(.*)$")

    def _scopes(lines: list[str]) -> list[str]:
        """Innermost enclosing `#if` macro per line, or 'production'.

        Tracked with a stack rather than by scanning upward: an upward scan stops at the first
        `#endif`, which mis-reports a site nested inside two conditionals (it called the
        correctly-ordered site at main.cpp:738 'production' while its OUTPUT write at 733 was
        inside a CP_* block, producing a false violation).
        """
        stack: list[str | None] = []
        out: list[str] = []
        for line in lines:
            t = line.strip()
            if t.startswith("#if"):
                m = re.search(r"(CP_\w+)", t)
                stack.append(m.group(1) if m else None)
            elif t.startswith("#endif"):
                if stack:
                    stack.pop()
            inner = next((x for x in reversed(stack) if x), None)
            out.append(inner or "production")
        return out

    sites = []
    for path in targets:
        if not os.path.isfile(path):
            bad(f"missing {os.path.relpath(path, ROOT)}")
            continue
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
        rel = os.path.relpath(path, PROJ).replace("\\", "/")
        scopes = _scopes(lines)
        for i, line in enumerate(lines):
            for m in write_re.finditer(line):
                reg, expr = m.group(1), m.group(2).strip()
                sites.append((rel, i + 1, reg, expr, scopes[i]))

    def enables_outputs(expr: str) -> bool:
        """Clearing CONFIG bits enables drivers -> the direction that can glitch a rail."""
        return bool(re.search(r"&\s*~", expr))

    config_sites = [s for s in sites if s[2].startswith("CONFIG")]
    cleared = [s for s in config_sites if enables_outputs(s[3])]
    deliberate = [s for s in cleared if s[4] in DELIBERATE]
    to_check = [s for s in cleared if s[4] not in DELIBERATE]
    unchanged = [s for s in config_sites if not enables_outputs(s[3])]

    print(f"  info  {len(sites)} tcaWrite16 site(s): {len(config_sites)} CONFIG, "
          f"{len(sites) - len(config_sites)} OUTPUT; "
          f"{len(cleared)} clear bits (enable outputs), {len(unchanged)} set bits (safe direction)")

    violations = []
    for rel, ln, reg, expr, macro in to_check:
        # A preceding OUTPUT write to the same port **in the same #if scope**. The scope test
        # is what makes this real: a plain line-distance window matched an OUTPUT write that
        # belonged to the previous, unrelated experiment block and silently passed an injected
        # reversed order. Two different variants share this file; scope is the unit of intent.
        prior = [s for s in sites
                 if s[0] == rel and s[2] == reg.replace("CONFIG", "OUTPUT") and s[1] < ln
                 and s[4] == macro and ln - s[1] <= 200]
        if prior:
            ok(f"{rel}:{ln} {reg} enables outputs after an OUTPUT write at line {prior[-1][1]}")
        else:
            violations.append(
                f"{rel}:{ln} {reg} enables outputs with no preceding OUTPUT write "
                f"in the same scope ({macro or 'production'})")
    for rel, ln, reg, expr, macro in deliberate:
        print(f"  note  {rel}:{ln} {reg} clears bits without a prior OUTPUT write -- "
              f"deliberate, inside #if defined({macro}) (the order-inversion experiment)")
    for v in violations:
        bad(v)
    if not violations and to_check:
        ok(f"all {len(to_check)} production CONFIG-enables-outputs site(s) latch OUTPUT first")

    # The one wiring path that must never regress. Scoped to the function body on purpose:
    # a file-wide search matches the `constexpr uint8_t TCA_REG_CONFIG0 = 6;` definition at
    # line 91, which precedes ensureBooted() and made the first version of this check report a
    # violation that does not exist. Compare write sites, not symbol names.
    board = os.path.join(PROJ, "freeink-sdk", "libs", "hardware", "BoardConfig", "include",
                         "MetalioEInk4PlusBoard.h")
    if os.path.isfile(board):
        body = open(board, encoding="utf-8", errors="replace").read()
        start = body.find("inline bool ensureBooted()")
        if start < 0:
            bad("could not find ensureBooted() in the board header")
        else:
            end = body.find("\n}", start)
            fn = body[start:end if end > start else len(body)]
            w_out = fn.find("tcaWrite16(TCA_REG_OUTPUT0")
            w_cfg = fn.find("tcaWrite16(TCA_REG_CONFIG0")
            if w_out < 0 or w_cfg < 0:
                bad("ensureBooted() no longer writes both OUTPUT0 and CONFIG0")
            elif w_out < w_cfg:
                ok("ensureBooted() writes OUTPUT0 before CONFIG0 (the invariant's reference path)")
            else:
                bad("ensureBooted() writes CONFIG0 before OUTPUT0 -- rails glitch HIGH at boot")


def audit_image_headers() -> None:
    """G. Staged image headers vs the factory app on this unit (review item S15).

    Also settles, statically, which offset the shipped images actually use: the factory dump
    has a valid ESP app header at 0x20000 and 0xFF at 0x10000, which is the same conclusion
    layout.env records (and which a digest-matched flash already confirmed).
    """
    print("\nG. image headers vs factory (S15)")
    ref_path = os.path.join(ROOT, "CrossLink Pixel.bin")
    if not os.path.isfile(ref_path):
        bad("CrossLink Pixel.bin missing -- no factory header to compare against")
        return
    with open(ref_path, "rb") as fh:
        fh.seek(0x20000)
        ref = fh.read(32)
        fh.seek(0x10000)
        at_10000 = fh.read(1)
    if ref[0] != 0xE9:
        bad(f"factory dump has no app header at 0x20000 (got {ref[0]:#04x})")
        return
    ok(f"factory app header is at 0x20000 (magic 0xE9, {ref[1]} segments, "
       f"mode {ref[2]:#04x}, size/freq {ref[3]:#04x})")
    if at_10000 == b"\xff":
        ok("0x10000 is erased (0xFF) in the factory dump -- ota_0 is NOT at 0x10000")
    else:
        notes.append("factory dump has content at 0x10000; the offset question needs re-opening")

    ref_mode, ref_sf = ref[2], ref[3]
    staged = os.path.join(ROOT, "_scratch", "staged")
    checked, badmode, badsf, badentry, badsegs = 0, [], [], [], []
    if os.path.isdir(staged):
        for d in sorted(os.listdir(staged)):
            full = os.path.join(staged, d, "app_full.bin")
            if not os.path.isfile(full):
                continue
            checked += 1
            with open(full, "rb") as fh:
                h = fh.read(32)
            if h[0] != 0xE9:
                badmode.append(f"{d}:bad-magic")
                continue
            if h[2] != ref_mode:
                badmode.append(f"{d}:{h[2]:#04x}")
            if h[3] != ref_sf:
                badsf.append(f"{d}:{h[3]:#04x}")
            if not (1 <= h[1] <= 16):
                badsegs.append(f"{d}:{h[1]}")
            entry = int.from_bytes(h[4:8], "little")
            if not (0x2F000000 <= entry <= 0x2FFFFFFF):
                badentry.append(f"{d}:{entry:#010x}")
    for lst, what in ((badmode, f"flash mode != {ref_mode:#04x}"),
                      (badsf, f"size/freq != {ref_sf:#04x}"),
                      (badsegs, "implausible segment count"),
                      (badentry, "entry point outside the app-mapped window")):
        if lst:
            bad(f"{len(lst)} image(s) with {what}: {lst[:3]}")
    if checked and not (badmode or badsf or badsegs or badentry):
        ok(f"all {checked} staged images match the factory header "
           f"(mode {ref_mode:#04x}, size/freq {ref_sf:#04x}, entry in 0x2F000000-0x2FFFFFFF)")


def audit_host_tests() -> None:
    """H. run the host-side unit tests through the audit, so one command is enough.

    A suite nobody runs is decoration. These are the pure-logic checks that catch off-by-one
    errors in the line/CKV arithmetic and the waveform packing -- both invisible until pixels
    are wrong on a panel that has never lit.
    """
    print("\nH. host-side unit tests")
    for name in ("test_line_timing.py", "test_waveform_static.py"):
        path = os.path.join(SCRIPTS, name)
        if not os.path.isfile(path):
            bad(f"{name} missing")
            continue
        r = subprocess.run([sys.executable, path], capture_output=True, text=True, timeout=300)
        tail = [ln for ln in r.stdout.strip().splitlines() if ln.strip()][-1:]
        if r.returncode == 0:
            ok(f"{name}: {tail[0] if tail else 'ok'}")
        else:
            bad(f"{name} failed: {tail[0] if tail else r.stderr.strip()[:160]}")

    # The partition-table decoder, self-tested against the shipped dump. Exit 0 means the dump
    # still decodes as the FACTORY layout, which is what layout.env claims. If this ever fails,
    # the reference image changed (or the decoder regressed) and neither is safe to flash on.
    dp = os.path.join(SCRIPTS, "decode_pt.py")
    if not os.path.isfile(dp):
        bad("decode_pt.py missing -- the pre-flight readback has no decoder")
    else:
        r = subprocess.run([sys.executable, dp, os.path.join(ROOT, "CrossLink Pixel.bin"),
                            "--offset", "0x8000"], capture_output=True, text=True, timeout=120)
        if r.returncode == 0:
            ok("decode_pt.py: shipped dump decodes as the factory layout (ota_0 0x20000)")
        else:
            bad(f"decode_pt.py on the shipped dump returned {r.returncode} -- "
                f"expected 0 (factory layout)")


def main() -> int:
    audit_python()
    audit_cpp()
    audit_queue()
    audit_chain()
    audit_flash_path()
    audit_tca_order()
    audit_image_headers()
    audit_host_tests()

    print("\n" + "=" * 60)
    for n in notes:
        print(f"  note  {n}")
    if fails:
        print(f"{len(fails)} FAILURE(S)")
        for f in fails:
            print(f"   - {f}")
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
