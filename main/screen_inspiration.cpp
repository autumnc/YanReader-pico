#include "pjournal_app.h"
#include "screen_editor.h"
#include "font_renderer.h"
#include "json_parser.h"
#include "journal_storage.h"
#include "ui_helpers.h"
#include "ime/IME.h"
#include "clipboard.h"
#include "editor_vk.h"   // 虚拟键盘：关键词编辑/检索没连蓝牙键盘时的唯一输入途径
#include "text_sel.h"    // 单行输入框的触摸选字 / 粘贴板（三模式共享底层件）
#include "hw/input.h"    // input_tap_xy：长按落点（键盘泵没消费时才取得到）
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>

extern u8g2_t *g_u8g2;
extern "C" {
    extern void u8g2_SetDrawColor(void *g_u8g2, int color);
    extern void u8g2_DrawPixel(void *g_u8g2, int x, int y);
    extern void u8g2_DrawBox(void *g_u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *g_u8g2, int x, int y, int w);
    extern void u8g2_DrawFrame(void *g_u8g2, int x, int y, int w, int h);
}

#define INSPIRATION_FILE "/sdcard/outline/inspiration.json"
#define MAX_PREVIEW 30
static const long MAX_INSPIRATION_FILE_SIZE = 256 * 1024;

enum InspMode { IM_LIST, IM_EDIT_KEYWORD, IM_SEARCH, IM_HELP };

static struct {
    InspMode mode = IM_LIST;
    int sel = 0;
    int scroll = 0;
    JsonValue data;  // { items: [ { content: "...", keywords: "" } ] }
    std::vector<JsonValue> *items = nullptr;
    size_t itemCount = 0;

    AppState returnTo = APP_MAIN;
    AppState editorReturnTo = APP_MAIN;

    // keyword edit
    std::string editBuf;
    int editCur = 0;
    bool imeActive = false;
    int editIdx = -1;  // which item's keyword is being edited

    // help
    int helpScroll = 0;

    // search
    std::string searchBuf;
    int searchCur = 0;
    bool searchImeActive = false;
    std::vector<int> searchResults;  // indices into items matching search

    // editor return handling
    std::string pendingInspirationId;
    bool preservePos = false;  // 发送到Flomo返回后保留列表位置
} g;

static void loadData() {
    g.data = JsonValue::loadFromFile(INSPIRATION_FILE);
    if (g.data.isNull() || !g.data.has("items") || !g.data["items"].isArray()) {
        g.data = JsonValue::object();
        g.data.set("items", JsonValue::array());
    }
    g.items = &g.data["items"].elements;
    g.itemCount = g.data["items"].size();
}

static void saveData() {
    // Ensure directory exists
    mkdir("/sdcard/outline", 0777);
    JsonValue::saveToFile(INSPIRATION_FILE, g.data);
}

static std::string makeId() {
    time_t now; time(&now); struct tm *tm = localtime(&now);
    char buf[32];
    static int seq = 0;
    snprintf(buf, sizeof(buf), "i%02d%02d%02d_%d", tm->tm_hour, tm->tm_min, tm->tm_sec, seq++);
    return buf;
}

static void refreshItems() {
    if (g.data.has("items") && g.data["items"].isArray()) {
        g.items = &g.data["items"].elements;
        g.itemCount = g.data["items"].size();
    }
}

static void doSearch() {
    g.searchResults.clear();
    if (g.searchBuf.empty()) return;
    std::string query = g.searchBuf;
    // Case-insensitive: lower both query and target
    for (auto &c : query) if (c >= 'A' && c <= 'Z') c += 32;
    for (int i = 0; i < (int)g.itemCount; i++) {
        auto &item = (*g.items)[i];
        std::string content = item["content"].asString();
        std::string keywords = item["keywords"].asString();
        std::string text = content + " " + keywords;
        for (auto &c : text) if (c >= 'A' && c <= 'Z') c += 32;
        if (text.find(query) != std::string::npos)
            g.searchResults.push_back(i);
    }
}

static std::string readFileContent(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return ""; }
    if (sz > MAX_INSPIRATION_FILE_SIZE) { fclose(f); return ""; }
    std::string content(static_cast<size_t>(sz), '\0');
    if (fread(&content[0], 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
        fclose(f); return "";
    }
    fclose(f);
    return content;
}

// ── Drawing ──────────────────────────────────────────────────────────────

static void drawList() {
    ui_clear();
    ui_draw_text_content(4, g_font.ascent(), "灵感", false);
    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);

    int y = FONT_H + 8 + LINE_SPACING;
    int vis = (STATUS_Y - y + LINE_SPACING - 1) / LINE_SPACING;
    if (vis < 1) vis = 1;

    if (g.sel < g.scroll) g.scroll = g.sel;
    if (g.sel >= g.scroll + vis) g.scroll = g.sel - vis + 1;

    for (int i = 0; i < vis && (g.scroll + i) < (int)g.itemCount; i++) {
        int idx = g.scroll + i;
        auto &item = (*g.items)[idx];
        std::string content = item["content"].asString();
        // Truncate to MAX_PREVIEW chars (UTF-8 safe)
        int chars = 0;
        int end = 0;
        while (end < (int)content.length() && chars < MAX_PREVIEW) {
            unsigned char c = (unsigned char)content[end];
            if (c < 0x80) end++;
            else if ((c & 0xE0) == 0xC0) end += 2;
            else if ((c & 0xF0) == 0xE0) end += 3;
            else end += 1;
            chars++;
        }
        std::string preview = content.substr(0, end);
        if (end < (int)content.length()) preview += "...";

        std::string kw = item["keywords"].asString();
        char buf[96];
        if (!kw.empty())
            snprintf(buf, sizeof(buf), "%s [%s]", preview.c_str(), kw.c_str());
        else
            snprintf(buf, sizeof(buf), "%s", preview.c_str());

        bool s = (idx == g.sel);
        ui_draw_text_content(8, y + i * LINE_SPACING, buf, s);
    }

    if (g.itemCount == 0) ui_draw_text_content(8, y, "暂无灵感 — 按a添加");

    // Status bar: ?:帮助 | keywords (快捷键详见帮助面板)
    {
        char sl[128];
        int n = snprintf(sl, sizeof(sl), "?:帮助");
        if (g.itemCount > 0 && g.sel >= 0 && g.sel < (int)g.itemCount) {
            std::string kw = (*g.items)[g.sel]["keywords"].asString();
            if (!kw.empty()) n += snprintf(sl + n, sizeof(sl) - n, " | %s", kw.c_str());
        }
        ui_draw_status(sl, "");
    }
    ui_commit();
}

// 关键词输入行的基线：绘制与触摸命中共用（同设置界面选择器的做法）。
static int kwEditBaseline() {
    return ui_title_baseline() + g_font.descent() + 12 + g_font.ascent();
}

// 检索框里文本的左端 / 基线：同上，绘制与命中共用。
static int searchInputX() { return g_font.textWidth("检索:") + 8; }
static int searchInputY() { return g_font.ascent(); }

// 触摸选区的反白 + 按钮条（共享件 text_sel）。两个输入界面画完本体后各调一次。
// 这两个字段都是**单行**（整块缓冲画在一行上），行表因此只有一行。
static void inspFieldOverlay(const std::string &buf, int x0, int baseline) {
    if (!textSelActive()) return;
    TextSelLine ln;
    ln.start = 0;
    ln.end = (int)buf.size();
    ln.x0 = x0;
    ln.baseline = baseline;
    TextSelView view{&ln, 1, editorVkVisible() ? editorVkTop() : SCREEN_H};
    textSelDraw(buf, view);
}

static void drawKeywordEdit() {
    ui_clear();
    ui_draw_text_content_centered(ui_title_baseline(), "编辑关键词", false);
    u8g2_DrawHLine(g_u8g2, 0, ui_title_baseline() + g_font.descent() + 4, SCREEN_W);
    std::string display = g.editBuf.empty() ? " " : g.editBuf;
    int ty = kwEditBaseline();
    ui_draw_text_content(4, ty, display.c_str());
    int cx = g_font.textWidth(g.editBuf.substr(0, g.editCur).c_str());
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, 4 + cx, ty + 4, 8, 3);
    u8g2_SetDrawColor(g_u8g2, 1);

    if (editorVkVisible()) editorVkDraw();   // 键盘面板自带候选区，原来那条候选 UI 就不画了
    else if (g_ime.composing()) drawIMEUIFullscreen();
    else ui_draw_status("Enter保存 Esc取消", imeStatusLabel(g.imeActive).c_str());
    u8g2_SetDrawColor(g_u8g2, 0);

    // 触摸选区的反白 + 按钮条（会话没开就是空操作）。画在最上面：键盘在它下面。
    inspFieldOverlay(g.editBuf, 4, ty);
    ui_commit();
}

static void drawSearch() {
    ui_clear();

    // Search input area
    ui_draw_text_content(4, g_font.ascent(), "检索:", false);
    std::string display = g.searchBuf.empty() ? " " : g.searchBuf;
    int inputX = searchInputX();
    int inputY = searchInputY();
    g_content_font.drawText(inputX, inputY, display.c_str());
    int cx = inputX + g_font.textWidth(g.searchBuf.substr(0, g.searchCur).c_str());
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, cx, inputY + 4, 8, 3);
    u8g2_SetDrawColor(g_u8g2, 1);

    int sepY = FONT_H + 4;
    u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);

    const bool vk = editorVkVisible();
    bool searchComposing = g.searchImeActive && g_ime.composing() && !vk;

    // Search results
    int listY = sepY + LINE_SPACING;
    // 列表底边：虚拟键盘弹着时裁到键盘面板顶边，否则照旧（候选条在时再让一让）。
    int listMaxY = vk ? (editorVkTop() - LINE_SPACING)
                      : (searchComposing ? imeFullscreenPanelTopY() - LINE_SPACING : SCREEN_H);
    int vis = (listMaxY - listY + LINE_SPACING - 1) / LINE_SPACING;
    if (vis < 1) vis = 1;

    if (g.scroll < 0) g.scroll = 0;
    if (g.sel < g.scroll) g.scroll = g.sel;
    if (g.sel >= g.scroll + vis) g.scroll = g.sel - vis + 1;

    int resultCount = (int)g.searchResults.size();
    for (int i = 0; i < vis && (g.scroll + i) < resultCount; i++) {
        int idx = g.searchResults[g.scroll + i];
        auto &item = (*g.items)[idx];
        std::string content = item["content"].asString();
        int chars = 0; int end = 0;
        while (end < (int)content.length() && chars < MAX_PREVIEW) {
            unsigned char c = (unsigned char)content[end];
            if (c < 0x80) end++;
            else if ((c & 0xE0) == 0xC0) end += 2;
            else if ((c & 0xF0) == 0xE0) end += 3;
            else end += 1;
            chars++;
        }
        std::string preview = content.substr(0, end);
        if (end < (int)content.length()) preview += "...";

        std::string kw = item["keywords"].asString();
        char buf[96];
        if (!kw.empty())
            snprintf(buf, sizeof(buf), "%s [%s]", preview.c_str(), kw.c_str());
        else
            snprintf(buf, sizeof(buf), "%s", preview.c_str());

        bool s = (g.scroll + i == g.sel);
        ui_draw_text_content(8, listY + i * LINE_SPACING, buf, s);
    }

    if (resultCount == 0) {
        if (g.searchBuf.empty())
            ui_draw_text_content(8, listY, "输入关键词检索");
        else
            ui_draw_text_content(8, listY, "未找到匹配项");
    }

    if (vk) editorVkDraw();
    else if (searchComposing) drawIMEUIFullscreen();
    else ui_draw_status("Esc返回", imeStatusLabel(g.searchImeActive).c_str());

    // 触摸选区的反白 + 按钮条（会话没开就是空操作）。
    inspFieldOverlay(g.searchBuf, searchInputX(), searchInputY());
    ui_commit();
}

static const char *HELP_LINES[] = {
    "── 灵感面板 ──",
    "a     添加灵感",
    "d     删除灵感",
    "Enter 编辑内容",
    "k     编辑关键词",
    "/     检索灵感",
    "c     复制到剪贴板",
    "f     发送到Flomo(#灵感)",
    "q/Esc 返回",
    "",
    "── 通用 ──",
    "?     帮助",
    "Ctrl+I 打开灵感面板",
};
static const int HELP_LINE_COUNT = sizeof(HELP_LINES) / sizeof(HELP_LINES[0]);

static void drawHelp() {
    ui_clear();
    ui_draw_text_content_centered(ui_title_baseline(), "快捷键帮助", false);
    u8g2_DrawHLine(g_u8g2, 0, ui_title_baseline() + g_font.descent() + 4, SCREEN_W);
    int contentY = ui_title_baseline() + g_font.descent() + 12;
    int contentMaxY = STATUS_Y;
    int maxVis = (contentMaxY - contentY) / LINE_SPACING;
    if (maxVis < 1) maxVis = 1;
    int maxScroll = HELP_LINE_COUNT - maxVis;
    if (maxScroll < 0) maxScroll = 0;
    if (g.helpScroll > maxScroll) g.helpScroll = maxScroll;
    if (g.helpScroll < 0) g.helpScroll = 0;
    for (int i = 0; i < maxVis && (g.helpScroll + i) < HELP_LINE_COUNT; i++) {
        int ly = contentY + i * LINE_SPACING;
        const char *line = HELP_LINES[g.helpScroll + i];
        // 以前判"是否小节标题"（行首 U+E2xx 图标字形）当 bold 传下去，但那个形参从没
        // 被用过；小节之间的区分本来就是行首图标给的，判定删掉。
        ui_draw_text_content(12, ly + g_font.ascent(), line, false);
    }
    ui_draw_status("Esc返回", "");
    ui_commit();
}

// ── Init & Handle ───────────────────────────────────────────────────────

void screen_inspiration_init(AppState returnTo, AppState editorReturnTo) {
    g.returnTo = returnTo;
    if (returnTo == APP_EDITOR && editorReturnTo != APP_INSPIRATION) {
        g.editorReturnTo = editorReturnTo;
    }
    g.mode = IM_LIST;
    g.imeActive = false;
    g_ime.setActive(false);
    loadData();

    // Handle returning from editor — read temp file and update content
    if (!g.pendingInspirationId.empty()) {
        std::string tempPath = std::string("/sdcard/pjournal/__inspiration_") + g.pendingInspirationId;
        std::string content = readFileContent(tempPath);
        if (!content.empty()) {
            std::string body = extractBody(content);
            if (body.empty()) body = content;
            if (g.items) {
                for (auto &item : *g.items) {
                    if (item["id"].asString() == g.pendingInspirationId) {
                        item.set("content", body);
                        saveData();
                        break;
                    }
                }
            }
        }
        // Always clean up temp file (even if editor was cancelled)
        remove(tempPath.c_str());
        g.pendingInspirationId.clear();
        return; // preserve sel/scroll
    }

    // Fresh init — reset navigation (skip if returning from Flomo send)
    if (!g.preservePos) {
        g.sel = 0;
        g.scroll = 0;
    }
    g.preservePos = false;
}

AppState screen_inspiration_handle(int key, ScreenContext &ctx) {
    // 换了子状态 = 换了字段：上一个输入框的选区 / 按钮条作废。
    {
        static InspMode s_prevMode = IM_LIST;
        if (g.mode != s_prevMode) { textSelReset(); s_prevMode = g.mode; }
    }

    // ── IM_HELP ──
    if (g.mode == IM_HELP) {
        if (key == 0x1B || key == 'q' || key == 'Q' || key == 0x0A || key == 0x0D) {
            g.mode = IM_LIST;
        } else if (key == KEY_UP) {
            if (g.helpScroll > 0) g.helpScroll--;
        } else if (key == KEY_DOWN) {
            g.helpScroll++;
        }
        drawHelp();
        return APP_INSPIRATION;
    }

    // ── IM_EDIT_KEYWORD ──
    if (g.mode == IM_EDIT_KEYWORD) {
        {   // 虚拟键盘点按 → 键码
            int vkKey = 0;
            bool turnOn = false;
            if (editorVkPumpTap(g.imeActive, &vkKey, &turnOn)) {
                if (turnOn) { g.imeActive = true; g_ime.setActive(true); }
                key = vkKey;
            }
        }
        // 触摸编辑（共享件 text_sel）：长按输入行 → 复制/剪切/粘贴/全选。
        // 排在键盘泵**之后**（键盘点按优先），下面各按键分支之前。
        if (key == KEY_TOUCH_LONG || textSelActive()) {
            int tx = 0, ty = 0;
            const bool hasTap = input_tap_xy(&tx, &ty);
            TextSelLine fln;
            fln.start = 0;
            fln.end = (int)g.editBuf.size();
            fln.x0 = 4;
            fln.baseline = kwEditBaseline();
            TextSelView fview{&fln, 1, editorVkVisible() ? editorVkTop() : SCREEN_H};
            if (key == KEY_TOUCH_LONG) {
                if (hasTap && textSelBegin(g.editBuf, g.editCur, fview, tx, ty)) {
                    g_ime.cancelComposition();
                    drawKeywordEdit();
                    return APP_INSPIRATION;
                }
                key = 0x1B;   // 没落在输入行上 → 维持"长按 = 返回"的老语义
            } else if (key != 0 &&
                       textSelHandleKey(g.editBuf, g.editCur, fview, key, tx, ty, hasTap,
                                        &ctx.statusMessage)) {
                drawKeywordEdit();
                return APP_INSPIRATION;
            }
        }
        if (g.imeActive && key != 0) {
            std::string imeOut;
            if (g_ime.handleKey(key, imeOut)) {
                if (!imeOut.empty()) { g.editBuf.insert(g.editCur, imeOut); g.editCur += (int)imeOut.length(); }
                drawKeywordEdit(); return APP_INSPIRATION;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g.imeActive = !g.imeActive; g_ime.setActive(g.imeActive);
            drawKeywordEdit(); return APP_INSPIRATION;
        }
        if (key == 0x1B) {
            g.mode = IM_LIST; g.imeActive = false; g_ime.setActive(false);
            editorVkAutoHide();
        } else if (key == 0x0A || key == 0x0D) {
            if (g.editIdx >= 0 && g.editIdx < (int)g.itemCount) {
                (*g.items)[g.editIdx].set("keywords", g.editBuf);
                saveData();
            }
            g.mode = IM_LIST; g.imeActive = false; g_ime.setActive(false);
            editorVkAutoHide();
        } else if (key == 0x7F || key == 0x08) {
            if (g.editCur > 0) {
                int prev = g.editCur - 1;
                while (prev > 0 && ((unsigned char)g.editBuf[prev] & 0xC0) == 0x80) prev--;
                g.editBuf.erase(prev, g.editCur - prev); g.editCur = prev;
            }
        } else if (key >= 0x20 && key <= 0x7E) {
            g.editBuf.insert(g.editCur, 1, (char)key); g.editCur++;
        }
        drawKeywordEdit();
        return APP_INSPIRATION;
    }

    // ── IM_SEARCH ──
    if (g.mode == IM_SEARCH) {
        {   // 虚拟键盘点按 → 键码
            int vkKey = 0;
            bool turnOn = false;
            if (editorVkPumpTap(g.searchImeActive, &vkKey, &turnOn)) {
                if (turnOn) { g.searchImeActive = true; g_ime.setActive(true); }
                key = vkKey;
            }
        }
        // 触摸编辑（共享件 text_sel）：长按检索行 → 复制/剪切/粘贴/全选。
        if (key == KEY_TOUCH_LONG || textSelActive()) {
            int tx = 0, ty = 0;
            const bool hasTap = input_tap_xy(&tx, &ty);
            TextSelLine fln;
            fln.start = 0;
            fln.end = (int)g.searchBuf.size();
            fln.x0 = searchInputX();
            fln.baseline = searchInputY();
            TextSelView fview{&fln, 1, editorVkVisible() ? editorVkTop() : SCREEN_H};
            if (key == KEY_TOUCH_LONG) {
                if (hasTap && textSelBegin(g.searchBuf, g.searchCur, fview, tx, ty)) {
                    g_ime.cancelComposition();
                    drawSearch();
                    return APP_INSPIRATION;
                }
                key = 0x1B;   // 没落在检索行上 → 维持"长按 = 返回"的老语义
            } else if (key != 0 &&
                       textSelHandleKey(g.searchBuf, g.searchCur, fview, key, tx, ty, hasTap,
                                        &ctx.statusMessage)) {
                doSearch();   // 缓冲变了 → 结果重算
                g.sel = 0; g.scroll = 0;
                drawSearch();
                return APP_INSPIRATION;
            }
        }
        if (g.searchImeActive && key != 0) {
            std::string imeOut;
            if (g_ime.handleKey(key, imeOut)) {
                if (!imeOut.empty()) {
                    g.searchBuf.insert(g.searchCur, imeOut);
                    g.searchCur += (int)imeOut.length();
                    doSearch();
                    g.sel = 0; g.scroll = 0;
                }
                drawSearch(); return APP_INSPIRATION;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g.searchImeActive = !g.searchImeActive; g_ime.setActive(g.searchImeActive);
            drawSearch(); return APP_INSPIRATION;
        }
        if (key == KEY_FULLWIDTH_TOGGLE) {
            g_ime.toggleFullwidth();
            drawSearch(); return APP_INSPIRATION;
        }
        if (key == 0x1B) {
            g.mode = IM_LIST; g.searchImeActive = false; g_ime.setActive(false);
            g.searchBuf.clear(); g.searchCur = 0; g.searchResults.clear();
            editorVkAutoHide();
        } else if (key == 0x0A || key == 0x0D) {
            // Enter on a search result — edit content
            if (g.sel >= 0 && g.sel < (int)g.searchResults.size()) {
                int idx = g.searchResults[g.sel];
                auto &item = (*g.items)[idx];
                std::string id = item["id"].asString();
                g.pendingInspirationId = id;
                if (g.returnTo == APP_EDITOR) app_editor_stash_session();
                ctx.editContent = item["content"].asString();
                ctx.editFilename = "__inspiration_" + id;
                // 标题走 editorTitle；promptText 必须清空：非空就是"提示写作"，
                // 正文上方会多出一条表头（ctx 是长命的，别留下上一次的残值）。
                ctx.promptText.clear();
                ctx.editorTitle = "灵感";
                ctx.prevState = APP_INSPIRATION;
                ctx.nextState = APP_EDITOR;
                g.searchImeActive = false; g_ime.setActive(false);
                editorVkAutoHide();
                app_editor_request_reinit();
                return APP_EDITOR;
            }
        } else if (key == KEY_UP) {
            if (g.sel > 0) g.sel--;
        } else if (key == KEY_DOWN) {
            if (g.sel < (int)g.searchResults.size() - 1) g.sel++;
        } else if (key == 0x7F || key == 0x08) {
            if (g.searchCur > 0) {
                int prev = g.searchCur - 1;
                while (prev > 0 && ((unsigned char)g.searchBuf[prev] & 0xC0) == 0x80) prev--;
                g.searchBuf.erase(prev, g.searchCur - prev); g.searchCur = prev;
                doSearch();
                g.sel = 0; g.scroll = 0;
            }
        } else if (key >= 0x20 && key <= 0x7E) {
            g.searchBuf.insert(g.searchCur, 1, (char)key); g.searchCur++;
            doSearch();
            g.sel = 0; g.scroll = 0;
        }
        drawSearch();
        return APP_INSPIRATION;
    }

    // ── IM_LIST ──
    if (key == 0x1B || key == 'q' || key == 'Q') {
        if (g.returnTo == APP_EDITOR) {
            app_editor_restore_stashed_session();
            ctx.prevState = g.editorReturnTo;
        }
        ctx.nextState = g.returnTo;
        return g.returnTo;
    } else if (key == KEY_UP) {
        if (g.sel > 0) g.sel--;
    } else if (key == KEY_DOWN) {
        if (g.sel < (int)g.itemCount - 1) g.sel++;
    } else if (key == 'a' || key == 'A') {
        // Add new inspiration — open editor
        JsonValue item;
        item.set("id", makeId());
        item.set("content", "");
        item.set("keywords", "");
        if (!g.data.has("items") || !g.data["items"].isArray())
            g.data.set("items", JsonValue::array());
        g.data["items"].pushBack(item);
        saveData();
        refreshItems();
        g.sel = (int)g.itemCount - 1;
        g.pendingInspirationId = item["id"].asString();
        // Open text editor for content
        if (g.returnTo == APP_EDITOR) app_editor_stash_session();
        ctx.editContent = "";
        ctx.editFilename = "__inspiration_" + item["id"].asString();
        // 同上：标题走 editorTitle；promptText 清空（非空 = 提示写作）。
        ctx.promptText.clear();
        ctx.editorTitle = "灵感";
        ctx.prevState = APP_INSPIRATION;
        ctx.nextState = APP_EDITOR;
        app_editor_request_reinit();
        return APP_EDITOR;
    } else if ((key == 'd' || key == 'D') && g.sel >= 0 && g.sel < (int)g.itemCount) {
        auto &arr = g.data["items"];
        arr.elements.erase(arr.elements.begin() + g.sel);
        saveData();
        refreshItems();
        if (g.sel >= (int)g.itemCount) g.sel = (int)g.itemCount - 1;
        if (g.sel < 0) g.sel = 0;
    } else if ((key == 0x0A || key == 0x0D) && g.sel >= 0 && g.sel < (int)g.itemCount) {
        // Edit content in text editor
        auto &item = (*g.items)[g.sel];
        std::string id = item["id"].asString();
        g.pendingInspirationId = id;
        if (g.returnTo == APP_EDITOR) app_editor_stash_session();
        ctx.editContent = item["content"].asString();
        ctx.editFilename = "__inspiration_" + id;
        // 同上：标题走 editorTitle；promptText 清空（非空 = 提示写作）。
        ctx.promptText.clear();
        ctx.editorTitle = "灵感";
        ctx.prevState = APP_INSPIRATION;
        ctx.nextState = APP_EDITOR;
        app_editor_request_reinit();
        return APP_EDITOR;
    } else if ((key == 'k' || key == 'K') && g.sel >= 0 && g.sel < (int)g.itemCount) {
        g.editIdx = g.sel;
        g.editBuf = (*g.items)[g.sel]["keywords"].asString();
        g.editCur = (int)g.editBuf.length();
        g.imeActive = true; g_ime.setActive(true);
        g.mode = IM_EDIT_KEYWORD;
        editorVkAutoShow();   // 没连蓝牙键盘就弹虚拟键盘（有则内部不动）
    } else if ((key == 'c' || key == 'C') && g.sel >= 0 && g.sel < (int)g.itemCount) {
        // Copy to the shared clipboard
        clipboardPush((*g.items)[g.sel]["content"].asString());
        ctx.statusMessage = "已复制到粘贴板";
        ctx.statusDuration = 30;
    } else if ((key == 'f' || key == 'F') && g.sel >= 0 && g.sel < (int)g.itemCount) {
        // Send selected inspiration to Flomo with #灵感 tag
        std::string text = (*g.items)[g.sel]["content"].asString();
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            text.pop_back();
        if (text.empty()) {
            ctx.statusMessage = "内容为空";
            ctx.statusDuration = 30;
        } else {
            text += " #灵感";
            g_flomoPendingText = text;
            g_flomoReturnTo = APP_INSPIRATION;
            g.preservePos = true;
            ctx.nextState = APP_SYNC_SEND_FLOMO;
            return APP_SYNC_SEND_FLOMO;
        }
    } else if (key == '/') {
        g.searchBuf.clear(); g.searchCur = 0;
        g.searchResults.clear();
        g.searchImeActive = true; g_ime.setActive(true);
        g.sel = 0; g.scroll = 0;
        g.mode = IM_SEARCH;
        editorVkAutoShow();
        drawSearch(); return APP_INSPIRATION;
    } else if (key == '?') {
        g.helpScroll = 0;
        g.mode = IM_HELP;
        drawHelp();
        return APP_INSPIRATION;
    }

    drawList();
    return APP_INSPIRATION;
}
