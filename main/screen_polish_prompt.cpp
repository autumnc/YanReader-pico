#include "screen_polish_prompt.h"
#include "font_renderer.h"
#include "settings_manager.h"
#include "ime/IME.h"
#include "ui/ime_field.h"  // 输入框绑定：落串/退格/光标的 UTF-8 算术（与写作/计划/阅读共用一份）
#include "ui_helpers.h"
#include "editor_vk.h"   // 虚拟键盘：没连蓝牙键盘时的唯一输入途径（自己解析点按）
#include "text_sel.h"    // 输入框的触摸选区（三模式共享件）
#include "hw/input.h"    // input_tap_xy：长按落点（键盘泵没消费时才取得到）

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "u8g2_shim.h"

// ── State ─────────────────────────────────────────────────────────────────
static struct {
    std::string buf;
    int cur = 0;       // 字节偏移
    int scroll = 0;    // 首个可见 vrow 索引
    bool imeActive = false;
} g;

static ImeField ppPromptField() { return ImeField{&g.buf, &g.cur}; }

// 按 '\n' 切逻辑行,返回每行的起始字节偏移。
static void splitLines(const std::string &s, std::vector<std::string> &lines, std::vector<int> &starts) {
    lines.clear(); starts.clear();
    size_t pos = 0;
    while (pos <= s.length()) {
        size_t nl = s.find('\n', pos);
        lines.push_back((nl == std::string::npos) ? s.substr(pos) : s.substr(pos, nl - pos));
        starts.push_back((int)pos);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

// 定位字节偏移 cur 所在逻辑行,输出行号、行起始偏移、行内格列位置。
static void locate(const std::string &s, int cur, int &lineIdx, int &lineStart, int &xCells) {
    std::vector<std::string> lines; std::vector<int> starts;
    splitLines(s, lines, starts);
    for (size_t i = 0; i < lines.size(); i++) {
        int ls = starts[i];
        int le = ls + (int)lines[i].length();
        if (cur >= ls && cur <= le) {
            lineIdx = (int)i; lineStart = ls;
            xCells = byteToCells(lines[i], cur - ls);
            return;
        }
        if (cur < ls) break;
    }
    if (!lines.empty()) {
        int i = (int)lines.size() - 1;
        lineIdx = i; lineStart = starts[i];
        xCells = byteToCells(lines[i], cur - lineStart);
    }
}

// ── 版式：绘制与触摸命中共用 ──────────────────────────────────────────────
// 一次算出正文的可见 vrow 表、正文区顶/底边、可见行数、光标所在 vrow，并把
// g.scroll 夹到光标可见。绘制和触摸选区都从这里取几何，避免两边各算一遍算岔。
static void ppLayout(std::vector<std::string> &lines, std::vector<int> &starts,
                     std::vector<VRow> &vrows, int &top, int &bodyBottom, int &vis, int &curVR) {
    splitLines(g.buf, lines, starts);
    vrows = buildVrows(lines);

    int lineIdx, lineStart, xCells;
    locate(g.buf, g.cur, lineIdx, lineStart, xCells);
    curVR = -1;
    for (int i = 0; i < (int)vrows.size(); i++) {
        if (vrows[i].lineIdx == lineIdx && vrows[i].start <= (g.cur - lineStart) &&
            (g.cur - lineStart) <= vrows[i].end) {
            curVR = i; break;
        }
    }
    if (curVR < 0) curVR = 0;

    top = ui_title_baseline() + g_font.descent() + 10;
    // 正文区底边：虚拟键盘弹着时裁到键盘面板顶边（否则正文会被面板盖住），
    // 没弹时照旧给全屏候选条让位。
    bodyBottom = (editorVkVisible() ? editorVkTop() : imeFullscreenPanelTopY()) - 10;
    vis = (bodyBottom - top + LINE_SPACING - 1) / LINE_SPACING;
    if (vis < 1) vis = 1;
    if (g.scroll > curVR) g.scroll = curVR;
    if (curVR >= g.scroll + vis) g.scroll = curVR - vis + 1;
    if (g.scroll < 0) g.scroll = 0;
}

// 可见 vrow → 触摸选区的行表（缓冲字节偏移 + 屏幕 x0/基线）。只放**当前可见**的
// 行：触摸本来就点不到卷出去的行，反白也只可能落在屏上。
static void ppLineTable(const std::vector<std::string> &lines, const std::vector<int> &starts,
                        const std::vector<VRow> &vrows, int top, int vis,
                        std::vector<TextSelLine> &out, int &bottom) {
    out.clear();
    for (int i = 0; i < vis && (g.scroll + i) < (int)vrows.size(); i++) {
        const VRow &vr = vrows[g.scroll + i];
        TextSelLine ln;
        ln.start = starts[vr.lineIdx] + vr.start;
        ln.end = starts[vr.lineIdx] + vr.end;
        ln.x0 = 8 + vr.indentCells * g_font.halfAdvance();
        ln.baseline = top + i * LINE_SPACING;
        out.push_back(ln);
    }
    bottom = editorVkVisible() ? editorVkTop() : SCREEN_H;
}

// ── Drawing ───────────────────────────────────────────────────────────────
static void drawPromptEditor() {
    ui_clear();
    ui_draw_text_centered(ui_title_baseline(), "润色提示词", false, true);
    u8g2_DrawHLine(g_u8g2, 0, ui_title_baseline() + g_font.descent() + 4, SCREEN_W);

    std::vector<std::string> lines; std::vector<int> starts;
    std::vector<VRow> vrows;
    int top = 0, bodyBottom = 0, vis = 0, curVR = 0;
    ppLayout(lines, starts, vrows, top, bodyBottom, vis, curVR);

    int lineIdx, lineStart, xCells;
    locate(g.buf, g.cur, lineIdx, lineStart, xCells);

    for (int i = 0; i < vis && (g.scroll + i) < (int)vrows.size(); i++) {
        const VRow &vr = vrows[g.scroll + i];
        std::string row = lines[vr.lineIdx].substr(vr.start, vr.end - vr.start);
        if (row.empty()) row = " ";
        ui_draw_text(8 + vr.indentCells * g_font.halfAdvance(), top + i * LINE_SPACING, row.c_str());
    }
    if (vrows.empty()) ui_draw_text(8, top, " ");

    int dispRow = curVR - g.scroll;
    int x = 8 + (vrows[curVR].indentCells +
                 byteToCells(lines[lineIdx], g.cur - lineStart) -
                 byteToCells(lines[lineIdx], vrows[curVR].start)) * g_font.halfAdvance();
    std::string curCh = g.buf.substr(g.cur, 1);
    int cw = (curCh.empty() || curCh == "\n") ? 8 : g_font.textWidth(curCh.c_str());
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, x, top + dispRow * LINE_SPACING + 4, cw, 3);
    u8g2_SetDrawColor(g_u8g2, 1);

    if (editorVkVisible()) {
        editorVkDraw();   // 面板自带候选区，原来那条全屏候选 UI 就不画了
    } else if (g.imeActive && g_ime.composing()) {
        drawIMEUIFullscreen();
    } else {
        ui_draw_status("Ctrl+S保存 Esc取消", imeStatusLabel(g.imeActive).c_str());
    }

    // 触摸选区的反白 + 按钮条（会话没开就是空操作）。画在最上面。
    {
        std::vector<TextSelLine> tl;
        int bottom = 0;
        ppLineTable(lines, starts, vrows, top, vis, tl, bottom);
        TextSelView view{tl.data(), (int)tl.size(), bottom};
        textSelDraw(g.buf, view);
    }
    ui_commit();
}

// ── Screen entry points ───────────────────────────────────────────────────
void screen_polish_prompt_init() {
    g.buf = g_settings.polishPrompt();
    g.cur = (int)g.buf.length();
    g.scroll = 0;
    g.imeActive = false;
    textSelReset();   // 上次留下的选区/按钮条作废（会话是模块级静态量，会跨屏残留）
    g_ime.setActive(false);
    editorVkAutoShow();   // 没连蓝牙键盘就把虚拟键盘弹出来
}

AppState screen_polish_prompt_handle(int key, ScreenContext &ctx) {
    // 虚拟键盘：落在键盘面板上的点按翻译成键码，喂给下面的输入逻辑。
    {
        int vkKey = 0;
        bool turnOn = false;
        if (editorVkPumpTap(g.imeActive, &vkKey, &turnOn)) {
            if (turnOn) { g.imeActive = true; g_ime.setActive(true); }
            key = vkKey;
        }
    }
    // 触摸编辑（共享件 text_sel）：长按正文 → 复制/剪切/粘贴/全选。
    // **排在键盘泵之后**（键盘点按优先）、**IME 分支之前**：否则拖动帧/长按会漏进
    // g_ime.handleKey——有未上屏组合时未知键会把第一个候选字莫名上屏。
    if (key == KEY_TOUCH_LONG || textSelActive()) {
        int tx = 0, ty = 0;
        const bool hasTap = input_tap_xy(&tx, &ty);
        std::vector<std::string> lines; std::vector<int> starts;
        std::vector<VRow> vrows;
        int top = 0, bodyBottom = 0, vis = 0, curVR = 0;
        ppLayout(lines, starts, vrows, top, bodyBottom, vis, curVR);
        std::vector<TextSelLine> tl;
        int bottom = 0;
        ppLineTable(lines, starts, vrows, top, vis, tl, bottom);
        TextSelView view{tl.data(), (int)tl.size(), bottom};
        if (key == KEY_TOUCH_LONG) {
            if (hasTap && textSelBegin(g.buf, g.cur, view, tx, ty)) {
                g_ime.cancelComposition();
                drawPromptEditor();
                return APP_POLISH_PROMPT;
            }
            key = 0x1B;   // 没落在正文行上 → 维持"长按 = 返回"的老语义
        } else if (key != 0 &&
                   textSelHandleKey(g.buf, g.cur, view, key, tx, ty, hasTap, &ctx.statusMessage)) {
            drawPromptEditor();
            return APP_POLISH_PROMPT;
        }
    }

    if (g.imeActive && key != 0) {
        std::string imeOut;
        // 多行字段：Enter 本来就落 '\n'（见下面那一支），输入法上屏带出的 '\n' 也别吞。
        if (imeFieldKeyText(g_ime, key, /*multiline=*/true, imeOut)) {
            if (!imeOut.empty()) {
                imeFieldInsert(ppPromptField(), imeOut);
            }
            drawPromptEditor();
            return APP_POLISH_PROMPT;
        }
    }
    if (key == KEY_IME_TOGGLE) {
        g.imeActive = !g.imeActive;
        g_ime.setActive(g.imeActive);
        drawPromptEditor();
        return APP_POLISH_PROMPT;
    }
    if (key == KEY_FULLWIDTH_TOGGLE) {
        g_ime.toggleFullwidth();
        drawPromptEditor();
        return APP_POLISH_PROMPT;
    }
    if (key == 0x13) {  // Ctrl+S 保存
        g_settings.setPolishPrompt(g.buf);
        g_ime.setActive(false);
        editorVkAutoHide();
        ctx.statusMessage = "润色提示词已保存";
        ctx.statusDuration = 30;
        return APP_SETTINGS;
    }
    if (key == 0x1B) {  // Esc 取消
        g_ime.setActive(false);
        editorVkAutoHide();
        return APP_SETTINGS;
    }

    if (key == 0x0A || key == 0x0D) {  // Enter 换行
        imeFieldInsert(ppPromptField(), "\n");
    } else if (key == 0x7F || key == 0x08) {  // Backspace
        imeFieldBackspace(ppPromptField());
    } else if (key == KEY_LEFT) {
        imeFieldMoveLeft(ppPromptField());
    } else if (key == KEY_RIGHT) {
        imeFieldMoveRight(ppPromptField());
    } else if (key == KEY_UP) {
        int lineIdx, lineStart, xCells;
        locate(g.buf, g.cur, lineIdx, lineStart, xCells);
        if (lineIdx > 0) {
            std::vector<std::string> lines; std::vector<int> starts;
            splitLines(g.buf, lines, starts);
            const std::string &target = lines[lineIdx - 1];
            g.cur = starts[lineIdx - 1] +
                cellsToByte(target, 0, (int)target.length(), xCells);
        }
    } else if (key == KEY_DOWN) {
        int lineIdx, lineStart, xCells;
        locate(g.buf, g.cur, lineIdx, lineStart, xCells);
        std::vector<std::string> lines; std::vector<int> starts;
        splitLines(g.buf, lines, starts);
        if (lineIdx < (int)lines.size() - 1) {
            const std::string &target = lines[lineIdx + 1];
            g.cur = starts[lineIdx + 1] +
                cellsToByte(target, 0, (int)target.length(), xCells);
        }
    } else if (key >= 0x20 && key <= 0x7E) {
        imeFieldInsert(ppPromptField(), std::string(1, (char)key));
    }

    drawPromptEditor();
    return APP_POLISH_PROMPT;
}
