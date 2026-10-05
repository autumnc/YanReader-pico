#pragma once

#include <string>
#include <vector>
#include <set>
#include "font_renderer.h"

class IME;

struct MdLineInfo;

// VRow structure for word-wrap rendering.
// indentPx = 该 vrow 的**首行缩进**（像素，普通段的首 vrow 才是 4 个半格，其余 0）。
// 从前叫 indentCells，是格数；拉丁改成按字体真实步进排之后，"格"不再是通用单位，
// 所以整条布局链（buildVrows / mdVrowX / 各消费点）都改用像素。
struct VRow { int lineIdx; int start; int end; int indentPx = 0; };

// ── 屏幕几何：运行时取值（横屏 1216×684 | 竖屏 684×1216）──
// 由 hw/board.cpp 的 epd_set_rotation 决定，切换方向后调用方整屏重刷。
int ui_screen_w();
int ui_screen_h();

// SCREEN_W/H 保留为宏（表达式求值），使所有移植的 screen_*.cpp 无需改动。
#define SCREEN_W (ui_screen_w())
#define SCREEN_H (ui_screen_h())

// 状态栏高随字号缩放（原 22px 固定 → 现容纳 TTF 抗锯齿字形）。
#define STATUS_BAR_FONT_SIZE 22
#define STATUS_BAR_H (g_font.lineHeight() + 4)
#define STATUS_BAR_Y (SCREEN_H - STATUS_BAR_H - 2)

// Font-dependent metrics (dynamic via g_font)
#define FONT_H (g_font.lineHeight())
#define STATUS_H (g_font.lineHeight())
#define VISIBLE_LINES ((SCREEN_H - STATUS_H) / FONT_H)
#define LINE_SPACING (g_font.lineHeight() + 4)
#define STATUS_Y (SCREEN_H - FONT_H - 2)

// 顶部单行标题（标题文字 + 下划线 + 正文起点）的公共基线。
// ui_draw_text*() 的 y 是**基线**而不是行顶：历史上各界面写死 28px，而 UI 字号的
// ascender 就有 39px 量级，字形的上升部直接顶出屏幕上缘 → 看起来"被截断半个字"。
// 标题基线必须由字体自己算，正文起点再从这个基线往下排。
inline int ui_title_baseline() { return g_font.ascent() + 6; }

// IME singleton (defined in ui_helpers.cpp)
extern IME &g_ime;

// UI helper functions
// right 右对齐。rightReserve > 0 时再往左让出这么多像素——给状态栏右端别的图标
// （写作/计划模式的键盘开关）腾位置：字符串是**右对齐**的，只在调用方按宽度截断
// 并不能真的把最右边那几个字形从图标底下挪出来，末尾的电池图标会正好压在图标槽里。
void ui_draw_status(const char *left, const char *right, int rightReserve = 0);
void ui_draw_title(const char *title);
void ui_clear();
void ui_commit();

// 对当前帧缓冲做一次整屏 GC16 全刷（清掉局刷/DU 攒下的残影）。不重绘、不重排：
// 帧缓冲里就是当前画面，全刷只是换 GC16 波形把它重画一遍。长按中间确认键调用，
// 各模式通用（见 main.cpp 的全局键处理）。
//
// 待机提示页也用它（standby_clock.cpp）——那里要的是"推上去但**别动**保留帧"，而本
// 函数本来就不碰保留帧（快照只由 ui_invalidate_snapshot / ui_restore_snapshot 管），
// 所以曾经另起的名字 ui_send_buffer() 是同一件事的第二个说法，已删。
void ui_full_refresh_now();
// 用快照恢复缓冲并整屏发送(休眠唤醒后清除"休眠中"提示)
void ui_restore_snapshot();
void ui_invalidate_snapshot();
// 编辑器快刷模式:进入编辑界面后开启,每个按键用 DU 差分整屏(仅驱动变化像素,
// 约 220ms)替代 GL16 整屏(约 410ms)。开启后的首次提交仍整屏刷(进界面的全屏变化),
// 之后才走 DU 快刷。
// **打字期间不做周期 GC16**:该模式与 ui_set_local_only 一样会挂起"攒够
// FOLLOW_GC16_EVERY 次局刷就整屏 GC16 清残影"的兜底,改为攒成 pending,等两者都关
// (离开编辑器/GTD 打字态)后的第一次提交补做——否则打字节奏里每几十秒会被 404ms
// 的全屏闪打断一次。
void ui_set_fast_partial(bool enable);
// 虚拟键盘打字时开:任何大小的变化都只做**局刷**(跟随 DU，8 帧/约 89ms，只驱动
// 变化矩形)，不再走"差分超过半屏就整屏 GL16"那条规则。键盘占着下半屏时，敲字
// 的差分常从正文一路跨到候选条，超过半屏就会整屏 GL16 闪一下——实测"按一下刷
// 一下"。局刷区域由 diff 决定，仅候选条变化时就只刷那一小条。
// 周期 GC16 兜底同样挂起(见 ui_set_fast_partial)：局刷每 8 次本会升级一次整屏
// GC16，落在输入法上就是"敲几个字闪一屏"，这正是要避免的。
void ui_set_local_only(bool enable);
int  ui_text_width(const char *text);
void ui_draw_text(int x, int y, const char *text, bool invert = false, bool bold = false);
void ui_draw_text_centered(int y, const char *text, bool invert = false, bool bold = false);
// 内容面版本：与上面两个逐行等价，只把字面从 g_font（恒内置）换成 g_content_font
// （用户在设置 → 字体里选的字体）。两个实例**共享同一套格子度量**（见 font_renderer.h
// 的共享格子模型），所以宽度、行高、反白块的几何与 ui_draw_text 完全一致——调用方的
// FONT_H / LINE_SPACING 一个都不用改。给"显示用户内容"的界面用（flomo 列表/详情/检索）。
// 没有 bold 形参：ui_draw_text 本来就不理会它，留个假参数只会掩盖"有没有真加粗"。
void ui_draw_text_content(int x, int y, const char *text, bool invert = false);
void ui_draw_text_content_centered(int y, const char *text, bool invert = false);
void ui_show_message(const char *msg, int duration = 2000);
void ui_show_message_centered(const char *msg);

// ── 就地轻提示（"已复制"这类）────────────────────────────────────────────
// 与 ui_show_message_centered 的区别：**不清屏、不阻塞**——在当前画面上盖一个
// **不透明**的白底黑框黑字小条，到点自己消失。宿主只管喊一句 ui_toast_show()：
// 框由 ui_commit() 在本帧内容**之上**画，所以所有走 ui_commit 的界面都不用改绘制
// 代码；"抹掉"靠宿主的下一次重绘（各界面空闲都 100ms 重绘一帧，唯一例外是编辑器
// 那个会跳重绘的空闲，见 screen_editor_idle）。
// bottomY > 0 = 框贴这个 y 之上（虚拟键盘 / 按钮条上面那条）；<= 0 = 贴状态栏上方。
void ui_toast_show(const char *msg, int bottomY = 0, int ms = 1400);
bool ui_toast_active();
void ui_toast_clear();
// 居中三行确认框：框宽/高随文字与 FONT_H 缩放（行距 = 行高，避免文字挤压）。
void ui_draw_confirm_dialog(const char *l1, const char *l2, const char *l3);

// Battery (PMU soc_permille)
void battery_init();
int battery_pct();
// 是否正在充电(PMU charge_state)。状态栏据此在电量数字后加一个 U+E039 闪电。
bool battery_charging();
// 设备电池文本:"电量";电量未知时返回空串。只给数字不带图标,见实现处的说明。
std::string battery_text();
// 设备电量 + 有蓝牙键盘时追加一个蓝牙图标（只表示连没连上，不带电量数字；
// 键盘低电由一次性提示负责，见 bt_keyboard 的 takeLowBatteryWarning）。
std::string battery_status_text();

// Word-wrap builder. mdInfoIn: precomputed per-line markdown info (skips
// internal mdClassifyLines). foldedHeadings: heading line indices whose body
// is collapsed — their vrows are omitted (editor view-state, viewer passes
// defaults).
std::vector<VRow> buildVrows(const std::vector<std::string> &lines,
                             const std::vector<MdLineInfo> *mdInfoIn = nullptr,
                             const std::set<int> *foldedHeadings = nullptr);

// ── 编辑区正文字号（"显示与版式 → 正文字号"）──────────────────────────────
// 设置项 editor_font_size（像素高，档位 34/45/56/68，45 = 与界面 20pt 同高的标准档）。
// 编辑器**正文整块**（布局、换行、光标、选区、触摸命中、竖排）都按这个字号算，界面框架
// （状态栏、输入法条、查找/帮助/快捷菜单浮层）仍是界面字号。
//
// 实现方式：进正文作用域时把 FontRenderer 的**共享格子**整块换成这个字号
// （font_renderer.h 的 FontScope），出来自动还原。这样正文那几十处派生式
// （FONT_H / LINE_SPACING / ascent() / halfAdvance()）连带 buildVrows、markdown_render
// 全都跟着走，不用逐个调用点改；代价是作用域里画界面框架的地方必须显式钉回
// `FontRenderer::uiPxHeight()`（见 drawIMEUI 等）。
int editorBodyFontPx();

// ── 界面框架字号哨兵 ─────────────────────────────────────────────────────
// 上面那套"正文整块换格子"有一个已知代价：**界面框架必须自己钉回界面字号**。漏钉的
// 症状只在用户把正文字号调离默认档之后才出现（默认档正文与界面同高，漏了也看不出来），
// 而且表现为"状态栏高了半格""候选条挤了"这种一眼看不出对错的错版——最难查的一类。
//
// 所以每个界面框架绘制入口在**钉回之后**对一次账：格子是不是真回到了界面档？不是就
// 记一条日志（**每个入口只报一次**，免得每帧刷屏）。正常路径的代价是一次整数比较；
// 买到的是"将来某次把 FontScope 那行删了/写错档"在日志里立刻有一句话，而不是靠肉眼
// 去比对一个只有在非默认设置下才显形的版式。
//
// 用法：在 `FontScope ui(FontRenderer::uiPxHeight());` 的下一行写 `UI_FONT_GUARD();`。
void uiFontGuard(const char *who);
#define UI_FONT_GUARD() uiFontGuard(__func__)

// ── 输入法候选行：字号 / 度量 / 测宽 / 直绘 ────────────────────────────────
// 设置项「候选字大小」(ime_cand_size，像素高，档位 34/45/56/68，45=标准) 的**唯一**
// 权威实现。两个消费者必须逐像素同一份：虚拟键盘的候选条(editor_vk.cpp)与实体键盘的
// 输入法条(drawIMEUI)。输入法按这里量出来的宽度分页，画的时候若换个字号或换套量法，
// "一行放几个候选"就会算错——多出来的被挤出屏幕右边，看不见也点不到。
// **行高不在这一组里**：那随版面（键盘面板要指尖点得着，行高 px+20；输入法条是贴身
// 一条，px+8），见 editor_vk.cpp 的 evkCandRowH 与 ui_helpers.cpp 的 imeBarRowH。
int imeCandFontPx();                // 设置值 clamp 到 [28,96]，认不出 → 45
int imeCandAscent();                // 该字号下的 ascent（字体没就绪 → 0.78em 近似）
int imeCandStrW(const char *s);     // 候选串在该字号下的像素宽（拉丁按真实步进，CJK = 字号宽）
// 按候选字号直绘一段候选串（拉丁按真实步进前进，CJK 走 TTF 全角）。invert = 反白
// （调用方已填黑底，这里把字画白）。内部临时切到内容面，画完还原。
void imeCandDrawText(int x, int baseline, const char *s, bool invert);

// IME drawing helpers
int imeStatusPanelTopY();
// 输入法条（编码行 + 候选行）的总高。面板贴 bottomY 往上排，顶边 = bottomY - 本值。
// 「候选字大小」改了它跟着变，所以给正文留位时要用这个函数，别写死行数。
int imeBarPanelH();
int imeFullscreenPanelTopY();
int imeCandidateLineWidth();
std::string imeStatusLabel(bool active);
// 画输入法条（编码行 + 候选行，两行贴着 bottomY 往上排）。三种落点见下面三个包装。
void drawIMEUI(int bottomY);
void drawIMEUIWithStatusBar();
void drawIMEUIFullscreen();
// 全屏候选面板的"给状态栏让位"版：整块面板上移，候选行的下沿落在
// 状态栏上沿之上 3px——面板和状态栏**同时**要画时用它（计划模式加/重命名任务的
// 输入框就是：面板按原样贴屏幕底，候选行整个被状态栏的白底盖住）。灵感/润色那些
// "面板与状态栏二选一"的屏仍用 drawIMEUIFullscreen()：那里候选行贴底才对。
void drawIMEUIFullscreenAboveStatusBar();

// WiFi helper functions
bool ensure_wifi_connected();
void restore_wifi_state(bool wasConnected);

// NTP time sync helper
bool syncNtpTime(const std::string &ntpServer, const std::string &timezone);

// Word-wrap byte ⇄ x(像素) 换算（从前是 byteToCells/cellsToByte 的"格"制版）。
// 拉丁按字体真实步进排之后，一行里的字符宽度不再整齐，"格"这个单位在布局链上
// 已经没有意义，所以统一换成像素。ASCII 的口径见 ui_helpers.cpp 的 charAdvancePx。
int byteToX(const std::string &line, int byteOffset);
int xToByte(const std::string &line, int start, int end, int targetX);

// Word/body helpers
int countVisibleChars(const std::string &text);
std::string extractBody(const std::string &content);
