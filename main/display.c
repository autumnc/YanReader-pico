/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 刷屏出口：按模式推屏、HV 轨空闲下电、pclk 欠载回退。
 *
 * Present path: push by mode, drop HV rails on idle, fall back pclk on
 * underrun.
 */

#include "display.h"

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "e0470_epaper_waveform.h"
#include "e0470_page_turn.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hw/board_hw.h"  // board_hl()：自检页自推屏要自己拿 epdiy 句柄
#include "reader_page_turn.h"
#include "reader_refresh_bridge.h"

#ifndef E0470_GRAY8_TEXT_WAVEFORM
#define E0470_GRAY8_TEXT_WAVEFORM E0470_GRAY8_WAVEFORM
#endif

static const char* TAG = "read_pico";
static bool s_bulk_io;

void display_set_bulk_io(bool active) {
    s_bulk_io = active;
    ESP_LOGI(TAG, "bulk I/O scan margin %s", active ? "on" : "off");
}

// HV 轨道空闲多久才断电。断电要等 500ms 放电，再上电又要几十毫秒，
// 所以连续操作期间一直保持常开，只有真的没人动才关掉省电并卸掉 VCOM。
// How long HV rails stay up when idle. Power-off waits 500 ms to discharge,
// and power-on takes tens of ms, so keep them on during a burst of work and
// drop them — and VCOM — only when nothing is happening.
#define RAILS_IDLE_TIMEOUT_MS 8000

// 0 表示轨道已断电；否则是到期时间（ms），到点后断电。
// 0 means the rails are off; otherwise a deadline (ms) after which the loop powers them down.
static int64_t rails_deadline_ms;

// ── HV 轨上电/下电互斥 ──────────────────────────────────────────────────
// 阅读器在 core0 同步推屏（present_begin → 驱动 → rails_keepalive），而 HV 轨空闲下电在
// core1 的 ui_render 任务里（ui_render.cpp 末尾的 rails_idle_check）。两核操作的是同一路
// SY7636A + FCA9555 电源轨，而 `rails_on` 是共享且不同步的布尔：core1 下电要 500ms 放电，
// 期间 rails_on 仍是 true —— 此刻 core0 来一次上电会看到"已上电"直接返回，然后对着正在
// 掉电的轨道刷屏 = 白刷一页（用户看到"按了下一页没反应，再按才刷新最后一页"）；反过来，
// core1 若在 core0 驱动到一半时到点下电，就把正在写的一半刷新掐掉 → 残影/重影/叠字。
// 所以推屏期间持有本互斥锁（present_begin 拿、rails_keepalive 放）；下电先**试**拿锁，
// 拿不到（正推屏）就跳过一次。原来注释里的"必须同任务"约束正是被阅读器路径打破的——
// 阅读器推屏在 core0、下电在 core1，本锁就是补上这层跨核互斥。
// / HV poweron/poweroff are on different cores: the reader presents on core0, the idle
// power-off runs on core1's ui_render task. Both drive one SY7636A + FCA9555 rail set,
// and `rails_on` is an unsynchronized shared bool. board_poweroff discharges for 500 ms
// while `rails_on` is still true, so a concurrent poweron sees "already on" and skips,
// then drives a discharging panel (the reported "next page didn't refresh; the one after
// showed the last content"). A poweroff landing mid-drive cuts a half-finished refresh
// (ghost/stacked text). Serialize: a present holds the mutex from present_begin through
// rails_keepalive; the idle check try-locks and skips when a present owns it.
static SemaphoreHandle_t s_rail_mtx;

void display_init(void) {
    if (s_rail_mtx == NULL) s_rail_mtx = xSemaphoreCreateMutex();
}

// ── 面板总线收线：**下电这一拍**也要做，不只是浅睡前 ──────────────────────
// 面板断了电之后，总线脚仍挂在 LCD/RMT 输出上、带着最后一帧的残余电平，就是给已经断电的
// 源极线一个缓慢漂移的偏置 —— 面板上的残余电荷因此越来越不均匀。用户侧看到的是
// "这一页放着不动，过一会儿底色自己发灰、发花，全刷一遍才好"，并且"有时有、有时没有"
// （取决于挂住前最后一次刷是整屏 GC16 还是差分、挂了多久、温度）。原话在 epdiy 的
// epd_lcd_bus_park 注释里。
//
// 浅睡那条路一直有这一手（main.cpp 的 enterLightSleep → epd_lcd_bus_park），而
// **空闲下电这条一直没有** —— 但空闲下电才是常态：放下机器超过 RAILS_IDLE_TIMEOUT_MS
// （8 秒）而还没到自动待机档的所有时间都算，夜里更是整段挂着。所以下电时就收线，
// 下一次推屏之前（present_begin）再接线回来。
// / Park the panel bus on the idle rail-drop as well, not only before light sleep:
// after power-off the bus pins still carry the last frame's levels and slowly bias the
// unpowered source lines, which is what leaves a screen sitting idle graying out.
// Present begins by unparking, so the pair is always balanced inside one present.
static bool s_bus_parked;

void display_bus_park(void) {
    if (s_bus_parked) return;
    epd_lcd_bus_park();
    s_bus_parked = true;
    ESP_LOGI(TAG, "panel bus parked (rails off)");
}

void display_bus_unpark(void) {
    if (!s_bus_parked) return;
    epd_lcd_bus_unpark();
    s_bus_parked = false;
}

// 推屏开始：拿锁 + 接回总线 + 上电。与 rails_keepalive()（放锁）严格成对。
// / Present begin: take the lock, unpark the bus, then power on. Paired with rails_keepalive().
static void present_begin(void) {
    if (s_rail_mtx) xSemaphoreTake(s_rail_mtx, portMAX_DELAY);
    display_bus_unpark();   // 空闲下电时收过的线，推屏前先接回来（见 display_bus_park）
    epd_poweron();
}

void rails_keepalive(void) {
    rails_deadline_ms = esp_timer_get_time() / 1000 + RAILS_IDLE_TIMEOUT_MS;
    if (s_rail_mtx) xSemaphoreGive(s_rail_mtx);
}

void rails_idle_check(int64_t now_ms) {
    if (rails_deadline_ms == 0 || now_ms < rails_deadline_ms) return;
    // 到点了。推屏会持有 s_rail_mtx，试拿失败就跳过这一次下电；推屏结束后
    // rails_keepalive 会重新武装 deadline，下一拍再算。
    if (!s_rail_mtx || xSemaphoreTake(s_rail_mtx, 0) != pdTRUE) return;
    if (rails_deadline_ms != 0 && now_ms >= rails_deadline_ms) {
        rails_deadline_ms = 0;
        epd_poweroff();
        display_bus_park();   // 下电即收线：不然总线带着最后一帧的残余电平给源极线加偏置
    }
    xSemaphoreGive(s_rail_mtx);
}

// ── 全设备夜间反色 ───────────────────────────────────────────────────────
// 反色的**唯一实施点**是这里，不是 HalDisplay。原因：本文件才是所有推屏的落地点 ——
// 阅读器经 HalDisplay::displayBuffer → s_mode_refresh → update_display_reader 到这里；
// 其余每个界面（书架/菜单/设置/写作/GTD/待机）由 core1 的渲染任务**直呼**
// update_display_* 到这里，那条路根本不经过 HalDisplay。以前反色只挂在 HalDisplay 上，
// 所以"夜间模式只对阅读模式生效" —— 这就是根因。
//
// front_fb 与 back_fb **成对**翻：epdiy 的差分就发生在这两者之间
// （epd_difference_image_cropped(front,bak)），只翻 front 会让"两者处处不等"，差分矩形
// 退化成整屏，打字每按一键就整屏驱动一遍（还闪）。翻完再翻回来，所以两块在静止时都是
// **不反色**的 —— 全仓读 fb 的地方（渲染任务的 diff_bounding_rect、阅读器直画 front_fb、
// 待机保留帧、u8g2/GfxRenderer）看到的仍是正常色，不需要任何"感知夜间"的分支。
// epdiy 对 back_fb 的局部回写也被"翻出去、翻回来"自动保住：写过的行 = ~(~front) = front，
// 没写的行 = ~(~back) = back，与不反色时的终态逐字节相同。
static bool s_night;

void display_set_night(bool on) { s_night = on; }

// 取反本身：整块 415,872 B，front/back 各一遍，一次推屏前后共 4 遍（夜间模式下每个
// 按键、每次区刷都要付）。按 32 位字翻是逐字节的 ~4 倍，语义逐位不变。
// 头部先按字节走到 4 字节对齐再进字循环（LX7 不支持非对齐字访问，不能假设 fb 的对齐；
// 实际两块 fb 都是堆缓冲，这里只是不靠这个假设），尾部不足 4 字节同样退回字节循环。
static void invert_buf(uint8_t *p, size_t n) {
    size_t i = 0;
    for (; i < n && ((uintptr_t)(p + i) & 3u) != 0; i++) p[i] = (uint8_t)~p[i];
    uint32_t *w = (uint32_t *)(void *)(p + i);
    const size_t nw = (n - i) / 4;
    for (size_t k = 0; k < nw; k++) w[k] = ~w[k];
    for (i += nw * 4; i < n; i++) p[i] = (uint8_t)~p[i];
}

static void night_flip(EpdiyHighlevelState *hl) {
    const size_t n = (size_t)epd_width() / 2 * epd_height();
    invert_buf(hl->front_fb, n);
    invert_buf(hl->back_fb, n);
}

// 罩住一次推屏。必须成对调用（中间不能提前 return）。
static void night_enter(EpdiyHighlevelState *hl) { if (s_night) night_flip(hl); }
static void night_leave(EpdiyHighlevelState *hl) { if (s_night) night_flip(hl); }

// 20 相完整/原厂 DU 按厂家时序走 FULL；只有触摸笔迹用的 8 帧跟随 DU 走 FAST。
// 20-phase full / vendor DU uses FULL timing; the 8-frame FOLLOW DU used for ink trails uses FAST.
static void use_scan_for(const EpdWaveform* waveform, enum EpdDrawMode mode) {
    const bool fast = (mode & 0xF) == MODE_DU && waveform == &E0470_FOLLOW_WAVEFORM;
    read_pico_epd_use_scan(fast ? READ_PICO_EPD_SCAN_FAST : READ_PICO_EPD_SCAN_FULL);
    // 高层刷新保持整屏扫描。预填行数必须**小于**两条队列的可用槽位总数：扫描要到第
    // (min_y + 预填) 行入队后才启动，填不满就没人启动扫描、也没人消费，两个生产者
    // 一起自锁。32 行队列（READ_PICO_EPD_SMALL_FEED_QUEUE）下总槽位 2*(32-1)=62，
    // 所以这里全部取 56 以下（epdiy 侧还会按真实队列容量再夹一次）。
    // 原来沿用 64 行队列时代的 127/64，队列减半后 64 就已经越界 —— 编辑器整屏极速
    // 刷新必卡死。High-level updates scan the full panel. Prefill must stay under the
    // combined usable slots of both queues (2*(size-1)=62 here) or the scan never starts
    // and both producers self-deadlock; epdiy clamps to the real capacity as well.
    epd_lcd_set_prefill_lines(s_bulk_io ? 56 : (fast ? 44 : 32));
}

// 自上次 GC16 以来的**整页**差分刷（DU/GL16）次数。**局部（区域）刷新不计数**：
// 菜单按压、按键反馈、键盘残影清理这些都是一小块区域的差分刷，按几下就攒够节拍、
// 触发一次整屏黑白闪烁，很烦。计数的只有"整页差分刷"（area == NULL），也就是翻页、
// 换界面这类真的会推进残影的动作 —— 这和参考固件（read_pico_firmware）的策略一致。
// Whole-page differentials since the last GC16. Area (partial) updates are excluded:
// menu taps / key feedback are small-area differentials and counting them would trip a
// full black-white flash after a few presses. Mirrors the reference firmware's policy.
static int s_soft_refreshes;

// 所有按 fb 刷屏的出口都经这里。GL16 必须全像素（白底补 1 帧靠它打到）。
// 差分刷攒够 APP_GC16_EVERY 次就把这一次升为全像素 GC16：区域、fb 都不变，只换模式，
// 屏上内容仍由 fb 决定，不会丢；全像素是为了让未变化像素也过一遍 LUT，否则压不掉灰底。
// **局部（area != NULL）刷新不计数、也不清零**：菜单按压、按键反馈、清键盘残影都是
// 一小块区域的差分刷，压不了整屏灰底，却在按几下之后就替整屏把节拍攒够了 —— 表现就是
// "点几下菜单突然全屏黑白闪一下"。它自己那块区域迟早会被下一次整页刷或 GC16 带上，
// 不需要独立记账。只有整页差分刷（area == NULL）才推进残影预算。
// 跟随 DU 波形只有 DU 一张表，不计数也不升级。
// Every fb present goes through here. GL16 must be full-pixel (the extra white
// frame depends on that). After APP_GC16_EVERY soft updates, this one is
// promoted to full-pixel GC16: area and fb stay the same, only the mode
// changes, so content is not lost. Full-pixel is so unchanged pixels also run
// the LUT; otherwise the gray floor will not clear. Area updates (area != NULL)
// neither count nor reset: they only touch a small rectangle, cannot clear the
// screen-wide gray floor, and counting them would let a few menu taps trip a
// full black-white flash. FOLLOW DU has only a DU table and does not count or
// promote.
// 软刷预算的对外两个口。UI 那条路（ui_render.cpp 的 do_full_refresh）自己也有整屏
// 推送，它挑"这次升不升 GC16"必须用**同一份**预算：两条路各记各的，"攒 N 次软刷升
// 一次全刷"就变成"每条路各攒 N 次"，总残影比设想的翻倍；反过来，一条路刚升过 GC16
// （残影已清），另一条的计数不跟着归零，就会紧接着再闪一次全屏黑白。
// 只有**整页**软刷记这份账（区域刷不记，理由见上），两处口径一致。
// / The soft-refresh budget is shared: ui_render's own full-screen present must make
// its GC16 decision against the same counter, or each path accumulates its own N and
// the ghost budget doubles (or the other path flashes right after a GC16 that already
// cleared the panel). Only whole-page soft updates count.
bool display_soft_refresh_due(void) {
    return APP_GC16_EVERY > 0 && s_soft_refreshes + 1 >= APP_GC16_EVERY;
}
void display_soft_refresh_reset(void) { s_soft_refreshes = 0; }

static enum EpdDrawError hl_update(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode, bool full,
    const EpdRect* area
) {
    full = full || (mode & 0xF) == MODE_GL16;
    if (waveform != &E0470_FOLLOW_WAVEFORM && area == NULL) {
        if ((mode & 0xF) == MODE_GC16) {
            s_soft_refreshes = 0;
        } else if (APP_GC16_EVERY > 0 && ++s_soft_refreshes >= APP_GC16_EVERY) {
            s_soft_refreshes = 0;
            mode = (enum EpdDrawMode)((mode & ~0xF) | MODE_GC16);
            full = true;
            ESP_LOGI(TAG, "promote to GC16 after %d soft refreshes", APP_GC16_EVERY);
        }
    }
    enum EpdDrawError r;
    if (area != NULL) {
        night_enter(hl);
        r = full ? epd_hl_update_area_full(hl, mode, 25, *area)
                 : epd_hl_update_area(hl, mode, 25, *area);
        night_leave(hl);
        return r;
    }
    night_enter(hl);
    r = full ? epd_hl_update_screen_full(hl, mode, 25) : epd_hl_update_screen(hl, mode, 25);
    night_leave(hl);
    return r;
}

enum EpdDrawError update_display_mode(
    EpdiyHighlevelState* hl, enum EpdDrawMode mode
) {
    use_scan_for(&E0470_WAVEFORM, mode);
    present_begin();
    enum EpdDrawError result = hl_update(hl, &E0470_WAVEFORM, mode, false, NULL);
    rails_keepalive();
    return result;
}

enum EpdDrawError update_display_from_white_with(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode
) {
    use_scan_for(waveform, mode);
    present_begin();
    epd_hl_waveform(hl, waveform);
    night_enter(hl);
    enum EpdDrawError result = epd_hl_update_screen_from_white(hl, mode, 25);
    night_leave(hl);
    epd_hl_waveform(hl, &E0470_WAVEFORM);
    s_soft_refreshes = 0;
    rails_keepalive();
    return result;
}

enum EpdDrawError update_display_from_white(EpdiyHighlevelState* hl) {
    return update_display_from_white_with(hl, &E0470_WAVEFORM, MODE_GC16);
}

// 把 front_fb 铺白再整屏 GC16，物理屏回到白底。**会冲掉 front_fb 里已经画好的内容**：
// 调用点必须本意就是"清屏"（把当前缓冲里的东西丢掉），不能拿它当"从白底出下一屏"用 ——
// 那会把下一页已经画好的内容清掉。阅读器里要"重置参考帧但保留内容"请用
// update_display_from_white()（本文件目前也只剩那条路径在用）。
enum EpdDrawError update_display_white(EpdiyHighlevelState* hl) {
    epd_hl_set_all_white(hl);
    return update_display_full(hl);
}

static bool s_white_exit;

void display_hold_white_exit(bool hold) {
    s_white_exit = hold;
}

bool display_take_white_exit(void) {
    const bool hold = s_white_exit;
    s_white_exit = false;
    return hold;
}

// ── 白底参考帧纪律（阅读器侧的裸声明桥，见 reader_refresh_bridge.h）──────────
void reader_set_gray_panel(int on) {
    display_hold_white_exit(on != 0);
}

// 只看不取：列表帧的收窄判据要问一句"这笔白底账还欠着吗"。欠着就老老实实走整屏那条路
// （from-white 重锚必须整屏），别用区域刷把这次重锚挤掉。
int reader_white_exit_pending(void) {
    return s_white_exit ? 1 : 0;
}

enum EpdDrawError update_display_full(EpdiyHighlevelState* hl) {
    use_scan_for(&E0470_WAVEFORM, MODE_GC16);
    present_begin();
    enum EpdDrawError result = hl_update(hl, &E0470_WAVEFORM, MODE_GC16, true, NULL);
    rails_keepalive();
    return result;
}

// 8 灰阶全屏全像素刷（"中间档"）。和 update_display_full 的唯一区别是换表：相位 30 相
// 而非 48 相，整屏约 360ms；灰阶只有 8 级，但正文是黑白像素，降级看不出来。全像素过
// LUT（full=true）才能把灰底压掉。刷完把默认波形装回去，并把软刷计数清零。
// / 8-gray full-pixel pass. Same as update_display_full except the table: 30 phases
// instead of 48 (about 360 ms), 8 gray levels instead of 16 — invisible for text.
enum EpdDrawError update_display_gray8(EpdiyHighlevelState* hl) {
    use_scan_for(&E0470_GRAY8_WAVEFORM, MODE_GC16);
    present_begin();
    epd_hl_waveform(hl, &E0470_GRAY8_WAVEFORM);
    enum EpdDrawError result = hl_update(hl, &E0470_GRAY8_WAVEFORM, MODE_GC16, true, NULL);
    epd_hl_waveform(hl, &E0470_WAVEFORM);
    // 软刷计数由 hl_update() 按 MODE_GC16 清零，这里不用再管。
    // / hl_update() already resets the soft-refresh counter for MODE_GC16.
    rails_keepalive();
    return result;
}

// 8 灰阶正文刷（"更快的局刷"）。和默认 GL16 走的是同一条差分路，只换表：30 相而非
// 37 相，每屏省约 80ms；表里 15→15 在开机补了一帧白推（见 E0470_GRAY8_TEXT_WAVEFORM），
// 所以不变的白底仍会被推一推，其它不变像素全保持 → 不闪。
// 走 *_full 是因为整块面板本来就要扫一遍（高层刷新 min_y 恒为 0），标全 dirty 不额外
// 花时间，却能让白推落到整屏而不是只在变化行上——压白底灰是它的用途。
//
// **这条也要计入残影预算**。它原来是直呼 epd_hl_update_screen_full 绕过 hl_update 的，
// 于是既不计数也不升级 —— 阅读器只要一直用 8 灰阶正文刷翻页，就永远攒不到 GC16，灰底
// 只增不减（"边读边变灰"）。现在到节拍这一次直接换成**默认表整屏全像素 GC16**：清灰底
// 要的是相位/灰阶精度，不是省那几十毫秒，所以宁可这次贵一点、清得干净。每 APP_GC16_EVERY
// 次正文刷才发生一次，用户看到的就是"翻着翻着来一次黑白整刷"。
// / 8-gray text pass ("faster partial"). Same differential path as the default
// GL16, only the table differs: 30 phases instead of 37, ~80 ms faster per
// screen. 15→15 carries a boot-added white tick, so unchanged white still gets
// pushed while every other unchanged pixel holds — no flash. The `_full`
// variant marks all lines dirty: the panel is scanned end to end anyway
// (min_y is always 0 on the high-level path), so it costs nothing extra and
// lets the white tick land on the whole screen instead of changed lines only.
// This pass also feeds the ghost budget: it used to call
// epd_hl_update_screen_full directly, bypassing hl_update, so reader page turns
// never counted and never promoted — the gray floor only grew. On the tick we
// promote to a full-pixel GC16 on the **default** table (clearing the gray
// floor wants phase/gray precision, not the 80 ms saved).
enum EpdDrawError update_display_gray8_text(EpdiyHighlevelState* hl) {
    if (APP_GC16_EVERY > 0 && s_soft_refreshes + 1 >= APP_GC16_EVERY) {
        s_soft_refreshes = 0;
        ESP_LOGI(TAG, "promote to GC16 after %d soft refreshes (text pass)", APP_GC16_EVERY);
        return update_display_full(hl);
    }
    use_scan_for(&E0470_GRAY8_TEXT_WAVEFORM, MODE_GL16);
    present_begin();
    epd_hl_waveform(hl, &E0470_GRAY8_TEXT_WAVEFORM);
    night_enter(hl);
    enum EpdDrawError result = epd_hl_update_screen_full(hl, MODE_GL16, 25);
    night_leave(hl);
    epd_hl_waveform(hl, &E0470_WAVEFORM);
    ++s_soft_refreshes;
    rails_keepalive();
    return result;
}

// 过渡屏（微读缓存进度 / 词典下载 / "正在连接 WiFi… N 秒"）专用：与 gray8_text 是
// **同一条刷法**（同一张 30 相 8 灰阶表、同样整屏差分、白推一帧所以不闪），区别只在
// 不记账、也不升级。
//
// 为什么单开一档：这些屏是"内容几乎不变、按秒重画一次"的过渡画面。按正文那样记账的话，
// 每 APP_GC16_EVERY 次推进就替它升一次整屏 GC16 —— 用户看到的正是"缓存图书时隔几秒
// 闪一下"（这本书缓存要好几分钟，一秒一次推进，14 次 ≈ 5 秒一次全刷）。而且这些推进
// 本来也不推进残影（整屏只有一小段进度条和几个数字在变），却把共享预算吃掉了，挤掉
// 正文翻页该得到的清账机会。
// 灰底由离场那一下清：这些屏的每条出口（缓存完成/失败/取消、连接成功/失败）都会回到
// 上一级界面，而那是"进新界面首帧"→ st.fullRefresh → 整屏 GC16（见 weDrive 与
// handleWifi / readerWifiConnect 的各条分支）。所以这里不必再替它记这笔账。
// / Transition-screen pass (WeRead cache progress, dictionary download, "connecting to
// WiFi… N s"): the same 30-phase 8-gray differential as gray8_text, but it neither
// counts toward the ghost budget nor promotes to GC16. These screens repaint once a
// second with almost unchanged content; counting them produced the "flashes every few
// seconds while caching" report and ate the shared budget text pages need. The gray
// floor is cleared when the screen is left, which always happens through a
// full-refresh first frame.
enum EpdDrawError update_display_gray8_status(EpdiyHighlevelState* hl) {
    use_scan_for(&E0470_GRAY8_TEXT_WAVEFORM, MODE_GL16);
    present_begin();
    epd_hl_waveform(hl, &E0470_GRAY8_TEXT_WAVEFORM);
    night_enter(hl);
    enum EpdDrawError result = epd_hl_update_screen_full(hl, MODE_GL16, 25);
    night_leave(hl);
    epd_hl_waveform(hl, &E0470_WAVEFORM);
    rails_keepalive();
    return result;
}

// 与 HalDisplay::RefreshMode 的枚举序一一对应（HalDisplay.h 里就是这六个，
// 顺序不能改，crossmux 侧直接把枚举值当 int 传进来）。
// Mirrors HalDisplay::RefreshMode — order matters, the crossmux side passes the
// enum value straight through as an int.
#define DISPLAY_KIND_FULL 0
#define DISPLAY_KIND_HALF 1
#define DISPLAY_KIND_FAST 2
#define DISPLAY_KIND_GRAY8 3
#define DISPLAY_KIND_GRAY8_TEXT 4
#define DISPLAY_KIND_STATUS 5

// ── 错相揭页（翻页动画）────────────────────────────────────────────────
// 阅读器翻页前用 reader_hint_page_turn() 放一个方向进来，这里**消费一次**：只有紧随
// 其后的这一次刷新会走动画。做成一次性而不是"设了就一直有效"，是因为这个提示放在
// 文件级静态里、阅读器那边又可能因为换章/图片页跳过这一帧 —— 留着不放就会在无关的
// 一帧上突然放一段揭页动画。取走即清，语义最直白。
static int s_turn_dir = -1;  // -1 = 无；否则是 e0470_turn_dir_t

// 揭页只有一条梯：默认表 GL16（37 相、16 级灰，@stride6 = 127 拍 ≈ 1.06s）。它比原来
// "正文页走黑白 DU 短梯"（0.29s）慢得多，是有意换观感的：黑白两级在整屏翻页时看着是
// "啪一下硬切"，灰阶梯才是"淌过去"。曾经还想按"变化千分比 < 300‰"给正文页再分一条
// 8 灰阶短梯（快 179ms），2026-10-07 删了 —— 那个判据实测是个硬币（见
// e0470_page_turn_ex() 的注释），而且灰色揭页本来就有 ~0.9s 的地板，换梯子买不到多少。


// 揭页失败时要退回安全时钟，而它的定义在下面（跟 guard_draw_result 在一起）。
// 这里先声明一次；C 的暂定定义 + 后面的带初值定义是合法的。
static int s_pclk_mhz;

void reader_hint_page_turn(int dir) {
    s_turn_dir = dir;
}

void reader_release_page_turn(void) {
    e0470_page_turn_release();
}

// 揭页动画期间的补采回调（阅读器注册 input_tick，见 reader_page_turn.h）。
// 在这里转一道手：阅读器不能 include e0470_page_turn.h（那会拖进 epdiy.h），
// 跟 reader_hint_page_turn 同一个理由。
void reader_page_turn_set_tick_hook(void (*hook)(void)) {
    e0470_page_turn_set_tick_hook(hook);
}

// 逻辑全屏矩形。错相揭页的 area 参数是逻辑坐标，而 epd_full_screen() 给的是**物理**
// 尺寸（epdiy 的 rotation 只存枚举、不换宽高），两者在竖屏下不同，所以这里显式用
// epd_rotated_*() 拿逻辑尺寸。
static EpdRect logical_full_screen(void) {
    EpdRect a = { .x = 0, .y = 0,
                  .width = epd_rotated_display_width(),
                  .height = epd_rotated_display_height() };
    return a;
}

// 错相揭页：整屏 16 条带依次入相，差分只算一次，每拍换一张相位 LUT。
// 它自己 epd_poweron()，但**不管扫描档位/预填** —— 那是调用方的活。
// 失败时只把 pclk 退回安全值（欠载时）并还原档位，**不做恢复性的整屏重刷**：
// 交给调用方回退到本档位本来该走的普通刷新，那样屏幕既能拿到新页面，
// 又能顺带把欠载交给 guard_draw_result 统一处理。基准也还是干净的 ——
// e0470 只在成功时 copy_front_to_back，highlevel.c 的回写同样只在成功后发生。
static enum EpdDrawError update_display_page_turn(EpdiyHighlevelState* hl, e0470_turn_dir_t dir) {
    // 揭页只有一条梯（默认表 GL16，见 s_turn_dir 上方）。扫描档位必须用快的：揭页每拍
    // 一屏，喂数本来就是瓶颈，所以一律 FAST 档。预填行数仍受队列容量限制：32 行队列下
    // 总槽位 2*(32-1)=62，取 44/56 都在安全线内。
    const EpdWaveform* wf = &E0470_WAVEFORM;
    const int mode = MODE_GL16;
    read_pico_epd_use_scan(READ_PICO_EPD_SCAN_FAST);
    epd_lcd_set_prefill_lines(s_bulk_io ? 56 : 44);
    present_begin();
    // 揭页内部也是 front/back 差分 + 成功时把 front 抄回 back，所以成对翻即可（同 hl_update）。
    night_enter(hl);
    enum EpdDrawError result = e0470_page_turn_ex(hl, logical_full_screen(), dir, wf, mode);
    night_leave(hl);
    // 不管成败都把档位/预填还原成常规 GL16 的值，下一次普通刷新才不会被这次带偏。
    use_scan_for(&E0470_WAVEFORM, MODE_GL16);
    rails_keepalive();
    if (result == EPD_DRAW_SUCCESS) {
        // 每个变化像素都完整走了一遍相位梯子，等价于整屏软刷，记进残影预算，否则翻页
        // 动画会把"攒够几次升 GC16"的节拍拉长。它是 GL16 的整条相位梯（37 相、16 级灰），
        // 来回扫的摆动与它替掉的那次差分正文刷同量级，所以和普通差分刷一样按 **1** 算。
        // 以前正文页走的是跟随表 DU（8 相、黑白两级），没有那种大摆动、压残影明显更弱，
        // 才特别按 2 算逼它早升 GC16 —— 换灰阶梯后那个理由不成立了。
        ++s_soft_refreshes;
        return result;
    }
    ESP_LOGW(TAG, "错相揭页未完成 (%d)，本次回退普通刷新", (int)result);
    if (result & EPD_DRAW_EMPTY_LINE_QUEUE) {
        s_pclk_mhz = DISPLAY_PCLK_SAFE_MHZ;
        read_pico_epd_set_pclk(DISPLAY_PCLK_SAFE_MHZ);
    }
    return result;
}

enum EpdDrawError update_display_reader(EpdiyHighlevelState* hl, int kind) {
    enum EpdDrawError result;

    // 白底参考帧纪律：上一帧在面板上留下了中灰（插图页/图片查看器/自检页），
    // 而这一帧要走差分刷 —— 那就是拿着中间灰当参考帧，先 GC16 铺一屏白再从白底
    // 出这一屏。**只对差分档做**：FULL/GRAY8 本来就是整屏全像素 GC16（每个像素
    // 都被重新驱动一遍，等价于从已知态出下一屏），再铺一次只是白花一次全刷 ——
    // 这条过滤同时保证了"图片页整屏重绘"和"图片查看器 Esc 回正文"不会变成两次全刷。
    //
    // **不能调 update_display_white()**：它先把 front_fb 铺白再 GC16 —— 但这一帧的
    // 内容（下一页正文）早在 renderCurrent 里就画进 front_fb 了，铺白把内容冲掉，
    // 下面那条差分刷拿到的就是一张空 fb，画出来整页空白（用户侧：插图页翻下一页
    // 必白屏，手动全刷才恢复）。改走 update_display_from_white()：只把 back_fb
    // （差分参考帧）重置为白、front_fb 内容原样保留，一次整屏全像素 GC16 同时清掉
    // 灰底 + 出图。出图后 back_fb == front_fb，下面那条差分刷和揭页动画都成了空
    // 差分（走了也白走），所以这里清掉揭页方向后直接返回。
    if (display_take_white_exit() &&
        (kind == DISPLAY_KIND_HALF || kind == DISPLAY_KIND_FAST || kind == DISPLAY_KIND_GRAY8_TEXT ||
         kind == DISPLAY_KIND_STATUS)) {
        ESP_LOGI(TAG, "gray panel -> from-white full refresh (reset reference, keep content)");
        s_turn_dir = -1;
        result = update_display_from_white(hl);
        guard_draw_result(hl, result);
        return result;
    }

    // 待处理的揭页方向（一次性）。只用在下面两个"差分正文刷"档位上：
    //   HALF(局刷 GL16) 与 GRAY8_TEXT(8 灰阶正文刷) 都是翻页会走到的档位；
    //   FULL/GRAY8 是整屏全像素清账，动画替代它们反而清不掉残影；FAST 是用户显式
    //   选了"极速"或实体键盘打字帧，再掺 1.1s 的动画就违背了那个选择。
    const int turn = s_turn_dir;
    s_turn_dir = -1;
    if (turn >= 0 && (kind == DISPLAY_KIND_HALF || kind == DISPLAY_KIND_GRAY8_TEXT)) {
        result = update_display_page_turn(hl, (e0470_turn_dir_t)turn);
        if (result == EPD_DRAW_SUCCESS) return result;
        // 失败就落到下面走这一档本来该走的普通刷新。
    }
    switch (kind) {
        // 这几档**不能直接 return**：它们自己都不调 guard_draw_result，直接返回就绕过了
        // 欠载兜底 —— 一旦线队列供数不足（GRAY8_TEXT 正是阅读器正文翻页的默认档），
        // PCLK 不会退回安全值、也不做"清屏 + from-white 重推"，它自己又不会恢复，
        // 于是之后每一帧继续欠载，屏幕长期花屏/半页。赋给 result 落到底下统一处理。
        case DISPLAY_KIND_FULL:
            result = update_display_full(hl);
            break;
        case DISPLAY_KIND_GRAY8:
            result = update_display_gray8(hl);
            break;
        case DISPLAY_KIND_GRAY8_TEXT:
            result = update_display_gray8_text(hl);
            break;
        // 过渡屏（进度条 / "正在连接…"）：同一条 8 灰阶差分路，不记账也不升级。
        case DISPLAY_KIND_STATUS:
            result = update_display_gray8_status(hl);
            break;
        default: {
            // HALF(局刷 GL16) / FAST(极速 DU)：差分刷，走 hl_update —— GL16 会被
            // 强制成全像素（白底补 1 帧），DU 走跟随表不计数；攒够 APP_GC16_EVERY
            // 次软刷自动升成整屏 GC16 清灰底。
            const enum EpdDrawMode mode = (kind == DISPLAY_KIND_FAST) ? MODE_DU : MODE_GL16;
            use_scan_for(&E0470_WAVEFORM, mode);
            present_begin();
            result = hl_update(hl, &E0470_WAVEFORM, mode, false, NULL);
            rails_keepalive();
            break;
        }
    }
    guard_draw_result(hl, result);
    return result;
}

// 指定波形整屏刷一次，刷完把默认波形装回去。用来 A/B 两条灰阶表。
// Present the whole screen with a given waveform, then restore the default. Used to A/B two gray tables.
enum EpdDrawError update_display_with(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode
) {
    use_scan_for(waveform, mode);
    present_begin();
    epd_hl_waveform(hl, waveform);
    enum EpdDrawError result = hl_update(hl, waveform, mode, false, NULL);
    epd_hl_waveform(hl, &E0470_WAVEFORM);
    rails_keepalive();
    return result;
}

enum EpdDrawError update_display_area_with(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode,
    EpdRect area
) {
    use_scan_for(waveform, mode);
    present_begin();
    epd_hl_waveform(hl, waveform);
    enum EpdDrawError result = hl_update(hl, waveform, mode, false, &area);
    epd_hl_waveform(hl, &E0470_WAVEFORM);
    rails_keepalive();
    return result;
}

// ── 灰阶自检页的自推屏（阅读器侧的裸声明桥，见 reader_refresh_bridge.h）────────
//
// 自检页要把**同一份 framebuffer** 用五条不同的刷法各推一遍、当场把耗时记下来，
// 但它所在的那个 TU 不能 include display.h（EpdFont 冲突，见 reader_page_turn.h）。
// 所以波形/模式类型只在这里出现，阅读器那头只递一个 `which` 序号上来。
//
// 这五条正好把官方固件 app_refresh.c 里那几个按钮的刷法一一对上：
//   0 整屏 GC16（默认表）—— 全刷基准，最快能到 ~0.6s 量级
//   1 8 灰阶表整屏 —— 30 相，~360ms（阅读器"清账档"用的就是它）
//   2 from-white 16 灰 —— GC16 但基准强制为白（官方的"白底出图"）
//   3 8 灰阶表 from-white —— 就是上面那条换 30 相表
//   4 局部 DU（上半屏）—— 差分档，看局部残影/串扰
// 返回耗时 ms（含 epd_poweron 的轨上电），参数不认识返回 -1。
int reader_refresh_test_present(int which) {
    EpdiyHighlevelState* hl = board_hl();
    if (!hl) return -1;

    const int64_t t0 = esp_timer_get_time();
    enum EpdDrawError result;
    switch (which) {
        case 0:
            result = update_display_full(hl);
            break;
        case 1:
            result = update_display_gray8(hl);
            break;
        case 2:
            result = update_display_from_white(hl);
            break;
        case 3:
            result = update_display_from_white_with(hl, &E0470_GRAY8_WAVEFORM, MODE_GC16);
            break;
        case 4: {
            EpdRect area = logical_full_screen();
            area.height /= 2;  // 上半屏
            result = update_display_area_with(hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, area);
            break;
        }
        default:
            return -1;
    }
    const int ms = (int)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "refresh self-test: which=%d %dms result=%d", which, ms, (int)result);
    // 这一屏是自检页自己推上去的，推完还要照常把"面板上现在是灰"记上（(b) 的白底纪律）。
    return ms;
}

// 见 reader_refresh_bridge.h：把 epdiy 那个「前导保持相跳过」的收益露出来。
// 开关本身由板级 bring-up 打开（read_pico_init 里的 epd_set_leading_skip(true)），
// 本仓库不动它 —— 这里只读数。
int reader_leading_skip(void) { return epd_last_leading_skip(); }

// ── 清一块区域的残影（输入法那两行）────────────────────────────────────────
//
// **必须是 GC16，不能是 GL16。** 这是实测踩出来的：很长一段时间里输入法编码区/候选区
// 的清残影都写的是 8 灰阶正文表 + MODE_GL16（表里 15→15 补了一帧白推），注释还写着
// "全像素刷，压得干净"。**那句话是错的** —— 全像素只保证"每个像素都过一遍 LUT"，而
// GL16 表里 `白→白` 这一格是**全保持**（gray8_gl16 的 (to=15,from=15) 30 相全是 0，
// 补的那一帧白推就是它唯一的驱动）。而残影恰恰就长在"现在是白、之前也是白"的像素上：
// 候选栏每敲一键整排换字，上一拍的墨痕早被它自己那次差分刷写进了 back_fb，等到清残影
// 这一拍，front 和 back 在这块地方**一模一样**，GL16 只给它一帧白推 —— 一帧 ≈ 十几毫秒
// 的推力，压不掉任何东西。所以"上屏/句读之后残影还在"。
//
// GC16 的 (to=15,from=15) 是完整梯子：**先黑推 10 帧、再白推 10 帧**（gray8_gc16 实测），
// 未变像素也被整个黑白摆动带一遍，墨痕这才真的被抹平。这就是全仓别处的清残影都是 GC16
// 的原因（KEY2 清残影、晃动全刷、软刷攒够 APP_GC16_EVERY 升的那一次）。
//
// 相位数与原来那条 GL16 路**相同**（都是 8 灰阶表的 30 相），而**区域只限制驱动范围、
// 不省时间**（高层刷新 min_y 恒为 0，整块面板本来就要扫一遍），所以换过来不多花时间；
// 唯一的代价是这块区域会真闪一下（黑白摆动）—— 那是"清干净"本身的价格，不是副作用。
//
// 用 *_area_full：把区域内**每一行**都标脏。只标有差异的行就退化成"只清这一拍恰好变了
// 的像素"，而残影恰恰长在没变的地方（差分刷的基准 back_fb 早被上一拍同步过了）。
// / Region ghost clean. Must be GC16: in GL16 white→white is *all-hold* (the
// gray8_text table's single white tick is its only drive), and ghost lives
// exactly on pixels that are white now and were white in the last frame — one
// tick cannot erase it. GC16 drives 15→15 through a full 10-black/10-white
// swing, which does. Same 30 phases as the GL16 table it replaces and, since
// an area only limits driving (never the panel scan), it costs no more time;
// the region does visibly blink, which is the price of actually clearing it.
// `_area_full` marks every line in the region dirty — marking only changed
// lines would clean just the pixels that happened to change this frame, and
// the ghost is on the ones that did not.
enum EpdDrawError update_display_area_clean(EpdiyHighlevelState* hl, EpdRect area) {
    if (area.width <= 0 || area.height <= 0) return EPD_DRAW_SUCCESS;
    use_scan_for(&E0470_GRAY8_WAVEFORM, MODE_GC16);
    present_begin();
    epd_hl_waveform(hl, &E0470_GRAY8_WAVEFORM);
    night_enter(hl);
    enum EpdDrawError result = epd_hl_update_area_full(hl, MODE_GC16, 25, area);
    night_leave(hl);
    epd_hl_waveform(hl, &E0470_WAVEFORM);
    rails_keepalive();
    return result;
}

// 供数不足时的兜底：把频率退回安全值，整屏白一次，让后面的差分刷有干净参考帧。
// Underrun fallback: drop to the safe clock and wipe the panel white so later differentials have a clean reference.
static int s_pclk_mhz = DISPLAY_PCLK_DEFAULT_MHZ;

int display_pclk_mhz(void) { return s_pclk_mhz; }

void guard_draw_result(EpdiyHighlevelState* hl, enum EpdDrawError result) {
    if (!(result & EPD_DRAW_EMPTY_LINE_QUEUE)) return;
    s_pclk_mhz = DISPLAY_PCLK_SAFE_MHZ;
    read_pico_epd_set_pclk(DISPLAY_PCLK_SAFE_MHZ);
    use_scan_for(&E0470_WAVEFORM, MODE_GC16);
    present_begin();
    epd_clear();
    // 清物理屏后仅重置旧帧基准，保留目标页；否则局部刷新会留下整页白屏。
    // Reset only the old-frame baseline after clearing; preserving the target prevents blank pages after partial updates.
    night_enter(hl);
    epd_hl_update_screen_from_white(hl, MODE_GC16, 25);
    night_leave(hl);
    s_soft_refreshes = 0;
    rails_keepalive();
    ESP_LOGW(TAG, "line queue underrun, pclk back to %d MHz", DISPLAY_PCLK_SAFE_MHZ);
}
