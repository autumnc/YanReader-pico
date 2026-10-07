/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 错相揭页引擎（从 wegooo-cell/read-pico-reader 的 e0470_page_turn 移植）。
 * Staggered page-turn engine, ported from wegooo-cell/read-pico-reader.
 *
 * 原理：16 带、每像素走完一整条相位梯子。屏幕按揭页方向切成 16 条带，每条带依次
 * 入相——第 k 拍时条带 b 走第 (k-b) 相。整块差分只算一次，每拍只换对应相位的 1K LUT，
 * 于是视觉上就是一条"擦除波"从一侧扫到另一侧。
 *
 * 墙钟 ≈ 拍数 × 拍长，而拍数 = (16-1)·stride + 相数（见 .c 里 TURN_BAND_STRIDE 的推导），
 * 所以**梯子选多长就是快慢的唯一杠杆**：默认表 GL16 37 相 ≈ 1.06s（揭页唯一的梯），
 * 跟随表 DU 8 相 ≈ 0.29s（黑白两级，只给触摸笔迹）。用哪条梯子由调用方决定，
 * 见 e0470_page_turn_ex()。灰色揭页在这块屏上有 ~0.9s 的地板（见 .c 的实测表）。
 */

#pragma once

#include "epd_highlevel.h"
#include "epdiy.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 逻辑屏幕上的揭页方向；库内按当前旋转映射到 framebuffer。
/// / Logical direction, mapped through the current rotation inside the engine.
typedef enum {
    E0470_TURN_LTR = 0,
    E0470_TURN_RTL = 1,
    E0470_TURN_TTB = 2,
    E0470_TURN_BTT = 3,
} e0470_turn_dir_t;

/// 每拍默认时长（下限；单拍扫描本身约 7ms，更慢时以扫描为准）。
/// 拍长**自适应**：某一拍的扫描比当前下限慢就把下限抬到它 +0.2ms（上限 12ms），
/// 让"波"扫过去的速度是匀的。不抬的话，喂数最吃力的那几拍会拖长，看着就是卡顿。
/// 墙钟时间 ≈ 拍数 × 下限 —— 想快，换更短的梯子（见 e0470_page_turn_ex），不是调这里。
#define E0470_TURN_DEFAULT_TICK_US 7500

const char* e0470_turn_dir_name(e0470_turn_dir_t dir);

/// 每拍目标时长（下限，实际以单拍扫描耗时为准）。调小更跟手，但拍间可能追不上扫描。
void e0470_page_turn_set_tick_us(int us);
int e0470_page_turn_tick_us(void);
/// 离开阅读时释放按需分配的 37KiB 相位表（PSRAM）。
void e0470_page_turn_release(void);

/// `area` 是**逻辑**坐标；无可用 GL16 时返回 `EPD_DRAW_NO_PHASES_AVAILABLE`，不刷屏。
/// 调用方负责 FAST 扫描档位、预填行数、HV 轨保活与失败恢复。
/// 等价于 `e0470_page_turn_ex(..., &E0470_WAVEFORM, MODE_GL16)`（默认表 37 相长梯）。
enum EpdDrawError e0470_page_turn(EpdiyHighlevelState* hl, EpdRect area, e0470_turn_dir_t dir);

/// 同上，但**由调用方指定相位梯子**（`waveform` + `mode` 决定用哪张表）。
///   `&E0470_WAVEFORM, MODE_GL16`        37 相、16 灰阶 → 墙钟 ~1.06s ← **揭页用这条**
///   `&E0470_FOLLOW_WAVEFORM, MODE_DU`   8 相、黑白两级 → 墙钟 ~0.29s（只给触摸笔迹）
/// **揭页只有这一条梯（GL16）**：曾经还有一条给正文页的 8 灰阶短梯（30 相 @stride5 =
/// 105 拍 ≈ 0.88s，快 179ms），2026-10-07 删了 —— 分档的判据（调用方的"变化千分比 <
/// 300‰"）实测是个硬币：同一章普通正文页连续测出 217/257/272/291/333/353/354‰，**300
/// 正好切在分布正中间**，于是同一章里一半的页走一条梯。为 179ms 留着它会误判、还让图片页
/// 有落到 8 级灰（出带状）的风险，不划算。GL16 用 16 级灰，对所有页都对。
/// 揭页的"波"必须是**灰阶渐变**，落在眼睛里比黑白两级顺得多。只有 FOLLOW（8 相、黑白
/// 两级）是硬切 —— 它当初是给触摸笔迹跟手用的，**别拿它做翻页动画**：快约 4 倍，但看着
/// 是"跳"不是"淌"。
/// 别指望换梯子提速：每拍扫描时长是波形族/脏宽的属性，拍数 × 单拍扫描在 stride 之间基本
/// 守恒，灰色揭页在这块屏上就是 ~0.9–1.0s 的地板（见 .c 的实测表）。
/// stride 按梯子自动选（调用方不用管），喂数跟不上会自动调大并记在日志里。
enum EpdDrawError e0470_page_turn_ex(
    EpdiyHighlevelState* hl, EpdRect area, e0470_turn_dir_t dir, const EpdWaveform* waveform, int mode
);

#ifdef __cplusplus
}
#endif
