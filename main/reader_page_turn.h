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
/// `fast != 0` 用短梯（跟随表 DU 8 相，~0.29s），正文页设它；`fast == 0` 用长梯
/// （默认表 GL16 37 相，~0.73s，16 灰阶），图片页/版式大变留着它。
/// 提示**只对紧接着的那一次 update_display_reader 有效**，那边取走就清空。
void reader_hint_page_turn(int dir, int fast);

/// 释放揭页引擎按需分配的 37KB 相位表（PSRAM）。离开阅读模式时调。
/// 直接转调 e0470_page_turn_release()：这里再包一层只是为了让阅读器不必
/// include e0470_page_turn.h（那会拖进 epdiy.h，见文件头的说明）。
void reader_release_page_turn(void);

#ifdef __cplusplus
}
#endif
