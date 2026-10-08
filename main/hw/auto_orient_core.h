#pragma once

// 自适应屏幕方向的**纯判定核**：一次加速度采样进，一个"该用哪一档"出。
//
// 为什么单独一个零依赖的头：这个功能里唯一有分歧、值得反复调的就是判据本身（门槛、
// 余量、连续拍数、冷却），而它跟 ESP-IDF、跟 board、跟主循环都没有关系。切成纯函数
// 之后主机 g++ 直接 include 就能把所有边界情形过一遍（tests/host/orient/），不用刷机
// 也不用 stub —— 真机上要验的只剩"哪根轴对应竖屏"这一位（见 kPortraitIsXAxis）。
// 硬件/界面那一半接线在 auto_orient.cpp。
//
// 采样是**设备帧**（见 read_pico_init.h：平放屏幕朝上为 +Z；立在 X 正边为 +X），
// 由 read_pico_accel_to_device() 从芯片帧换算。

#include <stdint.h>

// ── 判据常量（毫克）──────────────────────────────────────────────────────
//
// 姿态判据不能只挑"哪根轴最大"就完事，否则斜着拿、搁桌上都会乱认，所以三道并列：
//
//   有效重力：|‖a‖ − 1000| ≤ 250 —— 手在动就这一拍不判。静止时不管怎么斜，‖a‖ 恒等于
//     1g，所以这一条筛掉的是**运动**（尤其"晃动机身=全刷"那次甩动），不是姿态。
//     与 input.cpp 的 shake_poll 天然互补：那边的判据正是 ‖a‖ 偏离 1g，两边不吃同一口。
//   主轴门槛/余量：|主轴| ≥ 350 且 |主轴| ≥ |副轴| + 450。450mg 在 1g 上对应"偏离主轴
//     约 26.6°"：±26.6° 内算这一档，26.6°～63.4° 是**死区**（判不出 = 保持现状），再
//     过去才算另一档。死区这么宽是刻意的 —— 手持的晃动、斜靠在支架上都落在里面，
//     屏幕不会跟着抖。
//   连续拍数：同向连着 DWELL 拍才认（采样 120ms 一拍 ≈ 0.5s）。慢速翻转全程 ‖a‖≈1g，
//     挡它的正是这一条和"到位之前一直落在死区里"：翻到一半不会转屏。
#define AOC_GRAV_TOL_MG     250
#define AOC_AXIS_MIN_MG     350
#define AOC_AXIS_MARGIN_MG  450
#define AOC_DWELL_HITS      4
// 转一次是整屏 GC16（约 1.8s）。转完静默这么久，期间只重新攒 dwell —— 全刷里的手抖
// 不会被当成"下一个姿态已经稳了"而连转两下。
#define AOC_COOLDOWN_MS     2500
// 最近一次按键/触摸之后这么久内不转：正在打字、正在划列表的时候屏幕掉头，等于把用户
// 正按着的那一下丢进另一个版面。打字间隙（换词、想一下）比这长，所以停手一会儿就会转。
#define AOC_INPUT_QUIET_MS  800

// 论域：0 = 判不出（保持现状）1 = 横屏 2 = 竖屏
#define AOC_UNKNOWN   0
#define AOC_LANDSCAPE 1
#define AOC_PORTRAIT  2

// 设备帧的哪根轴对应"竖屏" —— **实机标定出来的那一位**。
// 静态推不出来：read_pico_init.h 只说了屏幕法线是 +Z，没说 X 轴对应面板哪条边，而
// 芯片帧/触摸帧各自都换过坐标系（read_pico_accel_to_device / input.cpp 的
// touch_to_logical），推不到结论。第一次上机的日志就是为它准备的（auto_orient.cpp
// 在判定变化时打 x/y/z）：竖着拿机器，屏幕应该变竖；若竖着拿反而横屏，把这里翻过来
// 重刷一次即可，别的一行都不用动。
static const int kPortraitIsXAxis = 1;

typedef struct {
    int cand;               // 当前候选档（AOC_UNKNOWN/AOC_LANDSCAPE/AOC_PORTRAIT）
    int hits;               // 候选连着成立了几拍
    int64_t hold_until_us;  // 冷却截止（由 aoc_note_rotated 置）
} AutoOrientCore;

static inline int aoc_abs(int v) { return v < 0 ? -v : v; }

// 一次采样 → 目标档。判不出返回 AOC_UNKNOWN。
static inline int aoc_classify(int x_mg, int y_mg, int z_mg) {
    // 模用平方比，省一次 isqrt（量程 ±2g，三轴平方和 ≤ 1.2e7，int 够）。
    const int m2 = x_mg * x_mg + y_mg * y_mg + z_mg * z_mg;
    const int lo = 1000 - AOC_GRAV_TOL_MG;
    const int hi = 1000 + AOC_GRAV_TOL_MG;
    if (m2 < lo * lo || m2 > hi * hi) return AOC_UNKNOWN;

    const int ax = aoc_abs(x_mg);
    const int ay = aoc_abs(y_mg);
    const int major = ax > ay ? ax : ay;
    const int minor = ax > ay ? ay : ax;
    if (major < AOC_AXIS_MIN_MG) return AOC_UNKNOWN;
    if (major < minor + AOC_AXIS_MARGIN_MG) return AOC_UNKNOWN;

    const int portrait = ((ax > ay) == (kPortraitIsXAxis != 0));
    return portrait ? AOC_PORTRAIT : AOC_LANDSCAPE;
}

// 喂一拍。cur = 当前实际朝向（1/2）。返回"该落到哪一档"，0 = 不用动。
//
// 姿态稳定成立满 DWELL 拍、和 cur 不同、且过了冷却 → **持续**返回这一档，直到调用方真
// 转了（转了之后 cur 跟上，返回值自然变成 0）。所以"什么时候真的转"由调用方定：正在
// 打字就先不转（见 auto_orient.cpp 的 tick），返回值不会因此丢掉、也不会重新攒 dwell。
static inline int aoc_feed(AutoOrientCore *st, int cur, int x_mg, int y_mg, int z_mg,
                           int64_t now_us) {
    const int r = aoc_classify(x_mg, y_mg, z_mg);
    if (r == AOC_UNKNOWN) {   // 死区/在动：候选作废，重新攒
        st->cand = AOC_UNKNOWN;
        st->hits = 0;
        return 0;
    }
    if (r != st->cand) {
        st->cand = r;
        st->hits = 1;
    } else if (st->hits < AOC_DWELL_HITS) {
        st->hits++;
    }
    if (st->hits < AOC_DWELL_HITS) return 0;
    if (r == cur) return 0;
    if (now_us < st->hold_until_us) return 0;
    return r;
}

// 调用方真的转过屏之后调一次：进冷却 + 清 dwell。
static inline void aoc_note_rotated(AutoOrientCore *st, int64_t now_us) {
    st->hold_until_us = now_us + (int64_t)AOC_COOLDOWN_MS * 1000;
    st->cand = AOC_UNKNOWN;
    st->hits = 0;
}

// 干净重来：启用/停用（模式进出）时清一遍，别让上一次攒的 dwell 带进来。
static inline void aoc_reset(AutoOrientCore *st) {
    st->cand = AOC_UNKNOWN;
    st->hits = 0;
    st->hold_until_us = 0;
}

// 该不该**现在**落地：有动作 + 最近没有输入。返回非 0 = 现在就转。
// 只挡"什么时候转"，不挡"要转到哪"——所以打字期间返回值一直挂着，停手就补上，
// 不用重新攒 dwell。
static inline int aoc_should_apply(int want, int64_t now_us, int64_t last_input_us) {
    if (want == 0) return 0;
    if (now_us - last_input_us < (int64_t)AOC_INPUT_QUIET_MS * 1000) return 0;
    return want;
}
