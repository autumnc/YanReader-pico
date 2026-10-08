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
    SemaphoreHandle_t mutex_ = nullptr;
    std::string title_;
    std::string busy_;
    std::string result_;
};

// 异步任务的**收尾**：报一次栈水线（余量）再自杀。只能用在实际干活的那些任务函数里，
// 而且是它们最后一件事（读的是"当前任务"的余量，读完就没了）。
//
// 为什么要它：这些 worker 的栈从**内部 RAM** 出（xTaskCreate 一律如此，PSRAM 那条路只对
// xTaskCreateStatic 有效），而内部 RAM 在这台机器上是紧的 —— 8192/6144 这些数字一直是估的，
// 谁也没量过。想知道能不能砍、能不能搬到 PSRAM 栈，先得有水线。
//
// **只登记，不打印**：打印一次（vsnprintf + 串口）自己就要几百字节栈，而我们恰恰在量
// "还剩多少栈"这种边际情况 —— 在快满的栈上再调一次格式化输出，会把要查的问题变成崩溃。
// 所以任务里只塞一个数就死，由不在这些栈上的主循环调 appAsyncStackReportTick() 打出来
//   I (…​) AsyncStack: file_mgr_start 栈余量 5236 / 8192 字节
// 每个 worker 只记**最坏那次**（余量最小），并且每 60s 整表重报一遍 —— 串口是流，谁
// 什么时候接上都能读齐，不必掐着点抓。审计完把这两处调用摘掉即可（app_async.cpp 里
// 对应实现一起删）。
void appAsyncTaskExit(const char *name, uint32_t stackBytes);
void appAsyncStackReportTick();
