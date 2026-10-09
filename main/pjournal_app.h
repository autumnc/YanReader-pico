#pragma once

#include <string>
#include "ui_helpers.h"

// App state enumeration
enum AppState {
    APP_MAIN,
    APP_EDITOR,
    APP_BROWSER,
    APP_VIEWER,
    APP_HISTORY,
    APP_SETTINGS,
    APP_PROMPT_SEL,
    APP_SYNC_WEBDAV,
    APP_SYNC_SEND_FLOMO,
    APP_BT_MANAGE,
    APP_FILE_MANAGER,
    APP_GTD,
    APP_OUTLINE,
    APP_INSPIRATION,
    APP_POLISH,
    APP_POLISH_PROMPT,
    APP_FLOMO,
    APP_READER,
    APP_QUIT,
};

// 界面属于哪个模式：0=阅读 1=写作 2=计划。按界面归属判断而不是另存一个 mode 变量，
// 这样用户在模式内部乱走(从 GTD 退回写作菜单)也不会把模式标错。
// 放在头里（原来是 main.cpp 的 file-static）是因为 Screen 的 leave 钩子也要用：
// 编辑器判断"这次离开是不是切模式"（目的地跨了模式 → 保住会话，回来接着用），
// 见 screen_editor_leave。语义一字未改。
inline int appModeOfState(AppState s) {
    if (s == APP_READER) return 0;
    if (s == APP_GTD) return 2;
    return 1;
}

// Screen context passed between screens
struct ScreenContext {
    AppState nextState = APP_MAIN;
    std::string selectedEntry;    // for viewer
    // for editor：提示词。**编辑器不再有"模式"这个开关**——有没有提示词就是
    // 提示写作 / 自由写作（见 screen_editor 的 editorPromptOn）。非空的入口只有
    // 写作主菜单（现在是空的，进来先写）与将来的调用方；空串 = 自由写作。
    std::string promptText;
    std::string editorTitle;      // optional editor status title override
    std::string editContent;      // body text to load into editor (from browser)
    std::string editFilename;     // original filename when editing existing entry
    std::string statusMessage;    // one-shot status message to show
    int statusDuration = 0;       // ticks to show status message
    AppState prevState = APP_MAIN;
};

// Arrow key codes (must match bt_keyboard.cpp)
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83
#define KEY_IME_TOGGLE 0x84
#define KEY_CTRL_ENTER 0x85
#define KEY_SHIFT_UP    0x86
#define KEY_SHIFT_DOWN  0x87
#define KEY_SHIFT_LEFT  0x88
#define KEY_SHIFT_RIGHT 0x89
#define KEY_CTRL_I      0x8A
#define KEY_FULLWIDTH_TOGGLE 0x8B
#define KEY_TRAD_TOGGLE 0x8C
#define KEY_LSHIFT_TAP 0x8D
#define KEY_HOME       0x8E
#define KEY_END        0x8F
#define KEY_PAGE_UP    0xA0
#define KEY_PAGE_DOWN  0xA1
#define KEY_SEARCH     0xA2
#define KEY_HELP       0xA3
#define KEY_REDO       0xA4
// 中间电容键(KEY2)长按 → 待机（light sleep，main.cpp 里全局处理）。阅读器里有三个
// 把它当动作键的子界面（删词典 / 解绑按键 / 删统计里的书）放行，见
// screen_reader_long_confirm_is_action()。
#define KEY_LONG_CONFIRM 0xA5
// 触摸拖动：手指在显示区按住移动时按帧上报，增量用 input_drag_xy() 取。
// 只给需要"按像素跟随手指"的界面（计划模式列表）用；主循环会把其它界面的
// 这个键清成 0，所以老界面完全不受影响——它们照旧拿抬手时的点按/翻页键。
#define KEY_TOUCH_DRAG 0xA6
// 触摸长按（按住不动 ≥600ms，按下点随 input_tap_xy() 一起给出）。
// 与键盘 Esc(0x1B) 分开：长按要按"按在哪里"弹菜单，Esc 只是返回。
// 主循环把其它界面的它翻回 0x1B，老界面完全不受影响。
#define KEY_TOUCH_LONG 0xA7
// 屏幕边缘向中间横划 → 返回（三种模式通用）。由 hw/input.cpp 在手势抬手时产生：
// 只有**按下点落在屏幕边缘带内**的横划才算返回；从中间起划的横划仍是原来的
// KEY_LEFT/KEY_RIGHT（翻页/移光标）。方向用 input_back_dir() 取（+1 = 手指左滑，
// 与 KEY_RIGHT 同一套约定），虚拟键盘候选行翻页靠它。
// 各模式的落点：写作/计划等同 Esc(0x1B) 退一层；阅读模式见 screen_reader_handle。
#define KEY_BACK       0xA8
// 左侧电容键(KEY1)/右侧电容键(KEY3) 的单击，**只发给阅读模式**：别的模式在 main.cpp
// 里就已经翻成 KEY_UP/KEY_DOWN（全设备一致：右侧键=上移、左侧键=下移），走不到这里。
// 阅读模式需要"哪一侧"这个信息，是因为**阅读页要的是"右侧=下一页、左侧=上一页"**，
// 与列表里的上/下移正好相反——所以不能像别的模式那样提前翻好。阅读模式的其它界面
// （目录/菜单/设置…）在 screen_reader_handle 里把它们翻回 KEY_UP/KEY_DOWN，照旧。
#define KEY_CAP_RIGHT  0xA9
#define KEY_CAP_LEFT   0xAA
// 同一对键的**双击**（阅读页专属）：右侧 = 下一章，左侧 = 上一章。
// 单击要排队等双击窗口（阅读页单击=翻页，刷一次要 1.8s，不排队会把第二下整个吞掉，
// 双击永远认不出来），见 main.cpp 的电容键分支。
#define KEY_NEXT_CHAPTER 0xAB
#define KEY_PREV_CHAPTER 0xAC
// 双指捏合（显示区两指间距缩小/放大）→ 缩小/放大。目前只有阅读模式的图片查看器
// 认这两个键。**必须是独立键码**：复用 KEY_UP/DOWN 会让阅读页在捏合时翻页，
// 复用 KEY_PAGE_* 更糟（竖滑同码）。两指手势自带"直到全部抬手前不再产生单指键"
// 的抑制位（见 hw/input.cpp），所以不会顺带甩出一个点按/滑动键。
#define KEY_PINCH_IN   0xAD   // 两指靠拢 = 缩小
#define KEY_PINCH_OUT  0xAE   // 两指张开 = 放大
// 晃动机身 = 一次全刷（清残影）。由 hw/input.cpp 读 SC7A20H 加速度计判定，阈值与
// 判定逻辑见那边；三个模式通用（阅读模式在自己的入口处认这个键，走它自己的全刷档）。
// 这个键位原来是"长按中间确认键"，现在那个键改成待机了。
#define KEY_SHAKE      0xAF
// 电源键短按 → 写作/阅读模式切换（阅读模式内同样切回写作）。
#define KEY_POWER      0xB0
// 电源键**长按满 8s**：PMU 的 KEY_FORCE_OFF 警告事件（再满 10s 就自己拉 EN 硬断电了）。
// 主机拿它当"要关机了"的信号，抢在那 2s 窗口里把「已关机」页铺满 —— 不铺的话屏上就
// 停在上一帧，看不出是关机（用户报过）。由 hw/input.cpp 的 poll_pmu_key() 产生。
#define KEY_POWER_HOLD 0xB1
// BLE 遥控器/翻页器的消费类(Consumer Control)按键：KEY_CONSUMER_BASE | 16 位 HID usage。
// 这类键只有蓝牙设备会产生；默认无动作，由阅读菜单「按键映射」绑到某个阅读动作。
// 必须与 bt_keyboard.cpp 的同名宏一致。
#define KEY_CONSUMER_BASE 0x2000
// Ctrl+0-9 → 快捷编辑文件切换 (0x90-0x99)
#define KEY_FILE_BASE 0x90

// 本帧的 key 是否来自蓝牙键盘/遥控器（由主循环每帧置位）。阅读模式的「按键映射」
// 捕获时靠它区分"用户按了蓝牙键"和"用户点了触摸屏"。
extern bool g_key_from_ble;

// Screen entry points (screens that remain in pjournal_app.cpp)
void screen_main_init();
AppState screen_main_handle(int key, ScreenContext &ctx);

void screen_browser_init();
AppState screen_browser_handle(int key, ScreenContext &ctx);

void screen_viewer_init(const std::string &filename);
AppState screen_viewer_handle(int key, ScreenContext &ctx);

void screen_history_init(const std::string &filename, AppState returnTo);
AppState screen_history_handle(int key, ScreenContext &ctx);

// ── 拆出去的屏不在这个头里 ────────────────────────────────────────────────
// 本头只声明**实现还在 pjournal_app.cpp 里**的屏（main/browser/viewer/history，
// 见上一段）。已经搬到自己 .cpp 的屏各有 screen_*.h，要用就直接 include 那一个：
//   screen_editor.h / screen_settings.h / screen_gtd.h / screen_outline.h /
//   screen_inspiration.h / screen_flomo.h / screen_reader.h / screen_bt_manage.h /
//   screen_file_manager.h / screen_polish.h / screen_polish_prompt.h
// 这里以前把它们的原型又抄了一遍 —— 同一份声明两处维护，正是"声明和实现对不上"
// 的温床（screen_reader.cpp 抄 ttf 接口抄到 ttf_font_open 返回值都变了，同一个毛病）。

// Flomo send text (set by browser/viewer before entering APP_SYNC_SEND_FLOMO)
extern std::string g_flomoPendingText;
extern AppState g_flomoReturnTo;

// 阅读器当前子界面里，长按中间确认键是不是一个**动作**（删词典/解绑按键/删统计里的书）
// 而不是"待机"。main.cpp 的全局"长按中间键 = 待机"靠它放行这三处，其余一律待机。
bool screen_reader_long_confirm_is_action();

// 全局蓝牙开关（设置键 "bt_enabled"，默认开）。实现在 main.cpp——只有它管着 bt_init
// 任务与协议栈的生命周期。关 = 拆栈、停射频、不再有任何连接尝试；开 = 重新初始化并
// 恢复自动重连。界面入口是蓝牙管理页最上面那一行（screen_bt_manage）。
bool app_bt_enabled();
void app_bt_set_enabled(bool on);
