/*
 * 图标字体层实现。stb_truetype 的实现由 font/ttf_font.c 提供(STB_TRUETYPE_IMPLEMENTATION)，
 * 这里只 include 头文件声明，链接同一份实现。
 */
#include "icon_font.h"

#include <stdlib.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "epdiy.h"
#include "fb_fast.h"   // 字形逐像素直写
#include "stb_truetype.h"

// 内嵌子集 (EMBED_FILES "assets/icon_font.ttf")
extern const uint8_t icon_font_ttf_start[] asm("_binary_icon_font_ttf_start");
extern const uint8_t icon_font_ttf_end[] asm("_binary_icon_font_ttf_end");

// 光栅缓冲边长上限。主菜单图标 MAIN_ICON_PX 要放到 100，字形位图会接近 100×100，
// 72(旧值) 会让 icon_font_draw_sized 在 "w > ICON_MAX_PX" 处静默 return —— 图标整个消失。
// 留一档余量给高瘦/超宽字形。
#define ICON_MAX_PX 112
static stbtt_fontinfo s_info;
static bool s_ready = false;
// 光栅缓冲放 PSRAM：112×112 = 12.5KB，按 BSS 静态占内部 RAM 太贵（内部已经很挤，
// 见 internal-ram-squeeze）。PSRAM 拿不到再退回内部 malloc；两者都失败则不放行 s_ready，
// 调用方因 !s_ready 提前返回，不会踩空指针。
static uint8_t* s_bitmap = NULL;

bool icon_font_init(void) {
    if (s_ready) return true;
    const uint8_t* data = icon_font_ttf_start;
    int len = (int)(icon_font_ttf_end - icon_font_ttf_start);
    if (len <= 0 || !stbtt_InitFont(&s_info, data, 0)) return false;
    if (!s_bitmap) {
        const size_t need = (size_t)ICON_MAX_PX * ICON_MAX_PX;
        s_bitmap = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
        if (!s_bitmap) s_bitmap = (uint8_t*)malloc(need);
        if (!s_bitmap) {
            ESP_LOGE("icon", "icon bitmap alloc failed (%u bytes)", (unsigned)need);
            return false;
        }
    }
    s_ready = true;
    return true;
}

// 子集里真有这个字形吗？（只给补充平面 PUA 用，见下面的说明。）
static bool icon_font_has_cp(uint32_t cp) {
    return s_ready && stbtt_FindGlyphIndex(&s_info, (int)cp) != 0;
}

bool icon_font_is_icon(uint32_t cp) {
    // ASCII (< 0x80) 由 FontRenderer 单独路由到等宽图标字体，不在此判断。
    // PUA (Nerd Font 图标)
    if (cp >= 0xE000 && cp <= 0xF8FF) return true;
    // 补充平面 PUA（MDI 图标全在这里，本子集是 U+F0046..U+F1AF1）。**旧判据整段漏掉**，
    // 而 markdown 的标题级别图标(U+F03A4..)/待办框(U+F0131/F0132)/折叠标志(U+F09DA)
    // 是当**文本**画的（走 FontRenderer::drawCellGlyph 的字形路）：判据为假就落到 TTF
    // 文本面，内置面与用户字体都没有这些码位 → 一个字也画不出来（"标题图标、任务图标
    // 缺了"的根因，与装没装外置字体无关）。
    // 判据以**字体里真有这个字形**为准，而不是拍一段范围：往子集里加图标不必回来改这里；
    // 子集里没有的码位也不会被误判成图标（继续走文本路，与从前一致）。
    if (cp >= 0xF0000 && cp <= 0x10FFFD) return icon_font_has_cp(cp);
    // 全角字符 (IME 全角模式 + 竖排标点)：FF01-FF5E + 表意空格 + 半角假名标点
    if (icon_font_is_fullwidth(cp)) return true;
    switch (cp) {
        case 0x1F786: case 0x2022: case 0x2026:
        case 0x2191: case 0x2193:
        case 0x25CF: case 0x25D0: case 0x25CB: case 0x2605:
        case 0x2610: case 0x2713: case 0x25B8: case 0x25BE: case 0x25A0:
        case 0x270E:
        case 0x3008: case 0x3009: case 0x300A: case 0x300B:
        case 0x300C: case 0x300D: case 0x300E: case 0x300F:
        case 0x2013: case 0x2018: case 0x2019: case 0x201C: case 0x201D:
        case 0x2500:
        case 0xFE31: case 0xFF02: case 0xFF07:
            return true;
        default:
            return false;
    }
}

bool icon_font_is_fullwidth(uint32_t cp) {
    return (cp >= 0xFF01 && cp <= 0xFF5E) || cp == 0x3000 ||
           cp == 0xFF61 || cp == 0xFF64;
}

// ---- 程序化补齐的几何图形 (子集缺失) ----
static void draw_prog(uint8_t* fb, int bx, int by, int bw, int bh,
                      uint32_t cp, uint8_t color) {
    int cx = bx + bw / 2;
    int cy = by + bh / 2;
    int r = (bw < bh ? bw : bh) / 2 - 1;
    if (r < 1) r = 1;
    switch (cp) {
        case 0x1F786:  // 空心圆
        case 0x25CB:
            epd_draw_circle(cx, cy, r, color, fb);
            break;
        case 0x25D0: {  // ◐ 圆内半边黑(左实右空)
            // 原来画的是"实心圆挖掉右半" —— 挖的时候把右半 (cx..cx+r) 连同轮廓一起涂白，
            // 结果只剩一个半圆饼，看不出是个圈；而且外径取的是整格 (2r ≈ 格宽)，
            // 比相邻的 ● 字形大了一倍多，全角/半角两个状态大小对不上。
            // 现在：外径照抄 ● 的字形尺寸(NF-Propo 的 ● 直径 ≈ 0.451em，见 draw_glyph
            // 的 px/1024 缩放)，先描一圈轮廓，再按行把左半圆填实——不涂白，右半自然
            // 留空，轮廓完整。
            int ro = (int)((long)bh * 451 / 1000) / 2;
            if (ro > r) ro = r;
            if (ro < 2) ro = 2;
            epd_draw_circle(cx, cy, ro, color, fb);
            const int ri = ro - 1;                 // 填到轮廓内侧，别糊掉圈线
            for (int dy = -ri; dy <= ri; dy++) {
                int hw = ri;                       // 本行左半宽：x²+y² ≤ r²
                while (hw > 0 && hw * hw + dy * dy > ri * ri) hw--;
                if (hw <= 0) continue;
                EpdRect run = { cx - hw, cy + dy, hw + 1, 1 };
                epd_fill_rect(run, color, fb);
            }
            break;
        }
        case 0x25B8: {  // 右三角
            epd_fill_triangle(bx + 2, by + 2, bx + 2, by + bh - 2,
                              bx + bw - 2, cy, color, fb);
            break;
        }
        case 0x25BE: {  // 下三角
            epd_fill_triangle(bx + 2, by + 2, bx + bw - 2, by + 2,
                              cx, by + bh - 2, color, fb);
            break;
        }
        case 0x1F441: {  // 👁 眼睛：「显示隐藏文件」开关（子集里没有 eye 字形）
            // 杏仁形轮廓 = 上下两条对称的椭圆弧，中间一个实心瞳孔。
            // 横长比例（宽 : 高 ≈ 8 : 5）—— 眼睛本来就不是圆的，画成正圆认不出来。
            const int w2 = (bw * 40) / 100;        // 半宽（占 box 宽的 80%）
            const int h2 = (bh * 25) / 100;        // 半高（上下弧各自的矢高）
            const int t = (bh + 27) / 28;          // 弧线粗细（56px 的 box → 2px）
            const long w2sq = (long)w2 * w2;
            if (w2 < 3 || h2 < 2 || t < 1) break;
            // 逐列画：dy = h2·√(1-(dx/w2)²)，两边都用整数比，不必开浮点。
            // 只在**相邻两列之间**填出落差（弧越陡，两列的 dy 差得越多）—— 不补这一步，
            // 靠近两端的地方会断成一串虚点。
            int prevTop = cy, prevBot = cy;
            for (int dx = -w2; dx <= w2; dx++) {
                long num = (long)h2 * h2 * (w2sq - (long)dx * dx);
                int dy = 0;
                while (dy < h2 && (long)(dy + 1) * (dy + 1) * w2sq <= num) dy++;
                const int top = cy - dy, bot = cy + dy;
                // 防呆：跨度只可能 ≤ 2·h2+t，超过就说明上面哪个式子写坏了，
                // 宁可这一列空着，也别糊成一根竖条。
                const int run = (top < prevTop ? top : prevTop);
                const int l = (top > prevTop ? top : prevTop) - run + t;
                if (l <= 2 * h2 + t) {
                    EpdRect r = { cx + dx, run, 1, l };
                    epd_fill_rect(r, color, fb);           // 上弧
                }
                const int run2 = (bot < prevBot ? bot : prevBot);
                const int l2 = (bot > prevBot ? bot : prevBot) - run2 + t;
                if (l2 <= 2 * h2 + t) {
                    EpdRect r = { cx + dx, run2 - t + 1, 1, l2 };
                    epd_fill_rect(r, color, fb);           // 下弧
                }
                prevTop = top;
                prevBot = bot;
            }
            // 瞳孔：实心圆。半径取矢高的 45% —— 再大就把上下弧顶穿了（那是"日"不是"眼"）。
            const int pr = (h2 * 45) / 100;
            if (pr < 2) break;
            for (int dy = -pr; dy <= pr; dy++) {
                int hw = 0;
                while (hw < pr && (long)(hw + 1) * (hw + 1) + (long)dy * dy <= (long)pr * pr) hw++;
                if (hw <= 0) continue;
                EpdRect r = { cx - hw, cy + dy, 2 * hw + 1, 1 };
                epd_fill_rect(r, color, fb);
            }
            break;
        }
        default:
            break;
    }
}

static void draw_glyph(uint8_t* fb, int bx, int by, int bw, int bh,
                       uint32_t cp, bool invert) {
    int px = bh;
    if (px > ICON_MAX_PX) px = ICON_MAX_PX;

    float scale = stbtt_ScaleForPixelHeight(&s_info, (float)px);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBox(&s_info, (int)cp, scale, scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;
    if (w > ICON_MAX_PX || h > ICON_MAX_PX) return;

    stbtt_MakeCodepointBitmap(&s_info, s_bitmap, w, h, w, scale, scale, (int)cp);
    fb_fast_sync();

    int draw_x = bx + (bw - w) / 2;
    int draw_y = by + (bh - h) / 2;
    for (int gy = 0; gy < h; gy++) {
        for (int gx = 0; gx < w; gx++) {
            uint8_t alpha = s_bitmap[gy * w + gx];
            if (alpha == 0) continue;
            uint8_t g;
            if (invert) g = (uint8_t)((alpha * 15 + 127) / 255);
            else        g = 15 - (uint8_t)((alpha * 15 + 127) / 255);
            fb_fast_pixel(fb, draw_x + gx, draw_y + gy, (uint8_t)(g << 4));
        }
    }
}

void icon_font_draw(uint8_t* fb, int boxX, int boxY, int boxW, int boxH,
                    uint32_t cp, bool invert) {
    if (!fb || boxW <= 0 || boxH <= 0) return;
    uint8_t color = invert ? 0xF0 : 0x00;

    // 程序化图形（子集缺失）
    switch (cp) {
        // 0x1F441(👁) 是文件浏览页 FAB 借用的"显示隐藏文件"字形。**故意不进
        // icon_font_is_icon()**：那样会把正文/输入里偶然出现的 👁 也从文本路改道到
        // 这里，而画得出来画不出来都只是小事、改道却是行为变化。调用方（FAB）直接
        // 调本函数，本来也不需要 is_icon 认它。
        case 0x1F786: case 0x25CB: case 0x25D0: case 0x25B8: case 0x25BE: case 0x1F441:
            draw_prog(fb, boxX, boxY, boxW, boxH, cp, color);
            return;
        case 0x270E:  // 铅笔 → NF-Propo 的 pencil (F03EB)
            cp = 0xF03EB;
            break;
        default:
            break;
    }

    if (!s_ready) return;
    draw_glyph(fb, boxX, boxY, boxW, boxH, cp, invert);
}

void icon_font_draw_baseline(uint8_t* fb, int x, int baselineY, int boxW, int boxH,
                             uint32_t cp, bool invert) {
    if (!fb || boxW <= 0 || boxH <= 0) return;
    if (cp == 0x270E) cp = 0xF03EB;  // 铅笔 → NF-Propo 的 pencil (F03EB)
    if (!s_ready) return;

    int px = boxH;
    if (px > ICON_MAX_PX) px = ICON_MAX_PX;

    float scale = stbtt_ScaleForPixelHeight(&s_info, (float)px);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBox(&s_info, (int)cp, scale, scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || w > ICON_MAX_PX || h > ICON_MAX_PX) return;

    stbtt_MakeCodepointBitmap(&s_info, s_bitmap, w, h, w, scale, scale, (int)cp);
    fb_fast_sync();

    // 水平在 boxW 内居中；垂直按基线放置：y0 为字形顶相对基线（负值），
    // 基线落在 baselineY 与正文 CJK 一致。
    int draw_x = x + (boxW - w) / 2;
    int draw_y = baselineY + y0;
    for (int gy = 0; gy < h; gy++) {
        for (int gx = 0; gx < w; gx++) {
            uint8_t alpha = s_bitmap[gy * w + gx];
            if (alpha == 0) continue;
            uint8_t g;
            if (invert) g = (uint8_t)((alpha * 15 + 127) / 255);
            else        g = 15 - (uint8_t)((alpha * 15 + 127) / 255);
            fb_fast_pixel(fb, draw_x + gx, draw_y + gy, (uint8_t)(g << 4));
        }
    }
}

void icon_font_draw_sized(uint8_t* fb, int boxX, int boxY, int boxW, int boxH,
                          uint32_t cp, bool invert, int targetPx) {
    if (!fb || boxW <= 0 || boxH <= 0) return;
    if (cp == 0x270E) cp = 0xF03EB;  // 铅笔 → NF-Propo 的 pencil
    if (!s_ready) return;
    if (targetPx <= 0) targetPx = boxH;

    int glyph = stbtt_FindGlyphIndex(&s_info, (int)cp);
    if (glyph <= 0) {
        ESP_LOGW("icon", "sized cp=0x%X: glyph missing from subset", (unsigned)cp);
        return;
    }

    // 先按 em 尺度(scale=1)取字形包围盒。
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&s_info, glyph, 1.0f, 1.0f, &x0, &y0, &x1, &y1);
    int gw = x1 - x0, gh = y1 - y0;
    if (gw <= 0 || gh <= 0) return;

    // contain 缩放：宽字形(cloud_sync/format_list_checks)按宽缩放、窄字形(pencil/
    // lightbulb)按高缩放，全部落在 box 内。之前只按固定高缩放，宽字形会被
    // ICON_MAX_PX 宽度上限拒收 → 图标丢失(主页只剩 4 个)。
    float scaleW = (float)(boxW - 4) / (float)gw;
    float scaleH = (float)(boxH - 4) / (float)gh;
    float scale = (scaleW < scaleH) ? scaleW : scaleH;
    if (scale <= 0) return;

    stbtt_GetGlyphBitmapBox(&s_info, glyph, scale, scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || w > ICON_MAX_PX || h > ICON_MAX_PX) return;

    // 诊断：每个码点打一次，确认字形是否缺失/超宽被拒。
    static uint32_t s_logged_cp[16];
    static int s_logged_n = 0;
    bool logged = false;
    for (int i = 0; i < s_logged_n; i++) if (s_logged_cp[i] == cp) logged = true;
    if (!logged && s_logged_n < 16) {
        s_logged_cp[s_logged_n++] = cp;
        ESP_LOGI("icon", "sized cp=0x%X gw=%d gh=%d scale=%.4f -> w=%d h=%d box=%dx%d",
                 (unsigned)cp, gw, gh, (double)scale, w, h, boxW, boxH);
    }

    stbtt_MakeGlyphBitmap(&s_info, s_bitmap, w, h, w, scale, scale, glyph);
    fb_fast_sync();

    int draw_x = boxX + (boxW - w) / 2;
    int draw_y = boxY + (boxH - h) / 2;
    for (int gy = 0; gy < h; gy++) {
        for (int gx = 0; gx < w; gx++) {
            uint8_t alpha = s_bitmap[gy * w + gx];
            if (alpha == 0) continue;
            uint8_t g;
            if (invert) g = (uint8_t)((alpha * 15 + 127) / 255);
            else        g = 15 - (uint8_t)((alpha * 15 + 127) / 255);
            fb_fast_pixel(fb, draw_x + gx, draw_y + gy, (uint8_t)(g << 4));
        }
    }
}
