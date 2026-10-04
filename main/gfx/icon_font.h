/*
 * 图标字体层：从 NF-Propo.ttf 子集 (assets/icon_font.ttf) 光栅化图标/符号，
 * 用 stb_truetype 逐字形抗锯齿，并程序化补齐子集中缺失的几何图形。
 *
 * 与正文 ttf_font 分离：正文走官方 builtin.ttf / SD 字体，图标走 NF-Propo 子集。
 * 图标按 cell/box 居中绘制，供 FontRenderer(内联符号) 与主菜单图标共用。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 加载内嵌子集。返回 false 表示失败（图标退化为空白）。
bool icon_font_init(void);

/// 该码点是否属于图标字体（PUA + 常用几何符号，正文字体不覆盖）。
bool icon_font_is_icon(uint32_t cp);

/// 该码点是否为全角文字字形（全角标点/字母/数字/表意空格/半角假名标点）。
/// 这些字形在横排里应像正文一样按基线对齐，而不是像图标那样垂直居中。
bool icon_font_is_fullwidth(uint32_t cp);

/// 在 box [boxX,boxX+boxW) x [boxY,boxY+boxH) 内居中绘制图标。
/// fb: epdiy 4bpp 帧缓冲；cp: 图标码点；invert: true=白图标(需 box 已填黑)。
void icon_font_draw(uint8_t* fb, int boxX, int boxY, int boxW, int boxH,
                    uint32_t cp, bool invert);

/// 拉丁字符按基线对齐绘制（与正文 CJK 同基线），避免 ASCII 逐个居中导致高低不平。
/// baselineY 为基线（与 ttf_draw_text_px 的 y 同义）；字形在 boxW 内水平居中。
void icon_font_draw_baseline(uint8_t* fb, int x, int baselineY, int boxW, int boxH,
                             uint32_t cp, bool invert);

/// 把图标字形按「目标像素高 targetPx」光栅化并居中于 box。与 icon_font_draw
/// 不同：targetPx 是字形实际高度而非 box 高，用于大图标（NF-Propo 图标包围盒
/// 普遍小于 em，直接按 box 高光栅化会偏小）。targetPx<=0 时退回 box 高。
void icon_font_draw_sized(uint8_t* fb, int boxX, int boxY, int boxW, int boxH,
                          uint32_t cp, bool invert, int targetPx);

#ifdef __cplusplus
}
#endif
