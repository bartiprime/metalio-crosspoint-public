#!/usr/bin/env python3
"""Version-stamp guard for tools/build.bat.

WHY THIS EXISTS
    CROSSPOINT_VERSION in idf/components/crosspoint_app/CMakeLists.txt is the
    ONLY source of the string the firmware's `ver` command prints.  An image was
    once built into build-r1018/ while the stamp still said "r101.7", was
    flashed, and reported r101.7 while everyone believed it was r101.8.  A
    build directory whose name says one revision and a binary that reports
    another is a silent lie, and it costs a whole flash cycle to discover.

WHAT IT DOES
    Compares the rNNNN token in the -B build directory name against the rNNNN
    derived from CROSSPOINT_VERSION.  Disagreement is a hard failure BEFORE
    idf.py runs (a full build is 10-20 min of wasted electricity otherwise).

EXIT CODES
    0  ok, or guard not applicable, or mismatch explicitly allowed
    3  mismatch (caller must abort the build)
    other  the guard itself could not run (caller should warn and continue)

Escape hatch: SST_ALLOW_STAMP_MISMATCH=1 downgrades the mismatch to a warning.

CLI:  stamp_guard.py --dir <build dir> --action <idf.py action>
"""

import argparse
import os
import re
import sys

CMAKE_REL = os.path.join("idf", "components", "crosspoint_app", "CMakeLists.txt")

# rNNNN inside a build dir name: must be a whole token, so "build-r1018b" (a
# typo) does NOT silently pass as r1018.
DIR_TOKEN_RE = re.compile(r"(?:^|[-_.])r(\d+)(?=$|[-_.])")
# CROSSPOINT_VERSION="MetalioEinkPlus r101.8"  -> 101, 8  -> "1018"
# A full bump carries no sub-version: "MetalioEinkPlus r102" -> 102, None -> "102",
# which matches a build-f102 / build-r102 directory. (r101.7 -> r101.8 was a
# sub-bump; r101.22 -> r102 is a full one, and the dotted form alone would have
# rejected build-r102 as a mismatch.)
VER_RE = re.compile(r"CROSSPOINT_VERSION\s*=\s*\"([^\"]*)\"")
VER_TOKEN_RE = re.compile(r"r(\d+)(?:\.(\d+))?")


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def dir_stamp(build_dir):
    """Return ('rNNNN', 'NNNN') from the build dir name, or (None, None)."""
    base = os.path.basename(os.path.normpath(build_dir))
    m = DIR_TOKEN_RE.search(base)
    if not m:
        return None, None
    return "r" + m.group(1), m.group(1)


def source_stamp(path):
    """Return (raw_version, 'rNNNN', 'NNNN', line_number) or Nones."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.readlines()
    except OSError as exc:
        print("[stamp-guard] WARN: cannot read %s (%s)" % (path, exc))
        return None, None, None, None
    for idx, line in enumerate(lines, 1):
        m = VER_RE.search(line)
        if not m:
            continue
        raw = m.group(1)
        t = VER_TOKEN_RE.search(raw)
        if not t:
            print("[stamp-guard] WARN: CROSSPOINT_VERSION=%r has no rNNN.N token" % raw)
            return raw, None, None, idx
        digits = t.group(1) + (t.group(2) or "")
        return raw, "r" + digits, digits, idx
    print("[stamp-guard] WARN: no CROSSPOINT_VERSION=\"...\" line in %s" % path)
    return None, None, None, None


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--dir", required=True, help="value passed to -B")
    ap.add_argument("--action", default="build", help="idf.py action (build/fullclean/...)")
    args = ap.parse_args()

    action = (args.action or "build").strip().lower()
    if action != "build":
        print("[stamp-guard] skip: action=%s is not a build - stamp guard not applicable" % action)
        return 0

    dname, ddigits = dir_stamp(args.dir)
    if dname is None:
        print("[stamp-guard] WARN: build dir %r carries no rNNNN token - "
              "stamp guard NOT applied" % os.path.basename(os.path.normpath(args.dir)))
        return 0

    cmake = os.path.join(repo_root(), CMAKE_REL)
    raw, sname, sdigits, lineno = source_stamp(cmake)
    if sname is None:
        print("[stamp-guard] WARN: could not parse CROSSPOINT_VERSION out of %s - "
              "stamp guard NOT applied" % CMAKE_REL)
        return 0

    ddigits = (ddigits or "").lstrip("0")
    sdigits = (sdigits or "").lstrip("0")
    if ddigits and ddigits == sdigits:
        print("stamp: build-%s == %s ok" % (dname, sname))
        return 0

    allow = os.environ.get("SST_ALLOW_STAMP_MISMATCH", "").strip() == "1"
    bar = "=" * 72
    print(bar)
    print("[build.bat] STAMP GUARD: build dir / CROSSPOINT_VERSION DISAGREE")
    print("  -B dir      : %s   (-> %s)" % (args.dir, dname))
    print("  source      : %s:%s" % (CMAKE_REL, lineno))
    print("  version     : CROSSPOINT_VERSION=\"%s\"   (-> %s)" % (raw, sname))
    print("  effect      : firmware would report %s from a %s image" % (sname, dname))
    print("")
    print("  FIX ONE OF:")
    print("    1) edit %s:%s so CROSSPOINT_VERSION's r-token matches the -B dir:" % (CMAKE_REL, lineno))
    print("         currently \"%s\" (normalises to %s); it must normalise to %s"
          % (raw, sname, dname))
    print("    2) build into a directory that matches the stamp (build-%s)" % sname)
    print("    3) SST_ALLOW_STAMP_MISMATCH=1 to build anyway (records the lie explicitly)")
    print(bar)
    if allow:
        print("[build.bat] WARN: SST_ALLOW_STAMP_MISMATCH=1 - ALLOWING the %s != %s "
              "mismatch; the resulting firmware will report %s" % (dname, sname, sname))
        return 0
    print("[build.bat] FATAL: refusing to build. Set SST_ALLOW_STAMP_MISMATCH=1 to override.")
    return 3


if __name__ == "__main__":
    sys.exit(main())
