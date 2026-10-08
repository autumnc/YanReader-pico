#include "app_async.h"

#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <mutex>

// 待打印的栈水线（见头文件里的说明：worker 只登记，主循环负责打印）。
// 定长数组 + 互斥量：worker 退出这件事本身就可能在两条 UI 路径上同时发生，
// 没有锁的话两个任务会抢同一个槽、把彼此的 name 和数字拼在一起打出去 —— 水线
// 是用来**归因**的，打了个张冠李戴的反而害事。槽满了就丢（诊断数据，丢一条无妨）。
//
// **每个 worker 只留一个槽、记最坏那次**：这些 worker 都是"跑一次报一次"，同一本书
// 进两次文件浏览就是两趟；水线只会被更坏的一趟刷新，留最小值才有意义。
namespace {
struct StackReport {
    const char *name;
    uint32_t stackBytes;
    uint32_t left;
};
// 16：水线是按**任务名**归并的，所以同一个 worker 名下可能是好几条差异极大的路径
// （设置那 8192 就同时装着同步时间/NTP、Flomo 登录/TLS、备份、恢复四种）。名字必须
// 一条路径一个 —— 合并着记出来的最小值会张冠李戴，按它砍栈正好砍在最深的那条上。
constexpr int kStackReportSlots = 16;
StackReport s_stackReports[kStackReportSlots];
int s_stackReportCount = 0;      // 已登记条数
int s_stackReportDrained = 0;    // 已"首次"打印到第几条
int64_t s_stackReportRepeatUs = 0;  // 上一次整表重报的时刻
std::mutex s_stackReportMutex;
constexpr int64_t kStackReportRepeatUs = 60000000;  // 60s 重报一次
}  // namespace

void appAsyncTaskExit(const char *name, uint32_t stackBytes) {
    // uxTaskGetStackHighWaterMark 在 IDF 里返回**字节**（不是字）。这里是这个任务整个生命
    // 周期里"最少时剩多少"，正是砍栈要的那个数。
    const uint32_t left = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    {
        // 锁必须在 vTaskDelete 之前放开：任务带着互斥量自杀，那把锁就再也回不来了
        // （从此一行水线都打不出，而且谁都查不出为什么）。所以下面一律用块把临界区圈死，
        // 绝不用会提前 return 的写法。
        std::lock_guard<std::mutex> lock(s_stackReportMutex);
        int slot = -1;
        for (int i = 0; i < s_stackReportCount; i++) {
            if (strcmp(s_stackReports[i].name, name) == 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            if (s_stackReportCount < kStackReportSlots) {
                slot = s_stackReportCount++;
                s_stackReports[slot] = StackReport{name, stackBytes, left};
            }
        } else if (left < s_stackReports[slot].left) {
            s_stackReports[slot].left = left;   // 同一个 worker 再来一趟：留余量最小的
        }
    }
    vTaskDelete(nullptr);
}

void appAsyncStackReportTick() {
    std::lock_guard<std::mutex> lock(s_stackReportMutex);
    // 新登记的立刻报，现场盯着看的人不用等。
    while (s_stackReportDrained < s_stackReportCount) {
        const StackReport &r = s_stackReports[s_stackReportDrained++];
        ESP_LOGI("AsyncStack", "%s 栈余量 %u / %u 字节", r.name, r.left, r.stackBytes);
    }
    // 再每 60s 把整表重报一遍。**这条是给"事后才接上串口"留的**：串口是流，抓晚了那几行
    // 就永远没了，而这些 worker 又非得用户动手才跑一次 —— 有这一遍，什么时候接上都读得齐。
    const int64_t now = esp_timer_get_time();
    if (s_stackReportCount > 0 && now - s_stackReportRepeatUs >= kStackReportRepeatUs) {
        s_stackReportRepeatUs = now;
        for (int i = 0; i < s_stackReportCount; i++) {
            const StackReport &r = s_stackReports[i];
            ESP_LOGI("AsyncStack", "(重报) %s 栈余量 %u / %u 字节", r.name, r.left, r.stackBytes);
        }
    }
}

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
                        UBaseType_t priority) {
    TaskHandle_t h = nullptr;
    if (xTaskCreate(fn, taskName, stackBytes, arg, priority, &h) != pdPASS) {
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
