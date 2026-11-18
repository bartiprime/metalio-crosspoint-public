"""
PlatformIO pre: script — make ricmoo/QRCode compile under C23.

WHY: GCC 15 (the ESP32-S31 RISC-V toolchain, ESP-IDF 6.1) defaults to C23, where
`bool`, `true` and `false` are KEYWORDS. ricmoo/QRCode's qrcode.h declares:

    typedef unsigned char bool;
    static const bool false = 0;
    static const bool true = 1;

which C23 rejects: "'bool' cannot be defined via 'typedef'" and "expected
identifier or '(' before 'false'".

The fix is scoped to this one library on purpose. ESP-IDF 6.1's own headers WANT
C23's built-in bool, so downgrading the build-wide C standard (the tempting
one-line -std=gnu17) breaks IDF instead. Guard the typedef instead.

Idempotent: the marker line is checked first, so an already-patched tree is left
alone and incremental builds do not recompile.
"""

Import("env")  # noqa: F821  (injected by PlatformIO's SCons environment)

import os

MARKER = "FREEINK_C23_BOOL_GUARD"

ORIGINAL = """typedef unsigned char bool;
static const bool false = 0;
static const bool true = 1;"""

PATCHED = """/* %s: C23 (GCC 15, ESP32-S31) makes bool/true/false keywords, so these
 * declarations are a hard error there. Only declare them for older C. */
#if !defined(__cplusplus) && !(defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L)
typedef unsigned char bool;
static const bool false = 0;
static const bool true = 1;
#endif""" % MARKER


def patch_qrcode(env):
    libdeps = os.path.join(env.subst("$PROJECT_DIR"), ".pio", "libdeps")
    if not os.path.isdir(libdeps):
        return
    for envdir in os.listdir(libdeps):
        header = os.path.join(libdeps, envdir, "QRCode", "src", "qrcode.h")
        if not os.path.isfile(header):
            continue
        with open(header, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        if MARKER in text:
            print("[patch_qrcode] already patched: %s" % header)
            continue
        if ORIGINAL not in text:
            # Different QRCode release; do not guess.
            print("[patch_qrcode] WARNING: expected declarations not found in %s" % header)
            continue
        with open(header, "w", encoding="utf-8", newline="\n") as f:
            f.write(text.replace(ORIGINAL, PATCHED))
        print("[patch_qrcode] patched for C23: %s" % header)


patch_qrcode(env)
