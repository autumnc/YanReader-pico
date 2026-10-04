#!/usr/bin/env python3
"""YanReader 标志字体：Noto Serif CJK SC SemiBold → 只含标志用字的 glyf 子集。

照 builtin.ttf 的 recipe（见 memory: font-architecture）：
  fontTools.subset 取字 → 手写 CFF→glyf（Cu2QuPen + TTGlyphPen）→ 补 loca/maxp/post →
  **hhea 必须改成 880/-120**。

那条 hhea 铁律是给 stb_truetype 的：它的 ScaleForPixelHeight 用
hhea ascender-descender 定标，而不是 em。Noto CJK 的 hhea 是 1160/-288（差 1448），
em 却是 1000 —— 不改的话字形只渲染到 69%，90px 的「研」看着像 62px。
"""
import sys
from fontTools import subset
from fontTools.ttLib import TTFont, newTable
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.ttGlyphPen import TTGlyphPen

SRC = "/usr/share/fonts/noto-cjk/NotoSerifCJK-SemiBold.ttc"
FONT_NUMBER = 2            # Noto Serif CJK SC
TEXT = "研读墨行砚石开YanReader·-—0123456789 "
OUT = sys.argv[1] if len(sys.argv) > 1 else "yanos_logo.ttf"
MAX_ERR = 0.6              # cu2qu 三次→二次的容差（em=1000，0.6 = 0.06% 视觉无损）


def subset_otf(path):
    opts = subset.Options()
    opts.drop_tables += ["DSIG"]
    opts.no_hinting = True
    opts.desubroutinize = True
    opts.notdef_outline = True
    opts.recalc_bounds = True
    opts.layout_features = []          # 单字成画，不需要 GSUB/GPOS
    font = TTFont(SRC, fontNumber=FONT_NUMBER)   # 直接从 ttc 取 SC 那一面
    subsetter = subset.Subsetter(options=opts)
    subsetter.populate(text=TEXT)
    subsetter.subset(font)
    font.save(path)
    return TTFont(path)


def otf_to_ttf(font):
    assert font.sfntVersion == "OTTO" and "CFF " in font, "不是 CFF 字体"
    glyph_order = font.getGlyphOrder()
    glyph_set = font.getGlyphSet()

    glyf = newTable("glyf")
    glyf.glyphOrder = glyph_order
    glyf.glyphs = {}
    for name in glyph_order:
        pen = TTGlyphPen(glyph_set)
        glyph_set[name].draw(Cu2QuPen(pen, MAX_ERR, reverse_direction=True))
        glyf.glyphs[name] = pen.glyph()
    font["glyf"] = glyf
    font["loca"] = newTable("loca")
    for t in ("CFF ", "VORG", "vhea", "vmtx", "BASE", "GSUB", "GPOS", "GDEF"):
        if t in font:
            del font[t]
    glyf.compile(font)
    font.sfntVersion = "\000\001\000\000"

    maxp = newTable("maxp")
    maxp.tableVersion = 0x00010000
    maxp.numGlyphs = len(glyph_order)
    gs = list(glyf.glyphs.values())
    maxp.maxPoints = max((len(g.coordinates) for g in gs), default=0)
    maxp.maxContours = max((len(g.endPtsOfContours) for g in gs), default=0)
    maxp.maxCompositePoints = 0
    maxp.maxCompositeContours = 0
    maxp.maxZones = 1
    maxp.maxTwilightPoints = 0
    maxp.maxStorage = 0
    maxp.maxFunctionDefs = 0
    maxp.maxInstructionDefs = 0
    maxp.maxStackElements = 0
    maxp.maxSizeOfInstructions = 0
    maxp.maxComponentElements = 0
    maxp.maxComponentDepth = 0
    font["maxp"] = maxp

    post = font["post"]
    post.formatType = 3.0              # 不留字形名，省体积（stb_truetype 用不到）
    post.extraNames = []
    post.mapping = {}
    post.glyphOrder = glyph_order

    head = font["head"]
    head.glyphDataFormat = 0
    head.indexToLocFormat = 0          # 编译时按实际偏移自动升级为 1

    hhea = font["hhea"]
    hhea.ascent, hhea.descent, hhea.lineGap = 880, -120, 0   # ★ 见模块 docstring
    hhea.numberOfHMetrics = len(font["hmtx"].metrics)
    return font


def rename(font, family):
    for rec in font["name"].names:
        if rec.nameID == 1:
            rec.string = family
        elif rec.nameID == 4:
            rec.string = family
        elif rec.nameID == 6:
            rec.string = family.replace(" ", "")
        elif rec.nameID == 16:
            rec.string = family


if __name__ == "__main__":
    f = subset_otf("/tmp/yanos/_subset.otf")
    otf_to_ttf(f)
    rename(f, "YanReader Logo")
    f.save(OUT)
    print(f"saved {OUT}")
