#!/usr/bin/env python3
"""把 NF-Propo.ttf (Nerd Font) 按 pjournal 实际用到的图标/符号码点裁成子集。

生成 main/assets/icon_font.ttf，供 gfx/icon_font.c 用 stb_truetype 加载。
只保留下列码点（与 font_renderer.cpp / markdown_render.cpp / main_menu_icons.h
中用到的图标一一对应）。缺失的 5 个几何图形 (U+25D0/U+25B8/U+25BE/U+270E/U+1F786)
由 icon_font.c 程序化绘制，不在此子集内。

用法:  python3 scripts/subset_icon_font.py [NF-Propo.ttf 路径]
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = Path(sys.argv[1]) if len(sys.argv) > 1 else (ROOT.parent / "NF-Propo.ttf")
OUT = ROOT / "main/assets/icon_font.ttf"

# 等宽 ASCII (NF-Proto 0.5em 等宽，编辑器 cell 模型依赖)
ASCII = list(range(0x20, 0x7F))

# 全角字符 (IME 全角模式 + 竖排标点)：NF-Proto 全角 1.0em
FULLWIDTH = [0x3000] + list(range(0xFF01, 0xFF5F)) + [0xFF61, 0xFF64]

# 状态栏电池/蓝牙/分割线/进度 (PUA，NF-Propo 自带)
STATUS = [0xE001, 0xE002, 0xE003,
          0xE004, 0xE005, 0xE006, 0xE007, 0xE008, 0xE009, 0xE00A, 0xE00B, 0xE00C]

# 文本内联符号：待办/完成、标题级别、折叠标志、次级列表空心圆、实心圆点
INLINE = [0xF0131, 0xF0132,
          0xF03A4, 0xF03A7, 0xF03AA, 0xF03AD, 0xF03B1, 0xF03B3,
          0xF09DA, 0x2022]

# 主菜单图标 (main_menu_icons.h)
# 0xF1AF1 是 NF-Propo 自带的一枚形状像"6"/"f"的图标，被借来当 Flomo 笔记的入口。
MENU = [0xF0335, 0xF03EB, 0xF05DA, 0xF062E, 0xF063F, 0xF0645, 0xF0756, 0xF1AF1]

# 虚拟键盘图形键帽 (editor_vk.cpp)：方向箭头、Shift、退格、回车、空格。
# 这些码点都在 5 位 PUA，icon_font_is_icon() 不覆盖，调用方直接调 icon_font_draw*。
KEYBOARD = [0xF0046, 0xF004E, 0xF0055, 0xF005E,   # ↓ ← → ↑
            0xF0069, 0xF006E,                     # apple-keyboard-shift, backspace-outline
            0xF0311, 0xF1050]                     # keyboard-return, keyboard-space

# 常见几何/标点符号 (builtin.ttf 未覆盖，需从图标字体补齐)
COMMON = [0x2026, 0x2191, 0x2193, 0x25CF, 0x25CB, 0x2605, 0x2610, 0x2713,
          0x25A0, 0x3008, 0x3009, 0x300A, 0x300B, 0x300C, 0x300D,
          0x300E, 0x300F, 0x2013, 0x2018, 0x2019, 0x201C, 0x201D,
          0x2500, 0xFE31]

# 标签栏图标 (main/tab_icons.h)。阅读模式根标签 + 计划模式视图标签，
# 以及两个列表页的搜索入口。名称取自 NF-Propo 的 MDI 码位表。
TABS = [0xF125F,   # md-bookshelf           书架
        0xF024B,   # md-folder              文件
        0xF082E,   # md-notebook            笔记
        0xF0493,   # md-cog                 设置
        0xF0687,   # md-inbox               收集箱
        0xF0734,   # md-arrow-right-bold    下一步
        0xF051F,   # md-timer-sand          等待
        0xF00D6,   # md-briefcase           项目
        0xF05E0,   # md-check-circle        已完成
        0xF0349]   # md-magnify             搜索

# 书架/笔记标签页搜索栏右侧的动作图标 (main/tab_icons.h 的 BAR_ICON_*)。
# 微读图标复用 MENU 组里的 0xF05DA(book_open_page_variant)，不重复列。
SHELF_BAR = [0xF0450]   # md-refresh   刷新（重新扫描书库）

# 文件浏览页的浮动按钮 (main/tab_icons.h 的 FAB_ICON_*)：网络文件管理。
# md-web 是个地球，比 wifi 图标更能说明"用浏览器打开"。
FAB = [0xF059F]         # md-web   网络文件管理（浏览器传书）

CHARSET = ASCII + FULLWIDTH + STATUS + INLINE + MENU + COMMON + KEYBOARD + TABS + SHELF_BAR + FAB


def main() -> int:
    from fontTools.subset import Subsetter, Options
    from fontTools.ttLib import TTFont

    if not SRC.is_file():
        print(f"missing source font: {SRC}", file=sys.stderr)
        return 1

    font = TTFont(SRC)
    options = Options()
    options.notdef_outline = True
    options.recommended_glyphs = True
    options.glyph_names = False
    options.ignore_missing_glyphs = True
    # 图标是单色、无连字，关掉 hinting 更小；保留 name 便于 stbtt 识别
    options.hinting = False
    options.layout_features = []
    options.name_IDs = ["*"]
    options.name_legacy = True

    text = "".join(chr(cp) for cp in sorted(set(CHARSET)))
    subsetter = Subsetter(options=options)
    subsetter.populate(text=text)
    subsetter.subset(font)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    font.save(OUT)
    font.close()
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes, {len(set(CHARSET))} glyphs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
