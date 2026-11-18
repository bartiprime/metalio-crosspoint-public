#!/bin/sh
# r101 — the cycle reset against the REAL src/util/BatteryStats.cpp, with NVS, the
# charger status and millis() modelled. Standalone (not in test/CMakeLists.txt, same
# shape as test/bq27220_gaugefix) because it compiles one production .cpp.
#
# Checks: brief plug keeps the counters · glitched charger read keeps them · a
# confirmed unplug zeroes awake/sleep/total/charge · the zeroed record is force-written
# so a reboot cannot resurrect the old cycle · the Reset button still clears and still
# persists · Reset mid-charge does not disarm the next automatic reset · a charge that
# starts in standby is credited from the wake · first boot / first unplug are no-ops ·
# the estimate window restarts on the same edge · the window stays capped at 2 h.
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
  -Istubs -I../../src \
  test_battstats_cycle_reset.cpp -o "$BUILD_DIR/test_battstats_cycle_reset"

"$BUILD_DIR/test_battstats_cycle_reset"
