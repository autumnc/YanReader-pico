#include "TimeUtils.h"

namespace TimeUtils {

namespace {
// 2020-01-01 00:00:00 UTC
constexpr uint32_t kValidFrom = 1577836800u;
}  // namespace

bool isClockValid() { return time(nullptr) >= static_cast<time_t>(kValidFrom); }

bool isClockValid(uint32_t epochSeconds) { return epochSeconds >= kValidFrom; }

uint32_t getCurrentValidTimestamp() {
  const time_t now = time(nullptr);
  return (now >= static_cast<time_t>(kValidFrom)) ? static_cast<uint32_t>(now) : 0;
}

uint32_t getAuthoritativeTimestamp() { return getCurrentValidTimestamp(); }

}  // namespace TimeUtils
