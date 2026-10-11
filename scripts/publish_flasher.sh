#!/usr/bin/env bash
# 把网页刷机台（docs/）+ 当前 Release 的整包发布到 gh-pages 分支，供 GitHub Pages 提供。
#
#   scripts/publish_flasher.sh                          # 取最新 Release 的整包，发布到 gh-pages（force-push）
#   scripts/publish_flasher.sh --tag v1.6.2             # 指定 tag
#   scripts/publish_flasher.sh --bin build/merged.bin   # 用本地合出来的包（还没发版也能先看效果）
#   scripts/publish_flasher.sh --out /tmp/site          # 只落盘不推，本地 python3 -m http.server 预览
#   scripts/publish_flasher.sh --bin X --origin /tmp/o.git   # 推到别的远端（也用于本地自测）
#
# 它在发版流程里的位置：
#   抬版本号（main/CMakeLists.txt 的 YAN_READER_VERSION）→ push → tag → merge_bin 合整包
#   → gh release 传两个 bin → 【本脚本】
#
# 为什么用 gh-pages 而不是把 bin 提进 master：
#   整包 10.7MB，提进 master 会让 .git 从 15MB 直接翻倍，之后每次发版再 +10.7MB 永久入史。
#   gh-pages 每次 force-push，历史不增长，也不需要任何新的 gh 权限（不用 workflow scope）。
#
# ★ 本脚本**绝不碰主工作区的 git 状态**（不 add / 不 commit / 不切分支）：
#   它在临时目录里重新 git init 一个独立仓库再 push。master 上就算有一堆未提交改动也不受影响。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
BRANCH="gh-pages"

die() { printf '✗ %s\n' "$*" >&2; exit 1; }
info() { printf '  %s\n' "$*"; }

TAG=""
BIN=""
OUT=""
ORIGIN_OVERRIDE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --tag) TAG="${2:?--tag 后面要跟 tag 名（如 v1.6.2）}"; shift 2 ;;
        --bin) BIN="${2:?--bin 后面要跟文件路径}"; shift 2 ;;
        --out) OUT="${2:?--out 后面要跟目录}"; shift 2 ;;
        --origin) ORIGIN_OVERRIDE="${2:?--origin 后面要跟远端 URL 或路径}"; shift 2 ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) die "未知参数：$1（试 --help）" ;;
    esac
done

[ -f "$REPO/docs/index.html" ] || die "找不到 $REPO/docs/index.html —— 刷机台的源码就在那儿"

if [ -n "$ORIGIN_OVERRIDE" ]; then
    ORIGIN="$ORIGIN_OVERRIDE"
    [ -z "$BIN" ] && die "用 --origin 时要配 --bin（--origin 是给本地自测 / 换远端用的）"
else
    ORIGIN="$(git -C "$REPO" remote get-url origin)"
fi

# 只从 github.com 的 URL 里解仓库名；--origin 指本地路径时算不出来（也不影响发布）。
SLUG="$(printf '%s' "$ORIGIN" |
    sed -nE 's#(git@|https://)github\.com[:/](.+)$#\2#p' |
    sed 's#\.git$##')"
if [ -z "$SLUG" ] && [ -z "$ORIGIN_OVERRIDE" ]; then
    die "从 origin（$ORIGIN）推不出 GitHub 仓库名"
fi

# 版本号的唯一来源是 main/CMakeLists.txt 的 YAN_READER_VERSION；tag 只是用来交叉核对。
CMAKE_VER="$(sed -n 's/.*YAN_READER_VERSION="\([^"]*\)".*/\1/p' "$REPO/main/CMakeLists.txt" | head -1)"
[ -n "$CMAKE_VER" ] || die "没能从 main/CMakeLists.txt 读出 YAN_READER_VERSION"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ── 1. 拿到整包 ────────────────────────────────────────────────────────

SRC_BIN=""
if [ -n "$BIN" ]; then
    [ -f "$BIN" ] || die "找不到 --bin 指定的文件：$BIN"
    SRC_BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
    if [ -z "$TAG" ]; then
        VER="$CMAKE_VER"
    fi
fi

if [ -z "$SRC_BIN" ]; then
    command -v gh >/dev/null 2>&1 || die "需要 gh 命令（或改用 --bin 指定本地整包）"
    if [ -z "$TAG" ]; then
        TAG="$(gh release view --repo "$SLUG" --json tagName -q .tagName)" \
            || die "读最新 Release 失败：$SLUG（网络？还是改用 --tag / --bin）"
        info "最新 Release：$TAG"
    fi
    gh release download "$TAG" --repo "$SLUG" --pattern '*-full.bin' --dir "$WORK" --clobber \
        || die "从 $TAG 下载 *-full.bin 失败"
    SRC_BIN="$(find "$WORK" -maxdepth 1 -name '*-full.bin' | head -1)"
    [ -n "$SRC_BIN" ] || die "$TAG 里没有 *-full.bin 资产（整包才用于刷机）"
fi

[ -n "${VER:-}" ] || VER="${TAG#v}"
VER="${VER#v}"

if [ -n "$TAG" ] && [ "$VER" != "$CMAKE_VER" ]; then
    printf '⚠ tag 是 %s（版本 %s），而 main/CMakeLists.txt 里是 %s —— 版本号是不是忘了抬？\n' \
        "$TAG" "$VER" "$CMAKE_VER" >&2
    printf '  站点会按 %s 发布。要按源码版本发就加 --bin 并去掉 --tag。\n' "$VER" >&2
fi

# ── 2. 先自检，不合格就不发布 ───────────────────────────────────────────

python3 - "$SRC_BIN" <<'PY' || die "整包自检不通过，已中止（没有生成站点、没有推送）"
import os, sys

path = sys.argv[1]
size = os.path.getsize(path)
LIMIT = 16 * 1024 * 1024

if size > LIMIT:
    sys.exit("✗ %s 有 %.2f MB，超过 16MB flash 上限" % (path, size / 1048576))
if size < 0x10001:
    sys.exit("✗ %s 只有 %d 字节，短得不像整包镜像" % (path, size))

with open(path, "rb") as f:
    boot = f.read(1)[0]
    f.seek(0x8000)
    part = f.read(2)
    f.seek(0x10000)
    app = f.read(1)[0]

if boot != 0xE9:
    sys.exit("✗ 0x0 处不是引导头（读到 0x%02X，应为 0xE9）—— 不是整包镜像" % boot)
# ★ 这一条才是拦住「误把 app-only 的 pjournal_pico.bin 当整包」的那道闸：
#   应用镜像本身也是 0xE9 开头，所以上面 0x0 那条查不出它，是 0x8000 这处露的馅。
if part[0] != 0xAA or part[1] != 0x50:
    sys.exit("✗ 0x8000 处不是分区表（读到 %02X %02X，应为 AA 50）—— 这多半是 app-only 的 pjournal_pico.bin，刷到 0x0 会让设备起不来" % (part[0], part[1]))
if app != 0xE9:
    sys.exit("✗ 0x10000 处不是应用镜像（读到 0x%02X，应为 0xE9）—— 不是整包镜像" % app)

print("✓ 整包自检通过：%d 字节（0x0 引导头 / 0x8000 分区表 / 0x10000 应用）" % size)
PY

# ── 3. 组装站点 ────────────────────────────────────────────────────────

STAGE="$WORK/site"
mkdir -p "$STAGE"
cp -a "$REPO/docs/." "$STAGE/"          # 注意结尾的 /. —— 连 .nojekyll 一起复制

DEST_NAME="yanreader-${VER}-full.bin"
mkdir -p "$STAGE/firmware"
cp "$SRC_BIN" "$STAGE/firmware/$DEST_NAME"

SIZE="$(stat -c%s "$STAGE/firmware/$DEST_NAME")"
SHA256="$(sha256sum "$STAGE/firmware/$DEST_NAME" | cut -d' ' -f1)"
MD5="$(md5sum "$STAGE/firmware/$DEST_NAME" | cut -d' ' -f1)"
TODAY="$(date +%F)"

cat > "$STAGE/firmware.json" <<JSON
{
  "version": "$VER",
  "tag": "${TAG:-v$VER}",
  "date": "$TODAY",
  "file": "firmware/$DEST_NAME",
  "size": $SIZE,
  "flashOffset": 0,
  "sha256": "$SHA256",
  "md5": "$MD5"
}
JSON

python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$STAGE/firmware.json" \
    || die "生成的 firmware.json 不是合法 JSON"

printf '\n站点已组装：%s\n' "$STAGE"
info "版本      $VER（tag ${TAG:-v$VER}）"
info "固件      firmware/$DEST_NAME · $SIZE 字节"
info "sha256    $SHA256"
info "md5       $MD5"
info "文件      $(find "$STAGE" -type f | wc -l) 个"

# ── 4a. --out：只落盘 ──────────────────────────────────────────────────

if [ -n "$OUT" ]; then
    rm -rf "$OUT"
    mkdir -p "$OUT"
    cp -a "$STAGE/." "$OUT/"
    printf '\n✓ 已落盘到 %s（未推送）\n' "$OUT"
    printf '  本地预览： cd %s && python3 -m http.server 8777\n' "$OUT"
    printf '  然后 Chrome 打开 http://localhost:8777/ （localhost 算安全上下文，WebSerial 可用）\n'
    exit 0
fi

# ── 4b. 发布到 gh-pages（临时目录里 force-push，不碰主工作区）────────────

cd "$STAGE"
git init -q -b "$BRANCH"
git add -A
git commit -q -m "刷机台站点：YanReader v$VER（$TODAY，sha256 ${SHA256:0:12}…）"

printf '\n正在推送 %s → %s …\n' "$BRANCH" "${SLUG:-$ORIGIN}"
if ! git push -f "$ORIGIN" "$BRANCH"; then
    die "推送失败。若提示认证问题，先跑一次： gh auth setup-git"
fi

cat <<EOF

✓ 已发布到 $BRANCH（远端 $ORIGIN）
EOF

if [ -n "$SLUG" ]; then
    OWNER="$(printf '%s' "$SLUG" | cut -d/ -f1)"
    NAME="$(printf '%s' "$SLUG" | cut -d/ -f2)"
    cat <<EOF

  站点地址（Pages 开启后）：https://$OWNER.github.io/$NAME/

  首次发布还需要开启 Pages（只需一次）：
    gh api -X POST repos/$SLUG/pages -f source[branch]=$BRANCH -f source[path]=/
  之后每次发版只要重跑本脚本，站点会自己更新（Pages 会在 push 后自动重建）。
EOF
fi
