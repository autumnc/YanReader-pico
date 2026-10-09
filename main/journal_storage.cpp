#include "journal_storage.h"
#include "safe_file.h"
#include "settings_manager.h"
#include <cstring>
#include <ctime>
#include <algorithm>
#include <functional>
#include <unordered_set>
#include <dirent.h>
#include <sys/stat.h>
#include <esp_log.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "read_pico_sd.h"

static const char *TAG = "Journal";

static bool isJournalExt(const std::string &fn) {
    auto dot = fn.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = fn.substr(dot);
    return ext == ".txt" || ext == ".md";
}

static bool isSafeJournalFilename(const std::string &fn) {
    if (fn.empty() || fn[0] == '.') return false;
    if (fn.find('/') != std::string::npos || fn.find('\\') != std::string::npos) return false;
    if (fn.find("..") != std::string::npos) return false;
    return isJournalExt(fn);
}

static std::string stemOf(const std::string &fn) {
    size_t dot = fn.rfind('.');
    return dot == std::string::npos ? fn : fn.substr(0, dot);
}

static void cleanupOldHistory(const std::string &dir, int keep) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> files;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_type != DT_REG) continue;
        std::string fn = de->d_name;
        if (fn.empty() || fn[0] == '.') continue;
        files.push_back(fn);
    }
    closedir(d);
    if ((int)files.size() <= keep) return;
    std::sort(files.begin(), files.end());
    for (int i = 0; i < (int)files.size() - keep; i++) {
        remove((dir + "/" + files[i]).c_str());
    }
}

static bool saveJournalHistoryVersion(const std::string &base, const std::string &filename,
                                      const std::string &oldContent) {
    std::string dir = base + "/.history/" + stemOf(filename);
    if (!ensureDirPath(dir)) return false;
    time_t now;
    time(&now);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d_%H%M%S", tm);
    std::string path = dir + "/" + std::string(ts) + ".txt";
    for (int i = 1; fileExists(path) && i < 100; i++) {
        path = dir + "/" + std::string(ts) + "_" + std::to_string(i) + ".txt";
    }
    bool ok = safeWriteFile(path, oldContent);
    if (ok) cleanupOldHistory(dir, 10);
    return ok;
}

static std::string historyDirFor(const std::string &base, const std::string &filename) {
    return base + "/.history/" + stemOf(filename);
}

static bool isSafeHistoryFilename(const std::string &fn) {
    if (fn.empty() || fn.find('/') != std::string::npos || fn.find("..") != std::string::npos) return false;
    return isJournalExt(fn);
}

// SD card mutex (recursive to handle nested public method calls)
static SemaphoreHandle_t s_sd_mutex = nullptr;

SemaphoreHandle_t JournalStorage::sdMutex() { return s_sd_mutex; }

JournalStorage g_journal;

bool JournalStorage::begin() {
    // SD 卡由 read_pico 驱动异步探测（read_pico_init 已发起 read_pico_sd_start_probe）。
    // 这里轮询挂载结果，等卡就绪（或超时）。挂载点与 pjournal 一致，均为 /sdcard。
    read_pico_sd_info_t info = {};
    esp_err_t err = ESP_ERR_NOT_FINISHED;
    for (int i = 0; i < 60; i++) {  // ~3s
        err = read_pico_sd_get_info(&info);
        if (err == ESP_ERR_INVALID_STATE) {
            read_pico_sd_start_probe();  // 探测尚未发起（极端情况）
        } else if (info.mounted) {
            break;
        } else if (err != ESP_ERR_NOT_FINISHED) {
            break;  // 无卡 / 挂载失败 / 需格式化
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (!info.mounted) {
        ESP_LOGW(TAG, "SD card not ready (%s), journal disabled", esp_err_to_name(err));
        mounted_ = false;
        return false;
    }

    mounted_ = true;
    if (!s_sd_mutex) s_sd_mutex = xSemaphoreCreateRecursiveMutex();
    ensureDir();
    ESP_LOGI(TAG, "SD card ready at %s (free=%llu MB)", basePath().c_str(),
             (unsigned long long)(info.free_bytes / (1024 * 1024)));
    return true;
}

void JournalStorage::deinit() {
    if (mounted_) {
        read_pico_sd_sync();  // 卸载落盘
        mounted_ = false;
    }
}

std::string JournalStorage::basePath() {
    return "/sdcard/pjournal";
}

void JournalStorage::ensureDir() {
    mkdir("/sdcard/pjournal", 0777);
}

void JournalStorage::scanIndex() {
    m_fileIndex.clear();
    m_dateSet.clear();
    DIR *dir = opendir(basePath().c_str());
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_type != DT_REG) continue;
            std::string fn = de->d_name;
            if (fn.size() > 4 && fn.substr(fn.size() - 4) == ".bak") {
                std::string orig = fn.substr(0, fn.size() - 4);
                std::string origPath = basePath() + "/" + orig;
                if (isJournalExt(orig) && !fileExists(origPath)) {
                    rename((basePath() + "/" + fn).c_str(), origPath.c_str());
                    fn = orig;
                }
            }
            if (fn[0] == '.' || !isJournalExt(fn)) continue;
            m_fileIndex.push_back(fn);
            m_dateSet.insert(fn.substr(0, 10));
        }
        closedir(dir);
    }
    // Newest first
    std::sort(m_fileIndex.begin(), m_fileIndex.end(), std::greater<std::string>());
    m_indexValid = true;
    m_revision++;   // 索引换了内容：stats() 的备忘作废
}

void JournalStorage::ensureIndex() {
    if (!m_indexValid) scanIndex();
}

void JournalStorage::indexAddFile(const std::string &fn) {
    // Overwrite case (editor save, sync download of existing file) — already indexed
    for (const auto &f : m_fileIndex) {
        if (f == fn) return;
    }
    auto it = std::lower_bound(m_fileIndex.begin(), m_fileIndex.end(), fn, std::greater<std::string>());
    m_fileIndex.insert(it, fn);
    m_dateSet.insert(fn.substr(0, 10));
    m_revision++;
}

void JournalStorage::indexRemoveFile(const std::string &fn) {
    auto &v = m_fileIndex;
    v.erase(std::remove(v.begin(), v.end(), fn), v.end());
    std::string date = fn.substr(0, 10);
    m_revision++;
    for (const auto &f : v) {
        if (f.substr(0, 10) == date) return;  // date still has other files
    }
    m_dateSet.erase(date);
}

bool JournalStorage::saveEntry(const std::string &text) {
    if (!mounted_) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureDir();
    time_t now;
    time(&now);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d_%H%M%S", tm);
    std::string fname = std::string(ts) + ".txt";

    if (!safeWriteFile(basePath() + "/" + fname, text)) {
        if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
        return false;
    }
    ESP_LOGI(TAG, "Saved: %s", fname.c_str());
    if (m_indexValid) indexAddFile(fname);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return true;
}

bool JournalStorage::saveEntryRaw(const std::string &filename, const std::string &content, bool createHistory) {
    if (!mounted_ || !isSafeJournalFilename(filename)) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureDir();
    std::string path = basePath() + "/" + filename;
    if (createHistory && g_settings.versionHistory() && filename.rfind("__", 0) != 0 && fileExists(path)) {
        bool oldTooLarge = false;
        std::string old = readWholeFile(path, READ_WHOLE_FILE_DEFAULT_MAX, &oldTooLarge);
        if (!oldTooLarge && old != content) saveJournalHistoryVersion(basePath(), filename, old);
    }
    if (!safeWriteFile(path, content)) {
        if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
        return false;
    }
    if (m_indexValid) indexAddFile(filename);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return true;
}

bool JournalStorage::saveRecoveryDraft(const std::string &content, const std::string &meta) {
    if (!mounted_) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    std::string dir = basePath() + "/.recovery";
    bool ok = ensureDirPath(dir) &&
              safeWriteFile(dir + "/editor.tmp", content) &&
              safeWriteFile(dir + "/editor.meta", meta);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return ok;
}

bool JournalStorage::loadRecoveryDraft(std::string &content, std::string &meta) {
    if (!mounted_) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    std::string dir = basePath() + "/.recovery";
    content = readWholeFile(dir + "/editor.tmp");
    meta = readWholeFile(dir + "/editor.meta");
    bool ok = !content.empty() || !meta.empty();
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return ok;
}

void JournalStorage::clearRecoveryDraft() {
    if (!mounted_) return;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    std::string dir = basePath() + "/.recovery";
    remove((dir + "/editor.tmp").c_str());
    remove((dir + "/editor.meta").c_str());
    remove((dir + "/editor.tmp.tmp").c_str());
    remove((dir + "/editor.meta.tmp").c_str());
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
}

std::vector<JournalHistoryVersion> JournalStorage::listHistoryVersions(const std::string &filename) {
    std::vector<JournalHistoryVersion> result;
    if (!mounted_ || !isSafeJournalFilename(filename)) return result;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    std::string dir = historyDirFor(basePath(), filename);
    DIR *d = opendir(dir.c_str());
    if (!d) {
        if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
        return result;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_type != DT_REG) continue;
        std::string fn = de->d_name;
        if (!isSafeHistoryFilename(fn)) continue;
        struct stat st;
        std::string path = dir + "/" + fn;
        if (stat(path.c_str(), &st) == 0) {
            JournalHistoryVersion v;
            v.filename = fn;
            v.mtime = st.st_mtime;
            v.size = (size_t)st.st_size;
            result.push_back(v);
        }
    }
    closedir(d);
    std::sort(result.begin(), result.end(), [](const JournalHistoryVersion &a, const JournalHistoryVersion &b) {
        if (a.filename != b.filename) return a.filename > b.filename;
        return a.mtime > b.mtime;
    });
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return result;
}

std::string JournalStorage::readHistoryVersion(const std::string &filename, const std::string &historyFilename) {
    if (!mounted_ || !isSafeJournalFilename(filename) || !isSafeHistoryFilename(historyFilename)) return "";
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    std::string content = readWholeFile(historyDirFor(basePath(), filename) + "/" + historyFilename);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return content;
}

bool JournalStorage::restoreHistoryVersion(const std::string &filename, const std::string &historyFilename) {
    if (!mounted_ || !isSafeJournalFilename(filename) || !isSafeHistoryFilename(historyFilename)) return false;
    std::string content = readHistoryVersion(filename, historyFilename);
    if (content.empty()) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureDir();
    std::string path = basePath() + "/" + filename;
    if (fileExists(path)) {
        bool oldTooLarge = false;
        std::string old = readWholeFile(path, READ_WHOLE_FILE_DEFAULT_MAX, &oldTooLarge);
        if (!oldTooLarge && old != content) saveJournalHistoryVersion(basePath(), filename, old);
    }
    bool ok = safeWriteFile(path, content);
    if (ok && m_indexValid) indexAddFile(filename);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return ok;
}

bool JournalStorage::deleteHistoryVersion(const std::string &filename, const std::string &historyFilename) {
    if (!mounted_ || !isSafeJournalFilename(filename) || !isSafeHistoryFilename(historyFilename)) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    bool ok = remove((historyDirFor(basePath(), filename) + "/" + historyFilename).c_str()) == 0;
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return ok;
}

std::vector<JournalEntry> JournalStorage::listEntries() {
    std::vector<JournalEntry> entries;
    if (!mounted_) return entries;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);

    // Rebuild index to pick up external changes; index is already newest-first
    scanIndex();

    for (const auto &fn : m_fileIndex) {
        JournalEntry e;
        e.filename = fn;
        e.date = fn.substr(0, 10);

        // Read only first 512 bytes for preview extraction (performance optimization)
        std::string content;
        std::string path = basePath() + "/" + fn;
        FILE *f = fopen(path.c_str(), "r");
        if (f) {
            char buf[512];
            int len = fread(buf, 1, sizeof(buf) - 1, f);
            if (len > 0) {
                buf[len] = 0;
                content = buf;
            }
            fclose(f);
        }

        if (!content.empty()) {
            size_t pos = content.find('\n');
            if (pos != std::string::npos) {
                std::string first = content.substr(0, pos);
                if (first.find("提示词:") == 0)
                    e.title = "提示写作";
                else if (first.find("自由写作") != std::string::npos)
                    e.title = "自由写作";
                size_t body_start = content.find("\n\n");
                if (body_start != std::string::npos && body_start < content.size()) {
                    std::string body = content.substr(body_start + 2);
                    std::string preview_text;
                    size_t start = 0;
                    while (start < body.size()) {
                        size_t nl = body.find('\n', start);
                        std::string line = (nl != std::string::npos) ? body.substr(start, nl - start) : body.substr(start);
                        // Skip metadata lines and prompt label
                        if (line == "自由写作" && e.title.empty()) e.title = "自由写作";
                        if (!line.empty() &&
                            line.find("日期:") != 0 &&
                            line.find("字数:") != 0 &&
                            line.find("提示词:") != 0 &&
                            line != "自由写作") {
                            preview_text = line;
                            break;
                        }
                        if (nl == std::string::npos) break;
                        start = nl + 1;
                    }
                    // 预览留够"横屏一行还富余"的量：列表行是**单行铺满**的（日期 +
                    // 预览吃掉其余宽度，见 pjournal_app 的 drawBrowser），横屏 1216px
                    // 一行 ≈ 54 个汉字 ≈ 165 字节 —— 原来的 40 字节（≈13 字）只够占住
                    // 左边一小段，右边整块空着。真正的裁剪交给绘制方按当前宽度做。
                    // 截在整字边界上：substr(0,N) 会切出半个汉字，画出来是乱码。
                    const size_t kPreviewBytes = 192;
                    if (preview_text.size() > kPreviewBytes) {
                        size_t cut = kPreviewBytes;
                        while (cut > 0 && ((unsigned char)preview_text[cut] & 0xC0) == 0x80) cut--;
                        preview_text.resize(cut);
                    }
                    e.preview = preview_text;
                }
            }
        }
        entries.push_back(e);
    }
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return entries;
}

std::vector<std::pair<std::string, time_t>> JournalStorage::listFileMtimes() {
    std::vector<std::pair<std::string, time_t>> result;
    if (!mounted_) return result;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);

    DIR *dir = opendir(basePath().c_str());
    if (!dir) { if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex); return result; }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_type != DT_REG) continue;
        std::string fn = de->d_name;
        if (fn[0] == '.') continue;
        if (fn.size() >= 2 && fn[0] == '_' && fn[1] == '_') continue;  // skip temp files
        if (!isJournalExt(fn)) continue;

        struct stat st;
        std::string full = basePath() + "/" + fn;
        repairSafeWriteFile(full);
        if (stat(full.c_str(), &st) == 0) {
            result.push_back({fn, st.st_mtime});
        }
    }
    closedir(dir);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return result;
}

std::string JournalStorage::readEntry(const std::string &filename) {
    if (!mounted_ || !isSafeJournalFilename(filename)) return "";
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    std::string path = basePath() + "/" + filename;
    repairSafeWriteFile(path);
    FILE *f = fopen(path.c_str(), "r");
    if (!f) { if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex); return ""; }
    std::string result;
    char buf[256];
    int n;
    while ((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
        buf[n] = 0;
        result += buf;
    }
    fclose(f);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return result;
}

bool JournalStorage::deleteEntry(const std::string &filename) {
    if (!mounted_ || !isSafeJournalFilename(filename)) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    bool ok = remove((basePath() + "/" + filename).c_str()) == 0;
    if (ok && m_indexValid) indexRemoveFile(filename);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return ok;
}

bool JournalStorage::hasEntry(const std::string &date) {
    if (!mounted_) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureIndex();
    bool found = m_dateSet.count(date) > 0;
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return found;
}

int JournalStorage::countToday() { return stats().todayCount; }

int JournalStorage::getStreak() { return stats().streak; }

int JournalStorage::totalEntries() { return stats().total; }

JournalStorage::Stats JournalStorage::stats() {
    Stats s;
    if (!mounted_) return s;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureIndex();

    time_t now; time(&now);
    char today[16];
    strftime(today, sizeof(today), "%Y-%m-%d", localtime(&now));

    if (!(m_statsValid && m_statsRev == m_revision && strcmp(m_statsDay, today) == 0)) {
        s.total = (int)m_fileIndex.size();

        // 今日篇数：只比文件名前 10 个字节（YYYY-MM-DD）。原来走 fn.substr(0, 10) 再比
        // std::string —— 10 个字符落在 SSO 上不分配，但每篇都白白多一次 strlen + 拷贝，
        // 而这一行每帧要跑遍整库。
        s.todayCount = 0;
        for (const auto &fn : m_fileIndex) {
            if (fn.size() >= 10 && memcmp(fn.data(), today, 10) == 0) s.todayCount++;
        }

        // 连续天数：从今天往前一天一天回数，撞到第一个空日就停。一天一次 localtime +
        // strftime 是这个算法固有的一部分，所以只在备忘失效时才跑（原来每帧跑一遍）。
        int streak = 0;
        for (int i = 0; i < 365; i++) {
            time_t t = now - i * 86400;
            char date[16];
            strftime(date, sizeof(date), "%Y-%m-%d", localtime(&t));
            if (m_dateSet.count(date))
                streak++;
            else
                break;
        }
        s.streak = streak;

        m_statsValid = true;
        m_statsRev = m_revision;
        memcpy(m_statsDay, today, sizeof(m_statsDay));
        m_statsTotal = s.total;
        m_statsToday = s.todayCount;
        m_statsStreak = s.streak;
    } else {
        s.total = m_statsTotal;
        s.todayCount = m_statsToday;
        s.streak = m_statsStreak;
    }

    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return s;
}
