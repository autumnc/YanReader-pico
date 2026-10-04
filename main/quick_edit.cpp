#include "quick_edit.h"
#include "settings_manager.h"
#include "safe_file.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <ctime>
#include <dirent.h>
#include <vector>
#include <esp_log.h>

static const char *TAG = "QuickEdit";
static const char *QUICK_INDEX_KEY = "quick_index";

bool g_quickEdit = false;

bool quickEditInit() {
    for (int i = 0; i < 10; i++) {
        std::string path = quickEditFilePath(i);
        FILE *f = fopen(path.c_str(), "a");  // 不存在则创建空文件
        if (f) {
            fclose(f);
        } else {
            ESP_LOGE(TAG, "Cannot create %s", path.c_str());
        }
    }
    ESP_LOGI(TAG, "Quick edit files ready, current=%d", quickEditIndex());
    return true;
}

int quickEditIndex() {
    std::string v = g_settings.getString(QUICK_INDEX_KEY, "0");
    int idx = atoi(v.c_str());
    if (idx < 0) idx = 0;
    if (idx > 9) idx = 9;
    return idx;
}

void quickEditSetIndex(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 9) idx = 9;
    g_settings.setString(QUICK_INDEX_KEY, std::to_string(idx));
}

void quickEditNext() { quickEditSetIndex((quickEditIndex() + 1) % 10); }
void quickEditPrev() { quickEditSetIndex((quickEditIndex() + 9) % 10); }

std::string quickEditFilePath(int idx) {
    char buf[16];
    snprintf(buf, sizeof(buf), "/sdcard/%d.txt", idx);
    return buf;
}

std::string quickEditLoad(int idx) {
    std::string path = quickEditFilePath(idx);
    repairSafeWriteFile(path);
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return "";
    std::string result;
    char buf[256];
    int n;
    while ((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
        buf[n] = 0;
        result += buf;
    }
    fclose(f);
    return result;
}

static void cleanupQuickHistory(const std::string &dir, int keep) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> files;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_type != DT_REG) continue;
        std::string fn = de->d_name;
        if (!fn.empty() && fn[0] != '.') files.push_back(fn);
    }
    closedir(d);
    if ((int)files.size() <= keep) return;
    std::sort(files.begin(), files.end());
    for (int i = 0; i < (int)files.size() - keep; i++) {
        remove((dir + "/" + files[i]).c_str());
    }
}

static void saveQuickHistoryVersion(int idx, const std::string &oldContent) {
    std::string dir = "/sdcard/.quick_history/" + std::to_string(idx);
    if (!ensureDirPath(dir)) return;
    time_t now;
    time(&now);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d_%H%M%S", tm);
    std::string path = dir + "/" + std::string(ts) + ".txt";
    for (int i = 1; fileExists(path) && i < 100; i++) {
        path = dir + "/" + std::string(ts) + "_" + std::to_string(i) + ".txt";
    }
    if (safeWriteFile(path, oldContent)) {
        cleanupQuickHistory(dir, 10);
    }
}

bool quickEditSave(int idx, const std::string &text, bool createHistory) {
    std::string path = quickEditFilePath(idx);
    if (createHistory && g_settings.versionHistory() && fileExists(path)) {
        std::string old = readWholeFile(path);
        if (old != text) saveQuickHistoryVersion(idx, old);
    }
    if (!safeWriteFile(path, text)) {
        ESP_LOGE(TAG, "Cannot write %s", path.c_str());
        return false;
    }
    return true;
}
