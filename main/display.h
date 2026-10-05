/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 墨水屏刷新、HV 轨空闲超时、pclk 回退。
 *
 * EPD present, HV-rail idle timeout, and pclk fallback.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "epd_highlevel.h"
#include "epdiy.h"
#include "read_pico_epd_timing.h"

#ifdef __cplusplus
extern "C" {
#endif

// LCD 像素时钟：只决定有效像素段占一行的多少，剩下的由行结束段补足——行周期被锁在
// 波形标定的帧周期上，所以调 pclk 不会让刷新变快，只影响 DMA 的供数余量。12MHz 是
// epdiy 的保守默认值，18MHz 实测稳定（前提是 PSRAM 跑在 120MHz，80MHz 下 16MHz
// 就喂不满 DMA），开机锁 18MHz 留足余量。
// LCD pixel clock: it only sets how much of the line the active pixels occupy;
// the line-end pad fills the rest. The line period is locked to the waveform
// frame time, so pclk does not make refresh faster — it only changes DMA slack.
// 12 MHz is epdiy's conservative default; 18 MHz is stable here when PSRAM runs
// at 120 MHz (at 80 MHz even 16 MHz starves DMA). Boot locks 18 MHz for margin.
#define DISPLAY_PCLK_DEFAULT_MHZ 18
// 出现供数不足（EPD_DRAW_EMPTY_LINE_QUEUE）时退回这个确定安全的频率。
// Fall back to this known-safe clock on underrun (EPD_DRAW_EMPTY_LINE_QUEUE).
#define DISPLAY_PCLK_SAFE_MHZ READ_PICO_EPD_PCLK_MIN_MHZ
// 工程页上还能继续往上试。行消隐与 CKV 宽度会跟着频率重解。
// The lab page can still step higher. Line blanking and CKV width re-solve with the clock.
#define DISPLAY_PCLK_MIN_MHZ READ_PICO_EPD_PCLK_MIN_MHZ
#define DISPLAY_PCLK_MAX_MHZ READ_PICO_EPD_PCLK_MAX_MHZ
#define DISPLAY_PCLK_STEP_MHZ 1

void rails_keepalive(void);
void rails_idle_check(int64_t now_ms);

/// 全设备夜间反色开关。反色的实施点在 display.c 的推屏处（front/back 成对取反），
/// **不在** HalDisplay —— 后者只覆盖阅读器一族，其余界面由渲染任务直呼本文件，
/// 从那里走（见 display.c 顶部说明）。由 board_set_night() 统一转发。
/// / Global night-inversion switch. Applied inside display.c's present sites, not in
/// HalDisplay (which only the reader family goes through).
void display_set_night(bool on);

/// 大量文件I/O期间增加扫描预填，调用方离开时恢复。/ Increase scan prefill during bulk file I/O; caller restores on exit.
void display_set_bulk_io(bool active);

enum EpdDrawError update_display_mode(EpdiyHighlevelState* hl, enum EpdDrawMode mode);
enum EpdDrawError update_display_from_white(EpdiyHighlevelState* hl);
enum EpdDrawError update_display_from_white_with(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode
);
/// 把前缓冲铺白再 GC16 全刷，物理屏回到白底。
/// Paint the front buffer white and GC16 the panel back to white.
enum EpdDrawError update_display_white(EpdiyHighlevelState* hl);
enum EpdDrawError update_display_full(EpdiyHighlevelState* hl);
/// 8 灰阶全屏全像素刷：换 30 相 8-Gray 表整屏过一次 GC16。相位少近一半（整屏约
/// 360ms vs 48 相 GC16），代价是灰阶从 16 级降到 8 级（文字仍是黑白，看不出来）。
/// 用来做"比局刷干净、比 GC16 快"的中间档——现有波形里最接近 Regal 体感的一档。
/// / 8-gray full-pixel GC16 pass with the 30-phase table: roughly half the phases of
/// the 48-phase GC16 (about 360 ms full screen) at 8 gray levels instead of 16.
enum EpdDrawError update_display_gray8(EpdiyHighlevelState* hl);
/// 8 灰阶正文刷：换 30 相 8-Gray 表的 GL16 差分（表在开机补了 15→15 的白推）。
/// 不变的白像素不驱动 → 不闪，比默认 37 相 GL16 每屏快约 80ms。正文翻页走这条。
/// / 8-gray text pass: 30-phase GL16 differential with the boot-added 15→15 white
/// tick. Unchanged white is not driven (no flash) and it is ~80 ms faster per
/// screen than the default 37-phase GL16. Used for text page turns.
enum EpdDrawError update_display_gray8_text(EpdiyHighlevelState* hl);
/// 阅读模式的统一推屏出口。kind 就是 HalDisplay::RefreshMode 的枚举序
/// (FULL/HALF/FAST/GRAY8/GRAY8_TEXT)。存在的理由：crossmux 的
/// HalDisplay::displayBuffer() 把 HALF/FAST 直接交给 epd_hl_update_screen()，
/// 绕过了 hl_update() 里的"GL16 必须全像素"和"软刷攒够升 GC16"——官方固件防残影
/// 的机制 1/2 就是在那两个档位上漏掉的。收口到本函数后四个档位重新走同一条路，
/// 并且欠载会被 guard_draw_result 兜住（以前没人看返回值，一欠载就半屏花且不恢复）。
/// / Unified present path for reader mode. kind is HalDisplay::RefreshMode's enum
/// order. crossmux's displayBuffer() sends HALF/FAST straight to
/// epd_hl_update_screen(), bypassing hl_update()'s GL16-full-pixel rule and the
/// soft-refresh → GC16 promotion — where official firmware's anti-ghost
/// mechanisms 1 and 2 were dropped. Funnelling all four tiers back through here
/// restores both, and guard_draw_result now catches underrun (the return value
/// used to be discarded, so an underrun left half a garbled screen forever).
enum EpdDrawError update_display_reader(EpdiyHighlevelState* hl, int kind);

enum EpdDrawError update_display_with(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode
);
/// 灰阶图还在屏上时置位：菜单盖上来或离页先刷白，避免从中间灰差分。
/// Set while a gray image is still on panel: wipe to white before the menu or leave so the next update is not a mid-gray differential.
void display_hold_white_exit(bool hold);
bool display_take_white_exit(void);
enum EpdDrawError update_display_area_with(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode,
    EpdRect area
);

/// 当前像素时钟。/ Current pixel clock.
int display_pclk_mhz(void);
/// 出现供数不足就退回安全频率并整屏重刷，其它错误码原样忽略。
/// On underrun, drop to the safe clock and full-refresh; other error bits are ignored.
void guard_draw_result(EpdiyHighlevelState* hl, enum EpdDrawError result);

/// 软刷预算（残影计数）的读写口。整页差分刷攒够 APP_GC16_EVERY 次要升一次全像素
/// GC16 —— 这份预算由 display.c 独有，UI 那条路（ui_render.cpp 的全屏推送）问它
/// 要不要升、升完通知它归零，两边就不再各记一份。见 display.c 里的说明。
/// / Shared soft-refresh budget: ui_render's full-screen present asks display.c whether
/// this refresh should be promoted to GC16 and resets the budget after a GC16.
bool display_soft_refresh_due(void);
void display_soft_refresh_reset(void);

#ifdef __cplusplus
}
#endif
