#pragma once
// read_pico 版的时钟 HAL：POSIX 系统时钟是唯一运行时时间源（开机由 RTC/人设写入），
// 需要联网对时的时候起一次 SNTP 并轮询结果。接口按 WeReadWebApi 的使用面裁剪。
#include <cstdint>
#include <ctime>

enum class ClockSyncState : uint8_t {
  Idle,
  Syncing,
  Succeeded,
  Failed,
};

class HalClock;
extern HalClock halClock;

class HalClock {
 public:
  void begin() {}
  void update();   // 轮询 SNTP 状态（也可由 syncState() 隐式触发）

  time_t nowUtc() const;
  bool hasValidTime() const;
  bool setUtcTime(time_t epoch);

  void setAutoSyncEnabled(bool enabled) { autoSync_ = enabled; }
  void setUseChinaServers(bool enabled) { useChina_ = enabled; }
  bool requestSync();
  bool syncNow(uint32_t timeoutMs = 10000);
  ClockSyncState syncState() const;

 private:
  bool autoSync_ = true;
  bool useChina_ = false;
  mutable ClockSyncState state_ = ClockSyncState::Idle;
  mutable int64_t deadlineUs_ = 0;
  bool sntpUp_ = false;
};
