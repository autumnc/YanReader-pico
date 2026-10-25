#pragma once
//
// CrossStore 应用商店（写作模式栈上的顶层界面）。
//
// 后端在 crossstore/store_package.*（目录/安装/卸载/权限/图标），沙箱在
// crossstore/CrossStoreRuntime.*（Lua 主机绑定）。本文件只做界面与状态机：
// 目录页 / 已安装页 / 应用详情 / 确认 / 下载进度 / 消息。
//
// 目录抓取、图标下载、安装都在后台任务里跑（AppAsyncJob），UI 线程只轮询结果；
// 安装进度由 net::stream 的进度回调写进原子量，界面每拍重画一次进度条。
// 网络不可用 / 没配 WiFi 时照常用缓存目录（离线可看已装应用）。

#include "pjournal_app.h"

void screen_appstore_enter(ScreenContext &ctx);
AppState screen_appstore_handle(int key, ScreenContext &ctx);
void screen_appstore_leave(AppState next);
