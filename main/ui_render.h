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

// 输入法条（编码行 + 候选行）画在屏幕上的矩形，由绘制方（ui_helpers 的 drawIMEUI）
// 在画的时候报上来。上屏后要用全像素刷把这两行的残影清一遍，而**那一刻组合已经结束、
// 面板这一帧根本没画**（g_ime.composing() 已是 false，从 IME 状态反推不出来），
// 所以只能靠"上一次画在哪儿"。逻辑坐标，y 是面板上沿（编码行顶），不是基线。
// 清的那一下**推迟到打字停顿**（距离最后一次输入法动作 IME_CLEAN_PAUSE_US），
// 不是上屏当场做 —— 清一遍是整屏 30 相全像素，每词一次会卡。**句读是例外**：敲完
// ，。！？ 之后输入法条自己就变空了，那一拍当场清、不推迟。见 ui_render.cpp。
void ui_render_note_ime_panel(int x, int y, int w, int h);

// 申报"输入法面板顶线（提交时算出来的 ime_top）**以上**一个像素都没动"。给**编辑器组合期
// 只重画输入法条**那条路用：它以上一帧的像素打底、只补了 drawIMEUI 那一块，渲染任务据此
// 只需要扫面板顶线**以下**那一段（约 26% → ~3ms，整屏是 28ms）。
//
// 这条不变量是**结构性**的：任何差分够到面板顶线以上的帧都不满足推迟条件
// （`d.y >= job.ime_top`），会当场驱动、不进合并窗口，也就不会被丢弃；被丢弃的帧里留下的、
// 没驱动的差异只可能在面板顶线以下。所以"线以上是空的"对推迟/丢弃帧恒成立。
//
// 调用方必须真的如此（正文/标题/顶栏一个都没碰），且这一帧是**以上一帧逐位打底**开的
// （ui_render_begin_frame_seeded 返回 true）—— 只有这条路才可能"只画了线以下"。渲染任务侧
// 每 8 拍抽检一次线上那段真差分（第 1 拍就查），发现申诉不实就告警并停快路 8 拍。
// 见 ui_render.cpp 的 render_present。
void ui_render_note_ime_above_clean(void);

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

// 同上的"接着上一帧画"，但底取 **core0 自己最后画过的那一帧**，不是渲染任务最后
// 认领的那一帧 —— 提交完到认领之间差一帧，拿错底会把旧正文铺回去（编辑器打字用）。
// 返回 true = 已开帧且底已铺好；false = **没开帧**，调用方必须退回 ui_clear() + 整屏重画。
// 判 false 的情形：双缓冲没起来、已经开着帧、还没画过任何一帧、取不到缓冲。
bool ui_render_begin_frame_seeded(void);

// 把"屏上现在的画面"留一份副本（占住一块工作缓冲），供 ui_render_restore_kept() 推回。
// 待机时钟/休眠前调一次。重复调用只有第一次生效。
void ui_render_keep_frame(void);

// 把保留帧按 from-white 重推上屏（唤醒、待机预览结束），推完才返回。
// 没保留过则退化成 ui_render_restore()。
void ui_render_restore_kept(void);

void ui_render_set_fast_partial(bool enable);
void ui_render_set_local_only(bool enable);
// 菜单型界面（设置各页 / 写作·计划主界面 / 清单）：一律不记残影账，见实现里的说明。
// 只在"这一屏不是打字界面"时置位（main.cpp 按 kScreens 的 local_only 列 + 计划模式浏览态算）。
void ui_render_set_menu_only(bool enable);

// 阅读模式**虚拟键盘打字帧**的区域推屏。注册进 crossmux 的 vk_present 钩子（见 main.cpp
// 与 crossmux_platform.h），由 HalDisplay::displayBufferVk 调用。panel_top = 键盘面板顶边
// （= 编码/候选两行上沿），cand_h = 那两行的总高，都是逻辑像素。
// 阅读器整条绘制路径绕过渲染任务（直画 front_fb、自己同步推屏），所以这条不能复用
// render_present 的作业队列，只能把同一套差分/波形判据重走一遍——实现在 ui_render.cpp。
void reader_vk_present(int panel_top, int cand_h);

// 阅读器**列表/菜单帧**（书架、目录、书签、设置…凡不是正文页的那些）的区域推屏。
// 矩形由本帧 front_fb 与 back_fb 的差分包围盒定（不需要渲染函数申报区域），因此滚动
// 列表不再整屏全像素驱动、也不吃那份共享残影预算（见 ui_render.cpp 的实现说明）。
// 调用方（screen_reader.cpp 的推屏收尾）判两件事才走这里：档位是 HALF、且不是
// RdMode::Reading；另外面板留着中灰时不走（reader_white_exit_pending）。
void reader_list_present(void);

// 阅读器虚拟键盘的"清残影欠账"：**口径与编辑器那套一一对齐**（设置项 ime_clean，
// 见 ui_render.cpp 里 reader_vk_present 上面那张表）。快档（上屏刷法=快）欠下的正文那块
// （跟随 DU 推的、发灰又带残影的）在停手 IME_CLEAN_PAUSE_US 之后由阅读器自己的帧循环
// （core0，每轮都调，空转帧也算）做一次区域 GC16 坐实。四档都只是**记账的时机**不同
// （句读后 / 上屏后 / 计数清 / 从不清），真正那一次区域 GC16 一律在这里做
// —— 2026-10-09 起句读档也不再当场清（当场清要并上本帧差分，正文一重排就是整块黑闪）。
// 整屏全刷过的那些帧（翻页全刷/换章/进界面首帧）调 forget 销账，免得停顿时再白闪一次。
// 两份账都在 core0，和渲染任务里那套 ime_clean 互不相干（各记各的，不共用计数器）。
void ui_render_reader_vk_settle_tick(void);
void ui_render_reader_vk_settle_forget(void);

#ifdef __cplusplus
}
#endif
