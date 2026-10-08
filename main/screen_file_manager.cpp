#include "screen_file_manager.h"
#include "file_manager_server.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "font_renderer.h"
#include "ui_helpers.h"
#include "app_async.h"
#include "app_services.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <atomic>
#include <cstdio>
#include "u8g2_shim.h"

static struct {
    bool serverRunning = false;
    bool wifiWasConnected = false;
    std::string ip;
    std::string status;
    uint16_t port = 80;
} g_fileMgrState;

static AppAsyncJob s_fileMgrJob;
static SemaphoreHandle_t s_fileMgrMutex = nullptr;

static bool fileMgrCancelled() {
    return s_fileMgrJob.cancelled();
}

static void fileMgrLock() {
    if (!s_fileMgrMutex) s_fileMgrMutex = xSemaphoreCreateMutex();
    if (s_fileMgrMutex) xSemaphoreTake(s_fileMgrMutex, portMAX_DELAY);
}

static void fileMgrUnlock() {
    if (s_fileMgrMutex) xSemaphoreGive(s_fileMgrMutex);
}

static void fileMgrSetStatus(const std::string &s) {
    fileMgrLock();
    g_fileMgrState.status = s;
    fileMgrUnlock();
}

static void fileMgrStartTask(void *arg) {
    (void)arg;
    bool ok = false;
    bool wifiWasConnected = false;
    std::string ip;
    uint16_t port = 80;

    fileMgrSetStatus("正在连接WiFi...");
    if (!app_connect_wifi_from_settings(&wifiWasConnected)) {
        fileMgrSetStatus(g_settings.wifiSsid().empty() ? "未配置WiFi" : "WiFi连接失败");
        goto done;
    }
    if (!wifiWasConnected) vTaskDelay(pdMS_TO_TICKS(500));
    if (fileMgrCancelled()) goto done;

    fileMgrSetStatus("正在获取IP...");
    for (int i = 0; i < 10 && !fileMgrCancelled(); i++) {
        ip = g_wifi.getIp();
        if (!ip.empty()) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (ip.empty()) {
        fileMgrSetStatus("获取IP失败");
        goto done;
    }
    if (fileMgrCancelled()) goto done;

    fileMgrSetStatus("正在启动服务...");
    if (file_manager_server_start(80)) {
        ok = true;
        port = file_manager_server_get_port();
    } else {
        fileMgrSetStatus("服务启动失败: " + std::string(file_manager_server_last_error()));
    }

done:
    if (fileMgrCancelled() && ok) {
        file_manager_server_stop();
        ok = false;
    }
    if (fileMgrCancelled()) app_disconnect_wifi_if_needed(wifiWasConnected);
    fileMgrLock();
    g_fileMgrState.wifiWasConnected = wifiWasConnected;
    g_fileMgrState.serverRunning = ok;
    g_fileMgrState.ip = ip;
    g_fileMgrState.port = port;
    if (ok) g_fileMgrState.status = "服务已启动";
    std::string result = g_fileMgrState.status;
    fileMgrUnlock();
    s_fileMgrJob.finish(ok ? "服务已启动" : result);
    vTaskDelete(nullptr);
}

void screen_file_manager_init() {
    if (!s_fileMgrMutex) s_fileMgrMutex = xSemaphoreCreateMutex();
    // 已经在跑了（启动任务还没结束就又进来一次）：直接复用现有任务，别 begin()。
    // begin() 会把 cancel_ 清回去并重新挂一个任务 → 两个 file_mgr_start 同时写
    // g_fileMgrState，且之前那次 cancel() 白取消。护栏同 startSettingsAsync
    // （screen_settings.cpp:1226）那一句。
    if (s_fileMgrJob.state() == AppAsyncState::Running) return;
    s_fileMgrJob.begin("文件管理", "正在连接WiFi...");
    fileMgrLock();
    g_fileMgrState.serverRunning = false;
    g_fileMgrState.ip.clear();
    g_fileMgrState.status = "正在连接WiFi...";
    g_fileMgrState.port = 80;
    g_fileMgrState.wifiWasConnected = g_wifi.isConnected();
    fileMgrUnlock();

    if (!s_fileMgrJob.start(fileMgrStartTask, "file_mgr_start", 6144)) {
        fileMgrSetStatus("系统繁忙,请重试");
        s_fileMgrJob.failToStart("系统繁忙,请重试");
    }
}

AppState screen_file_manager_handle(int key, ScreenContext &ctx) {
    (void)ctx;
    if (key == 'q' || key == 'Q' || key == 0x1B) {
        s_fileMgrJob.cancel();
        fileMgrLock();
        bool running = g_fileMgrState.serverRunning;
        bool wifiWasConnected = g_fileMgrState.wifiWasConnected;
        g_fileMgrState.serverRunning = false;
        fileMgrUnlock();
        if (running) file_manager_server_stop();
        app_disconnect_wifi_if_needed(wifiWasConnected);
        return APP_SETTINGS;
    }

    fileMgrLock();
    bool running = g_fileMgrState.serverRunning;
    std::string ip = g_fileMgrState.ip;
    std::string status = g_fileMgrState.status;
    uint16_t port = g_fileMgrState.port;
    fileMgrUnlock();

    ui_clear();
    int y = 43;
    ui_draw_text_centered(y, "文件管理", false, true);
    y += FONT_H;
    u8g2_DrawHLine(g_u8g2, 0, y, SCREEN_W);
    y += 23;

    if (running) {
        char line[80];
        snprintf(line, sizeof(line), "WiFi: %s", g_settings.wifiSsid().c_str());
        ui_draw_text(8, y, line);
        y += FONT_H;

        snprintf(line, sizeof(line), "地址: http://%s:%d", ip.c_str(), port);
        ui_draw_text(8, y, line);
        y += FONT_H;

        y += FONT_H;
        ui_draw_text(8, y, "请在浏览器中打开上述地址");
        y += FONT_H;
        ui_draw_text(8, y, "管理SD卡文件");
    } else {
        ui_draw_text_centered(SCREEN_H / 2, status.empty() ? "服务未启动" : status.c_str());
    }

    ui_draw_status("q:退出", "");
    ui_commit();
    return APP_FILE_MANAGER;
}
