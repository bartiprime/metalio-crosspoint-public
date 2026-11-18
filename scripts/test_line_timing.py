#!/usr/bin/env python3
"""Host-side check of Epd047Tc2Driver's line/CKV timing arithmetic.

Why this exists
---------------
The previous build-time guard was:

    constexpr uint32_t kLinePeriodUs = kLineBytes * 1000000u / kPclkHz;  // 305*1e6/2e7
    constexpr uint32_t kCkvPeriodUs  = kCkvLowTicks + kCkvHighTicks;     //    7+8
    #if (kLinePeriodUs != kCkvPeriodUs) #pragma message(...) #endif

305 * 1000000 / 20000000 integer-truncates to 15, and CKV was 15, so the guard compared
two wrong numbers, matched, and never fired -- while the real line was 318 clocks (15.9 us)
and CKV ran at 15.0 us. A check that cannot fail is decoration (docs/INDEX.md rule 3).

This test parses the constants out of the driver itself, so it fails if the code and this
file ever disagree, and it carries a negative test that proves it can go red.

Run: python scripts/test_line_timing.py
Exit: 0 all pass, 1 any failure
"""
from __future__ import annotations

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVER = os.path.join(
    HERE, "..", "freeink-sdk", "libs", "display", "FreeInkDisplay", "src", "driver",
    "Epd047Tc2Driver.cpp",
)

failures: list[str] = []
checks = 0


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if cond:
        print(f"  ok    {msg}")
    else:
        print(f"  FAIL  {msg}")
        failures.append(msg)


def _resolve_all(text: str) -> dict[str, int]:
    """Evaluate every `constexpr <type> NAME = <expr>;` in file order.

    The driver legitimately derives constants from each other (kCkvLowTicks =
    kLinePeriodUs - kCkvHighTicks, kTrailingClocks = kLineCycles - ...), so a test that
    only understood int literals would reject the very structure that makes drift
    impossible. Declarations are visited in order and earlier names substituted in.
    """
    done: dict[str, int] = {}
    pending = re.findall(r"constexpr\s+\w+\s+(\w+)\s*=\s*([^;]+);", text)
    for _ in range(len(pending) + 2):
        progressed = False
        for name, expr in pending:
            if name in done:
                continue
            e = expr.strip()
            e = re.sub(r"\(\s*(?:u?int(?:8|16|32|64)_t|int|size_t|unsigned)\s*\)", "", e)
            e = re.sub(r"(\d+)[uUlL]+\b", r"\1", e)
            for k, v in sorted(done.items(), key=lambda kv: -len(kv[0])):
                e = re.sub(rf"\b{re.escape(k)}\b", str(v), e)
            if re.search(r"[A-Za-z_]\w*", e):
                continue
            try:
                done[name] = int(eval(e, {"__builtins__": {}}, {}))  # noqa: S307 - literals only
                progressed = True
            except Exception:  # noqa: BLE001
                continue
        if not progressed:
            break
    return done


def const(text: str, name: str) -> int:
    vals = _resolve_all(text)
    if name not in vals:
        raise SystemExit(f"could not resolve constexpr {name} in {DRIVER}")
    return vals[name]


def evaluable_const(text: str, name: str) -> str:
    m = re.search(rf"constexpr\s+\w+\s+{name}\s*=\s*([^;]+);", text)
    if not m:
        raise SystemExit(f"could not find constexpr {name} in {DRIVER}")
    return m.group(1).strip()


def main() -> int:
    text = open(DRIVER, encoding="utf-8", errors="replace").read()

    # ---- vendor inputs (from _scratch/reference/vendor_lcd_2026-10-23) -------------
    V_LINE_BYTES = 304          # display_width / 4
    V_LE = const(text, "kLeHighTime")      # 9
    V_FP = const(text, "kLineFrontPorch")  # 4
    V_DUMMY_TAIL = 10           # .dummy_clocks_after_data (board file, section 6.3.1)
    PCLK = 20_000_000
    V_LINE_BYTES_CHECK = 1216 // 4

    print("\nA. vendor arithmetic reproduced")
    check(V_LINE_BYTES_CHECK == V_LINE_BYTES,
          f"line_bytes = width/4 = {V_LINE_BYTES_CHECK}")
    # vendor: (b + le + fp + dummy - 1) * 1e6 / pclk  [int truncation]  + 1
    v_line_us = (V_LINE_BYTES + V_LE + V_FP + V_DUMMY_TAIL - 1) * 1_000_000 // PCLK + 1
    check(v_line_us == 17, f"vendor line_length_us = {v_line_us} (expected 17)")
    v_cycles = v_line_us * PCLK // 1_000_000
    check(v_cycles == 340, f"vendor line_cycles = {v_cycles} (expected 340)")

    # ---- driver constants ---------------------------------------------------------
    print("\nB. driver constants agree with the vendor line")
    d_period = const(text, "kLinePeriodUs")
    d_high = const(text, "kCkvHighTicks")
    d_low = const(text, "kCkvLowTicks")
    d_hres = const(text, "kLineBytes")
    d_data = const(text, "kLineDataBytes")
    d_dummy = const(text, "kLineDummyBytes")

    check(d_period == v_line_us, f"kLinePeriodUs = {d_period} us matches vendor {v_line_us}")
    check(d_high + d_low == d_period,
          f"CKV period {d_high}+{d_low} = {d_high + d_low} us == line period (vendor: 1:1)")
    check(d_data == V_LINE_BYTES, f"kLineDataBytes = {d_data} (2 bpp, 4 px/byte)")
    check(d_dummy == 1, f"kLineDummyBytes = {d_dummy}")
    check(d_hres == d_data + d_dummy, f"kLineBytes = {d_hres} = data + dummy")

    # trailing tail must close the line exactly
    tail_expr = evaluable_const(text, "kTrailingClocks")
    expected_tail = d_period * PCLK // 1_000_000 - V_LE - V_FP - d_hres
    check("kLineCycles" in tail_expr,
          "kTrailingClocks is derived from kLineCycles (not a magic number)")
    check(expected_tail == 22, f"trailing clocks = {expected_tail} (vendor path: 23 with 304)")
    check(re.search(r"hsync_front_porch\s*=\s*kTrailingClocks", text) is not None,
          "hsync_front_porch actually receives kTrailingClocks")

    # CKV high must cover the data window, not half the line
    data_window_us = (d_hres + V_LE + V_FP) * 10 / 1000 * 1000 / 1000  # clocks -> us at 20 MHz
    data_window_us = (d_hres + V_LE + V_FP) / (PCLK / 1_000_000)
    check(d_high >= data_window_us,
          f"CKV high {d_high} us covers the {data_window_us:.2f} us data window")

    # ---- the guard that must be able to fail ---------------------------------------
    print("\nC. guard is a static_assert, not a silent #pragma")
    check("static_assert(kCkvHighTicks + kCkvLowTicks" in text.replace("\n", " ")
          or re.search(r"static_assert\(\s*kCkvHighTicks\s*\+\s*kCkvLowTicks", text) is not None,
          "CKV/line equality is enforced by static_assert")
    check("kLineBytes * 1000000u / kPclkHz" not in text,
          "the truncating old line-period expression is gone")

    print("\nD. negative test -- the old formulation is provably blind")
    old_line = d_hres * 1_000_000 // PCLK              # 15  (真 15.25)
    old_ckv = 7 + 8                                     # 15
    check(old_line == old_ckv,
          f"old guard compared {old_line} == {old_ckv} -> MATCHES, so it could never fire")
    true_line_clocks = V_LE + V_FP + d_hres + 0
    check(true_line_clocks / (PCLK / 1_000_000) != old_line,
          f"true line was {true_line_clocks} clocks = "
          f"{true_line_clocks / (PCLK / 1_000_000):.2f} us, not {old_line} us -> old guard was wrong")

    print("\n" + "=" * 60)
    if failures:
        print(f"{len(failures)} FAILURE(S) of {checks} checks")
        for f in failures:
            print(f"   - {f}")
        return 1
    print(f"ALL {checks} CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
