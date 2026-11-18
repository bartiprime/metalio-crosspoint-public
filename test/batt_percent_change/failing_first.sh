#!/bin/sh
# BATT6 (r165.2) — FAILING FIRST.
#
# Compiles failing_first_old_decision.cpp against the r165.1 decision EXACTLY as
# commit 568973c9 shipped it, extracted from git at run time (no copy is kept in the
# tree, so this cannot drift from history), and asserts that the old decision does
# NOT converge: a steady 90 % on a surface with no addressable box must produce far
# more than one whole-activity repaint request.
#
# The r165.2 test (run.sh) asserts exactly ONE on the same model. Run this first if
# you are re-reading this in a review: it is the r165.1 defect, isolated.
set -e
cd "$(dirname "$0")"
BUILD_DIR="${BUILD_DIR:-./build-failing-first}"
mkdir -p "$BUILD_DIR/display"

if ! git rev-parse --verify 568973c9 >/dev/null 2>&1; then
  echo "SKIP: 568973c9 is not in this clone (shallow?)" >&2
  exit 0
fi
git show 568973c9:src/display/BatteryPercentChange.h > "$BUILD_DIR/display/BatteryPercentChange.h"

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
  failing_first_old_decision.cpp -o "$BUILD_DIR/failing_first_old_decision"

# The probe exits 1 when the defect is present -- which is what we are asserting.
set +e
"$BUILD_DIR/failing_first_old_decision"
rc=$?
set -e
if [ "$rc" -eq 1 ]; then
  echo "PASS: the r165.1 decision does not converge (this is the defect r165.2 fixes)"
  exit 0
fi
echo "FAIL: the r165.1 decision converged -- the probe no longer reproduces the defect" >&2
exit 1
