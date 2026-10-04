/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 错相揭页引擎实现（移植自 wegooo-cell/read-pico-reader）。
 * 16 带、每像素走完一整条相位梯子；差分只算一次，每拍只换对应相位的 1K LUT。
 * 梯子由调用方给（见 e0470_page_turn_ex）：默认表 GL16 37 相（精度高、~0.73s），
 * 跟随表 DU 8 相（正文页用，~0.29s）。
 */

#include "e0470_page_turn.h"

#include <stdbool.h>
#include <string.h>

#include "e0470_epaper_waveform.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "epd_waveform.h"

static const char* TAG = "e0470_turn";

#define TURN_BANDS 16
#define TURN_PHASE_CAP 40
#define TURN_LINE_MAX 2048
// 拍长自适应的天花板：见下面 s_tick_us 的说明。
#define TURN_TICK_MAX_US 12000

// 相邻两条带**开相**之间隔多少拍。1 = 每条带紧接着前一条开相（原版）：第 15 拍起
// 16 条带同时"在相里"，整幅宽度都脏，一条扫描线要喂 16 个不同的 1K LUT。
// 而喂数线程的实测吞吐约 78 Mpx/s（见下），18MHz/fast 一行 10us 只够 ~640 像素 ——
// 全宽 1216 像素时生产速度只有消费的 2/3，队列 62 槽排空后就是 EPD_DRAW_EMPTY_LINE_QUEUE，
// 动画永远走不完（日志里 err=1024 每次都出现在"在相带宽到某个程度"的那一拍）。
//
// 于是 stride 是"在相带数"与"总拍数"之间的唯一旋钮：
//   在相带数 = ceil(phases/stride)，脏宽度 = 在相带数/bands；喂数要在这段时间里把
//   这么多像素推完（整屏喂完约 10.7ms，一整帧约 7ms，所以脏宽度超过 ~65% 就追不上）。
//   总拍数   = (bands-1)*stride + phases，而每拍至少一整帧扫描（~7ms），所以拍数就是墙钟时间。
// 把"脏宽 ≤ 65%"这条约束代进去就只剩一个变量：stride ≥ phases/10.4（TURN_BANDS=16），
//   总拍数 = 15·stride + phases ≈ (15/10.4 + 1)·phases ≈ 2.5·phases。
//   **所以真正的调速杠杆是梯子的相数，不是 stride** —— stride 只是把"波"摊到那条
//   约束线上，调大调小都换不回墙钟时间（大 stride = 拍数多但每拍干净，小 stride 反过来）。
//   短梯（跟随表 DU，8 相）总拍数 ≈ 20、长梯（默认表 GL16，37 相）≈ 95，差 4.7 倍。
//   见头文件 e0470_page_turn_ex()：正文页用短梯，图片页用长梯。
// 两种梯子各记一份 stride：它们对喂数的要求差 4 倍多，共用一份会让短梯的一次失败
// 把长梯也一起拖慢。只往大里调（见下面出错分支），一次失败换回整段会话的稳定，
// 避免这一帧好下一帧坏地抖。
#define TURN_BAND_STRIDE 4         // 长梯默认：脏宽 10/16 = 62%（65% 线内留一点给 UI/WiFi）
#define TURN_BAND_STRIDE_SAFE 6    // 长梯退守：39%
#define TURN_BAND_STRIDE_SHORT 2   // 短梯默认：脏宽 4/16 = 25%，拍数 15*2+8 = 38 → ~0.29s
#define TURN_BAND_STRIDE_SHORT_SAFE 4  // 短梯退守：2/16
static int s_stride_long = TURN_BAND_STRIDE;
static int s_stride_short = TURN_BAND_STRIDE_SHORT;

// ---- 本地"空"波形：1 相、全 0 表 -------------------------------------------------
// 错相揭页每拍只要**一相**扫描（相位由 epd_set_*_phase_luts 逐行/逐带选），波形本身
// 不提供任何动作，全部靠选进来的 1K LUT。所以这里自己造一张"1 相、全保持"的表，
// 挂在 MODE_DU 上（get_waveform_index 按 `mode & 0x3F` 匹配，MODE_DU == 1）。
// 之所以定义在本组件而不是共享的 e0470_epaper_waveform 里：那张表对其它刷新路径没有
// 意义，放这里就不会为了一个内部实现细节去改被两个工程共用的组件。
static uint8_t e0470_apply_data[1][16][4];
static const EpdWaveformPhases e0470_apply_phases = {
    .phases = 1,
    .luts = (const uint8_t*)&e0470_apply_data[0],
    .phase_times = NULL,
};
static const EpdWaveformPhases* e0470_apply_ranges[] = { &e0470_apply_phases };
static const EpdWaveformMode e0470_apply_mode = {
    .type = MODE_DU,
    .temp_ranges = 1,
    .range_data = &e0470_apply_ranges[0],
};
static const EpdWaveformMode* e0470_apply_modes[] = { &e0470_apply_mode };
static const EpdWaveformTempInterval e0470_apply_intervals[] = {
    { .min = 0, .max = 50 },
};
static const EpdWaveform E0470_APPLY_WAVEFORM = {
    .num_modes = 1,
    .num_temp_ranges = 1,
    .mode_data = e0470_apply_modes,
    .temp_intervals = e0470_apply_intervals,
};

// 相位表按需分配在 PSRAM：37KiB 常驻内部 RAM 会挤压 WiFi/USB 需要的连续块。
static uint8_t (*s_lut)[1024];
static const uint8_t* s_lut_ptr[TURN_PHASE_CAP];
static int s_band0[TURN_BANDS];
static int s_band1[TURN_BANDS];
static int8_t s_band_phase[TURN_BANDS];
// 逐行相位必须能被 DMA 侧读，放内部 RAM。负值 = 该行本次保持。
static int8_t s_line_phase[TURN_LINE_MAX] DRAM_ATTR;
static const EpdWaveformPhases* s_lut_src;
static int s_lut_n;
// 拍长下限的**配置值**（e0470_page_turn_set_tick_us 可改）。每帧的节拍从这里起算，
// 只在该帧内往上抬（见 e0470_page_turn 里的 tick_target）—— 不跨帧累积：一次偶发
// 抖动（WiFi/UI 抢核）不该让后面每一次翻页都跟着变慢。
static int s_tick_us = E0470_TURN_DEFAULT_TICK_US;

void e0470_page_turn_release(void) {
    heap_caps_free(s_lut);
    s_lut = NULL;
    s_lut_src = NULL;
    s_lut_n = 0;
}

const char* e0470_turn_dir_name(e0470_turn_dir_t dir) {
    switch (dir) {
        case E0470_TURN_LTR: return "ltr";
        case E0470_TURN_RTL: return "rtl";
        case E0470_TURN_TTB: return "ttb";
        case E0470_TURN_BTT: return "btt";
        default: return "?";
    }
}

void e0470_page_turn_set_tick_us(int us) {
    if (us < 0) us = 0;
    s_tick_us = us;
}

int e0470_page_turn_tick_us(void) {
    return s_tick_us;
}

static void clip_rect(EpdRect* r, int w, int h) {
    if (r->x < 0) {
        r->width += r->x;
        r->x = 0;
    }
    if (r->y < 0) {
        r->height += r->y;
        r->y = 0;
    }
    if (r->x + r->width > w) r->width = w - r->x;
    if (r->y + r->height > h) r->height = h - r->y;
}

/// 逻辑矩形 → framebuffer 矩形。注意 epd_width()/epd_height() 是**物理**尺寸，
/// 与 epd_set_rotation() 无关（epdiy 的 rotation 只存枚举，不换宽高），所以这里
/// 直接用物理尺寸做映射，与 highlevel.c 的 _inverse_rotated_area() 同源。
static EpdRect rotate_to_fb(EpdRect rect) {
    const int pw = epd_width();
    const int ph = epd_height();
    const int lx0 = rect.x;
    const int ly0 = rect.y;
    const int lx1 = rect.x + rect.width - 1;
    const int ly1 = rect.y + rect.height - 1;
    int ax, ay, bx, by;

    switch (epd_get_rotation()) {
        case EPD_ROT_LANDSCAPE:
            ax = lx0;
            ay = ly0;
            bx = lx1;
            by = ly1;
            break;
        case EPD_ROT_PORTRAIT:
            ax = pw - ly0 - 1;
            ay = lx0;
            bx = pw - ly1 - 1;
            by = lx1;
            break;
        case EPD_ROT_INVERTED_LANDSCAPE:
            ax = pw - lx0 - 1;
            ay = ph - ly0 - 1;
            bx = pw - lx1 - 1;
            by = ph - ly1 - 1;
            break;
        default:  // EPD_ROT_INVERTED_PORTRAIT
            ax = ly0;
            ay = ph - lx0 - 1;
            bx = ly1;
            by = ph - lx1 - 1;
            break;
    }

    const int x0 = ax < bx ? ax : bx;
    const int x1 = ax < bx ? bx : ax;
    const int y0 = ay < by ? ay : by;
    const int y1 = ay < by ? by : ay;
    return (EpdRect){ .x = x0, .y = y0, .width = x1 - x0 + 1, .height = y1 - y0 + 1 };
}

static void phys_to_logical(int px, int py, int* lx, int* ly) {
    const int pw = epd_width();
    const int ph = epd_height();
    switch (epd_get_rotation()) {
        case EPD_ROT_LANDSCAPE:
            *lx = px;
            *ly = py;
            break;
        case EPD_ROT_PORTRAIT:
            *lx = py;
            *ly = pw - px - 1;
            break;
        case EPD_ROT_INVERTED_LANDSCAPE:
            *lx = pw - px - 1;
            *ly = ph - py - 1;
            break;
        default:
            *lx = ph - py - 1;
            *ly = px;
            break;
    }
}

static bool dir_is_lr(e0470_turn_dir_t dir) {
    return dir == E0470_TURN_LTR || dir == E0470_TURN_RTL;
}

static bool dir_is_reverse(e0470_turn_dir_t dir) {
    return dir == E0470_TURN_RTL || dir == E0470_TURN_BTT;
}

/// 错相轴落在物理 y 上 → 整行共用一个相位（逐行相位表）；
/// 落在物理 x 上 → 一行内还要按列分段（列条带表）。
static bool uses_line_phase(e0470_turn_dir_t dir) {
    const bool lr = dir_is_lr(dir);
    switch (epd_get_rotation()) {
        case EPD_ROT_LANDSCAPE:
        case EPD_ROT_INVERTED_LANDSCAPE:
            return !lr;
        default:
            return lr;
    }
}

static int band_of(int coord, int origin, int span, int bands, bool reverse) {
    int band = (coord - origin) * bands / span;
    if (band < 0) band = 0;
    if (band >= bands) band = bands - 1;
    return reverse ? bands - 1 - band : band;
}

static void clear_bands(int* b0, int* b1, int n) {
    for (int i = 0; i < n; i++) {
        b0[i] = 0;
        b1[i] = 0;
    }
}

/// 沿错相轴走一遍，把连续同带的行程合并成 [b0, b1) 物理区间。
static void assign_bands(
    int* b0,
    int* b1,
    int bands,
    int from,
    int to,
    int step,
    bool along_x,
    int origin,
    int span,
    bool lr,
    bool reverse
) {
    clear_bands(b0, b1, bands);
    int cur = -1;
    int run = from;
    for (int p = from; p < to; p += step) {
        int lx, ly;
        if (along_x) phys_to_logical(p, 0, &lx, &ly);
        else phys_to_logical(0, p, &lx, &ly);
        const int coord = lr ? lx : ly;
        const int band = band_of(coord, origin, span, bands, reverse);
        if (band != cur) {
            if (cur >= 0) {
                b0[cur] = run;
                b1[cur] = p;
            }
            cur = band;
            run = p;
        }
    }
    if (cur >= 0) {
        b0[cur] = run;
        b1[cur] = to;
    }
}

static bool build_luts(const EpdWaveformPhases* gl) {
    if (!s_lut) {
        s_lut = heap_caps_aligned_alloc(
            16, sizeof(uint8_t[TURN_PHASE_CAP][1024]), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (!s_lut) return false;
    }
    if (s_lut_src == gl && s_lut_n == gl->phases) return true;
    const int n = gl->phases < TURN_PHASE_CAP ? gl->phases : TURN_PHASE_CAP;
    for (int phase = 0; phase < n; phase++) {
        epd_build_1ppB_lut_1k(s_lut[phase], gl, phase);
        s_lut_ptr[phase] = s_lut[phase];
    }
    s_lut_src = gl;
    s_lut_n = gl->phases;
    return true;
}

/// 把 front_fb 的 phys 区域抄进 back_fb —— 本次揭页已经把该区域驱动到 front 的内容，
/// 基准必须跟上，否则下一次差分会把这段重新算成"要变"。
/// 与 highlevel.c 的回写同一套半字节边界处理。
static void copy_front_to_back(EpdiyHighlevelState* hl, EpdRect phys) {
    const int w = epd_width();
    const int h = epd_height();
    clip_rect(&phys, w, h);
    if (phys.width <= 0 || phys.height <= 0) return;

    for (int y = phys.y; y < phys.y + phys.height; y++) {
        const uint8_t* src = hl->front_fb + y * w / 2;
        uint8_t* dst = hl->back_fb + y * w / 2;
        int x = phys.x;
        int x_last = phys.x + phys.width - 1;
        if (x & 1) {
            dst[x / 2] = (uint8_t)((src[x / 2] & 0xF0) | (dst[x / 2] & 0x0F));
            x++;
        }
        if ((x_last & 1) == 0) {
            dst[x_last / 2] = (uint8_t)((src[x_last / 2] & 0x0F) | (dst[x_last / 2] & 0xF0));
            x_last--;
        }
        if (x_last >= x) memcpy(dst + x / 2, src + x / 2, (size_t)((x_last - x + 1) / 2));
    }
}

static enum EpdDrawError turn_impl(
    EpdiyHighlevelState* hl, EpdRect area, e0470_turn_dir_t dir, const EpdWaveform* waveform, int mode
) {
    if (hl == NULL || waveform == NULL) return EPD_DRAW_NO_PHASES_AVAILABLE;
    if (dir > E0470_TURN_BTT) dir = E0470_TURN_RTL;

    const EpdWaveformPhases* gl = e0470_waveform_phases(waveform, mode);
    if (gl == NULL || gl->luts == NULL || gl->phases <= 0 || gl->phases > TURN_PHASE_CAP) {
        return EPD_DRAW_NO_PHASES_AVAILABLE;
    }

    // 短梯 / 长梯各一份 stride（见 TURN_BAND_STRIDE 的说明）。
    const bool short_ladder = (waveform == &E0470_FOLLOW_WAVEFORM);
    int* const stride_slot = short_ladder ? &s_stride_short : &s_stride_long;

    const int bands = TURN_BANDS;
    const int stride = *stride_slot;
    const int nphase = gl->phases;
    // 最后一条带在 (bands-1)*stride 拍开相，之后还要 nphase-1 拍把它走完。
    // stride=1 时就退化成原来的 bands + nphase - 1。
    const int ticks = (bands - 1) * stride + nphase;
    const int fb_w = epd_width();
    const int fb_h = epd_height();
    if (fb_h > TURN_LINE_MAX) return EPD_DRAW_INVALID_CROP;

    EpdRect phys = rotate_to_fb(area);
    clip_rect(&phys, fb_w, fb_h);
    if (phys.width <= 0 || phys.height <= 0) return EPD_DRAW_SUCCESS;

    const int y0 = phys.y;
    const int y1 = phys.y + phys.height;
    const bool lr = dir_is_lr(dir);
    const bool reverse = dir_is_reverse(dir);
    const bool line_ph = uses_line_phase(dir);
    const int origin = lr ? area.x : area.y;
    const int span = lr ? (area.width > 0 ? area.width : 1) : (area.height > 0 ? area.height : 1);

    if (line_ph) {
        assign_bands(s_band0, s_band1, bands, y0, y1, 1, false, origin, span, lr, reverse);
    } else {
        // 列条带必须 16 像素对齐（查表按 16 像素一个 uint32）。
        const int x_lo = (phys.x + 15) & ~15;
        const int x_hi = (phys.x + phys.width) & ~15;
        assign_bands(s_band0, s_band1, bands, x_lo, x_hi, 16, true, origin, span, lr, reverse);
    }
    // 16 条带必须都分到非空区间，否则"波"会缺段，画面留下没驱动的带。宁可放弃动画。
    int assigned = 0;
    for (int band = 0; band < bands; ++band) assigned += s_band0[band] < s_band1[band];
    if (assigned != bands) return EPD_DRAW_INVALID_CROP;

    if (!build_luts(gl)) return EPD_DRAW_NO_PHASES_AVAILABLE;
    epd_poweron();

    const int64_t t_all = esp_timer_get_time();
    const int64_t t_gen = esp_timer_get_time();
    epd_difference_image_cropped(
        hl->front_fb, hl->back_fb, phys, hl->difference_fb, hl->dirty_lines, hl->dirty_columns
    );
    const int64_t gen_us = esp_timer_get_time() - t_gen;

    int64_t draw_us = 0;
    int64_t tick_max_us = 0;
    int tick_target = s_tick_us;   // 本帧的节拍：从配置值起，帧内只往上抬
    int ticks_ran = 0;
    enum EpdDrawError err = EPD_DRAW_SUCCESS;
    for (int tick = 0; tick < ticks; tick++) {
        const int64_t t_tick = esp_timer_get_time();
        ticks_ran = tick + 1;
        memset(hl->dirty_lines, 0, sizeof(bool) * (size_t)fb_h);
        memset(hl->dirty_columns, 0, (size_t)fb_w / 2);
        memset(s_band_phase, -1, (size_t)bands);
        if (line_ph) memset(s_line_phase, -1, (size_t)fb_h);

        // 这一拍有哪些带正好走到"还没跑完"的相位。
        bool any = false;
        for (int band = 0; band < bands; band++) {
            const int phase = tick - band * stride;
            if (phase < 0 || phase >= nphase) continue;
            const int a = s_band0[band];
            const int b = s_band1[band];
            if (a >= b) continue;
            s_band_phase[band] = (int8_t)phase;
            if (line_ph) {
                memset(hl->dirty_lines + a, 1, (size_t)(b - a));
                memset(s_line_phase + a, (int)(int8_t)phase, (size_t)(b - a));
            } else {
                memset(hl->dirty_columns + a / 2, 0xFF, (size_t)((b - a) / 2));
            }
            any = true;
        }
        if (!any) continue;

        if (line_ph) {
            // 逐行相位版：整个 area 的行都要参与扫描（没分到带的行走负相位=保持），
            // 列方向则整宽都脏，否则差分裁剪会把保持的行整段跳过。
            const int xs = phys.x & ~1;
            const int xe = (phys.x + phys.width + 1) & ~1;
            if (xe > xs) {
                memset(hl->dirty_columns + xs / 2, 0xFF, (size_t)((xe - xs) / 2));
            }
            epd_set_line_phase_luts(s_lut_ptr, s_line_phase);
        } else {
            memset(hl->dirty_lines + y0, 1, (size_t)(y1 - y0));
            epd_set_col_phase_luts(s_lut_ptr, s_band0, s_band1, s_band_phase, bands);
        }

        const int64_t t_draw = esp_timer_get_time();
        err = epd_draw_base(
            epd_full_screen(),
            hl->difference_fb,
            epd_full_screen(),
            MODE_PACKING_1PPB_DIFFERENCE | MODE_DU,
            25,
            hl->dirty_lines,
            hl->dirty_columns,
            &E0470_APPLY_WAVEFORM
        );
        epd_clear_phase_luts();
        draw_us += esp_timer_get_time() - t_draw;
        // 补足到目标拍长，让"波"扫得均匀；扫描本身更慢时就以扫描为准（不补负值）。
        const int64_t used = esp_timer_get_time() - t_tick;
        if (used > tick_max_us) tick_max_us = used;
        // 拍长自适应（见头文件）：这一拍比下限还慢，说明喂数/调度在这段吃紧，把下限
        // 抬到它 +0.2ms —— 后面每拍都按这个节拍走，"波"的速度才匀。第 0、1 拍不采样：
        // 那两拍里混着相位 LUT 构建与差分生成的一次性开销，按它们定节拍会把整段动画
        // 拖成龟速。上限 12ms：一次异常（WiFi/UI 抢核）不该传染给后面每一次翻页。
        if (tick >= 2 && used > tick_target && used <= TURN_TICK_MAX_US) {
            tick_target = (int)used + 200;
        }
        if (err != EPD_DRAW_SUCCESS) {
            // 喂数跟不上时面板会报空行队列不够。把 stride 调大（脏带更窄）就稳了
            // （见 TURN_BAND_STRIDE 的说明）：只往大里调，一次失败换回整段会话的稳定。
            // 这一次的收尾交给调用方 —— 它会退回本档位该走的普通刷新，屏幕照样拿到新页面。
            const int fallback = short_ladder ? TURN_BAND_STRIDE_SHORT_SAFE : TURN_BAND_STRIDE_SAFE;
            if ((err & EPD_DRAW_EMPTY_LINE_QUEUE) != 0 && *stride_slot < fallback) {
                ESP_LOGW(TAG, "喂数跟不上（%s stride=%d），退回 %d",
                         short_ladder ? "短梯" : "长梯", *stride_slot, fallback);
                *stride_slot = fallback;
            }
            break;
        }
        if (tick_target > 0 && used < tick_target) {
            esp_rom_delay_us((uint32_t)(tick_target - used));
        }
    }

    // 扫描失败时屏幕可能停在中间相位，保留旧帧基准给调用方恢复。
    if (err == EPD_DRAW_SUCCESS) copy_front_to_back(hl, phys);
    ESP_LOGI(
        TAG,
        "dir=%s 梯=%s(%d相) stride=%d ticks=%d/%d gen=%d ms draw=%d ms scan_avg=%d us "
        "scan_max=%d us tick=%d us wall=%d ms err=%d",
        e0470_turn_dir_name(dir),
        short_ladder ? "跟随DU" : "GL16",
        nphase,
        stride,
        ticks_ran,
        ticks,
        (int)(gen_us / 1000),
        (int)(draw_us / 1000),
        ticks_ran ? (int)(draw_us / ticks_ran) : 0,
        (int)tick_max_us,
        tick_target,
        (int)((esp_timer_get_time() - t_all) / 1000),
        (int)err
    );
    return err;
}

enum EpdDrawError e0470_page_turn(EpdiyHighlevelState* hl, EpdRect area, e0470_turn_dir_t dir) {
    return turn_impl(hl, area, dir, &E0470_WAVEFORM, MODE_GL16);
}

enum EpdDrawError e0470_page_turn_ex(
    EpdiyHighlevelState* hl, EpdRect area, e0470_turn_dir_t dir, const EpdWaveform* waveform, int mode
) {
    return turn_impl(hl, area, dir, waveform, mode);
}
