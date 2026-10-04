/*
 * 渲染任务实现：差分 → 区域/波形决策 → epdiy 推屏。
 *
 * 线程模型
 * --------
 *   core0 (app 主任务)                        core1 (ui_render, prio 6)
 *   ui_clear()  ──取一块空闲缓冲──▶ 画 ──┐
 *                                        │ ui_commit() 投递，不等
 *                                        └──────▶ 差分 → 选波形 → 推屏 ──▶ 还缓冲
 *   两块缓冲轮流用，core0 最多领先两帧；两块都占住时在 ui_clear() 里阻塞（背压）。
 *
 * 差分基准是 epdiy 的 back_fb —— "上一次真正驱动到面板上的内容"，本来就是要比的
 * 对象，白拿一块 400KB，不用再维护帧快照。
 *
 * 推屏前把工作缓冲拷进 hl->front_fb（**不改指针**，理由见 copy_to_front）。
 * 阅读器不走这条路：它直接画 front_fb、直接调 epd_hl_update_screen，见文件里
 * copy_to_front 的注释。
 */
#include "ui_render.h"

#include "ui_helpers.h"   // SCREEN_W / SCREEN_H

#include "app_config.h"
#include "board.h"
#include "display.h"
#include "editor_vk.h"   // editorVkVisible/editorVkTop：虚拟键盘面板顶
#include "e0470_epaper_waveform.h"
#include "epdiy.h"
#include "ime/IME.h"   // g_ime.composing()（区域判定在 core0 侧算，见 ui_render_submit）
#include "u8g2_shim.h"

#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>   // esp_restart：渲染任务卡死时唯一的自救动作
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>   // xTaskCreatePinnedToCoreWithCaps：把栈放到 PSRAM
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

extern u8g2_t *g_u8g2;

static const char *TAG = "ui_render";

// epdiy 自己有两个 epd_prep 线程跑在 configMAX_PRIORITIES-1；渲染任务必须低于它们，
// 否则行打包抢不到 CPU，送行队列欠载（EPD_DRAW_EMPTY_LINE_QUEUE）。
#define UI_RENDER_CORE 1
#define UI_RENDER_PRIO 6
// 栈走 PSRAM（内部 RAM 常态只剩 ~12KB，这 12KB 是最大的一笔）。必须用
// xTaskCreatePinnedToCoreWithCaps 明确要 SPIRAM：xTaskCreate* 的栈**一定**落在
// 内部 RAM——IDF 的 pvPortMalloc 把 caps 硬编码成 MALLOC_CAP_INTERNAL，而
// CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 只管 xTaskCreateStatic。见 init。
#define UI_RENDER_STACK 12288
#define UI_RENDER_QUEUE_LEN 4
#define UI_RENDER_TAKE_MS 3000  // 取缓冲的兜底超时：异常时别把 core0 永久卡死
// 空闲轮询周期：只为让 rails_idle_check 有机会到点下电（8s 期限），别省成 portMAX_DELAY。
#define UI_RENDER_IDLE_MS 250

// 跟随 DU（8 帧，FAST 扫描）攒够这么多次就整屏 GC16 清一次残影。
#define FOLLOW_GC16_EVERY 8
// IME 候选/编码条局刷的合并窗口：窗口内的连续输入只推最后一次。
#define IME_DEFER_US 5000

// 打字期间**一次全刷都不做**：键区以前每 7 键把键盘矩形整块过一遍 8 灰阶 GL16 清残影，
// 结果就是打字打到一半莫名其妙闪一下键盘。现在清残影只落在"上屏"那一拍 —— 那一刻
// 用户刚停手看结果，正是时候，而且只清 编码区+候选区（见 render_present/flush_deferred
// 的 ime_commit 分支），键区本身不再单独清。整屏那次清账顺延到打字结束（键盘收起）后的
// 下一次提交，见 render_present 里 s_gc16_pending 的那条。

// 正文区（编辑器文本区）的"局刷"档位。区域只用于**限制驱动范围**（少留残影），
// 不省时间：高层刷新 min_y 恒为 0，整块面板本来就要扫一遍，耗时 = 相位数 × 帧周期。
// 所以这里挑画质而不是速度 —— 输入法区要极速（FOLLOW DU 8 帧 ≈89ms），正文区要干净。
#define BODY_PARTIAL_WAVEFORM (&E0470_WAVEFORM)
#define BODY_PARTIAL_MODE MODE_GL16

enum { JOB_PRESENT = 0, JOB_FULL, JOB_FROM_WHITE, JOB_INVALIDATE };

struct UiJob {
    int idx;         // 工作缓冲下标（-1 = 用 front_fb 直画，降级路径）
    int kind;
    int ime_top;     // IME 面板顶（逻辑 y）；-1 = 未在组合输入
    EpdRect cand;    // 编码区+候选区（逻辑）；width<=0 = 键盘没显示
    bool ime_commit; // 这一帧之前刚上屏过一次（IME::commitSeq 变了）
    bool force_full; // 强制整屏 GC16
};

// ── 两块工作缓冲的所有权 ─────────────────────────────────────────────────
// 信号量记"有几块空闲"，free_list 记"具体是哪几块"（短临界区，不阻塞）。
// 只要渲染任务结束时按它处理的那一块归还，core0 就不会拿到在推的那一块。
static uint8_t *s_fb[2];
static size_t s_fb_size;
static int s_taken = -1;  // core0 正在绘制的那块（-1 = 当前没开帧）
static int s_free_list[2];
static int s_free_n;
static portMUX_TYPE s_free_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_q;
static SemaphoreHandle_t s_free;  // 计数信号量（初值 2）
static int s_last_idx = -1;       // 渲染任务：最后一次推上屏的缓冲
static volatile bool s_active;    // init 完成

// ── 渲染任务私有状态（core0 只写三个开关）────────────────────────────────
static bool s_force_full_next = true;  // 首帧从白底出图，必须整屏 GC16
static int s_gc16_counter;
static int s_follow_partials;
static bool s_gc16_pending;   // 打字期间攒下的那次清残影
static bool s_ime_deferred;
static int64_t s_ime_defer_until;
static int s_defer_idx = -1;  // 合并窗口期间扣住不放的缓冲
static uint8_t *s_defer_cur;
static EpdRect s_defer_rect;
static EpdRect s_defer_cand;   // 本次合并窗口的编码区候选区矩形（core0 提交时带上来的）
static bool s_defer_commit;    // 本次合并窗口是"上屏"那一拍（要顺手清一遍候选区）
static volatile bool s_fast_partial, s_fast_partial_first, s_local_only;

// ── 策略开关（core0 调用，渲染任务读）────────────────────────────────────
void ui_render_set_fast_partial(bool enable) {
    if (enable && !s_fast_partial) s_fast_partial_first = true;
    s_fast_partial = enable;
}

void ui_render_set_local_only(bool enable) { s_local_only = enable; }

// ── 缓冲取还 ─────────────────────────────────────────────────────────────
static int acquire_buffer(TickType_t wait) {
    if (!s_active) return -1;
    if (xSemaphoreTake(s_free, wait) != pdTRUE) return -1;
    int idx = -1;
    portENTER_CRITICAL(&s_free_mux);
    if (s_free_n > 0) idx = s_free_list[--s_free_n];
    portEXIT_CRITICAL(&s_free_mux);
    if (idx < 0) xSemaphoreGive(s_free);  // 不该发生：令牌与表项一一对应
    return idx;
}

static void release_buffer(int idx) {
    if (idx < 0 || !s_free) return;
    portENTER_CRITICAL(&s_free_mux);
    if (s_free_n < 2) s_free_list[s_free_n++] = idx;
    portEXIT_CRITICAL(&s_free_mux);
    xSemaphoreGive(s_free);
}

// ── 几何/差分工具 ────────────────────────────────────────────────────────
// 新旧帧缓冲差异的包围盒（**逻辑**像素坐标）。四种旋转都精确：逐字节找出变化，
// 再把物理坐标反着映射回逻辑坐标。竖屏下以前一律退化成整屏，虚拟键盘打字就变成
// 每键整屏刷新；这里必须精确。无差异返回空矩形。
static EpdRect diff_bounding_rect(const uint8_t *a, const uint8_t *b) {
    const int fb_w = epd_width(), fb_h = epd_height();
    const int row_bytes = fb_w / 2;
    const int rot = epd_get_rotation();
    const int sw = SCREEN_W, sh = SCREEN_H;

    int x0 = sw, y0 = sh, x1 = -1, y1 = -1;
    auto add = [&](int px, int py) {
        int lx, ly;
        switch (rot) {
            case EPD_ROT_LANDSCAPE:          lx = px;             ly = py; break;
            case EPD_ROT_PORTRAIT:           lx = py;             ly = fb_w - 1 - px; break;
            case EPD_ROT_INVERTED_LANDSCAPE: lx = fb_w - 1 - px;  ly = fb_h - 1 - py; break;
            default:                         lx = fb_h - 1 - py;  ly = px; break;  // 270°
        }
        if (lx < x0) x0 = lx;
        if (lx > x1) x1 = lx;
        if (ly < y0) y0 = ly;
        if (ly > y1) y1 = ly;
    };

    for (int y = 0; y < fb_h; y++) {
        const uint8_t *ra = a + (size_t)y * row_bytes;
        const uint8_t *rb = b + (size_t)y * row_bytes;
        for (int xb = 0; xb < row_bytes; xb++) {
            if (ra[xb] != rb[xb]) { add(xb * 2, y); add(xb * 2 + 1, y); }
        }
    }
    EpdRect e = {0, 0, 0, 0};
    if (x1 < 0) return e;

    // 外扩 2px（抗锯齿 + 半字节边界余量）并夹到屏内
    x0 -= 2; if (x0 < 0) x0 = 0;
    x1 += 2; if (x1 >= sw) x1 = sw - 1;
    y0 -= 1; if (y0 < 0) y0 = 0;
    y1 += 1; if (y1 >= sh) y1 = sh - 1;
    EpdRect r = {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    return r;
}

// 两个矩形的并集；空矩形（width<=0）当"没有"处理。用来把"本帧变了的那块"扩到
// 盖住整个键盘：全像素刷会驱动区域内的**每一行**，所以区域必须覆盖所有变化像素，
// 否则区域外的变化就丢了。
static EpdRect rect_union(EpdRect a, EpdRect b) {
    if (a.width <= 0 || a.height <= 0) return b;
    if (b.width <= 0 || b.height <= 0) return a;
    const int x0 = a.x < b.x ? a.x : b.x;
    const int y0 = a.y < b.y ? a.y : b.y;
    const int xa = a.x + a.width, xb = b.x + b.width;
    const int ya = a.y + a.height, yb = b.y + b.height;
    const int x1 = xa > xb ? xa : xb;
    const int y1 = ya > yb ? ya : yb;
    EpdRect r = {x0, y0, x1 - x0, y1 - y0};
    return r;
}

// ── 推屏 ─────────────────────────────────────────────────────────────────
// 推屏前把这块工作缓冲**拷进** epdiy 的 front_fb（而不是临时改指针指过去）。
//
// front_fb 必须永远是"屏上当前的画面"，这条不变式有两个外部依赖方：
//   1. 阅读器整条绘制路径（GfxRenderer / HalDisplay::getFrameBuffer）画的就是
//      front_fb，并且自己直接调 epd_hl_update_screen —— 它**绕过**渲染任务。
//      一旦我们把 front_fb 指到工作缓冲上，阅读器就会画进我们的缓冲里。
//   2. 待机保留帧要从"屏上现在的画面"取，也只有 front_fb 靠得住（见 keep_frame）。
// 代价是一次 406KB 的 PSRAM 拷贝（约 5–10ms），换掉一层跨模块的隐式耦合。
static void copy_to_front(EpdiyHighlevelState *hl, uint8_t *src) {
    if (hl->front_fb != src) memcpy(hl->front_fb, src, s_fb_size);
}

// 整屏刷新：日常 GL16 差分，周期 GC16 清残影。force_gc16 时无条件整屏 GC16。
static void do_full_refresh(EpdiyHighlevelState *hl, uint8_t *cur, bool force_gc16) {
    copy_to_front(hl, cur);
    if (force_gc16 || ++s_gc16_counter >= APP_GC16_EVERY) {
        s_gc16_counter = 0;
        guard_draw_result(hl, update_display_full(hl));
    } else {
        guard_draw_result(hl, update_display_with(hl, &E0470_WAVEFORM, MODE_GL16));
    }
}

// 跟随 DU（8 帧，FAST 扫描）局部刷新一块区域；攒够 FOLLOW_GC16_EVERY 次后
// 整屏 GC16 清残影（打字期间只记账，见 s_gc16_pending）。
static void follow_du_refresh(EpdiyHighlevelState *hl, uint8_t *cur, EpdRect r) {
    copy_to_front(hl, cur);
    guard_draw_result(hl, update_display_area_with(hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, r));
    if (++s_follow_partials >= FOLLOW_GC16_EVERY) {
        s_follow_partials = 0;
        if (s_local_only || s_fast_partial) {
            // 正在打字（VK 局刷 / 实体键快刷）：只记账，不刷。404ms 的整屏 GC16
            // 会打断输入节奏，且它清的是"打字区之外"的残影，不值得打断用户。
            s_gc16_pending = true;
        } else {
            do_full_refresh(hl, cur, true);
        }
    }
}

// 丢掉合并窗口里扣住的那块缓冲（走别的推屏分支时调用）。
static void drop_defer() {
    s_ime_deferred = false;
    if (s_defer_idx >= 0) {
        release_buffer(s_defer_idx);
        s_defer_idx = -1;
    }
    s_defer_cur = nullptr;
    s_defer_cand = EpdRect{0, 0, 0, 0};
    s_defer_commit = false;
}

// 一帧的完整推屏决策。cur/rel_idx 是这一帧的内容与它占的缓冲（-1 = front_fb 直画）。
static void render_present(const UiJob &job, uint8_t *cur, int rel_idx) {
    // 先认领：本函数一返回，这块缓冲的内容就是"最新一帧"。ui_render_begin_overlay()
    // 靠它做叠加 —— 认领得早，叠加就不用等这次推屏做完。
    if (rel_idx >= 0) s_last_idx = rel_idx;
    EpdiyHighlevelState *hl = board_hl();

    if (job.force_full || s_force_full_next) {
        s_force_full_next = false;
        drop_defer();
        s_gc16_counter = 0;
        s_follow_partials = 0;
        s_gc16_pending = false;
        do_full_refresh(hl, cur, true);
        release_buffer(rel_idx);
        return;
    }

    // 差分基准 = 面板上实际的内容（epdiy 的 back_fb）。
    EpdRect d = diff_bounding_rect(cur, hl->back_fb);
    if (d.width <= 0 || d.height <= 0) {
        // 无变化：墨水屏双稳态，不刷。
        release_buffer(rel_idx);
        return;
    }

    // 1) 变化全落在 IME 面板（编码行 + 候选行 + 底栏）内 → 合并窗口。
    //    窗口内的连续输入只推最后一次，避免每键一次 8 帧跟随 DU 排队。
    //    这块缓冲要扣到窗口到期（flush 时还要用它的内容），期间不还。
    if (job.ime_top >= 0 && d.y >= job.ime_top && rel_idx >= 0) {
        drop_defer();
        s_defer_idx = rel_idx;
        s_defer_cur = cur;
        s_defer_rect = d;
        s_defer_cand = job.cand;      // flush 时顺手清一遍编码区候选区
        s_defer_commit = job.ime_commit;  // 只有上屏那一拍才清
        if (!s_ime_deferred) {
            s_ime_deferred = true;
            s_ime_defer_until = esp_timer_get_time() + IME_DEFER_US;
        }
        return;
    }

    drop_defer();  // 非 IME 条变化：覆盖任何 pending 的局刷

    // 打字状态已结束（实体键/VK 都收起），而打字期间攒下过一次没做的清残影：
    // 这次提交无论多小都整屏 GC16 一次补上，并把两个计数归零重新攒。
    if (s_gc16_pending && !s_local_only && !s_fast_partial) {
        s_gc16_pending = false;
        s_gc16_counter = 0;
        s_follow_partials = 0;
        do_full_refresh(hl, cur, true);
        release_buffer(rel_idx);
        return;
    }

    if (s_fast_partial && !s_fast_partial_first) {
        // 2) 编辑器实体键快刷：DU 差分整屏，只驱动本帧真正变化的像素。
        //    周期清残影不在这里记账 —— E0470_WAVEFORM 不是 FOLLOW 波形，display.c 的
        //    hl_update() 会按 APP_GC16_EVERY 自己把它升级成 GC16。
        copy_to_front(hl, cur);
        guard_draw_result(hl, update_display_with(hl, &E0470_WAVEFORM, MODE_DU));
    } else {
        s_fast_partial_first = false;
        // 3) 局刷判定。虚拟键盘打字（s_local_only）时只可能是"正文区也在变"：差分顶边
        //    落在输入法区之上的那几拍（打字本身只动输入法区，上面就拦下走合并窗口了）。
        //    用 GL16 局刷，画质优先（区域只限制驱动范围，不省时间 —— 见文件头）。
        //    其余界面（设置项选中、单行高亮等）沿用原来的"小变化局刷、大半屏整屏"。
        bool small = d.height <= SCREEN_H / 2;
        if (s_local_only) {
            // 虚拟键盘打字：**一律局刷，绝不整屏**。打字中途差分只落在候选条那一小条，
            // 而上屏那一拍从正文一路跨到候选条，高度常超半屏 —— 再按 small 判就会掉进
            // 整屏 GL16（≈410ms），每上屏一次闪一屏。区域只限制驱动范围、画质优先。
            //
            // 上屏那一拍（IME::commit 刚记过一次）把区域扩到编码区候选区、换 8 灰阶
            // 正文表过一遍：它 15→15 带一帧白推，是全像素刷，正好把候选栏反复换字攒的
            // 残影压干净。打字中途一次都不做，所以按一下绝不会闪。
            EpdRect r = d;
            const bool commit = job.ime_commit && job.cand.width > 0 && job.cand.height > 0;
            if (commit) r = rect_union(r, job.cand);
            copy_to_front(hl, cur);
            guard_draw_result(hl, update_display_area_with(
                hl, commit ? &E0470_GRAY8_TEXT_WAVEFORM : BODY_PARTIAL_WAVEFORM,
                BODY_PARTIAL_MODE, r));
        } else if (small) {
            follow_du_refresh(hl, cur, d);
        } else {
            do_full_refresh(hl, cur, false);
        }
    }
    release_buffer(rel_idx);
}

// 合并窗口到期：把扣住的那块缓冲推出去。
//
// 平时走 8 相跟随 DU（FAST 扫描 ≈89ms，只驱动变化矩形）。**打字中途不再有任何全刷**
// —— 以前每 VK_CLEAN_EVERY 键把整个键盘矩形过一遍 8 灰阶 GL16（30 相 ≈330ms）压键帽
// 残影，代价是打字打到一半闪一下键盘，规律得能数出来。
//
// 现在清残影只在**上屏那一拍**做，而且只清**编码区候选区**（不是整个键盘）：那一刻
// 用户刚停手看结果，候选栏从"一排候选"变成"上屏后的新一排"，正是残影要处理的时候。
// 用 8 灰阶正文表（带一帧 15→15 白推）全像素过一遍那块矩形 —— GL16 在这里是 *_full，
// 区域内每一行都被驱动到目标灰阶，压得干净；区域之外的正文一个像素都不动，不闪。
//
// 没有键盘时（"输入法条"形态）cand 是空的，commit 也带不进来，照旧只走 FOLLOW DU。
static void flush_deferred() {
    if (!s_ime_deferred) return;
    s_ime_deferred = false;
    const int idx = s_defer_idx;
    uint8_t *cur = s_defer_cur;
    s_defer_idx = -1;
    s_defer_cur = nullptr;
    const EpdRect cand = s_defer_cand;
    const bool commit = s_defer_commit;
    s_defer_cand = EpdRect{0, 0, 0, 0};
    s_defer_commit = false;
    if (idx < 0 || !cur) return;
    EpdiyHighlevelState *hl = board_hl();
    copy_to_front(hl, cur);

    if (commit && cand.width > 0 && cand.height > 0) {
        const EpdRect r = rect_union(s_defer_rect, cand);
        guard_draw_result(hl, update_display_area_with(hl, &E0470_GRAY8_TEXT_WAVEFORM, MODE_GL16, r));
    } else {
        guard_draw_result(hl, update_display_area_with(hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, s_defer_rect));
    }
    if (++s_follow_partials >= FOLLOW_GC16_EVERY) {
        s_follow_partials = 0;
        if (s_local_only || s_fast_partial) {
            s_gc16_pending = true;
        } else {
            do_full_refresh(hl, cur, true);
        }
    }
    release_buffer(idx);
}

// 立即整屏 GC16（长按全刷 / 休眠提示）。
static void full_refresh_now(uint8_t *cur, int rel_idx) {
    if (!cur) return;
    EpdiyHighlevelState *hl = board_hl();
    s_force_full_next = false;
    drop_defer();
    s_gc16_counter = 0;
    s_follow_partials = 0;
    s_gc16_pending = false;
    if (rel_idx >= 0) s_last_idx = rel_idx;
    do_full_refresh(hl, cur, true);
    release_buffer(rel_idx);
}

// 休眠唤醒：面板刚被物理清成白底（main.cpp 的 epd_clear），把当前缓冲里的画面
// 用 from-white 重推一遍：只重置旧帧基准，内容还是缓冲里那份（不会变白）。
static void from_white_now(uint8_t *cur, int rel_idx) {
    if (!cur) return;
    EpdiyHighlevelState *hl = board_hl();
    s_force_full_next = false;
    drop_defer();
    s_gc16_counter = 0;
    s_follow_partials = 0;
    s_gc16_pending = false;
    if (rel_idx >= 0) s_last_idx = rel_idx;
    copy_to_front(hl, cur);
    update_display_from_white(hl);
    release_buffer(rel_idx);
}

// 丢弃参考帧：下一帧无条件整屏 GC16。只在渲染任务里调（见 ui_render_invalidate）。
static void invalidate_state(void) {
    s_force_full_next = true;
    drop_defer();
    s_gc16_counter = 0;
    s_follow_partials = 0;
    s_gc16_pending = false;
}

// ── 渲染任务 ─────────────────────────────────────────────────────────────
static void ui_render_task(void *) {
    UiJob job;
    for (;;) {
        // 空闲时不能真的 portMAX_DELAY 睡死：rails_idle_check 在本循环末尾，
        // 它负责到点给 HV 轨下电（display.c 的 8s 期限）。阅读模式/联网期间
        // core0 不调它了，这里再睡死就永远不下电。
        TickType_t wait = pdMS_TO_TICKS(UI_RENDER_IDLE_MS);
        if (s_ime_deferred) {
            int64_t left_us = s_ime_defer_until - esp_timer_get_time();
            int left_ms = (int)((left_us + 999) / 1000);
            if (left_ms < 1) left_ms = 1;
            wait = pdMS_TO_TICKS(left_ms);
        }
        if (xQueueReceive(s_q, &job, wait) == pdTRUE) {
            uint8_t *cur = (job.idx >= 0) ? s_fb[job.idx] : board_hl()->front_fb;
            switch (job.kind) {
                case JOB_PRESENT: render_present(job, cur, job.idx); break;
                case JOB_FULL: full_refresh_now(cur, job.idx); break;
                case JOB_FROM_WHITE: from_white_now(cur, job.idx); break;
                case JOB_INVALIDATE: invalidate_state(); break;
            }
        } else {
            flush_deferred();  // 没到期就是空转，函数自己会立刻返回
        }
        // HV 轨空闲下电：和 epd_poweron 共用一路硬件，必须同任务。
        rails_idle_check((int64_t)(esp_timer_get_time() / 1000));
    }
}

// ── core0 侧接口 ─────────────────────────────────────────────────────────
// 双缓冲没起来（内存/任务创建失败，或取缓冲超时）时的降级：画在 front_fb 上，
// 提交时由 core0 同步跑一遍决策。
static uint8_t *s_sync_fb;

static int ime_top_now(void) {
    int top = -1;
    if (g_ime.composing()) {
        int st = imeStatusPanelTopY();
        int full = imeFullscreenPanelTopY();
        top = (full < st) ? full : st;
    }
    // 虚拟键盘面板也要算进来。面板顶 = 候选行顶，是最靠上的那块"输入法区域"；
    // 不含它的话，一次按键的差分框会从候选行（y≈面板顶）一路跨到按键高亮，
    // 顶边落在 IME 条之上 → 不走合并窗口 → 掉进下面"半屏以上整屏 GL16"那条规则，
    // 表现就是**每按一次闪一屏**。含进来之后，整块键盘区的变化都走
    // FOLLOW DU（8 帧 FAST 扫描 ≈89ms、只驱动变化矩形），不闪。
    // 注意：这里读的是 core0 侧的状态（editorVkVisible 只是 core0 改写的一个 bool），
    // 而 ime_top_now() 只在 core0 的提交路径上调用，不跨核。
    if (editorVkVisible()) {
        int vt = editorVkTop();
        if (top < 0 || vt < top) top = vt;
    }
    return top;
}

// 编码区+候选区矩形（逻辑坐标）—— 键盘面板顶部的两行，上屏时清的就是这一块。
// 没显示键盘时是空矩形。和 ime_top_now() 一样只能在 core0 的提交路径上读：
// editorVkVisible/editorVkTop 都是 core0 在改的状态。
// 只盖这两行、不含键帽：键帽是"按下反白"这种一眼就换掉的像素，不值得为它清一遍；
// 候选行是反复整排换字的，残影都攒在那里。
static EpdRect cand_rect_now(void) {
    EpdRect r = {0, 0, 0, 0};
    if (!editorVkVisible()) return r;
    const int top = editorVkTop();
    if (top < 0 || top >= SCREEN_H) return r;
    int h = editorVkCandH();
    if (h <= 0) return r;
    if (top + h > SCREEN_H) h = SCREEN_H - top;
    r.y = top;
    r.width = SCREEN_W;
    r.height = h;
    return r;
}

// 上屏认帧：IME::commit 每上屏一次给它自己的计数器 +1，这里比较前后值就知道
// "core0 刚上过屏"（渲染任务不该碰 g_ime，所以判定留在 core0 侧，只把 bool 传下去）。
// 每次提交都取走，避免把很久以前的某次上屏带到后面的帧上。
static uint32_t s_seen_commit_seq;
static bool take_ime_commit(void) {
    const uint32_t s = g_ime.commitSeq();
    if (s == s_seen_commit_seq) return false;
    s_seen_commit_seq = s;
    return true;
}

static void submit_sync(uint8_t *fb, bool force_full) {
    UiJob job = {};
    job.kind = JOB_PRESENT;
    job.idx = -1;
    job.force_full = force_full;
    job.ime_top = ime_top_now();
    job.cand = cand_rect_now();
    job.ime_commit = take_ime_commit();
    render_present(job, fb, -1);
}

void ui_render_init(void) {
    if (s_active) return;
    s_fb_size = (size_t)(epd_width() / 2) * epd_height();
    for (int i = 0; i < 2; i++) {
        s_fb[i] = (uint8_t *)heap_caps_malloc(s_fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_fb[i]) {
            ESP_LOGE(TAG, "工作缓冲 %d 分配失败，退回单缓冲直画", i);
            for (int j = 0; j < i; j++) { heap_caps_free(s_fb[j]); s_fb[j] = nullptr; }
            return;
        }
        memset(s_fb[i], 0xFF, s_fb_size);
    }
    s_q = xQueueCreate(UI_RENDER_QUEUE_LEN, sizeof(UiJob));
    s_free = xSemaphoreCreateCounting(2, 2);
    if (!s_q || !s_free) {
        ESP_LOGE(TAG, "队列/信号量创建失败，退回单缓冲直画");
        return;
    }
    s_free_list[0] = 0;
    s_free_list[1] = 1;
    s_free_n = 2;
    s_force_full_next = true;
    s_active = true;
    // 栈放 PSRAM（TCB 按 IDF 规定仍在内部 RAM）。这是"外部栈任务"的硬约束换来的：
    // 它绝不能自己写 flash（NVS 提交、固件写入会临时禁掉 cache，那一刻栈就读不到了）。
    // 本任务只做差分/选波形/推屏，没有任何 NVS/SD/OTA 操作，符合约束。任务常驻不退出，
    // 所以用不着 vTaskDeleteWithCaps（WithCaps 创建的栈只有它才能释放）。
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(ui_render_task, "ui_render", UI_RENDER_STACK, nullptr,
                                                    UI_RENDER_PRIO, nullptr, UI_RENDER_CORE,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "渲染任务创建失败，退回单缓冲直画");
        s_active = false;
        return;
    }
    ESP_LOGI(TAG, "渲染任务已启动（core%d, prio %d），双工作缓冲 %u B ×2",
             UI_RENDER_CORE, UI_RENDER_PRIO, (unsigned)s_fb_size);
}

uint8_t *ui_render_begin_frame(void) {
    if (!g_u8g2) return nullptr;
    if (s_taken >= 0) return s_fb[s_taken];  // 同一帧重复 ui_clear：沿用同一块

    int idx = acquire_buffer(pdMS_TO_TICKS(UI_RENDER_TAKE_MS));
    if (idx < 0 && s_active) {
        // 再宽限一轮：队列里可能正排着两帧整屏 GC16（每帧 ≤1s），不是卡住。
        ESP_LOGW(TAG, "取渲染缓冲超时，再等一轮");
        idx = acquire_buffer(pdMS_TO_TICKS(UI_RENDER_TAKE_MS));
    }
    if (idx < 0) {
        if (s_active) {
            // 渲染任务活着、却连着两轮（6 秒）没还回缓冲 —— 判定它卡在 epdiy 里了
            // （最常见是等一个再也不会来的 LCD VSYNC，见 render_lcd.c 的
            // `xSemaphoreTake(frame_done, portMAX_DELAY)`）。
            //
            // 此时**绝不能**在 core0 自己跑一遍 epdiy：驱动只有一份全局
            // render_context 和一根 frame_done 信号量，两个任务撞进去会把驱动状态和
            // 画面一起踩坏；更要命的是 core0 会卡在同一根信号量上——两个任务都是
            // **阻塞**而不是自旋，两核的 idle 任务照常喂狗，看门狗根本不会复位，结果
            // 就是屏幕和系统一起永久死住，只能抠电池。能自救的动作只有一个：重启。
            ESP_LOGE(TAG, "渲染任务连续 6 秒未归还缓冲，判定卡死，重启恢复");
            esp_restart();
        }
        // 没有渲染任务（创建失败/未初始化）：core0 直接画 front_fb 是安全的，
        // 提交时由 core0 同步跑一遍决策。**不能**沿用 u8g2 当前指向 —— 那可能正是
        // 渲染任务手里那块。
        s_sync_fb = board_hl()->front_fb;
        u8g2_set_fb(g_u8g2, s_sync_fb);
        return s_sync_fb;
    }
    s_taken = idx;
    s_sync_fb = nullptr;
    u8g2_set_fb(g_u8g2, s_fb[idx]);
    return s_fb[idx];
}

void ui_render_submit(bool force_full) {
    if (!g_u8g2) return;
    if (s_taken < 0) {
        // 没走 ui_clear 就提交。降级为同步推（内容就在当前 u8g2 缓冲里）。
        // 正常路径不该出现 —— 每个界面都是 ui_clear → 画 → ui_commit。
        static int warned;
        if (s_sync_fb) {
            uint8_t *fb = s_sync_fb;
            s_sync_fb = nullptr;
            submit_sync(fb, force_full);
        } else if (warned++ < 8) {
            ESP_LOGW(TAG, "提交了一帧没开过的帧（缺 ui_clear？）");
        }
        return;
    }
    UiJob job = {};
    job.idx = s_taken;
    job.kind = JOB_PRESENT;
    job.force_full = force_full;
    // IME 面板顶在这里算：core0 侧才能安全读 g_ime / g_font（渲染任务不该碰它们）。
    job.ime_top = ime_top_now();
    job.cand = cand_rect_now();
    job.ime_commit = take_ime_commit();
    const int idx = s_taken;
    s_taken = -1;
    if (xQueueSend(s_q, &job, pdMS_TO_TICKS(UI_RENDER_TAKE_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "推屏队列满，丢弃本帧");
        release_buffer(idx);
    }
}

// 保留帧占着一块缓冲（见 ui_render_keep_frame），空闲缓冲的目标数要相应减一。
static int s_kept_idx = -1;

void ui_render_drain(void) {
    if (!s_active) return;
    const int target = (s_kept_idx >= 0) ? 1 : 2;
    // 队列空 + 空闲缓冲数到位 = 已提交的帧全推完了。
    for (int i = 0; i < 2000; i++) {
        if (uxQueueMessagesWaiting(s_q) == 0 && uxSemaphoreGetCount(s_free) >= target) return;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGW(TAG, "drain 超时");
}

void ui_render_invalidate(void) {
    // 必须投给渲染任务做，不能在 core0 直接改：这些 static 里有正在飞的缓冲
    // （drop_defer 会还缓冲），和 core1 的推屏撞上就是缓冲被两边同时用。
    if (s_active && s_q) {
        UiJob job = {};
        job.kind = JOB_INVALIDATE;
        job.idx = -1;
        if (xQueueSend(s_q, &job, pdMS_TO_TICKS(UI_RENDER_TAKE_MS)) == pdTRUE) return;
    }
    invalidate_state();
}

static void post_last_frame(int kind);

void ui_render_full_refresh(void) {
    if (!g_u8g2) return;
    if (s_sync_fb) {  // 降级路径：当前帧还没推，直接按 GC16 同步推掉
        uint8_t *fb = s_sync_fb;
        s_sync_fb = nullptr;
        submit_sync(fb, true);
        return;
    }
    if (!s_active) return;
    if (s_taken >= 0) {  // 当前有一帧开着：把这块直接按 GC16 推掉
        ui_render_submit(true);
        ui_render_drain();
        return;
    }
    post_last_frame(JOB_FULL);
}

void ui_render_restore(void) {
    if (!g_u8g2) return;
    if (s_sync_fb) {
        uint8_t *fb = s_sync_fb;
        s_sync_fb = nullptr;
        submit_sync(fb, true);
        return;
    }
    if (!s_active) return;
    if (s_taken >= 0) ui_render_submit(false);  // 唤醒时不该有开着的帧
    post_last_frame(JOB_FROM_WHITE);
}

// 把"最后一次推上屏的那块缓冲"的内容投一个推屏作业，推完才返回。
// 当前没有开着的帧时用（有开着的帧走 ui_render_submit 那条路）。
static void post_last_frame(int kind) {
    if (!s_active || !s_q) return;
    ui_render_drain();
    const int idx = acquire_buffer(pdMS_TO_TICKS(UI_RENDER_TAKE_MS));
    if (idx < 0) return;
    if (s_last_idx >= 0 && s_last_idx != idx) memcpy(s_fb[idx], s_fb[s_last_idx], s_fb_size);
    UiJob job = {};
    job.kind = kind;
    job.idx = idx;
    if (xQueueSend(s_q, &job, pdMS_TO_TICKS(UI_RENDER_TAKE_MS)) != pdTRUE) {
        release_buffer(idx);
        return;
    }
    ui_render_drain();
}

void ui_render_begin_overlay(void) {
    // "在现有画面上叠加"（休眠提示那种只补画一角、不整屏重画的路径）。
    // 把已推上屏的那帧内容复制进一块空闲缓冲当绘制目标：叠出来的仍是"当前画面"，
    // 而且这块缓冲是 core0 独占的，撞不上推屏。
    // 读 s_fb[s_last_idx] 与渲染任务并发也是安全的 —— 双方都只读。
    if (!g_u8g2 || !s_active) return;
    if (s_taken >= 0 || s_sync_fb) return;  // 已经开着帧，直接往那块上画
    const int idx = acquire_buffer(pdMS_TO_TICKS(UI_RENDER_TAKE_MS));
    if (idx < 0) return;
    if (s_last_idx >= 0) {
        const int src = s_last_idx;
        if (src != idx) memcpy(s_fb[idx], s_fb[src], s_fb_size);
    } else {
        memset(s_fb[idx], 0xFF, s_fb_size);  // 还没推过任何一帧：白底
    }
    s_taken = idx;
    u8g2_set_fb(g_u8g2, s_fb[idx]);
}

// ── 保留帧（待机时钟 / 休眠）─────────────────────────────────────────────
// standbyClockDraw() 之前调一次：把"屏上现在的画面"留一块副本，等唤醒或预览结束后
// 用 ui_render_restore_kept() 原样推回去。期间这块工作缓冲一直占着（休眠时本来就
// 不画别的东西；预览那 3 秒也够用另一块）。
// 取的是 hl->front_fb 而不是某个工作缓冲：阅读器整条路径直接画 front_fb，
// "屏上现在的画面"只有它一定对（见 copy_to_front）。
void ui_render_keep_frame(void) {
    if (!s_active || s_kept_idx >= 0) return;
    ui_render_drain();  // 推完在飞的那一帧，front_fb 才是稳定的
    EpdiyHighlevelState *hl = board_hl();
    if (!hl || !hl->front_fb) return;
    const int idx = acquire_buffer(pdMS_TO_TICKS(UI_RENDER_TAKE_MS));
    if (idx < 0) return;
    memcpy(s_fb[idx], hl->front_fb, s_fb_size);
    s_kept_idx = idx;
}

void ui_render_restore_kept(void) {
    if (!s_active || s_kept_idx < 0) {
        ui_render_restore();  // 没保留过：退化成"把当前缓冲推一遍"
        return;
    }
    ui_render_drain();
    const int idx = s_kept_idx;
    s_kept_idx = -1;
    UiJob job = {};
    job.kind = JOB_FROM_WHITE;  // 面板刚被物理清成白底，基准要重置
    job.idx = idx;
    if (xQueueSend(s_q, &job, pdMS_TO_TICKS(UI_RENDER_TAKE_MS)) != pdTRUE) {
        release_buffer(idx);
        return;
    }
    ui_render_drain();  // 推完再返回：唤醒时下一帧就叠在这上面
}
