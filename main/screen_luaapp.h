#pragma once
//
// 运行 CrossStore 的 Lua 应用（写作模式栈上的顶层界面）。
//
// 包格式、清单校验、沙箱都在 crossstore/ 下；这里实现 Host：画布/列表两种版式、
// 按键与触摸分发、appdata、系统服务（时间/电量/阅读统计/网络对时/导出 SVG）。
// 一个应用同时只跑一个；离开时先 on_stop，再销毁 lua_State。

#include "pjournal_app.h"

// 商店详情页点"打开"时先投递 id，再把 AppState 切到 APP_LUA_APP；
// screen_luaapp_enter 取走它并从 /.crosspoint/apps/<id>.csp 加载。
void screen_luaapp_request(const char *id);

void screen_luaapp_enter(ScreenContext &ctx);
AppState screen_luaapp_handle(int key, ScreenContext &ctx);
void screen_luaapp_leave(AppState next);
