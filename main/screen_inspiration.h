#pragma once

#include "pjournal_app.h"

/// 灵感库（/sdcard/outline/inspiration.json）：列表 / 关键词编辑 / 检索 / 帮助。
/// 入口原来声明在 pjournal_app.h（那是"还留在 pjournal_app.cpp 里的屏"才该待的地方），
/// 但这个屏的实现早就在 screen_inspiration.cpp 了，所以跟其它屏一样收到自己的头里。
void screen_inspiration_init(AppState returnTo, AppState editorReturnTo);
AppState screen_inspiration_handle(int key, ScreenContext &ctx);
