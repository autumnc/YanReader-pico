#include "PdfImage.h"

#include "PdfJbig2.h"
#include "PdfScratch.h"

#include <cmath>
#include <cstring>

#include "../Epub/Epub/converters/DitherUtils.h"

// PdfScratch.h 末尾会 #undef 这个宏，这里自己再探一次（同 PdfJbig2.cpp）。
#if !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED) && __has_include(<esp_heap_caps.h>)
#include <esp_log.h>
#include <esp_timer.h>
#define PDF_IMAGE_LOG 1
#endif

namespace {

void setErr(PdfError *err, std::string *msg, PdfError e, const char *m) {
  if (err) *err = e;
  if (msg) *msg = m ? m : "";
}

int spaceComponents(PdfDocument &doc, const PdfValue *cs) {
  if (!cs) return 1;  // 缺省按灰度
  if (cs->type == PdfType::Name) {
    if (cs->str == "DeviceGray" || cs->str == "G" || cs->str == "CalGray") return 1;
    if (cs->str == "DeviceRGB" || cs->str == "RGB" || cs->str == "CalRGB") return 3;
    return -1;
  }
  if (cs->isArray() && cs->arr.size() >= 2 && cs->arr[0].isName("ICCBased")) {
    const PdfValue &ref = cs->arr[1];
    if (ref.type != PdfType::Ref) return -1;
    const PdfValue *icc = doc.object(ref.ref.num, nullptr, nullptr);
    if (!icc || !icc->isDict()) return -1;
    const PdfValue *n = icc->get("N");
    if (!n) return -1;
    const int v = n->asInt(0);
    if (v == 1 || v == 3) return v;
    return -1;
  }
  return -1;
}

uint32_t readSample(const uint8_t *row, size_t idx, int bpc) {
  if (bpc == 8) return row[idx];
  if (bpc == 16) return (static_cast<uint32_t>(row[idx * 2]) << 8) | row[idx * 2 + 1];
  const size_t bit = idx * static_cast<size_t>(bpc);
  const size_t byte = bit >> 3;
  const int shift = 8 - bpc - static_cast<int>(bit & 7);
  return (row[byte] >> shift) & ((1u << bpc) - 1u);
}

uint8_t decodeTo8(uint32_t v, int bpc, double dmin, double dmax) {
  const double maxV = static_cast<double>((1u << bpc) - 1u);
  double t = maxV > 0 ? static_cast<double>(v) / maxV : 0;
  double out = (dmin + t * (dmax - dmin)) * 255.0;
  if (out < 0) out = 0;
  if (out > 255) out = 255;
  return static_cast<uint8_t>(out + 0.5);
}

}  // namespace

bool pdfImageInfo(PdfDocument &doc, const PdfValue &imageDict, PdfImageInfo *out, PdfError *err,
                  std::string *msg) {
  if (!imageDict.isDict() || !imageDict.hasStream) {
    setErr(err, msg, PdfError::Syntax, "PDF 图像对象非法");
    return false;
  }
  const PdfValue *sub = imageDict.get("Subtype");
  if (!sub || !sub->isName("Image")) {
    setErr(err, msg, PdfError::Syntax, "PDF 对象不是图像 XObject");
    return false;
  }
  const PdfValue *im = imageDict.get("ImageMask");
  const bool imageMask = im && im->type == PdfType::Bool && im->boolean;
  const PdfValue *sm = imageDict.get("SMask");
  const PdfValue *mk = imageDict.get("Mask");
  if ((sm && !sm->isNull()) || (mk && !mk->isNull())) {
    setErr(err, msg, PdfError::Unsupported, "PDF 图像带蒙版不支持");
    return false;
  }

  const PdfValue *wv = imageDict.get("Width");
  const PdfValue *hv = imageDict.get("Height");
  const int w = wv ? wv->asInt(0) : 0;
  const int h = hv ? hv->asInt(0) : 0;
  if (w <= 0 || h <= 0 || w > 65535 || h > 65535) {
    setErr(err, msg, PdfError::TooLarge, "PDF 图像尺寸非法");
    return false;
  }
  if (static_cast<int64_t>(w) * h > PdfLimits::kMaxImagePixels) {
    setErr(err, msg, PdfError::TooLarge, "PDF 图像像素过多");
    return false;
  }

  PdfStreamInfo si;
  if (!doc.streamInfo(imageDict, &si, err, msg)) return false;

  bool dct = false;
  bool flate = false;
  bool jbig2 = false;
  if (si.filters.empty()) {
    flate = true;  // 未压缩位图
  } else if (si.filters.size() == 1) {
    if (si.filters[0].isDct()) dct = true;
    else if (si.filters[0].isFlate()) flate = true;
    else if (si.filters[0].isJbig2()) jbig2 = true;
    else {
      setErr(err, msg, PdfError::UnsupportedFilter, "PDF 图像滤镜不支持");
      return false;
    }
  } else {
    setErr(err, msg, PdfError::UnsupportedFilter, "PDF 图像多级滤镜不支持");
    return false;
  }
  if (jbig2 && si.filters[0].parms.isDict() && si.filters[0].parms.get("JBIG2Globals")) {
    setErr(err, msg, PdfError::Unsupported, "PDF JBIG2 全局符号表不支持");
    return false;
  }

  // 规范：ImageMask 的 BitsPerComponent 缺省为 1；普通图像缺省 8。
  int bpc = imageMask ? 1 : 8;
  if (const PdfValue *b = imageDict.get("BitsPerComponent")) {
    bpc = b->asInt(bpc);
  }
  if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) {
    setErr(err, msg, PdfError::Unsupported, "PDF 图像位深不支持");
    return false;
  }
  if ((jbig2 || imageMask) && bpc != 1) {
    setErr(err, msg, PdfError::Unsupported, "PDF 单色图像位深不支持");
    return false;
  }

  int comps = 1;
  if (dct) {
    comps = 1;  // DCT 的实际分量由 JPEG 头决定
  } else if (imageMask || jbig2) {
    comps = 1;  // 蒙版没有色彩空间；JBIG2 这里只支持单色
  } else {
    comps = spaceComponents(doc, imageDict.get("ColorSpace"));
    if (comps != 1 && comps != 3) {
      setErr(err, msg, PdfError::Unsupported, "PDF 图像色彩空间不支持");
      return false;
    }
  }

  out->width = w;
  out->height = h;
  out->bpc = bpc;
  out->components = comps;
  out->grayscale = (comps == 1);
  out->isDct = dct;
  out->isJbig2 = jbig2;
  out->imageMask = imageMask;
  (void)flate;
  return true;
}

bool pdfCopyDctStream(PdfDocument &doc, const PdfValue &imageDict, std::vector<uint8_t> *out,
                      PdfError *err, std::string *msg) {
  PdfStreamInfo si;
  if (!doc.streamInfo(imageDict, &si, err, msg)) return false;
  if (!si.hasDct()) {
    setErr(err, msg, PdfError::Unsupported, "PDF 图像不是 DCT 流");
    return false;
  }
  const PdfSource *src = doc.source();
  if (!src) {
    setErr(err, msg, PdfError::Io, "PDF 未打开");
    return false;
  }
  out->resize(static_cast<size_t>(si.len));
  if (si.len == 0) return true;
  if (src->readAt(si.off, out->data(), out->size()) != static_cast<int>(out->size())) {
    setErr(err, msg, PdfError::Io, "PDF 图像流读取失败");
    return false;
  }
  return true;
}

namespace {

// JBIG2：整条流解成 1bpp 位图后逐行量化到 PXC。
// 位图的 1 = 墨。普通单色图按 /Decode 走通用灰度映射；ImageMask 的绘制语义
// 与 jbig2dec/poppler 的渲染结果一致：默认 Decode [0 1] 时位 1 就是墨。
bool decodeJbig2ToPxc(PdfDocument &doc, const PdfStreamInfo &si, const PdfImageInfo &info,
                      const PdfValue &imageDict, PdfPxcSink &sink, PdfProgressFn progress,
                      void *progressCtx, PdfError *err, std::string *msg) {
  const PdfSource *src = doc.source();
  if (!src) {
    setErr(err, msg, PdfError::Io, "PDF 未打开");
    return false;
  }
  if (si.len > static_cast<uint64_t>(PdfLimits::kMaxStreamBytes)) {
    setErr(err, msg, PdfError::TooLarge, "PDF 图像数据过大");
    return false;
  }
  pdfscratch::ByteVec raw(static_cast<size_t>(si.len));
  if (si.len != 0 &&
      src->readAt(si.off, raw.data(), raw.size()) != static_cast<int>(raw.size())) {
    setErr(err, msg, PdfError::Io, "PDF 图像流读取失败");
    return false;
  }

  PdfJbig2Image img;
#if defined(PDF_IMAGE_LOG)
  const int64_t tStart = esp_timer_get_time();
#endif
  if (!pdfJbig2Decode(raw.data(), raw.size(), info.width, info.height, &img, progress,
                      progressCtx, err, msg)) {
    return false;
  }
#if defined(PDF_IMAGE_LOG)
  const int64_t tDecoded = esp_timer_get_time();
#endif
  pdfscratch::releaseVec(raw);  // shrink_to_fit() 在 -fno-exceptions 下不释放

  if (img.width != info.width || img.height != info.height) {
    setErr(err, msg, PdfError::Decode, "PDF JBIG2 页面尺寸不一致");
    return false;
  }

  double d0 = 0, d1 = 1;
  if (const PdfValue *dec = imageDict.get("Decode")) {
    if (dec->isArray() && dec->arr.size() >= 2) {
      d0 = dec->arr[0].asNum(0);
      d1 = dec->arr[1].asNum(1);
    }
  }
  // 蒙版显式 /Decode [1 0]（少见）时反相；默认 [0 1] 保持位 1 = 墨。
  const bool maskInvert = info.imageMask && d0 > d1;

  // ImageMask 页（扫描件）本来就是二值：直接落 1bpp 行（ver=4），存储和
  // SD 读写都省 4 倍。非蒙版页仍走抖动 → 4bpp（ver=3）。
  const bool oneBpp = info.imageMask;
  const int outRowBytes = oneBpp ? (info.width + 7) / 8 : pdfPxcRowBytes(info.width);
  // 末尾多留 4 字节：反相时按整字节取反，尾部填充位单独清。
  pdfscratch::ByteVec packed(static_cast<size_t>(outRowBytes) + 4);
  DitherRowState st;
  const int step = info.height > 64 ? info.height / 16 : 1;
#if defined(PDF_IMAGE_LOG)
  int64_t convUs = 0;
  int64_t writeUs = 0;
#endif
  for (int y = 0; y < info.height; y++) {
    const uint8_t *srcRow = img.bits.data() + static_cast<size_t>(y) * static_cast<size_t>(img.stride);
#if defined(PDF_IMAGE_LOG)
    const int64_t tc0 = esp_timer_get_time();
#endif
    if (oneBpp) {
      if (!maskInvert) {
        memcpy(packed.data(), srcRow, static_cast<size_t>(outRowBytes));
      } else {
        for (int i = 0; i < outRowBytes; i++) packed[i] = static_cast<uint8_t>(~srcRow[i]);
        const int rem = info.width & 7;
        if (rem) {
          // 页宽之外的填充位不能当成墨。
          packed[outRowBytes - 1] &= static_cast<uint8_t>(0xFFu << (8 - rem));
        }
      }
    } else {
      memset(packed.data(), 0, static_cast<size_t>(outRowBytes));
      for (int x = 0; x < info.width; x++) {
        const bool bit = ((srcRow[x >> 3] >> (7 - (x & 7))) & 1) != 0;
        const uint8_t gray = decodeTo8(bit ? 1u : 0u, 1, d0, d1);
        pdfPxcSet(packed.data(), x, grayToLevel16(gray, x, y, DitherMode::Ordered, st));
      }
    }
#if defined(PDF_IMAGE_LOG)
    convUs += esp_timer_get_time() - tc0;
    const int64_t tw0 = esp_timer_get_time();
#endif
    if (!sink.writeRow(y, packed.data(), outRowBytes)) {
      setErr(err, msg, PdfError::Io, "PDF 缓存写入失败");
      return false;
    }
#if defined(PDF_IMAGE_LOG)
    writeUs += esp_timer_get_time() - tw0;
#endif
    if (progress && (y % step) == 0 && !progress(progressCtx)) {
      setErr(err, msg, PdfError::Decode, "PDF 解码已取消");
      return false;
    }
  }
#if defined(PDF_IMAGE_LOG)
  ESP_LOGI("PdfImage", "jbig2 %dx%d %d ms, conv %d ms, write %d ms", info.width, info.height,
           static_cast<int>((tDecoded - tStart) / 1000),
           static_cast<int>(convUs / 1000), static_cast<int>(writeUs / 1000));
#endif
  return true;
}

}  // namespace

bool pdfDecodeImageToPxc(PdfDocument &doc, const PdfValue &imageDict, PdfPxcSink &sink,
                         PdfImageInfo *out, PdfProgressFn progress, void *progressCtx,
                         PdfError *err, std::string *msg) {
  PdfImageInfo info;
  if (!pdfImageInfo(doc, imageDict, &info, err, msg)) return false;
  if (info.isDct) {
    setErr(err, msg, PdfError::Unsupported, "DCT 图像由 JPEG 解码器处理");
    return false;
  }
  if (!out) out = &info;

  PdfStreamInfo si;
  if (!doc.streamInfo(imageDict, &si, err, msg)) return false;

  if (info.isJbig2) {
    if (!decodeJbig2ToPxc(doc, si, info, imageDict, sink, progress, progressCtx, err, msg)) {
      return false;
    }
    if (out) *out = info;
    return true;
  }

  // predictor 参数缺 Columns 时用图像宽度补齐；Colors 从色彩空间推导。
  std::vector<PdfFilterSpec> filters = si.filters;
  for (auto &f : filters) {
    if (!f.isFlate()) continue;
    if (!f.parms.isDict()) f.parms = PdfValue{};
    PdfValue *parms = &f.parms;
    const PdfValue *pred = parms->get("Predictor");
    const int predKind = pred ? pred->asInt(1) : 1;
    if (predKind > 1) {
      auto setInt = [&](const char *k, int v) {
        if (parms->get(k)) return;
        PdfValue x;
        x.type = PdfType::Int;
        x.num = v;
        parms->set(k, std::move(x));
      };
      setInt("Columns", info.width);
      setInt("Colors", info.components);
      setInt("BitsPerComponent", info.bpc);
    }
  }

  const size_t bitsPerPixel = static_cast<size_t>(info.components) * static_cast<size_t>(info.bpc);
  const size_t rowBytes =
      (static_cast<size_t>(info.width) * bitsPerPixel + 7) / 8;
  const uint64_t total = static_cast<uint64_t>(rowBytes) * static_cast<uint64_t>(info.height);
  if (total > static_cast<uint64_t>(PdfLimits::kMaxStreamBytes)) {
    setErr(err, msg, PdfError::TooLarge, "PDF 图像数据过大");
    return false;
  }

  const double dmin[3] = {0, 0, 0};
  const double dmax[3] = {1, 1, 1};
  double d0[3] = {0, 0, 0};
  double d1[3] = {1, 1, 1};
  if (const PdfValue *dec = imageDict.get("Decode")) {
    if (dec->isArray() && dec->arr.size() >= static_cast<size_t>(info.components) * 2) {
      for (int i = 0; i < info.components; i++) {
        d0[i] = dec->arr[static_cast<size_t>(i) * 2].asNum(0);
        d1[i] = dec->arr[static_cast<size_t>(i) * 2 + 1].asNum(1);
      }
    }
  }
  (void)dmin;
  (void)dmax;

  PdfFilteredReader reader;
  if (!reader.init(*doc.source(), si.off, si.len, filters, total + rowBytes, err, msg)) {
    return false;
  }

  // 行缓冲可到几 KB（宽页 RGB 16bpc），走 PSRAM。
  pdfscratch::ByteVec raw(rowBytes);
  pdfscratch::ByteVec packed(static_cast<size_t>(pdfPxcRowBytes(info.width)));
  DitherRowState st;
  const int step = info.height > 64 ? info.height / 16 : 1;
  for (int y = 0; y < info.height; y++) {
    size_t got = 0;
    while (got < rowBytes) {
      const int n = reader.read(raw.data() + got, rowBytes - got);
      if (n < 0) {
        setErr(err, msg, PdfError::Decode, "PDF 图像解码失败");
        return false;
      }
      if (n == 0) {
        setErr(err, msg, PdfError::Decode, "PDF 图像数据不足");
        return false;
      }
      got += static_cast<size_t>(n);
    }
    memset(packed.data(), 0, packed.size());
    for (int x = 0; x < info.width; x++) {
      uint8_t gray;
      const size_t base = static_cast<size_t>(x) * static_cast<size_t>(info.components);
      if (info.components == 1) {
        gray = decodeTo8(readSample(raw.data(), base, info.bpc), info.bpc, d0[0], d1[0]);
      } else {
        const uint8_t r = decodeTo8(readSample(raw.data(), base, info.bpc), info.bpc, d0[0], d1[0]);
        const uint8_t g = decodeTo8(readSample(raw.data(), base + 1, info.bpc), info.bpc, d0[1], d1[1]);
        const uint8_t b = decodeTo8(readSample(raw.data(), base + 2, info.bpc), info.bpc, d0[2], d1[2]);
        gray = static_cast<uint8_t>((77u * r + 150u * g + 29u * b) >> 8);
      }
      pdfPxcSet(packed.data(), x, grayToLevel16(gray, x, y, DitherMode::Ordered, st));
    }
    if (!sink.writeRow(y, packed.data(), static_cast<int>(packed.size()))) {
      setErr(err, msg, PdfError::Io, "PDF 缓存写入失败");
      return false;
    }
    if (progress && (y % step) == 0 && !progress(progressCtx)) {
      setErr(err, msg, PdfError::Decode, "PDF 解码已取消");
      return false;
    }
  }
  if (out) *out = info;
  return true;
}
