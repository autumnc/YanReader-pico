/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 阅读器 → 推屏层 的"白底参考帧纪律"提示，以及灰阶自检页的自推屏入口。
 *
 * 单独一个头的理由：阅读器那几个 TU **不能** include display.h
 * （display.h → epdiy.h → epd_internals.h，那里的 EpdFont typedef 与
 * crossmux 的 EpdFont(class) 在同一 TU 冲突）。这里只放不依赖 epdiy 的裸声明，
 * 实现在 display.c。
 *
 * ── 白底参考帧纪律是什么 ───────────────────────────────────────────────
 * 差分刷（GL16/DU）的基准是面板**当前**的像素。面板上如果留着中间灰，下一屏
 * 从这块灰上差分出来就是脏的。官方固件为此在每次离开灰阶画面时先 GC16 铺一屏
 * 白、再从白底出下一屏（"中间灰不能当参考帧"，app_refresh.c）。
 * 我们这边：插图页 / 图片查看器 / 灰阶自检页都会在面板上留下真正的中灰，而
 * 从它们翻到正文页走的是差分档 —— 所以需要在"上一帧是灰"这件事上记一笔账，
 * 由推屏层在紧接着的那一帧差分刷之前把白底补上。
 *
 * 记账**必须在推屏之后**做（语义是"面板现在是什么"），放到渲染入口就会让同一页
 * 的下一帧被自己置位、变成每帧白闪；消费侧只对差分档铺白（FULL/GRAY8 本来就是
 * 整屏全像素 GC16，再铺一次只是白花一次全刷）。
 */

#pragma once

#include <stdlib.h>   // atoi（reader_ime_frames_for_mode）

#ifdef __cplusplus
extern "C" {
#endif

/// 记下"面板上现在是不是中灰"。阅读器每次推屏**之后**调一次：
///   on != 0 —— 刚推上去的这一帧是灰阶画面（插图页/图片查看器/自检页）
///   on == 0 —— 刚推上去的这一帧是纯黑白（正文/菜单/列表）
/// 下一次走差分档的推屏会先 GC16 铺白再画，然后这笔账自动清掉（一次性）。
void reader_set_gray_panel(int on);

/// 面板上是不是还留着中灰（上一帧是灰阶画面，那一笔白底账还没清）。
/// 只在"要不要把这一帧收窄成区域刷"的判据里读（screen_reader.cpp 列表帧那条路）：
/// 白底重锚必须整屏做，收窄会把那次 from-white 挤掉，让中灰接着当差分基准。
/// 只看不取 —— 消费仍然是 display.c 里差分的那个 take。
int reader_white_exit_pending(void);

/// 灰阶自检页的自推屏：把当前 framebuffer 用指定的那条刷法推上去，返回耗时 ms。
/// 阅读器 TU 不碰波形/模式类型，所以这一层由 display.c 提供。
/// which: 0=整屏 GC16(默认表) 1=8 灰阶表整屏 2=from-white 16 灰
///        3=8 灰阶表 from-white 4=局部 DU(上半屏)
/// 返回 -1 表示参数不认识。
int reader_refresh_test_present(int which);

/// 最近一次扫描因「前导整屏保持相」被跳过的相数（epdiy 的 epd_last_leading_skip）。
/// 差分刷（GL16/DU 这类 MODE_*_DIFFERENCE 的档）才可能有；整屏全像素档恒为 0。
/// 这个开关（epd_set_leading_skip）由板级 bring-up（read_pico_init）开，本仓库不开它 ——
/// 这个访问器只是把它**看得见**：翻页那一行日志里的「跳相 N」就是它，N 长期为 0 说明
/// 这一档走在全像素路径上（那才是"该不该为它折腾"的判断依据）。
int reader_leading_skip(void);

/// GL16 表里「不变的白底」（15→15）挂几相白推 —— 局刷残影的那个旋钮。
///
/// 波形源表在 15→15 这一格是全保持，而差分刷的差分面会跳过不变的白像素。这是**故意的**
/// （不驱动 = 不闪），但它有个不对称：一屏里"由黑变白"的像素吃得到表里整整一梯白推，
/// 而"本来就没字的白底"只吃到开机挂上去的那很少几相。上一页的字被推走之后，空白处
/// 退不到真正的白轨，攒下来就是那层看得见的灰影。
///
/// 挂的相都是表里**已经在推白**的相（只把这一格的动作从"保持"改成"推白"），所以
/// **不增加相数、不增加刷新时间**；代价是推过头会把白底带出灰边，所以留成档位实测。
///   count > 0：挂这么多相；count < 0：挂满（该表所有白推相）；count == 0：关。
/// 三张可写 GL16 表（默认 37 相 / 完整 48 相 / 8 灰阶正文 30 相）一起改。
/// **必须在下一次推屏之前调**，不能在推屏走到一半时改表。
/// / How many white pushes to hang on the unchanged-white cell of the GL16 tables.
void reader_set_white_pushes(int count);

/// 当前白推档（未设过就是波形组件的编译期默认）。
int reader_white_pushes(void);

/// 同一格 (15→15) 的另一个旋钮：**先压黑几相再走白推**。
///
/// 差分刷把每个像素编码成一个字节 (to<<4)|from，**没变的白像素正好编成 15→15**，所以
/// 一屏里"本来就没字的白底"挨到的驱动全由这一格决定。源表在这一格是全保持 → 白底只会
/// 被白推、永远不会被压回去，白轨漂了没人纠正。这一项把 GC16 里 15→15 那段"先压黑再
/// 推白"的摆动借到 GL16 上，借的全是表里现成的压黑相（默认表 14 相、8 灰阶正文表 7 相），
/// 所以**不增加相数、不增加刷新时间**。
///
/// 代价：白底跟着闪一下，挂几相就闪多深。0 = 关（默认）；小档 2~4 = 轻压；<0 = 挂满。
/// 与白推共用这一格，同一相两边都选中时压黑优先。**必须在下一次推屏之前调。**
/// / How many darken phases to hang on the (15→15) cell before the white pushes.
void reader_set_black_pushes(int count);

/// 当前压黑档。
int reader_black_pushes(void);

/// **治"旧字迹的浅影"的那一个**（上面两个治的是背景，不是这个）。
///
/// 差分刷把每个像素编成一个字节 `(to<<4)|from`：
///   · 没变的白底 → 15→15 —— 归上面白推/压黑两个旋钮；
///   · **上一页的黑字要变白 → 15←0（以及 from<15 的各档）** —— 归这一项。
///
/// 阅读器的自适应和局刷都把整页文字判进「8 灰阶正文刷」（日志实测：变化 235~296‰ 全落这一档），
/// 而这张 30 相表的 `to=15` 行对 from=0（旧黑字）只有 **10 相**推白（默认 37 相表是 **18 相**）
/// —— 10 次推白退不到白轨，残留就是那层浅影。
/// 这一项把该行前导连续的空相（相 0..10）借给 from<15 推白：**不加相数**，时间上也几乎不花
/// （前导保持跳过实测只有 1 相「跳相 1」，最多多扫 1 相 ≈ 11ms）。只动 to=15 行、只动
/// from<15 的格子，(15,15) 不碰（那格归上面的白推/压黑两个旋钮）。
///   count > 0：借这么多相；count < 0：借满；count == 0：关。
/// 阅读器只在阅读模式里挂它、离开时归零。
/// / Erase-strength knob for the previous page's ink: borrows the 8-gray text
/// table's leading all-hold phases on the to=15 row into white pushes for
/// from<15. That cell — not (15,15) — is what erases the old text, and this
/// table only pushes it 10 times vs the default table's 18.
void reader_set_erase_pushes(int count);

/// 当前擦除加强档。
int reader_erase_pushes(void);

/// 「打字残影」= 输入法那两行**自己那一拍**（逐键、矩形只有输入法条）用几张相的跟随表。
///
/// 8 = 跟随表 8 相（改动前的行为，逐字节一致）；> 8 = E0470_IME_WAVEFORM，同一条公式，
/// 只把擦白那一侧的满推从 8 抬到 n（见波形头那段）。**相数就是时间**：n × 11.09ms，
/// 区域刷不省时间，所以这个旋钮的代价是线性的、按每一键付。
///
/// 表是共享的（波形组件里就一张），所以挂/摘必须在两个点上成对：开机（main.cpp）与
/// 改设置那一下（screen_settings 的 pickerApply —— 那个键**不能只写设置**）。
/// **阅读模式出门不用管**：这条只动编辑器打字那一拍用的表，阅读器自己的「残影治理」
/// 用的是另一套旋钮（reader_set_erase_pushes / white / black，见上面）。
/// / "Typing ghost": how many phases the IME strip's own frame uses. 8 = the follow
/// table (unchanged behaviour); > 8 = the IME table with the white budget raised to n.
void reader_set_ime_frames(int n);

/// 设置值（"8" / "12" / …）→ 相数；认不出或空 = 8（现状）。开机与改设置共用这一份映射。
/// 上限不在这里夹 —— 波形组件自己会夹到 [8, E0470_IME_FRAMES_MAX]。
/// / Settings string → phase count. Unknown/empty = 8. The upper clamp lives in the
/// waveform component.
static inline int reader_ime_frames_for_mode(const char *mode) {
    if (mode == NULL || mode[0] == '\0') return 8;
    const int n = atoi(mode);
    return n > 8 ? n : 8;
}

#ifdef __cplusplus
}
#endif
