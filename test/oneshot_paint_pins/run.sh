#!/bin/sh
# r165.6 — the battery one-shot pins are bound to the NEXT COMPOSE, against the
# REAL header the firmware compiles (src/display/OneShotPaintPins.h).
# Standalone (not in test/CMakeLists.txt, same shape as test/batt_percent_change):
# the header has no dependencies by design, so nothing needs stubbing.
#
# The fake models the ASYNC ordering the r165.3 harness got wrong: arm on the main
# task, then N loop passes with NO compose, then the compose on the render task.
#
# Checks, as COUNTS:
#   * arm -> 5 passes with no compose -> compose: the pins are still there, and the
#     compose consumes them exactly once (r165.3 dropped them at the first pass);
#   * arm -> no compose ever -> dropped by kComposeDeadlineMs, never applied later;
#   * arm -> a compose that REFUSES them (entry gate) -> the remainder is cleared at
#     the next pass, so no later paint inherits it;
#   * BATT5's armer (a skipped paint): dropped when nothing composes, and never in
#     two composes when one does -- this replaces the old testForeignPinsAreNotCleared,
#     which enshrined that armer's untracked leak;
#   * two armers in one pass are one intent: one compose carries both, no double drive;
#   * two requests are two pinned composes;
#   * pins armed and consumed inside a paint are not this tracker's;
#   * at most ONE compose observes an armed pair, under every schedule (measured).
#
# Run failing_first_asynch.sh first: it compiles r165.5's header and asserts the two
# defects this fixes.
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
  test_oneshot_paint_pins.cpp -o "$BUILD_DIR/test_oneshot_paint_pins"

"$BUILD_DIR/test_oneshot_paint_pins"
