#pragma once

#include "pjournal_app.h"

// Editor screen entry points
void screen_editor_init(ScreenContext &ctx);
AppState screen_editor_handle(int key, ScreenContext &ctx);

// 进入 / 离开编辑器（main.cpp 的 kScreens 生命周期钩子）。**要不要重建**这条规则
// 由编辑器自己拿着（见 .cpp）：从灵感/润色/历史回来时只是被浮层盖过，正文/光标/选区
// 都还在，不重建；从别处进来则重建。
void screen_editor_enter(ScreenContext &ctx);
void screen_editor_leave(AppState next);

// 重套本模式的屏幕方向（设置项「写作模式方向」，与 screen_gtd_apply_orientation 对称）。
void screen_editor_apply_orientation();

// Idle tick (no key): runs auto-save, repaints only if the screen is stale or
// forceRedraw is set. Returns true if a repaint happened.
bool screen_editor_idle(ScreenContext &ctx, bool forceRedraw);

// Mark the on-screen editor content stale (call when another screen painted
// over it, e.g. returning from inspiration/polish).
void screen_editor_reset_drawn();

// 宽高变了（自适应方向中途转屏）：既复位重绘记账，也作废按旧宽度烤出来的折行缓存。
// 见 screen_editor.cpp 里的实现注释。
void screen_editor_on_width_change();

// IME state for global Ctrl+Space toggle
bool app_ime_active();
void app_toggle_ime();
bool app_ime_fullwidth();
void app_toggle_fullwidth();
void app_toggle_trad();
void app_toggle_english();
void app_toggle_ime_delete_mode();

// Force editor re-initialization on next cycle
void app_editor_request_reinit();
bool app_editor_needs_reinit();

// Park/restore the current editor while a modal flow reuses the editor screen.
void app_editor_stash_session();
void app_editor_restore_stashed_session();
bool app_editor_has_stashed_session();

// 查找/替换对话框是否打开(供 main.cpp 屏蔽全局按键/物理按键)
bool app_editor_search_active();

// 快捷键帮助对话框是否打开(供 main.cpp 屏蔽全局按键/物理按键)
bool app_editor_help_active();

// 编辑器弹出层（点正文弹出的快捷菜单 / 二维码 / 触摸选区与按钮条 / 粘贴板列表）
// 是否打开。同上，用来屏蔽全局按键：菜单开着的时候双击 BOOT 不该把「全文润色」
// 抢走，全局 IME 热键也不该插进来把菜单刷掉。
bool app_editor_popup_active();

// 离开编辑器时清掉触摸选区与弹层（电源键切模式绕开了 screen_editor_init）。
void app_editor_leave_cleanup();

// 步 5「按下即反馈」：编辑器空转的每一小段里调一次（main.cpp 的 scrEditor 空转循环，
// 紧跟 input_tick 之后）。有按下沿、且落在虚拟键盘的键上、且渲染那边闲着时，只补画
// 那一个反白键帽推一帧出去；否则静默返回，一个副作用都不留。见 .cpp 的说明。
void screen_editor_press_feedback_tick();

// Get editor text for Flomo sending
std::string app_get_editor_text();

// Insert text at the cursor (IME commit / inspiration / polish all funnel here)
void editorInsertText(const std::string &text);

// Replace the entire editor text (used by AI polish confirm). Cursor → end.
void editorReplaceAllText(const std::string &text);

// Currently selected text (empty when no selection).
std::string app_get_selected_text();

// Replace only the selected text (selection polish confirm). Cursor → end of
// the inserted text; selection cleared.
void editorReplaceSelection(const std::string &text);
