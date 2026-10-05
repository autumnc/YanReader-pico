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

// 逐段建目录。**已存在的段必须先 stat 判一下再跳过**（与 safe_file.cpp 的 ensureDirPath
// 逐字同一套做法，HalStorage::mkdir 也是）。
//
// 不能写成「mkdir 返回 0 或 EEXIST 才算过」：整条路的第一段就是挂载点 /sdcard，而
// ESP-IDF 的 mkdir 对**已存在的目录**并不返回 EEXIST —— VFS 把挂载点前缀剥掉后对
// FatFS 的 0: 调 f_mkdir，create_name 见到空名直接 FR_INVALID_NAME（ff.c:2946），
// vfs_fat.c 的 fresult_to_errno 把它映射成 EINVAL(22)。于是老写法在**第一段就**
// `return false`，`/sdcard/flomo` 一辈子建不出来。
// 后果是整条 flomo 本地库链路静默全废：MemoDb::load 里 fopen 拿不到目录 → 空串 →
// 「暂无笔记」；MemoDb::save → flomoSafeWriteFile 里 `if (!flomoEnsureDir(...)) return false`
// → **一条也写不进去**（两步的返回值都被调用方忽略，屏上日志一片安静）。用户看到的
// 就是「每次进入都是暂无笔记，同步一下才（从服务器）拉出来」。
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
            struct stat st;
            if (stat(cur.c_str(), &st) == 0) {
                // 已存在（挂载点 /sdcard 也走这里）：是目录就跳过，不是就报错。
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
