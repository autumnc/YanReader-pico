/*
 * pjournal-pico 字体设置桩：NVS 里存外置字体路径（与 read_pico 原语义一致）。
 */

#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "settings";

static char s_font_path[160];  // 与 TTF_FONT_PATH_MAX 对齐

const char* app_settings_font_path(void) {
    return s_font_path[0] ? s_font_path : NULL;
}

void app_settings_set_font_path(const char* path) {
    if (!path) {
        s_font_path[0] = '\0';
    } else {
        strncpy(s_font_path, path, sizeof(s_font_path) - 1);
        s_font_path[sizeof(s_font_path) - 1] = '\0';
    }

    nvs_handle_t h;
    if (nvs_open("pjfont", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "path", s_font_path[0] ? s_font_path : "");
        nvs_commit(h);
        nvs_close(h);
    }
}

void settings_init(void) {
    s_font_path[0] = '\0';
    nvs_handle_t h;
    if (nvs_open("pjfont", NVS_READWRITE, &h) == ESP_OK) {
        size_t len = sizeof(s_font_path);
        if (nvs_get_str(h, "path", s_font_path, &len) != ESP_OK) {
            s_font_path[0] = '\0';
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "nvs_open pjfont failed, using builtin font");
    }
}
