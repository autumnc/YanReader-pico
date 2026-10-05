#!/usr/bin/env bash
# Host-side probe for the EPUB chapter parser (ChapterHtmlSlimParser). See README.md.
#
#   tests/host/parser/run.sh                 # build + run fixture.xhtml, diff vs expected.txt
#   tests/host/parser/run.sh FILE.xhtml      # build + run your own .xhtml (no diff)
#   tests/host/parser/run.sh --update        # regenerate expected.txt from fixture.xhtml
#   tests/host/parser/run.sh --verify-fix    # also build the PRE-FIX parser and show the delta
#
# Plain g++/gcc -- no ESP-IDF, no CMake, no device, no zip.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"
CROSS="$REPO/components/crossmux"
BUILD="$SCRIPT_DIR/build"
BIN="$BUILD/probe"
FIXTURE="$SCRIPT_DIR/fixture.xhtml"
GOLDEN="$SCRIPT_DIR/expected.txt"
PARSER_REL="components/crossmux/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp"

# The commit that fixed the note-number back-jump ("注文区点注号跳不回正文"). Its parent
# still treats every `<a class="...backref...">` as a non-internal link, so the note pages
# come out with zero link rects -- `--verify-fix` builds exactly that and shows the delta.
PRE_FIX_REF="${PRE_FIX_REF:-11344f2^}"

CC="${CC:-gcc}"
CXX="${CXX:-g++}"

mkdir -p "$BUILD/expat"

# --- expat, compiled as C: g++ rejects these .c files (void* conversions) ---------
for f in xmlparse xmlrole xmltok; do
  if [ ! -f "$BUILD/expat/$f.o" ]; then
    echo ">> compiling expat/$f.c"
    "$CC" -O1 -g -w -c "$CROSS/lib/expat/$f.c" \
        -I "$CROSS/lib/expat" -DXML_GE=0 -DXML_CONTEXT_BYTES=1024 -DXML_NS=1 \
        -o "$BUILD/expat/$f.o"
  fi
done

# --- include paths: every directory under components/crossmux, stubs first --------
# stubs/ MUST come first so our GfxRenderer.h / HalStorage.h / Arduino.h shadow the real ones.
INCLUDES=(-I "$SCRIPT_DIR/stubs")
while IFS= read -r dir; do INCLUDES+=(-I "$dir"); done < <(
  find "$CROSS" -type d \( -name .git -o -name build \) -prune -o -type d -print | sort
)

DEFS=(-DFREEINK_DEVICE_READPICO=1 -DFREEINK_DEVICE_EEGO_A4=0 -DXML_GE=0 -D__LINUX__)
CXXFLAGS=(-std=gnu++20 -O1 -g -w)

# Real sources that must NOT be stubbed. TextBlock.cpp in particular: its xpos array is
# arena-backed, and a stub ctor that drops it makes wordXpos(i) read out of bounds.
REAL_SRC=(
  "$CROSS/lib/Epub/Epub/blocks/TextBlock.cpp"
  "$CROSS/lib/Epub/Epub/ParsedText.cpp"
  "$CROSS/lib/Epub/Epub/css/CssParser.cpp"
  "$CROSS/lib/Epub/Epub/converters/ImageDimsProbe.cpp"
  "$CROSS/lib/Utf8/Utf8.cpp"
)

# build_probe <out-binary> <parser-source>
build_probe() {
  echo ">> compiling $1  (parser: ${2#"$REPO"/})"
  "$CXX" "${CXXFLAGS[@]}" "${INCLUDES[@]}" "${DEFS[@]}" \
      "$SCRIPT_DIR/probe.cpp" "$SCRIPT_DIR/stubs.cpp" \
      "${REAL_SRC[@]}" "$2" \
      "$BUILD/expat/"*.o -o "$1"
}

# run_probe <binary> <xhtml>   -- stdout is the transcript
run_probe() {
  mkdir -p "$BUILD/csscache"
  ( cd "$BUILD" && PROBE_CACHE_DIR="$BUILD/csscache" "$1" "$2" )
}

build_probe "$BIN" "$CROSS/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp"

case "${1:-}" in
  --update)
    run_probe "$BIN" "$FIXTURE" > "$GOLDEN"
    echo ">> wrote $GOLDEN"
    exit 0
    ;;
  --verify-fix)
    if ! git -C "$REPO" show "$PRE_FIX_REF:$PARSER_REL" > "$BUILD/ChapterHtmlSlimParser_prefix.cpp" 2>/dev/null; then
      echo ">> cannot read $PRE_FIX_REF:$PARSER_REL (not a git repo, or ref is gone) -- skipping"
      exit 0
    fi
    if cmp -s "$BUILD/ChapterHtmlSlimParser_prefix.cpp" "$CROSS/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp"; then
      echo ">> WARNING: the working copy equals $PRE_FIX_REF; the check is vacuous"
    fi
    build_probe "$BUILD/probe_prefix" "$BUILD/ChapterHtmlSlimParser_prefix.cpp"
    run_probe "$BUILD/probe_prefix" "$FIXTURE" > "$BUILD/prefix.txt"
    run_probe "$BIN" "$FIXTURE" > "$BUILD/current.txt"
    echo ">> delta  $PRE_FIX_REF  ->  working copy:"
    if diff -u "$BUILD/prefix.txt" "$BUILD/current.txt"; then
      echo ">> UNEXPECTED: no difference (is the fix present?)"
      exit 1
    fi
    echo ">> ok: the fix changes the transcript"
    exit 0
    ;;
esac

if [ "$#" -ge 1 ]; then
  run_probe "$BIN" "$1"
  exit 0
fi

echo ">> running probe on fixture.xhtml"
run_probe "$BIN" "$FIXTURE" > "$BUILD/fixture.out"

if [ ! -f "$GOLDEN" ]; then
  echo ">> no expected.txt yet; writing it"
  cp "$BUILD/fixture.out" "$GOLDEN"
  cat "$GOLDEN"
  exit 0
fi

if diff -u "$GOLDEN" "$BUILD/fixture.out"; then
  echo ">> ok: transcript matches expected.txt"
else
  echo ">> FAIL: transcript differs from expected.txt (run with --update to accept)"
  exit 1
fi
