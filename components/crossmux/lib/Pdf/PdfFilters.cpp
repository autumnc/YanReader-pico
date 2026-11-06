#include "PdfFilters.h"

#include <cstring>

namespace {
void setErr(PdfError *err, std::string *msg, PdfError e, const char *m) {
  if (err) *err = e;
  if (msg) *msg = m ? m : "";
}
}  // namespace

bool pdfParseFilters(const PdfValue &streamDict, std::vector<PdfFilterSpec> *out, PdfError *err,
                     std::string *msg) {
  out->clear();
  const PdfValue *f = streamDict.get("Filter");
  if (!f || f->isNull()) return true;

  std::vector<std::string> names;
  if (f->type == PdfType::Name) {
    names.push_back(f->str);
  } else if (f->isArray()) {
    for (const auto &v : f->arr) {
      if (v.type != PdfType::Name) {
        setErr(err, msg, PdfError::Syntax, "PDF /Filter 数组里有非名字项");
        return false;
      }
      names.push_back(v.str);
    }
  } else {
    setErr(err, msg, PdfError::Syntax, "PDF /Filter 类型错误");
    return false;
  }

  const PdfValue *dp = streamDict.get("DecodeParms");
  if (!dp || dp->isNull()) dp = streamDict.get("DP");
  for (size_t i = 0; i < names.size(); i++) {
    PdfFilterSpec spec;
    spec.name = names[i];
    if (dp) {
      if (dp->isArray()) {
        if (i < dp->arr.size() && dp->arr[i].isDict()) spec.parms = dp->arr[i];
      } else if (dp->isDict()) {
        spec.parms = *dp;
      }
    }
    out->push_back(std::move(spec));
  }
  return true;
}

PdfFilteredReader::~PdfFilteredReader() { deinit(); }

void PdfFilteredReader::deinit() {
  inflate_.deinit();
  inflateReady_ = false;
  // -fno-exceptions 下 shrink_to_fit() 是空操作，必须 swap 空向量才真正释放。
  pdfscratch::releaseVec(prevRow_);
  pdfscratch::releaseVec(rawRow_);
  pdfscratch::releaseVec(row_);
  pdfscratch::releaseVec(fillBuf_);
}

bool PdfFilteredReader::init(const PdfSource &src, uint64_t off, uint64_t len,
                             const std::vector<PdfFilterSpec> &filters, uint64_t expectedOut,
                             PdfError *err, std::string *msg) {
  deinit();
  src_ = &src;
  srcOff_ = off;
  srcLen_ = len;
  srcPos_ = 0;
  limit_ = expectedOut;
  produced_ = 0;
  failed_ = false;
  eof_ = false;
  rowValid_ = false;
  rowPos_ = 0;
  havePrev_ = false;
  predictor_ = false;
  noFilter_ = filters.empty();

  if (off > src.size() || len > src.size() - off) {
    setErr(err, msg, PdfError::Io, "PDF 流超出文件范围");
    return false;
  }

  const PdfFilterSpec *flate = nullptr;
  for (const auto &f : filters) {
    if (f.isDct()) {
      setErr(err, msg, PdfError::Unsupported, "DCT 流由图像层处理");
      return false;
    }
    if (!f.isFlate()) {
      setErr(err, msg, PdfError::UnsupportedFilter, "PDF 滤镜不支持");
      return false;
    }
    if (flate) {
      setErr(err, msg, PdfError::UnsupportedFilter, "PDF 多级滤镜不支持");
      return false;
    }
    flate = &f;
  }

  if (flate) {
    fillBuf_.assign(2048, 0);
    if (!inflate_.init(true)) {
      setErr(err, msg, PdfError::Decode, "PDF 解压器内存不足");
      return false;
    }
    inflateReady_ = true;
    inflate_.setFill(&PdfFilteredReader::fillCb, this);
    inflate_.setZlibWrapped();

    int pred = 1;
    if (flate->parms.isDict()) {
      const PdfValue *p = flate->parms.get("Predictor");
      if (p && (p->type == PdfType::Int || p->type == PdfType::Real)) pred = p->asInt(1);
      const PdfValue *c = flate->parms.get("Colors");
      if (c) colors_ = c->asInt(1);
      const PdfValue *b = flate->parms.get("BitsPerComponent");
      if (b) bpc_ = b->asInt(8);
      const PdfValue *n = flate->parms.get("Columns");
      if (n) columns_ = n->asInt(0);
    }
    if (pred != 1) {
      if (pred < 2 || (pred > 2 && pred < 10) || pred > 15) {
        setErr(err, msg, PdfError::UnsupportedFilter, "PDF predictor 不支持");
        return false;
      }
      if (columns_ <= 0 || columns_ > 1000000 || colors_ <= 0 || colors_ > 64 ||
          (bpc_ != 1 && bpc_ != 2 && bpc_ != 4 && bpc_ != 8 && bpc_ != 16)) {
        setErr(err, msg, PdfError::Decode, "PDF predictor 参数非法");
        return false;
      }
      predictor_ = true;
      predictorKind_ = pred;
      const int bitsPerPixel = colors_ * bpc_;
      bpp_ = bitsPerPixel < 8 ? 1 : bitsPerPixel / 8;
      rowBytes_ = (static_cast<size_t>(columns_) * static_cast<size_t>(bitsPerPixel) + 7) / 8;
      if (rowBytes_ == 0 || rowBytes_ > 64u * 1024u * 1024u) {
        setErr(err, msg, PdfError::TooLarge, "PDF 图像行过大");
        return false;
      }
      row_.assign(rowBytes_, 0);
      prevRow_.assign(rowBytes_, 0);
      rawRow_.assign(rowBytes_ + (pred >= 10 ? 1 : 0), 0);
    }
  }
  return true;
}

size_t PdfFilteredReader::fillCb(void *ctx, const uint8_t **data) {
  PdfFilteredReader *self = static_cast<PdfFilteredReader *>(ctx);
  const size_t n = self->fill();
  *data = self->fillBuf_.data();
  return n;
}

size_t PdfFilteredReader::fill() {
  if (!src_ || srcPos_ >= srcLen_) return 0;
  const uint64_t remain = srcLen_ - srcPos_;
  const size_t want = remain < fillBuf_.size() ? static_cast<size_t>(remain) : fillBuf_.size();
  const int n = src_->readAt(srcOff_ + srcPos_, fillBuf_.data(), want);
  if (n <= 0) {
    srcPos_ = srcLen_;
    return 0;
  }
  srcPos_ += static_cast<uint64_t>(n);
  return static_cast<size_t>(n);
}

// 读原始（未预测）字节。返回 >0 实际字节数；0 = 正常结束；-1 = 出错。
int PdfFilteredReader::readRaw(uint8_t *dst, size_t len) {
  if (noFilter_) {
    if (srcPos_ >= srcLen_) return 0;
    const uint64_t remain = srcLen_ - srcPos_;
    const size_t want = remain < len ? static_cast<size_t>(remain) : len;
    const int n = src_->readAt(srcOff_ + srcPos_, dst, want);
    if (n <= 0) return -1;
    srcPos_ += static_cast<uint64_t>(n);
    return n;
  }
  if (eof_) return 0;
  size_t produced = 0;
  const InflateStream::Status s = inflate_.readAtMost(dst, len, &produced);
  if (s == InflateStream::Status::Error) return -1;
  if (s == InflateStream::Status::Done) eof_ = true;
  return static_cast<int>(produced);
}

int PdfFilteredReader::readRawRow() {
  size_t got = 0;
  while (got < rawRow_.size()) {
    const int n = readRaw(rawRow_.data() + got, rawRow_.size() - got);
    if (n < 0) return -1;
    if (n == 0) {
      if (got == 0) return 0;
      return -1;  // 半行 = 截断
    }
    got += static_cast<size_t>(n);
  }
  return 1;
}

static inline uint8_t paeth(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = p > a ? p - a : a - p;
  const int pb = p > b ? p - b : b - p;
  const int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
  if (pb <= pc) return static_cast<uint8_t>(b);
  return static_cast<uint8_t>(c);
}

bool PdfFilteredReader::applyPredictor() {
  const size_t n = rowBytes_;
  if (predictorKind_ == 2) {
    memcpy(row_.data(), rawRow_.data(), n);
    for (size_t i = static_cast<size_t>(bpp_); i < n; i++) {
      row_[i] = static_cast<uint8_t>(row_[i] + row_[i - bpp_]);
    }
  } else {
    const uint8_t filter = rawRow_[0];
    const uint8_t *cur = rawRow_.data() + 1;
    const uint8_t *prev = prevRow_.data();
    for (size_t i = 0; i < n; i++) {
      const int left = i >= static_cast<size_t>(bpp_) ? row_[i - bpp_] : 0;
      const int up = prev[i];
      const int upleft = i >= static_cast<size_t>(bpp_) ? prev[i - bpp_] : 0;
      int v = cur[i];
      switch (filter) {
        case 0: break;
        case 1: v += left; break;
        case 2: v += up; break;
        case 3: v += (left + up) >> 1; break;
        case 4: v += paeth(left, up, upleft); break;
        default: return false;
      }
      row_[i] = static_cast<uint8_t>(v);
    }
  }
  prevRow_.assign(row_.begin(), row_.end());
  havePrev_ = true;
  return true;
}

int PdfFilteredReader::read(uint8_t *dst, size_t len) {
  if (failed_) return -1;
  size_t done = 0;
  while (done < len) {
    if (predictor_) {
      if (rowValid_ && rowPos_ < rowBytes_) {
        size_t n = rowBytes_ - rowPos_;
        if (n > len - done) n = len - done;
        memcpy(dst + done, row_.data() + rowPos_, n);
        rowPos_ += n;
        done += n;
        produced_ += n;
        if (limit_ && produced_ > limit_) {
          failed_ = true;
          return -1;
        }
        continue;
      }
      if (eof_) break;
      const int r = readRawRow();
      if (r < 0) {
        failed_ = true;
        return -1;
      }
      if (r == 0) break;
      if (!applyPredictor()) {
        failed_ = true;
        return -1;
      }
      rowValid_ = true;
      rowPos_ = 0;
      continue;
    }
    const int n = readRaw(dst + done, len - done);
    if (n < 0) {
      failed_ = true;
      return -1;
    }
    if (n == 0) break;
    done += static_cast<size_t>(n);
    produced_ += static_cast<size_t>(n);
    if (limit_ && produced_ > limit_) {
      failed_ = true;
      return -1;
    }
  }
  return static_cast<int>(done);
}
