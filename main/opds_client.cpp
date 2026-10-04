#include "opds_client.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_tls.h>

#include <cstdio>
#include <cstring>

static const char *TAG = "OPDS";

// ── URL 处理 ────────────────────────────────────────────────────────────
// 拆出 "scheme://host[:port]" 与 "/path?query#frag" 两段。
static bool splitUrl(const std::string &url, std::string &schemeHost, std::string &path) {
  size_t p = url.find("://");
  if (p == std::string::npos) return false;
  size_t slash = url.find('/', p + 3);
  if (slash == std::string::npos) {
    schemeHost = url;
    path = "/";
  } else {
    schemeHost = url.substr(0, slash);
    path = url.substr(slash);
  }
  return true;
}

// 规范化路径：解析 "." / ".."、折叠重复斜杠，保留查询串与片段与末尾斜杠。
static std::string normalizePath(const std::string &in) {
  std::string tail;  // ?query#frag
  std::string path = in;
  size_t q = path.find_first_of("?#");
  if (q != std::string::npos) {
    tail = path.substr(q);
    path = path.substr(0, q);
  }
  std::vector<std::string> segs;
  size_t i = 0;
  while (i < path.size()) {
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    std::string seg = path.substr(i, j - i);
    if (seg == "..") {
      if (!segs.empty()) segs.pop_back();
    } else if (!seg.empty() && seg != ".") {
      segs.push_back(seg);
    }
    i = j + 1;
  }
  std::string out = "/";
  for (size_t k = 0; k < segs.size(); k++) {
    out += segs[k];
    if (k + 1 < segs.size()) out += '/';
  }
  if (!path.empty() && path.back() == '/' && out.back() != '/') out += '/';
  return out + tail;
}

std::string opdsResolveUrl(const std::string &base, const std::string &href) {
  if (href.empty()) return base;
  if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0) return href;

  std::string schemeHost, basePath;
  if (!splitUrl(base, schemeHost, basePath)) return href;  // base 不是绝对地址

  if (href.rfind("//", 0) == 0) {  // 协议相对
    size_t c = schemeHost.find("://");
    return schemeHost.substr(0, c + 1) + href;
  }
  if (href[0] == '/') return schemeHost + normalizePath(href);

  // 相对：拼到 base 所在目录后。
  size_t slash = basePath.rfind('/');
  std::string dir = (slash == std::string::npos) ? "/" : basePath.substr(0, slash + 1);
  return schemeHost + normalizePath(dir + href);
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string opdsFilenameFromUrl(const std::string &url) {
  std::string s = url;
  size_t f = s.find_first_of("?#");
  if (f != std::string::npos) s = s.substr(0, f);
  size_t sl = s.rfind('/');
  if (sl != std::string::npos) s = s.substr(sl + 1);
  if (s.empty()) return "";

  // %XX 解码（UTF-8 字节原样保留）。
  std::string dec;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int hi = hexVal(s[i + 1]), lo = hexVal(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        dec += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    dec += s[i];
  }

  // 过滤掉路径分隔符、控制字符与 FAT 非法字符。
  std::string safe;
  for (char c : dec) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|') {
      safe += '_';
    } else {
      safe += c;
    }
  }
  if (safe.empty() || safe == "." || safe == "..") return "";
  return safe;
}

// ── HTTP 公共配置 ───────────────────────────────────────────────────────
static void fillConfig(esp_http_client_config_t &cfg, const std::string &url) {
  cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = 30000;
  cfg.skip_cert_common_name_check = true;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.max_redirection_count = 5;
  cfg.buffer_size = 4096;
}

static void setCommonHeaders(esp_http_client_handle_t client) {
  esp_http_client_set_header(client, "User-Agent", "pjournal-pico/1.0");
  esp_http_client_set_header(client, "Accept-Encoding", "identity");  // 免去 gzip 解压
}

// ── HTTP 请求（流式，走 perform 以便自动跟随重定向）───────────────────────
// 低层 open/fetch_headers API 不会跟随重定向（只有 perform 才会），而 OPDS
// 下载地址常 302 跳镜像；所以统一用 perform + ON_DATA 事件流式收 body。
struct HttpCtx {
  OpdsParser *parser = nullptr;
  FILE *fp = nullptr;
  const std::function<void(size_t, size_t)> *progress = nullptr;
  size_t got = 0;
  bool overflow = false;
  size_t cap = 0;  // >0 时超过即停（防 feed 无上限）
  int64_t deadline_us = 0;  // >0：整个请求的墙钟上限
  bool timed_out = false;
};

static esp_err_t httpEventHandler(esp_http_client_event_t *evt) {
  HttpCtx *c = static_cast<HttpCtx *>(evt->user_data);
  if (!c) return ESP_OK;
  if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) return ESP_OK;
  // 整体墙钟上限。cfg.timeout_ms 是**每次读**的超时，每收到一个字节就重置；对端只要
  // 每隔 29 秒挤一个字节，perform 就永远回不来，而它是在主任务上同步跑的 —— 整个 UI
  // 卡死，且按键取消也排不上队。到点把传输超时压到 1ms：下一次读立刻返回超时，
  // perform 随即退出。set_timeout_ms 是无锁公开 API，可以在事件回调里直接改。
  // 只在 ON_DATA 上判定就够：真的一个字节都不来，单次读的 30s 超时自然兜底。
  if (c->deadline_us && esp_timer_get_time() > c->deadline_us) {
    c->timed_out = true;
    if (evt->client) esp_http_client_set_timeout_ms(evt->client, 1);
    return ESP_OK;
  }
  if (evt->client) {
    // 每个分片都重新取状态，**不能**用锁存：跟随重定向时中间那个 302 响应
    // 也可能带一小段 body（HTML 提示页），锁存会把后面的正式内容一起丢掉。
    int st = esp_http_client_get_status_code(evt->client);
    if (st < 200 || st >= 300) return ESP_OK;  // 跳过重定向页/错误页的 body
  }
  // 上限（cap>0 时生效）：服务器端异常/恶意 feed 可以一直发，parser 里的条目表跟着长。
  // 到顶就不再喂，后面的数据丢掉（下载路径另有 fp 的写入失败兜底）。
  if (c->cap && c->got >= c->cap) {
    c->overflow = true;
    return ESP_OK;
  }
  if (c->parser) {
    c->parser->write(static_cast<const uint8_t *>(evt->data), static_cast<size_t>(evt->data_len));
  }
  if (c->fp) {
    size_t w = fwrite(evt->data, 1, static_cast<size_t>(evt->data_len), c->fp);
    if (w != static_cast<size_t>(evt->data_len)) c->overflow = true;
  }
  c->got += static_cast<size_t>(evt->data_len);
  if (c->progress && *c->progress) {
    size_t total = 0;
    if (evt->client) {
      int64_t cl = esp_http_client_get_content_length(evt->client);
      if (cl > 0) total = static_cast<size_t>(cl);
    }
    (*c->progress)(c->got, total);
  }
  return ESP_OK;
}

// 执行一次 GET。返回 true 表示 2xx 且 body 已通过 ctx 处理完。
static bool httpGet(const std::string &url, const char *accept, HttpCtx &ctx, std::string &err) {
  esp_http_client_config_t cfg;
  fillConfig(cfg, url);
  cfg.user_data = &ctx;
  cfg.event_handler = httpEventHandler;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    err = "HTTP 初始化失败";
    return false;
  }
  setCommonHeaders(client);
  if (accept) esp_http_client_set_header(client, "Accept", accept);

  esp_err_t e = esp_http_client_perform(client);  // 内部自动跟随重定向
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "请求失败: %s (%d) errno=%d url=%s", esp_err_to_name(e), (int)e,
             esp_http_client_get_errno(client), url.c_str());
    err = ctx.timed_out ? std::string("请求超时")
                        : (std::string("请求失败: ") + esp_err_to_name(e));
    esp_http_client_cleanup(client);
    return false;
  }
  int status = esp_http_client_get_status_code(client);
  if (status < 200 || status >= 300) {
    char buf[48];
    snprintf(buf, sizeof(buf), "HTTP %d", status);
    err = buf;
    esp_http_client_cleanup(client);
    return false;
  }
  esp_http_client_cleanup(client);
  return true;
}

// ── 拉取 feed ───────────────────────────────────────────────────────────
bool opdsFetchFeed(const std::string &url, std::vector<OpdsEntry> &entries,
                   std::string &nextPageUrl, std::string &err) {
  entries.clear();
  nextPageUrl.clear();
  err.clear();
  if (url.empty()) {
    err = "目录地址为空";
    return false;
  }

  OpdsParser parser;
  HttpCtx ctx;
  ctx.parser = &parser;
  ctx.cap = 512 * 1024;  // feed 上限：正常目录几百 KB 都到不了，超了必有蹊跷
  ctx.deadline_us = esp_timer_get_time() + 60LL * 1000000;  // 整个 feed 最多 60 秒
  if (!httpGet(url, "application/atom+xml,application/xml;q=0.9,*/*;q=0.8", ctx, err)) return false;
  size_t total = ctx.got;

  if (parser.error()) {
    err = "feed 解析失败";
    return false;
  }

  const std::vector<OpdsEntry> &raw = parser.getEntries();
  entries.reserve(raw.size());
  for (const auto &e : raw) {
    if (e.href.empty()) continue;
    OpdsEntry c = e;
    c.href = opdsResolveUrl(url, e.href);
    entries.push_back(std::move(c));
  }
  if (!parser.getNextPageUrl().empty()) nextPageUrl = opdsResolveUrl(url, parser.getNextPageUrl());
  ESP_LOGI(TAG, "feed %s: %u 条 (%u 字节)", url.c_str(), static_cast<unsigned>(entries.size()),
           static_cast<unsigned>(total));
  if (entries.empty()) {
    err = "feed 无条目";
    return false;
  }
  return true;
}

// ── 下载电子书 ──────────────────────────────────────────────────────────
bool opdsDownloadFile(const std::string &url, const std::string &destPath,
                      const std::function<void(size_t, size_t)> &progress, std::string &err) {
  err.clear();
  FILE *fp = fopen(destPath.c_str(), "wb");
  if (!fp) {
    err = "无法写入文件";
    return false;
  }
  // 显式给 FILE 一个内部 RAM 的缓冲：SDMMC 只按对齐判定能否直接 DMA，
  // 这里保持缓冲在 DMA 可达区，避免退回弹跳缓冲（见 sd-dma-buffer-hazard）。
  static char fbuf[4096];
  setvbuf(fp, fbuf, _IOFBF, sizeof(fbuf));

  HttpCtx ctx;
  ctx.fp = fp;
  ctx.progress = &progress;
  // 下载给足时间（大书 + 慢链路），但仍有上限：卡在"每 29 秒一个字节"的对端上时，
  // 主任务不能陪着它耗到天荒地老。到点会中止并在下面按失败清理掉半截文件。
  ctx.deadline_us = esp_timer_get_time() + 15LL * 60 * 1000000;
  bool ok = httpGet(url, "application/epub+zip,*/*;q=0.8", ctx, err);
  fclose(fp);

  if (ok && ctx.overflow) {
    err = "写入 SD 卡失败(空间不足?)";
    ok = false;
  }
  if (ok && ctx.got == 0) {
    err = "下载内容为空";
    ok = false;
  }
  if (!ok) {
    remove(destPath.c_str());
    return false;
  }
  ESP_LOGI(TAG, "下载完成 %s (%u 字节)", destPath.c_str(), static_cast<unsigned>(ctx.got));
  return true;
}
