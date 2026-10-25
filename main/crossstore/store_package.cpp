#include "store_package.h"

#include <HalStorage.h>
#include <ZipFile.h>
#include <esp_log.h>
#include <psa/crypto.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "CrossStoreRuntime.h"
#include "json_parser.h"
#include "net/http.h"
#include "recoverable_file.h"

namespace crossstore {
namespace {
constexpr char TAG[] = "STORE";
constexpr char ICON_HEADER[8] = {'C', 'S', 'I', '1', 0, 0, 0, 0};
constexpr size_t ICON_BUNDLE_BYTES = 7048;

struct IconVariant {
  uint8_t levels, size, bits;
  uint16_t offset, bytes;
};
// 与 .csi 协议固定：行优先、MSB-first。像素 0=黑、最大值=白。
constexpr IconVariant ICON_VARIANTS[] = {
    {2, 32, 1, 8, 128},    {2, 64, 1, 136, 512},   {4, 32, 2, 648, 256},
    {4, 64, 2, 904, 1024}, {16, 32, 4, 1928, 512}, {16, 96, 4, 2440, 4608},
};
static_assert(ICON_VARIANTS[5].offset + ICON_VARIANTS[5].bytes == ICON_BUNDLE_BYTES);

const IconVariant* iconVariant(int levels, int size) {
  for (const auto& v : ICON_VARIANTS)
    if (v.levels == levels && v.size == size) return &v;
  return nullptr;
}

bool jsonUInt(const JsonValue& v, unsigned minimum, unsigned maximum, unsigned& out) {
  if (!v.isNumber()) return false;
  const double n = v.asNumber();
  if (!std::isfinite(n) || n < minimum || n > maximum || std::floor(n) != n) return false;
  out = static_cast<unsigned>(n);
  return true;
}

bool copyField(const JsonValue& v, char* out, size_t capacity) {
  if (!v.isString()) return false;
  const std::string& s = v.stringValue;
  if (s.empty() || s.size() >= capacity || s.find('\0') != std::string::npos) return false;
  std::memcpy(out, s.c_str(), s.size() + 1);
  return true;
}

bool hashText(const char* value) {
  if (!value || std::strlen(value) != 64) return false;
  for (const char* p = value; *p; ++p)
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
  return true;
}

bool readTextFile(const char* path, std::string& out, size_t maximum) {
  bool ok = false;
  out = Storage.readFile(path, &ok, maximum);
  return ok && !out.empty() && out.size() <= maximum;
}

bool writeTextFile(const char* path, const std::string& body) {
  if (!Storage.ensureDirectoryExists(CACHE_DIR)) return false;
  auto file = Storage.open(path, FS_WRONLY | FS_CREAT | FS_TRUNC);
  if (!file) return false;
  const bool ok = file.write(body.data(), body.size()) == body.size();
  file.flush();
  return ok;
}

uint64_t fnv1a(const std::string& text) {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string catalogCachePath(const std::string& url) {
  char name[48];
  std::snprintf(name, sizeof(name), "%s/%016llx.json", CACHE_DIR,
                static_cast<unsigned long long>(fnv1a(url)));
  return name;
}

bool validRelative(const char* path) {
  if (!path || !*path || *path == '/' || std::strstr(path, "..") || std::strpbrk(path, ":\\?#%")) return false;
  for (const char* p = path; *p; ++p)
    if (static_cast<unsigned char>(*p) <= 32) return false;
  return true;
}

bool parseManifestValue(const JsonValue& value, Package& out, bool forInstall) {
  if (!value.isObject()) return false;
  unsigned schema = 0, api = 0, memory = 0;
  if (!jsonUInt(value["schema"], 1, 1, schema) || !jsonUInt(value["api"], 1, 1000, api)) return false;
  if (forInstall && api > API_VERSION) return false;
  if (!value["runtime"].isString() || value["runtime"].stringValue != "crossmux-lua54") return false;
  if (!value["entry"].isString() || value["entry"].stringValue != "main.lua") return false;
  if (!value["status"].isString() || value["status"].stringValue != "ready") return false;

  Package parsed;
  parsed.api = api;
  if (!copyField(value["id"], parsed.id, sizeof(parsed.id)) || !validId(parsed.id)) return false;
  if (!copyField(value["version"], parsed.version, sizeof(parsed.version)) ||
      !validVersion(parsed.version))
    return false;
  const JsonValue& name = value["name"];
  const JsonValue& description = value["description"];
  const JsonValue& localizedName = name["zh"].isString() ? name["zh"] : name["en"];
  const JsonValue& localizedDesc = description["zh"].isString() ? description["zh"] : description["en"];
  if (!copyField(localizedName, parsed.name, sizeof(parsed.name)) ||
      !copyField(localizedDesc, parsed.description, sizeof(parsed.description)))
    return false;
  if (!jsonUInt(value["memory_bytes"], 16384, MAX_VM_BYTES, memory)) return false;
  parsed.memoryBytes = memory;

  const JsonValue& requirements = value["requires"];
  if (!requirements.isArray() || requirements.size() > sizeof(CAPABILITIES) / sizeof(CAPABILITIES[0])) return false;
  for (const auto& item : requirements.elements) {
    if (!item.isString()) return false;
    const uint32_t bit = capabilityBit(item.stringValue.c_str());
    if (!bit || (parsed.requiredCapabilities & bit)) return false;
    parsed.requiredCapabilities |= bit;
  }
  const JsonValue& permissions = value["permissions"];
  if (!permissions.isNull()) {
    if (!permissions.isArray() || permissions.size() > sizeof(PERMISSIONS) / sizeof(PERMISSIONS[0]))
      return false;
    for (const auto& item : permissions.elements) {
      if (!item.isString()) return false;
      const uint32_t bit = permissionBit(item.stringValue.c_str());
      if (!bit || (parsed.permissions & bit)) return false;
      parsed.permissions |= bit;
    }
  }
  if (parsed.memoryBytes > 65536 && !(parsed.requiredCapabilities & capabilityBit("psram"))) return false;
  if (api < 3 && (parsed.permissions & (permissionBit("files_export") | permissionBit("keep_awake"))))
    return false;
  out = parsed;
  return true;
}

}  // namespace

uint32_t capabilityBit(const char* name) {
  if (!name) return 0;
  for (const auto& c : CAPABILITIES)
    if (!std::strcmp(name, c.name)) return c.bit;
  return 0;
}

uint32_t permissionBit(const char* name) {
  if (!name) return 0;
  for (const auto& p : PERMISSIONS)
    if (!std::strcmp(name, p.name)) return p.bit;
  return 0;
}

uint32_t deviceCapabilities() {
  // 本机固定硬件：WiFi、触摸、PSRAM、SD、RTC、加速度计（晃动全刷）与蜂鸣器。
  // 蜂鸣器支持 PCM 播放，所以 audio 也算有；没有 LoRa / ESP-NOW / 前光 / 无线电台音频。
  return capabilityBit("wifi") | capabilityBit("touch") | capabilityBit("psram") |
         capabilityBit("storage") | capabilityBit("rtc") | capabilityBit("imu") |
         capabilityBit("buzzer") | capabilityBit("audio");
}

bool validId(const char* id) {
  if (!id || id[0] < 'a' || id[0] > 'z') return false;
  const size_t length = std::strlen(id);
  if (length > 31) return false;
  for (size_t i = 1; i < length; ++i)
    if ((id[i] < 'a' || id[i] > 'z') && (id[i] < '0' || id[i] > '9') && id[i] != '_') return false;
  return true;
}

bool installedFilename(const char* filename) {
  if (!filename) return false;
  const size_t length = std::strlen(filename);
  if (length < 5 || length > 35 || std::strcmp(filename + length - 4, ".csp")) return false;
  char id[32]{};
  std::memcpy(id, filename, length - 4);
  return validId(id);
}

bool validVersion(const char* text) {
  if (!text) return false;
  unsigned dots = 0, digits = 0;
  for (; *text; ++text) {
    if (*text == '.') {
      if (!digits || ++dots > 2) return false;
      digits = 0;
    } else if (*text < '0' || *text > '9' || ++digits > 5)
      return false;
  }
  return dots == 2 && digits;
}

int compareVersions(const char* a, const char* b) {
  if (!validVersion(a) || !validVersion(b)) return 0;
  for (int part = 0; part < 3; ++part) {
    unsigned long va = 0, vb = 0;
    while (*a && *a != '.') va = va * 10 + static_cast<unsigned>(*a++ - '0');
    while (*b && *b != '.') vb = vb * 10 + static_cast<unsigned>(*b++ - '0');
    if (va != vb) return va < vb ? -1 : 1;
    if (*a == '.') ++a;
    if (*b == '.') ++b;
  }
  return 0;
}

bool parseManifest(const std::string& json, Package& out, bool forInstall) {
  if (json.empty() || json.size() > MAX_MANIFEST_BYTES) return false;
  const JsonValue value = JsonValue::parse(json);
  return parseManifestValue(value, out, forInstall);
}

bool readPackageEntry(const std::string& path, const char* entry, size_t maximum, std::string& out) {
  out.clear();
  ZipFile zip(path);
  size_t size = 0;
  if (!zip.getInflatedFileSize(entry, &size) || !size || size > maximum) return false;
  uint8_t* data = zip.readFileToMemory(entry, &size, false);
  if (!data) return false;
  out.assign(reinterpret_cast<const char*>(data), size);
  std::free(data);
  return true;
}

bool readPackageEntryInto(const std::string& path, const char* entry, size_t offset, uint8_t* out, size_t capacity,
                          size_t& bytes) {
  bytes = 0;
  std::string data;
  if (!readPackageEntry(path, entry, 32768, data)) return false;
  if (offset > data.size()) return false;
  bytes = std::min(data.size() - offset, capacity);
  std::memcpy(out, data.data() + offset, bytes);
  return true;
}

bool readPackage(const std::string& path, Package& out) {
  std::string manifest;
  return readPackageEntry(path, "manifest.json", MAX_MANIFEST_BYTES, manifest) &&
         parseManifest(manifest, out, true);
}

bool compatible(const Package& package, char* missing, size_t capacity) {
  if (capacity) missing[0] = 0;
  if (package.api > API_VERSION) {
    if (capacity) std::snprintf(missing, capacity, "需要更新固件（应用 API %u）", static_cast<unsigned>(package.api));
    return false;
  }
  const uint32_t absent = package.requiredCapabilities & ~deviceCapabilities();
  for (const auto& capability : CAPABILITIES) {
    if (!(absent & capability.bit) || !capacity) continue;
    const size_t used = std::strlen(missing);
    std::snprintf(missing + used, capacity - used, "%s%s", used ? ", " : "缺少硬件能力：", capability.name);
  }
  return !absent;
}

std::string installedPath(const char* id) { return std::string(INSTALL_DIR) + "/" + id + ".csp"; }

bool validSourceUrl(const char* url) {
  if (!url) return false;
  const size_t length = std::strlen(url);
  if (length >= 256) return false;
  const size_t prefix = !std::strncmp(url, "https://", 8) ? 8 : !std::strncmp(url, "http://", 7) ? 7 : 0;
  if (!prefix || !url[prefix] || url[prefix] == '/' || !std::strchr(url + prefix, '/')) return false;
  for (const char* p = url; *p; ++p)
    if (static_cast<unsigned char>(*p) <= 32 || *p == '\\' || *p == '@' || *p == '#') return false;
  return true;
}

bool catalogRelativeUrl(const std::string& base, const char* relative, std::string& out) {
  if (!validRelative(relative) || base.size() >= 256) return false;
  const size_t slash = base.rfind('/');
  if (slash == std::string::npos || slash < 8) return false;
  out.assign(base, 0, slash + 1);
  out += relative;
  return out.size() < 256;
}

bool parseCatalogPage(const std::string& json, const std::string& pageUrl, CatalogPage& out) {
  out = {};
  if (json.empty() || json.size() > MAX_CATALOG_BYTES) return false;
  const JsonValue doc = JsonValue::parse(json);
  if (!doc.isObject()) return false;
  unsigned schema = 0, api = 0;
  if (!jsonUInt(doc["schema"], 1, 1, schema) || !jsonUInt(doc["api"], 1, API_VERSION, api)) return false;
  const JsonValue& packages = doc["packages"];
  if (!packages.isArray() || packages.size() > CATALOG_PAGE_SIZE) return false;

  out.url = pageUrl;
  const JsonValue& next = doc["next"];
  const JsonValue& previous = doc["previous"];
  if (!next.isString() || (!next.stringValue.empty() && !validRelative(next.stringValue.c_str()))) return false;
  out.next = next.stringValue;
  if (!previous.isNull()) {
    if (!previous.isString() || (!previous.stringValue.empty() && !validRelative(previous.stringValue.c_str())))
      return false;
    out.previous = previous.stringValue;
  }

  const bool hasPage = !doc["page"].isNull(), hasPages = !doc["pages"].isNull(), hasTotal = !doc["total"].isNull();
  if (hasPage || hasPages || hasTotal) {
    unsigned page = 0, pages = 0, total = 0;
    if (!jsonUInt(doc["page"], 1, 4096, page) || !jsonUInt(doc["pages"], 1, 4096, pages) ||
        !jsonUInt(doc["total"], 0, 65535, total))
      return false;
    if (page > pages || total > pages * CATALOG_PAGE_SIZE || total < packages.size()) return false;
    if ((page == 1) != out.previous.empty() || (page == pages) != out.next.empty()) return false;
    out.page = page;
    out.pages = pages;
    out.total = total;
  }

  for (const auto& item : packages.elements) {
    CatalogEntry entry;
    if (!parseManifestValue(item, entry.package, false)) continue;
    const JsonValue& artifact = item["artifact"];
    const JsonValue& sha = item["sha256"];
    unsigned bytes = 0;
    if (!artifact.isString() || !sha.isString() || !hashText(sha.stringValue.c_str()) ||
        !jsonUInt(item["bytes"], 1, MAX_PACKAGE_BYTES, bytes))
      continue;
    if (!catalogRelativeUrl(pageUrl, artifact.stringValue.c_str(), entry.artifact)) continue;
    entry.sha256 = sha.stringValue;
    entry.bytes = bytes;
    const JsonValue& icon = item["icon"];
    if (icon.isObject()) {
      const JsonValue& iconArtifact = icon["artifact"];
      const JsonValue& iconSha = icon["sha256"];
      unsigned iconBytes = 0;
      if (iconArtifact.isString() && iconSha.isString() && hashText(iconSha.stringValue.c_str()) &&
          jsonUInt(icon["bytes"], 1, 65535, iconBytes) && iconBytes == ICON_BUNDLE_BYTES &&
          catalogRelativeUrl(pageUrl, iconArtifact.stringValue.c_str(), entry.iconArtifact)) {
        entry.iconSha256 = iconSha.stringValue;
        entry.iconBytes = iconBytes;
      }
    }
    out.entries.push_back(std::move(entry));
  }
  return true;
}

std::string catalogSource() {
  bool ok = false;
  const std::string stored = Storage.readFile(SOURCE_URL_FILE, &ok, 256);
  if (ok && !stored.empty()) {
    std::string trimmed = stored;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' || trimmed.back() == ' '))
      trimmed.pop_back();
    if (validSourceUrl(trimmed.c_str())) return trimmed;
  }
  return DEFAULT_INDEX;
}

bool fetchCatalogBody(const std::string& pageUrl, std::string& body, std::string& error) {
  error.clear();
  body.clear();
  if (!validSourceUrl(pageUrl.c_str())) {
    error = "目录地址无效";
    return false;
  }
  net::Request req;
  req.url = pageUrl;
  req.cap = MAX_CATALOG_BYTES;
  req.cap_hard = true;
  req.deadline_ms = 20000;
  const net::Response response = net::request(req);
  if (!response.ok) {
    error = response.error.empty() ? "目录下载失败" : response.error;
    return false;
  }
  if (!writeTextFile(catalogCachePath(pageUrl).c_str(), response.body))
    ESP_LOGW(TAG, "cannot cache catalog page");
  body = response.body;
  return true;
}

bool fetchCatalogPage(const std::string& pageUrl, CatalogPage& out, std::string& error) {
  std::string body;
  if (!fetchCatalogBody(pageUrl, body, error)) return false;
  if (!parseCatalogPage(body, pageUrl, out)) {
    error = "目录格式无效";
    return false;
  }
  return true;
}

bool loadCachedCatalogPage(const std::string& pageUrl, CatalogPage& out) {
  std::string body;
  if (!readTextFile(catalogCachePath(pageUrl).c_str(), body, MAX_CATALOG_BYTES)) return false;
  return parseCatalogPage(body, pageUrl, out);
}

void recoverInstalls() {
  auto dir = Storage.open(INSTALL_DIR);
  if (!dir || !dir.isDirectory()) return;
  while (auto file = dir.openNextFile()) {
    char name[80]{};
    file.getName(name, sizeof(name));
    const size_t length = std::strlen(name);
    constexpr char suffix[] = ".csp.previous";
    if (length <= sizeof(suffix) - 1 || std::strcmp(name + length - sizeof(suffix) + 1, suffix)) continue;
    name[length - sizeof(suffix) + 1] = 0;
    if (validId(name)) recoverFile(Storage, installedPath(name));
  }
}

bool hasInstallRoom(const char* id) {
  if (Storage.exists(installedPath(id).c_str())) return true;
  auto directory = Storage.open(INSTALL_DIR);
  if (!directory || !directory.isDirectory()) return true;
  size_t count = 0;
  while (auto file = directory.openNextFile()) {
    char name[64]{};
    file.getName(name, sizeof(name));
    if (file.isDirectory() || !installedFilename(name)) continue;
    if (++count >= MAX_INSTALLED_PACKAGES) return false;
  }
  return true;
}

bool downloadFile(const std::string& url, const char* path, size_t limit, size_t& bytes,
                  void (*progress)(void* ctx, size_t got, size_t total, int percent), void* ctx,
                  std::string& error) {
  bytes = 0;
  error.clear();
  if (!validSourceUrl(url.c_str()) || !limit) {
    error = "下载地址无效";
    return false;
  }
  if (!Storage.ensureDirectoryExists(INSTALL_DIR)) {
    error = "无法创建应用目录";
    return false;
  }
  Storage.remove(path);
  auto file = Storage.open(path, FS_WRONLY | FS_CREAT | FS_TRUNC);
  if (!file) {
    error = "无法写入 SD 卡";
    return false;
  }
  psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
  if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
    error = "SHA-256 初始化失败";
    return false;
  }
  net::Request req;
  req.url = url;
  req.cap = limit;
  req.cap_hard = true;
  req.deadline_ms = 120000;
  req.progress = [&](size_t got, size_t total) {
    if (progress) progress(ctx, got, total, total ? static_cast<int>(got * 100 / total) : -1);
  };
  bool writeError = false;
  const net::Response response = net::stream(req, [&](const uint8_t* data, size_t length) {
    if (file.write(data, length) != length) {
      writeError = true;
      return false;
    }
    if (psa_hash_update(&hash, data, length) != PSA_SUCCESS) {
      writeError = true;
      return false;
    }
    bytes += length;
    return true;
  });
  file.flush();
  file.close();
  psa_hash_abort(&hash);
  if (!response.ok || writeError) {
    Storage.remove(path);
    bytes = 0;
    error = writeError ? "写入 SD 卡失败" : (response.error.empty() ? "下载失败" : response.error);
    return false;
  }
  if (bytes != response.got || bytes > limit) {
    Storage.remove(path);
    bytes = 0;
    error = "下载大小不符";
    return false;
  }
  return true;
}

bool verifyHash(const char* path, size_t expectedBytes, const char* expectedHash) {
  if (!expectedBytes || expectedBytes > MAX_PACKAGE_BYTES || !hashText(expectedHash)) return false;
  auto file = Storage.open(path);
  if (!file || file.isDirectory() || file.size() != expectedBytes) return false;
  psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
  if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) return false;
  uint8_t buffer[512];
  size_t remaining = expectedBytes;
  bool ok = true;
  while (remaining) {
    const size_t want = std::min(remaining, sizeof(buffer));
    if (file.read(buffer, want) != static_cast<int>(want) || psa_hash_update(&hash, buffer, want) != PSA_SUCCESS) {
      ok = false;
      break;
    }
    remaining -= want;
  }
  uint8_t digest[32];
  size_t digestBytes = 0;
  if (ok && psa_hash_finish(&hash, digest, sizeof(digest), &digestBytes) != PSA_SUCCESS) ok = false;
  if (!ok) {
    psa_hash_abort(&hash);
    return false;
  }
  char hex[65];
  for (unsigned i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
  return !std::strcmp(hex, expectedHash);
}

bool installDownloaded(const char* path, const CatalogEntry& entry, std::string& error) {
  error.clear();
  Package found;
  if (!readPackage(path, found)) {
    error = "应用清单无效";
    return false;
  }
  if (std::strcmp(found.id, entry.package.id) || std::strcmp(found.version, entry.package.version) ||
      found.api != entry.package.api || found.requiredCapabilities != entry.package.requiredCapabilities ||
      found.memoryBytes != entry.package.memoryBytes || found.permissions != entry.package.permissions) {
    error = "应用清单与目录不一致";
    return false;
  }
  char missing[192];
  if (!compatible(found, missing, sizeof(missing))) {
    error = missing;
    return false;
  }
  std::string script;
  if (!readPackageEntry(path, "main.lua", MAX_SCRIPT_BYTES, script) || script.empty()) {
    error = "缺少 main.lua";
    return false;
  }
  if (static_cast<uint8_t>(script[0]) == 0x1b || script.find('\0') != std::string::npos) {
    error = "应用脚本格式无效";
    return false;
  }
  if (!hasInstallRoom(found.id)) {
    error = "应用数量已达上限";
    return false;
  }
  if (!Storage.ensureDirectoryExists(INSTALL_DIR)) {
    error = "无法创建应用目录";
    return false;
  }
  const std::string target = installedPath(found.id);
  if (!commitFile(Storage, path, target)) {
    error = "写入应用失败";
    return false;
  }
  if (found.permissions) setGrantedPermissions(found.id, found.permissions);
  return true;
}

bool uninstallPackage(const char* id) {
  if (!validId(id)) return false;
  const std::string path = installedPath(id);
  bool ok = !Storage.exists(path.c_str()) || Storage.remove(path.c_str());
  Storage.remove((path + ".previous").c_str());
  // 应用数据目录按文件逐个删（FAT 上 removeDir 不一定递归）。
  const std::string dataDir = std::string(APP_DATA_DIR) + "/" + id;
  if (auto dir = Storage.open(dataDir.c_str())) {
    if (dir.isDirectory()) {
      // 目录项迭代器依赖底层 DIR 游标：一边 openNextFile 一边 remove 当前项在某些
      // FAT 实现里会漏删/跳项。先把名字收完再删。
      std::vector<std::string> names;
      while (auto child = dir.openNextFile()) {
        char name[64]{};
        child.getName(name, sizeof(name));
        child.close();
        if (name[0] && names.size() < 64) names.emplace_back(name);
      }
      for (const auto& name : names) Storage.remove((dataDir + "/" + name).c_str());
    }
    Storage.rmdir(dataDir.c_str());
  }
  Storage.remove((std::string(PERMISSION_DIR) + "/" + id).c_str());
  return ok;
}

void scanInstalled(std::vector<InstalledApp>& out) {
  out.clear();
  auto dir = Storage.open(INSTALL_DIR);
  if (!dir || !dir.isDirectory()) return;
  while (auto file = dir.openNextFile()) {
    char name[64]{};
    file.getName(name, sizeof(name));
    if (file.isDirectory() || !installedFilename(name)) continue;
    InstalledApp app;
    const std::string path = std::string(INSTALL_DIR) + "/" + name;
    app.valid = readPackage(path, app.package);
    if (!app.valid) std::snprintf(app.package.id, sizeof(app.package.id), "%s", name);
    if (!app.valid) app.package.id[std::strlen(app.package.id) - 4] = 0;
    out.push_back(app);
  }
  std::sort(out.begin(), out.end(), [](const InstalledApp& a, const InstalledApp& b) {
    const int byName = std::strcmp(a.package.name, b.package.name);
    return byName ? byName < 0 : std::strcmp(a.package.id, b.package.id) < 0;
  });
}

bool installedVersion(const char* id, char* out, size_t capacity) {
  if (!out || !capacity) return false;
  out[0] = 0;
  Package package;
  if (!validId(id) || !readPackage(installedPath(id), package)) return false;
  std::snprintf(out, capacity, "%s", package.version);
  return true;
}

uint32_t grantedPermissions(const char* id) {
  if (!validId(id)) return 0;
  const std::string path = std::string(PERMISSION_DIR) + "/" + id;
  if (!recoverFile(Storage, path)) return 0;
  auto file = Storage.open(path.c_str());
  uint32_t granted = 0;
  if (!file || file.isDirectory() || file.size() != sizeof(granted) ||
      file.read(&granted, sizeof(granted)) != sizeof(granted))
    return 0;
  return granted;
}

bool setGrantedPermissions(const char* id, uint32_t granted) {
  if (!validId(id)) return false;
  if (!Storage.ensureDirectoryExists(PERMISSION_DIR)) return false;
  const std::string path = std::string(PERMISSION_DIR) + "/" + id;
  const std::string temp = path + ".part";
  {
    auto file = Storage.open(temp.c_str(), FS_WRONLY | FS_CREAT | FS_TRUNC);
    if (!file || file.write(reinterpret_cast<const uint8_t*>(&granted), sizeof(granted)) != sizeof(granted))
      return false;
    file.flush();
    file.close();
  }
  return commitFile(Storage, temp.c_str(), path);
}

bool permissionGranted(const char* id, const char* permission) {
  return (grantedPermissions(id) & permissionBit(permission)) != 0;
}

bool appdataRead(const char* id, const char* key, char* out, size_t capacity) {
  if (!validId(id) || !validId(key) || !out || !capacity) return false;
  const std::string path = std::string(APP_DATA_DIR) + "/" + id + "/" + key;
  if (!recoverFile(Storage, path)) return false;
  auto file = Storage.open(path.c_str());
  if (!file || file.isDirectory() || file.size() >= capacity) return false;
  const size_t bytes = file.size();
  if (file.read(out, bytes) != static_cast<int>(bytes)) return false;
  out[bytes] = 0;
  return true;
}

bool appdataWrite(const char* id, const char* key, const char* value, size_t size) {
  if (!validId(id) || !validId(key) || !value || size > 2048) return false;
  const std::string dir = std::string(APP_DATA_DIR) + "/" + id;
  const std::string path = dir + "/" + key;
  const std::string temp = path + ".part";
  if (!Storage.ensureDirectoryExists(dir.c_str())) return false;
  if (!Storage.exists(path.c_str())) {
    auto directory = Storage.open(dir.c_str());
    if (!directory || !directory.isDirectory()) return false;
    size_t files = 0;
    while (auto file = directory.openNextFile())
      if (++files >= 32) return false;
  }
  {
    auto file = Storage.open(temp.c_str(), FS_WRONLY | FS_CREAT | FS_TRUNC);
    if (!file || file.write(value, size) != size) return false;
    file.flush();
    file.close();
  }
  return commitFile(Storage, temp.c_str(), path);
}

bool validIconBundle(const uint8_t* header, size_t headerBytes, size_t bundleBytes) {
  return header && headerBytes == sizeof(ICON_HEADER) && bundleBytes == ICON_BUNDLE_BYTES &&
         !std::memcmp(header, ICON_HEADER, sizeof(ICON_HEADER));
}

bool readBundleIcon(const uint8_t* bundle, size_t bundleBytes, int levels, int size, uint8_t* out, size_t capacity,
                    size_t& bytes) {
  bytes = 0;
  const IconVariant* variant = iconVariant(levels, size);
  if (!variant || !bundle || !validIconBundle(bundle, sizeof(ICON_HEADER), bundleBytes) || !out ||
      capacity < variant->bytes)
    return false;
  std::memcpy(out, bundle + variant->offset, variant->bytes);
  bytes = variant->bytes;
  return true;
}

bool readBundleIconFile(const char* path, int levels, int size, uint8_t* out, size_t capacity, size_t& bytes) {
  bytes = 0;
  auto file = Storage.open(path);
  if (!file || file.isDirectory() || file.size() != ICON_BUNDLE_BYTES) return false;
  uint8_t bundle[ICON_BUNDLE_BYTES];
  if (file.read(bundle, sizeof(bundle)) != static_cast<int>(sizeof(bundle))) return false;
  return readBundleIcon(bundle, sizeof(bundle), levels, size, out, capacity, bytes);
}

std::string iconCachePath(const std::string& sha256) {
  char name[96];
  std::snprintf(name, sizeof(name), "%s/icons-%s.csi", CACHE_DIR, sha256.c_str());
  return name;
}

bool ensureCatalogIcon(const CatalogEntry& entry, bool allowNetwork, std::string& error) {
  error.clear();
  if (entry.iconArtifact.empty() || !hashText(entry.iconSha256.c_str())) return false;
  const std::string path = iconCachePath(entry.iconSha256);
  uint8_t header[sizeof(ICON_HEADER)];
  {
    auto file = Storage.open(path.c_str());
    if (file && !file.isDirectory() && file.size() == ICON_BUNDLE_BYTES &&
        file.read(header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
        validIconBundle(header, sizeof(header), file.size()))
      return true;
  }
  if (!allowNetwork) return false;
  if (!Storage.ensureDirectoryExists(CACHE_DIR)) return false;
  size_t bytes = 0;
  const std::string temporary = path + ".part";
  if (!downloadFile(entry.iconArtifact, temporary.c_str(), ICON_BUNDLE_BYTES, bytes, nullptr, nullptr, error))
    return false;
  const bool ok = bytes == ICON_BUNDLE_BYTES && verifyHash(temporary.c_str(), ICON_BUNDLE_BYTES, entry.iconSha256.c_str());
  if (!ok) {
    Storage.remove(temporary.c_str());
    error = "图标校验失败";
    return false;
  }
  if (!Storage.rename(temporary.c_str(), path.c_str())) {
    Storage.remove(temporary.c_str());
    error = "图标缓存失败";
    return false;
  }
  return true;
}

bool readInstalledIcon(const char* id, int levels, int size, uint8_t* out, size_t capacity, size_t& bytes) {
  bytes = 0;
  const IconVariant* variant = iconVariant(levels, size);
  if (!validId(id) || !variant || !out || capacity < variant->bytes) return false;
  char entry[48];
  std::snprintf(entry, sizeof(entry), "assets/icon-g%u-%u.bin", variant->levels, variant->size);
  std::string data;
  if (!readPackageEntry(installedPath(id), entry, variant->bytes, data) || data.size() != variant->bytes)
    return false;
  std::memcpy(out, data.data(), variant->bytes);
  bytes = variant->bytes;
  return true;
}

}  // namespace crossstore
