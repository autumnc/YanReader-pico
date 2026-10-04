#pragma once
// read_pico 版时间助手：只保留微信读书用到的"时钟可不可信 / 现在几点"两件事。
// 原版还带阅读统计的本地日界换算（依赖 CrossPointSettings），本移植不需要。
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

namespace TimeUtils {

// 系统时钟是否已经对过（>= 2020-01-01 视为可信）。
bool isClockValid();
bool isClockValid(uint32_t epochSeconds);

// 当前 epoch 秒；时钟不可信返回 0。
uint32_t getCurrentValidTimestamp();
uint32_t getAuthoritativeTimestamp();

}  // namespace TimeUtils
