#include "font_renderer.h"
#include "bt_keyboard.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "quick_edit.h"
#include "journal_storage.h"
#include "webdav_client.h"
#include "flomo_client.h"
#include "ime/IME.h"
#include "pjournal_app.h"
#include "screen_editor.h"
#include "editor_vk.h"   // editorVkVisible()：判断当前是不是虚拟键盘在打字（刷屏策略用）
#include "clipboard.h"   // 跨模式粘贴板（写作/阅读/计划共享）
#include "text_sel.h"    // 单行输入框的触摸选区会话（切模式时要收掉）
#include "screen_settings.h"
#include "screen_gtd.h"
#include "screen_outline.h"
#include "screen_inspiration.h"
#include "screen_bt_manage.h"
#include "screen_file_manager.h"
#include "screen_polish.h"
#include "screen_polish_prompt.h"
#include "screen_flomo.h"
#include "screen_reader.h"
#include "ui_helpers.h"
#include "ui_render.h"
#include "ui/screen.h"   // Screen：每屏一行（handle/idle_ms/键盘宿主/局刷）
#include "typing_click.h"
#include "pcf85063.h"
#include "standby_clock.h"
#include "boot_splash.h"  // Yan Reader 开机动画（研读｜研墨｜研行）

#include "board.h"
#include "board_hw.h"
#include "u8g2_shim.h"
#include "hw/input.h"
#include "crossmux_platform.h"
#include "display.h"
#include "font_store.h"
#include "read_pico_board.h"
#include "hw/auto_orient.h"   // auto_orient_tick：自适应屏幕方向（写作/计划模式的「自适应」档）
#include "read_pico_pmu.h"
#include "read_pico_sd.h"
#include "epdiy.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_pm.h>

#include <esp_sleep.h>
#include <nvs_flash.h>
#include <esp_sntp.h>
#include <esp_vfs_fat.h>
#include <driver/gpio.h>
#include <sys/time.h>
#include <freertos/semphr.h>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <atomic>

static const char *TAG = "Main";

// ── 分配失败黑匣子 ───────────────────────────────────────────────────────────
// IDF 的 OOM 回调是在分配**已经失败之后**才调的，不会重试，所以它救不了这一次，
// 只能当黑匣子：把"谁、要多大、要哪块内存"记下来。此前分配失败只拿到 NULL，
// 崩在往后几步的别处，串口上看不出根因（bt_keyboard.cpp 的 fgets 那次就是）。
// 回调运行在堆已耗尽的那一刻：绝不能在这里分配、加锁或走普通日志。
static std::atomic<size_t> s_oom_size{0};
static std::atomic<uint32_t> s_oom_caps{0};
static std::atomic<const char *> s_oom_fn{nullptr};
static std::atomic<unsigned> s_oom_count{0};

static void onAllocFailed(size_t size, uint32_t caps, const char *function_name) {
    s_oom_size.store(size, std::memory_order_relaxed);
    s_oom_caps.store(caps, std::memory_order_relaxed);
    s_oom_fn.store(function_name, std::memory_order_relaxed);
    s_oom_count.fetch_add(1, std::memory_order_relaxed);
    // ESP_EARLY_LOGx 走 ROM printf，不碰堆，是这里唯一安全的输出通道。
    ESP_EARLY_LOGE("Heap", "ALLOC FAILED size=%u caps=0x%x in %s",
                   (unsigned)size, (unsigned)caps,
                   function_name ? function_name : "?");
}

enum class AsyncUiState {
    Idle,
    Running,
    Done,
};

static std::atomic<AsyncUiState> s_webdavState{AsyncUiState::Idle};
static SyncResult s_webdavResult = {false, ""};
static int64_t s_webdavResultUntil = 0;

static std::atomic<AsyncUiState> s_flomoState{AsyncUiState::Idle};
static FlomoResult s_flomoResult = {false, ""};
static std::string s_flomoText;
static AppState s_flomoReturnTo = APP_EDITOR;
static int64_t s_flomoResultUntil = 0;
static SemaphoreHandle_t s_asyncResultMutex = nullptr;

static void ensureAsyncResultMutex() {
    if (!s_asyncResultMutex) s_asyncResultMutex = xSemaphoreCreateMutex();
}

static void lockAsyncResult() {
    ensureAsyncResultMutex();
    if (s_asyncResultMutex) xSemaphoreTake(s_asyncResultMutex, portMAX_DELAY);
}

static void unlockAsyncResult() {
    if (s_asyncResultMutex) xSemaphoreGive(s_asyncResultMutex);
}

static void drawCenteredBusy(const char *title, const char *line) {
    ui_clear();
    int cy = (SCREEN_H - 2 * LINE_SPACING) / 2;
    ui_draw_text_centered(cy, title, false, true);
    ui_draw_text_centered(cy + LINE_SPACING, line);
    ui_commit();
}

static void webdavSyncTask(void *arg) {
    (void)arg;
    bool wifiWasConnected = g_wifi.isConnected();
    SyncResult result = {false, ""};

    std::string url = g_settings.webdavUrl();
    std::string user = g_settings.webdavUsername();
    std::string pass = g_settings.webdavPassword();
    if (url.empty() || user.empty()) {
        result = {false, "请先配置WebDAV"};
    } else if (!ensure_wifi_connected()) {
        result = {false, "WiFi连接失败"};
    } else {
        g_webdav.configure(url, user, pass);
        result = g_webdav.sync("/sdcard/pjournal");
    }

    restore_wifi_state(wifiWasConnected);
    lockAsyncResult();
    s_webdavResult = result;
    unlockAsyncResult();
    s_webdavState.store(AsyncUiState::Done, std::memory_order_release);
    vTaskDelete(nullptr);
}

static void flomoSendTask(void *arg) {
    (void)arg;
    bool wifiWasConnected = g_wifi.isConnected();
    FlomoResult result = {false, ""};

    if (s_flomoText.empty()) {
        result = {false, "内容为空"};
    } else if (!ensure_wifi_connected()) {
        result = {false, "WiFi未连接"};
    } else {
        result = g_flomo.send(s_flomoText);
    }

    restore_wifi_state(wifiWasConnected);
    lockAsyncResult();
    s_flomoResult = result;
    unlockAsyncResult();
    s_flomoState.store(AsyncUiState::Done, std::memory_order_release);
    vTaskDelete(nullptr);
}

// BLE stack init + auto-connect in a background task so the main UI
// renders immediately instead of waiting ~1s for the BT controller.
//
// 只允许一个实例在跑：开机这一次最长要等 60 秒（键盘不在范围就一直重试），这期间
// 用户进一次待机再唤醒（enterLightSleep 收尾会再 spawn 一个）会再起一个，两个任务
// 同时对蓝牙栈 init/deinit、抢
// s_queue/s_devices_mutex，结果是控制器半死、键盘连不上。s_btInitRunning 只在
// 主任务里做"检查+置位"（两个 spawn 点都在主任务），置位后才创建任务。
static std::atomic<bool> s_btInitRunning{false};
static std::atomic<bool> s_btInitStop{false};

static void btInitBody() {
    ESP_LOGI(TAG, "Starting Bluetooth...");
    if (g_bt.init() != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth init failed");
        return;
    }
    g_bt.loadPairedDevices();
    if (g_bt.pairedDeviceCount() == 0) return;
    ESP_LOGI(TAG, "Found %d saved keyboard(s), will auto-connect...",
             g_bt.pairedDeviceCount());
    const BtPairedDevice *p = g_bt.getPairedDevice(0);
    int64_t deadline = esp_timer_get_time() + 60 * 1000000LL;
    // 200ms 一片而不是整 5 秒睡：休眠要起新实例时，旧实例最多 200ms 就能退出，
    // 不用等它睡满 5 秒（等待循环见 spawnBtInit）。
    while (!g_bt.isConnected() && esp_timer_get_time() < deadline &&
           !s_btInitStop.load(std::memory_order_relaxed)) {
        if (!g_bt.isScanning()) g_bt.connectBDA(p->bda, p->addr_type);
        for (int i = 0; i < 25 && !s_btInitStop.load(std::memory_order_relaxed); i++) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

static void btInitTask(void *arg) {
    btInitBody();
    s_btInitRunning.store(false, std::memory_order_release);
    vTaskDelete(NULL);
}

// 起一个 bt_init。若上一个还在跑，先请它退出（最多等 3 秒）；它退不干净就宁可不重启
// BT，也好过两个实例同时踩蓝牙栈。
static void spawnBtInit() {
    if (s_btInitRunning.load(std::memory_order_acquire)) {
        s_btInitStop.store(true, std::memory_order_relaxed);
        for (int i = 0; i < 30 && s_btInitRunning.load(std::memory_order_acquire); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_btInitStop.store(false, std::memory_order_relaxed);
        if (s_btInitRunning.load(std::memory_order_acquire)) {
            ESP_LOGW(TAG, "上一轮 BT 初始化仍未退出，跳过本次重启");
            return;
        }
    }
    s_btInitRunning.store(true, std::memory_order_release);
    if (xTaskCreatePinnedToCore(btInitTask, "bt_init", 8192, NULL, 5, NULL, 1) != pdPASS) {
        s_btInitRunning.store(false, std::memory_order_release);
        ESP_LOGE(TAG, "bt_init 任务创建失败");
    }
}

// ── Light sleep 空闲待机 ────────────────────────────────────────────────
// 键盘/物理按键无输入 ≥ N 分钟后进入 ESP light sleep(RAM 保留、BLE 射频关闭)。
// N 由「阅读设置 → 自动待机」定（关/5/10/15/20 分钟，见 g_settings.autoStandbyMinutes()）。
// 唤醒源：电源键(PMU 事件经 FCA9555 INT# 拉低 GPIO41)。墨水屏双稳态，
// 休眠画面保留；唤醒后 epd 重新上电、from-white 重绘恢复原画面。
#define AUTO_SLEEP_GRACE_US     (2 * 60 * 1000000LL)

// 最近一次用户输入(BLE 键或按键)的时间,0 表示启动后尚未记录
static int64_t s_last_activity_us = 0;
// 本帧的 key 来自蓝牙键盘/遥控器（见 pjournal_app.h）。每帧先置位,再被 hw 键补充。
bool g_key_from_ble = false;
// 唤醒后的首次按键释放不应再次触发休眠(唤醒按键与休眠按键是同一个键)
static bool s_boot_wake_release_pending = false;

// 物理按键时间判定(不依赖主循环节拍,各界面循环速度不同)
#define BTN_DEBOUNCE_US       (30000)     // 30ms 防抖
#define BTN_LONG_PRESS_US     (1000000)   // 1s 判定长按
#define BTN_DOUBLE_WINDOW_US  (300000)    // 300ms 双击窗口

// 单击动作排队: 松开后等待双击窗口确认非双击再执行,避免双击第一下误发导航键
static struct { int key = 0; int64_t queued_us = 0; } s_pending_single;

static void enterLightSleep(void);

// 长按电源键满 8s 时的关机收尾（PMU 报 KEY_FORCE_OFF，见 hw/input.cpp 的 poll_pmu_key）。
//
// 时间窗只有约 2s：PMU 在满 10s 时会自己拉 EN 硬断电，**主机拿不到任何通知**，所以
// 必须在这 2s 内把画面定稿。这里只做两件事：铺关机页 + 刷 SD；**不**调
// read_pico_pmu_power_off() 去主动下电 —— 键还按着，PMU 侧"EN=0 按住 ≥1 s 开机"会不会
// 把刚关掉的机器又点着，文档没有保证；与其赌，不如让它按原来的方式硬断电。
//
// 末尾那段阻塞是刻意的：一返回，主循环的 idle 重绘（或阅读器自己的重绘）就会把关机页
// 盖掉。等 PMU 断电就行。真等到超时（用户提前松了手）就作废快照，让下一帧把界面重画。
static void powerOffWithNotice() {
    const int64_t t0 = esp_timer_get_time();
    standbyShutdownDraw();   // 铺页 + 整屏 GC16（内部推完才返回）
    const esp_err_t se = read_pico_sd_sync();
    if (se != ESP_OK && se != ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG, "关机前 sd sync: %s", esp_err_to_name(se));
    }
    ESP_LOGW(TAG, "关机页已铺（%lld ms），等 PMU 拉 EN",
             (long long)((esp_timer_get_time() - t0) / 1000));
    for (int i = 0; i < 40; i++) vTaskDelay(pdMS_TO_TICKS(100));   // 最多等 4s
    ESP_LOGW(TAG, "等 PMU 断电超时（多半是用户松手了），交回主循环");
    ui_invalidate_snapshot();   // 快照还停在关机前那一帧，不作废就会误判"无变化"不重绘
}

// 阅读菜单「待机时钟」手动触发（阻塞到按键唤醒）。
void app_enterStandby() { enterLightSleep(); }

static void enterLightSleep(void) {
    IME::getInstance().flushUserDictSavesNow();

    // 待机画面：设置里可选「简约时钟 / 老黄历 / 书籍封面」整屏表盘，或「关闭」——关闭时
    // 什么都不画，屏上原样保留休眠前的画面。墨水屏双稳态，无论哪种画面都在整个休眠
    // 期间持续显示（零功耗）。绘制路径只发送不更新快照，快照保持休眠前画面，唤醒后据此
    // 恢复缓冲。表盘都不画底部工具栏：唤醒源只有电源键，屏上不留"按键唤醒"提示与状态栏。
    // 「书籍封面」要读 SD 上的封面缓存，所以**必须排在这里**——下面 epd_poweroff 之前、
    // 卡还挂着的时候。
    standbyClockDraw(standbyFaceFromKey(g_settings.getString("clock_face").c_str()));

    // 休眠前停掉按键音效的蜂鸣器 PCM 会话,避免休眠期耗电
    typingClickRelease();

    // 完全关断 BLE 射频(若键盘已连接,deinit 会同时断开 HID 连接)
    g_bt.deinit();

    // 墨水屏断电(双稳态保留画面)
    epd_poweroff();

    // 面板下电之后，把总线引脚（D0~D15 / XCL / XLE / XSTL / SPV / CKV）显式拉到低电平并
    // hold 住，整个浅睡期间都保持这个电平。理由见 epd_lcd_bus_park 的注释：面板断了电，
    // 而总线上还挂着最后一帧的残余电平，睡一晚就是给源极线一个漂移的偏置，底色的不均匀
    // 的灰多半有这个成分。唤醒时 display_bus_unpark() 把接线接回去（在下面 epd_poweron
    // 之前，第一次刷新要用总线）。
    //
    // 走 display_bus_park 而不是直接 epd_lcd_bus_park：同一件事 display.c 的空闲下电
    // 那一拍也要做（放下机器 8 秒后 HV 轨下电，画面在屏上挂很久的那种空闲），两边共用
    // 一份"收过线没有"的状态 —— 混着直接调 epd_* 会让下一次推屏漏掉接回来那一步。
    display_bus_park();

    // 电源键唤醒：PMU 事件拉低 FCA9555 INT#(GPIO41)，浅睡唤醒。
    gpio_config_t wcfg = {
        .pin_bit_mask = 1ULL << READ_PICO_IOE_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&wcfg);
    // 睡前把 PMU 事件队列清干净（take_key_short 会把队列里所有事件 ack 掉）：
    // 唤醒源是 IOE INT# 的低电平，若队列里还压着一个没 ack 的事件，那根线就一直是
    // 低的 —— gpio_wakeup_enable() 配好的一瞬间就会被"自己"唤醒，用户看到的就是
    // "按了待机、屏幕闪一下又回来了"。
    read_pico_pmu_take_key_short();
    read_pico_clear_ioe_int();
    gpio_wakeup_enable((gpio_num_t)READ_PICO_IOE_INT_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    // 只在**真按键**唤醒时才真醒。休眠期间 PMU 仍是 RUNNING，它自己的周期事件
    // （电量采样约 30s 一次，另有充电/低电/闹钟）都会拉低 CW_INT → FCA9555 INT# →
    // GPIO41，于是把浅睡唤醒 —— 这就是"待机过一小会儿自己醒"。这类假醒不该真醒：屏上
    // 本来就是待机画面，真醒了还得白等 2 分钟 grace 才睡回去（一次假醒换 2 分钟亮屏）。
    // read_pico_pmu_take_key_wakeup() 只对 KEY_DOWN/KEY_SHORT 返回 true，正好当"是不是人
    // 按的"判据。假的就 ack 干净再睡回去，**不设总圈数上限** —— 假醒是 30s 一次，正常
    // 情况就是该无限期睡下去。只防一种病态：INT# 一直拉着低（clear_int 只清口 0，清不掉
    // 口 1 的输入），那样会"睡下去立刻醒"式地空转。判据取**单次睡眠时长**：
    // 连睡 200ms 都睡不满且连续 10 次，才认输交回主循环。
    bool key_wake = false;
    int quick_wakes = 0;
    for (;;) {
        const int64_t t_sleep = esp_timer_get_time();
        esp_err_t ret = esp_light_sleep_start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "esp_light_sleep_start failed: %d", ret);
        }
        const int64_t slept_us = esp_timer_get_time() - t_sleep;
        gpio_wakeup_disable((gpio_num_t)READ_PICO_IOE_INT_GPIO);
        read_pico_clear_ioe_int();
        // take_key_wakeup 会把队列里的事件**连类型一起 ack 掉**（只认 KEY_DOWN/KEY_SHORT，
        // 其余静默丢弃），所以它必须先跑；随后 drain 只剩日志作用。判完再取一次快照，
        // 打上电量/电源态 —— 假醒几乎都是电量采样事件，这条日志够定位。
        key_wake = read_pico_pmu_take_key_wakeup();
        read_pico_pmu_drain_events();
        const pmu_snapshot_t *s = read_pico_pmu_get();
        ESP_LOGI(TAG, "Woke up, cause=%d key=%d slept=%lldms pwr=%s mv=%u",
                 (int)esp_sleep_get_wakeup_cause(), (int)key_wake,
                 (long long)(slept_us / 1000), s ? pmu_power_name(s->power_state) : "?",
                 s ? (unsigned)s->battery_mv : 0u);
        if (key_wake) break;
        if (slept_us < 200000) {
            if (++quick_wakes >= 10) {
                ESP_LOGW(TAG, "连续秒醒 %d 次，INT# 疑似清不掉，交回主循环", quick_wakes);
                break;
            }
        } else {
            quick_wakes = 0;
        }
        // 不是人按的（PMU 周期事件 / 充电器插入 / SD 插拔）：ack 干净、再睡回去。
        read_pico_pmu_take_key_short();
        read_pico_clear_ioe_int();
        gpio_wakeup_enable((gpio_num_t)READ_PICO_IOE_INT_GPIO, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();
    }

    // 先把 sleeping 期间锁住的那些总线脚解回来、接线重建（display_bus_park 的逆操作），
    // 再上电清屏 —— epd_clear 就要用总线了。
    display_bus_unpark();

    // 面板重新上电 + 清屏；ui_restore_snapshot 用 from-white 重绘恢复原画面。
    epd_poweron();
    epd_clear();
    ui_restore_snapshot();

    // 软件时钟在休眠期间冻结,从 PMU RTC 重同步,保证日记时间戳正确
    time_t t = g_rtc.getTime();
    if (t > 0) {
        struct timeval tv = {(time_t)t, 0};
        settimeofday(&tv, NULL);
    }

    // 唤醒用的是**电源键**，而按电源键在这台机器上还有"切模式"的含义。唤醒那一按的
    // KEY_DOWN 已经被 take_key_wakeup ack 掉了，可松手时的 KEY_SHORT 会在我们醒着之后
    // 才进队列 → 不拦的话就是"在待机画面按一下叫醒，回来发现模式被转了"。登记一次，
    // 让它把紧接着的那个抬手事件吞掉（input.cpp 里有时间窗）。
    //
    // **登记点必须放在这一行，不能提前**：上面那段尾巴（epd_poweron + epd_clear +
    // ui_restore_snapshot 从白重绘）本身就要 1~2s，而 input.cpp 那边是"从登记起 N 秒内
    // 才算唤醒那一按的抬手"。登记在尾巴之前的话，用户看到画面亮起来再松手时窗口早就过
    // 了 —— 抬手被当成正常短按上报，就是"按电源键唤醒顺带切了模式"（2026-10-08 反馈）。
    // 登记在这里，窗口从"画面已经回来"起算，用户松手的那一下稳稳落在窗内。
    if (key_wake) input_note_key_wake();

    // 后台重新 init BLE + 自动重连键盘(即使休眠失败也恢复 BT 栈)
    spawnBtInit();
}

static void checkLightSleep(AppState state) {
    // 自动待机时长（分钟）：0 = 关。设置入口在阅读模式的设置里，但三个模式共用这一个
    // 计时——原来写作模式那个「自动休眠」开关与它本来就是同一件事，已经删掉。
    const int standby_min = g_settings.autoStandbyMinutes();
    if (standby_min <= 0) return;
    if (g_wifi.isConnected()) return;  // 网络操作中不休眠
    if (state == APP_BT_MANAGE) return;  // 用户在蓝牙管理界面

    static int64_t s_last_wake_us = 0;
    int64_t now = esp_timer_get_time();

    // 启动后首次调用:以当前时刻作为空闲计时起点
    if (s_last_activity_us == 0) {
        s_last_activity_us = now;
        return;
    }

    // 唤醒后 2 分钟 grace,覆盖 BLE 重连窗口 + 用户操作
    if (s_last_wake_us != 0 && (now - s_last_wake_us) < AUTO_SLEEP_GRACE_US) return;

    // 键盘/按键空闲超过设定时长 → 进入待机
    if (now - s_last_activity_us >= (int64_t)standby_min * 60 * 1000000LL) {
        enterLightSleep();
        s_last_wake_us = esp_timer_get_time();
        s_last_activity_us = s_last_wake_us;  // 重置空闲计时基准,避免唤醒后立即再睡
    }
}

// ── SD 热插拔（运行中拔卡/插卡）─────────────────────────────────────────
// 开机那次等待（app_main 里）只覆盖"开机时没插卡"。运行中拔卡此前没人管：所有读写各自
// 报错、各界面各自提示，但没有一处**知道"卡没了"**；更麻烦的是插回来之后驱动那边
// media_invalidated 一直挂着，read_pico_sd_get_info 会一直回放旧错误 —— 不主动 remount
// 就永远读不回来（与"开机没卡"是同一个陷阱，见 read_pico_sd.c 的 observe_media_locked）。
//
// 每 500ms 问一次驱动（get_info 内部顺手把 CD 观察记回驱动：拔卡即置 media_invalidated），
// 只认两个状态迁移，每个迁移**每次插拔只做一次**（否则一张坏卡会 500ms 挂一次、日志刷屏）：
//   卡在（且已挂载）→ 说卡丢了：日志 + 让阅读器收尾（退出当前书、清空书架列表 —— 那些
//                    句柄已经随卡作废，继续读只会一路报错）。
//   说卡丢了 → 卡又在了：remount。这才是驱动要的时机（消费者先关句柄 —— 有书开着时
//                    阅读器那一拍已经先把它关掉了），挂上之后再让阅读器重扫书架。
// 阅读器那两个钩子只在"当前界面就是阅读器"时调：卡不在时别的界面（编辑器/WiFi…）各自
// 报错就够，而那两个钩子会当场渲染，调错界面就把人家正在看的画面盖掉了。
static void sdHotplugTick(AppState state) {
    static int64_t s_next_us = 0;
    static bool s_saw_lost = false;      // 上一次轮询时卡是"不在 / 没挂上"
    static bool s_tried_remount = false; // 本次"卡回来"已经试过挂载了吗
    const int64_t now = esp_timer_get_time();
    if (now < s_next_us) return;
    s_next_us = now + 500 * 1000;

    read_pico_sd_info_t info = {};
    read_pico_sd_get_info(&info);
    // 第一拍只记状态、不动作：开机就没插卡时 present/mounted 一直是假，若把这一拍也当成
    // "刚刚丢失"，每次无卡开机都会白打一行「SD 卡丢失」并让阅读器去收一个从未开过的尾。
    // 过渡只认**观察到变化**，不认"我开机第一次看就是这样"。
    static bool s_seeded = false;
    if (!s_seeded) {
        s_seeded = true;
        s_saw_lost = !(info.present && info.mounted);
        return;
    }
    if (info.present && info.mounted) {
        if (s_saw_lost) {
            s_saw_lost = false;
            s_tried_remount = false;
            ESP_LOGI(TAG, "SD 卡已恢复（已挂载）");
            if (state == APP_READER) screen_reader_on_sd_ready();
        }
        return;
    }

    if (!s_saw_lost) {
        s_saw_lost = true;
        s_tried_remount = false;
        ESP_LOGW(TAG, "SD 卡丢失: present=%d needs_format=%d err=%s", (int)info.present,
                 (int)info.needs_format, esp_err_to_name(info.error));
        if (state == APP_READER) screen_reader_on_sd_lost(!info.present);
    } else if (info.present && !s_tried_remount) {
        // 卡回来了但还没挂上：这一次插入只试一次（重新挂载会重走 40/20/10MHz 那条梯子，
        // 一张坏卡每 500ms 重试一次没有意义，还会把日志刷满）。
        s_tried_remount = true;
        const esp_err_t err = read_pico_sd_remount();
        ESP_LOGW(TAG, "SD 卡回来，重新挂载: %s", esp_err_to_name(err));
    }
}

// ── 每屏的 handle（Screen 表的实现，见 main/ui/screen.h）────────────────
// 这一批函数是**从主循环那个 switch 里原样搬出来的 case 体**：`break` 换成
// `return 下一个界面`，`currentState` 换成局部 `next`，其余一字未改——包括各屏自己
// 那份 `static bool xxxInited` 和退出时的清零，语义与搬迁前逐条对应。
// 空转时**不在这里睡**（除了 idle_ms=0 的几屏，它们本来就在体内 vTaskDelay）。

// 空转等待期间补采样触摸。触摸只在主循环顶部 input_poll() 采一次，而各界面空转
// 时靠 vTaskDelay 睡 50~200ms；cst836u 只返回"当前"按下状态（没有锁存寄存器），
// 一次 60~120ms 的点按若整个落在同一个睡眠窗口里就会**彻底丢失** —— 手感就是
// "点了没反应，得再点一次"。这里把等待切成小段、每段前补采一次；采到的按键由
// hw/input.cpp 暂存（input_tick），下一轮 input_poll() 取走。
static void idleWaitWithTouch(int total_ms) {
    const int step_ms = 20;   // 40~50Hz：60ms 的点按至少能采到两拍
    for (int left = total_ms; left > 0; ) {
        int step = (left > step_ms) ? step_ms : left;
        // input_pending_key() 内部就是补采一次触摸，顺手问一句"已经有待发按键了吗"：
        // 有就别再睡了 —— 主循环回到顶部 input_poll() 才把它取走，剩下的整段睡眠
        // 纯粹是加在"手指离开 → 分发"之间的延迟。实测一次点按在这段白等上中位花
        // 54ms（量化成 14 + 20k，k 就是没睡完的段数），早退之后只剩那 14ms。
        if (input_pending_key() != 0) break;
        vTaskDelay(pdMS_TO_TICKS(step));
        left -= step;
    }
}

// 界面属于哪个模式（appModeOfState）定义搬到 pjournal_app.h 了：screen_editor 的
// leave 钩子也要用它判断"这次离开是不是切模式"。

// 主循环里几个跨 case 要用的现场（原来是循环内的 static/local）。
static AppState inspReturnTo = APP_MAIN;        // 灵感面板：退出回哪儿
static AppState inspEditorReturnTo = APP_MAIN;  // 灵感面板 → 编辑器：回来时的落点

static AppState scrMain(int key, ScreenContext &ctx) {
    g_font.setSize(20);
    return screen_main_handle(key, ctx);
}

static AppState scrEditor(int key, ScreenContext &ctx) {
    g_font.setSize(g_settings.fontSize());
    {
        int fs = g_font.fontSize();
        IME::getInstance().setPageSize(fs <= 22 ? 7 : 5);
    }
    AppState next = APP_EDITOR;
    if (key > 0) next = screen_editor_handle(key, ctx);
    else {
        screen_editor_idle(ctx, false);
        // 空转不睡满 50ms：蓝牙键盘一有键就醒（waitKey 是 peek，不消费）。
        // 原来这里是整块 vTaskDelay(50)，实体键连打时每个键平均多等 25ms、最坏 50ms
        // —— 面板那一拍本身约 56ms，这一觉是纯加在端到端延迟上的。拆成 20ms 小段，
        // 顺带补采触摸（同 idleWaitWithTouch：主循环每轮只在顶部 input_poll 采一次，
        // 长睡会把一次短点按整个吞掉）。没连键盘时等价于原来的 50ms 睡眠。
        for (int left = 50; left > 0; ) {
            int step = (left > 20) ? 20 : left;
            input_tick();
            if (g_bt.waitKey(step)) break;
            left -= step;
        }
    }
    return next;
}

static AppState scrBrowser(int key, ScreenContext &ctx) {
    g_font.setSize(20);
    return screen_browser_handle(key, ctx);
}

static AppState scrViewer(int key, ScreenContext &ctx) {
    g_font.setSize(20);
    return screen_viewer_handle(key, ctx);
}

static AppState scrHistory(int key, ScreenContext &ctx) {
    g_font.setSize(20);
    return screen_history_handle(key, ctx);
}

static AppState scrSettings(int key, ScreenContext &ctx) {
    g_font.setSize(20);
    IME::getInstance().setPageSize(7);
    return screen_settings_handle(key, ctx);
}

static AppState scrBtManage(int key, ScreenContext &ctx) {
    g_font.setSize(20);
    return screen_bt_manage_handle(key, ctx);
}

static AppState scrFileManager(int key, ScreenContext &ctx) {
    g_font.setSize(20);
    return screen_file_manager_handle(key, ctx);
}

static AppState scrGtd(int key, ScreenContext &ctx) {
    g_font.setSize(20);
    IME::getInstance().setPageSize(7);
    return screen_gtd_handle(key, ctx);
}

static AppState scrOutline(int key, ScreenContext &ctx) {
    g_font.setSize(20);
    IME::getInstance().setPageSize(7);
    return screen_outline_handle(key, ctx);
}

static AppState scrInspiration(int key, ScreenContext &ctx) {
    g_font.setSize(20);
    IME::getInstance().setPageSize(7);
    return screen_inspiration_handle(key, ctx);
}

static AppState scrPolish(int key, ScreenContext &ctx) {
    g_font.setSize(20);
    IME::getInstance().setPageSize(7);
    return screen_polish_handle(key, ctx);
}

static AppState scrPolishPrompt(int key, ScreenContext &ctx) {
    g_font.setSize(20);
    IME::getInstance().setPageSize(7);
    return screen_polish_prompt_handle(key, ctx);
}

static AppState scrFlomo(int key, ScreenContext &ctx) {
    g_font.setSize(20);
    IME::getInstance().setPageSize(7);
    return screen_flomo_handle(key, ctx);
}

// WebDAV 同步：整屏就是一串异步状态（Idle → Running → 结果停留 2s → 回主菜单），
// 每拍自己 vTaskDelay（idle_ms = 0）。
static AppState scrSyncWebdav(int key, ScreenContext &ctx) {
    (void)key; (void)ctx;
    g_font.setSize(20);
    IME::getInstance().setPageSize(7);
    AsyncUiState state = s_webdavState.load(std::memory_order_acquire);
    if (state == AsyncUiState::Idle) {
        lockAsyncResult();
        s_webdavResult = {false, ""};
        unlockAsyncResult();
        s_webdavResultUntil = 0;
        s_webdavState.store(AsyncUiState::Running, std::memory_order_release);
        TaskHandle_t h = nullptr;
        if (xTaskCreate(webdavSyncTask, "webdav_sync", 12288, nullptr, 1, &h) != pdPASS) {
            lockAsyncResult();
            s_webdavResult = {false, "系统繁忙,请重试"};
            unlockAsyncResult();
            s_webdavState.store(AsyncUiState::Done, std::memory_order_release);
        }
        state = s_webdavState.load(std::memory_order_acquire);
    }

    if (state == AsyncUiState::Running) {
        drawCenteredBusy("WebDAV 同步", "正在同步...");
        vTaskDelay(pdMS_TO_TICKS(100));
        return APP_SYNC_WEBDAV;
    }

    if (s_webdavResultUntil == 0) {
        s_webdavResultUntil = esp_timer_get_time() + 2000000;
        lockAsyncResult();
        std::string message = s_webdavResult.message;
        unlockAsyncResult();
        drawCenteredBusy("WebDAV 同步",
                         message.empty() ? "同步结束" : message.c_str());
        return APP_SYNC_WEBDAV;
    }
    if (esp_timer_get_time() < s_webdavResultUntil) {
        vTaskDelay(pdMS_TO_TICKS(100));
        return APP_SYNC_WEBDAV;
    }
    s_webdavState.store(AsyncUiState::Idle, std::memory_order_release);
    return APP_MAIN;
}

// Flomo 发送：同 WebDAV，另外多一份"正文从哪儿来"的判断（编辑器 / 待发管道）。
static AppState scrSyncSendFlomo(int key, ScreenContext &ctx) {
    (void)key; (void)ctx;
    AsyncUiState state = s_flomoState.load(std::memory_order_acquire);
    if (state == AsyncUiState::Idle) {
        if (!g_flomoPendingText.empty()) {
            s_flomoText = std::move(g_flomoPendingText);
            g_flomoPendingText.clear();
            s_flomoReturnTo = g_flomoReturnTo;
        } else {
            s_flomoText = app_get_editor_text();
            s_flomoReturnTo = APP_EDITOR;
        }
        lockAsyncResult();
        s_flomoResult = {false, ""};
        unlockAsyncResult();
        s_flomoResultUntil = 0;
        s_flomoState.store(AsyncUiState::Running, std::memory_order_release);
        TaskHandle_t h = nullptr;
        if (xTaskCreate(flomoSendTask, "flomo_send", 8192, nullptr, 1, &h) != pdPASS) {
            lockAsyncResult();
            s_flomoResult = {false, "系统繁忙,请重试"};
            unlockAsyncResult();
            s_flomoState.store(AsyncUiState::Done, std::memory_order_release);
        }
        state = s_flomoState.load(std::memory_order_acquire);
    }

    if (state == AsyncUiState::Running) {
        ui_clear();
        ui_show_message_centered("正在发送...");
        ui_commit();
        vTaskDelay(pdMS_TO_TICKS(100));
        return APP_SYNC_SEND_FLOMO;
    }

    if (s_flomoResultUntil == 0) {
        s_flomoResultUntil = esp_timer_get_time() + 2000000;
        lockAsyncResult();
        std::string message = s_flomoResult.message;
        unlockAsyncResult();
        ui_clear();
        ui_show_message_centered(message.empty() ? "发送结束" : message.c_str());
        ui_commit();
        return APP_SYNC_SEND_FLOMO;
    }
    if (esp_timer_get_time() < s_flomoResultUntil) {
        vTaskDelay(pdMS_TO_TICKS(100));
        return APP_SYNC_SEND_FLOMO;
    }
    s_flomoText.clear();
    s_flomoState.store(AsyncUiState::Idle, std::memory_order_release);
    return s_flomoReturnTo;
}

static AppState scrReader(int key, ScreenContext &ctx) {
    // 阅读模式用 crossmux 自己的 GfxRenderer + ttf_font 渲染，不用 g_font。
    AppState next = APP_READER;
    if (key > 0) next = screen_reader_handle(key, ctx);
    else { screen_reader_handle(0, ctx); idleWaitWithTouch(80); }
    // 离开阅读模式的收尾（记进度 + 释放书对象 + 还方向）不在这里了：它是 kScreens 里
    // 那一行的 leave（screen_reader_leave），主循环无论从哪条路走出去都会调到。
    return next;
}

// ── 各屏的 enter 适配层 ─────────────────────────────────────────────────
// 屏自己的 init 签名不一样（有的要 ctx 里的东西，有的要"回到哪儿"），这里做一次薄
// 适配，塞进 kScreens 的 enter 列。**要不要重建**这条策略不在这一层 —— 多数屏是
// "每次进来都重建"，所以在 enter 里直接调 init；两个例外（计划模式和编辑器）各自
// 把策略收在自己的 .cpp 里（screen_gtd_enter / screen_editor_enter）。
static void browserEnter(ScreenContext &ctx) { (void)ctx; screen_browser_init(); }
static void viewerEnter(ScreenContext &ctx) { screen_viewer_init(ctx.selectedEntry); }
static void historyEnter(ScreenContext &ctx) { screen_history_init(ctx.selectedEntry, ctx.prevState); }
static void settingsEnter(ScreenContext &ctx) { (void)ctx; screen_settings_init(); }
static void btManageEnter(ScreenContext &ctx) { (void)ctx; screen_bt_manage_init(); }
static void fileManagerEnter(ScreenContext &ctx) { (void)ctx; screen_file_manager_init(); }
static void outlineEnter(ScreenContext &ctx) { (void)ctx; screen_outline_init(); }
static void inspirationEnter(ScreenContext &ctx) {
    (void)ctx;
    screen_inspiration_init(inspReturnTo, inspEditorReturnTo);
}
static void polishEnter(ScreenContext &ctx) { (void)ctx; screen_polish_init(); }
static void polishPromptEnter(ScreenContext &ctx) { (void)ctx; screen_polish_prompt_init(); }
static void flomoEnter(ScreenContext &ctx) { (void)ctx; screen_flomo_init(APP_MAIN); }

// ── 表 ─────────────────────────────────────────────────────────────────
// 一行一个界面。**没列到的状态**（APP_PROMPT_SEL）没有 handle，主循环按 default
// 处理——回主菜单，与搬迁前那个 switch 的 default 分支一致。
//
// 用**具名初始化**（C++20 designated initializers）：这一版起一行有七个槽，位置写法
// 读一次错一次；写上字段名之后，"这屏有没有 leave""idle 睡多久"是看出来的。
// 没写的字段取 Screen 里的默认值（idle_ms=100、vk_host=false、local_only=false）。
static const Screen kScreens[APP_QUIT + 1] = {
    /* APP_MAIN          */ {.name = "main",           .handle = scrMain,         .idle_ms = 200, .local_only = true },
    /* APP_EDITOR        */ {.name = "editor",         .enter = screen_editor_enter, .handle = scrEditor,
                             .leave = screen_editor_leave, .idle_ms = 0, .vk_host = true },
    /* APP_BROWSER       */ {.name = "browser",        .enter = browserEnter,     .handle = scrBrowser },
    /* APP_VIEWER        */ {.name = "viewer",         .enter = viewerEnter,      .handle = scrViewer },
    /* APP_HISTORY       */ {.name = "history",        .enter = historyEnter,     .handle = scrHistory },
    /* APP_SETTINGS      */ {.name = "settings",       .enter = settingsEnter,    .handle = scrSettings,
                             .vk_host = true, .local_only = true },
    /* APP_PROMPT_SEL    */ {.name = "prompt_sel"},   // 没有 handle：主循环回主菜单
    /* APP_SYNC_WEBDAV   */ {.name = "sync_webdav",    .handle = scrSyncWebdav,   .idle_ms = 0 },
    /* APP_SYNC_SEND_FLOMO*/{.name = "sync_flomo",     .handle = scrSyncSendFlomo,.idle_ms = 0 },
    /* APP_BT_MANAGE     */ {.name = "bt_manage",      .enter = btManageEnter,    .handle = scrBtManage,
                             .idle_ms = 30 },
    /* APP_FILE_MANAGER  */ {.name = "file_manager",   .enter = fileManagerEnter, .handle = scrFileManager,
                             .idle_ms = 200 },
    // 计划模式的 enter/leave 收在 screen_gtd.cpp 自己手里：**重跑 init 会把 tab 清回
    // 收集箱**，所以只有本次开机第一次进来才 init，之后重进只补方向。
    /* APP_GTD           */ {.name = "gtd",            .enter = screen_gtd_enter, .handle = scrGtd,
                             .leave = screen_gtd_leave, .vk_host = true },
    /* APP_OUTLINE       */ {.name = "outline",        .enter = outlineEnter,     .handle = scrOutline,
                             .vk_host = true },
    /* APP_INSPIRATION   */ {.name = "inspiration",    .enter = inspirationEnter, .handle = scrInspiration,
                             .vk_host = true },
    /* APP_POLISH        */ {.name = "polish",         .enter = polishEnter,      .handle = scrPolish,
                             .vk_host = true },
    /* APP_POLISH_PROMPT */ {.name = "polish_prompt",  .enter = polishPromptEnter,.handle = scrPolishPrompt,
                             .vk_host = true },
    /* APP_FLOMO         */ {.name = "flomo",          .enter = flomoEnter,       .handle = scrFlomo,
                             .vk_host = true },
    // 阅读模式**是**虚拟键盘宿主：它自己有一整套键盘的开关/命中/绘制（screen_reader
    // 里 20 多处翻 st.vkVisible）。这一列原来是 false —— 于是主循环每一轮都拿
    // editorVkSetVisible(false) 把阅读模式刚摆出来的键盘收掉，顺带把 s_userOverride
    // 钉成 true（那个标记的语义是"用户手动关过，别再自动弹回来"，被这么用一次就废了），
    // 还每轮白跑一遍 invalidateCandidateWidths()。见 [[vk-host-reader]]。
    // enter/leave = 真正的重建与收尾（阅读器没有"续上上次现场"这种东西：它自己用
    // s_return 记返回点，见 rdRestoreReturnPoint）。
    /* APP_READER        */ {.name = "reader",         .enter = screen_reader_enter, .handle = scrReader,
                             .leave = screen_reader_leave, .idle_ms = 0, .vk_host = true },
};
static_assert(sizeof(kScreens) / sizeof(kScreens[0]) == APP_QUIT + 1,
              "kScreens 必须一个界面一行（含 APP_QUIT 占位）");

// ── 界面生命周期：只有"进入"和"离开"两个事件 ────────────────────────────
// s_activeScreen = **已经跑过 enter** 的那个界面。主循环里任何改了 currentState 的
// 地方（电源键切模式、Ctrl+I 弹灵感、各屏 handle 的返回值）都不必自己记着"该不该
// init/收尾"——调一次 syncScreenLifecycle() 就行，它是幂等的：
//   换了界面 → 先 leave 旧屏、再 enter 新屏；没换 → 什么都不做。
// 调用点在派发**前后**各一次：前面那次收"派发之前就改了 currentState"的（切模式/
// 全局键），后面那次收各屏自己返回的 next。
static AppState s_activeScreen = APP_QUIT;

static void syncScreenLifecycle(AppState state, ScreenContext &ctx) {
    if (state == s_activeScreen) return;
    if (s_activeScreen != APP_QUIT) {
        const Screen &old = kScreens[s_activeScreen];
        if (old.leave) old.leave(state);   // 参数是"去哪儿"（编辑器要按目的地决定收尾）
    }
    s_activeScreen = state;
    if (state == APP_QUIT) return;
    const Screen &cur = kScreens[state];
    if (cur.enter) cur.enter(ctx);
}

// 现在这个界面允不允许自适应方向跟着转。
//
// **按界面点名，不按 appModeOfState 一刀切。** 那个映射把设置/主菜单/文件管理/历史
// 这些也算进写作模式(1)（它们只是借道那个模式进出，见 pjournal_app.h），而设置界面
// 自己就挂着「写作模式方向」这一项 —— 选完当场把这一屏也转过去，正是
// pickerApply 里 _gtd_orientation 那条注释在躲的事。
//
// 点名的是"会打字、会摆出候选行的几个界面"：候选行分页跟着屏宽走（IME 那次改动）
// 正是在这几个界面里生效，也是自适应真正有用的地方。阅读模式有自己那份方向设置
// （屏幕上刻意竖屏），一律不跟；设置/文件管理/历史同理 —— 「自适应」是各模式自己的
// 方向档，不是全局开关。
//
// **APP_MAIN 要点上**：它就是写作模式的主菜单（appModeOfState 把 APP_MAIN 算成模式 1，
// 也就是"进这一屏 = 在写作模式里"），计划模式自己的那一屏 APP_GTD 一直在名单里 ——
// 少了 APP_MAIN，两个模式的"进模式之后第一屏"就一个跟一个不跟（用户报的正是这个：
// 计划模式转、写作模式不转）。
static bool autoOrientWantedFor(AppState st) {
    switch (st) {
        case APP_MAIN:
        case APP_EDITOR:
        case APP_OUTLINE:
        case APP_INSPIRATION:
        case APP_POLISH:
        case APP_POLISH_PROMPT:
        case APP_FLOMO:
            return g_settings.getString("writing_orientation", "") == "auto";
        case APP_GTD:
            return g_settings.getString("gtd_orientation", "") == "auto";
        default:
            return false;
    }
}

// ── Application Main Loop ──────────────────────────────────────────────



extern "C" void app_main() {
    ESP_LOGI(TAG, "Yan Reader v" YAN_READER_VERSION " starting...");

    // 装黑匣子要在任何大分配之前：开机阶段（WiFi 静态池、epdiy 行队列）正是内堆
    // 最紧、最可能失败的时候，见下方 onAllocFailed。
    heap_caps_register_failed_alloc_callback(onAllocFailed);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS...");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed");
    }

    // Dynamic frequency scaling: CPU drops to 80MHz when idle (main loop delay),
    // ramps back to 240MHz while WiFi/BT are active (they hold PM locks).
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    if (esp_pm_configure(&pm_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure failed");
    }

    // ── 板级 bring-up：read_pico_init(EPD/SD 探测/PMU/触摸) + 上电 + 清屏 ──
    if (!board_init()) {
        ESP_LOGE(TAG, "Board initialization failed! System halted.");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // ── 双核分工：core1 的渲染任务（绘制缓冲轮换 + 差分 + 推屏）──────────────
    // 必须在任何 ui_clear() 之前建好 —— epdiy 推屏一次把调用者按住 89~410ms，
    // 搬去 core1 之后 core0 那段时间还能采样输入、服务 BLE/WiFi。详见 ui_render.h。
    ui_render_init();


    // ── WiFi 早初始化 ────────────────────────────────────────────────────────
    // esp_wifi_init() 会一次性分配静态 RX/TX 缓冲池，且**必须落在内部 DMA RAM**
    // （PSRAM 走不了）。等到阅读模式起来后内部 RAM 只剩 ~30KB（epdiy ~33KB +
    // Bluedroid + 阅读器状态），init 必然失败（esf_buf_setup_static: alloc eb fail），
    // OPDS/WiFi 传书就全废了。这里趁内部 RAM 还有 ~116KB 先把驱动初始化好；
    // 无线电不在 begin() 里启动（省电），connect() 时按需 esp_wifi_start()。
    g_wifi.begin();

    // 阅读模式(crossmux 阅读器)注入 epdiy 句柄，之后 HalDisplay/GfxRenderer 才能刷屏。
    crossmux_platform_set_display(board_hl());
    // 同时注入"全刷"落地点：HalDisplay 的 FULL_REFRESH 原来直接调 epd_hl_update_screen()，
    // 那是差分刷，画面无变化时是空操作（长按确认键全局刷新因此毫无反应）。改走这里，
    // 整屏全像素过一遍 GC16，并带上扫描时序/预填/HV 轨保活。
    crossmux_platform_set_full_refresh(update_display_full);
    // 8 灰阶全屏刷（30 相 8-Gray 表 GC16）：残影清账档。
    crossmux_platform_set_gray8_refresh(update_display_gray8);
    // 8 灰阶正文刷（30 相 8-Gray 表 GL16 差分）：正文翻页档，不闪且更快。
    crossmux_platform_set_gray8_text_refresh(update_display_gray8_text);
    // 上面三个钩子只管 FULL/GRAY8/GRAY8_TEXT 三档，HALF/FAST 还是走 epdiy 的裸差分刷
    // ——GL16 全像素、软刷升 GC16 这两条防残影机制在那两档上是缺的，而正文翻页恰好
    // 全走 HALF。注册统一出口后五档全部回到 display.c 的同一处。
    crossmux_platform_set_mode_refresh(update_display_reader);
    // 阅读模式虚拟键盘的打字帧：只推"与上一帧有差异的那块矩形"（编码候选两行快刷、
    // 键盘区/文本输入区局刷）。不注册的话每一帧都走上面的 HALF(整屏 GL16)→被升级成
    // 整屏全像素，用户侧就是"按一个按键就全刷一次"。见 ui_render.cpp 的 reader_vk_present。
    crossmux_platform_set_vk_present(reader_vk_present);

    // 从 NVS 读回外置字体路径（须在字体初始化前，才能让 ttf_font_init 打开 SD 字体）。
    font_store_init();

    // 套用全设备夜间反色（在推屏唯一出口 HalDisplay::displayBuffer 处逐帧取反）。
    // 必须在第一帧之前设好，否则开机首屏按日间画、之后才翻黑。
    board_set_night(g_settings.nightMode());

    // Initialize input (touch/3-key/PMU) after read_pico_init.
    input_init();

    // Initialize SD card (needed before settings AND before font, so SD 外置字体能加载)。
    // g_journal.begin() 内部轮询 read_pico_sd_get_info 直到探测完成并挂载 /sdcard。
    // 无卡时**不再直接停机**（原来是 while(1) 硬停，于是"开机没插卡"就只能重启整机），
    // 改成停在提示页等卡、插卡即继续启动。
    bool sd_ready = g_journal.begin();
    if (!sd_ready) {
        // SD 失败时仅内建字体可用，但这两行提示够用（内容面同样是内置，见上面）。
        g_font.begin(TTF_ROLE_CONTENT);
        g_font.setSize(20);
        while (!sd_ready) {
            // 每 500ms 问一次读卡器。首次探测把状态定死为"无卡"之后，只有驱动侧的插入
            // 重探测能把它拉回可挂载状态（read_pico_sd.c 的 observe_media_locked 观察到
            // 插入就把 probe_state 归零），所以看到 INVALID_STATE + present 要主动
            // start_probe()；否则这里问一万次也只会拿到同一个 NOT_FOUND。
            // 卡在但挂不上（要格式化 / 接触不良）也不死等：把原因写在屏上，用户拔插一次
            // 就是又一次 false→true，重探测会再放行一次。
            int shown = -1;  // 屏上正显示的提示：0 = 无卡，1 = 卡在但挂不上
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(500));
                read_pico_sd_info_t info = {};
                const esp_err_t err = read_pico_sd_get_info(&info);
                if (info.mounted) break;

                const int want = info.present ? 1 : 0;
                if (want != shown) {
                    shown = want;
                    ui_clear();
                    // ui_draw_text_centered 的 y 是**基线**，两行必须隔开一整个行高（UI 20pt = 45px）。
                    // 原来写死的 100/135 只差 35px，比行高还小 → 第二行压在上一行上。
                    ui_draw_text_centered(100, want == 0 ? "请插入SD卡"
                                          : (info.needs_format ? "SD卡需要格式化" : "SD卡挂载失败"));
                    ui_draw_text_centered(100 + g_font.lineHeight(),
                                          want == 0 ? "插入后自动继续启动" : "请重新插拔SD卡");
                    ui_commit();
                    ESP_LOGW(TAG, "waiting for SD card: %s", esp_err_to_name(err));
                }
                if (err == ESP_ERR_INVALID_STATE && info.present) read_pico_sd_start_probe();
            }
            ESP_LOGI(TAG, "SD card ready, continuing boot");
            sd_ready = g_journal.begin();
        }
    }
    ESP_LOGI(TAG, "Journal entries: %d", g_journal.totalEntries());

    // ── SD 写自检：确认卡已挂载且可写，并报告剩余空间。──
    // "保存失败，请检查SD卡" 通常是满卡/坏卡/只读，这里在启动时直接验证并打出 errno。
    {
        uint64_t total_bytes = 0;
        uint64_t free_bytes = 0;
        if (esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes) == ESP_OK) {
            unsigned long long free_mb = (unsigned long long)(free_bytes / (1024 * 1024));
            unsigned long long total_mb = (unsigned long long)(total_bytes / (1024 * 1024));
            ESP_LOGI(TAG, "SD: free=%llu MB total=%llu MB", free_mb, total_mb);
        } else {
            ESP_LOGW(TAG, "SD: info query failed errno=%d (%s)", errno, strerror(errno));
        }

        const char *p = "/sdcard/.sd_write_test";
        FILE *f = fopen(p, "w");
        if (!f) {
            ESP_LOGE(TAG, "SD write test: fopen failed errno=%d (%s)", errno, strerror(errno));
        } else {
            size_t n = fwrite("ok\n", 1, 3, f);
            int c = fclose(f);
            if (n != 3 || c != 0) {
                ESP_LOGE(TAG, "SD write test: write failed n=%u close=%d errno=%d (%s)",
                         (unsigned)n, c, errno, strerror(errno));
            } else {
                ESP_LOGI(TAG, "SD write test: OK (card writable)");
                remove(p);
            }
        }
    }

    // 共享粘贴板：SD 挂载后读一次（三模式共用同一份，写盘在 clipboardPush 里）。
    clipboardLoad();

    // Initialize the UI font AFTER SD mount so SD 外置字体 (ttf_font_init →
    // font_store_get_path → ttf_font_open) 能真正打开；无卡/路径无效回落内建。
    // 界面文本也用**用户选的字体**（除了图标/状态符号与虚拟键盘），所以 g_font 的
    // 文本面就是内容面；没装外置字体时内容面 = 内置，与从前逐像素一致。
    g_font.begin(TTF_ROLE_CONTENT);          // 界面文本：用户字体（未选则内置）
    g_content_font.begin(TTF_ROLE_CONTENT);  // 内容文本：编辑器正文/候选/日记查看器
    g_font.setSize(20);
    // g_vk_font（虚拟键盘）是静态构造的 TTF_ROLE_UI 实例，**不调 begin()**：它会
    // 重置共享格子模型（见 font_renderer.h）。这里什么都不用做。

    // Initialize settings (stored on SD card)
    g_settings.begin();

    // 应用方向设置(landscape 默认 | portrait)：须在 SD/设置之后，重启生效。
    // 改方向后由整屏重刷(下面 splash 的 ui_commit)出图。
    board_apply_orientation(g_settings.orientation().c_str());

    // 工作模式: "journal"(个人日记) 或 "quick"(快捷编辑), 重启生效
    g_quickEdit = (g_settings.appMode() == "quick");
    if (g_quickEdit) quickEditInit();

    // Initialize RTC (PMU 背书的软件 RTC)
    if (g_rtc.begin()) {
        ESP_LOGI(TAG, "PMU RTC initialized");
    } else {
        ESP_LOGW(TAG, "PMU RTC not available or invalid time");
    }

    // Initialize battery (PMU soc_permille)
    battery_init();

    // 开机直接进阅读模式：不再显示"个人日记/快捷编辑 + 版本号"启动画面，改画 Yan Reader
    // 开机动画（见 boot_splash.cpp）。它必须排在这里 —— board_apply_orientation() 上方
    // 已调用，动画的 ui_commit 正是让方向生效的那次整屏刷；早于方向设置画等于白画。
    // 末帧停在屏上，盖住阅读器装载(实测约 3s)那段空屏，首屏出图时被顶掉。
    // cy 保留给下方 NTP 同步提示定位用（动画不占这个变量）。
    bootSplashDraw();
    int cy = (SCREEN_H - 2 * LINE_SPACING) / 2;

    // Initialize WiFi manager (但不自动连接)
    // WiFi 将在需要时按需连接（WebDAV同步、Flomo发送、Deepseek提示生成等）

    // Set timezone from settings (for local time display)
    {
        std::string tz = g_settings.timezone();
        if (tz.empty()) tz = "CST-8";
        setenv("TZ", tz.c_str(), 1);
        tzset();
    }

    // Time sync: prefer RTC if its time is recent (>= July 2026), otherwise NTP
    {
        time_t rtcTime = g_rtc.getTime();
        bool rtcRecent = (rtcTime >= 1782864000); // July 1, 2026 00:00:00 UTC

        if (rtcRecent) {
            struct timeval tv = {(time_t)rtcTime, 0};
            settimeofday(&tv, NULL);
            struct tm *tm = localtime(&rtcTime);
            char ts[32];
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
            ESP_LOGI(TAG, "RTC time is recent, using directly: %s", ts);
        } else {
            ESP_LOGW(TAG, "RTC time (%lld) is before July 2026, attempting NTP sync...", (long long)rtcTime);
            std::string ssid = g_settings.wifiSsid();
            if (!ssid.empty()) {
                std::string ntp = g_settings.ntpServer();
                if (ntp.empty()) ntp = "pool.ntp.org";

                // 上面那帧（空白启动屏）已经提交了，这一帧是叠在它上面补一行提示：
                // 双缓冲下必须显式取回上一帧的内容，否则会画到别的缓冲上去。
                ui_render_begin_overlay();
                ui_draw_text_centered(cy + 2 * LINE_SPACING, "正在同步时间...");
                ui_commit();

                std::string pass = g_settings.wifiPassword();
                g_wifi.begin();
                if (g_wifi.connect(ssid.c_str(), pass.c_str())) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_sntp_stop();
                    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
                    esp_sntp_setservername(0, ntp.c_str());
                    esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
                    esp_sntp_init();

                    time_t now = 0;
                    for (int i = 0; i < 100; i++) {
                        vTaskDelay(pdMS_TO_TICKS(200));
                        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
                            time(&now);
                            break;
                        }
                    }
                    if (now > 1782864000) {
                        struct tm *tm = localtime(&now);
                        char ts[32];
                        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
                        ESP_LOGI(TAG, "NTP sync succeeded: %s", ts);
                        g_rtc.setTime(now);
                    } else {
                        ESP_LOGW(TAG, "NTP sync timeout (%s)", ntp.c_str());
                    }
                    esp_sntp_stop();
                } else {
                    ESP_LOGW(TAG, "WiFi connection failed for NTP sync");
                }
                g_wifi.disconnect();
            } else {
                ESP_LOGW(TAG, "WiFi not configured, cannot NTP sync");
            }
            // Fallback: use whatever RTC has, even if old
            if (rtcTime > 1704067200) {
                struct timeval tv = {(time_t)rtcTime, 0};
                settimeofday(&tv, NULL);
                ESP_LOGW(TAG, "Fallback to RTC time");
            }
        }
    }

    // Seed RNG with hardware random for prompt selection
    srand(esp_random());

    // Initialize IME
    auto &ime = IME::getInstance();
    ime.begin();
    // Set candidate page size based on the default UI font size (20pt)
    ime.setPageSize(7);
    // 候选字按显示宽度动态分页: 可用宽度与各界面候选行一致。
    // 量宽度只有一个口径：候选行(虚拟键盘的候选条、实体键盘的输入法条)都按
    // 「候选字大小」设置直写像素，见 ui_helpers 的 imeCandStrW —— 输入法算"一行放
    // 几个"和面板实际画得下几个，必须是同一份结论，否则大字号时最后一两个候选
    // 会被挤出候选行，看不见也点不到。
    ime.setWidthFn([](const char *s) -> int { return imeCandStrW(s); });
    // 候选行宽度**现取**(回调，不是常数)：SCREEN_W 是当前方向的逻辑宽 —— 本机开机是
    // 横屏(1216−12)，而敲字的界面大多在竖屏(684−12)，开机取一次就钉死在横屏那一档上，
    // 于是竖屏的候选页按 1204 切、行只有 672：一页里多出来的候选正是上面那句"看不见
    // 也点不到"。回调在每次 buildPage 现取，横竖屏各按各的宽度切。
    ime.setDisplayWidthFn(imeCandidateLineWidth);

    // Initialize Bluetooth keyboard in background (non-blocking, faster boot)
    spawnBtInit();

    ESP_LOGI(TAG, "Ready!");

    // ── App State Machine ────────────────────────────────────────────────
    // 快捷编辑模式直接进入编辑器(续上次文件); 默认进入阅读模式
    AppState currentState = g_quickEdit ? APP_EDITOR : APP_READER;
    ScreenContext ctx;
    if (g_quickEdit) {
        ctx.prevState = APP_SETTINGS;  // 编辑器 Esc → 设置, 设置 Esc → 编辑器
        ctx.promptText = "";
    }
    // 三种模式各自的"上次停留界面"：power 键在 阅读 → 写作 → 计划 → 阅读 间轮换。
    // 计划模式目前只有 GTD(任务/项目)这一块，根界面就是 APP_GTD。
    static AppState s_writingState = APP_MAIN;
    static AppState s_planState = APP_GTD;
    // ── 切模式不打断手头的工作 ────────────────────────────────────────────
    // 只记住"上次停在哪个界面"是不够的，真正的状态都不在这儿：计划模式的当前 tab /
    // 项目下钻 / 光标活在 screen_gtd.cpp 的文件级静态 g 里，写作模式的正文 / 光标 /
    // 未保存的草稿活在 screen_editor.cpp 的 g_editor 里，阅读模式的书 / 页码 / 标签
    // 活在 screen_reader.cpp 的 st（外加退出时记下的 s_return 返回点）。
    // 所以规则只有一条：**切模式时不要重跑那个模式的 init**——静态原样还在。
    // 各屏自己的"是否已经 init 过"现在住在屏幕自己的文件里（screen_gtd.cpp 的
    // s_gtdInited、screen_editor.cpp 的 s_editorInited），只在"本模式本次开机第一次
    // 进入"时置位，之后切模式、退回写作菜单都不清零；重跑 init 才是把用户工作冲掉的
    // 唯一原因（screen_gtd_init 会把 tab 清成收集箱、screen_editor_init 会把正文清空
    // 重载）。这个"什么时候置位"的决定归各屏的 enter 钩子，main 不再过问。
    // 唯一要"每次进入都补做"的是模式自己的方向设置（退出时还给了全局）。
    // 某个界面属于哪个模式：0=阅读 1=写作 2=计划。appModeOfState 见文件顶部。
    //
    // 开机首帧在这里同步一次生命周期：把 s_activeScreen（APP_QUIT）切到 currentState，
    // 跑一遍它的 enter（阅读器开局在书架时等价于原来的 screen_reader_init()）。
    syncScreenLifecycle(currentState, ctx);

    // 物理按键状态(时间制,不依赖主循环节拍)。电容键由 input_key_held 消抖后驱动。
    struct BtnState {
        int64_t press_start_us = 0;   // 当前按下起始时刻(0=未按下)
        int64_t last_release_us = 0;  // 上次松开时刻(双击窗口基准)
        bool is_double = false;       // 本次按下为双击第二下
        bool long_fired = false;      // 本次按下已触发长按
    } btn_user, btn_boot;

    while (currentState != APP_QUIT) {
        checkLightSleep(currentState);
        // 运行中拔卡/插卡（见 sdHotplugTick）：每 500ms 自己掐一次表，插在 checkLightSleep
        // 后面是因为它俩都是"这一拍先问一遍外部世界"的活，且都不该被下面的按键分发影响。
        sdHotplugTick(currentState);

        int key = g_bt.readKey();
        g_key_from_ble = (key != 0);
        if (key < 0) key = 0;

        // 触摸/3键/电源键短按 → 键码(与 g_bt.readKey 互补)
        {
            int hw = input_poll();
            if (hw != 0 && key == 0) key = hw;
        }

        // BLE 键盘输入视为活动,重置空闲休眠计时
        if (key > 0) s_last_activity_us = esp_timer_get_time();

        // 自适应屏幕方向（写作/计划模式选「自适应」时）：判定在 input_poll 里的采样钩子
        // 做（每 120ms 一拍，与"晃动机身=全刷"共用同一次 I2C 读），**落地只在这里** ——
        // 转一次屏是 drain + 整屏 GC16（约 1.8s），不能从补采样那种上下文里发。
        // s_last_activity_us 当"最近有没有按键/触摸"：打字、划列表期间不掉头。
        //
        // 转成了还得补两件事，都在下面：① 候选行按新宽度重分页（分页表只在 buildPage
        // 里算，而它只在候选变化时被叫 —— 光转屏没人叫它）；② 让**编辑器**这一屏重画
        // （它的空转 tick 按 drawnOnce 记账跳过重绘，方向换了它不会自己知道；别的界面
        // 空转时本来就每拍 draw+commit，作废快照那一步已经在 tick 里做过了）。
        if (auto_orient_tick(autoOrientWantedFor(currentState), s_last_activity_us)) {
            ime.repaginateForWidthChange();
            if (currentState == APP_EDITOR) screen_editor_reset_drawn();
        }

        // TEMP 堆水位（定位内部 RAM 耗尽崩溃，见 bt_keyboard.cpp:947 的 fgets 锁 OOM）
        {
            static int64_t s_heap_log_us = 0;
            static int64_t s_heap_info_us = 0;
            int64_t nowh = esp_timer_get_time();
            if (nowh - s_heap_log_us > 2000000) {
                s_heap_log_us = nowh;
                unsigned intFree = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                unsigned intLargest = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
                ESP_LOGI("Heap", "int free=%u largest=%u min=%u psram free=%u",
                         intFree, intLargest,
                         (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                // 提前预警：内堆见底时刷屏/网络一步就 OOM。留出 4KB 余量当红线——
                // 到这一步还没崩，说明是在哪次大分配前后擦边，先报警好定位。
                if (intFree < 4096 || intLargest < 3072) {
                    ESP_LOGW("Heap", "internal RAM low: free=%u largest=%u (red line 4096/3072)",
                             intFree, intLargest);
                }
                unsigned oomCount = s_oom_count.load(std::memory_order_relaxed);
                if (oomCount) {
                    const char *fn = s_oom_fn.load(std::memory_order_relaxed);
                    ESP_LOGE("Heap", "OOM seen %u time(s), last: size=%u caps=0x%x in %s",
                             oomCount, (unsigned)s_oom_size.load(std::memory_order_relaxed),
                             (unsigned)s_oom_caps.load(std::memory_order_relaxed), fn ? fn : "?");
                }
            }
            // 每 60s 打一次内部堆分区详情（含为 DMA 预留的那块）
            if (s_heap_info_us != 0 && nowh - s_heap_info_us > 60000000) {
                s_heap_info_us = nowh;
                ESP_LOGI("Heap", "---- internal regions ----");
                heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
            } else if (s_heap_info_us == 0) {
                s_heap_info_us = nowh;
            }
        }

        // Check for key repeat events
        g_bt.checkKeyRepeat();

        // 电源键短按 → 阅读/写作/计划三模式轮换（全局，最高优先级，不被任何界面吞掉）。
        // **双击待机已取消**：300ms 的双击窗口对电源键太短，实际按不出来。待机改由
        // 长按中间确认键触发（见下面那一段）。
        if (key == KEY_POWER) {
            // 切模式前把共享的单行输入框选区会话收掉：它和编辑器一样把状态挂在
            // 文件级静态量上，带着跨模式会污染下一个界面（比如在编辑器里凭空多出
            // 一条按钮条——两边共用同一个 edit_menu 控件）。
            textSelReset();
            int cur = appModeOfState(currentState);
            if (cur == 1) {
                s_writingState = currentState;
                // 编辑器被切走时"收选区/按钮条/粘贴板"那件事搬进了 screen_editor_leave
                // （它是切模式下唯一还留在屏幕上的现场清理，属于编辑器的内部事务）。
            } else if (cur == 2) {
                s_planState = currentState;
            }
            int next = (cur + 1) % 3;
            // 阅读器/计划模式的退出清理（释放书对象、还方向）现在由 Screen 表的 leave 钩子
            // 在下面 syncScreenLifecycle 里统一做 —— 这里只负责把 currentState 改掉。
            // 阅读器直绘 framebuffer 绕过了 ui_commit 的快照；写作/计划界面若本轮
            // 渲染结果与陈旧快照一致，memcmp 会误判"无变化"而跳过整屏重发，
            // 导致屏上残留阅读器画面。这里强制作废快照，下一次 ui_commit 必发。
            ui_invalidate_snapshot();
            if (next == 0) {
                currentState = APP_READER;
            } else if (next == 1) {
                currentState = s_writingState;
            } else {
                currentState = s_planState;
            }
            ESP_LOGI(TAG, "switch to mode %d (%d)", next, (int)currentState);
            // 切模式这一帧必须立刻跑一次生命周期：leave(旧) + enter(新)。放在 switch 之后
            // 也能兜住（dispatch 后面的收口还是同一对状态），但那要等本帧 dispatch 走完，
            // 中间如果 next==0 会进 scrReader 而阅读器还没 init。所以这里先同步一次。
            const int64_t tSwitch = esp_timer_get_time();
            syncScreenLifecycle(currentState, ctx);
            // 按下电源键（这一拍才开始处理）到新界面第一帧走完的总时长，含 leave(旧) +
            // enter(新)。新界面是阅读器时它自己那本「开书拆账」同时打出来，两边一对就差
            // 出 leave 那一份 —— 否则"卡这一段"永远分不清是旧界面的收尾还是新界面的开局。
            ESP_LOGI(TAG, "切模式耗时: %lld ms (→ state %d)",
                     (long long)((esp_timer_get_time() - tSwitch) / 1000), (int)currentState);
            key = 0;
        }

        // 电源键长按满 8s → 抢在 PMU 满 10s 硬断电之前把「已关机」铺满、刷 SD，然后
        // 在这里等断电（不再往下走，否则主循环会把这页盖掉）。排在短按切模式之后：
        // 两者是同一条 PMU 事件队列里的不同事件，poll_pmu_key 已经分开。
        if (key == KEY_POWER_HOLD) {
            ESP_LOGW(TAG, "电源键长按 → 关机 (state=%d)", (int)currentState);
            powerOffWithNotice();
            key = 0;
        }

        // 长按中间确认键 → 待机（light sleep，任意键唤醒表盘由 standbyClockDraw 铺好）。
        // 它原来干的是"整屏 GC16 全刷"，那个动作已经改由**晃动机身**触发（见下面的
        // KEY_SHAKE 与 hw/input.cpp 的 shake_poll）。
        // 唯一的例外是阅读器里三个把这个键当**动作键**的子界面——词典管理（长按删词典）、
        // 按键映射（长按解绑）、阅读统计里选中的是一本书时（长按删书）：它们的长按是
        // 二次确认式的动作，被待机吃掉功能就没了，所以放行给阅读器自己处理
        // （screen_reader_long_confirm_is_action）。
        if (key == KEY_LONG_CONFIRM && !screen_reader_long_confirm_is_action()) {
            ESP_LOGI(TAG, "长按确认键 → 待机 (state=%d)", (int)currentState);
            enterLightSleep();
            s_last_activity_us = esp_timer_get_time();
            key = 0;
        }

        // 晃动机身 → 一次全刷（清残影），三个模式通用。检测在 hw/input.cpp（读 SC7A20H
        // 加速度计）。阅读模式自己认这个键——它的全刷要走自己的刷新档位与计数
        // （见 screen_reader_handle），所以这里只给非阅读模式兜底。
        if (key == KEY_SHAKE && currentState != APP_READER) {
            ESP_LOGI(TAG, "晃动 → 全屏刷新 (state=%d)", (int)currentState);
            ui_full_refresh_now();
            key = 0;
        }

        // Global Ctrl+Space IME toggle (only for editor)
        // 切换后标记重绘:编辑器空闲路径不重绘,状态栏的输入法标签须立即刷新
        if (key == KEY_IME_TOGGLE && currentState == APP_EDITOR && !app_editor_search_active() && !app_editor_help_active() && !app_editor_popup_active()) {
            app_toggle_ime();
            screen_editor_reset_drawn();
            key = 0;
        }
        // Shift+Space fullwidth toggle (only when IME active in editor)
        if (key == KEY_FULLWIDTH_TOGGLE && currentState == APP_EDITOR && app_ime_active() && !app_editor_search_active() && !app_editor_help_active() && !app_editor_popup_active()) {
            app_toggle_fullwidth();
            screen_editor_reset_drawn();
            key = 0;
        }
        // Ctrl+Shift+F simplified/traditional toggle (only when IME active in editor)
        if (key == KEY_TRAD_TOGGLE && currentState == APP_EDITOR && app_ime_active() && !app_editor_search_active() && !app_editor_help_active() && !app_editor_popup_active()) {
            app_toggle_trad();
            screen_editor_reset_drawn();
            key = 0;
        }
        // Left Shift tap → temp English mode toggle (only when IME active in editor)
        if (key == KEY_LSHIFT_TAP && currentState == APP_EDITOR && app_ime_active() && !app_editor_search_active() && !app_editor_help_active() && !app_editor_popup_active()) {
            app_toggle_english();
            screen_editor_reset_drawn();
            key = 0;
        }
        // Ctrl+D → IME user word deletion mode (only when IME active in editor)
        if (key == 0x04 && currentState == APP_EDITOR && app_ime_active() &&
            !IME::getInstance().predicting() &&
            !app_editor_search_active() && !app_editor_help_active() && !app_editor_popup_active()) {
            app_toggle_ime_delete_mode();
            {
                std::string imeStatus = IME::getInstance().takeStatusMessage();
                if (!imeStatus.empty()) {
                    ctx.statusMessage = imeStatus;
                    ctx.statusDuration = 30;
                }
            }
            screen_editor_reset_drawn();
            key = 0;
        }

        // ── BT auto-reconnect retry ──────────────────────────────────────
        // 多设备: 断线后按最近使用顺序轮询已配对设备, 谁在线连谁
        // 面板内暂停自动重连, 避免干扰扫描/管理
        {
            static int64_t last_bt_retry_us = 0;
            static int64_t last_bt_reload_us = 0;
            static bool bt_list_loaded = false;
            static int bt_try_idx = 0;
            static bool bt_was_connected = false;

            if (currentState == APP_BT_MANAGE) {
                // 用户正在管理面板, 暂停自动重连
                bt_was_connected = false;
                last_bt_retry_us = 0;
            } else if (g_bt.isConnected()) {
                if (!bt_was_connected) {
                    ESP_LOGI(TAG, "Bluetooth connected, stopping retry logic");
                }
                bt_was_connected = true;
                last_bt_retry_us = 0;
            } else {
                if (bt_was_connected) {
                    ESP_LOGW(TAG, "Bluetooth disconnected");
                    bt_was_connected = false;
                }

                if (!bt_was_connected) {
                    // Periodically reload paired device list (反映面板增删/新连接)
                    if (g_bt.isInitialized()) {
                        int64_t now_us = esp_timer_get_time();
                        if (last_bt_reload_us == 0 || (now_us - last_bt_reload_us) > 30000000) {
                            last_bt_reload_us = now_us;
                            g_bt.loadPairedDevices();
                            bt_list_loaded = g_bt.pairedDeviceCount() > 0;
                            bt_try_idx = 0;
                            if (bt_list_loaded)
                                ESP_LOGI(TAG, "Loaded %d paired device(s)", g_bt.pairedDeviceCount());
                        }
                    }

                    if (bt_list_loaded && !g_bt.isConnecting()) {
                        int64_t now_us = esp_timer_get_time();
                        if (last_bt_retry_us == 0 || (now_us - last_bt_retry_us) > 2000000) {
                            last_bt_retry_us = now_us;
                            int n = g_bt.pairedDeviceCount();
                            if (n > 0) {
                                if (bt_try_idx >= n) bt_try_idx = 0;
                                const BtPairedDevice *p = g_bt.getPairedDevice(bt_try_idx);
                                ESP_LOGI(TAG, "BT auto-reconnect retry %d/%d...",
                                         bt_try_idx + 1, n);
                                g_bt.connectBDA(p->bda, p->addr_type);
                                bt_try_idx = (bt_try_idx + 1) % n;
                            }
                        }
                    }
                }
            }
        }

        // ── Physical key handling (3 电容键 → input_key_held) ────────────
        // 单击动作在双击窗口结束后才生效(防止双击第一下误发导航键,仅蓝牙管理面板有双击动作)
        if (s_pending_single.key != 0) {
            if (esp_timer_get_time() - s_pending_single.queued_us >= BTN_DOUBLE_WINDOW_US) {
                if (currentState == APP_BT_MANAGE ||
                    (currentState == APP_GTD && screen_gtd_accept_physical_buttons()) ||
                    currentState == APP_POLISH ||
                    currentState == APP_POLISH_PROMPT ||
                    currentState == APP_READER) {   // 阅读页：单击=翻页，双击=跳章（见下）
                    key = s_pending_single.key;
                }
                s_pending_single.key = 0;
            }
        }

        // USER key (电容键 KEY1)
        {
            bool held = input_key_held(PICO_KEY_USER);
            if (held) {
                if (btn_user.press_start_us == 0) {
                    btn_user.press_start_us = esp_timer_get_time();
                    s_last_activity_us = btn_user.press_start_us;
                    // 上次松开后的双击窗口内再次按下 → 双击第二下,取消第一下的单击排队
                    btn_user.is_double = (btn_user.last_release_us > 0 &&
                        (btn_user.press_start_us - btn_user.last_release_us) < BTN_DOUBLE_WINDOW_US);
                    if (btn_user.is_double) {
                        s_pending_single.key = 0;
                        ESP_LOGI(TAG, "USER double-click detected (gap=%lld ms)",
                                 (long long)(btn_user.press_start_us - btn_user.last_release_us) / 1000);
                    }
                }
                if (!btn_user.long_fired &&
                    (esp_timer_get_time() - btn_user.press_start_us) >= BTN_LONG_PRESS_US) {
                    btn_user.long_fired = true;
                    s_pending_single.key = 0;
                    if (currentState == APP_BT_MANAGE) {
                        // 蓝牙管理面板: 长按→连接选中设备(不经过短按,避免错位)
                        key = 0x0A;
                    } else if (currentState == APP_GTD && screen_gtd_accept_physical_buttons()) {
                        // GTD任务管理: 长按→Tab 切换标签
                        key = '\t';
                    } else if (currentState == APP_POLISH ||
                               currentState == APP_POLISH_PROMPT) {
                        // 润色面板/提示词编辑: 长按不动作,避免误进蓝牙管理
                        key = 0;
                    } else {
                        currentState = APP_BT_MANAGE;
                        key = 0;
                    }
                }
            } else {
                if (btn_user.press_start_us != 0) {
                    int64_t now = esp_timer_get_time();
                    int64_t dur = now - btn_user.press_start_us;
                    btn_user.press_start_us = 0;
                    if (dur >= BTN_DEBOUNCE_US && !btn_user.long_fired) {
                        bool gtdBrowse = (currentState == APP_GTD && screen_gtd_accept_physical_buttons());
                        bool gtdProjList = (currentState == APP_GTD && screen_gtd_in_project_list());
                        if (currentState == APP_BT_MANAGE && btn_user.is_double) {
                            // 双击: 管理模式→添加, 扫描模式→返回
                            key = screen_bt_manage_scan_mode() ? 0x1B : 'a';
                        } else if (currentState == APP_BT_MANAGE) {
                            // 单击: 上移,等待双击窗口确认非双击后生效
                            s_pending_single.key = KEY_UP;
                            s_pending_single.queued_us = now;
                        } else if (currentState == APP_POLISH && btn_user.is_double) {
                            // 润色面板双击: 确认(Enter)
                            key = 0x0A;
                        } else if (currentState == APP_POLISH) {
                            // 润色面板单击: 上滚
                            s_pending_single.key = KEY_UP;
                            s_pending_single.queued_us = now;
                        } else if (currentState == APP_POLISH_PROMPT && btn_user.is_double) {
                            // 提示词编辑双击: 确认(Enter=换行)
                            key = 0x0A;
                        } else if (currentState == APP_POLISH_PROMPT) {
                            // 提示词编辑单击: 上移
                            s_pending_single.key = KEY_UP;
                            s_pending_single.queued_us = now;
                        } else if (gtdBrowse && btn_user.is_double) {
                            // GTD任务管理双击: 项目列表→进入选中项目; 平铺/项目树→切换任务状态
                            key = gtdProjList ? 0x0A : ' ';
                        } else if (gtdBrowse) {
                            // GTD任务管理单击: 上移,等待双击窗口确认非双击后生效
                            s_pending_single.key = KEY_UP;
                            s_pending_single.queued_us = now;
                        } else if (currentState == APP_READER) {
                            // 阅读模式: 右侧键 = 下一页（阅读页由 screen_reader 认这一对
                            // "哪一侧"的键码自己翻；目录/菜单等界面它再翻回 KEY_UP）。
                            // 双击 = 下一章。**单击也必须排队等双击窗口**：阅读页翻一页要
                            // 刷 1.8s（日志 `翻页耗时`），第一下先发出去就把第二下整个吞在
                            // 刷屏里，双击永远认不出来——所以照 GTD/蓝牙面板那套先压住。
                            key = btn_user.is_double ? KEY_NEXT_CHAPTER : 0;
                            if (!btn_user.is_double) {
                                s_pending_single.key = KEY_CAP_RIGHT;
                                s_pending_single.queued_us = now;
                            }
                        } else if (currentState != APP_EDITOR) {
                            // 通用: USER 单击 = 上移。主界面/设置/浏览/查看/大纲/历史/灵感等
                            // 无双击动作的屏立即生效; 编辑器双击另有语义,单击不导航。
                            key = KEY_UP;
                        }
                    }
                    btn_user.last_release_us = now;
                    btn_user.long_fired = false;
                    btn_user.is_double = false;
                }
            }
        }

        // BOOT key (电容键 KEY2)
        {
            bool held = input_key_held(PICO_KEY_BOOT);
            if (held) {
                if (btn_boot.press_start_us == 0) {
                    btn_boot.press_start_us = esp_timer_get_time();
                    s_last_activity_us = btn_boot.press_start_us;
                    btn_boot.is_double = (btn_boot.last_release_us > 0 &&
                        (btn_boot.press_start_us - btn_boot.last_release_us) < BTN_DOUBLE_WINDOW_US);
                    if (btn_boot.is_double) s_pending_single.key = 0;
                }
                if (!btn_boot.long_fired &&
                    (esp_timer_get_time() - btn_boot.press_start_us) >= BTN_LONG_PRESS_US) {
                    // 长按左侧键 = 返回（三种模式通用，2026-10 从"待机"改过来）。
                    // 阅读模式认 KEY_BACK 的"无浮层→回书架 / 有浮层→退一层"语义，
                    // 其余模式在主循环后面被翻成 Esc 退一层。
                    // 唤醒时按住的那一下不算：否则开机瞬间就被自己"返回"一次。
                    btn_boot.long_fired = true;
                    s_pending_single.key = 0;
                    if (!s_boot_wake_release_pending) key = KEY_BACK;
                }
            } else {
                // 唤醒按键可能已在扫描间隙松开,直接清除挂起的唤醒保护标记
                if (s_boot_wake_release_pending && btn_boot.press_start_us == 0) {
                    s_boot_wake_release_pending = false;
                }
                if (btn_boot.press_start_us != 0) {
                    int64_t now = esp_timer_get_time();
                    int64_t dur = now - btn_boot.press_start_us;
                    btn_boot.press_start_us = 0;
                    if (dur >= BTN_DEBOUNCE_US && !btn_boot.long_fired) {
                        if (currentState == APP_BT_MANAGE) {
                            if (btn_boot.is_double) {
                                // 双击: 管理模式→删除, 扫描模式→返回
                                key = screen_bt_manage_scan_mode() ? 0x1B : 'd';
                            } else {
                                // 单击: 下移,等待双击窗口确认非双击后生效
                                s_pending_single.key = KEY_DOWN;
                                s_pending_single.queued_us = now;
                            }
                        } else if (currentState == APP_GTD && screen_gtd_accept_physical_buttons()) {
                            if (btn_boot.is_double) {
                                // GTD任务管理双击: 项目树→返回项目选择菜单; 其他→无动作
                                screen_gtd_physical_double_boot();
                            } else {
                                // GTD任务管理单击: 下移(不触发休眠)
                                s_pending_single.key = KEY_DOWN;
                                s_pending_single.queued_us = now;
                            }
                        } else if (currentState == APP_EDITOR && btn_boot.is_double && !app_editor_search_active() && !app_editor_help_active() && !app_editor_popup_active()) {
                            // 编辑器双击: 进入AI润色面板(全文润色)
                            screen_polish_set_scope(POLISH_WHOLE);
                            currentState = APP_POLISH;
                            key = 0;
                        } else if (currentState == APP_POLISH) {
                            if (btn_boot.is_double) {
                                // 润色面板双击: 取消(Esc)
                                key = 0x1B;
                            } else {
                                // 润色面板单击: 下滚
                                s_pending_single.key = KEY_DOWN;
                                s_pending_single.queued_us = now;
                            }
                        } else if (currentState == APP_POLISH_PROMPT) {
                            if (btn_boot.is_double) {
                                // 提示词编辑双击: 取消(Esc)
                                key = 0x1B;
                            } else {
                                // 提示词编辑单击: 下移
                                s_pending_single.key = KEY_DOWN;
                                s_pending_single.queued_us = now;
                            }
                        } else if (currentState == APP_READER && dur < BTN_LONG_PRESS_US) {
                            // 阅读模式: 左侧键 = 上一页；双击 = 上一章（同 USER 键那套，
                            // 单击排队等双击窗口——阅读页翻页刷屏 1.8s，不排队就吞掉第二下）。
                            key = btn_boot.is_double ? KEY_PREV_CHAPTER : 0;
                            if (!btn_boot.is_double) {
                                s_pending_single.key = KEY_CAP_LEFT;
                                s_pending_single.queued_us = now;
                            }
                        } else if (currentState != APP_EDITOR && dur < BTN_LONG_PRESS_US) {
                            // 通用: BOOT 单击 = 下移。主界面/设置/浏览/查看/大纲/历史/灵感等
                            // 无双击动作的屏立即生效。编辑器双击另有语义,单击不导航。
                            key = KEY_DOWN;
                        } else if (s_boot_wake_release_pending) {
                            // 这是 BOOT 唤醒按键的释放,不动作,避免唤醒即被当成一次返回
                            s_boot_wake_release_pending = false;
                        }
                        // 长按不再休眠: 已在上面的按下分支里发 KEY_BACK(long_fired 为真
                        // 时整个释放分支都不会进来)。待机改由长按中间确认键触发。
                    }
                    btn_boot.last_release_us = now;
                    btn_boot.long_fired = false;
                    btn_boot.is_double = false;
                }
            }
        }

        // Global Ctrl+I → inspiration panel (works from any screen including editor)
        if (key == KEY_CTRL_I && currentState != APP_INSPIRATION && !app_editor_search_active() && !app_editor_help_active() && !app_editor_popup_active()) {
            inspReturnTo = currentState;
            if (currentState == APP_EDITOR) inspEditorReturnTo = ctx.prevState;
            currentState = APP_INSPIRATION;
            key = 0;
        }

        // 触摸上下滑产生翻页键：**上下滑 = 整页翻**，这条在各界面内部实现（列表按屏翻、
        // 长文按屏滚），主循环只把还没实现的界面回退成单步上下。名单里的界面都自己
        // 按子状态决定：列表按屏翻（一屏放得下就什么都不做），表单/小菜单仍单步。
        // 不在名单里的（写作主菜单的图标行、编辑器正文、文件信息页、提示词编辑器…）
        // 保持原来的单步语义。
        switch (currentState) {
            case APP_SETTINGS:    // 分类/字段/选项弹层/词典表
            case APP_GTD:         // 任务列表（screen_gtd 的 gtdScrollByPage）
            case APP_READER:      // 目录/书架/书签/脚注/最近/统计… 见 screen_reader
            case APP_BROWSER:     // 日记列表
            case APP_VIEWER:      // 查看正文（横排按行、竖排按列）
            case APP_HISTORY:     // 历史版本列表 + 预览
            case APP_OUTLINE:     // 项目/标题树/标签/书签
            case APP_INSPIRATION: // 灵感列表 + 检索结果
            case APP_FLOMO:       // 条目列表 + 检索结果
            case APP_BT_MANAGE:   // 扫描列表 / 已配对列表
            case APP_POLISH:      // 润色结果页
                break;
            default:
                if (key == KEY_PAGE_UP) key = KEY_UP;
                else if (key == KEY_PAGE_DOWN) key = KEY_DOWN;
                break;
        }

        // 触摸拖动增量：计划模式的列表用它平滑滚屏，阅读模式用它拖选区的两个手柄，
        // 编辑器用它拖触摸选区的两个手柄。编辑器**不在选区会话时也会把这个键吃掉**，
        // 免得漏进后面的插入/IME 分支（IME 分支把任何键都喂给 g_ime，有未上屏组合时
        // 未知键会上屏一个候选字）。其它界面一律清掉。
        if (key == KEY_TOUCH_DRAG && currentState != APP_GTD && currentState != APP_READER
            && currentState != APP_EDITOR) key = 0;
        // 触摸长按：计划模式（长按列表项弹编辑菜单）、写作模式的大纲（同款菜单）、
        // 阅读模式（长按选词标注）、编辑器（长按正文选字弹按钮条 / 长按状态栏左端的
        // 模式标记 = AI 提示词）和蓝牙管理（长按已配对设备弹连接/删除菜单）都要拿到
        // 原始长按键，由各自界面决定语义；其余界面保持原来的"长按就是 Esc/返回"。
        // 各界面内部再按当前子界面细化（编辑器把这种长按弹成快捷菜单——返回改走菜单/
        // 虚拟按键；输入框型界面把没落在输入行上的长按翻回 0x1B）。
        // 设置界面与润色提示词是打字界面，也放进来——它们的文本框（多行/折行）要接
        // 共享的触摸选区（text_sel），长按得原样送到宿主。
        if (key == KEY_TOUCH_LONG && currentState != APP_GTD && currentState != APP_READER
            && currentState != APP_OUTLINE && currentState != APP_EDITOR
            && currentState != APP_FLOMO && currentState != APP_BT_MANAGE
            && currentState != APP_INSPIRATION && currentState != APP_SETTINGS
            && currentState != APP_POLISH_PROMPT) key = 0x1B;

        // 屏幕边缘向中间横划 = 返回（三种模式通用，见 pjournal_app.h KEY_BACK）。
        // ① 虚拟键盘候选行上的横划仍归键盘：editorVkSwipePage 按**按下点**认候选行，
        //    起点在候选行里的划动是"翻候选页"而不是返回——先替各界面问一次，免得
        //    "按住第一个候选字往左划"被当成边缘返回退出界面。
        // ② 阅读模式原样传下去：它自己知道现在是"阅读页无浮层"（→ 回书架）还是
        //    弹了菜单（→ 退一层），这两个语义串不成一个 0x1B，所以不在这里翻。
        // ③ 其余模式 = Esc(0x1B) 退一层（写作/计划/灵感/设置的老返回语义）。
        if (key == KEY_BACK) {
            int px = 0, py = 0, bdir = 1;
            input_back_dir(&bdir);
            if (editorVkVisible() && input_press_xy(&px, &py) && editorVkSwipePage(px, py, bdir)) {
                key = 0;   // 被候选行吃掉（翻候选页）
            } else if (currentState != APP_READER) {
                key = 0x1B;
            }
        }

        // 按键反馈音：全局生效（阅读/写作/计划/设置…所有界面一视同仁），凡这一轮真有一个键
        // 要交给界面就响一声。两个例外：① 写作模式的编辑器自己有一套管得更细的规则
        // （整词上屏按字数连响，见 screen_editor.cpp），在这里再响一次会变成回声；
        // ② KEY_TOUCH_DRAG —— 计划模式列表的平滑滚动是"一次拖动连发很多帧"，逐帧响会变
        // 成机关枪。开关本身在 typingClickPlay 内部的 typingClickEnabled() 把关。
        if (key > 0 && currentState != APP_EDITOR && key != KEY_TOUCH_DRAG) typingClickPlay(1);

        // 界面生命周期收口（前半）：上面那些"在派发之前就改了 currentState"的地方
        // （电源键切模式、Ctrl+I 弹灵感）在这一行得到 enter/leave。放在派发之前是因为
        // 各屏的 enter 里有"第一帧就要成立"的东西（阅读器重建 + 画首屏、计划模式定方向）。
        syncScreenLifecycle(currentState, ctx);

        // ── 派发（表驱动，见 main/ui/screen.h 与文件上方的 kScreens）──────────
        // 每屏一行；没列到的状态（APP_PROMPT_SEL）没有 handle，回主菜单——与原来的
        // switch default 分支一致。空转路径的睡眠统一在这里做（scrXxx 自己不睡），
        // idle_ms == 0 的那几屏（编辑器/WebDAV/Flomo/阅读器）在体内自己 vTaskDelay。
        const Screen &scr = kScreens[currentState];
        if (!scr.handle) {
            currentState = APP_MAIN;
        } else if (key > 0) {
            currentState = scr.handle(key, ctx);
        } else {
            currentState = scr.handle(0, ctx);
            if (scr.idle_ms > 0) idleWaitWithTouch(scr.idle_ms);
        }

        // 界面生命周期收口（后半）：收各屏 handle 返回的 next（Esc 退出、进子页…）。
        // 与上面那次同一个函数，幂等 —— 界面没换就是一次比较。
        syncScreenLifecycle(currentState, ctx);

        // 网页端「设为待机画面」的取件点。投递方是 httpd 任务（栈只有 8KB，解不了
        // 几百万像素的大图），真正解图必须落在主任务（16KB）上，见 screen_reader.h。
        // 没待办时就是一次原子读 + 提前返回。
        readerStandbyPump();

        // 离开"键盘宿主"界面就收起虚拟键盘。editorVkVisible() 是全局标志,不收起的话
        // 它会一直留在 true,ui_render 的 ime_top_now() 就会把**别的界面**的下半屏也
        // 当成输入法区——那里的变化全走 FOLLOW DU 极速快刷(8 帧,只驱动变化矩形,残影重)。
        // 设置界面里"下半屏一片极速快刷"就是这么来的。
        // 宿主名单后来扩到了"会打字但以前完全没键盘"的几个界面(设置/灵感/润色/提示词/
        // flomo)：它们各自在"可打字的子状态"里调 editorVkAutoShow()，退出子状态时调
        // editorVkAutoHide()——不在这里按子状态收，是因为那样收完这一帧没人重画，键盘
        // 面板会僵在屏幕上直到下次按键。这里只管**整界面**进出时的兜底。
        // 名单**不在这一行了**，它是 kScreens 的 vk_host 列（main/ui/screen.h）——
        // 加一个会打字的界面时改表，别再在这里补一个 ||。
        const bool vkHost = scr.vk_host;
        if (!vkHost && editorVkVisible()) editorVkSetVisible(false);
        // 打字极速刷新（整屏 DU 差分，每键约 220ms，比 GL16 整屏 410ms 快一倍）只在
        // **实体键盘**输入时开：物理键连发才需要抢这半拍。虚拟键盘是手点的，一键一次、
        // 还要走按下反馈，抢这点时间没意义；而整屏 DU 会把键盘之外的内容一起重画，
        // 反而白白吃掉局刷本该有的干净。VK 收起时（连了蓝牙键盘/手动收起）恢复极速。
        // 每轮都算：GTD 的虚拟键盘是**界面内**开关的，不重算就永远等不到状态切换；
        // ui_set_fast_partial 本身幂等，重复调用没有额外开销。
        bool typingHere = (currentState == APP_EDITOR) ||
                           (currentState == APP_GTD && screen_gtd_typing_mode());
        const bool vkHere = editorVkVisible();
        ui_set_fast_partial(typingHere && !vkHere);
        // 虚拟键盘打字：只做局刷。敲字的差分常从正文一路跨到候选条，超过半屏就会
        // 掉进"整屏 GL16"那条老规则——每键闪一屏，正是要避免的。局刷只驱动变化矩形，
        // 通常就是候选条那一小条（约 89ms），不闪。
        // 设置界面同理一律走局刷：翻列表/切子页的差分高度常超半屏，按"小变化局刷、
        // 大半屏整屏 GL16"判会整屏闪一下；设置里没有需要抢时间的输入，画质优先。
        // 写作模式主界面同理：左右切换功能图标时差分只有一两个图标那么大，按老规则
        // 会掉进 FOLLOW DU（快刷 8 帧 ≈89ms）——大块反白图标一快刷就留残影，下一格
        // 压上去更脏。这里没有抢时间的输入，改用 GL16 局刷（只驱动变化矩形，画质优先）。
        // 计划模式主界面一并算进来：它也是"翻列表 / 换选中行"这一类，没有抢时间的输入。
        // 但只在**没在打字**时算：打字期间那套 fast_partial/合并窗口的记账不能被
        // local_only 搅乱（ui_render 里攒够次数的整屏 GC16 要等两者都关才补做，打字
        // 中途一次全刷都不落）。VK 打字本来就由上面的 vkHost&&vkHere 置了 local_only，
        // 实体键则维持 s_fast_partial 的快刷。
        // 新扩的宿主（灵感/润色/提示词/flomo）只在**键盘真弹着**时算：没弹键盘时它们
        // 本来就没有抢时间的输入，走默认规则。
        const bool gtdBrowsing = (currentState == APP_GTD) && !typingHere;
        ui_set_local_only((vkHost && vkHere) || scr.local_only || gtdBrowsing);

        // 蓝牙键盘低电提示：电量是 HID 异步上报的（bt_keyboard 的事件回调），这里
        // 把待发标记取出来，借现成的居中提示通道报 1.5s。状态栏那个蓝牙图标只表示
        // "连没连上"、不再显示电量数字，所以快没电时得有这一声。
        // 阅读模式排除：那边的推屏不走 ui_commit/快照这套，弹完不会自己重绘回来。
        if (ctx.statusMessage.empty() && currentState != APP_READER &&
            g_bt.takeLowBatteryWarning()) {
            ctx.statusMessage = "蓝牙键盘电量低,请充电";
        }

        if (!ctx.statusMessage.empty()) {
            ui_clear();
            ui_show_message_centered(ctx.statusMessage.c_str());
            ctx.statusMessage.clear();
            vTaskDelay(pdMS_TO_TICKS(1500));
            // 编辑器空闲 tick 会跳过重绘,消息遮罩需在此手动刷掉
            if (currentState == APP_EDITOR) screen_editor_idle(ctx, true);
        }
    }

    ESP_LOGI(TAG, "Goodbye.");
}
