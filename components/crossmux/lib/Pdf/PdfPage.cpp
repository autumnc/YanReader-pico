#include "PdfPage.h"

namespace {

void setErr(PdfError *err, std::string *msg, PdfError e, const char *m) {
  if (err) *err = e;
  if (msg) *msg = m ? m : "";
}

bool parseBox(const PdfValue *v, PdfRect *out) {
  if (!v || !v->isArray() || v->arr.size() < 4) return false;
  double d[4];
  for (int i = 0; i < 4; i++) {
    if (v->arr[i].type != PdfType::Int && v->arr[i].type != PdfType::Real) return false;
    d[i] = v->arr[i].num;
  }
  out->x0 = d[0];
  out->y0 = d[1];
  out->x1 = d[2];
  out->y1 = d[3];
  out->normalize();
  return !out->empty();
}

int normRotate(int r) {
  int m = ((r % 360) + 360) % 360;
  if (m % 90 != 0) m = ((m + 45) / 90) * 90 % 360;
  return m;
}

}  // namespace

bool pdfReadPageGeom(const PdfValue &pageDict, PdfPageGeom *out, PdfError *err, std::string *msg) {
  PdfRect letter;
  letter.x1 = 612;
  letter.y1 = 792;

  PdfRect mb = letter;
  if (!parseBox(pageDict.get("MediaBox"), &mb)) mb = letter;

  PdfRect cb = mb;
  PdfRect own;
  if (parseBox(pageDict.get("CropBox"), &own)) {
    PdfRect in;
    in.x0 = own.x0 > mb.x0 ? own.x0 : mb.x0;
    in.y0 = own.y0 > mb.y0 ? own.y0 : mb.y0;
    in.x1 = own.x1 < mb.x1 ? own.x1 : mb.x1;
    in.y1 = own.y1 < mb.y1 ? own.y1 : mb.y1;
    cb = in.empty() ? mb : in;
  }

  int rot = 0;
  if (const PdfValue *r = pageDict.get("Rotate")) {
    if (r->type == PdfType::Int || r->type == PdfType::Real) rot = normRotate(r->asInt(0));
  }

  if (mb.w() <= 0 || mb.h() <= 0 || cb.w() <= 0 || cb.h() <= 0) {
    setErr(err, msg, PdfError::Syntax, "PDF 页面框非法");
    return false;
  }
  out->mediaBox = mb;
  out->cropBox = cb;
  out->rotate = rot;
  return true;
}
