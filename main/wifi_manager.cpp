#include "wifi_manager.h"
#include <cstring>
#include <vector>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "WiFi";
WifiManager g_wifi;

// 把 ESP-IDF 的 disconnect reason 翻成人话——"密码正确却连不上"时，
// 这一条就把原因指出来了（5GHz 网段 / 密码错 / 认证方式不匹配 等）。
static const char *wifiReasonText(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE: return "认证超时(AP 没回)";
        case WIFI_REASON_AUTH_LEAVE: return "被 AP 断开";
        case WIFI_REASON_ASSOC_TOOMANY: return "AP 连接数已满";
        case WIFI_REASON_ASSOC_LEAVE: return "自己主动断开";
        case WIFI_REASON_ASSOC_NOT_AUTHED: return "关联但未认证";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "四次握手超时 → 密码错 / 加密方式不匹配";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "握手超时 → 密码错";
        case WIFI_REASON_AUTH_FAIL: return "认证失败 → 密码错";
        case WIFI_REASON_ASSOC_FAIL: return "关联失败";
        case WIFI_REASON_CONNECTION_FAIL: return "连接失败(AP 无响应)";
        case WIFI_REASON_NO_AP_FOUND: return "找不到该 SSID（拼错 / 只跑 5GHz / 信号太弱）";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "找到 SSID 但加密方式不支持";
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "找到 SSID 但加密弱于阈值";
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: return "找到 SSID 但信号低于阈值";
        default: return "见 reason 码";
    }
}

// 连不上时扫一遍，确认这个 SSID 在 2.4GHz 上到底可不可见。
// ESP32-S3 只支持 2.4GHz——路由器如果只有 5GHz 频段（或同一 SSID 只广播 5GHz），
// 现象就是"密码明明是对的却连不上"。
static void log_scan_for(const char *ssid) {
    wifi_scan_config_t sc = {};
    sc.show_hidden = true;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        ESP_LOGW(TAG, "扫描失败（radio 没起来？）");
        return;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) {
        ESP_LOGW(TAG, "扫描结果: 一个 AP 都没扫到（天线/射频问题？）");
        return;
    }
    std::vector<wifi_ap_record_t> recs(num);
    if (esp_wifi_scan_get_ap_records(&num, recs.data()) != ESP_OK) return;
    bool found = false;
    for (uint16_t i = 0; i < num; i++) {
        if (strncmp(reinterpret_cast<const char *>(recs[i].ssid), ssid, sizeof(recs[i].ssid)) != 0) continue;
        found = true;
        ESP_LOGI(TAG, "扫描命中 \"%s\": rssi=%d 信道=%u auth=%d", ssid, recs[i].rssi,
                 (unsigned)recs[i].primary, (int)recs[i].authmode);
    }
    if (!found) {
        ESP_LOGW(TAG, "扫描到 %u 个 AP，但**没有** \"%s\"（SSID 拼错 / 只在 5GHz / 隐藏且不在范围内）",
                 (unsigned)num, ssid);
        for (uint16_t i = 0; i < num && i < 12; i++) {
            ESP_LOGI(TAG, "  可见 AP: \"%s\" rssi=%d ch=%u auth=%d",
                     reinterpret_cast<const char *>(recs[i].ssid), recs[i].rssi,
                     (unsigned)recs[i].primary, (int)recs[i].authmode);
        }
    }
}

static bool s_connected = false;
static bool s_auto_reconnect = false;
static uint8_t s_last_reason = 0;   // 最近一次断开原因，供失败后诊断打印
static EventGroupHandle_t s_wifi_event = NULL;
const int WIFI_CONNECTED_BIT = BIT0;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // STA_START also fires during connect()'s esp_wifi_start(), but the new
        // config isn't applied yet — auto-connecting here would use the stale
        // flash config and block the subsequent set_config. Only reconnect after
        // a real disconnect (s_auto_reconnect is turned on post-set_config).
        if (s_auto_reconnect) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto *ev = (wifi_event_sta_disconnected_t *)data;
        const uint8_t reason = ev ? ev->reason : 0;
        ESP_LOGW(TAG, "STA 断开 reason=%u (%s)", (unsigned)reason, wifiReasonText(reason));
        s_last_reason = reason;
        bool auto_reconnect = s_auto_reconnect;  // Read before clearing state
        s_connected = false;
        if (auto_reconnect) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        if (s_wifi_event) xEventGroupSetBits(s_wifi_event, WIFI_CONNECTED_BIT);
    }
}

bool WifiManager::begin() {
    if (_inited) return true;
    esp_netif_init();
    // 事件循环可能已创建，忽略已存在错误
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Event loop create failed: %d", ret);
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    // 配置存 RAM 而非 NVS：文件管理/同步只在需要时才临时起 WiFi，避免 NVS 磨损与
    // 分区差异导致的配置失效（与 read_pico 官方固件一致）。
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    // 这里**不**调 esp_wifi_start()：静态 RX/TX 缓冲池只在 esp_wifi_init() 分配，
    // 而它们必须是内部 DMA RAM（PSRAM 走不了）。阅读模式/epdiy/Bluedroid 起来后内部
    // RAM 只剩 ~30KB，此时再 init 会 `esf_buf_setup_static: alloc eb fail` 起不来。
    // 因此 begin() 必须在开机早期（内部 RAM ~170KB）就调用；start() 只分配可走 PSRAM
    // 的动态缓冲，按需在 connect() 里做。见 main.cpp 开头的 g_wifi.begin()。

    s_wifi_event = xEventGroupCreate();
    _inited = true;
    return true;
}

bool WifiManager::connect(const char *ssid, const char *password) {
    if (!ssid || !*ssid) return false;
    // 先抑制自动重连：esp_wifi_start() 会触发 STA_START，若此处 auto_reconnect
    // 已为 true，处理器会用旧配置抢先连接，导致下方的 set_config 报
    // "sta is connecting, cannot set config"，新 SSID 从未生效
    s_auto_reconnect = false;

    // Radio may have been stopped by a previous disconnect() — bring it back up
    if (!_started) {
        if (esp_wifi_start() != ESP_OK) return false;
        _started = true;
    }

    // 关键：**已连接时绝不能调 esp_wifi_connect()**。驱动会把请求丢掉，并顺手
    // 把现有连接拆掉（"sta is connected, disconnect before connecting to new ap"），
    // 于是本来好好的网被再按一次「连接」按断，然后干等 10 秒超时报"连接失败"——
    // 现象就是"按了连接没反应"。这里先判：连的就是这台且密码没换 → 直接成功；
    // 换了网络/密码 → 先断开、等驱动回到 idle，再重新配置。
    if (s_connected) {
        wifi_config_t cur = {};
        const char *wantPass = password ? password : "";
        const bool same = esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK &&
                          strncmp((const char *)cur.sta.ssid, ssid, sizeof(cur.sta.ssid)) == 0 &&
                          strncmp((const char *)cur.sta.password, wantPass, sizeof(cur.sta.password)) == 0;
        if (same) {
            ESP_LOGI(TAG, "已连接该 SSID，无需重连");
            return true;
        }
        ESP_LOGI(TAG, "切换网络，先断开当前连接再重连");
        esp_wifi_disconnect();
        for (int i = 0; i < 100 && s_connected; i++) vTaskDelay(pdMS_TO_TICKS(20));
    }

    // 空 SSID 之外最常见的一类"密码对却连不上"：SSID 或密码里混进了首尾空格
    // （虚拟键盘/复制粘贴/手机热点名称带前导空格）。这里只提示长度，不打印内容。
    {
        size_t sl = strlen(ssid), pl = password ? strlen(password) : 0;
        bool lead_or_trail = false;
        if (sl) lead_or_trail = ssid[0] == ' ' || ssid[sl - 1] == ' ';
        if (!lead_or_trail && pl) lead_or_trail = password[0] == ' ' || password[pl - 1] == ' ';
        ESP_LOGI(TAG, "开始连接: SSID=\"%s\"(len=%u) 密码长度=%u%s", ssid, (unsigned)sl, (unsigned)pl,
                 lead_or_trail ? "  ← 含首尾空格，需注意" : "");
    }

    wifi_config_t cfg = {};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (password) strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = (password && *password) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    // 现代路由器常要求 PMF(802.11w) 或 WPA3-SAE：不声明 capability 会在 4-way
    // 握手阶段被拒，表现为"密码正确却连不上"。与 read_pico 官方固件保持一致。
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config 失败: 0x%x (%s)", err, esp_err_to_name(err));
        return false;
    }
    // 新配置已生效，之后掉线才允许自动重连
    s_auto_reconnect = true;
    s_last_reason = 0;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect 失败: 0x%x (%s)", err, esp_err_to_name(err));
        return false;
    }

    // Wait for connection (10s timeout)
    if (s_wifi_event) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event, WIFI_CONNECTED_BIT,
                                                pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
        if (bits & WIFI_CONNECTED_BIT) return true;
    }
    // 失败：先停掉后台重连循环，再扫一遍给出可操作的结论。
    s_auto_reconnect = false;
    esp_wifi_disconnect();
    ESP_LOGW(TAG, "连接超时，最后断开原因=%u (%s)", (unsigned)s_last_reason, wifiReasonText(s_last_reason));
    log_scan_for(ssid);
    return false;
}

bool WifiManager::isConnected() { return s_connected; }

const char *WifiManager::lastReasonText() {
    if (s_connected) return "OK";
    return wifiReasonText(s_last_reason);
}

void WifiManager::disconnect() {
    s_auto_reconnect = false;
    esp_wifi_disconnect();
    s_connected = false;
    // Fully power down the radio; WiFi is only needed on demand
    if (esp_wifi_stop() == ESP_OK) _started = false;
}

std::string WifiManager::getIp() {
    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        char buf[16];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
        return buf;
    }
    return "";
}
