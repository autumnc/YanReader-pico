#pragma once

#include "pjournal_app.h"

// Bluetooth management screen entry points
void screen_bt_manage_init();
AppState screen_bt_manage_handle(int key, ScreenContext &ctx);
bool screen_bt_manage_scan_mode();  // true 时面板处于扫描(添加设备)模式
