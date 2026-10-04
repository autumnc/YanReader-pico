#pragma once
// 阅读模式的最小 Arduino 兼容层：只提供 kept lib 实际用到的 millis/delay/ESP/map。
// 不引入真正的 Arduino core，避免与 ESP-IDF 冲突。

#include <cstdint>
#include <cstddef>

#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

inline uint32_t millis() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

inline unsigned long micros() {
  return static_cast<unsigned long>(esp_timer_get_time());
}

inline void delay(uint32_t ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
}

// Arduino 的 random(min,max)：返回 [min, max)。微信读书的进度上报要在请求里
// 塞两个随机数（防缓存/防重放），用硬件随机源即可。
inline long random(long min, long max) {
  if (max <= min) return min;
  return min + static_cast<long>(esp_random() % static_cast<uint32_t>(max - min));
}
inline long random(long max) { return random(0, max); }

// Arduino 的 map()：长整型线性映射。
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
  if (in_max == in_min) return out_min;
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// kept lib 只用到 getFreeHeap/getMaxAllocHeap/getMinFreeHeap。
struct EspHostStub {
  uint32_t getFreeHeap() const { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }
  uint32_t getMaxAllocHeap() const { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }
  uint32_t getMinFreeHeap() const { return heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT); }
  uint32_t getFreePsram() const { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
  uint32_t getMaxAllocPsram() const { return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); }
  uint32_t getPsramSize() const { return heap_caps_get_total_size(MALLOC_CAP_SPIRAM); }
  uint32_t getHeapSize() const { return heap_caps_get_total_size(MALLOC_CAP_8BIT); }
};

inline EspHostStub ESP;
