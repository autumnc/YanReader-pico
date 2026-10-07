/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 阅读器 → 推屏层 的"这一帧请走错相揭页"提示。
 *
 * 单独一个头的原因是**不能**让 screen_reader.cpp 去 include display.h：display.h
 * 拖进 epdiy.h → epd_internals.h，里面 typedef 的 EpdFont 与 crossmux 的
 * EpdFont（class）在同一个 TU 里冲突（见 screen_reader.h 顶部的说明）。所以这里
 * 只放一个不依赖 epdiy 的裸声明，实现在 display.c。
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// 告诉推屏层"下一次刷新用错相揭页，方向 dir"。
/// dir 取 e0470_turn_dir_t 的值（0=LTR 1=RTL 2=TTB 3=BTT）；-1 = 取消待处理的揭页。
/// 揭页只有一条梯：默认表 GL16（37 相、16 灰阶，@stride6 = 127 拍 ≈ 1.06s）。
/// 曾经按"变化千分比 < 300‰"再分出一条正文短梯（8 灰阶 30 相，快 179ms），2026-10-07
/// 删了 —— 那个判据实测是个硬币，同一章正文页落在 217~354‰，300 正好切在中间。
/// 详见 e0470_page_turn_ex() 的注释。
/// 提示**只对紧接着的那一次 update_display_reader 有效**，那边取走就清空。
void reader_hint_page_turn(int dir);

/// 释放揭页引擎按需分配的 37KB 相位表（PSRAM）。离开阅读模式时调。
/// 直接转调 e0470_page_turn_release()：这里再包一层只是为了让阅读器不必
/// include e0470_page_turn.h（那会拖进 epdiy.h，见文件头的说明）。
void reader_release_page_turn(void);

/// 注册"揭页动画期间每 8 拍（≈60ms）调一次"的回调 —— 阅读器在这里注册 input_tick()。
/// 理由（2026-10-07 用户报的"点了要等一会儿才翻页、这期间怎么点都一样"）：一次翻页里
/// 重画(292~742ms) + 揭页动画(~1060ms) + 预热(126~321ms) 三段都是同步阻塞，
/// 中间只有两个采样点，而点按必须"按下"与"抬手"各被采到一次 —— 整段落在里面的点按
/// 一点痕迹都不留。动画是其中最大的一段，这里就是它的采样点。
/// 回调跑在调用推屏的那个任务里（阅读器整条绘制+推屏都在主循环那个任务，core0）。
void reader_page_turn_set_tick_hook(void (*hook)(void));

#ifdef __cplusplus
}
#endif
