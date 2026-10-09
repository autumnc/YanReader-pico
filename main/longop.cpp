#include "longop.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "longop";

// 环大小与名字长度都是"够诊断就行"的量级：一次欠载前后能发生几十条重活顶天了，
// 名字只留够认出是哪个文件（前缀 /sdcard/ 之类的丢掉没关系）。
#define LONGOP_SLOTS 8
#define LONGOP_NAME 40
// 打印窗口：欠载的 E 行到 guard_draw_result 跑起来之间隔着整帧扫描（12 MHz 下一个整屏
// GC16 也才 404ms，实测 1.3s 那笔是 epd_clear 的账，已去掉），给 8s 足够宽。
#define LONGOP_WINDOW_US (8 * 1000 * 1000)

typedef struct {
    char what[LONGOP_NAME];
    int64_t start_us;
    int64_t end_us;  // 0 = 还没结束（正在进行中）
} LongOpRec;

static LongOpRec s_rec[LONGOP_SLOTS];
static int s_next;  // 下一个要写的槽

// 保护环的写入；临界区只做几次内存拷贝，微秒级，不会自己变成"重活"。
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

extern "C" {

void longop_begin(const char *what) {
    portENTER_CRITICAL(&s_mux);
    LongOpRec *r = &s_rec[s_next];
    s_next = (s_next + 1) % LONGOP_SLOTS;
    int n = 0;
    if (what != nullptr) {
        // 只留最后一段路径，日志短一半。
        const char *base = strrchr(what, '/');
        base = base ? base + 1 : what;
        while (base[n] != '\0' && n < LONGOP_NAME - 1) {
            r->what[n] = base[n];
            n++;
        }
    }
    r->what[n] = '\0';
    r->start_us = esp_timer_get_time();
    r->end_us = 0;
    portEXIT_CRITICAL(&s_mux);
}

void longop_end(void) {
    portENTER_CRITICAL(&s_mux);
    // 从最新往回找第一条还没结束的。跨任务嵌套时可能关错一条（诊断用，认了），
    // 但同任务嵌套（设置落盘 → safeWriteFile 这种）关的就是最里层那条，正确。
    for (int i = 0; i < LONGOP_SLOTS; i++) {
        int idx = (s_next - 1 - i + LONGOP_SLOTS * 2) % LONGOP_SLOTS;
        if (s_rec[idx].end_us == 0 && s_rec[idx].what[0] != '\0') {
            s_rec[idx].end_us = esp_timer_get_time();
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

void longop_dump(const char *why) {
    LongOpRec snap[LONGOP_SLOTS];
    int order[LONGOP_SLOTS];  // 按结束时间由近及远
    int n = 0;
    const int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_mux);
    memcpy(snap, s_rec, sizeof(snap));
    portEXIT_CRITICAL(&s_mux);

    for (int i = 0; i < LONGOP_SLOTS; i++) {
        int idx = (s_next - 1 - i + LONGOP_SLOTS * 2) % LONGOP_SLOTS;
        if (snap[idx].what[0] == '\0' || snap[idx].start_us == 0) continue;
        int64_t end = snap[idx].end_us ? snap[idx].end_us : now;
        if (now - end > LONGOP_WINDOW_US) continue;
        order[n++] = idx;
    }

    if (n == 0) {
        ESP_LOGW(TAG, "%s；最近 8s 内没有重活（堆遍历/原子写都没跑）", why);
        return;
    }

    char line[256];
    int w = 0;
    for (int i = 0; i < n && w < (int)sizeof(line) - 60; i++) {
        const LongOpRec *r = &snap[order[i]];
        const int64_t end = r->end_us ? r->end_us : now;
        const long dur_ms = (long)((end - r->start_us) / 1000);
        const long ago_ms = (long)((now - end) / 1000);
        w += snprintf(line + w, sizeof(line) - w, "%s%s %ldms(%s%ldms)",
                      i ? " | " : "", r->what, dur_ms,
                      r->end_us ? "结束于" : "进行中", ago_ms);
    }
    ESP_LOGW(TAG, "%s；最近的 重活: %s", why, line);
}

}  // extern "C"
