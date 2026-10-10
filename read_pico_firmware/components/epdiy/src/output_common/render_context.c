#include "render_context.h"

#include <string.h>
#include "esp_log.h"

#include "../epdiy.h"
#include "lut.h"
#include "render_method.h"

/// For waveforms without per-phase timing, the default hold time for each line is 12us
const static int DEFAULT_FRAME_TIME = 120;

static inline int min(int x, int y) {
    return x < y ? x : y;
}

void get_buffer_params(
    RenderContext_t* ctx,
    int* bytes_per_line,
    const uint8_t** start_ptr,
    int* min_y,
    int* max_y,
    int* pixels_per_byte
) {
    EpdRect area = ctx->area;

    enum EpdDrawMode mode = ctx->mode;
    const EpdRect crop_to = ctx->crop_to;
    const bool horizontally_cropped = !(crop_to.x == 0 && crop_to.width == area.width);
    const bool vertically_cropped = !(crop_to.y == 0 && crop_to.height == area.height);

    // number of pixels per byte of input data
    int width_divider = 0;

    if (mode & MODE_PACKING_1PPB_DIFFERENCE) {
        *bytes_per_line = area.width;
        width_divider = 1;
    } else if (mode & MODE_PACKING_2PPB) {
        *bytes_per_line = area.width / 2 + area.width % 2;
        width_divider = 2;
    } else if (mode & MODE_PACKING_8PPB) {
        *bytes_per_line = (area.width / 8 + (area.width % 8 > 0));
        width_divider = 8;
    } else {
        ctx->error |= EPD_DRAW_INVALID_PACKING_MODE;
    }

    int crop_x = (horizontally_cropped ? crop_to.x : 0);
    int crop_y = (vertically_cropped ? crop_to.y : 0);
    int crop_h = (vertically_cropped ? crop_to.height : 0);

    const uint8_t* ptr_start = ctx->data_ptr;

    // Adjust for negative starting coordinates with optional crop
    if (area.x - crop_x < 0) {
        ptr_start += -(area.x - crop_x) / width_divider;
    }

    if (area.y - crop_y < 0) {
        ptr_start += -(area.y - crop_y) * *bytes_per_line;
    }

    // calculate start and end row with crop
    *min_y = area.y + crop_y;
    *max_y = min(*min_y + (vertically_cropped ? crop_h : area.height), area.height);
    *start_ptr = ptr_start;
    *pixels_per_byte = width_divider;
}

void IRAM_ATTR prepare_context_for_next_frame(RenderContext_t* ctx) {
    int frame_time = DEFAULT_FRAME_TIME;
    if (ctx->phase_times != NULL) {
        frame_time = ctx->phase_times[ctx->current_frame];
    }

    if (ctx->mode & MODE_EPDIY_MONOCHROME) {
        frame_time = MONOCHROME_FRAME_TIME;
    }
    ctx->frame_time = frame_time;

    const EpdWaveformPhases* phases
        = ctx->waveform->mode_data[ctx->waveform_index]->range_data[ctx->waveform_range];

    assert(ctx->lut_build_func != NULL);
    ctx->lut_build_func(ctx->conversion_lut, phases, ctx->current_frame);

    // 相位开始前倒掉行队列里的残留。
    // 生产者的最后一次 commit 有可能落在扫描收尾之后：这一行 DMA 再也不会取走，却留在队列
    // 里。下一个相位从零开始读，就会先读到它 —— 而它的像素是按**上一相位的 conversion_lut**
    // 查出来的，等于用错的波形驱动了开头几行。表现为"沿扫描方向平移的浅影"（全刷也清不掉），
    // 只能靠重建扫描路径或重启。清掉即可，不必重构整条流水线。
    //
    // 安全性：调用点就在相位循环开头（lcd_do_update），此时上一相位的帧结束 ISR 早已跑完
    // （frame_done 已被取走），两个喂数据线程也都 give 过 feed_done —— 生产者与消费者都没人
    // 碰这两个队列。lq_reset 本身**不是**原子操作，正是靠这个空窗才是安全的。
    // Drain the line queues at the start of a phase. A producer's last commit can land after the
    // scan finished: that line is never fetched by the DMA but stays queued, and the next phase
    // reads it first — encoded with the *previous* phase's LUT, i.e. driven with the wrong
    // waveform (a ghost shifted along the scan, which a full B/W refresh cannot clear). Safe
    // here because the frame-done ISR has fired and both feeders signalled done, so nobody else
    // touches the queues; lq_reset is not atomic and relies on that quiet window.
    ctx->stale_lines = 0;
    for (int i = 0; i < NUM_RENDER_THREADS; i++) {
        LineQueue_t* q = &ctx->line_queues[i];
        if (q->size <= 0) continue;
        const int pending
            = (atomic_load_explicit(&q->current, memory_order_acquire)
               - atomic_load_explicit(&q->last, memory_order_acquire) + q->size)
              % q->size;
        if (pending > 0) {
            ctx->stale_lines += pending;
            lq_reset(q);
        }
    }

    ctx->lines_prepared = 0;
    ctx->lines_consumed = 0;
    if (ctx->line_threads != NULL) {
        for (int i = 0; i < ctx->lines_total; i++) {
            ctx->line_threads[i] = i % NUM_RENDER_THREADS;
        }
    }
}

void epd_populate_line_mask(uint8_t* line_mask, const uint8_t* dirty_columns, int mask_len) {
    if (dirty_columns == NULL) {
        memset(line_mask, 0xFF, mask_len);
    } else {
        int pixels = mask_len * 4;
        for (int c = 0; c < pixels / 2; c += 2) {
            uint8_t mask = 0;
            mask |= (dirty_columns[c + 1] & 0xF0) != 0 ? 0xC0 : 0x00;
            mask |= (dirty_columns[c + 1] & 0x0F) != 0 ? 0x30 : 0x00;
            mask |= (dirty_columns[c] & 0xF0) != 0 ? 0x0C : 0x00;
            mask |= (dirty_columns[c] & 0x0F) != 0 ? 0x03 : 0x00;
            line_mask[c / 2] = mask;
        }
    }
}