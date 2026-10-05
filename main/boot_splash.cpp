#include "boot_splash.h"

#include <cmath>
#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "epdiy.h"
#include "fb_fast.h"
#include "ttf_font.h"
#include "u8g2_shim.h"
#include "ui_helpers.h"

static const char *TAG = "BootSplash";

// ── 设计栅格 ─────────────────────────────────────────────────────────────
// 版式是在 1216×684 横屏上定稿的（SVG 原型见 tools/ 同名脚本的产物）。运行时按
// min(宽比, 高比) 统一缩放，并把设计原点搬到屏幕中心 —— 竖屏(684×1216)因此同样
// 居中成立，不需要第二套坐标。
//
// 全部位置写成「相对设计中心的偏移」，落在中心附近的元素只改一个 0。
static constexpr int kDesignW  = 1216;
static constexpr int kDesignH  = 684;
static constexpr int kDesignCx = 608;
static constexpr int kDesignCy = 342;

// 砚台石框（俯视的圆角方砚）
static constexpr int kStoneX = 478 - kDesignCx;   // -130
static constexpr int kStoneY = 120 - kDesignCy;   // -222
static constexpr int kStoneW = 260;
static constexpr int kStoneH = 260;
static constexpr int kStoneR = 34;                // 圆角半径
static constexpr int kStoneT = 8;                 // 描边粗细
// 墨池
static constexpr int kPoolCx = 608 - kDesignCx;   // 0
static constexpr int kPoolCy = 250 - kDesignCy;   // -92
static constexpr int kPoolR  = 96;
// 白文「研」（画在墨池里）
static constexpr int kYanPx = 90;
static constexpr int kYanBaseline = 284 - kDesignCy;   // -58
// 词标
static constexpr int kWordPx = 112;
static constexpr int kWordBaseline = 512 - kDesignCy;  // 170
static constexpr int kRuleX = 418 - kDesignCx;         // -190
static constexpr int kRuleY = 546 - kDesignCy;         // 204
static constexpr int kRuleW = 380;
static constexpr int kRuleH = 3;
// 三模式
// 44px 是照横屏原型定的，可设备是**竖屏**：整块设计按 min(宽比,高比)=0.5625 缩下来，
// 到屏上只剩 25px 高 —— 就是"太小了看不清"。放到 64px（屏上约 36px），版式仍不越界：
// 字顶在分隔线之下 20px，字底距设计底边 50px，整行宽 645 < 1216。
static constexpr int kModesPx = 64;
static constexpr int kModesTracking = 14;              // 字距（ttf 没有字距参数，逐字画）
static constexpr int kModesBaseline = 626 - kDesignCy; // 284

static constexpr const char *kWordmark = "Yan Reader";
static constexpr const char *kModes = "研读 · 研墨 · 研行";

// ── 帧时序 ───────────────────────────────────────────────────────────────
// 中间帧走局刷 DU（~220ms/帧），只有定稿那帧整屏 GC16。等待时间取得比刷新略短：
// core0 提交后立刻返回，推屏在 core1；等待只需给动画一个节奏，剩下的由
// ui_render_begin_frame 等缓冲自然限速。
static constexpr int kFrameCount = 6;
static constexpr int kFrameDelayMs[kFrameCount] = {140, 160, 180, 180, 200, 0};

namespace {

// 灰度值 0..15（0 全墨、15 全白），与 fb_fast / ttf_draw_text_px 的 fg/bg 同一口径。
struct Canvas {
    uint8_t *fb = nullptr;
    int cx = 0, cy = 0;
    float s = 1.0f;
    int x(int dx) const { return cx + (int)lroundf((float)dx * s); }
    int y(int dy) const { return cy + (int)lroundf((float)dy * s); }
    int len(int v) const { return (int)lroundf((float)v * s); }
};

// ── 基本图元 ─────────────────────────────────────────────────────────────
// 全部走 fb_fast（旋转与尺寸只同步一次），坐标是逻辑坐标。

static void fillRect(const Canvas &c, int dx, int dy, int w, int h, int g) {
    if (w <= 0 || h <= 0) return;
    fb_fast_sync();
    fb_fast_fill_rect(c.fb, c.x(dx), c.y(dy), w, h, (uint8_t)((g & 0xF) << 4));
}

// 实心椭圆（像素坐标中心）。逐扫描线算半宽，一条线一次整行填。
static void fillEllipsePx(const Canvas &c, float fx, float fy, float rx, float ry, int g) {
    if (rx <= 0.0f || ry <= 0.0f) return;
    fb_fast_sync();
    const uint8_t col = (uint8_t)((g & 0xF) << 4);
    const int y0 = (int)floorf(fy - ry), y1 = (int)ceilf(fy + ry);
    for (int y = y0; y <= y1; ++y) {
        const float d = ((float)y + 0.5f - fy) / ry;
        const float t = 1.0f - d * d;
        if (t <= 0.0f) continue;
        const int half = (int)lroundf(rx * sqrtf(t));
        if (half <= 0) continue;
        fb_fast_fill_rect(c.fb, (int)lroundf(fx) - half, y, 2 * half, 1, col);
    }
}

static void fillDisc(const Canvas &c, int dx, int dy, int r, int g) {
    fillEllipsePx(c, (float)c.x(dx), (float)c.y(dy), (float)c.len(r), (float)c.len(r), g);
}

// 圆环（描边圆）：逐扫描线取"外半径内、内半径外"的两段。
static void strokeCircle(const Canvas &c, int dx, int dy, int r, int w, int g) {
    const int rp = c.len(r), wp = c.len(w);
    if (rp <= 0 || wp <= 0) return;
    fb_fast_sync();
    const int cxp = c.x(dx), cyp = c.y(dy);
    const uint8_t col = (uint8_t)((g & 0xF) << 4);
    const float ro = (float)rp + (float)wp * 0.5f;
    const float ri = (float)rp - (float)wp * 0.5f;
    const int ymax = (int)ceilf(ro);
    for (int y = -ymax; y <= ymax; ++y) {
        const float d2 = (float)(y * y);
        if (d2 > ro * ro) continue;
        const int xo = (int)lroundf(sqrtf(ro * ro - d2));
        if (d2 >= ri * ri || ri <= 0.0f) {
            fb_fast_fill_rect(c.fb, cxp - xo, cyp + y, 2 * xo + 1, 1, col);
        } else {
            const int xi = (int)lroundf(sqrtf(ri * ri - d2));
            fb_fast_fill_rect(c.fb, cxp - xo, cyp + y, xo - xi, 1, col);
            fb_fast_fill_rect(c.fb, cxp + xi + 1, cyp + y, xo - xi, 1, col);
        }
    }
}

// 圆弧：沿弧线摆一串实心小圆（粗折线）。只用于圆角，角度是屏幕系（0°=右，顺时针）。
static void strokeArcPx(const Canvas &c, float fx, float fy, float r, float thickness,
                        float a0, float a1, int g) {
    const float half = thickness * 0.5f;
    const int steps = (int)(fabsf(a1 - a0) * r / 90.0f * 1.6f) + 6;
    for (int i = 0; i <= steps; ++i) {
        const float a = (a0 + (a1 - a0) * (float)i / (float)steps) * 3.14159265f / 180.0f;
        fillEllipsePx(c, fx + r * cosf(a), fy + r * sinf(a), half, half, g);
    }
}

// 圆角矩形描边：四条直边 + 四个 1/4 圆环。dx/dy 是左上角（相对设计中心）。
static void strokeRoundRect(const Canvas &c, int dx, int dy, int w, int h, int rad, int t, int g) {
    const int rp = c.len(rad), tp = c.len(t);
    const int wp = c.len(w), hp = c.len(h);
    if (rp * 2 >= wp || rp * 2 >= hp || tp <= 0) return;
    fillRect(c, dx + rad, dy, w - 2 * rad, t, g);                 // 上
    fillRect(c, dx + rad, dy + h - t, w - 2 * rad, t, g);         // 下
    fillRect(c, dx, dy + rad, t, h - 2 * rad, g);                 // 左
    fillRect(c, dx + w - t, dy + rad, t, h - 2 * rad, g);         // 右
    const float cr = (float)rp - (float)tp * 0.5f;                // 圆角中心线半径
    const float th = (float)tp;
    const float lx = (float)c.x(dx + rad), rx = (float)c.x(dx + w - rad);
    const float ty = (float)c.y(dy + rad), by = (float)c.y(dy + h - rad);
    strokeArcPx(c, lx, ty, cr, th, 180.0f, 270.0f, g);            // 左上
    strokeArcPx(c, rx, ty, cr, th, 270.0f, 360.0f, g);            // 右上
    strokeArcPx(c, rx, by, cr, th, 0.0f, 90.0f, g);               // 右下
    strokeArcPx(c, lx, by, cr, th, 90.0f, 180.0f, g);             // 左下
}

// ── 文字 ─────────────────────────────────────────────────────────────────
// ttf_draw_text_px 没有字距参数，而词标下的三模式靠 14px 字距才透气（见原型），
// 所以这里逐字画：先量总宽（含字距），再从左端一个个摆。

static int utf8Len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static void drawTracked(const Canvas &c, int dx, int baseline, int px, const char *text,
                        int tracking, int fg) {
    const int pxp = c.len(px), trk = c.len(tracking);
    if (pxp <= 0) return;
    const char *starts[32];
    int widths[32];
    int n = 0, total = 0;
    for (const char *p = text; *p != '\0' && n < 32;) {
        char buf[8];
        const int L = utf8Len((unsigned char)*p);
        memcpy(buf, p, (size_t)L);
        buf[L] = '\0';
        starts[n] = p;
        widths[n] = ttf_text_width_px(pxp, buf);
        total += widths[n];
        p += L;
        ++n;
    }
    if (n == 0) return;
    total += trk * (n - 1);
    int cur = c.x(dx) - total / 2;
    const int base = c.y(baseline);
    for (int i = 0; i < n; ++i) {
        char buf[8];
        const int L = utf8Len((unsigned char)starts[i][0]);
        memcpy(buf, starts[i], (size_t)L);
        buf[L] = '\0';
        ttf_draw_text_px(c.fb, cur, base, pxp, buf, TTF_ALIGN_LEFT,
                         (uint8_t)fg, (uint8_t)(fg == 0 ? 15 : 0));
        cur += widths[i] + trk;
    }
}

// ── 版式 ─────────────────────────────────────────────────────────────────
// 砚池定稿（帧 4/5 共用）。
static void drawMark(const Canvas &c, int stoneGray, int poolGray, int yanFg) {
    strokeRoundRect(c, kStoneX, kStoneY, kStoneW, kStoneH, kStoneR, kStoneT, stoneGray);
    fillDisc(c, kPoolCx, kPoolCy, kPoolR, poolGray);
    drawTracked(c, kPoolCx, kYanBaseline, kYanPx, "研", 0, yanFg);
}

static void drawFrame(const Canvas &c, int i) {
    switch (i) {
        case 0:  // 一滴墨悬在上方
            fillEllipsePx(c, (float)c.x(0), (float)c.y(-192),
                          (float)c.len(15), (float)c.len(22), 0);
            break;
        case 1:  // 将触未触，第一圈涟漪
            fillEllipsePx(c, (float)c.x(0), (float)c.y(-114),
                          (float)c.len(9), (float)c.len(13), 0);
            strokeCircle(c, kPoolCx, kPoolCy, 46, 4, 0);
            break;
        case 2:  // 入池：涟漪荡开
            strokeCircle(c, kPoolCx, kPoolCy, 54, 5, 0);
            strokeCircle(c, kPoolCx, kPoolCy, 104, 4, 0);
            strokeCircle(c, kPoolCx, kPoolCy, 156, 3, 0);
            fillEllipsePx(c, (float)c.x(kPoolCx), (float)c.y(kPoolCy),
                          (float)c.len(17), (float)c.len(11), 0);
            break;
        case 3:  // 涟漪淡去，砚台与墨池浮现
            strokeCircle(c, kPoolCx, kPoolCy, 54, 5, 13);
            strokeCircle(c, kPoolCx, kPoolCy, 104, 4, 13);
            drawMark(c, 11, 7, 15);
            break;
        case 4:  // 砚池定形 + 白文「研」
            drawMark(c, 0, 0, 15);
            break;
        default:  // 帧 5：词标 + 三模式
            drawMark(c, 0, 0, 15);
            drawTracked(c, 0, kWordBaseline, kWordPx, kWordmark, 4, 0);
            fillRect(c, kRuleX, kRuleY, kRuleW, kRuleH, 0);
            drawTracked(c, 0, kModesBaseline, kModesPx, kModes, kModesTracking, 0);
            break;
    }
}

}  // namespace

void bootSplashDraw() {
    if (!g_u8g2) return;

    const int prev_role = ttf_get_role();   // 画完原样还回去（见收尾处的说明）

    if (ttf_font_open_logo() != ESP_OK) {
        // 标志字体装不上（理论上不会：它是常驻 flash 的 5.4KB）——退回改名前的行为。
        ESP_LOGW(TAG, "标志字体不可用，退化为清屏");
        ui_clear();
        ui_commit();
        return;
    }
    ttf_set_role(TTF_ROLE_CONTENT_ALT);

    Canvas c;
    c.cx = ui_screen_w() / 2;
    c.cy = ui_screen_h() / 2;
    const float sx = (float)ui_screen_w() / (float)kDesignW;
    const float sy = (float)ui_screen_h() / (float)kDesignH;
    c.s = (sx < sy) ? sx : sy;

    const int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < kFrameCount; ++i) {
        // 中间帧一律局刷 DU：整屏 GC16 一帧 404ms，六帧就是 2.4s —— 而这六帧的
        // 变化本来就集中在屏幕中央一小块。定稿帧关掉，让它走整屏。
        if (i == 1) ui_set_fast_partial(true);
        if (i == kFrameCount - 1) ui_set_fast_partial(false);

        ui_clear();
        c.fb = u8g2_GetBufferPtr(g_u8g2);   // ui_clear 已把 shim 指向本帧工作缓冲
        drawFrame(c, i);
        ui_commit();

        if (i + 1 < kFrameCount && kFrameDelayMs[i] > 0) {
            vTaskDelay(pdMS_TO_TICKS(kFrameDelayMs[i]));
        }
    }
    // 收尾整屏 GC16：动画途中局刷攒下的残影一次清掉，也让面板落到一个干净态。
    ui_full_refresh_now();

    ESP_LOGI(TAG, "开机动画 %d 帧，用时 %lld ms", kFrameCount,
             (long long)((esp_timer_get_time() - t0) / 1000));

    // ★ 立刻把次字面还回去 —— 那是阅读器给"本书第二个字体家族"的槽。
    // 顺序有讲究：必须先把 s_cur 挪出次字面再卸。ttf_font_close_alt() 内部是
    // face_enter(ALT) → face_unload() → face_leave(enter 时的 s_cur)，此刻 s_cur 正是
    // 次字面本身，先卸后还原就会把它留在**已卸空**的面上 —— 之后每一次绘制都在
    // `if (!font_ready)` 处静默返回，首屏一个字都不出来（见 ttf_font.c 里那条告警）。
    ttf_set_role(prev_role);
    ttf_font_close_alt();
}
