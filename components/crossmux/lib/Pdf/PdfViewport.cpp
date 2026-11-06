#include "PdfViewport.h"

#include <algorithm>
#include <cmath>

namespace {

void setErr(PdfError *err, std::string *msg, PdfError e, const char *m) {
  if (err) *err = e;
  if (msg) *msg = m ? m : "";
}

int normRotate(int r) {
  int m = ((r % 360) + 360) % 360;
  if (m % 90 != 0) m = ((m + 45) / 90) * 90 % 360;
  return m;
}

void cropDims(const PdfViewSpec &spec, int *cx0, int *cy0, int *cw, int *ch) {
  *cx0 = 0;
  *cy0 = 0;
  *cw = spec.srcW;
  *ch = spec.srcH;
  if (!spec.crop || !spec.cropBox.valid) return;
  int x0 = std::max(0, spec.cropBox.x0);
  int y0 = std::max(0, spec.cropBox.y0);
  int x1 = std::min(spec.srcW, spec.cropBox.x1);
  int y1 = std::min(spec.srcH, spec.cropBox.y1);
  if (x1 - x0 < 2 || y1 - y0 < 2) return;
  *cx0 = x0;
  *cy0 = y0;
  *cw = x1 - x0;
  *ch = y1 - y0;
}

}  // namespace

double pdfBaseScale(const PdfViewSpec &spec) {
  int cx0, cy0, cw, ch;
  cropDims(spec, &cx0, &cy0, &cw, &ch);
  const int rot = normRotate(spec.rotate);
  const int dw = (rot == 90 || rot == 270) ? ch : cw;
  const int dh = (rot == 90 || rot == 270) ? cw : ch;
  if (dw <= 0 || dh <= 0 || spec.areaW <= 0 || spec.areaH <= 0) return 1.0;
  const double fitW = static_cast<double>(spec.areaW) / dw;
  const double fitH = static_cast<double>(spec.areaH) / dh;
  switch (spec.zoomMode) {
    case kPdfZoomFitWidth: return fitW;
    case kPdfZoomOriginal: return 1.0;
    case kPdfZoomFitPage:
    default: return fitW < fitH ? fitW : fitH;
  }
}

bool pdfComputeView(const PdfViewSpec &spec, PdfViewMap *out, PdfError *err, std::string *msg) {
  if (spec.srcW <= 0 || spec.srcH <= 0) {
    setErr(err, msg, PdfError::Decode, "PDF 图像尺寸非法");
    return false;
  }
  if (spec.areaW <= 0 || spec.areaH <= 0) {
    setErr(err, msg, PdfError::Decode, "PDF 显示区非法");
    return false;
  }
  int cx0, cy0, cw, ch;
  cropDims(spec, &cx0, &cy0, &cw, &ch);
  const int rot = normRotate(spec.rotate);
  const int dw = (rot == 90 || rot == 270) ? ch : cw;
  const int dh = (rot == 90 || rot == 270) ? cw : ch;

  const double base = pdfBaseScale(spec);
  double zoom = spec.zoom;
  if (!(zoom > 0)) zoom = 1.0;
  if (zoom > 16.0) zoom = 16.0;
  const double scale = base * zoom;
  if (!(scale > 0) || !std::isfinite(scale)) {
    setErr(err, msg, PdfError::Decode, "PDF 缩放倍率非法");
    return false;
  }

  out->dispW = dw;
  out->dispH = dh;
  out->scale = scale;
  out->areaX = spec.areaX;
  out->areaY = spec.areaY;
  out->areaW = spec.areaW;
  out->areaH = spec.areaH;
  out->rotate = rot;
  out->cx0 = cx0;
  out->cy0 = cy0;
  out->cw = cw;
  out->ch = ch;

  out->offX = (spec.areaW - dw * scale) * 0.5 + spec.panX;
  out->offY = (spec.areaH - dh * scale) * 0.5 + spec.panY;
  return true;
}

bool PdfViewMap::screenToSource(double sx, double sy, int *outX, int *outY) const {
  const double ax = sx - areaX;
  const double ay = sy - areaY;
  const double u = (ax - offX) / scale;
  const double v = (ay - offY) / scale;
  if (u < 0 || v < 0 || u >= dispW || v >= dispH) return false;

  double lx = 0, ly = 0;
  switch (rotate) {
    case 90: lx = v; ly = ch - u; break;
    case 180: lx = cw - u; ly = ch - v; break;
    case 270: lx = cw - v; ly = u; break;
    case 0:
    default: lx = u; ly = v; break;
  }
  int x = static_cast<int>(std::floor(lx));
  int y = static_cast<int>(std::floor(ly));
  x = std::max(0, std::min(cw - 1, x));
  y = std::max(0, std::min(ch - 1, y));
  *outX = cx0 + x;
  *outY = cy0 + y;
  return true;
}

void PdfViewMap::sourceToDisplay(int srcX, int srcY, double *u, double *v) const {
  const double x = (srcX - cx0) + 0.5;
  const double y = (srcY - cy0) + 0.5;
  switch (rotate) {
    case 90: *u = ch - y; *v = x; break;
    case 180: *u = cw - x; *v = ch - y; break;
    case 270: *u = y; *v = cw - x; break;
    case 0:
    default: *u = x; *v = y; break;
  }
}

void PdfViewMap::sourceToScreen(int srcX, int srcY, double *sx, double *sy) const {
  double u, v;
  sourceToDisplay(srcX, srcY, &u, &v);
  *sx = areaX + offX + u * scale;
  *sy = areaY + offY + v * scale;
}

void pdfClampPan(const PdfViewMap &map, double *panX, double *panY) {
  const double maxX = (map.contentW() - map.areaW) * 0.5;
  const double maxY = (map.contentH() - map.areaH) * 0.5;
  if (maxX <= 0) {
    *panX = 0;
  } else {
    if (*panX < -maxX) *panX = -maxX;
    if (*panX > maxX) *panX = maxX;
  }
  if (maxY <= 0) {
    *panY = 0;
  } else {
    if (*panY < -maxY) *panY = -maxY;
    if (*panY > maxY) *panY = maxY;
  }
}

void pdfPanForAnchor(const PdfViewMap &map, double tapX, double tapY, int srcX, int srcY,
                     double *panX, double *panY) {
  double u, v;
  map.sourceToDisplay(srcX, srcY, &u, &v);
  const double ax = tapX - map.areaX;
  const double ay = tapY - map.areaY;
  const double offX = ax - u * map.scale;
  const double offY = ay - v * map.scale;
  *panX = offX - (map.areaW - map.dispW * map.scale) * 0.5;
  *panY = offY - (map.areaH - map.dispH * map.scale) * 0.5;
  pdfClampPan(map, panX, panY);
}
