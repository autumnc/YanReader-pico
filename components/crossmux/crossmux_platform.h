#pragma once
// 阅读模式对外接口：main 在 board_init 后注入 epdiy 句柄。
#include "epd_highlevel.h"

#ifdef __cplusplus
extern "C" {
#endif

// 把 epdiy 高层句柄交给阅读模式的显示 HAL。main 在 board_init 后调用一次。
void crossmux_platform_set_display(EpdiyHighlevelState* hl);

// FULL_REFRESH 的落地点由 main 提供：epd_hl_update_screen() 是**差分刷**，只在像素
// 与上一帧不同时才驱动它们；画面没变（菜单里任何一页不动、长按"全局刷新"）时差分
// 图为空，它会直接什么都不做。main 的 update_display_full() 会强制整屏全像素过一次
// LUT，并顺带处理扫描时序/预填/HV 轨保活。
// 未注册时退回 epd_hl_update_screen()。
// main provides the FULL_REFRESH landing point: epd_hl_update_screen() is differential
// and no-ops when the frame is unchanged. update_display_full() forces all pixels
// through the LUT and also handles scan timing, DMA prefill and HV rails.
typedef enum EpdDrawError (*crossmux_full_refresh_fn)(EpdiyHighlevelState* hl);
void crossmux_platform_set_full_refresh(crossmux_full_refresh_fn fn);

// 8 灰阶全屏刷的落地点，同样由 main 提供（update_display_gray8）：走 30 相 8-Gray 表
// 整屏全像素过一次 GC16，作为"比局刷干净、比 16 灰阶 GC16 快"的中间档。
// 阅读器的"自适应"策略用它。未注册时退回 FULL_REFRESH 的路径。
// main provides the 8-gray landing point (update_display_gray8): a full-pixel GC16
// pass with the 30-phase 8-gray table, the middle tier between GL16 and GC16.
// Used by the reader's adaptive strategy. Falls back to FULL_REFRESH when unset.
void crossmux_platform_set_gray8_refresh(crossmux_full_refresh_fn fn);

// 8 灰阶"正文刷"的落地点（update_display_gray8_text）：30 相 8-Gray 表的 GL16 差分，
// 不变的白像素不驱动（不闪），比默认 37 相 GL16 每屏快约 80ms。正文翻页用。
// / Landing point for the 8-gray text pass (update_display_gray8_text): a 30-phase
// GL16 differential that leaves unchanged white undriven (no flash) and is
// ~80 ms faster per screen than the default 37-phase GL16.
void crossmux_platform_set_gray8_text_refresh(crossmux_full_refresh_fn fn);

// 所有刷新档位的统一出口（可选，优先级最高）。kind 就是 HalDisplay::RefreshMode 的
// 枚举值（0=FULL 1=HALF 2=FAST 3=GRAY8 4=GRAY8_TEXT），main 侧再映射到 epdiy 模式。
// 不注册时 displayBuffer() 按老路走：FULL/GRAY8/GRAY8_TEXT 用上面三个钩子，HALF/FAST
// 直接 epd_hl_update_screen()。
// **为什么需要它**：epd_hl_update_screen() 是光秃秃的差分刷 —— 它不做"GL16 必须全像素"，
// 也不计数（软刷攒够 14 次升 GC16 是 main/display.c 里 hl_update() 干的活）。中文正文
// 页翻页主要走 HALF，正好全落在这个缺口里：灰底压不掉、越翻越脏。注册本钩子后阅读器
// 的每一档都回到 display.c 的同一个出口，官方那套防残影机制才真正生效。
// / Unified landing point for every refresh tier (optional, highest priority).
// kind is HalDisplay::RefreshMode. When it is not registered, displayBuffer()
// keeps the legacy path. The reason it exists: epd_hl_update_screen() is a bare
// differential and lacks both the GL16-full-pixel rule and the soft-refresh
// counter that live in main/display.c's hl_update(). Text page turns (HALF) were
// exactly the ones falling through that gap, so the gray floor never cleared.
typedef enum EpdDrawError (*crossmux_mode_refresh_fn)(EpdiyHighlevelState* hl, int kind);
void crossmux_platform_set_mode_refresh(crossmux_mode_refresh_fn fn);

// 阅读模式**虚拟键盘打字帧**的区域推屏落地点（可选，main 提供）。参数是**逻辑像素**：
// panel_top 是键盘面板顶边（= 编码/候选两行上沿），cand_h 是那两行的总高。实现自己
// 取 epdiy 句柄（board_hl()），所以这里不带 hl —— 这个类型要放进阅读器也能 include
// 的头里，而阅读器碰不得 epdiy.h（EpdFont 名字冲突，见 screen_reader.cpp 开头）。
// **为什么需要它**：阅读器给虚拟键盘打字帧走的是 HALF(局刷)，而 display.c 的
// hl_update() 有一条"GL16 恒全像素"的规矩 —— 于是每按一个键整屏全像素驱动一遍
// （≈410ms + 整屏闪一下）。区域刷只驱动与上一帧有差异的那块矩形，判据与波形挑选
// 在 ui_render.cpp 的 reader_vk_present 里，和写作模式的虚拟键盘是同一套。
// 未注册时 HalDisplay 退回 displayBuffer(HALF_REFRESH)（老行为）。
// / Landing point for the reader's virtual-keyboard typing frame (optional).
// All-in-area present: only the rect that differs from the previous frame is driven.
// panel_top/cand_h are logical pixels; the implementation grabs the epdiy handle itself
// because this type must live in a header the reader may include (it cannot touch
// epdiy.h). Without the hook HalDisplay falls back to a full-screen HALF refresh.
typedef void (*crossmux_vk_present_fn)(int panel_top, int cand_h);
void crossmux_platform_set_vk_present(crossmux_vk_present_fn fn);

#ifdef __cplusplus
}
#endif
