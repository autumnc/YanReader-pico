#include "input.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <stdio.h>

#include "auto_orient.h"    // 自适应方向：同一次加速度采样喂它一份（见 shake_poll）
#include "board_hw.h"
#include "cst836u.h"
#include "pjournal_app.h"  // KEY_UP/DOWN/LEFT/RIGHT/…
#include "read_pico_pmu.h"

static const char *TAG = "Input";

// ── 3 电容键：面板原生竖屏坐标系下，显示区下方那条触摸感应带。──
// 实测中心 x=80/240/400，y≈1500；显示区最大 y=1216，1300 作为分界。
// 键的物理位置不随显示旋转变化，所以用原生坐标命中即可（与方向无关）。
// 语义：KEY1(左,x<160)=下移，KEY2(中,160~320)=确认，KEY3(右,x≥320)=上移。
#define KEY_AREA_TOP 1300
#define KEY_PITCH    160

static int key_hit_test(uint16_t x, uint16_t y) {
    if (y < KEY_AREA_TOP) return -1;   // 显示区
    if (x < KEY_PITCH) return PICO_KEY_BOOT;   // KEY1(左) → 下移
    if (x < KEY_PITCH * 2) return 2;           // KEY2(中) → 确认
    return PICO_KEY_USER;                       // KEY3(右) → 上移
}

// 电容键保持状态。CST836U 硬件已消抖，这里读到即算按下。
// 曾用 2 帧消抖，但主循环 ~10Hz 轮询下 3 帧=300ms，快按会被吞掉 → 改为 0 帧。
static bool s_held[2] = {false, false};
static int s_held_cnt[2] = {0, 0};
#define HELD_DEBOUNCE 0

static void update_key_held(cst836u_touch_t *touch) {
    bool raw[2] = {false, false};
    if (touch->touched) {
        for (uint8_t i = 0; i < touch->count && i < CST836U_MAX_POINTS; i++) {
            int k = key_hit_test(touch->points[i].x, touch->points[i].y);
            if (k >= 0) {
                // 诊断：仅坐标变化时打印一次，确认按键条坐标范围与命中结果。
                static uint16_t last_kx = 0xFFFF, last_ky = 0xFFFF;
                if (touch->points[i].x != last_kx || touch->points[i].y != last_ky) {
                    ESP_LOGD(TAG, "key strip raw=(%u,%u) key=%d",
                             touch->points[i].x, touch->points[i].y, k);
                    last_kx = touch->points[i].x;
                    last_ky = touch->points[i].y;
                }
            }
            if (k == PICO_KEY_USER) raw[0] = true;
            else if (k == PICO_KEY_BOOT) raw[1] = true;
        }
    }
    for (int i = 0; i < 2; i++) {
        if (raw[i]) {
            if (s_held_cnt[i] < HELD_DEBOUNCE) s_held_cnt[i]++;
            else s_held[i] = true;
        } else {
            s_held_cnt[i] = 0;
            s_held[i] = false;
        }
    }
}

// 最近一次显示区点按的逻辑坐标（供主界面图标点选）。读后清除。
static bool s_tap_valid = false;
static int s_tap_lx = 0, s_tap_ly = 0;

// 最近一次拖动的位移增量（相对上一次上报位置，正 y = 手指下移）。读后清除。
static bool s_drag_valid = false;
static int s_drag_dx = 0, s_drag_dy = 0;

// 最近一次手势抬手时，按下点是否可读。坐标直接用 s_press_lx/s_press_ly（按下时记下
// 就不再改动的那一份）。和 s_tap_valid 一样是"只对产生它的那一帧有效"：input_poll()
// 开头清掉，阻塞段补采时随按键一起排队。
static bool s_press_origin_valid = false;

// 伴随 KEY_BACK 的方向（+1 手指左滑 / -1 手指右滑）。同 s_press_origin_valid，
// 只对产生它的那一帧有效。
static bool s_back_valid = false;
static int s_back_dir = 1;

bool input_key_held(int key) {
    if (key < 0 || key > 1) return false;
    return s_held[key];
}

bool input_tap_xy(int *x, int *y) {
    if (!s_tap_valid) return false;
    if (x) *x = s_tap_lx;
    if (y) *y = s_tap_ly;
    s_tap_valid = false;
    return true;
}

bool input_tap_peek_xy(int *x, int *y) {
    if (!s_tap_valid) return false;
    if (x) *x = s_tap_lx;
    if (y) *y = s_tap_ly;
    return true;
}

bool input_drag_xy(int *x, int *y) {
    if (!s_drag_valid) return false;
    if (x) *x = s_drag_dx;
    if (y) *y = s_drag_dy;
    s_drag_valid = false;
    return true;
}

bool input_back_dir(int *dir) {
    if (!s_back_valid) return false;
    if (dir) *dir = s_back_dir;
    s_back_valid = false;
    return true;
}

void input_init() {
    // 触摸/PMU 已在 read_pico_init 初始化；这里只清状态。
    s_held[0] = s_held[1] = false;
    s_held_cnt[0] = s_held_cnt[1] = 0;
}

// ── 显示区触摸手势（简化）：点按=确认、上下滑=翻页、左右滑=左右。──
// 手势坐标随显示方向旋转；这里按当前 rotation 把原生坐标映射到逻辑坐标。
static void touch_to_logical(uint16_t x, uint16_t y, int *lx, int *ly) {
    switch (epd_get_rotation()) {
        case EPD_ROT_INVERTED_PORTRAIT:  // 竖屏 684×1216（原生即竖屏，几乎不转）
            *lx = x;
            *ly = y;
            break;
        case EPD_ROT_LANDSCAPE:          // 横屏 1216×684
        default:
            // 原生(684×~1600) → 横屏逻辑(1216×684)：TODO 实机校准方向符号。
            *lx = (int)y;
            *ly = 684 - (int)x;
            break;
    }
}

static bool s_press_active = false;
static int64_t s_press_us = 0;
static int s_press_lx = 0, s_press_ly = 0;   // 按下点（逻辑坐标）
static int s_last_lx = 0, s_last_ly = 0;     // 最近有效点（逻辑坐标）
static uint16_t s_press_raw_x = 0, s_press_raw_y = 0;  // 调试用原始坐标
static bool s_long_fired = false;

// 按下点（见 input.h）。定义放在这里是因为坐标取自上面的 s_press_lx/ly。
bool input_press_xy(int *x, int *y) {
    if (!s_press_origin_valid) return false;
    if (x) *x = s_press_lx;
    if (y) *y = s_press_ly;
    s_press_origin_valid = false;
    return true;
}

// 补采的时间闸：一次 I2C 读约 0.5ms，12ms 一拍足够把任何一段长活切成 ≤12ms 的碎片，
// 代价是每 100ms 只花 4ms。见 input.h 的 input_tick_throttled。
static const int64_t INPUT_TICK_THROTTLE_US = 12000;
static int64_t s_throttle_tick_us = 0;

// 按下沿（见 input.h）：一次性交接，读走就清。坐标在手势状态机建立按下点时一起记。
static bool s_press_edge_pending = false;
static int s_press_edge_lx = 0, s_press_edge_ly = 0;
static uint32_t s_press_seq = 0;
bool input_press_edge_xy(int *x, int *y) {
    if (!s_press_edge_pending) return false;
    s_press_edge_pending = false;
    if (x) *x = s_press_edge_lx;
    if (y) *y = s_press_edge_ly;
    return true;
}
uint32_t input_press_seq() { return s_press_seq; }
void input_press_edge_discard() {
    s_press_edge_pending = false;
}

// ── 显示区拖动（按住移动）──────────────────────────────────────────────
// 手指按住并移动超过 DRAG_MIN_PX 后进入"拖动"：此后每次位置变化都上报一次
// 增量（相对上次上报的位置，不是相对按下点），由 KEY_TOUCH_DRAG 携带，
// input_drag_xy() 取走。拖动期间不再触发长按(0x1B)——长按的语义是"按住不动"，
// 手指已经动了就不该再算长按，否则慢慢拖到 600ms 会中途蹦出一个 Esc。
// 抬手时的点按/滑动判定保持原样（别的界面靠它翻页），拖动结束后的那一次
// 翻页键由计划模式自己按"刚拖过"的状态吃掉。
static bool s_dragging = false;
static int s_drag_last_lx = 0, s_drag_last_ly = 0;   // 上次上报增量时的位置
#define DRAG_MIN_PX   18

// "拖动过就不算点按"有个例外：手指点一下蹭出去 2~3mm 再回来，抬手点几乎还在
// 按下点上，这是**点按**不是拖拽；而真正的拖拽不会回到原点。所以拖过的手势
// 只要抬手点落在这个小半径内，仍然算点按。不加这条的话，一个抖动的点按既
// 上不了屏、在计划模式里还会被当成滚屏吃掉，手感就是"点了没反应"。
#define TAP_RETURN_PX  8

#define SWIPE_MIN_PX   40
#define LONG_PRESS_MS  600
// 边缘返回手势的判定带宽（逻辑像素，按触摸**按下点**量，与屏幕宽无关）。
// 约 13mm：够顺手，又不会把正文区的横划误吞成返回。
#define EDGE_SWIPE_PX  80

// ── 双指捏合 ────────────────────────────────────────────────────────────
// CST836U 最多同时报 2 个点（CST836U_MAX_POINTS）。两指都在显示区里就是捏合：
// 记下初始间距，之后间距每变化 PINCH_STEP_PX 就发一个 KEY_PINCH_IN/OUT。
//
// 单指状态机在两指期间必须**整体挂起**，而且挂起要一直持续到**全部手指抬起**——
// 两指是"先落一根再补一根"，第二根落下/抬起都会各喂进一次按下/抬手，凭空甩出一个
// 点按或翻页键（在图片查看器里就是"捏一下顺带换了张图"）。所以用 s_pinch_block
// 做闸：进过捏合态就一直关着，直到显示区一个点都不剩才放行。
static bool s_pinch_latched = false;      // 本次两指手势是否已建立基准
static bool s_pinch_block = false;        // 两指用过之后，等全部抬手才放行单指状态机
static int s_pinch_last_dist = 0;
#define PINCH_MIN_DIST_PX 40              // 两指太近不算捏合（避免误判成一指在抖）
#define PINCH_STEP_PX     24              // 间距每变化这么多发一个键

// 整数平方根（二分开方）。屏幕坐标差值最多几百，9~10 次就收敛；上限 4096 是留给
// 晃动检测的三轴平方和（≤1.2e7）用的，两处共用一个。
static int isqrt_i32(int v) {
    if (v <= 0) return 0;
    int lo = 0, hi = 4096;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (mid * mid <= v) lo = mid; else hi = mid - 1;
    }
    return lo;
}

// 触摸显示区的点击/滑动/长按 → 键码；无新手势返回 0。
// 长按在按住期间触发一次；点按/滑动在抬手时判定（用最近有效点，而不是按下点）。
static int poll_touch_gesture(cst836u_touch_t *touch) {
    // 取第一个显示区内的触摸点（按键条之外的点），并数一下显示区里一共几个点。
    bool on_display = false;
    int disp_pts = 0;
    uint16_t px = 0, py = 0;
    uint16_t px2 = 0, py2 = 0;
    if (touch->touched) {
        for (uint8_t i = 0; i < touch->count && i < CST836U_MAX_POINTS; i++) {
            // 只数**活着的**点：驱动把抬起(event==3)和 id>1 的槽位标成 active=false，
            // 但 count 仍可能把空槽算进去（CST836U 的 count 是上报的触点数）。
            // 不滤掉的话"两指抬手一根"会被当成还在捏合，间距一路乱跳。
            if (!touch->points[i].active) continue;
            if (key_hit_test(touch->points[i].x, touch->points[i].y) < 0) {
                if (disp_pts == 0) {
                    on_display = true;
                    px = touch->points[i].x;
                    py = touch->points[i].y;
                } else if (disp_pts == 1) {
                    px2 = touch->points[i].x;
                    py2 = touch->points[i].y;
                }
                disp_pts++;
            }
        }
    }

    int64_t now = esp_timer_get_time();
    int key = 0;

    if (disp_pts == 0 && s_pinch_block) {
        // 全部抬手：放行单指状态机。同时清干净，免得残留的按下点被下一次
        // "抬手"判定当成点按（那时 s_press_active 早已被捏合清掉）。
        s_pinch_block = false;
        s_pinch_latched = false;
        s_press_active = false;
        s_dragging = false;
        s_long_fired = false;
        s_press_edge_pending = false;
    }

    if (disp_pts >= 2) {
        const int ddx = (int)px - (int)px2;
        const int ddy = (int)py - (int)py2;
        const int dist = isqrt_i32(ddx * ddx + ddy * ddy);
        if (!s_pinch_latched || dist < PINCH_MIN_DIST_PX) {
            // 建立/重建基准。顺手把单指状态机清掉：两指落下前若已有一根手指按着，
            // 它已经跑过"按下"了，留着就会在抬手时按那个旧按下点判滑动/点按。
            s_pinch_latched = true;
            s_pinch_block = true;
            s_pinch_last_dist = dist;
            s_press_active = false;
            s_dragging = false;
            s_long_fired = false;
            // 单指状态机被捏合接管，那个还没被读走的按下沿也一并作废 —— 否则它会作为
            // "刚按下"喂给虚拟键盘的按下反馈，给一个正在做缩放手势的指头反白一个键。
            s_press_edge_pending = false;
        } else {
            const int d = dist - s_pinch_last_dist;
            if (d >= PINCH_STEP_PX) {
                s_pinch_last_dist = dist;
                key = KEY_PINCH_OUT;   // 两指张开 = 放大
            } else if (d <= -PINCH_STEP_PX) {
                s_pinch_last_dist = dist;
                key = KEY_PINCH_IN;    // 两指靠拢 = 缩小
            }
        }
        return key;   // 两指期间一律不理单指手势（也不理电容键）
    }
    s_pinch_latched = false;

    if (on_display && !s_pinch_block) {
        if (!s_press_active) {
            s_press_active = true;
            s_long_fired = false;
            s_press_us = now;
            s_press_raw_x = px;
            s_press_raw_y = py;
            touch_to_logical(px, py, &s_press_lx, &s_press_ly);
            s_last_lx = s_press_lx;
            s_last_ly = s_press_ly;
            // 按下沿（见 input.h）：坐标要等 touch_to_logical 算完才能记。
            s_press_edge_pending = true;
            s_press_edge_lx = s_press_lx;
            s_press_edge_ly = s_press_ly;
            s_press_seq++;
        } else {
            touch_to_logical(px, py, &s_last_lx, &s_last_ly);
            int dx = s_last_lx - s_press_lx;
            int dy = s_last_ly - s_press_ly;
            int64_t dt_ms = (now - s_press_us) / 1000;
            // 移动超过阈值就进入拖动：以按下点为基准播下"上次上报位置"，
            // 于是这一帧的首个增量已经包含按下到现在的全部位移。
            if (!s_dragging && !s_long_fired && (abs(dx) >= DRAG_MIN_PX || abs(dy) >= DRAG_MIN_PX)) {
                s_dragging = true;
                s_drag_last_lx = s_press_lx;
                s_drag_last_ly = s_press_ly;
            }
            if (s_dragging) {
                int ddx = s_last_lx - s_drag_last_lx;
                int ddy = s_last_ly - s_drag_last_ly;
                if (ddx != 0 || ddy != 0) {
                    s_drag_last_lx = s_last_lx;
                    s_drag_last_ly = s_last_ly;
                    s_drag_dx = ddx;
                    s_drag_dy = ddy;
                    s_drag_valid = true;
                    key = KEY_TOUCH_DRAG;
                    // 拖动帧也发布按下点：阅读模式要在"手指刚拖起来"的那一帧判断
                    // 抓的是哪个选区手柄（手还没离开按下点，就近认），光有增量认不出。
                    // 与抬手产生的键同一套语义——只对产生它的这一帧有效。
                    s_press_origin_valid = true;
                }
            }
            // 长按：按住不动超过阈值立即触发一次（不等抬手）。
            if (!s_long_fired && !s_dragging && dt_ms >= LONG_PRESS_MS
                && abs(dx) < SWIPE_MIN_PX && abs(dy) < SWIPE_MIN_PX) {
                s_long_fired = true;
                ESP_LOGD(TAG, "long press fired");
                // 长按 = 菜单/返回，并把按下点带上：要按位置弹菜单的界面（计划模式
                // 列表）用 input_tap_xy() 取。主循环会把其它界面的它翻回 0x1B。
                key = KEY_TOUCH_LONG;
                s_tap_lx = s_press_lx;
                s_tap_ly = s_press_ly;
                s_tap_valid = true;
                s_press_origin_valid = true;
            }
        }
    } else if (s_press_active) {
        int dx = s_last_lx - s_press_lx;
        int dy = s_last_ly - s_press_ly;
        int adx = abs(dx), ady = abs(dy);
        int64_t dt_ms = (now - s_press_us) / 1000;

        if (!s_long_fired) {
            // 点按：抬手前没动过、也没触发长按（长按已在按住期间触发）。
            // 拖动过的手势一般不算点按——拖了 20px 又被当成点按，会顺手把行点开；
            // 但抬手点仍在按下点附近（TAP_RETURN_PX 内）时算点按：那是抖动，不是拖拽。
            if (adx < SWIPE_MIN_PX && ady < SWIPE_MIN_PX) {
                if (!s_dragging || (adx <= TAP_RETURN_PX && ady <= TAP_RETURN_PX)) {
                    key = '\n';  // 点按 = 确认/回车
                    s_tap_lx = s_press_lx;
                    s_tap_ly = s_press_ly;
                    s_tap_valid = true;
                }
            } else if (ady >= adx) {
                // 上下滑 = 整页滚动：手指上滑→KEY_PAGE_DOWN(选项单向下滚看后面)，
                // 手指下滑→KEY_PAGE_UP(选项单向上滚看前面)。设置界面按页滚动，
                // 其余界面主循环回退为单步。dy>0 表示手指向下(y 增大)。
                key = (dy > 0) ? KEY_PAGE_UP : KEY_PAGE_DOWN;
            } else {
                // 手指左滑 = 内容右移(下一个)，右滑 = 左移(上一个)。
                key = (dx < 0) ? KEY_RIGHT : KEY_LEFT;
                // 从**屏幕边缘**往中间划 = 返回（三种模式通用）。判定用按下点：
                // 只有从边缘带起步的横划才算返回，从中间起划的照旧是翻页/移光标。
                // 屏幕宽用旋转后的逻辑宽（横/竖屏各算一次）。
                const int w = epd_rotated_display_width();
                const bool from_left = (dx > 0 && s_press_lx <= EDGE_SWIPE_PX);
                const bool from_right = (dx < 0 && s_press_lx >= w - EDGE_SWIPE_PX);
                if (from_left || from_right) {
                    key = KEY_BACK;
                    s_back_dir = (dx < 0) ? +1 : -1;   // +1 = 手指左滑，同 KEY_RIGHT 约定
                    s_back_valid = true;
                }
            }
        }
        // 抬手产生的按键（点按/上下滑/左右滑）都带上按下点：滑动那两种键本身
        // 不带坐标，要按"从哪儿开始划"区分语义的界面（虚拟键盘候选行）靠它。
        if (key != 0) s_press_origin_valid = true;
        ESP_LOGD(TAG, "gesture raw=(%u,%u) logical=(%d,%d)->(%d,%d) dt=%lld key=%d",
                 s_press_raw_x, s_press_raw_y,
                 s_press_lx, s_press_ly, s_last_lx, s_last_ly,
                 (long long)dt_ms, key);
        s_press_active = false;
        s_long_fired = false;
        s_dragging = false;
    }

    // 电容键 KEY2(中间)：按下→释放算一次确认；按住 ≥600ms 触发一次"长按确认"。
    static bool key3_was_down = false;
    static int64_t key3_down_us = 0;
    static bool key3_long_fired = false;
    bool key3_down = false;
    if (touch->touched) {
        for (uint8_t i = 0; i < touch->count && i < CST836U_MAX_POINTS; i++) {
            if (key_hit_test(touch->points[i].x, touch->points[i].y) == 2) key3_down = true;
        }
    }
    if (key3_down && !key3_was_down) {
        key3_down_us = now;
        key3_long_fired = false;
    } else if (key3_down && !key3_long_fired && (now - key3_down_us) >= LONG_PRESS_MS * 1000) {
        key3_long_fired = true;
        ESP_LOGD(TAG, "KEY2 long press → standby");
        key = KEY_LONG_CONFIRM;  // 长按中间键 = 待机（main.cpp 全局处理）
    }
    if (key3_was_down && !key3_down && key == 0 && !key3_long_fired) {
        ESP_LOGD(TAG, "KEY3 release → confirm");
        key = '\n';  // KEY2 释放 = 确认
    }
    key3_was_down = key3_down;

    return key;
}

// ── 阻塞段补采样 ─────────────────────────────────────────────────────────
// input_tick() 在整屏重绘/刷屏这类长阻塞段里被调用，采到的按键暂存在这里，
// 下一次 input_poll() 取走。按下的点按坐标也要跟着按键一起存，
// 否则 input_tap_xy() 拿不到那几个图标的点按位置。
static int s_pend_key = 0;
static bool s_pend_tap = false;
static int s_pend_tx = 0, s_pend_ty = 0;
static bool s_pend_drag = false;
static int s_pend_dx = 0, s_pend_dy = 0;
static bool s_pend_press = false;
static bool s_pend_back = false;
static int s_pend_dir = 1;

void input_tick() {
    if (g_hw.touch == NULL) return;
    if (s_pend_key != 0) return;   // 已有一个待发按键，别再采（不覆盖、不丢弃）
    cst836u_touch_t touch = {};
    if (cst836u_read(g_hw.touch, &touch) != ESP_OK) return;
    update_key_held(&touch);
    int k = poll_touch_gesture(&touch);
    if (k != 0) {
        s_pend_key = k;
        s_pend_tap = s_tap_valid;
        s_pend_tx = s_tap_lx;
        s_pend_ty = s_tap_ly;
        s_tap_valid = false;
        s_pend_press = s_press_origin_valid;
        s_press_origin_valid = false;
        // 拖动增量也要跟着按键一起存：漏掉一帧的过程量倒不至于错，
        // 但既然按键都在排队，增量一起排队才是完整的。
        s_pend_drag = s_drag_valid;
        s_pend_dx = s_drag_dx;
        s_pend_dy = s_drag_dy;
        s_drag_valid = false;
        // 边缘返回的方向也一起排队（同 KEY_BACK 一起取出）。
        s_pend_back = s_back_valid;
        s_pend_dir = s_back_dir;
        s_back_valid = false;
    }
}

int input_pending_key() {
    input_tick();
    return s_pend_key;
}

void input_tick_throttled() {
    const int64_t now = esp_timer_get_time();
    if (now - s_throttle_tick_us < INPUT_TICK_THROTTLE_US) return;
    s_throttle_tick_us = now;
    input_tick();
}

bool input_pending_tap_xy(int *x, int *y) {
    if (s_pend_key == 0 || !s_pend_tap) return false;
    if (x) *x = s_pend_tx;
    if (y) *y = s_pend_ty;
    return true;
}

// ── 晃动机身 = 一次全刷 ──────────────────────────────────────────────────
// SC7A20H 平时是掉电的（read_pico_init 校验完就 power_down），这里重新上电、常驻在
// ODR 12.5Hz 的低功耗档（~2µA），每 ~120ms 读一次三轴。
//
// 判据用**合加速度的模**，不用单轴：模与姿态无关，所以"把机器从平放慢慢翻成竖放"
// 这种手上动作不会误触发（|a| 全程 ≈1g），甩一下才会让它冲出 1g 一大截。再用相邻
// 两拍之间的跳变兜一道：慢动作两样都不超，快甩两样都超，连续 SHAKE_HOT_HITS 拍
// 才算数（单拍毛刺不算）。
//
// 一次全刷要 1.8s，触发后进冷却期；冷却期内照常采样（只是不判），目的是把基线刷
// 新掉——否则冷却一结束，那一拍会拿"1.8s 前的旧值"当跳变又点着一次。
//
// 阈值按"随手晃一下能认出来、搁到桌上不误触"定的：ODR 只有 12.5Hz（80ms 一个点）、
// LP 档也没开滤波，所以要求**连续两拍都热**——一次刻意晃动通常有 150ms 以上 |a| 超过
// 1.4g，能连上两拍；把机器搁到桌上那种一下一下的冲击多半只热一拍，不算。若实测
// "晃了没反应"，先降 SHAKE_DEV_MG（400 → 300），再考虑把 ODR 提到 50Hz
// （sc7a20h_apply_config 一次即可，power_up 会记住上次的配置）。
#define SHAKE_POLL_MS     120     // 采样周期（ODR 12.5Hz ≈ 80ms 一个点）
#define SHAKE_DEV_MG      400     // |a| 偏离 1g 超过这么多算"这一拍是热的"
#define SHAKE_STEP_MG     600     // 相邻两拍 |a| 的跳变超过这么多也算热
#define SHAKE_HOT_HITS    2       // 连续这么多拍热才算一次晃动
#define SHAKE_COOLDOWN_MS 3000    // 触发后的静默期

// 该发一次全刷时返回 true。没有加速度计（sensor_ready=false）时恒 false：功能静默
// 失效，不影响任何别的键。
static bool shake_poll() {
    static bool s_woke = false;
    static int64_t s_next_us = 0;
    static int64_t s_fire_us = 0;
    static uint32_t s_last_mag = 1000;
    static int s_hot = 0;
    if (!g_hw.sensor_ready || g_hw.sensor == NULL) return false;
    if (!s_woke) {
        read_pico_sensor_wake(g_hw.sensor);
        s_woke = true;
    }

    const int64_t now = esp_timer_get_time();
    if (now < s_next_us) return false;
    s_next_us = now + (int64_t)SHAKE_POLL_MS * 1000;

    sc7a20h_sample_t s = {};
    if (sc7a20h_read(g_hw.sensor, &s) != ESP_OK) return false;
    // 同一次读再喂一份给自适应方向（写作/计划模式那档「自适应」，见 hw/auto_orient.h）。
    // 它要的是**设备帧**（平放屏幕朝上为 +Z，见 read_pico_init.h），而下面 shake 的判据
    // 用的是合加速度的模 —— 与坐标系无关，所以这边原本读的是芯片原始帧。换算只改这一份
    // 副本，不动 shake 的口径；一次 I2C 读两个消费者，没有额外开销。
    // 放在这里等于两个调用点（input_poll 里触摸可用/不可用两条路）都接上了。
    {
        sc7a20h_sample_t ds = s;
        read_pico_accel_to_device(&ds);
        auto_orient_on_sample(ds.x_mg, ds.y_mg, ds.z_mg, now);
    }
    // 量程 ±2g，单轴最大 2000mg，三轴平方和 ≤ 1.2e7，isqrt_i32 的 4096 上限够用。
    const int mag2 = (int)s.x_mg * s.x_mg + (int)s.y_mg * s.y_mg + (int)s.z_mg * s.z_mg;
    const uint32_t mag = (uint32_t)isqrt_i32(mag2);
    const uint32_t dev = mag > 1000 ? mag - 1000 : 1000 - mag;
    const uint32_t step = mag > s_last_mag ? mag - s_last_mag : s_last_mag - mag;
    s_last_mag = mag;

    // 冷却期：取样、刷基线，但不判不发。
    if (now - s_fire_us < (int64_t)SHAKE_COOLDOWN_MS * 1000) {
        s_hot = 0;
        return false;
    }
    if (dev >= SHAKE_DEV_MG || step >= SHAKE_STEP_MG) {
        if (++s_hot >= SHAKE_HOT_HITS) {
            s_hot = 0;
            s_fire_us = now;
            ESP_LOGI(TAG, "晃动 → 全刷 (|a|=%umg 偏离=%u 跳变=%u)", (unsigned)mag,
                     (unsigned)dev, (unsigned)step);
            return true;
        }
    } else {
        s_hot = 0;
    }
    return false;
}

// ── 唤醒那一按的"抬手"要吞掉 ────────────────────────────────────────────────
// 待机时按电源键：KEY_DOWN 把设备叫醒（enterLightSleep 里 take_key_wakeup 已经 ack），
// 松手时 PMU 再补一个 KEY_SHORT —— 那个事件是**醒着的时候**才进队列的，若不拦就变成
// "按一下电源键唤醒，顺带把模式转了一圈"（用户在待机画面按醒，回来发现换模式了）。
// 登记时刻由 enterLightSleep 给（input_note_key_wake），这里只判时间窗。
//
// 窗口 2s：唤醒路径是 epd_poweron + epd_clear + 恢复快照（约 1s 起步），用户一般在
// 屏幕亮起来的瞬间松手，抬手事件稳稳落在窗内。窗口外一律照常上报 —— 真想切模式，
// 再按一下就行（按一次被吞是可恢复的，反过来漏判就是"永远切不动/莫名切"的毛病）。
static bool s_wake_swallow = false;
static int64_t s_wake_us = 0;
#define WAKE_SWALLOW_US (2000 * 1000LL)

void input_note_key_wake() {
    s_wake_swallow = true;
    s_wake_us = esp_timer_get_time();
}

// 扫一遍 PMU 事件队列，把电源键的两个意图抽出来：
//   短按                       → KEY_POWER（切模式）
//   长按满 8s（KEY_FORCE_OFF） → KEY_POWER_HOLD（要关机了，抢在硬断电前铺关机页）
// read_pico_pmu_take_key_short() 只认短按、其余事件一律静默 ack 丢掉 —— 8s 的关机警告
// 就是这么丢的，等 PMU 满 10s 直接拉 EN 时主机已经没法铺画面了。所以自己走一遍队列
// （结构与 take_key_short 相同：poll → 读队首事件 → ack），多认一个 KEY_FORCE_OFF。
static int poll_pmu_key() {
    bool short_press = false, force_off = false;
    // **只读事件**，不要全量快照（read_pico_pmu_poll）。这一句在**每一轮主循环**都跑，
    // 而全量快照是五路寄存器读 + 身份/状态校验重试（每次重试 20ms 死等），实测整段
    // 126~148ms —— 那段时间 cst836u 一次都采不到样，一次快按整个掉进去就是"按了没反应"。
    // 事件是 FIFO，攒着不会丢（见 read_pico_pmu_poll_events 的说明）。
    for (int i = 0; i < PMU_EVENT_FIFO_DEPTH; i++) {
        if (read_pico_pmu_poll_events() != ESP_OK) break;
        const pmu_snapshot_t *s = read_pico_pmu_get();
        if (s == NULL || !s->event_ok || s->pending_events == 0) break;
        const uint16_t id = s->event.event_id;
        if (id == 0) break;
        const uint8_t type = s->event.type;
        if (type == PMU_EVT_KEY_SHORT) {
            if (s_wake_swallow && esp_timer_get_time() - s_wake_us < WAKE_SWALLOW_US) {
                // 唤醒那一按的抬手（见上面 input_note_key_wake 的说明）：只唤醒，不切模式。
                ESP_LOGI(TAG, "吞掉唤醒那一按的抬手 id=%u held=%u（待机唤醒不切模式）", (unsigned)id,
                         (unsigned)s->event.arg0);
            } else {
                if (s_wake_swallow) {
                    // **登记过却超时** = 这一按被判成"唤醒之后的另一下"，会去切模式。
                    // 把间隔和按键时长都打出来：窗口只差一点就说明还得再放宽，差很远就是
                    // 登记点/唤醒路径另有问题（正常情况下这条不该出现）。
                    ESP_LOGW(TAG, "唤醒抬手超出窗 %lldms（窗 %lldms, held=%u）→ 当短按上报",
                             (long long)((esp_timer_get_time() - s_wake_us) / 1000),
                             (long long)(WAKE_SWALLOW_US / 1000), (unsigned)s->event.arg0);
                }
                short_press = true;
                ESP_LOGI(TAG, "KEY_SHORT id=%u held=%u", (unsigned)id, (unsigned)s->event.arg0);
            }
            s_wake_swallow = false;   // 只认一次；超时的登记到这里也顺手作废
        } else if (type == PMU_EVT_KEY_FORCE_OFF) {
            force_off = true;
            ESP_LOGW(TAG, "KEY_FORCE_OFF id=%u held=%u ms → 关机预警", (unsigned)id,
                     (unsigned)s->event.arg0);
        }
        if (read_pico_pmu_event_ack(id) != ESP_OK) break;
    }
    if (force_off) return KEY_POWER_HOLD;   // 优先：已经在关机路上了
    if (short_press) return KEY_POWER;
    return 0;
}

// 丢掉排队中的待发键与它随身的那一份手势状态（换屏时调，见 main.cpp 的派发之后）。
// 补采样（editor_vk.cpp 的 evkKey）会在**屏幕正重画**的时候采到按键并暂存，而那次
// 重画之后界面可能已经切走了 —— 这个键属于旧界面，投给新界面就是一次凭空多出来的按键
// （阅读器里翻一页、主菜单里选中一项）。今天只有补采样会在重画途中暂存按键，所以这条
// 不是防抖，是给"输入采样的时机"和"界面归属"重新对上号。
void input_flush_pending() {
    s_pend_key = 0;
    s_pend_tap = false;
    s_pend_drag = false;
    s_pend_press = false;
    s_pend_back = false;
    s_tap_valid = false;
    s_drag_valid = false;
    s_press_origin_valid = false;
    s_back_valid = false;
}

int input_poll() {
    // 点按坐标仅当帧有效：本帧未消费就丢弃，避免陈旧坐标被后续回车误当点选。
    s_tap_valid = false;
    // 拖动增量同理：只对产生它的那一帧有意义。
    s_drag_valid = false;
    // 手势按下点同理：本帧没有新手势抬手就不该有值——否则上一次点按的坐标
    // 会被后来某个 BLE 键盘的左右键读到，误当成"从候选行划了一下"。
    s_press_origin_valid = false;
    // 边缘返回的方向同理（只在 KEY_BACK 那一帧有意义）。
    s_back_valid = false;
    if (g_hw.touch == NULL) {
        // 电源键短按仍可用（PMU 独立于触摸）→ 写作/阅读模式切换；长按满 8s → 关机预警。
        const int pk = poll_pmu_key();
        if (pk != 0) return pk;
        return shake_poll() ? KEY_SHAKE : 0;
    }

    // 阻塞段里补采到的按键优先返回（附带它在按下时记下的点按坐标）。
    if (s_pend_key != 0) {
        int k = s_pend_key;
        s_pend_key = 0;
        if (s_pend_tap) {
            s_tap_lx = s_pend_tx;
            s_tap_ly = s_pend_ty;
            s_tap_valid = true;
            s_pend_tap = false;
        }
        if (s_pend_drag) {
            s_drag_dx = s_pend_dx;
            s_drag_dy = s_pend_dy;
            s_drag_valid = true;
            s_pend_drag = false;
        }
        s_press_origin_valid = s_pend_press;
        s_pend_press = false;
        if (s_pend_back) {
            s_back_dir = s_pend_dir;
            s_back_valid = true;
            s_pend_back = false;
        }
        return k;
    }

    cst836u_touch_t touch = {};
    if (cst836u_read(g_hw.touch, &touch) != ESP_OK) {
        // 触摸读失败**不能**吞掉别的输入源。电源键走 PMU、晃动走 SC7A20H，是两个完全
        // 独立的器件；触摸控制器一条 I2C 读失败没有任何理由让它们一起失灵。原来这里
        // 直接 `return 0`（本函数最下面的电源键与 shake_poll 全都轮不到），症状就是
        // 2026-10-07 那次"一直在晃屏也激发不出全刷、一直在操作屏幕不理"，而主循环心跳
        // 照常 —— 主循环活着、输入死了，病因就在这一行。
        // **不调 update_key_held()**：拿不到"当前按下状态"就不该改账。把它当"手指抬了"
        // 会凭空合成一次抬起，误触发长按/点按；失败期间静默、恢复后自然用新的一拍校正。
        // 失败时 poll_touch_gesture() 也不调，所以 s_held[] 停在旧值也不会吐出任何键。
        // 限速记一行：cst836u 的读失败路径（cst836u.c:188/195）本身一行日志都不打，
        // 不在这里记就只能靠猜 —— 这正是这个 bug 能藏住的原因。
        static int64_t s_touch_fail_log_us = 0;
        static uint32_t s_touch_fail_n = 0;
        s_touch_fail_n++;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - s_touch_fail_log_us > 3000000) {
            ESP_LOGW(TAG, "触摸读失败 %u 次/3s → 本拍跳过手势，电源键/晃动照常",
                     (unsigned)s_touch_fail_n);
            s_touch_fail_log_us = now_us;
            s_touch_fail_n = 0;
        }
    } else {
        update_key_held(&touch);
        int key = poll_touch_gesture(&touch);
        if (key != 0) return key;
    }

    // 电源键：短按 → 写作/阅读模式切换；长按满 8s → 关机预警（见 poll_pmu_key）。
    const int pk = poll_pmu_key();
    if (pk != 0) return pk;

    // 晃动机身 → 全刷（非待机模式下才有这一拍：休眠时主循环停了，这里不会被调用）。
    if (shake_poll()) return KEY_SHAKE;

    return 0;
}
