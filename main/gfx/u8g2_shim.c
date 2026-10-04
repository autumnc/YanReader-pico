/*
 * u8g2 兼容层实现：原语 → epdiy 4bpp framebuffer。
 */
#include "u8g2_shim.h"

#include "epdiy.h"
#include "fb_fast.h"   // 4bpp 快速直写（绕开 epd_draw_pixel 的逐像素旋转/边界开销）

// ---- 颜色取值 ----
static inline uint8_t shim_color(u8g2_t* u8g2) {
    switch (u8g2->color) {
        case 0: return 0x00;  // 黑
        case 1: return 0xF0;  // 白
        default: return 0x00; // 异或模式不在此处理
    }
}

// ---- 异或：逻辑坐标读物理灰度并取反（走 fb_fast 的缓存几何，不再逐像素旋转）----
static inline uint8_t shim_get_gray(u8g2_t* u8g2, int x, int y) {
    return fb_fast_get_gray(u8g2->fb, x, y, 15);
}

static inline void shim_set_gray(u8g2_t* u8g2, int x, int y, uint8_t g) {
    fb_fast_set_gray(u8g2->fb, x, y, g);
}

static inline void shim_xor_pixel(u8g2_t* u8g2, int x, int y) {
    fb_fast_xor(u8g2->fb, x, y);
}

void u8g2_InitDisplay(u8g2_t* u8g2) {
    (void)u8g2;  // 显示初始化由 hw/board 完成
}

void u8g2_SetPowerSave(u8g2_t* u8g2, uint8_t is_enable) {
    (void)u8g2;
    (void)is_enable;  // 上电/下电由 hw/board 与刷新流程控制
}

uint8_t* u8g2_GetBufferPtr(u8g2_t* u8g2) {
    return u8g2->fb;
}

uint32_t u8g2_GetBufferSize(u8g2_t* u8g2) {
    (void)u8g2;
    return (uint32_t)(epd_width() / 2 * epd_height());
}

void u8g2_set_fb(u8g2_t* u8g2, uint8_t* fb) {
    u8g2->fb = fb;
}

void u8g2_SendBuffer(u8g2_t* u8g2) {
    (void)u8g2;  // 真正刷屏由 ui_commit 统一驱动（整屏/局刷决策）
}

void u8g2_SetDrawColor(u8g2_t* u8g2, uint8_t color) {
    u8g2->color = color;
}

void u8g2_SetBitmapMode(u8g2_t* u8g2, uint8_t is_transparent) {
    u8g2->bitmap_mode = is_transparent;
}

// 矩形填充：走 fb_fast 直写（epd_fill_rect 内部也是逐像素调 epd_draw_pixel，
// 但每次都要重算旋转与边界；这里同步一次几何后循环内只剩内联地址计算）。
static inline void shim_fill(u8g2_t* u8g2, int x, int y, int w, int h, uint8_t color) {
    fb_fast_sync();
    fb_fast_fill_rect(u8g2->fb, x, y, w, h, color);
}

void u8g2_DrawPixel(u8g2_t* u8g2, int x, int y) {
    if (u8g2->color == 2) { shim_xor_pixel(u8g2, x, y); return; }
    fb_fast_sync();
    fb_fast_pixel(u8g2->fb, x, y, shim_color(u8g2));
}

void u8g2_DrawHLine(u8g2_t* u8g2, int x, int y, int w) {
    if (w <= 0) return;
    if (u8g2->color == 2) {
        for (int i = 0; i < w; i++) shim_xor_pixel(u8g2, x + i, y);
        return;
    }
    shim_fill(u8g2, x, y, w, 1, shim_color(u8g2));
}

void u8g2_DrawVLine(u8g2_t* u8g2, int x, int y, int h) {
    if (h <= 0) return;
    if (u8g2->color == 2) {
        for (int i = 0; i < h; i++) shim_xor_pixel(u8g2, x, y + i);
        return;
    }
    shim_fill(u8g2, x, y, 1, h, shim_color(u8g2));
}

void u8g2_DrawBox(u8g2_t* u8g2, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (u8g2->color == 2) {
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++)
                shim_xor_pixel(u8g2, x + xx, y + yy);
        return;
    }
    shim_fill(u8g2, x, y, w, h, shim_color(u8g2));
}

void u8g2_DrawFrame(u8g2_t* u8g2, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (u8g2->color == 2) {
        for (int i = 0; i < w; i++) { shim_xor_pixel(u8g2, x + i, y); shim_xor_pixel(u8g2, x + i, y + h - 1); }
        for (int i = 0; i < h; i++) { shim_xor_pixel(u8g2, x, y + i); shim_xor_pixel(u8g2, x + w - 1, y + i); }
        return;
    }
    uint8_t c = shim_color(u8g2);
    shim_fill(u8g2, x, y, w, 1, c);
    shim_fill(u8g2, x, y + h - 1, w, 1, c);
    shim_fill(u8g2, x, y, 1, h, c);
    shim_fill(u8g2, x + w - 1, y, 1, h, c);
}

void u8g2_DrawBitmap(u8g2_t* u8g2, int x, int y, int cnt, int h, const uint8_t* bitmap) {
    // MSB-first：bit 7 = 最左列，与 pjournal 字形数据一致。
    uint8_t transparent = (u8g2->bitmap_mode == 1);
    bool xorMode = (u8g2->color == 2);
    uint8_t c = shim_color(u8g2);
    if (!xorMode) fb_fast_sync();
    for (int row = 0; row < h; row++) {
        for (int byte = 0; byte < cnt; byte++) {
            uint8_t bits = bitmap[row * cnt + byte];
            for (int bit = 0; bit < 8; bit++) {
                if (!(bits & (0x80 >> bit))) continue;  // 0 位：透明模式跳过
                int px = x + byte * 8 + bit;
                int py = y + row;
                if (xorMode) { shim_xor_pixel(u8g2, px, py); continue; }
                fb_fast_pixel(u8g2->fb, px, py, c);
            }
        }
    }
    (void)transparent;  // 0 位天然跳过，语义一致
}

void u8g2_DrawXBM(u8g2_t* u8g2, int x, int y, int w, int h, const uint8_t* bitmap) {
    // LSB-first：bit 0 = 最左列。
    bool xorMode = (u8g2->color == 2);
    uint8_t c = shim_color(u8g2);
    if (!xorMode) fb_fast_sync();
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            if (!(bitmap[row * ((w + 7) / 8) + col / 8] & (1 << (col % 8)))) continue;
            if (xorMode) { shim_xor_pixel(u8g2, x + col, y + row); continue; }
            fb_fast_pixel(u8g2->fb, x + col, y + row, c);
        }
    }
}
