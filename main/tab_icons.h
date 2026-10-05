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
// 引进真字形要重裁 NF-Propo 子集，而当前环境没有 fontTools（见 subset_icon_font.py）。
static const uint32_t TAB_ICON_APPS_SENTINEL = 1;

// ── 书架/笔记标签页顶部搜索栏右侧的动作图标 ─────────────────────────────
// 搜索复用上面的 TAB_ICON_SEARCH。刷新是新加进子集的（scripts/subset_icon_font.py
// 的 SHELF_BAR 组）。
static const uint32_t BAR_ICON_REFRESH   = 0xF0450;  // md-refresh 重新扫描书库

// 微读（book_open_page_variant，复用 MENU 组已有的）。**书架栏上那枚已经删掉了**
// （微读收进 1 号位「应用」标签之后没必要再挂一枚），现在只给应用页的微读入口用。
static const uint32_t BAR_ICON_WEREAD    = 0xF05DA;  // md-book-open-page-variant 微读

// ── 文件浏览页的浮动按钮（网络文件管理）──────────────────────────────────
// 是新加进子集的（scripts/subset_icon_font.py 的 FAB 组）。
static const uint32_t FAB_ICON_WEB       = 0xF059F;  // md-web 地球：浏览器打开传书

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
