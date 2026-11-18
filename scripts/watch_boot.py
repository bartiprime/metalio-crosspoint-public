#!/usr/bin/env python3
"""Wait for the board's USB-Serial/JTAG to appear, then capture its boot output.

The application powers the USB peripheral down within seconds of starting, so the only way
to see a boot is to already be listening when the cable goes in.

Usage: python watch_boot.py [seconds_to_wait_for_port] [capture_seconds]
"""
import sys
import time

import serial
import serial.tools.list_ports as lp

wait_s = float(sys.argv[1]) if len(sys.argv) > 1 else 120.0
cap_s = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0

print(f"waiting up to {wait_s:.0f}s for a serial port to appear -- replug the device now", flush=True)

t0 = time.time()
port = None
while time.time() - t0 < wait_s:
    ports = list(lp.comports())
    if ports:
        port = ports[0].device
        print(f"port appeared: {port} ({ports[0].description}) at t={time.time()-t0:.1f}s", flush=True)
        break
    time.sleep(0.05)

if not port:
    sys.exit("no port appeared")

# Open immediately -- every millisecond counts before the app takes the peripheral down.
s = None
for _ in range(200):
    try:
        s = serial.Serial(port, 115200, timeout=1)
        break
    except Exception:
        time.sleep(0.02)

if s is None:
    sys.exit("port appeared but could not be opened")

s.dtr = False
s.rts = False

buf = b""
t0 = time.time()
while time.time() - t0 < cap_s:
    try:
        chunk = s.read(8192)
        if chunk:
            buf += chunk
    except Exception:
        # re-enumeration mid-capture: keep what we have
        time.sleep(0.1)
try:
    s.close()
except Exception:
    pass

text = buf.decode("utf-8", "replace")
print(f"bytes={len(buf)} rom_banners={text.count('ESP-ROM')} "
      f"lockups={text.count('CPU LockUp')} panic={text.count('Guru Meditation')}", flush=True)
print("---- output ----", flush=True)
print(text[:6000] if text.strip() else "(silence after port appeared)", flush=True)
