#include "safe_file.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_log.h>

static const char *TAG = "SafeFile";

bool fileExists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool ensureDirPath(const std::string &path) {
    if (path.empty()) return false;
    std::string cur;
    size_t start = (path[0] == '/') ? 1 : 0;
    if (start == 1) cur = "/";
    while (start < path.size()) {
        size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) {
            if (!cur.empty() && cur.back() != '/') cur += "/";
            cur += part;
            // 挂载点 /sdcard 等根目录不能 mkdir（返回 EINVAL）；已存在的目录也跳过。
            struct stat st;
            if (stat(cur.c_str(), &st) == 0) {
                if (!S_ISDIR(st.st_mode)) {
                    ESP_LOGE(TAG, "not a dir: %s", cur.c_str());
                    return false;
                }
            } else if (mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "mkdir failed: %s errno=%d", cur.c_str(), errno);
                return false;
            }
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

std::string readWholeFile(const std::string &path) {
    repairSafeWriteFile(path);
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return "";
    std::string result;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        result.append(buf, n);
    }
    fclose(f);
    return result;
}

static bool flushAndClose(FILE *f) {
    bool ok = fflush(f) == 0;
    int fd = fileno(f);
    if (fd >= 0 && fsync(fd) != 0) {
        ESP_LOGW(TAG, "fsync failed errno=%d; continuing after fflush", errno);
    }
    if (fclose(f) != 0) ok = false;
    return ok;
}

void repairSafeWriteFile(const std::string &path) {
    std::string tmp = path + ".tmp";
    std::string bak = path + ".bak";
    bool hasFinal = fileExists(path);
    bool hasBak = fileExists(bak);
    if (!hasFinal && hasBak) {
        rename(bak.c_str(), path.c_str());
    }
    if (fileExists(tmp)) remove(tmp.c_str());
}

bool safeWriteFile(const std::string &path, const std::string &content) {
    size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        if (!ensureDirPath(path.substr(0, slash))) return false;
    }

    std::string tmp = path + ".tmp";
    std::string bak = path + ".bak";
    repairSafeWriteFile(path);
    remove(tmp.c_str());

    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) {
        ESP_LOGE(TAG, "open tmp failed: %s errno=%d", tmp.c_str(), errno);
        return false;
    }
    size_t written = fwrite(content.data(), 1, content.size(), f);
    bool ok = (written == content.size()) && flushAndClose(f);
    if (!ok) {
        ESP_LOGE(TAG, "write tmp failed: %s (written=%u/%u) errno=%d",
                 tmp.c_str(), (unsigned)written, (unsigned)content.size(), errno);
        remove(tmp.c_str());
        return false;
    }

    remove(bak.c_str());
    bool hadOriginal = fileExists(path);
    if (hadOriginal && rename(path.c_str(), bak.c_str()) != 0) {
        ESP_LOGE(TAG, "backup failed: %s", path.c_str());
        remove(tmp.c_str());
        return false;
    }

    if (rename(tmp.c_str(), path.c_str()) != 0) {
        ESP_LOGE(TAG, "commit failed: %s", path.c_str());
        if (hadOriginal) rename(bak.c_str(), path.c_str());
        remove(tmp.c_str());
        return false;
    }

    if (hadOriginal) remove(bak.c_str());
    return true;
}
