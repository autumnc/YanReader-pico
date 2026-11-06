#pragma once
// 页面几何：MediaBox / CropBox / Rotate。页树继承已在 PdfDocument 里合并，
// 这里只做最终取值与兜底（缺框按 Letter，CropBox ∩ MediaBox）。

#include "PdfObject.h"
#include "PdfTypes.h"

struct PdfPageGeom {
  PdfRect mediaBox;
  PdfRect cropBox;
  int rotate = 0;  // 0/90/180/270
};

bool pdfReadPageGeom(const PdfValue &pageDict, PdfPageGeom *out, PdfError *err, std::string *msg);
