// SPDX-License-Identifier: MIT
// ── PDF 文档/页缓存层：HalPdfSource + 文档生命周期 + 视口 blit ─────────────
// 解析器在 components/crossmux/lib/Pdf/，只认 PdfSource 随机读抽象；这里把
// HalFile 包成 PdfSource，并把页面主图像解码成原生 PXC 缓存（JBIG2 扫描页
// 1bpp，其余 4bpp）。当前页在 PSRAM 留一份副本，翻页/缩放/菜单重绘不再读 SD。
//
// 缓存根与阅读器的书缓存同一套约定：/sdcard/.crossmux/pdf_<hash(path)>/。
// 文件名带格式版本 v3（= PixelCache/DitherUtils 的 16 级 4bpp 布局），换口径只改
// 名字即可自然作废。页缓存按页一个文件，旋转不生成变体（折进 PdfViewport）。
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include <DirectPixelWriter.h>
#include <HalStorage.h>
#include <JpegToFramebufferConverter.h>
#include <PdfContent.h>
#include <PdfDocument.h>
#include <PdfImage.h>
#include <PdfPage.h>
#include <PdfPxc.h>
#include <PdfScratch.h>
#include <PdfViewport.h>

#include "screen_reader_internal.h"

static const char *TAG = "PdfIO";

namespace {

// ── HalFile → PdfSource ─────────────────────────────────────────────────
struct PdfFileSource final : PdfSource {
  HalFile *file = nullptr;
  uint64_t bytes = 0;
  uint64_t size() const override { return bytes; }
  int readAt(uint64_t off, void *buf, size_t len) const override {
    if (!file || !file->isOpen() || off > bytes) return -1;
    const size_t avail = static_cast<size_t>(bytes - off);
    const size_t n = len < avail ? len : avail;
    if (n == 0) return 0;
    if (!file->seek64(off)) return -1;
    const int got = file->read(buf, n);
    return got;
  }
};

PdfFileSource g_src;
PdfDocument g_doc;
HalFile g_file;
std::string g_path;
std::string g_dir;
std::string g_title;

struct PageCache {
  bool ready = false;
  int page = -1;
  int width = 0;
  int height = 0;
  int pageRotate = 0;
  bool cropComputed = false;
  PdfPxcCropBox crop;
  std::string pxcPath;
};
PageCache g_page;

bool g_open = false;

// 不长期攥着 SD 句柄：关机/USB 模式前 read_pico_sd_sync() 要求所有句柄已还，
// 所以文档文件只在解析/解码期间打开，解码一结束就关。PdfDocument 只认 PdfSource
// 偏移，重开同一个文件继续读是安全的。
bool docFileOpen() {
  if (g_file.isOpen()) return true;
  if (g_path.empty()) return false;
  if (!Storage.openFileForRead(TAG, g_path, g_file)) return false;
  g_src.file = &g_file;
  g_src.bytes = g_file.fileSize64();
  return true;
}

void docFileClose() {
  if (g_file.isOpen()) g_file.close();
}

struct DocFileScope {
  bool ok = false;
  DocFileScope() { ok = docFileOpen(); }
  ~DocFileScope() { docFileClose(); }
  DocFileScope(const DocFileScope &) = delete;
  DocFileScope &operator=(const DocFileScope &) = delete;
};

std::string cacheDirFor(const std::string &path) {
  return std::string(CACHE_DIR) + "/pdf_" + std::to_string(std::hash<std::string>{}(path));
}

std::string pagePxcPath(int page) {
  char name[32];
  snprintf(name, sizeof(name), "/p%05d_v3.pxc", page);
  return g_dir + name;
}

void ramPageClear();

void resetPage() {
  g_page = PageCache{};
  ramPageClear();
}

// ── 当前页的 PSRAM 副本 ─────────────────────────────────────────────────
// 4bpp 页（JPEG/Flate）原生分辨率约 5 MB，PSRAM 放不下；1bpp 扫描页约 1.2 MB，
// 正好装得下。菜单/缩放/平移每次都整页重绘，从 SD 读 5 MB 要好几秒——这是
// "载入后 UI 很慢"的主因；有副本时 blit/裁边全走内存。
constexpr size_t kRamPageMaxBytes = 2 * 1024 * 1024;
pdfscratch::ByteVec g_ramPage;   // 含 5 字节 PXC 头
int g_ramPageNo = -1;

void ramPageClear() {
  // 用 swap 空向量释放：ESP-IDF 的 -fno-exceptions 构建下 shrink_to_fit() 是
  // 空操作，clear()+shrink_to_fit() 会让上一页的 1.26 MB 副本一直占着 PSRAM，
  // 下一页解码 img.bits 时直接 OOM（「PDF 图像内存不足」）。
  pdfscratch::releaseVec(g_ramPage);
  g_ramPageNo = -1;
}

bool ramPageMatches(int page) {
  return g_ramPageNo == page && g_ramPage.size() >= kPdfPxcHeaderBytes;
}

// 把页缓存文件整个读进 PSRAM；太大或读失败就放弃（调用方退回按需读 SD）。
bool ramPageLoad(const std::string &path, int page) {
  ramPageClear();
  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) return false;
  PdfFileSource fs;
  fs.file = &f;
  fs.bytes = f.fileSize64();
  PdfPxcHeader hdr;
  PdfError e = PdfError::Ok;
  std::string msg;
  bool ok = pdfPxcReadHeader(fs, &hdr, &e, &msg);
  const uint64_t need = static_cast<uint64_t>(kPdfPxcHeaderBytes) + hdr.payloadBytes();
  if (ok && need <= kRamPageMaxBytes && pdfscratch::largestFree() >= need + (need >> 4)) {
    g_ramPage.resize(static_cast<size_t>(need));
    ok = fs.readAt(0, g_ramPage.data(), g_ramPage.size()) == static_cast<int>(g_ramPage.size());
  } else {
    ok = false;
  }
  f.close();
  if (!ok) {
    ramPageClear();
    return false;
  }
  g_ramPageNo = page;
  return true;
}

// 内存版 PdfSource：blit/裁边按行读缓存时不再碰 SD。
struct PdfRamSource final : PdfSource {
  const uint8_t *data = nullptr;
  uint64_t bytes = 0;
  uint64_t size() const override { return bytes; }
  int readAt(uint64_t off, void *buf, size_t len) const override {
    if (!data || off > bytes) return -1;
    const size_t avail = static_cast<size_t>(bytes - off);
    const size_t n = len < avail ? len : avail;
    if (n == 0) return 0;
    memcpy(buf, data + off, n);
    return static_cast<int>(n);
  }
};

// 页缓存读取源：优先 PSRAM 副本，否则打开文件（RAII 归还句柄）。
struct PageSource {
  HalFile file;
  PdfFileSource fsrc;
  PdfRamSource rsrc;
  const PdfSource *src = nullptr;
  bool open(const std::string &path, int page) {
    if (ramPageMatches(page)) {
      rsrc.data = g_ramPage.data();
      rsrc.bytes = g_ramPage.size();
      src = &rsrc;
      return true;
    }
    if (!Storage.openFileForRead(TAG, path, file)) return false;
    fsrc.file = &file;
    fsrc.bytes = file.fileSize64();
    src = &fsrc;
    return true;
  }
  ~PageSource() {
    if (file.isOpen()) file.close();
  }
};

bool attachFingerprint(uint64_t fileSize) {
  g_dir = cacheDirFor(g_path);
  if (!Storage.ensureDirectoryExists(g_dir.c_str())) return false;
  const std::string fpPath = g_dir + "/fp_v1";
  const std::string want = std::to_string(fileSize);
  bool ok = false;
  const std::string have = Storage.readFile(fpPath.c_str(), &ok, 64);
  if (!ok || have != want) {
    // 同名文件换过内容（或旧缓存格式）：整目录重建，页缓存自然作废。
    Storage.removeDir(g_dir.c_str());
    if (!Storage.ensureDirectoryExists(g_dir.c_str())) return false;
    if (!Storage.writeFile(fpPath.c_str(), want)) return false;
  }
  return true;
}

// 页缓存 GC：每页 PXC 是原生分辨率（A4 300dpi 约 4.3 MB），整本翻一遍能吃掉
// 几百 MB SD。只留最近 kMaxCachedPages 页；翻回去的页再解一次即可。
constexpr int kMaxCachedPages = 8;

void gcOldPages(int keepPage) {
  DIR *dp = opendir(g_dir.c_str());
  if (!dp) return;
  struct Ent {
    std::string path;
    std::time_t mtime;
    int page;
  };
  std::vector<Ent> ents;
  struct dirent *de;
  while ((de = readdir(dp)) != nullptr) {
    int pg = -1;
    char tail[8];
    if (sscanf(de->d_name, "p%d_v3.pxc%7s", &pg, tail) != 1 || pg < 0) continue;
    if (pg == keepPage) continue;
    const std::string p = g_dir + "/" + de->d_name;
    struct stat st;
    if (stat(p.c_str(), &st) != 0) continue;
    ents.push_back(Ent{p, st.st_mtime, pg});
  }
  closedir(dp);
  if (ents.size() <= static_cast<size_t>(kMaxCachedPages - 1)) return;
  std::sort(ents.begin(), ents.end(), [](const Ent &a, const Ent &b) {
    if (a.mtime != b.mtime) return a.mtime < b.mtime;
    return a.page < b.page;
  });
  // 外加当前页 = 上限；从最旧的删到只剩上限-1 个非当前页。
  size_t excess = ents.size() - static_cast<size_t>(kMaxCachedPages - 1);
  for (size_t i = 0; i < excess; i++) Storage.remove(ents[i].path.c_str());
}

// 流式把 DCT 字节拷到临时 jpg：大页 JPEG 有几 MB，不整块进（内部 RAM）。──
bool extractDctToFile(PdfDocument &doc, const PdfStreamInfo &si, const std::string &tmpPath,
                      std::string *err) {
  HalFile out;
  if (!Storage.openFileForWrite(TAG, tmpPath, out)) {
    if (err) *err = "无法写临时文件";
    return false;
  }
  const PdfSource *src = doc.source();
  // 8 KB 缓冲不能放主任务栈（16 KB），走 PSRAM；主机端退回普通堆。
  uint8_t *buf = static_cast<uint8_t *>(pdfscratch::alloc(8192));
  if (!buf) {
    out.close();
    Storage.remove(tmpPath.c_str());
    if (err) *err = "内存不足";
    return false;
  }
  uint64_t done = 0;
  bool ok = true;
  while (done < si.len) {
    const size_t want = static_cast<size_t>(std::min<uint64_t>(si.len - done, 8192));
    const int n = src->readAt(si.off + done, buf, want);
    if (n != static_cast<int>(want)) {
      ok = false;
      break;
    }
    if (out.write(buf, want) != want) {
      ok = false;
      break;
    }
    done += want;
  }
  out.flush();
  out.close();
  pdfscratch::release(buf);
  if (!ok) {
    Storage.remove(tmpPath.c_str());
    if (err) *err = "PDF 图像流读取失败";
  }
  return ok;
}

// Flate 图像逐行写 PXC 文件（先写临时文件，成功后 rename）。───────────────
struct FilePxcSink final : PdfPxcSink {
  HalFile file;
  bool ok = false;
  bool begin(const std::string &path, int w, int h, int version) {
    if (!Storage.openFileForWrite(TAG, path, file)) return false;
    uint8_t hdr[kPdfPxcHeaderBytes];
    hdr[0] = static_cast<uint8_t>(w & 0xFF);
    hdr[1] = static_cast<uint8_t>(w >> 8);
    hdr[2] = static_cast<uint8_t>(h & 0xFF);
    hdr[3] = static_cast<uint8_t>(h >> 8);
    hdr[4] = static_cast<uint8_t>(version);
    if (file.write(hdr, sizeof(hdr)) != sizeof(hdr)) {
      file.close();
      Storage.remove(path.c_str());
      return false;
    }
    ok = true;
    return true;
  }
  bool writeRow(int, const uint8_t *row, int bytes) override {
    if (!ok) return false;
    if (file.write(row, static_cast<size_t>(bytes)) != static_cast<size_t>(bytes)) {
      ok = false;
      return false;
    }
    return true;
  }
  bool finish(const std::string &tmpPath, const std::string &finalPath) {
    file.flush();
    file.close();
    if (!ok) {
      Storage.remove(tmpPath.c_str());
      return false;
    }
    Storage.remove(finalPath.c_str());
    if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
      Storage.remove(tmpPath.c_str());
      return false;
    }
    return true;
  }
};

bool progressYield(void *) {
  static int64_t lastUs = 0;
  const int64_t now = esp_timer_get_time();
  if (lastUs == 0 || now - lastUs >= 250000) {
    lastUs = now;
    vTaskDelay(1);
  }
  return true;
}

bool cacheHeaderMatches(const std::string &path, int w, int h, int version) {
  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) return false;
  PdfFileSource src;
  src.file = &f;
  src.bytes = f.fileSize64();
  PdfPxcHeader hdr;
  PdfError e = PdfError::Ok;
  std::string msg;
  const bool ok = pdfPxcReadHeader(src, &hdr, &e, &msg) && hdr.width == w && hdr.height == h &&
                  hdr.version == version;
  f.close();
  return ok;
}

// 只保证 page 的缓存文件已解码落地（不碰 g_page / ramPage 副本）。
// 调用方负责持有 DocFileScope（文件要开着才能解析页字典）。
// progress 为空时用默认的 progressYield；预解码传自己的节拍回调。
bool ensurePageCache(int page, int *outW, int *outH, int *outRotate, PdfProgressFn progress,
                     std::string *err) {
  const PdfValue *dict = g_doc.pageDict(page);
  if (!dict) {
    if (err) *err = "PDF 页字典缺失";
    return false;
  }
  PdfPageGeom geom;
  PdfError e = PdfError::Ok;
  std::string msg;
  if (!pdfReadPageGeom(*dict, &geom, &e, &msg)) {
    if (err) *err = msg.empty() ? pdfErrorText(e) : msg;
    return false;
  }
  PdfPageImage pi;
  if (!pdfFindPageImage(g_doc, *dict, geom, &pi, &e, &msg)) {
    if (err) *err = msg.empty() ? pdfErrorText(e) : msg;
    return false;
  }
  PdfImageInfo info;
  if (!pdfImageInfo(g_doc, pi.imageDict, &info, &e, &msg)) {
    if (err) *err = msg.empty() ? pdfErrorText(e) : msg;
    return false;
  }

  const int w = info.width;
  const int h = info.height;
  if (w <= 0 || h <= 0 || w > 65535 || h > 65535) {
    if (err) *err = "PDF 页面尺寸非法";
    return false;
  }
  // JBIG2 蒙版页存 1bpp（ver=4），其余存 4bpp（ver=3）。
  const int pxcVersion = info.imageMask ? kPdfPxcVersionMask : kPdfPxcVersion;
  const std::string finalPath = pagePxcPath(page);
  if (!cacheHeaderMatches(finalPath, w, h, pxcVersion)) {
    const std::string tmpPath = g_dir + "/page.tmp";
    Storage.remove(tmpPath.c_str());

    if (info.isDct) {
      char jpgName[32];
      snprintf(jpgName, sizeof(jpgName), "/p%05d.dct.tmp", page);
      const std::string jpgPath = g_dir + jpgName;
      PdfStreamInfo si;
      if (!g_doc.streamInfo(pi.imageDict, &si, &e, &msg) ||
          !extractDctToFile(g_doc, si, jpgPath, &msg)) {
        if (err) *err = msg.empty() ? "PDF 图像抽取失败" : msg;
        return false;
      }
      RenderConfig cfg;
      cfg.x = 0;
      cfg.y = 0;
      cfg.maxWidth = w;
      cfg.maxHeight = h;
      cfg.useExactDimensions = true;
      cfg.maxSourcePixels = PdfLimits::kMaxImagePixels;
      cfg.useGrayscale = true;
      cfg.ditherMode = DitherMode::Ordered;
      cfg.cachePath = tmpPath;
      cfg.output = DecodeOutput::CacheOnly;
      cfg.abortPoll = nullptr;
      JpegToFramebufferConverter jpg;
      const bool ok = jpg.decodeToFramebuffer(jpgPath, g_rd, cfg);
      Storage.remove(jpgPath.c_str());
      if (!ok) {
        if (err) *err = "PDF 页面 JPEG 解码失败";
        return false;
      }
      Storage.remove(finalPath.c_str());
      if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
        Storage.remove(tmpPath.c_str());
        if (err) *err = "PDF 缓存写入失败";
        return false;
      }
    } else {
      FilePxcSink sink;
      if (!sink.begin(tmpPath, w, h, pxcVersion)) {
        if (err) *err = "PDF 缓存写入失败";
        return false;
      }
      if (!pdfDecodeImageToPxc(g_doc, pi.imageDict, sink, &info,
                               progress ? progress : progressYield, nullptr, &e, &msg)) {
        Storage.remove(tmpPath.c_str());
        if (err) *err = msg.empty() ? pdfErrorText(e) : msg;
        return false;
      }
      if (!sink.finish(tmpPath, finalPath)) {
        if (err) *err = "PDF 缓存写入失败";
        return false;
      }
    }
  }

  if (outW) *outW = w;
  if (outH) *outH = h;
  if (outRotate) *outRotate = geom.rotate;
  return true;
}

// 解码当前页到原生 PXC（不旋转）。成功返回时 g_page 填好。──────────────
bool decodePage(int page, std::string *err) {
  ramPageClear();   // 换页：先放弃上一页的副本
  DocFileScope fileScope;
  if (!fileScope.ok) {
    if (err) *err = "无法打开 PDF 文件";
    return false;
  }
  int w = 0, h = 0, rotate = 0;
  if (!ensurePageCache(page, &w, &h, &rotate, progressYield, err)) return false;

  g_page.ready = true;
  g_page.page = page;
  g_page.width = w;
  g_page.height = h;
  g_page.pageRotate = rotate;
  g_page.pxcPath = pagePxcPath(page);
  g_page.cropComputed = false;
  g_page.crop = PdfPxcCropBox{};
  // 小页（1bpp 扫描页）整页读进 PSRAM，后续重绘不再碰 SD；4bpp 页放不下就跳过。
  ramPageLoad(g_page.pxcPath, page);
  return true;
}

bool ensureCrop() {
  if (g_page.cropComputed) return true;
  g_page.cropComputed = true;
  g_page.crop = PdfPxcCropBox{};
  PageSource ps;
  if (!ps.open(g_page.pxcPath, g_page.page)) return false;
  PdfPxcHeader hdr;
  PdfError e = PdfError::Ok;
  std::string msg;
  if (pdfPxcReadHeader(*ps.src, &hdr, &e, &msg)) {
    (void)pdfPxcAutoCrop(*ps.src, hdr, &g_page.crop, &e, &msg);
  }
  return true;
}

}  // namespace

// ── 文档生命周期 ────────────────────────────────────────────────────────
bool pdfioOpen(const std::string &path, int *pageCount, std::string *title,
               std::string *err) {
  auto fail = [&](const std::string &m) {
    if (err) *err = m;
    if (pageCount) *pageCount = 0;
    g_open = false;
    return false;
  };
  pdfioClose();
  if (!Storage.exists(path.c_str())) return fail("文件不存在");
  if (!Storage.openFileForRead(TAG, path, g_file)) return fail("无法打开文件");
  g_src.file = &g_file;
  g_src.bytes = g_file.fileSize64();
  if (g_src.bytes < 8) {
    g_file.close();
    return fail("不是 PDF 文件");
  }
  g_path = path;

  PdfError e = PdfError::Ok;
  std::string msg;
  if (!g_doc.open(g_src, &e, &msg)) {
    g_file.close();
    return fail(msg.empty() ? pdfErrorText(e) : msg);
  }
  if (g_doc.pageCount() < 1) {
    g_doc.close();
    g_file.close();
    return fail("PDF 没有页面");
  }
  if (!attachFingerprint(g_src.bytes)) {
    g_doc.close();
    g_file.close();
    return fail("无法创建 PDF 缓存目录");
  }

  const size_t slash = path.find_last_of('/');
  std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  if (base.size() > 4 && base.compare(base.size() - 4, 4, ".pdf") == 0) base.resize(base.size() - 4);
  else if (base.size() > 4 && base.compare(base.size() - 4, 4, ".PDF") == 0) base.resize(base.size() - 4);
  g_title = base.empty() ? path : base;
  resetPage();
  g_open = true;
  docFileClose();   // 解析完就还句柄；下一次解码再按需打开（见 docFileOpen）
  if (title) *title = g_title;
  if (pageCount) *pageCount = g_doc.pageCount();
  if (err) err->clear();
  return true;
}

void pdfioClose() {
  if (g_open) ESP_LOGI(TAG, "close %s", g_path.c_str());
  g_doc.close();
  if (g_file.isOpen()) g_file.close();
  g_src.file = nullptr;
  g_src.bytes = 0;
  g_open = false;
  g_path.clear();
  g_dir.clear();
  g_title.clear();
  resetPage();
}

bool pdfioPageGeom(int page, int effRotate, PdfViewGeom *out, std::string *err) {
  if (!g_open || !out) {
    if (err) *err = "PDF 未打开";
    return false;
  }
  if (!pdfioEnsurePage(page, effRotate, err)) return false;
  int rot = ((g_page.pageRotate + effRotate) % 360 + 360) % 360;
  const bool swap = (rot == 90 || rot == 270);
  out->pxW = swap ? g_page.height : g_page.width;
  out->pxH = swap ? g_page.width : g_page.height;
  out->cropValid = false;
  out->crop[0] = 0.0f;
  out->crop[1] = 0.0f;
  out->crop[2] = 1.0f;
  out->crop[3] = 1.0f;
  if (ensureCrop() && g_page.crop.valid && g_page.crop.w() > 0 && g_page.crop.h() > 0) {
    out->cropValid = true;
    out->crop[0] = static_cast<float>(g_page.crop.x0) / std::max(1, g_page.width);
    out->crop[1] = static_cast<float>(g_page.crop.y0) / std::max(1, g_page.height);
    out->crop[2] = static_cast<float>(g_page.crop.x1) / std::max(1, g_page.width);
    out->crop[3] = static_cast<float>(g_page.crop.y1) / std::max(1, g_page.height);
  }
  return true;
}

bool pdfioEnsurePage(int page, int effRotate, std::string *err) {
  (void)effRotate;  // 旋转折进 PdfViewport，缓存永远存正向图
  if (!g_open) {
    if (err) *err = "PDF 未打开";
    return false;
  }
  page = std::max(0, std::min(g_doc.pageCount() - 1, page));
  if (g_page.ready && g_page.page == page) return true;
  const uint32_t start = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  const bool ok = decodePage(page, err);
  const uint32_t cost = static_cast<uint32_t>(esp_timer_get_time() / 1000) - start;
  if (!ok) {
    resetPage();
    return false;
  }
  ESP_LOGI(TAG, "page %d decoded in %u ms (%dx%d)", page + 1, cost, g_page.width,
           g_page.height);
  g_page.ready = true;
  g_page.page = page;
  gcOldPages(page);
  return true;
}

// 空闲帧预解码：把 page 的 PXC 缓存先解出来，但不改动「正在看的那一页」。
// 预解码峰值（页面位图 1.26 MB + 符号字典位图 ≈ 0.6 MB）和当前页的整页 PSRAM
// 副本放不进同一份 PSRAM，所以先腾出副本、解完再读回来（≈0.55 s）。
// progress 返回 false 时中止（半成品缓存会被删掉），下次空闲可重试。
bool pdfioPrefetchPage(int page, PdfProgressFn progress) {
  if (!g_open || !g_page.ready) return false;
  if (page < 0 || page >= g_doc.pageCount()) return false;
  if (page == g_page.page) return false;

  ramPageClear();
  const uint32_t start = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  bool ok = false;
  std::string err;
  {
    DocFileScope fileScope;
    if (fileScope.ok) {
      ok = ensurePageCache(page, nullptr, nullptr, nullptr,
                           progress ? progress : progressYield, &err);
    } else {
      err = "无法打开 PDF 文件";
    }
  }
  // 副本装回来：预解码这几秒里若有别的重绘，走的是 SD 按行读。
  if (!g_page.pxcPath.empty()) ramPageLoad(g_page.pxcPath, g_page.page);
  const uint32_t cost = static_cast<uint32_t>(esp_timer_get_time() / 1000) - start;
  ESP_LOGI(TAG, "prefetch page %d %s in %u ms%s%s", page + 1, ok ? "done" : "failed", cost,
           err.empty() ? "" : ": ", err.c_str());
  return ok;
}

bool pdfioPageCached(int page, int effRotate) {
  (void)effRotate;  // 与 ensurePage 一样：缓存只存正向图
  if (!g_open) return false;
  if (g_page.ready && g_page.page == page) return true;
  page = std::max(0, std::min(g_doc.pageCount() - 1, page));
  DocFileScope fileScope;
  if (!fileScope.ok) return false;
  const PdfValue *dict = g_doc.pageDict(page);
  if (!dict) return false;
  PdfPageGeom geom;
  PdfError e = PdfError::Ok;
  std::string msg;
  if (!pdfReadPageGeom(*dict, &geom, &e, &msg)) return false;
  PdfPageImage pi;
  if (!pdfFindPageImage(g_doc, *dict, geom, &pi, &e, &msg)) return false;
  if (!pi.found) return false;
  PdfImageInfo info;
  if (!pdfImageInfo(g_doc, pi.imageDict, &info, &e, &msg)) return false;
  const int ver = info.imageMask ? kPdfPxcVersionMask : kPdfPxcVersion;
  return cacheHeaderMatches(pagePxcPath(page), info.width, info.height, ver);
}

// ── 视口 blit ───────────────────────────────────────────────────────────
bool pdfioBlit(int page, int effRotate, int zoomMode, float zoomMul, float panX, float panY,
               int areaX, int areaY, int areaW, int areaH, std::string *err) {
  if (!g_open) {
    if (err) *err = "PDF 未打开";
    return false;
  }
  if (!g_page.ready || g_page.page != page) {
    if (!pdfioEnsurePage(page, effRotate, err)) return false;
  }

  PageSource ps;
  if (!ps.open(g_page.pxcPath, page)) {
    if (err) *err = "PDF 缓存读取失败";
    return false;
  }
  PdfPxcHeader hdr;
  PdfError e = PdfError::Ok;
  std::string msg;
  if (!pdfPxcReadHeader(*ps.src, &hdr, &e, &msg)) {
    if (err) *err = msg.empty() ? "PDF 缓存损坏" : msg;
    return false;
  }

  const bool cropOn = (st.pdf.cropMode == 1);
  if (cropOn) ensureCrop();

  PdfViewSpec spec;
  spec.srcW = hdr.width;
  spec.srcH = hdr.height;
  // 页面固有 /Rotate 与用户旋转叠加，顺时针。
  spec.rotate = ((g_page.pageRotate + effRotate) % 360 + 360) % 360;
  spec.crop = cropOn && g_page.crop.valid;
  spec.cropBox = g_page.crop;
  spec.zoomMode = zoomMode;
  spec.zoom = zoomMul > 0 ? zoomMul : 1.0;
  spec.areaX = areaX;
  spec.areaY = areaY;
  spec.areaW = areaW;
  spec.areaH = areaH;

  // 先算一次不含 pan 的视图，得到可平移余量，再把归一化 pan 落成像素。
  // panX/panY：0 = 显示内容最左/最上，1 = 最右/最下（与 PdfState 的注释一致）。
  PdfViewMap probe;
  if (!pdfComputeView(spec, &probe, &e, &msg)) {
    if (err) *err = msg.empty() ? pdfErrorText(e) : msg;
    return false;
  }
  const double slackX = std::max(0.0, probe.contentW() - areaW);
  const double slackY = std::max(0.0, probe.contentH() - areaH);
  // 记给输入层：拖动/方向键据此判断这个方向到底有没有可平移的余量，没有就翻页。
  st.pdf.panRoomX = static_cast<int>(slackX + 0.5);
  st.pdf.panRoomY = static_cast<int>(slackY + 0.5);
  const double px = (0.5 - std::clamp(panX, 0.0f, 1.0f)) * slackX;
  const double py = (0.5 - std::clamp(panY, 0.0f, 1.0f)) * slackY;
  spec.panX = px;
  spec.panY = py;

  PdfViewMap map;
  if (!pdfComputeView(spec, &map, &e, &msg)) {
    if (err) *err = msg.empty() ? pdfErrorText(e) : msg;
    return false;
  }

  pdfscratch::ByteVec row(static_cast<size_t>(hdr.rowBytes()));
  const double scale = map.scale;
  const int cw = map.cw;
  const int ch = map.ch;
  const int cx0 = map.cx0;
  const int cy0 = map.cy0;
  const int rot = map.rotate;

  DirectPixelWriter pw;
  pw.init(g_rd);

  auto readRow = [&](int sy) -> const uint8_t * {
    if (!pdfPxcReadRow(*ps.src, hdr, sy, row.data())) return nullptr;
    return row.data();
  };
  const int pxcVersion = hdr.version;

  // 屏幕像素覆盖的源区间 [a,b)（显示坐标），旋转后落到源行/列区间 [i0,i1)。
  auto span = [](double a, double b, bool rev, int n, int *i0, int *i1) {
    if (rev) {
      const double na = n - b;
      b = n - a;
      a = na;
    }
    int s = static_cast<int>(std::floor(a));
    int e = static_cast<int>(std::ceil(b));
    if (s < 0) s = 0;
    if (e > n) e = n;
    if (e < s) e = s;
    *i0 = s;
    *i1 = e;
  };
  // 缩小采样要平均而不是最近邻：4bpp 抖动图缩 3~7 倍时，抽点会丢笔画、出摩尔纹。
  // 每次最多取 4×4 个样本，超大缩小时按步长跳过（抖动统计上仍然成立）。
  auto runCnt = [](int n) {
    if (n <= 4) return 1;
    return (n + 3) / 4;
  };
  auto sampleCnt = [&](int n) {
    const int st = runCnt(n);
    return (n + st - 1) / st;
  };

  const bool revLX = (rot == 180 || rot == 270);
  const bool revLY = (rot == 180 || rot == 90);

  if (rot == 0 || rot == 180) {
    // 外圈 y 定源行带，内圈 x 定源列带。
    pdfscratch::Vec<int> colS(static_cast<size_t>(areaW)), colE(static_cast<size_t>(areaW));
    for (int i = 0; i < areaW; i++) {
      const int x = areaX + i;
      const double a = (x - areaX - map.offX) / scale;
      const double b = (x + 1 - areaX - map.offX) / scale;
      span(a, b, revLX, cw, &colS[i], &colE[i]);
    }
    pdfscratch::Vec<int> acc(static_cast<size_t>(areaW));
    for (int y = areaY; y < areaY + areaH; y++) {
      const double a = (y - areaY - map.offY) / scale;
      const double b = (y + 1 - areaY - map.offY) / scale;
      int r0 = 0, r1 = 0;
      span(a, b, revLY, ch, &r0, &r1);
      if (r0 >= r1) continue;
      std::fill(acc.begin(), acc.end(), 0);
      const int stepR = runCnt(r1 - r0);
      int nr = 0;
      for (int ly = r0; ly < r1; ly += stepR) {
        const uint8_t *r = readRow(cy0 + ly);
        if (!r) break;
        nr++;
        for (int i = 0; i < areaW; i++) {
          const int c0 = colS[i], c1 = colE[i];
          if (c0 >= c1) continue;
          const int stepC = runCnt(c1 - c0);
          int sum = 0;
          for (int lx = c0; lx < c1; lx += stepC) sum += pdfPxcLevelAt(r, cx0 + lx, pxcVersion);
          acc[i] += sum;
        }
      }
      if (nr <= 0) continue;
      pw.beginRow(y);
      for (int i = 0; i < areaW; i++) {
        const int cols = colE[i] - colS[i];
        if (cols <= 0) continue;
        const int n = nr * sampleCnt(cols);
        pw.writeGray16(areaX + i, (acc[i] + n / 2) / n);
      }
    }
  } else {
    // 90/270：外圈 x 定源行带，内圈 y 定源列带。
    pdfscratch::Vec<int> colS(static_cast<size_t>(areaH)), colE(static_cast<size_t>(areaH));
    for (int j = 0; j < areaH; j++) {
      const int y = areaY + j;
      const double a = (y - areaY - map.offY) / scale;
      const double b = (y + 1 - areaY - map.offY) / scale;
      span(a, b, revLX, cw, &colS[j], &colE[j]);
    }
    pdfscratch::Vec<int> acc(static_cast<size_t>(areaH));
    for (int i = 0; i < areaW; i++) {
      const int x = areaX + i;
      const double a = (x - areaX - map.offX) / scale;
      const double b = (x + 1 - areaX - map.offX) / scale;
      int r0 = 0, r1 = 0;
      span(a, b, revLY, ch, &r0, &r1);
      if (r0 >= r1) continue;
      std::fill(acc.begin(), acc.end(), 0);
      const int stepR = runCnt(r1 - r0);
      int nr = 0;
      for (int ly = r0; ly < r1; ly += stepR) {
        const uint8_t *r = readRow(cy0 + ly);
        if (!r) break;
        nr++;
        for (int j = 0; j < areaH; j++) {
          const int c0 = colS[j], c1 = colE[j];
          if (c0 >= c1) continue;
          const int stepC = runCnt(c1 - c0);
          int sum = 0;
          for (int lx = c0; lx < c1; lx += stepC) sum += pdfPxcLevelAt(r, cx0 + lx, pxcVersion);
          acc[j] += sum;
        }
      }
      if (nr <= 0) continue;
      for (int j = 0; j < areaH; j++) {
        const int cols = colE[j] - colS[j];
        if (cols <= 0) continue;
        const int n = nr * sampleCnt(cols);
        pw.beginRow(areaY + j);
        pw.writeGray16(x, (acc[j] + n / 2) / n);
      }
    }
  }

  if (err) err->clear();
  return true;
}

bool pdfioAnchorPan(int page, int effRotate, int zoomMode, float currentZoom, float targetZoom,
                    float panX, float panY, int areaX, int areaY, int areaW, int areaH,
                    int tapX, int tapY, float *outPanX, float *outPanY) {
  if (!g_open || !outPanX || !outPanY) return false;
  if (!g_page.ready || g_page.page != page) {
    if (!pdfioEnsurePage(page, effRotate, nullptr)) return false;
  }
  HalFile f;
  if (!Storage.openFileForRead(TAG, g_page.pxcPath, f)) return false;
  PdfFileSource src;
  src.file = &f;
  src.bytes = f.fileSize64();
  PdfPxcHeader hdr;
  PdfError e = PdfError::Ok;
  std::string msg;
  if (!pdfPxcReadHeader(src, &hdr, &e, &msg)) {
    f.close();
    return false;
  }

  const bool cropOn = (st.pdf.cropMode == 1);
  if (cropOn) ensureCrop();
  const int rot = ((g_page.pageRotate + effRotate) % 360 + 360) % 360;

  auto makeSpec = [&](float zoom) {
    PdfViewSpec s;
    s.srcW = hdr.width;
    s.srcH = hdr.height;
    s.rotate = rot;
    s.crop = cropOn && g_page.crop.valid;
    s.cropBox = g_page.crop;
    s.zoomMode = zoomMode;
    s.zoom = zoom > 0 ? zoom : 1.0;
    s.areaX = areaX;
    s.areaY = areaY;
    s.areaW = areaW;
    s.areaH = areaH;
    return s;
  };

  // 当前视图：按 blit 同一套换算把归一化 pan 落成像素，再反求点下的源像素。
  PdfViewSpec cur = makeSpec(currentZoom);
  PdfViewMap curProbe;
  if (!pdfComputeView(cur, &curProbe, &e, &msg)) {
    f.close();
    return false;
  }
  const double curSlackX = std::max(0.0, curProbe.contentW() - areaW);
  const double curSlackY = std::max(0.0, curProbe.contentH() - areaH);
  cur.panX = (0.5 - std::clamp(panX, 0.0f, 1.0f)) * curSlackX;
  cur.panY = (0.5 - std::clamp(panY, 0.0f, 1.0f)) * curSlackY;

  PdfViewMap curMap;
  if (!pdfComputeView(cur, &curMap, &e, &msg)) {
    f.close();
    return false;
  }
  int sx = 0, sy = 0;
  const bool inside = curMap.screenToSource(tapX, tapY, &sx, &sy);

  PdfViewSpec target = makeSpec(targetZoom);
  PdfViewMap targetMap;
  if (!pdfComputeView(target, &targetMap, &e, &msg)) {
    f.close();
    return false;
  }
  f.close();

  if (!inside) {
    // 点在留白上：没有可锚的像素，放大后居中即可。
    *outPanX = 0.5f;
    *outPanY = 0.5f;
    return true;
  }
  double px = 0, py = 0;
  pdfPanForAnchor(targetMap, tapX, tapY, sx, sy, &px, &py);
  // pdfPanForAnchor 已夹在 ±slack/2，这里换算回归一化 pan（0=最左/上）。
  const double slackX = std::max(0.0, targetMap.contentW() - areaW);
  const double slackY = std::max(0.0, targetMap.contentH() - areaH);
  *outPanX = slackX > 0 ? static_cast<float>(std::clamp(0.5 - px / slackX, 0.0, 1.0)) : 0.5f;
  *outPanY = slackY > 0 ? static_cast<float>(std::clamp(0.5 - py / slackY, 0.0, 1.0)) : 0.5f;
  return true;
}
