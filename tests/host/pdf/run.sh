#!/usr/bin/env bash
# Host-side probe for the minimal PDF reader (components/crossmux/lib/Pdf).
#
#   tests/host/pdf/run.sh            build fixtures + probe, run, diff expected.txt
#   tests/host/pdf/run.sh --update   regenerate expected.txt
#
# Plain g++/gcc + Python stdlib zlib. No ESP-IDF, no third-party PDF library.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"
CROSS="$REPO/components/crossmux"
BUILD="$SCRIPT_DIR/build"
FIXTURES="$BUILD/fixtures"
BIN="$BUILD/probe"
GOLDEN="$SCRIPT_DIR/expected.txt"
JPEG="$REPO/tests/host/jpeg/fixture.jpg"

CC="${CC:-gcc}"
CXX="${CXX:-g++}"
mkdir -p "$BUILD"

echo ">> generating fixtures"
python3 "$SCRIPT_DIR/make_fixtures.py" "$FIXTURES" "$JPEG"

echo ">> compiling miniz (C)"
"$CC" -O1 -w -c "$CROSS/lib/miniz/src/miniz_impl.c" \
    -I "$CROSS/lib/miniz/src" -I "$CROSS/lib/miniz/third_party" \
    -o "$BUILD/miniz_impl.o"

INCLUDES=()
while IFS= read -r dir; do INCLUDES+=(-I "$dir"); done < <(
  find "$CROSS" -type d \( -name .git -o -name build \) -prune -o -type d -print | sort
)

# JBIG2 解码器没有仓库内夹具（主机上没有 jbig2enc 可生成编码流）；
# 它是在主机上用 jbig2dec 对拍真实扫描件语料验证的，这里只保证能编译链接。
PDF_SRC=(
  "$CROSS/lib/Pdf/PdfObject.cpp"
  "$CROSS/lib/Pdf/PdfFilters.cpp"
  "$CROSS/lib/Pdf/PdfXref.cpp"
  "$CROSS/lib/Pdf/PdfDocument.cpp"
  "$CROSS/lib/Pdf/PdfPage.cpp"
  "$CROSS/lib/Pdf/PdfContent.cpp"
  "$CROSS/lib/Pdf/PdfPxc.cpp"
  "$CROSS/lib/Pdf/PdfViewport.cpp"
  "$CROSS/lib/Pdf/PdfImage.cpp"
  "$CROSS/lib/Pdf/PdfJbig2.cpp"
)

echo ">> compiling probe"
"$CXX" -std=gnu++20 -O1 -g -w "${INCLUDES[@]}" -D__LINUX__ \
    "$SCRIPT_DIR/probe.cpp" "$SCRIPT_DIR/buildscratch_stub.cpp" "${PDF_SRC[@]}" \
    "$CROSS/lib/miniz/src/InflateStream.cpp" \
    "$BUILD/miniz_impl.o" -o "$BIN"

echo ">> running"
"$BIN" "$FIXTURES" | tee "$BUILD/out.txt"

if [ "${1:-}" = "--update" ]; then
  cp "$BUILD/out.txt" "$GOLDEN"
  echo ">> expected.txt updated"
else
  echo ">> diffing against expected.txt"
  diff -u "$GOLDEN" "$BUILD/out.txt"
  echo ">> PDF host probe passed"
fi
