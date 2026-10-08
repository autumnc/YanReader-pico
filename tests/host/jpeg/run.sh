#!/usr/bin/env bash
# 主机侧对拍：ProgressiveJpegScaled 的降尺度解 vs stb 全解 + 像素域箱平均。
# 见 README.md。
#
#   tests/host/jpeg/run.sh                  # 编 + 拿 fixture.jpg 按阈值判定
#   tests/host/jpeg/run.sh FILE.jpg [...]   # 编 + 只打统计（任意素材，不判定）
#   tests/host/jpeg/run.sh --bench          # 顺带报主机耗时
#   tests/host/jpeg/run.sh --dump DIR       # 顺带把「我们解的」和「参考的」各写一份 PGM
#
# 纯 g++，没有 ESP-IDF、没有 CMake、没有设备。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"
JPEG="$REPO/components/crossmux/lib/JpegToBmpConverter"
BUILD="$SCRIPT_DIR/build"
BIN="$BUILD/probe"
FIXTURE="$SCRIPT_DIR/fixture.jpg"

CXX="${CXX:-g++}"
CXXFLAGS=(-std=gnu++20 -O2 -g -w -I "$JPEG")

# 参考解默认走 djpeg（libjpeg 自己的降尺度解，和本解码器输出同一个量）；没有就提示
# 一句、改用 stb 兜底，但那套口径在彩色硬边处会虚高，别据它下结论。
if ! command -v djpeg >/dev/null 2>&1; then
  echo ">> 没找到 djpeg（libjpeg-turbo）：参考解退化成 stb 口径，彩色封面会虚高；" >&2
  echo "   apt install libjpeg-turbo-progs / pacman -S libjpeg-turbo 之后再看。不停下。" >&2
  set -- "$@" --ref-stb
fi

mkdir -p "$BUILD"

# 只在源文件变过时重编（stb_image.h 是大头，两秒起步）。
SOURCES=(
  "$SCRIPT_DIR/probe.cpp"
  "$SCRIPT_DIR/stb_host.cpp"
  "$JPEG/ProgressiveJpegScaled.cpp"
)

need_build=0
[ -x "$BIN" ] || need_build=1
for s in "${SOURCES[@]}"; do
  [ "$s" -nt "$BIN" ] && need_build=1
done
[ "$JPEG/ProgressiveJpegScaled.h" -nt "$BIN" ] && need_build=1
[ "$JPEG/stb_image.h" -nt "$BIN" ] && need_build=1

if [ "$need_build" -eq 1 ]; then
  echo ">> compiling probe"
  "$CXX" "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BIN"
fi

# 给了 .jpg 就当素材用；只给开关（--bench/--dump …）就拿 fixture 顶上。
HAS_FILE=0
for a in "$@"; do
  case "$a" in
    *.jpg|*.jpeg|*.JPG|*.JPEG) HAS_FILE=1 ;;
  esac
done
if [ "$HAS_FILE" -eq 0 ] && [ "$#" -eq 0 ]; then
  echo ">> running probe on fixture.jpg (--check)"
  exec "$BIN" --check "$FIXTURE"
elif [ "$HAS_FILE" -eq 0 ]; then
  exec "$BIN" "$@" "$FIXTURE"
else
  exec "$BIN" "$@"
fi
