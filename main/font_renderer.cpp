#include "font_renderer.h"

#include <cstring>
#include <cmath>
#include <esp_log.h>

#include "u8g2_shim.h"
#include "ttf_font.h"
#include "icon_font.h"
#include "epdiy.h"

static const char *TAG = "Font";

// g_font / g_content_font 的文本面就是内容面（默认参数）—— 界面文本也要用用户选的
// 字体。g_vk_font 例外：虚拟键盘固定内置字体（见 font_renderer.h）。
// g_font 还把**拉丁**钉回内置等宽路（第二个参数）：用户字体的比例拉丁塞进半格会
// 溢出到邻格、相邻字母叠在一起，界面上的拉丁（设置项里的字体名等）尤其难看；
// CJK 不受影响，仍走用户字体。正文那类内容实例不钉 —— 见 drawCellGlyph 的说明。
FontRenderer g_font(TTF_ROLE_CONTENT, /*latinBuiltin=*/true);
FontRenderer g_content_font;
FontRenderer g_vk_font(TTF_ROLE_UI);

// 共享格子模型（见头文件说明）。定义一次，全部实例共用。
int  FontRenderer::font_size_   = 22;
int  FontRenderer::line_height_ = 50;
int  FontRenderer::ascent_      = 44;
int  FontRenderer::descent_     = 6;
int  FontRenderer::px_          = 50;
bool FontRenderer::loaded_      = false;

// 界面（UI 档）格子的 px 高，由 setSize() 记录 —— setGridPx() **不碰它**，所以
// 正文作用域一开一关不会把它带走。见 uiPxHeight()。
// 初值**必须与 px_ 的初值相同**（=50）：setSize() 被调用之前两边就该相等，否则上电
// 早期那一小段（开机动画等）会被 uiFontGuard 当成"界面字号没钉回"误报。
static int s_ui_px = 50;
int FontRenderer::uiPxHeight() { return s_ui_px; }

// 定义于 main.cpp / ui_helpers（u8g2 shim 句柄，含 epdiy framebuffer 指针）

// ---- 状态栏专用图标（pjournal 自定义 PUA，NF-Propo 无对应字形，程序化绘制） ----
static bool isStatusSymbol(uint32_t cp) {
    return cp == 0xE001 || cp == 0xE002 || cp == 0xE003 ||
           (cp >= 0xE004 && cp <= 0xE00C) ||
           (cp >= 0xE018 && cp <= 0xE02D) ||
           cp == 0xE039;   // 充电闪电
}

static int batteryLevel(uint32_t cp) {
    if (cp >= 0xE018 && cp <= 0xE022) return cp - 0xE018;
    if (cp >= 0xE023 && cp <= 0xE02D) return cp - 0xE023;
    return -1;  // E001 = 通用电池（无分级）
}

static int progressLevel(uint32_t cp) {
    return (cp >= 0xE004 && cp <= 0xE00C) ? (cp - 0xE004) : -1;
}

// UTF-8 编码（返回值 = 字节数）
static int utf8Encode(uint32_t cp, char *out) {
    if (cp <= 0x7F) { out[0] = (char)cp; return 1; }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

bool FontRenderer::begin(int role) {
    role_ = role;
    // 官方 ttf_font 层（内置 builtin.ttf 或 SD 外置字体）+ 图标字体子集
    esp_err_t err = ttf_font_init();
    icon_font_init();
    if (err != ESP_OK && !ttf_font_ready()) {
        ESP_LOGE(TAG, "ttf_font_init failed: %s", esp_err_to_name(err));
    }
    font_size_ = 0;  // 强制 setSize 生效
    return setSize(20);   // 默认 UI 字号（见 setSize() 的档位表）
}

bool FontRenderer::setSize(int fontSize) {
    if (fontSize == font_size_) return loaded_;

    // 只有这三个档位：20pt 是当前 UI 默认字号（22pt 降一档，见 settings_manager 的
    // fontSize()），18pt 是密度更高的备用档。line_height_ 同时是 CJK 的格宽，
    // 所以它一变，全 UI 的标题栏/状态栏/列表行高（都写成 FONT_H 的派生式）一起等比缩。
    if (fontSize == 22) {
        line_height_ = 50;
    } else if (fontSize == 20) {
        line_height_ = 45;
    } else if (fontSize == 18) {
        line_height_ = 41;
    } else {
        return false;
    }

    px_ = line_height_;
    font_size_ = fontSize;
    s_ui_px = px_;   // 界面格子的 px 高：正文作用域里"钉回界面字号"用的就是它
    // 格子几何永远由**内置面**定义：换用户字体时标题基线/反白块高度不漂移，
    // 且 main.cpp 各处 setSize 语义不变。见头文件"共享格子模型"。
    ttf_set_role(TTF_ROLE_UI);
    int asc = ttf_ascender_px(px_);
    if (asc <= 0 || asc >= px_) asc = px_ * 78 / 100;  // 未就绪时用 0.78em 近似
    ascent_ = asc;
    descent_ = px_ - ascent_;
    if (descent_ < 0) descent_ = 0;

    loaded_ = true;  // 图标字体至少可用；正文字体缺失时 CJK 回落为空白
    ESP_LOGI(TAG, "Font %dpt: line=%d asc=%d desc=%d px=%d",
             font_size_, line_height_, ascent_, descent_, px_);
    return true;
}

bool FontRenderer::reloadFont() {
    ttf_set_role(role_);        // 只清本实例所用字面的缓存
    ttf_font_cache_clear();
    int sz = font_size_;
    font_size_ = 0;  // 强制 setSize 用当前字体重算 asc/desc
    return setSize(sz);         // 内部会把角色切回 UI 量格子
}

// ── 共享格子的作用域快照（见头文件 GridSnapshot / FontScope）──────────────
FontRenderer::GridSnapshot FontRenderer::snapshotGrid() {
    return GridSnapshot{font_size_, line_height_, ascent_, descent_, px_, loaded_};
}

void FontRenderer::restoreGrid(const GridSnapshot &s) {
    font_size_   = s.size;
    line_height_ = s.lineH;
    ascent_      = s.asc;
    descent_     = s.desc;
    px_          = s.px;
    loaded_      = s.loaded;
}

// 按 px 直接定格子高。字形缓存是按 (codepoint, size) 存的（见 ttf_font.c 的
// cache_bucket），所以来回换字号只是多几份缓存条目，不会串号；也正因为如此这里
// **不清缓存** —— 清一次会把整段的 CJK 位图全丢掉，正文/界面来回换就是每帧重光栅化。
void FontRenderer::setGridPx(int px) {
    if (px < FONT_GRID_PX_MIN) px = FONT_GRID_PX_MIN;
    if (px > FONT_GRID_PX_MAX) px = FONT_GRID_PX_MAX;
    line_height_ = px;
    px_          = px;
    // 格子几何恒由**内置面**定义（同 setSize）：换用户字体不漂移。font_size_ 不动，
    // 理由见头文件。
    ttf_set_role(TTF_ROLE_UI);
    int asc = ttf_ascender_px(px_);
    if (asc <= 0 || asc >= px_) asc = px_ * 78 / 100;  // 未就绪时用 0.78em 近似
    ascent_  = asc;
    descent_ = px_ - ascent_;
    if (descent_ < 0) descent_ = 0;
    loaded_  = true;
}

uint32_t FontRenderer::utf8Decode(const char *&str) {
    if (!str || !*str) return 0;
    uint8_t c = (uint8_t)*str;
    if (c < 0x80) { str++; return c; }
    if ((c & 0xE0) == 0xC0) {
        if ((str[1] & 0xC0) != 0x80) { str++; return 0; }
        uint32_t cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(str[1] & 0x3F);
        str += 2; return cp;
    }
    if ((c & 0xF0) == 0xE0) {
        if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80) { str++; return 0; }
        uint32_t cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(str[1] & 0x3F) << 6) |
                      (uint32_t)(str[2] & 0x3F);
        str += 3; return cp;
    }
    if ((c & 0xF8) == 0xF0) {
        if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80 ||
            (str[3] & 0xC0) != 0x80) { str++; return 0; }
        uint32_t cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(str[1] & 0x3F) << 12) |
                      ((uint32_t)(str[2] & 0x3F) << 6) | (uint32_t)(str[3] & 0x3F);
        str += 4; return cp;
    }
    str++;
    return 0;
}

// 电池是横向长条图标，占**两格**：一格宽的方格里塞 4 格电量只会得到 2px 的碎块，
// 缩到状态栏里根本数不出来。其余状态图标保持一格。
static bool isWideStatusSymbol(uint32_t cp) {
    return cp == 0xE001 || (cp >= 0xE018 && cp <= 0xE02D);   // 电池 / 电池电平 / 键盘电量
}

int FontRenderer::charWidth(uint32_t cp) {
    if (isWideStatusSymbol(cp)) return line_height_;   // 电池：两格
    if (isStatusSymbol(cp)) return halfAdvance();      // 其余窄图标
    return cp < 0x80 ? halfAdvance() : line_height_;
}

int FontRenderer::textWidth(const char *text) {
    int w = 0;
    while (*text) {
        uint32_t cp = utf8Decode(text);
        if (cp == 0) continue;
        w += charWidth(cp);
    }
    return w;
}

// ---- 程序化状态图标 ----

// 横向电池：外壳 + 正极帽 + 内部**分格**表示电量（自左向右逐格点亮）。
// level<0 表示电量未知：只画空壳。分格比"一根实心条"更容易一眼数出大概剩多少。
static void drawBatteryGlyph(uint8_t *fb, int bx, int by, int bw, int bh,
                             int level, uint8_t color) {
    // 正极帽：长条电池用 3px，小图标退回 2px。
    const int nubW = (bh >= 20) ? 3 : 2;
    if (bw <= nubW + 6 || bh < 6) return;
    const int bodyW = bw - nubW;

    EpdRect body = { bx, by, bodyW, bh };
    epd_draw_rect(body, color, fb);
    EpdRect nub = { bx + bodyW, by + bh / 4, nubW, bh / 2 };
    epd_fill_rect(nub, color, fb);

    if (level < 0) return;   // 未知电量：留空壳

    const int pad = (bh >= 14) ? 3 : 2;
    const int ix = bx + 1 + pad, iy = by + 1 + pad;
    const int inW = bodyW - 2 - pad * 2;   // 外壳内净区（扣掉 1px 边框）
    const int inH = bh - 2 - pad * 2;
    if (inW < 3 || inH < 3) return;

    // 4 格。内宽不够就缩缝隙，再不够就退化成实心条（宁可没格，也不能画出框）。
    const int segs = 4;
    int gap = (inW >= 28) ? 3 : 2;
    int segW = (inW - gap * (segs - 1)) / segs;
    if (segW < 2) { gap = 1; segW = (inW - gap * (segs - 1)) / segs; }
    if (segW < 1) {   // 退化态：一整条
        EpdRect s = { ix, iy, inW, inH };
        epd_fill_rect(s, color, fb);
        return;
    }

    // 向上取整：只要有电（1%）就点亮首格，空电池=关机，不该出现。
    int filled = (level * segs + 9) / 10;
    if (filled < 1) filled = 1;
    if (filled > segs) filled = segs;

    for (int i = 0; i < filled; i++) {
        const int sx = ix + i * (segW + gap);
        if (sx + segW > ix + inW) break;
        EpdRect s = { sx, iy, segW, inH };
        epd_fill_rect(s, color, fb);
    }
}

// 充电标识：一个闪电。画在电池**旁边**（自己占一个图标位），不压在格子上——
// 图标只有几十像素、格子和底色黑白相间，反白画的闪电会有一半看不见。
static void drawChargeBolt(uint8_t *fb, int cx, int cy, int r, uint8_t color) {
    if (r < 3) r = 3;
    const int w = (r * 2) / 3;
    const int w2 = (w < 2) ? 2 : w;
    // 上下两个三角错开，拼成经典的 Z 形闪电
    epd_fill_triangle(cx + w2, cy - r, cx - w2, cy + r / 5, cx + w2 / 3, cy + r / 5,
                      color, fb);
    epd_fill_triangle(cx - w2, cy + r, cx + w2, cy - r / 5, cx - w2 / 3, cy - r / 5,
                      color, fb);
}

static void drawBluetoothGlyph(uint8_t *fb, int cx, int cy, int r, uint8_t color) {
    // 蓝牙 rune：竖线 + 上下两三角（用 epd_fill_triangle）
    epd_draw_vline(cx, cy - r, 2 * r, color, fb);
    epd_fill_triangle(cx, cy - r + 2, cx + r - 2, cy - 2, cx, cy + r - 4,
                      color, fb);
    epd_fill_triangle(cx, cy - r + 4, cx - r + 2, cy + 2, cx, cy + r - 2,
                      color, fb);
}

static void drawProgressPie(uint8_t *fb, int cx, int cy, int r, int level,
                            uint8_t color) {
    if (r < 2) return;
    epd_draw_circle(cx, cy, r, color, fb);
    if (level <= 0) return;
    if (level >= 8) { epd_fill_circle(cx, cy, r - 1, color, fb); return; }

    // 从 12 点方向顺时针填充 level/8 扇区，用三角扇近似
    float full = 360.0f * level / 8.0f;
    int steps = 6;
    float prev_x = 0, prev_y = (float)-r;
    for (int i = 1; i <= steps; i++) {
        float a = full * i / steps * (float)M_PI / 180.0f;
        float nx = sinf(a) * r;
        float ny = -cosf(a) * r;
        epd_fill_triangle(cx, cy, cx + (int)lroundf(prev_x), cy + (int)lroundf(prev_y),
                          cx + (int)lroundf(nx), cy + (int)lroundf(ny), color, fb);
        prev_x = nx; prev_y = ny;
    }
}

void FontRenderer::drawStatusSymbol(int x, int y, uint32_t cp, bool invert, int cellW) {
    uint8_t *fb = u8g2_GetBufferPtr(g_u8g2);
    if (!fb) return;
    uint8_t color = invert ? 0xF0 : 0x00;
    int top = y - ascent_;
    int boxH = line_height_;

    if (cp == 0xE003) {  // 分割线
        int cx = x + cellW / 2;
        EpdRect bar = { cx - 1, top + 4, 2, boxH - 8 };
        epd_fill_rect(bar, color, fb);
        return;
    }

    int lvl = batteryLevel(cp);
    if (cp == 0xE001 || lvl >= 0) {  // 电池 / 电池电平：占两格，横向长条
        int bw = cellW - 4;
        int bh = boxH / 2;
        if (bh > bw / 2) bh = bw / 2;   // 长边至少是短边的两倍，才像电池
        if (bh < 10) bh = 10;
        int bx = x + (cellW - bw) / 2;
        int by = top + (boxH - bh) / 2;
        drawBatteryGlyph(fb, bx, by, bw, bh, cp == 0xE001 ? -1 : lvl, color);
        return;
    }

    if (cp == 0xE002) {  // 蓝牙
        int r = cellW / 2 - 2;
        if (r < 2) r = 2;
        int cx = x + cellW / 2;
        int cy = top + boxH / 2;
        drawBluetoothGlyph(fb, cx, cy, r, color);
        return;
    }

    if (cp == 0xE039) {  // 充电闪电（电池旁边的那一格）
        int r = cellW / 3;
        if (r < 3) r = 3;
        drawChargeBolt(fb, x + cellW / 2, top + boxH / 2, r, color);
        return;
    }

    int plvl = progressLevel(cp);
    if (plvl >= 0) {  // 进度圆环
        int r = cellW / 2 - 2;
        if (r < 2) r = 2;
        int cx = x + cellW / 2;
        int cy = top + boxH / 2;
        drawProgressPie(fb, cx, cy, r, plvl, color);
        return;
    }
}

// ---- 单字符绘制 ----

void FontRenderer::drawCellGlyph(int x, int y, int cellW, uint32_t cp,
                                 bool invert, bool bold) {
    if (!g_u8g2) return;
    // 只有走到 ttf 分支的字形才受文本面影响（ASCII/图标走 icon_font，与面无关），
    // 但选面是 O(1)，放在最前面最省心。
    ttf_set_role(role_);
    uint8_t *fb = u8g2_GetBufferPtr(g_u8g2);

    // 反色：先填黑 cell
    if (invert) {
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, x, y - ascent_, cellW, line_height_);
        u8g2_SetDrawColor(g_u8g2, 1);
    }

    if (isStatusSymbol(cp)) {
        drawStatusSymbol(x, y, cp, invert, cellW);
        if (bold) drawStatusSymbol(x + 1, y, cp, invert, cellW);
        return;
    }

    // 拉丁字母/数字/标点：装了外置字体(内容面不是内置)就用**用户字体的字形**，
    // 仍然画进这个固定半格 —— 格子模型(1 格 = halfAdvance)与所有测量纹丝不动。
    // 但比例拉丁塞进半格会**溢出到邻格**：字形按 px_(=行高)光栅，宽字母(w/m/W)有
    // 40px 量级，而格宽只有 halfAdvance(≈25px)，下面 dx = x + (cellW-gw)/2 直接变成
    // 负数 → 相邻字母叠在一起（"英文字连在一起"）。这正是界面实例 g_font 用
    // latin_builtin_ 把拉丁钉回内置等宽路的原因（NF-Propo 0.5em，逐像素与没装外置
    // 字体时一致）。正文/候选那类内容实例不钉：在那儿用用户字体的比例拉丁是想要的
    // 样子，且整行英文的步进同样是半格，取舍与从前一致。
    // 缺字一律由 ttf 层自动回落到内置面补(见 ttf_font.c 的替补)。
    // 虚拟键盘那个实例文本面是 UI(role_ == TTF_ROLE_UI)，所以键盘的字母不受影响。
    const bool user_latin = (cp < 0x80) && role_ == TTF_ROLE_CONTENT &&
                            !latin_builtin_ && !ttf_font_is_builtin();
    if (cp < 0x80 && !user_latin) {
        // ASCII（等宽 0.5em）按基线对齐，与 CJK 同基线，避免拉丁字符高低不平。
        icon_font_draw_baseline(fb, x, y, cellW, line_height_, cp, invert);
        if (bold) icon_font_draw_baseline(fb, x + 1, y, cellW, line_height_, cp, invert);
        return;
    }
    // 图标/全角符号走 NF-Propo 子集。全角文字字形（标点/字母/数字）按基线对齐，
    // 与正文 CJK 同基线；纯图标（PUA/几何符号）居中。
    // (cp < 0x80 走到这里只有"用户字体的拉丁"这一种情况，下面的 ttf 分支照画。)
    if (cp >= 0x80 && icon_font_is_icon(cp)) {
        if (icon_font_is_fullwidth(cp)) {
            icon_font_draw_baseline(fb, x, y, cellW, line_height_, cp, invert);
            if (bold) icon_font_draw_baseline(fb, x + 1, y, cellW, line_height_, cp, invert);
        } else {
            int boxY = y - ascent_;
            icon_font_draw(fb, x, boxY, cellW, line_height_, cp, invert);
            if (bold) icon_font_draw(fb, x + 1, boxY, cellW, line_height_, cp, invert);
        }
        return;
    }

    // 其余（CJK 等）走官方 ttf_font —— 文本面由上面的 ttf_set_role(role_) 选好，
    // 缺字会自动去内置面补。用户字体的拉丁（上面的 user_latin）也落到这里，
    // 只是它的 cellW 是那个半格。
    char ch[5];
    int n = utf8Encode(cp, ch);
    ch[n] = '\0';
    int gw = ttf_text_width_px(px_, ch);
    int dx = x + (cellW - gw) / 2;
    uint8_t fg = invert ? 15 : 0;
    uint8_t bg = invert ? 0 : 15;
    if (bold) {
        ttf_draw_text_px(fb, dx + 1, y, px_, ch, TTF_ALIGN_LEFT, fg, bg);
        ttf_draw_text_px(fb, dx, y + 1, px_, ch, TTF_ALIGN_LEFT, fg, bg);
    }
    ttf_draw_text_px(fb, dx, y, px_, ch, TTF_ALIGN_LEFT, fg, bg);
}

int FontRenderer::drawText(int x, int y, const char *text, bool invert) {
    int orig_x = x;
    while (*text) {
        uint32_t cp = utf8Decode(text);
        if (cp == 0) continue;
        int adv = charWidth(cp);
        drawCellGlyph(x, y, adv, cp, invert, false);
        x += adv;
    }
    return x - orig_x;
}

int FontRenderer::drawTextStyled(int x, int y, const char *text, const TextStyle &ts) {
    int orig_x = x;
    int w = textWidth(text);

    if (ts.invert) {
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, x, y - ascent_, w, line_height_);
        u8g2_SetDrawColor(g_u8g2, 1);
    }

    while (*text) {
        uint32_t cp = utf8Decode(text);
        if (cp == 0) continue;
        int adv = charWidth(cp);
        drawCellGlyph(x, y, adv, cp, ts.invert, ts.bold);

        if (ts.emph && cp != 0x20) {
            int cx = x + adv / 2;
            int cy = y + descent_ + 1;
            u8g2_DrawBox(g_u8g2, cx - 1, cy, 3, 2);
        }
        x += adv;
    }

    if (ts.underline) u8g2_DrawHLine(g_u8g2, orig_x, y + descent_, w);
    if (ts.strike) {
        int sy = y - ascent_ + line_height_ / 2;
        u8g2_DrawHLine(g_u8g2, orig_x, sy, w);
        u8g2_DrawHLine(g_u8g2, orig_x, sy + 1, w);
    }
    if (ts.invert) u8g2_SetDrawColor(g_u8g2, 0);
    return w;
}
