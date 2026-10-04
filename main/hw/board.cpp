#include "board.h"
#include "board_hw.h"

#include <cstring>
#include <esp_log.h>

#include "epdiy.h"
#include "read_pico_pmu.h"
#include "u8g2_shim.h"
#include "ui_render.h"   // ui_render_drain：切旋转前等 core1 的推屏收尾
#include <HalDisplay.h>  // board_set_night：翻全局反色标志

static const char *TAG = "Board";

read_pico_handle_t g_hw;
u8g2_t *g_u8g2 = nullptr;

static u8g2_struct s_u8g2;

// 最近一次通过 board_apply_orientation 应用的方向（阅读模式强制横屏后据此恢复）。
static char s_orientation[16] = "landscape";

bool board_init() {
    if (read_pico_init(&g_hw) != ESP_OK) {
        ESP_LOGE(TAG, "read_pico_init failed");
        return false;
    }

    // 开机上电 + 清屏 + 前缓冲铺白（对齐 read_pico app_main 的顺序）。
    epd_poweron();
    epd_clear();
    epd_hl_set_all_white(&g_hw.hl);

    // 出厂标定的 VCOM（PMU 存有则覆盖板级默认，改善灰阶/残影）。
    int vcom_mv = 0;
    if (g_hw.pmu_ready && read_pico_pmu_vcom_get(&vcom_mv) == ESP_OK) {
        epd_set_vcom((uint16_t)vcom_mv);
        ESP_LOGI(TAG, "panel VCOM loaded from PMU: %d mV", vcom_mv);
    }

    // u8g2 shim 句柄绑定 epdiy framebuffer。
    s_u8g2.fb = g_hw.framebuffer;
    s_u8g2.color = 0;
    s_u8g2.bitmap_mode = 0;
    g_u8g2 = &s_u8g2;

    ESP_LOGI(TAG, "board ready, fb=%p (%d×%d physical)",
             (void *)g_hw.framebuffer, epd_width(), epd_height());
    return true;
}

EpdiyHighlevelState *board_hl() {
    return &g_hw.hl;
}

void board_set_night(bool on) {
    // 全局标志在 HalDisplay 里（每个推屏出口统一取反一次），所以这里只翻标志。
    display.setInverted(on);
}

void board_apply_orientation(const char *orientation) {
    // 面板物理 1216×684：横屏是自然方向，竖屏用官方固件的 INVERTED_PORTRAIT。
    const char *name = (orientation && orientation[0]) ? orientation : "landscape";
    enum EpdRotation rot = EPD_ROT_LANDSCAPE;
    if (strcmp(name, "portrait") == 0) {
        rot = EPD_ROT_INVERTED_PORTRAIT;
    }
    // 推屏任务在 core1，它算差分矩形和推屏用的是同一个旋转值。切换旋转前必须等它
    // 闲下来，否则「按旧旋转算出的矩形、按新旋转送进驱动」会刷错地方。切换之后
    // 调用方还会 ui_invalidate_snapshot()，下一帧整屏 GC16 会把画面拉正。
    ui_render_drain();
    // 记录最近应用的方向，供阅读模式退出后恢复。
    strncpy(s_orientation, name, sizeof(s_orientation) - 1);
    s_orientation[sizeof(s_orientation) - 1] = '\0';
    epd_set_rotation(rot);
    ESP_LOGI(TAG, "orientation=%s → logical %d×%d",
             name, epd_rotated_display_width(), epd_rotated_display_height());
}

// 阅读模式强制横屏：screen_reader.cpp 不能 include epdiy.h（EpdFont 符号与
// crossmux 冲突），旋转切换集中在这里，经 board_reader.h 暴露最小原型。
void board_force_landscape() {
    epd_set_rotation(EPD_ROT_LANDSCAPE);
    ESP_LOGI(TAG, "orientation=landscape (reader) → logical %d×%d",
             epd_rotated_display_width(), epd_rotated_display_height());
}

void board_force_portrait() {
    epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);
    ESP_LOGI(TAG, "orientation=portrait (reader) → logical %d×%d",
             epd_rotated_display_width(), epd_rotated_display_height());
}

void board_restore_orientation() {
    board_apply_orientation(s_orientation);
}
