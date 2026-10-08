#include "app_services.h"

#include "settings_manager.h"
#include "wifi_manager.h"

#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

bool app_connect_wifi_from_settings(bool *outWasConnected) {
    const bool was = g_wifi.isConnected();
    if (outWasConnected) *outWasConnected = was;
    if (was) return true;
    std::string ssid = g_settings.wifiSsid();
    if (ssid.empty()) return false;
    std::string pass = g_settings.wifiPassword();
    g_wifi.begin();
    return g_wifi.connect(ssid.c_str(), pass.c_str());
}

void app_disconnect_wifi_if_needed(bool wasConnected) {
    if (!wasConnected) g_wifi.disconnect();
}

void app_scan_directory(const std::string &dir, std::vector<AppFileEntry> &out,
                        AppFileKindFn kindFn, bool includeHidden) {
    out.clear();
    DIR *dp = opendir(dir.c_str());
    if (!dp) return;
    std::vector<AppFileEntry> dirs, files;
    struct dirent *e;
    while ((e = readdir(dp)) != nullptr) {
        std::string name = e->d_name;
        if (name.empty()) continue;
        if (name == "." || name == "..") continue;
        if (!includeHidden && name[0] == '.') continue;
        std::string full = dir + "/" + name;
        struct stat sb;
        if (stat(full.c_str(), &sb) != 0) continue;
        const bool isDir = S_ISDIR(sb.st_mode);
        if (!isDir && !S_ISREG(sb.st_mode)) continue;
        int kind = kindFn ? kindFn(name, isDir) : (isDir ? -1 : 0);
        AppFileEntry ent{full, name, isDir, kind};
        if (isDir) dirs.push_back(std::move(ent));
        else files.push_back(std::move(ent));
    }
    closedir(dp);
    auto byName = [](const AppFileEntry &a, const AppFileEntry &b) { return a.name < b.name; };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    out = std::move(dirs);
    out.insert(out.end(), files.begin(), files.end());
}
