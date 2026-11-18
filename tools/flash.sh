#!/usr/bin/env bash
# Flash the Metalio E-INK4 Plus build: stub + compression first, ROM as a LOUD fallback.
#
# WHY THIS ORDER (measured, not assumed):
#   * The ROM loader was the bottleneck, not the wire. Measured on this unit: 136/139/147 s
#     for the 6.5 MB app image = ~48 KB/s. COM4 enumerates as a USB-Serial-JTAG CDC device,
#     so 921600 baud is nominal - the transfer is USB-paced - and the 52%-of-link figure is
#     really per-block ROM turnaround (16 KB blocks, ack each).
#   * esptool 5.6.0 DOES ship a stub for this chip: targets/esp32s31.py:242 sets
#     ESP32S31ROM.STUB_CLASS = ESP32S31StubLoader, blob at targets/stub_flasher/2/esp32s31.json.
#     So --no-stub was a choice, never a capability limit.
#   * --no-stub also silently turned COMPRESSION off: `write-flash --help` reads
#     "--compress -z  Compress data during transfer (default unless --no-stub is specified)".
#     These images deflate (zlib level 9) to 58.4% of raw - 6,583,776 -> 3,843,106 bytes -
#     so the fast path removes ~42% of the bytes that have to cross the wire, before any
#     stub-side handshake win.
#   * The vendor's official web flasher (burner.cloudzao.com/en/registry) does exactly this
#     on this exact board: its esptool-js wrapper special-cases ESP32-S31 to upload a stub
#     ("ESP32-S31"===chip.CHIP_NAME ? customStubUpload() : runStub()) and then writes with
#     compress:!0. It never uses the ROM loader for data transfer on S31. Vendor and
#     upstream esptool agree; our --no-stub was the outlier.
#
# The ladder is never silent. If the fast path fails we PRINT that and fall back to the
# known-good ROM path (identical addresses and bytes, just slower). What we still refuse is
# a baud ladder: a quiet downgrade is how a degrading link stayed hidden before, so there is
# one baud rate, used by both paths.
#
# Success is "Hash of data verified" in the output, NOT the exit code - esptool can exit 0 on
# a partial write when the port resets mid-transfer. Both paths do the MD5 write+verify that
# prints that line (this chip's ROM implements SPI_FLASH_MD5).
#
# Flash params are unchanged and are a no-op patch here: the built bootloader header at 0x2000
# already reads `e9 03 02 4f` = dio / 80m / 16MB, and esptool only rewrites the header at the
# bootloader offset. This matches build/flasher_args.json (0x2000/0x8000/0x10000), which in
# turn matches the partition table read back from this unit (see partitions.csv).
#
# Idempotent: same regions written again, nothing erased, no partition layout or NVS touched.
# Deliberately NOT used: --erase-all (wipes NVS) and --skip-flashed (skips the write when the
# MD5 already matches, and with it the "Hash of data verified" line we detect success by).
#
# WHICH IMAGE GETS FLASHED (TOOL1) -- this script no longer has a silent default.
#   On 4 Nov 2026 this very script wrote a 1 Nov image (6,618,240 B) from build/ while
#   build-r991 held the new one; only the byte count in the output revealed it. The
#   default path was the defect: "no argument" meant "whatever build/ happens to hold",
#   and build/ is the directory that goes stale because every worker builds in build-*.
#   So:
#     * with an explicit path, that path is used (and reported);
#     * with NO path, the newest build*/crosspoint_metalio.bin is resolved and reported,
#       and if it is NOT build/crosspoint_metalio.bin the script ABORTS. Refusing beats
#       guessing: the whole point is that the operator's assumption and the artifact on
#       disk disagreed, and only they can say which one they meant.
#   The resolved path, size and mtime are printed BEFORE esptool opens the port, so a
#   wrong artifact is visible in the first three lines of the output rather than in a
#   byte count after 6 MB have crossed the wire.
#
# Usage:
#   tools/flash.sh <image.bin>    # flash exactly this app image (the normal way)
#   tools/flash.sh                # refuses unless the newest image IS build/'s
#   PORT=COM7 BAUD=2000000 tools/flash.sh <image.bin>
#   ES=/path/to/esptool.exe tools/flash.sh <image.bin>
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

PORT="${PORT:-COM4}"
BAUD="${BAUD:-921600}"
ES="${ES:-/m/cpc/python_env/idf6.1_py3.12_env/Scripts/esptool.exe}"
DEFAULT_IMAGE="build/crosspoint_metalio.bin"
# BOOT_DIR: where bootloader/ and partition_table/ are read from. Defaults to build/, and
# exists because a worktree that builds only into build-rNNNN has no build/ at all - which is
# how a hand-rolled esptool invocation replaced this script on 18 Nov 2026.
BOOT_DIR="${BOOT_DIR:-build}"

# artifact_info <path> -> "size=... mtime=..." (empty when the file is missing).
artifact_size() {
  stat -c '%s' "$1" 2>/dev/null || echo '?'
}
artifact_mtime() {
  date -r "$1" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || stat -c '%y' "$1" 2>/dev/null || echo '?'
}
report_artifact() {
  local f="$1" label="$2"
  printf 'flash: %s = %s\n' "$label" "$f"
  printf 'flash:   size  = %s bytes\n' "$(artifact_size "$f")"
  printf 'flash:   mtime = %s\n' "$(artifact_mtime "$f")"
}

resolve_newest_image() {
  # One level deep, the layout every build dir here uses. Only real files are
  # candidates, so an empty build-*/ cannot win by being newer as a directory.
  local -a cands=()
  local f
  for f in build/crosspoint_metalio.bin build-*/crosspoint_metalio.bin; do
    [[ -f "$f" ]] && cands+=("$f")
  done
  [[ ${#cands[@]} -eq 0 ]] && return 1
  # Portable newest-first: compare mtimes with stat, sleep-free.
  local best="" best_ts=0 ts
  for f in "${cands[@]}"; do
    ts="$(stat -c '%Y' "$f" 2>/dev/null || echo 0)"
    if (( ts > best_ts )); then
      best_ts=$ts
      best="$f"
    fi
  done
  printf '%s\n' "$best"
}

if [[ $# -ge 1 && -n "${1:-}" ]]; then
  APP_IMAGE="$1"
  if [[ ! -f "$APP_IMAGE" ]]; then
    echo "flash: app image not found: $APP_IMAGE" >&2
    exit 2
  fi
  report_artifact "$APP_IMAGE" "artifact (explicit)"
  abs_given="$(readlink -f "$APP_IMAGE" 2>/dev/null || echo "$APP_IMAGE")"
  abs_default="$(readlink -f "$DEFAULT_IMAGE" 2>/dev/null || echo "$DEFAULT_IMAGE")"
  if [[ "$abs_given" != "$abs_default" ]]; then
    # Not an error -- naming a build-* image is the documented workflow. It is said out
    # loud because the bootloader and partition table still come from build/ below.
    echo "flash: NOTE this is NOT $DEFAULT_IMAGE; bootloader + partition-table come from ${BOOT_DIR}/" >&2
  fi
else
  NEWEST="$(resolve_newest_image)" || {
    echo "flash: no crosspoint_metalio.bin found in build/ or build-*/ - build first" >&2
    exit 2
  }
  report_artifact "$NEWEST" "artifact (newest of build/, build-*/)"
  if [[ ! -f "$DEFAULT_IMAGE" ]]; then
    echo "flash: REFUSING to flash: the newest image is $NEWEST but $DEFAULT_IMAGE does not exist." >&2
    echo "flash: name the artifact you mean:  tools/flash.sh $NEWEST" >&2
    exit 3
  fi
  report_artifact "$DEFAULT_IMAGE" "default (build/)"
  if [[ "$NEWEST" != "$DEFAULT_IMAGE" ]]; then
    echo "flash: REFUSING to flash: the newest image is $NEWEST," >&2
    echo "flash: but the build/ default was what 'no argument' used to mean - that is the" >&2
    echo "flash: stale-image trap that wrote a 1 Nov image on 4 Nov 2026. Nothing is flashed." >&2
    echo "flash: name the artifact you mean, e.g.:  tools/flash.sh $NEWEST" >&2
    exit 3
  fi
  APP_IMAGE="$NEWEST"
fi
for f in "${BOOT_DIR}/bootloader/bootloader.bin" "${BOOT_DIR}/partition_table/partition-table.bin"; do
  if [[ ! -f "$f" ]]; then
    echo "flash: missing $f - run the build first, or set BOOT_DIR to the build dir that has it" >&2
    exit 2
  fi
done

# Shipping-configuration gate. A build whose CP_* options differ from the set the device runs is not
# comparable to it -- on 18 Nov 2026 r165.2..r165.6 silently took CP_ICON_PARTIAL_REPAINT=ON while the
# device ran OFF. Tests cannot see that. Refusing beats flashing something that measures the flag
# instead of the round.
if [[ "${SKIP_FLAG_CHECK:-0}" != "1" ]]; then
  if ! "${PYTHON:-python}" tools/check_build_flags.py "$BOOT_DIR"; then
    echo "flash: REFUSING to flash: ${BOOT_DIR} is not the shipping configuration." >&2
    echo "flash: rebuild with the flags in tools/shipping_flags.txt, or set SKIP_FLAG_CHECK=1 if the" >&2
    echo "flash: difference is deliberate and recorded." >&2
    exit 4
  fi
fi
if [[ ! -x "$ES" ]]; then
  echo "flash: esptool not found at $ES (override with ES=/path/to/esptool.exe)" >&2
  exit 2
fi

OUT="$(mktemp)"
trap 'rm -f "$OUT"' EXIT

# flash_once <label> <stub|rom>  -> 0 only on a verified write.
# Note the flag placement: --no-stub is a TOP-LEVEL esptool option, while --compress /
# --no-compress belong to the write-flash subcommand. Passing them in the wrong group is a
# hard "No such option" error, so they are assembled separately here.
flash_once() {
  local label="$1" mode="$2"
  local start rc elapsed
  local -a cmd=("$ES" --chip esp32s31 --port "$PORT" --baud "$BAUD"
                --before default-reset --after hard-reset)
  if [[ "$mode" == rom ]]; then
    cmd+=(--no-stub)
  fi
  cmd+=(write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m)
  if [[ "$mode" == stub ]]; then
    cmd+=(--compress)
  else
    cmd+=(--no-compress)
  fi
  cmd+=(0x2000 "${BOOT_DIR}/bootloader/bootloader.bin"
        0x8000 "${BOOT_DIR}/partition_table/partition-table.bin"
        0x10000 "$APP_IMAGE")

  echo "flash: ${label} - ${PORT} @ ${BAUD} baud"
  echo "flash:   app image: ${APP_IMAGE} ($(artifact_size "$APP_IMAGE") bytes)"
  start=$SECONDS
  "${cmd[@]}" > "$OUT" 2>&1
  rc=$?
  elapsed=$(( SECONDS - start ))

  if grep -q "Hash of data verified" "$OUT"; then
    echo "flash: OK - ${label}, ${elapsed}s"
    # Echo what esptool actually clocked, so a slower link is visible run to run.
    grep -oE 'Wrote [0-9]+ bytes( \([0-9]+ compressed\))? at 0x[0-9a-f]+ in [0-9.]+ seconds.*' "$OUT" \
      | sed 's/^/flash:   /'
    return 0
  fi

  echo "flash: FAILED - ${label} after ${elapsed}s (esptool exit ${rc})" >&2
  # Only the tail: a failed esptool run can emit thousands of retry lines.
  tail -5 "$OUT" >&2
  return 1
}

if flash_once "stub + compression" stub; then
  exit 0
fi

echo "flash: fast path failed - FALLING BACK to ROM loader, uncompressed (slower)" >&2
if flash_once "ROM, no compression (fallback)" rom; then
  echo "flash: NOTE - fast path is failing on this link; investigate before trusting timings" >&2
  exit 0
fi

echo "flash: both paths failed - check the cable/port, or hold BOOT and retry" >&2
exit 1