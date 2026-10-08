#pragma once

// 自适应屏幕方向（写作/计划模式的方向设置里那一档「自适应」）。
//
// 判据本身在 auto_orient_core.h（零依赖的纯函数，主机上跑 tests/host/orient/）；这里
// 只做接线，且**判定与落地分在两处**：
//
//   · 判定在采样钩子里：hw/input.cpp 每 120ms 读一次三轴，顺手喂进来，只更新状态机。
//     采样与"晃动机身=全刷"共用同一次 I2C 读，没有额外开销。
//   · 落地只在主循环里：转一次屏 = ui_render_drain() + 整屏 GC16（约 1.8s）。这条不能
//     从"空转等键时补采样触摸"那种上下文发出（input_poll 的两个分支都会走到采样），
//     所以 tick() 由 main.cpp 每轮调一次。
//   · "现在这个界面该不该跟"也由 main.cpp 每轮告诉它（写作/计划模式 + 该模式的方向
//     设置 = 自适应）。本模块不认识 AppState，也不读设置 —— 那样才好单独测。

#include <stdint.h>
#include <stdbool.h>

// 采样钩子（**设备帧**，毫克；芯片帧→设备帧的换算见 read_pico_init.h）。
// 只在允许跟随时才需要，但一直喂也无害：不在跟随时 tick 会把状态清掉。
void auto_orient_on_sample(int x_mg, int y_mg, int z_mg, int64_t now_us);

// 主循环每轮。active = 当前界面允许跟随（模式 + 设置）。
// last_input_us = 最近一次按键/触摸的时刻（0 = 很久没有）：打字/划列表期间不落地，
// 停手 AOC_INPUT_QUIET_MS 之后补上。
//
// 返回**这一拍真把屏幕转过去了**。转屏本身只动旋转值，屏上画面还得有人重画一帧
// —— 调用方（main.cpp）拿这个返回值去做那件事：候选行按新宽度重分页，以及把
// "编辑器这一屏画过了"那笔账作废（它的空转 tick 按 drawnOnce 记账跳过重绘，
// 方向换了它不会自己知道）。
bool auto_orient_tick(bool active, int64_t last_input_us);

// 模式进入、第一帧排版之前问一次：有新鲜采样且判得出，就填 *portrait 并返回 true
// （调用方据此 board_force_portrait/landscape）；判不出或采样太旧返回 false，调用方
// 保持原样（跟随全局方向）。这是"进模式时方向要在第一帧之前定下来"那条约定要的。
bool auto_orient_initial(bool *portrait);
