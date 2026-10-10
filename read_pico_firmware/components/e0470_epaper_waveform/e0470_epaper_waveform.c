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

// ── (15→15)「不变的白底」这一格：白推 + 压黑两个旋钮 ──────────────────────
// 源表在这一格是全保持。它为什么重要：差分刷把每个像素编码成一个字节 (to<<4)|from，
// **没变的白像素正好编成 15→15** —— 也就是说一屏里"本来就没字的白底"挨到的驱动，
// 全由这一格决定，而"由黑变白"的像素走的是 0→15（默认表 18 相白推、正文表 10 相）。
// 两边不对称：白底只吃到挂上去的那点推力，退不到白轨，攒下来就是那层灰影。
//
// 两个旋钮挂的都是**表里现成的相**（只是把这一格的动作从"保持"改成"压黑/推白"），
// 所以不增加相数、不增加刷新时间：
//   白推（往白轨推）—— 挂在已经「往白推」的相上；取候选里**最后**几相（白推阶梯的
//                     饱和端，对推向白轨最有效）。
//   压黑（先往黑轨打回去再推白）—— 挂在已经「往黑压」的相上；取候选里**最前**几相
//                     （默认表 3~15 是那一段擦除，正文表 11~17）。这一项就是把 GC16
//                     里 15→15 那段"先压黑再推白"的摆动借到 GL16 上，代价是白底会跟着
//                     闪一下——挂几相就是闪多深，得实测。
// 同一相被两个旋钮都选中时压黑优先（它在前，是"先打回去"那一半）。
// 先把这一格清零再重挂：同一张表反复调用结果只由两个 count 决定（幂等），换档不必重算整表。
// / The (15→15) cell: white pushes and black pushes, both hung on phases that
// already carry that action elsewhere in the to=15 row — no extra phases, no
// extra time. Idempotent: the cell is cleared, then rebuilt from the two counts.
static void e0470_gl16_1515_build(
    uint8_t (*data)[16][4], int frames, int white_count, int black_count
) {
    int wcand[E0470_FULL_GL16_FRAMES];
    int bcand[E0470_FULL_GL16_FRAMES];
    for (int f = 0; f < frames; f++) lut_set(data, f, 15, 15, 0);
    int wn = 0, bn = 0;
    for (int f = 0; f < frames; f++) {
        int has_white = 0, has_black = 0;
        for (int from = 0; from < 15; from++) {
            const int a = lut_get(data, f, 15, from);
            if (a == 2) has_white = 1;
            else if (a == 1) has_black = 1;
        }
        if (has_white) wcand[wn++] = f;
        if (has_black) bcand[bn++] = f;
    }

    // 压黑：取最前几相（擦除段），挂满 = 全部候选。
    int want_b = (black_count < 0) ? bn : black_count;
    if (want_b > bn) want_b = bn;
    for (int i = 0; i < want_b; i++) lut_set(data, bcand[i], 15, 15, 1);

    // 白推：取最后几相（饱和端），挂满 = 全部候选。压黑已经占住的相跳过。
    int want_w = (white_count < 0) ? wn : white_count;
    if (want_w > wn) want_w = wn;
    for (int i = wn - want_w; i < wn; i++) {
        if (lut_get(data, wcand[i], 15, 15) == 0) lut_set(data, wcand[i], 15, 15, 2);
    }

    // 异常表（源表里没有白推相）：退回老做法，取倒数第三相，保底 1 帧。
    if (wn == 0 && white_count != 0) {
        const int tick = frames > 2 ? frames - 3 : 0;
        lut_set(data, tick, 15, 15, 2);
    }
}

// 当前档位。白推：0 = 关，>0 = 每张表挂这么多相，<0 = 挂满。压黑同义。
// 见 e0470_waveform_set_white_pushes / e0470_waveform_set_black_pushes。
static int s_white_pushes = E0470_GL16_WHITE_PUSHES_DEFAULT;
static int s_black_pushes = E0470_GL16_BLACK_PUSHES_DEFAULT;

// 表内容的版本号：每改一次表 +1。谁把"从这些表展开出来的 LUT"缓存了，就拿它判陈旧 ——
// 只比表指针和相数是不够的（档位改的就是同一张表的内容，指针不变）。
// 现役消费方：错相揭页的相位 LUT（e0470_page_turn.c 的 build_luts）。它原来只在
// 指针/相数变化时重建，于是"进书后第一页翻页建好 LUT、之后在设置里改档"完全不生效 ——
// 四档看着一模一样就是这个原因。
// / Content generation of the writable waveform tables, bumped on every change.
// Consumers that cache LUTs expanded from these tables must compare this too —
// the pointer and phase count stay the same when only a knob changes.
static int s_wf_gen = 1;

// 8 灰阶正文表的擦除加强档（见 e0470_waveform_set_erase_pushes）。0 = 关。
static int s_erase_pushes = E0470_GRAY8_TEXT_ERASE_PUSHES_DEFAULT;

// 8 灰阶正文表（E0470_GRAY8_TEXT_WAVEFORM 的 GL16 —— 阅读器**正文页翻页走的就是它**，
// 自适应和局刷都会把整页文字判进这一档）**擦除不够**：
//   · 它的 to=15 行对 from=0（旧黑字）只有 **10 相推白**，默认 37 相表是 **18 相**；
//     （主机端逐格拆过：8 灰阶表 to=15 行 = 18 保持 + 10 推白 + 2 整定，
//      默认 GL16 表 = 16 保持 + 18 推白 + 3 整定。）
// 于是"上一页的黑字该变白"这件事只被推了 10 次，退不到白轨 —— 留在屏上就是那层看得见的
// 浅影（用户描述："旧字迹的浅影"）。压黑/白推那两个旋钮动的是 (15,15)（本来就白、还是白
// 的背景），跟这里**不是同一格**，所以它们治不了这个。
//
// 这里把该行**前导的空相**借给 from<15 推白：那些相对 from<15 本来就是"保持"（相 0..10
// 全保持），改成推白**不增加相数**。时间上也几乎不花：前导保持跳过实测只有 1 相
// （日志「跳相 1」），把它变活最多多扫 1 相 ≈ 11ms。只动 to=15 行、只动 from<15 的格子；
// (15,15) 一格都不碰，仍归白推/压黑那两个旋钮。
//   count > 0：借这么多相；count < 0：全部借满；count == 0：关。
// / Borrow the leading all-hold phases of the 8-gray text table's to=15 row into
// white pushes for from<15 — the old ink is erased with only 10 pushes (the
// default table uses 18), which is exactly the faint previous-page ghost. The
// borrowed phases already hold for those cells, so no phase is added and the
// measured leading-hold skip is 1 phase, i.e. at most ~11 ms more. Only the
// to=15 row and only from<15; the (15,15) cell is untouched.
static void e0470_gray8_text_erase_boost(void) {
    if (s_erase_pushes == 0) return;
    uint8_t (*data)[16][4] = e0470_gray8_text_data;
    const int frames = E0470_GRAY8_GL16_FRAMES;
    // 只借**前导连续**的那一段空相（相 0..10），借到该行第一个有动作的相就停。
    // 表尾那两相（28、29）也是空的，但它们是"驱动完让面板歇一拍"的整定相
    // （见 E0470_TRIM_HOLD），拿它们推白等于把整定期也变成驱动期，所以不碰。
    int cand[E0470_GRAY8_GL16_FRAMES];
    int n = 0;
    for (int f = 0; f < frames; f++) {
        int used = 0;
        for (int from = 0; from < 15; from++) {
            if (lut_get(data, f, 15, from) != 0) {
                used = 1;
                break;
            }
        }
        if (used) break;
        cand[n++] = f;
    }
    int want = (s_erase_pushes < 0) ? n : s_erase_pushes;
    if (want > n) want = n;
    for (int i = 0; i < want; i++) {
        for (int from = 0; from < 15; from++) lut_set(data, cand[i], 15, from, 2);
    }
}

// 把当前三个档位刷到三张可写的 GL16 表上（默认 37 相、完整 48 相、8 灰阶正文 30 相）。
// 必须在下一次推屏之前调，不能在 epd_hl_update_* 走到一半时改表。
// **必须是纯函数**：同一个档位组合重复调用结果一致。8 灰阶那张要从源表整体重建 ——
// 借空相那一步改的就是"空相"本身，不重建的话第二次调用会把剩下的空相也借走，越借越多。
// / Re-apply all knobs. Pure: same knobs ⇒ same tables. The 8-gray one is rebuilt
// from the source because the erase boost consumes the very hold phases it looks
// for, so re-running it without a rebuild would keep borrowing more each time.
static void e0470_1515_apply(void) {
    e0470_gl16_1515_build(
        e0470_full_gl16_live, E0470_FULL_GL16_FRAMES, s_white_pushes, s_black_pushes
    );
    e0470_gl16_1515_build(e0470_gl16_data, E0470_GL16_FRAMES, s_white_pushes, s_black_pushes);
    memcpy(e0470_gray8_text_data, e0470_gray8_gl16_data, sizeof(e0470_gray8_text_data));
    e0470_gl16_1515_build(
        e0470_gray8_text_data, E0470_GRAY8_GL16_FRAMES, s_white_pushes, s_black_pushes
    );
    e0470_gray8_text_erase_boost();
    s_wf_gen++;
}

int e0470_waveform_generation(void) {
    return s_wf_gen;
}

void e0470_waveform_set_white_pushes(int count) {
    if (count == s_white_pushes) return;
    s_white_pushes = count;
    e0470_1515_apply();
    ESP_LOGI(TAG, "白推档 = %d（<0 挂满，0 关）", count);
}

int e0470_waveform_white_pushes(void) {
    return s_white_pushes;
}

void e0470_waveform_set_black_pushes(int count) {
    if (count == s_black_pushes) return;
    s_black_pushes = count;
    e0470_1515_apply();
    ESP_LOGI(TAG, "压黑档 = %d（<0 挂满，0 关）", count);
}

int e0470_waveform_black_pushes(void) {
    return s_black_pushes;
}

void e0470_waveform_set_erase_pushes(int count) {
    if (count == s_erase_pushes) return;
    s_erase_pushes = count;
    e0470_1515_apply();
    ESP_LOGI(TAG, "擦除加强 = %d（<0 借满，0 关）", count);
}

int e0470_waveform_erase_pushes(void) {
    return s_erase_pushes;
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
    s_black_pushes = E0470_GL16_BLACK_PUSHES_DEFAULT;
    s_erase_pushes = E0470_GRAY8_TEXT_ERASE_PUSHES_DEFAULT;
    e0470_1515_apply();
    ESP_LOGI(
        TAG, "白推档 = %d，压黑档 = %d，擦除加强 = %d（<0 挂满/借满，0 关）",
        s_white_pushes, s_black_pushes, s_erase_pushes
    );
}
