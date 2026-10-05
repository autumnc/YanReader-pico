#pragma once

#include "pjournal_app.h"

// 进入 / 离开计划模式（main.cpp 的 kScreens 生命周期钩子）。**只在本模式本次开机
// 第一次进入时**跑 screen_gtd_init()（它会把当前 tab/项目下钻/光标清成收集箱首页），
// 之后每次重进只补一次方向 —— 这条"故意不清"的规则跟着这两个函数一起住在屏幕自己的
// 文件里，不再散在 main.cpp 的注释里。
void screen_gtd_enter(ScreenContext &ctx);
void screen_gtd_leave(AppState next);

void screen_gtd_init();
// 退出计划模式：把方向还给全局设置（本模式可以有自己的方向，见 .cpp 的说明）。
void screen_gtd_exit();
// 重新套用本模式的方向设置。**每次进入计划模式都要调**（screen_gtd_exit 把方向还给
// 全局了），但 screen_gtd_init() 只在本模式本次开机第一次进入时才跑——它会把当前
// tab/项目下钻/光标清成收集箱首页（见 .cpp 的说明）。切模式回来时只调本函数。
void screen_gtd_apply_orientation();
AppState screen_gtd_handle(int key, ScreenContext &ctx);
// 返回是否处于 GTD 列表浏览模式(该模式下物理按键导航快捷键生效)
bool screen_gtd_accept_physical_buttons();
// 是否处于 GTD 项目标签的项目列表视图(项目树顶层)
bool screen_gtd_in_project_list();
// 虚拟键盘此刻是否占着屏幕下半部分(输入浮层 + 键盘已展开)。main 用它决定刷屏策略:
// 键盘开着时走 DU 局刷,收起后恢复常规策略。
bool screen_gtd_vk_up();
// 是否处于"要打字"的模式（新建/重命名/编辑字段/写笔记/筛选…）。main 用它判断
// 此刻的输入是打字：配合 editorVkVisible() 决定要不要开打字极速刷新。
bool screen_gtd_typing_mode();
// 物理按键双击 BOOT: 项目树内返回项目选择菜单; 其他视图无动作
void screen_gtd_physical_double_boot();
