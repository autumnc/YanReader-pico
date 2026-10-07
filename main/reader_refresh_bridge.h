/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 阅读器 → 推屏层 的"白底参考帧纪律"提示，以及灰阶自检页的自推屏入口。
 *
 * 和 reader_page_turn.h 同样的理由单独一个头：阅读器那几个 TU **不能** include
 * display.h（display.h → epdiy.h → epd_internals.h，那里的 EpdFont typedef 与
 * crossmux 的 EpdFont(class) 在同一 TU 冲突）。这里只放不依赖 epdiy 的裸声明，
 * 实现在 display.c。
 *
 * ── 白底参考帧纪律是什么 ───────────────────────────────────────────────
 * 差分刷（GL16/DU）的基准是面板**当前**的像素。面板上如果留着中间灰，下一屏
 * 从这块灰上差分出来就是脏的。官方固件为此在每次离开灰阶画面时先 GC16 铺一屏
 * 白、再从白底出下一屏（"中间灰不能当参考帧"，app_refresh.c）。
 * 我们这边：插图页 / 图片查看器 / 灰阶自检页都会在面板上留下真正的中灰，而
 * 从它们翻到正文页走的是差分档 —— 所以需要在"上一帧是灰"这件事上记一笔账，
 * 由推屏层在紧接着的那一帧差分刷之前把白底补上。
 *
 * 记账**必须在推屏之后**做（语义是"面板现在是什么"），放到渲染入口就会让同一页
 * 的下一帧被自己置位、变成每帧白闪；消费侧只对差分档铺白（FULL/GRAY8 本来就是
 * 整屏全像素 GC16，再铺一次只是白花一次全刷）。
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// 记下"面板上现在是不是中灰"。阅读器每次推屏**之后**调一次：
///   on != 0 —— 刚推上去的这一帧是灰阶画面（插图页/图片查看器/自检页）
///   on == 0 —— 刚推上去的这一帧是纯黑白（正文/菜单/列表）
/// 下一次走差分档的推屏会先 GC16 铺白再画，然后这笔账自动清掉（一次性）。
void reader_set_gray_panel(int on);

/// 面板上是不是还留着中灰（上一帧是灰阶画面，那一笔白底账还没清）。
/// 只在"要不要把这一帧收窄成区域刷"的判据里读（screen_reader.cpp 列表帧那条路）：
/// 白底重锚必须整屏做，收窄会把那次 from-white 挤掉，让中灰接着当差分基准。
/// 只看不取 —— 消费仍然是 display.c 里差分的那个 take。
int reader_white_exit_pending(void);

/// 灰阶自检页的自推屏：把当前 framebuffer 用指定的那条刷法推上去，返回耗时 ms。
/// 阅读器 TU 不碰波形/模式类型，所以这一层由 display.c 提供。
/// which: 0=整屏 GC16(默认表) 1=8 灰阶表整屏 2=from-white 16 灰
///        3=8 灰阶表 from-white 4=局部 DU(上半屏)
/// 返回 -1 表示参数不认识。
int reader_refresh_test_present(int which);

/// 最近一次扫描因「前导整屏保持相」被跳过的相数（epdiy 的 epd_last_leading_skip）。
/// 差分刷（GL16/DU 这类 MODE_*_DIFFERENCE 的档）才可能有；整屏全像素档恒为 0。
/// 这个开关（epd_set_leading_skip）由板级 bring-up（read_pico_init）开，本仓库不开它 ——
/// 这个访问器只是把它**看得见**：翻页那一行日志里的「跳相 N」就是它，N 长期为 0 说明
/// 这一档走在全像素路径上（那才是"该不该为它折腾"的判断依据）。
int reader_leading_skip(void);

#ifdef __cplusplus
}
#endif
