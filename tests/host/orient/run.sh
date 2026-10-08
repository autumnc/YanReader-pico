#!/usr/bin/env bash
# Build + run the host-side adaptive-orientation harness. See README.md.
#
#   tests/host/orient/run.sh          # build + run all scenarios
#
# Plain g++ -std=c++17; no ESP-IDF, no CMake, no device, no stubs
# (main/hw/auto_orient_core.h is dependency-free).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD="$SCRIPT_DIR/build"
BIN="$BUILD/orient_driver"

mkdir -p "$BUILD"

CXX="${CXX:-g++}"
CXXFLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Wno-sign-compare)

echo ">> compiling $BIN"
"$CXX" "${CXXFLAGS[@]}" -I "$REPO/main/hw" "$SCRIPT_DIR/orient_driver.cpp" -o "$BIN"
echo
exec "$BIN"
