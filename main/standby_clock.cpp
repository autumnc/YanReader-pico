#include "standby_clock.h"

#include "chinese_almanac.h"
#include "fb_fast.h"        // 封面灰阶落屏：4bpp 直写（u8g2 shim 只有 1 位）
#include "font_renderer.h"
#include "pcf85063.h"
#include "ttf_font.h"       // 「关闭」表盘的提示页：按像素高直绘大字
#include "screen_reader.h"  // readerLastBookCover / readerCoverScale（「书籍封面」表盘）
#include "ui_helpers.h"
#include "ui_render.h"      // ui_render_keep_frame（留一份休眠前画面）

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "u8g2_shim.h"

// ── 枚举 ↔ 设置键 ─────────────────────────────────────────────────────────
StandbyFace standbyFaceFromKey(const char *key) {
    if (!key) return StandbyFace::Off;
    if (strcmp(key, "clock") == 0) return StandbyFace::Clock;
    if (strcmp(key, "almanac") == 0) return StandbyFace::Almanac;
    if (strcmp(key, "cover") == 0) return StandbyFace::Cover;
    return StandbyFace::Off;
}

const char *standbyFaceKey(StandbyFace f) {
    switch (f) {
        case StandbyFace::Clock: return "clock";
        case StandbyFace::Almanac: return "almanac";
        case StandbyFace::Cover: return "cover";
        default: return "off";
    }
}

const char *standbyFaceLabel(StandbyFace f) {
    switch (f) {
        case StandbyFace::Clock: return "简约时钟";
        case StandbyFace::Almanac: return "老黄历";
        case StandbyFace::Cover: return "书籍封面";
        default: return "关闭";
    }
}

// ── 时间来源 ──────────────────────────────────────────────────────────────
// 返回本地时间（跟随设置的 TZ）。未对时（RTC 无效）时返回 false。
static bool localNow(struct tm &out) {
    time_t t = g_rtc.getTime();
    if (t <= 0) t = time(nullptr);
    if (t < 1600000000) return false;  // 早于 2020 年视为未对时
    localtime_r(&t, &out);
    return true;
}

// 北京时间的 time_t（老黄历按东八区计算，与用户时区无关）。
static time_t beijingNow() {
    time_t t = g_rtc.getTime();
    if (t <= 0) t = time(nullptr);
    return t + 8 * 3600;
}

static const char *kWdayCn[7] = {"日", "一", "二", "三", "四", "五", "六"};

// ── 七段数字 ──────────────────────────────────────────────────────────────
// 位序：bit0=a(上) bit1=b(右上) bit2=c(右下) bit3=d(下) bit4=e(左下)
//       bit5=f(左上) bit6=g(中)
static const uint8_t kSegDigit[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};

static void segBox(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    u8g2_DrawBox(g_u8g2, x, y, w, h);
}

// 在 (x,y,w,h) 画一个七段数字。调用前须 SetDrawColor(0)。
static void drawSevenSeg(int x, int y, int w, int h, int digit) {
    if (digit < 0 || digit > 9) return;
    const uint8_t m = kSegDigit[digit];
    int t = h / 8;
    if (t < 5) t = 5;
    if (t * 2 >= w) t = (w - 1) / 2;
    if (t < 1) t = 1;
    int vh = (h - 3 * t) / 2;  // 竖段长（上下各一 + 三条横段）
    if (vh < 1) vh = 1;

    if (m & 0x01) segBox(x + t, y, w - 2 * t, t);            // a
    if (m & 0x40) segBox(x + t, y + t + vh, w - 2 * t, t);   // g
    if (m & 0x08) segBox(x + t, y + h - t, w - 2 * t, t);    // d
    if (m & 0x20) segBox(x, y + t, t, vh);                   // f
    if (m & 0x02) segBox(x + w - t, y + t, t, vh);           // b
    if (m & 0x10) segBox(x, y + 2 * t + vh, t, vh);          // e
    if (m & 0x04) segBox(x + w - t, y + 2 * t + vh, t, vh);  // c
}

// 未对时占位：数字位里的横杠（"--"）。
static void drawDash(int x, int y, int w, int h) {
    int t = h / 8;
    if (t < 5) t = 5;
    int bw = w * 3 / 5;
    if (bw < 8) bw = 8;
    segBox(x + (w - bw) / 2, y + h / 2 - t / 2, bw, t);
}

// 冒号（两个方点，宽度 colW，随数字高度缩放）。
static void drawColon(int x, int y, int colW, int h) {
    int s = colW > h / 6 ? h / 6 : colW;
    if (s < 4) s = 4;
    int cx = x + (colW - s) / 2;
    segBox(cx, y + h * 3 / 10, s, s);
    segBox(cx, y + h * 6 / 10, s, s);
}

static void drawCentered(int baseline, const char *s) {
    int w = g_font.textWidth(s);
    int x = (SCREEN_W - w) / 2;
    if (x < 0) x = 0;
    g_font.drawText(x, baseline, s, false);
}

// 在 [x, x+w) 里居中画一行（不能借 drawCentered：它是按整屏居中的）。
static void drawCenteredIn(int x, int w, int baseline, const char *s) {
    int tw = g_font.textWidth(s);
    int tx = x + (w - tw) / 2;
    if (tx < x) tx = x;
    g_font.drawText(tx, baseline, s, false);
}

// HH:MM 五个格子（4 数字 + 冒号）。调用前须 SetDrawColor(0)。
// 简约时钟表盘与书籍封面表盘共用同一套七段数学，尺寸由调用方按可用宽度定。
static void drawTimeSevenSeg(int x, int top, int dw, int dh, int colonW, int gap, bool valid,
                             const struct tm &tm) {
    int cx = x;
    for (int i = 0; i < 5; i++) {
        if (i == 2) {
            drawColon(cx, top, colonW, dh);
            cx += colonW + gap;
            continue;
        }
        if (!valid) {
            drawDash(cx, top, dw, dh);
        } else {
            // i=0,1 是小时，i=3,4 是分钟
            int v = (i < 2) ? tm.tm_hour : tm.tm_min;
            int dig = (i == 0 || i == 3) ? v / 10 : v % 10;
            drawSevenSeg(cx, top, dw, dh, dig);
        }
        cx += dw + gap;
    }
}

// 把 items 依次接在 prefix 后，只保留放得下（≤ maxW）的部分。
static std::string fitItems(const std::string &prefix, const char *const *items, int cnt,
                            int maxW) {
    std::string s = prefix;
    for (int i = 0; i < cnt; i++) {
        std::string cand = s + items[i];
        if (g_font.textWidth(cand.c_str()) > maxW) break;
        s = cand;
    }
    return s;
}

// ── 简约时钟表盘 ──────────────────────────────────────────────────────────
static void drawClockFace(const struct tm &tm, bool valid) {
    int W = SCREEN_W, H = SCREEN_H;

    // 由可用宽度反推数字宽：4 个数字 + 冒号(0.35 数字宽) + 5 个间隙
    const int margin = W / 16 > 40 ? W / 16 : 40;
    const int gap = 12;
    int dw = (int)((float)(W - 2 * margin - 5 * gap) / 4.35f);
    int dh = (int)(dw / 0.55f);
    int maxH = H * 45 / 100;
    if (dh > maxH) {
        dh = maxH;
        dw = (int)(dh * 0.55f);
    }
    if (dw < 20) dw = 20;

    const int colonW = dw * 35 / 100;
    int totalW = 4 * dw + colonW + 5 * gap;

    // 竖向排版：时间块 + 两行文字（日期 / 农历）。**不再留底部状态栏**——休眠只有
    // 电源键能唤醒，屏上不留提示条，也不挂电量（见本文件末尾 Off 分支的说明）。
    int blockH = dh + FONT_H * 12 / 5;
    int timeTop = (H - blockH) / 2;
    int minTop = 8;
    int maxTop = H - FONT_H * 12 / 5 - dh;
    if (timeTop < minTop) timeTop = minTop;
    if (timeTop > maxTop) timeTop = maxTop;
    if (timeTop < 8) timeTop = 8;

    int x = (W - totalW) / 2;
    if (x < 0) x = 0;

    u8g2_SetDrawColor(g_u8g2, 0);
    drawTimeSevenSeg(x, timeTop, dw, dh, colonW, gap, valid, tm);

    int y = timeTop + dh + FONT_H * 6 / 5;
    if (valid) {
        char dateBuf[64];
        snprintf(dateBuf, sizeof(dateBuf), "%d 年 %d 月 %d 日  星期%s",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, kWdayCn[tm.tm_wday % 7]);
        drawCentered(y, dateBuf);

        // 农历行（按北京时计算）
        struct tm bj;
        time_t bt = beijingNow();
        gmtime_r(&bt, &bj);
        AlmanacDay d{};
        if (computeAlmanac(bj, d)) {
            char lunar[80];
            snprintf(lunar, sizeof(lunar), "农历 %s%s%s · %s%s年",
                     d.lunarLeap ? "闰" : "",
                     chinese_almanac::kLunarMonthNames[d.lunarMonth - 1],
                     chinese_almanac::kLunarDayNames[d.lunarDay - 1],
                     chinese_almanac::kStemNames[d.yearStemIdx],
                     chinese_almanac::kBranchNames[d.yearBranchIdx]);
            drawCentered(y + FONT_H * 6 / 5, lunar);
        }
    } else {
        drawCentered(y, "尚未对时");
        drawCentered(y + FONT_H * 6 / 5, "请到 设置 → 网络同步时间");
    }

    // **不画底部状态栏**（电量 + 唤醒提示）：休眠只有电源键能唤醒，屏上不留提示；
    // 电量那种状态栏元素也不该出现在待机画面上。
}

// ── 老黄历表盘 ────────────────────────────────────────────────────────────
static void drawAlmanacFace(bool valid) {
    int W = SCREEN_W, H = SCREEN_H;
    u8g2_SetDrawColor(g_u8g2, 0);

    if (!valid) {
        drawCentered(H / 2, "尚未对时");
        drawCentered(H / 2 + FONT_H, "请到 设置 → 网络同步时间");
        return;
    }

    struct tm bj;
    time_t bt = beijingNow();
    gmtime_r(&bt, &bj);
    AlmanacDay d{};
    if (!computeAlmanac(bj, d)) {
        drawCentered(H / 2, "日期超出黄历范围");
        return;
    }
    const int margin = 24;
    const int availW = W - 2 * margin;
    const int lineH = FONT_H * 6 / 5;
    const int nInfo = 4;  // 干支 / 节气 / 宜 / 忌(附冲煞)

    // 自上而下排，整卡垂直居中；底部只留一行边距（底部工具栏已去掉，见函数末尾）。
    // 大号日名取屏高 28%，同时受可用宽度约束（两位数字要放得下）。
    int heroH = H * 28 / 100;
    int dw = heroH * 55 / 100;
    int maxDw = (availW - dw / 8) / 2;
    if (dw > maxDw && maxDw > 8) {
        dw = maxDw;
        heroH = dw * 100 / 55;
    }
    if (heroH < FONT_H) heroH = FONT_H;

    int cardH = FONT_H + FONT_H / 2 + heroH + FONT_H * 6 / 5 + FONT_H / 2 + 1 +
                FONT_H * 2 / 5 + (nInfo - 1) * lineH + FONT_H;
    int top = (H - cardH) / 2;
    int maxBottom = H - FONT_H;  // 底部留一行边距（不再有工具栏）
    if (top + cardH > maxBottom) top = maxBottom - cardH;
    if (top < 4) top = 4;

    int dateY = top + FONT_H * 3 / 4;
    int heroTop = top + FONT_H + FONT_H / 2;
    int capY = heroTop + heroH + FONT_H * 6 / 5;
    int sepY = capY + FONT_H / 2;
    int firstInfoY = sepY + FONT_H * 9 / 10;

    // 顶部：公历日期 + 星期
    char topBuf[80];
    snprintf(topBuf, sizeof(topBuf), "%d 年 %d 月 %d 日  星期%s",
             d.gregYear, d.gregMonth, d.gregDay, kWdayCn[d.weekdayIdx % 7]);
    drawCentered(dateY, topBuf);

    // 主视觉：公历日（大号七段数字）
    {
        int dd = d.gregDay;
        int nd = dd >= 10 ? 2 : 1;
        int totalW = nd * dw + (nd - 1) * (dw / 8);
        int x = (W - totalW) / 2;
        if (x < 0) x = 0;
        if (nd == 2) {
            drawSevenSeg(x, heroTop, dw, heroH, dd / 10);
            drawSevenSeg(x + dw + dw / 8, heroTop, dw, heroH, dd % 10);
        } else {
            drawSevenSeg(x, heroTop, dw, heroH, dd);
        }
    }

    // 农历说明（大号日名下方）
    {
        char lunar[64];
        snprintf(lunar, sizeof(lunar), "农历 %s%s月%s",
                 d.lunarLeap ? "闰" : "",
                 chinese_almanac::kLunarMonthNames[d.lunarMonth - 1],
                 chinese_almanac::kLunarDayNames[d.lunarDay - 1]);
        drawCentered(capY, lunar);
    }

    // 分隔线
    u8g2_DrawHLine(g_u8g2, margin, sepY, W - 2 * margin);

    // 信息行：干支(+生肖) / 节气 / 宜 / 忌(+冲煞)
    {
        char t[48];
        std::string gz;
        snprintf(t, sizeof(t), "%s%s年", chinese_almanac::kStemNames[d.yearStemIdx],
                 chinese_almanac::kBranchNames[d.yearBranchIdx]);
        gz = t;
        snprintf(t, sizeof(t), "  %s%s月", chinese_almanac::kStemNames[d.monthStemIdx],
                 chinese_almanac::kBranchNames[d.monthBranchIdx]);
        gz += t;
        snprintf(t, sizeof(t), "  %s%s日", chinese_almanac::kStemNames[d.dayStemIdx],
                 chinese_almanac::kBranchNames[d.dayBranchIdx]);
        gz += t;
        // 生肖：放得下才加（竖屏会窄）
        snprintf(t, sizeof(t), "  【%s】", chinese_almanac::kZodiacNames[d.yearBranchIdx]);
        {
            std::string cand = gz + t;
            if (g_font.textWidth(cand.c_str()) <= availW) gz = cand;
        }
        // 仍放不下就去掉月/日，只留年干支
        if (g_font.textWidth(gz.c_str()) > availW) {
            snprintf(t, sizeof(t), "%s%s年", chinese_almanac::kStemNames[d.yearStemIdx],
                     chinese_almanac::kBranchNames[d.yearBranchIdx]);
            gz = t;
        }
        drawCentered(firstInfoY, gz.c_str());
    }

    {
        char buf[80];
        if (d.daysToNextTerm <= 1) {
            snprintf(buf, sizeof(buf), "节气  %s · %s",
                     chinese_almanac::kSolarTermNames[d.termCurrentIdx],
                     d.daysToNextTerm == 1 ? "明日交节" : "今日交节");
        } else {
            snprintf(buf, sizeof(buf), "节气  %s · 距%s %d 天",
                     chinese_almanac::kSolarTermNames[d.termCurrentIdx],
                     chinese_almanac::kSolarTermNames[d.termNextIdx], d.daysToNextTerm);
        }
        drawCentered(firstInfoY + lineH, buf);
    }

    {
        std::string s = fitItems("宜  ", chinese_almanac::kYiPool[d.yiIdx], 4, availW);
        drawCentered(firstInfoY + 2 * lineH, s.c_str());
    }
    {
        // 忌 + 冲煞（放得下才追加）
        std::string s = fitItems("忌  ", chinese_almanac::kJiPool[d.jiIdx], 4, availW);
        char t[32];
        snprintf(t, sizeof(t), "  ·冲%s", chinese_almanac::kBranchNames[d.clashBranchIdx]);
        std::string cand = s + t;
        if (g_font.textWidth(cand.c_str()) <= availW) s = cand;
        drawCentered(firstInfoY + 3 * lineH, s.c_str());
    }

    // **不画底部工具栏**（时钟 + 唤醒提示）：与简约时钟表盘一致，待机画面上只剩
    // 表盘本身（日期/干支/节气/宜忌），不留提示条也不留状态栏。
}

// ── 书籍封面表盘 ──────────────────────────────────────────────────────────
// 整屏只放封面：**不画时刻 / 日期 / 书名 / 进度**（用户要求：待机时把空间全留给封面，
// 封面以最清晰的形式显示）。原来那套"封面 + 一列信息"的版式（横屏封面在左、信息在右，
// 竖屏封面上 3/5、信息在下）连同它的书名折行工具 wrapToWidth / utf8Step 一起撤掉了；
// 要恢复的话照 drawClockFace 的排版口径重写一列即可，书名那一列是"分隔线 + 垂直居中
// 折行 + 进度钉底"三段，源文在 // 已废弃 那段记录里。
//
// 清晰度靠两件事，都不在绘制这一层：
//   1) 框几乎占满整屏（原来竖屏只有 60% 的高度给封面）：同一张图下采样比更小，
//      丢的细节更少；
//   2) readerCoverScale 里改成**只缩不放**（scale ≤ 1）：源图比框小就按原尺寸画。
//      放大要么复制像素、要么插值，画出来的细节都是编的，只会更糊。
// 封面像素用 fb_fast 直写（0..15 灰阶），不走 g_rd.drawGrayscale16Pixel：g_rd 绑的是
// front_fb，而待机表盘画在 ui_render 的工作缓冲上（见 ui_render.cpp 那条不变式）。

static void drawCoverFace() {
    // 页边距只留短边的 2.5%（原来是 4.5%，那个值是给信息列让出来的）：面板四周本来
    // 就有 3~4px 盖边，留一点点就够，剩下的全给封面。框就是整屏减这一圈。
    // 框的定义挪到 screen_reader 的 readerStandbyCoverBox：待机封面的缓存文件是按
    // 这个尺寸解的，两边算出来必须一样，那条 1:1 直拷快路径才命中得了。
    int boxX = 0, boxY = 0, boxW = 0, boxH = 0;
    readerStandbyCoverBox(boxX, boxY, boxW, boxH);

    // 书名与进度不再显示，但 readerLastBookCover 是"最后一本书 + 它的封面缓存路径"
    // 的唯一出口，两个出参照样接着 —— title 还能区分"没读过书"和"这本书没封面"两种情况。
    std::string bmp, title;
    int percent = 0;
    const bool hasBook = readerLastBookCover(bmp, title, percent);

    bool drew = false;
    if (hasBook) {
        const size_t cap = static_cast<size_t>(boxW) * static_cast<size_t>(boxH);
        // 一张整屏封面约 300KB：**必须 PSRAM**（内部 RAM 挤不出这么大一块，见
        // internal-ram-squeeze）。拿不到就退占位框 —— 表盘本身还是要出来的。
        uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buf) {
            int dw = 0, dh = 0, ox = 0, oy = 0;
            if (readerCoverScale(bmp, boxW, boxH, buf, cap, dw, dh, ox, oy)) {
                fb_fast_sync();  // 每帧入口同步一次旋转/尺寸（见 fb_fast.h）
                uint8_t *fb = u8g2_GetBufferPtr(g_u8g2);
                const int bx = boxX + ox, by = boxY + oy;
                for (int y = 0; y < dh; y++) {
                    const uint8_t *row = buf + static_cast<size_t>(y) * static_cast<size_t>(dw);
                    for (int x = 0; x < dw; x++) fb_fast_set_gray(fb, bx + x, by + y, row[x]);
                }
                drew = true;
            }
            heap_caps_free(buf);
        }
    }
    u8g2_SetDrawColor(g_u8g2, 0);
    if (!drew) {
        // 没有封面可画（没读过书 / 这本书还没生成封面缓存 / PSRAM 要不到）：
        // 细框 + 一行说明，别让整屏白得看不出"这是待机画面"。
        u8g2_DrawFrame(g_u8g2, boxX, boxY, boxW, boxH);
        drawCenteredIn(boxX, boxW, boxY + boxH / 2 + g_font.ascent() / 2,
                       hasBook ? "暂无封面" : "还没读过书");
    }
}

// ── 「关闭」表盘的提示页 ──────────────────────────────────────────────────
// 「关闭」以前是"什么都不画"，屏上原样留着待机前的画面。问题是待机＝light sleep，
// 屏幕纹丝不动、按键又全没反应，看起来和死机一模一样 —— 分不出"关了"还是"卡住了"。
// 所以这里铺一页提示：整屏一行大字，一眼可辨。原厂固件也是这个做法（试睡/关机之前
// 先整屏定稿一页大字，注释里写得很明白："避免客人把静止的演示页当成死机"）。
// 措辞用「待机」不用「关机」：light sleep 会被电源键唤醒、唤醒后回到原来那一页，
// 说"关机"是错的。
static void drawCenteredPx(int baseline, int px, const char *s, uint8_t fg, uint8_t bg) {
    uint8_t *fb = u8g2_GetBufferPtr(g_u8g2);
    int total = 0;
    for (const char *p = s; *p != '\0';) {
        const unsigned char c = (unsigned char)*p;
        const int m = (c < 0x80) ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
        if (m == 1) {
            char ch[2] = {(char)c, '\0'};
            total += ttf_text_width_px(px, ch);
        } else {
            total += px;   // CJK：一格一个字号宽（同 editor_vk 候选字的口径）
        }
        p += m;
    }
    int x = (SCREEN_W - total) / 2;
    if (x < 0) x = 0;
    for (const char *p = s; *p != '\0';) {
        const unsigned char c = (unsigned char)*p;
        const int m = (c < 0x80) ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
        char ch[5];
        memcpy(ch, p, (size_t)m);
        ch[m] = '\0';
        ttf_draw_text_px(fb, x, baseline, px, ch, EPD_DRAW_ALIGN_LEFT, fg, bg);
        x += (m == 1) ? ttf_text_width_px(px, ch) : px;
        p += m;
    }
}

// 大字标题 + 小字唤醒提示，竖向居中。字号按屏高定，横竖屏都占得住。
static void drawOffNotice() {
    ui_clear();   // 白底，绘制色复位为 0（黑）
    const int pxTitle = 120;
    const int pxSub = 48;
    const int gap = 72;
    const int block = pxTitle + gap + pxSub;
    const int top = (SCREEN_H - block) / 2;
    drawCenteredPx(top + ttf_ascender_px(pxTitle), pxTitle, "已待机", 0, 15);
    drawCenteredPx(top + pxTitle + gap + ttf_ascender_px(pxSub), pxSub, "按电源键唤醒", 0, 15);
}

// ── 对外接口 ──────────────────────────────────────────────────────────────
void standbyClockDraw(StandbyFace face) {
    if (!g_u8g2) return;

    // 先把"屏上现在的画面"留一份（**放在任何绘制之前**）：待机画面只是临时盖上去，
    // 唤醒 / 预览结束后由 ui_restore_snapshot() 原样推回。
    ui_render_keep_frame();

    if (face == StandbyFace::Off) {
        // 关闭：铺一页提示（见上）。keep_frame（上面）已经把休眠前的画面留了副本，
        // 唤醒时 ui_restore_snapshot() 照旧把它推回来；所以这里只**发送**、不更新快照
        // ——与其它三个表盘同一套写法。
        const int prev_role = ttf_get_role();
        ttf_set_role(TTF_ROLE_CONTENT);   // 本页走内容面（正文那一路的字体）
        drawOffNotice();
        ttf_set_role(prev_role);
        ui_send_buffer();
        return;
    }

    ui_clear();  // 白底，绘制色恢复为 0（黑）
    struct tm lt{};
    bool valid = localNow(lt);
    if (face == StandbyFace::Almanac)
        drawAlmanacFace(valid);
    else if (face == StandbyFace::Cover)
        drawCoverFace();   // 封面表盘不显示时刻/日期，用不上 lt/valid
    else
        drawClockFace(lt, valid);

    ui_send_buffer();  // 整屏 GC16；不更新快照，唤醒后据此恢复
}

// ── 关机页 ────────────────────────────────────────────────────────────────
// 长按电源键满 8s，PMU 报 KEY_FORCE_OFF（警告），再满 10s 直接拉 EN 硬断电。这一页就是
// 在那 2s 窗口里铺的：整屏一行「已关机」，断电后墨水屏双稳态把画面留屏上 —— 这就是
// "关机画面"。措辞和原厂一致（原厂 APP_SLEEP_OFF 的提示也是"断电，长按约 1 秒开机。"）。
//
// 与「关闭」表盘那页的区别：那页是 light sleep（按电源键就能醒、醒了回到原来那页），
// 说「待机」；这页是真断电，醒来是冷启动，所以说「关机」。
void standbyShutdownDraw() {
    if (!g_u8g2) return;
    const int prev_role = ttf_get_role();
    ttf_set_role(TTF_ROLE_CONTENT);
    ui_clear();
    const int pxTitle = 120, pxSub = 48, gap = 72;
    const int block = pxTitle + gap + pxSub;
    const int top = (SCREEN_H - block) / 2;
    drawCenteredPx(top + ttf_ascender_px(pxTitle), pxTitle, "已关机", 0, 15);
    drawCenteredPx(top + pxTitle + gap + ttf_ascender_px(pxSub), pxSub, "长按电源键约 1 秒开机", 0, 15);
    ttf_set_role(prev_role);
    ui_send_buffer();   // 整屏 GC16，推完才返回（ui_render_full_refresh 是同步的）
}

void standbyClockPreview(StandbyFace face, int ms) {
    standbyClockDraw(face);
    vTaskDelay(pdMS_TO_TICKS(ms));
    ui_restore_snapshot();
}
