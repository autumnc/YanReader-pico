#pragma once

// Flomo 笔记（写作模式主菜单入口）。移植自 ../Flomo/ —— 那个项目是个独立的 C1
// 客户端，这里把它并进写作模式：本地库 /sdcard/flomo/memos.json + 手动同步。
// 「新增/编辑」直接复用写作模式的文本编辑器（screen_editor）：先把正文写进
// journal 目录下的临时文件、从编辑器返回后再读回来（与灵感面板同一套手法，
// 见 screen_inspiration.cpp）。登录信息复用设置里的 flomo_email/flomo_pass。

#include "pjournal_app.h"

void screen_flomo_init(AppState returnTo);
AppState screen_flomo_handle(int key, ScreenContext &ctx);
