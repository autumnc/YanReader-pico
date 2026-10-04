#pragma once

#include <cstdint>

// 主菜单图标：从 NF-Propo 图标字体子集按需光栅化（scripts/subset_icon_font.py 的
// MENU 组），替代原 48px 位图。drawMainMenuIcon 用 icon_font_draw 按码点绘制。
struct MainMenuIconGlyph {
    uint32_t codepoint;   // Nerd Font 图标码点
};

// 与 NF-Propo Nerd Font 一一对应（子集已包含这 7 个码点）。
static const MainMenuIconGlyph MAIN_ICON_PROMPT   = { 0xF0335 };  // lightbulb
static const MainMenuIconGlyph MAIN_ICON_FREE     = { 0xF03EB };  // pencil
static const MainMenuIconGlyph MAIN_ICON_VIEW     = { 0xF05DA };  // book_open_page_variant
static const MainMenuIconGlyph MAIN_ICON_SYNC     = { 0xF063F };  // cloud_sync
static const MainMenuIconGlyph MAIN_ICON_GTD      = { 0xF0756 };  // format_list_checks
static const MainMenuIconGlyph MAIN_ICON_OUTLINE  = { 0xF0645 };  // file_tree
static const MainMenuIconGlyph MAIN_ICON_SETTINGS = { 0xF062E };  // tune
static const MainMenuIconGlyph MAIN_ICON_FLOMO    = { 0xF1AF1 };  // 形状像 "6"/"f" 的一枚图标，当 Flomo 笔记入口
