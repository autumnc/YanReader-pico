#pragma once
// ── 最小 PDF 解析器的公共类型与限额 ──────────────────────────────────────
// 只覆盖「页面图像型 PDF」需要的那一小块：经典 xref / xref stream / ObjStm /
// 页面树继承 / 内容流里的 q Q cm Do / DCTDecode 与 FlateDecode 位图。
// 不认识的构造一律返回错误码，解析器里没有 assert/abort，也没有不受限的分配。

#include <cstdint>
#include <string>

enum class PdfError : uint8_t {
  Ok = 0,
  Io,
  Syntax,
  NotFound,
  Unsupported,
  UnsupportedFilter,
  Encrypted,
  TooLarge,
  NoPageImage,
  Decode,
};

inline const char *pdfErrorText(PdfError e) {
  switch (e) {
    case PdfError::Ok: return "OK";
    case PdfError::Io: return "读取失败";
    case PdfError::Syntax: return "PDF 结构损坏";
    case PdfError::NotFound: return "PDF 对象不存在";
    case PdfError::Unsupported: return "PDF 特性不支持";
    case PdfError::UnsupportedFilter: return "PDF 滤镜不支持";
    case PdfError::Encrypted: return "加密 PDF 不支持";
    case PdfError::TooLarge: return "PDF 页面过大";
    case PdfError::NoPageImage: return "此 PDF 不是页面图像型";
    case PdfError::Decode: return "PDF 解码失败";
  }
  return "PDF 错误";
}

struct PdfRef {
  int num = -1;
  int gen = 0;
  bool valid() const { return num >= 0; }
  bool operator==(const PdfRef &o) const { return num == o.num && gen == o.gen; }
};

struct PdfRect {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  double w() const { return x1 - x0; }
  double h() const { return y1 - y0; }
  double area() const { return w() * h(); }
  bool empty() const { return !(x1 > x0 && y1 > y0); }
  void normalize() {
    if (x0 > x1) {
      double t = x0;
      x0 = x1;
      x1 = t;
    }
    if (y0 > y1) {
      double t = y0;
      y0 = y1;
      y1 = t;
    }
  }
};

// 所有上限集中在处：坏文件最先撞到的是这些数，而不是 OOM 或死循环。
namespace PdfLimits {
constexpr int kMaxPages = 5000;
constexpr int kMaxPageTreeDepth = 32;
constexpr int kMaxXrefChain = 32;
constexpr int kMaxObjectCache = 512;
constexpr int kMaxObjStmCacheBytes = 16 * 1024 * 1024;
constexpr int kMaxStreamBytes = 32 * 1024 * 1024;      // 解压后
constexpr int kMaxContentBytes = 4 * 1024 * 1024;      // 单页内容流合计
constexpr int64_t kMaxImagePixels = 16 * 1000 * 1000;  // PDF 专用 16 MP
constexpr int kMaxFormDepth = 4;
constexpr int kMaxContentOps = 2000000;
}  // namespace PdfLimits
