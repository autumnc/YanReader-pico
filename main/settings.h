/*
 * pjournal-pico 字体设置桩：只为 ttf_font.c 提供外置字体路径的读写。
 * read_pico 原 settings.h 还带睡眠档/唤醒原因等，pjournal 用不到，
 * 这里只保留字体路径两个函数，语义与原版一致：空路径 = 固件内建字体。
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 空路径表示固件内建字体；非空为 SD 上的 TTF。/ Empty path is the built-in font; non-empty is a TTF on the SD card.
const char* app_settings_font_path(void);
void app_settings_set_font_path(const char* path);

/// 从 NVS 读回外置字体路径（需在 SD 挂载后、字体初始化前调用）。
/// / Restore the external font path from NVS (call after SD mount, before font init).
void settings_init(void);

#ifdef __cplusplus
}
#endif
