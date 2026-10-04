#pragma once

#include <Arduino.h>
#include <esp_log.h>

// 阅读模式日志：直接映射到 ESP-IDF 日志。kept lib 只用到 ERR/INF/DBG。
// （原 FreeInkUI 的 Serial/MySerialImpl/getLastLogs 等在此组件中未使用，已移除。）
#define LOG_ERR(origin, format, ...) ESP_LOGE(origin, format, ##__VA_ARGS__)
#define LOG_INF(origin, format, ...) ESP_LOGI(origin, format, ##__VA_ARGS__)
#define LOG_DBG(origin, format, ...) ESP_LOGD(origin, format, ##__VA_ARGS__)
#define LOG_WRN(origin, format, ...) ESP_LOGW(origin, format, ##__VA_ARGS__)
