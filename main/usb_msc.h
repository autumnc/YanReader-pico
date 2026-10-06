#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 某个按键是否应触发退出（由上层做命中测试，例如只认"退出"按钮的落点，避免误触
// 任意键就退出）。返回 true 才走退出流程。
typedef bool (*usb_msc_should_exit_cb_t)(int key);

// 用户按下退出键、但主机仍未安全弹出介质时的回调：上层借此重画"请先在电脑上弹出"的
// 警告。传 NULL 表示不需要视觉反馈（仍会拒绝退出）。
typedef void (*usb_msc_blocked_cb_t)(void);

// 进入「U 盘模式」：先把 SD 从应用侧卸载（read_pico_sd_sync），再用一张裸 SDMMC 卡
// 重新接管外设，经 TinyUSB MSC 把整卡暴露给电脑；阻塞直到 should_exit(key) 判定退出、
// 且主机已安全弹出（或已断开），随后卸载 MSC 并 read_pico_sd_remount() 重挂，最后
// 轮询等挂载就绪。
//
// 调用前必须已关闭所有 SD 文件句柄（书对象/字体/进度/笔记都在 screen_reader_exit
// 里释放过了）。退出后 /sdcard 已重新挂载，可直接重新扫书架。
//
// / Enter U-disk mode: unmount the SD from the app, re-init a raw SDMMC card and
// expose it to the host over TinyUSB MSC; block until should_exit(key) reports exit
// AND the host has safely ejected (or disconnected), then tear down MSC and remount
// the SD (polling until ready). All SD file handles must be closed before calling
// (screen_reader_exit already released the book/font/progress).
//
// 返回：
//   ESP_OK             完整走完（MSC 初始化成功 → 用户确认退出 → 已重挂）
//   ESP_ERR_NOT_FOUND  没插卡 / 卡探测失败，未进入 U 盘模式
//   其它               卡就绪但 MSC 初始化失败（已回滚重挂）
esp_err_t usb_msc_run(usb_msc_should_exit_cb_t should_exit, usb_msc_blocked_cb_t on_blocked);

#ifdef __cplusplus
}
#endif
