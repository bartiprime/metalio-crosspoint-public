#!/usr/bin/env python3
"""SUPERSEDED by src/system/S31PmaRegionFix.cpp -- read this note first.

The Arduino S31 framework links PREBUILT IDF libraries, so patching IDF source has no
effect on that build (proven: the image was byte-identical). The working fix defines the
symbol in the application instead, which stops the archive member from being pulled in
at all. Keep this script only for a framework = espidf build, where IDF really is
compiled from source.

Make IDF's ESP32-S31 PSRAM PMA programming switchable, so it can be A/B tested.

WHY
    Every app built with this platform dies before setup() at
    esp_hw_support/port/esp32s31/cpu_region_protect.c:49 -- the single line that
    programs PMA entry 7 so external RAM (0x50000000-0x54000000) is reachable:

        PMA_RESET_AND_ENTRY_SET_NAPOT(7, SOC_EXTRAM_LOW,
                                      (SOC_EXTRAM_HIGH - SOC_EXTRAM_LOW),
                                      PMA_NAPOT | PMA_RWX);

    The observed fault is an INSTRUCTION ACCESS FAULT at that very function, whose code
    lives in flash-mapped memory (0x40000000+ on this SoC), followed by a second fault at
    _vector_table. The file itself carries "TODO: [ESP32S31] IDF-15238", i.e. Espressif's
    own S31 support here is unfinished. PMA uses vendor CSRs (PMACFG0=0xBC0,
    PMAADDR0=0xBD0), and the macro first RESETS the entry (zeroing both CSRs) before
    setting NAPOT -- so if entry 7 is what makes flash executable on this silicon,
    zeroing it would unmap the code that is still running. That is exactly the symptom.

    This script wraps the line in a compile-time switch so the hypothesis can be tested
    without editing the toolchain by hand:

        -DCROSSPOINT_S31_SKIP_PSRAM_PMA=1   -> skip the write entirely
        (undefined)                        -> original behaviour

    Cost of skipping: PSRAM at 0x50000000 is not mapped, so SPIRAM init will fail. That
    is an acceptable price for a bring-up test; if it boots, we know the PMA write is the
    fault and can work out the correct entry.

The patch is idempotent and reversible (--revert restores the pristine line).
"""
import os
import shutil
import sys

FRAMEWORK = r"M:/cpc/packages/framework-espidf"
TARGET = os.path.join(
    FRAMEWORK, "components", "esp_hw_support", "port", "esp32s31", "cpu_region_protect.c"
)
BACKUP_DIR = r"M:/_HermesWork/crosspoint-fork/_scratch/fw_analysis/idf_originals"

ORIGINAL = (
    "    //without setting this, psram cannot be reached\n"
    "    PMA_RESET_AND_ENTRY_SET_NAPOT(7, SOC_EXTRAM_LOW, "
    "(SOC_EXTRAM_HIGH - SOC_EXTRAM_LOW), PMA_NAPOT | PMA_RWX);\n"
)

PATCHED = (
    "    //without setting this, psram cannot be reached\n"
    "    // [crosspoint-patch] switchable: see scripts/patch_s31_pma.py. Brief disable of\n"
    "    // PMA entry 7 to test whether this write is what faults dfuring app start-up.\n"
    "#if !defined(CROSSPOINT_S31_SKIP_PSRAM_PMA)\n"
    "    PMA_RESET_AND_ENTRY_SET_NAPOT(7, SOC_EXTRAM_LOW, "
    "(SOC_EXTRAM_HIGH - SOC_EXTRAM_LOW), PMA_NAPOT | PMA_RWX);\n"
    "#endif\n"
)


def main():
    revert = "--revert" in sys.argv
    with open(TARGET, encoding="utf-8", newline="") as fh:
        src = fh.read()

    nl = "\r\n" if "\r\n" in src else "\n"
    original = ORIGINAL.replace("\n", nl)
    patched = PATCHED.replace("\n", nl)

    os.makedirs(BACKUP_DIR, exist_ok=True)
    backup = os.path.join(BACKUP_DIR, "cpu_region_protect.c.orig")

    if revert:
        if not os.path.exists(backup):
            sys.exit("no backup to restore")
        shutil.copy(backup, TARGET)
        print("reverted from", backup)
        return

    if "CROSSPOINT_S31_SKIP_PSRAM_PMA" in src:
        print("already patched")
        return

    if original not in src:
        sys.exit("anchor not found -- inspect the file, IDF may have changed")

    if not os.path.exists(backup):
        shutil.copy(TARGET, backup)
        print("backup written to", backup)

    with open(TARGET, "w", encoding="utf-8", newline="") as fh:
        fh.write(src.replace(original, patched, 1))
    print("patched:", TARGET)


if __name__ == "__main__":
    main()
