#!/usr/bin/env bash
# 微信读书官方 Skill/Gateway 客户端纯函数的主机对拍：body 拼装 / 搜索解析 / 推荐解析。
# 不需要 IDF 环境，g++ 即可。
#
#   tests/host/weread_skill/run.sh
#
# 编 weread_skill_client.cpp + flomo_json.cpp + regress.cpp + stubs.cpp。网络路径
# (net::request) 与 g_settings.getString 由 stubs.cpp 给空实现；esp_log.h / esp_err.h
# 是本目录自带的桩（抢先于 IDF 的 include 路径）。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD="$SCRIPT_DIR/build"
mkdir -p "$BUILD"

CXX="${CXX:-g++}"
CXXFLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter)

echo ">> compiling weread_skill tests"
"$CXX" "${CXXFLAGS[@]}" \
    -I "$SCRIPT_DIR" -I "$REPO/main" \
    "$SCRIPT_DIR/regress.cpp" \
    "$SCRIPT_DIR/stubs.cpp" \
    "$REPO/main/weread_skill_client.cpp" \
    "$REPO/main/flomo_json.cpp" \
    -o "$BUILD/regress"

"$BUILD/regress"
