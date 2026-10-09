#pragma once

// ── 欠载探针：记录"刚才做过哪些会长时间关中断的重活" ──────────────────────────
//
// 为什么需要它：epdiy 的供数线程（每核一个、优先级 configMAX_PRIORITIES-1）是**最高
// 优先级**，普通界面任务再重也抢不走它的 CPU —— 能把它挡住的只有"别人关中断"这一件事
// （heap_caps_* 的 MULTI_HEAP_LOCK、SD 卡/文件系统事务那类）。它一旦被挡住 ~1ms，62 行的
// 行队列就被 DMA 抽干 → EPD_DRAW_EMPTY_LINE_QUEUE → guard_draw_result 整屏重刷，用户看到
// 的就是"打字打着一整屏闪一下"（2026-10-09 报的"大清屏"）。
//
// 已经用日志对上了：5 次欠载的 E 行全部落在某条 `Heap:` 行之后 1~2ms（那条每 2 秒一次的
// 堆水位日志要遍历 TLSF 空闲链，关中断 ms 级）。但靠"对时间戳猜"太慢，所以留这个环：
// 每个嫌疑重活开始/结束时记一笔（名字 + 起止时间），guard_draw_result 在欠载日志里把它们
// 全打出来 —— 下次再闪，日志直接写"当时正在写哪个文件 / 遍历堆用了多久"。
// / A small ring of "long, interrupt-disabling operations" (heap walks, atomic file
// writes) so an underrun's recovery log can name what was in flight just before it.
// The epdiy feeder runs at max priority, so only an interrupt-disabling stretch can
// starve it; this records those stretches instead of guessing from timestamps.

#ifdef __cplusplus
extern "C" {
#endif

// 重活开始/结束（严格成对；嵌套时 end 关掉**最近开始且还没结束**的那一条）。
// what 可以是文件全路径，打印时只取最后一段。
void longop_begin(const char *what);
void longop_end(void);

// 把最近 WINDOW 内（含还在进行中的）的重活打一条 WARN 日志，前缀用 why。
// 没有就明说"（无）"—— 那也是有信息量的一句：说明这次欠载跟重活无关。
void longop_dump(const char *why);

#ifdef __cplusplus
}
#endif
