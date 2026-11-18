#!/bin/sh
# BATT6 — the percentage change decision against the REAL header the firmware
# compiles (src/display/BatteryPercentChange.h). Standalone (not in
# test/CMakeLists.txt, same shape as test/battstats_cycle_reset): the header has no
# dependencies at all by design, so nothing needs stubbing.
#
# Checks, as COUNTS:
#   * a steady percentage produces zero repaint requests over thousands of passes;
#   * a surface this path can NEVER address (the FreeInkUI header band) asks for
#     EXACTLY ONE whole-activity repaint per distinct value, then nothing -- the
#     r165.1 loop, fixed. See failing_first.sh for the OLD behaviour on the same model;
#   * no request while a clean is armed or a transition is in flight;
#   * adversarial 90<->91 alternation is bounded by the 10 s per-surface rate limit;
#   * one change on an addressable box is exactly one in-place patch;
#   * a burst of 40 changes while the path is blocked collapses to one drive with the
#     final value;
#   * a screen with no number costs nothing;
#   * a box recorded by a paint the glass never took is not treated as "already
#     showing" (it waits, and costs no commands at all).
set -e
cd "$(dirname "$0")"
BUILD_DIR="${BUILD_DIR:-./build}"
mkdir -p "$BUILD_DIR"

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
  -I../../src \
  test_batt_percent_change.cpp -o "$BUILD_DIR/test_batt_percent_change"

"$BUILD_DIR/test_batt_percent_change"
