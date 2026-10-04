#pragma once

// 模式独立方向的切换（阅读模式 / 计划模式各有自己的一份方向设置）。
//
// screen_reader.cpp 不能 include epdiy.h / board.h：epdiy.h 会连带 epd_internals.h，
// 其中 typedef 的 EpdGlyph/EpdUnicodeInterval/EpdFont 与 crossmux 的 EpdFontData.h/
// EpdFont.h（EpdFont 是 class）符号冲突。因此旋转切换在 board.cpp 实现，这里只暴露
// 最小原型（无任何 epdiy 头依赖）——需要切方向的界面（含 screen_gtd.cpp）引这个头。

// 进入阅读模式：强制横屏（EPD_ROT_LANDSCAPE），使 HalDisplay 返回 1216×684。
void board_force_landscape(void);

// 阅读器内切换到竖屏（EPD_ROT_INVERTED_PORTRAIT），使 HalDisplay 返回 684×1216。
void board_force_portrait(void);

// 退出阅读模式：恢复最近一次 board_apply_orientation 应用的方向设置。
void board_restore_orientation(void);
