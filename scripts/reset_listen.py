#!/usr/bin/env python3
"""Reset the board and capture its console output.

Usage: python reset_listen.py [COM4] [seconds]

Why a helper: the board's USB-Serial/JTAG disappears when the application powers the main
rail down, and `esptool ... --after hard-reset` alone often does not actually reset over
USB. This runs the reset first, then opens the port with DTR/RTS deasserted (asserting DTR
holds the chip in reset), and reports how many ROM banners appear -- more than one means a
bootloop.
"""
import subprocess
import sys
import time

py = r"M:/hf/penv/Scripts/python.exe"
esp = r"M:/cpc/packages/tool-esptoolpy/esptool.py"

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0

warm = "--no-reset" in sys.argv

if not warm:
    subprocess.run(
        [py, esp, "--chip", "esp32s31", "--port", port, "--before", "usb-reset",
         "--after", "hard-reset", "run"],
        capture_output=True, text=True, timeout=40,
    )
    time.sleep(1.5)

import serial  # noqa: E402  (penv python has pyserial)

try:
    s = serial.Serial(port, 115200, timeout=1)
except Exception as exc:  # noqa: BLE001
    sys.exit(f"cannot open {port}: {exc}")

s.dtr = False
s.rts = False
buf = b""
t0 = time.time()
while time.time() - t0 < seconds:
    try:
        chunk = s.read(8192)
        if chunk:
            buf += chunk
    except Exception:  # noqa: BLE001 - re-enumeration mid-read
        time.sleep(0.2)
try:
    s.close()
except Exception:  # noqa: BLE001
    pass

text = buf.decode("utf-8", "replace")
print(f"bytes={len(buf)} rom_banners={text.count('ESP-ROM')} "
      f"lockups={text.count('CPU LockUp')} panics={text.count('Guru Meditation') + text.count('abort()')}")
print("---- output ----")
print(text[-2500:] if text.strip() else "(silence)")
