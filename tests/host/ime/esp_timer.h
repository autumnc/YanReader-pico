// Host stub for esp_timer.h, used by tests/host/ime.
//
// IME.cpp uses esp_timer_get_time() as a monotonic microsecond clock (perf logging,
// fuzzy-config cache TTL, user-dict save deferral). steady_clock is the right host
// analogue: monotonic, no wall-clock jumps. Host-test only.
#pragma once

#include <chrono>
#include <cstdint>

static inline int64_t esp_timer_get_time(void) {
    return (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
