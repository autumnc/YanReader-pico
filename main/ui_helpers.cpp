#include "ui_helpers.h"
#include "ui_render.h"   // 绘制/推屏决策都在 core1 的渲染任务里
#include "font_renderer.h"
#include "markdown_render.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "ime/IME.h"
#include "bt_keyboard.h"
#include "display.h"
#include "app_config.h"
#include "read_pico_pmu.h"
#include "e0470_epaper_waveform.h"
#include "u8g2_shim.h"
#include "epdiy.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>

// ── 运行时屏幕几何（由 epd 旋转决定）───────────────────────────────────
int ui_screen_w() { return epd_rotated_display_width(); }
int ui_screen_h() { return epd_rotated_display_height(); }

// ── Battery（PMU soc_permille，‰ → %）────────────────────────────────────
void battery_init() {
    // 电量/PMU 已在 read_pico_init 初始化，这里无需额外配置。
}

// 显示层单向步进（抗抖）。PMU 的 soc 是电压估的：一开 WiFi/一刷屏，负载突变就能让
// 它跳好几个点，照直显示就是电量数字反复弹跳。这里给"显示值"加一层：
//   · 充电（charge_state=CHARGING）只允许涨、放电（NOT_CHARGING）只允许跌，
//     每 30 秒最多走 1 个点 —— 单向是关键，它把来回弹的那几个点直接压掉；
//   · 换向（插上/拔掉充电器）立刻解锁一拍，免得"刚插上还没动"；
//   · 与真值差超过 5 个点就不再当抖动，直接跟上，别让数字卡住几分钟不更新；
//   · 充满只认 PMU 自己的 FULL_INFERRED，没确认前最高显示 99%。
// 参考 Metalio-E-INK4-Plus 的 bq27220_gauge.cc（同样单向步进 + 充满确认），
// 但输入换成 PMU 快照，不再自己算电压→SOC。
static const int64_t kBatteryStepUs = 30LL * 1000 * 1000;
static const int kBatterySnapPoints = 5;
static int s_batt_shown = -1;
static int s_batt_dir = 0;  // +1 充电 / -1 放电 / 0 状态未知
static int64_t s_batt_step_us = 0;

int battery_pct() {
    if (!read_pico_pmu_ready()) return -1;
    static int64_t last_read_us = 0;
    int64_t now = esp_timer_get_time();
    if (last_read_us != 0 && (now - last_read_us) < 5000000)
        return s_batt_shown;
    last_read_us = now;
    // poll 只刷 STATUS/QUICK/事件，够取电量，比全量 refresh 轻。
    if (read_pico_pmu_poll() != ESP_OK) return s_batt_shown;
    const pmu_snapshot_t *s = read_pico_pmu_get();
    if (!s) return s_batt_shown;
    // 0xFFFF = PMU 自己说"未知"（协议里 soc 无效的哨兵）。以前这里会算成 6553%
    // 再被夹到 100% —— 未知电量显示满电，方向正好相反，宁可保留上一次的值。
    if (s->soc_permille == 0xFFFF) return s_batt_shown;
    int target = s->soc_permille / 10;
    if (target < 0) target = 0;
    if (target > 100) target = 100;
    if (target >= 100 && s->charge_state != PMU_CHARGE_FULL_INFERRED) target = 99;

    int dir = 0;
    if (s->charge_state == PMU_CHARGE_CHARGING) dir = +1;
    else if (s->charge_state == PMU_CHARGE_NOT_CHARGING) dir = -1;

    if (s_batt_shown < 0) {  // 首帧：直接取真值，不做步进
        s_batt_shown = target;
        s_batt_dir = dir;
        s_batt_step_us = now;
        return s_batt_shown;
    }
    if (dir != s_batt_dir) {  // 插拔充电器：解锁一拍
        s_batt_dir = dir;
        s_batt_step_us = now;
    }
    const int gap = target - s_batt_shown;
    if (dir == 0 || gap > kBatterySnapPoints || gap < -kBatterySnapPoints) {
        s_batt_shown = target;  // 状态未知或差得太远：这不是负载抖动，直接跟上
    } else if (now - s_batt_step_us >= kBatteryStepUs) {
        const int want = (dir > 0) ? 1 : -1;
        if ((want > 0 && gap > 0) || (want < 0 && gap < 0)) {
            s_batt_shown += want;
            s_batt_step_us = now;
        }
    }
    return s_batt_shown;
}

// 是否正在充电（PMU charge_state）。与 battery_pct 共用同一份快照与 5s 限流。
bool battery_charging() {
    if (!read_pico_pmu_ready()) return false;
    battery_pct();   // 副作用即"必要时刷新一次快照"，省一套计时
    const pmu_snapshot_t *s = read_pico_pmu_get();
    // 只用 charge_state：语义明确（PMU_CHARGE_CHARGING=2）。status flags 里那一位
    // 协议注释写 bit2、宏写 bit3，按位取反而可能误判，不如不用。
    return s && s->charge_state == PMU_CHARGE_CHARGING;
}

// 设备电池文本:"电量";电量未知时返回空串
// 只给数字、不再带电池图标:数字已经把电量说清楚了,再顶一个图标是重复信息,白白多占
// 状态栏宽度(横屏状态栏本来就挤)。充电时仍跟一个闪电——那是数字里没有的信息。
std::string battery_text() {
    int bpct = battery_pct();
    if (bpct < 0) return "";
    char buf[24];
    snprintf(buf, sizeof(buf), "%d%%", bpct);
    std::string s = buf;
    if (battery_charging()) s += "\xEE\x80\xB9";   // U+E039 充电闪电
    return s;
}

// 设备电量 + 有蓝牙键盘时追加一个蓝牙图标。
// 键盘这里只表示"连没连上"，**不再显示键盘电量数字**：状态栏本来就只有一格宽
// （竖屏尤其挤），"蓝牙 85%" 那种写法把有限的地方又占去一块；键盘电量本来就是
// 个慢变量，盯着数字没有意义。真该管的时候——快没电了——改由一次性提示来说，
// 见 bt_keyboard 的 takeLowBatteryWarning() 与 main 主循环里的 statusMessage。
std::string battery_status_text() {
    std::string s = battery_text();
    if (s.empty()) return "";
    // U+E002 蓝牙图标,前导空格作为与电池组的间隔
    if (g_bt.isConnected()) s += " \xEE\x80\x82";
    return s;
}

// 纯电平图标(无数字)那两个变体 battery_icon_text / battery_icon_status_text 已删：
// 全工程零调用点 —— 状态栏一律走 battery_status_text 的数字形式，阅读器/待机/关于页
// 各自画裸 "%d%%"。E018..E02D 这些电平码点本来也只被它们用，一并去掉；码点本身是
// font_renderer 程序化绘制的矢量图形(不在 TTF 子集里)，删掉不省也不费字体空间。

// ── Word wrap helpers ────────────────────────────────────────────────────
static int charCellWidth(unsigned char c) {
    return (c < 0x80) ? 1 : 2;
}

static int utf8CharLen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

int byteToCells(const std::string &line, int byteOffset) {
    int cells = 0;
    for (int i = 0; i < byteOffset; ) {
        cells += charCellWidth((unsigned char)line[i]);
        i += utf8CharLen((unsigned char)line[i]);
    }
    return cells;
}

int cellsToByte(const std::string &line, int start, int end, int targetCells) {
    int cells = byteToCells(line, start);
    for (int ci = start; ci < end; ) {
        unsigned char c = (unsigned char)line[ci];
        int cc = charCellWidth(c);
        if (cells + cc > targetCells) {
            return (targetCells - cells <= cells + cc - targetCells) ? ci : ci + utf8CharLen(c);
        }
        cells += cc;
        ci += utf8CharLen(c);
    }
    return end;
}

// Leading markdown block marker length in bytes (0 if the line doesn't start
// with one). Keeps the marker ("# ", "> ", "- ", "- [ ] ", "1. ", ...) from
// being split onto its own vrow by the space word-break below. Nested list
// markers include their leading whitespace (mdListMarker.start).
static int mdPrefixLen(const std::string &line) {
    MdListMarker m = mdListMarker(line);
    if (m.ok) return m.start + m.len;
    int len = (int)line.size();
    if (len >= 2 && line[0] == '>' && line[1] == ' ') return 2;
    int h = 0;
    while (h < len && h < 6 && line[h] == '#') h++;
    if (h >= 1 && h < len && line[h] == ' ') return h + 1;
    return 0;
}

// Cells reserved per vrow for the RENDERED block indent so wrapped content
// doesn't overrun the screen. Must match markdown_render.cpp layout: heading at
// cell 2, list/task at marker cells (content at start+cells), quote 4 cells
// (bar at cell 4 + 2px gap). Nested markers reserve leading ws + marker cells.
static int mdIndentCells(const std::string &line) {
    MdListMarker m = mdListMarker(line);
    if (m.ok) return m.start + m.cells;
    int len = (int)line.size();
    if (len >= 2 && line[0] == '>' && line[1] == ' ') return 4;
    return mdPrefixLen(line) > 0 ? 2 : 0;  // heading
}

std::vector<VRow> buildVrows(const std::vector<std::string> &lines,
                             const std::vector<MdLineInfo> *mdInfoIn,
                             const std::set<int> *foldedHeadings) {
    std::vector<VRow> vrows;
    bool firstLineIndent = g_settings.firstLineIndent();
    std::vector<MdLineInfo> localInfo;
    const std::vector<MdLineInfo> *mdInfoPtr = mdInfoIn;
    if (!mdInfoPtr && firstLineIndent) {
        localInfo = mdClassifyLines(lines);
        mdInfoPtr = &localInfo;
    }
    std::vector<char> hidden = mdFoldHiddenLines(lines, mdInfoPtr, foldedHeadings);
    bool folding = foldedHeadings && !foldedHeadings->empty();
    for (int li = 0; li < (int)lines.size(); li++) {
        if (folding && hidden[li]) continue;
        const auto &line = lines[li];
        int len = (int)line.length();
        if (len == 0) {
            vrows.push_back({li, 0, 0});
            continue;
        }
        int maxc = SCREEN_W / g_font.halfAdvance();
        int indent = mdIndentCells(line);
        // 折叠标题行渲染为「级别图标 + uF09DA 折叠标志」共 4 格,比未折叠多 2 格;
        // 折行预留须同步加宽,否则换行处内容会画到屏幕右缘之外。
        if (folding && mdInfoPtr && (*mdInfoPtr)[li].headingLevel > 0 &&
            !(*mdInfoPtr)[li].inCodeBlock && foldedHeadings->count(li)) {
            indent = 4;
        }
        int prefixEnd = mdPrefixLen(line);
        int firstIndent = 0;
        if (firstLineIndent && mdInfoPtr) {
            const MdLineInfo &info = (*mdInfoPtr)[li];
            if (info.headingLevel == 0 && !info.list && !info.task &&
                !info.quote && !info.inCodeBlock && !info.hr) {
                firstIndent = 4;  // two Chinese-width characters
            }
        }
        int pos = 0;
        while (pos < len) {
            int cells = 0;
            int end = pos;
            int lastBreak = -1;
            int pe = (pos == 0) ? prefixEnd : 0;  // only the first vrow has the marker
            // cap 用标记的格数而非字节数(pe):CJK 标记(如 `一、`/`1、`)字节数大于格数,
            // 用 pe 会让首 vrow 内容多塞几格、画到屏幕右缘之外。
            int rowIndent = (pos == 0) ? firstIndent : 0;
            int cap = maxc - indent - rowIndent + ((pos == 0) ? byteToCells(line, prefixEnd) : 0);
            if (cap > maxc) cap = maxc;
            if (cap < 1) cap = 1;
            while (end < len) {
                unsigned char c = (unsigned char)line[end];
                int cc = charCellWidth(c);
                if (cells + cc > cap) break;
                cells += cc;
                int clen = utf8CharLen(c);
                if (c == ' ' && end >= pe) {
                    lastBreak = end + 1;
                } else if (c >= 0x80 && end >= pe) {
                    // CJK: 每个汉字都允许折行。否则连续无空格中文被当作一个单词,
                    // 英文+空格+汉字时会固定在空格处折行,英文行尾留大片空白。
                    lastBreak = end + clen;
                }
                end += clen;
            }
            if (end >= len) {
                vrows.push_back({li, pos, len, rowIndent});
                break;
            }
            if (lastBreak > pos) {
                vrows.push_back({li, pos, lastBreak, rowIndent});
                pos = lastBreak;
                while (pos < len && line[pos] == ' ') pos++;
            } else {
                vrows.push_back({li, pos, end, rowIndent});
                pos = end;
            }
        }
    }
    return vrows;
}

// ── IME singleton ─────────────────────────────────────────────────────────
IME &g_ime = IME::getInstance();

// ── IME drawing helper ────────────────────────────────────────────────────
// 与 drawIMEUI(anchorBottom=true) 的行位一致：候选行贴底、编码行依次向上。
// 原版固定 STATUS_Y-67 是为 22px 位图字号标定的；这里按 FONT_H 反推，随字号缩放。
int imeStatusPanelTopY() {
    int bottom = STATUS_BAR_Y;
    int sepY = bottom - (2 * FONT_H - g_font.ascent()) - 4;
    int codeBase = sepY - 7;
    return codeBase - g_font.ascent() - 6;
}

int imeFullscreenPanelTopY() {
    return SCREEN_H - (2 * FONT_H + 8);
}

int imeCandidateLineWidth() {
    return SCREEN_W - 12;
}

std::string imeStatusLabel(bool active) {
    if (!active) return "EN";
    if (g_ime.isDeleteMode()) return "[删]";
    if (g_ime.english()) return "[英]";
    std::string label = "[中]";
    // U+25CF ● / U+25D0 ◐：全黑 vs 圆内左半边黑，一眼能分出全角/半角。
    // 这两个字形 icon_font 里都没有，◐ 由 icon_font.c 程序化绘制（描圈+填左半），
    // ● 走 NF-Propo 的字形；命中测试按这两个码点识别（screen_editor.cpp）。
    label += g_ime.fullwidth() ? "\xe2\x97\x8f" : "\xe2\x97\x90";
    label += g_ime.trad() ? "繁" : "简";
    return label;
}

static void popUtf8Char(std::string &s) {
    if (s.empty()) return;
    size_t pos = s.size() - 1;
    while (pos > 0 && (((unsigned char)s[pos] & 0xC0) == 0x80)) pos--;
    s.erase(pos);
}

static std::string fitTextWidth(const std::string &text, int maxW) {
    if (maxW <= 0) return "";
    if (g_font.textWidth(text.c_str()) <= maxW) return text;
    const std::string ell = "...";
    if (g_font.textWidth(ell.c_str()) > maxW) return "";
    std::string out = text;
    while (!out.empty() && g_font.textWidth((out + ell).c_str()) > maxW)
        popUtf8Char(out);
    return out.empty() ? ell : out + ell;
}

static std::string shiftUtf8Char(std::string &s) {
    if (s.empty()) return "";
    size_t len = utf8CharLen((unsigned char)s[0]);
    std::string ch = s.substr(0, len);
    s.erase(0, len);
    return ch;
}

static std::string fitTextWidthMiddle(const std::string &text, int maxW) {
    if (maxW <= 0) return "";
    if (g_font.textWidth(text.c_str()) <= maxW) return text;
    const std::string ell = "...";
    if (g_font.textWidth(ell.c_str()) > maxW) return "";
    std::string head = text;
    std::string tail;
    bool trimHead = false;
    while (!head.empty() && g_font.textWidth((head + ell + tail).c_str()) > maxW) {
        if (trimHead) {
            tail = shiftUtf8Char(head) + tail;
        } else {
            popUtf8Char(head);
        }
        trimHead = !trimHead;
    }
    return head.empty() ? fitTextWidth(text, maxW) : head + ell + tail;
}

void drawIMEUI(int baseY, bool anchorBottom) {
    if (!g_ime.composing()) return;

    std::string code = g_ime.displayCode();
    std::string mode = g_ime.modeLabel();
    if (!mode.empty()) code = code.empty() ? mode : (mode + " " + code);
    auto &cands = g_ime.candidates();
    int pageSize = g_ime.pageSize();
    int curPage = g_ime.currentPage();
    int totalPages = g_ime.totalPages();
    if (totalPages < 1) totalPages = 1;

    char pageInfo[32];
    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);

    u8g2_SetDrawColor(g_u8g2, 1);

    // anchorBottom: 行位与编辑器 compose 条一致(候选行贴面板底边,
    // 基线离底 descent+3,分割线、编码行依次向上),词库管理等面板型调用用。
    // 底边锚定状态栏分割线 STATUS_BAR_Y 而非 baseY+67(=STATUS_Y,随字号浮动)。
    int codeBase, sepY, candBase;
    if (anchorBottom) {
        int bottom = STATUS_BAR_Y;
        candBase = bottom - g_font.descent() - 3;
        sepY = bottom - (2 * FONT_H - g_font.ascent()) - 4;
        codeBase = sepY - 7;
    } else {
        codeBase = baseY + 4 + g_font.ascent();
        sepY = baseY + FONT_H + 4;
        candBase = baseY + FONT_H + 8 + g_font.ascent();
    }

    // 白底清出候选条区域;anchorBottom 清到分割线为止,不抹掉状态栏
    u8g2_SetDrawColor(g_u8g2, 1);
    int panelBottom = anchorBottom ? STATUS_BAR_Y : SCREEN_H;
    u8g2_DrawBox(g_u8g2, 0, baseY, SCREEN_W,
                 panelBottom - baseY);
    u8g2_SetDrawColor(g_u8g2, 1);

    int tw = g_font.textWidth(pageInfo);
    int pw = tw + 8;
    int px = SCREEN_W - pw - 4;
    code = fitTextWidth(code, px - 12);
    int cw = g_font.textWidth(code.c_str()) + 8;
    u8g2_DrawBox(g_u8g2, 4, codeBase - g_font.ascent(), cw, FONT_H);
    u8g2_SetDrawColor(g_u8g2, 0);
    g_content_font.drawText(4, codeBase, code.c_str(), false);
    u8g2_SetDrawColor(g_u8g2, 1);

    u8g2_DrawBox(g_u8g2, px, codeBase - g_font.ascent(), pw, FONT_H);
    u8g2_SetDrawColor(g_u8g2, 0);
    g_content_font.drawText(px + 4, codeBase, pageInfo, false);
    u8g2_SetDrawColor(g_u8g2, 1);

    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);
    u8g2_SetDrawColor(g_u8g2, 1);

    int hl = g_ime.highlightIdx();
    int x = 4;
    for (int i = 0; i < (int)cands.size(); i++) {
        char idx[16];
        snprintf(idx, sizeof(idx), "%d.", (i % pageSize) + 1);
        std::string prefix = std::string(" ") + idx;
        std::string part = prefix + cands[i];
        int partW = g_font.textWidth(part.c_str());
        int availW = SCREEN_W - x - 8;
        bool truncated = false;
        if (partW > availW) {
            std::string word = fitTextWidthMiddle(cands[i], availW - g_font.textWidth(prefix.c_str()));
            part = prefix + word;
            partW = g_font.textWidth(part.c_str());
            truncated = true;
        }
        if (partW <= 0 || x + partW + 8 > SCREEN_W) break;
        // 白底清出该段区域,高亮候选反白(黑底白字)
        u8g2_SetDrawColor(g_u8g2, 1);
        u8g2_DrawBox(g_u8g2, x, candBase - g_font.ascent(), partW, FONT_H);
        if (i == hl) {
            u8g2_SetDrawColor(g_u8g2, 0);
            u8g2_DrawBox(g_u8g2, x, candBase - g_font.ascent(), partW, FONT_H);
            g_content_font.drawText(x, candBase, part.c_str(), true);   // 白字
        } else {
            u8g2_SetDrawColor(g_u8g2, 0);
            g_content_font.drawText(x, candBase, part.c_str(), false);  // 黑字
        }
        x += partW;
        if (truncated) break;
    }
    u8g2_SetDrawColor(g_u8g2, 1);
}

void drawIMEUIWithStatusBar() {
    drawIMEUI(imeStatusPanelTopY(), true);
}

void drawIMEUIFullscreen() {
    drawIMEUI(imeFullscreenPanelTopY(), false);
}

// 见头文件。非锚定布局的内容高固定是 2*FONT_H + 8（编码行 + 分割线 + 候选行），
// 想让候选行的下沿落在 bottom 上，面板顶就得从 bottom 往上退这么多。
// 落点与 drawIMEUIWithStatusBar() 那套一致（状态栏上沿之上 3px）。
void drawIMEUIFullscreenAboveStatusBar() {
    drawIMEUI(STATUS_BAR_Y - 3 - (2 * FONT_H + 8), false);
}

// ── UI Helpers ────────────────────────────────────────────────────────────
// 绘制/推屏的决策全部搬到了 core1 的渲染任务（ui_render.cpp），这里只剩入口。
void ui_clear() {
    if (!g_u8g2) return;
    // 取一块空闲工作缓冲当本帧绘制目标（可能阻塞：推屏追不上绘制）。整屏置白走
    // memset：u8g2_DrawBox 要逐像素做旋转映射 + 半字节读改写，83 万像素实测约
    // 100ms——比整幅正文重绘还贵；整屏都是白色，两个半字节都是 15，等价。
    uint8_t *fb = ui_render_begin_frame();
    if (!fb) return;
    memset(fb, 0xFF, u8g2_GetBufferSize(g_u8g2));
    u8g2_SetDrawColor(g_u8g2, 0);
}

// 提交本帧：core0 不等推屏，立刻可以去采样输入。差分、选波形、推屏都在 core1。
// 就地轻提示：帧尾盖一个不透明白框（见 ui_helpers.h 的说明）。
static char s_toastMsg[64] = {0};
static int64_t s_toastUntilUs = 0;
static int s_toastBottomY = 0;

void ui_toast_clear() {
    s_toastUntilUs = 0;
    s_toastMsg[0] = 0;
}

void ui_toast_show(const char *msg, int bottomY, int ms) {
    if (!msg || !*msg) { ui_toast_clear(); return; }
    snprintf(s_toastMsg, sizeof(s_toastMsg), "%s", msg);
    s_toastBottomY = bottomY;
    s_toastUntilUs = esp_timer_get_time() + (int64_t)ms * 1000;
}

bool ui_toast_active() {
    return s_toastUntilUs > 0 && esp_timer_get_time() < s_toastUntilUs;
}

// 画在**本帧内容之上**（所以是"盖在当前画面上"而不是清屏重画）。白底 + 黑框 +
// 黑字：三样都不透光，底下的正文一个字都透不出来。
static void ui_toast_draw() {
    if (!ui_toast_active()) { s_toastUntilUs = 0; return; }
    const int tw = g_font.textWidth(s_toastMsg);
    const int bw = tw + 24;
    const int bh = FONT_H + 10;
    int bx = (SCREEN_W - bw) / 2;
    if (bx < 0) bx = 0;
    const int bottom = (s_toastBottomY > 0) ? s_toastBottomY : STATUS_Y;
    int by = bottom - bh - 8;
    if (by < 0) by = 0;
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, bx, by, bw, bh);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, bx, by, bw, bh);
    // y 是基线（与 ui_draw_status 同式：框内垂直居中）。
    g_font.drawText(bx + 12, by + (bh - g_font.lineHeight()) / 2 + g_font.ascent(), s_toastMsg, false);
    u8g2_SetDrawColor(g_u8g2, 1);
}

void ui_commit() { ui_toast_draw(); ui_render_submit(false); }

// 长按中间确认键：立即整屏 GC16 全刷。不重绘——帧缓冲里就是当前画面，全刷只是
// 换 GC16 波形把它重画一遍，用来清掉局刷/DU 积累的残影。各模式通用（main.cpp）。
void ui_full_refresh_now() { ui_render_full_refresh(); }

// 只发送缓冲不更新参考帧（休眠提示用：唤醒后需恢复提示前的画面）。
void ui_send_buffer() { ui_render_full_refresh(); }

// 恢复待机前的画面：standbyClockDraw 之前 ui_render_keep_frame() 留过一份副本，
// 这里把它按 from-white 推回去（面板唤醒时刚被物理清成白底）。
void ui_restore_snapshot() { ui_render_restore_kept(); }

// 丢弃参考帧，使下一次 ui_commit 无条件整屏发送。
// 用于 light sleep 唤醒后面板被复位、需要强制重绘的场景。
void ui_invalidate_snapshot() { ui_render_invalidate(); }

// IME 候选/编码条局刷的合并窗口现在由渲染任务自己在队列超时里做（见 ui_render.cpp
// 的 flush_deferred），core0 不用再每轮冲刷。保留空函数以免改各界面调用点。
void ui_flush_ime_deferred() {}

void ui_set_fast_partial(bool enable) { ui_render_set_fast_partial(enable); }
void ui_set_local_only(bool enable) { ui_render_set_local_only(enable); }

int ui_text_width(const char *text) { return g_font.textWidth(text); }

void ui_draw_text(int x, int y, const char *text, bool invert, bool bold) {
    if (invert) {
        int w = g_font.textWidth(text);
        int bh = g_font.lineHeight();
        int asc = g_font.ascent();
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, x, y - asc, w, bh);
        u8g2_SetDrawColor(g_u8g2, 1);
        g_font.drawText(x, y, text, true);
        u8g2_SetDrawColor(g_u8g2, 0);
    } else {
        g_font.drawText(x, y, text, invert);
    }
}

void ui_draw_text_centered(int y, const char *text, bool invert, bool bold) {
    int w = g_font.textWidth(text);
    int x = (SCREEN_W - w) / 2; if (x < 0) x = 0;
    if (invert) {
        int bh = g_font.lineHeight();
        int asc = g_font.ascent();
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, x, y - asc, w, bh);
        u8g2_SetDrawColor(g_u8g2, 1);
        g_font.drawText(x, y, text, true);
        u8g2_SetDrawColor(g_u8g2, 0);
    } else {
        g_font.drawText(x, y, text, invert);
    }
}

void ui_draw_text_content(int x, int y, const char *text, bool invert) {
    // 与 ui_draw_text 同一套步骤，只换字面实例（度量共享，见 font_renderer.h）。
    if (invert) {
        int w = g_content_font.textWidth(text);
        int bh = g_content_font.lineHeight();
        int asc = g_content_font.ascent();
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, x, y - asc, w, bh);
        u8g2_SetDrawColor(g_u8g2, 1);
        g_content_font.drawText(x, y, text, true);
        u8g2_SetDrawColor(g_u8g2, 0);
    } else {
        g_content_font.drawText(x, y, text, false);
    }
}

void ui_draw_text_content_centered(int y, const char *text, bool invert) {
    int w = g_content_font.textWidth(text);
    int x = (SCREEN_W - w) / 2; if (x < 0) x = 0;
    ui_draw_text_content(x, y, text, invert);
}

void ui_draw_status(const char *left, const char *right, int rightReserve) {
    // 状态栏使用当前字号;编辑器的字号设置应同时影响正文和状态栏。
    int y = STATUS_BAR_Y;
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, y, SCREEN_W);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, 0, y + 1, SCREEN_W, FONT_H + 3);
    u8g2_SetDrawColor(g_u8g2, 0);
    // 文字在状态栏内垂直居中
    int textY = y + 1 + (STATUS_BAR_H - g_font.lineHeight()) / 2 + g_font.ascent();
    if (left) g_font.drawText(4, textY, left, false);
    if (right) {
        int rw = g_font.textWidth(right);
        int rx = SCREEN_W - rw - 4 - rightReserve;
        if (rx < 4) rx = 4;   // 让位让过头时别把字符串推到屏幕外
        g_font.drawText(rx, textY, right, false);
    }
    u8g2_SetDrawColor(g_u8g2, 1);
}

void ui_show_message_centered(const char *msg) {
    // 全屏消息优先：顺手收掉还没到点的就地提示，免得小框盖在对话框上。
    ui_toast_clear();
    // 先清屏,确保消息框是不透明对话框而不是盖在旧画面上
    ui_clear();
    int mw = g_font.textWidth(msg);
    int mx = (SCREEN_W - mw) / 2 - 8; if (mx < 0) mx = 0;
    int my = (SCREEN_H - FONT_H - 28) / 2 + 28;
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, mx, my, mw + 16, FONT_H + 8);
    u8g2_SetDrawColor(g_u8g2, 0);
    g_font.drawText(mx + 8, my + 4, msg, false);
    ui_commit();
}

void ui_draw_confirm_dialog(const char *l1, const char *l2, const char *l3) {
    // 行距 = FONT_H（原固定 30px 比 UI 行高还小，文字会互相挤压），
    // 框宽取最长一行的宽度，框高按 3 行 + 上下留白。
    int padX = 40, padTop = 24, padBottom = 22;
    int lineH = FONT_H;
    int w1 = g_font.textWidth(l1);
    int w2 = g_font.textWidth(l2);
    int w3 = g_font.textWidth(l3);
    int bw = w1; if (w2 > bw) bw = w2; if (w3 > bw) bw = w3;
    bw += padX * 2;
    int bh = padTop + 3 * lineH + padBottom;
    int bx = (SCREEN_W - bw) / 2;
    int by = (SCREEN_H - bh) / 2 - 20;
    if (by < 0) by = 0;

    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, bx, by, bw, bh);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, bx, by, bw, bh);
    int base = by + padTop + g_font.ascent();
    ui_draw_text_centered(base, l1);
    ui_draw_text_centered(base + lineH, l2);
    ui_draw_text_centered(base + 2 * lineH, l3);
    u8g2_SetDrawColor(g_u8g2, 1);
}

// ── WiFi helper functions ────────────────────────────────────────────────
bool ensure_wifi_connected() {
    if (g_wifi.isConnected()) return true;

    std::string ssid = g_settings.wifiSsid();
    std::string pass = g_settings.wifiPassword();
    if (ssid.empty()) return false;

    g_wifi.begin();
    if (!g_wifi.connect(ssid.c_str(), pass.c_str())) {
        return false;
    }

    for (int i = 0; i < 100; i++) {
        if (g_wifi.isConnected()) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

void restore_wifi_state(bool wasConnected) {
    if (!wasConnected) {
        g_wifi.disconnect();
    }
}

// ── Word/body helpers ────────────────────────────────────────────────────
int countVisibleChars(const std::string &text) {
    int count = 0;
    for (size_t i = 0; i < text.length();) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x80) {
            if (c > 0x20 && c < 0x7F) count++;
            i++;
        } else {
            count++;
            if ((c & 0xE0) == 0xC0) i += 2;
            else if ((c & 0xF0) == 0xE0) i += 3;
            else if ((c & 0xF8) == 0xF0) i += 4;
            else i++;
        }
    }
    return count;
}

std::string extractBody(const std::string &content) {
    if (content.empty()) return "";
    std::string result;
    size_t pos = 0;
    bool inMeta = true;
    while (pos < content.length()) {
        size_t nl = content.find('\n', pos);
        std::string line = (nl == std::string::npos) ? content.substr(pos) : content.substr(pos, nl - pos);
        if (inMeta) {
            std::string t = line;
            size_t f = t.find_first_not_of(" \t\r");
            if (f != std::string::npos) t = t.substr(f);
            if (t.empty() || t.find("日期:")==0 || t.find("字数:")==0 || t.find("提示词:")==0 || t=="自由写作") {}
            else { inMeta = false; if (!result.empty()) result += "\n"; result += line; }
        } else { if (!result.empty()) result += "\n"; result += line; }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return result;
}
