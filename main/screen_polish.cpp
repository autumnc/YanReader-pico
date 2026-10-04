#include "pjournal_app.h"
#include "screen_polish.h"
#include "screen_editor.h"
#include "deepseek_client.h"
#include "font_renderer.h"
#include "ui_helpers.h"
#include "ime/IME.h"
#include "wifi_manager.h"
#include "editor_vk.h"   // 虚拟键盘：优化指令对话框没连蓝牙键盘时的唯一输入途径

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern u8g2_t *g_u8g2;

extern "C" {
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
}

// Work chain: DeepSeek polish, show result. The dialog (R key) edits the extra
// instruction before re-running the chain.
enum PolishPhase {
    P_WORKING,
    P_RESULT,
    P_ERROR,
    P_EDIT_INSTR,
};

static struct {
    PolishPhase phase = P_WORKING;
    bool requested = false;      // work chain has run
    bool emptyContent = false;   // editor text was empty at entry
    std::string original;        // captured editor text
    std::string result;          // polished text
    std::string resultSource;    // "DeepSeek"
    std::string error;           // final error message
    std::string customInstr;     // user's extra instruction (R dialog)
    int scroll = 0;
    std::vector<std::string> lines;   // result split by '\n'
    std::vector<VRow> vrows;          // wrapped result rows
    // instruction dialog
    std::string instrBuf;
    int instrCur = 0;
    bool imeActive = false;
} g;

// Set by the entry path (BOOT double-click = whole text, Ctrl+O = selection).
static PolishScope g_scope = POLISH_WHOLE;

void screen_polish_set_scope(PolishScope scope) {
    g_scope = scope;
}

// The chain (WiFi + DeepSeek HTTP) runs in a background task so the main loop
// stays responsive and can honour Esc-cancel. The task never touches the panel;
// the handle loop draws every frame while it runs.
static volatile bool s_chainDone = false;
static volatile bool s_cancel = false;

static bool isEmptyText(const std::string &s) {
    for (char c : s)
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') return false;
    return true;
}

static void prepareResult() {
    g.lines.clear();
    std::string cur;
    size_t n = g.result.size();
    if (n > 0 && g.result.back() == '\n') n--;
    for (size_t i = 0; i < n; i++) {
        char c = g.result[i];
        if (c == '\n') { g.lines.push_back(cur); cur.clear(); }
        else cur += c;
    }
    g.lines.push_back(cur);
    g.vrows = buildVrows(g.lines);
    g.scroll = 0;
}

// ── Drawing ──────────────────────────────────────────────────────────────

static void drawWorking(const char *msg) {
    ui_clear();
    ui_draw_text(4, g_font.ascent(), "AI润色", false, true);
    ui_draw_text_centered(SCREEN_H / 2, msg);
    ui_draw_status(s_cancel ? "正在取消..." : "按Esc取消", "");
    ui_commit();
}

static void drawResult() {
    ui_clear();
    char src[32];
    snprintf(src, sizeof(src), "来源:%s", g.resultSource.c_str());
    ui_draw_text(4, g_font.ascent(), g_scope == POLISH_SELECTION ? "AI润色(选区)" : "AI润色", false, true);
    ui_draw_text(SCREEN_W - g_font.textWidth(src) - 4, g_font.ascent(), src);
    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);

    int y = FONT_H + 8 + LINE_SPACING;
    int vis = (STATUS_Y - y + LINE_SPACING - 1) / LINE_SPACING;
    if (vis < 1) vis = 1;
    int maxScroll = (int)g.vrows.size() - vis;
    if (maxScroll < 0) maxScroll = 0;
    if (g.scroll > maxScroll) g.scroll = maxScroll;
    if (g.scroll < 0) g.scroll = 0;

    for (int i = 0; i < vis && (g.scroll + i) < (int)g.vrows.size(); i++) {
        const VRow &vr = g.vrows[g.scroll + i];
        std::string row = g.lines[vr.lineIdx].substr(vr.start, vr.end - vr.start);
        ui_draw_text(8 + vr.indentCells * g_font.halfAdvance(), y + i * LINE_SPACING, row.c_str());
    }
    if (g.vrows.empty()) ui_draw_text(8, y, "(无内容)");

    ui_draw_status("Enter确认 R重润色 Esc取消", "");
    ui_commit();
}

static void drawError() {
    ui_clear();
    ui_draw_text(4, g_font.ascent(), "润色失败", false, true);
    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);
    int y = FONT_H + 8 + LINE_SPACING;
    std::vector<std::string> elines = { g.error };
    auto evrows = buildVrows(elines);
    int vis = (STATUS_Y - y + LINE_SPACING - 1) / LINE_SPACING;
    if (vis < 1) vis = 1;
    for (int i = 0; i < vis && i < (int)evrows.size(); i++) {
        std::string row = g.error.substr(evrows[i].start, evrows[i].end - evrows[i].start);
        ui_draw_text(8 + evrows[i].indentCells * g_font.halfAdvance(), y + i * LINE_SPACING, row.c_str());
    }
    ui_draw_status("任意键返回", "");
    ui_commit();
}

static void drawInstrDialog() {
    ui_clear();
    ui_draw_text_centered(ui_title_baseline(), "优化指令", false, true);
    u8g2_DrawHLine(g_u8g2, 0, ui_title_baseline() + g_font.descent() + 4, SCREEN_W);

    int y = ui_title_baseline() + g_font.descent() + 12;
    ui_draw_text(4, y + g_font.ascent(), "默认原则:轻度润色,保留原文风格与叙事,", false);
    ui_draw_text(4, y + g_font.ascent() + LINE_SPACING, "以语义通顺为主,避免大幅修改。", false);

    int inputBaseline = y + g_font.ascent() + 2 * LINE_SPACING + g_font.ascent();
    ui_draw_text(4, inputBaseline, "补充指令:");
    std::string display = g.instrBuf.empty() ? " " : g.instrBuf;
    int inputX = 4 + g_font.textWidth("补充指令:");
    g_font.drawText(inputX, inputBaseline, display.c_str());
    int cx = inputX + g_font.textWidth(g.instrBuf.substr(0, g.instrCur).c_str());
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, cx, inputBaseline + 4, 8, 3);
    u8g2_SetDrawColor(g_u8g2, 1);

    if (editorVkVisible()) {
        editorVkDraw();   // 面板自带候选区，原来那条全屏候选 UI 就不画了
    } else if (g.imeActive && g_ime.composing()) {
        drawIMEUIFullscreen();
    } else {
        ui_draw_status("Enter确认 Esc取消", imeStatusLabel(g.imeActive).c_str());
    }
    ui_commit();
}

// ── Work chain ───────────────────────────────────────────────────────────

// Runs the blocking chain (WiFi + DeepSeek HTTP) in a task so the main loop
// stays responsive and can honour Esc-cancel. Draws nothing; the handle loop
// repaints the working frame each cycle.
static void runPolishChainTask(void *arg) {
    (void)arg;
    bool wasConnected = g_wifi.isConnected();
    bool ok = false;
    bool wifiOk = ensure_wifi_connected();

    if (wifiOk && !s_cancel) {
        DeepseekResult dr = g_deepseek.polishText(g.original, g.customInstr, &s_cancel);
        if (dr.success) {
            g.result = dr.content;
            g.resultSource = "DeepSeek";
            ok = true;
        } else {
            g.error = dr.content;
        }
    } else {
        g.error = s_cancel ? "已取消" : "WiFi连接失败";
    }

    restore_wifi_state(wasConnected);
    g.phase = ok ? P_RESULT : P_ERROR;
    if (ok) prepareResult();
    s_chainDone = true;
    vTaskDelete(nullptr);
}

// ── Init & Handle ────────────────────────────────────────────────────────

void screen_polish_init() {
    g.phase = P_WORKING;
    g.requested = false;
    g.scroll = 0;
    g.customInstr.clear();
    g.instrBuf.clear();
    g.instrCur = 0;
    g.imeActive = false;
    g_ime.setActive(false);
    IME::getInstance().setPageSize(7);
    s_cancel = false;
    s_chainDone = false;

    g.original = (g_scope == POLISH_SELECTION) ? app_get_selected_text() : app_get_editor_text();
    g.emptyContent = isEmptyText(g.original);
}

AppState screen_polish_handle(int key, ScreenContext &ctx) {
    if (g.emptyContent) {
        ctx.statusMessage = "编辑区内容为空";
        ctx.statusDuration = 30;
        return APP_EDITOR;
    }

    // ── instruction dialog (R key) ──
    if (g.phase == P_EDIT_INSTR) {
        // 虚拟键盘点按：落在键盘面板上的点按翻译成键码，喂给下面的输入逻辑。
        {
            int vkKey = 0;
            bool turnOn = false;
            if (editorVkPumpTap(g.imeActive, &vkKey, &turnOn)) {
                if (turnOn) { g.imeActive = true; g_ime.setActive(true); }
                key = vkKey;
            }
        }
        if (g.imeActive && key != 0) {
            std::string imeOut;
            if (g_ime.handleKey(key, imeOut)) {
                if (!imeOut.empty()) {
                    g.instrBuf.insert(g.instrCur, imeOut);
                    g.instrCur += (int)imeOut.length();
                }
                drawInstrDialog();
                return APP_POLISH;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g.imeActive = !g.imeActive;
            g_ime.setActive(g.imeActive);
            drawInstrDialog();
            return APP_POLISH;
        }
        if (key == KEY_FULLWIDTH_TOGGLE) {
            g_ime.toggleFullwidth();
            drawInstrDialog();
            return APP_POLISH;
        }
        if (key == 0x1B) {
            // Esc: cancel the dialog, back to the result page.
            g.phase = P_RESULT;
            g.imeActive = false;
            g_ime.setActive(false);
            editorVkAutoHide();
            drawResult();
            return APP_POLISH;
        }
        if (key == 0x0A || key == 0x0D) {
            // Enter: accept the instruction (empty → default polish) and re-run.
            g.customInstr = g.instrBuf;
            g.imeActive = false;
            g_ime.setActive(false);
            editorVkAutoHide();
            g.phase = P_WORKING;
            g.requested = false;
            return APP_POLISH;
        }
        if (key == 0x7F || key == 0x08) {
            if (g.instrCur > 0) {
                int prev = g.instrCur - 1;
                while (prev > 0 && ((unsigned char)g.instrBuf[prev] & 0xC0) == 0x80) prev--;
                g.instrBuf.erase(prev, g.instrCur - prev);
                g.instrCur = prev;
            }
        } else if (key >= 0x20 && key <= 0x7E) {
            g.instrBuf.insert(g.instrCur, 1, (char)key);
            g.instrCur++;
        }
        drawInstrDialog();
        return APP_POLISH;
    }

    // ── work phase: chain runs in a task so Esc can cancel ──
    if (g.phase == P_WORKING) {
        if (!g.requested) {
            g.requested = true;
            s_cancel = false;
            s_chainDone = false;
            TaskHandle_t h = nullptr;
            if (xTaskCreate(runPolishChainTask, "polish", 8192, nullptr, 1, &h) != pdPASS) {
                g.error = "系统繁忙,请重试";
                g.phase = P_ERROR;
                drawError();
                return APP_POLISH;
            }
            drawWorking("DeepSeek润色中...");
            return APP_POLISH;
        }
        if (key == 0x1B || key == 'q' || key == 'Q') s_cancel = true;
        if (s_chainDone) {
            if (s_cancel) {
                g_ime.setActive(app_ime_active());
                return APP_EDITOR;
            }
            if (g.phase == P_RESULT) {
                drawResult();
                return APP_POLISH;
            }
            drawError();
            return APP_POLISH;
        }
        drawWorking("DeepSeek润色中...");
        return APP_POLISH;
    }

    // ── result page ──
    if (g.phase == P_RESULT) {
        if (key == 0x0A || key == 0x0D) {
            if (g_scope == POLISH_SELECTION) editorReplaceSelection(g.result);
            else editorReplaceAllText(g.result);
            g_ime.setActive(app_ime_active());
            return APP_EDITOR;
        }
        if (key == 'r' || key == 'R') {
            g.instrBuf = g.customInstr;   // pre-fill so the user can adjust
            g.instrCur = (int)g.instrBuf.length();
            g.imeActive = true;
            g_ime.setActive(true);
            g.phase = P_EDIT_INSTR;
            editorVkAutoShow();   // 没连蓝牙键盘就弹虚拟键盘（有则内部不动）
            drawInstrDialog();
            return APP_POLISH;
        }
        if (key == 0x1B || key == 'q' || key == 'Q') {
            g_ime.setActive(app_ime_active());
            return APP_EDITOR;
        }
        if (key == KEY_UP) {
            if (g.scroll > 0) g.scroll--;
        } else if (key == KEY_DOWN) {
            g.scroll++;
        }
        drawResult();
        return APP_POLISH;
    }

    // ── error page: any key returns to the editor ──
    if (g.phase == P_ERROR) {
        if (key != 0) {
            g_ime.setActive(app_ime_active());
            return APP_EDITOR;
        }
        drawError();
        return APP_POLISH;
    }

    drawResult();
    return APP_POLISH;
}
