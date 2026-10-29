#pragma once

#include <cstdint>
#include <cstring>
#include <esp_err.h>
#include <esp_bt_defs.h>
#include <esp_gap_ble_api.h>

#define MAX_BT_DEVICES 10

// Key repeat timing
#define KEY_REPEAT_DELAY_MS    500  // Initial delay before first repeat
#define KEY_REPEAT_INTERVAL_MS 50   // Interval between repeats
// 丢失 key-up 保护:按键"按住"超过此上限(几乎都是蓝牙丢了 key-up 通知,键盘早已松开)
// 即清掉该键停止自动重复,否则会以 50ms 间隔无限刷出一堆字符。
#define KEY_REPEAT_MAX_MS      3000 // Max hold time before a key is treated as released

struct BtDeviceInfo {
    esp_bd_addr_t bda;
    esp_ble_addr_type_t addr_type;
    char name[32];
    int rssi;
};

struct BtPairedDevice {
    esp_bd_addr_t bda;
    esp_ble_addr_type_t addr_type;
    char name[32];
};

class BtKeyboard {
public:
    BtKeyboard() = default;
    static BtKeyboard& getInstance();

    esp_err_t init();
    void deinit();

    // Scan for BLE HID keyboards (non-blocking, starts background task)
    void scanDevices();

    // Access scan results (valid after scan completes)
    int deviceCount();
    const BtDeviceInfo* getDevice(int idx);
    void clearDevices();

    // Connect to a discovered device by index
    esp_err_t connectDevice(int idx);
    void disconnect();

    // Persistent pairing: save/load devices on SD card for auto-reconnect
    void savePairedDevice(const uint8_t *bda, esp_ble_addr_type_t addr_type, const char *name);
    void loadPairedDevices();
    bool removePairedDevice(const uint8_t *bda);
    int pairedDeviceCount();
    const BtPairedDevice* getPairedDevice(int idx);
    int connectedPairedIndex();  // paired-list index of connected device, -1 if none
    esp_err_t connectBDA(const uint8_t *bda, esp_ble_addr_type_t addr_type);
    // 收掉已经收工的连接任务（PSRAM 栈那趟要 vTaskDeleteWithCaps 才放得掉栈）。
    // 由持有者周期调用：main 的 bt_init 循环每 200ms 一次、deinit 开头各一次。
    // 没在"收工且未收尸"状态时是空操作。
    void reapFinishedConnect();

    // 断连期间的"在场扫描"：重连循环每拍调 presenceTick() 维护扫描起停；
    // 看到已配对设备在广播时 takePresenceHit() 取走它的**配对地址**，由调用方立刻连。
    // 没有这一步，回连只能靠"盲连"（不在场的设备要空等 30 秒），键盘开机后那段
    // 几十秒的广播窗口很容易整段错过。
    void presenceTick();
    bool takePresenceHit(esp_bd_addr_t bda, esp_ble_addr_type_t *addr_type);
    // 在场扫描是否在跑（扫描健康时，主循环把"盲连"退成低频安全网）。
    bool presenceScanActive() const;

    // Keyboard input。返回 pjournal 键码；BLE 遥控器的消费类键 >0xFF（见 KEY_CONSUMER_BASE）。
    uint16_t readKey();
    void flushKeys();
    void checkKeyRepeat();  // Check for key repeat events

    // 等"下一个键就绪"，但**不取走**（xQueuePeek）：返回 true = 队列里已经压着键，
    // 调用方该立刻回主循环去 readKey；false = 超时。给界面空转的睡眠用 —— 原来整块
    // 睡 50ms，蓝牙键盘连打时每个键都得先等满这一觉，端到端白多出最多 50ms。
    // 用 peek 而不是 receive 是刻意的：取键的统一出口仍然是 input_poll，这里只是
    // 提早醒来，不让两处都能消费队列。队列没建（未初始化）时直接返回 false。
    bool waitKey(uint32_t timeout_ms);

    // Status
    bool isConnected() const;
    bool isInitialized() const;  // true after init() completes (async boot)
    bool isScanning();
    bool isConnecting() const;  // 新增：检查是否正在连接
    void setConnected(bool c);
    int keyboardBatteryPct();  // 键盘电池电量 %，-1=未知/未连接
    // 键盘电量低于阈值时置一次待发标记，UI 取一次即清（见 .cpp 的说明）。
    bool takeLowBatteryWarning();

private:
    bool connected_ = false;
};

extern BtKeyboard g_bt;
