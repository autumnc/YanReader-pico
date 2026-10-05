#pragma once

// 4bpp 帧缓冲快速直写。
//
// epdiy 的 epd_draw_pixel() 每写一个像素都要：一次 _rotate() 旋转 switch，两次
// 跨编译单元的 epd_width()/epd_height() 做边界检查，再两次取下标算地址。一屏中文
// 正文约 40 万像素，这里是渲染开销的大头——实测写作模式整屏重绘 ~280ms，其中
// ~200ms 耗在这一串调用上（主循环被阻塞，触摸点按连按会丢）。
//
// 这里把旋转与尺寸缓存起来（每次绘制入口同步一次），逐像素只剩一次内联的地址
// 计算 + 半字节读改写。语义与 epd_draw_pixel 完全一致：旋转按 epdiy 的 _rotate，
// 越界丢弃；负坐标同样会被边界检查挡掉（等价于 epdiy 走 uint16_t 回绕后越界）。
//
// 只在旋转/尺寸变化时重新调用 epd_get_rotation()/epd_width()/epd_height()，所以
// 方向切换后第一次绘制会自动跟上，不依赖 epdiy 内部的任何缓存。

#include <stdint.h>
#include <string.h>

#include "epdiy.h"

typedef struct {
    int w;    // 物理缓冲宽（= epd_width()）
    int h;    // 物理缓冲高（= epd_height()）
    int rot;  // 缓存的 EPD_ROT_*；-1 表示未同步
} FbFastGeom;

static FbFastGeom g_fb_fast = {0, 0, -1};

// 每个绘制入口调用一次（不是每像素）。
static inline void fb_fast_sync(void) {
    int rot = (int)epd_get_rotation();
    if (rot != g_fb_fast.rot) {
        g_fb_fast.rot = rot;
        g_fb_fast.w = epd_width();
        g_fb_fast.h = epd_height();
    }
}

// ── 旋转映射的唯一出处（P4）─────────────────────────────────────────────
// 全仓只在这里定义"逻辑坐标 ↔ 物理坐标"的四个方向。参数是 (rot, 物理宽, 物理高)，
// 而不是上面那份缓存 —— 这样 ui_render 的差分路径（core1）可以直接用它、不必读
// g_fb_fast 这个 core0 侧共享的缓存结构，thread-safety 与原来逐字一致。
// 两份方向定义（to/from）刻意同处一文件：改方向时一处改、两处对。
static inline void fb_rot_to_phys(int rot, int w, int h, int x, int y, int *px, int *py) {
    int ax = x, ay = y;
    switch (rot) {
        case EPD_ROT_LANDSCAPE:
            break;
        case EPD_ROT_PORTRAIT: {
            int t = ax; ax = ay; ay = t;
            ax = w - ax - 1;
            break;
        }
        case EPD_ROT_INVERTED_LANDSCAPE:
            ax = w - ax - 1;
            ay = h - ay - 1;
            break;
        case EPD_ROT_INVERTED_PORTRAIT: {
            int t = ax; ax = ay; ay = t;
            ay = h - ay - 1;
            break;
        }
        default:
            break;
    }
    *px = ax;
    *py = ay;
}

// 物理坐标 → 逻辑坐标（fb_rot_to_phys 的逆）。ui_render 的差分包围盒把变化的物理点
// 映回逻辑坐标时用；方向定义与上面同源，不再是 ui_render 里那份手抄的 switch。
// 未知 rot 落到 270°（沿用 ui_render 原来的 default 语义；实际 rot 恒为四种之一）。
static inline void fb_rot_from_phys(int rot, int w, int h, int px, int py, int *lx, int *ly) {
    switch (rot) {
        case EPD_ROT_LANDSCAPE:          *lx = px;         *ly = py;         break;
        case EPD_ROT_PORTRAIT:           *lx = py;         *ly = w - 1 - px; break;
        case EPD_ROT_INVERTED_LANDSCAPE: *lx = w - 1 - px; *ly = h - 1 - py; break;
        default:                         *lx = h - 1 - py; *ly = px;         break;  // 270°
    }
}

// 逻辑坐标 → 物理坐标。越界返回 0，否则 *px/*py 有效并返回 1。
static inline int fb_fast_to_phys(int x, int y, int *px, int *py) {
    int ax, ay;
    fb_rot_to_phys(g_fb_fast.rot, g_fb_fast.w, g_fb_fast.h, x, y, &ax, &ay);
    if (ax < 0 || ax >= g_fb_fast.w) return 0;
    if (ay < 0 || ay >= g_fb_fast.h) return 0;
    *px = ax;
    *py = ay;
    return 1;
}

static inline uint8_t *fb_fast_byte(uint8_t *fb, int px, int py) {
    return fb + (size_t)py * (size_t)(g_fb_fast.w / 2) + (px >> 1);
}

// 4bpp 颜色（低 4 位有效）→ 帧缓冲。
static inline void fb_fast_pixel(uint8_t *fb, int x, int y, uint8_t color) {
    int px, py;
    if (!fb_fast_to_phys(x, y, &px, &py)) return;
    uint8_t *p = fb_fast_byte(fb, px, py);
    if (px & 1) *p = (uint8_t)((*p & 0x0F) | (color & 0xF0));
    else        *p = (uint8_t)((*p & 0xF0) | (color >> 4));
}

// 取当前灰度（0..15）。
static inline uint8_t fb_fast_get_gray(uint8_t *fb, int x, int y, uint8_t fallback) {
    int px, py;
    if (!fb_fast_to_phys(x, y, &px, &py)) return fallback;
    uint8_t b = *fb_fast_byte(fb, px, py);
    return (px & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0x0F);
}

// 写当前灰度（0..15）。
static inline void fb_fast_set_gray(uint8_t *fb, int x, int y, uint8_t g) {
    int px, py;
    if (!fb_fast_to_phys(x, y, &px, &py)) return;
    uint8_t *p = fb_fast_byte(fb, px, py);
    if (px & 1) *p = (uint8_t)((*p & 0x0F) | ((g & 0x0F) << 4));
    else        *p = (uint8_t)((*p & 0xF0) | (g & 0x0F));
}

// 不带边界检查的旋转映射（矩形填充自行裁剪）。逻辑与 fb_fast_to_phys 相同，
// 只是省掉边界检查 —— 共用 fb_rot_to_phys，方向只有一份定义。
static inline void fb_fast_map(int x, int y, int *px, int *py) {
    fb_rot_to_phys(g_fb_fast.rot, g_fb_fast.w, g_fb_fast.h, x, y, px, py);
}

// 矩形填充：整字节写。逐像素的 fb_fast_pixel 实测约 120ns/像素（半字节读改写），
// 而键盘面板底、键帽、状态栏填色这类大块矩形动辄十几万像素，是重绘的大头。
// 旋转是「转置+翻转」的仿射映射，逻辑矩形映到物理仍是矩形，所以先把矩形映射成
// 物理包围盒，再逐物理行按字节填充：首尾半字节单独处理，中间整字节走 memset。
// 颜色非 0x00/0xF0（如抗锯齿中间灰）时逐字节写入，语义与逐像素完全一致。
static inline void fb_fast_fill_rect(uint8_t *fb, int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0 || g_fb_fast.rot < 0) return;
    int px0, py0, px1, py1;
    fb_fast_map(x, y, &px0, &py0);
    fb_fast_map(x + w - 1, y + h - 1, &px1, &py1);
    if (px0 > px1) { int t = px0; px0 = px1; px1 = t; }
    if (py0 > py1) { int t = py0; py0 = py1; py1 = t; }
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > g_fb_fast.w - 1) px1 = g_fb_fast.w - 1;
    if (py1 > g_fb_fast.h - 1) py1 = g_fb_fast.h - 1;
    if (px0 > px1 || py0 > py1) return;

    // 一个字节装两个像素：偶像素=低半字节，奇像素=高半字节。
    uint8_t full = (uint8_t)((color >> 4) | (color & 0xF0));
    int stride = g_fb_fast.w / 2;
    int b0 = px0 >> 1, b1 = px1 >> 1;
    uint8_t mask0 = (px0 & 1) ? 0xF0 : 0xFF;   // 首字节：起点落在高半字节
    uint8_t mask1 = (px1 & 1) ? 0xFF : 0x0F;   // 末字节：终点落在低半字节
    for (int py = py0; py <= py1; py++) {
        uint8_t *row = fb + (size_t)py * (size_t)stride;
        if (b0 == b1) {
            uint8_t m = (uint8_t)(mask0 & mask1);
            row[b0] = (uint8_t)((row[b0] & ~m) | (full & m));
        } else {
            row[b0] = (uint8_t)((row[b0] & ~mask0) | (full & mask0));
            int mid = b1 - b0 - 1;
            if (mid > 0) memset(row + b0 + 1, full, (size_t)mid);
            row[b1] = (uint8_t)((row[b1] & ~mask1) | (full & mask1));
        }
    }
}

// 灰度取反（异或绘制用）。
static inline void fb_fast_xor(uint8_t *fb, int x, int y) {
    int px, py;
    if (!fb_fast_to_phys(x, y, &px, &py)) return;
    uint8_t *p = fb_fast_byte(fb, px, py);
    if (px & 1) *p ^= 0xF0;
    else        *p ^= 0x0F;
}
