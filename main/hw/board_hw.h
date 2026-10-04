#pragma once

// 板级**硬件句柄**：epdiy 的高层状态机、触摸、加速度计、PMU。
//
// 跟 board.h 的区别只有一个，但很要紧：这个头会拖进 read_pico_init.h →
// epd_highlevel.h / cst836u.h / sc7a20h.h 一整套驱动。谁 include 它，谁的编译
// 依赖里就多一整套硬件驱动。
//
// 所以只有两类文件该 include：
//   · hw/ 自己（board.cpp 定义这些句柄，input.cpp 摸触摸和加速度计）；
//   · 推屏路径（ui_render.cpp）—— 它本来就在直接操作 epdiy 帧缓冲。
// 普通 UI 模块（各 screen_*.cpp）要画图请 include "u8g2_shim.h"，要板级开关请
// include "board.h"；这两个都不带驱动。

#include "read_pico_init.h"  // read_pico_handle_t（内含 epd_highlevel.h）

// 板级句柄（定义于 hw/board.cpp）：触摸 / 加速度计 / PMU 就绪标志。
// 注意 g_hw.hl 只在 board.cpp 内部用；推屏要拿 epdiy 句柄请用下面的 board_hl()。
extern read_pico_handle_t g_hw;

// epdiy 高层状态（波形 + front/back 帧缓冲）。推屏路径的唯一入口。
EpdiyHighlevelState *board_hl();
