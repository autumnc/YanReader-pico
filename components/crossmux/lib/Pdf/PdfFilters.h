#pragma once
// ── 流滤镜 ────────────────────────────────────────────────────────────────
// 这里只解 FlateDecode（含 PNG/TIFF predictor）。DCTDecode 由 PdfImage 把
// 压缩字节原样抽给 JPEG 解码器；JBIG2Decode 由 PdfJbig2 解码。任何别的
// 滤镜都明确报 UnsupportedFilter，绝不悄悄跳过。

#include <cstdint>
#include <string>
#include <vector>

#include "PdfObject.h"
#include "PdfScratch.h"
#include "PdfSource.h"
#include "PdfTypes.h"

#include <InflateStream.h>

struct PdfFilterSpec {
  std::string name;
  PdfValue parms;  // DecodeParms 对应项；没有则为 Null
  bool isFlate() const { return name == "FlateDecode" || name == "Fl"; }
  bool isDct() const { return name == "DCTDecode" || name == "DCT"; }
  bool isJbig2() const { return name == "JBIG2Decode" || name == "JBIG2"; }
};

// 从流字典的 /Filter 与 /DecodeParms 解析滤镜链。两者都可能是单个值或数组；
// DecodeParms 允许少于 Filter 的项数（缺的按空处理）。
bool pdfParseFilters(const PdfValue &streamDict, std::vector<PdfFilterSpec> *out, PdfError *err,
                     std::string *msg);

// 顺序读取解码后的流字节。支持无滤镜与单一 FlateDecode（+/Predictor）。
// 构造后调用 init()；read() 返回实际字节数，0 = 正常结束，-1 = 出错。
class PdfFilteredReader {
 public:
  PdfFilteredReader() = default;
  ~PdfFilteredReader();
  PdfFilteredReader(const PdfFilteredReader &) = delete;
  PdfFilteredReader &operator=(const PdfFilteredReader &) = delete;

  // expectedOut 用于总量上限校验（0 = 不限，调用方仍应给 PdfLimits 上限）。
  bool init(const PdfSource &src, uint64_t off, uint64_t len,
            const std::vector<PdfFilterSpec> &filters, uint64_t expectedOut, PdfError *err,
            std::string *msg);
  int read(uint8_t *dst, size_t len);
  // 已交付的字节数。
  uint64_t produced() const { return produced_; }
  bool failed() const { return failed_; }
  void deinit();

 private:
  static size_t fillCb(void *ctx, const uint8_t **data);
  size_t fill();
  // 读原始（未做 predictor）字节；0 = 流结束，-1 = 错误。
  int readRaw(uint8_t *dst, size_t len);
  // 读满 predictor 要求的原始一行；返回 1 = 好，0 = 流结束，-1 = 错误。
  int readRawRow();
  bool applyPredictor();

  const PdfSource *src_ = nullptr;
  uint64_t srcOff_ = 0;
  uint64_t srcLen_ = 0;
  uint64_t srcPos_ = 0;
  uint64_t limit_ = 0;
  uint64_t produced_ = 0;

  InflateStream inflate_;
  bool inflateReady_ = false;
  bool noFilter_ = false;
  bool failed_ = false;
  bool eof_ = false;
  // 喂给解压器的输入块也走 PSRAM：这个对象常常整个坐在 16 KB 的主任务栈上。
  pdfscratch::ByteVec fillBuf_;

  // predictor 状态
  bool predictor_ = false;
  int predictorKind_ = 0;  // 2 = TIFF，10..15 = PNG
  int colors_ = 1;
  int bpc_ = 8;
  int columns_ = 0;
  int bpp_ = 1;
  size_t rowBytes_ = 0;
  // 解压行缓冲（宽页 RGB/16bpc 可到几十 KB），走 PSRAM。
  pdfscratch::ByteVec prevRow_;
  pdfscratch::ByteVec rawRow_;  // 含 PNG 的行首 filter 字节
  pdfscratch::ByteVec row_;     // 解码后的一行
  size_t rowPos_ = 0;
  bool havePrev_ = false;
  bool rowValid_ = false;
};
