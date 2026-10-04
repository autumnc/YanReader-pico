#include "dictionary_store.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "json_parser.h"

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

// ── HTTP 核心 ───────────────────────────────────────────────────────────
// perform + ON_DATA 流式收 body：只有 perform 会跟随重定向，而 GitHub Release
// 的下载地址必然 302 跳到 objects.githubusercontent.com。
struct HttpSink {
  std::string *mem = nullptr;                  // 收进内存（清单）
  FILE *fp = nullptr;                          // 落盘（词典文件）
  const std::function<void(size_t, size_t)> *progress = nullptr;
  size_t got = 0;
  size_t cap = 0;                              // >0 时超过即停
  bool overflow = false;                       // 落盘/超限失败
  bool wantCrc = false;
  uint32_t crc = 0xFFFFFFFF;
  int64_t deadline_us = 0;                     // >0：整个请求的墙钟上限
  bool timed_out = false;
};

static esp_err_t httpEventHandler(esp_http_client_event_t *evt) {
  HttpSink *s = static_cast<HttpSink *>(evt->user_data);
  if (!s) return ESP_OK;
  if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) return ESP_OK;
  // 整体墙钟上限，理由见 opds_client.cpp 同处注释：timeout_ms 是每次读的超时，
  // 会被每个到达的字节重置，对端慢慢挤字节就能把主任务永远钉在 perform 里。
  if (s->deadline_us && esp_timer_get_time() > s->deadline_us) {
    s->timed_out = true;
    if (evt->client) esp_http_client_set_timeout_ms(evt->client, 1);
    return ESP_OK;
  }
  if (evt->client) {
    // 每个分片都重取状态码（不能锁存）：跟重定向时中间那个 302 也带一小段
    // body，锁存会把正式内容一起丢掉。见 opds_client.cpp 同处注释。
    int st = esp_http_client_get_status_code(evt->client);
    if (st < 200 || st >= 300) return ESP_OK;
  }
  const size_t n = static_cast<size_t>(evt->data_len);
  if (s->cap && s->got + n > s->cap) {
    s->overflow = true;
    return ESP_FAIL;  // 中止请求
  }
  if (s->mem) {
    s->mem->append(static_cast<const char *>(evt->data), n);
  }
  if (s->fp) {
    if (fwrite(evt->data, 1, n, s->fp) != n) {
      s->overflow = true;
      return ESP_FAIL;
    }
  }
  if (s->wantCrc) s->crc = crc32Update(s->crc, static_cast<const uint8_t *>(evt->data), n);
  s->got += n;
  if (s->progress && *s->progress) {
    size_t total = 0;
    if (evt->client) {
      int64_t cl = esp_http_client_get_content_length(evt->client);
      if (cl > 0) total = static_cast<size_t>(cl);
    }
    (*s->progress)(s->got, total);
  }
  return ESP_OK;
}

// 执行一次 GET，body 交给 sink 处理。返回 true 表示 2xx 且完整收完。
static bool httpGet(const std::string &url, HttpSink &sink, std::string &err) {
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = 30000;
  cfg.skip_cert_common_name_check = true;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.max_redirection_count = 5;
  cfg.buffer_size = 4096;
  cfg.user_data = &sink;
  cfg.event_handler = httpEventHandler;
  // 单次请求的墙钟上限：词典文件可能很大、链路可能很慢，给 10 分钟；超过说明对端
  // 在挤牙膏（或链路已死而 socket 还活着），中止比把 UI 卡死强。
  sink.deadline_us = esp_timer_get_time() + 10LL * 60 * 1000000;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    err = "HTTP 初始化失败";
    return false;
  }
  esp_http_client_set_header(client, "User-Agent", "pjournal-pico/1.0");
  esp_http_client_set_header(client, "Accept-Encoding", "identity");  // 免 gzip 解压

  esp_err_t e = esp_http_client_perform(client);
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "请求失败: %s errno=%d url=%s", esp_err_to_name(e),
             esp_http_client_get_errno(client), url.c_str());
    err = sink.timed_out ? std::string("请求超时")
                         : (std::string("请求失败: ") + esp_err_to_name(e));
    esp_http_client_cleanup(client);
    return false;
  }
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (status < 200 || status >= 300) {
    char buf[32];
    snprintf(buf, sizeof(buf), "HTTP %d", status);
    err = buf;
    return false;
  }
  if (sink.overflow) {
    err = "写入失败(空间不足?)";
    return false;
  }
  return true;
}

// ── 清单 ────────────────────────────────────────────────────────────────
bool dictCatalogFetch(const std::string &url, DictCatalog &out, std::string &err) {
  out = DictCatalog{};
  err.clear();
  if (url.empty()) {
    err = "清单地址为空";
    return false;
  }

  std::string body;
  HttpSink sink;
  sink.mem = &body;
  sink.cap = 512 * 1024;
  if (!httpGet(url, sink, err)) return false;
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

  std::function<void(size_t, size_t)> fileCb;  // 每轮的进度桥（栈上，勿换 new）

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

    HttpSink sink;
    sink.fp = fp;
    sink.wantCrc = true;
    if (progress) {
      fileCb = [&progress, i, fileCount, phase](size_t got, size_t total) {
        progress(i, fileCount, got, total, phase);
      };
      sink.progress = &fileCb;
    }
    bool ok = httpGet(url, sink, err);
    fclose(fp);
    sink.progress = nullptr;

    if (!ok) {
      ESP_LOGW(TAG, "下载 %s 失败: %s", fr.name.c_str(), err.c_str());
      removeTree(staging);
      return false;
    }
    if (sink.got == 0) {
      err = fr.name + " 内容为空";
      removeTree(staging);
      return false;
    }
    // 校验：大小必须完全一致；crc32 仅在清单给了非 0 值时校验。
    if (fr.size && sink.got != fr.size) {
      char b[96];
      snprintf(b, sizeof(b), "%s 大小不符 (%u/%u)", fr.name.c_str(),
               static_cast<unsigned>(sink.got), static_cast<unsigned>(fr.size));
      err = b;
      removeTree(staging);
      return false;
    }
    if (fr.crc32) {
      uint32_t crc = sink.crc ^ 0xFFFFFFFFu;
      if (crc != fr.crc32) {
        char b[96];
        snprintf(b, sizeof(b), "%s 校验失败 (%08x/%08x)", fr.name.c_str(),
                 static_cast<unsigned>(crc), static_cast<unsigned>(fr.crc32));
        err = b;
        removeTree(staging);
        return false;
      }
    }
    if (progress) progress(i, fileCount, sink.got, sink.got, "校验通过");
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
