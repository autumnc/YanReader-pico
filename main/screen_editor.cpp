#include "screen_editor.h"
#include "editor_vk.h"
#include "screen_polish.h"
#include "font_renderer.h"
#include "journal_storage.h"
#include "deepseek_client.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "quick_edit.h"
#include "typing_click.h"
#include "ui_helpers.h"
#include "hw/input.h"
#include "markdown_render.h"
#include "vertical_layout.h"
#include "icon_font.h"          // 状态栏左端的模式图标（主菜单同一套）
#include "main_menu_icons.h"    // MAIN_ICON_PROMPT / MAIN_ICON_FREE
#include "builtin_prompts.h"    // 状态栏模式标记点一下=取一条内置提示词
#include "qrcodegen.h"  // 「二维码」菜单项：把全文编成码给手机扫
#include "ime/IME.h"
#include "ui/ime_field.h"  // 输入框绑定：落串/退格/光标的 UTF-8 算术（与计划/阅读共用一份）
#include <cstdio>
#include <cstdlib>   // rand()：状态栏模式标记/Ctrl+P 取内置提示词
#include <cstring>
#include <ctime>
#include <set>
#include <esp_timer.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

extern "C" {
    // 图标字体不走 u8g2，直接写 4bpp 帧缓冲；缓冲指针从 shim 现取（ui_render 每帧
    // 会换绘制目标，缓存的指针会指到上一帧那块）。
}

#include "clipboard.h"
#include "edit_menu.h"   // 触摸选区的按钮条 + 粘贴板列表（三模式共享）
#include <Utf8.h>        // utf8NextCodepoint：选词时按 UTF-8 边界走
#include "u8g2_shim.h"

#define EDITOR_MAX_CELLS (SCREEN_W / g_font.halfAdvance())

// ── Editor state ─────────────────────────────────────────────────────────

struct EditorSnapshot {
    std::string text;
    int cx = 0;
    int cy = 0;
    int scroll = 0;
};

enum class UndoGroup {
    None,
    Typing,
    Delete,
    Structural,
};

struct EditorState {
    std::vector<std::string> lines;
    int cx = 0, cy = 0;
    int scroll = 0;
    int targetCx = -1;
    // 提示词。**有没有提示词就是"提示写作 / 自由写作"的唯一判据**（见 editorPromptOn）：
    // 合并成一个「写作」模式后不再单独记一个模式布尔量，否则会出现"切了模式却没有提示词"
    // 的半吊子状态。Ctrl+P 与状态栏那个模式标记都只改这一个变量。
    std::string promptText;
    std::string titleOverride;
    bool imeActive = false;
    bool confirmSave = false;
    bool vrowsDirty = true;
    std::vector<VRow> cachedVrows;
    bool cachedFirstLineIndent = false;
    // 折行缓存是按哪个**正文字号**算出来的（"显示与版式 → 正文字号"改了就要重排）。
    int cachedBodyPx = 0;
    int cachedWordCount = 0;
    bool wordCountDirty = true;
    bool mdInfoDirty = true;
    std::vector<MdLineInfo> cachedMdInfo;
    bool cachedMdOn = false;
    // 折叠的标题行号(Ctrl+T 切换)。视图态:不随文本持久化,按行号平移维护。
    std::set<int> foldedHeadings;
    std::vector<EditorSnapshot> undoStack;
    std::vector<EditorSnapshot> redoStack;
    size_t undoBytes = 0;
    size_t redoBytes = 0;
    UndoGroup lastUndoGroup = UndoGroup::None;
    int64_t lastUndoTime = 0;
    int64_t autoSaveTime = 0;
    bool modifiedSinceSave = false;
    std::string savedFilename;
    bool recoveryPrompt = false;
    std::string recoveryContent;
    std::string recoveryMeta;
    uint32_t lastRecoveryHash = 0;
    bool promptGenerating = false;

    // 单击正文空白处弹出的快捷菜单（以前这里是"插一个回车"）。菜单是模态的：
    // 上面的所有按键都只喂给它，Esc 收起。
    bool menuActive = false;
    int menuSel = 0;

    // 「二维码」子界面：把编辑区全文编码成二维码，手机扫一下取走。
    bool qrActive = false;
    std::string qrText;       // 实际编码的内容（按容量截断后）
    bool qrTruncated = false; // 是否因超出二维码容量被截断
    int qrBytes = 0;          // 原文总字节数（截断时提示用）

    // Selection
    bool hasSelection = false;
    int selAnchorCy = 0, selAnchorCx = 0;

    // ── 触摸选区会话（长按选字 → 拖两柄 → 底部按钮条）──────────────────────
    // 选区模型**一个字节都不新增**：还是上面那套 hasSelection / selAnchor / (cy,cx)。
    // 下面的字段只描述"触摸会话"本身。会话期间的活动端按约定恒为**较晚**的一端
    // （selAnchor 是起点、cy/cx 是终点），于是两个柄各自对应一个明确的字段。
    bool selTouch = false;        // 会话开着（按钮条/拖动/粘贴板列表都在里面）
    int selGrab = -1;             // 点中的柄（画成空心）：-1 / 0 头柄 / 1 尾柄
    int selDrag = -1;             // 正在拖的柄：-1 没拖 / 0 / 1
    int selDragX = 0, selDragY = 0;   // 拖动锚点，从**按下点**起算（同阅读器）
    bool selDragActive = false;   // 这一轮按下真的拖过：抬手那一下补的键要吃掉
    // Whether the editor content is currently on screen. Idle ticks skip the
    // full redraw once it is; reset when another screen paints over it.
    bool drawnOnce = false;

    // 查找/替换对话框 (Ctrl+/ 打开)
    struct {
        bool active = false;
        std::string term;        // 查找词(UTF-8,可含换行)
        std::string rep;         // 替换文本
        int termCur = 0, repCur = 0;  // 各字段光标(字节偏移)
        bool focusRep = false;   // false=查找字段, true=替换字段
        bool imeActive = false;  // 对话框内输入法开关
        int cur = -1;            // 当前匹配索引(matches 为空时 -1)
        std::vector<std::pair<int,int>> matches;  // 匹配区间[docStart,docEnd)
    } search;

    // 快捷键帮助对话框 (Ctrl+?)
    bool helpActive = false;
    int helpScroll = 0;
};

static EditorState g_editor;
static EditorState s_stashedEditor;
static bool s_hasStashedEditor = false;

static volatile bool s_promptTaskDone = false;
static DeepseekResult s_promptTaskResult = {false, ""};
static std::string s_promptTaskContext;
static SemaphoreHandle_t s_promptResultMutex = nullptr;

static void ensurePromptResultMutex() {
    if (!s_promptResultMutex) s_promptResultMutex = xSemaphoreCreateMutex();
}

static void lockPromptResult() {
    ensurePromptResultMutex();
    if (s_promptResultMutex) xSemaphoreTake(s_promptResultMutex, portMAX_DELAY);
}

static void unlockPromptResult() {
    if (s_promptResultMutex) xSemaphoreGive(s_promptResultMutex);
}

// Mark every line-derived cache (vrows, word count, markdown info) stale.
static void markDirty() {
    g_editor.vrowsDirty = true;
    g_editor.wordCountDirty = true;
    g_editor.mdInfoDirty = true;
}

// 打字机模式:光标居中 + 按键音效是否生效
static bool editorTypewriter() { return g_settings.inputMode() == "typewriter"; }
static bool editorVertical() { return g_settings.editorOrientation() == "vertical"; }

// 提示写作 / 自由写作同属「写作」一个模式，区别只在**有没有提示词**：
// 有提示词 → 提示写作（正文上方一条提示词表头、状态栏灯泡图标 / "提示写作"）；
// 没有 → 自由写作。Ctrl+P 与状态栏左端那个模式标记都只改 promptText 这一个变量。
static bool editorPromptOn() { return !g_editor.promptText.empty(); }

// 状态栏左端的**文字**版本（横排用）：提示写作 / 自由写作。
// 标题被覆盖时（快捷文件、历史版本预览）显示的是文件名。
static const char *editorStatusTitle() {
    if (!g_editor.titleOverride.empty()) return g_editor.titleOverride.c_str();
    return editorPromptOn() ? "提示写作" : "自由写作";
}

// 状态栏左端把"提示写作"/"自由写作"换成图标（与主菜单同一套：灯泡 / 铅笔），
// **竖屏专用**（横排 1216px 宽，四个字放得下，照旧写文字）。
// 竖屏状态栏只有 SCREEN_W(684) 宽，四个汉字（再加" 竖排"共六个）就占掉小半个状态栏，
// 右端的字数/输入法/蓝牙/电量组被挤得没地方——图标只用一个字形宽。图标跟着有没有
// 提示词换：灯泡=提示写作、铅笔=自由写作。**这个图标本身可点**：点切模式、提示写作下
// 长按走 AI（见 s_statusModeHit）。
// 返回 0 表示照旧用文字：标题被覆盖时（快捷文件、历史版本预览等）显示的是文件名，
// 没有对应图标，也不该被图标糊掉。
static uint32_t editorStatusModeIcon() {
    if (!g_editor.titleOverride.empty()) return 0;
    return editorPromptOn() ? MAIN_ICON_PROMPT.codepoint : MAIN_ICON_FREE.codepoint;
}

// 行数变化时平移折叠标题的行号,保持折叠集与缓冲区对齐。
static void foldLinesInserted(int at, int count) {
    if (g_editor.foldedHeadings.empty()) return;
    std::set<int> shifted;
    for (int e : g_editor.foldedHeadings)
        shifted.insert(e >= at ? e + count : e);
    g_editor.foldedHeadings.swap(shifted);
}

static void foldLinesErased(int at, int count) {
    if (g_editor.foldedHeadings.empty()) return;
    std::set<int> shifted;
    for (int e : g_editor.foldedHeadings) {
        if (e >= at && e < at + count) continue;   // 被删除的折叠标题
        shifted.insert(e >= at + count ? e - count : e);
    }
    g_editor.foldedHeadings.swap(shifted);
}

static uint32_t fnv1a(const std::string &s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h ? h : 1;
}

static std::string metaValue(const std::string &meta, const char *key) {
    std::string prefix = std::string(key) + "=";
    size_t pos = meta.find(prefix);
    if (pos == std::string::npos) return "";
    pos += prefix.size();
    size_t end = meta.find('\n', pos);
    return meta.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

// ── Selection helpers ────────────────────────────────────────────────────
// Selection is defined by anchor (selAnchorCy, selAnchorCx) and cursor (cy, cx).
// The "start" is the earlier position, "end" is the later one.

struct TextPos { int cy, cx; };

static bool posLess(const TextPos &a, const TextPos &b) {
    if (a.cy != b.cy) return a.cy < b.cy;
    return a.cx < b.cx;
}

static void getSelRange(TextPos &start, TextPos &end) {
    if (!g_editor.hasSelection) {
        start = {g_editor.cy, g_editor.cx};
        end = start;
        return;
    }
    TextPos anchor = {g_editor.selAnchorCy, g_editor.selAnchorCx};
    TextPos cursor = {g_editor.cy, g_editor.cx};
    if (posLess(anchor, cursor)) { start = anchor; end = cursor; }
    else { start = cursor; end = anchor; }
}

static std::string getSelectedText() {
    TextPos start, end;
    getSelRange(start, end);
    if (start.cy == end.cy && start.cx == end.cx) return "";
    std::string result;
    if (start.cy == end.cy) {
        result = g_editor.lines[start.cy].substr(start.cx, end.cx - start.cx);
    } else {
        result = g_editor.lines[start.cy].substr(start.cx) + "\n";
        for (int i = start.cy + 1; i < end.cy; i++)
            result += g_editor.lines[i] + "\n";
        result += g_editor.lines[end.cy].substr(0, end.cx);
    }
    return result;
}

static void deleteSelection() {
    TextPos start, end;
    getSelRange(start, end);
    if (start.cy == end.cy && start.cx == end.cx) return;
    // Keep text before start and after end, join on same line
    g_editor.lines[start.cy] = g_editor.lines[start.cy].substr(0, start.cx)
        + g_editor.lines[end.cy].substr(end.cx);
    // Remove lines between start and end
    if (end.cy > start.cy) {
        g_editor.lines.erase(g_editor.lines.begin() + start.cy + 1,
                             g_editor.lines.begin() + end.cy + 1);
        foldLinesErased(start.cy + 1, end.cy - start.cy);
    }
    g_editor.cy = start.cy;
    g_editor.cx = start.cx;
    g_editor.hasSelection = false;
    g_editor.targetCx = -1;
    markDirty();
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
}

static void clearSelection() {
    g_editor.hasSelection = false;
}

static void extendSelection() {
    if (!g_editor.hasSelection) {
        g_editor.selAnchorCy = g_editor.cy;
        g_editor.selAnchorCx = g_editor.cx;
        g_editor.hasSelection = true;
    }
}

// Move cursor vertically by `step` visual rows (negative = up, positive = down),
// preserving the target visual column. Returns true if the cursor moved.
static bool moveCursorVertical(int step, const std::vector<VRow> &vrows) {
    int curVR = -1;
    for (int vi = 0; vi < (int)vrows.size(); vi++) {
        if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
            curVR = vi; break;
        }
    }
    if (curVR < 0) return false;
    int targetVR = curVR + step;
    if (targetVR < 0) targetVR = 0;
    if (targetVR > (int)vrows.size() - 1) targetVR = (int)vrows.size() - 1;
    if (targetVR == curVR) return false;
    auto &dst = vrows[targetVR];
    if (g_editor.targetCx < 0)
        g_editor.targetCx = byteToCells(g_editor.lines[g_editor.cy], g_editor.cx);
    int visualCol = g_editor.targetCx % EDITOR_MAX_CELLS;
    g_editor.cy = dst.lineIdx;
    int vrowStartCells = byteToCells(g_editor.lines[g_editor.cy], dst.start);
    int targetCells = vrowStartCells + visualCol;
    g_editor.cx = cellsToByte(g_editor.lines[g_editor.cy], dst.start, dst.end, targetCells);
    return true;
}

// 一屏可显示的行数(减去状态栏并留一行上下文), 作为 PageUp/PageDown 的翻页步长。
static int editorPageRows() {
    // 底边(状态栏上沿)是**界面字号**下的量,行高是**正文字号**的 —— 分开算,别一起放大。
    int statusY;
    { FontScope ui(FontRenderer::uiPxHeight()); statusY = STATUS_Y; }
    int rows = (statusY - FONT_H + LINE_SPACING - 1) / LINE_SPACING - 1;
    if (rows < 1) rows = 1;
    return rows;
}

static const std::vector<MdLineInfo>& getMdInfo(bool mdOn);

static const std::vector<VRow>& getVrows() {
    bool firstLineIndent = g_settings.firstLineIndent();
    // 折行宽度走 buildVrows 内部的 g_font.halfAdvance()，也就是**正文字号**的半个字宽。
    // 本函数可能被界面字号的上下文调到（screen_editor_handle 的第一句就是它），也可能
    // 在正文作用域里被调到，所以自己钉一次字号：缓存的 vrow 永远是正文口径，谁读都对。
    const int bodyPx = editorBodyFontPx();
    if (g_editor.vrowsDirty || g_editor.cachedFirstLineIndent != firstLineIndent ||
        g_editor.cachedBodyPx != bodyPx) {
        FontScope body(bodyPx);
        g_editor.cachedBodyPx = bodyPx;
        g_editor.cachedFirstLineIndent = firstLineIndent;
        // 传缓存避免重复 classify;md 渲染关闭时缓存全零,须传 nullptr 让
        // buildVrows 自行 classify(首行缩进仍需区分标题/列表)。
        bool mdOn = g_settings.markdownRender();
        const auto &mi = getMdInfo(mdOn);
        g_editor.cachedVrows = buildVrows(g_editor.lines, mdOn ? &mi : nullptr,
                                          &g_editor.foldedHeadings);
        g_editor.vrowsDirty = false;
    }
    return g_editor.cachedVrows;
}

static const std::vector<MdLineInfo>& getMdInfo(bool mdOn) {
    // Recompute when lines changed OR when the markdown toggle changed.
    if (g_editor.mdInfoDirty || g_editor.cachedMdOn != mdOn) {
        g_editor.cachedMdOn = mdOn;
        if (mdOn) {
            g_editor.cachedMdInfo = mdClassifyLines(g_editor.lines);
        } else {
            g_editor.cachedMdInfo.assign(g_editor.lines.size(), MdLineInfo{});
        }
        g_editor.mdInfoDirty = false;
    }
    return g_editor.cachedMdInfo;
}

static int getWordCount() {
    if (g_editor.wordCountDirty) {
        std::string fullText;
        for (auto &l : g_editor.lines) {
            if (!fullText.empty()) fullText += '\n';
            fullText += l;
        }
        g_editor.cachedWordCount = countVisibleChars(fullText);
        g_editor.wordCountDirty = false;
    }
    return g_editor.cachedWordCount;
}

// ── Quick edit (快捷编辑) helpers ─────────────────────────────────────────
static std::string currentEditorText() {
    std::string text;
    for (auto &l : g_editor.lines) { text += l; text += '\n'; }
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

static void loadLinesIntoEditor(const std::string &content) {
    g_editor.lines.clear();
    g_editor.foldedHeadings.clear();  // 折叠是视图态,整篇重载(undo/加载)后不保留
    if (content.empty()) {
        g_editor.lines.push_back("");
        g_editor.cx = g_editor.cy = 0;
        return;
    }
    size_t pos = 0;
    while (pos < content.length()) {
        size_t nl = content.find('\n', pos);
        g_editor.lines.push_back((nl == std::string::npos) ? content.substr(pos) : content.substr(pos, nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    while (g_editor.lines.size() > 1 && g_editor.lines.back().empty())
        g_editor.lines.pop_back();
    g_editor.cx = (int)g_editor.lines.back().length();
    g_editor.cy = (int)g_editor.lines.size() - 1;
}

static EditorSnapshot makeSnapshot() {
    EditorSnapshot s;
    s.text = currentEditorText();
    s.cx = g_editor.cx;
    s.cy = g_editor.cy;
    s.scroll = g_editor.scroll;
    return s;
}

static void restoreSnapshot(const EditorSnapshot &s) {
    loadLinesIntoEditor(s.text);
    g_editor.cy = s.cy;
    if (g_editor.cy < 0) g_editor.cy = 0;
    if (g_editor.cy >= (int)g_editor.lines.size()) g_editor.cy = (int)g_editor.lines.size() - 1;
    g_editor.cx = s.cx;
    if (g_editor.cx < 0) g_editor.cx = 0;
    if (g_editor.cx > (int)g_editor.lines[g_editor.cy].length())
        g_editor.cx = (int)g_editor.lines[g_editor.cy].length();
    g_editor.scroll = s.scroll;
    g_editor.targetCx = -1;
    g_editor.hasSelection = false;
    markDirty();
}

static void trimUndoStack(std::vector<EditorSnapshot> &stack, size_t &bytes) {
    const size_t maxBytes = 192 * 1024;
    const size_t maxItems = 32;
    while (stack.size() > maxItems || bytes > maxBytes) {
        if (stack.empty()) break;
        size_t sz = stack.front().text.size();
        bytes = (bytes >= sz) ? (bytes - sz) : 0;
        stack.erase(stack.begin());
    }
}

static void clearRedoHistory() {
    g_editor.redoStack.clear();
    g_editor.redoBytes = 0;
}

static void recordUndoSnapshot(UndoGroup group = UndoGroup::Structural) {
    int64_t now = esp_timer_get_time();
    bool mergeable = group == UndoGroup::Typing || group == UndoGroup::Delete;
    if (mergeable && g_editor.lastUndoGroup == group &&
        now - g_editor.lastUndoTime < 1000000) {
        g_editor.lastUndoTime = now;
        clearRedoHistory();
        return;
    }
    EditorSnapshot s = makeSnapshot();
    if (!g_editor.undoStack.empty() && g_editor.undoStack.back().text == s.text) return;
    if (s.text.size() > 96 * 1024) {
        g_editor.undoStack.clear();
        g_editor.undoBytes = 0;
        clearRedoHistory();
        g_editor.lastUndoGroup = UndoGroup::None;
        return;
    }
    size_t sz = s.text.size();
    g_editor.undoStack.push_back(std::move(s));
    g_editor.undoBytes += sz;
    trimUndoStack(g_editor.undoStack, g_editor.undoBytes);
    clearRedoHistory();
    g_editor.lastUndoGroup = group;
    g_editor.lastUndoTime = now;
}

static bool undoEditor() {
    if (g_editor.undoStack.empty()) return false;
    EditorSnapshot cur = makeSnapshot();
    size_t curSize = cur.text.size();
    g_editor.redoStack.push_back(std::move(cur));
    g_editor.redoBytes += curSize;
    trimUndoStack(g_editor.redoStack, g_editor.redoBytes);
    EditorSnapshot s = std::move(g_editor.undoStack.back());
    size_t sz = s.text.size();
    g_editor.undoBytes = (g_editor.undoBytes >= sz) ? (g_editor.undoBytes - sz) : 0;
    g_editor.undoStack.pop_back();
    restoreSnapshot(s);
    g_editor.lastUndoGroup = UndoGroup::None;
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
    return true;
}

static bool redoEditor() {
    if (g_editor.redoStack.empty()) return false;
    EditorSnapshot cur = makeSnapshot();
    size_t curSize = cur.text.size();
    g_editor.undoStack.push_back(std::move(cur));
    g_editor.undoBytes += curSize;
    trimUndoStack(g_editor.undoStack, g_editor.undoBytes);
    EditorSnapshot s = std::move(g_editor.redoStack.back());
    size_t sz = s.text.size();
    g_editor.redoBytes = (g_editor.redoBytes >= sz) ? (g_editor.redoBytes - sz) : 0;
    g_editor.redoStack.pop_back();
    restoreSnapshot(s);
    g_editor.lastUndoGroup = UndoGroup::None;
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
    return true;
}

static void clearUndoHistory() {
    g_editor.undoStack.clear();
    g_editor.redoStack.clear();
    g_editor.undoBytes = 0;
    g_editor.redoBytes = 0;
    g_editor.lastUndoGroup = UndoGroup::None;
    g_editor.lastUndoTime = 0;
}

static void loadQuickEditFile() {
    loadLinesIntoEditor(quickEditLoad(quickEditIndex()));
    g_editor.scroll = 0;
    g_editor.targetCx = -1;
    markDirty();
    g_editor.modifiedSinceSave = false;
    g_editor.autoSaveTime = 0;
    clearUndoHistory();
}

// 是否为快捷编辑主会话(直接编辑 /sdcard/{n}.txt)。g_quickEdit 模式下若正
// 在编辑灵感/日记内容(savedFilename 非空),则不属于快捷文件会话。
static bool inQuickFileSession() {
    return g_quickEdit && g_editor.savedFilename.empty();
}

static bool saveRecoveryDraftIfChanged() {
    std::string text = currentEditorText();
    std::string meta;
    meta += std::string("mode=") + (inQuickFileSession() ? "quick" : "journal") + "\n";
    meta += "filename=" + g_editor.savedFilename + "\n";
    meta += "quick_index=" + std::to_string(quickEditIndex()) + "\n";
    time_t now;
    time(&now);
    meta += "timestamp=" + std::to_string((long long)now) + "\n";
    uint32_t hash = fnv1a(text + "\n" + metaValue(meta, "mode") + "\n" +
                          metaValue(meta, "filename") + "\n" +
                          metaValue(meta, "quick_index"));
    if (hash == g_editor.lastRecoveryHash) return true;
    if (!g_journal.saveRecoveryDraft(text, meta)) return false;
    g_editor.lastRecoveryHash = hash;
    return true;
}

static void quickEditSwitchTo(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 9) idx = 9;
    if (idx == quickEditIndex()) return;
    if (quickEditSave(quickEditIndex(), currentEditorText())) g_journal.clearRecoveryDraft();
    quickEditSetIndex(idx);
    loadQuickEditFile();
}

// ── 查找/替换 (Ctrl+/) ────────────────────────────────────────────────────
// 匹配区间用"文档字节偏移"表示:文档 = currentEditorText()(行间以 '\n' 连接,
// 无结尾换行)。docOffsetToPos/posToDocOffset 与行坐标互转。

static const char *ELLIPSIS = "\xe2\x80\xa6";  // "…" U+2026 (3 bytes)

// pos 之后第一个 UTF-8 字符边界(跳过后续字节);越界返回串尾。
static int utf8Next(const std::string &s, int pos) {
    if (pos < 0 || pos >= (int)s.length()) return (int)s.length();
    const char *p = s.c_str() + pos;
    FontRenderer::utf8Decode(p);
    return (int)(p - s.c_str());
}
// pos 之前一个 UTF-8 字符边界。
static int utf8Prev(const std::string &s, int pos) {
    if (pos <= 0) return 0;
    int prev = pos - 1;
    while (prev > 0 && ((unsigned char)s[prev] & 0xC0) == 0x80) prev--;
    return prev;
}

// 字符串中的 UTF-8 字符(码点)数。
static int utf8Count(const std::string &s) {
    int n = 0;
    for (int i = 0; i < (int)s.length(); i = utf8Next(s, i)) n++;
    return n;
}

// 正文当前行 + 光标 —— ImeField 形态。跨行合行、撤销快照、脏标记这些宿主逻辑不进
// 共享层；这里只把"落串/退格/光标左右移"的 UTF-8 算术交出去。
static ImeField editorLineField() {
    return ImeField{&g_editor.lines[g_editor.cy], &g_editor.cx};
}

// 喂给输入法的"文档上下文":光标之前的正文尾部约 200 字。从末行往前拼、拼够就停,
// 免得为一个尾窗口把整篇正文复制一遍。只取光标之前的内容,后面还没写的不算上下文。
static std::string editorImeContextText() {
    const int kTargetChars = 200;
    const size_t kMaxBytes = 640;  // 200 个 CJK 字的上限,超出按字节裁并回退到字边界
    std::string tail;
    int chars = 0;
    for (int y = g_editor.cy; y >= 0; y--) {
        std::string line = (y == g_editor.cy)
            ? g_editor.lines[y].substr(0, g_editor.cx)
            : g_editor.lines[y];
        tail.insert(0, line);
        chars += utf8Count(line) + 1;  // +1 记行间换行
        if (y > 0) tail.insert(0, "\n");
        if (chars >= kTargetChars) break;
    }
    while (!tail.empty() && tail.front() == '\n') tail.erase(0, 1);
    if (tail.size() > kMaxBytes) {
        size_t start = tail.size() - kMaxBytes;
        while (start < tail.size() && ((unsigned char)tail[start] & 0xC0) == 0x80) start++;
        tail.erase(0, start);
    }
    return tail;
}

static void moveCursorVerticalInline(int step) {
    if (step < 0) {
        if (g_editor.cx > 0) {
            g_editor.cx = utf8Prev(g_editor.lines[g_editor.cy], g_editor.cx);
        } else if (g_editor.cy > 0) {
            g_editor.cy--;
            g_editor.cx = (int)g_editor.lines[g_editor.cy].length();
        }
    } else if (step > 0) {
        if (g_editor.cx < (int)g_editor.lines[g_editor.cy].length()) {
            g_editor.cx = utf8Next(g_editor.lines[g_editor.cy], g_editor.cx);
        } else if (g_editor.cy < (int)g_editor.lines.size() - 1) {
            g_editor.cy++;
            g_editor.cx = 0;
        }
    }
    g_editor.targetCx = -1;
}

static void moveCursorVerticalColumn(int dir, const VerticalData &data) {
    int curCol = verticalFindCol(data, g_editor.lines, g_editor.cy, g_editor.cx);
    if (curCol < 0) return;
    int row = verticalCellRow(data.cells[g_editor.cy], g_editor.cx) - data.cols[curCol].start;
    int dstCol = curCol + dir;
    if (dstCol < 0) dstCol = 0;
    if (dstCol >= (int)data.cols.size()) dstCol = (int)data.cols.size() - 1;
    const auto &dst = data.cols[dstCol];
    g_editor.cy = dst.lineIdx;
    g_editor.cx = verticalRowToByte(data.cells[dst.lineIdx], dst.start, dst.end, row);
    g_editor.targetCx = -1;
}

// 竖排选区高亮:对 [hStart, hEnd) 范围内的字符格做 XOR 反白
// (与横排选区同机制,draw color 2),格 = 整个字身方(线高×线高)
static void drawVerticalHighlight(const VerticalData &data,
                                  int scrollCol, const VerticalLayoutMetrics &vm,
                                  TextPos hStart, TextPos hEnd) {
    int cell = g_font.lineHeight();
    for (int ci = 0; ci < vm.cols; ci++) {
        int colIdx = scrollCol + ci;
        if (colIdx < 0 || colIdx >= (int)data.cols.size()) continue;
        const auto &col = data.cols[colIdx];
        if (col.lineIdx < hStart.cy || col.lineIdx > hEnd.cy) continue;
        const auto &cells = data.cells[col.lineIdx];
        int x = vm.x + vm.w - vm.colAdvance - ci * vm.colAdvance;
        for (int i = col.start; i < col.end; i++) {
            int b = cells[i].start;
            bool geStart = col.lineIdx > hStart.cy || (col.lineIdx == hStart.cy && b >= hStart.cx);
            bool ltEnd = col.lineIdx < hEnd.cy || (col.lineIdx == hEnd.cy && b < hEnd.cx);
            if (geStart && ltEnd) {
                int row = i - col.start;
                u8g2_SetDrawColor(g_u8g2, 2);
                u8g2_DrawBox(g_u8g2, x, vm.y + row * vm.rowAdvance, cell, cell);
                u8g2_SetDrawColor(g_u8g2, 1);
            }
        }
    }
}

static void docOffsetToPos(int off, int &cy, int &cx) {
    int remain = off;
    for (int i = 0; i < (int)g_editor.lines.size(); i++) {
        int len = (int)g_editor.lines[i].length();
        if (remain <= len) { cy = i; cx = remain; return; }
        remain -= len + 1;
    }
    cy = (int)g_editor.lines.size() - 1;
    cx = (int)g_editor.lines.back().length();
}

static int posToDocOffset(int cy, int cx) {
    int off = 0;
    for (int i = 0; i < cy; i++) off += (int)g_editor.lines[i].length() + 1;
    off += cx;
    return off;
}

static void searchComputeMatches() {
    auto &sh = g_editor.search;
    sh.matches.clear();
    sh.cur = -1;
    if (sh.term.empty()) return;
    const std::string doc = currentEditorText();
    const std::string &t = sh.term;
    size_t pos = 0;
    while (pos < doc.length()) {
        size_t f = doc.find(t, pos);
        if (f == std::string::npos) break;
        sh.matches.push_back({(int)f, (int)(f + t.length())});
        pos = f + t.length();
    }
    if (!sh.matches.empty()) sh.cur = 0;
}

// 把编辑器光标定位到第 idx 个匹配(并滚动跟随),不清除编辑内容。
static void searchGotoMatch(int idx) {
    auto &sh = g_editor.search;
    if (idx < 0 || idx >= (int)sh.matches.size()) return;
    sh.cur = idx;
    int cy, cx;
    docOffsetToPos(sh.matches[idx].first, cy, cx);
    g_editor.cy = cy;
    g_editor.cx = cx;
    g_editor.hasSelection = false;
    g_editor.targetCx = -1;
    markDirty();
}

// 匹配已由 searchComputeMatches 计算好:定位到从 startOffset 起(含)的第一个
// 匹配,没有则回到第一个(环绕)。
static void searchRefindFrom(int startOffset) {
    auto &sh = g_editor.search;
    int n = (int)sh.matches.size();
    if (n == 0) { sh.cur = -1; return; }
    for (int i = 0; i < n; i++) {
        if (sh.matches[i].first >= startOffset) { searchGotoMatch(i); return; }
    }
    searchGotoMatch(0);
}

// 关键词变化后重算匹配并定位。
static void searchAfterTermChange() {
    searchComputeMatches();
    if (g_editor.search.matches.empty()) return;
    searchRefindFrom(posToDocOffset(g_editor.cy, g_editor.cx));
}

static void searchNextMatch() {
    auto &sh = g_editor.search;
    if (sh.matches.empty()) return;
    searchGotoMatch((sh.cur + 1) % (int)sh.matches.size());
}

static void searchPrevMatch() {
    auto &sh = g_editor.search;
    if (sh.matches.empty()) return;
    int n = (int)sh.matches.size();
    searchGotoMatch((sh.cur - 1 + n) % n);
}

// 用 replacement 替换文档 [start,end) 字节区间,光标移到替换文本之后。
static void applyDocReplace(int start, int end, const std::string &repl) {
    recordUndoSnapshot(UndoGroup::Structural);
    std::string doc = currentEditorText();
    std::string newDoc = doc.substr(0, start) + repl + doc.substr(end);
    loadLinesIntoEditor(newDoc);
    int off = start + (int)repl.length();
    int cy, cx;
    docOffsetToPos(off, cy, cx);
    g_editor.cy = cy; g_editor.cx = cx;
    g_editor.hasSelection = false;
    g_editor.targetCx = -1;
    markDirty();
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
}

static void searchReplaceCurrent() {
    auto &sh = g_editor.search;
    if (sh.matches.empty() || sh.cur < 0) return;
    auto &m = sh.matches[sh.cur];
    int start = m.first, end = m.second;
    applyDocReplace(start, end, sh.rep);
    searchAfterTermChange();  // 文档已变,重新计算匹配并定位
}

// 全部替换,返回替换次数。替换后重新定位匹配。
static int searchReplaceAll() {
    auto &sh = g_editor.search;
    if (sh.term.empty()) return 0;
    std::string doc = currentEditorText();
    const std::string &t = sh.term;
    const std::string &r = sh.rep;
    std::string out;
    size_t pos = 0;
    int count = 0;
    while (pos < doc.length()) {
        size_t f = doc.find(t, pos);
        if (f == std::string::npos) break;
        out += doc.substr(pos, f - pos);
        out += r;
        pos = f + t.length();
        count++;
    }
    out += doc.substr(pos);
    if (count == 0) {
        searchRefindFrom(posToDocOffset(g_editor.cy, g_editor.cx));
        return 0;
    }
    recordUndoSnapshot(UndoGroup::Structural);
    loadLinesIntoEditor(out);
    g_editor.hasSelection = false;
    g_editor.targetCx = -1;
    markDirty();
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
    searchAfterTermChange();  // 文档已变,重新计算匹配并定位
    return count;
}

// ── 面板绘制 ──────────────────────────────────────────────────────────────

// 第 idx 条匹配所在行的文本及其高亮字节区间(跨行匹配只高亮到行尾)。
static std::string searchMatchContext(int idx, int &hlStart, int &hlEnd) {
    auto &sh = g_editor.search;
    auto &m = sh.matches[idx];
    int cyS, cxS; docOffsetToPos(m.first, cyS, cxS);
    int cyE, cxE; docOffsetToPos(m.second, cyE, cxE);
    std::string line = g_editor.lines[cyS];
    hlStart = cxS;
    hlEnd = (cyE == cyS) ? cxE : (int)line.length();
    return line;
}

// 截取包含 [hlStart,hlEnd) 且不超 maxW 的窗口;必要时前置/追加 "…"。
static std::string searchContextWindow(const std::string &line, int hlStart, int hlEnd,
                                       int maxW, int &outStart, int &outEnd) {
    int len = (int)line.length();
    if (g_font.textWidth(line.c_str()) <= maxW) {
        outStart = hlStart; outEnd = hlEnd;
        return line;
    }
    int ew = g_font.textWidth(ELLIPSIS);
    int budget = maxW - 2 * ew;
    if (budget < ew) budget = ew;
    int ws = hlStart, we = hlEnd;
    while (we < len) {
        int next = utf8Next(line, we);
        if (g_font.textWidth(line.substr(ws, next - ws).c_str()) > budget) break;
        we = next;
    }
    while (ws > 0) {
        int prev = utf8Prev(line, ws);
        if (g_font.textWidth(line.substr(prev, we - prev).c_str()) > budget) break;
        ws = prev;
    }
    std::string mid = line.substr(ws, we - ws);
    bool pre = ws > 0, post = we < len;
    std::string disp;
    int preLen = 0;
    if (pre) { disp += ELLIPSIS; preLen = 3; }
    disp += mid;
    if (post) disp += ELLIPSIS;
    outStart = preLen + (hlStart - ws);
    outEnd = preLen + (hlEnd - ws);
    return disp;
}

// 输入框文本窗口:保证光标可见,超宽时在光标两侧截断。
static void searchFieldView(const std::string &s, int cur, int maxW,
                            std::string &disp, int &dispCur) {
    if (g_font.textWidth(s.c_str()) <= maxW) { disp = s; dispCur = cur; return; }
    int len = (int)s.length();
    int ws = cur, we = cur;
    while (we < len) {
        int next = utf8Next(s, we);
        if (g_font.textWidth(s.substr(ws, next - ws).c_str()) > maxW) break;
        we = next;
    }
    while (ws > 0) {
        int prev = utf8Prev(s, ws);
        if (g_font.textWidth(s.substr(prev, we - prev).c_str()) > maxW) break;
        ws = prev;
    }
    disp = s.substr(ws, we - ws);
    dispCur = cur - ws;
}

// 绘制第 idx 条匹配:整行墨色文字,命中词 XOR 反显;当前匹配整行反显。
static void drawSearchMatchLine(int idx, int y, bool isCurrent) {
    int hlS = 0, hlE = 0;
    std::string line = searchMatchContext(idx, hlS, hlE);
    int outS = 0, outE = 0;
    std::string disp = searchContextWindow(line, hlS, hlE, SCREEN_W - 8, outS, outE);
    g_content_font.drawText(4, y, disp.c_str());
    if (isCurrent) {
        u8g2_SetDrawColor(g_u8g2, 2);  // XOR: 整行反显标记当前匹配
        u8g2_DrawBox(g_u8g2, 0, y - g_font.ascent(), SCREEN_W, FONT_H);
    } else {
        int midX = 4 + g_font.textWidth(disp.substr(0, outS).c_str());
        int midW = g_font.textWidth(disp.substr(outS, outE - outS).c_str());
        u8g2_SetDrawColor(g_u8g2, 2);  // XOR: 反显命中词
        u8g2_DrawBox(g_u8g2, midX, y - g_font.ascent(), midW, FONT_H);
    }
    u8g2_SetDrawColor(g_u8g2, 0);  // 恢复墨色
}

static void drawSearchPanel() {
    // 查找浮层是界面框架，几何按界面字号（它会在正文作用域里被调到）。
    FontScope ui(FontRenderer::uiPxHeight());
    UI_FONT_GUARD();
    g_editor.drawnOnce = true;
    ui_clear();
    auto &sh = g_editor.search;
    const int rowH = LINE_SPACING;

    // 标题行 + 匹配信息
    ui_draw_text(4, FONT_H, "查找/替换", false, true);
    std::string info;
    if (sh.term.empty()) info = "输入关键词";
    else if (sh.matches.empty()) info = "未找到";
    else info = std::to_string(sh.cur + 1) + "/" + std::to_string((int)sh.matches.size());
    ui_draw_text(SCREEN_W - 4 - g_font.textWidth(info.c_str()), FONT_H, info.c_str());

    // 查找字段
    {
        int y = FONT_H + rowH;
        ui_draw_text(4, y, "查找:");
        int tx = 4 + g_font.textWidth("查找:");
        std::string field = sh.term;
        for (auto &c : field) if (c == '\n') c = ' ';  // '\n' 同为1字节,光标偏移不变
        std::string disp; int dispCur;
        searchFieldView(field, sh.termCur, SCREEN_W - 8 - (tx - 4), disp, dispCur);
        g_content_font.drawText(tx, y, disp.c_str());
        if (!sh.focusRep) {
            int cx = tx + g_font.textWidth(disp.substr(0, dispCur).c_str());
            u8g2_SetDrawColor(g_u8g2, 0);
            u8g2_DrawBox(g_u8g2, cx, y + 4, 8, 3);
            u8g2_SetDrawColor(g_u8g2, 0);
        }
    }
    // 替换字段
    {
        int y = FONT_H + 2 * rowH;
        ui_draw_text(4, y, "替换:");
        int tx = 4 + g_font.textWidth("替换:");
        std::string disp; int dispCur;
        searchFieldView(sh.rep, sh.repCur, SCREEN_W - 8 - (tx - 4), disp, dispCur);
        g_content_font.drawText(tx, y, disp.c_str());
        if (sh.focusRep) {
            int cx = tx + g_font.textWidth(disp.substr(0, dispCur).c_str());
            u8g2_SetDrawColor(g_u8g2, 0);
            u8g2_DrawBox(g_u8g2, cx, y + 4, 8, 3);
            u8g2_SetDrawColor(g_u8g2, 0);
        }
    }
    // 分隔线:区分输入区与匹配区
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 3 * rowH - 4, SCREEN_W);

    // 匹配区:一行一条匹配,显示上下文并反显命中词;当前匹配整行反显
    {
        int y = FONT_H + 4 * rowH;
        int total = (int)sh.matches.size();
        if (sh.term.empty()) {
            g_font.drawText(4, y, "输入关键词");
        } else if (total == 0) {
            g_font.drawText(4, y, "未找到匹配");
        } else {
            const int show = 4;
            int winStart;
            if (total <= show) {
                winStart = 0;
            } else {
                winStart = sh.cur - (show - 1);
                if (winStart < 0) winStart = 0;
                if (winStart + show > total) winStart = total - show;
            }
            for (int li = 0; li < show; li++) {
                int idx = winStart + li;
                if (idx >= total) break;
                drawSearchMatchLine(idx, y + li * rowH, idx == sh.cur);
            }
        }
    }

    // 状态栏
    std::string imeLabel = imeStatusLabel(sh.imeActive);
    std::string right = imeLabel;
    std::string bt = battery_text();   // 编辑器状态栏不显示蓝牙图标，见下面 drawEditor 的说明
    if (!bt.empty()) right += " " + bt;
    ui_draw_status(sh.focusRep ? "替换字段" : "查找字段", right.c_str());

    if (sh.imeActive && g_ime.composing()) drawIMEUIFullscreen();
    ui_commit();
}

// ── 开/关与按键处理 ───────────────────────────────────────────────────────

static void searchOpen() {
    auto &sh = g_editor.search;
    if (g_editor.hasSelection) {
        sh.term = getSelectedText();  // 选中文本作为关键词(可含换行)
        TextPos start, end;
        getSelRange(start, end);
        g_editor.cy = start.cy;  // 光标回到选区起点,首个匹配即选中文本
        g_editor.cx = start.cx;
        clearSelection();
        g_editor.targetCx = -1;
    }
    sh.termCur = (int)sh.term.length();
    sh.focusRep = false;
    sh.repCur = (int)sh.rep.length();
    sh.imeActive = false;
    g_ime.setActive(false);  // 取消编辑器输入法组合,对话框默认英文
    sh.active = true;
    searchAfterTermChange();
}

static void searchClose() {
    auto &sh = g_editor.search;
    sh.active = false;
    sh.imeActive = false;
    sh.matches.clear();
    sh.cur = -1;
    g_ime.setActive(g_editor.imeActive);  // 恢复编辑器输入法状态
    if (g_editor.imeActive) g_ime.setDocumentContext(editorImeContextText());
}

// 搜索框当前聚焦的那个框（替换框 / 查找框）。
static ImeField searchField() {
    auto &sh = g_editor.search;
    return sh.focusRep ? ImeField{&sh.rep, &sh.repCur} : ImeField{&sh.term, &sh.termCur};
}

static void searchInsertFocused(const std::string &ins) { imeFieldInsert(searchField(), ins); }
static void searchBackspaceFocused() { imeFieldBackspace(searchField()); }
static void searchMoveFocusedLeft() { imeFieldMoveLeft(searchField()); }
static void searchMoveFocusedRight() { imeFieldMoveRight(searchField()); }

static void drawEditor();

static AppState screen_editor_search_handle(int key, ScreenContext &ctx) {
    auto &sh = g_editor.search;

    // 对话框内输入法(与编辑器共用 g_ime)
    if (sh.imeActive && key != 0) {
        std::string imeOut;
        if (g_ime.handleKey(key, imeOut)) {
            if (!imeOut.empty()) {
                searchInsertFocused(imeOut);
                if (!sh.focusRep) searchAfterTermChange();
            }
            drawSearchPanel();
            return APP_EDITOR;
        }
    }

    if (key == KEY_SEARCH || key == 0x1B) {
        // 关闭时把当前匹配设为选区:正文立即反显匹配位置(横竖排共用选区高亮),
        // 不然对话框全程盖住正文,关掉后匹配处只有光标,难以定位。
        auto &sh2 = g_editor.search;
        if (sh2.cur >= 0 && sh2.cur < (int)sh2.matches.size()) {
            int cy, cx;
            docOffsetToPos(sh2.matches[sh2.cur].first, cy, cx);
            g_editor.selAnchorCy = cy;
            g_editor.selAnchorCx = cx;
            docOffsetToPos(sh2.matches[sh2.cur].second, cy, cx);
            g_editor.cy = cy;
            g_editor.cx = cx;
            g_editor.hasSelection = true;
            g_editor.targetCx = -1;
            markDirty();
        }
        searchClose();
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }
    if (key == KEY_IME_TOGGLE) {
        sh.imeActive = !sh.imeActive;
        g_ime.setActive(sh.imeActive);
    } else if (key == KEY_FULLWIDTH_TOGGLE) {
        g_ime.toggleFullwidth();
    } else if (key == KEY_TRAD_TOGGLE) {
        g_ime.toggleTrad();
    } else if (key == KEY_LSHIFT_TAP) {
        g_ime.toggleEnglish();
    } else if (key == 0x09) {  // Tab: 切换字段
        sh.focusRep = !sh.focusRep;
    } else if (key == 0x0A || key == 0x0D || key == KEY_DOWN) {  // 下一处
        searchNextMatch();
    } else if (key == KEY_CTRL_ENTER || key == KEY_UP) {  // 上一处
        searchPrevMatch();
    } else if (key == 0x12) {  // Ctrl+R: 替换当前
        searchReplaceCurrent();
    } else if (key == 0x01) {  // Ctrl+A: 全部替换
        int n = searchReplaceAll();
        ctx.statusMessage = (n > 0) ? ("已替换 " + std::to_string(n) + " 处") : "未找到匹配";
        ctx.statusDuration = 30;
    } else if (key == 0x7F || key == 0x08) {  // Backspace
        searchBackspaceFocused();
        if (!sh.focusRep) searchAfterTermChange();
    } else if (key == KEY_LEFT) {
        searchMoveFocusedLeft();
    } else if (key == KEY_RIGHT) {
        searchMoveFocusedRight();
    } else if (key == KEY_HOME) {
        searchField().setCur(0);
    } else if (key == KEY_END) {
        imeFieldMoveEnd(searchField());
    } else if (key >= 0x20 && key <= 0x7E) {
        searchInsertFocused(std::string(1, (char)key));
        if (!sh.focusRep) searchAfterTermChange();
    }

    drawSearchPanel();
    return APP_EDITOR;
}

// ── 快捷键帮助对话框 (Ctrl+?) ─────────────────────────────────────────────
// 每行一条快捷键。含查找/替换对话框内的快捷键(见前4行)。
static const char *HELP_LINES[] = {
    "Ctrl+? 开关本帮助  Esc关闭",
    "Ctrl+/ 查找/替换",
    "  Enter下一处 Ctrl+Enter上一处",
    "  Tab切字段 Ctrl+R替换当前",
    "  Ctrl+A全部替换",
    "Ctrl+A全选  Ctrl+C复制",
    "Ctrl+X剪切  Ctrl+V粘贴",
    "Ctrl+Z撤销  Ctrl+R重做",
    "Ctrl+S保存  Ctrl+Q退出",
    "Ctrl+O润色选区",
    "Ctrl+T折叠/展开标题",
    "Ctrl+I灵感面板",
    "Ctrl+F发送Flomo",
    "Ctrl+Y历史版本",
    "Ctrl+N/P快捷编辑文件",
    "Ctrl+Space输入法开关",
    "Ctrl+D删除用户词",
    "Shift+Space全半角切换",
    "Ctrl+Shift+F简繁",
    "左Shift临时英文",
    "Home/End 行首/行尾",
    "PgUp/PgDn 翻页",
    "双击BOOT全文润色",
    "点正文弹出快捷菜单",
    "Ctrl+0-9快捷编辑文件切换",
};
static const int HELP_COUNT = (int)(sizeof(HELP_LINES) / sizeof(HELP_LINES[0]));

// 帮助可见行数(标题下到状态栏之间)。
static int helpMaxVis() {
    // 帮助浮层是界面框架,几何恒按界面字号(本函数被 drawHelpPanel 与 help 的按键处理
    // 两条路调,后者在正文作用域里)。
    FontScope ui(FontRenderer::uiPxHeight());
    return (STATUS_Y - FONT_H - LINE_SPACING + LINE_SPACING - 1) / LINE_SPACING;
}

static void drawHelpPanel() {
    // 帮助浮层是界面框架（会在正文作用域里被调到）。
    FontScope ui(FontRenderer::uiPxHeight());
    UI_FONT_GUARD();
    g_editor.drawnOnce = true;
    ui_clear();
    const int rowH = LINE_SPACING;
    int maxVis = helpMaxVis();
    int maxScroll = HELP_COUNT - maxVis;
    if (maxScroll < 0) maxScroll = 0;
    if (g_editor.helpScroll > maxScroll) g_editor.helpScroll = maxScroll;

    ui_draw_text(4, FONT_H, "快捷键帮助", false, true);
    std::string info = std::to_string(g_editor.helpScroll + 1) + "/" + std::to_string(HELP_COUNT);
    ui_draw_text(SCREEN_W - 4 - g_font.textWidth(info.c_str()), FONT_H, info.c_str());

    for (int i = 0; i < maxVis && (g_editor.helpScroll + i) < HELP_COUNT; i++) {
        ui_draw_text(4, FONT_H + rowH + i * rowH, HELP_LINES[g_editor.helpScroll + i]);
    }

    ui_draw_status("Up/Down滚动 PgUp/PgDn翻页 Esc关闭", "");
    ui_commit();
}

static AppState screen_editor_help_handle(int key, ScreenContext &ctx) {
    (void)ctx;
    auto &g = g_editor;
    int maxVis = helpMaxVis();
    int maxScroll = HELP_COUNT - maxVis;
    if (maxScroll < 0) maxScroll = 0;
    if (key == KEY_HELP || key == 0x1B) {  // Ctrl+? 或 Esc 关闭
        g.helpActive = false;
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }
    if (key == KEY_DOWN || key == 0x0A || key == 0x0D) {
        if (g.helpScroll < maxScroll) g.helpScroll++;
    } else if (key == KEY_UP) {
        if (g.helpScroll > 0) g.helpScroll--;
    } else if (key == KEY_PAGE_DOWN) {
        g.helpScroll += maxVis;
        if (g.helpScroll > maxScroll) g.helpScroll = maxScroll;
    } else if (key == KEY_PAGE_UP) {
        g.helpScroll -= maxVis;
        if (g.helpScroll < 0) g.helpScroll = 0;
    }
    drawHelpPanel();
    return APP_EDITOR;
}

// ── Editor drawing ────────────────────────────────────────────────────────
// 虚拟键盘是否正在挤占正文区。
static bool editorVkActive() {
    return editorVkVisible();
}

// ── 状态栏可点标记（全角/半角 ● ◐、简/繁）──────────────────────────────
// 这两个标记本来就是"当前模式"的显示，点它切模式是最自然的操作。位置按绘制同一套
// 度量算（逐 UTF-8 字符累加 advance），绘制时记录、点按时查，不用第二套坐标。
struct EditorStatusHit {
    int x, y, w, h;   // 命中矩形
    int key;          // KEY_FULLWIDTH_TOGGLE / KEY_TRAD_TOGGLE
};
static std::vector<EditorStatusHit> s_statusHits;

static void buildStatusToggleHits(const std::string &right) {
    s_statusHits.clear();
    if (right.empty()) return;
    int rw = g_font.textWidth(right.c_str());
    int x = SCREEN_W - rw - 4;                       // 与 ui_draw_status 的落点一致
    int y = STATUS_BAR_Y + 1 + (STATUS_BAR_H - g_font.lineHeight()) / 2;
    int h = g_font.lineHeight();
    if (h < 8) h = 8;
    const char *p = right.c_str();
    while (*p) {
        const char *nx = p;
        uint32_t cp = FontRenderer::utf8Decode(nx);
        if (cp == 0) break;
        int cw = g_font.textWidth(std::string(p, nx - p).c_str());
        int key = 0;
        if (cp == 0x25CF || cp == 0x25D0) key = KEY_FULLWIDTH_TOGGLE;   // ● / ◐
        else if (cp == 0x7B80 || cp == 0x7E41) key = KEY_TRAD_TOGGLE;   // 简 / 繁
        // 左右各放宽 4px：这些标记只有十几像素宽，按指尖尺寸给点容差。
        if (key) s_statusHits.push_back({x - 4, y, cw + 8, h, key});
        x += cw;
        p = nx;
    }
}

// 点按命中状态栏标记 → 返回对应的切换键码，否则 0。
static int editorStatusToggleAt(int x, int y) {
    for (const auto &hit : s_statusHits) {
        if (x >= hit.x && x < hit.x + hit.w && y >= hit.y && y < hit.y + hit.h) return hit.key;
    }
    return 0;
}

// 状态栏左端的模式图标。画在 x=4（ui_draw_status 里左端文字的位置），返回它占用的
// 宽度（含右侧间隔）——图标右边还要接文字时（竖排标记）靠它定位。
// 直接写帧缓冲（与主菜单 drawMainMenuIcon 同一条路），不走 u8g2 的绘制原语。
static int editorStatusDrawIcon(uint32_t cp) {
    uint8_t *fb = g_u8g2 ? u8g2_GetBufferPtr(g_u8g2) : nullptr;
    const int box = FONT_H - 8;   // 比行高略小：四周留白，图标才不像顶满格子
    if (!fb || !cp || box < 8) return 0;
    const int y = STATUS_BAR_Y + 1 + (STATUS_BAR_H - box) / 2;
    // invert=false：状态栏是白底黑字，图标也要是黑的（icon 的 invert=true 是"白图标"，
    // 那种画法要求底色已经填黑）。
    icon_font_draw_sized(fb, 4, y, box, box, cp, false, box);
    return box + 6;
}

// ── 状态栏左端的「写作模式」标记 ──────────────────────────────────────────
// 提示写作/自由写作合并成一个「写作」模式后，模式本身不再是个开关，改**点状态栏左端
// 那个标记**：横屏它是"自由写作 / 提示写作"文字，竖屏是同一个位置的图标（见
// editorStatusModeIcon），点一下切模式、长按（提示写作下）走 AI 生成提示词。
// 落在状态栏上，不侵占正文区（先前那个右上角浮动按钮会盖住首行末尾）。
// 几何与状态栏同一套落点（左端 x=4），绘制时记录、点按/长按时查。
static struct {
    int x = 0, y = 0, w = 0, h = 0;
} s_statusModeHit;

static bool editorStatusModeHitAt(int x, int y) {
    if (s_statusModeHit.w <= 0) return false;
    return x >= s_statusModeHit.x && x < s_statusModeHit.x + s_statusModeHit.w &&
           y >= s_statusModeHit.y && y < s_statusModeHit.y + s_statusModeHit.h;
}

// 取一条内置提示词（切到提示写作 / Ctrl+P 补空时用）。
static void editorTakeBuiltinPrompt() {
    g_editor.promptText = BUILTIN_PROMPTS[rand() % BUILTIN_PROMPT_COUNT];
}

// 切换写作模式：**有没有提示词就是模式本身**（见 editorPromptOn）。
// 提示写作 → 自由写作（清空）；自由写作 → 提示写作（补一条内置提示词）。
static void editorToggleWritingMode() {
    if (editorPromptOn()) g_editor.promptText.clear();
    else editorTakeBuiltinPrompt();
}

// 状态栏：右端为虚拟键盘开关图标让位并截断右侧文字，最后补画图标
// (须在 ui_draw_status 之后，否则被状态栏白底盖掉)。
// leftIcon != 0 时左端用图标代替 left 文字：先让 ui_draw_status **不画**左端文字
// （图标是"只画字形"的，压不住底下的汉字，留着会从图标缝里露出来），再在图标右边
// 补画 left——那是图标之外还要保留的短文字（竖排标记）。
// modeTarget = 左端这个是"写作模式"标记（可点切换），记下命中区域供点按/长按查。
static void editorStatusBar(const char *left, std::string right, uint32_t leftIcon = 0,
                            bool modeTarget = false) {
    // 状态栏是界面框架：底边、行高、字号全按**界面字号**，不跟正文一起放大。
    FontScope ui(FontRenderer::uiPxHeight());
    int slot = editorVkIconSlotW();
    if (slot > 0) {
        right = editorVkTruncateToWidth(right, SCREEN_W - slot - 16);
    }
    // 右端留给键盘开关图标：状态栏文字是右对齐的，只按宽度截断挪不开最右边的
    // 电池图标（末尾那几个字形仍旧落在图标槽里）——rightReserve 才是真让位。
    ui_draw_status(leftIcon ? nullptr : left, right.c_str(), slot);
    if (leftIcon) {
        const int w = editorStatusDrawIcon(leftIcon);
        if (w > 0 && left && *left) {
            // 与 ui_draw_status 里左端文字的落点/基线一致，图标右边接着排。
            const int ty = STATUS_BAR_Y + 1 + (STATUS_BAR_H - g_font.lineHeight()) / 2 + g_font.ascent();
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(4 + w, ty, left, false);
            u8g2_SetDrawColor(g_u8g2, 1);
        }
    }
    editorVkDrawIcon();
    // 左端模式标记的命中区域：竖屏是图标（按图标格子算），横屏是文字（按文字宽度算，
    // 再放宽 10px 指尖容差）。高度取整条状态栏——它才十几像素高，不再往上抠。
    s_statusModeHit.w = 0;
    if (modeTarget) {
        s_statusModeHit.x = 0;
        s_statusModeHit.y = STATUS_BAR_Y;
        s_statusModeHit.h = STATUS_BAR_H;
        s_statusModeHit.w = leftIcon ? (FONT_H - 8 + 12) : ((left ? g_font.textWidth(left) : 0) + 10);
    }
    // 记录本次实际画出的全角/简繁标记位置（截断之后的串），供点按使用。
    buildStatusToggleHits(right);
}

// 光标落入折叠区时自动展开最近的折叠标题。放 drawEditor 入口覆盖所有重绘路径
// (主循环尾部、搜索跳转、Ctrl+X/V 早退、idle)。
static void reconcileFoldsForCursor() {
    if (g_editor.foldedHeadings.empty()) return;
    int cy = g_editor.cy;
    const auto &mi = getMdInfo(g_settings.markdownRender());
    if (cy < 0 || cy >= (int)mi.size()) return;
    int hideLevel = 0, foldAt = -1;
    for (int li = 0; li <= cy; li++) {
        int lvl = mi[li].headingLevel;
        bool isH = lvl > 0 && !mi[li].inCodeBlock;
        if (hideLevel != 0 && !(isH && lvl <= hideLevel)) continue;
        if (isH) {
            if (g_editor.foldedHeadings.count(li)) { hideLevel = lvl; foldAt = li; }
            else { hideLevel = 0; foldAt = -1; }
        }
    }
    // foldAt == cy 时光标在折叠标题行本身(可见边界),不算落入折叠区
    if (hideLevel != 0 && foldAt >= 0 && foldAt != cy) {
        g_editor.foldedHeadings.erase(foldAt);
        g_editor.vrowsDirty = true;
    }
}

// 提示词表头折行范围。drawEditor 与 editorVerticalVm 共用,保证表头占用的
// 行数在绘制与竖排布局计算之间一致。
static std::vector<std::pair<const char *, const char *>> promptWrappedRowRanges() {
    std::vector<std::pair<const char *, const char *>> rows;
    if (g_editor.promptText.empty()) return rows;
    const int maxW = SCREEN_W - 8;
    const char *p = g_editor.promptText.c_str();
    while (*p) {
        const char *rowStart = p;
        int rowW = 0;
        while (*p) {
            const char *next = p;
            uint32_t cp = FontRenderer::utf8Decode(next);
            if (cp == 0) { p = next; continue; }
            int cw = g_font.charWidth(cp);
            if (rowW + cw > maxW && rowW > 0) break;
            rowW += cw;
            p = next;
        }
        if (p > rowStart) rows.push_back({rowStart, p});
    }
    return rows;
}

// 竖排布局度量。draw 与左右/PageUp/PageDown 导航必须用同一套参数,否则
// 导航按 STATUS_Y 全高切列、绘制按候选条保留高度切列,列边界错位导致光标跳行。
static VerticalLayoutMetrics editorVerticalVm() {
    int y = FONT_H;
    int pRows = (int)promptWrappedRowRanges().size();
    if (pRows > 0) y += (pRows + 1) * LINE_SPACING;
    bool reserveIME = g_editor.imeActive;
    int contentEndY;
    {
        // 底边这一组(键盘面板顶 / 候选条上沿 / 状态栏上沿)全是**界面字号**下的量,
        // 正文字号改了不该带着它们跑。行高那半边(上面的 y)才是正文字号。
        FontScope ui(FontRenderer::uiPxHeight());
        if (editorVkActive()) {
            // 虚拟键盘面板比 IME 候选条高得多,且自带候选条,正文直接裁到面板顶边。
            contentEndY = editorVkTop();
        } else if (reserveIME) {
            // 候选条底边锚定分割线(276)后,编码行白框上沿 = 265 - 2*字号
            // (22pt:221 / 20pt:225 / 18pt:229,18pt 实测再 +3)。竖排正文下探到编码行上沿
            // 附近,行数随之变化(锚定 STATUS_Y 时更多)。
            const int fs = g_font.fontSize();
            contentEndY = (fs == 18) ? 232 : (265 - 2 * fs);
        } else {
            contentEndY = STATUS_Y;
        }
    }
    // 竖排首字墨迹顶边与横排首行对齐(横排首行基线 y,顶边 y-ascent);
    // 竖排基线 = vm.y + ascent,故 vm.y 取 y-ascent,顶部不留整行空白
    int vTop = y - g_font.ascent();
    return verticalMetrics(6, vTop, SCREEN_W - 12, contentEndY - vTop);
}

// ── 触摸选区的几何 ──────────────────────────────────────────────────────
// 命中测试必须与 drawEditor 的排布逐项对齐（提示词表头占几行、虚拟键盘把正文裁到
// 哪儿），否则手指点到的字和反白出来的字会差一行。两侧共用下面这两个函数。

// 正文第一行的**基线** y（提示词表头之后）。
static int editorBodyTopY() {
    int y = FONT_H;
    if (!g_editor.promptText.empty()) {
        int pRows = (int)promptWrappedRowRanges().size();
        if (pRows > 0) y += (pRows + 1) * LINE_SPACING;
    }
    return y;
}

// 正文区底边：虚拟键盘在时贴键盘顶（按钮条就落在键盘上面那条），否则让给
// IME 候选条 / 状态栏。与 drawEditor 里 contentEndY 的算法一致。
static int editorBodyBottomY() {
    // 整条算式都是**界面字号**下的量(键盘面板顶 / 候选条上沿 / 状态栏上沿),与正文字号
    // 无关。本函数会被正文作用域调到,所以自己钉一次界面字号,谁调用都拿到同一个底边。
    FontScope ui(FontRenderer::uiPxHeight());
    if (editorVkActive()) return editorVkTop();
    if (g_editor.imeActive) {
        if (editorVertical()) {
            const int fs = g_font.fontSize();
            return (fs == 18) ? 232 : (265 - 2 * fs);
        }
        return imeStatusPanelTopY();
    }
    return STATUS_Y;
}

// 正文区一屏能放几行（与 drawEditor 的 visibleVrows 同式）。
static int editorVisibleRows() {
    int n = (editorBodyBottomY() - editorBodyTopY() + LINE_SPACING - 1) / LINE_SPACING;
    if (n < 1) n = 1;
    return n;
}

// 手指 y → vrow 全局下标（不是行号：一条长行会折成多条 vrow）。落空返回 -1。
static int editorVrowAtY(int y) {
    const int top = editorBodyTopY();
    if (y < top - g_font.ascent() || y >= editorBodyBottomY()) return -1;
    int r = (y - (top - g_font.ascent())) / LINE_SPACING;
    if (r < 0 || r >= editorVisibleRows()) return -1;
    return g_editor.scroll + r;
}

// 下一个 UTF-8 边界（跨过当前字符的全部续字节）。不前进就返回原值，调用方须自断。
static int utf8NextBoundary(const std::string &s, int b) {
    const int n = (int)s.size();
    if (b >= n) return n;
    int i = b + 1;
    while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static int utf8PrevBoundary(const std::string &s, int b) {
    int i = b - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return (i < 0) ? 0 : i;
}

// 从 b 处解一个码点（b 必须是边界）。
static uint32_t utf8CpAt(const std::string &s, int b) {
    const unsigned char *p = (const unsigned char *)s.c_str() + b;
    return utf8NextCodepoint(&p);
}

// 词与词之间的分隔：空白 + 各类标点。汉字的**行内**换行机会（utf8IsCjkBreakable）
// 比这里宽得多（汉字本身就可断行），拿来做选词边界会把整句选中，所以另写一份。
static bool editorIsSep(uint32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x3000) return true;
    if (cp < 0x80) {
        if ((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
            (cp >= 'a' && cp <= 'z') || cp == '_') return false;
        return true;   // 其余 ASCII（标点/符号/控制符）都当分隔
    }
    if (cp >= 0x3000 && cp <= 0x303F) return true;   // 、。〈〉《》「」『』【】…
    if (cp >= 0xFF01 && cp <= 0xFF0F) return true;   // ！＂＃…／
    if (cp >= 0xFF1A && cp <= 0xFF20) return true;   // ：；＜＝＞？＠
    if (cp >= 0xFF3B && cp <= 0xFF40) return true;   // ［＼］＾＿｀
    if (cp >= 0xFF5B && cp <= 0xFF65) return true;   // ｛｜｝～｡｢｣､･
    if (cp >= 0x2000 && cp <= 0x206F) return true;   // 通用标点（—‘’“”…–）
    return false;   // 其余非 ASCII 当词字符（汉字/假名/谚文/带重音的拉丁…）
}

// 一条 vrow 内 手指 x → 字节偏移：取 [start, end] 里画出来离 x 最近的边界。
// end 也算一个位置：点行尾右半边要能落到"行末"（否则永远选不到最后一个字之后的空位）。
static bool editorPosInVrow(int vrIdx, int x, TextPos &out) {
    const auto &vrows = getVrows();
    if (vrIdx < 0 || vrIdx >= (int)vrows.size()) return false;
    const VRow &vr = vrows[vrIdx];
    const std::string &line = g_editor.lines[vr.lineIdx];
    const bool mdOn = g_settings.markdownRender();
    const std::vector<MdLineInfo> &mdInfo = getMdInfo(mdOn);
    const MdLineInfo &mdi = mdInfo[vr.lineIdx];
    const bool folded = !g_editor.foldedHeadings.empty() &&
                        g_editor.foldedHeadings.count(vr.lineIdx);
    const int mdCursor = (vr.lineIdx == g_editor.cy) ? g_editor.cx : -1;
    int bestB = vr.start, bestD = 0x7FFFFFFF;
    int b = vr.start;
    for (;;) {
        const int bb = (b > vr.end) ? vr.end : b;
        const int xb = 4 + mdVrowX(line, mdi, bb, vr.start, vr.indentCells, mdCursor, folded);
        const int d = (xb > x) ? (xb - x) : (x - xb);
        if (d < bestD) { bestD = d; bestB = bb; }
        if (bb >= vr.end) break;
        const int next = utf8NextBoundary(line, bb);
        if (next <= bb) break;   // 保险：不前进就停，别死循环
        b = next;
    }
    out.cy = vr.lineIdx;
    out.cx = bestB;
    return true;
}

// 手指点 → 位置。内核是 editorPosInVrow(上面那个)：拆出来是为了屏边自动滚动——
// 那时目标行**还没显示出来**（在视口外一行），拿不到 y，只能直接给 vrow 下标。
static bool editorPosAtPoint(int x, int y, TextPos &out) {
    if (editorVertical()) return false;
    const int vrIdx = editorVrowAtY(y);
    if (vrIdx < 0) return false;
    return editorPosInVrow(vrIdx, x, out);
}

// 落点所在的"词"（两个分隔之间的整段），返回 [wStart, wEnd)。落在分隔/行首尾返回 false。
// 只在**本行**内扩，词不跨行。
static bool editorWordRangeAt(const TextPos &pos, TextPos &wStart, TextPos &wEnd) {
    if (pos.cy < 0 || pos.cy >= (int)g_editor.lines.size()) return false;
    const std::string &line = g_editor.lines[pos.cy];
    if (pos.cx < 0 || pos.cx > (int)line.size()) return false;
    // 点在行尾（cx == size）时看前一个字符：长按行末的空隙应该选到最后一个词。
    int probe = pos.cx;
    if (probe >= (int)line.size()) {
        if (probe == 0) return false;
        probe = utf8PrevBoundary(line, probe);
    }
    if (editorIsSep(utf8CpAt(line, probe))) return false;
    int s = probe, e = probe;
    while (s > 0) {
        const int p = utf8PrevBoundary(line, s);
        if (p == s) break;
        if (editorIsSep(utf8CpAt(line, p))) break;
        s = p;
    }
    while (e < (int)line.size()) {
        if (editorIsSep(utf8CpAt(line, e))) break;
        const int n = utf8NextBoundary(line, e);
        if (n <= e) break;
        e = n;
    }
    if (e <= s) return false;
    wStart = {pos.cy, s};
    wEnd = {pos.cy, e};
    return true;
}

// 端点 → 屏幕坐标 + 它在第几条 vrow 上。**不做视口判断**：端点被滚出视口时 y 就是
// 视口外的值（屏边自动滚动要拿它把柄钉在视口边上，见 editorSelHandleBoxes）。
static bool editorPosToScreenRaw(const TextPos &p, int &x, int &rowTopY, int &vrIdxOut) {
    if (editorVertical()) return false;
    const auto &vrows = getVrows();
    int vrIdx = -1;
    for (int i = 0; i < (int)vrows.size(); i++) {
        if (vrows[i].lineIdx == p.cy && vrows[i].start <= p.cx && p.cx <= vrows[i].end) {
            vrIdx = i;
            break;
        }
    }
    if (vrIdx < 0) return false;
    vrIdxOut = vrIdx;
    const int vis = vrIdx - g_editor.scroll;
    const VRow &vr = vrows[vrIdx];
    const std::string &line = g_editor.lines[vr.lineIdx];
    const std::vector<MdLineInfo> &mdInfo = getMdInfo(g_settings.markdownRender());
    const MdLineInfo &mdi = mdInfo[vr.lineIdx];
    const bool folded = !g_editor.foldedHeadings.empty() &&
                        g_editor.foldedHeadings.count(vr.lineIdx);
    // 与绘制时同一份 mdCursor：标记行的字形位置会随"光标在不在标记里"变。
    const int mdCursor = (vr.lineIdx == g_editor.cy) ? g_editor.cx : -1;
    x = 4 + mdVrowX(line, mdi, p.cx, vr.start, vr.indentCells, mdCursor, folded);
    rowTopY = editorBodyTopY() + vis * LINE_SPACING;
    return true;
}

// 端点 → 屏幕坐标。rowTopY 是该 vrow 的**基线** y（mdDrawVrow 就是按基线画的）。
// 端点不在当前视口里（被滚出去了）返回 false——"光标附近有没有长按"这类判定要的就是
// 这个口径（屏幕外的端点不算"手指附近"）。
static bool editorPosToScreen(const TextPos &p, int &x, int &rowTopY) {
    int vrIdx = -1;
    if (!editorPosToScreenRaw(p, x, rowTopY, vrIdx)) return false;
    const int vis = vrIdx - g_editor.scroll;
    return vis >= 0 && vis < editorVisibleRows();
}

// 手指底下是不是"字"。editorPosAtPoint 在正文区里**永远**能命中（取最近的字界），
// 所以光看它不够——空行、行尾右侧一大片空白它也会给个位置。这里再按屏幕 x 校一次：
// 命中的那个字节画出来的位置离手指不超过半个字宽，才算"点在字上"。
// 用在**双击选词**（editorBeginTouchSelection 内部）和选区会话里"点别处是否顺手挪
// 光标"——这两处都要"确实点在字上"；普通单击落光标不走这里（空行也该落光标）。
static bool editorTapOnGlyph(int x, int y, TextPos &p) {
    if (!editorPosAtPoint(x, y, p)) return false;
    if (p.cy < 0 || p.cy >= (int)g_editor.lines.size()) return false;
    if (g_editor.lines[p.cy].empty()) return false;
    int gx = 0, gy = 0;
    if (!editorPosToScreen(p, gx, gy)) return false;
    return std::abs(gx - x) <= g_font.halfAdvance();
}

// 选区两端的柄，画在反白块的**外面**（头柄在首行上方、尾柄在末行下方），
// 于是是白底黑块——落在反白块里会被 XOR 搅成一团，也分不出是哪一端。
// 24×24：原来是 10×10，两次被反馈"太小、按不准"，一路放大到 24。
static const int ED_SEL_HANDLE = 12;  // 半边长；成品 24×24

// 两个柄的左上角。*sPin / *ePin 告诉调用方这一端**其实在视口外**、被钉在视口边上了
// （画成空心框，不给"抓/拖"以外的含义）。整段两端都取不到位置时（*sx = -1）不画。
static void editorSelHandleBoxes(int *sx, int *sy, int *ex, int *ey, bool *sPin = nullptr,
                                bool *ePin = nullptr) {
    *sx = *ex = -1;
    bool pinS = false, pinE = false;
    if (sPin) *sPin = false;
    if (ePin) *ePin = false;
    if (!g_editor.hasSelection || !g_editor.selTouch) return;
    TextPos s0, e0;
    getSelRange(s0, e0);
    if (s0.cy == e0.cy && s0.cx == e0.cx) return;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0, vs = -1, ve = -1;
    if (!editorPosToScreenRaw(s0, x1, y1, vs)) return;
    if (!editorPosToScreenRaw(e0, x2, y2, ve)) return;
    // 端点被滚出视口：**不能**干脆不画——屏边自动滚动一开，头柄常被滚到屏幕上边之外，
    // 两个柄一起消失的话就再没法接着拖、也抓不住（安卓是把屏幕外那端钉在边上画个小
    // 箭头）。这里同样钉在视口首/末行上，纵向让下面的翻柄逻辑去处理。
    const int visRows = editorVisibleRows();
    if (vs < g_editor.scroll) {
        vs = g_editor.scroll;
        y1 = editorBodyTopY();
        pinS = true;
    } else if (vs >= g_editor.scroll + visRows) {
        vs = g_editor.scroll + visRows - 1;
        y1 = editorBodyTopY() + (visRows - 1) * LINE_SPACING;
        pinS = true;
    }
    if (ve < g_editor.scroll) {
        ve = g_editor.scroll;
        y2 = editorBodyTopY();
        pinE = true;
    } else if (ve >= g_editor.scroll + visRows) {
        ve = g_editor.scroll + visRows - 1;
        y2 = editorBodyTopY() + (visRows - 1) * LINE_SPACING;
        pinE = true;
    }
    if (sPin) *sPin = pinS;
    if (ePin) *ePin = pinE;
    const int hs = ED_SEL_HANDLE * 2;
    const int hx = x1 - 1;
    const int exx = x2 - hs + 1;
    // 头柄默认在首行**上方**（白底黑块落在反白块外面）。但选区首行贴着正文顶边
    // （正文第一行 y1 = FONT_H = 16，减掉 ascent 和柄高就成了负数）时，画出去就是
    // 屏幕外面——看得见、按不到。这时把它翻到首行**下方**（与尾柄同侧），靠 x 分开；
    // 选区很窄、两柄正好叠在一起时，尾柄再往下错一格。
    int syTop = y1 - g_font.ascent() - 2 - hs;
    if (syTop < 1) syTop = y1 + FONT_H - g_font.ascent() + 2;
    int eyTop = y2 + FONT_H - g_font.ascent() + 2;
    if (syTop == eyTop && hx < exx + hs && exx < hx + hs) eyTop = syTop + hs + 2;
    // 兜底：别画到屏幕外（正文贴着底边时尾柄也会越界）。
    if (syTop < 0) syTop = 0;
    if (eyTop < 0) eyTop = 0;
    if (syTop + hs > SCREEN_H) syTop = SCREEN_H - hs;
    if (eyTop + hs > SCREEN_H) eyTop = SCREEN_H - hs;
    *sx = hx; *sy = syTop;
    *ex = exx; *ey = eyTop;
}

// 点在哪个柄上：0 头 / 1 尾 / -1 都不是。
static int editorSelHandleAt(int x, int y) {
    int sx, sy, ex, ey;
    editorSelHandleBoxes(&sx, &sy, &ex, &ey);
    if (sx < 0 || ex < 0) return -1;
    const int SLOP = 10;
    const int hs = ED_SEL_HANDLE * 2;
    if (x >= sx - SLOP && x <= sx + hs + SLOP && y >= sy - SLOP && y <= sy + hs + SLOP) return 0;
    if (x >= ex - SLOP && x <= ex + hs + SLOP && y >= ey - SLOP && y <= ey + hs + SLOP) return 1;
    return -1;
}

// 按住的那一下抓的是哪一端。先认"手指就在柄上/旁边"，再认"落在反白块里"（取近的
// 那端）。认不出来返回 -1 —— 在正文上随便划一下不该把选区改掉。
static int editorSelDragGrab(int x, int y) {
    if (!g_editor.hasSelection || !g_editor.selTouch) return -1;
    int sx, sy, ex, ey;
    editorSelHandleBoxes(&sx, &sy, &ex, &ey);
    if (sx < 0 || ex < 0) return -1;
    const int scx = sx + ED_SEL_HANDLE, scy = sy + ED_SEL_HANDLE;
    const int ecx = ex + ED_SEL_HANDLE, ecy = ey + ED_SEL_HANDLE;
    // 柄本体只有 10px，手指点不了那么准：命中圈放到 ~56×64（同阅读器）。
    const bool onS = std::abs(x - scx) <= 28 && std::abs(y - scy) <= 32;
    const bool onE = std::abs(x - ecx) <= 28 && std::abs(y - ecy) <= 32;
    if (onS && !onE) return 0;
    if (onE && !onS) return 1;
    // 两柄都在圈里：短词（词长不到两个柄宽）和字小的行常这样，两柄本来就叠在一起，
    // 认不出用户想抓哪一端 —— 交给拖动方向定（同下面的 2）。
    if (onS && onE) return 2;
    // 不靠近柄：落在反白块里就取近的那端（用来往里收）。这里用**不做视口判断**的
    // 版本：拖动自动滚动之后端点常常在屏幕外，那些情形下"整片可见区都在反白里"，
    // 用带视口判断的版本会直接 -1，按下去的拖动就白丢了。
    TextPos s0, e0;
    getSelRange(s0, e0);
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0, vDummy = 0;
    if (!editorPosToScreenRaw(s0, x1, y1, vDummy)) return -1;
    if (!editorPosToScreenRaw(e0, x2, y2, vDummy)) return -1;
    const bool inside = y >= y1 - g_font.ascent() && y <= y2 + FONT_H - g_font.ascent() &&
                        x >= x1 - 6 && x <= x2 + g_font.halfAdvance() * 2;
    if (!inside) return -1;
    // 手指压在反白块**里面**（长按选完词最常见的下一步就是接着拖）：不按"离哪端近"
    // 抓，而是交给**拖动方向**定——往右拖抓尾、往左拖抓头（见 editorSelDragStep）。
    // 按近端抓的话，从词里往左拖会抓住头、把词越缩越小，和安卓的"顺着手指出去长"
    // 正好相反。
    return 2;   // 2 = 待定，由方向定
}

// ── 屏边自动滚动（安卓习惯）─────────────────────────────────────────────
// 手指拖出正文区的上下边 → 视口一行一行跟着走，被拖的那一端留在屏边继续往外长。
// **不能自己改 g_editor.scroll**：drawEditor 每帧都要把 scroll 夹到"活动端可见"的
// 位置（见那里 cursorVR 那几句），自己滚的字下一帧就被拉回去。所以做法是把活动端
// 挪到**视口外一行**，让那把夹子替我们滚——正好一步。
//
// 手指压出正文区多远：负数 = 在上边之外，正数 = 在下边之外，0 = 还在正文区里。
static int editorSelDragEdgeDepth() {
    const int top = editorBodyTopY() - g_font.ascent();
    const int bot = editorBodyBottomY();
    if (g_editor.selDragY < top) return -(top - g_editor.selDragY);
    if (g_editor.selDragY >= bot) return g_editor.selDragY - bot + 1;
    return 0;
}

// 屏边自动滚动的端点：视口外**紧邻**的那一行（下边 = scroll+visRows、上边 = scroll-1）
// 上的一个字节。返回 false = 手指还在正文区里，按老样子用 editorPosAtPoint。
static bool editorSelDragEdgeTarget(TextPos &out) {
    const int depth = editorSelDragEdgeDepth();
    if (depth == 0) return false;
    const auto &vrows = getVrows();
    if (vrows.empty()) return false;
    int vrIdx = (depth < 0) ? (g_editor.scroll - 1)
                            : (g_editor.scroll + editorVisibleRows());
    if (vrIdx < 0) vrIdx = 0;
    if (vrIdx > (int)vrows.size() - 1) vrIdx = (int)vrows.size() - 1;
    const VRow &vr = vrows[vrIdx];
    const std::string &line = g_editor.lines[vr.lineIdx];
    int b = (depth < 0) ? vr.start : vr.end;
    // 折行边界上"上一行的 end"和"下一行的 start"是同一个字节，而回溯"活动端在哪一
    // vrow"取的是**第一个**匹配的行——往上时直接用 start 会被算成上一行，活动端就
    // 没出视口、滚动不发生。往行里让一个字。行首(start=0)和空行不受影响。
    if (depth < 0 && b > 0 && b < (int)line.size()) {
        const int n = utf8NextBoundary(line, b);
        if (n > b && n <= vr.end) b = n;
    }
    out.cy = vr.lineIdx;
    out.cx = b;
    return true;
}

// 按端点 p 挪动被拖的那一端。会话里约定 anchor = 起点、cy/cx = 终点，所以头柄动
// anchor、尾柄动 (cy,cx)，各自夹住不许交叉。返回选区是否真的变了。
static bool editorSelDragApply(const TextPos &p) {
    auto &g = g_editor;
    const TextPos cur{g.cy, g.cx};
    if (g.selDrag == 0) {
        const TextPos a = posLess(cur, p) ? p : cur;   // 拖过头就贴住终点
        if (a.cy != g.selAnchorCy || a.cx != g.selAnchorCx) {
            g.selAnchorCy = a.cy;
            g.selAnchorCx = a.cx;
            markDirty();
            g.targetCx = -1;
            return true;
        }
    } else {
        const TextPos a = posLess(cur, p) ? p : TextPos{g.selAnchorCy, g.selAnchorCx};
        if (a.cy != g.cy || a.cx != g.cx) {
            g.cy = a.cy;
            g.cx = a.cx;
            markDirty();
            g.targetCx = -1;
            return true;
        }
    }
    return false;
}

// 空闲那半边的屏边滚动（拖动帧只在**手指移动**时才上报，压在边上不动就只剩这里）。
// 按压得多深滚得越快：边带里 100ms 一行，每多压一个行距减一档，最快 30ms 一行。
static int64_t s_edgeScrollUs = 0;

static bool editorSelDragIdleScroll() {
    auto &g = g_editor;
    if (g.selDrag < 0) return false;
    const int depth = editorSelDragEdgeDepth();
    if (depth == 0) { s_edgeScrollUs = 0; return false; }
    const int64_t now = esp_timer_get_time();
    const int64_t gap = (depth >= 3 * LINE_SPACING) ? 30000
                        : (depth >= LINE_SPACING)   ? 60000
                                                    : 100000;
    if (s_edgeScrollUs != 0 && now - s_edgeScrollUs < gap) return false;
    s_edgeScrollUs = now;
    TextPos p;
    if (!editorSelDragEdgeTarget(p)) return false;
    if (!editorSelDragApply(p)) return false;
    ui_clear();
    drawEditor();
    ui_commit();
    return true;
}

// 拖动一帧（KEY_TOUCH_DRAG）：把被拖的那一端挪到手指底下的位置。返回正在拖的柄
// （-1 = 这一帧没在拖），*changed 表示选区真的变了（没变就别重绘：端点吸附到字，
// 多数拖动帧是空操作）。
// 锚点用**按下点**：hw/input 的第一帧增量就是"按下点→当前点"，从按下点起算，
// 加完这一帧正好落在手指现在的位置。
static int editorSelDragStep(bool &changed) {
    changed = false;
    int ddx = 0, ddy = 0;
    if (!input_drag_xy(&ddx, &ddy)) return -1;
    auto &g = g_editor;
    if (g.selDrag < 0) {
        int px = 0, py = 0;
        if (!input_press_xy(&px, &py)) return -1;   // 认不出按下点就宁可不拖
        g.selDrag = editorSelDragGrab(px, py);
        if (g.selDrag < 0) return -1;               // 这一轮拖动整个丢掉
        g.selDragX = px;
        g.selDragY = py;
        s_edgeScrollUs = 0;   // 新一轮：第一次压到屏边立刻滚，不等间隔
    }
    g.selDragX += ddx;
    g.selDragY += ddy;
    // 抓的是"未定"（手指原本压在反白块里）：按这一帧的**累计位移**定是哪一端——
    // 右/下拖抓尾、左/上拖抓头，也就是"顺着手指长出去"。位移还太小就先不动，
    // 免得一按下去的抖动就把选区改成某一端。
    if (g.selDrag == 2) {
        if (std::abs(ddx) < 8 && std::abs(ddy) < 8) return 2;
        const bool forward = (std::abs(ddx) >= std::abs(ddy)) ? (ddx > 0) : (ddy > 0);
        g.selDrag = forward ? 1 : 0;
    }
    // 手指压在正文区外就是屏边自动滚动（目标行在视口外一行），否则照旧吸附手指底下。
    TextPos p;
    if (!editorSelDragEdgeTarget(p) && !editorPosAtPoint(g.selDragX, g.selDragY, p))
        return g.selDrag;
    changed = editorSelDragApply(p);
    return g.selDrag;
}

static void drawEditorSelHandles() {
    int sx, sy, ex, ey;
    bool sPin = false, ePin = false;
    editorSelHandleBoxes(&sx, &sy, &ex, &ey, &sPin, &ePin);
    if (sx < 0 || ex < 0) return;
    const int hs = ED_SEL_HANDLE * 2;
    // 抓着的那一端画成空心（黑框白心），跟"没抓"的实心块区分开：用户能看出下一次
    // 点词会挪哪一端。正在被拖的那一端同样算"抓着"。被钉在视口边上的那端也是空心
    // ——它其实在屏幕外，实心块会让人以为选区的这一头就画在这儿。
    const bool gs = (g_editor.selGrab == 0 || g_editor.selDrag == 0 || sPin);
    const bool ge = (g_editor.selGrab == 1 || g_editor.selDrag == 1 || ePin);
    u8g2_SetDrawColor(g_u8g2, 0);
    if (gs) u8g2_DrawFrame(g_u8g2, sx, sy, hs, hs);
    else u8g2_DrawBox(g_u8g2, sx, sy, hs, hs);
    if (ge) u8g2_DrawFrame(g_u8g2, ex, ey, hs, hs);
    else u8g2_DrawBox(g_u8g2, ex, ey, hs, hs);
    u8g2_SetDrawColor(g_u8g2, 1);
}

// 触摸选区的浮层：两柄 + 按钮条 + 粘贴板列表。画在正文之后、状态栏之前。
// 按钮条贴正文区底边（虚拟键盘在时就是键盘上面那条，不藏键盘）。
static void drawEditorTouchOverlays() {
    if (g_editor.selTouch) drawEditorSelHandles();
    editMenuSetBottom(editorBodyBottomY());
    if (editMenuActive()) editMenuDraw();
    if (editMenuPickerActive()) editMenuPickerDraw();
}

static void drawEditor() {
    // 正文整块按「显示与版式 → 正文字号」排版：行高、行距、字宽、光标/选区几何一次全对。
    // 界面框架（状态栏 / 输入法条 / 虚拟键盘 / 各种浮层）在**各自函数里**显式钉回界面
    // 字号，所以下面照常调用它们即可，不必在这里进进出出。
    FontScope body(editorBodyFontPx());
    g_editor.drawnOnce = true;
    reconcileFoldsForCursor();
    int y = FONT_H;

    if (!g_editor.promptText.empty()) {
        for (auto &r : promptWrappedRowRanges()) {
            ui_draw_text(4, y, std::string(r.first, r.second - r.first).c_str(), false, true);
            y += LINE_SPACING;
        }
        u8g2_DrawHLine(g_u8g2, 0, y, SCREEN_W);
        y += LINE_SPACING;
    }

    if (editorVertical()) {
        bool composing = g_ime.composing();
        bool vkOn = editorVkActive();
        VerticalLayoutMetrics vm = editorVerticalVm();
        bool mdOn = g_settings.markdownRender();
        mdSetRenderEnabled(mdOn);
        const std::vector<MdLineInfo> &mdInfo = getMdInfo(mdOn);
        auto hidden = mdFoldHiddenLines(g_editor.lines, mdOn ? &mdInfo : nullptr,
                                        &g_editor.foldedHeadings);
        auto data = buildVerticalData(g_editor.lines, vm.rows, &hidden,
                                      mdOn ? &mdInfo : nullptr, &g_editor.foldedHeadings,
                                      g_editor.cy, g_editor.cx);
        int cursorCol = verticalFindCol(data, g_editor.lines, g_editor.cy, g_editor.cx);
        if (editorTypewriter() && cursorCol >= 0) {
            g_editor.scroll = cursorCol - vm.cols / 2;
        } else {
            if (cursorCol < g_editor.scroll) g_editor.scroll = cursorCol;
            if (cursorCol >= g_editor.scroll + vm.cols) g_editor.scroll = cursorCol - vm.cols + 1;
            if (g_editor.scroll < 0) g_editor.scroll = 0;
        }
        int maxScroll = (int)data.cols.size() - vm.cols;
        if (maxScroll < 0) maxScroll = 0;
        if (!editorTypewriter()) {
            if (g_editor.scroll > maxScroll) g_editor.scroll = maxScroll;
        }

        if (composing && !vkOn) drawIMEUIWithStatusBar();
        VerticalGuideStyle guideStyle = VerticalGuideStyle::Solid;
        std::string guideStyleKey = g_settings.verticalReferenceLineStyle();
        if (guideStyleKey == "dash") guideStyle = VerticalGuideStyle::Dash;
        else if (guideStyleKey == "dot") guideStyle = VerticalGuideStyle::Dot;
        drawVerticalCols(g_editor.lines, data, g_editor.scroll, vm,
                         g_settings.verticalReferenceLine(), guideStyle);
        // Markdown 关闭时折叠标题行末补折叠标志;开启时 mdVerticalCells 已含折叠标志格
        if (!mdOn) {
            for (int li : g_editor.foldedHeadings) {
                if (li < 0 || li >= (int)g_editor.lines.size()) continue;
                int colIdx = -1;
                for (int i = 0; i < (int)data.cols.size(); i++)
                    if (data.cols[i].lineIdx == li) colIdx = i;
                if (colIdx < 0 || colIdx < g_editor.scroll || colIdx >= g_editor.scroll + vm.cols)
                    continue;
                const auto &col = data.cols[colIdx];
                if (col.end != (int)data.cells[li].size()) continue;
                int row = col.end - col.start;
                if (row >= vm.rows) continue;
                int ci = colIdx - g_editor.scroll;
                int x = vm.x + vm.w - vm.colAdvance - ci * vm.colAdvance;
                int wpx = g_font.textWidth(kFoldMarker);
                g_font.drawText(x + (g_font.lineHeight() - wpx) / 2,
                                vm.y + row * vm.rowAdvance + g_font.ascent(),
                                kFoldMarker, false);
            }
        }
        if (g_editor.hasSelection) {
            TextPos selStart, selEnd;
            getSelRange(selStart, selEnd);
            drawVerticalHighlight(data, g_editor.scroll, vm, selStart, selEnd);
        }
        drawVerticalCursor(g_editor.lines, data, g_editor.scroll, vm, g_editor.cy, g_editor.cx);

        int wc = getWordCount();
        char left[48];
        uint32_t leftIcon = 0;
        if (inQuickFileSession()) {
            snprintf(left, sizeof(left), "[%d] 竖排", quickEditIndex());
        } else if (uint32_t ic = editorStatusModeIcon()) {
            leftIcon = ic;                         // 提示写作/自由写作 → 图标
            snprintf(left, sizeof(left), "竖排");   // 图标右边保留排版标记
        } else {
            // 标题被覆盖（文件名/历史版本预览）：没图标，文字照旧，竖排标记跟在后面
            snprintf(left, sizeof(left), "%s 竖排", editorStatusTitle());
        }
        std::string imeLabel = imeStatusLabel(g_editor.imeActive);
        std::string right = std::to_string(wc) + "字 " + imeLabel;
        std::string bt = battery_text();   // 编辑器状态栏不显示蓝牙图标，见下
        if (!bt.empty()) right += " " + bt;
        if (vkOn) editorVkDraw();
        drawEditorTouchOverlays();
        // 左端标记可点（点=切模式、提示写作下长按=AI）——快捷文件会话除外，那里
        // Ctrl+P 是"上一个文件"，左端显示的是「[n] 竖排」，跟写作模式无关。
        editorStatusBar(left, right, leftIcon, !inQuickFileSession());
        return;
    }

    const auto& vrows = getVrows();
    bool composing = g_ime.composing();
    bool vkOn = editorVkActive();
    // IME 开启期间恒定保留候选条区域,选字后候选条隐藏不再引起正文重排跳动
    bool reserveIME = g_editor.imeActive;
    // 虚拟键盘面板自带候选条、且比 IME 条高得多,同时显示时以键盘为准。
    // 底边与状态栏上沿都是**界面字号**下的量，正文放大不该带着它们跑；行数 =
    // (底边 - 正文顶 y) / 正文行距。
    int contentEndY, statusY;
    {
        FontScope ui(FontRenderer::uiPxHeight());
        statusY = STATUS_Y;
        contentEndY = vkOn ? editorVkTop()
                           : (reserveIME ? imeStatusPanelTopY() : STATUS_Y);
    }
    int visibleVrows = (contentEndY - y + LINE_SPACING - 1) / LINE_SPACING;
    if (visibleVrows < 1) visibleVrows = 1;

    int cursorVR = -1;
    for (int vi = 0; vi < (int)vrows.size(); vi++) {
        if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
            cursorVR = vi;
            break;
        }
    }

    int normalVisibleVrows = (statusY - y + LINE_SPACING - 1) / LINE_SPACING;
    int effectiveVisibleVrows = vkOn ? visibleVrows
                                     : (reserveIME ? (normalVisibleVrows - 2) : normalVisibleVrows);
    if (effectiveVisibleVrows < 1) effectiveVisibleVrows = 1;

    if (editorTypewriter() && cursorVR >= 0) {
        // 打字机模式:光标始终钉在可视区中间行。居中按「未弹出候选区」的行数
        // (normalVisibleVrows)计算,滚动与 IME 候选面板的弹出/消失无关,光标屏上
        // 位置稳定不动(候选面板压在底部约 2 行,居中光标不受影响)。scroll 不设
        // clamp,文首/文末及短文档经下方绘制循环对越界下标跳过 → 自然留白。
        int rows = normalVisibleVrows; if (rows < 1) rows = 1;
        g_editor.scroll = cursorVR - rows / 2;
        // 极端小屏兜底:候选区几乎占满内容区时,保证光标仍落在候选条上方的可视行,
        // 不因居中而藏到候选面板之下。
        if (composing && cursorVR - g_editor.scroll >= visibleVrows) {
            g_editor.scroll = cursorVR - visibleVrows + 1;
            if (g_editor.scroll < 0) g_editor.scroll = 0;
        }
    } else {
        if (cursorVR < g_editor.scroll) g_editor.scroll = cursorVR;
        if (cursorVR >= g_editor.scroll + effectiveVisibleVrows)
            g_editor.scroll = cursorVR - effectiveVisibleVrows + 1;
        if (g_editor.scroll < 0) g_editor.scroll = 0;
    }

    bool mdOn = g_settings.markdownRender();
    mdSetRenderEnabled(mdOn);
    const std::vector<MdLineInfo> &mdInfo = getMdInfo(mdOn);
    for (int i = 0; i < visibleVrows; i++) {
        int idx = g_editor.scroll + i;
        if (idx < 0 || idx >= (int)vrows.size()) continue;  // 打字机留白行
        auto &vr = vrows[idx];
        int mdCursor = (vr.lineIdx == g_editor.cy) ? g_editor.cx : -1;
        bool folded = !g_editor.foldedHeadings.empty() &&
                      g_editor.foldedHeadings.count(vr.lineIdx);
        mdDrawVrow(4, y + i * LINE_SPACING, g_editor.lines[vr.lineIdx], vr.start, vr.end,
                   mdInfo[vr.lineIdx], vr.indentCells, mdCursor, folded);
    }

    // Selection highlight
    if (g_editor.hasSelection) {
        TextPos selStart, selEnd;
        getSelRange(selStart, selEnd);
        for (int i = 0; i < visibleVrows; i++) {
            int vrIdx = g_editor.scroll + i;
            if (vrIdx < 0 || vrIdx >= (int)vrows.size()) continue;  // 打字机留白行
            auto &vr = vrows[vrIdx];
            int lineIdx = vr.lineIdx;
            int rowStart = vr.start, rowEnd = vr.end;
            if (lineIdx < selStart.cy || lineIdx > selEnd.cy) continue;
            // Calculate overlap of [rowStart, rowEnd) with selection on this line
            int hlStart = rowStart, hlEnd = rowEnd;
            if (lineIdx == selStart.cy) hlStart = std::max(hlStart, selStart.cx);
            if (lineIdx == selEnd.cy) hlEnd = std::min(hlEnd, selEnd.cx);
            if (hlStart >= hlEnd) continue;
            // Highlight range [hlStart, hlEnd) on this vrow
            const MdLineInfo &mdi = mdInfo[lineIdx];
            bool folded = !g_editor.foldedHeadings.empty() &&
                          g_editor.foldedHeadings.count(lineIdx);
            int mdCursor = (lineIdx == g_editor.cy) ? g_editor.cx : -1;
            int xOff = 4 + mdVrowX(g_editor.lines[lineIdx], mdi, hlStart, rowStart,
                                   vr.indentCells, mdCursor, folded);
            int selEndX = 4 + mdVrowX(g_editor.lines[lineIdx], mdi, hlEnd, rowStart,
                                      vr.indentCells, mdCursor, folded);
            int selW = selEndX - xOff;
            int ly = y + i * LINE_SPACING;
            u8g2_SetDrawColor(g_u8g2, 2);  // XOR mode
            u8g2_DrawBox(g_u8g2, xOff, ly - g_font.ascent(), selW, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 1);  // restore
        }
    }

    if (cursorVR >= 0 && cursorVR >= g_editor.scroll && cursorVR < g_editor.scroll + visibleVrows) {
        auto &vr = vrows[cursorVR];
        const std::string &line = g_editor.lines[vr.lineIdx];
        const MdLineInfo &mdi = mdInfo[vr.lineIdx];
        bool folded = !g_editor.foldedHeadings.empty() &&
                      g_editor.foldedHeadings.count(vr.lineIdx);
        int cx = 4 + mdVrowX(line, mdi, g_editor.cx, vr.start, vr.indentCells,
                             g_editor.cx, folded);
        int cy_draw = y + (cursorVR - g_editor.scroll) * LINE_SPACING;
        int cw = g_font.halfAdvance();
        if (g_editor.cx < (int)line.length()) {
            const char *cp = line.c_str() + g_editor.cx;
            unsigned char b = (unsigned char)*cp;
            std::string oneChar;
            if (b < 0x80) oneChar = line.substr(g_editor.cx, 1);
            else if ((b & 0xE0) == 0xC0) oneChar = line.substr(g_editor.cx, 2);
            else if ((b & 0xF0) == 0xE0) oneChar = line.substr(g_editor.cx, 3);
            else if ((b & 0xF8) == 0xF0) oneChar = line.substr(g_editor.cx, 4);
            if (!oneChar.empty()) cw = g_font.textWidth(oneChar.c_str());
        }
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, cx, cy_draw + 4, cw, 3);
        u8g2_SetDrawColor(g_u8g2, 1);
    }

    drawEditorTouchOverlays();

    if (composing && !vkOn) drawIMEUIWithStatusBar();

    int wc = getWordCount();
    char left[48];
    // 横排状态栏够宽（1216px），照旧写文字"提示写作 / 自由写作"；换成图标的是竖屏那条路
    // （见 editorStatusModeIcon 的说明）。
    if (inQuickFileSession()) snprintf(left, sizeof(left), "[%d]", quickEditIndex());
    else snprintf(left, sizeof(left), "%s", editorStatusTitle());
    std::string imeLabel = imeStatusLabel(g_editor.imeActive);
    std::string right = std::to_string(wc) + "字 " + imeLabel;
    // 状态栏只报**本机**电量：蓝牙图标（连通与否的外显）在编辑器里没意义——键盘就在
    // 手边写字，连没连上一敲就知道，反而占着右端。键盘快没电时另有一次性居中提示
    // （bt_keyboard 的 takeLowBatteryWarning，main.cpp 那支），不看状态栏也漏不掉。
    std::string bt = battery_text();
    if (!bt.empty()) right += " " + bt;

    if (vkOn) editorVkDraw();
    // 同竖屏那条路：横屏左端是"自由写作/提示写作"文字，一样可点、提示写作下可长按。
    editorStatusBar(left, right, 0, !inQuickFileSession());
}

static void drawConfirmDialog() {
    ui_draw_confirm_dialog("是否保存当前内容？", "Enter=保存", "ESC=放弃");
}

static void drawRecoveryDialog() {
    ui_draw_confirm_dialog("发现未保存草稿", "Enter=恢复", "ESC=忽略");
}

static void drawPromptGenerating() {
    ui_clear();
    ui_show_message_centered("AI生成提示中...");
    ui_draw_status("请稍候", "");
    ui_commit();
}

static void runPromptTask(void *arg) {
    (void)arg;
    bool wifiWasConnected = g_wifi.isConnected();
    DeepseekResult result = {false, ""};
    if (ensure_wifi_connected()) {
        result = g_deepseek.generatePrompt(s_promptTaskContext);
    } else {
        result = {false, "WiFi连接失败"};
    }
    restore_wifi_state(wifiWasConnected);
    lockPromptResult();
    s_promptTaskResult = result;
    unlockPromptResult();
    s_promptTaskDone = true;
    vTaskDelete(nullptr);
}

// 触发 AI 提示词生成（后台任务 + "正在生成…"浮层）。原来挂在 Ctrl+P 上；
// 提示写作/自由写作合并后改由**长按状态栏左端的"提示写作"标记**触发
// （Ctrl+P 和单击那个标记都只是切模式）。ctx 只在起任务失败时报一句状态。
static void startAiPromptGeneration(ScreenContext &ctx) {
    std::string exp = g_settings.personalExperience();
    std::string hob = g_settings.personalHobbies();
    s_promptTaskContext.clear();
    if (!exp.empty()) s_promptTaskContext += "我的经历:" + exp + ";";
    if (!hob.empty()) s_promptTaskContext += "我的爱好:" + hob + ";";
    if (s_promptTaskContext.empty()) s_promptTaskContext = "一个普通用户";
    lockPromptResult();
    s_promptTaskResult = {false, ""};
    unlockPromptResult();
    s_promptTaskDone = false;
    g_editor.promptGenerating = true;
    TaskHandle_t h = nullptr;
    if (xTaskCreate(runPromptTask, "prompt_gen", 8192, nullptr, 1, &h) != pdPASS) {
        g_editor.promptGenerating = false;
        ctx.statusMessage = "系统繁忙,请重试";
        ctx.statusDuration = 30;
        ui_clear(); drawEditor(); ui_commit();
        return;
    }
    drawPromptGenerating();
}

// ── Editor save helper ────────────────────────────────────────────────────
static bool saveCurrentContent(bool createHistory = true) {
    std::string text = currentEditorText();
    if (inQuickFileSession()) {
        // 快捷编辑: 直接写回 /sdcard/{n}.txt, 允许保存空文件
        bool ok = quickEditSave(quickEditIndex(), text, createHistory);
        if (ok) g_journal.clearRecoveryDraft();
        return ok;
    }
    if (text.empty()) {
        ESP_LOGW("Editor", "save skipped: empty text");
        return false;
    }

    time_t now; time(&now); struct tm *tm = localtime(&now);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    int wc = getWordCount();
    std::string headerStr; headerStr.resize(128);
    int hlen = snprintf(&headerStr[0], 128, "日期: %s\n字数: %d\n\n", ts, wc);
    headerStr.resize(hlen);
    std::string fullText;
    if (editorPromptOn())
        fullText = headerStr + "提示词: " + g_editor.promptText + "\n\n" + text;
    else
        fullText = headerStr + "自由写作\n\n" + text;

    if (g_editor.savedFilename.empty()) {
        char fname[32];
        strftime(fname, sizeof(fname), "%Y-%m-%d_%H%M%S", tm);
        g_editor.savedFilename = std::string(fname) + ".txt";
    }
    bool ok = g_journal.saveEntryRaw(g_editor.savedFilename, fullText, createHistory);
    if (ok) g_journal.clearRecoveryDraft();
    return ok;
}

static AppState finishEditor(ScreenContext &ctx) {
    g_editor.modifiedSinceSave = false;
    if (saveCurrentContent()) {
        ctx.nextState = ctx.prevState;
        return ctx.prevState;
    }
    ctx.statusMessage = "保存失败，请检查SD卡";
    ctx.nextState = ctx.prevState;
    return ctx.prevState;
}

// ── 快捷菜单（单击正文弹出）─────────────────────────────────────────────
// 点正文空白以前是"插一个回车"，现在弹这个菜单——回车本身虚拟键盘和物理键盘
// 都还按得到，而这些动作原先只有背得下快捷键（Ctrl+F/Ctrl+Q、BOOT 双击）才够得着。
// 注意：这里**没有**「语音听写」——Read Pico 板上没有麦克风也没有音频编解码器，
// 整个语音听写栈（voice_input / screen_voice / 设置里的识别服务）已移除。
// 将来真有麦了再把那一套加回来。
static const char *kEditorMenuItems[] = {"全文润色", "发送到Flomo", "二维码", "保存并返回"};
static const int kEditorMenuCount = 4;
// 行高随字号变（FONT_H 是运行时宏），所以只能现算，不能存成 static const。
// 快捷菜单是界面框架(浮在正文之上),几何恒按界面字号——它也被正文作用域里的触摸
// 命中路径调到,所以在这里钉回,不让菜单随正文字号一起长。
static int editorMenuRowH() { FontScope ui(FontRenderer::uiPxHeight()); return FONT_H + 12; }
static int editorMenuTitleH() { FontScope ui(FontRenderer::uiPxHeight()); return FONT_H + 10; }

static void editorQrOpen();
static void drawEditorQr();

// 菜单框几何：居中，宽按最长项算。绘制与命中都用这一份，免得两处各算一遍错位。
static void editorMenuRect(int &x, int &y, int &w, int &h) {
    int maxW = 0;
    for (int i = 0; i < kEditorMenuCount; i++) {
        int tw = g_font.textWidth(kEditorMenuItems[i]);
        if (tw > maxW) maxW = tw;
    }
    w = maxW + 32;
    if (w > SCREEN_W - 16) w = SCREEN_W - 16;
    h = editorMenuTitleH() + editorMenuRowH() * kEditorMenuCount + 10;
    x = (SCREEN_W - w) / 2;
    y = (SCREEN_H - h) / 2;
}

// 菜单行 → 索引；返回 -1 表示点在框外。
static int editorMenuRowAt(int tx, int ty) {
    int x, y, w, h;
    editorMenuRect(x, y, w, h);
    if (tx < x || tx >= x + w || ty < y || ty >= y + h) return -1;
    int r = (ty - (y + editorMenuTitleH())) / editorMenuRowH();
    if (r < 0 || r >= kEditorMenuCount) return -1;
    return r;
}

// 浮在已画好的编辑器之上（调用方负责 ui_clear + drawEditor）。
static void drawEditorMenuBox() {
    // 快捷菜单浮层：界面字号（它会在正文作用域里被调到）。
    FontScope ui(FontRenderer::uiPxHeight());
    UI_FONT_GUARD();
    int x, y, w, h;
    editorMenuRect(x, y, w, h);
    // 3px 黑框 + 白底：正文在下面，这里把面板范围挖白，文字才点得清。
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, x, y, w, h);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, x + 3, y + 3, w - 6, h - 6);
    u8g2_SetDrawColor(g_u8g2, 0);
    ui_draw_text(x + 10, y + FONT_H, "快捷菜单", false, true);
    for (int i = 0; i < kEditorMenuCount; i++) {
        int ry = y + editorMenuTitleH() + i * editorMenuRowH() + FONT_H;
        ui_draw_text(x + 12, ry, kEditorMenuItems[i], i == g_editor.menuSel);
    }
}

static void editorMenuOpen() {
    g_editor.menuActive = true;
    g_editor.menuSel = 0;
}

static AppState screen_editor_menu_handle(int key, ScreenContext &ctx) {
    auto &g = g_editor;
    if (key == 0x1B) {
        g.menuActive = false;
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }
    if (key == KEY_UP) {
        g.menuSel = (g.menuSel + kEditorMenuCount - 1) % kEditorMenuCount;
    } else if (key == KEY_DOWN) {
        g.menuSel = (g.menuSel + 1) % kEditorMenuCount;
    } else if (key == '\n') {
        int tx = 0, ty = 0;
        if (input_tap_xy(&tx, &ty)) {
            int row = editorMenuRowAt(tx, ty);
            if (row < 0) {  // 点在框外：跟弹窗一样收起来
                g.menuActive = false;
                ui_clear(); drawEditor(); ui_commit();
                return APP_EDITOR;
            }
            g.menuSel = row;
        }
        switch (g.menuSel) {
        case 0:  // 全文润色（与 BOOT 双击同一条路）
            g.menuActive = false;
            screen_polish_set_scope(POLISH_WHOLE);
            return APP_POLISH;
        case 1:  // 发送到 Flomo（与 Ctrl+F 同一条路：不带 text，main 会取编辑区全文）
            g.menuActive = false;
            ctx.nextState = APP_SYNC_SEND_FLOMO;
            return APP_SYNC_SEND_FLOMO;
        case 2:  // 二维码
            g.menuActive = false;
            editorQrOpen();
            drawEditorQr();
            return APP_EDITOR;
        case 3: {  // 保存并返回
            g.menuActive = false;
            const bool hasContent =
                g.lines.size() > 1 || (g.lines.size() == 1 && !g.lines[0].empty());
            // 空文档直接走：saveCurrentContent() 对空正文是"保存失败"，会把"没内容"
            // 报成 SD 卡错误。
            if (!hasContent) {
                ctx.nextState = ctx.prevState;
                return ctx.prevState;
            }
            return finishEditor(ctx);
        }
        default:
            break;
        }
    }
    ui_clear(); drawEditor(); drawEditorMenuBox(); ui_commit();
    return APP_EDITOR;
}

// ── 二维码（编辑区全文）─────────────────────────────────────────────────
// 与阅读菜单里的「二维码」同源（同一个 qrcodegen），区别只是内容取编辑区全文。
// 二维码 v40-L 字节模式约 2953 字节，长文必须截断——按 UTF-8 边界切，别切碎字。
static const size_t kEditorQrMaxBytes = 2500;

static void editorQrOpen() {
    auto &g = g_editor;
    std::string text = currentEditorText();
    g.qrBytes = (int)text.size();
    g.qrTruncated = false;
    if (text.size() > kEditorQrMaxBytes) {
        size_t cut = kEditorQrMaxBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) cut--;
        text.resize(cut);
        g.qrTruncated = true;
    }
    g.qrText = std::move(text);
    g.qrActive = true;
    g.drawnOnce = true;  // 防 idle tick 拿 drawEditor() 把它盖掉
}

static void drawEditorQr() {
    // 二维码页是界面框架（会在正文作用域里被调到）。
    FontScope ui(FontRenderer::uiPxHeight());
    UI_FONT_GUARD();
    g_editor.drawnOnce = true;
    ui_clear();
    ui_draw_text(4, FONT_H, "二维码", false, true);
    const int top = 2 * FONT_H;
    const int availH = STATUS_BAR_Y - top - 6;
    const int box = std::min(SCREEN_W - 16, availH);
    if (box <= 0) {
        ui_draw_status("任意键返回", "");
        ui_commit();
        return;
    }
    const int cap = qrcodegen_BUFFER_LEN_FOR_VERSION(40);
    static std::vector<uint8_t> temp, qr;  // 静态：v40 的两块缓冲各 ~4KB，别放栈上
    temp.resize(cap);
    qr.resize(cap);
    if (g_editor.qrText.empty() ||
        !qrcodegen_encodeText(g_editor.qrText.c_str(), temp.data(), qr.data(),
                              qrcodegen_Ecc_LOW, 4, 40, qrcodegen_Mask_AUTO, true)) {
        ui_draw_text_centered(SCREEN_H / 2, "编辑区没有内容");
        ui_draw_status("任意键返回", "");
        ui_commit();
        return;
    }
    const int size = qrcodegen_getSize(qr.data());
    int px = box / size;
    if (px < 1) px = 1;
    if (px > 8) px = 8;
    const int dim = size * px;
    const int x0 = (SCREEN_W - dim) / 2;
    const int y0 = top + (availH - dim) / 2;
    u8g2_SetDrawColor(g_u8g2, 0);
    for (int cy = 0; cy < size; cy++)
        for (int cx = 0; cx < size; cx++)
            if (qrcodegen_getModule(qr.data(), cx, cy))
                u8g2_DrawBox(g_u8g2, x0 + px * cx, y0 + px * cy, px, px);
    std::string hint = "任意键返回";
    if (g_editor.qrTruncated) {
        hint = "内容过长，仅编码前 " + std::to_string(kEditorQrMaxBytes) + " 字节（共 " +
               std::to_string(g_editor.qrBytes) + "）";
    }
    ui_draw_status(hint.c_str(), "");
    ui_commit();
}

static AppState screen_editor_qr_handle(int key, ScreenContext &ctx) {
    (void)key;
    (void)ctx;
    g_editor.qrActive = false;
    ui_clear(); drawEditor(); ui_commit();
    return APP_EDITOR;
}

// ── Screen entry points ──────────────────────────────────────────────────
void screen_editor_init(ScreenContext &ctx) {
    // 编辑器整个界面按**正文字号**跑（见 screen_editor_handle 处的说明）。
    FontScope body(editorBodyFontPx());
    g_editor.lines.clear();
    g_editor.autoSaveTime = 0;
    g_editor.savedFilename = ctx.editFilename;

    // 快捷编辑主会话: 直接加载 /sdcard/{n}.txt; 有传入内容(灵感/日记编辑)时不走快捷文件
    bool quickFile = g_quickEdit && ctx.editContent.empty() && g_editor.savedFilename.empty();
    if (quickFile) {
        loadQuickEditFile();
    } else if (!ctx.editContent.empty()) {
        size_t pos = 0;
        while (pos < ctx.editContent.length()) {
            size_t nl = ctx.editContent.find('\n', pos);
            g_editor.lines.push_back((nl == std::string::npos) ? ctx.editContent.substr(pos) : ctx.editContent.substr(pos, nl - pos));
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        while (g_editor.lines.size() > 1 && g_editor.lines.back().empty())
            g_editor.lines.pop_back();
        g_editor.cx = (int)g_editor.lines.back().length();
        g_editor.cy = (int)g_editor.lines.size() - 1;
        ctx.editContent.clear();
    } else {
        g_editor.lines.push_back("");
        g_editor.cx = g_editor.cy = 0;
    }

    g_editor.scroll = 0;
    g_editor.targetCx = -1;
    // 一进来（非快捷文件）就是中文态：这个入口本来就是拿来写中文的，每次还得先点
    // 一下「中」太别扭。快捷文件保持原状，不改既有习惯。
    // （提示写作/自由写作合并后，进入的都是"没有提示词"那一态——提示词由 Ctrl+P 或
    //   状态栏那个模式标记带出来，所以这里不再按模式分。）
    const bool freeWriting = !quickFile;
    g_editor.imeActive = freeWriting;
    g_ime.setActive(freeWriting);
    g_ime.setFullwidth(false);
    g_ime.setEnglish(false);
    g_editor.confirmSave = false;
    g_editor.modifiedSinceSave = false;
    g_editor.drawnOnce = false;
    g_editor.menuActive = false;
    g_editor.menuSel = 0;
    g_editor.qrActive = false;
    g_editor.qrText.clear();
    g_editor.qrTruncated = false;
    g_editor.qrBytes = 0;
    // 触摸选区会话与选区一起清掉：以前 init 从没清过 hasSelection（那时选区只影响
    // 绘制，不留痕），现在它带着按钮条/两柄，留着就会在重进编辑器时冒出来。
    g_editor.hasSelection = false;
    g_editor.selTouch = false;
    g_editor.selGrab = -1;
    g_editor.selDrag = -1;
    g_editor.selDragActive = false;
    editMenuClose();
    editMenuClosePicker();
    markDirty();
    g_editor.promptText = quickFile ? std::string() : ctx.promptText;
    g_editor.titleOverride = ctx.editorTitle;
    ctx.editFilename.clear();
    ctx.editorTitle.clear();

    // 重置查找/替换对话框(重开编辑器时关闭)
    g_editor.search.active = false;
    g_editor.search.imeActive = false;
    g_editor.search.matches.clear();
    g_editor.search.cur = -1;

    // 重置快捷键帮助对话框
    g_editor.helpActive = false;
    g_editor.helpScroll = 0;

    // 虚拟键盘：未连蓝牙键盘则默认显示(用户手动开关过则以手动为准)
    editorVkInit();
    clearUndoHistory();

    std::string recoveryContent, recoveryMeta;
    if (g_settings.recoveryDraft() && g_journal.loadRecoveryDraft(recoveryContent, recoveryMeta)) {
        g_editor.recoveryPrompt = true;
        g_editor.recoveryContent = recoveryContent;
        g_editor.recoveryMeta = recoveryMeta;
    } else {
        g_editor.recoveryPrompt = false;
        g_editor.recoveryContent.clear();
        g_editor.recoveryMeta.clear();
    }
    g_editor.lastRecoveryHash = 0;
    g_editor.promptGenerating = false;
    s_promptTaskDone = false;
}

// 键盘整块换图样（键位布局、中/英）后的重绘。这两种切换会把键位、键帽标签、
// 候选条一次全换掉，局刷擦不干净旧键框，残影正好压在眼睛盯着的键盘上。
// ui_invalidate_snapshot() 丢掉帧快照后，ui_commit 走"没有快照"的首帧路径 ——
// 一次整屏 GC16，没有第二次刷屏（不是 ui_commit + ui_full_refresh_now 那种刷两遍）。
static void editorRedrawFull() {
    ui_clear();
    drawEditor();
    ui_invalidate_snapshot();
    ui_commit();
}

// 虚拟键盘按键反馈：点一下键盘立刻响一声，并且把紧随其后的"上屏音"吃掉 —— 同一个
// 按键不该响两次。中文组合期间字母键要按好几下才出候选，只在 commit 时响的话键盘
// 像坏的；英文/数字直接就上屏，那时这一声同时也就是上屏音。
// 每次按键事件进来先清零，下面各条上屏分支都读它。
static bool s_vkAteCommitClick = false;

// ── 触摸选区会话 ────────────────────────────────────────────────────────
// 长按正文 → 选词 + 底部按钮条；拖两柄调边界；按钮条上复制/剪切/粘贴/全选/润色。
// "复制完在光标附近长按即可粘贴"就是同一个入口：长按落在词上 → 编辑按钮条，落在
// 空白/标点上（光标附近通常是空白）→ 只有粘贴/全选/取消。

// 全选（与 Ctrl+A 同一条路）。
static void editorSelectAll() {
    auto &g = g_editor;
    if (g.lines.size() == 1 && g.lines[0].empty()) return;   // 空文档不建空选区
    g.selAnchorCy = 0;
    g.selAnchorCx = 0;
    g.cy = (int)g.lines.size() - 1;
    g.cx = (int)g.lines[g.cy].length();
    g.hasSelection = true;
    g.targetCx = -1;
    markDirty();
}

// ── 剪贴板动作：触摸按钮条与实体键盘快捷键**共用**这一份 ──────────────────
// 入口有两个（按钮条点一下 / Ctrl+C），落点只有一个：同一个函数、同一份粘贴板。
// 复制与剪切只差一个 deleteSelection，所以合成一个带 cut 开关的函数。
static void editorActionCopy(ScreenContext &ctx, bool cut) {
    (void)ctx;   // 提示改走 ui_toast，不再回 ctx.statusMessage
    if (!g_editor.hasSelection) return;
    clipboardPush(getSelectedText());
    if (cut) deleteSelection();
    else clearSelection();
    const bool trunc = clipboardLastTruncated();
    // 就地轻提示（不走 ctx.statusMessage 那条全屏消息通道）。
    ui_toast_show(cut ? (trunc ? "已剪切（超长已截断）" : "已剪切")
                      : (trunc ? "已复制（超长已截断）" : "已复制"),
                  editorBodyBottomY());
}

// 粘贴一段文本：有选区则整体替换，否则插到光标处（editorReplaceSelection 自带
// 拆行 + undo；旧的 g_clipboard 直接 insert 会把多行挤成一行，这里一并修好）。
static void editorActionPasteText(const std::string &text) {
    if (text.empty()) return;
    editorReplaceSelection(text);
    clearSelection();
    // 一处收口：Ctrl+V、按钮条的「粘贴」、粘贴板列表挑一条，都从这里过。
    ui_toast_show("已粘贴", editorBodyBottomY());
}

// 「粘贴」按钮 / Ctrl+V 的入口：空板报一句；只有一条直接粘（复制完就地粘贴的
// 主路径，不该多弹一层列表）；多条才开历史列表让挑。
// 返回 true = 已经粘完，false = 开了列表（调用方别再把会话收掉）。
static bool editorActionPaste(ScreenContext &ctx) {
    if (clipboardEmpty()) {
        ctx.statusMessage = "粘贴板是空的";
        ctx.statusDuration = 30;
        return true;
    }
    if (clipboardCount() == 1) {
        editorActionPasteText(clipboardLatest());   // 提示在里面统一发
        return true;
    }
    return false;   // 多条 → 开列表
}

static void editorEndTouchSelection() {
    auto &g = g_editor;
    editMenuClose();
    editMenuClosePicker();
    g.selTouch = false;
    g.selDrag = -1;
    g.selGrab = -1;
    g.selDragActive = false;
}

// 长按正文：
//   · 落在**词**上 → 选它 + 弹「编辑」按钮条（这是进选区的唯一入口）；
//   · 落在**光标附近**的空白/标点上 → 只弹「粘贴/全选/取消」，不选字、光标不动
//     （"复制完在光标附近长按可粘贴"）；
//   · 别处的空白、以及正文区外面（状态栏、提示词表头、虚拟键盘）→ 返回 false，
//     交回老语义（长按 = 返回）。满屏随手一按就弹菜单的话，返回手势就没地方长了。
static bool editorBeginTouchSelection(int x, int y) {
    auto &g = g_editor;
    if (editorVkActive() && y >= editorVkTop()) return false;
    const int top = editorBodyTopY();
    if (y < top - g_font.ascent() || y >= editorBodyBottomY()) return false;

    TextPos p, ws, we;
    const bool onWord = editorPosAtPoint(x, y, p) && editorWordRangeAt(p, ws, we);
    if (!onWord) {
        // 竖排没有选词也没有两柄（见 §竖排），正文里的长按只有"弹粘贴菜单"这一条
        // 路可走，所以照旧给它；横排则要求手指压在**光标那一行**、且横向离光标不超过
        // 6 个半字宽——"光标附近"就是这个圈。再往外就是"空白处"，留给返回手势。
        bool nearCursor = editorVertical();
        if (!nearCursor) {
            int cxScr = 0, cyScr = 0;
            if (editorPosToScreen(TextPos{g.cy, g.cx}, cxScr, cyScr)) {
                const int radX = g_font.halfAdvance() * 6;
                const int radY = FONT_H / 2;
                nearCursor = y >= cyScr - g_font.ascent() - radY &&
                             y <= cyScr + FONT_H - g_font.ascent() + radY &&
                             x >= cxScr - radX && x <= cxScr + radX;
            }
        }
        if (!nearCursor) return false;
    }

    // 有未上屏的 IME 组合先取消：会话里所有按键都从下面那条专用分支走，不会经过
    // IME，但取消一下才不会让组合悬着（下一个键不再可能把它上屏）。
    g_ime.cancelComposition();
    g.selDrag = -1;
    g.selGrab = -1;
    g.selDragActive = false;
    g.selTouch = true;

    if (onWord) {
        // 会话里约定 anchor = 起点、cy/cx = 终点（见 EditorState 里的说明）。
        g.selAnchorCy = ws.cy;
        g.selAnchorCx = ws.cx;
        g.cy = we.cy;
        g.cx = we.cx;
        g.hasSelection = true;
        g.targetCx = -1;
        markDirty();
        editMenuOpen(EM_EDIT);
    } else {
        // 标点/空白/竖排：不建选区（竖排本批只有菜单里的"全选"可用）。
        clearSelection();
        editMenuOpen(EM_PASTE_ONLY);
    }
    editorRedrawFull();
    return true;
}

// 执行按钮条上的一项。EM_EDIT = {复制,剪切,粘贴,全选,润色}；EM_PASTE_ONLY = {粘贴,全选,取消}。
// **每一项都落到与实体键盘快捷键同一个函数上**（复制/剪切 → editorActionCopy，
// 粘贴 → editorActionPasteText，全选 → editorSelectAll，润色 → POLISH_SELECTION）：
// 触摸与键盘共用一套机制、一份粘贴板。
static AppState editorSelMenuActivate(int act, ScreenContext &ctx) {
    const EditMenuKind kind = editMenuKind();

    if (kind == EM_EDIT) {
        switch (act) {
        case 0:   // 复制
            editorActionCopy(ctx, false);
            editorEndTouchSelection();
            editorRedrawFull();
            return APP_EDITOR;
        case 1:   // 剪切
            editorActionCopy(ctx, true);
            editorEndTouchSelection();
            editorRedrawFull();
            return APP_EDITOR;
        case 2:   // 粘贴 → 只有一条直接粘；多条开历史列表挑（列表首项就是最近的）
            if (editorActionPaste(ctx)) {
                editorEndTouchSelection();
                editorRedrawFull();
            } else {
                editMenuOpenPicker();
                editorRedrawFull();
            }
            return APP_EDITOR;
        case 3:   // 全选
            editorSelectAll();
            editorEndTouchSelection();
            editorRedrawFull();
            return APP_EDITOR;
        case 4:   // 润色 → 与 Ctrl+O 同一条路（预览确认后才替换选区）
            editorEndTouchSelection();
            screen_polish_set_scope(POLISH_SELECTION);
            return APP_POLISH;
        default:
            break;
        }
        return APP_EDITOR;
    }

    // EM_PASTE_ONLY
    switch (act) {
    case 0:   // 粘贴 → 只有一条直接粘；多条开历史列表挑
        if (editorActionPaste(ctx)) {
            editorEndTouchSelection();
            editorRedrawFull();
        } else {
            editMenuOpenPicker();
            editorRedrawFull();
        }
        return APP_EDITOR;
    case 1:   // 全选
        editorSelectAll();
        editorEndTouchSelection();
        editorRedrawFull();
        return APP_EDITOR;
    default:  // 取消
        editorEndTouchSelection();
        editorRedrawFull();
        return APP_EDITOR;
    }
}

// 选区会话里的按键。返回 APP_EDITOR；*consumed = false 表示这一键不归选区管，
// 调用方接着走常规分支（降级是安全的：常规分支对"有选区"本来就是先删再插，
// 方向键本来也会先清选区）。
static AppState screen_editor_selection_handle(int key, ScreenContext &ctx, bool &consumed) {
    auto &g = g_editor;
    consumed = true;

    // ① 粘贴板列表开着：它是模态的，全给它。
    if (editMenuPickerActive()) {
        if (key == KEY_UP || key == KEY_PAGE_UP) {
            editMenuPickerMove(-1);
            ui_clear(); drawEditor(); ui_commit();
        } else if (key == KEY_DOWN || key == KEY_PAGE_DOWN) {
            editMenuPickerMove(+1);
            ui_clear(); drawEditor(); ui_commit();
        } else if (key == KEY_TOUCH_DRAG) {
            // 拖动帧不该从列表底下穿到正文上。
        } else if (key == 0x1B) {
            editMenuClosePicker();
            editorRedrawFull();
        } else if (key == '\n') {
            int tx = 0, ty = 0;
            int idx = -1;
            if (input_tap_xy(&tx, &ty)) idx = editMenuPickerHit(tx, ty);
            else idx = editMenuPickerSel();   // 实体/蓝牙键盘回车：粘当前高亮那条
            editMenuClosePicker();
            if (idx >= 0) {
                const std::string item = clipboardAt(idx);
                if (!item.empty()) editorActionPasteText(item);   // 与 Ctrl+V 同一个函数
            }
            editorEndTouchSelection();
            editorRedrawFull();
        }
        return APP_EDITOR;
    }

    // ② 拖动帧：把被拖的那一端挪到手指底下。
    if (key == KEY_TOUCH_DRAG) {
        bool changed = false;
        if (editorSelDragStep(changed) >= 0) {
            g.selDragActive = true;
            if (changed) { ui_clear(); drawEditor(); ui_commit(); }
        }
        return APP_EDITOR;
    }

    // ③ 拖完抬手，hw/input 照常补一个键（拖到远处 = 上下/左右滑）：那是同一个手势的
    //    尾巴，不是新命令——吃掉它，否则刚拖到位的端点又被挪一格，或者顺手把按钮条
    //    上的按钮按了。补一次全刷清掉拖动过程（局刷）留下的残影。
    if (g.selDragActive) {
        g.selDrag = -1;
        g.selDragActive = false;
        if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN ||
            key == '\n') {
            editorRedrawFull();
            return APP_EDITOR;
        }
    }

    // ④ 长按 / Esc。长按再指到**别的词**上就是"改选那里"（安卓的长按一直就是这个
    //    意思，会话已经开着也一样）——editorBeginTouchSelection 自己会重开；指到空白
    //    上则只收会话（反白一并撤掉，柄和菜单都收）。Esc / 确认键纯收。
    if (key == KEY_TOUCH_LONG) {
        int tx = 0, ty = 0;
        if (input_tap_xy(&tx, &ty) && editorBeginTouchSelection(tx, ty)) return APP_EDITOR;
        editorEndTouchSelection();
        clearSelection();
        editorRedrawFull();
        return APP_EDITOR;
    }
    if (key == KEY_LONG_CONFIRM || key == 0x1B) {
        editorEndTouchSelection();
        clearSelection();
        editorRedrawFull();
        return APP_EDITOR;
    }

    // ⑤ 上下键移按钮条高亮。
    if (key == KEY_UP || key == KEY_DOWN) {
        editMenuMove(key == KEY_UP ? -1 : +1);
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }

    // ⑥ 回车：点按 → 按坐标命中按钮条 / 两柄；无坐标回车 → 执行高亮项。
    if (key == '\n') {
        int tx = 0, ty = 0;
        if (input_tap_xy(&tx, &ty)) {
            const int btn = editMenuHit(tx, ty);
            if (btn >= 0) {
                editMenuSetSel(btn);
                return editorSelMenuActivate(btn, ctx);
            }
            const int h = editorSelHandleAt(tx, ty);
            if (h >= 0) {   // 点柄 = 抓起/放下（放下后画成实心）
                g.selGrab = (g.selGrab == h) ? -1 : h;
                ui_clear(); drawEditor(); ui_commit();
                return APP_EDITOR;
            }
            // 点在别处：收掉会话**并撤掉反白**（安卓里点选区外面也是这个效果）。
            // 注意 editorEndTouchSelection() 只管会话（柄/按钮条），反白是 hasSelection
            // 画的——不 clearSelection() 的话，反白会留在屏上继续亮着，只是柄没了。
            // 落在**字**上再顺手把光标挪到手指处——点一下接着打字，是选完字之后最常
            // 接的动作。
            editorEndTouchSelection();
            clearSelection();
            TextPos p;
            if (editorTapOnGlyph(tx, ty, p)) {
                g.cy = p.cy;
                g.cx = p.cx;
                g.targetCx = -1;
                markDirty();
            }
            editorRedrawFull();
            return APP_EDITOR;
        }
        return editorSelMenuActivate(editMenuSel(), ctx);
    }

    // ⑦ 其它任何键（实体键盘/蓝牙）：收掉按钮条，放行给常规分支。
    editorEndTouchSelection();
    consumed = false;
    return APP_EDITOR;
}

// 上一次落在正文上的点按（时间 + 落点）。只用来认"连点两下 = 选词"（安卓的习惯）。
static int64_t s_lastTapUs = 0;
static int s_lastTapX = -1000;
static int s_lastTapY = -1000;

AppState screen_editor_handle(int key, ScreenContext &ctx) {
    // ── 编辑器的字号切分 ────────────────────────────────────────────────────
    // 「显示与版式 → 正文字号」只管**正文那一块**。做法是把 FontRenderer 的共享格子
    // 在**整个编辑器界面**上换成正文的 px，出去自动还原——这样正文的排版、换行、光标、
    // 选区、触摸命中、竖排导航（全都读那套派生式）一次全对，一个调用点都不用改。
    //
    // 代价：界面框架在正文作用域里会被一起放大，所以它们必须**自己钉回界面字号**——
    // 状态栏(editorStatusBar)、输入法条(drawIMEUI)、虚拟键盘(editorVk*)、快捷菜单
    // (editorMenu*)、查找/帮助/二维码浮层、轻提示(ui_toast_draw)，以及那几个混着
    // 界面量的度量函数（editorBodyBottomY / editorVerticalVm / editorPageRows /
    // helpMaxVis）。新加"框架里画的东西"时照此办理：函数开头一行
    // `FontScope ui(FontRenderer::uiPxHeight());`。
    //
    // 放在 handle/idle/init 三个入口而不是 drawEditor 一个地方：命中测试与绘制必须
    // 同源，而 handle 里到处是正文几何。
    FontScope body(editorBodyFontPx());
    const auto& vrows = getVrows();
    s_vkAteCommitClick = false;

    if (g_editor.promptGenerating) {
        if (!s_promptTaskDone) {
            drawPromptGenerating();
            return APP_EDITOR;
        }
        g_editor.promptGenerating = false;
        lockPromptResult();
        DeepseekResult result = s_promptTaskResult;
        unlockPromptResult();
        if (result.success && !result.content.empty()) {
            g_editor.promptText = result.content;   // 有提示词 = 提示写作（见 editorPromptOn）
        } else {
            if (g_editor.promptText.empty()) g_editor.promptText = "今天发生了什么？";
            if (!result.content.empty()) {
                ctx.statusMessage = result.content;
                ctx.statusDuration = 30;
            }
        }
        s_promptTaskContext.clear();
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }

    if (g_editor.recoveryPrompt) {
        if (key == 0x0A || key == 0x0D || key == 'y' || key == 'Y') {
            std::string fn = metaValue(g_editor.recoveryMeta, "filename");
            if (!fn.empty()) g_editor.savedFilename = fn;
            loadLinesIntoEditor(g_editor.recoveryContent);
            g_editor.scroll = 0;
            g_editor.targetCx = -1;
            g_editor.modifiedSinceSave = true;
            g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
            g_editor.recoveryPrompt = false;
            g_editor.recoveryContent.clear();
            g_editor.recoveryMeta.clear();
            markDirty();
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
        if (key == 0x1B || key == 'n' || key == 'N') {
            g_journal.clearRecoveryDraft();
            g_editor.recoveryPrompt = false;
            g_editor.recoveryContent.clear();
            g_editor.recoveryMeta.clear();
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
        ui_clear(); drawEditor(); drawRecoveryDialog(); ui_commit();
        return APP_EDITOR;
    }

    if (g_editor.confirmSave) {
        if (key == 0x0A || key == 0x0D || key == 'y' || key == 'Y') {
            g_editor.confirmSave = false;
            return finishEditor(ctx);
        }
        if (key == 0x1B || key == 'n' || key == 'N') {
            g_editor.confirmSave = false;
            ctx.nextState = ctx.prevState;
            return ctx.prevState;
        }
        ui_clear(); drawEditor(); drawConfirmDialog(); ui_commit();
        return APP_EDITOR;
    }

    // 查找/替换对话框 (Ctrl+/) — 模态子状态,处理所有按键
    if (g_editor.search.active) {
        return screen_editor_search_handle(key, ctx);
    }
    if (key == KEY_SEARCH) {
        searchOpen();
        ui_clear(); drawSearchPanel(); ui_commit();
        return APP_EDITOR;
    }

    // 快捷菜单（点正文弹出）/ 二维码 — 都是模态子状态，先于一切按键处理
    if (g_editor.menuActive) {
        return screen_editor_menu_handle(key, ctx);
    }
    if (g_editor.qrActive) {
        return screen_editor_qr_handle(key, ctx);
    }

    // 快捷键帮助对话框 (Ctrl+?) — 模态子状态
    if (g_editor.helpActive) {
        return screen_editor_help_handle(key, ctx);
    }
    if (key == KEY_HELP) {
        g_editor.helpActive = true;
        g_editor.helpScroll = 0;
        ui_clear(); drawHelpPanel(); ui_commit();
        return APP_EDITOR;
    }

    // 触摸选区会话：拖动两柄、按钮条（复制/剪切/粘贴/全选/润色）、粘贴板列表。
    // 必须排在下面的虚拟键盘块与 **IME 分支**之前：g_editor.imeActive 默认开着，
    // IME 分支会把**任何**键喂给 g_ime.handleKey()，有未上屏组合时未知键会上屏
    // 第一个候选（IME.cpp 的未知键分支）——新放行的 KEY_TOUCH_DRAG 正好会撞上。
    // 拖动手势与选区无关时（不在会话里）也要在这里吃掉，同样是为了不喂给 IME。
    if (g_editor.selTouch) {
        bool consumed = true;
        AppState r = screen_editor_selection_handle(key, ctx, consumed);
        if (consumed) return r;
        // 没被消化：选区会话已经收掉了，接着走常规分支（见函数的说明）。
    } else if (key == KEY_TOUCH_DRAG) {
        return APP_EDITOR;   // 不在选区里：编辑器不认拖动，吞掉别漏进 IME
    }

    // 虚拟键盘：蓝牙键盘连上则自动收起；点按先翻译成普通键码，再交给下方既有的
    // IME 分支 / 回车 / 退格 / ASCII 插入逻辑处理——不重复实现任何输入逻辑。
    // input_tap_xy() 读后即清，改写 key 后不会再次进入本段，不会递归。
    editorVkSyncBtState();
    if (key == '\n') {
        int tx = 0, ty = 0;
        if (input_tap_xy(&tx, &ty)) {
            // 状态栏左端的写作模式标记：点一下切模式（提示写作 ⇄ 自由写作，
            // 切过去时补一条内置提示词；长按走 AI，见下面 KEY_TOUCH_LONG）。
            // 放最前面：它落在状态栏上，那一块本来也没有别的可点东西。
            if (editorStatusModeHitAt(tx, ty)) {
                editorToggleWritingMode();
                screen_editor_reset_drawn();
                ui_clear(); drawEditor(); ui_commit();
                return APP_EDITOR;
            }
            if (editorVkIconHit(tx, ty)) {
                editorVkSetVisible(!editorVkVisible());
                ui_clear(); drawEditor(); ui_commit();
                return APP_EDITOR;
            }
            // 状态栏上的全角/半角、简/繁标记：点一下切模式（与 Ctrl+Shift 快捷键等价）。
            if (int tog = editorStatusToggleAt(tx, ty)) {
                if (tog == KEY_FULLWIDTH_TOGGLE) app_toggle_fullwidth();
                else app_toggle_trad();
                screen_editor_reset_drawn();
                ui_clear(); drawEditor(); ui_commit();
                return APP_EDITOR;
            }
            // 这一按有没有落在键盘上。落在键盘上就继续往下走"翻译成普通键码"的老路；
            // 没落在键盘上 = 点在正文/状态栏空白 → 弹快捷菜单（不再插回车）。
            const bool onVk = editorVkActive() && ty >= editorVkTop();
            if (onVk) {
                EditorVkHit hit;
                int k = editorVkHitTest(tx, ty, &hit);
                if (k == EVK_NONE) return APP_EDITOR;  // 点在键盘空白/键角死区：吞掉本次点按
                // 按下反馈：记下命中键，反色随下面各分支的重绘一起上屏（不额外推屏）。
                editorVkMarkPressed(hit);
                // 这五类键的动作会把键面本身重画成新状态（标签换字、反白翻转），
                // 按下前那份几何再叠上去就是两个标签摞一起（中英切换时"拼""英"叠字）。
                // 候选数字键同理：上屏后候选栏整排换新（多半是空的），按下前那份
                // 候选几何（"2.方法"）再补画一遍就成了清不掉的残留。
                if (k == EVK_PAGE || k == EVK_LANG || k == EVK_LAYOUT || k == EVK_CTRL ||
                    k == EVK_SHIFT || (k >= '1' && k <= '9'))
                    editorVkClearPressed();
                // 虚拟键盘的按键反馈：点哪个键都先响一声。紧跟着的上屏分支会因为
                // s_vkAteCommitClick 不再重复响一次（同一个按键响两下会像回声）。
                typingClickPlay(1);
                s_vkAteCommitClick = true;
                if (k == EVK_PAGE) {
                    // 换面板（字母/符号/数字）已在命中测试里完成，这里只需重绘；
                    // 键区整块换图样，局刷残影最明显，走全刷。
                    editorRedrawFull();
                    return APP_EDITOR;
                }
                if (k == EVK_LANG) {
                    // 未开输入法 → 开中文；已开 → 拼音/英文互切。
                    // 中/英会连带换掉键面标签、14/26 键布局、候选条 → 整块重画，全刷。
                    if (!g_editor.imeActive) app_toggle_ime();
                    else g_ime.toggleEnglish();
                    editorRedrawFull();
                    return APP_EDITOR;
                }
                if (k == EVK_LAYOUT) {
                    // 布局已在命中测试里翻转(26→14→18→9 键循环)，这里只需重绘；
                    // 键位整块换图样，同样走全刷。
                    editorRedrawFull();
                    return APP_EDITOR;
                }
                if (k == EVK_T9) {
                    // 键区整块换成 T9 面板(或面板里换了读音)：和布局切换一样是整片
                    // 换图样，走全刷。
                    editorRedrawFull();
                    return APP_EDITOR;
                }
                if (k == EVK_CTRL || k == EVK_SHIFT) {
                    // 待发状态已在命中测试里翻转，这里只需重绘(反白反馈)。
                    // 下一个普通键会在 editorVkHitTest 里被翻译成组合键/大写。
                    ui_clear(); drawEditor(); ui_commit();
                    return APP_EDITOR;
                }
                if (k == EVK_SEP || k == EVK_CLEAR || k == EVK_NOOP) {
                    // 九宫格的「1 分词」/「重输」：命中测试里已经确定音节 / 清空组合，
                    // 候选条随之变了；EVK_NOOP（组合中的「0」）则本来就无动作。
                    // 三者都只需要重绘（键面没换图样，不必全刷），顺便把按下反馈刷出去。
                    ui_clear(); drawEditor(); ui_commit();
                    return APP_EDITOR;
                }
                key = k;  // 普通键码，继续走下面的常规流程
            }
            if (!onVk) {
                // 点正文按安卓的习惯来：**单击一律定位光标**（点哪光标去哪），同一处
                // 连点两下 → 选中中间那个词 + 弹「编辑」按钮条（安卓的双击选词），
                // 直接复用长按那条路（editorBeginTouchSelection），命中词才算数。
                // 用 editorPosAtPoint 而不是 editorTapOnGlyph：前者在正文区里永远命中
                // （空行、行尾右侧的空白也取最近的字界），正是"点空白也落光标"要的；
                // 后者额外要求落在**字**上，会把空白处让出去。
                // 快捷菜单改由**长按**弹出（见下面 KEY_TOUCH_LONG）；单击不再弹菜单，
                // 否则跟"点一下接着打字"抢动作。
                // 物理回车和虚拟键盘的回车键都不受影响——input_tap_xy() 只对触摸点按有值。
                const int64_t now = esp_timer_get_time();
                const bool dbl = (now - s_lastTapUs) < 400000 &&
                                 std::abs(tx - s_lastTapX) < 30 && std::abs(ty - s_lastTapY) < 30;
                s_lastTapUs = now;
                s_lastTapX = tx;
                s_lastTapY = ty;
                if (dbl && editorBeginTouchSelection(tx, ty)) return APP_EDITOR;

                TextPos p;
                if (editorPosAtPoint(tx, ty, p)) {
                    clearSelection();
                    g_editor.cy = p.cy;
                    g_editor.cx = p.cx;
                    g_editor.targetCx = -1;
                    markDirty();
                    ui_clear(); drawEditor(); ui_commit();
                }
                // 正文区之外（状态栏非标记处等）：什么都不做。
                return APP_EDITOR;
            }
        }
    }

    // 触摸长按：
    //  ① 状态栏左端的模式标记 + 正在提示写作（有提示词）→ 让 AI 生成新提示词。
    //     这条必须留在最前：它跟"选字"共用同一个长按键，靠落点分辨。
    //  ② 正文上 → 落在词上就选中它并弹「编辑」按钮条，落在空白/标点上（光标附近
    //     通常是空白）就只弹「粘贴/全选/取消」——即"复制完在光标附近长按可粘贴"。
    //  ③ 别处（空白、状态栏、虚拟键盘）→ **弹快捷菜单**。长按=返回的老语义已废：
    //     返回改由虚拟按键/菜单的「保存并返回」承担，这一按留给菜单——它是触摸下
    //     唯一能直达「保存并返回/全文润色/二维码」的入口，不能再跟返回抢。
    if (key == KEY_TOUCH_LONG) {
        int tx = 0, ty = 0;
        const bool haveXY = input_tap_xy(&tx, &ty);
        if (haveXY && editorStatusModeHitAt(tx, ty) && editorPromptOn()) {
            startAiPromptGeneration(ctx);
            return APP_EDITOR;
        }
        if (haveXY && editorBeginTouchSelection(tx, ty)) return APP_EDITOR;
        editorMenuOpen();
        ui_clear(); drawEditor(); drawEditorMenuBox(); ui_commit();
        return APP_EDITOR;
    }

    if (key == 0x1B) {
        if (inQuickFileSession()) {
            // 快捷编辑: 自动保存后跳到设置面板
            if (quickEditSave(quickEditIndex(), currentEditorText())) g_journal.clearRecoveryDraft();
            g_editor.modifiedSinceSave = false;
            ctx.nextState = APP_SETTINGS;
            return APP_SETTINGS;
        }
        bool hasContent = g_editor.lines.size() > 1 ||
            (g_editor.lines.size() == 1 && !g_editor.lines[0].empty());
        if (hasContent && g_editor.modifiedSinceSave) {
            g_editor.confirmSave = true;
            ui_clear(); drawEditor(); drawConfirmDialog(); ui_commit();
            return APP_EDITOR;
        }
        ctx.nextState = ctx.prevState; return ctx.prevState;
    }

    // Ctrl+O → 选区润色(需先用Shift+方向键选择文字)
    if (key == 0x0F) {
        if (!g_editor.hasSelection) {
            ctx.statusMessage = "请先用Shift+方向键选择文字";
            ctx.statusDuration = 30;
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
        screen_polish_set_scope(POLISH_SELECTION);
        return APP_POLISH;
    }

    // Ctrl+A → 全选（与长按菜单里的「全选」同一个函数）
    if (key == 0x01) {
        editorSelectAll();
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }

    // 虚拟键盘候选行左右划翻页。抬手时 input.cpp 会把一次横滑补成一个 KEY_LEFT/RIGHT,
    // 那在编辑器里是"移光标"。起点(input_press_xy)落在候选行时这个键归键盘：翻候选，
    // 并且必须在这里吃掉，否则划一下翻页会顺带把光标挪一格。
    // input_press_xy() 只在触摸手势抬手的那一帧有值，BLE 键盘的左右键读不到，不会误吃。
    if ((key == KEY_LEFT || key == KEY_RIGHT) && editorVkActive()) {
        int px = 0, py = 0;
        if (input_press_xy(&px, &py) &&
            editorVkSwipePage(px, py, key == KEY_RIGHT ? +1 : -1)) {
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
    }

    // T9 候选面板里的上下滑：左列滚读音、宫格翻候选页。同一划在正文里是滚一屏，
    // 起点落在面板上就得归键盘，否则滚面板会顺带把正文也滚走一屏。
    // 这里认 KEY_UP/DOWN 而不是 KEY_PAGE_UP/DOWN：触摸竖滑本来是后者，但主循环
    // (main.cpp) 对非设置/计划界面会把 PAGE 键回退成单步 UP/DOWN，走到这儿已经是
    // KEY_UP/DOWN 了；只认 PAGE 键的话这一支永远不命中——竖滑落到下面，在 IME 活跃
    // 时又被当成翻候选页的 0x80/0x81（KEY_UP/DOWN 与 IME_KEY_UP/DOWN 同值）吃掉，
    // 表现就是"在读音列上滑也在翻候选，后面的读音永远翻不到"。
    if ((key == KEY_UP || key == KEY_DOWN ||
         key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) && editorVkActive()) {
        int px = 0, py = 0;
        const bool next = (key == KEY_DOWN || key == KEY_PAGE_DOWN);
        const bool havePress = input_press_xy(&px, &py);
        const bool taken = havePress && editorVkSwipeScroll(px, py, next ? +1 : -1);
        ESP_LOGI("EdVk", "swipe key=%d next=%d press=%d (%d,%d) taken=%d",
                 key, (int)next, (int)havePress, px, py, (int)taken);
        if (taken) {
            // 面板整块换内容（读音表滚动/宫格换页），和面板开合一样走全刷。
            editorRedrawFull();
            return APP_EDITOR;
        }
    }

    if (g_editor.imeActive && key != 0) {
        std::string imeOut;
        if (g_ime.handleKey(key, imeOut)) {
            std::string imeStatus = g_ime.takeStatusMessage();
            if (!imeStatus.empty()) {
                ctx.statusMessage = imeStatus;
                ctx.statusDuration = 30;
            }
            // 英文模式连续上屏两个候选词且中间无空格时, 自动补空格分隔
            if (g_ime.english() && !imeOut.empty() && !g_editor.hasSelection) {
                const std::string &line = g_editor.lines[g_editor.cy];
                if (g_editor.cx > 0) {
                    char prev = line[g_editor.cx - 1];
                    if (prev != ' ' && prev != '\t') imeOut.insert(imeOut.begin(), ' ');
                }
            }
            // 不再限定打字机模式：按键音在正常模式下是虚拟键盘的按键反馈，开关由
            // typingClickPlay 内部的 typingClickEnabled() 把关。
            if (!s_vkAteCommitClick) {
                // 中文音效触发:key=每个被 IME 消费的键一声;count=上屏按字数连响;single=上屏一声
                if (g_settings.clickChineseMode() == "key") {
                    typingClickPlay(1);
                } else if (g_settings.clickChineseMode() == "count") {
                    int n = utf8Count(imeOut); if (n > 0) typingClickPlay(n);
                } else {
                    if (!imeOut.empty()) typingClickPlay(1);
                }
            }
            editorInsertText(imeOut);
            // 刚上屏的词要立刻进文档上下文: 同一篇里再次输入同一个人名/术语时它就该
            // 排在前面。空串(取消组合等)不必重扫。
            if (!imeOut.empty()) g_ime.setDocumentContext(editorImeContextText());
            ui_clear(); drawEditor();
            ui_commit();
            return APP_EDITOR;
        }
    }

    if (key == 9) {
        recordUndoSnapshot(UndoGroup::Structural);
        g_editor.lines[g_editor.cy].insert(g_editor.cx, 4, ' ');
        g_editor.cx += 4;
        g_editor.targetCx = -1;
        markDirty();
        g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
        g_editor.modifiedSinceSave = true;
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == 0x0E) { // Ctrl+N → 下一个快捷编辑文件
        if (inQuickFileSession()) {
            quickEditSwitchTo((quickEditIndex() + 1) % 10);
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
    }
    if (key == 0x10) { // Ctrl+P
        if (inQuickFileSession()) {  // 快捷编辑: 上一个文件
            quickEditSwitchTo((quickEditIndex() + 9) % 10);
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
        // 提示写作/自由写作合并后，Ctrl+P 就在这两者之间切（与点状态栏左端那个标记
        // 一模一样）：有提示词 → 退回自由写作；没有 → 取一条内置提示词。
        // AI 生成提示词挂在"长按提示写作标记"上（见 KEY_TOUCH_LONG 分支）。
        editorToggleWritingMode();
        screen_editor_reset_drawn();
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }
    if (key == 0x13) {
        std::string text = currentEditorText();
        if (!text.empty()) {
            if (saveCurrentContent()) ctx.statusMessage = "已保存";
            else ctx.statusMessage = "保存失败";
        }
        ui_clear(); drawEditor(); ui_commit();
        g_editor.autoSaveTime = 0;
        g_editor.modifiedSinceSave = false;
        return APP_EDITOR;
    }  // Ctrl+S
    if (key == 0x11) {
        if (inQuickFileSession()) {
            if (quickEditSave(quickEditIndex(), currentEditorText())) g_journal.clearRecoveryDraft();
            g_editor.modifiedSinceSave = false;
            ctx.nextState = APP_SETTINGS;
            return APP_SETTINGS;
        }
        ctx.nextState = ctx.prevState; return ctx.prevState;
    }  // Ctrl+Q
    if (key == 0x06) {  // Ctrl+F
        ctx.nextState = APP_SYNC_SEND_FLOMO;
        return APP_SYNC_SEND_FLOMO;
    }
    if (key == 0x1A) {  // Ctrl+Z
        if (!undoEditor()) {
            ctx.statusMessage = "没有可撤销内容";
            ctx.statusDuration = 30;
        }
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }
    if (key == KEY_REDO || key == 0x12) {  // Ctrl+Shift+Z / Ctrl+R
        if (!redoEditor()) {
            ctx.statusMessage = "没有可重做内容";
            ctx.statusDuration = 30;
        }
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }
    if (key == 0x19) {  // Ctrl+Y → 当前日记历史版本
        if (inQuickFileSession()) {
            ctx.statusMessage = "快捷编辑历史暂未支持";
            ctx.statusDuration = 30;
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
        if (!saveCurrentContent()) {
            ctx.statusMessage = "保存失败，无法查看历史";
            ctx.statusDuration = 30;
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
        g_editor.modifiedSinceSave = false;
        ctx.selectedEntry = g_editor.savedFilename;
        ctx.prevState = APP_EDITOR;
        ctx.nextState = APP_HISTORY;
        return APP_HISTORY;
    }
    if (key >= KEY_FILE_BASE && key <= KEY_FILE_BASE + 9) {  // Ctrl+0-9 直接切换文件
        if (inQuickFileSession()) {
            quickEditSwitchTo(key - KEY_FILE_BASE);
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
    }

    // ── Heading fold toggle (Ctrl+T) ─────────────────────────────────
    if (key == 0x14) { // Ctrl+T — 折叠/展开当前标题下的正文
        const auto &mi = getMdInfo(g_settings.markdownRender());
        if (g_editor.cy >= 0 && g_editor.cy < (int)mi.size() && mi[g_editor.cy].headingLevel > 0) {
            if (!g_editor.foldedHeadings.erase(g_editor.cy))
                g_editor.foldedHeadings.insert(g_editor.cy);
            g_editor.vrowsDirty = true;  // 纯视图态,不进撤销快照
        }
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }

    // ── 粘贴板操作（Ctrl+C / Ctrl+X / Ctrl+V）────────────────────────
    // 走共享的 clipboard（三模式同一份，落盘）。Ctrl+V 只取最近一条；
    // 想从历史里挑，用长按弹出的「粘贴」/「粘贴板」（见选区菜单）。
    // 复制/剪切/粘贴：与长按按钮条走**同一个** editorActionCopy/editorActionPasteText，
    // 同一份粘贴板（触摸与实体键盘同步）。
    if (key == 0x03) { // Ctrl+C — 复制
        editorActionCopy(ctx, false);
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == 0x18) { // Ctrl+X — 剪切
        editorActionCopy(ctx, true);
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == 0x16) { // Ctrl+V — 粘贴最近一条
        editorActionPasteText(clipboardLatest());
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }

    if (editorVertical() && (key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT ||
                             key == KEY_RIGHT || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN)) {
        clearSelection();
        VerticalLayoutMetrics vm = editorVerticalVm();
        bool mdOn = g_settings.markdownRender();
        mdSetRenderEnabled(mdOn);
        const auto &mdInfo = getMdInfo(mdOn);
        auto hidden = mdFoldHiddenLines(g_editor.lines, mdOn ? &mdInfo : nullptr,
                                        &g_editor.foldedHeadings);
        auto data = buildVerticalData(g_editor.lines, vm.rows, &hidden,
                                      mdOn ? &mdInfo : nullptr, &g_editor.foldedHeadings);
        if (key == KEY_UP) {
            moveCursorVerticalInline(-1);
        } else if (key == KEY_DOWN) {
            moveCursorVerticalInline(1);
        } else if (key == KEY_LEFT) {
            moveCursorVerticalColumn(1, data);
        } else if (key == KEY_RIGHT) {
            moveCursorVerticalColumn(-1, data);
        } else if (key == KEY_PAGE_UP) {
            moveCursorVerticalColumn(-vm.cols, data);
        } else if (key == KEY_PAGE_DOWN) {
            moveCursorVerticalColumn(vm.cols, data);
        }
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }

    // ── Shift+arrow: extend selection ─────────────────────────────────
    if (key == KEY_SHIFT_LEFT) {
        if (g_editor.cx > 0) {
            extendSelection();
            imeFieldMoveLeft(editorLineField());
        } else if (g_editor.cy > 0) {
            extendSelection();
            g_editor.cy--;
            g_editor.cx = (int)g_editor.lines[g_editor.cy].length();
        }
        g_editor.targetCx = -1;
        markDirty();
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == KEY_SHIFT_RIGHT) {
        if (g_editor.cx < (int)g_editor.lines[g_editor.cy].length()) {
            extendSelection();
            imeFieldMoveRight(editorLineField());
        } else if (g_editor.cy < (int)g_editor.lines.size() - 1) {
            extendSelection();
            g_editor.cy++;
            g_editor.cx = 0;
        }
        g_editor.targetCx = -1;
        markDirty();
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == KEY_SHIFT_UP) {
        if (g_editor.cy > 0) {
            extendSelection();
        }
        int curVR = -1;
        for (int vi = 0; vi < (int)vrows.size(); vi++) {
            if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
                curVR = vi; break;
            }
        }
        if (curVR > 0) {
            auto &prev = vrows[curVR - 1];
            if (g_editor.targetCx < 0)
                g_editor.targetCx = byteToCells(g_editor.lines[g_editor.cy], g_editor.cx);
            int visualCol = g_editor.targetCx % EDITOR_MAX_CELLS;
            g_editor.cy = prev.lineIdx;
            int vrowStartCells = byteToCells(g_editor.lines[g_editor.cy], prev.start);
            int targetCells = vrowStartCells + visualCol;
            g_editor.cx = cellsToByte(g_editor.lines[g_editor.cy], prev.start, prev.end, targetCells);
        }
        markDirty();
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == KEY_SHIFT_DOWN) {
        if (g_editor.cy < (int)g_editor.lines.size() - 1) {
            extendSelection();
        }
        int curVR = -1;
        for (int vi = 0; vi < (int)vrows.size(); vi++) {
            if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
                curVR = vi; break;
            }
        }
        if (curVR >= 0 && curVR < (int)vrows.size() - 1) {
            auto &next = vrows[curVR + 1];
            if (g_editor.targetCx < 0)
                g_editor.targetCx = byteToCells(g_editor.lines[g_editor.cy], g_editor.cx);
            int visualCol = g_editor.targetCx % EDITOR_MAX_CELLS;
            g_editor.cy = next.lineIdx;
            int vrowStartCells = byteToCells(g_editor.lines[g_editor.cy], next.start);
            int targetCells = vrowStartCells + visualCol;
            g_editor.cx = std::min(cellsToByte(g_editor.lines[g_editor.cy], next.start, next.end, targetCells),
                                   (int)g_editor.lines[g_editor.cy].length());
        }
        markDirty();
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }

    // Navigation & editing
    if (key == 0x0A || key == 0x0D) { // Enter
        recordUndoSnapshot(UndoGroup::Structural);
        if (g_editor.hasSelection) deleteSelection();
        // 列表行在行尾回车时自动续行:下一行带上同款列表标记
        std::string prefix;
        if (g_editor.cx >= (int)g_editor.lines[g_editor.cy].length()) {
            const std::string &cur = g_editor.lines[g_editor.cy];
            MdListMarker m = mdListMarker(cur);
            if (m.ok) {
                bool emptyItem = cur.substr(m.start + m.len).find_first_not_of(" \t") == std::string::npos;
                if (!emptyItem) {
                    std::string lead = cur.substr(0, m.start);  // 嵌套缩进
                    if (m.task) {
                        prefix = lead + "- [ ] ";
                    } else if (m.ordered) {
                        int d = m.start;
                        while (d < (int)cur.length() && cur[d] >= '0' && cur[d] <= '9') d++;
                        if (d == m.start) {  // 中文序号:一、二、十、… 递增(一→二→…→十→十一)
                            int nlen = 0;
                            int n = mdCnNumValue(cur, m.start, nlen);
                            if (n >= 0)
                                prefix = lead + mdCnNumeral(n + 1) + cur.substr(m.start + nlen, m.len - nlen);
                            else
                                prefix = lead + cur.substr(m.start, m.len);
                        } else {
                            int n = 0;
                            for (int k = m.start; k < d; k++) n = n * 10 + (cur[k] - '0');
                            n++;
                            char num[16];
                            snprintf(num, sizeof(num), "%d", n);
                            prefix = lead + num + cur.substr(d, m.len - (d - m.start));
                        }
                    } else {
                        prefix = lead + cur.substr(m.start, 1) + " ";  // 保留 -/*/+
                    }
                }
            }
        }
        std::string rest = g_editor.lines[g_editor.cy].substr(g_editor.cx);
        g_editor.lines[g_editor.cy] = g_editor.lines[g_editor.cy].substr(0, g_editor.cx);
        g_editor.cx = 0; g_editor.cy++;
        g_editor.lines.insert(g_editor.lines.begin() + g_editor.cy, prefix + rest);
        foldLinesInserted(g_editor.cy, 1);
        g_editor.cx = (int)prefix.length();  // 光标落在续行标记之后
        g_editor.targetCx = -1;
        markDirty();
        g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
        g_editor.modifiedSinceSave = true;
    } else if (key == 0x7F || key == 0x08) { // Backspace
        if (g_editor.hasSelection) {
            recordUndoSnapshot(UndoGroup::Delete);
            deleteSelection();
        } else if (g_editor.cx > 0) {
            recordUndoSnapshot(UndoGroup::Delete);
            imeFieldBackspace(editorLineField());
            if (g_editor.imeActive) g_ime.handleHostBackspace();
        } else if (g_editor.cy > 0) {
            recordUndoSnapshot(UndoGroup::Delete);
            g_editor.cx = (int)g_editor.lines[g_editor.cy-1].length();
            g_editor.lines[g_editor.cy-1] += g_editor.lines[g_editor.cy];
            g_editor.lines.erase(g_editor.lines.begin() + g_editor.cy);
            foldLinesErased(g_editor.cy, 1);
            g_editor.cy--;
        }
        g_editor.targetCx = -1;
        markDirty();
        g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
        g_editor.modifiedSinceSave = true;
    } else if (key >= 0x20 && key <= 0x7E) { // ASCII printable
        recordUndoSnapshot(UndoGroup::Typing);
        if (g_editor.hasSelection) deleteSelection();
        imeFieldInsert(editorLineField(), std::string(1, (char)key));
        g_editor.targetCx = -1;
        markDirty();
        g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
        g_editor.modifiedSinceSave = true;
        // 同上：不限定模式，虚拟键盘的字母/数字键在正常模式下也该有反馈。
        if (!s_vkAteCommitClick) typingClickPlay(1);
    } else if (key == KEY_LEFT) {
        clearSelection();
        if (g_editor.cx > 0) {
            imeFieldMoveLeft(editorLineField());
        } else if (g_editor.cy > 0) {
            g_editor.cy--;
            g_editor.cx = (int)g_editor.lines[g_editor.cy].length();
        }
        g_editor.targetCx = -1;
    } else if (key == KEY_RIGHT) {
        clearSelection();
        if (g_editor.cx < (int)g_editor.lines[g_editor.cy].length()) {
            imeFieldMoveRight(editorLineField());
        } else if (g_editor.cy < (int)g_editor.lines.size() - 1) {
            g_editor.cy++;
            g_editor.cx = 0;
        }
        g_editor.targetCx = -1;
    } else if (key == KEY_UP) {
        clearSelection();
        int curVR = -1;
        for (int vi = 0; vi < (int)vrows.size(); vi++) {
            if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
                curVR = vi; break;
            }
        }
        if (curVR > 0) {
            auto &prev = vrows[curVR - 1];
            if (g_editor.targetCx < 0)
                g_editor.targetCx = byteToCells(g_editor.lines[g_editor.cy], g_editor.cx);
            int visualCol = g_editor.targetCx % EDITOR_MAX_CELLS;
            g_editor.cy = prev.lineIdx;
            int vrowStartCells = byteToCells(g_editor.lines[g_editor.cy], prev.start);
            int targetCells = vrowStartCells + visualCol;
            g_editor.cx = cellsToByte(g_editor.lines[g_editor.cy], prev.start, prev.end, targetCells);
        }
    } else if (key == KEY_DOWN) {
        clearSelection();
        int curVR = -1;
        for (int vi = 0; vi < (int)vrows.size(); vi++) {
            if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
                curVR = vi; break;
            }
        }
        if (curVR >= 0 && curVR < (int)vrows.size() - 1) {
            auto &next = vrows[curVR + 1];
            if (g_editor.targetCx < 0)
                g_editor.targetCx = byteToCells(g_editor.lines[g_editor.cy], g_editor.cx);
            int visualCol = g_editor.targetCx % EDITOR_MAX_CELLS;
            g_editor.cy = next.lineIdx;
            int vrowStartCells = byteToCells(g_editor.lines[g_editor.cy], next.start);
            int targetCells = vrowStartCells + visualCol;
            g_editor.cx = std::min(cellsToByte(g_editor.lines[g_editor.cy], next.start, next.end, targetCells),
                                   (int)g_editor.lines[g_editor.cy].length());
        }
    } else if (key == KEY_HOME) {
        clearSelection();
        editorLineField().setCur(0);
        g_editor.targetCx = -1;
    } else if (key == KEY_END) {
        clearSelection();
        imeFieldMoveEnd(editorLineField());
        g_editor.targetCx = -1;
    } else if (key == KEY_PAGE_UP) {
        clearSelection();
        moveCursorVertical(-editorPageRows(), vrows);
    } else if (key == KEY_PAGE_DOWN) {
        clearSelection();
        moveCursorVertical(editorPageRows(), vrows);
    }

    ui_clear(); drawEditor(); ui_commit();
    return APP_EDITOR;
}

// ── Idle tick (no key) ────────────────────────────────────────────────────
// main loop calls this instead of screen_editor_handle(0, ctx). Runs the
// auto-save schedule and only repaints when the screen may be stale, avoiding
// a full redraw every 50ms idle tick.
// 上一帧画没画就地提示（"已复制"这类，见 ui_helpers.h）。
static bool s_toastShown = false;

bool screen_editor_idle(ScreenContext &ctx, bool forceRedraw) {
    (void)ctx;

    // 正文作用域：见 screen_editor_handle 处的说明。
    FontScope body(editorBodyFontPx());
    // 提示框不会动，所以不必每帧重画：只在"画/不画"翻转的那一下补一帧就够了——
    // 过期那次重绘正好把白框抹掉。编辑器空闲会跳过重绘，这一补只能在这里做；
    // 顺着 forceRedraw 走是为了让当时开着的模态面板（查找/帮助/快捷菜单/恢复提示）
    // 用它自己的画法重绘，而不是被下面这句"重画编辑器"盖掉。
    const bool toastNow = ui_toast_active();
    if (toastNow != s_toastShown) {
        s_toastShown = toastNow;
        forceRedraw = true;
    }
    // 屏边自动滚动的"持续"那半边：手指压在正文区外的边带上不动时，下面那些模态
    // 分支都不会走到（drawEditor 也不跑），得在这里补一帧。见 editorSelDragIdleScroll。
    if (editorSelDragIdleScroll()) return true;
    if (g_editor.promptGenerating) {
        if (s_promptTaskDone) {
            g_editor.promptGenerating = false;
            lockPromptResult();
            DeepseekResult result = s_promptTaskResult;
            unlockPromptResult();
            if (result.success && !result.content.empty()) {
                g_editor.promptText = result.content;   // 有提示词 = 提示写作
            } else {
                if (g_editor.promptText.empty()) g_editor.promptText = "今天发生了什么？";
            }
            s_promptTaskContext.clear();
            ui_clear(); drawEditor(); ui_commit();
            return true;
        }
        drawPromptGenerating();
        return true;
    }
    if (g_editor.recoveryPrompt) {
        if (forceRedraw || !g_editor.drawnOnce) {
            ui_clear(); drawEditor(); drawRecoveryDialog(); ui_commit();
            return true;
        }
        return false;
    }
    // Auto-save on idle ticks (快捷编辑始终自动保存)
    if (g_editor.autoSaveTime > 0 && esp_timer_get_time() > g_editor.autoSaveTime) {
        g_editor.autoSaveTime = 0;
        bool shouldCommit = inQuickFileSession() || g_settings.autoSave();
        if (shouldCommit) {
            if (saveCurrentContent(false)) {
                g_editor.modifiedSinceSave = false;
            } else if (g_editor.modifiedSinceSave && g_settings.recoveryDraft()) {
                saveRecoveryDraftIfChanged();
            }
        } else if (g_editor.modifiedSinceSave && g_settings.recoveryDraft()) {
            saveRecoveryDraftIfChanged();
        }
    }
    // 查找/替换对话框打开时,面板只在按键时变化;空闲重绘走面板
    if (g_editor.search.active) {
        if (forceRedraw || !g_editor.drawnOnce) {
            ui_clear(); drawSearchPanel(); ui_commit();
            return true;
        }
        return false;
    }
    // 快捷键帮助对话框同理
    if (g_editor.helpActive) {
        if (forceRedraw || !g_editor.drawnOnce) {
            ui_clear(); drawHelpPanel(); ui_commit();
            return true;
        }
        return false;
    }
    // 快捷菜单（浮在编辑器上）与二维码整屏：同理，空闲重绘要走它们自己的画法，
    // 否则一空闲就被下面那句"重画编辑器"盖掉。
    if (g_editor.menuActive) {
        if (forceRedraw || !g_editor.drawnOnce) {
            ui_clear(); drawEditor(); drawEditorMenuBox(); ui_commit();
            return true;
        }
        return false;
    }
    if (g_editor.qrActive) {
        if (forceRedraw || !g_editor.drawnOnce) {
            drawEditorQr();
            return true;
        }
        return false;
    }
    if (forceRedraw || !g_editor.drawnOnce) {
        ui_clear(); drawEditor(); ui_commit();
        return true;
    }
    return false;
}

void screen_editor_reset_drawn() {
    g_editor.drawnOnce = false;
}

// 查找/替换对话框是否打开(供 main.cpp 屏蔽全局按键)
bool app_editor_search_active() {
    return g_editor.search.active;
}

// 快捷键帮助对话框是否打开(供 main.cpp 屏蔽全局按键)
bool app_editor_help_active() {
    return g_editor.helpActive;
}

// 编辑器里的弹出层（快捷菜单 / 二维码 / 触摸选区与它的按钮条、粘贴板列表）是否
// 打开(供 main.cpp 屏蔽全局按键)
bool app_editor_popup_active() {
    return g_editor.menuActive || g_editor.qrActive || g_editor.selTouch ||
           editMenuActive() || editMenuPickerActive();
}

// 离开编辑器时清掉触摸选区与它的弹层。电源键切模式（main.cpp）绕开了
// screen_editor_init，不清的话切回来还挂着上一次的选区/按钮条。
void app_editor_leave_cleanup() {
    g_editor.hasSelection = false;
    editorEndTouchSelection();
    // 走了就别留着白框等下一次进编辑器（空闲重绘的"画没画"记账也一并复位）。
    ui_toast_clear();
    s_toastShown = false;
}

// ── App-level helpers ────────────────────────────────────────────────────
bool app_ime_active() {
    return g_editor.imeActive;
}

void app_toggle_ime() {
    g_editor.imeActive = !g_editor.imeActive;
    g_ime.setActive(g_editor.imeActive);
    if (g_editor.imeActive) g_ime.setDocumentContext(editorImeContextText());
}

bool app_ime_fullwidth() {
    return g_ime.fullwidth();
}

void app_toggle_fullwidth() {
    g_ime.toggleFullwidth();
}

void app_toggle_trad() {
    g_ime.toggleTrad();
}

void app_toggle_english() {
    g_ime.toggleEnglish();
}

void app_toggle_ime_delete_mode() {
    g_ime.toggleDeleteMode();
}

static bool g_editorNeedsReinit = false;

// ── 进入 / 离开编辑器（main.cpp 的 kScreens 生命周期钩子）───────────────────
// 这两条策略原来长在 main.cpp 的 scrEditor() 里（一份 `static bool editorInited`
// 外加一串"去哪儿才重置"的判断）。搬到编辑器自己的文件里：判据（哪些界面只是盖在
// 编辑器之上的一层）和状态（g_editor / 暂存会话）在同一个文件，改一处就够。
static bool s_editorInited = false;    // 本"进入会话"是否已经 init 过
static bool s_keepOnReturn = false;    // 上一处离开是去浮层（灵感/润色/历史）

void screen_editor_enter(ScreenContext &ctx) {
    // 本轮切换进编辑器时屏幕已被上一层盖过：置脏，这一帧必须整屏重绘（否则屏幕停在
    // 上一个界面画面上，直到按第一个键才动）。原来这一句在主循环里按 currentState 判，
    // 现在跟着"进入"这个事件走 —— 从浮层回来同样要重绘，所以放在重建判断**之前**。
    screen_editor_reset_drawn();
    // 从浮层回来**不重建**：灵感/润色/历史只是盖在编辑器之上的一层，正文/光标/选区
    // 都还在（浮层走之前已经 stash 过会话，见 app_editor_stash_session 的调用点）。
    if (s_keepOnReturn) {
        s_keepOnReturn = false;
        return;
    }
    // 其余来源一律重建。app_editor_needs_reinit() 是**显式**的重建信号（往编辑器里塞
    // 新内容时投递，调用点在别的界面：screen_flomo / screen_inspiration / history），
    // 而且全都发生在"返回 APP_EDITOR 之前"——所以在这里问一次和一帧一帧地问等价。
    if (!s_editorInited || app_editor_needs_reinit()) {
        screen_editor_init(ctx);
        s_editorInited = true;
    }
}

void screen_editor_leave(AppState next) {
    if (next == APP_INSPIRATION || next == APP_POLISH || next == APP_HISTORY) {
        s_keepOnReturn = true;    // 只是被浮层盖住：会话原样留着，回来接着用
        return;
    }
    if (next == APP_SYNC_SEND_FLOMO) return;   // 转发到 flomo：会话留着，回来接着用
    // 切模式（电源键）走的是**另一条路**：它在派发之前就把 currentState 改了，本函数
    // 得到的 next 是另一个模式的落点（阅读/计划）。那种情况必须**保住**会话——原来的
    // main.cpp 里 `editorInited = false` 只写在 APP_EDITOR 的 case 体里，切模式那帧根本
    // 不进这个 case，所以它天然保住了会话（见 [[mode-switch-session]]）。现在 leave 被
    // 所有路径共用，就得把这条判据写出来：**目的地跨了模式 → 是切模式，保住**。
    if (appModeOfState(next) != appModeOfState(APP_EDITOR)) {
        // 切模式：会话留住，但触摸选区/按钮条/粘贴板列表这些"挂在屏幕上的东西"要收掉
        // （切回来是直接用留住的会话，不收就会挂上一次的选区）。这一句原来在 main.cpp
        // 的电源键分支里（cur==1 && currentState==APP_EDITOR）。
        app_editor_leave_cleanup();
        return;
    }
    // 同模式内的离开（Esc 回写作菜单 / 去文件管理 / 设置…）才是真的走了：下次进来重跑
    // init（新内容 / 新文件）。需要"回来看到新内容"的入口会显式调 app_editor_request_reinit()。
    s_editorInited = false;
}

void app_editor_request_reinit() {
    g_editorNeedsReinit = true;
}

bool app_editor_needs_reinit() {
    if (g_editorNeedsReinit) {
        g_editorNeedsReinit = false;
        return true;
    }
    return false;
}

void app_editor_stash_session() {
    if (s_hasStashedEditor) return;
    s_stashedEditor = g_editor;
    s_hasStashedEditor = true;
    g_ime.cancelComposition();
}

void app_editor_restore_stashed_session() {
    if (!s_hasStashedEditor) return;
    g_editor = s_stashedEditor;
    s_hasStashedEditor = false;
    // 归还那份暂存副本。它和 g_editor 是一份完整深拷贝（正文、竖排缓存、撤销/重做
    // 栈，加起来可达几百 KB），还回去再留着纯属白占 PSRAM——下次 app_editor_stash_session
    // 会重新拷一份。不清的话这份副本要么等下一次 stash 覆盖，要么留到重启。
    s_stashedEditor = EditorState{};
    g_editor.drawnOnce = false;
    g_ime.setActive(g_editor.imeActive);
    if (g_editor.imeActive) g_ime.setDocumentContext(editorImeContextText());
    markDirty();
}

bool app_editor_has_stashed_session() {
    return s_hasStashedEditor;
}

std::string app_get_editor_text() {
    return currentEditorText();
}

// Insert text at the cursor; IME commit / inspiration / polish all funnel here.
void editorInsertText(const std::string &text) {
    if (text.empty()) return;
    recordUndoSnapshot(UndoGroup::Typing);
    if (g_editor.hasSelection) deleteSelection();
    imeFieldInsert(editorLineField(), text);   // 删除选区可能并了行，字段要在它之后取
    g_editor.targetCx = -1;
    markDirty();
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
}

// Replace the entire editor text (AI polish confirm). Cursor moves to the end.
void editorReplaceAllText(const std::string &text) {
    recordUndoSnapshot(UndoGroup::Structural);
    loadLinesIntoEditor(text);
    g_editor.scroll = 0;
    g_editor.targetCx = -1;
    g_editor.hasSelection = false;
    markDirty();
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
}

// Currently selected text (empty when no selection).
std::string app_get_selected_text() {
    return getSelectedText();
}

// Replace only the selected text (selection polish confirm). Cursor ends at
// the end of the inserted text; the selection is cleared.
void editorReplaceSelection(const std::string &text) {
    // deleteSelection() removes the selection, joins its lines and puts the
    // cursor at the selection start — the natural insertion point.
    recordUndoSnapshot(UndoGroup::Structural);
    if (g_editor.hasSelection) deleteSelection();
    if (!text.empty()) {
        // Split on '\n' (drop a trailing empty segment, like loadLinesIntoEditor)
        // so a multi-line polish result splits cleanly across lines.
        std::vector<std::string> ins;
        size_t pos = 0;
        while (pos < text.length()) {
            size_t nl = text.find('\n', pos);
            ins.push_back(nl == std::string::npos ? text.substr(pos) : text.substr(pos, nl - pos));
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        if (ins.size() > 1 && ins.back().empty()) ins.pop_back();

        std::string tail = g_editor.lines[g_editor.cy].substr(g_editor.cx);
        g_editor.lines[g_editor.cy] = g_editor.lines[g_editor.cy].substr(0, g_editor.cx) + ins[0];
        if (ins.size() == 1) {
            g_editor.lines[g_editor.cy] += tail;
            g_editor.cx = (int)g_editor.lines[g_editor.cy].length() - (int)tail.length();
        } else {
            int insertAt = g_editor.cy + 1;
            for (size_t i = 1; i < ins.size() - 1; i++)
                g_editor.lines.insert(g_editor.lines.begin() + insertAt++, ins[i]);
            g_editor.lines.insert(g_editor.lines.begin() + insertAt, ins.back() + tail);
            foldLinesInserted(g_editor.cy + 1, (int)ins.size() - 1);
            g_editor.cy = insertAt;
            g_editor.cx = (int)ins.back().length();
        }
    }
    g_editor.targetCx = -1;
    markDirty();
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
}
