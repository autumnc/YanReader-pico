#include "bt_keyboard.h"
#include "safe_file.h"
#include "settings_manager.h"   // 「字母选字按键」/「长按输入符号」两个实体键盘设置
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>
#include <esp_hidh.h>
#include <esp_hid_common.h>
#include <esp_heap_caps.h>   // bt_conn 创建失败时打内部 RAM 余量（见 requestConnect）

static const char *TAG = "BtKeybrd";

#define HID_REPORT_LEN  8
#define MAX_KEYS        6
#define SCAN_DURATION   5

// 消费类(Consumer Control)报告里的 16 位 usage 编成的虚拟键码：KEY_CONSUMER_BASE|usage。
// 遥控器/翻页器几乎都只发消费类报告(音量、播放、上一曲…)，老代码整包丢弃 → 按键全无反应。
// 超过 0xFF 的部分只有它用得到；上层「按键映射」把它绑到某个阅读动作。
// 必须与 pjournal_app.h 的同名宏一致。
#define KEY_CONSUMER_BASE 0x2000

static QueueHandle_t s_queue = nullptr;

// 入队：键码现在可能 >0xFF（消费类键），所以队列元素是 uint16_t。
static void pushKey(uint16_t k) { if (s_queue) xQueueSendToBack(s_queue, &k, 0); }

// 消费类(Consumer Control)报告的按键消抖状态：按住时报告会原地重发，
// 靠 s_last_consumer 只在变化时报一次；s_consumer_press_us 用于超时兜底。
static uint16_t s_last_consumer = 0;
static int64_t s_consumer_press_us = 0;

// Special key codes returned for non-ASCII keys
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83
#define KEY_IME_TOGGLE 0x84
#define KEY_CTRL_ENTER 0x85
#define KEY_SHIFT_UP    0x86
#define KEY_SHIFT_DOWN  0x87
#define KEY_SHIFT_LEFT  0x88
#define KEY_SHIFT_RIGHT 0x89
#define KEY_CTRL_I      0x8A
#define KEY_FULLWIDTH_TOGGLE 0x8B
#define KEY_TRAD_TOGGLE 0x8C
#define KEY_LSHIFT_TAP 0x8D
#define KEY_HOME       0x8E
#define KEY_END        0x8F
#define KEY_PAGE_UP    0xA0
#define KEY_PAGE_DOWN  0xA1
#define KEY_SEARCH     0xA2
#define KEY_HELP       0xA3
#define KEY_REDO       0xA4
// 中间电容键长按 → 阅读/写作/计划三模式轮换（main.cpp 全局处理）。
// 必须与 pjournal_app.h 的同名宏一致。
#define KEY_LONG_CONFIRM 0xA5
// 晃动机身 = 一次全刷（清残影）。蓝牙键盘上由 Alt+R 产生（见下面的修饰键分支），
// 主机在 main.cpp 的非阅读界面兜底分支里把它翻成 ui_full_refresh_now()。
// 必须与 pjournal_app.h 的同名宏一致。
#define KEY_SHAKE      0xAF
// M1 方向键旁那颗"返回"→ 阅读页回书架 / 其余界面退一层（main.cpp 认这个键）。
// 必须与 pjournal_app.h 的同名宏一致。
#define KEY_BACK       0xA8
// Win/Cmd 键**单击**（按下又抬手，中间没按别的键）→ 输入法「字母键选字」待选。
// 必须与 pjournal_app.h 的同名宏一致。
#define KEY_META_TAP   0xB3
// Ctrl+0-9 → 快捷编辑文件切换 (0x90-0x99)
#define KEY_FILE_BASE 0x90

// HID Usage ID → ASCII
static const uint8_t s_asc_low[] = {
    'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    '1','2','3','4','5','6','7','8','9','0',
    0x0a,0x1b,0x08,0x09,0x20,
    '-','=','[',']','\\',
    '#',';','\'','`',',','.','/',
};
static const uint8_t s_asc_shift[] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    0x0a,0x1b,0x08,0x09,0x20,
    '_','+','{','}','|',
    '~',':','"','~','<','>','?',
};

// M1「锁定」键（Windows 模式发 Win+L）触发的**大写锁存**。只影响字母键：置位时
// 与按着 Shift 同效（hid_to_ascii 里把 Shift 位补上），再按一下解除。不做"Shift 反相"：
// 这里没有宿主 OS 的 CapsLock 状态机，简单锁存更不容易让用户按出意外的小写。
static bool s_caps_lock = false;

static uint8_t hid_to_ascii(uint8_t kc, uint8_t mod) {
    if (s_caps_lock && kc >= 4 && kc <= 29) mod |= 0x02;   // 字母键：锁存即 Shift
    if (kc == 82) return (mod & 0x22) ? KEY_SHIFT_UP : KEY_UP;
    if (kc == 81) return (mod & 0x22) ? KEY_SHIFT_DOWN : KEY_DOWN;
    if (kc == 80) return (mod & 0x22) ? KEY_SHIFT_LEFT : KEY_LEFT;
    if (kc == 79) return (mod & 0x22) ? KEY_SHIFT_RIGHT : KEY_RIGHT;
    if (kc == 74) return KEY_HOME;       // Home
    if (kc == 75) return KEY_PAGE_UP;    // PageUp
    if (kc == 77) return KEY_END;        // End
    if (kc == 78) return KEY_PAGE_DOWN;  // PageDown
    if (kc < 4 || kc > 103) return 0;
    uint8_t i = kc - 4;
    if (i >= sizeof(s_asc_low)) return 0;
    bool shift = (mod & 0x22) != 0;
    if (i <= 25) return shift ? ('A' + i) : ('a' + i);
    return shift ? s_asc_shift[i] : s_asc_low[i];
}

// M1 小键盘功能键的重映射（定义在后面，要读 s_m1_layout）。
static int m1ConsumerRemap(uint16_t usage);

// 消费类报告 → 键码。usage 为 0 表示全部松开。不自动重复（遥控器按键没有重复语义）。
static void handleConsumerReport(const uint8_t *data, size_t len) {
    uint16_t usage = 0;
    if (data && len >= 1) usage = data[0];
    if (data && len >= 2) usage = (uint16_t)(data[0] | (data[1] << 8));  // HID 报告是小端
    if (usage == s_last_consumer) return;  // 按住时报告原地重发，只在变化时报一次
    s_last_consumer = usage;
    if (usage == 0) return;
    s_consumer_press_us = esp_timer_get_time();
    const int m1 = m1ConsumerRemap(usage);
    if (m1) {
        pushKey((uint16_t)m1);
        ESP_LOGI(TAG, "M1 consumer key usage=0x%04X → 0x%02X (功能键重映射)", (unsigned)usage, m1);
        return;
    }
    pushKey((uint16_t)(KEY_CONSUMER_BASE | (usage & 0x1FFF)));
    ESP_LOGI(TAG, "consumer key usage=0x%04X → 0x%04X", (unsigned)usage,
             (unsigned)(KEY_CONSUMER_BASE | (usage & 0x1FFF)));
}

static BtKeyboard *s_self = nullptr;
BtKeyboard g_bt;
static esp_hidh_dev_t *s_dev = nullptr;

static bool s_connected = false;
static bool s_init_done = false;   // set once esp_hidh init completes
static bool s_scanning = false;
static bool s_connecting = false;  // 新增：标记正在连接中
static int64_t s_connect_started_us = 0;  // 本次连接尝试的起始时刻(仅在 s_connecting 为真时有意义)
static bool s_deiniting = false;   // deinit 进行中:阻止重连逻辑再发起新连接尝试
// 修饰键单击检测的武装标记：一个修饰位一个。判据与左Shift那一套相同——按下武装、
// 期间按了别的键就撤、松开时仍武装才算"单击"（修饰键+字母是系统组合键，不是我们要的）。
// 哪一位的单击会变成 KEY_META_TAP 由设置「字母选字按键」现读（见 letterSelModBit）。
static const uint8_t s_mod_bits[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
static bool s_mod_tap_armed[8] = {false};

// 「长按输入符号」（设置 long_press_symbol，见 SettingsManager::kbLongPressSymbol）：
// 按下先不发，抬手时才发原字符；一直按住超过这个时长，就改发键帽上那个符号
// （相当于按住 sym 再按该键）。按下即发、长按再"退格改掉"的做法在墨水屏上要刷两次，
// 而且输入法把键吃进编码/选词时退格会误删别的内容，所以这里选择推迟到抬手。
// 哪些键有"符号"见 hasShiftSymbol / m1LetterSymbol；空格/回车/退格/方向键照旧。
static const int64_t LONG_PRESS_SYMBOL_US = 500 * 1000;   // 与 KEY_REPEAT_DELAY_MS 对齐
static uint8_t s_key_pending[MAX_KEYS] = {0};         // 待发的原字符（0=没有）
static uint8_t s_key_pending_shift[MAX_KEYS] = {0};   // 长按时要发的符号字符
static bool s_key_long_fired[MAX_KEYS] = {false};     // 符号已发：按住不再自动重复

// 键帽上有"上档符号"的 HID usage：标准键盘的数字行(30..39)与标点行(45..56)。
// 字母(4..29)的 Shift 层只是大写，不算"符号"；空格/回车/退格/Tab/方向键不在此列。
static bool hasShiftSymbol(uint8_t kc) {
    return (kc >= 30 && kc <= 39) || (kc >= 45 && kc <= 56);
}

// Mospace M1 这类手机式迷你键盘（BLE 名 "M1 Keyboard"）：没有单独的数字/标点排，
// 数字和符号全印在字母键帽上，靠左下角 "123"（sym）键切层。键盘内部这层切换主机
// 看不到，没法从 HID 报告反推，所以只能按它的键帽把 a..z → 符号写死。
// 连的是普通键盘时 s_m1_layout=false，字母长按仍走原来的自动重复。
static bool s_m1_layout = false;

// M1 小键盘的功能键重映射（2026-10-29 用户要求）：
//   音量+ / 音量-  → PageUp / PageDown
//   搜索键         → 模式切换（等同长按中间电容键）
//   地球键         → 开关输入法（等同 Ctrl+Space）
//   返回键（方向键旁）→ KEY_BACK（阅读页无浮层=回书架，其余=退一层）
// 只对 M1 生效：消费类 usage 本来由「按键映射」按需绑定，遥控器/翻页器的音量键
// 不该被这里改成翻页（而且写死会盖掉用户自己的绑定）。M1 的这几个键有的走消费类
// 报告、有的走键盘报告（0x7F~0x81 是 HID 键盘页自己的音量键，0x90/0x91 是 LANG1/2），
// 两条路都会进到这里（键盘页那条在下面的循环里直接调 remap）。
// 消费类 usage：0xE9 音量增、0xEA 音量减、0x221 AC Search、0x290 Keyboard Layout Select。
static int m1ConsumerRemap(uint16_t usage) {
    if (!s_m1_layout) return 0;
    switch (usage) {
        case 0x00E9: return KEY_PAGE_UP;        // 音量+
        case 0x00EA: return KEY_PAGE_DOWN;      // 音量-
        case 0x0221: return KEY_LONG_CONFIRM;   // 搜索 → 模式切换
        case 0x0290: return KEY_IME_TOGGLE;     // 地球键 → 输入法开关
        // 方向键旁那颗"返回"发的是消费类 AC Back(0x224)，此前只落成 KEY_CONSUMER_BASE|0x224
        // （没人接，按了毫无反应）。KEY_BACK 的语义就是阅读器那位：阅读页无浮层→回书架，
        // 有菜单/注文等浮层→先退一层；其余模式由 main.cpp 翻成 Esc 退一层。
        case 0x0224: return KEY_BACK;           // 返回 → 回书架 / 退一层
        default: return 0;
    }
}

// 字母键帽上的符号（下标 = HID usage - 4，a..z）：
//   Q1 W2 E3 R4 T5 Y6 U7 I8 O9 P0 / A- S/ D: F; G( H) J$ K' L" / Z% X& C* V? B! N, M.
// N/M 两个键帽上的点太小，照片上认成 ~ 和 ` 了；实机实测是逗号和句号（2026-10-22）。
static uint8_t m1LetterSymbol(uint8_t kc) {
    static const char kSym[] = "-!*:3;()8$'\".,9014/57?2&6%";
    if (kc < 4 || kc > 29) return 0;
    return (uint8_t)kSym[kc - 4];
}

// 丢弃所有待发键（断连/清队列时用，**不要**补发：这一下按键已经失效）。
static void clearPendingKeys() {
    memset(s_key_pending, 0, sizeof(s_key_pending));
    memset(s_key_pending_shift, 0, sizeof(s_key_pending_shift));
    memset(s_key_long_fired, 0, sizeof(s_key_long_fired));
}

// 又有键按下（或组合键开始）时调用：待发的键按"短按"打出去，保持原按下顺序。
static void flushPendingKeys() {
    for (int i = 0; i < MAX_KEYS; i++) {
        if (s_key_pending[i]) {
            pushKey(s_key_pending[i]);
            s_key_pending[i] = 0;
            s_key_pending_shift[i] = 0;
        }
    }
}

// 「字母选字按键」设置 → 哪一个修饰位的单击该发 KEY_META_TAP。返回 0 表示设置里选的
// 不是修饰键（Esc/Tab/关闭）：那几种是普通按键，由输入法在按键处理里自己认
// （见 IME::letterSelTriggerKey）；Esc/Tab 不能在这里拦，否则菜单/对话框里的原义就没了。
// "win" = 左右任一，是改动前的行为（也是旧布尔值 "1" 映射过来的值）。
static uint8_t letterSelModBit() {
    const std::string v = g_settings.imeMetaLetterSelKey();
    if (v == "lctrl") return 0x01;
    if (v == "lshift") return 0x02;
    if (v == "lalt") return 0x04;
    if (v == "lwin") return 0x08;
    if (v == "rctrl") return 0x10;
    if (v == "rshift") return 0x20;
    if (v == "ralt") return 0x40;
    if (v == "rwin") return 0x80;
    if (v == "win") return 0x08 | 0x80;
    return 0;
}
static int s_kb_battery = -1;           // 键盘电池电量 %，-1=未知/未连接

// 低电提示：键盘电量掉到 KB_LOW_BATT_PCT 以下时置一次待发标记，UI 主循环取走即清。
// 只发一次（每次连接），靠 s_kb_low_batt_armed 记账；电量回升过阈值+5%（充电）
// 就重新武装，拔下来再掉回去还会再提醒一次，中间来回抖动不会反复弹。
static constexpr int KB_LOW_BATT_PCT = 15;
static bool s_kb_low_batt_pending = false;
static bool s_kb_low_batt_armed = true;

// 断连/换设备时键盘电量整体失效：一并把低电提示复位，下次连接重新计一次。
// 待发标记也要清——键盘已经走了，这条提示不再有意义。
static void resetKbBattery() {
    s_kb_battery = -1;
    s_kb_low_batt_pending = false;
    s_kb_low_batt_armed = true;
}
static uint8_t s_last_keys[MAX_KEYS] = {0};
static uint8_t s_last_mod = 0;
static int64_t s_key_press_time[MAX_KEYS] = {0};
static int64_t s_last_repeat_time[MAX_KEYS] = {0};
static esp_ble_addr_type_t s_paired_addr_type = BLE_ADDR_TYPE_RANDOM;

// 连接在后台任务里执行: esp_hidh_dev_open 是同步阻塞的(连接失败要等链路层
// 超时 ~30s),不能放在主循环里,否则 UI 会卡死。s_connect_task 非空表示任务存活。
static TaskHandle_t s_connect_task = nullptr;

// 单次连接尝试的最长寿命。正常路径不用它兜底: 失败时 esp_hidh_dev_open 会在 ~30s 后
// 返回 NULL 并自行复位 s_connecting。取 60s 是给"卡死"留的出口 —— 任务创建失败、
// Bluedroid 内部命令阻塞、OPEN 事件丢失都会让 s_connecting 永远为真,而主循环的
// 自动重连门是 !isConnecting()（main.cpp），于是键盘再也不会自动重连，日志上只看得到
// 每 30s 一次的配对列表重载，完全看不出原因。超时后主动放弃这次尝试，让重试继续。
static const int64_t CONNECT_ATTEMPT_TIMEOUT_US = 60 * 1000000LL;
static struct {
    uint8_t bda[ESP_BD_ADDR_LEN];
    esp_ble_addr_type_t addr_type;
} s_connect_req;

// Device list collected during scan
static BtDeviceInfo s_found_devices[MAX_BT_DEVICES];
static int s_found_count = 0;
static SemaphoreHandle_t s_devices_mutex = nullptr;

// Paired device list (persisted to /sdcard/settings/bt_paired, most recent first)
static BtPairedDevice s_paired[MAX_BT_DEVICES];
static int s_paired_count = 0;

// BLE scan params (extended)
static esp_ble_ext_scan_params_t s_ext_scan_params = {};

// Find device index by BDA, or -1 if not found
static int find_device(esp_bd_addr_t bda) {
    for (int i = 0; i < s_found_count; i++) {
        if (memcmp(s_found_devices[i].bda, bda, ESP_BD_ADDR_LEN) == 0)
            return i;
    }
    return -1;
}

// Manually parse AD data for device name (fallback)
static uint8_t* find_name_in_ad(uint8_t *data, uint8_t len, uint8_t *out_len) {
    *out_len = 0;
    uint8_t pos = 0;
    while (pos < len) {
        uint8_t field_len = data[pos];
        if (field_len == 0) break;
        if (pos + field_len >= len) break;
        uint8_t type = data[pos + 1];
        if (type == ESP_BLE_AD_TYPE_NAME_CMPL || type == ESP_BLE_AD_TYPE_NAME_SHORT) {
            *out_len = field_len - 1;
            return &data[pos + 2];
        }
        pos += field_len + 1;
    }
    return nullptr;
}

// Add or update a device entry (caller must hold mutex)
static int add_or_update_device(esp_bd_addr_t bda, esp_ble_addr_type_t addr_type,
                                 uint8_t *name, uint8_t name_len, int rssi) {
    int idx = find_device(bda);
    if (idx >= 0) {
        // Update existing — only update name if we didn't have one before
        if (!s_found_devices[idx].name[0] && name && name_len > 0) {
            uint8_t copy_len = (name_len > 31) ? 31 : name_len;
            memcpy(s_found_devices[idx].name, name, copy_len);
            s_found_devices[idx].name[copy_len] = '\0';
            ESP_LOGI(TAG, "  -> [%d] updated name: %s", idx, s_found_devices[idx].name);
        }
        s_found_devices[idx].rssi = rssi;
        return idx;
    }
    if (s_found_count >= MAX_BT_DEVICES) return -1;
    idx = s_found_count;
    memcpy(s_found_devices[idx].bda, bda, ESP_BD_ADDR_LEN);
    s_found_devices[idx].addr_type = addr_type;
    s_found_devices[idx].rssi = rssi;
    if (name && name_len > 0) {
        uint8_t copy_len = (name_len > 31) ? 31 : name_len;
        memcpy(s_found_devices[idx].name, name, copy_len);
        s_found_devices[idx].name[copy_len] = '\0';
    } else {
        snprintf(s_found_devices[idx].name, sizeof(s_found_devices[idx].name),
                 "BLE-%02x%02x%02x", bda[3], bda[4], bda[5]);
    }
    s_found_count++;
    ESP_LOGI(TAG, "  -> [%d] %s (rssi=%d)", idx, s_found_devices[idx].name, rssi);
    return idx;
}

// ── 异步保存配对设备 ─────────────────────────────────────────────
// OPEN_EVENT 在 esp_hidh 事件任务里触发；同步 safeWriteFile(含 fsync) 会阻塞该任务，
// 拖慢 BLE 事件处理。这里把落盘挪到低优先级一次性任务，避免影响连接稳定性。
static esp_bd_addr_t s_save_bda;
static esp_ble_addr_type_t s_save_at = (esp_ble_addr_type_t)0;
static char s_save_name[32];
static TaskHandle_t s_save_task = NULL;

static void save_paired_task(void *arg) {
    if (s_self) {
        s_self->savePairedDevice(s_save_bda, s_save_at, s_save_name);
    }
    s_save_task = NULL;
    vTaskDelete(NULL);
}

static void save_paired_async(const uint8_t *bda, esp_ble_addr_type_t addr_type,
                              const char *name) {
    memcpy(s_save_bda, bda, ESP_BD_ADDR_LEN);
    s_save_at = addr_type;
    snprintf(s_save_name, sizeof(s_save_name), "%.31s", name ? name : "?");
    if (s_save_task == NULL) {
        xTaskCreate(save_paired_task, "bt_save", 4096, NULL, 2, &s_save_task);
    }
}

static void hidh_cb(void *handler_args, esp_event_base_t base, int32_t id, void *event_data) {
    auto event = (esp_hidh_event_t)id;
    auto *param = (esp_hidh_event_data_t *)event_data;
    switch (event) {
    case ESP_HIDH_OPEN_EVENT:
        s_connecting = false;  // 连接完成
        if (param->open.status == ESP_OK) {
            s_dev = param->open.dev;
            s_connected = true;
            // 认出 M1 小键盘：它的字母键帽上印着符号（Q1 A- Z% …），长按要按这张表走。
            const char *nm = esp_hidh_dev_name_get(param->open.dev);
            s_m1_layout = nm && strstr(nm, "M1 Keyboard") != nullptr;
            if (s_self) {
                s_self->setConnected(true);
            }
            // 带上「长按输入符号」的**生效值**：这个功能只改 bt_keyboard 的键路径，
            // 用户报"没生效"时先看这一行就知道是开关没开（或存的是 "0"）还是代码没走到。
            ESP_LOGI(TAG, "Keyboard connected: %s (long_press_symbol=%d, m1_layout=%d)",
                     nm ?: "?", (int)g_settings.kbLongPressSymbol(), (int)s_m1_layout);
        } else {
            ESP_LOGE(TAG, "Keyboard HID open failed: %d", param->open.status);
        }
        // Save device info whenever we have a valid device handle,
        // so BLE-paired devices are persisted for auto-reconnect.
        // 异步落盘，避免阻塞 HID 事件任务。
        if (param->open.dev) {
            const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
            if (bda) {
                const char *dev_name = esp_hidh_dev_name_get(param->open.dev);
                save_paired_async(bda, s_paired_addr_type, dev_name ? dev_name : "?");
            }
        }
        break;
    case ESP_HIDH_CLOSE_EVENT:
        s_dev = nullptr;
        s_connected = false;
        s_connecting = false;  // 连接断开
        s_m1_layout = false;
        resetKbBattery();      // 键盘电量失效（低电提示一并复位）
        memset(s_last_keys, 0, MAX_KEYS);
        memset(s_key_press_time, 0, MAX_KEYS * sizeof(int64_t));
        memset(s_last_repeat_time, 0, MAX_KEYS * sizeof(int64_t));
        memset(s_mod_tap_armed, 0, sizeof(s_mod_tap_armed));
        clearPendingKeys();
        if (s_self) s_self->setConnected(false);
        ESP_LOGI(TAG, "Keyboard disconnected (rsn=0x%x)", param->close.reason);
        break;
    case ESP_HIDH_INPUT_EVENT: {
        if (!s_queue) break;
        // 只解析键盘报告。带触控板的键盘会额外发鼠标/消费类报告(相对位移、滚轮、
        // 媒体键),若按键盘格式解析,会把位移字节当成按键,造成"不按键也乱码输入"。
        // 消费类报告单独走 handleConsumerReport：BLE 遥控器/翻页器只发这个，
        // 丢掉的话遥控器按什么都没反应。
        if (param->input.usage & ESP_HID_USAGE_CCONTROL) {
            handleConsumerReport(param->input.data, param->input.length);
            break;
        }
        if (param->input.usage != ESP_HID_USAGE_KEYBOARD) {
            static bool s_non_kbd_logged = false;
            if (!s_non_kbd_logged) {
                s_non_kbd_logged = true;
                ESP_LOGI(TAG, "ignoring non-keyboard input reports (usage=%d)", (int)param->input.usage);
            }
            break;
        }
        uint8_t *data = param->input.data;
        size_t len = param->input.length;
        if (!data || len < 2) break;
        uint8_t mod = data[0];
        // 修饰键单击检测：按下时武装，期间按下任何键则解除，松开时若仍武装则触发。
        // 逐位判（Ctrl/Shift/Alt/GUI 的左右各一位，互不重叠）；具体哪一位变成
        // 「字母选字」的触发键由设置决定，见 letterSelModBit / 下面的松开分支。
        for (int i = 0; i < 8; i++) {
            if ((mod & s_mod_bits[i]) && !(s_last_mod & s_mod_bits[i]))
                s_mod_tap_armed[i] = true;
        }
        const uint8_t *keys = (len >= HID_REPORT_LEN) ? (data + 2) : (data + 1);
        int nkeys = (len >= HID_REPORT_LEN) ? 6 : ((int)len - 1);
        if (nkeys > MAX_KEYS) nkeys = MAX_KEYS;

        // Track which keys are currently pressed for repeat logic
        bool current_pressed[MAX_KEYS] = {false};

        for (int i = 0; i < nkeys; i++) {
            uint8_t kc = keys[i];
            if (kc == 0) continue;

            // Check if this key was already pressed
            bool old = false;
            int slot = -1;
            for (int j = 0; j < MAX_KEYS; j++) {
                if (s_last_keys[j] == kc) {
                    old = true;
                    slot = j;
                    break;
                }
            }

            // Mark as currently pressed
            if (slot >= 0) current_pressed[slot] = true;

            // If new key press, record time and send event
            if (!old) {
                // 又有键按下 → 之前挂起的键按短按打出去（保持按下顺序），
                // 同时说明它不会再走"长按输入符号"。
                flushPendingKeys();
                // 与修饰键组合使用的按键按下 → 取消所有单击检测（Ctrl+C 之类不是单击）
                memset(s_mod_tap_armed, 0, sizeof(s_mod_tap_armed));
                // Find empty slot for this new key
                int newSlot = -1;
                for (int j = 0; j < MAX_KEYS; j++) {
                    if (s_last_keys[j] == 0) {
                        s_last_keys[j] = kc;
                        s_key_press_time[j] = esp_timer_get_time();
                        current_pressed[j] = true;
                        newSlot = j;
                        break;
                    }
                }

                // Ctrl modifier handling
                bool ctrl = (mod & 0x11) != 0;
                bool shift = (mod & 0x22) != 0;
                bool alt = (mod & 0x44) != 0;   // 左/右 Alt（HID 修饰位 2、3，见 hid_to_ascii）
                if (alt && kc == 21) {
                    // Alt+R → 手动来一次大刷新（把残影一次刷掉）。复用 KEY_SHAKE 那个键码：
                    // main.cpp 对非阅读界面的兜底分支拿到它就调 ui_full_refresh_now()，
                    // 阅读器自己那一档另有处理。HID usage 21 = 'r'。
                    // **必须排在下面通用字母映射之前**，否则同一个 r 还会被当正文插进去。
                    // 虚拟键盘没有 Alt，这条只对蓝牙键盘生效。
                    uint8_t fr = KEY_SHAKE;
                    pushKey(fr);
                    continue;
                }
                if (ctrl && shift && kc == 9) {
                    // Ctrl+Shift+F → simplified/traditional toggle (before generic Ctrl+letter)
                    uint8_t tt = KEY_TRAD_TOGGLE;
                    pushKey(tt);
                    continue;
                }
                if (ctrl && kc == 12) {
                    // Ctrl+I → inspiration panel (must check before generic Ctrl+letter)
                    uint8_t ci = KEY_CTRL_I;
                    pushKey(ci);
                    continue;
                }
                if (ctrl && kc == 44) {
                    // Ctrl+Space → IME toggle
                    uint8_t toggle = KEY_IME_TOGGLE;
                    pushKey(toggle);
                    continue;
                }
                if (shift && kc == 44) {
                    // Shift+Space → fullwidth toggle
                    uint8_t fwt = KEY_FULLWIDTH_TOGGLE;
                    pushKey(fwt);
                    continue;
                }
                if (ctrl && kc == 40) {
                    // Ctrl+Enter → special key
                    uint8_t ce = KEY_CTRL_ENTER;
                    pushKey(ce);
                    continue;
                }
                if (ctrl && shift && kc == 56) {
                    // Ctrl+? (Shift+/) → 快捷键帮助对话框
                    uint8_t h = KEY_HELP;
                    pushKey(h);
                    continue;
                }
                if (ctrl && shift && kc == 29) {
                    // Ctrl+Shift+Z → redo
                    uint8_t r = KEY_REDO;
                    pushKey(r);
                    continue;
                }
                if (ctrl && kc == 56) {
                    // Ctrl+/ → 搜索/替换对话框 (HID usage 56 = '/')
                    uint8_t s = KEY_SEARCH;
                    pushKey(s);
                    continue;
                }
                if (ctrl && kc >= 4 && kc <= 29) {
                    // Ctrl+letter → control character (0x01-0x1A)
                    uint8_t cc = kc - 3;
                    pushKey(cc);
                    continue;
                }
                if (ctrl && kc >= 30 && kc <= 39) {
                    // Ctrl+0-9 → 快捷编辑文件切换. HID usage: '1'=30 ... '9'=38, '0'=39
                    int fileIdx = (kc == 39) ? 0 : (kc - 29);
                    uint8_t fk = KEY_FILE_BASE + fileIdx;
                    pushKey(fk);
                    continue;
                }

                // M1 小键盘的特殊键在 Windows 模式下发的是 Win+字母 快捷键，照字母放
                // 出去会往正文里塞 d/l/h。按键帽重映射（只对 M1 生效）：
                //   Esc 键 → Win+D：发 0x1B（返回）
                //   语音键 → Win+H：发 Tab
                //   锁定键 → Win+L：大写锁存开/关（见 s_caps_lock）
                // press_time 清零 = 按住不自动重复（这些键在 M1 上会一直重发报告）。
                if (s_m1_layout && (mod & 0x88)) {
                    if (kc == 7) {  // Win+D → Esc
                        uint8_t esc = 0x1B;
                        pushKey(esc);
                        s_key_press_time[newSlot] = 0;
                        continue;
                    }
                    if (kc == 11) {  // Win+H（语音键）→ Tab
                        pushKey('\t');
                        s_key_press_time[newSlot] = 0;
                        continue;
                    }
                    if (kc == 15) {  // Win+L（锁定键）→ CapsLock 锁存开/关
                        s_caps_lock = !s_caps_lock;
                        ESP_LOGI(TAG, "M1 CapsLock %s", s_caps_lock ? "开" : "关");
                        s_key_press_time[newSlot] = 0;
                        continue;
                    }
                }

                // M1 的 ⇧（大小写切换）键发的是 Caps Lock(0x39)。本机原来直接忽略，
                // 按下去毫无反应。它在这台键盘上就是 Shift，按本机左 Shift 单击的老
                // 语义处理：编辑器里切中英。Caps Lock 是锁存键、按住会重发报告，
                // 所以只有第一次按下才发，press_time 清零保证不重复。
                if (s_m1_layout && kc == 57) {
                    uint8_t ev = KEY_LSHIFT_TAP;
                    pushKey(ev);
                    s_key_press_time[newSlot] = 0;
                    continue;
                }

                // M1 的功能键：键盘页上的音量键是 0x7F~0x81（0x80 增 / 0x81 减），
                // 地球键是 LANG1/LANG2(0x90/0x91)。这些 usage 不会变成字符，以前被
                // hid_to_ascii 静默丢掉，按了毫无反应（见 m1ConsumerRemap 的总说明；
                // 有的 M1 固件把音量和搜索走消费类报告，那条路已按功能键重映射）。
                if (s_m1_layout && (kc == 0x80 || kc == 0x81 || kc == 0x90 || kc == 0x91)) {
                    uint8_t ev = KEY_IME_TOGGLE;   // 地球键
                    if (kc == 0x80) ev = KEY_PAGE_UP;
                    else if (kc == 0x81) ev = KEY_PAGE_DOWN;
                    pushKey(ev);
                    s_key_press_time[newSlot] = 0;   // 按住不重复（同上面几个 M1 键）
                    continue;
                }

                // 「长按输入符号」：先挂起，抬手时才发原字符；按住到阈值改发键帽上的符号。
                // 标准键盘 = 数字/标点键的 Shift 层；M1 小键盘 = 字母键帽上印的那个符号。
                // 待 Ctrl/Alt/GUI/Shift 时不算（组合键；Shift 本身已经在上档）。
                uint8_t symChar = 0;
                if (hasShiftSymbol(kc)) {
                    symChar = hid_to_ascii(kc, (uint8_t)(mod | 0x22));
                } else if (s_m1_layout) {
                    symChar = m1LetterSymbol(kc);
                }
                const bool delaySym = newSlot >= 0 && symChar != 0 &&
                                      !(mod & (0x11 | 0x22 | 0x44 | 0x88)) &&
                                      g_settings.kbLongPressSymbol();
                if (delaySym) {
                    s_key_pending[newSlot] = hid_to_ascii(kc, mod);
                    s_key_pending_shift[newSlot] = symChar;
                    s_key_long_fired[newSlot] = false;
                } else {
                    uint8_t ascii = hid_to_ascii(kc, mod);
                    if (ascii) pushKey(ascii);
                    // M1 上还没认出来的功能键：打一行日志，方便下次照 usage 补映射。
                    else if (s_m1_layout) ESP_LOGI(TAG, "M1 未映射键盘 usage=0x%02X (mod=0x%02X)", kc, mod);
                }
            }
        }

        // Clear keys that were released
        for (int i = 0; i < MAX_KEYS; i++) {
            if (s_last_keys[i] != 0 && !current_pressed[i]) {
                // 短按：原字符这时候才发（长按那一路已经在 checkKeyRepeat 里发过符号）。
                // 抬手时再按一下按住时长兜底：主循环被推屏占住、错过了 checkKeyRepeat
                // 那一拍时，长按（≥阈值）也不会被当成短按打回原字符。
                if (s_key_pending[i]) {
                    const int64_t held = esp_timer_get_time() - s_key_press_time[i];
                    pushKey(held >= LONG_PRESS_SYMBOL_US ? s_key_pending_shift[i]
                                                        : s_key_pending[i]);
                }
                s_key_pending[i] = 0;
                s_key_pending_shift[i] = 0;
                s_key_long_fired[i] = false;
                s_last_keys[i] = 0;
                s_key_press_time[i] = 0;
                s_last_repeat_time[i] = 0;
            }
        }

        // 修饰键抬手：单按（期间没按别的键）才发键。被设置选为「字母选字」触发键的那一位
        // 发 KEY_META_TAP（用途见 pjournal_app.h）；没选中的左Shift保留老语义（中英切换），
        // 其余修饰键单击本来就没有动作。
        for (int i = 0; i < 8; i++) {
            if (!(mod & s_mod_bits[i]) && (s_last_mod & s_mod_bits[i])) {
                if (s_mod_tap_armed[i]) {
                    const uint8_t selBit = letterSelModBit();  // 只在真单击时读设置
                    if (selBit & s_mod_bits[i]) {
                        uint8_t ev = KEY_META_TAP;
                        pushKey(ev);
                    } else if (s_mod_bits[i] == 0x02) {
                        uint8_t ev = KEY_LSHIFT_TAP;
                        pushKey(ev);
                    }
                }
                s_mod_tap_armed[i] = false;
            }
        }

        s_last_mod = mod;
        break;
    }
    case ESP_HIDH_BATTERY_EVENT: {
        const int lvl = param->battery.level;
        s_kb_battery = lvl;
        ESP_LOGI(TAG, "Keyboard battery: %d%%", lvl);
        // 低电提示（一次性，见 s_kb_low_batt_pending 的说明）。电量未知(-1)时不表态。
        if (lvl >= 0) {
            if (lvl < KB_LOW_BATT_PCT) {
                if (s_kb_low_batt_armed) {
                    s_kb_low_batt_armed = false;
                    s_kb_low_batt_pending = true;
                }
            } else if (lvl >= KB_LOW_BATT_PCT + 5) {
                s_kb_low_batt_armed = true;   // 充上电了：下次再掉下来重新提醒
            }
        }
        break;
    }
    default:
        break;
    }
}

// Check if advertising data contains the HID service UUID (0x1812)
static bool has_hid_service(uint8_t *data, uint8_t len) {
    uint8_t pos = 0;
    while (pos + 1 < len) {
        uint8_t field_len = data[pos];
        if (field_len == 0) break;
        if (pos + 1 + field_len > len) break;
        uint8_t type = data[pos + 1];
        if (type == ESP_BLE_AD_TYPE_16SRV_CMPL || type == ESP_BLE_AD_TYPE_16SRV_PART) {
            for (uint8_t i = 0; i + 1 < field_len - 1; i += 2) {
                if (pos + 2 + i + 1 < len &&
                    data[pos + 2 + i] == 0x12 && data[pos + 2 + i + 1] == 0x18)
                    return true;
            }
        }
        pos += field_len + 1;
    }
    return false;
}

// SMP 安全参数：SC + Just Works（无 MITM），配对不需要用户操作。
// 实测 TypixNode KBD 就是这样配对成功的（日志里走 SC，生成 LTK，auth success，
// 全程没有配对码）；M1 / Eyelash40 也一直是这套。不要改成 MITM：本机没有键盘，
// 只能当 DisplayYesNo，而对方若是 KeyboardOnly 就要走 Passkey Entry —— 配对码只
// 出现在串口日志里，用户根本看不到，而且配错还会让协议栈删掉本机 bond。
static void applySecurityParams() {
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t key_size = 16;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, 1);
}

static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            // Skip non-HID devices
            if (!has_hid_service(param->scan_rst.ble_adv,
                param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len))
                break;
            uint8_t name_len = 0;
            uint8_t *name = esp_ble_resolve_adv_data_by_type(
                param->scan_rst.ble_adv,
                param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len,
                ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
            if (!name || name_len == 0) {
                name = esp_ble_resolve_adv_data_by_type(
                    param->scan_rst.ble_adv,
                    param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len,
                    ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
            }
            if (!name || name_len == 0) {
                name = find_name_in_ad(param->scan_rst.ble_adv,
                    param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len, &name_len);
            }
            if (s_devices_mutex) xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
            add_or_update_device(param->scan_rst.bda, param->scan_rst.ble_addr_type,
                                 name, name_len, param->scan_rst.rssi);
            if (s_devices_mutex) xSemaphoreGive(s_devices_mutex);
        }
        break;
    case ESP_GAP_BLE_EXT_ADV_REPORT_EVT: {
        auto &rpt = param->ext_adv_report.params;
        // Skip non-HID devices
        if (!has_hid_service(rpt.adv_data, rpt.adv_data_len))
            break;
        uint8_t name_len = 0;
        uint8_t *name = esp_ble_resolve_adv_data_by_type(
            rpt.adv_data, rpt.adv_data_len,
            ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
        if (!name || name_len == 0) {
            name = esp_ble_resolve_adv_data_by_type(
                rpt.adv_data, rpt.adv_data_len,
                ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
        }
        // Manual fallback if API fails with extended data
        if (!name || name_len == 0) {
            name = find_name_in_ad(rpt.adv_data, rpt.adv_data_len, &name_len);
        }
        if (s_devices_mutex) xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
        add_or_update_device(rpt.addr, (esp_ble_addr_type_t)rpt.addr_type,
                             name, name_len, rpt.rssi);
        if (s_devices_mutex) xSemaphoreGive(s_devices_mutex);
        break;
    }
    case ESP_GAP_BLE_SCAN_TIMEOUT_EVT:
        ESP_LOGI(TAG, "Scan timeout");
        break;
    case ESP_GAP_BLE_NC_REQ_EVT:
        ESP_LOGI(TAG, "BLE NC_REQ passkey: %06" PRIu32, param->ble_security.key_notif.passkey);
        esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        ESP_LOGI(TAG, "BLE pairing code: %06" PRIu32, param->ble_security.key_notif.passkey);
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        ESP_LOGI(TAG, "BLE SEC_REQ - responding");
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        ESP_LOGI(TAG, "BLE PASSKEY_REQ");
        break;
    case ESP_GAP_BLE_KEY_EVT:
        ESP_LOGI(TAG, "BLE KEY type = %d", param->ble_security.ble_key.key_type);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            ESP_LOGI(TAG, "BLE auth success");
        } else {
            ESP_LOGE(TAG, "BLE auth fail: 0x%x", param->ble_security.auth_cmpl.fail_reason);
            // 0x50 = ESP_AUTH_SMP_PAIR_AUTH_FAIL。典型成因是两边 bond 不一致：本机
            // 这份被删了（Bluedroid 在每次 auth 失败时都会自动删本机的），键盘那头
            // 还留着旧记录，于是它拒绝这次重新配对。这时只能让键盘忘掉旧主机、重新
            // 进入配对模式；删本机 bond 无济于事（本来就已经没有了），所以这里不动
            // 任何密钥，只把线索打到日志里。
            if (param->ble_security.auth_cmpl.fail_reason == ESP_AUTH_SMP_PAIR_AUTH_FAIL) {
                ESP_LOGW(TAG, "配对被拒(0x50)：若反复失败，请让键盘重新进入配对模式");
            }
        }
        break;
    default:
        break;
    }
}

static void scan_task(void *arg) {
    if (s_scanning) { vTaskDelete(NULL); return; }
    s_scanning = true;

    // Clear previous results
    if (s_devices_mutex) xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
    s_found_count = 0;
    if (s_devices_mutex) xSemaphoreGive(s_devices_mutex);

    // 防御性停扫：正常路径上一轮窗口结束时已经停过；这里再停一次，避免遗留的
    // 扫描命令让下面的 start 被控制器拒绝。未在扫描时这条命令只会返回错误，
    // 无副作用。3 秒预热正好给异步命令留出完成时间。
    if (!s_deiniting) esp_ble_gap_stop_ext_scan();

    vTaskDelay(pdMS_TO_TICKS(3000));
    // 预热期间可能被 deinit(待机)或连接请求取消。栈已经拆掉时绝不能再碰 GAP API。
    if (s_deiniting || !s_scanning) {
        s_scanning = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Scanning for BLE devices...");

    esp_err_t ret;
    ret = esp_ble_gap_set_ext_scan_params(&s_ext_scan_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_ext_scan_params failed: %d", ret);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    // 设参数和启动之间还有一次异步往返；这 100ms 里可能刚好被 deinit(待机)拆栈，
    // 或用户已经点选设备开始连接。此时绝不能再把扫描打开 —— 打开之后收尾逻辑会
    // 因为 s_scanning 已被清掉而跳过停扫，射频就遗留下来了。
    if (s_deiniting || !s_scanning) {
        s_scanning = false;
        vTaskDelete(NULL);
        return;
    }
    bool scan_started = false;
    ret = esp_ble_gap_start_ext_scan(SCAN_DURATION * 100, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start_ext_scan failed: %d", ret);
    } else {
        scan_started = true;
        ESP_LOGI(TAG, "Extended scan started for %d seconds", SCAN_DURATION);
    }

    vTaskDelay(pdMS_TO_TICKS((SCAN_DURATION + 3) * 1000));
    // duration 到点后控制器会自己停扫（日志里有 "Scan timeout"），period=0 只是
    // 不周期重扫。这里再显式停一次，兜住"启动/取消命令交错"的情况，也保证收工后
    // 射频确实空闲、不干扰紧接着的连接。deinit 之后栈已经拆了，绝不能再碰 GAP API。
    if (scan_started && !s_deiniting) esp_ble_gap_stop_ext_scan();
    ESP_LOGI(TAG, "Scan complete, found %d devices", s_found_count);
    s_scanning = false;
    // 不要在这里 setConnected(false)：扫描结束时连接可能刚建立，误清会把已连接的
    // 设备标记为断开，触发自动重连→关闭真实连接→循环。连接状态只由 OPEN/CLOSE 事件维护。
    vTaskDelete(NULL);
}

// 后台执行实际连接。esp_hidh_dev_open 同步阻塞: 成功时返回 dev,随后由异步的
// ESP_HIDH_OPEN_EVENT 复位 s_connecting;失败时返回 NULL 且不发任何事件,
// 必须在这里复位 s_connecting,否则重连逻辑会永远被 isConnecting() 卡住。
static void connect_task(void *arg) {
    if (s_scanning) {
        s_scanning = false;
        esp_ble_gap_stop_ext_scan();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    char hex[13];
    snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x",
             s_connect_req.bda[0], s_connect_req.bda[1], s_connect_req.bda[2],
             s_connect_req.bda[3], s_connect_req.bda[4], s_connect_req.bda[5]);
    ESP_LOGI(TAG, "Connecting to %s...", hex);

    // 不要在这里删 bond。esp_ble_remove_bond_device() 是**异步**的：删除命令可能正好
    // 落在连接后的加密握手中间，协议栈会认为密钥/设备不存在，SMP 以 PAIR_AUTH_FAIL
    // (0x50) 失败并把链路断掉 —— 用户看到的就是"连上就断"，而且 Bluedroid 还会顺手
    // 把本机这份 bond 删掉，于是下次只能重新配对，越删越坏（TypixNode KBD 实测）。
    // 正常重连直接复用手上的 LTK；真的密钥不匹配时协议栈会在 auth 失败时自己删掉
    // 本机这份 bond，下一次尝试走全新配对，不需要我们抢时序。
    esp_hidh_dev_t *dev = esp_hidh_dev_open(s_connect_req.bda, ESP_HID_TRANSPORT_BLE,
                                            s_connect_req.addr_type);
    if (dev == nullptr) {
        s_connecting = false;
        ESP_LOGE(TAG, "HID open failed for %s", hex);
    }
    s_connect_task = nullptr;
    vTaskDelete(NULL);
}

// 记录连接请求并启动后台连接任务。s_connected/s_connecting/s_deiniting 门控避免重复发起,
// 连接中状态下后续请求直接跳过。
static void requestConnect(const uint8_t *bda, esp_ble_addr_type_t addr_type) {
    if (s_deiniting || s_connected || s_connecting) {
        ESP_LOGI(TAG, "Already connected or connecting, skip connect request");
        return;
    }
    memcpy(s_connect_req.bda, bda, ESP_BD_ADDR_LEN);
    s_connect_req.addr_type = addr_type;
    s_connecting = true;
    s_connect_started_us = esp_timer_get_time();
    s_connect_task = nullptr;
    // 必须检查返回值。这里原先不看，一旦任务创建失败(内堆不足)就没人再复位
    // s_connecting，主循环的 !isConnecting() 门永远打不开 —— 键盘从此不会自动重连，
    // 且全程没有任何日志（日志里"auto-reconnect retry"之后 connect_task 那句
    // "Connecting to %s..." 一行都没有，就是这个失败形态）。
    if (xTaskCreate(connect_task, "bt_conn", 4096, NULL, 3, &s_connect_task) != pdPASS) {
        s_connect_task = nullptr;
        s_connecting = false;
        s_connect_started_us = 0;
        // 打的是**内部** RAM：xTaskCreate 的 TCB+栈只能在内部 RAM 上分配，而这台机器
        // 内部 RAM 常年吃紧（蓝牙 + WiFi 的地板见 internal-ram-floor 那条笔记），
        // 用 esp_get_free_heap_size() 会把 8MB PSRAM 算进去，完全看不出真正的原因。
        ESP_LOGE(TAG, "bt_conn task create failed, internal free=%u, giving up this attempt",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}

BtKeyboard& BtKeyboard::getInstance() {
    static BtKeyboard inst;
    s_self = &inst;
    return inst;
}

esp_err_t BtKeyboard::init() {
    s_self = this;  // g_bt 全局实例直接使用；补齐单例指针，否则 setConnected/save-on-open 被跳过
    // 守卫用 s_init_done（只在**全部**成功时才置位），不能再用 `if (s_queue)`：
    // 队列是在蓝牙栈之前就建好的，一旦后面某步失败（唤醒时内部 RAM 正紧，很容易），
    // 下次 init 就会因为这个守卫直接返回"成功"，而控制器/Bluedroid 是半死的 ——
    // 表现是自动休眠唤醒后键盘再也连不上，直到重启。
    if (s_init_done) return ESP_OK;

    // 上一次失败留下的残骸先清掉，让这次能从干净状态重来。
    if (s_queue) { vQueueDelete(s_queue); s_queue = nullptr; }
    if (s_devices_mutex) { vSemaphoreDelete(s_devices_mutex); s_devices_mutex = nullptr; }

    // 失败回退：按"走到哪一步"逐级关回去（stage 越大表示走得越深），最后删掉队列/互斥量，
    // 保证返回值不为 OK 时系统状态和没调用过 init 一样。
    auto fail = [](esp_err_t err, int stage) -> esp_err_t {
        // esp_hidh_deinit **必须**在 bluedroid 关掉之前调（它要经 bluedroid 关设备、
        // 删事件循环）。stage 6 = esp_hidh_init 失败；若失败原因是上一次清理残留的
        // "Already initialized"，这里正是把残骸收掉的时机，否则每轮 init 都会栽在同一处。
        if (stage >= 6) esp_hidh_deinit();
        if (stage >= 6) esp_bluedroid_disable();
        if (stage >= 5) esp_bluedroid_deinit();
        if (stage >= 4) esp_bt_controller_disable();
        if (stage >= 3) esp_bt_controller_deinit();
        if (s_queue) { vQueueDelete(s_queue); s_queue = nullptr; }
        if (s_devices_mutex) { vSemaphoreDelete(s_devices_mutex); s_devices_mutex = nullptr; }
        s_init_done = false;
        ESP_LOGE(TAG, "BT init failed (stage %d): %s", stage, esp_err_to_name(err));
        return err;
    };

    s_deiniting = false;
    s_queue = xQueueCreate(32, sizeof(uint16_t));
    if (!s_queue) return ESP_FAIL;

    s_devices_mutex = xSemaphoreCreateMutex();
    if (!s_devices_mutex) return fail(ESP_FAIL, 2);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) return fail(ret, 2);
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) return fail(ret, 3);

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (ret != ESP_OK) return fail(ret, 4);
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) return fail(ret, 5);

    esp_ble_gap_register_callback(ble_gap_cb);

    // SMP/security 参数：SC + Just Works（见 applySecurityParams 上面的说明）。
    applySecurityParams();

    esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler);

    esp_hidh_config_t hid_cfg = {
        .callback = hidh_cb,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    ret = esp_hidh_init(&hid_cfg);
    if (ret != ESP_OK) return fail(ret, 6);

    // Pre-configure extended scan params
    s_ext_scan_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    s_ext_scan_params.filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    s_ext_scan_params.scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE;
    s_ext_scan_params.cfg_mask = ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK;
    s_ext_scan_params.uncoded_cfg.scan_type = BLE_SCAN_TYPE_ACTIVE;
    s_ext_scan_params.uncoded_cfg.scan_interval = 0x50;
    s_ext_scan_params.uncoded_cfg.scan_window = 0x30;

    ESP_LOGI(TAG, "BT keyboard driver initialized");
    s_init_done = true;
    return ESP_OK;
}

void BtKeyboard::deinit() {
    s_deiniting = true;
    s_connect_task = nullptr;
    // 扫描用射频，而且拆 bluedroid 时扫描还在跑会报一堆错 —— 先停掉它。
    if (s_scanning) {
        s_scanning = false;
        esp_ble_gap_stop_ext_scan();
    }
    if (s_dev) {
        esp_hidh_dev_close(s_dev);
        // esp_hidh_deinit 要求所有设备已关闭,而 close 是异步的
        // (close 事件约 60-100ms 后到达)。不等它完成就 deinit 会返回错误,
        // 导致唤醒后 esp_hidh_init 无法重新初始化、键盘无法重连。
        vTaskDelay(pdMS_TO_TICKS(200));
        s_dev = nullptr;
    }
    // 栈拆了，连接状态就不能活过 teardown。close 事件是异步的（约 60-100ms 后才到），
    // 上面那 200ms 通常够，但栈正在关时事件可能根本送不进来 —— 那 s_connected 就**停在
    // true**，而 s_dev 已经是空。唤醒后 isConnected() 一直为真：阅读模式所有输入框的
    // 虚拟键盘（门控全是 !isConnected()）再也弹不出来，开机自动重连也直接跳过。
    // 这里显式清掉，也让状态栏的蓝牙图标别再画着"已连接"。
    s_connected = false;
    connected_ = false;
    // 先关 bluedroid:若有连接尝试在飞行中(重试失败后立即重发,休眠时刻
    // 几乎必然撞上),它会在栈关闭过程中失败并自行从设备列表释放。等它退出后
    // 再 esp_hidh_deinit 才能走完清理;否则 esp_hidh_deinit 因列表非空提前返回,
    // 泄漏事件循环与信号量,唤醒后 esp_hidh_init 报 Already initialized 失败,
    // BT 瘫到下一次完整 deinit(表现为自动休眠唤醒后键盘永远连不上)。
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    int waited = 0;
    while (s_connecting && waited < 3000) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    if (s_connecting) {
        // esp_hidh_dev_open 是同步阻塞的（内部 WAIT_CB 取 semaphore 用 portMAX_DELAY，
        // 见 IDF 的 ble_hidh.c），而它在**真正连接之前**就把设备挂进了全局设备表。
        // 栈关掉通常能让它报错退出，万一是卡在配对/加密阶段没退，设备就一直在表里。
        ESP_LOGW(TAG, "连接尝试 %dms 后仍在飞行中，hidh 可能无法完全清理", waited);
    }
    s_connecting = false;
    // 退避重试：之前只调一次、失败就静默丢返回值(esp_hidh_deinit 在设备表非空时开头
    // 就返回 ESP_ERR_INVALID_STATE)，留下事件循环与信号量泄漏 —— 唤醒后
    // esp_hidh_init 报 Already initialized，键盘一直连不上，直到下次完整 deinit。
    // 函数开头就是守卫判断，失败时重复调用无副作用。
    esp_err_t hret = ESP_FAIL;
    for (int i = 0; i < 4; i++) {
        hret = esp_hidh_deinit();
        if (hret == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (hret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_deinit 失败(%s)：BT 这次清理不干净，需下个 init 周期自愈",
                 esp_err_to_name(hret));
    }
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    if (s_queue) { vQueueDelete(s_queue); s_queue = nullptr; }
    if (s_devices_mutex) { vSemaphoreDelete(s_devices_mutex); s_devices_mutex = nullptr; }
    s_init_done = false;
}

void BtKeyboard::scanDevices() {
    // 上一轮扫描还没结束就别再起一个：连点两下会白建一个 4KB 内部栈的任务。
    // **必须在清空列表之前拦下**：以前先清列表再判断 s_scanning，扫描还在跑时
    // 第二次点"添加设备"会把已经收集到的设备全抹掉，而剩下的扫描窗口里未必会
    // 再上报一遍 —— 列表就一直是空的。
    if (s_scanning) {
        ESP_LOGI(TAG, "Scan already in progress, keep current results");
        return;
    }
    if (s_dev) {
        esp_hidh_dev_close(s_dev);
        esp_hidh_dev_free(s_dev);
        s_dev = nullptr;
    }
    s_connected = false;
    connected_ = false;
    resetKbBattery();   // 键盘电量失效（低电提示一并复位）
    memset(s_last_keys, 0, MAX_KEYS);
    clearPendingKeys();
    // Clear device list
    if (s_devices_mutex) xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
    s_found_count = 0;
    if (s_devices_mutex) xSemaphoreGive(s_devices_mutex);
    xTaskCreate(scan_task, "bt_scan", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "BT scan started for HID keyboards");
}

int BtKeyboard::deviceCount() {
    return s_found_count;
}

const BtDeviceInfo* BtKeyboard::getDevice(int idx) {
    if (idx < 0 || idx >= s_found_count) return nullptr;
    return &s_found_devices[idx];
}

void BtKeyboard::clearDevices() {
    if (s_devices_mutex) xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
    s_found_count = 0;
    if (s_devices_mutex) xSemaphoreGive(s_devices_mutex);
}

bool BtKeyboard::isScanning() {
    return s_scanning;
}

bool BtKeyboard::isConnecting() const {
    if (!s_connecting) return false;
    // 自愈：连接尝试有可能永远不结束（任务创建失败、Bluedroid 命令阻塞、OPEN 事件
    // 丢失）。超时即视为本次尝试失败，主动放行 —— 否则主循环每次都被 !isConnecting()
    // 挡回去，键盘再也不会自动重连，而日志上完全看不出原因。
    if (s_connect_started_us != 0 &&
        esp_timer_get_time() - s_connect_started_us > CONNECT_ATTEMPT_TIMEOUT_US) {
        ESP_LOGW(TAG, "Connect attempt stuck for %lld ms, abandoning it",
                 (long long)((esp_timer_get_time() - s_connect_started_us) / 1000));
        s_connecting = false;
        s_connect_started_us = 0;
        return false;
    }
    return true;
}

esp_err_t BtKeyboard::connectDevice(int idx) {
    if (idx < 0 || idx >= s_found_count) return ESP_ERR_INVALID_ARG;
    if (s_dev) {
        esp_hidh_dev_close(s_dev);
        s_dev = nullptr;
    }
    s_connected = false;
    connected_ = false;
    resetKbBattery();   // 键盘电量失效（低电提示一并复位）

    auto &d = s_found_devices[idx];
    ESP_LOGI(TAG, "Connecting to %s...", d.name);
    s_paired_addr_type = d.addr_type;

    // Save device info immediately so it's persisted even if HID channel
    // encounters issues after BLE pairing succeeds
    savePairedDevice(d.bda, d.addr_type, d.name);

    requestConnect(d.bda, d.addr_type);
    return ESP_OK;
}

void BtKeyboard::disconnect() {
    s_connecting = false;  // 取消连接中状态
    if (s_dev) {
        esp_hidh_dev_close(s_dev);
        s_dev = nullptr;
    }
    s_connected = false;
    connected_ = false;
    resetKbBattery();   // 键盘电量失效（低电提示一并复位）
    memset(s_last_keys, 0, MAX_KEYS);
    clearPendingKeys();
    if (s_queue) xQueueReset(s_queue);
}

int BtKeyboard::keyboardBatteryPct() {
    return s_kb_battery;
}

// 取一次"键盘电量低"的待发提示，取走即清。UI 主循环每轮问一次（见 main.cpp）。
// 置位在 HID 事件回调（另一个任务）里，这里只做一次读+清——漏掉或重复一次都无所谓，
// 不值得为它上锁。
bool BtKeyboard::takeLowBatteryWarning() {
    if (!s_kb_low_batt_pending) return false;
    s_kb_low_batt_pending = false;
    return true;
}

uint16_t BtKeyboard::readKey() {
    uint16_t c = 0;
    if (s_queue && xQueueReceive(s_queue, &c, 0) == pdTRUE) return c;
    return 0;
}

bool BtKeyboard::waitKey(uint32_t timeout_ms) {
    if (!s_queue) return false;
    uint16_t c = 0;
    // peek：看一眼就走，键留在队列里给 readKey/input_poll 那条唯一出口取。
    return xQueuePeek(s_queue, &c, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void BtKeyboard::flushKeys() {
    // 待发键也一起丢掉：调用方要的是"从现在起没有旧输入"，抬手时再补打出来
    // 一个字符就违背了这个意思。
    clearPendingKeys();
    if (s_queue) xQueueReset(s_queue);
}

void BtKeyboard::checkKeyRepeat() {
    if (!s_queue || !s_connected) return;

    int64_t now = esp_timer_get_time();
    int64_t delay_us = KEY_REPEAT_DELAY_MS * 1000;
    int64_t interval_us = KEY_REPEAT_INTERVAL_MS * 1000;

    // 消费键的"丢失抬起"保护：遥控器丢了 key-up 时清掉去重标记，否则同一个键
    // 再按一次会被 handleConsumerReport 当成"还按着"直接吞掉。
    if (s_last_consumer != 0 && now - s_consumer_press_us >= (int64_t)KEY_REPEAT_MAX_MS * 1000) {
        s_last_consumer = 0;
    }

    // Check each key slot for repeat
    for (int i = 0; i < MAX_KEYS; i++) {
        uint8_t kc = s_last_keys[i];
        if (kc == 0) continue;

        int64_t press_time = s_key_press_time[i];
        if (press_time == 0) continue;

        int64_t elapsed = now - press_time;

        // 丢失 key-up 保护:真正按住会持续收到键盘报告,按住超过上限(3s)几乎
        // 都是 key-up 通知丢了——此时键盘早已松开,清掉槽位停止无限自动重复。
        if (elapsed >= (int64_t)KEY_REPEAT_MAX_MS * 1000) {
            ESP_LOGW(TAG, "key 0x%02X held >%dms (key-up lost?), stop repeat",
                     kc, KEY_REPEAT_MAX_MS);
            // 还在等长按判定的键（主循环被长时间占住才会走到这里）：按长按的意图
            // 补发上档符号，别让这一键凭空消失。
            if (s_key_pending[i]) pushKey(s_key_pending_shift[i]);
            s_key_pending[i] = 0;
            s_key_pending_shift[i] = 0;
            s_key_long_fired[i] = false;
            s_last_keys[i] = 0;
            s_key_press_time[i] = 0;
            s_last_repeat_time[i] = 0;
            continue;
        }

        // 长按输入符号：待发键到阈值就改发键帽上的符号（sym+键），按住期间也不再自动重复。
        // 频率很低（用户想打符号才按住），留一行日志方便以后查"为什么没生效"。
        if (s_key_pending[i]) {
            if (elapsed >= LONG_PRESS_SYMBOL_US) {
                ESP_LOGI(TAG, "long-press fire kc=0x%02X elapsed=%lldms -> 0x%02X",
                         kc, (long long)(elapsed / 1000), (unsigned)s_key_pending_shift[i]);
                pushKey(s_key_pending_shift[i]);
                s_key_pending[i] = 0;
                s_key_pending_shift[i] = 0;
                s_key_long_fired[i] = true;
            }
            continue;   // 还没到阈值：这一键还没发出去，当然不重复
        }
        // 符号已经发过：按住别再刷出一串符号
        if (s_key_long_fired[i]) continue;

        // Check if key held long enough for repeat
        if (elapsed >= delay_us) {
            // Initialize last repeat time on first check
            if (s_last_repeat_time[i] == 0) {
                s_last_repeat_time[i] = press_time + delay_us;
            }

            // Send one repeat if we're past the next repeat time
            if (now >= s_last_repeat_time[i] + interval_us) {
                // Ctrl modifier handling for repeat
                bool ctrl = (s_last_mod & 0x11) != 0;
                if ((s_last_mod & 0x44) && kc == 21) {
                    // Alt+R → 全刷；按住不自动重复（免得一直整屏刷）。不消费的话
                    // 下面 hid_to_ascii 会把它当普通 'r'，正文字符会连成一串。
                } else if (ctrl && (s_last_mod & 0x22) && kc == 9) {
                    // Ctrl+Shift+F → trad toggle; consume repeat
                } else if (ctrl && kc == 12) {
                    uint8_t ci = KEY_CTRL_I;
                    pushKey(ci);
                } else if (ctrl && kc == 44) {
                    uint8_t toggle = KEY_IME_TOGGLE;
                    pushKey(toggle);
                } else if (ctrl && kc == 40) {
                    uint8_t ce = KEY_CTRL_ENTER;
                    pushKey(ce);
                } else if (ctrl && (s_last_mod & 0x22) && kc == 56) {
                    // Ctrl+? → 帮助; 按住不自动重复,避免误开关
                } else if (ctrl && (s_last_mod & 0x22) && kc == 29) {
                    // Ctrl+Shift+Z → redo; no key repeat
                } else if (ctrl && kc == 56) {
                    // Ctrl+/ → 搜索对话框; 按住不自动重复,避免误开/误关
                } else if ((s_last_mod & 0x22) && kc == 44) {
                    // Shift+Space → fullwidth toggle (repeat)
                    uint8_t fwt = KEY_FULLWIDTH_TOGGLE;
                    pushKey(fwt);
                } else if (ctrl && kc >= 4 && kc <= 29) {
                    uint8_t cc = kc - 3;
                    pushKey(cc);
                } else if (ctrl && kc >= 30 && kc <= 39) {
                    // Ctrl+0-9 repeat → 文件切换码(编辑器对同文件 no-op)
                    int fileIdx = (kc == 39) ? 0 : (kc - 29);
                    uint8_t fk = KEY_FILE_BASE + fileIdx;
                    pushKey(fk);
                } else {
                    uint8_t ascii = hid_to_ascii(kc, s_last_mod);
                    if (ascii) pushKey(ascii);
                }

                // Update last repeat time
                s_last_repeat_time[i] = now;
            }
        }
    }
}

void BtKeyboard::savePairedDevice(const uint8_t *bda, esp_ble_addr_type_t addr_type, const char *name) {
    // Upsert: move to front (most recently used first), drop oldest if full
    int idx = -1;
    for (int i = 0; i < s_paired_count; i++) {
        if (memcmp(s_paired[i].bda, bda, ESP_BD_ADDR_LEN) == 0) { idx = i; break; }
    }
    // 记住旧名字：连接后加密失败时 esp_hidh_dev_name_get() 返回空串，HID OPEN
    // 事件照样会走到这里。以前空名字会覆盖掉扫描时存下的 "TypixNode KBD"，变成
    // "BLE-24e2ac" —— 一次失败的连接就把列表里的名字毁了。空名字时保留旧名字。
    char prev_name[32] = "";
    if (idx >= 0) {
        snprintf(prev_name, sizeof(prev_name), "%s", s_paired[idx].name);
        for (int i = idx; i < s_paired_count - 1; i++) s_paired[i] = s_paired[i + 1];
        s_paired_count--;
    }
    if (s_paired_count >= MAX_BT_DEVICES) s_paired_count = MAX_BT_DEVICES - 1;
    for (int i = s_paired_count; i > 0; i--) s_paired[i] = s_paired[i - 1];
    s_paired_count++;
    memcpy(s_paired[0].bda, bda, ESP_BD_ADDR_LEN);
    s_paired[0].addr_type = addr_type;
    if (name && name[0]) {
        snprintf(s_paired[0].name, sizeof(s_paired[0].name), "%.31s", name);
    } else if (prev_name[0]) {
        snprintf(s_paired[0].name, sizeof(s_paired[0].name), "%s", prev_name);
    } else {
        snprintf(s_paired[0].name, sizeof(s_paired[0].name), "BLE-%02x%02x%02x", bda[3], bda[4], bda[5]);
    }
    // 原子落盘(tmp+fsync+rename)：满卡/掉电不会把 bt_paired 截断成空，
    // 失败时 safeWriteFile 会打出 errno，便于确认"保存失败"是否 SD 卡问题。
    char line[64];
    std::string content;
    content.reserve((size_t)s_paired_count * 40);
    for (int i = 0; i < s_paired_count; i++) {
        snprintf(line, sizeof(line), "%02x%02x%02x%02x%02x%02x\n%d\n%s\n",
                 s_paired[i].bda[0], s_paired[i].bda[1], s_paired[i].bda[2],
                 s_paired[i].bda[3], s_paired[i].bda[4], s_paired[i].bda[5],
                 (int)s_paired[i].addr_type, s_paired[i].name);
        content += line;
    }
    if (!safeWriteFile("/sdcard/settings/bt_paired", content)) {
        ESP_LOGE(TAG, "Failed to save paired devices (SD card write error)");
        return;
    }
    ESP_LOGI(TAG, "Saved %d paired device(s), first %s", s_paired_count, s_paired[0].name);
}

void BtKeyboard::loadPairedDevices() {
    s_paired_count = 0;
    FILE *f = fopen("/sdcard/settings/bt_paired", "r");
    if (!f) return;
    char line[64];
    while (s_paired_count < MAX_BT_DEVICES) {
        if (!fgets(line, sizeof(line), f)) break;
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strlen(line) != 12) continue;  // skip invalid line
        char hex[13];
        strncpy(hex, line, 12); hex[12] = '\0';
        int at = 0;
        if (!fgets(line, sizeof(line), f)) break;
        at = atoi(line);
        char name[32] = "";
        if (fgets(line, sizeof(line), f)) {
            nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            snprintf(name, sizeof(name), "%.31s", line);
        }
        for (int i = 0; i < 6; i++) {
            // 必须初始化 + 看返回值：文件被截断/损坏时这一行的 12 个字符未必是十六
            // 进制（解析失步后会把上一台的地址行或名字行当成本行的 bda），sscanf 失败
            // 时 byte 会留着栈垃圾 → 拿一个随机地址去 connectBDA。
            unsigned int byte = 0;
            if (sscanf(hex + i * 2, "%02x", &byte) != 1) byte = 0;
            s_paired[s_paired_count].bda[i] = (uint8_t)byte;
        }
        s_paired[s_paired_count].addr_type = (esp_ble_addr_type_t)at;
        if (!name[0]) {
            snprintf(name, sizeof(name), "BLE-%02x%02x%02x",
                     s_paired[s_paired_count].bda[3],
                     s_paired[s_paired_count].bda[4],
                     s_paired[s_paired_count].bda[5]);
        }
        snprintf(s_paired[s_paired_count].name, sizeof(s_paired[s_paired_count].name), "%s", name);
        s_paired_count++;
    }
    fclose(f);
    ESP_LOGI(TAG, "Loaded %d paired device(s)", s_paired_count);
}

bool BtKeyboard::removePairedDevice(const uint8_t *bda) {
    for (int i = 0; i < s_paired_count; i++) {
        if (memcmp(s_paired[i].bda, bda, ESP_BD_ADDR_LEN) == 0) {
            for (int j = i; j < s_paired_count - 1; j++) s_paired[j] = s_paired[j + 1];
            s_paired_count--;
            // 与 savePairedDevice 同一套写法：拼好整份内容走 safeWriteFile（tmp+fsync+
            // rename）。这里原来是 fopen("w") 就地截断重写 —— 删一台设备时若掉电/拔卡，
            // 文件会停在 0 字节或半条记录，**没被删的那几台也一起丢**；而且 fopen 失败
            // 时它已经改过内存、却 return true，界面报"已删除"、重启后设备又回来。
            std::string content;
            content.reserve((size_t)s_paired_count * 40);
            char line[64];
            for (int k = 0; k < s_paired_count; k++) {
                snprintf(line, sizeof(line), "%02x%02x%02x%02x%02x%02x\n%d\n%s\n",
                         s_paired[k].bda[0], s_paired[k].bda[1], s_paired[k].bda[2],
                         s_paired[k].bda[3], s_paired[k].bda[4], s_paired[k].bda[5],
                         (int)s_paired[k].addr_type, s_paired[k].name);
                content += line;
            }
            const bool saved = safeWriteFile("/sdcard/settings/bt_paired", content);
            if (!saved) ESP_LOGE(TAG, "Failed to save paired devices (SD card write error)");
            ESP_LOGI(TAG, "Removed paired device, %d remaining", s_paired_count);
            return saved;
        }
    }
    return false;
}

int BtKeyboard::pairedDeviceCount() {
    return s_paired_count;
}

const BtPairedDevice* BtKeyboard::getPairedDevice(int idx) {
    if (idx < 0 || idx >= s_paired_count) return nullptr;
    return &s_paired[idx];
}

int BtKeyboard::connectedPairedIndex() {
    if (!s_connected || !s_dev) return -1;
    const uint8_t *bda = esp_hidh_dev_bda_get(s_dev);
    if (!bda) return -1;
    for (int i = 0; i < s_paired_count; i++) {
        if (memcmp(s_paired[i].bda, bda, ESP_BD_ADDR_LEN) == 0) return i;
    }
    return -1;
}

bool BtKeyboard::isConnected() const {
    // s_dev 只在 HIDH 打开事件里赋值、在关闭事件里清空，「连接中 ⇒ s_dev 非空」是本文件
    // 既有的约定（sendKey / connectedPairedIndex 都按它判）。唯独这里只看 s_connected：
    // 键盘掉线而 CLOSE 事件没送到（链路丢了事件、键盘没电自己关）时，s_connected 会**永久
    // 停在 true**。阅读模式所有输入框的虚拟键盘自动弹出都写着 `!g_bt.isConnected()`，于是
    // 键盘再也弹不出来（用户看到的就是"整个阅读模式都弹不出虚拟键盘"），开机自动重连也
    // 因为"已连接"直接跳过。把设备句柄一起判，句柄没了就当没连上，状态能自愈。
    // / Treat a missing device handle as disconnected: without it a lost CLOSE event
    // / leaves s_connected stuck true forever, which silently disables the on-screen
    // / keyboard (every reader input box gates its popup on !isConnected()).
    return s_connected && s_dev != nullptr;
}

bool BtKeyboard::isInitialized() const {
    return s_init_done;
}

void BtKeyboard::setConnected(bool c) {
    s_connected = c;
    connected_ = c;
}

esp_err_t BtKeyboard::connectBDA(const uint8_t *bda, esp_ble_addr_type_t addr_type) {
    if (!s_init_done) return ESP_FAIL;  // BLE stack not initialized yet

    if (s_dev) {
        esp_hidh_dev_close(s_dev);
        s_dev = nullptr;
    }
    s_connected = false;
    connected_ = false;

    s_paired_addr_type = addr_type;
    char hex[13];
    snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    ESP_LOGI(TAG, "Auto-connecting to saved device %s...", hex);
    requestConnect(bda, addr_type);
    return ESP_OK;
}
