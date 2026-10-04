#pragma once
// Arduino WiFi.h 的最小替身：WeReadHttpClient 只用它判断"网络是否就绪"。
// 这里直接问 ESP-IDF 的 station 接口，避免组件反向依赖 main 里的 wifi_manager。
#include <esp_wifi.h>

#include <cstdint>

typedef enum {
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6,
} wl_status_t;

class WiFiStub {
 public:
  wifi_mode_t getMode() const {
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    return mode;
  }
  wl_status_t status() const {
    wifi_ap_record_t ap{};
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? WL_CONNECTED : WL_DISCONNECTED;
  }
  uint32_t localIP() const { return 0; }
};

extern WiFiStub WiFi;
