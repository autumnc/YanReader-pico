#include "auto_orient.h"

#include <esp_log.h>
#include <esp_timer.h>

#include "auto_orient_core.h"
#include "board.h"
#include "ui_render.h"   // ui_render_invalidate：转屏后丢掉参考帧，下一帧整屏 GC16

static const char *TAG = "AutoOrient";

// 判定状态机（纯核，见 auto_orient_core.h）+ 最近一次采样（模式进入时要用它现判一次）。
static AutoOrientCore s_core;
static int64_t s_sample_us = 0;                 // 0 = 还没采过
static int s_sx = 0, s_sy = 0, s_sz = 0;
static bool s_active = false;                   // 当前界面允不允许跟
static int s_pending = AOC_UNKNOWN;             // 判定核给出的、还没落地的目标档
static int s_last_log = AOC_UNKNOWN;            // 上次打过的判定（同一档不重复打）

// 当前实际朝向（判定核的 cur）。
static int curState() { return board_is_portrait() ? AOC_PORTRAIT : AOC_LANDSCAPE; }

static const char *name(int o) {
    switch (o) {
        case AOC_PORTRAIT:  return "竖屏";
        case AOC_LANDSCAPE: return "横屏";
        default:            return "判不出";
    }
}

void auto_orient_on_sample(int x_mg, int y_mg, int z_mg, int64_t now_us) {
    s_sample_us = now_us;
    s_sx = x_mg;
    s_sy = y_mg;
    s_sz = z_mg;
    s_pending = aoc_feed(&s_core, curState(), x_mg, y_mg, z_mg, now_us);

    // 判定变了就打一行。**只在跟随时打**（不然谁在别的模式里翻一下机器都刷一行日志），
    // 而第一次上机标定 kPortraitIsXAxis 那一位正好就是在跟着的模式里做的：
    // 竖着拿应该打"竖屏"，若打出"横屏"就把 core 里那个常量翻过来。
    if (s_active) {
        const int r = aoc_classify(x_mg, y_mg, z_mg);
        if (r != s_last_log) {
            s_last_log = r;
            ESP_LOGI(TAG, "x=%d y=%d z=%d → %s", x_mg, y_mg, z_mg, name(r));
        }
    }
}

bool auto_orient_tick(bool active, int64_t last_input_us) {
    if (!active) {
        if (s_active) {   // 刚离开能跟的界面：清干净，别把攒的 dwell 带到下一次
            aoc_reset(&s_core);
            s_active = false;
        }
        s_pending = AOC_UNKNOWN;
        return false;
    }
    if (!s_active) {   // 刚进（模式/设置改动）：干净开始，方向由模式进入时那一次定
        s_active = true;
        aoc_reset(&s_core);
        s_pending = AOC_UNKNOWN;
        return false;
    }

    const int64_t now = esp_timer_get_time();
    const int want = aoc_should_apply(s_pending, now, last_input_us);
    if (want == 0) return false;

    const bool portrait = (want == AOC_PORTRAIT);
    if (board_is_portrait() == portrait) {   // 已经是这一档（谁先转了）：销账
        s_pending = AOC_UNKNOWN;
        return false;
    }
    ESP_LOGI(TAG, "自适应转屏 → %s", name(want));
    board_rotate_live(portrait);
    ui_render_invalidate();   // 参考帧作废：下一帧整屏 GC16 把画面拉正（约 1.8s）。
                              // 就是 ui_invalidate_snapshot()，转屏这条路上的老写法。
    aoc_note_rotated(&s_core, now);
    s_pending = AOC_UNKNOWN;
    s_last_log = AOC_UNKNOWN;   // 转完下一拍重新打一行（标定时能看清转到了哪）
    return true;                // 转是转了，画面还得调用方催一帧（见 .h）
}

bool auto_orient_initial(bool *portrait) {
    if (s_sample_us == 0) return false;
    // 采样太旧就别用：休眠回来、或"开机到现在还没轮到一次采样"的那一段，旧值代表的
    // 是上一个姿势。宁可保持原样（跟随全局方向），也不按一个陈旧姿势硬转。
    if (esp_timer_get_time() - s_sample_us > 1000 * 1000) return false;
    const int r = aoc_classify(s_sx, s_sy, s_sz);
    if (r == AOC_UNKNOWN) return false;
    if (portrait) *portrait = (r == AOC_PORTRAIT);
    return true;
}
