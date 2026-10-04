/*
 * u8g2 兼容层：把 pjournal 用到的 u8g2 原语映射到 epdiy 4bpp 灰度 framebuffer。
 * 只覆盖 pjournal 实际用到的 9 个原语 + SendBuffer/GetBuffer + Init/PowerSave。
 * 坐标是「逻辑坐标」(旋转后)，由 epdiy 的 _rotate 映射到物理帧缓冲。
 *
 * 颜色语义：SetDrawColor(0)=黑(0x00)、(1)=白(0xF0)、(2)=异或(逐像素取反灰度)。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct u8g2_struct {
    uint8_t* fb;          // epdiy 4bpp 物理 framebuffer (epd_width()/2 * epd_height() 字节)
    int color;            // 当前颜色：0=黑 1=白 2=异或
    uint8_t bitmap_mode;  // 0=实心 1=透明(DrawBitmap 用)
} u8g2_struct;

typedef u8g2_struct u8g2_t;

// 全局 shim 句柄（定义在 hw/board.cpp）。各绘制模块以前各自就地写一份
// `extern u8g2_t *g_u8g2;`，外加十来行手搓的 u8g2 函数原型——那些原型把形参写成
// `void *`，跟这里的 `u8g2_t *` 对不上，同一个 C 链接符号有了两套声明。
// 声明收在这儿，谁要画图就 include 这一个头，不用再抄。
extern u8g2_t *g_u8g2;

void u8g2_InitDisplay(u8g2_t* u8g2);
void u8g2_SetPowerSave(u8g2_t* u8g2, uint8_t is_enable);
uint8_t* u8g2_GetBufferPtr(u8g2_t* u8g2);
uint32_t u8g2_GetBufferSize(u8g2_t* u8g2);
// 换绘制目标（双工作缓冲轮换用）。ui_render 在每帧起始把 shim 指向当前工作缓冲；
// 此后所有 u8g2_Draw* 都写进那一块，直到下一次 set。
void u8g2_set_fb(u8g2_t* u8g2, uint8_t* fb);
void u8g2_SendBuffer(u8g2_t* u8g2);
void u8g2_SetDrawColor(u8g2_t* u8g2, uint8_t color);
void u8g2_SetBitmapMode(u8g2_t* u8g2, uint8_t is_transparent);
void u8g2_DrawPixel(u8g2_t* u8g2, int x, int y);
void u8g2_DrawHLine(u8g2_t* u8g2, int x, int y, int w);
void u8g2_DrawVLine(u8g2_t* u8g2, int x, int y, int h);
void u8g2_DrawBox(u8g2_t* u8g2, int x, int y, int w, int h);
void u8g2_DrawFrame(u8g2_t* u8g2, int x, int y, int w, int h);
// MSB-first 1-bit 位图（u8g2 DrawBitmap 语义）
void u8g2_DrawBitmap(u8g2_t* u8g2, int x, int y, int cnt, int h, const uint8_t* bitmap);
// LSB-first 1-bit 位图（u8g2 DrawXBM 语义）
void u8g2_DrawXBM(u8g2_t* u8g2, int x, int y, int w, int h, const uint8_t* bitmap);

#ifdef __cplusplus
}
#endif
