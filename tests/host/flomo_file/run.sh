#!/usr/bin/env bash
# flomo 本地库「每次进入都是暂无笔记」的根因回归测试。不需要 IDF 环境，g++ 即可。
#
#   tests/host/flomo_file/run.sh              # 跑测例（应全绿）
#   tests/host/flomo_file/run.sh --verify-fix # 用 git HEAD 里**修复前**的 flomo_file.cpp
#                                             # 重跑同一套测例，应挂
#
# 关键在编译参数 -Dmkdir=test_mkdir：Linux 的 mkdir 对已存在目录一律 EEXIST(17)，
# 而设备上（ESP-IDF 的挂载点 /sdcard）返回 EINVAL(22)，不换掉它就复现不出这个 bug。
# 详见 regress.cpp 的头注释。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD="$SCRIPT_DIR/build"
mkdir -p "$BUILD"

CXX="${CXX:-g++}"
CXXFLAGS=(-std=c++17 -O1 -g -Wall -Wno-unused-function)

# $1 = 输出二进制, $2 = 要编的 flomo_file.cpp（默认工作区那份）
build() {
    local out="$1" file_src="$2"
    echo ">> compiling $out"
    "$CXX" "${CXXFLAGS[@]}" -Dmkdir=test_mkdir \
        -I "$SCRIPT_DIR" -I "$REPO/main" \
        "$SCRIPT_DIR/regress.cpp" \
        "$REPO/main/flomo_db.cpp" \
        "$REPO/main/flomo_json.cpp" \
        "$file_src" \
        -o "$out"
}

BIN="$BUILD/regress"
build "$BIN" "$REPO/main/flomo_file.cpp"

case "${1:-}" in
    --verify-fix)
        # 修复前的那份从 git HEAD 取（工作区里已经修好了）。
        if ! git -C "$REPO" show HEAD:main/flomo_file.cpp > "$BUILD/flomo_file_prefix.cpp" 2>/dev/null; then
            echo ">> 读不到 HEAD:main/flomo_file.cpp（不是 git 仓库?）—— 跳过 pre-fix 检查"
            exit 0
        fi
        if cmp -s "$BUILD/flomo_file_prefix.cpp" "$REPO/main/flomo_file.cpp"; then
            echo ">> 注意: HEAD:main/flomo_file.cpp 与工作区逐字相同，pre-fix 检查是空转"
            echo ">>       （修复已经提交了？）"
        fi
        build "$BUILD/regress_prefix" "$BUILD/flomo_file_prefix.cpp" >/dev/null
        echo ">> 修复前版本（预期 FAIL）:"
        if "$BUILD/regress_prefix"; then
            echo ">> 意外: 修复前版本居然全过 —— 这测例抓不住那个 bug"
            exit 1
        fi
        echo ">> 如预期在修复前版本上失败"
        echo
        echo ">> 修复后版本（预期 PASS）:"
        "$BIN"
        ;;
    *)
        "$BIN"
        ;;
esac
