#pragma once
// CrossStore 包/目录模型（本工程版）。
//
// 协议与校验规则照搬上游 zhuzhe1983/crossmux 的 src/crossstore（Package /
// PackageManifest / CatalogPage / Permissions / RecoverableFile / IconBundle），
// 但底层换了：JSON 用本仓的 JsonValue，HTTP 用 net::request/stream，SHA 用
// mbedTLS 的 PSA 接口，文件用 crossmux 的 HalStorage（ZipFile 也要用它）。
//
// 目录抓取在这里只做**一页一页的同步拉取 + 按 URL 缓存**；上游那套 64 页后台
// 快照 worker 没有搬——本机只有 22 个包、两页，代价不成比例。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace crossstore {
constexpr char INSTALL_DIR[] = "/.crosspoint/apps";
constexpr char DOWNLOAD_PATH[] = "/.crosspoint/apps/download.part";
constexpr char APP_DATA_DIR[] = "/.crosspoint/appdata";
constexpr char PERMISSION_DIR[] = "/.crosspoint/app-permissions";
constexpr char CACHE_DIR[] = "/.crosspoint/store-cache";
constexpr char SOURCE_URL_FILE[] = "/.crosspoint/crossstore-url";
constexpr char DEFAULT_INDEX[] = "https://crossstore.github.io/CrossStore/index.json";
constexpr size_t MAX_INSTALLED_PACKAGES = 64;
constexpr size_t MAX_PACKAGE_BYTES = 1024 * 1024;
constexpr size_t MAX_MANIFEST_BYTES = 4096;
constexpr size_t MAX_CATALOG_BYTES = 24576;
constexpr size_t CATALOG_PAGE_SIZE = 16;

// ── 能力 / 权限位（与协议一字不差，位号固定）────────────────────────────
struct Capability {
  const char* name;
  uint32_t bit;
};
constexpr Capability CAPABILITIES[] = {
    {"buzzer", 1u << 10}, {"audio", 1u << 11},  {"wifi", 1u << 0},   {"lora", 1u << 1},
    {"esp_now", 1u << 2}, {"tox", 1u << 3},     {"imu", 1u << 4},    {"touch", 1u << 5},
    {"frontlight", 1u << 6}, {"rtc", 1u << 7},  {"psram", 1u << 8},  {"storage", 1u << 9},
    {"radio_audio", 1u << 12}};

struct Permission {
  const char* name;
  uint32_t bit;
};
constexpr Permission PERMISSIONS[] = {
    {"keep_awake", 1u << 9},   {"files_export", 1u << 8}, {"device_info", 1u << 7},
    {"sound", 1u << 6},        {"network", 1u << 0},      {"radio", 1u << 1},
    {"notifications", 1u << 2}, {"background", 1u << 3},  {"reading_data", 1u << 4},
    {"wallpaper", 1u << 5},
};

uint32_t capabilityBit(const char* name);
uint32_t permissionBit(const char* name);
// 本机实际有的能力。没有的包可在商店里看到、但装不进去（compatible 会点名）。
uint32_t deviceCapabilities();

struct Package {
  char id[32]{}, version[24]{}, name[96]{}, description[192]{};
  uint32_t requiredCapabilities = 0;
  uint32_t permissions = 0;
  uint32_t memoryBytes = 0;
  uint32_t api = 1;
};

struct CatalogEntry {
  Package package;
  std::string artifact;      // 已按目录页 URL 解析成绝对 URL
  std::string sha256;
  uint32_t bytes = 0;
  std::string iconArtifact;  // 可选；空 = 没有图标
  std::string iconSha256;
  uint32_t iconBytes = 0;
};

struct CatalogPage {
  std::string url;       // 这一页的绝对 URL
  std::string next;      // 相对路径；空 = 最后一页
  std::string previous;  // 相对路径；空 = 第一页
  unsigned page = 0, pages = 0, total = 0;
  std::vector<CatalogEntry> entries;
};

// ── 清单 / 包 ─────────────────────────────────────────────────────────
bool validId(const char* id);
bool installedFilename(const char* filename);
bool validVersion(const char* text);
int compareVersions(const char* a, const char* b);
// forInstall=false 用于目录条目（目录里的 api 可以比本机新，装的时候再拦）。
bool parseManifest(const std::string& json, Package& out, bool forInstall);
bool readPackage(const std::string& path, Package& out);
// 从 .csp 里读一个条目到内存（最多 maximum 字节）。成功时 out 被填满。
bool readPackageEntry(const std::string& path, const char* entry, size_t maximum, std::string& out);
// 读一个条目的一段到调用方缓冲（asset 用），最多 capacity 字节。
bool readPackageEntryInto(const std::string& path, const char* entry, size_t offset, uint8_t* out, size_t capacity,
                          size_t& bytes);
// 兼容性：失败时 missing 填人话（缺的能力以英文名列出）。
bool compatible(const Package& package, char* missing, size_t capacity);

std::string installedPath(const char* id);

// ── 目录抓取与缓存 ───────────────────────────────────────────────────
bool validSourceUrl(const char* url);
bool catalogRelativeUrl(const std::string& base, const char* relative, std::string& out);
bool parseCatalogPage(const std::string& json, const std::string& pageUrl, CatalogPage& out);
// 只抓正文（网络）并把原文写进缓存。后台任务用：解析留在 UI 线程做。
bool fetchCatalogBody(const std::string& pageUrl, std::string& body, std::string& error);
// 抓取（网络）并把原文写进缓存；失败时 error 是人话。
bool fetchCatalogPage(const std::string& pageUrl, CatalogPage& out, std::string& error);
// 只读缓存（离线用）。没有缓存返回 false。
bool loadCachedCatalogPage(const std::string& pageUrl, CatalogPage& out);
// 目录源：优先 /.crosspoint/crossstore-url，缺省 DEFAULT_INDEX。
std::string catalogSource();

// ── 安装 / 卸载 ──────────────────────────────────────────────────────
void recoverInstalls();
bool hasInstallRoom(const char* id);
// 流式下载到 path（同时算 SHA-256），超过 limit 中止；进度回调可为空。
// 成功时 bytes 是实际字节数。
bool downloadFile(const std::string& url, const char* path, size_t limit, size_t& bytes,
                  void (*progress)(void* ctx, size_t got, size_t total, int percent), void* ctx, std::string& error);
bool verifyHash(const char* path, size_t expectedBytes, const char* expectedHash);
// 把下载好的 .part 转正：re-read manifest、校验 id/version/api/caps/memory、读 main.lua
// 判字节码/NUL，最后原子提交为 <id>.csp 并授予清单里声明的权限。
bool installDownloaded(const char* path, const CatalogEntry& entry, std::string& error);
bool uninstallPackage(const char* id);

// 已安装：扫目录，能解 manifest 的收进来；invalid 也留在列表里以便删除。
struct InstalledApp {
  Package package;
  bool valid = false;
};
void scanInstalled(std::vector<InstalledApp>& out);
bool installedVersion(const char* id, char* out, size_t capacity);

// ── 权限 / 应用数据 ───────────────────────────────────────────────────
uint32_t grantedPermissions(const char* id);
bool setGrantedPermissions(const char* id, uint32_t granted);
bool permissionGranted(const char* id, const char* permission);
bool appdataRead(const char* id, const char* key, char* out, size_t capacity);
bool appdataWrite(const char* id, const char* key, const char* value, size_t size);

// ── 图标 ─────────────────────────────────────────────────────────────
// .csi 图标包（7048 字节）里的一个变体；levels ∈ {2,4,16}，size ∈ {32,64,96}。
bool validIconBundle(const uint8_t* header, size_t headerBytes, size_t bundleBytes);
bool readBundleIcon(const uint8_t* bundle, size_t bundleBytes, int levels, int size, uint8_t* out, size_t capacity,
                    size_t& bytes);
bool readBundleIconFile(const char* path, int levels, int size, uint8_t* out, size_t capacity, size_t& bytes);
std::string iconCachePath(const std::string& sha256);
// 目录条目的图标：先查缓存；没有就下载（network=false 时只查缓存）。
bool ensureCatalogIcon(const CatalogEntry& entry, bool allowNetwork, std::string& error);
// 已安装包的图标（包内 assets/icon-gX-Y.bin）。
bool readInstalledIcon(const char* id, int levels, int size, uint8_t* out, size_t capacity, size_t& bytes);
}  // namespace crossstore
