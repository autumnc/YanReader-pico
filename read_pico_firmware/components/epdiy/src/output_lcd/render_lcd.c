#include <stdint.h>
#include <string.h>

#include "../output_common/render_method.h"

#ifdef RENDER_METHOD_LCD

#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_timer.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#if __has_include(<rom/cache.h>)
#include <rom/cache.h>
#elif __has_include(<esp32s3/rom/cache.h>)
#include <esp32s3/rom/cache.h>
#endif
#else
#include <rom/cache.h>
#endif

#include "../epd_internals.h"
#include "../output_common/line_queue.h"
#include "../output_common/lut.h"
#include "../output_common/render_context.h"
#include "epd_board.h"
#include "epdiy.h"
#include "lcd_driver.h"
#include "render_lcd.h"

// declare vector optimized line mask application.
void epd_apply_line_mask_VE(uint8_t* line, const uint8_t* mask, int mask_len);

__attribute__((optimize("O3"))) static bool IRAM_ATTR
retrieve_line_isr(RenderContext_t* ctx, uint8_t* buf) {
    if (ctx->lines_consumed >= ctx->lines_total) {
        return false;
    }
    int thread = ctx->line_threads[ctx->lines_consumed];
    if (thread >= NUM_RENDER_THREADS) {
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
        memset(buf, 0x00, ctx->display_width / 4);
        ctx->lines_consumed += 1;
        return pdFALSE;
    }

    LineQueue_t* lq = &ctx->line_queues[thread];

    BaseType_t awoken = pdFALSE;

    if (lq_read(lq, buf) != 0) {
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
        memset(buf, 0x00, ctx->display_width / 4);
    }

    if (ctx->lines_consumed >= ctx->display_height) {
        memset(buf, 0x00, ctx->display_width / 4);
    }
    ctx->lines_consumed += 1;
    return awoken;
}

/// start the next frame in the current update cycle
static void IRAM_ATTR handle_lcd_frame_done(RenderContext_t* ctx) {
    epd_lcd_frame_done_cb(NULL, NULL);
    epd_lcd_line_source_cb(NULL, NULL);

    // 扫描先于供数结束时解除生产者等待，禁止残留行进入下一相位。
    // Release producers if scan ends before consumption; never carry stale lines into the next phase.
    if (ctx->lines_consumed < ctx->lines_total) {
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
    }

    BaseType_t task_awoken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->frame_done, &task_awoken);

    portYIELD_FROM_ISR();
}

// frame_done 只有 LCD 帧结束 ISR（上面的 handle_lcd_frame_done）会给出。面板排线/
// DMA 一旦故障，这一帧就永远结束不了，ISR 不来，等待方**永久**停在 portMAX_DELAY 上。
// 关键在于它是**阻塞**不是自旋：两个核的 idle 任务照常喂狗，task WDT 永不复位 —— 症状
// 是整机画面冻死、只能拔电。给等待设上限：正常单相 ~11ms、整幅 GC16 36 相 ≈400ms，
// 2s 已是两个数量级余量。超时置欠载错误位并按现有恢复路径退出。
static bool lcd_wait_frame_done(RenderContext_t* ctx) {
    if (xSemaphoreTake(ctx->frame_done, pdMS_TO_TICKS(2000)) == pdTRUE) return true;
    // ISR 若姗姗来迟，别把这次的 give 留给下一帧（会让下一次等待立刻假返回）。
    xSemaphoreTake(ctx->frame_done, 0);
    ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
    ESP_LOGE("epdiy", "等待帧结束信号超时（LCD/DMA 故障？），中止本次刷新");
    return false;
}

void lcd_do_update(RenderContext_t* ctx) {
    epd_set_mode(1);

    // 本次扫描周期里被倒掉的残留行合计（见 prepare_context_for_next_frame 里的清理）。
    // 健康时是 0；非 0 就是"上一相的尾巴喂到了这一相"——那几行的影会以错的波形印在屏上。
    int stale_total = 0;

    for (uint8_t k = 0; k < ctx->cycle_frames; k++) {
        epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);
        prepare_context_for_next_frame(ctx);
        stale_total += ctx->stale_lines;

        // start both feeder tasks
        xTaskNotifyGive(ctx->feed_tasks[!xPortGetCoreID()]);
        xTaskNotifyGive(ctx->feed_tasks[xPortGetCoreID()]);

        // transmission is started in renderer threads, now wait util it's done
        if (!lcd_wait_frame_done(ctx)) {
            // 帧都没结束，喂数据线程大概率也收不了尾（DMA 不再取行 → 队列一直是满的，
            // 生产者挂在 give 上）。有上限地等一轮，再按欠载路径退出。
            for (int i = 0; i < NUM_RENDER_THREADS; i++) {
                xSemaphoreTake(ctx->feed_done_smphr[i], pdMS_TO_TICKS(500));
                xSemaphoreTake(ctx->feed_done_smphr[i], 0);   // 收尾期间迟到的 give 也清掉
            }
            break;
        }

        for (int i = 0; i < NUM_RENDER_THREADS; i++) {
            xSemaphoreTake(ctx->feed_done_smphr[i], portMAX_DELAY);
        }

        // DMA 回调和生产者均已停止后再清队列，由调用方执行欠载恢复。
        // Reset only after DMA callbacks and producers stop; let the caller recover from underrun.
        if (ctx->error) {
            for (int i = 0; i < NUM_RENDER_THREADS; i++) {
                lq_reset(&ctx->line_queues[i]);
            }
            break;
        }

        ctx->current_frame++;

        // make the watchdog happy.
        vTaskDelay(0);
    }

    epd_lcd_line_source_cb(NULL, NULL);
    epd_lcd_frame_done_cb(NULL, NULL);

    // 只在真的倒掉过东西时出声，并限速：一次扫描里有残留就可能连着几次都有，不限速会把
    // 串口刷满（而且这条日志要在推屏收尾时打，不能拖慢相位循环）。正常一次都不出现。
    if (stale_total > 0) {
        static int64_t s_last_log_us = 0;
        const int64_t now = esp_timer_get_time();
        if (now - s_last_log_us > 1000000) {
            s_last_log_us = now;
            ESP_LOGW(
                "epdiy",
                "相位残留行：本帧倒掉 %d 行（%d 相位，屏高 %d）",
                stale_total,
                (int)ctx->cycle_frames,
                ctx->display_height
            );
        }
    }

    epd_set_mode(0);
}

__attribute__((optimize("O3"))) static bool IRAM_ATTR
push_pixels_isr(RenderContext_t* ctx, uint8_t* buf) {
    // Output no-op outside of drawn area
    if (ctx->lines_consumed < ctx->area.y) {
        memset(buf, 0, ctx->display_width / 4);
    } else if (ctx->lines_consumed >= ctx->area.y + ctx->area.height) {
        memset(buf, 0, ctx->display_width / 4);
    } else {
        memcpy(buf, ctx->static_line_buffer, ctx->display_width / 4);
    }
    ctx->lines_consumed += 1;
    return pdFALSE;
}

/**
 * Populate the line mask for use in epd_push_pixels.
 */
static void push_pixels_populate_line(RenderContext_t* ctx, int color) {
    // Select fill pattern by draw color
    int fill_byte = 0;
    switch (color) {
        case 0:
            fill_byte = DARK_BYTE;
            break;
        case 1:
            fill_byte = CLEAR_BYTE;
            break;
        default:
            fill_byte = 0x00;
    }

    // Compute a line mask based on the drawn area
    uint8_t* dirtyness = malloc(ctx->display_width / 2);
    assert(dirtyness != NULL);

    memset(dirtyness, 0, ctx->display_width / 2);

    for (int i = 0; i < ctx->display_width; i++) {
        if ((i >= ctx->area.x) && (i < ctx->area.x + ctx->area.width)) {
            dirtyness[i / 2] |= i % 2 ? 0xF0 : 0x0F;
        }
    }
    epd_populate_line_mask(ctx->line_mask, dirtyness, ctx->display_width / 4);

    // mask the line pattern with the populated mask
    memset(ctx->static_line_buffer, fill_byte, ctx->display_width / 4);
    epd_apply_line_mask(ctx->static_line_buffer, ctx->line_mask, ctx->display_width / 4);

    free(dirtyness);
}

void epd_push_pixels_lcd(RenderContext_t* ctx, short time, int color) {
    ctx->current_frame = 0;
    ctx->lines_total = ctx->display_height;
    ctx->lines_consumed = 0;
    // 这一块是 LCD 外设逐行 DMA 读走的行源缓冲，必须留在内部 RAM：
    // 上层若把 SPIRAM_MALLOC_ALWAYSINTERNAL 调到 0（默认 malloc 优先落 PSRAM），
    // 普通 malloc 会让它落到 PSRAM。显式要内部内存，与改动前行为一致。
    ctx->static_line_buffer = heap_caps_malloc(ctx->display_width / 4, MALLOC_CAP_INTERNAL);
    assert(ctx->static_line_buffer != NULL);

    push_pixels_populate_line(ctx, color);
    epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);
    epd_lcd_line_source_cb((line_cb_func_t)&push_pixels_isr, ctx);

    epd_set_mode(1);
    epd_lcd_start_frame();
    // 同样加超时：这是清屏/推像素的路径，不该为一次硬件异常把调用方焊死在信号量上。
    // 这里不复用 lcd_wait_frame_done —— 它会置 ctx->error，而错误位会让后续 push_pixels
    // 直接早退（见本文件上面的 `if (ctx->error) return;`），副作用太大。
    if (xSemaphoreTake(ctx->frame_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        xSemaphoreTake(ctx->frame_done, 0);
        ESP_LOGE("epdiy", "push_pixels 等待帧结束信号超时（LCD/DMA 故障？）");
    }
    epd_set_mode(0);

    free(ctx->static_line_buffer);
    ctx->static_line_buffer = NULL;
}

#define int_min(a, b) (((a) < (b)) ? (a) : (b))

// 每帧开扫之前先算好多少行。这段预填是帧与帧之间的空档：行队列只要跑在 DMA 前面
// 就行，一行 16us 才消费一条，喂线程几微秒就算完一条，所以预填不需要很多；太少了
// 会在 ISR 延迟抖动时欠载（EPD_DRAW_EMPTY_LINE_QUEUE）。
static int s_prefill_lines = 127;

void epd_lcd_set_prefill_lines(int lines) {
    if (lines < 4) lines = 4;
    s_prefill_lines = lines;
}

int epd_lcd_prefill_lines(void) {
    return s_prefill_lines;
}
// 生产者等空槽的自旋上限；超过就按欠载处理（丢这一帧），不再无限自旋把整机锁死。
// Upper bound on a producer's wait for a free slot; past it we report underrun (drop the
// frame) instead of spinning forever and locking the whole device up.
#define FEED_SPIN_LIMIT 20000000u

__attribute__((optimize("O3"))) void IRAM_ATTR
lcd_calculate_frame(RenderContext_t* ctx, int thread_id) {
    assert(ctx->lut_lookup_func != NULL);
    uint8_t* input_line = ctx->feed_line_buffers[thread_id];

    LineQueue_t* lq = &ctx->line_queues[thread_id];
    int l = 0;

    // 另一生产者可能已启动并报告欠载；不得再次启动同一帧。
    // The other producer may have started and underrun already; never start the frame twice.
    if (ctx->error) return;

    // line must be able to hold 2-pixel-per-byte or 1-pixel-per-byte data
    memset(input_line, 0x00, ctx->display_width);

    EpdRect area = ctx->area;
    int min_y, max_y, bytes_per_line, _ppB;
    const uint8_t* ptr_start;
    get_buffer_params(ctx, &bytes_per_line, &ptr_start, &min_y, &max_y, &_ppB);

    assert(area.width == ctx->display_width && area.x == 0 && !ctx->error);

    // index of the line that triggers the frame output when processed
    int trigger_line = int_min(s_prefill_lines, max_y - min_y);

    // 扫描要到第 (min_y + trigger_line) 行入队才启动，而队列满了生产者只能原地等，
    // 所以预填行数必须小于两条队列加起来的可用槽位数。否则两个生产者在扫描启动前
    // 就把各自的队列填满，没人消费、也没人启动扫描 —— 双方死等。
    // 队列从 64 行缩到 32 行（READ_PICO_EPD_SMALL_FEED_QUEUE）后只剩 2*(32-1)=62 个
    // 槽位，而 display.c 沿用老的 64/127 预填，直接越界。
    // The scan starts only once line (min_y + trigger_line) is enqueued, and a full queue
    // blocks the producer — so prefill must stay under the combined usable slots of both
    // queues (2*(size-1)). With the feed queue shrunk to 32 lines there are only 62 slots,
    // while display.c still asks for the old 64/127 prefill.
    const int feed_capacity = 2 * (ctx->line_queues[0].size - 1);
    int max_trigger = feed_capacity - 4 - min_y;   // -4: 留出在飞行的行与调度抖动余量
    if (max_trigger < 1) max_trigger = 1;
    if (trigger_line > max_trigger) trigger_line = max_trigger;

    while (l = atomic_fetch_add(&ctx->lines_prepared, 1), l < ctx->lines_total) {
        ctx->line_threads[l] = thread_id;

        // queue is sufficiently filled to fill both bounce buffers, frame
        // can begin
        if (l - min_y == trigger_line) {
            epd_lcd_line_source_cb((line_cb_func_t)&retrieve_line_isr, ctx);
            epd_lcd_start_frame();
        }

        if (l < min_y || l >= max_y
            || (ctx->drawn_lines != NULL && !ctx->drawn_lines[l - area.y])) {
            uint8_t* buf = NULL;
            for (uint32_t spins = 0; buf == NULL; ) {
                // break in case of errors
                if (ctx->error & EPD_DRAW_EMPTY_LINE_QUEUE) {
                    lq_reset(lq);
                    return;
                };

                buf = lq_current(lq);
                // 兜底：宁可丢这一帧，也不要把整机锁死在这里。
                if (buf == NULL && ++spins > FEED_SPIN_LIMIT) {
                    ESP_LOGE(
                        "epdiy",
                        "feed spin stuck: prefill=%d queue=%d min_y=%d max_y=%d l=%d",
                        s_prefill_lines, lq->size, min_y, max_y, l
                    );
                    ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
                    lq_reset(lq);
                    return;
                }
            }
            memset(buf, 0x00, lq->element_size);
            lq_commit(lq);
            continue;
        }

        uint32_t* lp = (uint32_t*)input_line;
        const uint8_t* ptr = ptr_start + bytes_per_line * (l - min_y);

        Cache_Start_DCache_Preload((uint32_t)ptr, ctx->display_width, 0);

        lp = (uint32_t*)ptr;

        uint8_t* buf = NULL;
        for (uint32_t spins = 0; buf == NULL; ) {
            // break in case of errors
            if (ctx->error & EPD_DRAW_EMPTY_LINE_QUEUE) {
                lq_reset(lq);
                return;
            };

            buf = lq_current(lq);
            // 兜底：宁可丢这一帧，也不要把整机锁死在这里。
            if (buf == NULL && ++spins > FEED_SPIN_LIMIT) {
                ESP_LOGE(
                    "epdiy",
                    "feed spin stuck: prefill=%d queue=%d min_y=%d max_y=%d l=%d",
                    s_prefill_lines, lq->size, min_y, max_y, l
                );
                ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
                lq_reset(lq);
                return;
            }
        }

        // 每段选哪张相位 LUT：
        //   - 列条带优先（错相揭页按 x 切条带，每带相位不同，带内该动的动、带外保持）；
        //   - 否则看逐行相位（一行一个相位，整行只走一相）；
        //   - 都没有就退回整屏一张 conversion_lut，即原来的行为。
        // 负相位 = 保持：把输出缓冲先清 0（1ppB VE 编码里动作 0 就是不驱动），
        // 该段被"写满 0"就等于整段保持不动。
        if (ctx->col_band_n > 0 && ctx->phase_luts && ctx->col_band_x0 && ctx->col_band_x1 &&
            ctx->col_band_phase) {
            memset(buf, 0x00, ctx->display_width / 4);
            for (int band = 0; band < ctx->col_band_n; ++band) {
                int8_t phase = ctx->col_band_phase[band];
                if (phase < 0) continue;
                int x0 = ctx->col_band_x0[band];
                int x1 = ctx->col_band_x1[band];
                // 查表按 16 像素（一个 uint32）为单位，未对齐的边界直接跳过这一带。
                if (x0 < 0 || x1 > ctx->display_width || x0 >= x1 || (x0 & 15) || (x1 & 15)) {
                    continue;
                }
                ctx->lut_lookup_func(
                    (const uint32_t*)(ptr + x0),
                    buf + x0 / 4,
                    ctx->phase_luts[phase],
                    (uint32_t)(x1 - x0)
                );
            }
        } else if (ctx->line_phase && ctx->phase_luts && l < ctx->display_height) {
            const int8_t phase = ctx->line_phase[l];
            if (phase >= 0) {
                ctx->lut_lookup_func(lp, buf, ctx->phase_luts[phase], ctx->display_width);
            } else {
                // 负相位 = 这一行本次保持不驱动。
                memset(buf, 0x00, ctx->display_width / 4);
            }
        } else {
            ctx->lut_lookup_func(lp, buf, ctx->conversion_lut, ctx->display_width);
        }

        // apply the line mask
        epd_apply_line_mask_VE(buf, ctx->line_mask, ctx->display_width / 4);

        lq_commit(lq);
    }
}

#endif
