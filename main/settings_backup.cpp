// 配置与阅读记录备份 / 恢复到 TF 卡。
//
// 本机的"配置"本来就是一叠文件：<SD>/settings/ 一个键一个文件（settings_manager.cpp
// 的 BASE_DIR），阅读位置 <SD>/reader_progress.txt、阅读笔记 <SD>/reader_notes.txt、
// 阅读统计 <SD>/reader_stats.json（reading_stats.cpp 的 kStatsPath），再加编辑器的
// .quick_history/ 版本历史。所以备份 = **目录树的复制**，不必像社区固件那样自己搓一个
// 序列化格式 —— 那边设置是内存里一个定长结构体，只能序列化；我们这边文件本身就是格式，
// 拔卡插电脑上直接能看、能改、能只挑一个键拷回去，这比任何自造格式都有用。
//
// 位置：<SD>/settings_backup/。
//
//   save   —— 先整棵写进 settings_backup.tmp/，全部成功之后再 removeTree(旧) +
//             rename(tmp → 正式)。掉电最坏毁掉 staging，上一份备份始终完整。
//   restore—— 没有这种余地（要盖的就是正在用的那些文件），退而求其次：**每个文件各自
//             tmp+rename**。掉电最坏是"一半新一半旧"，但每个文件都是完整的，不会出现
//             半截 JSON / 半条笔记（那种才是读的时候直接崩的）。
//
// 恢复**不删**备份里没有的文件：备份之后新出现的设置键、新写的书签，恢复后原样留着。
// 保守方向是对的 —— 恢复的意图是"把当时那一套拿回来"，不是"把之后的一切抹掉"。
//
// 不备份 .crossmux/（阅读缓存，能重建，还可能有几百 MB）和 books/、pjournal/（用户自己的
// 书与日记，不是本固件的配置，而且大）。备份只是配置，应当小到能随手塞进任何一张卡。

#include "settings_backup.h"

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <esp_log.h>

#include "read_pico_sd.h"

static const char *TAG = "CfgBackup";

static const char *kStageDir = "/sdcard/settings_backup.tmp";
static const char *kLiveDir = "/sdcard/settings_backup";
// 正式的备份目录里放一份"这是什么时候、哪个版本的备份"，纯给人看的记录。
// 不带它也不影响恢复 —— 恢复只看有没有目录、文件全不全。
static const char *kInfoName = "backup_info.txt";

// 备份内容表。src 是本机上的绝对路径，name 是它在备份目录里的落点。
// dir=true 的整棵递归复制；false 的复制单个文件。源不存在就跳过（不是错误）——
// "从没记过笔记"和"笔记丢了"是两回事，前者不该让整次备份失败。
struct BackupEntry {
    const char *src;
    const char *name;
    bool dir;
};
static const BackupEntry kEntries[] = {
    {"/sdcard/settings", "settings", true},
    {"/sdcard/.quick_history", ".quick_history", true},
    {"/sdcard/reader_progress.txt", "reader_progress.txt", false},
    {"/sdcard/reader_notes.txt", "reader_notes.txt", false},
    {"/sdcard/reader_stats.json", "reader_stats.json", false},
};
static const int kEntryCount = (int)(sizeof(kEntries) / sizeof(kEntries[0]));

// ── 小工具（与 dictionary_store.cpp 同一套，各自 static，不跨文件共用）──────────
static std::string pathJoin(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

static bool dirExists(const std::string &p) {
    struct stat s;
    return stat(p.c_str(), &s) == 0 && S_ISDIR(s.st_mode);
}

// 递归删目录/文件。不存在也算成功。
static bool removeTree(const std::string &p) {
    struct stat s;
    if (stat(p.c_str(), &s) != 0) return true;
    if (!S_ISDIR(s.st_mode)) return remove(p.c_str()) == 0;

    DIR *dp = opendir(p.c_str());
    if (!dp) return false;
    struct dirent *e;
    bool ok = true;
    while ((e = readdir(dp)) != nullptr) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (!removeTree(pathJoin(p, e->d_name))) ok = false;
    }
    closedir(dp);
    if (!ok) return false;
    return rmdir(p.c_str()) == 0;
}

static bool cardReady() {
    read_pico_sd_info_t info = {};
    return read_pico_sd_get_info(&info) == ESP_OK && info.mounted;
}

// 复制一个文件，落到 dst 的过程是 tmp+rename（见文件头）。4KB 一趟，内存里只占 4KB ——
// reader_stats.json 也就几 KB，但 .quick_history/ 里可能有大段旧稿，流式才是对的。
static bool copyFileAtomic(const std::string &src, const std::string &dst) {
    FILE *in = fopen(src.c_str(), "rb");
    if (in == nullptr) return false;
    const std::string tmp = dst + ".tmp";
    FILE *out = fopen(tmp.c_str(), "wb");
    if (out == nullptr) {
        fclose(in);
        return false;
    }
    static char buf[4096];
    bool ok = true;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (ferror(in)) ok = false;
    if (fclose(out) != 0) ok = false;   // 落盘错误只在这里暴露
    fclose(in);
    if (!ok) {
        remove(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), dst.c_str()) != 0) {
        ESP_LOGW(TAG, "改名失败: %s", tmp.c_str());
        remove(tmp.c_str());
        return false;
    }
    return true;
}

// 递归复制目录。dst 不存在就建。任何一个文件失败就整体返回 false（调用方据此丢弃整趟）。
static bool copyTree(const std::string &srcDir, const std::string &dstDir) {
    struct stat s;
    if (stat(srcDir.c_str(), &s) != 0 || !S_ISDIR(s.st_mode)) return true;  // 源目录不存在：不算错
    if (!dirExists(dstDir) && mkdir(dstDir.c_str(), 0777) != 0) {
        ESP_LOGW(TAG, "建目录失败: %s", dstDir.c_str());
        return false;
    }
    DIR *dp = opendir(srcDir.c_str());
    if (dp == nullptr) return false;
    struct dirent *e;
    bool ok = true;
    while ((e = readdir(dp)) != nullptr) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        const std::string src = pathJoin(srcDir, e->d_name);
        const std::string dst = pathJoin(dstDir, e->d_name);
        struct stat es;
        if (stat(src.c_str(), &es) != 0) continue;
        if (S_ISDIR(es.st_mode)) {
            if (!copyTree(src, dst)) ok = false;
        } else if (S_ISREG(es.st_mode)) {
            if (!copyFileAtomic(src, dst)) {
                ESP_LOGW(TAG, "复制失败: %s", src.c_str());
                ok = false;
            }
        }
    }
    closedir(dp);
    return ok;
}

static int copyOneEntry(const BackupEntry &e, const std::string &backupRoot,
                        bool restoreDirection) {
    const std::string src = restoreDirection ? pathJoin(backupRoot, e.name) : std::string(e.src);
    const std::string dst = restoreDirection ? std::string(e.src) : pathJoin(backupRoot, e.name);
    struct stat s;
    if (stat(src.c_str(), &s) != 0) return 0;   // 这一项没有：跳过，不是错误
    if (e.dir) return copyTree(src, dst) ? 1 : -1;
    return copyFileAtomic(src, dst) ? 1 : -1;
}

// ── 时间戳（纯记录）──────────────────────────────────────────────────────
// 时钟没同步时 time() 还在 1970，写"1970-01-01"只会让人以为固件出错，所以那种情况
// 干脆写"时钟未同步"。这一行不参与任何校验，错了也照常能恢复。
static std::string nowStamp() {
    time_t now;
    time(&now);
    if (now < 1704067200) return "时钟未同步";   // 2024-01-01 之前一律视为未同步
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", tm);
    return ts;
}

static std::string infoPath() { return pathJoin(kLiveDir, kInfoName); }

std::string settings_backup_stamp() {
    FILE *f = fopen(infoPath().c_str(), "r");
    if (f == nullptr) return "";
    char line[128] = {0};
    std::string stamp;
    // 第一行是标题，第二行才是时间戳（见 saveBackupInfo）。
    while (fgets(line, sizeof(line), f) != nullptr) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.rfind("time=", 0) == 0) {
            stamp = s.substr(5);
            break;
        }
    }
    fclose(f);
    return stamp;
}

bool settings_backup_exists() {
    if (!dirExists(kLiveDir)) return false;
    // 正常备份都有 backup_info.txt；万一它没写成（磁盘满/老固件留下的目录），只要内容表
    // 里至少有一项在，也认 —— 判"能不能恢复"看的是内容，不是那张便条。
    if (!settings_backup_stamp().empty()) return true;
    struct stat s;
    for (int i = 0; i < kEntryCount; i++) {
        if (stat(pathJoin(kLiveDir, kEntries[i].name).c_str(), &s) == 0) return true;
    }
    return false;
}

static bool writeBackupInfo(const std::string &root) {
    const std::string path = pathJoin(root, kInfoName);
    char body[256];
    snprintf(body, sizeof(body), "pjournal-pico 配置备份\nversion=%s\ntime=%s\n",
             YAN_READER_VERSION, nowStamp().c_str());
    const std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (f == nullptr) return false;
    const size_t n = strlen(body);
    bool ok = fwrite(body, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;   // 落盘错误只在这里暴露（与 copyFileAtomic 同一套）
    if (!ok) {
        remove(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

// ── 对外 ────────────────────────────────────────────────────────────────
esp_err_t settings_backup_save() {
    if (!cardReady()) {
        ESP_LOGW(TAG, "保存备份: 卡没挂上");
        return ESP_ERR_INVALID_STATE;
    }
    if (!removeTree(kStageDir) || mkdir(kStageDir, 0777) != 0) {
        ESP_LOGE(TAG, "保存备份: 建 staging 失败");
        return ESP_FAIL;
    }
    int copied = 0;
    for (int i = 0; i < kEntryCount; i++) {
        const int r = copyOneEntry(kEntries[i], kStageDir, false);
        if (r > 0) copied++;
        else if (r < 0) {
            ESP_LOGE(TAG, "保存备份: 复制 %s 失败", kEntries[i].src);
            removeTree(kStageDir);
            return ESP_FAIL;
        }
    }
    writeBackupInfo(kStageDir);

    // 就位：旧备份先改名让路，staging 改上来，再删旧的。中间掉电最坏是"旧的还在，
    // 只是名字变成 .previous"——比"先删了旧的再改名失败，两边都没了"强。
    const std::string prev = std::string(kLiveDir) + ".previous";
    removeTree(prev);
    const bool rotated = (rename(kLiveDir, prev.c_str()) == 0);
    if (rename(kStageDir, kLiveDir) != 0) {
        ESP_LOGE(TAG, "保存备份: 就位失败");
        if (rotated) rename(prev.c_str(), kLiveDir);
        removeTree(kStageDir);
        return ESP_FAIL;
    }
    if (rotated) removeTree(prev);
    ESP_LOGI(TAG, "备份已保存: %s（%d 项）", kLiveDir, copied);
    return ESP_OK;
}

esp_err_t settings_backup_restore() {
    if (!cardReady()) {
        ESP_LOGW(TAG, "恢复备份: 卡没挂上");
        return ESP_ERR_INVALID_STATE;
    }
    const std::string prev = std::string(kLiveDir) + ".previous";
    std::string source = kLiveDir;
    if (!dirExists(source)) {
        if (!dirExists(prev)) return ESP_ERR_NOT_FOUND;
        source = prev;   // 上一次保存掉在"已改名、还没就位"那一拍：拿旧的那份
    }
    int restored = 0;
    for (int i = 0; i < kEntryCount; i++) {
        const int r = copyOneEntry(kEntries[i], source, true);
        if (r > 0) restored++;
        else if (r < 0) {
            ESP_LOGE(TAG, "恢复备份: 写回 %s 失败", kEntries[i].src);
            return ESP_FAIL;
        }
    }
    if (restored == 0) {
        ESP_LOGW(TAG, "恢复备份: 目录里一项都没有，文件不像备份");
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "备份已恢复: %s → 本机（%d 项）", source.c_str(), restored);
    return ESP_OK;
}
