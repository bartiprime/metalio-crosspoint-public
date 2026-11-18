#!/bin/sh
# r98.2 — the BQ27220 capacity load against a model gauge, including the exact state r98.1 left the
# device in (CFGUPDATE latched + sealed). Five scenarios, each its own process because the state
# machine is a process-wide singleton.
#
#   latched        the shipped defect: must clear CFGUPDATE, re-seal, and reinit 0x12
#   fresh          TI's 3000 mAh defaults: must write 2000 and 0x12 must follow the reinit
#   healthy        already coherent: must not unseal and must not write
#   exitrefused    EXIT never obeyed: must still terminate and still seal, and say so
#   writerejected  DM write never commits: must still exit + seal and report the failure
set -e
cd "$(dirname "$0")"
SDK="../../freeink-sdk/libs/hardware"
BUILD_DIR="${BUILD_DIR:-./build}"
mkdir -p "$BUILD_DIR"

if command -v c++ >/dev/null 2>&1; then CXX=c++; elif command -v clang++ >/dev/null 2>&1; then CXX=clang++; else CXX=g++; fi
"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -DFREEINK_DEVICE_X3=1 -DFREEINK_DEVICE_X4=1 -DFREEINK_BATTERY_I2C_GAUGE=1 \
  -Istubs -I"$SDK/BatteryMonitor/test/host/stubs" -I"$SDK/BoardConfig/include" \
  -I"$SDK/XteinkDetect/test/host/stubs" -I../../src \
  test_gaugefix.cpp -o "$BUILD_DIR/test_gaugefix"

status=0
for s in latched fresh healthy exitrefused writerejected; do
  "$BUILD_DIR/test_gaugefix" "$s" || status=1
done
exit $status
