#!/usr/bin/env bash
# Host-side checks for the 16-level gray write-back core (DitherUtils.h). See README.md.
#
#   tests/host/dither/run.sh          # build + run all 7 assertion groups
#
# 纯 g++ -std=c++17：DitherUtils.h 只 include <stdint.h>，不需要 IDF、不需要桩件、
# 不需要设备、不需要 builtin.ttf。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD="$SCRIPT_DIR/build"
BIN="$BUILD/dither_test"

mkdir -p "$BUILD"

CXX="${CXX:-g++}"
CXXFLAGS=(-std=c++17 -O1 -g -Wall -Wno-unused-function)

echo ">> compiling $BIN"
"$CXX" "${CXXFLAGS[@]}" \
    -I "$REPO/components/crossmux/lib/Epub/Epub/converters" \
    "$SCRIPT_DIR/dither_test.cpp" \
    -o "$BIN"

exec "$BIN" "$@"
