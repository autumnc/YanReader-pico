#include "app_async.h"

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
                        UBaseType_t priority) {
    TaskHandle_t h = nullptr;
    if (xTaskCreate(fn, taskName, stackBytes, arg, priority, &h) != pdPASS) return false;
    return true;
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

void AppAsyncJob::reset() {
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
