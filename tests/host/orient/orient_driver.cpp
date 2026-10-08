// 自适应屏幕方向的**判定核**主机测试：喂合成的重力向量，断言状态机的输出序列。
//
// 只 include main/hw/auto_orient_core.h —— 那个头零依赖（只 <stdint.h>），所以这里既
// 不用 ESP-IDF 也不用任何 stub，普通 g++ 直接编。真机上要验的只剩"哪根轴对应竖屏"
// 这一位（core 里的 kPortraitIsXAxis），见 README.md。
//
// 采样节拍与设备一致：120ms 一拍（input.cpp 的 SHAKE_POLL_MS）。

#include "auto_orient_core.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_fail = 0;
static int g_checks = 0;

static const char *name(int o) {
    switch (o) {
        case AOC_LANDSCAPE: return "横屏";
        case AOC_PORTRAIT:  return "竖屏";
        default:            return "判不出";
    }
}

static void check(bool ok, const std::string &what) {
    g_checks++;
    if (ok) {
        printf("  ok   %s\n", what.c_str());
    } else {
        g_fail++;
        printf("  FAIL %s\n", what.c_str());
    }
}

static void checkEq(int got, int want, const std::string &what) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s（期望 %s，实得 %s）", what.c_str(), name(want), name(got));
    check(got == want, buf);
}

// 喂采样的一台"机器"：cur 是当前实际朝向，真转了就跟着变并记一条事件（模拟设备上
// board_rotate_live + aoc_note_rotated 那两步）。
struct Rig {
    struct Ev {
        int step;
        int to;
    };
    AutoOrientCore st;
    int cur;
    int64_t now_us;
    int step;
    int64_t last_input_us;   // 0 = 很久没输入
    std::vector<Ev> ev;

    explicit Rig(int start) : st{}, cur(start), now_us(1000000), step(0), last_input_us(0) {}

    void feed(int x, int y, int z) {
        step++;
        const int want = aoc_feed(&st, cur, x, y, z, now_us);
        const int act = aoc_should_apply(want, now_us, last_input_us);
        if (act) {
            ev.push_back({step, act});
            cur = act;
            aoc_note_rotated(&st, now_us);
        }
        now_us += 120000;
    }

    void feedN(int n, int x, int y, int z) {
        for (int i = 0; i < n; i++) feed(x, y, z);
    }
};

// ── 单拍判定：姿态 → 档 ─────────────────────────────────────────────────
static void scenarioClassify() {
    printf("== 单拍判定（姿态表）==\n");
    struct C {
        const char *what;
        int x, y, z;
        int want;
    };
    const C cases[] = {
        {"平放屏幕朝上 (0,0,1000)", 0, 0, 1000, AOC_UNKNOWN},
        {"平放屏幕朝下 (0,0,-980)", 0, 0, -980, AOC_UNKNOWN},
        {"竖着拿 (900,40,300)", 900, 40, 300, AOC_PORTRAIT},
        {"竖着拿反手 (-900,40,300)", -900, 40, 300, AOC_PORTRAIT},
        {"横着拿 (30,950,250)", 30, 950, 250, AOC_LANDSCAPE},
        {"横着拿反手 (30,-950,250)", 30, -950, 250, AOC_LANDSCAPE},
        {"偏 25.5°（容忍边内）(900,430,300)", 900, 430, 300, AOC_PORTRAIT},
        {"偏 30°（死区）(866,500,300)", 866, 500, 300, AOC_UNKNOWN},
        {"偏 32°（死区）(760,480,520)", 760, 480, 520, AOC_UNKNOWN},
        {"正好 45° (707,707,700)", 707, 707, 700, AOC_UNKNOWN},
        {"后仰 45° 但姿态是竖屏 (700,200,700)", 700, 200, 700, AOC_PORTRAIT},
        {"甩动那一拍 (900,40,1400)", 900, 40, 1400, AOC_UNKNOWN},
        {"自由落体似的 (0,0,300)", 0, 0, 300, AOC_UNKNOWN},
    };
    for (const C &c : cases) {
        checkEq(aoc_classify(c.x, c.y, c.z), c.want, c.what);
    }
}

// ── 连续拍数：不到 DWELL 不转，到了转一次 ────────────────────────────────
static void scenarioDwell() {
    printf("== 连续拍数（DWELL=%d 拍）==\n", AOC_DWELL_HITS);
    Rig r(AOC_LANDSCAPE);
    r.feedN(AOC_DWELL_HITS - 1, 900, 40, 300);
    check(r.ev.empty(), "差一拍不转");
    r.feed(900, 40, 300);
    check(r.ev.size() == 1 && r.ev[0].to == AOC_PORTRAIT, "凑满 DWELL 转到竖屏");
    r.feedN(50, 900, 40, 300);
    check(r.ev.size() == 1, "转完一直拿着不反复转");

    // 来回抖：两拍横两拍竖，永远凑不满连续同向。
    Rig g(AOC_LANDSCAPE);
    for (int i = 0; i < 20; i++) {
        g.feed(900, 40, 300);
        g.feed(900, 40, 300);
        g.feed(30, 950, 250);
    }
    check(g.ev.empty(), "来回换手（两拍一换）凑不满 DWELL");
}

// ── 慢速翻转：全程 1g，中途一拍都不该动 ─────────────────────────────────
static void scenarioSlowFlip() {
    printf("== 慢速翻转 90°（全程 ‖a‖≈1044）==\n");
    Rig r(AOC_LANDSCAPE);
    int firstPortraitStep = -1;
    printf("  步  角度   x    y    z   判定        累计转屏\n");
    for (int deg = 0; deg <= 90; deg += 5) {
        const double rad = deg * M_PI / 180.0;
        const int x = (int)lround(1000.0 * sin(rad));
        const int y = (int)lround(1000.0 * cos(rad));
        const int c = aoc_classify(x, y, 300);
        if (c == AOC_PORTRAIT && firstPortraitStep < 0) firstPortraitStep = r.step + 1;
        printf("  %2d  %3d°  %4d %4d %4d  %-8s  %zu\n", r.step + 1, deg, x, y, 300, name(c),
               r.ev.size());
        r.feed(x, y, 300);
    }
    check(r.ev.size() == 1, "整段只转一次");
    if (!r.ev.empty()) {
        checkEq(r.ev[0].to, AOC_PORTRAIT, "转到的档");
        char buf[128];
        snprintf(buf, sizeof(buf), "转点在第一次判出竖屏之后攒满 DWELL（第 %d 步，期望 %d）",
                 r.ev[0].step, firstPortraitStep + AOC_DWELL_HITS - 1);
        check(r.ev[0].step == firstPortraitStep + AOC_DWELL_HITS - 1, buf);
        check(firstPortraitStep >= 12, "翻转中途（死区里）没有提前认出来");
    }
}

// ── 甩动：‖a‖ 冲出去的那几拍不判 ───────────────────────────────────────
static void scenarioShake() {
    printf("== 晃动机身（‖a‖ 冲出 1g）==\n");
    Rig r(AOC_LANDSCAPE);
    r.feedN(6, 900, 40, 1400);   // 姿态像竖屏，模 1665mg
    check(r.ev.empty(), "甩动期间不判");
    r.feedN(AOC_DWELL_HITS, 900, 40, 300);
    check(r.ev.size() == 1 && r.ev[0].to == AOC_PORTRAIT, "甩完静止下来按新姿态转一次");
}

// ── 冷却：转完一阵内不连转 ──────────────────────────────────────────────
static void scenarioCooldown() {
    printf("== 冷却（%dms 内不连转）==\n", AOC_COOLDOWN_MS);
    Rig r(AOC_LANDSCAPE);
    r.feedN(AOC_DWELL_HITS, 900, 40, 300);
    check(r.ev.size() == 1 && r.cur == AOC_PORTRAIT, "先转到竖屏");
    const int flipStep = r.ev[0].step;
    r.feedN(20, 30, 950, 250);   // 2.4s，还在冷却里
    check((int)r.ev.size() == 1, "冷却期里姿态再变也不动");
    r.feedN(6, 30, 950, 250);
    check(r.ev.size() == 2 && r.ev[1].to == AOC_LANDSCAPE, "冷却一过补上");
    if (r.ev.size() == 2) {
        const int gap_ms = (int)((int64_t)(r.ev[1].step - flipStep) * 120);
        char buf[128];
        snprintf(buf, sizeof(buf), "补上的转点离上一次 %dms（应 ≥ 冷却 %dms）", gap_ms,
                 AOC_COOLDOWN_MS);
        check(gap_ms >= AOC_COOLDOWN_MS, buf);
    }
}

// ── 打字抑制：手上有动作时不落地，停手就补 ───────────────────────────────
static void scenarioQuietWindow() {
    printf("== 打字抑制（最近 %dms 内有输入就不转）==\n", AOC_INPUT_QUIET_MS);
    Rig r(AOC_LANDSCAPE);
    r.last_input_us = r.now_us;   // 一直在打字：每拍都刚有输入
    for (int i = 0; i < 30; i++) {
        r.last_input_us = r.now_us;
        r.feed(900, 40, 300);
    }
    check(r.ev.empty(), "打字期间不掉头");
    // 停手：下一拍就补上（不用重新攒 DWELL）。
    r.last_input_us = r.now_us - (int64_t)(AOC_INPUT_QUIET_MS + 100) * 1000;
    r.feed(900, 40, 300);
    check(r.ev.size() == 1 && r.ev[0].to == AOC_PORTRAIT, "停手后立刻补上");
}

// ── 搁桌上：平放 + 噪声，绝不误触 ───────────────────────────────────────
static void scenarioDeskIdle() {
    printf("== 搁桌上（平放、被人碰一下的噪声）==\n");
    Rig r(AOC_LANDSCAPE);
    unsigned seed = 20261008u;
    int worst = 0;
    for (int i = 0; i < 200; i++) {
        seed = seed * 1103515245u + 12345u;
        const int nx = (int)((seed >> 16) % 301) - 150;
        seed = seed * 1103515245u + 12345u;
        const int ny = (int)((seed >> 16) % 301) - 150;
        seed = seed * 1103515245u + 12345u;
        const int z = 850 + (int)((seed >> 16) % 301);
        if (abs(nx) > worst) worst = abs(nx);
        r.feed(nx, ny, z);
    }
    check(r.ev.empty(), "200 拍噪声里一次都没转");
    printf("  （最大横向分量 %dmg，门槛 %dmg）\n", worst, AOC_AXIS_MIN_MG);
}

// ── 重置：模式切换别把上一次攒的 dwell 带进来 ───────────────────────────
static void scenarioReset() {
    printf("== 停用/启用（模式进出）==\n");
    Rig r(AOC_LANDSCAPE);
    r.feedN(AOC_DWELL_HITS - 1, 900, 40, 300);   // 攒了 3 拍
    aoc_reset(&r.st);
    r.feed(900, 40, 300);                        // 重置后只喂 1 拍
    check(r.ev.empty(), "reset 之后不会拿上一次的 dwell 顶数");
    r.feedN(AOC_DWELL_HITS - 1, 900, 40, 300);
    check(r.ev.size() == 1 && r.ev[0].to == AOC_PORTRAIT, "重新攒满才转");
}

int main() {
    printf("kPortraitIsXAxis = %d（实机标定那一位；竖着拿应该判竖屏）\n\n", kPortraitIsXAxis);
    printf("常量：门槛 %dmg 余量 %dmg 重力带 ±%dmg DWELL %d 拍 冷却 %dms 输入静默 %dms\n\n",
           AOC_AXIS_MIN_MG, AOC_AXIS_MARGIN_MG, AOC_GRAV_TOL_MG, AOC_DWELL_HITS, AOC_COOLDOWN_MS,
           AOC_INPUT_QUIET_MS);

    scenarioClassify();
    scenarioDwell();
    scenarioSlowFlip();
    scenarioShake();
    scenarioCooldown();
    scenarioQuietWindow();
    scenarioDeskIdle();
    scenarioReset();

    printf("\n%d 项检查，失败 %d 项\n", g_checks, g_fail);
    if (g_fail) {
        printf("FAIL: 自适应方向判定核\n");
        return 1;
    }
    printf("PASS: 自适应方向判定核\n");
    return 0;
}
