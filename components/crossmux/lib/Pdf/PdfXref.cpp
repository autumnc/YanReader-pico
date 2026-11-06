#include "PdfXref.h"

#include <cstring>

#include "PdfFilters.h"

namespace {

constexpr int kMaxXrefObjects = 200000;

void setErr(PdfError *err, std::string *msg, PdfError e, const char *m) {
  if (err) *err = e;
  if (msg) *msg = m ? m : "";
}

uint64_t beValue(const uint8_t *p, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++) v = (v << 8) | p[i];
  return v;
}

// 在 [off, off+kMaxStreamBytes) 里找 endstream，返回其前的数据长度（去掉一个 EOL）。
bool scanEndstream(const PdfSource &src, uint64_t off, uint64_t *len) {
  static const char kEnd[] = "endstream";
  const uint64_t size = src.size();
  if (off >= size) return false;
  const uint64_t limit = (size - off) > PdfLimits::kMaxStreamBytes
                             ? off + PdfLimits::kMaxStreamBytes
                             : size;
  uint8_t buf[1024 + 8];
  uint64_t p = off;
  int carry = 0;
  while (p < limit) {
    const size_t want = static_cast<size_t>(limit - p) < 1024 ? static_cast<size_t>(limit - p)
                                                              : 1024;
    const int n = src.readAt(p, buf + carry, want);
    if (n <= 0) return false;
    const size_t total = static_cast<size_t>(n) + static_cast<size_t>(carry);
    for (size_t i = 0; i + 9 <= total; i++) {
      if (memcmp(buf + i, kEnd, 9) == 0) {
        uint64_t end = p - static_cast<uint64_t>(carry) + i;
        if (end > off && end - 1 < size) {
          uint8_t c = 0;
          if (src.readAt(end - 1, &c, 1) == 1 && c == '\n') {
            end--;
            if (end > off && src.readAt(end - 1, &c, 1) == 1 && c == '\r') end--;
          } else if (c == '\r') {
            end--;
          }
        }
        *len = end - off;
        return true;
      }
    }
    carry = total > 8 ? 8 : static_cast<int>(total);
    memcpy(buf, buf + total - static_cast<size_t>(carry), static_cast<size_t>(carry));
    p += static_cast<uint64_t>(n);
  }
  return false;
}

// 从文件尾部 4KB 找 startxref 后的数字。
bool findStartXref(const PdfSource &src, uint64_t *out) {
  const uint64_t size = src.size();
  if (size < 16) return false;
  const uint64_t span = size > 4096 ? 4096 : size;
  std::vector<uint8_t> buf(static_cast<size_t>(span));
  if (src.readAt(size - span, buf.data(), buf.size()) != static_cast<int>(span)) return false;
  static const char kKey[] = "startxref";
  for (int i = static_cast<int>(span) - 10; i >= 0; i--) {
    if (memcmp(buf.data() + i, kKey, 9) != 0) continue;
    uint64_t p = size - span + static_cast<uint64_t>(i) + 9;
    uint8_t c = 0;
    while (p < size) {
      if (src.readAt(p, &c, 1) != 1) return false;
      if (c == 9 || c == 10 || c == 13 || c == 32) {
        p++;
        continue;
      }
      break;
    }
    uint64_t v = 0;
    bool any = false;
    while (p < size) {
      if (src.readAt(p, &c, 1) != 1) break;
      if (c < '0' || c > '9') break;
      v = v * 10 + (c - '0');
      any = true;
      p++;
      if (v > (1ull << 40)) return false;
    }
    if (any) {
      *out = v;
      return true;
    }
  }
  return false;
}

}  // namespace

bool PdfXref::ensureCapacity(int size, PdfError *err, std::string *msg) {
  if (size <= 0 || size > kMaxXrefObjects) {
    setErr(err, msg, PdfError::TooLarge, "PDF 对象数超出上限");
    return false;
  }
  const size_t n = static_cast<size_t>(size);
  if (entries_.size() < n) {
    entries_.resize(n);
    seen_.resize(n, 0);
  }
  return true;
}

void PdfXref::putEntry(int num, const PdfXrefEntry &e) {
  if (num < 0 || static_cast<size_t>(num) >= entries_.size()) return;
  if (seen_[num]) return;  // 新段已经写过，旧段不覆盖
  entries_[num] = e;
  seen_[num] = 1;
}

void PdfXref::mergeTrailer(const PdfValue &t) {
  if (!t.isDict()) return;
  if (!hasTrailer_) {
    trailer_ = t;
    hasTrailer_ = true;
    return;
  }
  for (const auto &kv : t.dict) {
    if (!trailer_.get(kv.first.c_str())) trailer_.set(kv.first.c_str(), kv.second);
  }
}

bool PdfXref::load(const PdfSource &src, PdfError *err, std::string *msg) {
  uint64_t off = 0;
  if (!findStartXref(src, &off)) {
    setErr(err, msg, PdfError::Syntax, "PDF 缺少 startxref");
    return false;
  }
  return loadAt(src, off, err, msg);
}

bool PdfXref::loadAt(const PdfSource &src, uint64_t startXref, PdfError *err, std::string *msg) {
  entries_.clear();
  seen_.clear();
  trailer_ = PdfValue{};
  hasTrailer_ = false;
  if (startXref >= src.size()) {
    setErr(err, msg, PdfError::Syntax, "PDF startxref 偏移越界");
    return false;
  }
  if (!loadSection(src, startXref, 0, err, msg)) {
    // 保留底层具体原因，只在其缺失时补一句上下文，方便定位坏文件。
    if (err && *err == PdfError::Syntax && (!msg || msg->empty())) {
      setErr(err, msg, PdfError::Syntax, "PDF 交叉引用表损坏");
    } else if (msg) {
      *msg = "PDF 交叉引用表损坏: " + *msg;
    }
    return false;
  }
  if (!hasTrailer_) {
    setErr(err, msg, PdfError::Syntax, "PDF 缺少 trailer");
    return false;
  }
  if (const PdfValue *enc = trailer_.get("Encrypt"); enc && !enc->isNull()) {
    setErr(err, msg, PdfError::Encrypted, "加密 PDF 不支持");
    return false;
  }
  const PdfValue *root = trailer_.get("Root");
  if (!root || root->type != PdfType::Ref) {
    setErr(err, msg, PdfError::Syntax, "PDF trailer 缺少 Root");
    return false;
  }
  return true;
}

bool PdfXref::loadSection(const PdfSource &src, uint64_t off, int depth, PdfError *err,
                          std::string *msg) {
  if (depth >= PdfLimits::kMaxXrefChain) {
    setErr(err, msg, PdfError::TooLarge, "PDF 交叉引用链过长");
    return false;
  }
  if (off >= src.size()) {
    setErr(err, msg, PdfError::Syntax, "PDF 交叉引用偏移越界");
    return false;
  }
  PdfLexer lex(src, off);
  PdfToken t;
  if (!lex.next(t, err, msg)) return false;
  if (t.isKeyword("xref")) return loadClassic(src, off, depth, err, msg);
  if (t.kind == PdfToken::Int) return loadXrefStream(src, off, depth, err, msg);
  setErr(err, msg, PdfError::Syntax, "PDF 交叉引用位置既不是表也不是流");
  return false;
}

bool PdfXref::loadClassic(const PdfSource &src, uint64_t off, int depth, PdfError *err,
                          std::string *msg) {
  PdfLexer lex(src, off);
  PdfToken t;
  if (!lex.next(t, err, msg) || !t.isKeyword("xref")) {
    setErr(err, msg, PdfError::Syntax, "PDF 经典 xref 表头缺失");
    return false;
  }
  for (;;) {
    if (!lex.next(t, err, msg)) return false;
    if (t.isKeyword("trailer")) break;
    if (t.kind != PdfToken::Int) {
      setErr(err, msg, PdfError::Syntax, "PDF xref 子段头缺失");
      return false;
    }
    const long long start = t.i;
    PdfToken ct;
    if (!lex.next(ct, err, msg) || ct.kind != PdfToken::Int) {
      setErr(err, msg, PdfError::Syntax, "PDF xref 子段计数缺失");
      return false;
    }
    const long long count = ct.i;
    if (start < 0 || count < 0 || count > kMaxXrefObjects ||
        start + count > kMaxXrefObjects) {
      setErr(err, msg, PdfError::TooLarge, "PDF xref 子段过大");
      return false;
    }
    if (!ensureCapacity(static_cast<int>(start + count), err, msg)) return false;
    for (long long i = 0; i < count; i++) {
      PdfToken ot, gt, kt;
      if (!lex.next(ot, err, msg) || !lex.next(gt, err, msg) || !lex.next(kt, err, msg)) {
        return false;
      }
      if (ot.kind != PdfToken::Int || gt.kind != PdfToken::Int ||
          kt.kind != PdfToken::Keyword) {
        setErr(err, msg, PdfError::Syntax, "PDF xref 条目格式错误");
        return false;
      }
      PdfXrefEntry e;
      if (kt.text == "n") {
        e.type = PdfXrefEntry::InUse;
        e.off = static_cast<uint64_t>(ot.i);
        e.gen = static_cast<uint32_t>(gt.i);
      } else {
        e.type = PdfXrefEntry::Free;
      }
      putEntry(static_cast<int>(start + i), e);
    }
  }

  PdfParser parser(src);
  PdfValue tr;
  if (!parser.parseValueAt(lex.pos(), &tr, err, msg)) return false;
  if (!tr.isDict()) {
    setErr(err, msg, PdfError::Syntax, "PDF trailer 不是字典");
    return false;
  }
  mergeTrailer(tr);

  // 混合文件：先补 xref stream 里的压缩对象，再沿 /Prev 找更老的段。
  if (const PdfValue *xs = tr.get("XRefStm")) {
    if (xs->type == PdfType::Int) {
      if (!loadSection(src, static_cast<uint64_t>(xs->asInt()), depth + 1, err, msg)) return false;
    }
  }
  if (const PdfValue *prev = tr.get("Prev")) {
    if (prev->type == PdfType::Int) {
      if (!loadSection(src, static_cast<uint64_t>(prev->asInt()), depth + 1, err, msg)) {
        return false;
      }
    }
  }
  return true;
}

bool PdfXref::loadXrefStream(const PdfSource &src, uint64_t off, int depth, PdfError *err,
                             std::string *msg) {
  PdfParser parser(src);
  PdfValue obj;
  int num = 0, gen = 0;
  if (!parser.parseIndirect(off, &num, &gen, &obj, err, msg)) return false;
  if (!obj.isDict() || !obj.hasStream) {
    setErr(err, msg, PdfError::Syntax, "PDF xref stream 不是流对象");
    return false;
  }

  const PdfValue *sv = obj.get("Size");
  if (!sv || sv->type != PdfType::Int) {
    setErr(err, msg, PdfError::Syntax, "PDF xref stream 缺少 Size");
    return false;
  }
  const int size = sv->asInt();
  if (!ensureCapacity(size, err, msg)) return false;

  const PdfValue *wv = obj.get("W");
  if (!wv || !wv->isArray() || wv->arr.size() != 3) {
    setErr(err, msg, PdfError::Syntax, "PDF xref stream 的 W 非三元组");
    return false;
  }
  int w[3];
  for (int i = 0; i < 3; i++) {
    if (wv->arr[i].type != PdfType::Int) {
      setErr(err, msg, PdfError::Syntax, "PDF xref stream 的 W 非整数");
      return false;
    }
    w[i] = wv->arr[i].asInt();
    if (w[i] < 0 || w[i] > 8) {
      setErr(err, msg, PdfError::Syntax, "PDF xref stream 的 W 越界");
      return false;
    }
  }
  const int rowLen = w[0] + w[1] + w[2];
  if (rowLen <= 0 || rowLen > 24) {
    setErr(err, msg, PdfError::Syntax, "PDF xref stream 行宽非法");
    return false;
  }

  // /Index 缺省 [0 Size]
  std::vector<std::pair<int, int>> index;
  const PdfValue *iv = obj.get("Index");
  if (iv && iv->isArray()) {
    if (iv->arr.size() % 2 != 0) {
      setErr(err, msg, PdfError::Syntax, "PDF xref stream 的 Index 长度非偶数");
      return false;
    }
    for (size_t i = 0; i + 1 < iv->arr.size(); i += 2) {
      const int first = iv->arr[i].asInt(-1);
      const int cnt = iv->arr[i + 1].asInt(-1);
      if (first < 0 || cnt < 0) {
        setErr(err, msg, PdfError::Syntax, "PDF xref stream 的 Index 非法");
        return false;
      }
      index.emplace_back(first, cnt);
    }
  } else {
    index.emplace_back(0, size);
  }
  uint64_t totalRows = 0;
  for (const auto &p : index) totalRows += static_cast<uint64_t>(p.second);
  const uint64_t totalBytes = totalRows * static_cast<uint64_t>(rowLen);
  if (totalBytes > static_cast<uint64_t>(PdfLimits::kMaxStreamBytes)) {
    setErr(err, msg, PdfError::TooLarge, "PDF xref stream 解压后过大");
    return false;
  }

  uint64_t len = 0;
  if (!obj.streamLenIndirect) {
    len = obj.streamLen;
  } else if (!scanEndstream(src, obj.streamOff, &len)) {
    setErr(err, msg, PdfError::Syntax, "PDF xref stream 长度未知");
    return false;
  }

  std::vector<PdfFilterSpec> filters;
  if (!pdfParseFilters(obj, &filters, err, msg)) return false;
  for (auto &f : filters) {
    if (f.isFlate() && f.parms.isDict() && !f.parms.get("Columns")) {
      PdfValue cols;
      cols.type = PdfType::Int;
      cols.num = rowLen;
      f.parms.set("Columns", std::move(cols));
    }
  }
  PdfFilteredReader reader;
  if (!reader.init(src, obj.streamOff, len, filters, totalBytes, err, msg)) return false;
  pdfscratch::ByteVec data(static_cast<size_t>(totalBytes));
  size_t got = 0;
  while (got < data.size()) {
    const int n = reader.read(data.data() + got, data.size() - got);
    if (n < 0) {
      setErr(err, msg, PdfError::Decode, "PDF xref stream 解码失败");
      return false;
    }
    if (n == 0) break;
    got += static_cast<size_t>(n);
  }
  if (got != data.size()) {
    setErr(err, msg, PdfError::Decode, "PDF xref stream 数据不足");
    return false;
  }

  const uint8_t *p = data.data();
  for (const auto &seg : index) {
    if (!ensureCapacity(seg.first + seg.second, err, msg)) return false;
    for (int i = 0; i < seg.second; i++) {
      const uint64_t type = w[0] ? beValue(p, w[0]) : 1;
      p += w[0];
      const uint64_t f2 = beValue(p, w[1]);
      p += w[1];
      const uint64_t f3 = beValue(p, w[2]);
      p += w[2];
      PdfXrefEntry e;
      if (type == 1) {
        e.type = PdfXrefEntry::InUse;
        e.off = f2;
        e.gen = static_cast<uint32_t>(f3);
      } else if (type == 2) {
        e.type = PdfXrefEntry::Compressed;
        e.streamNum = static_cast<int32_t>(f2);
        e.streamIdx = static_cast<int32_t>(f3);
      } else {
        e.type = PdfXrefEntry::Free;
      }
      putEntry(seg.first + i, e);
    }
  }

  mergeTrailer(obj);
  if (const PdfValue *prev = obj.get("Prev")) {
    if (prev->type == PdfType::Int) {
      if (!loadSection(src, static_cast<uint64_t>(prev->asInt()), depth + 1, err, msg)) {
        return false;
      }
    }
  }
  return true;
}

const PdfXrefEntry *PdfXref::find(int objNum) const {
  if (objNum < 0 || static_cast<size_t>(objNum) >= entries_.size()) return nullptr;
  if (!seen_[objNum]) return nullptr;
  const PdfXrefEntry &e = entries_[objNum];
  if (e.type == PdfXrefEntry::Free) return nullptr;
  return &e;
}
