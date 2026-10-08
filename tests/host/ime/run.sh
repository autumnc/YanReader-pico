#!/usr/bin/env bash
# Build + run the host-side pinyin IME harness. See README.md.
#
#   tests/host/ime/run.sh                    # build, run diagnostics + the regression test
#   tests/host/ime/run.sh --regress-only     # build, run only the regression test (quiet)
#   tests/host/ime/run.sh --verify-fix       # prove the regression fails on the pre-fix IME.cpp
#   tests/host/ime/run.sh zhege 1            # build, then one ad-hoc diagnostic scenario
#   tests/host/ime/run.sh --regress jiushi 就是
#   tests/host/ime/run.sh --enter nihao      # 输入中按回车 → 编码原样上屏（加 --highlight 换语义）
#
# Plain g++ -std=c++17; no ESP-IDF, no CMake, no device.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD="$SCRIPT_DIR/build"
BIN="$BUILD/ime_driver"

mkdir -p "$BUILD"

# The .incbin paths in stubs.cpp are relative to the repo root, so build from there.
cd "$REPO"

CXX="${CXX:-g++}"
# -Wno-sign-compare: the warnings come from the firmware sources (main/ime/IME.cpp), which
# are compiled verbatim; the ESP-IDF build does not enable -Wall, and we do not patch them.
CXXFLAGS=(-std=c++17 -O1 -g -Wall -Wno-unused-variable -Wno-unused-function -Wno-sign-compare)

# $1 = output binary, $2 = path to the IME.cpp to compile (repo one by default)
build() {
    local out="$1" ime_src="$2"
    echo ">> compiling $out"
    "$CXX" "${CXXFLAGS[@]}" \
        -I "$SCRIPT_DIR" \
        -I "$REPO/main" \
        -I "$REPO/main/ime" \
        "$SCRIPT_DIR/ime_driver.cpp" \
        "$SCRIPT_DIR/stubs.cpp" \
        "$ime_src" \
        "$REPO/main/ime/yong_dict.cpp" \
        "$REPO/main/ime/yong_pinyin.cpp" \
        -o "$out"
}

build "$BIN" "$REPO/main/ime/IME.cpp"

# The two sequences from the bug report. The intermediate keystroke matters, so the
# driver feeds them one at a time (see ime_driver.cpp).
REGRESS_CASES=("zhege 这个" "jiushi 就是")

run_regress() {
    local bin="$1" expect="$2" failed=0
    for case in "${REGRESS_CASES[@]}"; do
        for paging in "" "--fixed"; do
            if "$bin" --regress $case $paging > "$BUILD/regress.out" 2>&1; then
                echo "  pass [$expect] regress $case ${paging:-width-paging}"
            else
                echo "  FAIL [$expect] regress $case ${paging:-width-paging}"
                sed -n '/=== regression/,$p' "$BUILD/regress.out"
                failed=1
            fi
        done
    done
    return $failed
}

# 「输入中按回车 = 编码原样上屏」（用户 2026-10-05 报告"这个功能没了"）。不放进
# run_regress：那组是拿 pre-fix 的 IME.cpp 反证用的，而这条从来没坏过 —— 它是
# **回归护栏**，钉住这个语义别在以后被改掉。
ENTER_CASES=("--enter nihao" "--enter zhege" "--enter nihao --highlight")

run_enter() {
    local bin="$1" expect="$2" failed=0
    for case in "${ENTER_CASES[@]}"; do
        if "$bin" $case > "$BUILD/enter.out" 2>&1; then
            echo "  pass [$expect] $case"
        else
            echo "  FAIL [$expect] $case"
            sed -n '/=== Enter/,$p' "$BUILD/enter.out"
            failed=1
        fi
    done
    return $failed
}

# 候选分页的行宽：横屏一页装得下的 ≥ 竖屏（用户 2026-10-08：横屏空间大，候选该更多）。
# 同 ENTER_CASES 一样是**回归护栏**，不进 run_regress：那组是拿 pre-fix 的 IME.cpp
# 反证用的，而这条只钉 IME 对"喂进来的行宽"的契约（只用 setDisplayWidth），两个版本
# 都过。设备上真正出过的问题是 main.cpp 喂了一个开机快照 —— 那不在这个 harness 的
# 射程里，靠 --paging 打印出来的两行数字人工对一眼。
PAGING_CASES=("nihao" "zhege" "jiushi")

run_paging() {
    local bin="$1" expect="$2" failed=0
    for case in "${PAGING_CASES[@]}"; do
        if "$bin" --paging "$case" > "$BUILD/paging.out" 2>&1; then
            echo "  pass [$expect] --paging $case  $(sed -n 's/^  PASS: //p' "$BUILD/paging.out")"
        else
            echo "  FAIL [$expect] --paging $case"
            sed -n '/=== 候选分页/,$p' "$BUILD/paging.out"
            failed=1
        fi
    done
    return $failed
}

case "${1:-}" in
    --regress-only)
        echo ">> regression test (current main/ime/IME.cpp)"
        run_regress "$BIN" "current"
        echo ">> Enter contract"
        run_enter "$BIN" "current"
        echo ">> 候选行宽：横屏一页 ≥ 竖屏一页"
        run_paging "$BIN" "current"
        echo ">> regression passed"
        exit 0
        ;;
    --verify-fix)
        # Build a second binary against the PRE-FIX IME.cpp taken from git HEAD (the
        # working copy has the fix). Expectation: passes now, fails before the fix.
        if ! git -C "$REPO" show HEAD:main/ime/IME.cpp > "$BUILD/IME_prefix.cpp" 2>/dev/null; then
            echo ">> cannot read HEAD:main/ime/IME.cpp (not a git repo?) - skipping pre-fix check"
            exit 0
        fi
        if cmp -s "$BUILD/IME_prefix.cpp" "$REPO/main/ime/IME.cpp"; then
            echo ">> WARNING: HEAD:main/ime/IME.cpp is identical to the working copy;"
            echo ">>          the pre-fix check is vacuous (is the fix committed?)"
        fi
        build "$BUILD/ime_driver_prefix" "$BUILD/IME_prefix.cpp" >/dev/null

        echo ">> regression against the PRE-FIX build (expect FAIL):"
        if run_regress "$BUILD/ime_driver_prefix" "pre-fix"; then
            echo ">> UNEXPECTED: pre-fix build passed the regression"
            exit 1
        fi
        echo ">> regression against the CURRENT build (expect PASS):"
        run_regress "$BIN" "current"
        echo ">> verified: fails before the fix, passes after"
        exit 0
        ;;
esac

if [ "$#" -ge 1 ]; then
    exec "$BIN" "$@"
fi

echo ">> running diagnostic scenarios"
for letters in zhege jiushi; do
    for idx in 0 1 2; do
        echo
        echo "############################################################"
        echo "# $letters  commit index $idx  (width paging)"
        echo "############################################################"
        "$BIN" "$letters" "$idx"
    done
    echo
    echo "############################################################"
    echo "# $letters  commit index 0  (fixed page-size paging)"
    echo "############################################################"
    "$BIN" "$letters" 0 --fixed
done

echo
echo "############################################################"
echo "# regression test (zhege/jiushi, both paging modes)"
echo "############################################################"
run_regress "$BIN" "current"
echo
echo "############################################################"
echo "# 输入中按回车 = 编码原样上屏"
echo "############################################################"
run_enter "$BIN" "current"
echo
echo "############################################################"
echo "# 候选分页的行宽：横屏一页装得下的 ≥ 竖屏"
echo "############################################################"
run_paging "$BIN" "current"
echo ">> all regression cases passed"
