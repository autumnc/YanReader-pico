/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * E0470A01（684×1216，40pin）的 epdiy 波形。
 * 自行调整屏幕波形会使设备失去保修。
 *   E0470_WAVEFORM         默认：裁剪 GC16 36 相 / GL16 37 相 + 50/50 阈值 DU
 *   E0470_FULL_WAVEFORM    GC16 / GL16 各 48 相，一个温度档 0-50°C
 *   E0470_GRAY8_WAVEFORM   8 灰阶：GC16 / GL16 各 30 相，灰阶少一半换来整屏约 360ms
 *   E0470_FOLLOW_WAVEFORM  跟随 DU：8 帧（黑 7 / 白 8），用于触摸笔迹
 * 默认与 8 灰阶波形挂阈值 DU（目标 0-7→黑、8-15→白）；波形保留只驱动 0/15 的源表。
 * GL16 给 15→15 挂白推（档位见 e0470_waveform_set_white_pushes）；刷新走全像素，
 * 否则差分跳过白底。开机先调一次 e0470_waveform_init()。
 *
 * epdiy waveforms for the E0470A01 (684×1216, 40-pin).
 * Changing panel waveforms voids the warranty.
 *   E0470_WAVEFORM         Default: trimmed GC16 36 / GL16 37 + 50/50 threshold DU
 *   E0470_FULL_WAVEFORM    GC16 / GL16 48 phases each, one 0–50°C temp range
 *   E0470_GRAY8_WAVEFORM   8-gray: GC16 / GL16 30 phases; half the steps, ~360 ms full
 *   E0470_FOLLOW_WAVEFORM  Follow DU: 8 frames (black 7 / white 8) for touch ink
 * Default and 8-gray hang a threshold DU (dest 0–7→black, 8–15→white); the
 * source tables still drive only 0/15. GL16 hangs white pushes on 15→15
 * (knob: e0470_waveform_set_white_pushes); refresh is full-pixel or the diff
 * skips already-white.
 * Call e0470_waveform_init() once at boot.
 */

#pragma once

#include <stdint.h>

#include "epd_waveform.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 完整灰阶表：GC16 / GL16 各 48 相，白推动梯子 17 档、去重后 11 级可分辨灰阶。
/// DU 是 20 相，只驱动目标 0/15；E0470_FULL_WAVEFORM 原样保留对照。
/// / Full gray tables: GC16 / GL16 48 phases, 17-step white-push ladder,
/// 11 distinguishable after collapse. DU is 20 phases, dest 0/15 only;
/// E0470_FULL_WAVEFORM is kept as a reference.
#define E0470_FULL_DU_FRAMES 20
#define E0470_FULL_GC16_FRAMES 48
#define E0470_FULL_GL16_FRAMES 48
extern const EpdWaveform E0470_FULL_WAVEFORM;

/// 另一份表：只有 8 级可分辨灰阶（净推动梯子 8 档），30 相，整屏约 360ms（默认表约 430ms）。
/// 灰阶档数 = 白推动梯子的档数，在这块屏上 16 级与 30 相不可兼得，所以它是备选不是默认。
/// / Alternate table: 8 distinguishable grays (8-step net push), 30 phases,
/// ~360 ms full (default ~430 ms). Gray count is the white-push ladder; 16
/// levels and 30 phases cannot both fit this panel, so this is optional.
#define E0470_GRAY8_GC16_FRAMES 30
#define E0470_GRAY8_GL16_FRAMES 30
extern const EpdWaveform E0470_GRAY8_WAVEFORM;

/// 8 灰阶 GL16 的可写副本，唯一区别是开机给 15→15 挂上白推（源表里 15→15 是全保持，
/// 挂几相由 e0470_waveform_set_white_pushes 定）。正文翻页用它：30 相差分、不变的白
/// 像素只吃白推不换灰阶（不闪），比默认 37 相 GL16 每屏快约 80ms；代价是灰阶 8 级而非
/// 16 级，正文是黑白像素看不出来。
/// / Writable copy of the 8-gray GL16 table whose only change is the white pushes
/// hung on 15→15 (the source is all-hold there; the count is set by
/// e0470_waveform_set_white_pushes). Used for text page turns: 30-phase
/// differential, unchanged white only pushed and never re-graded (no flash),
/// about 80 ms faster per screen than the default 37-phase GL16, at 8 levels.
extern const EpdWaveform E0470_GRAY8_TEXT_WAVEFORM;

/// 裁剪版保留新屏所需的 3 个尾部保持相。完整48相实测未明显改善边缘残影，因此恢复
/// 已验证的主动段裁剪，避免无收益地增加约150ms；无损前导保持跳过仍由渲染器执行。
/// / Trim keeps the 3 trailing hold phases the new panel needs. The full 48
/// did not clearly help edge ghosting, so the proven active-segment trim is
/// restored instead of adding ~150 ms for no gain. Lossless leading-hold
/// skip is still done by the renderer.
#define E0470_TRIM_ERASE_MAX 11
#define E0470_TRIM_SAT_CUT 5
#define E0470_TRIM_WHITE_SAT_CUT 0
#define E0470_TRIM_HOLD 3
#define E0470_GC16_FRAMES 36
#define E0470_GL16_FRAMES 37

/// 帧周期取算法文档第 8 节的设计点 ν≈90Hz。
/// / Frame period is the §8 design point ν≈90 Hz.
#define E0470_WAVEFORM_FRAME_US 11090
extern const EpdWaveform E0470_WAVEFORM;

/// 触摸笔迹跟手用的 8 帧短 DU，走 FAST 扫描（帧周期约 7ms）。
/// 推动次数按 |to-from| 比例分配，满幅迁移往黑推 7 相、往白推 8 相。
/// / 8-frame short DU for touch ink, FAST scan (~7 ms/frame). Push count
/// scales with |to-from|; full travel is 7 black / 8 white.
#define E0470_FOLLOW_BLACK_FRAMES 7
#define E0470_FOLLOW_WHITE_FRAMES 8
#define E0470_FOLLOW_FRAMES 8
extern const EpdWaveform E0470_FOLLOW_WAVEFORM;

/// 按上面的公式生成 frames 帧的跟随表写进 dst（容量 frames×16×4 字节）。
/// / Build a follow table of `frames` into dst (frames×16×4 bytes).
void e0470_follow_lut_build(int frames, uint8_t (*dst)[16][4]);

/// 在一条 EpdWaveform 里按模式（MODE_GC16 / MODE_GL16 / MODE_DU）找唯一温度档的相位表。
/// / Look up the single temp-range phase table for MODE_GC16 / MODE_GL16 / MODE_DU.
const EpdWaveformPhases* e0470_waveform_phases(const EpdWaveform* waveform, int mode);

/// 查 (from→to) 在第 phase 相的 2bit 动作：0 保持、1 压黑、2 擦白。
/// / 2-bit action for (from→to) at `phase`: 0 hold, 1 darken, 2 erase.
int e0470_phase_action(const EpdWaveformPhases* phases, int phase, int to, int from);

/// 生成 E0470_WAVEFORM 与 E0470_FOLLOW_WAVEFORM 的表。必须在第一次刷新之前调用一次。
/// / Build E0470_WAVEFORM and E0470_FOLLOW_WAVEFORM. Call once before the first refresh.
void e0470_waveform_init(void);

/// GL16 表里「不变的白底」（15→15）挂几相白推。源表在这一格是全保持，而差分刷会跳过
/// 不变的白像素 —— 上一页的字推走之后，空白处只挨到挂上去的那点推力，退不干净就是残影。
/// 挂的相都是表里**已经在推白**的相（只是把这一格的动作从"保持"改成"推白"），所以
/// **不增加相数、不增加刷新时间**；代价是推过头会把白底带出灰边。
///
/// count: >0 挂这么多相（取白推阶梯的饱和端，即末尾几相）；<0 = 挂满（该表所有白推相
///         全挂）；0 = 关。三张可写 GL16 表（默认 37 相、完整 48 相、8 灰阶正文 30 相）
///         一起改。**必须在下一次推屏之前调**，不能在推屏走到一半时改表。
/// / How many phases of white push to hang on the 15→15 cell in the GL16 tables.
/// All phases used are already white-pushing ones, so no phase count and no
/// refresh time is added; hanging too many can shade the white background.
/// >0 = that many, <0 = all, 0 = off. Affects all three writable GL16 tables.
/// Call before the next push, never mid-scan.
void e0470_waveform_set_white_pushes(int count);

/// 当前白推档（可能被 set 改过，未改过则是编译期默认）。
/// / Current white-push knob (the compile-time default until set).
int e0470_waveform_white_pushes(void);

/// 白推档的编译期默认值：1 帧 = 老行为（只挂 1 相），-1 = 挂满。
/// / Compile-time default for the knob: 1 = legacy single tick, -1 = all phases.
#ifndef E0470_GL16_WHITE_PUSHES_DEFAULT
#define E0470_GL16_WHITE_PUSHES_DEFAULT 1
#endif

/// GL16 表里同一格（15→15，「不变的白底」）的另一个旋钮：先往黑轨打回去几相，再走上面
/// 那套白推。差分刷把**没变的白像素**编成 15→15，而源表在这一格是全保持 —— 于是白底只
/// 会被白推、永远不会被压回去，白轨漂了没人纠正。这一项就是把 GC16 里 15→15 那段
/// 「先压黑 10 相、再推白 10 相」的摆动借到 GL16 上，借的还是表里现成的压黑相
/// （默认表 14 相、8 灰阶正文表 7 相），所以**不增加相数、不增加刷新时间**。
/// 它顺手解决了「白底发灰」的根：先打回黑轨再推白，等于把白轨重新锚定一次。
///
/// 代价：白底会跟着闪一下 —— 挂几相就闪多深。0 = 关（默认，与改动前完全一致）；
/// 小档（2~4）= 轻压；<0 = 挂满（白底明显闪，接近一次局部全刷）。
/// 与白推共用 (15,15) 这一格，同一相被两边都选中时压黑优先。
/// 三张可写 GL16 表一起改，**必须在下一次推屏之前调**。
/// / How many darken phases to hang on the 15→15 cell before the white pushes:
/// borrows the GC16 dark-then-white swing into the GL16 tables using phases that
/// already darken elsewhere, so no phase count and no time is added. Off by
/// default; a small count anchors the white rail back, <0 = all (visible flash).
void e0470_waveform_set_black_pushes(int count);

/// 当前压黑档。
/// / Current black-push knob.
int e0470_waveform_black_pushes(void);

/// 压黑档的编译期默认值：0 = 关（与改动前一致）。
/// / Compile-time default: 0 = off, matching the pre-change behaviour.
#ifndef E0470_GL16_BLACK_PUSHES_DEFAULT
#define E0470_GL16_BLACK_PUSHES_DEFAULT 0
#endif

/// 可写波形表**内容**的版本号：每调一次上面两个 set 就 +1。
///
/// 给"把从这些表展开出来的 LUT 缓存住"的组件判陈旧用。原来错相揭页只比
/// (表指针, 相数)，而档位改的正是同一张表的内容 —— 指针和相数都不变，于是缓存永不失效：
/// 进书后第一页翻页建好相位 LUT，之后再在设置里改档，屏幕一点变化都没有（四档同观感）。
/// 缓存方把这一项一起比对即可。
/// / Content generation of the writable tables; bumped by either set() call.
/// Caches expanded from these tables must compare it alongside the pointer and
/// phase count — neither of those changes when only a knob does.
int e0470_waveform_generation(void);

/// 8 灰阶正文表（正文页翻页实际走的那张）的**擦除加强**档 —— 治"旧字迹的浅影"的那一个。
///
/// 上面两个旋钮（白推 / 压黑）改的都是 `(15,15)` —— **本来就白、还是白**的那个像素，也就是
/// **背景**。而"上一页的字没擦干净"是另一个格子：旧黑字要变白，走的是 `(15, from<15)`。
/// 差分刷的每个像素都编成 `(to<<4)|from`，所以那格才是"旧字迹"的出口。
///
/// 8 灰阶正文表的 `to=15` 行对 `from=0`（旧黑字）只有 **10 相**推白（默认 37 相表是 **18 相**），
/// 所以 10 次推白退不到白轨，残留在屏上就是那层浅影。因为阅读器的自适应与局刷都把整页文字
/// 判进这一档（日志实测：变化 235~296‰ 全部 → 8 灰阶正文刷），这条路上的残影基本全由这里决定。
///
/// 这一项把该行**前导连续的空相**借给 `from<15` 推白：那些相对 `from<15` 本来就是"保持"
/// （30 相表里是相 0..10，共 11 相），改成推白**不增加相数**。时间上也几乎不花 —— 前导保持
/// 跳过实测只有 1 相（日志「跳相 1」），最多多扫 1 相 ≈ 11ms。只动 to=15 行、只动 from<15
/// 的格子，`(15,15)` 一格都不碰（仍归白推/压黑）。表尾那两相整定保持**不借**。
///   count > 0：借这么多相；count < 0：借满；count == 0：关（默认）。
/// 借过头会把白底推得更白一点点（推白只会朝白轨饱和，不会伤灰阶），留成档位实测。
/// / Erase-strength knob for the 8-gray text table: borrow its leading all-hold
/// phases on the to=15 row into white pushes for from<15. That cell — not
/// (15,15) — is where a previous page's black text gets erased, and this table
/// only pushes it 10 times vs the default table's 18, which is the faint
/// previous-page ghost. No phase count is added; the measured leading-hold skip
/// is 1 phase, so at most ~11 ms. (15,15) is untouched, and the trailing settle
/// holds are not borrowed.
void e0470_waveform_set_erase_pushes(int count);

/// 当前擦除加强档。
/// / Current erase-strength knob.
int e0470_waveform_erase_pushes(void);

/// 擦除加强档的编译期默认值：0 = 关（与改动前一致）。
/// / Compile-time default: 0 = off.
#ifndef E0470_GRAY8_TEXT_ERASE_PUSHES_DEFAULT
#define E0470_GRAY8_TEXT_ERASE_PUSHES_DEFAULT 0
#endif

#ifdef __cplusplus
}
#endif
