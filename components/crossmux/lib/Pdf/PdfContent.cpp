#include "PdfContent.h"

#include <cmath>
#include <cstring>

namespace {

constexpr double kCoverageMin = 0.55;

void setErr(PdfError *err, std::string *msg, PdfError e, const char *m) {
  if (err) *err = e;
  if (msg) *msg = m ? m : "";
}

struct Mat {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

// 行向量约定：p' = p × M。m1 先作用，再作用 m2。
Mat mul(const Mat &m1, const Mat &m2) {
  Mat r;
  r.a = m1.a * m2.a + m1.b * m2.c;
  r.b = m1.a * m2.b + m1.b * m2.d;
  r.c = m1.c * m2.a + m1.d * m2.c;
  r.d = m1.c * m2.b + m1.d * m2.d;
  r.e = m1.e * m2.a + m1.f * m2.c + m2.e;
  r.f = m1.e * m2.b + m1.f * m2.d + m2.f;
  return r;
}

void apply(const Mat &m, double x, double y, double *ox, double *oy) {
  *ox = m.a * x + m.c * y + m.e;
  *oy = m.b * x + m.d * y + m.f;
}

bool parseMatrix(const PdfValue *v, Mat *out) {
  if (!v || !v->isArray() || v->arr.size() < 6) return false;
  double n[6];
  for (int i = 0; i < 6; i++) {
    if (v->arr[i].type != PdfType::Int && v->arr[i].type != PdfType::Real) return false;
    n[i] = v->arr[i].num;
  }
  out->a = n[0];
  out->b = n[1];
  out->c = n[2];
  out->d = n[3];
  out->e = n[4];
  out->f = n[5];
  return true;
}

class Scanner {
 public:
  Scanner(PdfDocument &doc, PdfError *err, std::string *msg)
      : doc_(doc), err_(err), msg_(msg) {}

  bool scanPage(const PdfValue &pageDict, const PdfPageGeom &geom, PdfPageImage *out);

 private:
  struct Op {
    bool isName = false;
    double num = 0;
    std::string name;
  };

  bool collectContents(const PdfValue &dict, pdfscratch::ByteVec *out);
  bool scanBuffer(const pdfscratch::ByteVec &data, const Mat &baseCtm, const PdfValue &resources,
                  int depth);
  bool lookupXObject(const PdfValue &resources, const std::string &name, PdfValue *out);
  bool resourcesCopy(const PdfValue &src, PdfValue *out);
  void considerImage(const PdfValue &img, const Mat &ctm);
  bool handleOperator(const std::string &op, const std::vector<Op> &ops, Mat *ctm,
                      std::vector<Mat> *stack, const PdfValue &resources, int depth);

  PdfDocument &doc_;
  PdfError *err_;
  std::string *msg_;
  double visibleArea_ = 1;
  bool found_ = false;
  bool bestHasMask_ = false;
  bool nonAxis_ = false;
  double bestArea_ = 0;
  PdfRect bestRect_;
  PdfValue best_;
};

bool Scanner::resourcesCopy(const PdfValue &src, PdfValue *out) {
  if (src.isDict()) {
    *out = src;
    return true;
  }
  if (src.type == PdfType::Ref) {
    const PdfValue *v = doc_.object(src.ref.num, err_, msg_);
    if (v && v->isDict()) {
      *out = *v;
      return true;
    }
  }
  return false;
}

bool Scanner::collectContents(const PdfValue &dict, pdfscratch::ByteVec *out) {
  out->clear();
  const PdfValue *c = dict.get("Contents");
  if (!c) return true;

  std::vector<PdfValue> streams;
  if (c->type == PdfType::Ref) {
    const PdfValue *v = doc_.object(c->ref.num, err_, msg_);
    if (!v) return false;
    streams.push_back(*v);
  } else if (c->isArray()) {
    for (const auto &item : c->arr) {
      if (item.type == PdfType::Ref) {
        const PdfValue *v = doc_.object(item.ref.num, err_, msg_);
        if (!v) return false;
        streams.push_back(*v);
      } else if (item.isDict() && item.hasStream) {
        streams.push_back(item);
      }
    }
  } else if (c->isDict() && c->hasStream) {
    streams.push_back(*c);
  }

  for (const auto &s : streams) {
    if (!s.hasStream) continue;
    pdfscratch::ByteVec buf;
    if (!doc_.streamToMemory(s, &buf, PdfLimits::kMaxContentBytes, err_, msg_)) return false;
    if (out->size() + buf.size() + 1 > static_cast<size_t>(PdfLimits::kMaxContentBytes)) {
      setErr(err_, msg_, PdfError::TooLarge, "PDF 页面内容过大");
      return false;
    }
    out->insert(out->end(), buf.begin(), buf.end());
    out->push_back('\n');
  }
  return true;
}

bool Scanner::lookupXObject(const PdfValue &resources, const std::string &name, PdfValue *out) {
  if (!resources.isDict()) return false;
  const PdfValue *x = resources.get("XObject");
  if (!x) return false;
  PdfValue xd;
  if (!resourcesCopy(*x, &xd) || !xd.isDict()) return false;
  const PdfValue *item = xd.get(name.c_str());
  if (!item) return false;
  if (item->type == PdfType::Ref) {
    const PdfValue *v = doc_.object(item->ref.num, err_, msg_);
    if (!v || !v->isDict()) return false;
    *out = *v;
    return true;
  }
  if (item->isDict()) {
    *out = *item;
    return true;
  }
  return false;
}

void Scanner::considerImage(const PdfValue &img, const Mat &ctm) {
  if (!img.isDict() || !img.hasStream) return;
  const PdfValue *sub = img.get("Subtype");
  if (!sub || !sub->isName("Image")) return;

  // 先对 90° 旋转的放置做一次宽容判定：只接受轴对齐。
  const double base = fabs(ctm.a) > fabs(ctm.d) ? fabs(ctm.a) : fabs(ctm.d);
  const double eps = 1e-6 * (base > 1 ? base : 1);
  if (fabs(ctm.b) > eps || fabs(ctm.c) > eps) {
    nonAxis_ = true;
    return;
  }
  double x0, y0, x1, y1, x2, y2, x3, y3;
  apply(ctm, 0, 0, &x0, &y0);
  apply(ctm, 1, 0, &x1, &y1);
  apply(ctm, 0, 1, &x2, &y2);
  apply(ctm, 1, 1, &x3, &y3);
  PdfRect r;
  r.x0 = x0 < x1 ? x0 : x1;
  r.x1 = x0 > x1 ? x0 : x1;
  r.y0 = y0 < y1 ? y0 : y1;
  r.y1 = y0 > y1 ? y0 : y1;
  if (x2 < r.x0) r.x0 = x2;
  if (x2 > r.x1) r.x1 = x2;
  if (x3 < r.x0) r.x0 = x3;
  if (x3 > r.x1) r.x1 = x3;
  if (y2 < r.y0) r.y0 = y2;
  if (y2 > r.y1) r.y1 = y2;
  if (y3 < r.y0) r.y0 = y3;
  if (y3 > r.y1) r.y1 = y3;

  const double area = (r.x1 - r.x0) * (r.y1 - r.y0);
  if (area <= bestArea_) return;
  const PdfValue *sm = img.get("SMask");
  const PdfValue *mask = img.get("Mask");
  // /ImageMask true 是扫描件常见的模板蒙版（JBIG2 位图），由 PdfImage 处理；
  // 软蒙版 / 显式蒙版仍然不支持。
  bestHasMask_ = (sm && !sm->isNull()) || (mask && !mask->isNull());
  bestArea_ = area;
  bestRect_ = r;
  best_ = img;
  found_ = true;
}

bool Scanner::handleOperator(const std::string &op, const std::vector<Op> &ops, Mat *ctm,
                             std::vector<Mat> *stack, const PdfValue &resources, int depth) {
  if (op == "q") {
    stack->push_back(*ctm);
    return true;
  }
  if (op == "Q") {
    if (!stack->empty()) {
      *ctm = stack->back();
      stack->pop_back();
    }
    return true;
  }
  if (op == "cm") {
    if (ops.size() < 6) return true;
    Mat m;
    double n[6];
    const size_t base = ops.size() - 6;
    for (int i = 0; i < 6; i++) {
      if (ops[base + static_cast<size_t>(i)].isName) return true;
      n[i] = ops[base + static_cast<size_t>(i)].num;
    }
    m.a = n[0];
    m.b = n[1];
    m.c = n[2];
    m.d = n[3];
    m.e = n[4];
    m.f = n[5];
    *ctm = mul(m, *ctm);
    return true;
  }
  if (op == "Do") {
    const Op *name = nullptr;
    for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
      if (it->isName) {
        name = &*it;
        break;
      }
    }
    if (!name) return true;
    PdfValue xobj;
    if (!lookupXObject(resources, name->name, &xobj)) return true;
    const PdfValue *sub = xobj.get("Subtype");
    if (sub && sub->isName("Image")) {
      considerImage(xobj, *ctm);
      return true;
    }
    if (sub && sub->isName("Form")) {
      if (depth >= PdfLimits::kMaxFormDepth) {
        setErr(err_, msg_, PdfError::Unsupported, "PDF 表单嵌套过深");
        return false;
      }
      PdfValue formRes = resources;
      if (const PdfValue *r = xobj.get("Resources")) {
        PdfValue tmp;
        if (resourcesCopy(*r, &tmp)) formRes = std::move(tmp);
      }
      Mat formMat;
      if (const PdfValue *mx = xobj.get("Matrix")) {
        if (!parseMatrix(mx, &formMat)) formMat = Mat{};
      }
      pdfscratch::ByteVec buf;
      if (!collectContents(xobj, &buf)) return false;
      if (buf.empty()) return true;
      return scanBuffer(buf, mul(formMat, *ctm), formRes, depth + 1);
    }
    return true;
  }
  if (op == "BI") {
    setErr(err_, msg_, PdfError::Unsupported, "PDF 内联图像不支持");
    return false;
  }
  return true;
}

bool Scanner::scanBuffer(const pdfscratch::ByteVec &data, const Mat &baseCtm,
                         const PdfValue &resources, int depth) {
  PdfMemSource mem(data.data(), data.size());
  PdfLexer lex(mem, 0);
  std::vector<Op> ops;
  std::vector<Mat> stack;
  Mat ctm = baseCtm;
  int nesting = 0;
  int tokens = 0;

  for (;;) {
    PdfToken t;
    if (!lex.next(t, err_, msg_)) return false;
    if (t.kind == PdfToken::Eof) return true;
    if (++tokens > PdfLimits::kMaxContentOps) {
      setErr(err_, msg_, PdfError::TooLarge, "PDF 内容操作过多");
      return false;
    }
    switch (t.kind) {
      case PdfToken::ArrOpen:
      case PdfToken::DictOpen:
        nesting++;
        continue;
      case PdfToken::ArrClose:
      case PdfToken::DictClose:
        if (nesting > 0) nesting--;
        continue;
      case PdfToken::Int:
      case PdfToken::Real:
        if (nesting == 0 && ops.size() < 256) {
          Op o;
          o.num = t.num;
          ops.push_back(std::move(o));
        }
        continue;
      case PdfToken::Name:
        if (nesting == 0 && ops.size() < 256) {
          Op o;
          o.isName = true;
          o.name = std::move(t.text);
          ops.push_back(std::move(o));
        }
        continue;
      case PdfToken::String:
        continue;  // 内容流里的字符串与图像放置无关
      case PdfToken::Keyword:
        if (nesting > 0) continue;
        if (t.text == "true" || t.text == "false" || t.text == "null") continue;
        if (!handleOperator(t.text, ops, &ctm, &stack, resources, depth)) return false;
        ops.clear();
        continue;
      default: continue;
    }
  }
}

bool Scanner::scanPage(const PdfValue &pageDict, const PdfPageGeom &geom, PdfPageImage *out) {
  visibleArea_ = geom.cropBox.area();
  if (visibleArea_ <= 0) visibleArea_ = 1;

  PdfValue resources;
  if (const PdfValue *r = pageDict.get("Resources")) resourcesCopy(*r, &resources);

  pdfscratch::ByteVec contents;
  if (!collectContents(pageDict, &contents)) return false;
  if (!contents.empty()) {
    if (!scanBuffer(contents, Mat{}, resources, 0)) return false;
  }

  if (!found_) {
    if (nonAxis_) {
      setErr(err_, msg_, PdfError::Unsupported, "PDF 图像非轴对齐");
    } else {
      setErr(err_, msg_, PdfError::NoPageImage, "此 PDF 不是页面图像型");
    }
    return false;
  }
  if (bestHasMask_) {
    setErr(err_, msg_, PdfError::Unsupported, "PDF 图像软蒙版不支持");
    return false;
  }
  const double coverage = bestArea_ / visibleArea_;
  if (coverage < kCoverageMin) {
    setErr(err_, msg_, PdfError::NoPageImage, "此 PDF 不是页面图像型");
    return false;
  }
  out->found = true;
  out->imageDict = std::move(best_);
  out->visible = geom.cropBox;
  out->coverage = coverage;
  out->placement = bestRect_;
  return true;
}

}  // namespace

bool pdfFindPageImage(PdfDocument &doc, const PdfValue &pageDict, const PdfPageGeom &geom,
                      PdfPageImage *out, PdfError *err, std::string *msg) {
  Scanner s(doc, err, msg);
  return s.scanPage(pageDict, geom, out);
}
