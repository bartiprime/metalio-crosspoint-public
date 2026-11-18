#!/usr/bin/env bash
# Fetch the dependencies this repository does NOT track, then verify the ones it does.
#
# WHY THIS EXISTS
#   The Application component needs two things that are too large to vendor and are
#   git-ignored by design:
#     1. the Arduino core, as an ESP-IDF component at idf/components/arduino (79 MB),
#     2. Arduino-wolfSSL, at .pio/libdeps/metalio_eink4_plus/Arduino-wolfSSL (45 MB, GPLv2).
#   A fresh clone therefore fails at CONFIGURE time with
#       CMake Error at idf/components/crosspoint_app/CMakeLists.txt:196
#         Arduino-wolfSSL not found at .../.pio/libdeps/metalio_eink4_plus/Arduino-wolfSSL/src
#   which reads like a broken tree. It is not: it is a missing prerequisite.
#
#   Everything else the build needs is vendored in idf/components/extern/ (SdFat,
#   ArduinoJson, QRCode, PNGdec, JPEGDEC, AnimatedGIF, WebSockets) or fetched by the
#   IDF component manager into idf/managed_components/ on the first build.
#
# Run from anywhere; paths are resolved from this script's location. Idempotent.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARDUINO_DIR="$ROOT/idf/components/arduino"
WOLF_DIR="${CROSSPOINT_WOLFSSL_DIR:-$ROOT/.pio/libdeps/metalio_eink4_plus/Arduino-wolfSSL}"

ARDUINO_REF="4.0.0-RC1"   # reports version 4.0.0; the only 4.x ref with variants/esp32s31
WOLF_REF="5.7.2"          # wolfssl/Arduino-wolfSSL @ 5.7.2, as pinned in platformio.ini

say() { printf 'bootstrap: %s\n' "$*"; }
die() { printf 'bootstrap: ERROR: %s\n' "$*" >&2; exit 1; }

# Native Windows git cannot be handed an MSYS path (/m/...) as an argument - it fails with
# "cannot change to '/m/...'". Clone with a RELATIVE destination from the parent directory
# instead: bash's cd is MSYS-aware, the argument git sees contains no path prefix. Works
# unchanged on Linux/macOS.
clone_into() { # <url> <ref> <absolute destination>
    local url="$1" ref="$2" dest="$3" parent name
    parent="$(dirname "$dest")"
    name="$(basename "$dest")"
    mkdir -p "$parent"
    ( cd "$parent" && git clone --depth 1 --branch "$ref" "$url" "$name" ) || die "git clone failed for $url"
}

command -v git >/dev/null || die "git not found"

# --- 1. Arduino core --------------------------------------------------------
if [ -f "$ARDUINO_DIR/package.json" ] && [ -d "$ARDUINO_DIR/variants/esp32s31" ]; then
    say "arduino core present ($ARDUINO_DIR)"
else
    [ -e "$ARDUINO_DIR" ] && die "$ARDUINO_DIR exists but is not the 4.0.0-RC1 core (no variants/esp32s31). Move it away first."
    say "fetching arduino-esp32 $ARDUINO_REF -> $ARDUINO_DIR"
    clone_into https://github.com/espressif/arduino-esp32 "$ARDUINO_REF" "$ARDUINO_DIR"
fi

# --- 2. Arduino-wolfSSL -----------------------------------------------------
if [ -f "$WOLF_DIR/src/user_settings.h" ]; then
    say "wolfSSL present ($WOLF_DIR)"
else
    [ -e "$WOLF_DIR" ] && die "$WOLF_DIR exists but has no src/user_settings.h. Move it away first."
    say "fetching Arduino-wolfSSL $WOLF_REF -> $WOLF_DIR"
    mkdir -p "$(dirname "$WOLF_DIR")"
    clone_into https://github.com/wolfSSL/Arduino-wolfSSL "$WOLF_REF" "$WOLF_DIR"
fi

# --- 3. vendored third-party libraries -------------------------------------
missing=0
for n in SdFat ArduinoJson QRCode PNGdec JPEGDEC AnimatedGIF WebSockets; do
    if [ ! -d "$ROOT/idf/components/extern/$n/src" ]; then
        printf 'bootstrap: MISSING vendored library: idf/components/extern/%s/src\n' "$n" >&2
        missing=1
    fi
done
[ "$missing" -eq 0 ] || die "the clone is incomplete (vendored libraries absent) - re-clone the repository"
say "vendored libraries present (7/7)"

# --- 4. IDF environment -----------------------------------------------------
for v in ESP_IDF_VERSION IDF_PATH IDF_TOOLS_PATH IDF_PYTHON_ENV_PATH; do
    if [ -z "${!v:-}" ]; then
        say "WARNING: $v is not exported; tools/build.bat will refuse to start"
    fi
done
if [ -n "${IDF_PATH:-}" ] && [ ! -d "$IDF_PATH/components/esp_hw_support/port/esp32s31" ]; then
    say "WARNING: $IDF_PATH has no esp32s31 port - this is not a from-source ESP-IDF 6.1 with S31 support"
fi

say "done. Next: export the four IDF variables, then run tools/build.bat (see README)."
