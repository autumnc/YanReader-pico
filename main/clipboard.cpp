#include "clipboard.h"

#include "safe_file.h"

#include <esp_log.h>

#include <cstddef>
#include <vector>

static const char *TAG = "Clipboard";
static const char *kPath = "/sdcard/settings/clipboard.txt";
static constexpr size_t kClipboardFileMax = 64 * 1024;

static std::vector<std::string> s_items;   // s_items[0] = 最新
static bool s_loaded = false;
static bool s_lastTruncated = false;

// ── 转义 ────────────────────────────────────────────────────────────────
// 换行是"一条一行"的分隔符，必须转掉；反斜杠也要转，否则正文里本来就有的 "\n"
// 两个字面字符读回时会变成一个真换行。（阅读器笔记那份是同一套思路。）
static std::string escapeItem(const std::string &s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') continue;
        else o += c;
    }
    return o;
}

static std::string unescapeItem(const std::string &s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        const char n = s[i + 1];
        if (n == 'n') { o += '\n'; i++; }
        else if (n == '\\') { o += '\\'; i++; }
        else o += s[i];   // 认不出的转义原样保留，别把内容吃掉
    }
    return o;
}

// 按 UTF-8 边界截到 max 字节以内（别把最后一个字切碎）。
static std::string clampUtf8(const std::string &s, size_t max) {
    if (s.size() <= max) return s;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) cut--;
    return s.substr(0, cut);
}

static void loadFromDisk() {
    s_items.clear();
    bool truncated = false;
    const std::string raw = readWholeFile(kPath, kClipboardFileMax, &truncated);
    if (truncated) ESP_LOGW(TAG, "clipboard file too large, ignored: %s", kPath);
    size_t start = 0;
    for (;;) {
        const size_t nl = raw.find('\n', start);
        std::string line = (nl == std::string::npos) ? raw.substr(start)
                                                     : raw.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();  // 在电脑上编辑过的文件
        if (!line.empty()) s_items.push_back(unescapeItem(line));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (static_cast<int>(s_items.size()) > kClipboardMax) s_items.resize(kClipboardMax);
}

static void ensureLoaded() {
    if (s_loaded) return;
    s_loaded = true;
    loadFromDisk();
    ESP_LOGI(TAG, "loaded %d item(s)", static_cast<int>(s_items.size()));
}

static void saveToDisk() {
    std::string out;
    for (const auto &it : s_items) {
        out += escapeItem(it);
        out += '\n';
    }
    if (!safeWriteFile(kPath, out)) ESP_LOGW(TAG, "save failed: %s", kPath);
}

// ── API ────────────────────────────────────────────────────────────────
static const std::string kEmpty;

int clipboardCount() {
    ensureLoaded();
    return static_cast<int>(s_items.size());
}

const std::string &clipboardAt(int i) {
    ensureLoaded();
    if (i < 0 || i >= static_cast<int>(s_items.size())) return kEmpty;
    return s_items[i];
}

std::string clipboardLatest() { return clipboardAt(0); }

bool clipboardEmpty() { return clipboardCount() == 0; }

void clipboardPush(const std::string &text) {
    ensureLoaded();
    if (text.empty()) return;
    s_lastTruncated = false;
    std::string item = text;
    if (item.size() > static_cast<size_t>(kClipboardItemMax)) {
        item = clampUtf8(item, static_cast<size_t>(kClipboardItemMax));
        s_lastTruncated = true;
    }
    // 同内容提到最前：反复复制同一段别把历史挤满。
    for (size_t i = 0; i < s_items.size(); i++) {
        if (s_items[i] == item) { s_items.erase(s_items.begin() + i); break; }
    }
    s_items.insert(s_items.begin(), item);
    if (static_cast<int>(s_items.size()) > kClipboardMax) s_items.resize(kClipboardMax);
    saveToDisk();
}

void clipboardClear() {
    ensureLoaded();
    s_items.clear();
    saveToDisk();
}

void clipboardLoad() { ensureLoaded(); }

bool clipboardLastTruncated() { return s_lastTruncated; }
