#include "PdfPxc.h"

#include <algorithm>

#include "PdfScratch.h"

namespace {

void setErr(PdfError *err, std::string *msg, PdfError e, const char *m) {
  if (err) *err = e;
  if (msg) *msg = m ? m : "";
}

uint16_t le16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

}  // namespace

bool pdfPxcReadHeader(const PdfSource &src, PdfPxcHeader *out, PdfError *err, std::string *msg) {
  uint8_t h[kPdfPxcHeaderBytes];
  if (src.size() < static_cast<uint64_t>(kPdfPxcHeaderBytes) ||
      src.readAt(0, h, sizeof(h)) != static_cast<int>(sizeof(h))) {
    setErr(err, msg, PdfError::Io, "PDF 缓存头读取失败");
    return false;
  }
  out->width = le16(h);
  out->height = le16(h + 2);
  out->version = h[4];
  if ((out->version != kPdfPxcVersion && out->version != kPdfPxcVersionMask) || out->width <= 0 ||
      out->height <= 0) {
    setErr(err, msg, PdfError::Decode, "PDF 缓存格式不匹配");
    return false;
  }
  const uint64_t need = static_cast<uint64_t>(kPdfPxcHeaderBytes) + out->payloadBytes();
  if (src.size() < need) {
    setErr(err, msg, PdfError::Io, "PDF 缓存文件不完整");
    return false;
  }
  return true;
}

bool pdfPxcReadRow(const PdfSource &src, const PdfPxcHeader &hdr, int y, uint8_t *row) {
  if (y < 0 || y >= hdr.height) return false;
  const uint64_t off =
      static_cast<uint64_t>(kPdfPxcHeaderBytes) + static_cast<uint64_t>(y) * hdr.rowBytes();
  return src.readAt(off, row, static_cast<size_t>(hdr.rowBytes())) == hdr.rowBytes();
}

bool pdfPxcAutoCrop(const PdfSource &src, const PdfPxcHeader &hdr, PdfPxcCropBox *out,
                    PdfError *err, std::string *msg) {
  out->valid = false;
  const int w = hdr.width;
  const int h = hdr.height;
  if (w < 8 || h < 8) {
    setErr(err, msg, PdfError::TooLarge, "PDF 图像过小");
    return false;
  }
  const int rb = hdr.rowBytes();
  pdfscratch::ByteVec row(static_cast<size_t>(rb));

  // ── 纸白估计：四边采样做 16 级直方图，取中位数 ──────────────────────────
  uint32_t hist[16] = {0};
  auto sampleRow = [&](int y) {
    if (!pdfPxcReadRow(src, hdr, y, row.data())) return false;
    for (int x = 0; x < w; x++) hist[pdfPxcLevelAt(row.data(), x, hdr.version)]++;
    return true;
  };
  const int edgeRows = h < 8 ? h : 3;
  for (int i = 0; i < edgeRows; i++) {
    if (!sampleRow(i)) {
      setErr(err, msg, PdfError::Io, "PDF 缓存读取失败");
      return false;
    }
    if (!sampleRow(h - 1 - i)) {
      setErr(err, msg, PdfError::Io, "PDF 缓存读取失败");
      return false;
    }
  }
  for (int y = 0; y < h; y += 16) {
    if (!pdfPxcReadRow(src, hdr, y, row.data())) {
      setErr(err, msg, PdfError::Io, "PDF 缓存读取失败");
      return false;
    }
    const int n = w < 4 ? w : 4;
    for (int x = 0; x < n; x++) {
      hist[pdfPxcLevelAt(row.data(), x, hdr.version)]++;
      hist[pdfPxcLevelAt(row.data(), w - 1 - x, hdr.version)]++;
    }
  }
  uint64_t total = 0;
  for (uint32_t v : hist) total += v;
  uint64_t acc = 0;
  int paper = 15;
  if (total) {
    for (int i = 0; i < 16; i++) {
      acc += hist[i];
      if (acc * 2 >= total) {
        paper = i;
        break;
      }
    }
  }
  const int inkLevel = paper > 2 ? paper - 2 : 1;

  // ── 全页统计：每行/每列的墨迹像素数 ─────────────────────────────────────
  pdfscratch::Vec<uint32_t> colCount(static_cast<size_t>(w), 0);
  int firstRow = -1, lastRow = -1;
  const int rowThr = std::max(2, w / 200);
  for (int y = 0; y < h; y++) {
    if (!pdfPxcReadRow(src, hdr, y, row.data())) {
      setErr(err, msg, PdfError::Io, "PDF 缓存读取失败");
      return false;
    }
    int cnt = 0;
    for (int x = 0; x < w; x++) {
      if (pdfPxcLevelAt(row.data(), x, hdr.version) <= inkLevel) {
        cnt++;
        colCount[static_cast<size_t>(x)]++;
      }
    }
    if (cnt >= rowThr) {
      if (firstRow < 0) firstRow = y;
      lastRow = y;
    }
  }
  if (firstRow < 0) {
    setErr(err, msg, PdfError::Decode, "PDF 页面疑似空白");
    return false;
  }
  const int colThr = std::max(2, h / 200);
  int firstCol = -1, lastCol = -1;
  for (int x = 0; x < w; x++) {
    if (colCount[static_cast<size_t>(x)] >= static_cast<uint32_t>(colThr)) {
      if (firstCol < 0) firstCol = x;
      lastCol = x;
    }
  }
  if (firstCol < 0) {
    setErr(err, msg, PdfError::Decode, "PDF 页面疑似空白");
    return false;
  }

  int x0 = firstCol - 2;
  int y0 = firstRow - 2;
  int x1 = lastCol + 3;
  int y1 = lastRow + 3;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > w) x1 = w;
  if (y1 > h) y1 = h;

  // 每边最多裁 15%，防止把正文当边切掉。
  const int mx = w * 15 / 100;
  const int my = h * 15 / 100;
  if (x0 > mx) x0 = mx;
  if (y0 > my) y0 = my;
  if (w - x1 > mx) x1 = w - mx;
  if (h - y1 > my) y1 = h - my;

  const double area = static_cast<double>(x1 - x0) * (y1 - y0);
  const double full = static_cast<double>(w) * h;
  if (area < full * 0.20 || area > full * 0.98) {
    return true;  // 不值得裁：保持整页
  }
  out->valid = true;
  out->x0 = x0;
  out->y0 = y0;
  out->x1 = x1;
  out->y1 = y1;
  return true;
}
