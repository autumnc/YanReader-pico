#pragma once
// 主机测试用的 esp_log 空壳：wifi_store.cpp 里只有一句 ESP_LOGE（写盘失败时）。
#include <cstdio>
#define ESP_LOGE(tag, fmt, ...) std::fprintf(stderr, "[E %s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
