// esp_log 的主机端桩（与 tests/host/flomo_file/esp_log.h 同款）。
// 只有被顶层 include 路径抢先时才会命中——固件构建永远看不到它。
#pragma once

#include <cstdio>

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I %s: " fmt "\n", tag, ##__VA_ARGS__)
