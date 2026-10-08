#pragma once

// 板级**应用接口**：初始化、屏幕方向、夜间反色、u8g2 绘制句柄。
//
// 故意保持 epdiy 无关 —— 这里不 include epd_highlevel.h 之类，任何 UI 模块
// 都能放心 include 而不背上整套驱动依赖。要摸硬件的（推屏、触摸、加速度计）
// 请 include "board_hw.h"。

#include <stdint.h>
#include <stdbool.h>

#include "u8g2_shim.h"  // u8g2_t + g_u8g2（shim 句柄）

// 板级 bring-up：read_pico_init + 上电 + 清屏 + 清白。失败返回 false。
bool board_init();

// 应用方向设置（"landscape" 默认 | "portrait"）。改旋转后由调用方整屏重刷。
void board_apply_orientation(const char *orientation);

// ── 模式独立的临时方向切换（阅读模式 / 计划模式各有自己的一份方向设置）─────
// 阅读模式要横屏才能拿到 1216×684 的版面，计划模式同理；退出时恢复用户设置的方向。
// 以前这三条放在单独的 hw/board_reader.h 里，理由是"screen_reader.cpp 不能 include
// board.h"——P1.1 把 board.h 拆成"不含 epdiy"之后这条理由消失了（本头现在只有
// u8g2_shim.h + stdint/stdbool），于是合并回来，少一个裸声明头。
//
// 进入阅读模式：强制横屏（EPD_ROT_LANDSCAPE），使 HalDisplay 返回 1216×684。
void board_force_landscape(void);

// 阅读器内切换到竖屏（EPD_ROT_INVERTED_PORTRAIT），使 HalDisplay 返回 684×1216。
void board_force_portrait(void);

// 退出阅读模式：恢复最近一次 board_apply_orientation 应用的方向设置。
void board_restore_orientation(void);

// ── 自适应方向（写作/计划模式的「自适应」档）─────────────────────────────
// 当前是不是竖屏。按旋转后的逻辑宽高现算（684×1216 即竖屏），不维护影子变量 ——
// 谁在什么时候把旋转改过都能问出真值。
bool board_is_portrait(void);

// 运行中临时转屏：与 board_apply_orientation 同一套时序 —— 先 ui_render_drain() 等
// core1 把在飞的推屏收尾（否则"按旧旋转算的矩形、按新旋转送进驱动"会刷错地方），
// 再改旋转；调用方随后 ui_invalidate_snapshot()，下一帧整屏 GC16 把画面拉正。
// **不写 s_orientation**：那是"全局方向"，模式退出时要还原的目标，自适应中途转的屏
// 不能把它改掉。已经是这一档就直接返回（连 drain 都不做）。
void board_rotate_live(bool portrait);

// 全设备夜间反色：翻 HalDisplay 的全局反色标志，推屏唯一出口据此逐帧取反。
// 开机时按设置调一次；用户在设置里切换时由 screen_settings 直接调。
void board_set_night(bool on);
