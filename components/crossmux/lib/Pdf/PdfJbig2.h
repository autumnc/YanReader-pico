#pragma once
// ── JBIG2（PDF /JBIG2Decode，嵌入式组织）────────────────────────────────
// 扫描件 PDF 的主图常用 JBIG2 单色压缩。这里只实现这类文件最常见的子集：
//   算术编码（MQ）、模板 0 通用区域、无细化/无 Huffman 的符号字典、
//   文本区域（单条带）、页面信息段。
// 结构、语义与 jbig2dec 0.20 逐位对齐（已在 239 页扫描件上与 jbig2dec 对拍
// 全部一致）。别的构造一律返回错误，不做不了装懂。
//
// 输出 1 bit/像素：stride 字节/行，MSB 在前，1 = 墨。所有大缓冲走 PSRAM。

#include <cstddef>
#include <cstdint>
#include <string>

#include "PdfScratch.h"
#include "PdfTypes.h"

// 返回 false 表示调用方要求中止解码。
using PdfJbig2ProgressFn = bool (*)(void *ctx);

struct PdfJbig2Image {
  int width = 0;
  int height = 0;
  int stride = 0;
  pdfscratch::ByteVec bits;  // height * stride 字节，1 = 墨

  bool empty() const { return width <= 0 || height <= 0; }
  int get(int x, int y) const {
    if (x < 0 || x >= width || y < 0 || y >= height) return 0;
    return (bits[static_cast<size_t>(y) * static_cast<size_t>(stride) + (x >> 3)] >>
            (7 - (x & 7))) &
           1;
  }
  void set(int x, int y, int v) {
    uint8_t &b = bits[static_cast<size_t>(y) * static_cast<size_t>(stride) + (x >> 3)];
    const uint8_t m = static_cast<uint8_t>(1u << (7 - (x & 7)));
    if (v) b = static_cast<uint8_t>(b | m);
    else b = static_cast<uint8_t>(b & static_cast<uint8_t>(~m));
  }
};

// 解码一条 PDF /JBIG2Decode 流（嵌入式段组织，不是 .jb2 文件头）。
// expectedWidth/Height：流里没有页面信息段时用来兜底（<=0 表示未知）。
// progress 可为空，返回 false 时中止（错误码 Decode）。
bool pdfJbig2Decode(const uint8_t *data, size_t len, int expectedWidth, int expectedHeight,
                    PdfJbig2Image *out, PdfJbig2ProgressFn progress, void *ctx, PdfError *err,
                    std::string *msg);
