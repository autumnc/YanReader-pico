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

// 全设备夜间反色：翻 HalDisplay 的全局反色标志，推屏唯一出口据此逐帧取反。
// 开机时按设置调一次；用户在设置里切换时由 screen_settings 直接调。
void board_set_night(bool on);
