#include "opds_client.h"

#include <esp_log.h>

#include <cstdio>
#include <cstring>

#include "net/http.h"

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
// 请求构造集中在这里；收发本身在 net/http（见 [[net/http]] 的文件头注释）。
static net::Request makeRequest(const std::string &url, const char *accept) {
  net::Request req;
  req.url = url;
  req.headers = {{"User-Agent", "pjournal-pico/1.0"},
                 {"Accept-Encoding", "identity"},  // 免去 gzip 解压
                 {"Accept", accept}};
  req.buffer_size = 4096;
  return req;
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
  net::Request req = makeRequest(url, "application/atom+xml,application/xml;q=0.9,*/*;q=0.8");
  req.cap = 512 * 1024;           // 软上限：正常目录几百 KB 都到不了，超了必有蹊跷
  req.deadline_ms = 60LL * 1000;  // 整个 feed 最多 60 秒
  net::Response resp = net::stream(req, [&parser](const uint8_t *d, size_t n) {
    parser.write(d, n);
    return true;
  });
  if (!resp.ok) {
    err = resp.error;
    return false;
  }
  size_t total = resp.got;

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

  net::Request req = makeRequest(url, "application/epub+zip,*/*;q=0.8");
  // 下载给足时间（大书 + 慢链路），但仍有上限：卡在"每 29 秒一个字节"的对端上时，
  // 主任务不能陪着它耗到天荒地老。到点会中止并在下面按失败清理掉半截文件。
  req.deadline_ms = 15LL * 60 * 1000;
  req.progress = progress;
  net::Response resp = net::stream(req, [fp](const uint8_t *d, size_t n) {
    return fwrite(d, 1, n, fp) == n;
  });
  fclose(fp);

  bool ok = resp.ok;
  if (resp.sink_failed) {
    err = "写入 SD 卡失败(空间不足?)";
    ok = false;
  }
  if (ok && resp.got == 0) {
    err = "下载内容为空";
    ok = false;
  }
  if (!ok) {
    if (err.empty()) err = resp.error;
    remove(destPath.c_str());
    return false;
  }
  ESP_LOGI(TAG, "下载完成 %s (%u 字节)", destPath.c_str(), static_cast<unsigned>(resp.got));
  return true;
}
