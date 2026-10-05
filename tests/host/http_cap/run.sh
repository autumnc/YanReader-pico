#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD="$SCRIPT_DIR/build"
BIN="$BUILD/http_cap_test"

mkdir -p "$BUILD"

CXX="${CXX:-g++}"
CXXFLAGS=(-std=c++17 -O1 -g -Wall -Wextra)

echo ">> compiling $BIN"
"$CXX" "${CXXFLAGS[@]}" -I "$REPO/main" "$SCRIPT_DIR/http_cap_test.cpp" -o "$BIN"

exec "$BIN"
