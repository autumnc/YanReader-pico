#include "dictionary_store.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_log.h>

#include "json_parser.h"
#include "net/http.h"

static const char *TAG = "DictStore";

// ── 小工具 ──────────────────────────────────────────────────────────────
static std::string pathJoin(const std::string &a, const std::string &b) {
  if (a.empty()) return b;
  if (a.back() == '/') return a + b;
  return a + "/" + b;
}

static bool dirExists(const std::string &p) {
  struct stat s;
  return stat(p.c_str(), &s) == 0 && S_ISDIR(s.st_mode);
}

static void ensureDir(const std::string &p) {
  if (!dirExists(p)) mkdir(p.c_str(), 0777);
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

// 目录内所有普通文件的字节数之和；同时把最大的那个文件名带出来（错误提示用）。
static size_t dirSizeBytes(const std::string &p) {
  DIR *dp = opendir(p.c_str());
  if (!dp) return 0;
  struct dirent *e;
  size_t total = 0;
  while ((e = readdir(dp)) != nullptr) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    struct stat s;
    if (stat(pathJoin(p, e->d_name).c_str(), &s) == 0 && S_ISREG(s.st_mode))
      total += static_cast<size_t>(s.st_size);
  }
  closedir(dp);
  return total;
}

// 版本标记文件内容 "1:<revision>\n"；缺失或格式不符返回 0（= 未安装）。
static int readMarkerRevision(const std::string &dir) {
  FILE *fp = fopen(pathJoin(dir, DICT_MARKER_NAME).c_str(), "rb");
  if (!fp) return 0;
  char buf[32] = {0};
  size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  fclose(fp);
  if (n == 0) return 0;
  int ver = 0, rev = 0;
  if (sscanf(buf, "%d:%d", &ver, &rev) != 2) return 0;
  return ver == 1 ? rev : 0;
}

static bool writeMarker(const std::string &dir, int revision) {
  std::string p = pathJoin(dir, DICT_MARKER_NAME);
  FILE *fp = fopen(p.c_str(), "wb");
  if (!fp) return false;
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "1:%d\n", revision);
  bool ok = fwrite(buf, 1, static_cast<size_t>(n), fp) == static_cast<size_t>(n);
  fclose(fp);
  if (!ok) remove(p.c_str());
  return ok;
}

std::string dictFormatSize(size_t bytes) {
  char buf[32];
  if (bytes < 1024) snprintf(buf, sizeof(buf), "%u B", static_cast<unsigned>(bytes));
  else if (bytes < 1024 * 1024) snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
  else if (bytes < 1024ull * 1024 * 1024) snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
  else snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
  return buf;
}

// ── CRC-32（IEEE，与 zlib / ZIP / StarDict 清单一致）─────────────────────
// 清单里的 crc32 已用 zlib.crc32 对齐验证过（century-en-zh.ifo = 2657825229），
// 所以这里实现标准反射多项式 0xEDB88320，初值/末值取反。
static uint32_t s_crcTable[256];
static bool s_crcTableReady = false;

static void crc32Init() {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    s_crcTable[i] = c;
  }
  s_crcTableReady = true;
}

static inline uint32_t crc32Update(uint32_t crc, const uint8_t *p, size_t n) {
  if (!s_crcTableReady) crc32Init();
  for (size_t i = 0; i < n; i++) crc = s_crcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

// ── HTTP ────────────────────────────────────────────────────────────────
// 收发本身在 net/http（perform + ON_DATA，才能跟随重定向：GitHub Release 的
// 下载地址必然 302 跳到 objects.githubusercontent.com）。这里只管 URL 与
// "收到的东西往哪放"。
static net::Request makeRequest(const std::string &url) {
  net::Request req;
  req.url = url;
  req.headers = {{"User-Agent", "pjournal-pico/1.0"},
                 {"Accept-Encoding", "identity"}};  // 免 gzip 解压
  req.buffer_size = 4096;
  // 单次请求的墙钟上限：词典文件可能很大、链路可能很慢，给 10 分钟；超过说明对端
  // 在挤牙膏（或链路已死而 socket 还活着），中止比把 UI 卡死强。
  req.deadline_ms = 10LL * 60 * 1000;
  return req;
}

// ── 清单 ────────────────────────────────────────────────────────────────
bool dictCatalogFetch(const std::string &url, DictCatalog &out, std::string &err) {
  out = DictCatalog{};
  err.clear();
  if (url.empty()) {
    err = "清单地址为空";
    return false;
  }

  net::Request req = makeRequest(url);
  req.cap = 512 * 1024;  // 硬上限：清单超了必有蹊跷，直接中止
  req.cap_hard = true;
  net::Response resp = net::stream(req);  // sink 为空 → body 收进内存
  if (!resp.ok) {
    err = resp.error;
    return false;
  }
  const std::string &body = resp.body;
  ESP_LOGI(TAG, "清单 %u 字节", static_cast<unsigned>(body.size()));

  JsonValue root = JsonValue::parse(body);
  if (!root.isObject()) {
    err = "清单解析失败";
    return false;
  }
  if (root["version"].asInt(0) != 1) {
    err = "清单版本不支持";
    return false;
  }
  out.revision = root["revision"].asInt(0);
  out.baseUrl = root["baseUrl"].asString();
  if (out.baseUrl.empty()) {
    err = "清单缺少 baseUrl";
    return false;
  }
  if (out.baseUrl.back() != '/') out.baseUrl += '/';

  const JsonValue &arr = root["dictionaries"];
  // size() 对**对象**返回 memberKeys.size()，而 operator[](size_t) 取的是 elements[]，
  // 清单里把 dictionaries 写成对象（而不是数组）时两者口径不一致 → 空 vector 取下标
  // → 崩。清单正文来自网络，必须先确认是数组。
  if (!arr.isArray()) {
    err = "清单格式错误：dictionaries 不是数组";
    return false;
  }
  for (size_t i = 0; i < arr.size(); i++) {
    const JsonValue &d = arr[i];
    DictCatalogItem it;
    it.id = d["id"].asString();
    if (it.id.empty()) continue;
    // 目录名会被拼进路径：拒绝路径分隔符，免得清单被篡改后写出根目录。
    if (it.id.find('/') != std::string::npos || it.id.find('\\') != std::string::npos ||
        it.id[0] == '.') {
      ESP_LOGW(TAG, "忽略可疑词典 id: %s", it.id.c_str());
      continue;
    }
    it.name = d["name"].asString(it.id);
    it.description = d["description"].asString();
    it.revision = d["revision"].asInt(1);
    const JsonValue &files = d["files"];
    if (!files.isArray()) continue;   // 同上：不是数组就别按下标取
    for (size_t k = 0; k < files.size(); k++) {
      const JsonValue &f = files[k];
      DictFileRef fr;
      fr.name = f["name"].asString();
      // 文件名同样会被拼进路径，做同样的拒绝。
      if (fr.name.empty() || fr.name[0] == '.' || fr.name.find('/') != std::string::npos ||
          fr.name.find('\\') != std::string::npos)
        continue;
      fr.size = static_cast<size_t>(f["size"].asNumber(0));
      fr.crc32 = static_cast<uint32_t>(f["crc32"].asNumber(0));
      it.totalSize += fr.size;
      it.files.push_back(std::move(fr));
    }
    if (it.files.empty()) continue;
    out.items.push_back(std::move(it));
  }
  if (out.items.empty()) {
    err = "清单无词典";
    return false;
  }
  return true;
}

// ── 本地已装 ────────────────────────────────────────────────────────────
void dictListLocal(std::vector<DictLocalItem> &out) {
  out.clear();
  DIR *dp = opendir(DICT_ROOT_DIR);
  if (!dp) return;
  struct dirent *e;
  while ((e = readdir(dp)) != nullptr) {
    std::string name = e->d_name;
    if (name.empty() || name[0] == '.') continue;  // 跳过 . 与 .staging/.bak
    std::string full = pathJoin(DICT_ROOT_DIR, name);
    if (!dirExists(full)) continue;
    DictLocalItem it;
    it.id = name;
    it.revision = readMarkerRevision(full);
    it.sizeBytes = dirSizeBytes(full);
    out.push_back(std::move(it));
  }
  closedir(dp);
}

bool dictDelete(const std::string &id, std::string &err) {
  err.clear();
  if (id.empty() || id[0] == '.' || id.find('/') != std::string::npos) {
    err = "非法词典名";
    return false;
  }
  removeTree(pathJoin(DICT_ROOT_DIR, "." + id + ".staging"));
  removeTree(pathJoin(DICT_ROOT_DIR, "." + id + ".bak"));
  if (!removeTree(pathJoin(DICT_ROOT_DIR, id))) {
    err = "删除失败";
    return false;
  }
  ESP_LOGI(TAG, "已删除词典 %s", id.c_str());
  return true;
}

// ── 安装 ────────────────────────────────────────────────────────────────
bool dictInstall(const DictCatalog &cat, int idx,
                 const std::function<void(int, int, size_t, size_t, const char *)> &progress,
                 std::string &err) {
  err.clear();
  if (idx < 0 || idx >= static_cast<int>(cat.items.size())) {
    err = "序号越界";
    return false;
  }
  const DictCatalogItem &it = cat.items[idx];
  const std::string staging = pathJoin(DICT_ROOT_DIR, "." + it.id + ".staging");
  const std::string finalDir = pathJoin(DICT_ROOT_DIR, it.id);
  const std::string backup = pathJoin(DICT_ROOT_DIR, "." + it.id + ".bak");

  const int fileCount = it.fileCount();

  ensureDir(DICT_ROOT_DIR);
  removeTree(staging);
  ensureDir(staging);

  for (int i = 0; i < fileCount; i++) {
    const DictFileRef &fr = it.files[i];
    const std::string url = cat.baseUrl + fr.name;
    const std::string dest = pathJoin(staging, fr.name);

    char phase[64];
    snprintf(phase, sizeof(phase), "下载 %s", fr.name.c_str());
    if (progress) progress(i, fileCount, 0, fr.size, phase);

    FILE *fp = fopen(dest.c_str(), "wb");
    if (!fp) {
      err = "无法写入 " + fr.name;
      removeTree(staging);
      return false;
    }
    // 显式给 FILE 一块内部 RAM、64 字节对齐的缓冲：SDMMC 只按对齐判定能否
    // 直接 DMA，否则会退回弹跳缓冲（内部 DMA RAM 只剩 ~30KB，会分配失败）。
    // 见 [[sd-dma-buffer-hazard]]。
    static char s_fbuf[4096] __attribute__((aligned(64)));
    setvbuf(fp, s_fbuf, _IOFBF, sizeof(s_fbuf));

    net::Request req = makeRequest(url);
    if (progress) {
      req.progress = [&progress, i, fileCount, phase](size_t got, size_t total) {
        progress(i, fileCount, got, total, phase);
      };
    }
    uint32_t crc = 0xFFFFFFFF;  // 边落盘边算，省一遍读
    net::Response resp = net::stream(req, [fp, &crc](const uint8_t *d, size_t n) {
      if (fwrite(d, 1, n, fp) != n) return false;
      crc = crc32Update(crc, d, n);
      return true;
    });
    fclose(fp);

    if (resp.sink_failed) err = "写入失败(空间不足?)";
    if (!resp.ok) {
      if (err.empty()) err = resp.error;
      ESP_LOGW(TAG, "下载 %s 失败: %s", fr.name.c_str(), err.c_str());
      removeTree(staging);
      return false;
    }
    if (resp.got == 0) {
      err = fr.name + " 内容为空";
      removeTree(staging);
      return false;
    }
    // 校验：大小必须完全一致；crc32 仅在清单给了非 0 值时校验。
    if (fr.size && resp.got != fr.size) {
      char b[96];
      snprintf(b, sizeof(b), "%s 大小不符 (%u/%u)", fr.name.c_str(),
               static_cast<unsigned>(resp.got), static_cast<unsigned>(fr.size));
      err = b;
      removeTree(staging);
      return false;
    }
    if (fr.crc32) {
      uint32_t got = crc ^ 0xFFFFFFFFu;
      if (got != fr.crc32) {
        char b[96];
        snprintf(b, sizeof(b), "%s 校验失败 (%08x/%08x)", fr.name.c_str(),
                 static_cast<unsigned>(got), static_cast<unsigned>(fr.crc32));
        err = b;
        removeTree(staging);
        return false;
      }
    }
    if (progress) progress(i, fileCount, resp.got, resp.got, "校验通过");
  }

  if (!writeMarker(staging, it.revision)) {
    err = "写入标记失败";
    removeTree(staging);
    return false;
  }

  // 换入：旧目录先挪到 .bak，再把 .staging 改名，最后删 .bak。中途失败可回滚。
  removeTree(backup);
  if (dirExists(finalDir) && rename(finalDir.c_str(), backup.c_str()) != 0) {
    err = "替换旧词典失败";
    removeTree(staging);
    return false;
  }
  if (rename(staging.c_str(), finalDir.c_str()) != 0) {
    err = "安装失败(改名)";
    removeTree(staging);
    if (dirExists(backup)) rename(backup.c_str(), finalDir.c_str());  // 回滚
    return false;
  }
  removeTree(backup);
  ESP_LOGI(TAG, "已安装词典 %s (rev %d)", it.id.c_str(), it.revision);
  return true;
}
