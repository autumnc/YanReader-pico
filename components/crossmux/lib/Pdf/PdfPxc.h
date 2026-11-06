#pragma once
// ── PXC（页缓存）读写与图像分析 ─────────────────────────────────────────
// 文件头：uint16 w、uint16 h、uint8 ver，之后是逐行像素。
//   ver=3：4bpp 16 级，与 PixelCache / DitherUtils 一致；每行 (w+1)/2 字节，
//          偶列低半字节、奇列高半字节，0 = 全墨、15 = 全白。
//   ver=4：1bpp 二值，给 JBIG2 ImageMask 页用（扫描件都是二值，4bpp 存是
//          白占 4 倍空间、4 倍 SD 读写）；每行 (w+7)/8 字节，MSB 在前，1 = 墨。
//
// 旋转**不生成** PXC 变体：旋转折进 PdfViewport 的坐标反变换，源缓存永远正向。

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "PdfSource.h"
#include "PdfTypes.h"

constexpr int kPdfPxcVersion = 3;
constexpr int kPdfPxcVersionMask = 4;
constexpr int kPdfPxcHeaderBytes = 5;

struct PdfPxcHeader {
  int width = 0;
  int height = 0;
  int version = 0;
  bool is1bpp() const { return version == kPdfPxcVersionMask; }
  int rowBytes() const { return is1bpp() ? (width + 7) / 8 : (width + 1) / 2; }
  uint64_t payloadBytes() const { return static_cast<uint64_t>(rowBytes()) * height; }
};

inline int pdfPxcRowBytes(int w) { return (w + 1) / 2; }

inline uint8_t pdfPxcGet(const uint8_t *row, int x) {
  const uint8_t b = row[x >> 1];
  return (x & 1) ? static_cast<uint8_t>(b >> 4) : static_cast<uint8_t>(b & 0x0F);
}

inline uint8_t pdfPxcGetBit(const uint8_t *row, int x) {
  return static_cast<uint8_t>((row[x >> 3] >> (7 - (x & 7))) & 1);
}

// 统一取 0..15 灰度级：1bpp 里位 1 = 墨 = 0，位 0 = 纸 = 15。
inline uint8_t pdfPxcLevelAt(const uint8_t *row, int x, int version) {
  if (version == kPdfPxcVersionMask) return pdfPxcGetBit(row, x) ? 0 : 15;
  return pdfPxcGet(row, x);
}

inline void pdfPxcSet(uint8_t *row, int x, uint8_t level) {
  uint8_t &b = row[x >> 1];
  if (x & 1) {
    b = static_cast<uint8_t>((b & 0x0F) | static_cast<uint8_t>((level & 0x0F) << 4));
  } else {
    b = static_cast<uint8_t>((b & 0xF0) | (level & 0x0F));
  }
}

// 行写入接口：图像解码器按行产出，设备端落文件、主机端落内存。
class PdfPxcSink {
 public:
  virtual ~PdfPxcSink() = default;
  virtual bool writeRow(int y, const uint8_t *row, int bytes) = 0;
};

bool pdfPxcReadHeader(const PdfSource &src, PdfPxcHeader *out, PdfError *err, std::string *msg);
bool pdfPxcReadRow(const PdfSource &src, const PdfPxcHeader &hdr, int y, uint8_t *row);

// 自动裁边：在**源像素**坐标里返回墨迹外接框（已外扩 2px、每边至多 15%）。
struct PdfPxcCropBox {
  bool valid = false;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // [x0,x1) × [y0,y1)
  int w() const { return x1 - x0; }
  int h() const { return y1 - y0; }
};

bool pdfPxcAutoCrop(const PdfSource &src, const PdfPxcHeader &hdr, PdfPxcCropBox *out,
                    PdfError *err, std::string *msg);
