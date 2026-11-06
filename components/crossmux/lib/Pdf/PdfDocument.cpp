#include "PdfDocument.h"

#include <cstring>
#include <initializer_list>

namespace {

constexpr int kMaxObjStmObjects = 1000000;

void setErr(PdfError *err, std::string *msg, PdfError e, const char *m) {
  if (err) *err = e;
  if (msg) *msg = m ? m : "";
}

bool parseBox(const PdfValue &v, PdfRect *out) {
  if (!v.isArray() || v.arr.size() < 4) return false;
  double d[4];
  for (int i = 0; i < 4; i++) {
    if (v.arr[i].type != PdfType::Int && v.arr[i].type != PdfType::Real) return false;
    d[i] = v.arr[i].num;
  }
  out->x0 = d[0];
  out->y0 = d[1];
  out->x1 = d[2];
  out->y1 = d[3];
  out->normalize();
  return !out->empty();
}

int normalizeRotate(int r) {
  int m = ((r % 360) + 360) % 360;
  if (m % 90 != 0) m = ((m + 45) / 90) * 90 % 360;
  return m;
}

int valueRotate(const PdfValue *v) {
  if (!v || (v->type != PdfType::Int && v->type != PdfType::Real)) return 0;
  return normalizeRotate(static_cast<int>(v->num));
}

PdfValue intValue(int v) {
  PdfValue out;
  out.type = PdfType::Int;
  out.num = v;
  return out;
}

PdfValue boxValue(const PdfRect &r) {
  PdfValue out;
  out.type = PdfType::Array;
  out.arr.reserve(4);
  for (double d : {r.x0, r.y0, r.x1, r.y1}) {
    PdfValue v;
    v.type = PdfType::Real;
    v.num = d;
    out.arr.push_back(std::move(v));
  }
  return out;
}

}  // namespace

PdfDocument::~PdfDocument() { close(); }

void PdfDocument::close() {
  src_ = nullptr;
  xref_ = PdfXref();
  catalog_.reset();
  cache_.clear();
  cacheOrder_.clear();
  objStm_.clear();
  objStmOrder_.clear();
  objStmBytes_ = 0;
  pages_.clear();
}

bool PdfDocument::open(const PdfSource &src, PdfError *err, std::string *msg) {
  close();
  src_ = &src;
  if (!xref_.load(src, err, msg)) {
    src_ = nullptr;
    return false;
  }
  const PdfValue *root = xref_.trailer().get("Root");
  if (!root || root->type != PdfType::Ref) {
    setErr(err, msg, PdfError::Syntax, "PDF trailer 缺少 Root");
    src_ = nullptr;
    return false;
  }
  const PdfValue *cat = object(root->ref.num, err, msg);
  if (!cat || !cat->isDict()) {
    setErr(err, msg, PdfError::Syntax, "PDF Root 对象不是字典");
    src_ = nullptr;
    return false;
  }
  catalog_ = std::make_shared<PdfValue>(*cat);
  if (!buildPages(err, msg)) {
    src_ = nullptr;
    return false;
  }
  return true;
}

void PdfDocument::cacheObject(int num, PdfValue &&v) {
  auto it = cache_.find(num);
  if (it != cache_.end()) {
    it->second = std::move(v);
    return;
  }
  cache_.emplace(num, std::move(v));
  cacheOrder_.push_back(num);
  while (cache_.size() > static_cast<size_t>(PdfLimits::kMaxObjectCache) &&
         !cacheOrder_.empty()) {
    const int old = cacheOrder_.front();
    cacheOrder_.pop_front();
    if (old != num) cache_.erase(old);
  }
}

const PdfValue *PdfDocument::object(int num, PdfError *err, std::string *msg) {
  auto it = cache_.find(num);
  if (it != cache_.end()) return &it->second;
  if (!src_) {
    setErr(err, msg, PdfError::Io, "PDF 未打开");
    return nullptr;
  }
  const PdfXrefEntry *e = xref_.find(num);
  if (!e) {
    setErr(err, msg, PdfError::NotFound, "PDF 对象不存在");
    return nullptr;
  }
  PdfValue v;
  if (e->type == PdfXrefEntry::InUse) {
    PdfParser parser(*src_);
    int n = 0, g = 0;
    if (!parser.parseIndirect(e->off, &n, &g, &v, err, msg)) return nullptr;
    if (n != num) {
      setErr(err, msg, PdfError::Syntax, "PDF 对象号与交叉引用表不符");
      return nullptr;
    }
  } else if (e->type == PdfXrefEntry::Compressed) {
    if (!loadFromObjStm(num, e->streamNum, e->streamIdx, &v, err, msg)) return nullptr;
  } else {
    setErr(err, msg, PdfError::NotFound, "PDF 对象不存在");
    return nullptr;
  }
  cacheObject(num, std::move(v));
  return &cache_.find(num)->second;
}

const PdfValue *PdfDocument::resolveRef(const PdfValue &v, PdfError *err, std::string *msg) {
  if (v.type == PdfType::Ref) return object(v.ref.num, err, msg);
  if (v.isDict()) return &v;
  setErr(err, msg, PdfError::Syntax, "PDF 期望字典或引用");
  return nullptr;
}

bool PdfDocument::streamInfo(const PdfValue &dict, PdfStreamInfo *info, PdfError *err,
                             std::string *msg) {
  if (!dict.isDict() || !dict.hasStream) {
    setErr(err, msg, PdfError::Syntax, "PDF 对象不是流");
    return false;
  }
  info->off = dict.streamOff;
  uint64_t len = dict.streamLen;
  if (dict.streamLenIndirect) {
    const PdfValue *lv = object(dict.streamLenRef.num, err, msg);
    if (!lv || (lv->type != PdfType::Int && lv->type != PdfType::Real)) {
      setErr(err, msg, PdfError::Syntax, "PDF 流长度对象非法");
      return false;
    }
    if (lv->num < 0) {
      setErr(err, msg, PdfError::Syntax, "PDF 流长度为负");
      return false;
    }
    len = static_cast<uint64_t>(lv->num);
  }
  if (!src_ || info->off > src_->size() || len > src_->size() - info->off) {
    setErr(err, msg, PdfError::Io, "PDF 流超出文件范围");
    return false;
  }
  info->len = len;
  return pdfParseFilters(dict, &info->filters, err, msg);
}

bool PdfDocument::streamToMemory(const PdfValue &dict, pdfscratch::ByteVec *out, uint64_t maxOut,
                                 PdfError *err, std::string *msg) {
  out->clear();
  PdfStreamInfo si;
  if (!streamInfo(dict, &si, err, msg)) return false;
  if (si.hasDct()) {
    setErr(err, msg, PdfError::Unsupported, "DCT 流由图像层处理");
    return false;
  }
  PdfFilteredReader reader;
  if (!reader.init(*src_, si.off, si.len, si.filters, maxOut, err, msg)) return false;
  uint8_t chunk[1024];
  for (;;) {
    const int n = reader.read(chunk, sizeof(chunk));
    if (n < 0) {
      setErr(err, msg, PdfError::Decode, "PDF 流解码失败");
      return false;
    }
    if (n == 0) break;
    if (out->size() + static_cast<size_t>(n) > maxOut) {
      setErr(err, msg, PdfError::TooLarge, "PDF 流过大");
      return false;
    }
    out->insert(out->end(), chunk, chunk + n);
  }
  return true;
}

bool PdfDocument::objStmBytes(int streamNum, std::shared_ptr<pdfscratch::ByteVec> *out,
                              PdfError *err, std::string *msg) {
  auto it = objStm_.find(streamNum);
  if (it != objStm_.end()) {
    *out = it->second;
    return true;
  }
  const PdfValue *obj = object(streamNum, err, msg);
  if (!obj || !obj->isDict() || !obj->hasStream) {
    setErr(err, msg, PdfError::Syntax, "PDF 对象流缺失");
    return false;
  }
  PdfValue dict = *obj;  // 后续 object() 可能触发缓存淘汰
  auto buf = std::make_shared<pdfscratch::ByteVec>();
  if (!streamToMemory(dict, buf.get(), PdfLimits::kMaxObjStmCacheBytes, err, msg)) return false;
  objStm_[streamNum] = buf;
  objStmOrder_.push_back(streamNum);
  objStmBytes_ += buf->size();
  while (objStmBytes_ > static_cast<size_t>(PdfLimits::kMaxObjStmCacheBytes) &&
         objStmOrder_.size() > 1) {
    const int old = objStmOrder_.front();
    objStmOrder_.pop_front();
    auto oit = objStm_.find(old);
    if (oit != objStm_.end()) {
      objStmBytes_ -= oit->second->size();
      objStm_.erase(oit);
    }
  }
  *out = buf;
  return true;
}

bool PdfDocument::loadFromObjStm(int num, int streamNum, int streamIdx, PdfValue *out,
                                 PdfError *err, std::string *msg) {
  std::shared_ptr<pdfscratch::ByteVec> buf;
  if (!objStmBytes(streamNum, &buf, err, msg)) return false;
  const PdfValue *obj = object(streamNum, err, msg);
  if (!obj || !obj->isDict()) {
    setErr(err, msg, PdfError::Syntax, "PDF 对象流字典缺失");
    return false;
  }
  // 拷贝出头部参数，避免后续 object() 淘汰缓存。
  const int n = obj->get("N") ? obj->get("N")->asInt(0) : 0;
  const int first = obj->get("First") ? obj->get("First")->asInt(-1) : -1;
  if (n <= 0 || n > kMaxObjStmObjects || first < 0 ||
      static_cast<uint64_t>(first) > buf->size()) {
    setErr(err, msg, PdfError::Syntax, "PDF 对象流头部非法");
    return false;
  }

  PdfMemSource ms(buf->data(), buf->size());
  PdfLexer lex(ms, 0);
  pdfscratch::Vec<int> nums(static_cast<size_t>(n));
  pdfscratch::Vec<int> offs(static_cast<size_t>(n));
  for (int i = 0; i < n; i++) {
    PdfToken t1, t2;
    if (!lex.next(t1, err, msg) || !lex.next(t2, err, msg) || t1.kind != PdfToken::Int ||
        t2.kind != PdfToken::Int) {
      setErr(err, msg, PdfError::Syntax, "PDF 对象流索引损坏");
      return false;
    }
    nums[i] = static_cast<int>(t1.i);
    offs[i] = static_cast<int>(t2.i);
  }

  int idx = -1;
  if (streamIdx >= 0 && streamIdx < n && nums[streamIdx] == num) {
    idx = streamIdx;
  } else {
    for (int i = 0; i < n; i++) {
      if (nums[i] == num) {
        idx = i;
        break;
      }
    }
  }
  if (idx < 0) {
    setErr(err, msg, PdfError::NotFound, "PDF 对象不在对象流中");
    return false;
  }
  const uint64_t off = static_cast<uint64_t>(first) + static_cast<uint64_t>(offs[idx]);
  if (off >= buf->size()) {
    setErr(err, msg, PdfError::Syntax, "PDF 对象流内偏移越界");
    return false;
  }
  PdfParser parser(ms);
  return parser.parseValueAt(off, out, err, msg);
}

bool PdfDocument::buildPages(PdfError *err, std::string *msg) {
  pages_.clear();
  const PdfValue *pagesRef = catalog_ ? catalog_->get("Pages") : nullptr;
  if (!pagesRef) {
    setErr(err, msg, PdfError::Syntax, "PDF Catalog 缺少 Pages");
    return false;
  }
  const PdfValue *rootNode = resolveRef(*pagesRef, err, msg);
  if (!rootNode || !rootNode->isDict()) {
    setErr(err, msg, PdfError::Syntax, "PDF 页面树根不是字典");
    return false;
  }
  PdfValue rootCopy = *rootNode;
  PdfRect letter;
  letter.x1 = 612;
  letter.y1 = 792;
  std::unordered_set<int> active;
  if (!walkNode(rootCopy, nullptr, letter, false, letter, false, 0, 0, &active, err, msg)) {
    return false;
  }
  if (pages_.empty()) {
    setErr(err, msg, PdfError::Syntax, "PDF 没有页面");
    return false;
  }
  return true;
}

bool PdfDocument::walkNode(const PdfValue &node, const std::shared_ptr<const PdfValue> &resources,
                           const PdfRect &mediaBox, bool hasMediaBox, const PdfRect &cropBox,
                           bool hasCropBox, int rotate, int depth,
                           std::unordered_set<int> *active, PdfError *err, std::string *msg) {
  if (depth > PdfLimits::kMaxPageTreeDepth) {
    setErr(err, msg, PdfError::TooLarge, "PDF 页面树过深");
    return false;
  }

  std::shared_ptr<const PdfValue> res = resources;
  PdfRect mb = mediaBox;
  bool hasMb = hasMediaBox;
  PdfRect cb = cropBox;
  bool hasCb = hasCropBox;
  int rot = rotate;

  if (const PdfValue *r = node.get("Resources")) {
    res = std::make_shared<PdfValue>(*r);
  }
  if (const PdfValue *m = node.get("MediaBox")) {
    PdfRect b;
    if (parseBox(*m, &b)) {
      mb = b;
      hasMb = true;
    }
  }
  if (const PdfValue *c = node.get("CropBox")) {
    PdfRect b;
    if (parseBox(*c, &b)) {
      cb = b;
      hasCb = true;
    }
  }
  if (const PdfValue *r = node.get("Rotate")) {
    rot = valueRotate(r);
  }

  const PdfValue *kids = node.get("Kids");
  const PdfValue *type = node.get("Type");
  const bool isLeaf = !kids || (type && type->isName("Page"));
  if (isLeaf) {
    if (static_cast<int>(pages_.size()) >= PdfLimits::kMaxPages) {
      setErr(err, msg, PdfError::TooLarge, "PDF 页数超出上限");
      return false;
    }
    if (hasMb && hasCb) {
      PdfRect in;
      in.x0 = cb.x0 > mb.x0 ? cb.x0 : mb.x0;
      in.y0 = cb.y0 > mb.y0 ? cb.y0 : mb.y0;
      in.x1 = cb.x1 < mb.x1 ? cb.x1 : mb.x1;
      in.y1 = cb.y1 < mb.y1 ? cb.y1 : mb.y1;
      cb = in.empty() ? mb : in;
      hasCb = true;
    }
    auto page = std::make_shared<PdfValue>(node);
    if (res && !page->get("Resources")) page->set("Resources", *res);
    if (hasMb && !page->get("MediaBox")) page->set("MediaBox", boxValue(mb));
    if (!page->get("CropBox")) {
      if (hasCb) {
        page->set("CropBox", boxValue(cb));
      } else if (hasMb) {
        page->set("CropBox", boxValue(mb));
      }
    }
    page->set("Rotate", intValue(rot));
    pages_.push_back(page);
    return true;
  }

  if (!kids->isArray()) {
    setErr(err, msg, PdfError::Syntax, "PDF 页面树 Kids 不是数组");
    return false;
  }
  for (const auto &kid : kids->arr) {
    int refNum = -1;
    PdfValue kidCopy;
    if (kid.type == PdfType::Ref) {
      refNum = kid.ref.num;
      if (active->count(refNum)) {
        setErr(err, msg, PdfError::Syntax, "PDF 页面树存在环");
        return false;
      }
      const PdfValue *kv = object(refNum, err, msg);
      if (!kv || !kv->isDict()) {
        setErr(err, msg, PdfError::Syntax, "PDF 页面树子节点非法");
        return false;
      }
      kidCopy = *kv;
    } else if (kid.isDict()) {
      kidCopy = kid;
    } else {
      continue;
    }
    if (refNum >= 0) active->insert(refNum);
    const bool ok =
        walkNode(kidCopy, res, mb, hasMb, cb, hasCb, rot, depth + 1, active, err, msg);
    if (refNum >= 0) active->erase(refNum);
    if (!ok) return false;
  }
  return true;
}

const PdfValue *PdfDocument::pageDict(int index) const {
  if (index < 0 || static_cast<size_t>(index) >= pages_.size()) return nullptr;
  return pages_[static_cast<size_t>(index)].get();
}
