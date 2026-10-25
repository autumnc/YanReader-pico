#include "app_async.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/idf_additions.h>

void AppAsyncJob::ensureMutex() {
    if (!mutex_) mutex_ = xSemaphoreCreateMutex();
}

void AppAsyncJob::lock() {
    ensureMutex();
    if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
}

void AppAsyncJob::unlock() {
    if (mutex_) xSemaphoreGive(mutex_);
}

void AppAsyncJob::begin(const std::string &title, const std::string &busy) {
    cancel_.store(false, std::memory_order_release);
    lock();
    title_ = title;
    busy_ = busy;
    result_.clear();
    unlock();
    drawn = false;
    untilUs = 0;
    state_.store(AppAsyncState::Running, std::memory_order_release);
}

bool AppAsyncJob::start(TaskFunction_t fn, const char *taskName, uint32_t stackBytes, void *arg,
                        UBaseType_t priority, bool psramStack) {
    TaskHandle_t h = nullptr;
    BaseType_t ok = pdFAIL;
    if (capsTask_) reapCapsTask();   // 上一次还没收尸：先收掉再起新的
    if (psramStack) {
        ok = xTaskCreateWithCaps(fn, taskName, stackBytes, arg, priority, &h,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        // 外部栈任务不得在禁用 cache 期间跑：调用方只做 SD/网络，不碰 flash。
        if (ok == pdPASS) capsTask_ = h;
    }
    if (ok != pdPASS) ok = xTaskCreate(fn, taskName, stackBytes, arg, priority, &h);
    if (ok != pdPASS) {
        // 任务建不起来**几乎只有一种原因：内部 RAM 里找不到这么长的连续块**（xTaskCreate 的栈
        // 只能从内部 RAM 出）。把当时的余量和最大块一起打出来，免得事后只能对着界面上那句
        // "任务启动失败"猜。最常见的现场是 WiFi 开着 —— 那时最大连续块只有 8192 上下。
        ESP_LOGW("AsyncJob", "%s 任务创建失败（栈 %u）：内部RAM余 %u，最大块 %u", taskName,
                 (unsigned)stackBytes, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    return true;
}

void AppAsyncJob::reapCapsTask() {
    TaskHandle_t h = capsTask_;
    capsTask_ = nullptr;
    if (!h) return;
    if (state_.load(std::memory_order_acquire) == AppAsyncState::Running) {
        // 绝不能在任务还在跑的时候删它（它可能正持有 s_mutex / 结果串）。
        capsTask_ = h;
        return;
    }
    vTaskDeleteWithCaps(h);
}

void AppAsyncJob::finish(const std::string &result) {
    lock();
    result_ = result;
    unlock();
    state_.store(AppAsyncState::Done, std::memory_order_release);
}

void AppAsyncJob::failToStart(const std::string &result) {
    finish(result);
}

void AppAsyncJob::cancel() {
    cancel_.store(true, std::memory_order_release);
}

bool AppAsyncJob::cancelled() const {
    return cancel_.load(std::memory_order_acquire);
}

void AppAsyncJob::reset() {
    cancel_.store(false, std::memory_order_release);
    lock();
    result_.clear();
    unlock();
    drawn = false;
    untilUs = 0;
    state_.store(AppAsyncState::Idle, std::memory_order_release);
}

AppAsyncState AppAsyncJob::state() const {
    return state_.load(std::memory_order_acquire);
}

std::string AppAsyncJob::title() {
    lock();
    std::string s = title_;
    unlock();
    return s;
}

std::string AppAsyncJob::busy() {
    lock();
    std::string s = busy_;
    unlock();
    return s;
}

std::string AppAsyncJob::result() {
    lock();
    std::string s = result_;
    unlock();
    return s;
}
