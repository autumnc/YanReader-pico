#!/usr/bin/env bash
# 主机端验证「ASCII 按字体真实字宽排版」（折行几何）。见 layout_equiv.cpp 的说明。
#
#   tests/host/layout/run.sh                 # 默认档（px=45, 屏宽 960）
#   tests/host/layout/run.sh --all           # 再把偶数档 50pt 也跑一遍（[4] 的严格等价路径）
#   tests/host/layout/run.sh --px 50         # 换字号（45 = 20pt 默认，50 = 22pt）
#   tests/host/layout/run.sh --screen 684    # 换屏宽（默认 960）
#
# 纯 g++，不需要 IDF 环境、不需要 builtin.ttf、不需要设备。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD="$SCRIPT_DIR/build"
BIN="$BUILD/layout_equiv"

mkdir -p "$BUILD"

# 对照物（改造前的折行几何）每次从 git HEAD 重新抽：手抄一份会随正文改动而静默过期，
# 护栏就会一直"绿"得没有意义。git 不可用时它自己跳过并说明。
python3 "$SCRIPT_DIR/gen_old.py"

CXX="${CXX:-g++}"
CXXFLAGS=(-std=c++17 -O1 -g -Wall -Wno-unused-variable -Wno-unused-function -Wno-sign-compare)

echo ">> compiling $BIN"
"$CXX" "${CXXFLAGS[@]}" -I "$BUILD" "$SCRIPT_DIR/layout_equiv.cpp" -o "$BIN"

if [ "${1:-}" = "--all" ]; then
    shift
    rc=0
    for px in 45 50; do
        echo
        echo "################ px=$px ################"
        "$BIN" --px "$px" "$@" || rc=1
    done
    exit $rc
fi

exec "$BIN" "$@"
