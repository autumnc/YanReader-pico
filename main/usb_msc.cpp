/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * U 盘模式：把 SD 卡整卡经 TinyUSB MSC 暴露给电脑，设备侧不再访问 SD。
 *
 * U-disk mode: expose the whole SD card to the host over TinyUSB MSC; the device
 * stops touching the SD while the host owns it.
 */

#include "usb_msc.h"

#include <esp_log.h>
#include <stdlib.h>

#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw/input.h"
#include "read_pico_sd.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

static const char *TAG = "usb_msc";

// 主机对介质的占用状态。esp_tinyusb 已经强实现 start_stop_cb（弹出时把卡挂回 APP），
// 所以不能 override 它；但 PREVENT/ALLOW（Windows 挂载/安全删除）和 START STOP UNIT
// 两个命令都落到 tud_msc_scsi_complete_cb 的 default 分支，override 这一个弱回调即可
// 同时拿到两路信号：
//   - Windows 挂载/占用时 prohibit_removal=1，安全删除时=0；
//   - macOS/Linux 弹出时 START STOP UNIT(load_eject=1, start=0)。
// 只有主机明确"放开"介质后，本机按键退出才被允许。
static volatile bool s_host_hold = false;

extern "C" void tud_msc_scsi_complete_cb(uint8_t lun, uint8_t const scsi_cmd[16]) {
  (void)lun;
  switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL: {
      const scsi_prevent_allow_medium_removal_t *cmd =
          (const scsi_prevent_allow_medium_removal_t *)scsi_cmd;
      s_host_hold = (cmd->prohibit_removal != 0);
      break;
    }
    case SCSI_CMD_START_STOP_UNIT: {
      const scsi_start_stop_unit_t *cmd = (const scsi_start_stop_unit_t *)scsi_cmd;
      if (cmd->load_eject && !cmd->start) s_host_hold = false;  // 主机弹出介质
      break;
    }
    default:
      break;
  }
}

// SD 卡引脚与 read_pico_sd.c 一致：CLK=38, CMD=42, D0=44, 1-bit。
#define SD_PIN_CLK GPIO_NUM_38
#define SD_PIN_CMD GPIO_NUM_42
#define SD_PIN_D0  GPIO_NUM_44

// MSC 存储实例的 base_path：本轮 mount_point=USB 只做扇区级暴露、从不挂到 APP，
// 所以这个路径不会被用到，写上只为和 read_pico_sd.c 的 /sdcard 约定对齐。
// 注意 base_path 是 char*（非 const），必须给可写缓冲，否则 -Wwrite-strings 报错。
static char s_msc_base_path[] = "/sdcard";

// 裸卡初始化（不挂 FATFS）：把 read_pico_sd_sync() 卸载后空出来的 SDMMC 外设重新
// 接管，产出一张给 TinyUSB MSC 用的 sdmmc_card_t。电气参数与 read_pico_sd.c 的
// mount_card 对齐（1-bit + 40MHz 高速），保证同一张卡用同样的时序。
static esp_err_t storage_init_sdmmc(sdmmc_card_t **out_card) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

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

    sdmmc_card_t *card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
    if (card == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = host.init();
    if (ret != ESP_OK) {
        free(card);
        return ret;
    }
    ret = sdmmc_host_init_slot(host.slot, &slot);
    if (ret != ESP_OK) {
        host.deinit_p(host.slot);
        free(card);
        return ret;
    }

    // 上电后首次时钟协商偶尔超时。这里不无限重试（无卡时会把整机按死在 U 盘页），
    // 给 ~3s 窗口，仍失败就按"无卡/卡坏"回滚。
    bool ok = false;
    for (int i = 0; i < 30; i++) {
        if (sdmmc_card_init(&host, card) == ESP_OK) {
            ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!ok) {
        host.deinit_p(host.slot);
        free(card);
        return ESP_ERR_NOT_FOUND;
    }

    *out_card = card;
    return ESP_OK;
}

// 裸卡回滚：反解 sdmmc_card_init 建立的 host 与 slot，并释放卡结构。TinyUSB 的
// storage_sdmmc_close 只把内部指针置空、不碰 host/卡内存，所以这里要自己收。
static void storage_teardown_sdmmc(sdmmc_card_t **out_card) {
    if (*out_card == NULL) return;
    sdmmc_host_t *host = &(*out_card)->host;
    if (host->flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
        host->deinit_p(host->slot);
    } else if (host->deinit) {
        host->deinit();
    }
    free(*out_card);
    *out_card = NULL;
}

// 轮询等 read_pico_sd_remount() 的异步探测把卡挂回 /sdcard（上限 ~3s）。
static void wait_sd_remounted(void) {
    read_pico_sd_info_t info = {};
    for (int i = 0; i < 60; i++) {
        esp_err_t e = read_pico_sd_get_info(&info);
        if (info.mounted) break;
        if (e != ESP_ERR_NOT_FINISHED) break;  // 探测已结束（无卡/挂载失败）
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t usb_msc_run(usb_msc_should_exit_cb_t should_exit, usb_msc_blocked_cb_t on_blocked) {
    // 1. 应用侧卸载（把 FATFS 句柄与 host 一并释放）。
    esp_err_t sync_err = read_pico_sd_sync();
    if (sync_err != ESP_OK && sync_err != ESP_ERR_NOT_FINISHED &&
        sync_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "read_pico_sd_sync: %s", esp_err_to_name(sync_err));
    }

    // 2. 重新接管 SDMMC 外设，裸卡就绪（不挂 FATFS）。
    sdmmc_card_t *card = NULL;
    esp_err_t err = storage_init_sdmmc(&card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SDMMC re-init failed: %s", esp_err_to_name(err));
        read_pico_sd_remount();
        wait_sd_remounted();
        return err;
    }

    // 3. TinyUSB MSC 存储实例 + USB 设备栈。mount_point=USB：只做扇区级暴露，
    //    设备侧不挂 FATFS，所以电脑端独占卡、本机绝不并发读写。
    tinyusb_msc_storage_handle_t handle = NULL;
    tinyusb_msc_storage_config_t storage_cfg = {};
    storage_cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    storage_cfg.medium.card = card;
    storage_cfg.fat_fs.base_path = s_msc_base_path;
    storage_cfg.fat_fs.config.max_files = 5;
    storage_cfg.fat_fs.config.format_if_mount_failed = false;
    storage_cfg.fat_fs.do_not_format = true;
    storage_cfg.fat_fs.format_flags = 0;

    err = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MSC storage create failed: %s", esp_err_to_name(err));
        storage_teardown_sdmmc(&card);
        read_pico_sd_remount();
        wait_sd_remounted();
        return err;
    }

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb install failed: %s", esp_err_to_name(err));
        tinyusb_msc_delete_storage(handle);
        storage_teardown_sdmmc(&card);
        read_pico_sd_remount();
        wait_sd_remounted();
        return err;
    }

    ESP_LOGI(TAG, "U-disk mode active, tap the exit button to eject");

    // 4. 阻塞直到用户触发"退出"（should_exit 判定，通常是点中屏幕上的退出按钮）。
    //    主机仍占用介质（未安全弹出）时拒绝退出：回调 on_blocked 让上层刷新警告；
    //    只有主机放开（s_host_hold=false）或已断开（tud_mounted()=false，例如拔线/
    //    主机停机）才真正退出，避免撕下 MSC 时写坏卡。
    while (true) {
        int k = input_poll();
        if (k == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!should_exit(k)) continue;          // 非退出动作（没点中按钮等）→ 忽略
        if (tud_mounted() && s_host_hold) {
            ESP_LOGW(TAG, "host still holds medium, eject on PC first");
            if (on_blocked) on_blocked();
            continue;
        }
        break;
    }

    // 5. 逆序卸载：先删存储（关介质/卸 MSC 驱动），再关 USB 栈，最后收裸卡。
    tinyusb_msc_delete_storage(handle);
    tinyusb_driver_uninstall();
    storage_teardown_sdmmc(&card);

    // 6. 重挂并等就绪。
    read_pico_sd_remount();
    wait_sd_remounted();
    return ESP_OK;
}
