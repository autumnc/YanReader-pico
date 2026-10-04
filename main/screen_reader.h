#pragma once

#include <string>

#include "pjournal_app.h"
//
// 用 read_pico 官方固件的 ui 库与底层驱动（epdiy 4bpp framebuffer + ttf_font），
// 逻辑层复用 crossmux 的 lib/（Epub/Txt/Xtc/Section/Page/GfxRenderer/HalDisplay/
// HalStorage/Dictionary）。本模块只重建阅读器的 UI 外壳：书架、翻页、目录、
// 阅读设定、词典、虚拟键盘（接系统拼音输入法）。
//
// 注意：本 TU 不能 #include read_pico 的 epdiy.h —— 它会连带 epd_internals.h，
// 其中 typedef 的 EpdGlyph/EpdUnicodeInterval/EpdFont 与 crossmux 的
// EpdFontData.h/EpdFont.h（EpdFont 是 class）冲突。逻辑尺寸用 HalDisplay 的
// getDisplayWidth/Height（内部已调 epd_rotated_*），字体度量经 GfxRenderer 取得。

void screen_reader_init();
void screen_reader_exit();
AppState screen_reader_handle(int key, ScreenContext &ctx);

// ── 待机表盘「书籍封面」的两个出口 ───────────────────────────────────────
// 只有 screen_reader.cpp 知道进度表、封面缓存路径的算法和封面解码器，所以这两件事
// 都由它出面。做成"取数"而不是"绘制"：待机表盘画在 ui_render 的工作缓冲上，而这里
// 能用的画布只有 g_rd（绑在 front_fb 上，见 ui_render.cpp 里那条不变式）。

// 最后阅读的一本书：reader_progress.txt 的第一条（该文件**最新在前**，翻开一本书或
// 翻页都会把当前书挪到表头）。没有记录（没读过书 / 卡没插）返回 false。
// coverBmp = 该书封面 BMP 的缓存路径（**可能还没生成**：书没打开过就没有封面文件，
// 调用方自己判存在）；title / percent = 书名与阅读进度百分比，取不到时留空 / 为 0。
bool readerLastBookCover(std::string &coverBmp, std::string &title, int &percent);

// 待机封面的框：整屏减去短边 2.5% 的一圈边距（面板本身还有 3~4px 盖边，留一点就够，
// 剩下的全给封面）。生成端（generateStandbyCoverForOpenedBook）和解码端（待机表盘的
// drawCoverFace）必须用**同一个框**，否则 1:1 直拷那条快路径永远命中不了，而且缓存
// 文件的尺寸会跟画出来的框对不上。所以框的计算只有这一份，两边都调它。
// / The standby cover box: the screen minus 2.5% of the short edge. Both the generator
// and the decoder must use the same box, or the 1:1 fast path never hits.
void readerStandbyCoverBox(int &x, int &y, int &w, int &h);

// 把封面按"装进 boxW×boxH"缩放成 0..15 灰度（15=白）写进调用方给的 out，并回吐实际
// 尺寸 dw/dh 与在框内的居中偏移 ox/oy。out 至少要 boxW*boxH 字节 —— 一张整屏封面约
// 300KB，**必须取自 PSRAM**（内部 RAM 挤不出这么大一块，见 internal-ram-squeeze）。
// **只缩不放**：源图比框小的那一边按原尺寸来（dw/dh 就是源尺寸），多出来的框留白。
// 只读只算不画，像素由调用方自己 blit。
bool readerCoverScale(const std::string &bmpPath, int boxW, int boxH, uint8_t *out, size_t outCap,
                      int &dw, int &dh, int &ox, int &oy);
