#include "flomo_file.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_log.h>

static const char *TAG = "FlomoFile";

bool flomoFileExists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool flomoEnsureDir(const std::string &path) {
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
            if (mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "mkdir failed: %s errno=%d", cur.c_str(), errno);
                return false;
            }
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
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

std::string flomoReadWholeFile(const std::string &path) {
    flomoRepairFile(path);
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return "";
    std::string result;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) result.append(buf, n);
    fclose(f);
    return result;
}

void flomoRepairFile(const std::string &path) {
    std::string tmp = path + ".tmp";
    std::string bak = path + ".bak";
    bool hasFinal = flomoFileExists(path);
    bool hasBak = flomoFileExists(bak);
    if (!hasFinal && hasBak) rename(bak.c_str(), path.c_str());
    if (flomoFileExists(tmp)) remove(tmp.c_str());
}

bool flomoSafeWriteFile(const std::string &path, const std::string &content) {
    size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        if (!flomoEnsureDir(path.substr(0, slash))) return false;
    }

    std::string tmp = path + ".tmp";
    std::string bak = path + ".bak";
    flomoRepairFile(path);
    remove(tmp.c_str());

    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) {
        ESP_LOGE(TAG, "open tmp failed: %s", tmp.c_str());
        return false;
    }
    size_t written = fwrite(content.data(), 1, content.size(), f);
    bool ok = (written == content.size()) && flushAndClose(f);
    if (!ok) {
        ESP_LOGE(TAG, "write tmp failed: %s", tmp.c_str());
        remove(tmp.c_str());
        return false;
    }

    remove(bak.c_str());
    bool hadOriginal = flomoFileExists(path);
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
