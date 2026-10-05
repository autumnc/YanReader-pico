// Host stub for the ESP-IDF logging header, used by tests/host/ime.
//
// IME.cpp logs through ESP_LOGE/W/I, so this shim maps them onto fprintf(stderr).
// This file is host-test only: the firmware build never sees it (it gets the real
// esp_log.h from ESP-IDF), and it is picked up here only because tests/host/ime is
// on the include path ahead of the IDF one.
#pragma once

#include <cstdio>

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) fprintf(stderr, "D %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) fprintf(stderr, "V %s: " fmt "\n", tag, ##__VA_ARGS__)
