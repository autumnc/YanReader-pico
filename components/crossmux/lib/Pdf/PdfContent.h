#pragma once
// ── 内容流扫描：找出页面的主图像 XObject ─────────────────────────────────
// 只关心 q/Q/cm/Do 与图像/表单 XObject。文字、路径、颜色操作符全部忽略
// （扫描件常带 OCR 文字层，不能因为「有字」就拒绝）。
//
// 判定：主图像放置矩形面积 ≥ 可见页面（CropBox）面积的 55%。
// 非轴对齐（剪切/斜切/90° 内嵌）记 Unsupported；/SMask /Mask /ImageMask
// 同样不支持。内联图像 BI 不支持。

#include <string>
#include <vector>

#include "PdfDocument.h"
#include "PdfObject.h"
#include "PdfPage.h"
#include "PdfTypes.h"

struct PdfPageImage {
  bool found = false;
  PdfValue imageDict;    // 图像 XObject 字典副本（含流偏移）
  PdfRect placement;     // 页面坐标系里的放置矩形
  PdfRect visible;       // CropBox
  double coverage = 0;   // placement.area / visible.area
};

bool pdfFindPageImage(PdfDocument &doc, const PdfValue &pageDict, const PdfPageGeom &geom,
                      PdfPageImage *out, PdfError *err, std::string *msg);
