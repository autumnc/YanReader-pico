#!/usr/bin/env bash
# 已保存 WiFi 网络表（main/wifi_store.cpp）纯函数的主机对拍：序列化 / 解析 / 转义往返。
# 不需要 IDF 环境，g++ 即可。
#
#   tests/host/wifi_store/run.sh
#
# 只编 wifi_store.cpp 与 regress.cpp；load/save 引用的读盘/写盘/g_settings 由 stubs.cpp
# 提供空实现（见 stubs.cpp 头注释）。**不**链接 safe_file.cpp —— 它那头带着 longop 的
# 记号，主机测试骨架目前是断的，本测例也不需要真的读写文件。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD="$SCRIPT_DIR/build"
mkdir -p "$BUILD"

CXX="${CXX:-g++}"
CXXFLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter)

echo ">> compiling wifi_store tests"
"$CXX" "${CXXFLAGS[@]}" \
    -I "$SCRIPT_DIR" -I "$REPO/main" \
    "$SCRIPT_DIR/regress.cpp" \
    "$SCRIPT_DIR/stubs.cpp" \
    "$REPO/main/wifi_store.cpp" \
    -o "$BUILD/regress"

"$BUILD/regress"
