#pragma once
// ── 界面契约（Screen）────────────────────────────────────────────────────
// 主循环原来是一个 18 个 case 的 switch：每个 case 自己记 `xxxInited`、自己决定空转
// 睡多久、自己在退出时收尾。三样东西散在 3 个文件里（main.cpp 的 switch、
// screen_xxx.cpp 的静态量、ui_render.cpp 的宿主名单），加一个屏要改的地方全靠记性，
// 漏一个就是"键盘僵在屏幕上"或"回来时草稿没了"。
//
// 这一层把"每个屏长什么样"变成一张表（main.cpp 的 kScreens），每屏一行：
//
//   · `handle`  —— 唯一的必需项：吃一个键（key==0 表示空转一拍）返回下一个界面。
//                  **空转路径不许自己睡**，睡眠由主循环按 `idle_ms` 统一做（=0 表示
//                  "本屏自己睡"，只有那几个自己做异步等待的屏在用）。
//   · `enter`   —— 每次**进入**本屏那一拍调一次（切到它的第一帧，或它要求重来那一帧）。
//   · `leave`   —— 每次**离开**本屏（切到别的屏）那一帧调一次。
//   · `idle_ms` —— 空转一拍的睡眠时长。
//   · `vk_host` —— 虚拟键盘宿主：离开这个界面要把虚键盘收起来。原来是 main.cpp 里
//                  一串手写的 `||`，漏一个的后果写在 ui_render.cpp 的注释里。
//   · `local_only` —— 这个界面一律局刷（没有抢时间的输入，画质优先）。
//
// ── 生命周期只有"进入"和"离开"两个事件 ──────────────────────────────────
// 框架只负责**什么时候叫**，"叫了做什么"由每个界面自己在 enter/leave 里定。这一版
// 起主循环不再有 `static bool xxxInited`（那 11 份标志正是"规则只活在注释里"的病根）。
// 两种常见的策略各自长这样：
//
//   · 每次进来都重建：enter 里直接调自己的 init。多数界面是这种（不需要 leave）。
//   · 跨进入存活：**故意不清**的那种（计划模式的 tab/下钻/光标必须留着，重跑 init
//     会把 tab 清回收件箱），在 enter 里用一个文件级静态挡住重跑 —— 于是"为什么故意
//     不清"这句话就写在那个静态旁边，而不是散在 main.cpp 的注释里。
//
// `enter` 里**不要**做"每帧都要钉一次"的准备动作（`g_font.setSize`、IME 的
// `setPageSize`）：那些是防御性的（设置页一改界面字号，别的屏必须立刻钉回来），留在
// 各自的 handle 包装里逐帧做。见 main.cpp 各 `scrXxx`。
#include "pjournal_app.h"  // AppState / ScreenContext

struct Screen {
    const char *name = "?";
    // 每次进入本屏那一拍调一次（可为 null）。
    void (*enter)(ScreenContext &ctx) = nullptr;
    // 吃一个键并返回下一个界面。key > 0 = 有键；key == 0 = 空转一拍。
    AppState (*handle)(int key, ScreenContext &ctx) = nullptr;
    // 每次离开本屏（切到别的界面）那一帧调一次（可为 null）。
    // 参数是"去哪儿"——极少数界面要按目的地决定收尾（编辑器见到灵感/润色/历史时
    // 刻意不重置，因为它只是被一层浮层盖住）。
    void (*leave)(AppState next) = nullptr;
    // 空转一拍的睡眠时长（ms）。0 = 本屏在自己的 handle 里睡（那几个异步等待的屏）。
    int idle_ms = 100;
    // 虚拟键盘宿主：离开这个界面时收起虚键盘。
    bool vk_host = false;
    // 一律局刷（差分常超半屏也不掉进整屏 GL16，避免翻页时整屏闪一下）。
    bool local_only = false;
};
