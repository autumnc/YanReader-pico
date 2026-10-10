/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * E0470A01 波形装配：裁剪默认表、8 灰阶、跟随 DU。
 * 自行调整屏幕波形会使设备失去保修。
 *
 * E0470A01 waveform assembly: trimmed default, 8-gray, follow DU.
 * Changing panel waveforms voids the warranty.
 */

#include "e0470_epaper_waveform.h"

#include <assert.h>
#include <string.h>

#include "esp_log.h"

#include "e0470_waveform_trim.h"
#include "du.h"
#include "gc16.h"
#include "gl16.h"
#include "gray8_gc16.h"
#include "gray8_gl16.h"

static const char* TAG = "e0470wf";

// 温度档 0-50°C。在此固件中没有温度分档的演示。
// / One 0–50°C temp range. This firmware has no multi-range demo.
static const EpdWaveformTempInterval e0470_intervals[] = {
    { .min = 0, .max = 50 },
};

// 把 (from, to) 的一个 2bit 动作写进 epdiy 的表：data[frame][to][from/4]，高位是 from0。
// / Write one 2-bit (from, to) action into the epdiy table: data[frame][to][from/4], MSB is from0.
static inline void lut_or(uint8_t (*data)[16][4], int f, int to, int from, int action) {
    data[f][to][from / 4] |= (uint8_t)(action << (6 - 2 * (from % 4)));
}

static inline int lut_get(const uint8_t (*data)[16][4], int f, int to, int from) {
    return (data[f][to][from / 4] >> (6 - 2 * (from % 4))) & 3;
}

static inline void lut_set(uint8_t (*data)[16][4], int f, int to, int from, int v) {
    const int shift = 6 - 2 * (from % 4);
    data[f][to][from / 4] = (uint8_t)((data[f][to][from / 4] & ~(3 << shift)) | (v << shift));
}

void e0470_follow_lut_build(int frames, uint8_t (*dst)[16][4]) {
    memset(dst, 0, (size_t)frames * 16 * 4);
    // 两个方向各有自己的满推次数；短表（连续 DU 单帧、连调 dufr n）按表长封顶。
    // / Each direction has its own full-push count; short tables (1-frame
    // continuous DU, live-tune dufr n) cap at the table length.
    const int black_max = frames < E0470_FOLLOW_BLACK_FRAMES ? frames : E0470_FOLLOW_BLACK_FRAMES;
    const int white_max = frames < E0470_FOLLOW_WHITE_FRAMES ? frames : E0470_FOLLOW_WHITE_FRAMES;
    for (int to = 0; to < 16; to++) {
        for (int from = 0; from < 16; from++) {
            if (to == from) continue;
            const int diff = to > from ? to - from : from - to;
            const int action = to > from ? 2 : 1;  // 往白推 0b10，往黑推 0b01 / 0b10 erase, 0b01 darken
            const int budget = action == 2 ? white_max : black_max;
            // 推动次数 = ceil(diff · 满推 / 15)，至少 1：差得远多推，差得近少推。
            // / Push count = ceil(diff · full / 15), at least 1: far travels more, near travels less.
            const int pushes = (diff * budget + 14) / 15;
            for (int f = 0; f < pushes; f++) lut_or(dst, f, to, from, action);
        }
    }
}

/* ---- 跟随 DU：开机按公式生成 / Follow DU: built at boot ---- */
static uint8_t e0470_follow_data[E0470_FOLLOW_FRAMES][16][4];
static const EpdWaveformPhases e0470_follow_phases = {
    .phases = E0470_FOLLOW_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&e0470_follow_data[0],
};
static const EpdWaveformPhases* e0470_follow_ranges[] = { &e0470_follow_phases };
static const EpdWaveformMode e0470_follow_mode = {
    .type = 1,  // MODE_DU / MODE_DU
    .temp_ranges = 1,
    .range_data = &e0470_follow_ranges[0],
};
static const EpdWaveformMode* e0470_follow_modes[] = { &e0470_follow_mode };

const EpdWaveform E0470_FOLLOW_WAVEFORM = {
    .num_modes = 1,
    .num_temp_ranges = 1,
    .mode_data = e0470_follow_modes,
    .temp_intervals = e0470_intervals,
};

/* ---- 阈值 DU / Threshold DU ---- */
// 源表只认目标 0/15。中间灰按 50/50 切开，暗的走整段到黑、亮的走整段到白。
// from 不切片，沿用源表对真实起点的时间序列，上一帧残留的浅墨也会被推到黑或白。
// / Source tables only drive dest 0/15. Mid grays split 50/50: dark runs
// the full path to black, light the full path to white. from is not sliced;
// the source time series for the real start is reused, so leftover ink
// from the last frame is also pushed to black or white.
static uint8_t e0470_complete_du_data[E0470_FULL_DU_FRAMES][16][4];
static const EpdWaveformPhases e0470_complete_du_phases = {
    .phases = E0470_FULL_DU_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&e0470_complete_du_data[0],
};
static const EpdWaveformPhases* e0470_complete_du_ranges[] = {
    &e0470_complete_du_phases,
};
static const EpdWaveformMode e0470_complete_du_mode = {
    .type = 1,
    .temp_ranges = 1,
    .range_data = &e0470_complete_du_ranges[0],
};

static void e0470_complete_du_build(void) {
    memset(e0470_complete_du_data, 0, sizeof(e0470_complete_du_data));
    for (int to = 0; to < 16; to++) {
        const int to_bin = to < 8 ? 0 : 15;
        for (int from = 0; from < 16; from++) {
            for (int f = 0; f < E0470_FULL_DU_FRAMES; f++) {
                const int action = lut_get(e0470_full_du_data, f, to_bin, from);
                if (action != 0) lut_or(e0470_complete_du_data, f, to, from, action);
            }
        }
    }
}

// 白底 15→15 的白推档（实现见文件尾部 e0470_gl16_white_pushes）：开机从这里取默认值。
// / White-push knob for the 15→15 cell; the default is the documented boot value.

/* ---- 完整表 / Full tables ---- */
// DU 20 相，GC16 48 相；GL16 用 RAM 副本以便白底挂白推（档位见 e0470_waveform_set_white_pushes）。
// / DU 20, GC16 48; GL16 uses a RAM copy so the white-bg pushes can be hung on it.
static uint8_t e0470_full_gl16_live[E0470_FULL_GL16_FRAMES][16][4];
static const EpdWaveformPhases e0470_full_gl16_live_phases = {
    .phases = E0470_FULL_GL16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&e0470_full_gl16_live[0],
};
static const EpdWaveformPhases* e0470_full_gl16_live_ranges[] = {
    &e0470_full_gl16_live_phases,
};
static const EpdWaveformMode e0470_full_gl16_live_mode = {
    .type = 5, .temp_ranges = 1, .range_data = &e0470_full_gl16_live_ranges[0],
};
static const EpdWaveformMode* e0470_full_modes[] = {
    &e0470_full_du_mode,
    &e0470_full_gc16_mode,
    &e0470_full_gl16_live_mode,
};

const EpdWaveform E0470_FULL_WAVEFORM = {
    .num_modes = 3,
    .num_temp_ranges = 1,
    .mode_data = e0470_full_modes,
    .temp_intervals = e0470_intervals,
};

/* ---- 8 灰阶表 / 8-gray tables ---- */
// GC16 / GL16 各 30 相，拿灰阶档数换速度。
// / GC16 / GL16 30 phases each; trade gray steps for speed.
static const EpdWaveformMode* e0470_gray8_modes[] = {
    &e0470_complete_du_mode,
    &e0470_gray8_gc16_mode,
    &e0470_gray8_gl16_mode,
};

const EpdWaveform E0470_GRAY8_WAVEFORM = {
    .num_modes = 3,
    .num_temp_ranges = 1,
    .mode_data = e0470_gray8_modes,
    .temp_intervals = e0470_intervals,
};

/* ---- 8 灰阶正文表 / 8-gray text table ---- */
// 8 灰阶 GL16 的 RAM 副本。源表的 15→15 是全保持，这里照 E0470_WAVEFORM 的做法挂上
// 白推，于是它成了"30 相的不闪表"：差分时不变的白像素**只吃白推、不换灰阶**（正文黑
// 白，看不见），把正文行间的白底压一压。正文翻页用它换掉默认的 37 相 GL16，每屏省约 80ms。
// / RAM copy of the 8-gray GL16 table with the white pushes hung on 15→15, the same
// trick E0470_WAVEFORM uses. That makes it a 30-phase non-flashing table for text:
// unchanged white only gets pushed, never re-graded. Replaces the default
// 37-phase GL16 for text page turns, about 80 ms faster per screen.
static uint8_t e0470_gray8_text_data[E0470_GRAY8_GL16_FRAMES][16][4];
static const EpdWaveformPhases e0470_gray8_text_phases = {
    .phases = E0470_GRAY8_GL16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&e0470_gray8_text_data[0],
};
static const EpdWaveformPhases* e0470_gray8_text_ranges[] = { &e0470_gray8_text_phases };
static const EpdWaveformMode e0470_gray8_text_mode = {
    .type = 5, .temp_ranges = 1, .range_data = &e0470_gray8_text_ranges[0],
};
static const EpdWaveformMode* e0470_gray8_text_modes[] = {
    &e0470_complete_du_mode,
    &e0470_gray8_gc16_mode,
    &e0470_gray8_text_mode,
};

const EpdWaveform E0470_GRAY8_TEXT_WAVEFORM = {
    .num_modes = 3,
    .num_temp_ranges = 1,
    .mode_data = e0470_gray8_text_modes,
    .temp_intervals = e0470_intervals,
};

/* ---- 默认表 / Default tables ---- */
// 完整灰阶表裁掉余量，开机算进 RAM。
// / Trim slack from the full gray tables into RAM at boot.
static uint8_t e0470_gc16_data[E0470_FULL_GC16_FRAMES][16][4];
static uint8_t e0470_gl16_data[E0470_FULL_GL16_FRAMES][16][4];
static const EpdWaveformPhases e0470_gc16_phases = {
    .phases = E0470_GC16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&e0470_gc16_data[0],
};
static const EpdWaveformPhases e0470_gl16_phases = {
    .phases = E0470_GL16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&e0470_gl16_data[0],
};
static const EpdWaveformPhases* e0470_gc16_ranges[] = { &e0470_gc16_phases };
static const EpdWaveformPhases* e0470_gl16_ranges[] = { &e0470_gl16_phases };
static const EpdWaveformMode e0470_gc16_mode = {
    .type = 2, .temp_ranges = 1, .range_data = &e0470_gc16_ranges[0],
};
static const EpdWaveformMode e0470_gl16_mode = {
    .type = 5, .temp_ranges = 1, .range_data = &e0470_gl16_ranges[0],
};
static const EpdWaveformMode* e0470_modes[] = {
    &e0470_complete_du_mode,
    &e0470_gc16_mode,
    &e0470_gl16_mode,
};

const EpdWaveform E0470_WAVEFORM = {
    .num_modes = 3,
    .num_temp_ranges = 1,
    .mode_data = e0470_modes,
    .temp_intervals = e0470_intervals,
};

const EpdWaveformPhases* e0470_waveform_phases(const EpdWaveform* waveform, int mode) {
    if (waveform == NULL) return NULL;
    const int type = mode & 0x3F;
    for (int i = 0; i < waveform->num_modes; i++) {
        if (waveform->mode_data[i]->type == type) return waveform->mode_data[i]->range_data[0];
    }
    return NULL;
}

int e0470_phase_action(const EpdWaveformPhases* phases, int phase, int to, int from) {
    if (phases == NULL || phases->luts == NULL) return 0;
    if (phase < 0 || phase >= phases->phases) return 0;
    if ((unsigned)to > 15 || (unsigned)from > 15) return 0;
    const uint8_t* cell = phases->luts + ((size_t)phase * 16 + to) * 4 + from / 4;
    return (*cell >> (6 - 2 * (from % 4))) & 3;
}

// 白底 15→15 源表全保持。挂在已经「往白推」的那几相上再推，**不增加相数**。
// 差分会跳过未变白像素，GL16 必须走全像素这帧才打到白底。
//
// 为什么不止 1 帧：一屏里各像素类的推力是不对称的 —— 由黑变白（0→15）在正文表里有
// 10 相、默认 37 相表里有 20 相，而**不变的白底（15→15）只吃到挂上去的那 1 相**。
// 残影正好攒在这一类上：上一页的字被推走之后，本页没字的空白只挨过 1 次推，退不干净。
// 挂满 = 让白底也吃到它那张表自己的白推预算，代价 0ms（相是现成的，只是把这一格的
// 动作从"保持"改成"推白"）。留成档位是因为推过头会把白底带出灰边，得实测。
//
// count: >0 挂这么多相（取候选里**最后**的几相，那是白推阶梯的饱和端，对推向白轨最
//         有效）；<0 = 挂满（候选相全挂）；0 = 关。
// 先把 (15,15) 的所有位清零再重挂 —— 同一张表反复调用结果只由 count 决定（幂等），
// 所以换档不必重算整表。返回实际挂上的相数。
// / count: >0 that many phases (the LAST candidates — the saturated end of the white
// ladder); <0 = all; 0 = off. Clears every (15,15) bit first, so repeated calls are
// idempotent and a knob change needs no rebuild. Returns the count actually hung.
static int e0470_gl16_white_pushes(uint8_t (*data)[16][4], int frames, int count) {
    for (int f = 0; f < frames; f++) lut_set(data, f, 15, 15, 0);
    int cand[E0470_FULL_GL16_FRAMES];
    int n = 0;
    for (int f = 0; f < frames; f++) {
        for (int from = 0; from < 15; from++) {
            if (lut_get(data, f, 15, from) == 2) {
                cand[n++] = f;
                break;
            }
        }
    }
    if (n == 0) {
        // 异常表（源表里没有白推相）：退回老做法，取倒数第三相，保底 1 帧。
        const int tick = frames > 2 ? frames - 3 : 0;
        lut_set(data, tick, 15, 15, 2);
        return 1;
    }
    int want = (count < 0) ? n : count;
    if (want > n) want = n;
    for (int i = n - want; i < n; i++) lut_set(data, cand[i], 15, 15, 2);
    return want;
}

// 当前档位。0 = 关，>0 = 每张表挂这么多相，<0 = 挂满。见 e0470_waveform_set_white_pushes。
static int s_white_pushes = E0470_GL16_WHITE_PUSHES_DEFAULT;

// 把当前档位刷到三张可写的 GL16 表上（默认 37 相、完整 48 相、8 灰阶正文 30 相）。
// 必须在下一次推屏之前调，不能在 epd_hl_update_* 走到一半时改表。
// / Re-apply the knob to the three writable GL16 tables. Call before the next
// push, never while an epd_hl_update_* is mid-scan.
static void e0470_white_pushes_apply(int count) {
    e0470_gl16_white_pushes(e0470_full_gl16_live, E0470_FULL_GL16_FRAMES, count);
    e0470_gl16_white_pushes(e0470_gl16_data, E0470_GL16_FRAMES, count);
    e0470_gl16_white_pushes(e0470_gray8_text_data, E0470_GRAY8_GL16_FRAMES, count);
}

void e0470_waveform_set_white_pushes(int count) {
    if (count == s_white_pushes) return;
    s_white_pushes = count;
    e0470_white_pushes_apply(count);
    ESP_LOGI(TAG, "白推档 = %d（<0 挂满，0 关）", count);
}

int e0470_waveform_white_pushes(void) {
    return s_white_pushes;
}

void e0470_waveform_init(void) {
    e0470_follow_lut_build(E0470_FOLLOW_FRAMES, e0470_follow_data);
    e0470_complete_du_build();

    const e0470_trim_t trim = {
        .erase_max = E0470_TRIM_ERASE_MAX,
        .sat_cut = E0470_TRIM_SAT_CUT,
        .white_sat_cut = E0470_TRIM_WHITE_SAT_CUT,
        .hold = E0470_TRIM_HOLD,
    };
    const int gc = e0470_waveform_trim(&e0470_full_gc16_phases, &trim, e0470_gc16_data);
    const int gl = e0470_waveform_trim(&e0470_full_gl16_phases, &trim, e0470_gl16_data);
    assert(gc == E0470_GC16_FRAMES);
    assert(gl == E0470_GL16_FRAMES);

    memcpy(e0470_full_gl16_live, e0470_full_gl16_data, sizeof(e0470_full_gl16_live));
    memcpy(e0470_gray8_text_data, e0470_gray8_gl16_data, sizeof(e0470_gray8_text_data));

    s_white_pushes = E0470_GL16_WHITE_PUSHES_DEFAULT;
    e0470_white_pushes_apply(s_white_pushes);
    ESP_LOGI(TAG, "白推档 = %d（<0 挂满，0 关）", s_white_pushes);
}
