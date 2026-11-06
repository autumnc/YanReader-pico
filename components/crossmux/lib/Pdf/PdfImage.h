#pragma once
// ── 页面图像 → PXC ───────────────────────────────────────────────────────
// 支持：
//   DCTDecode（JPEG）：这里只做参数校验与压缩字节抽取（设备端交给现有
//       JpegToFramebufferConverter，主机测试比对原始字节）；
//   FlateDecode（含 PNG/TIFF predictor）：流式解码，逐行转 8 位灰并量化成 4bpp；
//   JBIG2Decode：整条流交给 PdfJbig2 解成 1bpp 位图，再按下面的蒙版规则落地。
//   /ImageMask true（扫描件模板蒙版，多为 JBIG2）：行原样写 1bpp PXC（ver=4，
//       位 1 = 墨），不展开成 4bpp——缓存体积和 SD 读写都省 4 倍。
//   /Decode [1 0] 反相在写 1bpp 时按字节取反（尾部填充位清零）。
// 色彩：DeviceGray / DeviceRGB / ICCBased(N=1,3)；1/2/4/8/16 bpc；
//       /Decode 反相与任意端点。Indexed、CMYK、SMask、JBIG2 全局表报不支持。

#include <cstdint>
#include <string>
#include <vector>

#include "PdfDocument.h"
#include "PdfObject.h"
#include "PdfPxc.h"
#include "PdfTypes.h"

struct PdfImageInfo {
  int width = 0;
  int height = 0;
  int bpc = 8;
  int components = 1;
  bool grayscale = true;
  bool isDct = false;
  bool isJbig2 = false;
  bool imageMask = false;  // /ImageMask true：按蒙版绘制语义量化
};

// 校验图像字典并给出原生尺寸/色彩信息。不分配像素缓冲。
bool pdfImageInfo(PdfDocument &doc, const PdfValue &imageDict, PdfImageInfo *out, PdfError *err,
                  std::string *msg);

// 解码 Flate 图像并逐行写入 sink；DCT 图像返回 Unsupported。
// progress 可为空，返回 false 时中止（错误码 Decode）。
using PdfProgressFn = bool (*)(void *ctx);
bool pdfDecodeImageToPxc(PdfDocument &doc, const PdfValue &imageDict, PdfPxcSink &sink,
                         PdfImageInfo *out, PdfProgressFn progress, void *progressCtx,
                         PdfError *err, std::string *msg);

// DCT：把压缩流原样拷出（主机测试 / 设备端落临时 .jpg 用）。
bool pdfCopyDctStream(PdfDocument &doc, const PdfValue &imageDict, std::vector<uint8_t> *out,
                      PdfError *err, std::string *msg);
