#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "read_pico_init.h"
#include "epd_highlevel.h"

struct u8g2_struct;
typedef struct u8g2_struct u8g2_t;

// 板级全局句柄（定义于 hw/board.cpp）：hl / framebuffer / touch / sensor / pmu_ready。
extern read_pico_handle_t g_hw;
// u8g2 shim 句柄：g_u8g2->fb 指向 epdiy 4bpp framebuffer。
extern u8g2_t *g_u8g2;

// 板级 bring-up：read_pico_init + 上电 + 清屏 + 清白。失败返回 false。
bool board_init();

EpdiyHighlevelState *board_hl();

// 应用方向设置（"landscape" 默认 | "portrait"）。改旋转后由调用方整屏重刷。
void board_apply_orientation(const char *orientation);

// 全设备夜间反色：翻 HalDisplay 的全局反色标志，推屏唯一出口据此逐帧取反。
// 开机时按设置调一次；用户在设置里切换时由 screen_settings 直接调。
void board_set_night(bool on);
