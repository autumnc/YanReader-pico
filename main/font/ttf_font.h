/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 可变 TTF 字形缓存：卡上字体或内置子集，按字重光栅化后画到 framebuffer。
 *
 * Variable TTF glyph cache: SD fonts or the built-in subset, rasterized
 * at the current weight onto the framebuffer.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// ⚠ 本头**刻意不 include epdiy.h**（架构整改 P1.3）。理由：
//
// 1) epdiy.h 连带 epd_internals.h，其中 typedef 的 EpdGlyph / EpdUnicodeInterval /
//    EpdFont 与 crossmux 的 EpdFontData.h / EpdFont.h（EpdFont 是 class）在同一 TU
//    里撞名 —— 阅读模式的 TU 一个都不能碰 epdiy.h。
// 2) 而 ttf_* 是**跨组件契约**：main 的界面层和 crossmux 的 GfxRenderer.cpp 都要声明它。
//    以前它拉 epdiy.h，于是每个"不能吃 epdiy"的 TU 只能手抄一份接口
//    （screen_reader.cpp 曾整段照抄，连 ttf_font_item_t 都字段级复制）。
// 现在只剩 stdint / stdbool / esp_err 三个轻量依赖，任何 TU 都能直接包含。
//
// 唯一曾经需要 epdiy 的是下面几个原型里的 `enum EpdFontFlags align` 参数 —— 已改成
// `int`，见 ttf_draw_text_px 上方的说明。

#ifdef __cplusplus
extern "C" {
#endif

#define TTF_SIZE_SMALL 0
#define TTF_SIZE_LARGE 1

// ── 角色(role)：同一时刻只能有一个「当前字面(face)」在绘制 ──────────────
// CONTENT = 用户所选字体，画用户读/写/打的字；**界面文本默认也走这一面**
//           （见 font_renderer.h 的 g_font）。没装外置字体时它等于内置字体。
// UI      = 恒为内置 builtin.ttf。保留它是为了两件事：内置/用户字体并存的回退面
//           （见下）与"必须固定用内置字体"的少数界面（虚拟键盘）。
// CONTENT_ALT = 书内 CSS 里**第二个家族**的字面（祖堂集：正文宋体 st、注文/引文仿宋 fs）。
// 它和 CONTENT 同侧（画的都是书里的字），只是另带一份字面与字形缓存；没打开时
// ttf_set_role(CONTENT_ALT) 静默退回 CONTENT，所以不必到处判空。
// 绘制前必须选面：ttf_set_role() 只改一个指针，O(1)。见 ttf_font.c 中 s_cur 的说明。
// **缺字自动替补**：某个面画不出一个码点时，绘制/度量会自动去别的面取那个字形
// （正文面 ↔ 内置面互补，见 fallback_role_for），所以调用方不必为"用户的字体没这个
// 字"写任何判断 —— 它只影响字形从哪来，不影响步进与缓存归属。
#define TTF_ROLE_CONTENT 0
#define TTF_ROLE_UI      1
#define TTF_ROLE_CONTENT_ALT 2
#define TTF_ROLE_COUNT   3

#define TTF_FONT_MAX 24
#define TTF_FONT_NAME_MAX 64
#define TTF_FONT_PATH_MAX 160

typedef struct {
    char name[TTF_FONT_NAME_MAX];
    char path[TTF_FONT_PATH_MAX];
} ttf_font_item_t;

#define TTF_FONT_BUILTIN "builtin"

/// 灰阶字覆盖率伽马。小于 1 抬中间覆盖率，抗锯齿边缘更深；满墨仍是 0。
/// Coverage gamma for gray glyphs. Below 1 lifts mid coverage so AA edges are darker; full ink stays 0.
#ifndef TTF_COVER_GAMMA
#define TTF_COVER_GAMMA 0.6f
#endif

esp_err_t ttf_font_init(void);
esp_err_t ttf_font_open(const char* path);
esp_err_t ttf_font_open_builtin(void);
/// 打开/关闭"次字面"（书内 CSS 的第二个家族，画注文/引文那一路字）。
/// 不开时 ttf_set_role(TTF_ROLE_CONTENT_ALT) 就是内容面，绘制力零变化。
/// 关闭即把它占的 PSRAM（IO 块缓存 + 字形缓存 + loca）全部还回去 —— 换书/退出阅读
/// 必须调，否则上一本书的仿宋会一直占着 ~1MB。
esp_err_t ttf_font_open_alt(const char* path);
/// 把**内嵌的 Yan Reader 标志字体**（Noto Serif CJK SC 子集，仅标志用字）装进次字面
/// (CONTENT_ALT)，供开机动画使用。装不上不是错误 —— 调用方退化为清屏即可。
///
/// ★ 画完必须立刻 ttf_font_close_alt()：次字面是阅读器留给"本书第二个字体家族"的
/// 槽（见 TTF_ROLE_CONTENT_ALT 的说明），标志字体留在里面会让**没有次家族的书**
/// 的注文/引文顶着标志字体渲染。
esp_err_t ttf_font_open_logo(void);
void ttf_font_close_alt(void);
bool ttf_font_alt_ready(void);
/// 「SD 上的字体文件在接下来这段时间不可用」：true = 内容面切内建、关掉次字面、并拒绝
/// 任何按路径重开；false = 解除禁令（**恢复用户字体要由调用方自己按设置重开**，这里
/// 不替它记路径，见 screen_reader.cpp 的 applyUserContentFont()）。
///
/// 两个用途，本质是同一件事 —— 别让字体面攥着一条已经失效的 SD 句柄：
///   1) 控制任务独占传字体（整卡在写）；
///   2) U 盘模式：SD 整卡交给电脑（usb_msc_run 会卸载再重挂），前后必须各调一次，
///      否则退出后 fatfs 实例换了新挂载，旧的 fd 仍在，整机掉字。
esp_err_t ttf_font_suspend_sd(bool suspend);
/// 内容面**就是**内置字体（= 用户没选外置字体）。此时内置面根本不会被单独加载
/// （ttf_set_role(UI) 会直接映射到内容面），一切与"只有一份字体"时完全一致。
/// 绘制层用它决定"要不要走用户字体的字形"（见 font_renderer.cpp 的 ASCII 分支）。
bool ttf_font_is_builtin(void);
bool ttf_font_path_is_builtin(const char* path);
void ttf_font_unload(void);
/// 选定后续 ttf_* 绘制/度量作用于哪个字面。UI 面未装好时静默退回内容面
/// (内容面就是内置字体时二者等价，此时 UI 面根本不会被单独加载)。
void ttf_set_role(int role);
int ttf_get_role(void);
int ttf_font_scan(void);
int ttf_font_count(void);
const ttf_font_item_t* ttf_font_item(int index);
const char* ttf_font_path(void);
const char* ttf_font_display_name(void);
bool ttf_font_ready(void);
int ttf_ascender(int size);
int ttf_ascender_px(int pixel_height);

/// 水平对齐标志。**值刻意与 epdiy 的 EPD_DRAW_ALIGN_*** 相同（LEFT=0x2 /
/// RIGHT=0x4 / CENTER=0x8，另有 BACKGROUND=0x1 本层用不到），但名字是这一层自己的：
/// epdiy 的 enum EpdFontFlags 归 epdiy 所有，本头不能 include 它、也不能重定义一份
/// （同名的第二个定义会在同时见到 epd_internals.h 的 TU 里撞重定义）。于是 ttf_draw_*
/// 的 align 形参是 int，调用方传这三个之一；数值一致，ABI 与语义零变化。
enum TtfDrawAlign {
    TTF_ALIGN_LEFT   = 0x2,
    TTF_ALIGN_RIGHT  = 0x4,
    TTF_ALIGN_CENTER = 0x8,
};

void ttf_draw_text(
    uint8_t* framebuffer, int x, int y, int size, const char* text,
    int align, uint8_t fg, uint8_t bg
);

void ttf_draw_text_px(
    uint8_t* framebuffer, int x, int y, int pixel_height, const char* text,
    int align, uint8_t fg, uint8_t bg
);
/// 覆盖率过半才落墨，像素只有 fg/bg。/ Ink only when coverage is over half; pixels are fg/bg only.
void ttf_draw_text_px_bw(
    uint8_t* framebuffer, int x, int y, int pixel_height, const char* text,
    int align, uint8_t fg, uint8_t bg
);

/// 把整段文本用到的字形块一次性预取到 PSRAM 块缓存里，随后逐词绘制不再碰 SD。
/// 逐词预取只能看见下一个词的几个字，跨整页排序才能把随机小读并成顺序大读 ——
/// 这就是它存在的理由。须在 ttf_set_role() 选好字面之后调用。
/// 纯优化：内建字体/已整表映射时直接返回，不分配任何内存。
void ttf_warm_text_px(int pixel_height, const char* text);

void ttf_measure_line(int size, const char* text, int* above, int* below);
void ttf_measure_line_px(int pixel_height, const char* text, int* above, int* below);
int ttf_text_width_px(int pixel_height, const char* text);
/// 单码点步进(px)，与 ttf_text_width_px 同一个实现：ASCII(<0x80) 走**每字面 128 项
/// 缓存表**（见 ttf_font.c 的 ascii_adv_ensure），其余码点走通用度量。与
/// ttf_text_width_px 逐值相等是构造保证，不是两处对齐。
///
/// role 必须显式给（不用 s_cur）：FontRenderer::setSize()/setGridPx() 量完 ascent 会
/// 把当前面留在 UI 上且不还原，所以"当前面"不是可靠输入。给 -1 会落到内容面。
int ttf_char_advance_px(int role, int pixel_height, uint32_t codepoint);
void ttf_set_weight(int wght);
int ttf_get_weight(void);

/// 正文墨色（排版设定里的「字重」与「对比度」）。**只在正文渲染期间打开**：调用方画书页
/// 正文的前后 begin/end 一次（见 screen_reader.cpp 的 RdBodyInkScope）。画在作用域之外的
/// 界面外壳（菜单/状态栏/列表/词典）仍旧按默认墨色画，所以调它不会把本机界面一起改了。
///
/// dw    = CSS 字重增量（**只加粗**，0..+200 左右；0 = 原样）。字体自带 wght 轴
///         （fvar/gvar）时走**真实变体**；没有（内置字体、普通静态字体）就**合成**：
///         按 em 比例膨胀笔画（r = px·dw/12800 = δ/2，与 KOReader/crengine 的
///         FT_Outline_Embolden 同口径）。**不做变细**：stb 没有轮廓腐蚀的公开 API，
///         硬做只会把细笔画啃掉。
/// gamma = 覆盖率的曲线 cover' = 255·(cover/255)^gamma，默认 0.6（= TTF_COVER_GAMMA）：
///         小 → 抗锯齿边缘更深（柔、墨重）；大 → 更浅（锐、干净）。
///
/// 两者都**不改步进与度量**（见 ttf_font.c 里 cp_advance 那段说明）：改它们绝不重排正文，
/// 也不会让"量的"与"画的"分家。
void ttf_body_ink_begin(int dw, float gamma);
void ttf_body_ink_end(void);

void ttf_font_cache_clear(void);

#ifdef __cplusplus
}
#endif
