#include "settings_manager.h"
#include "safe_file.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "Settings";
static const char *BASE_DIR = "/sdcard/settings";

SettingsManager g_settings;

static std::map<std::string, std::string> s_cache;
static std::mutex s_cacheMutex;

struct PendingSettingWrite {
    bool erase = false;
    std::string value;
};

static std::map<std::string, PendingSettingWrite> s_pendingWrites;
static bool s_flushTaskRunning = false;

// 串行化"真正落盘"那一段（取 pending + 逐个写文件）。两条 flush 路径 —— 700ms 后台
// 任务 settingsFlushTask 与显式 SettingsManager::flush()（备份/恢复/关机前调用）——
// 会并发走到这里；没有它就会同时写同一个 `<key>.tmp`，safeWriteFile 的 .tmp→rename
// 就不再原子（改前那份串行性来自 s_cacheMutex 一把锁盖住内存+落盘，拆成两段后漏了）。
// 顺带让 flush() 真的**等**：后台任务正在写时它堵在这儿，写完才回去取剩余 pending，
// 于是"备份前 flush()"不再只堵一半（原来后台已把 pending 抽走、还在写文件，flush()
// 看到空 pending 就返回，备份到卡上的仍是旧值）。
static std::mutex s_flushIOMutex;

static void settingsFlushPendingNow() {
    std::lock_guard<std::mutex> io(s_flushIOMutex);
    std::map<std::string, PendingSettingWrite> writes;
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        writes.swap(s_pendingWrites);
    }
    if (writes.empty()) return;
    mkdir(BASE_DIR, 0777);
    std::vector<std::pair<std::string, PendingSettingWrite>> failed;
    for (const auto &kv : writes) {
        const std::string path = std::string(BASE_DIR) + "/" + kv.first;
        if (kv.second.erase) {
            remove(path.c_str());
        } else if (!safeWriteFile(path, kv.second.value)) {
            ESP_LOGE(TAG, "Failed to write %s", path.c_str());
            failed.push_back({kv.first, kv.second});
        }
    }
    if (!failed.empty()) {
        // 写失败的**要放回 pending**。原来就地丢掉，于是这次改的值只活在 s_cache（内存）
        // 里、磁盘上还是旧值：本次会话看着一切正常，一重启就悄悄退回旧密码/旧 token，
        // 用户只会觉得"改了没生效"。放回后由显式 flush()（关机/备份前必调）或下一次
        // set() 再试。
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        for (auto &kv : failed) s_pendingWrites.emplace(kv.first, kv.second);
    }
}

static void settingsFlushTask(void *) {
    // 来回最多几趟。写失败的值会被上面放回 pending，所以"SD 一直写不进去"能让这个循环
    // 永远不停 —— 赌 8 趟（≈5.6s）之后收手，剩下的留给显式 flush()（关机/备份前一定调）
    // 或下一次 set()。收手时也必须清 s_flushTaskRunning，否则此后再也没人调度。
    for (int pass = 0; pass < 8; pass++) {
        vTaskDelay(pdMS_TO_TICKS(700));
        settingsFlushPendingNow();
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        if (s_pendingWrites.empty()) break;
    }
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        s_flushTaskRunning = false;
    }
    vTaskDelete(nullptr);
}

static void settingsScheduleFlush() {
    bool shouldStart = false;
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        if (!s_flushTaskRunning) {
            s_flushTaskRunning = true;
            shouldStart = true;
        }
    }
    if (!shouldStart) return;
    TaskHandle_t h = nullptr;
    if (xTaskCreate(settingsFlushTask, "settings_flush", 6144, nullptr, 1, &h) != pdPASS) {
        {
            std::lock_guard<std::mutex> lock(s_cacheMutex);
            s_flushTaskRunning = false;
        }
        settingsFlushPendingNow();
    }
}

bool SettingsManager::begin() {
    mkdir(BASE_DIR, 0777);
    ESP_LOGI(TAG, "Settings directory: %s", BASE_DIR);
    return true;
}

std::string SettingsManager::get(const std::string &key) {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    auto it = s_cache.find(key);
    if (it != s_cache.end()) return it->second;
    std::string path = std::string(BASE_DIR) + "/" + key;
    repairSafeWriteFile(path);
    FILE *f = fopen(path.c_str(), "r");
    if (!f) {
        // Cache the miss too: most keys are never written, and settings are only
        // ever created through set() (which updates the cache), so an absent key
        // stays absent. Without this, every read of such a key paid 3 stat() calls
        // plus a failed fopen on the SD card.
        s_cache[key] = "";
        return "";
    }
    std::string val;
    char buf[256];
    int n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        // 按长度 append：设置值里出现 0x00 时 `+= buf` 会截断（同 journal_storage）。
        val.append(buf, static_cast<size_t>(n));
    }
    fclose(f);
    s_cache[key] = val;
    return val;
}

void SettingsManager::set(const std::string &key, const std::string &val) {
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        s_cache[key] = val;
        s_pendingWrites[key] = PendingSettingWrite{false, val};
    }
    settingsScheduleFlush();
}

std::string SettingsManager::getString(const std::string &key, const std::string &def) {
    std::string v = get(key);
    return v.empty() ? def : v;
}

void SettingsManager::setString(const std::string &key, const std::string &val) {
    set(key, val);
}

void SettingsManager::erase(const std::string &key) {
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        s_cache[key] = "";
        s_pendingWrites[key] = PendingSettingWrite{true, ""};
    }
    settingsScheduleFlush();
}

void SettingsManager::flush() {
    settingsFlushPendingNow();
}

// Convenience accessors
std::string SettingsManager::flomoEmail() { return get("flomo_email"); }
std::string SettingsManager::flomoPassword() { return get("flomo_pass"); }
std::string SettingsManager::flomoToken() { return get("flomo_token"); }
std::string SettingsManager::webdavUrl() { return get("webdav_url"); }
std::string SettingsManager::webdavUsername() { return get("webdav_user"); }
std::string SettingsManager::webdavPassword() { return get("webdav_pass"); }
std::string SettingsManager::deepseekKey() { return get("deepseek_key"); }
std::string SettingsManager::polishPrompt() { return get("polish_prompt"); }
std::string SettingsManager::personalExperience() { return get("personal_exp"); }
std::string SettingsManager::personalHobbies() { return get("personal_hob"); }
std::string SettingsManager::wifiSsid() { return get("wifi_ssid"); }
std::string SettingsManager::wifiPassword() { return get("wifi_pass"); }

void SettingsManager::setFlomoEmail(const std::string &v) { set("flomo_email", v); }
void SettingsManager::setFlomoPassword(const std::string &v) { set("flomo_pass", v); }
void SettingsManager::setFlomoToken(const std::string &v) { set("flomo_token", v); }
void SettingsManager::setWebdavUrl(const std::string &v) { set("webdav_url", v); }
void SettingsManager::setWebdavUsername(const std::string &v) { set("webdav_user", v); }
void SettingsManager::setWebdavPassword(const std::string &v) { set("webdav_pass", v); }
void SettingsManager::setDeepseekKey(const std::string &v) { set("deepseek_key", v); }
void SettingsManager::setPolishPrompt(const std::string &v) { set("polish_prompt", v); }
void SettingsManager::setPersonalExperience(const std::string &v) { set("personal_exp", v); }
void SettingsManager::setPersonalHobbies(const std::string &v) { set("personal_hob", v); }
void SettingsManager::setWifiSsid(const std::string &v) { set("wifi_ssid", v); }
void SettingsManager::setWifiPassword(const std::string &v) { set("wifi_pass", v); }
std::string SettingsManager::timezone() { return get("timezone"); }
std::string SettingsManager::ntpServer() { return get("ntp_server"); }
void SettingsManager::setTimezone(const std::string &v) { set("timezone", v); }
void SettingsManager::setNtpServer(const std::string &v) { set("ntp_server", v); }
bool SettingsManager::autoSave() { return get("auto_save") == "1"; }
int SettingsManager::autoStandbyMinutes() {
    // 只认 0/5/10/15/20；没写过或写坏 → 10 分钟（就是原来那个固定超时，行为不变）。
    const int m = atoi(getString("reader_auto_standby", "10").c_str());
    if (m == 0 || m == 5 || m == 10 || m == 15 || m == 20) return m;
    return 10;
}
bool SettingsManager::sleepScreen() { return get("sleep_screen") == "1"; }  // default off(白屏)
bool SettingsManager::markdownRender() { return get("md_render") != "0"; }  // default on
bool SettingsManager::firstLineIndent() { return get("first_line_indent") == "1"; }  // default off
bool SettingsManager::versionHistory() { return get("version_history") == "1"; }  // default off
bool SettingsManager::recoveryDraft() { return get("recovery_draft") != "0"; }  // default on
// 全局蓝牙开关。**默认开**：键盘/遥控器是这台机器的主要输入手段，这条只给"彻底
// 关掉蓝牙省电/少干扰"的人用，界面入口是蓝牙管理页最上面那一行。
bool SettingsManager::btEnabled() { return get("bt_enabled") != "0"; }  // default on
bool SettingsManager::verticalReferenceLine() { return get("vertical_ref_line") == "1"; }  // default off

int SettingsManager::fontSize() {
    // UI 字号。20pt 是默认档（2026-10-03 从 22pt 降下来）；18pt 暂不暴露给设置，
    // 后续通过 SD 卡外置字体再扩展字号。阅读模式的 UI 字体也读这个值（见
    // screen_reader.cpp 的 g_uiFont），所以改这里两个模式的界面一起变。
    return 20;
}

std::string SettingsManager::appMode() { return getString("app_mode", "journal"); }

std::string SettingsManager::homeView() { return getString("home_view", "week"); }

std::string SettingsManager::inputMode() { return getString("input_mode", "normal"); }
std::string SettingsManager::imeFuzzy() { return getString("ime_fuzzy", "zcs"); }
std::string SettingsManager::imePredictMode() { return getString("ime_predict_mode", "always"); }
bool SettingsManager::imeSentence() { return getString("ime_sentence", "1") == "1"; }
std::string SettingsManager::imeMetaLetterSelKey() {
    std::string v = getString("ime_meta_letter_sel", "win");
    if (v == "1") return "win";   // 旧布尔值：开 = 左右任一 Win（改动前的行为）
    if (v == "0") return "off";   // 旧布尔值：关
    return v;
}
// **默认开**：这是用户主动要的功能（长按数字/标点出上档符号）。默认关的结果是
// 用户刷完机直接试、看到"长按还是重复原字符"，以为功能没做出来。想回到原来的
// "按下即出、按住重复"就在这里关掉（存 "0"）。
bool SettingsManager::kbLongPressSymbol() { return getString("long_press_symbol", "1") == "1"; }
bool SettingsManager::imeDocContext() { return getString("ime_doc_context", "1") == "1"; }
bool SettingsManager::imeCandidateHighlight() { return getString("ime_candidate_highlight", "0") == "1"; }
bool SettingsManager::imeDebug() { return getString("ime_debug", "0") == "1"; }
std::string SettingsManager::editorOrientation() { return getString("editor_orientation", "horizontal"); }
std::string SettingsManager::orientation() { return getString("orientation", "landscape"); }
void SettingsManager::setOrientation(const std::string &v) { setString("orientation", v); }
// 阅读器方向与全局屏幕方向各自独立：改这个不旋转主界面（否则设置界面本身会转过去），
// 只有阅读模式进/出时 applyReaderOrientation()/board_restore_orientation() 才应用。
// **默认竖屏**：这台机器是当竖读器用的（正文 684 宽 × 1216 高），横屏只留给编辑器/主界面。
// 改默认值之前存过这个键的用户不受影响（存的还是他挑的那档）；没碰过这项的从竖屏起。
std::string SettingsManager::readerOrientation() { return getString("reader_orientation", "portrait"); }
void SettingsManager::setReaderOrientation(const std::string &v) { setString("reader_orientation", v); }
// 全设备夜间反色。旧键 reader_night 是阅读器专用的，首次读不到新键时回退过去，
// 免得用户之前开的夜间就这么丢了。
bool SettingsManager::nightMode() {
    std::string v = getString("night_mode", "");
    if (v.empty()) v = getString("reader_night", "0");
    return v == "1";
}
void SettingsManager::setNightMode(bool on) { setString("night_mode", on ? "1" : "0"); }
std::string SettingsManager::verticalReferenceLineStyle() { return getString("vertical_ref_line_style", "solid"); }

bool SettingsManager::typingClickEnabled() {
    // 按键反馈音任何模式都能开(见 typing_click 的 enabled)。
    // 但"默认开"只保留在打字机模式下：没设置过的机器在正常模式里不该凭空出声，
    // 想开的人去设置里打开一次即可(那一下会写入 "1"，之后两种模式都生效)。
    std::string v = get("click_enabled");
    if (v.empty()) return inputMode() == "typewriter";
    return v != "0";
}

std::string SettingsManager::clickChineseMode() { return getString("click_chinese", "key"); }

std::string SettingsManager::imeCleanMode() { return getString("ime_clean", "punct"); }

std::string SettingsManager::imeCommitMode() { return getString("ime_commit_mode", "solid"); }

// 「打字残影」：编码区+候选区自己那一拍（逐键、矩形只有输入法那两行）用几张相的跟随表。
// 默认 "8" = 现状（跟随表），与改动前逐字节一致。见 ui_render.cpp 的 ime_ghost_mode_policy。
// **这个键不是"只写就生效"**：除写键还得调 reader_set_ime_frames() 重建共享表。
// 老机器上存过 "0"/"1"/"2"（已下线的那三档）→ 认不出 → 8，等于回到现状，不用迁移。
std::string SettingsManager::imeGhostMode() { return getString("ime_ghost_mode", "8"); }

int SettingsManager::typingClickVolume() {
    // 默认 100：duty 映射的 legal_peak 就是满摆幅，以前默认 80 白扔了 20% 动态范围。
    // makeHit 归一化到满摆幅，所以 100% 也不会撞占空比两端削波。
    std::string v = get("click_volume");
    if (v.empty()) return 100;
    int n = atoi(v.c_str());
    if (n < 0 || n > 100) return 100;
    return n;
}

int SettingsManager::dailyGoalMinutes() {
    // 阅读统计的每日目标。达到目标的那一天才算"达标日"(连续天数/热力图勾选都看它)。
    std::string v = get("daily_goal");
    if (v.empty()) return 30;
    int n = atoi(v.c_str());
    if (n < 5 || n > 600) return 30;
    return n;
}
