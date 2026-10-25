#include "screen_appstore.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <HalStorage.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "app_async.h"
#include "app_services.h"
#include "crossstore/store_package.h"
#include "font_renderer.h"
#include "fb_fast.h"
#include "hw/input.h"
#include "screen_luaapp.h"
#include "settings_manager.h"
#include "ui_helpers.h"
#include "ui_render.h"
#include "wifi_manager.h"

namespace {

using namespace crossstore;

// ── 图标缓存 ─────────────────────────────────────────────────────────────
// 目录图标 .csi 的 16 级 96px 变体（4608 字节/枚）。槽位与当前页的格子一一对应：
// 换页/换标签时整批作废。放 PSRAM；分不到就整块不画图标，退化成占位。
constexpr int kIconSource = 96;
constexpr size_t kIconBytes = kIconSource * kIconSource / 2;
constexpr int kMaxCells = 24;  // 竖屏 3 列 × 7 行 + 余量

struct IconCache {
    uint8_t *pixels = nullptr;
    char keys[kMaxCells][72] = {};
    uint8_t state[kMaxCells] = {};  // 0=空 1=就绪 2=失败 3=正在下载
};

struct StoreState {
    bool initialized = false;
    bool dirty = true;

    enum class View { Grid, Detail, Confirm, Busy, Message } view = View::Grid;
    enum class Tab { Catalog, Installed } tab = Tab::Catalog;
    enum class MsgReturn { Grid, Detail } msgReturn = MsgReturn::Grid;

    int sel = 0;
    int detailAction = 0;
    int confirmSel = 1;  // 默认停在"取消"，防误触
    bool confirmUninstall = false;
    bool confirmIsUpdate = false;
    CatalogEntry confirmEntry;
    std::string confirmLabel;

    std::string source;      // 当前目录页 URL
    std::string pendingUrl;  // job 正在抓的 URL
    CatalogPage page;
    bool pageLoaded = false;
    bool pageFromCache = false;

    std::vector<InstalledApp> installed;
    std::vector<std::string> pageStack;

    std::string status;   // 标题下的小字
    std::string message;  // Message 视图
    std::string messageSub;
    bool loadAttempted = false;      // 本次目录已发起过一次加载（失败别每拍重试）
    bool autoRefreshIssued = false;  // 缓存页已发起过一次后台刷新

    // 详情页数据（目录条目与已安装条目统一成这一份）
    struct Detail {
        Package pkg;
        std::string artifact;
        std::string sha256;
        uint32_t bytes = 0;
        bool valid = false;
        bool catalog = false;  // 来自目录（可安装/更新）
        bool installed = false;
    } detail;

    int progress = -1;
    unsigned long long progressGot = 0, progressTotal = 0;
};

StoreState s;
IconCache s_icons;

// ── 后台任务 ─────────────────────────────────────────────────────────────
AppAsyncJob s_job;
SemaphoreHandle_t s_mutex = nullptr;

enum class JobKind { None, Catalog, Icon, Install };
JobKind s_jobKind = JobKind::None;
std::string s_jobUrl;
CatalogEntry s_jobEntry;
std::string s_jobBody;
std::string s_jobError;
std::string s_jobStatus;
bool s_jobOk = false;
// 唯一的工作槽被图标/目录任务占着时，用户点了"安装"：把这次安装记在这里，
// 等当前任务的结果被 consumeJob 收走后再接手。否则 startJob 静默返回 false，
// Busy 屏就永远停在"正在处理…"（图标下载最长能等 120s，用户完全来得及点进来）。
bool s_pendingInstall = false;
CatalogEntry s_pendingEntry;
std::atomic<bool> s_wifiConnectedByUs{false};
std::atomic<int> s_dlPercent{-1};
std::atomic<unsigned long long> s_dlGot{0}, s_dlTotal{0};

void lock() {
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
}
void unlock() {
    if (s_mutex) xSemaphoreGive(s_mutex);
}
void setJobStatus(const char *text) {
    lock();
    s_jobStatus = text ? text : "";
    unlock();
}
std::string jobStatus() {
    lock();
    const std::string out = s_jobStatus;
    unlock();
    return out;
}

void downloadProgress(void *, size_t got, size_t total, int percent) {
    s_dlGot.store(got, std::memory_order_relaxed);
    s_dlTotal.store(total, std::memory_order_relaxed);
    s_dlPercent.store(percent, std::memory_order_relaxed);
}

std::string wifiEmptyHint() { return "未配置 WiFi，只能查看已安装应用"; }

// 这次进店值不值得发起网络请求：已经连着，或设置里至少存了一个 SSID。
// 只判 SSID 不判密码 —— 开放网络本来就是空密码。没有账号时静默用缓存，
// 免得每次进店都跑一趟注定失败的 10 秒连接。
bool catalogNetworkPossible() {
    return g_wifi.isConnected() || !g_settings.wifiSsid().empty();
}

// 任务体里**不要**留跨 vTaskDelete 的 C++ 对象：任务函数只调 body，body 返回后
// 局部量已析构，再 vTaskDelete（见 freertos-vtaskdelete-no-cpp-unwind）。
void catalogBody() {
    std::string url;
    lock();
    url = s_jobUrl;
    unlock();

    setJobStatus(g_wifi.isConnected() ? "正在加载目录…" : "正在连接 WiFi…");
    if (!g_wifi.isConnected()) {
        // 失败有两种，别混成一句「未配置」：根本没存过 SSID 才是未配置；存过却连不上
        // （密码错 / AP 不在范围）要把驱动给的原因说出来，否则用户看到"WiFi 明明连着"
        // 却被告知没配置。
        const std::string ssid = g_settings.wifiSsid();
        bool was = false;   // 走到这里本来就没连；留着接 app_connect_wifi_from_settings 的回填
        if (!app_connect_wifi_from_settings(&was)) {
            lock();
            s_jobOk = false;
            s_jobError = ssid.empty() ? wifiEmptyHint()
                                      : std::string("WiFi 连接失败：") + g_wifi.lastReasonText();
            s_jobBody.clear();
            unlock();
            return;
        }
        if (!was) s_wifiConnectedByUs.store(true, std::memory_order_relaxed);
        setJobStatus("正在加载目录…");
    }
    std::string body, error;
    const bool ok = fetchCatalogBody(url, body, error);
    lock();
    s_jobOk = ok;
    s_jobBody.swap(body);
    s_jobError = ok ? std::string() : (error.empty() ? "目录加载失败" : error);
    unlock();
}

void iconBody() {
    CatalogEntry entry;
    lock();
    entry = s_jobEntry;
    unlock();
    std::string error;
    const bool ok = ensureCatalogIcon(entry, true, error);
    lock();
    s_jobOk = ok;
    s_jobError = ok ? std::string() : error;
    unlock();
}

void installBody() {
    CatalogEntry entry;
    lock();
    entry = s_jobEntry;
    unlock();

    std::string error;
    bool ok = false;
    size_t bytes = 0;
    setJobStatus("正在下载…");
    if (downloadFile(entry.artifact, DOWNLOAD_PATH, MAX_PACKAGE_BYTES, bytes, downloadProgress, nullptr, error)) {
        if (bytes != entry.bytes) {
            error = "下载大小不符";
        } else {
            setJobStatus("正在校验…");
            if (!verifyHash(DOWNLOAD_PATH, entry.bytes, entry.sha256.c_str())) {
                error = "校验失败，包可能已损坏";
            } else {
                setJobStatus("正在安装…");
                ok = installDownloaded(DOWNLOAD_PATH, entry, error);
            }
        }
    }
    Storage.remove(DOWNLOAD_PATH);
    lock();
    s_jobOk = ok;
    s_jobError = ok ? std::string() : (error.empty() ? "安装失败" : error);
    unlock();
}

void jobTask(void *) {
    JobKind kind;
    lock();
    kind = s_jobKind;
    unlock();
    switch (kind) {
        case JobKind::Catalog: catalogBody(); break;
        case JobKind::Icon: iconBody(); break;
        case JobKind::Install: installBody(); break;
        default: break;
    }
    s_job.finish("done");
    // PSRAM 栈的任务不能自删（vTaskDelete 不释放 WithCaps 静态分配的栈/TCB，自删
    // 路径还会临时建清理任务，内部 RAM 紧时 abort）。挂起自己，等主任务收尸。
    if (s_job.createdWithCaps())
        vTaskSuspend(nullptr);
    else
        vTaskDelete(nullptr);
}

bool startJob(JobKind kind, const char *name) {
    if (s_job.state() == AppAsyncState::Running) return false;
    lock();
    s_jobKind = kind;
    s_jobOk = false;
    s_jobError.clear();
    s_jobStatus.clear();
    unlock();
    s_job.reapCapsTask();   // 上一轮若已 Done 但还没被消费（异常路径），先收掉
    s_job.begin("应用商店", "");
    s_dlPercent.store(-1, std::memory_order_relaxed);
    s_dlGot.store(0, std::memory_order_relaxed);
    s_dlTotal.store(0, std::memory_order_relaxed);
    // 栈放 PSRAM（7680 字节）。这不是为了省那点内部 RAM，而是**死锁式的二选一**：
    // 商店的任务自己要去连 WiFi，而连上 WiFi 后内部 RAM 最大连续块只剩 ~1.5KB
    // （实测 free=3179 largest=1472，蓝牙键盘开着）；反过来先占一块 7680 的内部栈，
    // 内部只剩 ~1.7KB，WiFi 握手分配 probe/assoc 缓冲时直接 ALLOC FAILED，最后
    // 报 reason=2 超时、「WiFi 连接失败」。任务只在 SD 上读写、走网络，不在禁用
    // cache 期间运行，满足外部栈任务的前提。
    if (!s_job.start(jobTask, name, 7680, nullptr, 1, /*psramStack=*/true)) {
        lock();
        s_jobKind = JobKind::None;
        s_jobOk = false;
        s_jobError = "系统繁忙，请稍后重试";
        unlock();
        s_job.finish("failed");
        return false;
    }
    return true;
}

// ── 数据小工具 ───────────────────────────────────────────────────────────
const InstalledApp *installedById(const char *id) {
    for (const auto &app : s.installed)
        if (!std::strcmp(app.package.id, id)) return &app;
    return nullptr;
}

void reloadInstalled() {
    scanInstalled(s.installed);
    s.dirty = true;
}

void clearIcons() {
    std::memset(s_icons.keys, 0, sizeof(s_icons.keys));
    std::memset(s_icons.state, 0, sizeof(s_icons.state));
}

void allocIcons() {
    if (s_icons.pixels) return;
    s_icons.pixels = static_cast<uint8_t *>(
        heap_caps_malloc(static_cast<size_t>(kMaxCells) * kIconBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    clearIcons();
}

void freeIcons() {
    if (s_icons.pixels) heap_caps_free(s_icons.pixels);
    s_icons.pixels = nullptr;
}

std::string iconKeyForCatalog(const CatalogEntry &e) {
    if (!e.iconSha256.empty()) return e.iconSha256;
    return std::string("id:") + e.package.id;
}

std::string iconKeyForInstalled(const InstalledApp &a) {
    return std::string("in:") + a.package.id + ":" + a.package.version;
}

int itemCount() {
    if (s.tab == StoreState::Tab::Catalog) return s.pageLoaded ? static_cast<int>(s.page.entries.size()) : 0;
    return static_cast<int>(s.installed.size());
}

int gridCols() { return SCREEN_W >= 1000 ? 4 : 3; }
// 竖屏 3 列能放下 96px（目录图标源就是 96，1:1 最清楚）；横屏只有 3 行高，
// 再大就掉到 2 行，16 条目一页要滚很久，所以横屏停在 68。
int gridIconPx() { return SCREEN_W >= 1000 ? 68 : 96; }

// ── 字体/文本小工具 ──────────────────────────────────────────────────────
// 商店自己的字号档（与界面字号无关：这里是一整屏应用内容，用户反馈原来的
// 30/22/29/38 太小）。格高就是行高，改成别的值时必须回头核 gridCellH /
// 详情页的 description 起点、确认框的按钮位置。
constexpr int kNamePx = 44;
constexpr int kSubPx = 32;
constexpr int kBodyPx = 40;
constexpr int kHeadPx = 52;

int pxAscent(int px) {
    FontScope scope(px);
    return g_font.ascent();
}
int pxLine(int px) {
    FontScope scope(px);
    return g_font.lineHeight();
}
int pxWidth(int px, const char *text) {
    FontScope scope(px);
    return g_font.textWidth(text);
}
void drawTextPx(int px, int x, int baseline, const char *text, bool bold = false, bool invert = false) {
    FontScope scope(px);
    if (!bold && !invert) {
        g_font.drawText(x, baseline, text);
    } else {
        TextStyle ts;
        ts.bold = bold;
        ts.invert = invert;
        g_font.drawTextStyled(x, baseline, text, ts);
    }
}
void drawCenteredPx(int px, int cx, int baseline, const char *text, bool bold = false, bool invert = false) {
    drawTextPx(px, cx - pxWidth(px, text) / 2, baseline, text, bold, invert);
}

std::string fitText(int px, const std::string &text, int maxWidth) {
    FontScope scope(px);
    if (g_font.textWidth(text.c_str()) <= maxWidth) return text;
    size_t end = text.size();
    while (end > 0) {
        do {
            --end;
        } while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80);
        const std::string candidate = text.substr(0, end) + "…";
        if (g_font.textWidth(candidate.c_str()) <= maxWidth) return candidate;
    }
    return "…";
}

void wrapText(int px, const char *text, int maxWidth, int maxLines, std::vector<std::string> &out) {
    out.clear();
    if (!text || !*text) return;
    FontScope scope(px);
    std::string line;
    const char *p = text;
    while (*p && static_cast<int>(out.size()) < maxLines) {
        if (*p == '\n') {
            out.push_back(line);
            line.clear();
            ++p;
            continue;
        }
        const char *start = p;
        const uint32_t cp = FontRenderer::utf8Decode(p);
        const std::string glyph(start, p - start);
        if (cp == '\r') continue;
        if (g_font.textWidth((line + glyph).c_str()) > maxWidth && !line.empty()) {
            // 优先在最后一个空格处断行；没有空格（CJK）就硬断。
            const size_t space = line.find_last_of(' ');
            if (space != std::string::npos && space > 0) {
                out.push_back(line.substr(0, space));
                line = line.substr(space + 1);
                if (static_cast<int>(out.size()) >= maxLines) break;
            } else {
                out.push_back(line);
                line.clear();
                if (static_cast<int>(out.size()) >= maxLines) break;
            }
        }
        if (line.empty() && glyph == " ") continue;
        line += glyph;
    }
    if (!line.empty() && static_cast<int>(out.size()) < maxLines) out.push_back(line);
    if (static_cast<int>(out.size()) == maxLines && p && *p) {
        if (!out.empty()) {
            std::string last = out.back();
            while (!last.empty() && g_font.textWidth((last + "…").c_str()) > maxWidth) {
                size_t end = last.size();
                do {
                    --end;
                } while (end > 0 && (static_cast<unsigned char>(last[end]) & 0xC0) == 0x80);
                last.resize(end);
            }
            out.back() = last + "…";
        }
    }
}

// ── 版式 ─────────────────────────────────────────────────────────────────
int headerHeight() {
    FontScope ui(FontRenderer::uiPxHeight());
    return g_font.lineHeight() + 16;
}
int smallFooterPx() { return std::max(24, FontRenderer::uiPxHeight() * 2 / 3); }
int footerHeight() { return pxLine(smallFooterPx()) + 12; }
int footerTop() { return SCREEN_H - footerHeight(); }

int statusLineHeight() { return s.status.empty() ? 0 : pxLine(kSubPx) + 4; }
int bodyTop() { return headerHeight() + statusLineHeight() + 6; }

int gridRows(int cellH) {
    const int bottom = footerTop() - (s.tab == StoreState::Tab::Catalog && s.pageLoaded &&
                                              (!s.page.next.empty() || !s.page.previous.empty())
                                          ? 46
                                          : 4);
    return std::max(1, (bottom - bodyTop()) / std::max(1, cellH));
}
// 格子内容从图标上沿到副标题下沿 = icon + 10 + 名称行 + 副标题行；只留 8px 行距，
// 否则竖屏 96px 图标 + 44/32 号字只能摆 4 行。
int gridCellH() { return gridIconPx() + 10 + kNamePx + 2 + kSubPx + 8; }

// 「已安装」可能多于屏上能放的格数（目录每页 ≤16，装得下；已安装上限 64，装不下）。
// 选中项始终保持在视口内：以选中行为中心滚动，滚到首尾时贴边。
// 目录标签的项数不会超过容量，这个式子退化为 0。
int gridStart(int count, int cols, int rows) {
    if (count <= 0 || cols <= 0 || rows <= 0) return 0;
    const int selRow = std::max(0, std::min(s.sel, count - 1)) / cols;
    const int maxStartRow = std::max(0, (count + cols - 1) / cols - rows);
    const int startRow = std::max(0, std::min(selRow - rows / 2, maxStartRow));
    return startRow * cols;
}

// 底部动作按钮（详情/确认共用）。返回 y 与高。
void actionGeometry(int count, int &y, int &h) {
    h = 56;
    y = footerTop() - h - 8;
    (void)count;
}
void actionRect(int index, int count, int &x, int &y, int &w, int &h) {
    const int margin = 16, gap = 10;
    actionGeometry(count, y, h);
    w = (SCREEN_W - 2 * margin - gap * (count - 1)) / std::max(1, count);
    x = margin + index * (w + gap);
}

// ── 图标绘制 ─────────────────────────────────────────────────────────────
void drawPlaceholderIcon(int x, int y, int box, const std::string &name, bool invert) {
    uint8_t *fb = ui_render_begin_frame();
    if (!fb) return;
    fb_fast_sync();
    // 外框 + 首字：目录图标没下载下来时也要看得出这是一枚应用。
    const uint8_t fg = invert ? 0 : 0;  // 占位一律黑线；选中时黑底反白框
    (void)fg;
    const int t = std::max(2, box / 22);
    if (invert) fb_fast_fill_rect(fb, x, y, box, box, 0x00);
    fb_fast_fill_rect(fb, x, y, box, t, invert ? 0xF0 : 0x00);
    fb_fast_fill_rect(fb, x, y + box - t, box, t, invert ? 0xF0 : 0x00);
    fb_fast_fill_rect(fb, x, y, t, box, invert ? 0xF0 : 0x00);
    fb_fast_fill_rect(fb, x + box - t, y, t, box, invert ? 0xF0 : 0x00);
    std::string initial;
    const char *p = name.c_str();
    if (*p) {
        const char *start = p;
        FontRenderer::utf8Decode(p);
        initial.assign(start, p - start);
    }
    if (!initial.empty()) {
        const int px = std::max(20, box * 2 / 5);
        FontScope scope(px);
        const int w = g_font.textWidth(initial.c_str());
        drawTextPx(px, x + (box - w) / 2, y + (box + g_font.ascent()) / 2, initial.c_str(), false, invert);
    }
}

// 4bpp 行优先 MSB-first：像素 i 的高半字节在前。v=0 黑、15 白。
void drawPackedIcon(const uint8_t *pixels, int x, int y, int box, bool invert) {
    if (!pixels || box <= 0) return;
    uint8_t *fb = ui_render_begin_frame();
    if (!fb) return;
    fb_fast_sync();
    for (int dy = 0; dy < box; ++dy) {
        const int sy = dy * kIconSource / box;
        for (int dx = 0; dx < box; ++dx) {
            const int sx = dx * kIconSource / box;
            const int idx = sy * kIconSource + sx;
            uint8_t v = (pixels[idx >> 1] >> ((idx & 1) ? 0 : 4)) & 0x0F;
            if (invert) v = static_cast<uint8_t>(15 - v);
            fb_fast_set_gray(fb, x + dx, y + dy, v);
        }
    }
}

// ── 图标按需加载 ─────────────────────────────────────────────────────────
int slotFor(int cell) {
    return cell >= 0 && cell < kMaxCells ? cell : -1;
}

int slotForKey(const std::string &key) {
    for (int i = 0; i < kMaxCells; ++i)
        if (!std::strcmp(s_icons.keys[i], key.c_str())) return i;
    return -1;
}

// 一次只做一枚：本地的直接读，网络的起后台任务。返回是否改变了画面。
bool loadOneIcon() {
    if (s.view != StoreState::View::Grid || !s_icons.pixels) return false;
    if (s_job.state() == AppAsyncState::Running) return false;

    const int cols = gridCols();
    const int rows = gridRows(gridCellH());
    const int count = itemCount();
    const int start = gridStart(count, cols, rows);
    const int capacity = std::min(count - start, rows * cols);
    for (int j = 0; j < capacity; ++j) {
        const int i = start + j;
        // 与 renderGrid 同一套映射：槽位 = 屏上格位，条目下标从滚动起点算。
        const int slot = slotFor(j);
        if (slot < 0) continue;
        std::string key;
        const CatalogEntry *ce = nullptr;
        const InstalledApp *ia = nullptr;
        if (s.tab == StoreState::Tab::Catalog) {
            if (i >= static_cast<int>(s.page.entries.size())) continue;
            ce = &s.page.entries[i];
            key = iconKeyForCatalog(*ce);
        } else {
            if (i >= static_cast<int>(s.installed.size())) continue;
            ia = &s.installed[i];
            key = iconKeyForInstalled(*ia);
        }
        if (std::strcmp(s_icons.keys[slot], key.c_str()) != 0) {
            std::snprintf(s_icons.keys[slot], sizeof(s_icons.keys[slot]), "%s", key.c_str());
            s_icons.state[slot] = 0;
        }
        if (s_icons.state[slot] != 0) continue;

        uint8_t *dst = s_icons.pixels + static_cast<size_t>(slot) * kIconBytes;
        size_t bytes = 0;
        if (s.tab == StoreState::Tab::Installed) {
            if (readInstalledIcon(ia->package.id, 16, 96, dst, kIconBytes, bytes) && bytes == kIconBytes) {
                s_icons.state[slot] = 1;
                s.dirty = true;
            } else {
                s_icons.state[slot] = 2;
            }
            return true;
        }
        // 目录页：先看磁盘缓存（.csi），没有再看能不能联网抓。
        if (!ce->iconArtifact.empty() && !ce->iconSha256.empty()) {
            const std::string cache = iconCachePath(ce->iconSha256);
            if (readBundleIconFile(cache.c_str(), 16, 96, dst, kIconBytes, bytes) && bytes == kIconBytes) {
                s_icons.state[slot] = 1;
                s.dirty = true;
                return true;
            }
        }
        if (ce->iconArtifact.empty() || ce->iconSha256.empty()) {
            s_icons.state[slot] = 2;
            return true;
        }
        if (!g_wifi.isConnected()) {
            s_icons.state[slot] = 2;  // 没有网络：本次先占位，刷新/换页再试
            return true;
        }
        lock();
        s_jobEntry = *ce;
        unlock();
        s_icons.state[slot] = 3;
        if (!startJob(JobKind::Icon, "store_icon")) s_icons.state[slot] = 2;
        return true;
    }
    return false;
}

// ── 目录加载 ─────────────────────────────────────────────────────────────
void startCatalogFetch(const std::string &url) {
    lock();
    s_jobUrl = url;
    s.pendingUrl = url;
    unlock();
    if (s_job.state() == AppAsyncState::Running) {
        // 图标任务占着槽：把"已发起"的闸放开，任务结束后的空闲帧会重发。
        // 以前这里直接 startJob 拿 false 就丢，界面会一直停在"正在加载目录…"。
        s.loadAttempted = false;
        return;
    }
    if (!startJob(JobKind::Catalog, "store_catalog")) {
        std::string err;
        lock();
        err = s_jobError;
        unlock();
        s.status = err.empty() ? "目录加载未能启动" : err;
        s.dirty = true;
    }
}

void clearCatalogState(bool clearPage) {
    if (clearPage) {
        s.page = CatalogPage{};
        s.pageLoaded = false;
    }
    s.pageFromCache = false;
    clearIcons();
    s.sel = 0;
}

void requestCatalog(const std::string &url, bool pushCurrent) {
    if (pushCurrent && s.pageLoaded && !s.page.url.empty()) s.pageStack.push_back(s.page.url);
    clearCatalogState(true);
    s.tab = StoreState::Tab::Catalog;
    s.view = StoreState::View::Grid;
    // 未连接但有账号：这一趟会先在后台连 WiFi（catalogBody），状态就说清楚。
    s.status = g_wifi.isConnected() ? "正在加载目录…"
                                    : (catalogNetworkPossible() ? "正在连接 WiFi…" : "正在加载目录…");
    s.dirty = true;
    s.loadAttempted = true;
    s.autoRefreshIssued = false;
    CatalogPage cached;
    if (loadCachedCatalogPage(url, cached)) {
        s.page = cached;
        s.pageLoaded = true;
        s.pageFromCache = true;
        s.status = catalogNetworkPossible() ? "正在刷新…" : "离线缓存";
        s.dirty = true;
    }
    startCatalogFetch(url);
}

// ── 任务结果 ─────────────────────────────────────────────────────────────
// 槽空出来之后接手排队的那次安装。调用点都在 consumeJob 里、当前结果处理完之后。
void startPendingInstall() {
    if (!s_pendingInstall) return;
    if (s_job.state() == AppAsyncState::Running) return;   // 不该发生：调用方刚 reset 过
    s_pendingInstall = false;
    lock();
    s_jobEntry = s_pendingEntry;
    unlock();
    if (startJob(JobKind::Install, "store_install")) {
        setJobStatus("正在下载…");
        s.dirty = true;
        return;
    }
    std::string err;
    lock();
    err = s_jobError;
    unlock();
    s.view = StoreState::View::Message;
    s.msgReturn = StoreState::MsgReturn::Detail;
    s.message = "安装未能启动";
    s.messageSub = err;
    s.dirty = true;
}

void consumeJob() {
    if (s_job.state() != AppAsyncState::Done) return;
    JobKind kind;
    bool ok;
    std::string error, body;
    CatalogEntry entry;
    lock();
    kind = s_jobKind;
    ok = s_jobOk;
    error = s_jobError;
    body.swap(s_jobBody);
    entry = s_jobEntry;
    s_jobKind = JobKind::None;
    unlock();
    s_job.reapCapsTask();   // 结果已经拷出来，worker 可以回收了
    s_job.reset();
    if (kind == JobKind::None) return;

    if (kind == JobKind::Catalog) {
        const std::string url = s.pendingUrl;
        CatalogPage page;
        if (ok && parseCatalogPage(body, url, page)) {
            s.page = page;
            s.pageLoaded = true;
            s.source = url;
            s.pageFromCache = false;
            s.status.clear();
            clearIcons();
            s.sel = 0;
            s.dirty = true;
        } else {
            const std::string why = ok ? "目录格式无效" : error;
            if (s.pageLoaded) {
                s.status = std::string("目录刷新失败：") + why;
            } else {
                CatalogPage cached;
                if (loadCachedCatalogPage(url, cached)) {
                    s.page = cached;
                    s.pageLoaded = true;
                    s.pageFromCache = true;
                    s.status = "离线缓存";
                } else {
                    s.status = why;
                }
            }
            s.dirty = true;
        }
        startPendingInstall();
        return;
    }

    if (kind == JobKind::Icon) {
        if (s_icons.pixels) {
            // 找到这一枚图标对应的格子（任务期间用户可能换页，键对不上就丢掉）。
            const int slot = slotForKey(iconKeyForCatalog(entry));
            if (slot >= 0 && s_icons.state[slot] == 3) {
                size_t bytes = 0;
                uint8_t *dst = s_icons.pixels + static_cast<size_t>(slot) * kIconBytes;
                const std::string cache = iconCachePath(entry.iconSha256);
                if (ok && readBundleIconFile(cache.c_str(), 16, 96, dst, kIconBytes, bytes) &&
                    bytes == kIconBytes) {
                    s_icons.state[slot] = 1;
                    s.dirty = true;
                } else {
                    // 失败别无限重试：换页/刷新才再试。
                    s_icons.state[slot] = 2;
                }
            }
        }
        startPendingInstall();
        return;
    }

    if (kind == JobKind::Install) {
        s.view = StoreState::View::Message;
        s.msgReturn = StoreState::MsgReturn::Grid;
        if (ok) {
            reloadInstalled();
            s.message = s.confirmIsUpdate ? "更新完成" : "安装完成";
            s.messageSub = s.confirmLabel;
            s.status.clear();
            s.tab = StoreState::Tab::Installed;
            s.sel = 0;
        } else {
            s.message = "安装失败";
            s.messageSub = error;
        }
        s.dirty = true;
        return;
    }
}

// ── 渲染：公共框架 ───────────────────────────────────────────────────────
void drawHeader() {
    FontScope ui(FontRenderer::uiPxHeight());
    const int h = headerHeight();
    const int baseline = pxAscent(kHeadPx) + 6;
    drawTextPx(kHeadPx, 18, baseline, "应用商店", true);
    // 两个标签贴右上角。
    const int tabPx = std::max(28, FontRenderer::uiPxHeight() * 9 / 10);
    const int right = SCREEN_W - 18;
    const int installedW = pxWidth(tabPx, "已安装");
    const int allW = pxWidth(tabPx, "全部");
    const int allX = right - installedW - 28 - allW;
    const bool catalog = s.tab == StoreState::Tab::Catalog;
    drawTextPx(tabPx, allX, baseline - (FontRenderer::uiPxHeight() - tabPx) / 2, "全部", catalog);
    drawTextPx(tabPx, right - installedW, baseline - (FontRenderer::uiPxHeight() - tabPx) / 2, "已安装", !catalog);
    uint8_t *fb = ui_render_begin_frame();
    if (fb) {
        fb_fast_sync();
        if (catalog)
            fb_fast_fill_rect(fb, allX, baseline + 4, allW, 2, 0x00);
        else
            fb_fast_fill_rect(fb, right - installedW, baseline + 4, installedW, 2, 0x00);
    }

    if (!s.status.empty()) {
        const int sy = h + pxAscent(kSubPx) + 2;
        drawTextPx(kSubPx, 18, sy, fitText(kSubPx, s.status, SCREEN_W - 140).c_str());
        drawTextPx(kSubPx, SCREEN_W - 18 - pxWidth(kSubPx, "刷新"), sy, "刷新");
    }
    fb = ui_render_begin_frame();
    if (fb) {
        fb_fast_sync();
        fb_fast_fill_rect(fb, 0, h - 1, SCREEN_W, 1, 0x00);
    }
}

void drawFooter(const char *hint, const char *right = "") {
    const int px = smallFooterPx();
    const int y = SCREEN_H - 10;
    drawTextPx(px, 18, y, hint);
    if (right && *right) drawTextPx(px, SCREEN_W - 18 - pxWidth(px, right), y, right);
}

void drawHeaderTabsHit(int x, int y) {
    if (y > headerHeight()) return;
    FontScope ui(FontRenderer::uiPxHeight());
    const int tabPx = std::max(28, FontRenderer::uiPxHeight() * 9 / 10);
    const int installedW = pxWidth(tabPx, "已安装");
    const int allW = pxWidth(tabPx, "全部");
    const int right = SCREEN_W - 18;
    const int allX = right - installedW - 28 - allW;
    const bool overTab = x >= allX - 10;
    if (!overTab) return;
    const auto next = (x >= right - installedW - 8) ? StoreState::Tab::Installed : StoreState::Tab::Catalog;
    if (next == s.tab) return;
    s.tab = next;
    s.sel = 0;
    clearIcons();
    s.dirty = true;
    if (s.tab == StoreState::Tab::Catalog && !s.pageLoaded) requestCatalog(catalogSource(), false);
    if (s.tab == StoreState::Tab::Installed) reloadInstalled();
}

// ── 渲染：网格 ───────────────────────────────────────────────────────────
void renderGrid() {
    drawHeader();
    const int count = itemCount();
    if (count == 0) {
        const int cx = SCREEN_W / 2;
        const int cy = (bodyTop() + footerTop()) / 2;
        const char *main = s.tab == StoreState::Tab::Installed ? "还没有安装任何应用" : "暂无应用目录";
        drawCenteredPx(kNamePx, cx, cy, main, true);
        if (s.tab == StoreState::Tab::Catalog)
            drawCenteredPx(kSubPx, cx, cy + pxLine(kSubPx), "检查 WiFi 后按 R 刷新");
        else
            drawCenteredPx(kSubPx, cx, cy + pxLine(kSubPx), "先到「全部」标签安装应用");
        drawFooter("←→↑↓ 选择  Enter 打开  Esc 返回", s.tab == StoreState::Tab::Catalog ? "R 刷新" : "");
        return;
    }

    const int cols = gridCols();
    const int cellW = SCREEN_W / cols;
    const int cellH = gridCellH();
    const int iconPx = gridIconPx();
    const int rows = gridRows(cellH);
    if (s.sel >= count) s.sel = count - 1;
    if (s.sel < 0) s.sel = 0;
    const int start = gridStart(count, cols, rows);
    const int capacity = std::min(count - start, rows * cols);

    for (int j = 0; j < capacity; ++j) {
        const int i = start + j;
        const int r = j / cols, c = j % cols;
        const int cx = c * cellW + cellW / 2;
        const int top = bodyTop() + r * cellH;
        const int iconX = cx - iconPx / 2;
        const bool sel = i == s.sel;

        const std::string name = s.tab == StoreState::Tab::Catalog
                                     ? std::string(s.page.entries[i].package.name)
                                     : std::string(s.installed[i].package.name);
        if (sel) {
            uint8_t *fb = ui_render_begin_frame();
            if (fb) {
                fb_fast_sync();
                fb_fast_fill_rect(fb, iconX - 6, top - 2, iconPx + 12, iconPx + 12, 0x00);
            }
        }
        // 图标槽按**屏上的格位**（j）算，不按条目下标：已安装列表会滚动，绝对下标可能
        // 超过 kMaxCells，而屏上同时最多只有 rows*cols ≤ 24 枚。滚过之后同一槽位可能还
        // 留着上一屏那枚图标 —— 键对不上就画占位，等空闲帧的 loadOneIcon 换掉。
        const int slot = slotFor(j);
        const std::string wantKey = s.tab == StoreState::Tab::Catalog
                                        ? iconKeyForCatalog(s.page.entries[i])
                                        : iconKeyForInstalled(s.installed[i]);
        const uint8_t *pixels =
            (s_icons.pixels && slot >= 0 && s_icons.state[slot] == 1 && wantKey == s_icons.keys[slot])
                ? s_icons.pixels + static_cast<size_t>(slot) * kIconBytes
                : nullptr;
        if (pixels)
            drawPackedIcon(pixels, iconX, top, iconPx, sel);
        else
            drawPlaceholderIcon(iconX, top, iconPx, name, sel);

        const int nameY = top + iconPx + 10 + pxAscent(kNamePx);
        const std::string label = fitText(kNamePx, name, cellW - 16);
        drawCenteredPx(kNamePx, cx, nameY, label.c_str(), sel, sel);

        std::string sub;
        if (s.tab == StoreState::Tab::Catalog) {
            const auto &e = s.page.entries[i];
            const InstalledApp *app = installedById(e.package.id);
            if (app) {
                if (compareVersions(e.package.version, app->package.version) > 0)
                    sub = "更新 " + std::string(e.package.version);
                else
                    sub = "已安装";
            } else {
                char missing[64]{};
                sub = compatible(e.package, missing, sizeof(missing)) ? e.package.version : "不兼容";
            }
        } else {
            sub = s.installed[i].valid ? s.installed[i].package.version : "损坏";
        }
        const int subY = nameY + pxLine(kSubPx) - 2;
        drawCenteredPx(kSubPx, cx, subY, fitText(kSubPx, sub, cellW - 16).c_str(), false, sel);
    }

    // 翻页条（只在目录标签、且确实有上一页/下一页时占位）。
    if (s.tab == StoreState::Tab::Catalog && s.pageLoaded &&
        (!s.page.next.empty() || !s.page.previous.empty())) {
        const int y = footerTop() - 36;
        if (!s.page.previous.empty()) drawTextPx(kSubPx, 18, y, "‹ 上一页");
        if (!s.page.next.empty())
            drawTextPx(kSubPx, SCREEN_W - 18 - pxWidth(kSubPx, "下一页 ›"), y, "下一页 ›");
        char pos[32];
        if (s.page.pages)
            std::snprintf(pos, sizeof(pos), "%u / %u", s.page.page, s.page.pages);
        else
            pos[0] = 0;
        if (pos[0]) drawCenteredPx(kSubPx, SCREEN_W / 2, y, pos);
    }

    char right[32] = "";
    if (s.tab == StoreState::Tab::Catalog) {
        std::snprintf(right, sizeof(right), "R 刷新");
        drawFooter("←→↑↓ 选择  Enter 查看  Esc 返回", right);
    } else {
        drawFooter("←→↑↓ 选择  Enter 查看  Esc 返回");
    }
}

// ── 详情 ─────────────────────────────────────────────────────────────────
enum class Action { Open, Install, Update, Uninstall, Back };

struct ActionItem {
    Action kind;
    const char *label;
};

void buildActions(std::vector<ActionItem> &out) {
    out.clear();
    const InstalledApp *app = installedById(s.detail.pkg.id);
    if (app && app->valid) out.push_back({Action::Open, "打开"});
    if (s.detail.catalog) {
        char missing[64]{};
        if (compatible(s.detail.pkg, missing, sizeof(missing))) {
            if (!app)
                out.push_back({Action::Install, "安装"});
            else if (compareVersions(s.detail.pkg.version, app->package.version) > 0)
                out.push_back({Action::Update, "更新"});
        }
    }
    if (app) out.push_back({Action::Uninstall, "卸载"});
    out.push_back({Action::Back, "返回"});
}

const char *permissionLabel(uint32_t bit) {
    if (bit == permissionBit("network")) return "联网";
    if (bit == permissionBit("radio")) return "无线";
    if (bit == permissionBit("notifications")) return "通知";
    if (bit == permissionBit("background")) return "后台";
    if (bit == permissionBit("reading_data")) return "阅读数据";
    if (bit == permissionBit("wallpaper")) return "壁纸";
    if (bit == permissionBit("sound")) return "声音";
    if (bit == permissionBit("device_info")) return "设备信息";
    if (bit == permissionBit("files_export")) return "导出文件";
    if (bit == permissionBit("keep_awake")) return "保持唤醒";
    return "其它";
}
const char *capabilityLabel(uint32_t bit) {
    if (bit == capabilityBit("wifi")) return "WiFi";
    if (bit == capabilityBit("touch")) return "触摸";
    if (bit == capabilityBit("psram")) return "大内存";
    if (bit == capabilityBit("storage")) return "存储";
    if (bit == capabilityBit("rtc")) return "时钟";
    if (bit == capabilityBit("imu")) return "传感器";
    if (bit == capabilityBit("buzzer")) return "蜂鸣器";
    if (bit == capabilityBit("audio")) return "音频";
    return "其它";
}
std::string permissionList(uint32_t mask) {
    std::string out;
    for (const auto &p : PERMISSIONS) {
        if (!(mask & p.bit)) continue;
        if (!out.empty()) out += "、";
        out += permissionLabel(p.bit);
    }
    return out.empty() ? "无" : out;
}
std::string capabilityList(uint32_t mask) {
    std::string out;
    for (const auto &c : CAPABILITIES) {
        if (!(mask & c.bit)) continue;
        if (!out.empty()) out += "、";
        out += capabilityLabel(c.bit);
    }
    return out.empty() ? "无" : out;
}

void renderDetail() {
    drawHeader();
    const int top = bodyTop();
    const int iconPx = 112;
    std::string wantKey;
    if (s.detail.catalog)
        wantKey = s.detail.sha256.empty() ? std::string("id:") + s.detail.pkg.id : s.detail.sha256;
    else
        wantKey = std::string("in:") + s.detail.pkg.id + ":" + s.detail.pkg.version;
    const uint8_t *pixels = nullptr;
    for (int i = 0; s_icons.pixels && i < kMaxCells; ++i) {
        if (s_icons.state[i] == 1 && !std::strcmp(s_icons.keys[i], wantKey.c_str())) {
            pixels = s_icons.pixels + static_cast<size_t>(i) * kIconBytes;
            break;
        }
    }
    if (pixels)
        drawPackedIcon(pixels, 18, top, iconPx, false);
    else
        drawPlaceholderIcon(18, top, iconPx, s.detail.pkg.name, false);

    const int tx = 18 + iconPx + 22;
    const int tw = SCREEN_W - tx - 18;
    drawTextPx(kHeadPx, tx, top + pxAscent(kHeadPx), fitText(kHeadPx, s.detail.pkg.name, tw).c_str(), true);
    const int line2 = top + pxAscent(kHeadPx) + pxLine(kSubPx);
    drawTextPx(kSubPx, tx, line2, fitText(kSubPx, s.detail.pkg.id, tw).c_str());
    char meta[96];
    if (s.detail.bytes)
        std::snprintf(meta, sizeof(meta), "v%s  ·  %.1f KB", s.detail.pkg.version, s.detail.bytes / 1024.0);
    else
        std::snprintf(meta, sizeof(meta), "v%s", s.detail.pkg.version);
    drawTextPx(kSubPx, tx, line2 + pxLine(kSubPx), meta, false);
    if (installedById(s.detail.pkg.id))
        drawTextPx(kSubPx, tx, line2 + 2 * pxLine(kSubPx), "已安装", true);

    // 字号加大后标题右侧那几行（id / 版本 / 已安装）会比图标还高，简介不能还从
    // top+iconPx+16 起 —— 否则会和「已安装」那一行叠在一起。取两者的大者。
    const int metaBottom = top + pxAscent(kHeadPx) + 3 * pxLine(kSubPx) +
                           (pxLine(kSubPx) - pxAscent(kSubPx));
    int y = std::max(top + iconPx + 16, metaBottom + 8);
    std::vector<std::string> lines;
    wrapText(kBodyPx, s.detail.pkg.description, SCREEN_W - 36, 5, lines);
    for (const auto &line : lines) {
        y += pxLine(kBodyPx);
        drawTextPx(kBodyPx, 18, y - (pxLine(kBodyPx) - pxAscent(kBodyPx)), line.c_str());
    }
    y += 10;
    char missing[96]{};
    const bool ok = compatible(s.detail.pkg, missing, sizeof(missing));
    const std::string perm = permissionList(s.detail.pkg.permissions);
    const std::string caps = capabilityList(s.detail.pkg.requiredCapabilities);
    const int infoPx = kSubPx;
    const int infoLine = pxLine(infoPx);
    if (y + 4 * infoLine < footerTop() - 70) {
        drawTextPx(infoPx, 18, y + infoLine - 6, ("权限：" + perm).c_str());
        drawTextPx(infoPx, 18, y + 2 * infoLine - 6, ("需要：" + caps).c_str());
        std::string compatText = ok ? "与本机兼容" : std::string("缺少：") + missing;
        drawTextPx(infoPx, 18, y + 3 * infoLine - 6, fitText(infoPx, compatText, SCREEN_W - 36).c_str());
    }

    std::vector<ActionItem> actions;
    buildActions(actions);
    if (s.detailAction >= static_cast<int>(actions.size())) s.detailAction = 0;
    for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
        int x, ay, w, h;
        actionRect(i, static_cast<int>(actions.size()), x, ay, w, h);
        const bool sel = i == s.detailAction;
        uint8_t *fb = ui_render_begin_frame();
        if (fb) {
            fb_fast_sync();
            if (sel) fb_fast_fill_rect(fb, x, ay, w, h, 0x00);
            else {
                fb_fast_fill_rect(fb, x, ay, w, 2, 0x00);
                fb_fast_fill_rect(fb, x, ay + h - 2, w, 2, 0x00);
                fb_fast_fill_rect(fb, x, ay, 2, h, 0x00);
                fb_fast_fill_rect(fb, x + w - 2, ay, 2, h, 0x00);
            }
        }
        drawCenteredPx(std::max(20, kBodyPx - 2), x + w / 2, ay + (h + pxAscent(std::max(20, kBodyPx - 2))) / 2 - 2,
                       actions[i].label, true, sel);
    }
    drawFooter("↑↓←→ 选择  Enter 执行  Esc 返回");
}

// ── 确认 ─────────────────────────────────────────────────────────────────
void confirmGeometry(int &bx, int &by, int &bw, int &bh) {
    bw = std::min(SCREEN_W - 60, 560);
    bh = 270;   // 字号加大后 250 里"标题+应用名+副标题"离按钮只剩十几像素
    bx = (SCREEN_W - bw) / 2;
    by = (SCREEN_H - bh) / 2;
}
void confirmButtonRect(int index, int &x, int &y, int &w, int &h) {
    int bx, by, bw, bh;
    confirmGeometry(bx, by, bw, bh);
    w = (bw - 3 * 16) / 2;
    h = 60;
    x = bx + 16 + index * (w + 16);
    y = by + bh - h - 16;
}

void renderConfirm() {
    drawHeader();
    int bx, by, bw, bh;
    confirmGeometry(bx, by, bw, bh);
    uint8_t *fb = ui_render_begin_frame();
    if (fb) {
        fb_fast_sync();
        fb_fast_fill_rect(fb, bx, by, bw, bh, 0xF0);
        fb_fast_fill_rect(fb, bx, by, bw, 2, 0x00);
        fb_fast_fill_rect(fb, bx, by + bh - 2, bw, 2, 0x00);
        fb_fast_fill_rect(fb, bx, by, 2, bh, 0x00);
        fb_fast_fill_rect(fb, bx + bw - 2, by, 2, bh, 0x00);
    }
    const int cx = bx + bw / 2;
    int y = by + 28;
    const std::string title = s.confirmUninstall ? "卸载应用？" : (s.confirmIsUpdate ? "更新应用？" : "安装应用？");
    drawCenteredPx(kHeadPx, cx, y + pxAscent(kHeadPx), title.c_str(), true);
    y += pxLine(kHeadPx) + 14;
    drawCenteredPx(kBodyPx, cx, y + pxAscent(kBodyPx), fitText(kBodyPx, s.confirmLabel, bw - 32).c_str());
    y += pxLine(kBodyPx) + 8;
    char sub[96];
    if (s.confirmUninstall) {
        std::snprintf(sub, sizeof(sub), "应用数据也会一并删除");
    } else {
        std::snprintf(sub, sizeof(sub), "版本 %s  ·  %.1f KB", s.confirmEntry.package.version,
                      s.confirmEntry.bytes / 1024.0);
    }
    drawCenteredPx(kSubPx, cx, y + pxAscent(kSubPx), sub);

    const char *labels[2] = {"确定", "取消"};
    for (int i = 0; i < 2; ++i) {
        int x, by2, w, h;
        confirmButtonRect(i, x, by2, w, h);
        const bool sel = i == s.confirmSel;
        if (fb) {
            if (sel) fb_fast_fill_rect(fb, x, by2, w, h, 0x00);
            else {
                fb_fast_fill_rect(fb, x, by2, w, 2, 0x00);
                fb_fast_fill_rect(fb, x, by2 + h - 2, w, 2, 0x00);
                fb_fast_fill_rect(fb, x, by2, 2, h, 0x00);
                fb_fast_fill_rect(fb, x + w - 2, by2, 2, h, 0x00);
            }
        }
        drawCenteredPx(kBodyPx, x + w / 2, by2 + (h + pxAscent(kBodyPx)) / 2 - 2, labels[i], true, sel);
    }
    drawFooter("←→ 选择  Enter 确定  Esc 取消");
}

// ── 消息 / 忙碌 ──────────────────────────────────────────────────────────
void renderMessage() {
    drawHeader();
    const int cx = SCREEN_W / 2;
    std::vector<std::string> lines;
    wrapText(kHeadPx, s.message.c_str(), SCREEN_W - 80, 3, lines);
    int y = SCREEN_H / 2 - 50;
    for (const auto &line : lines) {
        drawCenteredPx(kHeadPx, cx, y, line.c_str(), true);
        y += pxLine(kHeadPx);
    }
    if (!s.messageSub.empty()) {
        std::vector<std::string> sub;
        wrapText(kSubPx, s.messageSub.c_str(), SCREEN_W - 100, 3, sub);
        y += 10;
        for (const auto &line : sub) {
            y += pxLine(kSubPx);
            drawCenteredPx(kSubPx, cx, y, line.c_str());
        }
    }
    drawFooter("按任意键继续");
}

void renderBusy() {
    drawHeader();
    const int cx = SCREEN_W / 2;
    const int cy = SCREEN_H / 2 - 30;
    const std::string status = jobStatus();
    drawCenteredPx(kHeadPx, cx, cy, status.empty() ? "正在处理…" : status.c_str(), true);

    const int bw = std::min(SCREEN_W - 120, 520), bh = 16;
    const int bx = cx - bw / 2, by = cy + 44;
    uint8_t *fb = ui_render_begin_frame();
    if (fb) {
        fb_fast_sync();
        fb_fast_fill_rect(fb, bx, by, bw, 2, 0x00);
        fb_fast_fill_rect(fb, bx, by + bh - 2, bw, 2, 0x00);
        fb_fast_fill_rect(fb, bx, by, 2, bh, 0x00);
        fb_fast_fill_rect(fb, bx + bw - 2, by, 2, bh, 0x00);
        const int pct = s.progress < 0 ? -1 : std::min(100, s.progress);
        if (pct > 0) fb_fast_fill_rect(fb, bx + 3, by + 3, (bw - 6) * pct / 100, bh - 6, 0x00);
    }
    char line[80];
    if (s.progress >= 0)
        std::snprintf(line, sizeof(line), "%d%%", std::min(100, s.progress));
    else
        line[0] = 0;
    if (line[0]) drawCenteredPx(kSubPx, cx, by + bh + pxAscent(kSubPx) + 8, line);
    drawFooter("安装期间请勿断电");
}

void render() {
    ui_clear();
    switch (s.view) {
        case StoreState::View::Grid: renderGrid(); break;
        case StoreState::View::Detail: renderDetail(); break;
        case StoreState::View::Confirm: renderConfirm(); break;
        case StoreState::View::Busy: renderBusy(); break;
        case StoreState::View::Message: renderMessage(); break;
    }
    ui_commit();
    s.dirty = false;
}

// ── 交互 ─────────────────────────────────────────────────────────────────
void openDetailCatalog(int index) {
    if (index < 0 || index >= static_cast<int>(s.page.entries.size())) return;
    const CatalogEntry &e = s.page.entries[index];
    s.detail.pkg = e.package;
    s.detail.artifact = e.artifact;
    s.detail.sha256 = e.sha256;
    s.detail.bytes = e.bytes;
    s.detail.valid = true;
    s.detail.catalog = true;
    s.detail.installed = installedById(e.package.id) != nullptr;
    s.detailAction = 0;
    s.view = StoreState::View::Detail;
    s.dirty = true;
}

void openDetailInstalled(int index) {
    if (index < 0 || index >= static_cast<int>(s.installed.size())) return;
    const InstalledApp &a = s.installed[index];
    s.detail.pkg = a.package;
    s.detail.artifact.clear();
    s.detail.sha256.clear();
    s.detail.bytes = 0;
    s.detail.valid = a.valid;
    s.detail.catalog = false;
    s.detail.installed = true;
    s.detailAction = 0;
    s.view = StoreState::View::Detail;
    s.dirty = true;
}

void openDetailSelected() {
    if (s.tab == StoreState::Tab::Catalog)
        openDetailCatalog(s.sel);
    else
        openDetailInstalled(s.sel);
}

void moveGrid(int key) {
    const int cols = gridCols();
    const int count = itemCount();
    if (count <= 0) return;
    if (key == KEY_LEFT) {
        if (s.sel % cols > 0) --s.sel;
    } else if (key == KEY_RIGHT) {
        if (s.sel % cols < cols - 1 && s.sel + 1 < count) ++s.sel;
    } else if (key == KEY_UP) {
        if (s.sel - cols >= 0) s.sel -= cols;
    } else if (key == KEY_DOWN) {
        if (s.sel + cols < count) s.sel += cols;
    }
    s.dirty = true;
}

AppState activateAction(const ActionItem &action) {
    switch (action.kind) {
        case Action::Open: {
            const InstalledApp *app = installedById(s.detail.pkg.id);
            if (app && app->valid) {
                screen_luaapp_request(app->package.id);
                return APP_LUA_APP;
            }
            return APP_APPSTORE;
        }
        case Action::Install:
        case Action::Update: {
            const InstalledApp *app = installedById(s.detail.pkg.id);
            s.confirmEntry.package = s.detail.pkg;
            s.confirmEntry.artifact = s.detail.artifact;
            s.confirmEntry.sha256 = s.detail.sha256;
            s.confirmEntry.bytes = s.detail.bytes;
            s.confirmIsUpdate = action.kind == Action::Update;
            s.confirmUninstall = false;
            s.confirmLabel = s.detail.pkg.name;
            s.confirmSel = 1;
            s.view = StoreState::View::Confirm;
            s.dirty = true;
            (void)app;
            return APP_APPSTORE;
        }
        case Action::Uninstall: {
            s.confirmUninstall = true;
            s.confirmIsUpdate = false;
            s.confirmLabel = s.detail.pkg.name;
            const std::string id = s.detail.pkg.id;
            std::snprintf(s.confirmEntry.package.id, sizeof(s.confirmEntry.package.id), "%s", id.c_str());
            s.confirmSel = 1;
            s.view = StoreState::View::Confirm;
            s.dirty = true;
            return APP_APPSTORE;
        }
        case Action::Back:
            s.view = StoreState::View::Grid;
            s.dirty = true;
            return APP_APPSTORE;
    }
    return APP_APPSTORE;
}

void startInstall() {
    s.view = StoreState::View::Busy;
    s.progress = -1;
    s.dirty = true;
    if (s_job.state() == AppAsyncState::Running) {
        // 唯一的工作槽被图标/目录任务占着（进详情页那一刻图标可能还在下载）。
        // 排队，等 consumeJob 收走当前结果时再真正开装 —— 别在这里静默失败，
        // 否则 Busy 屏永远显示"正在处理…"。
        lock();
        s_pendingEntry = s.confirmEntry;
        s_pendingInstall = true;
        unlock();
        setJobStatus("正在准备安装…");
        return;
    }
    lock();
    s_jobEntry = s.confirmEntry;
    unlock();
    if (startJob(JobKind::Install, "store_install")) {
        setJobStatus("正在下载…");
        s.dirty = true;
        return;
    }
    std::string err;
    lock();
    err = s_jobError;
    unlock();
    s.view = StoreState::View::Message;
    s.msgReturn = StoreState::MsgReturn::Detail;
    s.message = "安装未能启动";
    s.messageSub = err;
    s.dirty = true;
}

void doUninstall() {
    const std::string id = s.confirmEntry.package.id;
    const bool ok = uninstallPackage(id.c_str());
    reloadInstalled();
    s.view = StoreState::View::Message;
    s.msgReturn = StoreState::MsgReturn::Grid;
    s.message = ok ? "已卸载" : "卸载失败";
    s.messageSub = s.confirmLabel;
    s.dirty = true;
}

AppState handleGridKey(int key) {
    if (key == 0x1B || key == KEY_BACK) return APP_READER;
    if (key == 'r' || key == 'R') {
        s.loadAttempted = false;
        s.autoRefreshIssued = false;
        requestCatalog(s.source.empty() ? catalogSource() : s.source, false);
        return APP_APPSTORE;
    }
    if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        if (s.tab != StoreState::Tab::Catalog || !s.pageLoaded) return APP_APPSTORE;
        if (key == KEY_PAGE_DOWN && !s.page.next.empty()) {
            std::string url;
            if (catalogRelativeUrl(s.page.url, s.page.next.c_str(), url)) {
                s.pageStack.push_back(s.page.url);
                requestCatalog(url, false);
            }
        } else if (key == KEY_PAGE_UP && !s.page.previous.empty()) {
            std::string url;
            if (catalogRelativeUrl(s.page.url, s.page.previous.c_str(), url)) requestCatalog(url, false);
        }
        return APP_APPSTORE;
    }
    if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN) {
        moveGrid(key);
        return APP_APPSTORE;
    }
    if (key == '\n' || key == '\r') {
        int x, y;
        if (input_tap_xy(&x, &y)) {
            if (y <= headerHeight()) {
                drawHeaderTabsHit(x, y);
                return APP_APPSTORE;
            }
            if (!s.status.empty() && y < headerHeight() + statusLineHeight() && x > SCREEN_W - 100) {
                requestCatalog(s.source.empty() ? catalogSource() : s.source, false);
                return APP_APPSTORE;
            }
            if (s.tab == StoreState::Tab::Catalog && s.pageLoaded &&
                (!s.page.next.empty() || !s.page.previous.empty())) {
                const int navY = footerTop() - 36;
                if (y >= navY - 20 && y <= navY + 20) {
                    if (x < SCREEN_W / 2 && !s.page.previous.empty()) {
                        std::string url;
                        if (catalogRelativeUrl(s.page.url, s.page.previous.c_str(), url)) requestCatalog(url, false);
                    } else if (x >= SCREEN_W / 2 && !s.page.next.empty()) {
                        std::string url;
                        if (catalogRelativeUrl(s.page.url, s.page.next.c_str(), url)) {
                            s.pageStack.push_back(s.page.url);
                            requestCatalog(url, false);
                        }
                    }
                    return APP_APPSTORE;
                }
            }
            const int cols = gridCols();
            const int cellH = gridCellH();
            const int cellW = SCREEN_W / cols;
            const int count = itemCount();
            const int rows = gridRows(cellH);
            const int row = (y - bodyTop()) / std::max(1, cellH);
            if (row < 0 || row >= rows) return APP_APPSTORE;
            const int col = std::min(cols - 1, std::max(0, x / cellW));
            // 视口滚过之后，点到的条目要加上滚动起点（见 gridStart）。
            const int index = gridStart(count, cols, rows) + row * cols + col;
            if (index >= count) return APP_APPSTORE;
            s.sel = index;
            openDetailSelected();
            return APP_APPSTORE;
        }
        openDetailSelected();
        return APP_APPSTORE;
    }
    return APP_APPSTORE;
}

AppState handleDetailKey(int key) {
    std::vector<ActionItem> actions;
    buildActions(actions);
    if (key == 0x1B || key == KEY_BACK) {
        s.view = StoreState::View::Grid;
        s.dirty = true;
        return APP_APPSTORE;
    }
    if (key == KEY_LEFT || key == KEY_UP) {
        s.detailAction = (s.detailAction + static_cast<int>(actions.size()) - 1) % std::max(1, static_cast<int>(actions.size()));
        s.dirty = true;
        return APP_APPSTORE;
    }
    if (key == KEY_RIGHT || key == KEY_DOWN) {
        s.detailAction = (s.detailAction + 1) % std::max(1, static_cast<int>(actions.size()));
        s.dirty = true;
        return APP_APPSTORE;
    }
    if (key == '\n' || key == '\r') {
        int x, y;
        if (input_tap_xy(&x, &y)) {
            for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
                int rx, ry, rw, rh;
                actionRect(i, static_cast<int>(actions.size()), rx, ry, rw, rh);
                if (x >= rx && x < rx + rw && y >= ry && y < ry + rh) {
                    s.detailAction = i;
                    return activateAction(actions[i]);
                }
            }
            return APP_APPSTORE;
        }
        return activateAction(actions[s.detailAction]);
    }
    return APP_APPSTORE;
}

AppState handleConfirmKey(int key) {
    if (key == 0x1B || key == KEY_BACK) {
        s.view = StoreState::View::Detail;
        s.dirty = true;
        return APP_APPSTORE;
    }
    if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN) {
        s.confirmSel = 1 - s.confirmSel;
        s.dirty = true;
        return APP_APPSTORE;
    }
    if (key == '\n' || key == '\r') {
        int x, y;
        if (input_tap_xy(&x, &y)) {
            for (int i = 0; i < 2; ++i) {
                int rx, ry, rw, rh;
                confirmButtonRect(i, rx, ry, rw, rh);
                if (x >= rx && x < rx + rw && y >= ry && y < ry + rh) {
                    s.confirmSel = i;
                    break;
                }
            }
        }
        if (s.confirmSel == 0) {
            if (s.confirmUninstall)
                doUninstall();
            else
                startInstall();
        } else {
            s.view = StoreState::View::Detail;
            s.dirty = true;
        }
    }
    return APP_APPSTORE;
}

AppState handleMessageKey(int key) {
    (void)key;
    if (s.msgReturn == StoreState::MsgReturn::Detail)
        s.view = StoreState::View::Detail;
    else
        s.view = StoreState::View::Grid;
    s.dirty = true;
    return APP_APPSTORE;
}

}  // namespace

// ── 公开入口 ─────────────────────────────────────────────────────────────
void screen_appstore_enter(ScreenContext &ctx) {
    (void)ctx;
    if (!s.initialized) {
        s = StoreState{};
        s.initialized = true;
        s.source = catalogSource();
        s_wifiConnectedByUs.store(false, std::memory_order_relaxed);
        recoverInstalls();
        reloadInstalled();
        allocIcons();
        // 先贴缓存，再在首次空闲帧发起刷新（不在这里阻塞）。
        CatalogPage cached;
        if (loadCachedCatalogPage(s.source, cached)) {
            s.page = cached;
            s.pageLoaded = true;
            s.pageFromCache = true;
            s.status = catalogNetworkPossible() ? "正在刷新…" : "离线缓存";
        } else {
            s.status = "正在加载应用目录…";
        }
        s.dirty = true;
    } else {
        reloadInstalled();
        if (!s_icons.pixels) allocIcons();
        // 从 Lua 应用回来：目录页保持原样，只刷新已安装状态。
        // 但上一次要是一个目录页都没拿到（典型：当时还没配 WiFi），就得允许再试一次。
        // loadAttempted 是"失败别每拍重试"的闸，跨进店保留着会把用户永远钉在旧错误上：
        // 配好网回来，屏上仍是「未配置 WiFi」，空闲帧也不再发请求。
        if (!s.pageLoaded) {
            s.loadAttempted = false;
            s.autoRefreshIssued = false;
            s.status = "正在加载应用目录…";
        } else if (s.pageFromCache) {
            // 上次刷新失败（或没来得及）的缓存页：再进店给一次机会，最多一次。
            s.autoRefreshIssued = false;
        }
        s.dirty = true;
    }
    // 上次离开时任务还在跑：等它结束再消费（consumeJob 会忽略已换页的结果）。
    s.view = StoreState::View::Grid;
    s.dirty = true;
}

AppState screen_appstore_handle(int key, ScreenContext &ctx) {
    (void)ctx;
    // 先把上一拍的任务结果收掉，再画这一拍。
    consumeJob();

    AppState next = APP_APPSTORE;
    if (key > 0) {
        switch (s.view) {
            case StoreState::View::Grid: next = handleGridKey(key); break;
            case StoreState::View::Detail: next = handleDetailKey(key); break;
            case StoreState::View::Confirm: next = handleConfirmKey(key); break;
            case StoreState::View::Message: next = handleMessageKey(key); break;
            case StoreState::View::Busy: next = APP_APPSTORE; break;
        }
        if (next != APP_APPSTORE) return next;
    } else {
        // 空闲帧：首次进入时补一次目录加载；缓存页补一次后台刷新；任务结束后补画。
        const bool idle = s_job.state() != AppAsyncState::Running && s_jobKind == JobKind::None;
        if (!s.pageLoaded && !s.loadAttempted && idle) {
            s.loadAttempted = true;
            startCatalogFetch(s.source.empty() ? catalogSource() : s.source);
        } else if (s.pageLoaded && s.pageFromCache && !s.autoRefreshIssued && idle &&
                   catalogNetworkPossible()) {
            s.autoRefreshIssued = true;
            startCatalogFetch(s.page.url.empty() ? s.source : s.page.url);
            s.status = "正在刷新…";
            s.dirty = true;
        }
        if (s.view == StoreState::View::Busy) {
            const int pct = s_dlPercent.load(std::memory_order_relaxed);
            const unsigned long long got = s_dlGot.load(std::memory_order_relaxed);
            if (pct != s.progress || got != s.progressGot) {
                s.progress = pct;
                s.progressGot = got;
                s.progressTotal = s_dlTotal.load(std::memory_order_relaxed);
                s.dirty = true;
            }
        }
        if (s.view == StoreState::View::Grid) loadOneIcon();
    }

    if (s.dirty) render();
    return APP_APPSTORE;
}

void screen_appstore_leave(AppState next) {
    (void)next;
    s_job.cancel();
    // 排着队的那次安装跟着这次离店作废，别下次进店被顺手拉起来。
    s_pendingInstall = false;
    // 任务已经跑完但还没被消费（用户等不及先走了）：顺手收掉 PSRAM 栈。
    // 还在 Running 的不能删；它结束后会挂起，下次进店 consumeJob 时回收。
    if (s_job.state() == AppAsyncState::Done) s_job.reapCapsTask();
    freeIcons();
    // 若这台 WiFi 是进商店时替用户拉起来的，走的时候还回去（任务还在跑也不影响：
    // 下一次进商店会重新连）。
    if (s_wifiConnectedByUs.exchange(false, std::memory_order_relaxed)) g_wifi.disconnect();
}
