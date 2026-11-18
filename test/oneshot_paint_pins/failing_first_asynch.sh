#!/bin/sh
# r165.6 — FAILING FIRST, with the ASYNC ordering.
#
# r165.3's harness passed because it modelled arm -> paint synchronously. This
# script compiles the PRE-r165.6 header, extracted from git at run time (no copy is
# kept in the tree, so it cannot drift from history), and drives it through a
# race-controlled fake: arm, then N loop passes with NO compose, then the compose.
#
# The probe asserts the two defects and exits 1 when they are reproduced; this
# script treats that as PASS. run.sh then asserts the opposite on the real header.
set -e
cd "$(dirname "$0")"
BUILD_DIR="${BUILD_DIR:-./build-failing-first}"
mkdir -p "$BUILD_DIR/display"

# r165.5 (the last commit that shipped the one-pass pins).
PRE_FIX_SHA=4cafdc3f
if ! git rev-parse --verify "$PRE_FIX_SHA" >/dev/null 2>&1; then
  echo "SKIP: $PRE_FIX_SHA is not in this clone (shallow?)" >&2
  exit 0
fi
git show "$PRE_FIX_SHA:src/display/OneShotPaintPins.h" > "$BUILD_DIR/display/OneShotPaintPins.h"
if ! grep -q "class PassBoundPins" "$BUILD_DIR/display/OneShotPaintPins.h"; then
  echo "FAIL: $PRE_FIX_SHA does not carry PassBoundPins -- wrong baseline" >&2
  exit 1
fi

if command -v clang++ >/dev/null 2>&1; then
  CXX=clang++
elif command -v g++ >/dev/null 2>&1; then
  CXX=g++
elif command -v c++ >/dev/null 2>&1; then
  CXX=c++
else
  echo "no C++ compiler on PATH" >&2
  exit 1
fi

"$CXX" -std=c++20 -Wall -Wextra -Werror \
  -I"$BUILD_DIR" \
  failing_first_asynch.cpp -o "$BUILD_DIR/failing_first_asynch"

set +e
"$BUILD_DIR/failing_first_asynch"
rc=$?
set -e
if [ "$rc" -eq 1 ]; then
  echo "PASS: r165.5's pins do not survive to the deferred compose, and BATT5's armer leaks (this is the defect r165.6 fixes)"
  exit 0
fi
echo "FAIL: the r165.5 pins behaved -- the probe no longer reproduces the defect" >&2
exit 1
