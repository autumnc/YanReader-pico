#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

enum class AppAsyncState {
    Idle,
    Running,
    Done,
};

class AppAsyncJob {
public:
    void begin(const std::string &title, const std::string &busy);
    bool start(TaskFunction_t fn, const char *taskName, uint32_t stackBytes, void *arg = nullptr,
               UBaseType_t priority = 1);
    void finish(const std::string &result);
    void failToStart(const std::string &result);
    void reset();

    AppAsyncState state() const;
    std::string title();
    std::string busy();
    std::string result();

    bool drawn = false;
    int64_t untilUs = 0;

private:
    void ensureMutex();
    void lock();
    void unlock();

    std::atomic<AppAsyncState> state_{AppAsyncState::Idle};
    SemaphoreHandle_t mutex_ = nullptr;
    std::string title_;
    std::string busy_;
    std::string result_;
};
