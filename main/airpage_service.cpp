#include "airpage_service.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "JpegToBmpConverter.h"
#include "app_async.h"
#include "app_services.h"
#include "crossstore/store_package.h"
#include "pcf85063.h"
#include "screen_reader.h"
#include "settings_manager.h"
#include "ui_helpers.h"
#include "wifi_manager.h"

extern bool g_lua_keep_awake;

namespace airpage_service {
namespace {

constexpr char TAG[] = "AIRP";
constexpr char kDir[] = "/AirPage";
constexpr char kLatest[] = "/AirPage/latest.bmp";
constexpr char kPart[] = "/AirPage/latest.part";
constexpr char kConverted[] = "/AirPage/latest.conv";
constexpr char kWallpaper[] = "/AirPage/wallpaper.bmp";
constexpr char kIdPath[] = "/.crosspoint/airpage_device_id";
constexpr char kModeDir[] = "/.crosspoint/airpage";
constexpr char kModePath[] = "/.crosspoint/airpage/mode";
constexpr size_t kPageSize = 20;
constexpr size_t kMaxDownload = 8u * 1024u * 1024u;
constexpr int64_t kPollIntervalUs = 20 * 1000 * 1000;

// ── 状态（只在 UI 任务上读写；工作任务只通过 AppAsyncJob::finish 交结果）──
struct Entry {
  std::string path;
  std::string name;
  int w = 0;
  int h = 0;
  bool current = false;
};

AppAsyncJob s_job;
std::atomic<bool> s_abandon{false};
std::atomic<bool> s_wifiOwned{false};

bool s_started = false;
bool s_keepAwakeAllowed = false;   // 由 system() 每次从权限表刷新（live 模式要用）
bool s_overlay = false;
bool s_live = true;
std::string s_overlayPath;
std::string s_state = "empty";
std::string s_notice;          // "wallpaper"：status 里显示到 s_noticeUntilUs 为止
int64_t s_noticeUntilUs = 0;
int s_version = 0;
std::vector<Entry> s_page;
size_t s_pageStart = 0;
size_t s_total = 0;
bool s_polled = false;
int64_t s_lastFetchUs = 0;
bool s_listDirty = false;    // 历史页待重建（由 pump 在 Lua 回调之外做）
bool s_pendingFetch = false; // 拉图待启动（同上：别在 Lua 回调里起网络任务）

// ── 小工具 ──────────────────────────────────────────────────────────────
bool validId(const std::string &s) {
  if (s.size() != 16) return false;
  for (char c : s) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
  }
  return true;
}

std::string generateId() {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  std::string id;
  id.reserve(16);
  while (id.size() < 16) {
    const uint8_t v = esp_random() & 0x3F;
    if (v < 62) id.push_back(kAlphabet[v]);
  }
  return id;
}

const std::string &deviceId() {
  static std::string cached;
  if (!cached.empty()) return cached;
  bool ok = false;
  const std::string stored = Storage.readFile(kIdPath, &ok, 64);
  if (ok && validId(stored)) {
    cached = stored;
    return cached;
  }
  cached = generateId();
  Storage.ensureDirectoryExists("/.crosspoint");
  if (!Storage.writeFile(kIdPath, cached)) ESP_LOGW(TAG, "设备 ID 落盘失败，重启后会换一个");
  ESP_LOGI(TAG, "设备 ID: %s", cached.c_str());
  return cached;
}

std::string uploadUrl() {
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "https://airpage.crossmux.cn/?id=%s&w=%d&h=%d&mode=gray16",
                deviceId().c_str(), SCREEN_W, SCREEN_H);
  return buf;
}

bool networkPossible() { return !g_settings.wifiSsid().empty(); }

bool loadRealtimeMode() {
  bool ok = false;
  const std::string v = Storage.readFile(kModePath, &ok, 8);
  return v.empty() || v[0] != '0';   // 缺省开，与上游一致
}

void saveRealtimeMode(bool enabled) {
  Storage.ensureDirectoryExists(kModeDir);
  if (!Storage.writeFile(kModePath, enabled ? "1" : "0")) ESP_LOGW(TAG, "实时接收模式落盘失败");
}

bool inspectBmp(const std::string &path, int *width, int *height) {
  if (path.empty() || !Storage.exists(path.c_str())) return false;
  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) return false;
  Bitmap bmp(f, false);
  if (bmp.parseHeaders() != BmpReaderError::Ok) return false;
  const int w = bmp.getWidth(), h = bmp.getHeight();
  if (w <= 0 || h <= 0) return false;
  if (width) *width = w;
  if (height) *height = h;
  return true;
}

bool filesEqual(const char *a, const char *b) {
  HalFile fa, fb;
  if (!Storage.openFileForRead(TAG, a, fa) || !Storage.openFileForRead(TAG, b, fb)) return false;
  if (fa.fileSize64() != fb.fileSize64()) return false;
  uint8_t ba[128], bb[128];
  while (fa.available() > 0) {
    const size_t want = std::min<size_t>(sizeof(ba), static_cast<size_t>(fa.available()));
    const int ra = fa.read(ba, want);
    const int rb = fb.read(bb, want);
    if (ra != static_cast<int>(want) || rb != ra || std::memcmp(ba, bb, static_cast<size_t>(ra)) != 0) return false;
  }
  return fb.available() == 0;
}

bool copyFile(const char *src, const char *dst) {
  Storage.remove(dst);
  HalFile in, out;
  if (!Storage.openFileForRead(TAG, src, in) || !Storage.openFileForWrite(TAG, dst, out)) return false;
  const uint64_t expected = in.fileSize64();
  uint64_t copied = 0;
  uint8_t buf[256];
  while (in.available() > 0) {
    const size_t want = std::min<size_t>(sizeof(buf), static_cast<size_t>(in.available()));
    const int got = in.read(buf, want);
    if (got <= 0 || out.write(buf, static_cast<size_t>(got)) != static_cast<size_t>(got)) return false;
    copied += static_cast<uint64_t>(got);
  }
  out.flush();
  return copied == expected;
}

uint64_t localDateKey() {
  if (!g_rtc.hasValidTime()) return 0;
  const time_t t = g_rtc.getTime();
  if (t < 1600000000) return 0;
  struct tm tm {};
  localtime_r(&t, &tm);
  // 80：%02d 的实参是 int，编译器按最坏情况（11 字符/字段 × 6）算上界 66，
  // 24 会被 -Werror=format-truncation 拦下。字段本身已被上面的 t 范围收住。
  char buf[80];
  std::snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
  return std::strtoull(buf, nullptr, 10);
}

// 历史文件名：有 RTC 用 YYYYMMDD_HHMMSS（好认、按名字排序就是时间序），没有就用
// 递增序号。冲突加 -01…-99 后缀。
std::string nextArchivePath() {
  const uint64_t key = localDateKey();
  char base[64];
  if (key) {
    std::snprintf(base, sizeof(base), "%04u%02u%02u_%02u%02u%02u", static_cast<unsigned>(key / 10000000000ull % 10000),
                  static_cast<unsigned>(key / 100000000ull % 100), static_cast<unsigned>(key / 1000000ull % 100),
                  static_cast<unsigned>(key / 10000ull % 100), static_cast<unsigned>(key / 100ull % 100),
                  static_cast<unsigned>(key % 100));
    for (int i = 0; i < 100; ++i) {
      char path[128];
      if (i == 0)
        std::snprintf(path, sizeof(path), "%s/%s.bmp", kDir, base);
      else
        std::snprintf(path, sizeof(path), "%s/%s-%02d.bmp", kDir, base, i);
      if (!Storage.exists(path)) return path;
    }
  }
  for (uint32_t seq = static_cast<uint32_t>(s_total) + 1; seq < 100000000u; ++seq) {
    char path[64];
    std::snprintf(path, sizeof(path), "%s/%08u.bmp", kDir, static_cast<unsigned>(seq));
    if (!Storage.exists(path)) return path;
  }
  return std::string(kDir) + "/archive.bmp";
}

// 历史里只收我们自己的 BMP：latest/wallpaper/临时文件都不算。
bool isHistoryName(const char *name) {
  if (!name || name[0] == '.') return false;
  const size_t n = std::strlen(name);
  if (n < 5) return false;
  const char *ext = name + n - 4;
  if (ext[0] != '.' || (ext[1] != 'b' && ext[1] != 'B') || (ext[2] != 'm' && ext[2] != 'M') ||
      (ext[3] != 'p' && ext[3] != 'P'))
    return false;
  return std::strcmp(name, "latest.bmp") != 0 && std::strcmp(name, "wallpaper.bmp") != 0;
}

std::string stripExt(const std::string &name) {
  const size_t dot = name.find_last_of('.');
  return dot == std::string::npos ? name : name.substr(0, dot);
}

void rebuildPage(size_t start) {
  const int64_t t0 = esp_timer_get_time();
  bool hasCurrent = inspectBmp(kLatest, nullptr, nullptr);
  std::vector<std::string> names;
  auto dir = Storage.open(kDir);
  if (dir && dir.isDirectory()) {
    dir.rewindDirectory();
    for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (f.isDirectory()) continue;
      char name[96];
      if (f.getName(name, sizeof(name)) == 0 || !isHistoryName(name)) continue;
      names.emplace_back(name);
    }
  }
  std::sort(names.begin(), names.end(), std::greater<std::string>());   // 时间戳/序号都是越大越新

  s_total = names.size() + (hasCurrent ? 1 : 0);
  if (s_total == 0) {
    start = 0;
  } else if (start >= s_total) {
    start = ((s_total - 1) / kPageSize) * kPageSize;
  }
  s_pageStart = start;
  s_page.clear();
  const size_t end = std::min(s_total, start + kPageSize);
  for (size_t i = start; i < end; ++i) {
    Entry e;
    if (hasCurrent && i == 0) {
      e.path = kLatest;
      e.name = "当前图片";
      e.current = true;
    } else {
      const std::string &file = names[hasCurrent ? i - 1 : i];
      e.path = std::string(kDir) + "/" + file;
      e.name = stripExt(file);
    }
    if (!inspectBmp(e.path, &e.w, &e.h)) continue;
    s_page.push_back(std::move(e));
  }
  ESP_LOGI(TAG, "rebuildPage: %u 条（当前=%d），用时 %lld ms", static_cast<unsigned>(s_page.size()),
           hasCurrent ? 1 : 0, (esp_timer_get_time() - t0) / 1000);
}

int pageSize() { return static_cast<int>(s_page.size()); }

// ── 下载 / 落盘 ─────────────────────────────────────────────────────────
bool normalizePart() {
  HalFile f;
  if (!Storage.openFileForRead(TAG, kPart, f)) return false;
  uint8_t sig[2]{};
  if (f.read(sig, sizeof(sig)) != static_cast<int>(sizeof(sig))) return false;

  if (sig[0] == 'B' && sig[1] == 'M') return inspectBmp(kPart, nullptr, nullptr);
  if (sig[0] != 0xFF || sig[1] != 0xD8) return false;

  // JPEG：截到当前屏大小（fit，不裁）转成 Gray8 BMP；显示和壁纸都只认 BMP。
  HalFile in, out;
  Storage.remove(kConverted);
  if (!Storage.openFileForRead(TAG, kPart, in) || !Storage.openFileForWrite(TAG, kConverted, out)) return false;
  const bool ok = JpegToBmpConverter::jpegFileToBmpStreamWithSize(
      in, out, SCREEN_W, SCREEN_H, JpegToBmpConverter::Output::Gray8, /*crop=*/false);
  out.flush();
  if (!ok) {
    Storage.remove(kConverted);
    return false;
  }
  Storage.remove(kPart);
  if (!Storage.rename(kConverted, kPart)) return false;
  return inspectBmp(kPart, nullptr, nullptr);
}

enum class Stage { Failed, Unchanged, Changed };

Stage installPart() {
  if (!normalizePart()) {
    Storage.remove(kPart);
    return Stage::Failed;
  }
  if (Storage.exists(kLatest) && filesEqual(kPart, kLatest)) {
    Storage.remove(kPart);
    return Stage::Unchanged;
  }

  const std::string archive = nextArchivePath();
  const bool hadCurrent = Storage.exists(kLatest);
  if (hadCurrent && !Storage.rename(kLatest, archive.c_str())) {
    ESP_LOGW(TAG, "归档当前图片失败: %s", archive.c_str());
    Storage.remove(kPart);
    return Stage::Failed;
  }
  if (!Storage.rename(kPart, kLatest)) {
    ESP_LOGW(TAG, "安装新图片失败");
    // 旧图还在 archive 上，尽量搬回去，别让用户连上一张都丢了。
    if (hadCurrent) Storage.rename(archive.c_str(), kLatest);
    Storage.remove(kPart);
    return Stage::Failed;
  }
  ESP_LOGI(TAG, "新图片已安装，旧图归档为 %s", archive.c_str());
  return Stage::Changed;
}

// 启动时把上次没装完的 part 收尾；能装就装，不能装就删。
void recoverPart() {
  if (!Storage.exists(kPart)) return;
  if (installPart() == Stage::Changed) s_version++;
}

void finishCancelled() {
  Storage.remove(kPart);
  if (s_wifiOwned.exchange(false)) g_wifi.disconnect();
}

void fetchBody() {
  bool wasConnected = false;
  if (!g_wifi.isConnected()) {
    if (!app_connect_wifi_from_settings(&wasConnected)) {
      // 连不上就是"未联网"，不是"操作失败"：Lua 对这两种态给的引导不一样
      // （offline = 点刷新重连，failed = 重试）。
      s_job.finish("offline");
      return;
    }
    if (!wasConnected) s_wifiOwned.store(true, std::memory_order_relaxed);
  }
  if (s_abandon.load(std::memory_order_acquire)) {
    finishCancelled();
    s_job.finish("cancelled");
    return;
  }

  Storage.mkdir(kDir, true);
  Storage.remove(kPart);

  const std::string &id = deviceId();
  const char *hosts[] = {"crossmux.cn", "crossmux.com"};
  const char *suffixes[] = {"latest", "latest.bmp"};
  bool ok = false;
  std::string error;
  for (const char *host : hosts) {
    for (const char *suffix : suffixes) {
      char url[160];
      std::snprintf(url, sizeof(url), "https://airpage.%s/api/device/%s/%s", host, id.c_str(), suffix);
      size_t bytes = 0;
      if (crossstore::downloadFile(url, kPart, kMaxDownload, bytes, nullptr, nullptr, error)) {
        ESP_LOGI(TAG, "下载成功: %s (%u 字节)", url, static_cast<unsigned>(bytes));
        ok = true;
        break;
      }
      ESP_LOGW(TAG, "下载失败 %s: %s", url, error.c_str());
      if (s_abandon.load(std::memory_order_acquire)) {
        finishCancelled();
        s_job.finish("cancelled");
        return;
      }
    }
    if (ok) break;
  }
  if (!ok) {
    Storage.remove(kPart);
    s_job.finish(g_wifi.isConnected() ? "failed" : "offline");
    return;
  }

  if (s_abandon.load(std::memory_order_acquire)) {
    finishCancelled();
    s_job.finish("cancelled");
    return;
  }

  switch (installPart()) {
    case Stage::Changed:
      s_job.finish("changed");
      break;
    case Stage::Unchanged:
      s_job.finish("unchanged");
      break;
    case Stage::Failed:
      s_job.finish("failed");
      break;
  }
}

void fetchTask(void *) {
  fetchBody();
  // WithCaps 任务不能自删：挂着等主任务 reapCapsTask（见 app_async.h）。
  vTaskSuspend(nullptr);
}

void consumeJob() {
  if (s_job.state() != AppAsyncState::Done) return;
  const std::string result = s_job.result();
  s_job.reapCapsTask();
  s_job.reset();
  s_abandon.store(false, std::memory_order_relaxed);
  if (!s_started) return;
  if (result == "changed") {
    s_version++;
    s_state = "ready";
    // 列表重建放到 pump 里：这里可能正处在 status 回调内，连开二十多个 BMP 头
    // 会吃掉 CrossStore 给的 1.5s 宿主 I/O 预算。
    s_listDirty = true;
  } else if (result == "unchanged") {
    s_state = "ready";
  } else if (result == "offline") {
    s_state = "offline";
  } else if (result == "failed") {
    s_state = "failed";
  }
  // "cancelled"：离开应用时放弃的那一单，什么都不改。
}

bool startFetch() {
  if (s_job.state() == AppAsyncState::Running) return true;
  if (!networkPossible()) {
    s_state = "offline";
    return false;
  }
  s_abandon.store(false, std::memory_order_relaxed);
  s_job.reapCapsTask();
  s_job.begin("AirPage", "");
  // 栈走 PSRAM：任务自己要连 WiFi，而 WiFi 一起来内部 RAM 的最大连续块只剩 1~2KB。
  if (!s_job.start(fetchTask, "airpage_fetch", 16384, nullptr, 1, /*psramStack=*/true)) {
    s_state = "failed";
    return false;
  }
  s_state = "downloading";
  s_lastFetchUs = esp_timer_get_time();
  return true;
}

void maybePoll() {
  if (!s_live || s_overlay || !networkPossible()) return;
  if (s_job.state() == AppAsyncState::Running) return;
  const int64_t now = esp_timer_get_time();
  if (s_polled && now - s_lastFetchUs < kPollIntervalUs) return;
  s_polled = true;
  startFetch();
}

void start() {
  if (s_started) return;
  if (s_job.state() == AppAsyncState::Done) {
    s_job.reapCapsTask();
    s_job.reset();
  }
  Storage.ensureDirectoryExists(kDir);
  Storage.ensureDirectoryExists(kModeDir);
  // 设备 ID 在 UI 线程上先生成好：下拉任务也会读它，别让两个线程同时第一次
  // 填那个 static std::string。
  deviceId();
  s_live = loadRealtimeMode();
  recoverPart();
  s_overlay = false;
  s_overlayPath.clear();
  s_version = 0;
  s_notice.clear();
  s_pageStart = 0;
  s_page.clear();
  s_total = 0;
  s_listDirty = true;   // 真正的目录扫描交给 pump（Lua 回调之外）
  s_pendingFetch = false;
  if (inspectBmp(kLatest, nullptr, nullptr))
    s_state = "ready";
  else if (Storage.exists(kLatest))
    s_state = "invalid";
  else if (!networkPossible())
    s_state = "offline";
  else
    s_state = "empty";
  s_started = true;
  s_polled = false;
  s_lastFetchUs = 0;
  // 没给 keep_awake 权限时不能替应用把休眠顶住（live 仍然照常轮询）。
  if (s_live && s_keepAwakeAllowed) g_lua_keep_awake = true;
  ESP_LOGI(TAG, "启动: 状态=%s 实时=%d（历史页由 pump 重建）", s_state.c_str(), s_live ? 1 : 0);
}

const char *stateText() {
  static char last[24] = "";
  static char text[24];
  // "wallpaper" 这类一次性提示留 4 秒：状态文字变了 Lua 才会重画，
  // 只报一拍的话屏幕上根本来不及看见（e-ink 一次重画本身就要两百毫秒）。
  const int64_t now = esp_timer_get_time();
  if (!s_notice.empty() && now < s_noticeUntilUs) {
    std::snprintf(text, sizeof(text), "%s", s_notice.c_str());
  } else {
    if (!s_notice.empty()) s_notice.clear();
    if (s_job.state() == AppAsyncState::Running && !s_abandon.load(std::memory_order_relaxed))
      std::snprintf(text, sizeof(text), "downloading");
    else
      std::snprintf(text, sizeof(text), "%s", s_state.c_str());
  }
  // 状态文字本身就是一次"界面修订"：Lua 的 refresh 只在 version 变了才调 show()，
  // 不在这里抬版本的话，empty→下载中→失败 这些状态切换永远画不出来。
  if (std::strcmp(text, last) != 0) {
    std::snprintf(last, sizeof(last), "%s", text);
    ++s_version;
  }
  return text;
}

bool inPage(int64_t index) { return index >= 1 && index <= pageSize(); }

}  // namespace

// 每次服务调用的墙钟耗时：CrossStore 对单个回调的宿主 I/O 有 1.5s 上限，
// 超了就报 "application I/O budget exceeded" 把应用打崩。日志里留一笔好定位。
struct ActionTimer {
  const char *action;
  int64_t startedUs;
  ~ActionTimer() {
    const int64_t ms = (esp_timer_get_time() - startedUs) / 1000;
    if (ms >= 100) ESP_LOGW(TAG, "airpage.%s 用时 %lld ms", action, ms);
  }
};

bool system(const char *action, const char *argument, int64_t index, crossstore::ServiceReply &reply,
            const Grants &grants) {
  if (!action) return false;
  ActionTimer timer{action, esp_timer_get_time()};
  // start() 与 live 都要据此决定能不能顶住休眠；每一条调用都刷新一次。
  s_keepAwakeAllowed = grants.keep_awake;

  if (!std::strcmp(action, "start")) {
    start();
    return true;
  }

  if (!std::strcmp(action, "status")) {
    // Lua 用 status 的 ok 当"权限齐了"的判据；缺任一权限就让它显示那句提示。
    if (!grants.network || !grants.device_info || !s_started) return false;
    // 这里**只读内存**：收结果/重建历史/起轮询都在 pump() 里（见头文件那段说明）。
    // 单个回调的宿主 I/O 预算是 1.5s，status 每 500ms 调一次，任何 SD/网络活都不能放这。
    std::snprintf(reply.text, sizeof(reply.text), "%s", stateText());
    reply.a = pageSize();
    reply.b = s_version;
    reply.c = s_overlay ? 1 : 0;
    return true;
  }

  if (!std::strcmp(action, "url")) {
    if (!grants.network || !grants.device_info) return false;
    std::snprintf(reply.text, sizeof(reply.text), "%s", uploadUrl().c_str());
    return true;
  }

  if (!std::strcmp(action, "history")) {
    if (!s_started || !inPage(index)) return false;
    const Entry &e = s_page[static_cast<size_t>(index - 1)];
    std::snprintf(reply.text, sizeof(reply.text), "%s", e.name.c_str());
    reply.a = e.w;
    reply.b = e.h;
    reply.c = e.current ? 1 : 0;
    return true;
  }

  if (!std::strcmp(action, "show")) {
    if (!s_started) return false;
    std::string path;
    if (index == 0) {
      if (!inspectBmp(kLatest, nullptr, nullptr)) return false;
      path = kLatest;
    } else if (inPage(index)) {
      path = s_page[static_cast<size_t>(index - 1)].path;
    } else {
      return false;
    }
    s_overlayPath = std::move(path);
    s_overlay = true;
    return true;
  }

  if (!std::strcmp(action, "hide")) {
    s_overlay = false;
    s_overlayPath.clear();
    return true;
  }

  if (!std::strcmp(action, "page")) {
    if (!s_started || !argument) return false;
    if (!std::strcmp(argument, "first")) {
      rebuildPage(0);
      return true;
    }
    if (!std::strcmp(argument, "next")) {
      if (s_pageStart + kPageSize >= s_total) return false;
      rebuildPage(s_pageStart + kPageSize);
      return true;
    }
    if (!std::strcmp(argument, "previous")) {
      if (s_pageStart < kPageSize) return false;
      rebuildPage(s_pageStart - kPageSize);
      return true;
    }
    return false;
  }

  if (!std::strcmp(action, "fetch")) {
    if (!grants.network) return false;
    if (!s_started) start();
    // 只挂号：起任务会在第一次连 WiFi 时连带 driver 初始化，那一两秒不能算进
    // 本回调的 I/O 预算（见 pump）。
    s_pendingFetch = true;
    return true;
  }

  if (!std::strcmp(action, "live")) {
    if (!argument) return false;
    if (!std::strcmp(argument, "off")) {
      s_live = false;
      saveRealtimeMode(false);
      g_lua_keep_awake = false;
      return true;
    }
    if (!std::strcmp(argument, "on")) {
      if (!grants.network || !grants.keep_awake) return false;
      s_live = true;
      saveRealtimeMode(true);
      g_lua_keep_awake = true;
      s_polled = false;   // 下一拍立刻拉一次
      return true;
    }
    return false;
  }

  if (!std::strcmp(action, "wallpaper")) {
    if (!grants.wallpaper) return false;
    std::string source;
    if (s_overlay && !s_overlayPath.empty())
      source = s_overlayPath;
    else if (inspectBmp(kLatest, nullptr, nullptr))
      source = kLatest;
    if (source.empty()) return false;
    if (!copyFile(source.c_str(), kWallpaper)) return false;
    std::string err;
    if (!readerSetStandbyBmp(kWallpaper, err)) {
      ESP_LOGW(TAG, "壁纸设置失败: %s", err.c_str());
      return false;
    }
    s_notice = "wallpaper";
    s_noticeUntilUs = esp_timer_get_time() + 4 * 1000 * 1000;
    return true;
  }

  return false;
}

void pump() {
  if (!s_started) return;
  // 顺序要紧：先收结果（可能抬版本并挂上列表重建），再重建，最后才决定要不要拉图。
  consumeJob();
  if (s_listDirty) {
    s_listDirty = false;
    rebuildPage(s_pageStart);
  }
  if (s_pendingFetch) {
    s_pendingFetch = false;
    startFetch();
    return;
  }
  maybePoll();
}

void reset() {
  s_started = false;
  s_listDirty = false;
  s_pendingFetch = false;
  s_overlay = false;
  s_overlayPath.clear();
  s_page.clear();
  s_pageStart = 0;
  s_total = 0;
  s_notice.clear();
  g_lua_keep_awake = false;

  if (s_job.state() == AppAsyncState::Running) {
    // 任务可能正卡在 TLS/解压里，不能删；打放弃标记，让它自己收尾（含断 WiFi）。
    s_abandon.store(true, std::memory_order_release);
    s_job.cancel();
    return;
  }
  s_abandon.store(false, std::memory_order_relaxed);
  if (s_job.state() == AppAsyncState::Done) {
    s_job.reapCapsTask();
    s_job.reset();
  }
  if (s_wifiOwned.exchange(false)) g_wifi.disconnect();
}

bool overlayActive() { return s_overlay; }

const std::string &overlayPath() { return s_overlayPath; }

int version() { return s_version; }

}  // namespace airpage_service
