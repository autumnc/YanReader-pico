#include "HalClock.h"

#include <esp_sntp.h>
#include <esp_timer.h>

HalClock halClock;

namespace {
// 2020-01-01，早于它的系统时钟都算没对上。
constexpr time_t kValidFrom = 1577836800;
constexpr int64_t kSyncTimeoutUs = 15LL * 1000 * 1000;
}  // namespace

time_t HalClock::nowUtc() const { return time(nullptr); }

bool HalClock::hasValidTime() const { return nowUtc() >= kValidFrom; }

bool HalClock::setUtcTime(time_t epoch) {
  if (epoch < kValidFrom) return false;
  struct timeval tv = {};
  tv.tv_sec = epoch;
  settimeofday(&tv, nullptr);
  return true;
}

bool HalClock::requestSync() {
  if (!sntpUp_) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, useChina_ ? "ntp.aliyun.com" : "pool.ntp.org");
    esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    esp_sntp_init();
    sntpUp_ = true;
  } else {
    esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  }
  state_ = ClockSyncState::Syncing;
  deadlineUs_ = esp_timer_get_time() + kSyncTimeoutUs;
  return true;
}

ClockSyncState HalClock::syncState() const {
  if (state_ != ClockSyncState::Syncing) return state_;
  if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
    state_ = ClockSyncState::Succeeded;
    deadlineUs_ = 0;
  } else if (deadlineUs_ && esp_timer_get_time() > deadlineUs_) {
    state_ = ClockSyncState::Failed;
    deadlineUs_ = 0;
  }
  return state_;
}

void HalClock::update() { (void)syncState(); }

bool HalClock::syncNow(uint32_t timeoutMs) {
  if (!requestSync()) return false;
  const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeoutMs) * 1000;
  while (esp_timer_get_time() < deadline) {
    if (syncState() == ClockSyncState::Succeeded) return true;
    if (syncState() == ClockSyncState::Failed) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  state_ = ClockSyncState::Failed;
  return false;
}
