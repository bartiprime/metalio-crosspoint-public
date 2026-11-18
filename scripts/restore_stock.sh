#!/usr/bin/env bash
# Restore the Metalio E-INK4 Plus to its pre-experiment state.
#
# Writes a full 16 MB dump back over the whole flash in 256 KB chunks, one process per
# chunk. A single 16 MB write-flash dies partway (the USB-Serial/JTAG re-enumerates), which
# is why this loops instead of issuing one command.
#
# OFFSETS: this restores whatever dump you give it, byte for byte, from 0x0. The dump that
# actually ships on this unit is "CrossLink Pixel.bin": 16 MB, partition table at 0x8000, and
# a valid ESP app image at 0x20000 with 0x10000 ERASED. (An earlier note in this header said
# ota_0 was at 0x10000 -- that is the replacement table recorded in partitions.csv, which is
# not what the factory dump contains. Verified by reading the header bytes directly.)
# STOCKFW.bin uses yet another layout (ota_0 at 0x80000) and is not this unit's dump.
# Always re-read the partition table at 0x8000 after restoring before flashing an app.
#
# PREREQUISITE: no BOOT button is needed, even if the flashed app bootloops --
# scripts/loop_sync.py catches the ROM window from inside the flasher. Only a USB peripheral
# that has dropped off the bus entirely needs a cable replug.
#
# Usage: bash restore_stock.sh [COM4] [backup.bin]
set -euo pipefail

ROOT="M:/_HermesWork/crosspoint-fork"
CHUNK_DIR="$ROOT/_scratch/fw_analysis/chunks16"
PY="M:/hf/penv/Scripts/python.exe"
PORT="${1:-COM4}"

# Pick the dump. The original per-experiment backup (device_backup_16MB_sleepnever.bin) is no
# longer on disk, so fall back to the shipped-image dump rather than dying at "missing backup"
# on a board that needs recovering -- that failure would land at the worst possible moment.
DEFAULT_BACKUP="$ROOT/_scratch/fw_analysis/device_backup_16MB_sleepnever.bin"
FALLBACK_BACKUP="$ROOT/CrossLink Pixel.bin"
if [ -n "${2:-}" ]; then
    BACKUP="$2"
elif [ -f "$DEFAULT_BACKUP" ]; then
    BACKUP="$DEFAULT_BACKUP"
elif [ -f "$FALLBACK_BACKUP" ]; then
    BACKUP="$FALLBACK_BACKUP"
    echo "note: per-experiment backup absent; using the shipped-image dump ($(basename "$FALLBACK_BACKUP"))" >&2
else
    echo "no backup available: neither $DEFAULT_BACKUP nor $FALLBACK_BACKUP exists" >&2
    exit 1
fi
SZ=$(stat -c%s "$BACKUP")
[ "$SZ" = "16777216" ] || { echo "backup is $SZ bytes, expected 16777216" >&2; exit 1; }

# Build the 256 KB chunks once, inside the sandbox.
if [ ! -f "$CHUNK_DIR/c063.bin" ]; then
    echo "writing chunks to $CHUNK_DIR ..."
    "$PY" - "$BACKUP" "$CHUNK_DIR" <<'PY'
import os, sys
src, outdir = sys.argv[1], sys.argv[2]
os.makedirs(outdir, exist_ok=True)
d = open(src, "rb").read()
CH = 256 * 1024
for i in range(len(d) // CH):
    open(os.path.join(outdir, f"c{i:03d}.bin"), "wb").write(d[i * CH:(i + 1) * CH])
print("chunks:", len(d) // CH)
PY
fi

CH=$((0x40000))
ok=0
for i in $(seq 0 63); do
    OFF=$((i * CH))
    F=$(printf "%s/c%03d.bin" "$CHUNK_DIR" "$i")
    if timeout 120 "$PY" "$ROOT/crosspoint-reader/scripts/flash_retry.py" "$PORT" "$OFF" "$F" 3 \
         | grep -q "WRITE COMPLETE"; then
        ok=$((ok + 1))
    else
        echo "chunk $i @0x$(printf %06X "$OFF") FAILED" >&2
        exit 1
    fi
done

echo "RESTORE DONE: $ok/64 chunks written - device should boot the factory firmware."
