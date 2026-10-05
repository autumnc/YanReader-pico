#include "ui_helpers.h"
#include "ui_render.h"   // 绘制/推屏决策都在 core1 的渲染任务里
#include "font_renderer.h"
#include "markdown_render.h"
#include "ttf_font.h"     // 候选行的直绘/测宽（屏幕像素高由「候选字大小」定，不走格子模型）
#include "icon_font.h"    // 未装外置字体时拉丁字母的半格字形
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

// ── 界面框架字号哨兵（见 ui_helpers.h）──────────────────────────────────
void uiFontGuard(const char *who) {
    const int cur = g_font.pxHeight();
    const int ui = FontRenderer::uiPxHeight();
    if (cur == ui) return;   // 正常路径：一次比较，到此为止
    // 对不上才走这里。who 是各入口的 __func__，静态存储期，存指针不会悬空；
    // 但字符串字面量可能被链接器合并，所以按内容比而不是按指针比。
    // 记满这几位就不再记新的：真有多个入口同时错，头几条足够定位。
    static const char *reported[8] = {};
    static int nReported = 0;
    for (int i = 0; i < nReported; i++)
        if (std::strcmp(reported[i], who) == 0) return;
    if (nReported < 8) reported[nReported++] = who;
    ESP_LOGE("UIFont",
             "%s: 画界面框架时格子是 %dpx，界面档是 %dpx —— 少了 "
             "FontScope(FontRenderer::uiPxHeight())，或把它写成了别的档 / 位置不对",
             who, cur, ui);
}

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
// 一个字符的**步进(px)**。只按首字节判断（<0x80 = 拉丁，其余一律全角），与老
// charCellWidth 同口径；变的只有拉丁那一支：从前恒为半格，现在按**内容面的真实
// 步进**前进（装了外置字体时拉丁是比例的），与 FontRenderer::charWidth() 同一条规则。
// 这里是自由函数、没有 FontRenderer 实例，所以复述一遍规则 —— 复述的只有"谁"，
// "多少"仍旧只有 ttf 层一处（ttf_char_advance_px）。
//   内置内容面 → 半格（= 没装外置字体时的逐像素原样）；
//   装了外置字体 → 那个字面的真实步进（内置的 ASCII 步进表，O(1)，见 ttf_font.c）。
static int charAdvancePx(unsigned char c) {
    if (c >= 0x80) return g_font.cjkAdvance();
    if (ttf_font_is_builtin()) return g_font.halfAdvance();
    return ttf_char_advance_px(TTF_ROLE_CONTENT, g_font.pxHeight(), (uint32_t)c);
}

static int utf8CharLen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// 行内字节偏移 → 距行首的像素 x。老的 byteToCells 的像素版（"格"已废）。
int byteToX(const std::string &line, int byteOffset) {
    if (byteOffset > (int)line.size()) byteOffset = (int)line.size();
    int x = 0;
    for (int i = 0; i < byteOffset; ) {
        x += charAdvancePx((unsigned char)line[i]);
        i += utf8CharLen((unsigned char)line[i]);
    }
    return x;
}

// [start, end) 内离 targetX 最近的字符**边界**字节偏移（x 是距行首的像素）。
// 平手取靠左的那个边界，所以 xToByte(line, 0, len, byteToX(line, k)) == k 恒成立。
int xToByte(const std::string &line, int start, int end, int targetX) {
    if (start < 0) start = 0;
    if (end > (int)line.size()) end = (int)line.size();
    int x = byteToX(line, start);
    for (int ci = start; ci < end; ) {
        unsigned char c = (unsigned char)line[ci];
        int cc = charAdvancePx(c);
        if (x + cc > targetX) {
            return (targetX - x <= x + cc - targetX) ? ci : ci + utf8CharLen(c);
        }
        x += cc;
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

// 每条 vrow 为该行**渲染出来的块记号**预留的像素宽，好让折下去的内容不冲出屏幕。
// 口径必须与 markdown_render.cpp 的排版一致（见那里的 mdPrefixAdvancePx）：
// 引用 4 格、标题 2 格（折叠 4 格，由调用方覆盖）、列表/待办 = 记号本身占的格。
// 嵌套记号的预留含前导空白。
//
// **有序记号是唯一要"量"的一支**：它的原文照原样画，装了外置字体后宽度不再是
// 整数格。算式与 mdPrefixAdvancePx 共用 mdRawMarkerIndentPx（内置面下与
// (m.start+m.cells)*半格 逐像素相同，所以老布局不变）。
//
// 引用这里刻意还是 4 格、**不加** mdPrefixAdvancePx 那个 +2px：改造前预留就是 4 格
// 而内容从 4 格+2px 起（少留 2px 的老毛病）。保留原值 = 内置面用户的行宽一字不差。
static int mdIndentPx(const std::string &line) {
    const int cell = g_font.halfAdvance();
    MdListMarker m = mdListMarker(line);
    if (m.ok) {
        if (m.ordered) return mdRawMarkerIndentPx(line, m);
        return (m.start + m.cells) * cell;
    }
    int len = (int)line.size();
    if (len >= 2 && line[0] == '>' && line[1] == ' ') return 4 * cell;
    return mdPrefixLen(line) > 0 ? 2 * cell : 0;  // heading
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
        // 折行预算 = SCREEN_W 里**能排下的整格数** × 半格。刻意不是 SCREEN_W：
        // 改造前 maxc = SCREEN_W / halfAdvance() 是整除（960/22 = 43 格），右边
        // 那点零头(14px)本来就没用上。直接用 SCREEN_W 会让**没装外置字体的人**
        // 也整体重排一遍 —— 那不是这次要改的东西。
        const int cellw = g_font.halfAdvance();
        const int maxpx = (SCREEN_W / cellw) * cellw;
        int indentPx = mdIndentPx(line);
        // 折叠标题行渲染为「级别图标 + uF09DA 折叠标志」共 4 格,比未折叠多 2 格;
        // 折行预留须同步加宽,否则换行处内容会画到屏幕右缘之外。
        if (folding && mdInfoPtr && (*mdInfoPtr)[li].headingLevel > 0 &&
            !(*mdInfoPtr)[li].inCodeBlock && foldedHeadings->count(li)) {
            indentPx = 4 * cellw;
        }
        int prefixEnd = mdPrefixLen(line);
        int firstIndentPx = 0;
        if (firstLineIndent && mdInfoPtr) {
            const MdLineInfo &info = (*mdInfoPtr)[li];
            if (info.headingLevel == 0 && !info.list && !info.task &&
                !info.quote && !info.inCodeBlock && !info.hr) {
                firstIndentPx = 4 * cellw;  // two Chinese-width characters
            }
        }
        int pos = 0;
        while (pos < len) {
            int used = 0;
            int end = pos;
            int lastBreak = -1;
            int pe = (pos == 0) ? prefixEnd : 0;  // only the first vrow has the marker
            // 首 vrow 的记号字节已经画在预留区里了，所以把它占的宽**加回**预算。
            // 用 byteToX(prefixEnd) 而不是它占的格数：CJK 记号(如 `一、`/`1、`)的
            // 字节数大于格数，也大于它真正画的宽；量出来的才是它实际吃的宽度。
            int rowIndentPx = (pos == 0) ? firstIndentPx : 0;
            int cap = maxpx - indentPx - rowIndentPx + ((pos == 0) ? byteToX(line, prefixEnd) : 0);
            if (cap > maxpx) cap = maxpx;
            // 至少容得下当前位置这一个字 —— 否则内层 while 一个都不收，end 不前进，
            // 外层 while 就死循环了（"格"时代 cap≥1 天然够，改成像素后不够）。
            int firstAdv = charAdvancePx((unsigned char)line[pos]);
            if (cap < firstAdv) cap = firstAdv;
            while (end < len) {
                unsigned char c = (unsigned char)line[end];
                int cc = charAdvancePx(c);
                if (used + cc > cap) break;
                used += cc;
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
                vrows.push_back({li, pos, len, rowIndentPx});
                break;
            }
            if (lastBreak > pos) {
                vrows.push_back({li, pos, lastBreak, rowIndentPx});
                pos = lastBreak;
                while (pos < len && line[pos] == ' ') pos++;
            } else {
                vrows.push_back({li, pos, end, rowIndentPx});
                pos = end;
            }
        }
    }
    return vrows;
}

// ── IME singleton ─────────────────────────────────────────────────────────
IME &g_ime = IME::getInstance();

// ── 编辑区正文字号（"显示与版式 → 正文字号"）──────────────────────────────
// 见 ui_helpers.h。45 = 与界面 20pt(line_height) 同高的标准档，也就是"没改过"时的
// 逐像素原样。编辑器正文整块的排版都按这个字号算（实现见 font_renderer.h 的 FontScope）。
int editorBodyFontPx() {
    int px = atoi(g_settings.getString("editor_font_size", "45").c_str());
    if (px < 28) px = 45;      // 没设过/写坏 → 回标准档
    if (px > 96) px = 96;      // 上限：与候选字号同一量程
    return px;
}

// ── 输入法候选行：字号 / 度量 / 测宽 / 直绘（唯一权威实现）────────────────
// 见 ui_helpers.h。这一套原来只存在于 editor_vk.cpp（static），于是实体键盘的输入法条
// 只能按**界面字号**画、按界面字号量：「候选字大小」改了它纹丝不动，而输入法分页早已
// 按候选字号算宽度，两种口径一混，"一行放几个"就对不上（大字号时最后一两个候选被挤出
// 屏幕右边，看不见也点不到）。搬到这一份之后，虚拟键盘的候选条与 drawIMEUI 共用同一个
// 字号、同一份量法、同一支直绘。
int imeCandFontPx() {
    int px = atoi(g_settings.getString("ime_cand_size", "45").c_str());
    if (px < 28) px = 45;      // 没设过/写坏 → 回标准档
    if (px > 96) px = 96;      // 上限：再大面板就吃光正文了
    return px;
}

// 候选字的 ascent。照 FontRenderer::setSize 的口径：字体没就绪时用 0.78em 近似。
int imeCandAscent() {
    const int px = imeCandFontPx();
    int a = ttf_ascender_px(px);
    if (a <= 0 || a >= px) a = px * 78 / 100;
    return a;
}

// 候选串在候选字号下的像素宽：拉丁按内容字面的**真实步进**（内置面 = 半格），
// 其余取该字号下的字形宽。与 imeCandDrawText 同一份口径（那边照步进前进）。
int imeCandStrW(const char *s) {
    if (!s) return 0;
    const int px = imeCandFontPx();
    int w = 0;
    while (*s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c < 0x80) {
            w += ttf_font_is_builtin()
                     ? px / 2
                     : ttf_char_advance_px(TTF_ROLE_CONTENT, px, (uint32_t)c);
            s += 1;
            continue;
        }
        const int m = (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
        char ch[5];
        memcpy(ch, s, m);
        ch[m] = '\0';
        w += ttf_text_width_px(px, ch);
        s += m;
    }
    return w;
}

// 按候选字号画一段候选串：ASCII 走 NF-Propo 等宽（同 FontRenderer 的路由），CJK 走
// TTF 直绘。invert = 反白（外部已填黑底，这里把字画白）。
void imeCandDrawText(int x, int baseline, const char *s, bool invert) {
    const int px = imeCandFontPx();
    uint8_t *fb = u8g2_GetBufferPtr(g_u8g2);
    const uint8_t fg = invert ? 15 : 0;
    const uint8_t bg = invert ? 0 : 15;
    // 候选串是**输入法正文**，走内容面（= 用户选的字体），与编辑器正文同一路。
    // 必须显式选面：虚拟键盘的键帽是 g_vk_font 画的，s_cur 大概率正停在内置面上，
    // 不选就会让候选字顶着内置字体渲染。画完还原调用方的面。
    const int prev_role = ttf_get_role();
    ttf_set_role(TTF_ROLE_CONTENT);
    // 拉丁字母按 FontRenderer 同一口径：装了外置字体就用用户字体的字形、**按字体自己的
    // 步进前进**（不再居中塞半格）；没装就走内置等宽路（半格 + 图标字体字形）。
    // imeCandStrW 量的正是这份步进，两边不会打架。
    while (*s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c < 0x80) {
            if (ttf_font_is_builtin()) {
                icon_font_draw_baseline(fb, x, baseline, px / 2, px, c, invert);
                x += px / 2;
            } else {
                char ch[2] = {static_cast<char>(c), '\0'};
                ttf_draw_text_px(fb, x, baseline, px, ch, TTF_ALIGN_LEFT, fg, bg);
                x += ttf_char_advance_px(TTF_ROLE_CONTENT, px, (uint32_t)c);
            }
            s += 1;
            continue;
        }
        const int m = (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
        char ch[5];
        memcpy(ch, s, m);
        ch[m] = '\0';
        const int gw = ttf_text_width_px(px, ch);
        ttf_draw_text_px(fb, x + (px - gw) / 2, baseline, px, ch, TTF_ALIGN_LEFT, fg, bg);
        x += px;   // CJK advance = 一个字号宽，与原 line_height_ 口径一致
        s += m;
    }
    ttf_set_role(prev_role);   // 还原调用方的面（见函数头）
}

// 编码行的量宽：钉在 g_ime_font（拉丁恒等宽）上。fitTextWidth 的 measure 形参是
// **函数指针**，成员函数传不进去，所以套这一层；不套就会用 g_font 的比例拉丁去截、
// 却用 g_ime_font 的等宽去画 —— 量画分家，截出来的 "..." 位置对不上。
static int imeCodeStrW(const char *s) { return g_ime_font.textWidth(s); }

// ── 输入法条（编码行 + 候选行）的两行高 ──────────────────────────────────
// 行高**刻意与虚拟键盘不同**：键盘面板的键要指尖点得着，候选行给到 px+20；这条是贴在
// 正文下面的窄条，px+8 就够，别白吃正文行数。45(标准) 下两行共 106，与改造前那条
// (107) 基本一致，所以默认档看不出变化；「候选字大小」调大/调小这条跟着长/缩。
// 编码行仍按**界面字号**画（与旧行为一致），所以它至少要有 FONT_H 高。
static int imeBarRowH(int row) {
    const int px = imeCandFontPx();
    const int textH = (row == 0 && px < FONT_H) ? FONT_H : px;
    return textH + 8;
}

// 面板总高（编码行 + 候选行）。面板贴 bottomY 往上排，所以顶边 = bottomY - 本值。
// 非 static：阅读模式（screen_reader.cpp）要在**没弹虚拟键盘**时给这条留出正文底边，
// 两处必须同一个式子，不然候选条一出现正文就会被压掉一行。
int imeBarPanelH() { return imeBarRowH(0) + imeBarRowH(1); }

// ── IME drawing helper ────────────────────────────────────────────────────
// 编码行 + 候选行的总高推出来的顶边。随界面字号缩放，也随「候选字大小」缩放。
int imeStatusPanelTopY() {
    // 界面框架的几何：本函数会在编辑器正文作用域里被调到（算正文底边），钉回界面字号。
    FontScope ui(FontRenderer::uiPxHeight());
    return STATUS_BAR_Y - imeBarPanelH();
}

int imeFullscreenPanelTopY() { return SCREEN_H - imeBarPanelH(); }

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

// 量宽可换：编码行与别的界面短串按**界面字号**量（默认 g_font），候选行按**候选字号**
// 量（imeCandStrW）。量法与画法必须是同一份，否则截出来的 "..." 会与实际画出来的宽度打架。
static std::string fitTextWidth(const std::string &text, int maxW,
                                int (*measure)(const char *) = nullptr) {
    auto tw = [&](const std::string &s) {
        return measure ? measure(s.c_str()) : g_font.textWidth(s.c_str());
    };
    if (maxW <= 0) return "";
    if (tw(text) <= maxW) return text;
    const std::string ell = "...";
    if (tw(ell) > maxW) return "";
    std::string out = text;
    while (!out.empty() && tw(out + ell) > maxW)
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

static std::string fitTextWidthMiddle(const std::string &text, int maxW,
                                      int (*measure)(const char *) = nullptr) {
    auto tw = [&](const std::string &s) {
        return measure ? measure(s.c_str()) : g_font.textWidth(s.c_str());
    };
    if (maxW <= 0) return "";
    if (tw(text) <= maxW) return text;
    const std::string ell = "...";
    if (tw(ell) > maxW) return "";
    std::string head = text;
    std::string tail;
    bool trimHead = false;
    while (!head.empty() && tw(head + ell + tail) > maxW) {
        if (trimHead) {
            tail = shiftUtf8Char(head) + tail;
        } else {
            popUtf8Char(head);
        }
        trimHead = !trimHead;
    }
    return head.empty() ? fitTextWidth(text, maxW, measure) : head + ell + tail;
}

// 输入法条：编码行 + 候选行，两行贴着 bottomY 往上排（bottomY = 面板底边）。三个落点
// 由下面三个包装给：状态栏分割线 / 屏底 / 状态栏上方 3px。
//
// 两行的字号是**分开**的：编码行是界面字号（g_content_font，与旧行为一致），候选行跟
// 「候选字大小」设置——与虚拟键盘的候选行共用同一份实现（imeCandFontPx/imeCandStrW/
// imeCandDrawText）。以前两行都按界面字号画，设置改了实体键盘这边纹丝不动。
//
// 旧接口是 drawIMEUI(baseY, anchorBottom)：面板顶由调用方算、行位再按 anchorBottom 分叉
// 算第二遍——同一套几何两处维护。现在面板高由两行高推出来，调用方只报一个底边。
void drawIMEUI(int bottomY) {
    // 输入法条是**界面框架**：编码行的行高按界面字号算（候选行另有「候选字大小」）。
    // 本函数会在编辑器正文作用域里被调到（实体键盘打字），所以必须自己钉回界面字号，
    // 否则编码行会随正文字号一起长高。
    FontScope ui(FontRenderer::uiPxHeight());
    UI_FONT_GUARD();
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

    const int row0H = imeBarRowH(0);
    const int row1H = imeBarRowH(1);
    const int top = bottomY - row0H - row1H;
    const int candRowY = top + row0H;      // 两行的交界 = 那条分隔线
    // 行内居中。编码行按界面字号的 ascent/descent；候选行按候选字号（块高 = 候选字号）。
    const int codeBase = top + (row0H + g_font.ascent() - g_font.descent()) / 2;
    const int candBase = candRowY + (row1H - imeCandFontPx()) / 2 + imeCandAscent();
    const int hlH = imeCandFontPx();
    const int hlY = candRowY + (row1H - hlH) / 2;

    // 白底清出整块面板（编码行 + 候选行），清到 bottomY 为止，不抹状态栏。
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, 0, top, SCREEN_W, bottomY - top);
    // 记下这块矩形：上屏之后要用全像素刷把这两行的残影清一遍（推迟到打字停顿，
    // 见 ui_render.cpp 的 IME_CLEAN_PAUSE_US），而上屏那一刻组合已经结束、面板这一帧
    // 根本没画（g_ime.composing() 已是 false，从状态反推不出来），只能由绘制方在这里
    // 报上去。见 ui_render.h 的 ui_render_note_ime_panel。
    ui_render_note_ime_panel(0, top, SCREEN_W, bottomY - top);

    // ── 编码行：编码串 + 右端页码（都是界面字号，白底黑字）──
    //
    // **整行钉在 g_ime_font 上** —— 这是全工程**唯一**保留"ASCII = 半格"的地方。
    // 编码行是给人逐个字母读的**码**（"nihao"、"zhong"），不是正文散文：等宽排开才好
    // 认、才好对着键盘找下一个键；比例排会让 'i' 缩成一条、'm' 撑开，读码的人反而
    // 得重新找位置。g_ime_font = (role=CONTENT, latinBuiltin=true)：拉丁恒走内置等宽，
    // CJK（码串里的"拼音/英"这类模式标签）仍走内容面 = 用户选的字体。
    // 它与 g_font 共用同一份静态格子（px_/ascent_ 都是 static），所以行高、基线不变。
    // **量（imeCodeStrW）与画（g_ime_font）必须同一个实例**，否则截出来的 "..." 会
    // 按比例拉丁算、按等宽画。
    int pageW = g_ime_font.textWidth(pageInfo) + 8;
    int pageX = SCREEN_W - pageW - 4;
    code = fitTextWidth(code, pageX - 12, imeCodeStrW);
    int codeW = g_ime_font.textWidth(code.c_str()) + 8;
    u8g2_DrawBox(g_u8g2, 4, codeBase - g_font.ascent(), codeW, FONT_H);
    u8g2_SetDrawColor(g_u8g2, 0);
    g_ime_font.drawText(4, codeBase, code.c_str(), false);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, pageX, codeBase - g_font.ascent(), pageW, FONT_H);
    u8g2_SetDrawColor(g_u8g2, 0);
    g_ime_font.drawText(pageX + 4, codeBase, pageInfo, false);
    u8g2_SetDrawColor(g_u8g2, 1);

    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, candRowY, SCREEN_W);
    u8g2_SetDrawColor(g_u8g2, 1);

    // ── 候选行：按候选字号量宽/直绘（imeCandStrW 量的正是 imeCandDrawText 画的）──
    int hl = g_ime.highlightIdx();
    int x = 4;
    for (int i = 0; i < (int)cands.size(); i++) {
        char idx[16];
        snprintf(idx, sizeof(idx), "%d.", (i % pageSize) + 1);
        std::string prefix = std::string(" ") + idx;
        std::string part = prefix + cands[i];
        int partW = imeCandStrW(part.c_str());
        int availW = SCREEN_W - x - 8;
        bool truncated = false;
        if (partW > availW) {
            std::string word = fitTextWidthMiddle(cands[i], availW - imeCandStrW(prefix.c_str()),
                                                  imeCandStrW);
            part = prefix + word;
            partW = imeCandStrW(part.c_str());
            truncated = true;
        }
        if (partW <= 0 || x + partW + 8 > SCREEN_W) break;
        // 高亮候选反白（黑底白字）。面板白底已整块清过，不必再逐段清。
        if (i == hl) {
            u8g2_SetDrawColor(g_u8g2, 0);
            u8g2_DrawBox(g_u8g2, x, hlY, partW, hlH);
        }
        imeCandDrawText(x, candBase, part.c_str(), i == hl);
        x += partW;
        if (truncated) break;
    }
    u8g2_SetDrawColor(g_u8g2, 1);
}

void drawIMEUIWithStatusBar() {
    FontScope ui(FontRenderer::uiPxHeight());   // 底边是界面字号下的量
    UI_FONT_GUARD();
    drawIMEUI(STATUS_BAR_Y);
}

void drawIMEUIFullscreen() {
    drawIMEUI(SCREEN_H);
}

// 见头文件：面板与状态栏**同时**要画时用它——整块面板上移到状态栏上沿之上 3px。
void drawIMEUIFullscreenAboveStatusBar() {
    FontScope ui(FontRenderer::uiPxHeight());   // 底边是界面字号下的量
    UI_FONT_GUARD();
    drawIMEUI(STATUS_BAR_Y - 3);
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
    // 轻提示是界面框架：由 ui_commit() 在**当前**绘制作用域里调用，而编辑器正文那一片
    // 是正文字号 —— 这里钉回界面字号，提示框不跟着正文一起放大。
    FontScope ui(FontRenderer::uiPxHeight());
    UI_FONT_GUARD();
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

// 恢复待机前的画面：standbyClockDraw 之前 ui_render_keep_frame() 留过一份副本，
// 这里把它按 from-white 推回去（面板唤醒时刚被物理清成白底）。
void ui_restore_snapshot() { ui_render_restore_kept(); }

// 丢弃参考帧，使下一次 ui_commit 无条件整屏发送。
// 用于 light sleep 唤醒后面板被复位、需要强制重绘的场景。
void ui_invalidate_snapshot() { ui_render_invalidate(); }

// 注：IME 候选/编码条局刷的合并窗口由渲染任务自己在队列超时里做（见 ui_render.cpp
// 的 flush_deferred），core0 不必每轮冲刷 —— 原先那个空壳 ui_flush_ime_deferred()
// 和它在主循环里的调用点已删。

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
    // 状态栏是**界面框架**：底边、条高、字号全按界面字号。编辑器正文有自己的字号
    // （「显示与版式 → 正文字号」），那一个**只管正文**，不带着状态栏一起长——
    // 所以这里必须钉回界面字号，免得在正文作用域里被调到时整条状态栏变高。
    FontScope ui(FontRenderer::uiPxHeight());
    UI_FONT_GUARD();
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
    // 对话框是界面框架（编辑器正文作用域里也会调到，如"AI生成提示中..."），钉回界面字号。
    FontScope ui(FontRenderer::uiPxHeight());
    UI_FONT_GUARD();
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
    // 确认框是界面框架（编辑器正文作用域里也会调到：保存/恢复草稿），钉回界面字号。
    FontScope ui(FontRenderer::uiPxHeight());
    UI_FONT_GUARD();
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
