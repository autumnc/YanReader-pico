/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * SDMMC 探测、挂载、格式化。
 *
 * SDMMC probe, mount, and format.
 */

#include "read_pico_sd.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "read_pico_board.h"
#include "sdmmc_cmd.h"

#define SD_MOUNT_POINT "/sdcard"
#define SD_PIN_CLK GPIO_NUM_38
#define SD_PIN_CMD GPIO_NUM_42
#define SD_PIN_D0 GPIO_NUM_44

// 一次 unaligned/PSRAM 传输中，驱动经 DMA 缓冲分块搬运的块数（× 512B = 4KB）。
// / Blocks moved per DMA chunk for unaligned/PSRAM transfers (x 512B = 4KB).
#define SD_DMA_CHUNK_BLOCKS 8

static const char* TAG = "sd_card";
static sdmmc_card_t* card;

// 驱动复用型 DMA 中转缓冲。SDMMC 外设只认内部 RAM，且要求缓冲对齐；一旦调用方
// 给来 PSRAM 指针或非对齐缓冲，驱动就**临场** heap_caps_malloc 一块 bounce buffer
// 转抄。本项目内部 RAM 长期只剩 ~15KB（大量外设静态池 + epdiy 队列），厚书排版时
// 这块临时分配会失败：
//   sdmmc_cmd: allocate_dma_buf: not enough mem, err=0x101
// → sdmmc_write_blocks failed → 整页序列化失败 → Section 构建失败（BuildError::Io）
// → 从某一章起整本书都是白页（玄鵺小说集.epub 从 spine[2..6] 开始）。
// 挂载时一次性预分配、交给驱动长期复用，此后任何传输都不再需要临时分配；
// 顺带把本该 512B 一趟的 bounce 写提升到 4KB 一趟（原速率的 8 倍）。
//   A reusable DMA bounce buffer. The driver otherwise mallocs one per unaligned/
//   PSRAM transfer, which fails once the scarce internal RAM is momentarily gone.
static void* dma_bounce_buf = NULL;
static int probe_state;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool media_invalidated;
static read_pico_sd_info_t cached_info;

// 失效只影响快照，消费者释放句柄前不得卸载卡。/ Invalidation changes snapshots only; consumers must close handles before unmount.
static void observe_media_locked(bool present) {
    const bool inserted = present && !cached_info.present;
    if (!present && (cached_info.mounted || probe_state == 1)) media_invalidated = true;
    // 开机时没插卡：探测定格为"无卡"（probe_state=2），此后 read_pico_sd_start_probe()
    // 只会把缓存里的 NOT_FOUND 原样回放，永远不会再试挂载 —— "首次无卡开机、后插卡不认"
    // 就是这么来的。这里在观察到插入、状态机空闲、没有待处理的失效、也从未挂载时把
    // probe_state 归零，下一次 start_probe() 就会真的去挂一次。
    // 只在"从来没挂过"这条路上放行：拔卡会置 media_invalidated，那条路仍须消费者先关闭
    // 文件句柄再显式 remount，不能由这里替它做主（rpr 的写死前提也是这个）。
    // / Booted with no card: the probe latches "absent" (probe_state=2) and
    // start_probe() then just replays the cached NOT_FOUND forever — that is why
    // a card inserted after an empty boot was never picked up. Clear probe_state
    // when an insertion is seen while idle, not invalidated and never mounted, so
    // the next start_probe() actually tries to mount. Only the never-mounted path
    // is unlocked: removal sets media_invalidated, and that path still requires
    // consumers to close handles and remount explicitly.
    if (inserted && probe_state == 2 && !media_invalidated && !cached_info.mounted) {
        probe_state = 0;
        cached_info.error = ESP_ERR_INVALID_STATE;
        ESP_LOGI(TAG, "SD_CD inserted after empty boot, allowing re-probe");
    }
    cached_info.present = present;
    if (!present || media_invalidated) {
        memset(&cached_info, 0, sizeof(cached_info));
        cached_info.present = present;
        cached_info.error = present ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    }
}

static void publish_info(const read_pico_sd_info_t* info) {
    bool present = read_pico_sd_present();
    portENTER_CRITICAL(&state_lock);
    cached_info = *info;
    observe_media_locked(present);
    probe_state = 2;
    portEXIT_CRITICAL(&state_lock);
}

static void fill_info(read_pico_sd_info_t* info, esp_err_t mount_err) {
    memset(info, 0, sizeof(*info));
    info->present = read_pico_sd_present();
    info->error = mount_err;
    info->needs_format = info->present && !info->mounted
        && mount_err != ESP_OK
        && mount_err != ESP_ERR_TIMEOUT
        && mount_err != ESP_ERR_NOT_FOUND
        && mount_err != ESP_ERR_NOT_FINISHED;
    if (mount_err != ESP_OK || card == NULL) return;

    info->mounted = true;
    info->needs_format = false;
    strlcpy(info->name, card->cid.name, sizeof(info->name));
    info->capacity_bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
    uint64_t total_bytes = 0;
    info->error = esp_vfs_fat_info(
        SD_MOUNT_POINT, &total_bytes, &info->free_bytes
    );
}

static void ensure_font_dirs(void) {
    if (mkdir("/sdcard/assets", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir assets: %d", errno);
    }
    if (mkdir("/sdcard/assets/fonts", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir assets/fonts: %d", errno);
    }
    if (mkdir("/sdcard/fonts", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir fonts: %d", errno);
    }
}

static esp_err_t mount_card(bool format_if_failed) {
    if (card != NULL) return ESP_OK;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // 1-bit 只能靠提时钟换带宽。20MHz 默认对随机小读太慢，40MHz 多数卡能稳住。
    // / 1-bit only buys bandwidth by raising the clock. 20 MHz is too slow for
    // random small reads; 40 MHz holds on most cards.
    //
    // 但"多数卡"不是"所有卡"：有的卡在 40MHz 下能读到 CID、一读数据块就 CRC 错，
    // 有的卡上电后要更久才谈得拢时钟。所以**挂不上就降档重试**（40 → 20 → 10 MHz，
    // 每档之间留 200ms 让卡走完上电/复位流程）。10MHz 在 1-bit 上带宽只剩四分之一，
    // 但"这张卡能用"永远优先于"这张卡快" —— 一直挂在 40MHz 上失败，用户看到的是
    // "卡不认"，连书都打不开。
    // 降档重试能真的换档，靠的是 esp_vfs_fat_sdmmc_mount 失败时会 call_host_deinit
    // （vfs_fat_sdmmc.c 的 cleanup 分支）—— 下一次进来是全新的 host，max_freq_khz
    // 重新生效。同一档内重试是没意义的（同样的时钟、同样的结果）。
    // / Falling back through 40/20/10 MHz on mount failure. The fallback only works
    // because esp_vfs_fat_sdmmc_mount deinits the host on failure, so the next call
    // re-inits it with the new max_freq_khz.
    static const int kMountFreqKhz[] = {SDMMC_FREQ_HIGHSPEED, 20000, 10000};
    const int kMountTries = sizeof(kMountFreqKhz) / sizeof(kMountFreqKhz[0]);

    // 预分配 DMA 中转缓冲并交给驱动复用（见 dma_bounce_buf 注释）。挂载时内部 RAM
    // 尚充裕，这里一次分配成功后，后续所有非对齐/PSRAM 传输都不再临场 malloc。
    // 分配失败也不致命：驱动退回原来的"每次临时分配"，只是仍可能在内存紧张时失败。
    // / Pre-allocate the reusable DMA bounce buffer while internal RAM is still
    // plentiful. Failure is non-fatal: the driver falls back to per-transfer mallocs.
    if (dma_bounce_buf == NULL) {
        dma_bounce_buf = heap_caps_malloc(512 * SD_DMA_CHUNK_BLOCKS, MALLOC_CAP_DMA);
        if (dma_bounce_buf == NULL) {
            ESP_LOGW(TAG, "DMA bounce buffer alloc failed, using per-transfer mallocs");
        }
    }
    if (dma_bounce_buf != NULL) {
        host.unaligned_multi_block_rw_max_chunk_size = SD_DMA_CHUNK_BLOCKS;
        host.dma_aligned_buffer = dma_bounce_buf;
    }

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_PIN_CLK;
    slot.cmd = SD_PIN_CMD;
    slot.d0 = SD_PIN_D0;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.cd = GPIO_NUM_NC;
    slot.wp = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    // max_files：FATFS VFS 同时可打开的文件数（vfs_fat.c 的 files[] 槽位数）。
    // 8 对阅读器不够用：打开一本书时 Epub/Section 会长期占用若干句柄，微信读书
    // 缓存整本再叠加 images.work 索引 + 打包输出，槽位用满时 f_open 返回
    // FR_TOO_MANY_OPEN_FILES → VFS 报 errno=23(ENFILE)，界面看到的是"SD 卡读写
    // 失败"。槽位本身只占 sizeof(FIL)（动态缓冲是打开时才分配），放宽到 24 的成本
    // 可以忽略，代价是同时打开更多文件时堆占用更高。
    /// max_files: number of simultaneously open files in the FATFS VFS. 8 is too
    /// few once the reader holds epub/section handles and WeRead packaging runs.
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_failed,
        .max_files = 24,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < kMountTries; i++) {
        host.max_freq_khz = kMountFreqKhz[i];
        err = esp_vfs_fat_sdmmc_mount(
            SD_MOUNT_POINT, &host, &slot, &mount_config, &card
        );
        if (err == ESP_OK) {
            if (i > 0) {
                ESP_LOGW(
                    TAG, "Mounted at %d kHz after %d failed trie(s) at higher clocks",
                    kMountFreqKhz[i], i
                );
            }
            return err;
        }
        // 挂载失败时驱动已经把 host 拆干净了（见上面 call_host_deinit 的说明），
        // card 指针也必须自己置回 NULL —— 它这时指向一块已经被 free 的 sdmmc_card_t。
        card = NULL;
        ESP_LOGW(TAG, "Mount at %d kHz failed: %s", kMountFreqKhz[i], esp_err_to_name(err));
        // 卡根本不在：降档也不会有卡，别再多花 400ms。
        // / No card at all: falling back cannot help.
        if (err == ESP_ERR_NOT_FOUND) break;
        if (i + 1 < kMountTries) vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGW(TAG, "Mount failed on all clocks");
    return err;
}

static void close_card(void) {
    if (card != NULL) {
        esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "unmount %s", esp_err_to_name(err));
        }
        card = NULL;
    }
}

static void probe_task(void* arg) {
    (void)arg;
    read_pico_sd_info_t info = { 0 };
    info.present = read_pico_sd_present();
    if (!info.present) {
        info.error = ESP_ERR_NOT_FOUND;
        publish_info(&info);
        ESP_LOGI(TAG, "SD_CD absent, skip mount");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = mount_card(false);
    fill_info(&info, err);
    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Mounted %s, capacity=%llu MB, free=%llu MB",
            info.name,
            (unsigned long long)(info.capacity_bytes / (1024 * 1024)),
            (unsigned long long)(info.free_bytes / (1024 * 1024))
        );
    }
    publish_info(&info);
    if (info.needs_format) {
        ESP_LOGW(TAG, "SD present but no FAT, ask user to format");
    }
    vTaskDelete(NULL);
}

esp_err_t read_pico_sd_start_probe(void) {
    bool present = read_pico_sd_present();
    portENTER_CRITICAL(&state_lock);
    observe_media_locked(present);
    esp_err_t err = ESP_OK;
    if (probe_state == 1) err = ESP_ERR_NOT_FINISHED;
    else if (media_invalidated) err = cached_info.error;
    else if (probe_state == 2) err = cached_info.mounted ? ESP_OK :
        cached_info.error != ESP_OK ? cached_info.error : ESP_FAIL;
    else if (!present) {
        probe_state = 2;
        err = ESP_ERR_NOT_FOUND;
    } else {
        memset(&cached_info, 0, sizeof(cached_info));
        cached_info.present = true;
        cached_info.error = ESP_ERR_NOT_FINISHED;
        probe_state = 1;
    }
    bool start = err == ESP_OK && probe_state == 1;
    portEXIT_CRITICAL(&state_lock);
    if (!start) return err;
    BaseType_t created = xTaskCreate(
        probe_task, "sd_probe", 4096, NULL, 5, NULL
    );
    if (created != pdPASS) {
        portENTER_CRITICAL(&state_lock);
        probe_state = 0;
        cached_info.error = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&state_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_ERR_NOT_FINISHED;
}

esp_err_t read_pico_sd_get_info(read_pico_sd_info_t* info) {
    if (info == NULL) return ESP_ERR_INVALID_ARG;
    bool present = read_pico_sd_present();
    portENTER_CRITICAL(&state_lock);
    observe_media_locked(present);
    *info = cached_info;
    esp_err_t err = !present || media_invalidated ? info->error :
        probe_state == 0 ? ESP_ERR_INVALID_STATE :
        probe_state == 1 ? ESP_ERR_NOT_FINISHED : info->error;
    portEXIT_CRITICAL(&state_lock);
    return err;
}

// 驱动操作期间占用忙状态，不在临界区执行 I/O。/ Reserve busy state across driver I/O outside the critical section.
static bool begin_operation(void) {
    portENTER_CRITICAL(&state_lock);
    bool ready = probe_state != 1;
    if (ready) probe_state = 1;
    portEXIT_CRITICAL(&state_lock);
    return ready;
}

esp_err_t read_pico_sd_remount(void) {
    if (!begin_operation()) return ESP_ERR_NOT_FINISHED;
    close_card();
    portENTER_CRITICAL(&state_lock);
    memset(&cached_info, 0, sizeof(cached_info));
    media_invalidated = false;
    probe_state = 0;
    portEXIT_CRITICAL(&state_lock);
    return read_pico_sd_start_probe();
}

esp_err_t read_pico_sd_sync(void) {
    if (!begin_operation()) return ESP_ERR_NOT_FINISHED;
    close_card();
    read_pico_sd_info_t info = { .error = ESP_ERR_INVALID_STATE };
    publish_info(&info);
    return ESP_OK;
}

esp_err_t read_pico_sd_format(void) {
    read_pico_sd_info_t current;
    (void)read_pico_sd_get_info(&current);
    portENTER_CRITICAL(&state_lock);
    bool invalid = media_invalidated;
    bool busy = probe_state == 1;
    if (!invalid && !busy) probe_state = 1;
    portEXIT_CRITICAL(&state_lock);
    if (invalid) return current.present ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    if (busy) return ESP_ERR_NOT_FINISHED;
    if (!read_pico_sd_present()) {
        read_pico_sd_info_t info = { .error = ESP_ERR_NOT_FOUND };
        publish_info(&info);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = ESP_OK;
    if (card == NULL) {
        err = mount_card(true);
    } else {
        err = esp_vfs_fat_sdcard_format(SD_MOUNT_POINT, card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "format %s", esp_err_to_name(err));
        }
    }

    read_pico_sd_info_t info = { 0 };
    fill_info(&info, err);
    if (err == ESP_OK) {
        ensure_font_dirs();
        fill_info(&info, ESP_OK);
        ESP_LOGI(TAG, "formatted %s", info.name);
    }
    publish_info(&info);
    return err;
}
