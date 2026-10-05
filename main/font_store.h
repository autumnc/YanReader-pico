/*
 * 外置字体路径的持久化（NVS 命名空间 "pjfont"）。
 *
 * 文件与 API 都叫 font_store_*：以前它叫 settings.c / app_settings_*，与
 * settings_manager.cpp（SD 卡 /sdcard/settings/ 那一套，22 个文件在用）同名不同物，
 * 读代码时极易混。这里只管一件事——用户选的字体路径；空路径 = 固件内建字体。
 * read_pico 原 settings.h 还带睡眠档/唤醒原因等，pjournal 用不到，没有移植。
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 空路径表示固件内建字体；非空为 SD 上的 TTF。/ Empty path is the built-in font; non-empty is a TTF on the SD card.
const char* font_store_get_path(void);
void font_store_set_path(const char* path);

/// 从 NVS 读回外置字体路径（需在 SD 挂载后、字体初始化前调用）。
/// / Restore the external font path from NVS (call after SD mount, before font init).
void font_store_init(void);

#ifdef __cplusplus
}
#endif
