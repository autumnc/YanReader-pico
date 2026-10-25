#include "screen_luaapp.h"

#include <HalStorage.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "airpage_service.h"
#include "app_services.h"
#include "crossstore/CrossStoreRuntime.h"
#include "crossstore/CrossStoreSvg.h"
#include "crossstore/LuaReadingQueries.h"
#include "crossstore/recoverable_file.h"
#include "crossstore/store_package.h"
#include "fb_fast.h"
#include "font_renderer.h"
#include "hw/input.h"
#include "pcf85063.h"
#include "qrcodegen.h"
#include "reading_stats.h"
#include "screen_reader.h"
#include "settings_manager.h"
#include "typing_click.h"
#include "ui_helpers.h"
#include "ui_render.h"
#include "wifi_manager.h"

// main.cpp 的浅睡判断读这一位。应用退出时必须清掉：应用崩了也不能把整机钉醒。
// **必须在匿名 namespace 之外声明**，否则内部链接绑不上 main.cpp 里的定义。
extern bool g_lua_keep_awake;

namespace {

using namespace crossstore;

constexpr const char *kTAG = "LuaApp";
constexpr int kFontPx[4] = {22, 28, 36, 48};
constexpr int kSidePadding = 12;
constexpr int kRowHeight = 68;
constexpr int kTileHeight = 92;
constexpr int kTextPx = 28;
constexpr int kSmallPx = 22;

struct LuaState {
    bool active = false;
    bool dirty = true;
    bool exitRequested = false;
    bool exitToReader = false;
    bool failed = false;
    bool canvasMode = false;
    bool qrReady = false;

    char id[32]{};
    char path[96]{};
    Package pkg;
    char heading[128]{};
    char caption[256]{};
    char rowsLabel[MAX_ROWS][96]{};
    char rowsValue[MAX_ROWS][160]{};
    int rowCount = 0;
    int columns = 0;
    int sel = 0;
    int scroll = 0;

    char pendingRoute[32]{};
    char pendingBook[256]{};
    char pendingExport[64]{};
    char qrContent[384]{};
    char errorText[192]{};

    int lastW = -1, lastH = -1;
    int64_t lastTickUs = 0;
    int64_t toastUntilUs = 0;
    Canvas *canvas = nullptr;
};

LuaState s;
char s_pendingId[32]{};
std::unique_ptr<crossstore::Runtime> s_runtime;
std::unique_ptr<Host> s_host;

// qrcodegen 的两块工作缓冲（每块 800 多字节）；只在主循环任务里用。
uint8_t s_qrTemp[qrcodegen_BUFFER_LEN_FOR_VERSION(16)];
uint8_t s_qrCode[qrcodegen_BUFFER_LEN_FOR_VERSION(16)];

// 网络对时：0=idle 1=working 2=success 3=failed。后台任务写、UI 读。
std::atomic<int> s_syncState{0};
int64_t s_syncStartedUs = 0;

// ── 文本度量 ─────────────────────────────────────────────────────────────
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
        if (cp == '\r') continue;
        const std::string glyph(start, p - start);
        if (g_font.textWidth((line + glyph).c_str()) > maxWidth && !line.empty()) {
            const size_t space = line.find_last_of(' ');
            if (space != std::string::npos && space > 0 && space + 1 < line.size()) {
                out.push_back(line.substr(0, space));
                line = line.substr(space + 1);
            } else {
                out.push_back(line);
                line.clear();
            }
            if (static_cast<int>(out.size()) >= maxLines) break;
        }
        if (line.empty() && glyph == " ") continue;
        line += glyph;
    }
    if (!line.empty() && static_cast<int>(out.size()) < maxLines) out.push_back(line);
}

// ── 版式 ─────────────────────────────────────────────────────────────────
int headerHeight() {
    FontScope ui(FontRenderer::uiPxHeight());
    return g_font.lineHeight() + 12;
}
int footerHeight() {
    return pxLine(kSmallPx) + 8;
}
int contentTop() { return headerHeight() + 6; }
int contentBottom() { return SCREEN_H - footerHeight() - 4; }

struct Bounds {
    int x, y, w, h;
};
Bounds canvasBounds() {
    const int top = contentTop() + 2;
    const int bottom = contentBottom();
    return Bounds{kSidePadding, top, SCREEN_W - 2 * kSidePadding, std::max(1, bottom - top)};
}

int listTop() {
    int y = contentTop() + 2;
    if (s.caption[0]) y += pxLine(kTextPx) + 8;
    return y;
}
int visibleRows() {
    return std::max(1, (contentBottom() - listTop()) / kRowHeight);
}
int tileColumns() {
    const int cols = s.columns > 0 ? s.columns : 1;
    return std::min(cols, std::max(1, (SCREEN_W - 2 * kSidePadding) / 76));
}
int tileCellW() {
    const int cols = tileColumns();
    constexpr int gap = 8;
    return (SCREEN_W - 2 * kSidePadding - gap * (cols - 1)) / cols;
}
int visibleTileRows() {
    return std::max(1, (contentBottom() - listTop()) / kTileHeight);
}

// ── 画布 ─────────────────────────────────────────────────────────────────
uint8_t *s_fb = nullptr;
Bounds s_bounds{0, 0, 0, 0};

uint8_t shade4(uint8_t gray) {
    if (gray <= 2) return 0;
    if (gray <= 7) return 5;
    if (gray <= 12) return 10;
    return 15;
}
void fillRect(int x, int y, int w, int h, uint8_t gray) {
    if (!s_fb || w <= 0 || h <= 0) return;
    const int x0 = std::max(x, s_bounds.x);
    const int y0 = std::max(y, s_bounds.y);
    const int x1 = std::min(x + w, s_bounds.x + s_bounds.w);
    const int y1 = std::min(y + h, s_bounds.y + s_bounds.h);
    if (x1 > x0 && y1 > y0)
        fb_fast_fill_rect(s_fb, x0, y0, x1 - x0, y1 - y0, static_cast<uint8_t>(gray << 4));
}
void strokeRect(int x, int y, int w, int h, int thickness, uint8_t gray) {
    if (thickness <= 0 || w <= 0 || h <= 0) return;
    const int t = std::min(thickness, std::min(w, h));
    fillRect(x, y, w, t, gray);
    fillRect(x, y + h - t, w, t, gray);
    fillRect(x, y + t, t, h - 2 * t, gray);
    fillRect(x + w - t, y + t, t, h - 2 * t, gray);
}

// 圆角矩形的每一行相对左右边各缩进多少。参考实现把 Box 的 radius / Circle 的
// 半高直接交给渲染器的圆角填充；这里没有那层 API，用行扫描近似。
int roundInset(int row, int h, int r) {
    if (r <= 0 || h <= 0) return 0;
    if (r > h / 2) r = h / 2;
    if (row < 0 || row >= h) return 0;
    int dy = 0;
    if (row < r) dy = r - 1 - row;
    else if (row >= h - r) dy = row - (h - r);
    else return 0;
    const int d = static_cast<int>(std::sqrt(static_cast<double>(r) * r - static_cast<double>(dy) * dy));
    return r - d;
}
void fillRoundedRect(int x, int y, int w, int h, int r, uint8_t gray) {
    if (r <= 0) {
        fillRect(x, y, w, h, gray);
        return;
    }
    for (int row = 0; row < h; ++row) {
        const int inset = roundInset(row, h, r);
        fillRect(x + inset, y + row, w - 2 * inset, 1, gray);
    }
}
void strokeRoundedRect(int x, int y, int w, int h, int thickness, int r, uint8_t gray) {
    if (thickness <= 0 || w <= 0 || h <= 0) {
        return;
    }
    if (r <= 0) {
        strokeRect(x, y, w, h, thickness, gray);
        return;
    }
    const int t = std::min(thickness, std::min(w, h));
    const int ir = std::max(0, r - t);
    for (int row = 0; row < h; ++row) {
        const int oi = roundInset(row, h, r);
        if (row < t || row >= h - t) {
            fillRect(x + oi, y + row, w - 2 * oi, 1, gray);
            continue;
        }
        const int ii = roundInset(row - t, h - 2 * t, ir);
        fillRect(x + oi, y + row, std::max(0, ii - oi), 1, gray);
        fillRect(x + w - ii, y + row, std::max(0, ii - oi), 1, gray);
    }
}
void drawLine(int x0, int y0, int x1, int y1, int thickness, uint8_t gray) {
    if (!s_fb) return;
    const int t = std::max(1, thickness);
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int oy = 0; oy < t; ++oy)
            for (int ox = 0; ox < t; ++ox) {
                const int x = x0 + ox - t / 2, y = y0 + oy - t / 2;
                if (x >= s_bounds.x && x < s_bounds.x + s_bounds.w && y >= s_bounds.y &&
                    y < s_bounds.y + s_bounds.h)
                    fb_fast_set_gray(s_fb, x, y, gray);
            }
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void drawCanvasText(const DrawCommand &c, const char *label) {
    if (!label || !*label) return;
    const int px = kFontPx[std::min<int>(3, std::max<int>(0, c.font))];
    const bool bold = c.font >= 2;
    const bool invert = c.gray >= 13;
    const int lineHeight = std::max(1, pxLine(px));
    const int maxLines = std::max(1, c.height / lineHeight);
    std::vector<std::string> lines;
    wrapText(px, label, c.width, maxLines, lines);
    int baseline = s_bounds.y + c.y + pxAscent(px);
    for (const auto &line : lines) {
        const int width = pxWidth(px, line.c_str());
        int x = s_bounds.x + c.x;
        if (c.align == 1)
            x += (c.width - width) / 2;
        else if (c.align == 2)
            x += c.width - width;
        drawTextPx(px, x, baseline, line.c_str(), bold, invert);
        baseline += lineHeight;
        if (baseline - pxAscent(px) > s_bounds.y + c.y + c.height) break;
    }
}

void drawArrowButton(int cx, int cy, int width, int height, const char *label, uint8_t ink) {
    const int half = std::max(4, std::min(width, height) / 5);
    const bool left = !std::strcmp(label, "←"), right = !std::strcmp(label, "→");
    const bool up = !std::strcmp(label, "↑"), down = !std::strcmp(label, "↓");
    const bool erase = !std::strcmp(label, "⌫");
    if (erase) {
        drawLine(cx - half, cy, cx - half / 2, cy - half / 2, 2, ink);
        drawLine(cx - half, cy, cx - half / 2, cy + half / 2, 2, ink);
        drawLine(cx - half / 2, cy - half / 2, cx + half, cy - half / 2, 2, ink);
        drawLine(cx - half / 2, cy + half / 2, cx + half, cy + half / 2, 2, ink);
        drawLine(cx + half, cy - half / 2, cx + half, cy + half / 2, 2, ink);
        drawLine(cx, cy - half / 4, cx + half / 2, cy + half / 4, 2, ink);
        drawLine(cx, cy + half / 4, cx + half / 2, cy - half / 4, 2, ink);
    } else if (left || right) {
        const int tip = cx + (left ? -half : half), back = tip + (left ? half : -half);
        drawLine(cx - half, cy, cx + half, cy, 2, ink);
        drawLine(tip, cy, back, cy - half, 2, ink);
        drawLine(tip, cy, back, cy + half, 2, ink);
    } else if (up || down) {
        const int tip = cy + (up ? -half : half), back = tip + (up ? half : -half);
        drawLine(cx, cy - half, cx, cy + half, 2, ink);
        drawLine(cx, tip, cx - half, back, 2, ink);
        drawLine(cx, tip, cx + half, back, 2, ink);
    }
}

void drawQr(const DrawCommand &c) {
    if (!s.qrReady || !s.qrContent[0]) return;
    if (!qrcodegen_encodeText(s.qrContent, s_qrTemp, s_qrCode, qrcodegen_Ecc_MEDIUM, 1, 16,
                              qrcodegen_Mask_AUTO, true))
        return;
    const int size = qrcodegen_getSize(s_qrCode);
    constexpr int quiet = 4;
    const int modules = size + quiet * 2;
    const int scale = std::max(1, c.width / modules);
    const int bx = s_bounds.x + c.x + (c.width - modules * scale) / 2;
    const int by = s_bounds.y + c.y + (c.height - modules * scale) / 2;
    fillRect(s_bounds.x + c.x, s_bounds.y + c.y, c.width, c.height, 15);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if (qrcodegen_getModule(s_qrCode, x, y))
                fillRect(bx + (x + quiet) * scale, by + (y + quiet) * scale, scale, scale, 0);
}

void drawCanvas() {
    if (!s_fb || !s.canvas) return;
    const Canvas &cv = *s.canvas;
    if (cv.controlCount && (s.sel < 0 || static_cast<size_t>(s.sel) >= cv.controlCount)) s.sel = 0;
    const int selected = (cv.controlCount && s.sel >= 0 && static_cast<size_t>(s.sel) < cv.controlCount)
                             ? cv.controls[s.sel]
                             : -1;
    for (size_t i = 0; i < cv.count; ++i) {
        const DrawCommand &c = cv.commands[i];
        const char *label = cv.text + c.text;
        switch (c.kind) {
            case DrawKind::Box:
            case DrawKind::Circle: {
                const int x = s_bounds.x + c.x, y = s_bounds.y + c.y;
                // Circle 的半径是 min(w,h)/2（参考实现如此，宽 ≠ 高时得到胶囊形）；
                // Box 用命令自带的 radius。0 = 直角，走原来的矩形快路径。
                const int radius = (c.kind == DrawKind::Circle)
                                       ? std::min<int>(c.width, c.height) / 2
                                       : std::min<int>(c.radius, std::min<int>(c.width, c.height) / 2);
                if (c.style)
                    strokeRoundedRect(x, y, c.width, c.height, c.style, radius, shade4(c.gray));
                else
                    fillRoundedRect(x, y, c.width, c.height, radius, shade4(c.gray));
                break;
            }
            case DrawKind::Line: {
                const int x = s_bounds.x + c.x;
                const int y0 = s_bounds.y + c.y + (c.radius ? c.height - 1 : 0);
                const int y1 = s_bounds.y + c.y + (c.radius ? 0 : c.height - 1);
                drawLine(x, y0, x + c.width - 1, y1, std::max<int>(1, c.style), shade4(c.gray));
                break;
            }
            case DrawKind::Text:
                drawCanvasText(c, label);
                break;
            case DrawKind::Button: {
                const bool dark = c.style == 3;
                const uint8_t background = dark ? 0 : (c.style == 2 ? 10 : 15);
                const uint8_t foreground = dark ? 15 : 0;
                const int x = s_bounds.x + c.x, y = s_bounds.y + c.y;
                // 参考实现：圆角 min(16, 高/4)。
                const int radius = std::min<int>(16, c.height / 4);
                fillRoundedRect(x, y, c.width, c.height, radius, background);
                if (c.style != 1)
                    strokeRoundedRect(x, y, c.width, c.height, 1, radius, foreground);
                const bool vector = !std::strcmp(label, "←") || !std::strcmp(label, "→") ||
                                    !std::strcmp(label, "↑") || !std::strcmp(label, "↓") ||
                                    !std::strcmp(label, "⌫");
                if (vector)
                    drawArrowButton(x + c.width / 2, y + c.height / 2, c.width, c.height, label, foreground);
                else {
                    const std::string shown = fitText(kTextPx, label, c.width - 8);
                    drawCenteredPx(kTextPx, x + c.width / 2,
                                   y + (c.height + pxAscent(kTextPx)) / 2 - 1, shown.c_str(),
                                   c.style >= 2, dark);
                }
                if (static_cast<int>(i) == selected)
                    strokeRoundedRect(x + 3, y + 3, c.width - 6, c.height - 6, 2,
                                      std::max(0, radius - 3), foreground);
                break;
            }
            case DrawKind::Hit:
                break;
            case DrawKind::Qr:
                drawQr(c);
                break;
            case DrawKind::Raster: {
                const int width = c.radius, height = c.font;
                if (width <= 0 || height <= 0) break;
                for (int y = 0; y < height; ++y) {
                    int x = 0;
                    while (x < width) {
                        const char value = label[y * width + x];
                        int end = x + 1;
                        while (end < width && label[y * width + end] == value) ++end;
                        const uint8_t gray = value <= '9' ? static_cast<uint8_t>(value - '0')
                                                          : static_cast<uint8_t>(value - 'A' + 10);
                        const int left = s_bounds.x + c.x + x * c.width / width;
                        const int top = s_bounds.y + c.y + y * c.height / height;
                        const int right = s_bounds.x + c.x + end * c.width / width;
                        const int bottom = s_bounds.y + c.y + (y + 1) * c.height / height;
                        fillRect(left, top, right - left, bottom - top, gray);
                        x = end;
                    }
                }
                break;
            }
        }
    }
}

// ── 系统服务 ─────────────────────────────────────────────────────────────
void dateFromEpoch(uint32_t epoch, char *out, size_t capacity) {
    if (capacity) out[0] = 0;
    const uint32_t ordinal = RdTime::dayOrdinal(epoch);
    if (ordinal) RdTime::formatOrdinal(ordinal, out, capacity);
}

void syncTask(void *) {
    bool was = g_wifi.isConnected();
    if (!was && !app_connect_wifi_from_settings(&was)) {
        s_syncState.store(3, std::memory_order_relaxed);
        vTaskDelete(nullptr);
        return;
    }
    std::string ntp = g_settings.ntpServer();
    if (ntp.empty()) ntp = "pool.ntp.org";
    std::string tz = g_settings.timezone();
    if (tz.empty()) tz = "CST-8";
    esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, ntp.c_str());
    esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    esp_sntp_init();
    setenv("TZ", tz.c_str(), 1);
    tzset();
    time_t now = 0;
    for (int i = 0; i < 100; ++i) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            time(&now);
            break;
        }
    }
    esp_sntp_stop();
    const bool ok = now > 1704067200;
    if (ok) g_rtc.setTime(now);
    app_disconnect_wifi_if_needed(was);
    s_syncState.store(ok ? 2 : 3, std::memory_order_relaxed);
    vTaskDelete(nullptr);
}

void startSync() {
    if (s_syncState.load(std::memory_order_relaxed) == 1) return;
    s_syncState.store(1, std::memory_order_relaxed);
    s_syncStartedUs = esp_timer_get_time();
    if (xTaskCreate(syncTask, "lua_ntp", 4096, nullptr, 2, nullptr) != pdPASS)
        s_syncState.store(3, std::memory_order_relaxed);
}

// ── Host ─────────────────────────────────────────────────────────────────
class LuaHost final : public Host {
  public:
    LuaHost() {
        // 授权位一次读出来（SD 上的 /.crosspoint 权限文件）。原来 allowed() 每次现读，
        // 而每条 app.system 要查四种权限、历史页一个回调又要发六七次 system：
        // 一次点击就是几十次 SD stat/open，累计起来能吃掉 CrossStore 给单个回调的
        // 1.5s 宿主 I/O 预算（"application I/O budget exceeded"）。应用开着的时候
        // 商店设置进不来，授权不会变，缓存到这次会话结束没有语义风险。
        permissions_ = grantedPermissions(s.id);
        ESP_LOGI(kTAG, "应用 %s 授权位=0x%02x（缓存）", s.id, static_cast<unsigned>(permissions_));
    }
    void *resizeAllocation(void *ptr, size_t bytes) override {
        if (!bytes) {
            heap_caps_free(ptr);
            return nullptr;
        }
        void *out = heap_caps_realloc(ptr, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out) {
            // realloc 失败时原块还在；拷过去再放，别让 Lua 堆指针悬空。
            out = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!out) out = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
            if (out && ptr) {
                const size_t old = heap_caps_get_allocated_size(ptr);
                std::memcpy(out, ptr, std::min(old, bytes));
            }
            if (out) heap_caps_free(ptr);
        }
        return out;
    }

    void title(const char *text) override {
        std::snprintf(s.heading, sizeof(s.heading), "%s", text ? text : "");
        s.dirty = true;
    }
    void clear() override {
        if (s.canvas) s.canvas->clear();
        s.canvasMode = false;
        s.qrReady = false;
        s.rowCount = 0;
        s.columns = 0;
        s.caption[0] = 0;
        s.sel = 0;
        s.scroll = 0;
        s.dirty = true;
    }
    void grid(unsigned columns) override {
        s.columns = static_cast<int>(std::min<unsigned>(columns, 8));
        s.canvasMode = false;
        s.dirty = true;
    }
    void caption(const char *text) override {
        std::snprintf(s.caption, sizeof(s.caption), "%s", text ? text : "");
        s.dirty = true;
    }
    bool row(const char *label, const char *value) override {
        if (s.rowCount >= static_cast<int>(MAX_ROWS)) return false;
        std::snprintf(s.rowsLabel[s.rowCount], sizeof(s.rowsLabel[0]), "%s", label ? label : "");
        std::snprintf(s.rowsValue[s.rowCount], sizeof(s.rowsValue[0]), "%s", value ? value : "");
        ++s.rowCount;
        s.dirty = true;
        return true;
    }
    bool capability(const char *name) const override {
        return name && (deviceCapabilities() & capabilityBit(name)) != 0;
    }
    bool chinese() const override { return true; }
    uint32_t milliseconds() const override {
        return static_cast<uint32_t>(esp_timer_get_time() / 1000);
    }
    bool read(const char *key, char *out, size_t capacity) override {
        return appdataRead(s.id, key, out, capacity);
    }
    bool write(const char *key, const char *value, size_t size) override {
        return appdataWrite(s.id, key, value, size);
    }
    void input(const char *, const char *) override {
        ui_toast_show("此应用需要键盘输入，当前固件暂不支持", 0, 1800);
        s.toastUntilUs = esp_timer_get_time() + 1800000;
        s.dirty = true;
    }
    void notify(const char *message) override {
        if (!allowed("notifications")) return;
        ui_toast_show(message ? message : "", 0, 1600);
        s.toastUntilUs = esp_timer_get_time() + 1600000;
        s.dirty = true;
    }
    bool open(const char *route) override {
        if (!route) return false;
        if (std::strcmp(route, "library") && std::strcmp(route, "file_transfer") && std::strcmp(route, "opds"))
            return false;
        if (!std::strcmp(route, "opds") && !allowed("network")) return false;
        std::snprintf(s.pendingRoute, sizeof(s.pendingRoute), "%s", route);
        return true;
    }
    void size(int &width, int &height) const override {
        const Bounds b = canvasBounds();
        width = b.w;
        height = b.h;
    }
    bool canvas() override {
        if (!s.canvas) {
            void *memory = heap_caps_malloc(sizeof(Canvas), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!memory) return false;
            s.canvas = new (memory) Canvas();
        }
        s.canvasMode = true;
        s.qrReady = false;
        s.sel = 0;
        s.dirty = true;
        return true;
    }
    bool draw(const DrawCommand &command, const char *label) override {
        if (!s.canvasMode || !s.canvas) return false;
        if (command.kind == DrawKind::Button || command.kind == DrawKind::Hit) {
            const Bounds b = canvasBounds();
            if (command.x < 0 || command.y < 0 || command.x + command.width > b.w ||
                command.y + command.height > b.h)
                return false;
        }
        s.dirty = true;
        return s.canvas->add(command, label ? label : "");
    }
    void exit() override { s.exitRequested = true; }
    bool asset(const char *name, size_t offset, char *out, size_t capacity, size_t &bytes) override {
        if (!name || std::strncmp(name, "assets/", 7) || std::strstr(name, "..") || std::strchr(name, '\\'))
            return false;
        return readPackageEntryInto(s.path, name, offset, reinterpret_cast<uint8_t *>(out), capacity, bytes);
    }
    void measure(const char *text, int maximum, unsigned font, int &width, int &height) override {
        const int px = kFontPx[std::min<unsigned>(3, font)];
        std::vector<std::string> lines;
        wrapText(px, text ? text : "", std::max(1, maximum), 12, lines);
        width = 0;
        for (const auto &line : lines) width = std::max(width, pxWidth(px, line.c_str()));
        height = static_cast<int>(lines.size()) * pxLine(px);
    }
    const char *context() const override { return ""; }
    bool system(const char *command, const char *argument, int64_t index, ServiceReply &reply) override;
    bool chat(const char *, const char *, int64_t, const char *, ServiceReply &) override { return false; }
    bool qr(int x, int y, int size, const char *content) override {
        if (!s.canvas || !s.canvasMode || s.qrReady || !content) return false;
        const Bounds b = canvasBounds();
        const size_t length = std::strlen(content);
        if (x < 0 || y < 0 || size < 90 || x + size > b.w || y + size > b.h) return false;
        if (length == 0 || length >= sizeof(s.qrContent)) return false;
        std::snprintf(s.qrContent, sizeof(s.qrContent), "%s", content);
        DrawCommand command;
        command.kind = DrawKind::Qr;
        command.x = static_cast<int16_t>(x);
        command.y = static_cast<int16_t>(y);
        command.width = command.height = static_cast<int16_t>(size);
        s.qrReady = s.canvas->add(command, "");
        s.dirty = true;
        return s.qrReady;
    }
    bool sound(const char *cue) override {
        if (!cue || !allowed("sound")) return false;
        typingClickPlay(1);
        return true;
    }
    bool allowed(const char *name) const override {
        if (!name) return false;
        const uint32_t bit = permissionBit(name);
        return bit && (s.pkg.permissions & bit) && (permissions_ & bit);
    }

  private:
    uint32_t permissions_ = 0;
};

bool LuaHost::system(const char *command, const char *argument, int64_t index, ServiceReply &reply) {
    if (!command) return false;
    // AirPage 原生服务。权限位在这里一次算好交给服务；服务自己只知道"能不能"，
    // 不认识 CrossStore 的包/授权表。
    if (!std::strncmp(command, "airpage.", 8)) {
        const int64_t startedUs = esp_timer_get_time();
        airpage_service::Grants grants;
        grants.network = allowed("network");
        grants.device_info = allowed("device_info");
        grants.wallpaper = allowed("wallpaper");
        grants.keep_awake = allowed("keep_awake");
        const int before = airpage_service::version();
        const bool beforeOverlay = airpage_service::overlayActive();
        const std::string beforePath = airpage_service::overlayPath();
        const bool ok = airpage_service::system(command + 8, argument, index, reply, grants);
        // 轮询拿到新图时 Lua 的 refresh 会自己重画；图片视图里它不重画，
        // 但那正是"屏上还是那张图"——换过图才需要宿主补一帧。
        // show/hide 换的是**覆盖层的有没有**，swipe 换的是覆盖层里的**哪张**：
        // Lua 那边 image() 成功后不调 show()，不在这里补脏的话，点「查看图片」
        // 或左右滑都画不出来。
        if (ok && (airpage_service::version() != before ||
                   airpage_service::overlayActive() != beforeOverlay ||
                   airpage_service::overlayPath() != beforePath))
            s.dirty = true;
        // 这里计的是 accountHostIo 看到的那段墙钟（含四次权限查询），
        // 跟 airpage_service 自己的 ActionTimer 不是一回事。
        const int64_t ms = (esp_timer_get_time() - startedUs) / 1000;
        if (ms >= 100) ESP_LOGW(kTAG, "%s 用时 %lld ms", command, ms);
        return ok;
    }
    if (!std::strcmp(command, "canvas.export")) {
        if (!allowed("files_export") || !argument || !validId(argument) || !s.canvasMode || !s.canvas)
            return false;
        std::snprintf(s.pendingExport, sizeof(s.pendingExport), "%s", argument);
        return true;
    }
    if (!std::strcmp(command, "power.keep_awake")) {
        if (!argument || (std::strcmp(argument, "on") && std::strcmp(argument, "off"))) return false;
        const bool enable = !std::strcmp(argument, "on");
        if (enable && !allowed("keep_awake")) return false;
        g_lua_keep_awake = enable;
        return true;
    }
    if (!std::strcmp(command, "device.sync")) {
        if (!allowed("network")) return false;
        if (!g_wifi.isConnected()) {
            std::snprintf(reply.text, sizeof(reply.text), "connecting");
            startSync();
            return false;
        }
        startSync();
        return s_syncState.load(std::memory_order_relaxed) == 1;
    }
    if (!std::strcmp(command, "device.sync_status")) {
        if (!allowed("network")) return false;
        const int state = s_syncState.load(std::memory_order_relaxed);
        if (state == 1) {
            if (esp_timer_get_time() - s_syncStartedUs < 15000000) return false;
            s_syncState.store(3, std::memory_order_relaxed);
            std::snprintf(reply.text, sizeof(reply.text), "failed");
            return true;
        }
        if (state == 2) {
            std::snprintf(reply.text, sizeof(reply.text), "success");
            s_syncState.store(0, std::memory_order_relaxed);
            return true;
        }
        if (state == 3) {
            std::snprintf(reply.text, sizeof(reply.text), "failed");
            s_syncState.store(0, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
    if (!std::strcmp(command, "device.time")) {
        if (!g_rtc.hasValidTime()) return false;
        const time_t now = g_rtc.getTime();
        struct tm local {};
        localtime_r(&now, &local);
        std::strftime(reply.text, sizeof(reply.text), "%H:%M", &local);
        std::strftime(reply.extra, sizeof(reply.extra), "%Y-%m-%d", &local);
        reply.a = local.tm_wday;
        reply.b = local.tm_sec;
        reply.c = static_cast<int64_t>(now);
        return true;
    }
    if (!std::strcmp(command, "device.battery")) {
        reply.a = battery_pct();
        return true;
    }
    if (!std::strcmp(command, "device.id")) {
        if (!allowed("device_info")) return false;
        uint8_t mac[6]{};
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return false;
        std::snprintf(reply.text, sizeof(reply.text), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2],
                      mac[3], mac[4], mac[5]);
        return true;
    }

    if (std::strncmp(command, "reading.", 8) || !allowed("reading_data")) return false;
    const auto &days = ReadingStats::readingDays();
    const auto &books = ReadingStats::books();

    if (!std::strcmp(command, "reading.calendar")) {
        uint32_t reference = RdTime::todayOrdinal();
        if (!reference && !days.empty()) reference = days.back().dayOrdinal;
        int year = 0;
        unsigned month = 0, day = 0;
        if (argument && *argument) {
            if (!crossstore::reading::parseMonth(argument, year, month)) return false;
        } else {
            if (!reference || !RdTime::dateFromOrdinal(reference, year, month, day)) return false;
        }
        std::snprintf(reply.text, sizeof(reply.text), "%04d-%02u", year, month);
        reply.a = RdTime::ordinalForDate(year, month, 1);
        reply.b = RdTime::daysInMonth(year, month);
        reply.c = reference;
        return true;
    }
    if (!std::strcmp(command, "reading.date")) {
        const uint32_t maximum = RdTime::ordinalForDate(2106, 12, 31);
        if (index < 0 || static_cast<uint64_t>(index) > maximum) return false;
        if (index == 0)
            std::snprintf(reply.text, sizeof(reply.text), "1970-01-01");
        else
            RdTime::formatOrdinal(static_cast<uint32_t>(index), reply.text, sizeof(reply.text));
        reply.a = index;
        reply.b = static_cast<int64_t>(
            crossstore::reading::millisecondsOnDay(days, static_cast<uint32_t>(index)));
        return true;
    }
    if (!std::strcmp(command, "reading.date_book")) {
        const uint32_t maximum = RdTime::ordinalForDate(2106, 12, 31);
        uint32_t day = 0;
        bool reverse = false;
        if (!argument || !crossstore::reading::parseDay(argument, maximum, day, reverse)) return false;
        if (index < 1 || static_cast<uint64_t>(index) > books.size()) return false;
        const auto match = crossstore::reading::nextBookOnDay(books, day, static_cast<size_t>(index), reverse);
        reply.a = static_cast<int64_t>(match.book);
        reply.b = static_cast<int64_t>(match.milliseconds);
        reply.c = static_cast<int64_t>(match.next);
        return true;
    }
    if (!std::strcmp(command, "reading.achievements")) {
        reply.c = 0;  // 本机没有成就系统；应用会显示"未启用"
        return true;
    }
    if (!std::strcmp(command, "reading.achievement")) return false;
    if (!std::strcmp(command, "reading.summary")) {
        reply.a = static_cast<int64_t>(ReadingStats::totalReadingMs());
        reply.b = static_cast<int64_t>(ReadingStats::todayReadingMs());
        reply.c = static_cast<int64_t>(ReadingStats::recentReadingMs(7));
        return true;
    }
    if (!std::strcmp(command, "reading.counts")) {
        reply.a = ReadingStats::booksStarted();
        reply.b = ReadingStats::booksFinished();
        reply.c = ReadingStats::currentStreakDays();
        return true;
    }
    if (!std::strcmp(command, "reading.goal")) {
        reply.a = static_cast<int64_t>(ReadingStats::goalMs());
        reply.b = ReadingStats::maxStreakDays();
        reply.c = ReadingStats::readDaysCount();
        return true;
    }
    if (!std::strcmp(command, "reading.day")) {
        if (index < 1 || static_cast<size_t>(index) > days.size()) return false;
        const auto &day = days[index - 1];
        reply.a = day.dayOrdinal;
        reply.b = static_cast<int64_t>(day.readingMs);
        return true;
    }
    if (!std::strcmp(command, "reading.book") || !std::strcmp(command, "reading.open") ||
        !std::strcmp(command, "reading.book_detail")) {
        if (index < 1 || static_cast<size_t>(index) > books.size()) return false;
        const auto &book = books[index - 1];
        if (!std::strcmp(command, "reading.book_detail")) {
            char first[24]{}, last[24]{}, completed[24]{};
            dateFromEpoch(book.firstReadAt, first, sizeof(first));
            dateFromEpoch(book.lastReadAt, last, sizeof(last));
            dateFromEpoch(book.completedAt, completed, sizeof(completed));
            std::snprintf(reply.text, sizeof(reply.text), "%s", book.chapterTitle.c_str());
            std::snprintf(reply.extra, sizeof(reply.extra), "%s|%s|%s", first, last, completed);
            reply.a = book.sessions;
            reply.b = book.lastSessionMs;
            reply.c = book.chapterProgressPercent;
            return true;
        }
        if (!std::strcmp(command, "reading.open")) {
            if (book.path.empty() || book.path.size() >= sizeof(s.pendingBook)) return false;
            std::snprintf(s.pendingBook, sizeof(s.pendingBook), "%s", book.path.c_str());
            std::snprintf(s.pendingRoute, sizeof(s.pendingRoute), "reading_book");
            return true;
        }
        std::snprintf(reply.text, sizeof(reply.text), "%s",
                      book.title.empty() ? book.path.c_str() : book.title.c_str());
        std::snprintf(reply.extra, sizeof(reply.extra), "%s", book.author.c_str());
        reply.a = static_cast<int64_t>(book.totalReadingMs);
        reply.b = book.lastProgressPercent;
        reply.c = book.completed ? 1 : 0;
        return true;
    }
    return false;
}

// ── 导出 SVG ─────────────────────────────────────────────────────────────
bool exportCanvas(const char *name, std::string &pathOut) {
    if (!s.canvas || !s.canvasMode || !validId(name) || !permissionGranted(s.id, "files_export")) return false;
    const std::string directoryPath = std::string("/CrossStore/") + s.id;
    if (!Storage.ensureDirectoryExists(directoryPath.c_str())) return false;
    auto directory = Storage.open(directoryPath.c_str());
    if (!directory || !directory.isDirectory()) return false;
    size_t files = 0;
    while (auto entry = directory.openNextFile()) {
        if (++files >= 32) return false;
    }
    directory.close();
    const std::string path = directoryPath + "/" + name + ".svg";
    const std::string temporary = path + ".part";
    const Bounds b = canvasBounds();
    bool ok = false;
    {
        auto file = Storage.open(temporary.c_str(), FS_WRONLY | FS_CREAT | FS_TRUNC);
        if (file) {
            ok = writeSvg(*s.canvas, b.w, b.h, [&](const char *data, size_t bytes) {
                return file.write(reinterpret_cast<const uint8_t *>(data), bytes) == bytes;
            });
            file.flush();
            file.close();
        }
    }
    if (ok) {
        ok = commitFile(Storage, temporary.c_str(), path);
        if (ok) pathOut = path;
    }
    if (!ok) Storage.remove(temporary.c_str());
    return ok;
}

// ── 渲染 ─────────────────────────────────────────────────────────────────
void drawHeader() {
    FontScope ui(FontRenderer::uiPxHeight());
    const int baseline = g_font.ascent() + 5;
    const std::string title = s.heading[0] ? s.heading : (s.pkg.name[0] ? s.pkg.name : s.id);
    const int rightReserve = 90;
    drawTextPx(FontRenderer::uiPxHeight(), kSidePadding, baseline,
               fitText(FontRenderer::uiPxHeight(), title, SCREEN_W - 2 * kSidePadding - rightReserve).c_str(),
               true);
    const char *hint = "Esc 返回";
    drawTextPx(kSmallPx, SCREEN_W - kSidePadding - pxWidth(kSmallPx, hint), baseline, hint);
    uint8_t *fb = ui_render_begin_frame();
    if (fb) {
        fb_fast_sync();
        fb_fast_fill_rect(fb, 0, headerHeight() - 1, SCREEN_W, 1, 0x00);
    }
}

void drawFooter(bool navigation) {
    const int baseline = SCREEN_H - 9;
    if (navigation)
        drawTextPx(kSmallPx, kSidePadding, baseline, "↑↓ 选择  Enter 打开");
    else
        drawTextPx(kSmallPx, kSidePadding, baseline, "按应用提示操作");
    const char *hint = "Esc 返回";
    drawTextPx(kSmallPx, SCREEN_W - kSidePadding - pxWidth(kSmallPx, hint), baseline, hint);
}

void renderList() {
    drawHeader();
    if (s.caption[0]) {
        const int y = contentTop() + 2 + pxAscent(kTextPx);
        drawCenteredPx(kTextPx, SCREEN_W / 2, y,
                       fitText(kTextPx, s.caption, SCREEN_W - 2 * kSidePadding).c_str());
    }
    const int top = listTop();
    const int rows = visibleRows();
    if (s.rowCount == 0) {
        if (!s.caption[0]) drawCenteredPx(kTextPx, SCREEN_W / 2, SCREEN_H / 2, "应用没有绘制内容");
        drawFooter(false);
        return;
    }
    if (s.sel >= s.rowCount) s.sel = s.rowCount - 1;
    if (s.sel < 0) s.sel = 0;
    if (s.sel < s.scroll) s.scroll = s.sel;
    if (s.sel >= s.scroll + rows) s.scroll = s.sel - rows + 1;
    if (s.scroll < 0) s.scroll = 0;
    uint8_t *fb = ui_render_begin_frame();
    for (int visible = 0; visible < rows; ++visible) {
        const int index = s.scroll + visible;
        if (index >= s.rowCount) break;
        const int y = top + visible * kRowHeight;
        const bool selected = index == s.sel;
        if (selected && fb) {
            fb_fast_sync();
            fb_fast_fill_rect(fb, kSidePadding - 6, y, SCREEN_W - 2 * kSidePadding + 12, kRowHeight - 2,
                              0x00);
        }
        const std::string value = s.rowsValue[index];
        int labelWidth = SCREEN_W - 2 * kSidePadding - 12;
        if (!value.empty())
            labelWidth -= std::min(SCREEN_W / 2, pxWidth(kSmallPx, value.c_str())) + 20;
        drawTextPx(kTextPx, kSidePadding, y + 30,
                   fitText(kTextPx, s.rowsLabel[index], std::max(20, labelWidth)).c_str(), true, selected);
        if (!value.empty()) {
            const std::string shown = fitText(kSmallPx, value, SCREEN_W / 2);
            drawTextPx(kSmallPx, SCREEN_W - kSidePadding - pxWidth(kSmallPx, shown.c_str()), y + 34,
                       shown.c_str(), false, selected);
        }
        if (fb && !selected) {
            fb_fast_sync();
            fb_fast_fill_rect(fb, kSidePadding, y + kRowHeight - 1, SCREEN_W - 2 * kSidePadding, 1, 0x00);
        }
    }
    drawFooter(true);
}

void renderTiles() {
    drawHeader();
    if (s.caption[0]) {
        const int y = contentTop() + 2 + pxAscent(kTextPx);
        drawCenteredPx(kTextPx, SCREEN_W / 2, y,
                       fitText(kTextPx, s.caption, SCREEN_W - 2 * kSidePadding).c_str());
    }
    const int cols = tileColumns();
    const int cellW = tileCellW();
    const int rowsVisible = visibleTileRows();
    const int top = listTop();
    const int count = s.rowCount;
    if (count == 0) {
        drawFooter(false);
        return;
    }
    if (s.sel >= count) s.sel = count - 1;
    if (s.sel < 0) s.sel = 0;
    const int selRow = s.sel / cols;
    if (selRow < s.scroll) s.scroll = selRow;
    if (selRow >= s.scroll + rowsVisible) s.scroll = selRow - rowsVisible + 1;
    if (s.scroll < 0) s.scroll = 0;
    uint8_t *fb = ui_render_begin_frame();
    constexpr int gap = 8;
    for (int r = 0; r < rowsVisible; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int index = (s.scroll + r) * cols + c;
            if (index >= count) break;
            const int x = kSidePadding + c * (cellW + gap);
            const int y = top + r * kTileHeight;
            const bool selected = index == s.sel;
            if (selected && fb) {
                fb_fast_sync();
                fb_fast_fill_rect(fb, x, y + 4, cellW, kTileHeight - 12, 0x00);
            }
            const std::string label = fitText(kTextPx, s.rowsLabel[index], cellW - 12);
            drawCenteredPx(kTextPx, x + cellW / 2, y + kTileHeight / 2 + pxAscent(kTextPx) / 2 - 4,
                           label.c_str(), true, selected);
            if (!selected && fb) {
                fb_fast_sync();
                fb_fast_fill_rect(fb, x, y + kTileHeight - 7, cellW, 1, 0x00);
                fb_fast_fill_rect(fb, x, y + 4, 1, kTileHeight - 12, 0x00);
                fb_fast_fill_rect(fb, x + cellW - 1, y + 4, 1, kTileHeight - 12, 0x00);
            }
        }
    }
    drawFooter(true);
}

void renderCanvasView() {
    drawHeader();
    s_fb = ui_render_begin_frame();
    s_bounds = canvasBounds();
    drawCanvas();
    s_fb = nullptr;
    drawFooter(false);
}

void renderError() {
    drawHeader();
    const int cx = SCREEN_W / 2;
    std::vector<std::string> lines;
    wrapText(kTextPx, s.errorText, SCREEN_W - 60, 5, lines);
    int y = SCREEN_H / 2 - static_cast<int>(lines.size()) * pxLine(kTextPx) / 2 + pxAscent(kTextPx);
    for (const auto &line : lines) {
        drawCenteredPx(kTextPx, cx, y, line.c_str(), true);
        y += pxLine(kTextPx) + 4;
    }
    drawFooter(false);
}

// ── AirPage 的整屏图片 ───────────────────────────────────────────────────
// Lua 在图片视图里只画过一个空画布（show() 里 view=='image' 直接 return），真正
// 的像素在这里铺。和待机「图片」表盘同一条路子：逐行解 BMP 直接写 framebuffer，
// fit + 居中，不落整屏中转缓冲。指纹/残留交给主循环的差分刷新。
struct AirpageOverlayCtx {
    uint8_t *fb;
};
void airpageOverlayRow(void *ctx, int x, int y, const uint8_t *row, int w) {
    uint8_t *fb = static_cast<AirpageOverlayCtx *>(ctx)->fb;
    for (int i = 0; i < w; ++i) fb_fast_set_gray(fb, x + i, y, row[i]);
}

void renderAirpageOverlay() {
    uint8_t *fb = ui_render_begin_frame();
    if (!fb) return;
    fb_fast_sync();
    AirpageOverlayCtx ctx{fb};
    int dw = 0, dh = 0, ox = 0, oy = 0;
    if (!readerCoverScaleTo(airpage_service::overlayPath(), SCREEN_W, SCREEN_H, 0, 0,
                            airpageOverlayRow, &ctx, dw, dh, ox, oy)) {
        drawCenteredPx(kTextPx, SCREEN_W / 2, SCREEN_H / 2, "图片打不开，按返回键退出");
    }
}

void render() {
    ui_clear();
    if (airpage_service::overlayActive())
        renderAirpageOverlay();
    else if (s.failed)
        renderError();
    else if (s.canvasMode && s.canvas)
        renderCanvasView();
    else if (s.columns > 0)
        renderTiles();
    else
        renderList();
    ui_commit();
    s.dirty = false;
}

// ── 事件 ─────────────────────────────────────────────────────────────────
void showError(const char *text) {
    std::snprintf(s.errorText, sizeof(s.errorText), "%s", text && *text ? text : "应用运行失败");
    s.failed = true;
    s.active = false;
    s.dirty = true;
}

bool dispatch(const char *event, int index = 0, const char *text = "", int x = -1, int y = -1) {
    if (!s_runtime) return false;
    if (!s_runtime->event(event, index, text, x, y)) {
        showError(s_runtime->error());
        return false;
    }
    return s_runtime->handled();
}

void navigateCanvas(int dx, int dy) {
    if (!s.canvas || s.canvas->controlCount == 0) return;
    if (s.sel < 0 || static_cast<size_t>(s.sel) >= s.canvas->controlCount) s.sel = 0;
    const DrawCommand &current = s.canvas->commands[s.canvas->controls[s.sel]];
    const int cx = current.x + current.width / 2, cy = current.y + current.height / 2;
    int best = -1;
    long bestScore = 0;
    for (size_t i = 0; i < s.canvas->controlCount; ++i) {
        if (static_cast<int>(i) == s.sel) continue;
        const DrawCommand &other = s.canvas->commands[s.canvas->controls[i]];
        const int ox = other.x + other.width / 2, oy = other.y + other.height / 2;
        const int vx = ox - cx, vy = oy - cy;
        if ((dx > 0 && vx <= 0) || (dx < 0 && vx >= 0) || (dy > 0 && vy <= 0) || (dy < 0 && vy >= 0)) continue;
        const long primary = dx ? std::abs(vx) : std::abs(vy);
        const long secondary = dx ? std::abs(vy) : std::abs(vx);
        const long score = primary * 4 + secondary;
        if (best < 0 || score < bestScore) {
            best = static_cast<int>(i);
            bestScore = score;
        }
    }
    if (best >= 0) {
        s.sel = best;
        s.dirty = true;
    }
}

void navigateList(int key) {
    const int count = s.rowCount;
    if (s.columns > 0) {
        const int cols = tileColumns();
        if (count <= 0) return;
        if (key == KEY_LEFT && s.sel % cols > 0) --s.sel;
        else if (key == KEY_RIGHT && s.sel % cols < cols - 1 && s.sel + 1 < count) ++s.sel;
        else if (key == KEY_UP && s.sel - cols >= 0) s.sel -= cols;
        else if (key == KEY_DOWN && s.sel + cols < count) s.sel += cols;
    } else {
        if (count <= 0) return;
        if (key == KEY_UP && s.sel > 0) --s.sel;
        else if (key == KEY_DOWN && s.sel + 1 < count) ++s.sel;
        else if (key == KEY_PAGE_UP) s.sel = std::max(0, s.sel - visibleRows());
        else if (key == KEY_PAGE_DOWN) s.sel = std::min(count - 1, s.sel + visibleRows());
    }
    s.dirty = true;
}

void navigate(int key) {
    if (s.canvasMode) {
        if (key == KEY_UP) navigateCanvas(0, -1);
        else if (key == KEY_DOWN) navigateCanvas(0, 1);
        else if (key == KEY_LEFT) navigateCanvas(-1, 0);
        else if (key == KEY_RIGHT) navigateCanvas(1, 0);
        return;
    }
    navigateList(key);
}

void activateSelected() {
    if (!s_runtime) return;
    if (s.canvasMode) {
        if (!s.canvas || s.canvas->controlCount == 0) return;
        if (s.sel < 0 || static_cast<size_t>(s.sel) >= s.canvas->controlCount) s.sel = 0;
        dispatch("on_select", s.canvas->commands[s.canvas->controls[s.sel]].action);
        return;
    }
    if (s.sel < 0 || s.sel >= s.rowCount) return;
    dispatch("on_select", s.sel + 1);
}

bool hitCanvas(int screenX, int screenY, bool longPress) {
    if (!s.canvas || !s.canvasMode) return false;
    const Bounds b = canvasBounds();
    const int x = screenX - b.x, y = screenY - b.y;
    if (x < 0 || y < 0 || x >= b.w || y >= b.h) return false;
    for (size_t i = 0; i < s.canvas->controlCount; ++i) {
        const DrawCommand &c = s.canvas->commands[s.canvas->controls[i]];
        if (x >= c.x && x < c.x + c.width && y >= c.y && y < c.y + c.height) {
            s.sel = static_cast<int>(i);
            dispatch("on_select", c.action, longPress ? "long" : "", x, y);
            return true;
        }
    }
    return false;
}

bool hitList(int screenX, int screenY) {
    if (s.canvasMode) return false;
    const int top = listTop();
    if (screenY < top || screenY >= contentBottom()) return false;
    if (s.columns > 0) {
        const int cols = tileColumns();
        const int cellW = tileCellW();
        const int col = (screenX - kSidePadding) / (cellW + 8);
        if (col < 0 || col >= cols) return false;
        const int row = (screenY - top) / kTileHeight;
        const int index = (s.scroll + row) * cols + col;
        if (index >= s.rowCount) return false;
        s.sel = index;
        dispatch("on_select", index + 1);
        return true;
    }
    const int index = s.scroll + (screenY - top) / kRowHeight;
    if (index >= s.rowCount) return false;
    s.sel = index;
    dispatch("on_select", index + 1);
    return true;
}

void applyPendingRoute() {
    if (!s.pendingRoute[0]) return;
    if (!s.exitToReader) {
        if (!std::strcmp(s.pendingRoute, "library")) {
            screen_reader_request_tab(0);
            s.exitToReader = true;
        } else if (!std::strcmp(s.pendingRoute, "file_transfer")) {
            screen_reader_request_netshare();
            s.exitToReader = true;
        } else if (!std::strcmp(s.pendingRoute, "opds")) {
            screen_reader_request_opds();
            s.exitToReader = true;
        } else if (!std::strcmp(s.pendingRoute, "reading_book")) {
            screen_reader_request_open_book(s.pendingBook);
            s.exitToReader = true;
        }
    }
    s.pendingRoute[0] = 0;
}

void finishPendingExport() {
    if (!s.pendingExport[0]) return;
    char name[64];
    std::snprintf(name, sizeof(name), "%s", s.pendingExport);
    s.pendingExport[0] = 0;
    std::string path;
    const bool ok = exportCanvas(name, path);
    dispatch("on_export", ok ? 1 : 0, ok ? path.c_str() : "");
}

void pump() {
    if (!s_runtime || s.failed) return;
    // AirPage 原生服务的收结果/重建历史/起轮询都放在这里：它在**Lua 回调之外**，
    // 不受 CrossStore 单回调 1.5s 宿主 I/O 预算的约束。必须在 on_tick 之前，
    // 这样 Lua 这一拍拿到的版本号和条目数才是同一份（换图 -> 重建 -> 重画）。
    airpage_service::pump();
    const int64_t now = esp_timer_get_time();
    const Bounds b = canvasBounds();
    if (b.w != s.lastW || b.h != s.lastH) {
        s.lastW = b.w;
        s.lastH = b.h;
        dispatch("on_resize");
    }
    if (now - s.lastTickUs >= 500000) {
        s.lastTickUs = now;
        dispatch("on_tick");
    }
    if (s.toastUntilUs && now >= s.toastUntilUs) {
        s.toastUntilUs = 0;
        s.dirty = true;
    }
    if (!s.failed && s.pendingExport[0]) finishPendingExport();
    if (!s.failed) applyPendingRoute();
}

void destroyCanvas() {
    if (!s.canvas) return;
    s.canvas->~Canvas();
    heap_caps_free(s.canvas);
    s.canvas = nullptr;
}

void unload() {
    if (s_runtime && !s.failed) s_runtime->event("on_stop");
    s_runtime.reset();
    s_host.reset();
    destroyCanvas();
    // 原生服务的收尾要在 Lua 停掉之后：它可能还有下载任务在跑，也有 WiFi 是我们开的。
    airpage_service::reset();
    g_lua_keep_awake = false;
    s.active = false;
}

AppState handleKey(int key) {
    if (s.failed) return (key == 0x1B || key == KEY_BACK) ? APP_APPSTORE : APP_LUA_APP;
    if (key == 0x1B || key == KEY_BACK) {
        // 参考实现的语义：on_back 返回/存在就由应用自己导航；**应用没有 on_back
        // （或没认）才退出**。dispatch 的返回值就是 handled。
        const bool handled = dispatch("on_back");
        if (s.failed) return APP_LUA_APP;
        if (!handled || s.exitRequested) return APP_APPSTORE;
        return APP_LUA_APP;
    }
    if (key == KEY_TOUCH_LONG) {
        int x = 0, y = 0;
        // 整屏图片上没有按钮：点哪儿都算"看完了，回去"。Lua 的 on_image 就是 home()。
        if (airpage_service::overlayActive()) {
            if (input_tap_xy(&x, &y)) dispatch("on_image");
        } else if (input_tap_xy(&x, &y)) {
            hitCanvas(x, y, true);
        }
        return APP_LUA_APP;
    }
    if (key == '\n' || key == '\r') {
        int x = 0, y = 0;
        if (airpage_service::overlayActive()) {
            // 触摸/确认键都退出图片视图；图片铺满全屏，没有可点的按钮。
            (void)input_tap_xy(&x, &y);
            dispatch("on_image");
            return APP_LUA_APP;
        }
        if (input_tap_xy(&x, &y)) {
            if (s.canvasMode)
                hitCanvas(x, y, false);
            else
                hitList(x, y);
        } else {
            activateSelected();
        }
        return APP_LUA_APP;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        dispatch("on_key", 0, key == KEY_UP ? "up" : "down");
        if (s.failed) return APP_LUA_APP;
        if (s.exitRequested) return APP_APPSTORE;
        if (s_runtime->handled()) return APP_LUA_APP;
        navigate(key);
        return APP_LUA_APP;
    }
    if (key == KEY_LEFT || key == KEY_RIGHT) {
        if (s.canvasMode) {
            dispatch("on_swipe", 0, key == KEY_LEFT ? "left" : "right");
            if (s.failed) return APP_LUA_APP;
            if (s_runtime->handled()) return APP_LUA_APP;
        }
        navigate(key);
        return APP_LUA_APP;
    }
    if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        // 竖滑（主循环把触摸竖滑翻成 PAGE 键）。应用先拿 on_swipe，再退回列表翻页。
        dispatch("on_swipe", 0, key == KEY_PAGE_DOWN ? "up" : "down");
        if (s.failed) return APP_LUA_APP;
        if (s_runtime->handled()) return APP_LUA_APP;
        if (!s.canvasMode) navigateList(key);
        return APP_LUA_APP;
    }
    return APP_LUA_APP;
}

}  // namespace

// ── 对外接口 ─────────────────────────────────────────────────────────────
void screen_luaapp_request(const char *id) {
    s_pendingId[0] = 0;
    if (id && validId(id)) std::snprintf(s_pendingId, sizeof(s_pendingId), "%s", id);
}

void screen_luaapp_enter(ScreenContext &) {
    s = LuaState{};
    // 上一次会话的覆盖层/下载任务可能还没收尾（应用崩溃时 unload 不一定走到）。
    airpage_service::reset();
    g_lua_keep_awake = false;
    if (!s_pendingId[0]) {
        showError("没有要打开的应用");
        render();
        return;
    }
    std::snprintf(s.id, sizeof(s.id), "%s", s_pendingId);
    s_pendingId[0] = 0;
    const std::string path = installedPath(s.id);
    std::snprintf(s.path, sizeof(s.path), "%s", path.c_str());
    if (!readPackage(path, s.pkg)) {
        showError("应用包损坏，无法读取清单");
        render();
        return;
    }
    char missing[96]{};
    if (!compatible(s.pkg, missing, sizeof(missing))) {
        showError(missing);
        render();
        return;
    }
    std::string source;
    if (!readPackageEntry(path, "main.lua", MAX_SCRIPT_BYTES, source)) {
        showError("应用包缺少 main.lua");
        render();
        return;
    }
    // 授权位是安装时按清单写的。早期装的包或落盘失败时这个文件可能不在，
    // 而商店里没有改权限的界面 —— 不补写的话应用会永远停在
    // "请在商店设置中允许…" 那一页（AirPage 的 status 就靠它返回 ok）。
    const std::string permissionPath = std::string(PERMISSION_DIR) + "/" + s.id;
    if (s.pkg.permissions && !Storage.exists(permissionPath.c_str())) {
        ESP_LOGW(kTAG, "应用 %s 缺授权文件，按清单补写 0x%02x", s.id, static_cast<unsigned>(s.pkg.permissions));
        setGrantedPermissions(s.id, s.pkg.permissions);
    }
    size_t budget = s.pkg.memoryBytes;
    if (budget < 16384) budget = 65536;
    if (budget > MAX_VM_BYTES) budget = MAX_VM_BYTES;
    s_host = std::make_unique<LuaHost>();
    s_runtime = std::make_unique<Runtime>(*s_host);
    if (!s_runtime->load(source.data(), source.size(), budget)) {
        showError(s_runtime->error());
        s_runtime.reset();
        s_host.reset();
        render();
        return;
    }
    s.active = true;
    s.lastTickUs = esp_timer_get_time();
    s.lastW = -1;
    s.lastH = -1;
    finishPendingExport();
    applyPendingRoute();
    // 应用在 on_start 里就要跳去阅读器（reading.open 之类）：这一帧别画 —— 主循环
    // 下一次 handle 立刻返回 APP_READER，画了只会先闪一下应用界面。
    if (!s.exitToReader) render();
}

AppState screen_luaapp_handle(int key, ScreenContext &) {
    if (s.exitToReader) return APP_READER;
    if (s.exitRequested) return APP_APPSTORE;
    if (key > 0) {
        const AppState next = handleKey(key);
        if (next != APP_LUA_APP) return next;
        // 触摸/长按/激活那几条路里，应用可能已经 exit() 或投递了阅读器跳转，
        // 但它们不在 handleKey 的每个出口里各查一遍 —— 这里统一收口。
        if (s.exitRequested) return APP_APPSTORE;
        if (s.exitToReader) return APP_READER;
    } else if (!s.failed) {
        pump();
        if (s.exitToReader) return APP_READER;
        if (s.exitRequested) return APP_APPSTORE;
        if (s.failed) s.dirty = true;
    }
    if (s.dirty) render();
    return APP_LUA_APP;
}

void screen_luaapp_leave(AppState) {
    unload();
}
