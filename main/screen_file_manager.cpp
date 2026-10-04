#include "screen_file_manager.h"
#include "file_manager_server.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "font_renderer.h"
#include "ui_helpers.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>

extern u8g2_t *g_u8g2;

extern "C" {
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
}

static struct {
    bool serverRunning = false;
    bool wifiWasConnected = false;
    std::string ip;
    uint16_t port = 80;
} g_fileMgrState;

void screen_file_manager_init() {
    g_fileMgrState.serverRunning = false;
    g_fileMgrState.ip = "";
    g_fileMgrState.port = 80;

    // Show starting message
    ui_clear();
    ui_draw_text_centered(SCREEN_H / 2 - FONT_H / 2, "正在连接WiFi...");
    ui_commit();

    // Connect WiFi if not already connected
    g_fileMgrState.wifiWasConnected = g_wifi.isConnected();
    if (!g_fileMgrState.wifiWasConnected) {
        std::string ssid = g_settings.wifiSsid();
        if (ssid.empty()) {
            ui_clear();
            ui_draw_text_centered(SCREEN_H / 2 - FONT_H, "未配置WiFi");
            ui_draw_text_centered(SCREEN_H / 2, "请先在设置中配置WiFi");
            ui_commit();
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
        std::string pass = g_settings.wifiPassword();
        g_wifi.begin();
        if (!g_wifi.connect(ssid.c_str(), pass.c_str())) {
            ui_clear();
            ui_draw_text_centered(SCREEN_H / 2 - FONT_H / 2, "WiFi连接失败");
            ui_commit();
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
        // Wait for IP to be assigned
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Get IP (retry a few times)
    for (int i = 0; i < 10; i++) {
        g_fileMgrState.ip = g_wifi.getIp();
        if (!g_fileMgrState.ip.empty()) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (g_fileMgrState.ip.empty()) {
        ui_clear();
        ui_draw_text_centered(SCREEN_H / 2 - FONT_H / 2, "获取IP失败");
        ui_commit();
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    // Start HTTP server
    ui_clear();
    ui_draw_text_centered(SCREEN_H / 2 - FONT_H / 2, "正在启动服务...");
    ui_commit();

    if (file_manager_server_start(80)) {
        g_fileMgrState.serverRunning = true;
        g_fileMgrState.port = file_manager_server_get_port();
    } else {
        ui_clear();
        ui_draw_text_centered(SCREEN_H / 2 - FONT_H / 2, "服务启动失败");
        ui_commit();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

AppState screen_file_manager_handle(int key, ScreenContext &ctx) {
    // Exit on q/Q/ESC
    if (key == 'q' || key == 'Q' || key == 0x1B) {
        // Stop server
        if (g_fileMgrState.serverRunning) {
            file_manager_server_stop();
            g_fileMgrState.serverRunning = false;
        }
        // Disconnect WiFi if we connected it
        if (!g_fileMgrState.wifiWasConnected) {
            g_wifi.disconnect();
        }
        return APP_SETTINGS;
    }

    // Draw screen
    ui_clear();
    int y = 43;
    ui_draw_text_centered(y, "文件管理", false, true);
    y += FONT_H;
    u8g2_DrawHLine(g_u8g2, 0, y, SCREEN_W);
    y += 23;

    if (g_fileMgrState.serverRunning) {
        char line[64];

        snprintf(line, sizeof(line), "WiFi: %s", g_settings.wifiSsid().c_str());
        ui_draw_text(8, y, line);
        y += FONT_H;

        snprintf(line, sizeof(line), "地址: http://%s:%d", g_fileMgrState.ip.c_str(), g_fileMgrState.port);
        ui_draw_text(8, y, line);
        y += FONT_H;

        y += FONT_H;  // blank line
        ui_draw_text(8, y, "请在浏览器中打开上述地址");
        y += FONT_H;
        ui_draw_text(8, y, "管理SD卡文件");
    } else {
        ui_draw_text_centered(SCREEN_H / 2, "服务未启动");
    }

    // Status bar
    ui_draw_status("q:退出", "");

    ui_commit();
    return APP_FILE_MANAGER;
}
