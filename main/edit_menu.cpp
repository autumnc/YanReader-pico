#include "edit_menu.h"

#include "clipboard.h"
#include "ui_helpers.h"

#include <esp_log.h>

#include <cstddef>
#include <string>
#include "u8g2_shim.h"

// ── 状态 ────────────────────────────────────────────────────────────────
static EditMenuKind s_kind = EM_NONE;
static int s_sel = 0;
static int s_bottom = 0;        // 正文区底边；<=0 = 屏底

static bool s_pickOpen = false;
static int s_pickSel = 0;
static int s_pickScroll = 0;

static const char *kEditLabels[] = {"复制", "剪切", "粘贴", "全选", "润色"};
static const char *kPasteLabels[] = {"粘贴", "全选", "取消"};
static const char *kFieldLabels[] = {"复制", "剪切", "粘贴", "全选"};
static const char *kPickTitle = "粘贴板";

// ── 按钮条 ──────────────────────────────────────────────────────────────
int editMenuCount() {
    if (s_kind == EM_EDIT) return 5;
    if (s_kind == EM_PASTE_ONLY) return 3;
    if (s_kind == EM_FIELD) return 4;
    return 0;
}

const char *editMenuLabel(int i) {
    if (i < 0) return "";
    if (s_kind == EM_EDIT) return (i < 5) ? kEditLabels[i] : "";
    if (s_kind == EM_PASTE_ONLY) return (i < 3) ? kPasteLabels[i] : "";
    if (s_kind == EM_FIELD) return (i < 4) ? kFieldLabels[i] : "";
    return "";
}

static void bandRect(int *bx, int *by, int *bw, int *bh) {
    // 按钮条是**界面框架**：它贴在正文底边之上，几何恒按界面字号，不跟正文字号一起长。
    // 绘制与命中都从这里算，所以钉在这里一处就够（见 screen_editor_handle 的说明）。
    FontScope ui(FontRenderer::uiPxHeight());
    *bw = SCREEN_W;
    *bh = 2 * FONT_H + 18;
    *bx = 0;
    int bottom = (s_bottom > 0 && s_bottom <= SCREEN_H) ? s_bottom : SCREEN_H;
    *by = bottom - *bh - 4;
    if (*by < 0) *by = 0;
}

void editMenuOpen(EditMenuKind kind) {
    s_kind = kind;
    s_sel = 0;
}

void editMenuClose() {
    s_kind = EM_NONE;
    s_sel = 0;
}

bool editMenuActive() { return s_kind != EM_NONE; }

EditMenuKind editMenuKind() { return s_kind; }

void editMenuSetBottom(int bottomY) { s_bottom = bottomY; }

int editMenuSel() { return s_sel; }

void editMenuSetSel(int i) {
    const int n = editMenuCount();
    if (n <= 0) { s_sel = 0; return; }
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    s_sel = i;
}

void editMenuMove(int delta) {
    const int n = editMenuCount();
    if (n <= 0) return;
    s_sel = ((s_sel + delta) % n + n) % n;
}

int editMenuHit(int x, int y) {
    FontScope ui(FontRenderer::uiPxHeight());
    const int n = editMenuCount();
    if (n <= 0) return -1;
    int bx, by, bw, bh;
    bandRect(&bx, &by, &bw, &bh);
    if (y < by || y >= by + bh) return -1;
    const int seg = bw / n;
    if (seg <= 0) return -1;
    int i = (x - bx) / seg;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return i;
}

void editMenuDraw() {
    FontScope ui(FontRenderer::uiPxHeight());
    UI_FONT_GUARD();
    const int n = editMenuCount();
    if (n <= 0) return;
    int bx, by, bw, bh;
    bandRect(&bx, &by, &bw, &bh);

    // 黑框 + 白底：正文在下面，这里把面板范围挖白，文字才点得清
    // （与编辑器快捷菜单面板 drawEditorMenuBox 同一套画法）。
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, bx, by, bw, bh);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, bx + 2, by + 2, bw - 4, bh - 4);

    const int seg = bw / n;
    u8g2_SetDrawColor(g_u8g2, 0);
    for (int i = 1; i < n; i++) u8g2_DrawBox(g_u8g2, bx + i * seg, by + 2, 1, bh - 4);
    u8g2_SetDrawColor(g_u8g2, 1);

    const int baseline = by + bh / 2 + g_font.ascent() / 2;
    for (int i = 0; i < n; i++) {
        const char *lb = editMenuLabel(i);
        int x = bx + i * seg + (seg - g_font.textWidth(lb)) / 2;
        if (x < bx + i * seg + 6) x = bx + i * seg + 6;   // 键太窄时别贴边
        ui_draw_text(x, baseline, lb, i == s_sel);
    }
    u8g2_SetDrawColor(g_u8g2, 0);
}

// ── 粘贴板列表 ──────────────────────────────────────────────────────────
// 每行只显示开头一段：条目最长 4KB，直接画会溢出浮层、量宽度也要命。
#define PICK_PREVIEW_CHARS 18

static std::string previewOf(const std::string &s) {
    std::string o;
    int chars = 0;
    for (size_t i = 0; i < s.size() && chars < PICK_PREVIEW_CHARS;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
        if (i + len > s.size()) break;
        for (size_t k = 0; k < len; k++) {
            char ch = s[i + k];
            // 连续字节不会是 0x0A/0x0D/0x09，所以这里只吃真换行/制表符。
            o += (ch == '\n' || ch == '\r' || ch == '\t') ? ' ' : ch;
        }
        chars++;
        i += len;
    }
    if (o.size() < s.size()) o += "…";
    return o;
}

static void pickerRect(int n, int *bx, int *by, int *bw, int *bh, int *rows) {
    // 粘贴板选择器是**界面框架**（浮在正文之上）：几何恒按界面字号。
    FontScope ui(FontRenderer::uiPxHeight());
    int maxRows = (SCREEN_H - 4 * FONT_H) / LINE_SPACING;   // 上下各留两行余量
    if (maxRows > 10) maxRows = 10;
    if (maxRows < 3) maxRows = 3;
    int r = (n < maxRows) ? n : maxRows;
    if (r < 1) r = 1;
    *rows = r;

    int w = g_font.textWidth(kPickTitle) + 2 * FONT_H;
    for (int i = 0; i < n; i++) {
        const int tw = g_font.textWidth(previewOf(clipboardAt(i)).c_str()) + 3 * FONT_H;
        if (tw > w) w = tw;
    }
    if (w < 240) w = 240;
    if (w > SCREEN_W - 24) w = SCREEN_W - 24;
    *bw = w;
    *bh = 8 + FONT_H + 6 + (*rows) * LINE_SPACING + 8;
    *bx = (SCREEN_W - w) / 2;
    *by = (SCREEN_H - *bh) / 2;
}

// 选项 i 的文字基线（绘制与命中共用，与设置界面选择器同一套几何）。
static int pickerRowY(int by, int i) {
    FontScope ui(FontRenderer::uiPxHeight());
    const int sepY = by + 8 + FONT_H + 2;
    return sepY + 6 + g_font.ascent() + i * LINE_SPACING;
}

void editMenuOpenPicker() {
    s_pickOpen = true;
    s_pickSel = 0;
    s_pickScroll = 0;
    if (clipboardCount() <= 0) s_pickSel = -1;
}

void editMenuClosePicker() {
    s_pickOpen = false;
    s_pickSel = -1;
    s_pickScroll = 0;
}

bool editMenuPickerActive() { return s_pickOpen; }

int editMenuPickerSel() { return s_pickOpen ? s_pickSel : -1; }

void editMenuPickerSetSel(int i) { s_pickSel = i; }

void editMenuPickerMove(int delta) {
    const int n = clipboardCount();
    if (n <= 0) return;
    if (s_pickSel < 0) { s_pickSel = 0; return; }
    s_pickSel += delta;
    if (s_pickSel < 0) s_pickSel = 0;
    if (s_pickSel > n - 1) s_pickSel = n - 1;
}

void editMenuPickerDraw() {
    FontScope ui(FontRenderer::uiPxHeight());
    UI_FONT_GUARD();
    if (!s_pickOpen) return;
    const int n = clipboardCount();
    int bx, by, bw, bh, rows;
    pickerRect(n, &bx, &by, &bw, &bh, &rows);
    if (s_pickSel < s_pickScroll) s_pickScroll = s_pickSel;
    if (s_pickSel >= s_pickScroll + rows) s_pickScroll = s_pickSel - rows + 1;
    if (s_pickScroll < 0) s_pickScroll = 0;

    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, bx, by, bw, bh);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, bx, by, bw, bh);

    ui_draw_text(bx + 10, by + 8 + g_font.ascent(), kPickTitle, false, true);
    u8g2_DrawHLine(g_u8g2, bx + 4, by + 8 + FONT_H + 2, bw - 8);

    if (n <= 0) {
        const char *msg = "（粘贴板是空的）";
        const int y = pickerRowY(by, 0);
        ui_draw_text(bx + (bw - g_font.textWidth(msg)) / 2, y, msg, false);
    } else {
        for (int i = 0; i < rows && s_pickScroll + i < n; i++) {
            const int idx = s_pickScroll + i;
            const int y = pickerRowY(by, i);
            const std::string lb = previewOf(clipboardAt(idx));
            const int tx = bx + (bw - g_font.textWidth(lb.c_str())) / 2;
            if (idx == s_pickSel) {
                u8g2_SetDrawColor(g_u8g2, 0);
                u8g2_DrawBox(g_u8g2, bx + 4, y - g_font.ascent() - 2, bw - 8, FONT_H + 4);
                u8g2_SetDrawColor(g_u8g2, 1);
                g_font.drawText(tx, y, lb.c_str(), true);
                u8g2_SetDrawColor(g_u8g2, 0);
            } else {
                g_font.drawText(tx, y, lb.c_str(), false);
            }
        }
    }
    u8g2_SetDrawColor(g_u8g2, 0);
}

int editMenuPickerHit(int x, int y) {
    FontScope ui(FontRenderer::uiPxHeight());
    if (!s_pickOpen) return -1;
    const int n = clipboardCount();
    if (n <= 0) return -1;
    int bx, by, bw, bh, rows;
    pickerRect(n, &bx, &by, &bw, &bh, &rows);
    if (x < bx || x >= bx + bw || y < by || y >= by + bh) return -1;
    const int top = pickerRowY(by, 0) - g_font.ascent() - 2;
    const int r = (y - top) / LINE_SPACING;
    if (y < top || r < 0 || r >= rows) return -1;   // 点在标题/分隔线上
    const int idx = s_pickScroll + r;
    if (idx < 0 || idx >= n) return -1;
    return idx;
}
