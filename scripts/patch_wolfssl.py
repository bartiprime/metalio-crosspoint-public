"""Append CrossPoint's wolfSSL overrides to Arduino-wolfSSL's user_settings.h.

TWO ENTRY POINTS, one body:
  * PlatformIO: `pre:scripts/patch_wolfssl.py` (extra_scripts), where SCons injects
    `env` and PROJECT_DIR comes from it.
  * ESP-IDF: idf/components/crosspoint_app/CMakeLists.txt runs it with the IDF python
    before globbing the wolfSSL sources, because that build NEVER reads platformio.ini.
    Without that second entry point these overrides -- C6 (trace off) included -- would
    be a no-op in the shipped image; they previously reached the tree only because a
    `pio pkg install` had once patched the cached package.
    Optional argv[1]: the Arduino-wolfSSL package directory (the one holding src/).

Exit status: 0 when at least one user_settings.h was patched, 1 when none was found,
so a build hook cannot silently skip the patch.
"""

import sys
from pathlib import Path

MARKER = "/* CrossPoint wolfSSL compatibility overrides */"
OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
/* C6 (upstream a5340ea6): Arduino-wolfSSL's user_settings.h defines DEBUG_WOLFSSL
   unconditionally, so every WOLFSSL_MSG/ENTER/LEAVE call and its literal is linked
   in. The trace never reaches the log anyway: SecureClient only calls
   wolfSSL_Debugging_ON() in FREEINK_WOLFSSL_DEBUG builds, which no env sets.
   Undefining it here removes the calls and their literals (~39 KB of flash).
   SECOND, INTENDED EFFECT: wolfcrypt/settings.h then takes its
   `#ifndef DEBUG_WOLFSSL -> #define NO_ERROR_STRINGS` branch, so wolfSSL's
   error-string table goes too (part of the same 39 KB). That is safe for this
   firmware because it never formats a wolfSSL error: SecureClient.cpp reads
   wolfSSL_get_error() INTEGERS only (lines 135/204) and nothing in src/ or lib/
   calls ERR_error_string/wolfSSL_ERR_error_string. Checked, not assumed.
   NOTHING ELSE MOVES: the error queue needs OPENSSL_EXTRA, the debug allocator
   needs WOLFSSL_ESPIDF (not defined here), and no struct layout depends on
   DEBUG_WOLFSSL, so there is no runtime behaviour change.
   A debug build is still possible: pass -DFREEINK_WOLFSSL_DEBUG as a GLOBAL build
   flag (env build_flags / PLATFORMIO_BUILD_FLAGS) so the wolfSSL sources see it too.
   This must sit at the END of user_settings.h: settings.h includes it and then
   tests DEBUG_WOLFSSL, so the #undef has to have run by the time it returns. */
#ifndef FREEINK_WOLFSSL_DEBUG
#undef DEBUG_WOLFSSL
#endif
/* MEMFIX-PORT: 8192 handles up to RSA-4096 keys (the public-CA maximum,
   ISRG Root X1 included) with half the per-bignum heap of 16384: with
   WOLFSSL_SMALL_STACK each fast-math temp is FP_MAX_BITS/8 * 2 bytes on the
   heap, and TLS cert verification allocates dozens at once. */
#undef FP_MAX_BITS
#define FP_MAX_BITS 8192
"""


def patch_user_settings(path: Path, project_dir: Path) -> None:
    text = path.read_text()
    desired = (text.split(MARKER, 1)[0].rstrip() if MARKER in text else text.rstrip()) + OVERRIDES + "\n"
    if desired == text:
        # Already patched: do NOT touch the file. This header is included by every
        # wolfSSL translation unit, and the IDF build runs this script at configure
        # time -- an unconditional write would change its mtime and rebuild all of
        # wolfSSL on every configure.
        print(f"wolfSSL settings already patched: {path.name}")
        return
    path.write_text(desired)
    try:
        shown = path.relative_to(project_dir)
    except ValueError:
        shown = path
    print(f"Patched wolfSSL settings: {shown}")


def main() -> int:
    if len(sys.argv) > 1:
        # Build-hook form: the Arduino-wolfSSL package directory (or any path inside it).
        package_dir = Path(sys.argv[1]).resolve()
        while package_dir.name and not (package_dir / "src").is_dir():
            package_dir = package_dir.parent
        found = [package_dir / "src" / "user_settings.h"]
        targets = [t for t in found if t.is_file()]
        project_dir = package_dir
    else:
        try:
            Import("env")  # SCons, PlatformIO pre-script
        except NameError:
            project_dir = Path(__file__).resolve().parents[1]
        else:
            project_dir = Path(env.subst("$PROJECT_DIR"))
        targets = sorted(project_dir.glob(".pio/libdeps/*/Arduino-wolfSSL/src/user_settings.h"))

    for target in targets:
        patch_user_settings(target, project_dir)
    if not targets:
        print("patch_wolfssl: no Arduino-wolfSSL/src/user_settings.h found", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
