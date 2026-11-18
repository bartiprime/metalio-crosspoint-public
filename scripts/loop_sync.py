#!/usr/bin/env python3
"""Win the 1-second bootloop race and park the chip in ROM download mode.

WHY THIS EXISTS
    The flashed firmware reboots roughly once per second. Launching `esptool` as a CLI
    costs ~1.5 s of interpreter start-up per attempt -- longer than the window it is
    trying to hit -- and each failed attempt also re-pays that cost. The device also
    re-enumerates, so a serial handle opened at the wrong moment dies with
    "Cannot configure port ... device does not exist".

    This stays resident: imports happen once, then it loops open -> usb-reset -> SYNC
    with no per-attempt start-up cost. Each cycle is tens of milliseconds, so dozens of
    windows per second get a chance.

WHAT SUCCESS LOOKS LIKE
    "SYNCED" plus a chip name. At that point the ROM is sitting in download mode and
    stays there after this process exits and the port closes, so the very next
    `esptool --before no-reset write-flash ...` succeeds immediately.

USAGE
    python loop_sync.py [COM4] [seconds]
"""
import sys
import time

sys.path.insert(0, r"M:/cpc/packages/tool-esptoolpy")
import esptool  # noqa: E402

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
budget_s = float(sys.argv[2]) if len(sys.argv) > 2 else 120.0

t0 = time.time()
outer = 0
while time.time() - t0 < budget_s:
    outer += 1
    try:
        # connect_attempts covers sync retries inside one port-open; the outer loop
        # covers the re-enumeration failures that happen before sync can be attempted.
        esp = esptool.detect_chip(
            port=port,
            baud=115200,          # USB-Serial/JTAG ignores this, but the API wants it
            connect_mode="usb-reset",
            connect_attempts=3,
        )
        print(f"SYNCED after {outer} cycles: {esp.CHIP_NAME}", flush=True)
        # Deliberately NO reset: leave the ROM parked in download mode.
        sys.exit(0)
    except SystemExit:
        raise
    except Exception as exc:  # noqa: BLE001 - any failure just means "try the next window"
        msg = f"{type(exc).__name__}: {exc}"
        if outer % 20 == 0:
            print(f"{outer} cycles, last: {msg[:110]}", flush=True)
        time.sleep(0.05)

print(f"gave up after {outer} cycles in {time.time() - t0:.1f}s", flush=True)
sys.exit(1)
