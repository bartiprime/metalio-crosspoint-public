#!/usr/bin/env python3
"""Refuse a build whose firmware flags differ from the set the device actually runs.

Why this exists
---------------
`CP_*` CMake options are product behaviour, not build hygiene: they select panel waveforms, rail cuts,
wake-clean policy and which producer owns the battery glyph. A worker given the brief "build it" takes
the CMake *defaults*, which are not necessarily what the flashed baseline used. On 2026-11-18 the
r165.2..r165.6 builds silently ran with CP_ICON_PARTIAL_REPAINT=ON while the device ran OFF -- an A/B
against the device would have measured the flag, not the round. Host tests cannot see this, and the
firmware builds cleanly either way.

Usage
-----
    python tools/check_build_flags.py <build-dir> [--quiet]

Exit codes: 0 identical, 1 drift, 2 cannot read the build dir or the reference file.

The reference is `tools/shipping_flags.txt`. States it does not know about are reported as drift, so a
newly added option cannot slip in unrecorded.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, "tools", "shipping_flags.txt")
CACHE = "CMakeCache.txt"
PAT = re.compile(r"^(CP_[A-Z0-9_]+):BOOL=(ON|OFF)$", re.MULTILINE)


def load_reference(path):
    ref = {}
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, _, value = line.partition("=")
            if name and value:
                ref[name.strip()] = value.strip()
    return ref


def load_cache(build_dir):
    path = os.path.join(build_dir, CACHE)
    if not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return dict(PAT.findall(fh.read()))


def main(argv):
    quiet = "--quiet" in argv
    args = [a for a in argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        print("usage: check_build_flags.py <build-dir> [--quiet]", file=sys.stderr)
        return 2

    build_dir = args[0]
    if not os.path.isfile(REF):
        print(f"check_build_flags: missing reference {REF}", file=sys.stderr)
        return 2
    have = load_cache(build_dir)
    if have is None:
        print(f"check_build_flags: no {CACHE} in {build_dir} - configure the build first", file=sys.stderr)
        return 2

    ref = load_reference(REF)
    drift = []
    for name in sorted(set(ref) | set(have)):
        want = ref.get(name)
        got = have.get(name)
        if want != got:
            drift.append((name, want, got))

    if not drift:
        if not quiet:
            print(f"check_build_flags: OK - {len(ref)} CP_* options match tools/shipping_flags.txt")
        return 0

    print(f"check_build_flags: DRIFT in {build_dir} - this build is NOT the shipping configuration",
          file=sys.stderr)
    print(f"{'option':38s} {'shipping':>8s} {'this build':>10s}", file=sys.stderr)
    for name, want, got in drift:
        print(f"{name:38s} {str(want):>8s} {str(got):>10s}", file=sys.stderr)
    print("", file=sys.stderr)
    print("Either rebuild with the shipping flags (see the header of tools/shipping_flags.txt), or, if the",
          file=sys.stderr)
    print("change is deliberate, update tools/shipping_flags.txt in the same commit that changes the",
          file=sys.stderr)
    print("behaviour, and record it in the round's artefact. Do not flash a drifted build.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
