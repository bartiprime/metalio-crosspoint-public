#!/usr/bin/env bash
# Fetch the dependencies this repository does NOT track, then verify the ones it does.
#
# WHY THIS EXISTS
#   Two build inputs are too large to vendor and are git-ignored by design, so a fresh
#   clone does not have them and the build stops at CONFIGURE time:
#     1. the Arduino core, at idf/components/arduino  (42.9 MB release asset, LGPL-2.1),
#     2. Arduino-wolfSSL, at .pio/libdeps/metalio_eink4_plus/Arduino-wolfSSL (45 MB, GPLv2).
#   Without them you get, respectively,
#       CMakeLists.txt:... "arduino" component not found
#       CMake Error at crosspoint_app/CMakeLists.txt:196
#         Arduino-wolfSSL not found at .../.pio/libdeps/metalio_eink4_plus/Arduino-wolfSSL/src
#   Neither means the tree is broken. They are missing prerequisites.
#
# THE ARDUINO CORE IS NOT "just clone arduino-esp32"
#   The core this firmware is built against is the release ASSET
#   esp32-core-4.0.0-RC1.tar.xz from espressif/arduino-esp32 releases - not the git tag of
#   the same name, which is a DIFFERENT tree. On top of it we apply one small patch
#   (tools/patches/arduino-tinyusb-msc-guard.patch, 4 files) that guards Arduino's TinyUSB
#   MSC translation units on CONFIG_TINYUSB_ENABLED as well as CONFIG_TINYUSB_MSC_ENABLED.
#   Without the guard those TUs compile with no TinyUSB headers and fail with
#   "'tinyusb_add_string_descriptor' was not declared in this scope". Building from the git
#   tag instead reproduces exactly that failure.
#
#   Everything else the build needs is vendored in idf/components/extern/ (SdFat,
#   ArduinoJson, QRCode, PNGdec, JPEGDEC, AnimatedGIF, WebSockets) or fetched by the IDF
#   component manager into idf/managed_components/ on the first build.
#
# Run from anywhere; paths resolve from this script's location. Idempotent.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARDUINO_DIR="$ROOT/idf/components/arduino"
WOLF_DIR="${CROSSPOINT_WOLFSSL_DIR:-$ROOT/.pio/libdeps/metalio_eink4_plus/Arduino-wolfSSL}"

ARDUINO_TARBALL="esp32-core-4.0.0-RC1.tar.xz"
ARDUINO_URL="https://github.com/espressif/arduino-esp32/releases/download/4.0.0-RC1/$ARDUINO_TARBALL"
ARDUINO_SHA256="870e098411ed1bae1a701dcb4a7096179441be800305de41f78c8b081c1fcc95"
ARDUINO_PATCH="$ROOT/tools/patches/arduino-tinyusb-msc-guard.patch"

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

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
    else die "no sha256sum/shasum available to verify the Arduino core download"; fi
}

command -v git >/dev/null || die "git not found"

# --- 1. Arduino core --------------------------------------------------------
if [ -f "$ARDUINO_DIR/package.json" ] && [ -d "$ARDUINO_DIR/variants/esp32s31" ] \
   && grep -q 'CONFIG_TINYUSB_ENABLED && CONFIG_TINYUSB_MSC_ENABLED' "$ARDUINO_DIR/cores/esp32/USBMSC.cpp"; then
    say "arduino core present, patched ($ARDUINO_DIR)"
else
    [ -e "$ARDUINO_DIR" ] && die "$ARDUINO_DIR exists but is not the patched 4.0.0-RC1 core. Move it away first."

    CACHE_DIR="$ROOT/.cache"
    TARBALL="$CACHE_DIR/$ARDUINO_TARBALL"
    mkdir -p "$CACHE_DIR"

    if [ -f "$TARBALL" ] && [ "$(sha256_of "$TARBALL")" != "$ARDUINO_SHA256" ]; then
        say "cached tarball has the wrong checksum; refetching"
        rm -f "$TARBALL"
    fi

    if [ ! -f "$TARBALL" ]; then
        say "downloading $ARDUINO_TARBALL (42.9 MB)"
        # Both gh and curl are NATIVE binaries here: an MSYS path (/m/...) as -O/-o is not
        # translated, so write from inside the cache dir with a bare filename.
        if command -v gh >/dev/null 2>&1; then
            ( cd "$CACHE_DIR" && gh release download 4.0.0-RC1 -R espressif/arduino-esp32 \
                -p "$ARDUINO_TARBALL" -O "$ARDUINO_TARBALL" --clobber ) || true
        fi
        if [ ! -f "$TARBALL" ]; then
            ( cd "$CACHE_DIR" && curl -fL --retry 2 -o "$ARDUINO_TARBALL" "$ARDUINO_URL" ) \
                || die "download failed. Fetch $ARDUINO_URL by hand into $CACHE_DIR/ and re-run."
        fi
    fi

    [ -f "$TARBALL" ] || die "no $ARDUINO_TARBALL in $CACHE_DIR after download"
    [ "$(sha256_of "$TARBALL")" = "$ARDUINO_SHA256" ] \
        || die "checksum mismatch for $ARDUINO_TARBALL (expected $ARDUINO_SHA256)"

    say "extracting -> $ARDUINO_DIR"
    mkdir -p "$ARDUINO_DIR"
    tar -xJf "$TARBALL" -C "$ARDUINO_DIR" --strip-components=1 || die "tar failed (needs xz support)"

    say "applying tools/patches/arduino-tinyusb-msc-guard.patch"
    ( cd "$ARDUINO_DIR" && { git apply -p1 "$ARDUINO_PATCH" 2>/dev/null || patch -p1 -i "$ARDUINO_PATCH"; } ) \
        || die "could not apply the TinyUSB guard patch"
    grep -q 'CONFIG_TINYUSB_ENABLED && CONFIG_TINYUSB_MSC_ENABLED' "$ARDUINO_DIR/cores/esp32/USBMSC.cpp" \
        || die "patch did not land in cores/esp32/USBMSC.cpp"
fi

# --- 2. Arduino-wolfSSL -----------------------------------------------------
if [ -f "$WOLF_DIR/src/user_settings.h" ]; then
    say "wolfSSL present ($WOLF_DIR)"
else
    [ -e "$WOLF_DIR" ] && die "$WOLF_DIR exists but has no src/user_settings.h. Move it away first."
    say "fetching Arduino-wolfSSL $WOLF_REF -> $WOLF_DIR"
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
