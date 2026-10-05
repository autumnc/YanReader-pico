#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
mkdir -p tests/host/safe_file/build
out=tests/host/safe_file/build/safe_file_test
echo ">> compiling $PWD/$out"
g++ -std=c++17 -Wall -Wextra -Itests/host/safe_file -Imain \
  main/safe_file.cpp tests/host/safe_file/safe_file_test.cpp \
  -o "$out"
"$out"
