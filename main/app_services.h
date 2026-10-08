#pragma once

#include <string>
#include <vector>

struct AppFileEntry {
    std::string path;
    std::string name;
    bool isDir = false;
    int kind = 0;
};

using AppFileKindFn = int (*)(const std::string &name, bool isDir);

bool app_connect_wifi_from_settings(bool *outWasConnected = nullptr);
void app_disconnect_wifi_if_needed(bool wasConnected);

void app_scan_directory(const std::string &dir, std::vector<AppFileEntry> &out,
                        AppFileKindFn kindFn = nullptr, bool includeHidden = false);
