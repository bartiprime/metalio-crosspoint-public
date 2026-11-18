#!/usr/bin/env bash
# Restore the ESP32-S31 DIO bootloader artifact that the pioarduino package omits.
#
# WHY THIS EXISTS
#   framework-arduinoespressif32-libs/esp32s31/bin/ ships ONLY
#   bootloader_qio_80m.elf. Our board must use DIO (both reference images,
#   STOCKFW.bin and "CrossLink Pixel.bin", carry a flash-mode byte of 0x02 =
#   DIO @ 40 MHz), so the builder looks for bootloader_dio_80m.elf and fails with
#   "Source ... not found, needed by target bootloader.bin".
#
# WHY A COPY IS HARMLESS
#   The .elf is bootloader *code*. The platform converts it with
#       esptool --chip esp32s31 elf2image --flash-mode <board_build.flash_mode> ...
#   so the resulting bootloader.bin header is stamped DIO regardless of the name
#   the elf was shipped under. Nothing in the code path reads the elf's own
#   header byte.
#
# TRADE-OFF
#   The qio variant is built for 80 MHz. Verify the produced bootloader.bin
#   header after a build: byte 2 should be 0x02 (DIO/40MHz) once the board
#   config supplies 40m, or 0x02|0x_0 with an 80 MHz nibble if flash_freq is 80m.
#   If the device fails to boot, try flash_freq 40m first -- the stock image runs
#   40 MHz.
#
# RUN THIS after a clean of the PlatformIO packages, or on a new machine.
set -euo pipefail

BIN_DIR="${1:-M:/cpc/packages/framework-arduinoespressif32-libs/esp32s31/bin}"
SRC="$BIN_DIR/bootloader_qio_80m.elf"
DST="$BIN_DIR/bootloader_dio_80m.elf"

[ -f "$SRC" ] || { echo "missing source: $SRC" >&2; exit 1; }

if [ -f "$DST" ]; then
    cmp -s "$SRC" "$DST" && echo "already in place: $DST" && exit 0
    echo "WARNING: $DST exists and differs from $SRC -- leaving it alone" >&2
    exit 1
fi

cp "$SRC" "$DST"
echo "created $DST (copy of bootloader_qio_80m.elf)"
