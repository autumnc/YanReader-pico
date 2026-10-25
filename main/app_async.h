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
    // psramStack=true 时任务栈从 PSRAM 出（xTaskCreateWithCaps），失败再退回内部 RAM。
    // 给"任务自己要去连 WiFi"的作业用：连上 WiFi 后内部 RAM 的最大连续块只剩 ~1.5KB，
    // 内部栈的任务根本建不起来；反过来先占内部栈，WiFi 握手又没内存可用。
    bool start(TaskFunction_t fn, const char *taskName, uint32_t stackBytes, void *arg = nullptr,
               UBaseType_t priority = 1, bool psramStack = false);
    // WithCaps 建的任务不能自己 vTaskDelete（IDF 会泄漏栈与 TCB；自删路径还要临时建
    // 清理任务，内部 RAM 紧时直接 abort）。约定：这类任务的入口在 finish() 之后
    // vTaskSuspend 自己，由主任务在消费完结果后调 reapCapsTask()，用
    // vTaskDeleteWithCaps 把栈和 TCB 一并释放。非 WithCaps 任务仍自己 vTaskDelete。
    bool createdWithCaps() const { return capsTask_ != nullptr; }
    void reapCapsTask();
    void finish(const std::string &result);
    void failToStart(const std::string &result);
    void cancel();
    bool cancelled() const;
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
    std::atomic<bool> cancel_{false};
    TaskHandle_t capsTask_ = nullptr;   // 只在 PSRAM 栈创建成功时非空
    SemaphoreHandle_t mutex_ = nullptr;
    std::string title_;
    std::string busy_;
    std::string result_;
};
