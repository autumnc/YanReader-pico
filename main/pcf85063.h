#pragma once

#include <cstdint>
#include <ctime>
#include <driver/i2c_master.h>

// RTC 适配器（替代原 PCF85063 RTC）：Read Pico 由 CW32L010 PMU 维护 RTC，
// 这里保留原 PCF85063 的类名与接口，内部改走 PMU 快照 unix_sec / TIME_SYNC。
class PCF85063 {
public:
    PCF85063() = default;

    // 探测 PMU 就绪（PMU 已在 read_pico_init 初始化）。
    bool begin();

    // 写时间到 PMU RTC（call after NTP sync）。
    bool setTime(time_t unixTime);

    // 读时间。返回 0 表示无效或未对时。
    time_t getTime();

    bool hasValidTime();

private:
    bool _initialized = false;
};

extern PCF85063 g_rtc;

// 暴露 read_pico 的共享 I2C 总线（原 PCF85063 提供，供板载外设挂接）。
i2c_master_bus_handle_t pjournal_get_i2c_bus();
