#include "safe_file.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_log.h>

#include "longop.h"

// 原子写（.tmp → fsync → rename 那一串文件事务）也在欠载嫌疑名单里：写卡期间驱动那一侧
// 有它自己的锁与事务，落盘又是毫秒级。挂个 RAII 把名字记进探针环，欠载日志就能点名
// "当时正在写 <文件名>"（见 longop.h）。开销是两次微秒级临界区，写文件本身贵得多。
// / Wrap atomic file writes into the longop ring so an underrun can name the file.
namespace {
struct LongOpFile {
    explicit LongOpFile(const std::string &path) { longop_begin(path.c_str()); }
    ~LongOpFile() { longop_end(); }
};
}  // namespace

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

std::string readWholeFile(const std::string &path, size_t maxBytes, bool *truncated) {
    if (truncated) *truncated = false;
    repairSafeWriteFile(path);
    struct stat st;
    if (maxBytes > 0 && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_size > static_cast<off_t>(maxBytes)) {
        if (truncated) *truncated = true;
        ESP_LOGW(TAG, "read skipped: %s is %lld bytes > limit %u",
                 path.c_str(), (long long)st.st_size, (unsigned)maxBytes);
        return "";
    }
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return "";
    std::string result;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (maxBytes > 0 && result.size() + n > maxBytes) {
            const size_t keep = maxBytes > result.size() ? maxBytes - result.size() : 0;
            if (keep > 0) result.append(buf, keep);
            if (truncated) *truncated = true;
            ESP_LOGW(TAG, "read truncated: %s exceeds %u bytes", path.c_str(), (unsigned)maxBytes);
            break;
        }
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

// 原子写的两半，两个写入口（整份内容 / 流式）共用，免得备份-改名的先后次序在两处走样。
// 打开 <path>.tmp 供写入：建目录 → 清掉上次残留的 .tmp / 从 .bak 回滚 → 以 "w" 打开。
// 失败返回 NULL，且此时不留下 .tmp。tmp/bak 是出参，供后面的 commitTmpFile 使用。
static FILE *openTmpForWrite(const std::string &path, std::string &tmp, std::string &bak) {
    size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        if (!ensureDirPath(path.substr(0, slash))) return nullptr;
    }
    tmp = path + ".tmp";
    bak = path + ".bak";
    repairSafeWriteFile(path);
    remove(tmp.c_str());

    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) ESP_LOGE(TAG, "open tmp failed: %s errno=%d", tmp.c_str(), errno);
    return f;
}

// 把写好的 .tmp 提交成正式文件：原文件先退到 .bak，再把 .tmp 改名过来；任一步失败都把
// .bak 改回去、删掉 .tmp，返回 false（盘上仍是上一份完整内容，不是半份新的）。
static bool commitTmpFile(const std::string &path, const std::string &tmp, const std::string &bak) {
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

bool safeWriteFile(const std::string &path, const std::string &content) {
    LongOpFile _probe(path);
    std::string tmp, bak;
    FILE *f = openTmpForWrite(path, tmp, bak);
    if (!f) return false;
    const size_t written = fwrite(content.data(), 1, content.size(), f);
    bool ok = (written == content.size()) && flushAndClose(f);
    if (!ok) {
        ESP_LOGE(TAG, "write tmp failed: %s (written=%u/%u) errno=%d",
                 tmp.c_str(), (unsigned)written, (unsigned)content.size(), errno);
        remove(tmp.c_str());
        return false;
    }
    return commitTmpFile(path, tmp, bak);
}

bool safeWriteFileStream(const std::string &path,
                         bool (*writeChunk)(FILE *tmp, void *ctx),
                         void *ctx) {
    LongOpFile _probe(path);
    std::string tmp, bak;
    FILE *f = openTmpForWrite(path, tmp, bak);
    if (!f) return false;
    // 回调自己中止（false）与短写/fsync 失败同等对待：都删 tmp 回滚，绝不提交半份内容。
    bool ok = writeChunk != nullptr && writeChunk(f, ctx);
    if (ok) ok = flushAndClose(f);
    else fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "stream write tmp failed: %s errno=%d", tmp.c_str(), errno);
        remove(tmp.c_str());
        return false;
    }
    return commitTmpFile(path, tmp, bak);
}
