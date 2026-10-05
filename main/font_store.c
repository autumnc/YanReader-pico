/*
 * 外置字体路径的持久化（NVS 命名空间 "pjfont"，与 read_pico 原语义一致）。
 * 名字从 settings.c 改成 font_store.c —— 见 font_store.h 顶部为什么。
 */

#include "font_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "font_store";

static char s_font_path[160];  // 与 TTF_FONT_PATH_MAX 对齐

const char* font_store_get_path(void) {
    return s_font_path[0] ? s_font_path : NULL;
}

void font_store_set_path(const char* path) {
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

void font_store_init(void) {
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
