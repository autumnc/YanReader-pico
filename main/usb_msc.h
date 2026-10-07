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

// 退出已被接受、开始卸载 MSC 并重挂 SD 时的回调：这一趟要卸 USB 设备栈、删存储、
// 收裸卡、重挂 FATFS 再轮询等就绪（秒级），期间屏幕上若还停着"退出 U 盘模式"那一页，
// 用户根本分不清到底退没退。上层借此画一屏"正在退出…"（不要再画按得动的按钮）。
// 传 NULL 表示不需要视觉反馈。
typedef void (*usb_msc_exiting_cb_t)(void);

// 进入「U 盘模式」：先把 SD 从应用侧卸载（read_pico_sd_sync），再用一张裸 SDMMC 卡
// 重新接管外设，经 TinyUSB MSC 把整卡暴露给电脑；阻塞直到 should_exit(key) 判定退出、
// 且主机已安全弹出（或已断开），随后卸载 MSC 并 read_pico_sd_remount() 重挂，最后
// 轮询等挂载就绪。
//
// 调用前必须已关闭所有 SD 文件句柄（书对象/进度/笔记都在 screen_reader_exit 里释放过了），
// **但字体面不在其中**：exit 做的恰恰是"按设置把 SD 上的用户字体重新打开"，所以调用方
// 必须在进来之前 ttf_font_suspend_sd(true)（切内建、关掉 SD 上的 fd），退出之后
// ttf_font_suspend_sd(false) 再 applyUserContentFont() 重开 —— 否则重挂后那套句柄指向
// 的是已卸载的 FATFS 实例，整机会一直掉字（见 screen_reader.cpp 的 MenuAct::UsbDrive）。
// 退出后 /sdcard 已重新挂载，可直接重新扫书架。
//
// / Enter U-disk mode: unmount the SD from the app, re-init a raw SDMMC card and
// expose it to the host over TinyUSB MSC; block until should_exit(key) reports exit
// AND the host has safely ejected (or disconnected), then tear down MSC and remount
// the SD (polling until ready). Book/progress handles are released by the caller;
// **font faces are not** — the caller must suspend them around this call
// (ttf_font_suspend_sd) and reopen them after the remount.
//
// 返回：
//   ESP_OK             完整走完（MSC 初始化成功 → 用户确认退出 → 已重挂）
//   ESP_ERR_NOT_FOUND  没插卡 / 卡探测失败，未进入 U 盘模式
//   其它               卡就绪但 MSC 初始化失败（已回滚重挂）
esp_err_t usb_msc_run(usb_msc_should_exit_cb_t should_exit, usb_msc_blocked_cb_t on_blocked,
                      usb_msc_exiting_cb_t on_exiting);

#ifdef __cplusplus
}
#endif
