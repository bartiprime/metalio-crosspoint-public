#!/usr/bin/env python3
"""Sync, upload the stub and write flash as one retried unit.

Why this shape: on this device the ROM refuses flash operations without the stub
("Failed to enter flash download mode, result 0106"), and uploading the stub makes the
USB-Serial/JTAG re-enumerate, which invalidates the serial handle. Any single
sync->write sequence can therefore die at an arbitrary point. What works is retrying the
*whole* unit inside one resident process, because the chip is usually left sitting in
download mode after a failure, so the next attempt connects almost instantly.

Usage:
    python flash_retry.py COM4 <offset> <file> [attempts]
"""
import sys
import time

sys.path.insert(0, r"M:/cpc/packages/tool-esptoolpy")
import esptool  # noqa: E402
from esptool import cmds  # noqa: E402

port = sys.argv[1]
offset = int(sys.argv[2], 0)
path = sys.argv[3]
max_attempts = int(sys.argv[4]) if len(sys.argv) > 4 else 8

modes = ["usb-reset", "no-reset", "usb-reset", "default-reset"]

for attempt in range(1, max_attempts + 1):
    mode = modes[(attempt - 1) % len(modes)]
    print(f"--- attempt {attempt}/{max_attempts} (connect_mode={mode})", flush=True)
    esp = None
    try:
        # inner sync loop: dozens of tries per second, no interpreter restart
        deadline = time.time() + 25
        while time.time() < deadline and esp is None:
            try:
                esp = esptool.detect_chip(
                    port=port, baud=115200, connect_mode=mode, connect_attempts=3
                )
            except SystemExit:
                raise
            except Exception:  # noqa: BLE001
                time.sleep(0.05)
        if esp is None:
            print("   no sync in 25s", flush=True)
            continue

        print(f"   synced: {esp.CHIP_NAME}; uploading stub", flush=True)
        esp = cmds.run_stub(esp)
        print("   stub running; writing", flush=True)
        cmds.write_flash(
            esp, [(offset, path)], flash_mode="dio", flash_freq="80m", flash_size="16MB"
        )
        print("WRITE COMPLETE", flush=True)
        try:
            esp.hard_reset()
        except Exception:  # noqa: BLE001
            pass
        sys.exit(0)
    except SystemExit:
        raise
    except Exception as exc:  # noqa: BLE001
        print(f"   failed: {type(exc).__name__}: {str(exc)[:120]}", flush=True)
        time.sleep(0.4)

print("all attempts failed", flush=True)
sys.exit(1)
