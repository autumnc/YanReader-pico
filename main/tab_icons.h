#pragma once

#include <cstdint>

// 标签栏图标：阅读模式根标签、计划模式的视图标签，以及两个列表页的搜索入口。
// 码位取自 NF-Propo 的 MDI 表，由 scripts/subset_icon_font.py 的 TABS 组裁进
// assets/icon_font.ttf。这几个都在 5 位 PUA 区，icon_font_is_icon() 不覆盖，
// 所以调用方必须直接调 icon_font_draw_sized()，别指望文本路径自动路由。
//
// 尺寸约束：字形位图超过 icon_font.c 的 ICON_MAX_PX 会被**静默丢弃**（画出来是空的）。
// 标签栏的图标只有 FONT_H 量级（几十像素），远低于上限，但改大图标时记得这条。

// ── 阅读模式根标签（书架 / 文件 / 笔记 / 设置）──────────────────────────
static const uint32_t TAB_ICON_BOOKSHELF = 0xF125F;  // md-bookshelf
static const uint32_t TAB_ICON_FILES     = 0xF024B;  // md-folder
static const uint32_t TAB_ICON_NOTES     = 0xF082E;  // md-notebook
static const uint32_t TAB_ICON_SETTINGS  = 0xF0493;  // md-cog

// ── 计划模式视图标签（收集箱 / 下一步 / 等待 / 项目 / 已完成）──────────
static const uint32_t TAB_ICON_INBOX     = 0xF0687;  // md-inbox
static const uint32_t TAB_ICON_NEXT      = 0xF0734;  // md-arrow-right-bold
static const uint32_t TAB_ICON_WAITING   = 0xF051F;  // md-timer-sand
static const uint32_t TAB_ICON_PROJECT   = 0xF00D6;  // md-briefcase
static const uint32_t TAB_ICON_DONE      = 0xF05E0;  // md-check-circle

// ── 搜索入口 ───────────────────────────────────────────────────────────
static const uint32_t TAB_ICON_SEARCH    = 0xF0349;  // md-magnify

// ── 程序化绘制的标签图标（不是码点，只是 drawTabBar 的分派哨兵）──────────
// 0 = 统计（柱状图，drawStatsTabIcon）、1 = 应用（九宫格，drawAppsTabIcon）。
// 想换成真字形就得把码点加进 scripts/subset_icon_font.py 再重裁子集；重裁本身能跑 ——
// pip 装不上 fontTools（PEP 668），但 `uv run --with fonttools -- python3
// scripts/subset_icon_font.py` 可以（2026-10-13 的 APPS 组就是这么裁出来的）。
static const uint32_t TAB_ICON_APPS_SENTINEL = 1;

// ── 书架/笔记标签页顶部搜索栏右侧的动作图标 ─────────────────────────────
// 搜索复用上面的 TAB_ICON_SEARCH。刷新是新加进子集的（scripts/subset_icon_font.py
// 的 SHELF_BAR 组）。
static const uint32_t BAR_ICON_REFRESH   = 0xF0450;  // md-refresh 重新扫描书库

// ── 「应用」页两个入口的专用图标 ────────────────────────────────────────
// 这两位是 2026-10-13 **用户自己画好加进 NF-Propo 源字体**的（不在 MDI 通用码位表里），
// 再由 scripts/subset_icon_font.py 的 APPS 组裁进子集 —— 之前「微信读书」借的是
// md-book-open-page-variant(0xF05DA)、「DeepSeek 对话」是程序化画的，两枚都只是"意思
// 差不多"。换成专用图形后一眼就分得出谁是谁。**换 NF-Propo 时必须确认源字体里还有
// 这两位**，否则 icon_font_draw_sized 会静默画成空白（日志里有一行 glyph missing）。
static const uint32_t APP_ICON_WEREAD    = 0xF1AF3;  // 微信读书
static const uint32_t APP_ICON_DEEPSEEK  = 0xF1AF4;  // DeepSeek 对话
// 应用商店：购物袋（FA 段的 shopping_bag）。之前是程序化画的店面，缩小后雨棚/门
// 几条细线容易糊成一块；这枚实心图形在 96px 下轮廓清楚。4 位码位只走
// icon_font_draw_sized()，别交给文本路径。
static const uint32_t APP_ICON_STORE     = 0xF290;

// ── 文件浏览页的浮动按钮（网络文件管理）──────────────────────────────────
// 是新加进子集的（scripts/subset_icon_font.py 的 FAB 组）。
static const uint32_t FAB_ICON_WEB       = 0xF059F;  // md-web 地球：浏览器打开传书
// 眼睛（「显示隐藏文件」开关）：**没进子集**（当初只是没裁，不是裁不了 —— 见上面
// subset_icon_font.py 那条），由 gfx/icon_font.c 的 draw_prog() 按几何画
// （同 ◐/▸ 那几个的做法）。所以它只能走
// icon_font_draw()，**不能**用 icon_font_draw_sized()——后者找不到字形会直接返回空白。
static const uint32_t FAB_ICON_EYE       = 0x1F441;  // 👁 眼睛：显示隐藏文件

// ── 标签栏几何：阅读模式和计划模式共用一套 ───────────────────────────────
// 图标从原来的 42~45px（一个 UI 行高）放大到 56px。原因：e-ink 上手指落点很糙，
// 图标小的时候用户得瞄着字形去点，实际按不准——放大是给眼睛用的，命中区另外
// 单独放宽（见 tabHit / GTD 的标签点按分支，整条横带都算，不看字形）。
// 两个模式用同一组数值，切模式时图标大小和离顶边的距离不会跳。
// 尺寸上限是 icon_font.c 的 ICON_MAX_PX(112)，这里离得很远。
#define TAB_ICON_PX      56   // 字形边长
#define TAB_ICON_INSET   20   // 字形上沿距屏幕顶部
#define TAB_BOX_PAD       8   // 活动标签黑底在字形四周多出的留白
#define TAB_BAND_BOTTOM (TAB_ICON_INSET + TAB_ICON_PX + TAB_BOX_PAD)  // 84：标签栏底边(分隔线)
