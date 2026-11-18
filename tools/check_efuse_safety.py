#!/usr/bin/env python3
"""eFuse safety gate — FAIL THE BUILD if any path could burn an eFuse.

WHY THIS EXISTS (operator rule, 1 Nov 2026)
    "Make sure we never ever touch the efuse and that the efuse can never be touched."
    An eFuse burn is IRREVERSIBLE. There is no rollback, no reflash, no recovery: a
    wrong SECURE_BOOT / FLASH_ENC setting turns the first boot into a permanent
    change to the silicon's one-time-programmable block. Documentation cannot
    prevent that, because the dangerous edit is a one-line Kconfig change made at
    2 a.m. by someone who has not read the document. So the rule is enforced by the
    build: this script runs on EVERY build (see the crosspoint_app CMakeLists.txt)
    and a non-zero exit stops configure/ninja before a single object is compiled.

WHAT IT REJECTS
    1. Any eFuse WRITE/burn API referenced in our own sources:
           esp_efuse_write_*      esp_efuse_burn*
           ESP_EFUSE_WRITE_*      efuse_ll_set_*
       Reads are fine and are expected (CrossPointWebServer.cpp identifies the chip
       from ESP_EFUSE_USER_DATA; ObfuscationUtils.cpp derives its key material from
       the same read-only fields). Only the WRITE surface is banned.
    2. CONFIG_SECURE_BOOT=y or CONFIG_SECURE_FLASH_ENC_ENABLED=y in idf/sdkconfig or
       idf/sdkconfig.defaults. These are the real hazard: the BOOTLOADER — not our
       code — burns the key/encryption eFuses on the first boot of such an image,
       and the device is then permanently that. We can refuse to write an eFuse and
       still be destroyed by a config line, which is why both are checked.

WHERE IT LOOKS
    src/, lib/, idf/components/, freeink-sdk/ — the code we build. NOT
    idf/managed_components (third-party, never edited here) and NOT the IDF tree
    itself (${IDF_PATH}), which is a read-only SDK we do not own.

HOW TO USE
    python3 tools/check_efuse_safety.py            # from anywhere; root is inferred
    python3 tools/check_efuse_safety.py --root M:/_HermesWork/crosspoint-fork/crosspoint-reader
    Exit 0 = clean. Exit 1 = violation(s) printed with file:line. Exit 2 = setup error.

There is deliberately NO --ignore / --force flag. If a violation is ever genuinely
required, that is a conversation with the operator, not a flag a build script can
pass on its own.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# --- what counts as touching the eFuse ---------------------------------------
WRITE_API_PATTERNS = [
    re.compile(r"\besp_efuse_write_\w*"),
    re.compile(r"\besp_efuse_burn\w*"),
    re.compile(r"\bESP_EFUSE_WRITE_\w*"),
    re.compile(r"\befuse_ll_set_\w*"),
]

# Source trees we build from. managed_components and the IDF itself are excluded
# on purpose: they are third-party and are not edited in this repository.
SOURCE_DIRS = ["src", "lib", os.path.join("idf", "components"), "freeink-sdk"]
EXCLUDED_PATH_PARTS = ("managed_components", os.sep + "build", "/build-", os.sep + "build-")
SOURCE_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc"}

# --- what counts as arming an irreversible config -----------------------------
CONFIG_FILES = [os.path.join("idf", "sdkconfig"), os.path.join("idf", "sdkconfig.defaults")]
CONFIG_FORBIDDEN = ["CONFIG_SECURE_BOOT", "CONFIG_SECURE_FLASH_ENC_ENABLED"]


def _iter_sources(root: str):
    for rel in SOURCE_DIRS:
        base = os.path.join(root, rel)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [
                d for d in dirnames if d not in ("managed_components", ".git", "node_modules")
            ]
            for name in filenames:
                if os.path.splitext(name)[1].lower() not in SOURCE_EXTS:
                    continue
                full = os.path.join(dirpath, name)
                norm = full.replace("\\", "/")
                if "/build/" in norm or "/build-" in norm:
                    continue
                yield full


def check_sources(root: str):
    problems = []
    for path in _iter_sources(root):
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                for lineno, line in enumerate(fh, 1):
                    for pat in WRITE_API_PATTERNS:
                        m = pat.search(line)
                        if m:
                            rel = os.path.relpath(path, root).replace("\\", "/")
                            problems.append((rel, lineno, m.group(0), line.strip()))
                            break
        except OSError:
            continue
    return problems


def check_configs(root: str):
    problems = []
    for rel in CONFIG_FILES:
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                for lineno, line in enumerate(fh, 1):
                    stripped = line.strip()
                    if stripped.startswith("#"):
                        continue  # "# CONFIG_SECURE_BOOT is not set" is the GOOD case
                    for key in CONFIG_FORBIDDEN:
                        if stripped == f"{key}=y":
                            problems.append((rel.replace("\\", "/"), lineno, key, stripped))
        except OSError as exc:
            print(f"[efuse-gate] cannot read {path}: {exc}", file=sys.stderr)
            return None
    return problems


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    default_root = os.path.dirname(here)  # tools/ -> repo root
    ap = argparse.ArgumentParser(description="Refuse to build anything that could burn an eFuse.")
    ap.add_argument("--root", default=default_root, help="repository root (default: parent of tools/)")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    if not os.path.isdir(os.path.join(root, "src")):
        print(f"[efuse-gate] ERROR: {root} does not look like the firmware tree (no src/)", file=sys.stderr)
        return 2

    src_problems = check_sources(root)
    cfg_problems = check_configs(root)
    if cfg_problems is None:
        return 2

    if not src_problems and not cfg_problems:
        print("[efuse-gate] OK: no eFuse write API in our sources; "
              "CONFIG_SECURE_BOOT and CONFIG_SECURE_FLASH_ENC_ENABLED are off")
        return 0

    bar = "=" * 78
    print(bar, file=sys.stderr)
    print("EFUSE SAFETY GATE: BUILD REFUSED", file=sys.stderr)
    print(bar, file=sys.stderr)
    print("An eFuse burn is IRREVERSIBLE. This build is blocked until the following are gone.", file=sys.stderr)
    if src_problems:
        print("\n  eFuse WRITE API referenced in our sources:", file=sys.stderr)
        for rel, lineno, token, text in src_problems:
            print(f"    {rel}:{lineno}: {token}", file=sys.stderr)
            print(f"        {text}", file=sys.stderr)
        print("  eFuse is READ-ONLY forever. Remove the write; there is no variant of this that is allowed.",
              file=sys.stderr)
    if cfg_problems:
        print("\n  Irreversible security configuration enabled:", file=sys.stderr)
        for rel, lineno, key, text in cfg_problems:
            print(f"    {rel}:{lineno}: {text}", file=sys.stderr)
        print("  CONFIG_SECURE_BOOT / CONFIG_SECURE_FLASH_ENC_ENABLED make the BOOTLOADER burn the",
              file=sys.stderr)
        print("  key/encryption eFuses on the first boot of this image. Turn them off in both",
              file=sys.stderr)
        print("  idf/sdkconfig and idf/sdkconfig.defaults.", file=sys.stderr)
    print(bar, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
