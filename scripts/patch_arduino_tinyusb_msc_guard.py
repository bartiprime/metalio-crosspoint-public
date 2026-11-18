"""Require Arduino's TinyUSB master switch for its MSC class sources.

WHY
    Arduino-esp32 gates ``cores/esp32/USBMSC.cpp`` and ``FirmwareMSC.cpp`` (and
    their headers) on ``CONFIG_TINYUSB_MSC_ENABLED`` alone -- in PioArduino that
    symbol can only become true when ``CONFIG_TINYUSB_ENABLED`` is also on,
    because Arduino's own Kconfig declares the dependency.

    This project instead takes ``CONFIG_TINYUSB_MSC_ENABLED`` from the
    **esp_tinyusb** managed component's Kconfig (same symbol name, no such
    dependency), because scope C4 uses esp_tinyusb's MSC-over-SDMMC storage and
    deliberately keeps ``CONFIG_TINYUSB_ENABLED`` OFF so the Arduino core never
    claims the USB peripheral the USB-Serial/JTAG console lives on.

    Result without this patch: the two Arduino TUs get a body but no TinyUSB
    headers, and the build dies with 'TUD_MSC_DESC_LEN was not declared'.

    The guard added here is semantically inert for anybody who uses Arduino's
    own MSC (they must have ``CONFIG_TINYUSB_ENABLED=y`` anyway).

WHY A SCRIPT
    ``idf/components/arduino`` is a junction to the shared PlatformIO package
    ``framework-arduinoespressif32`` (`.gitignore:48`), so the edit is not part
    of the repository. This script is the tracked, reproducible form of it --
    same convention as ``patch_arduino_ble_controller.py`` and
    ``patch_arduino_rom_libc.py``. Run it once per machine (idempotent).

USAGE
    python scripts/patch_arduino_tinyusb_msc_guard.py [--check]
    --check exits 1 if any file still needs the guard (CI / pre-build gate).
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
JUNCTION = REPO / "idf" / "components" / "arduino"

FILES = (
    "cores/esp32/USBMSC.cpp",
    "cores/esp32/USBMSC.h",
    "cores/esp32/FirmwareMSC.cpp",
    "cores/esp32/FirmwareMSC.h",
)

OLD = "#if CONFIG_TINYUSB_MSC_ENABLED"
NEW = (
    "// CrossPoint (Metalio E-INK4 Plus): Arduino's TinyUSB MSC class belongs to\n"
    "// Arduino's TinyUSB stack, so it needs the master switch as well as the MSC\n"
    "// feature. esp_tinyusb's Kconfig defines CONFIG_TINYUSB_MSC_ENABLED on its\n"
    "// own (this build uses that component's MSC over SDMMC), and without\n"
    "// CONFIG_TINYUSB_ENABLED these TUs have no TinyUSB headers at all.\n"
    "// Reproducible from scripts/patch_arduino_tinyusb_msc_guard.py\n"
    "#if CONFIG_TINYUSB_ENABLED && CONFIG_TINYUSB_MSC_ENABLED"
)


def framework_root() -> Path:
    if not JUNCTION.exists():
        raise SystemExit(f"missing {JUNCTION} (the Arduino package junction)")
    return JUNCTION.resolve()


def main(argv: list[str]) -> int:
    check = "--check" in argv
    root = framework_root()
    pending = []
    for rel in FILES:
        path = root / rel
        if not path.is_file():
            print(f"SKIP {rel}: not present")
            continue
        text = path.read_text(encoding="utf-8")
        if NEW.splitlines()[-1] in text:
            print(f"OK   {rel}")
            continue
        if text.count(OLD) != 1:
            print(f"WARN {rel}: guard not found exactly once, leaving alone")
            continue
        pending.append(path)
        if check:
            print(f"NEED {rel}")
            continue
        path.write_text(text.replace(OLD, NEW, 1), encoding="utf-8")
        print(f"PATCHED {rel}")

    if check and pending:
        return 1
    if not pending:
        print("nothing to do")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
