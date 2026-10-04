/*
 * 双核分工：渲染任务 + 双工作缓冲 + 区域刷新策略。
 *
 * core0（app 主任务，见 main.cpp）只做：采样输入 → 状态机 → 往**当前工作缓冲**里画。
 * core1（本模块的 ui_render 任务）只做：差分 → 选波形/区域 → epdiy 推屏 → HV 轨下电。
 *
 * 为什么要拆：刷屏是纯 RTOS 阻塞（epdiy 每个波形相位等一次 frame_done 信号量），
 * 整幅 GC16 一次把调用者按住约 400ms，这期间主循环不采样触摸、不排 BLE 按键队列、
 * 不服务 WiFi 回调。搬核省的是**阻塞**不是算力（epdiy 自己已经把行打包摊到两个核），
 * 所以别指望"整屏刷得更快"，收益是 core0 在那几十到几百毫秒里还活着。
 *
 * 帧缓冲：两块 PSRAM 工作缓冲轮流用，绘制目标靠 u8g2_set_fb() 重指。
 * 差分基准直接用 epdiy 的 back_fb —— 它是"上一次真正驱动到面板上的内容"，
 * 本来就是我们要比的对象，白拿一块 400KB。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 建渲染任务（core1, prio 6, 12KB 栈走 PSRAM）、双工作缓冲、作业队列。
// 必须在 board_init() 之后、任何 ui_clear() 之前调用一次。
void ui_render_init(void);

// 开一帧：取一块空闲工作缓冲并把 u8g2 的绘制目标重指过去，返回该缓冲。
// 两块都被占（推屏追不上绘制）时会阻塞 —— 这是唯一的背压点，最多等两帧的余量。
// 同一帧内重复调用返回同一块（幂等）。
uint8_t *ui_render_begin_frame(void);

// 提交当前帧：交给渲染任务推屏，**不等待**。core0 立刻可以接着画下一帧。
void ui_render_submit(bool force_full);

// 等已提交的帧全部推完。切模式、休眠、进阅读器前用。
void ui_render_drain(void);

// 丢弃参考帧：下一帧无条件整屏 GC16。
void ui_render_invalidate(void);

// 立即整屏 GC16，推完才返回。不重绘——推的就是当前缓冲里的画面。
void ui_render_full_refresh(void);

// 休眠唤醒：面板刚被物理清成白底，把当前缓冲里的画面用 from-white 重推一遍
// （只重置旧帧基准，不把画面变白）。推完才返回。
void ui_render_restore(void);

// 把上一帧内容复制进一块空闲缓冲并设为绘制目标（在"当前画面"上叠加绘制用）。
// 用于那些"只补画一小块、不整屏重画"的路径：帮助页/选择器/菜单浮层/待机提示。
// 双缓冲下必须显式取回上一帧 —— 那是它们以前依赖同一块缓冲的隐含前提。
void ui_render_begin_overlay(void);

// 把"屏上现在的画面"留一份副本（占住一块工作缓冲），供 ui_render_restore_kept() 推回。
// 待机时钟/休眠前调一次。重复调用只有第一次生效。
void ui_render_keep_frame(void);

// 把保留帧按 from-white 重推上屏（唤醒、待机预览结束），推完才返回。
// 没保留过则退化成 ui_render_restore()。
void ui_render_restore_kept(void);

void ui_render_set_fast_partial(bool enable);
void ui_render_set_local_only(bool enable);

#ifdef __cplusplus
}
#endif
