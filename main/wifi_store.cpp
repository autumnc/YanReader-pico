#include "wifi_store.h"

#include "safe_file.h"
#include "settings_manager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <esp_log.h>

static const char *TAG = "WifiStore";

// 与 reader_progress.txt / reader_notes.txt 同目录同风格（SD 根、纯文本、一行一条）。
static const char *kWifiStorePath = "/sdcard/wifi_networks.txt";

// 字段分隔符。与阅读器那两张表用的 \x1f 同一个字节（行内不可能自然出现，且下面
// escapeField 会把正文里真的 \x1f 转义掉，所以按它切分是无歧义的）。
static const char kSep = '\x1f';

WifiStore g_wifiStore;

// ── 纯函数：转义 / 反转义 ────────────────────────────────────────────────
// 比阅读器那套 rdNoteEscape 更彻底：WIFI 密码里出现反斜杠、用户从别处粘贴进来的
// 制表符/换行都可能，所以把"所有不可打印字节"统一成 \xHH，反斜杠转义成 \\。
// **不做任何 trim**：SSID 首尾带空格是真实存在（手机热点），connect() 那边还专门
// 为它打过一条日志（见 wifi_manager.cpp），这里一 trim 就把那种网静默弄坏了。
std::string WifiStore::escapeField(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c < 0x20 || c == 0x7f) {
            char buf[5];
            snprintf(buf, sizeof(buf), "\\x%02x", c);
            out += buf;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string WifiStore::unescapeField(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out.push_back(s[i]);
            continue;
        }
        const char n = s[i + 1];
        if (n == '\\') {
            out.push_back('\\');
            i++;
        } else if (n == 'x' && i + 3 < s.size()) {
            const int hi = hexVal(s[i + 2]), lo = hexVal(s[i + 3]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 3;
            } else {
                out.push_back(s[i]);   // 不是合法 \xHH：原样留着（容错，不吞字符）
            }
        } else {
            out.push_back(s[i]);       // 未知转义：原样留着
        }
    }
    return out;
}

// ── 纯函数：序列化 / 解析 ────────────────────────────────────────────────
// 一行 = ssid \x1f pass \x1f flags \x1f lastUsed，行尾 \n。flags bit0 = 开放网络。
// 行序 = 最近用过的在前（与 reader_progress.txt 一样，"顺序本身就是信息"）。
std::string WifiStore::serialize(const std::vector<WifiSavedNet> &v) {
    std::string out;
    for (const auto &n : v) {
        out += escapeField(n.ssid);
        out.push_back(kSep);
        out += escapeField(n.pass);
        out.push_back(kSep);
        out += n.open ? "1" : "0";
        out.push_back(kSep);
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n.lastUsed));
        out += buf;
        out.push_back('\n');
    }
    return out;
}

std::vector<WifiSavedNet> WifiStore::parse(const std::string &content) {
    std::vector<WifiSavedNet> out;
    size_t pos = 0;
    while (pos <= content.size()) {
        size_t nl = content.find('\n', pos);
        const size_t end = (nl == std::string::npos) ? content.size() : nl;
        std::string line = content.substr(pos, end - pos);
        pos = (nl == std::string::npos) ? content.size() + 1 : nl + 1;

        if (!line.empty() && line.back() == '\r') line.pop_back();   // 容错 CRLF
        if (line.empty()) continue;

        // 按裸 \x1f 切成 4 段。字段内部不可能有裸 \x1f（escapeField 转义过），所以
        // 这个切分是无歧义的。段数不对的行整行跳过（容错：宁可丢一条也不整个表作废）。
        std::vector<std::string> parts;
        size_t start = 0;
        for (;;) {
            const size_t sep = line.find(kSep, start);
            if (sep == std::string::npos) {
                parts.push_back(line.substr(start));
                break;
            }
            parts.push_back(line.substr(start, sep - start));
            start = sep + 1;
        }
        if (parts.size() < 4) continue;

        WifiSavedNet n;
        n.ssid = unescapeField(parts[0]);
        if (n.ssid.empty()) continue;   // 空 SSID 不是合法网络
        n.pass = unescapeField(parts[1]);
        n.open = (atoi(parts[2].c_str()) & 1) != 0 || n.pass.empty();
        n.lastUsed = atoll(parts[3].c_str());
        out.push_back(std::move(n));
    }
    return out;
}

// ── 文件 IO ──────────────────────────────────────────────────────────────
void WifiStore::load() const {
    if (loaded_) return;
    loaded_ = true;
    nets_.clear();

    const std::string content = readWholeFile(kWifiStorePath);
    if (!content.empty()) nets_ = parse(content);

    // 迁移：本仓库原来只有一对 wifi_ssid/wifi_pass。表为空而旧键非空 → 收进内存里的表，
    // **先不写盘**（打开一次 WiFi 管理页不该动 SD 卡）；等第一次 remember/forget 再落盘。
    // 迁移只在内存里做也安全：下次开机 load() 会照旧从旧键再合成一遍，幂等。
    if (nets_.empty()) {
        const std::string ssid = g_settings.wifiSsid();
        if (!ssid.empty()) {
            WifiSavedNet n;
            n.ssid = ssid;
            n.pass = g_settings.wifiPassword();
            n.open = n.pass.empty();
            nets_.push_back(std::move(n));
        }
    }
}

bool WifiStore::save() const {
    if (!safeWriteFile(kWifiStorePath, serialize(nets_))) {
        ESP_LOGE(TAG, "写入 %s 失败", kWifiStorePath);
        return false;
    }
    return true;
}

// ── 表操作 ───────────────────────────────────────────────────────────────
const WifiSavedNet *WifiStore::at(int i) const {
    load();
    if (i < 0 || i >= static_cast<int>(nets_.size())) return nullptr;
    return &nets_[i];
}

bool WifiStore::has(const char *ssid) const { return find(ssid) != nullptr; }

const WifiSavedNet *WifiStore::find(const char *ssid) const {
    if (!ssid) return nullptr;
    load();
    for (const auto &n : nets_)
        if (n.ssid == ssid) return &n;
    return nullptr;
}

bool WifiStore::remember(const char *ssid, const char *pass) {
    if (!ssid || !*ssid) return false;
    load();

    std::string s(ssid);
    if (s.size() > 32) s.resize(32);          // 与 wifi_config_t.ssid 同宽
    std::string p = pass ? pass : "";
    if (p.size() > 63) p.resize(63);          // 与 wifi_config_t.password 同宽

    for (size_t i = 0; i < nets_.size(); i++) {
        if (nets_[i].ssid == s) { nets_.erase(nets_.begin() + i); break; }
    }
    WifiSavedNet n;
    n.ssid = s;
    n.pass = p;
    n.open = p.empty();
    n.lastUsed = static_cast<int64_t>(time(nullptr));
    nets_.insert(nets_.begin(), std::move(n));
    if (static_cast<int>(nets_.size()) > kMax) nets_.resize(kMax);   // 丢最久没用过的

    return save();
}

void WifiStore::touch(const char *ssid) {
    if (!ssid || !*ssid) return;
    load();
    for (size_t i = 0; i < nets_.size(); i++) {
        if (nets_[i].ssid == ssid) {
            WifiSavedNet n = nets_[i];
            n.lastUsed = static_cast<int64_t>(time(nullptr));
            nets_.erase(nets_.begin() + i);
            nets_.insert(nets_.begin(), std::move(n));
            return;   // 只刷新顺序/时间戳，不落盘
        }
    }
}

bool WifiStore::forget(const char *ssid) {
    if (!ssid || !*ssid) return false;
    load();
    for (size_t i = 0; i < nets_.size(); i++) {
        if (nets_[i].ssid == ssid) {
            nets_.erase(nets_.begin() + i);
            save();
            return true;
        }
    }
    return false;
}

std::string WifiStore::mostRecentSsid() const {
    load();
    return nets_.empty() ? std::string() : nets_[0].ssid;
}

std::string WifiStore::mostRecentPass() const {
    load();
    return nets_.empty() ? std::string() : nets_[0].pass;
}
