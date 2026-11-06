#pragma once
// ── 视图几何（纯函数，不碰屏幕也不碰文件）────────────────────────────────
// 缩放方式 + 用户倍率 + 平移 + 旋转 + 裁边 → 源像素与屏幕像素的互变换。
// 旋转在这里折进坐标映射，源 PXC 永远正向。
//
// 连续坐标约定：源像素 (x,y) 覆盖 [x,x+1)×[y,y+1)；显示坐标 u 向右、v 向下。
// 旋转角是**顺时针**度数（PDF /Rotate 正方向），0/90/180/270。

#include <string>

#include "PdfPxc.h"
#include "PdfTypes.h"

enum PdfZoomMode : int {
  kPdfZoomFitWidth = 0,
  kPdfZoomFitPage = 1,
  kPdfZoomOriginal = 2,
};

struct PdfViewSpec {
  int srcW = 0;
  int srcH = 0;
  int rotate = 0;  // 顺时针 0/90/180/270（页面固有 + 用户）
  bool crop = false;
  PdfPxcCropBox cropBox;  // 源像素坐标，crop=true 时有效
  int zoomMode = kPdfZoomFitPage;
  double zoom = 1.0;
  double panX = 0;  // 屏幕像素，相对居中位置
  double panY = 0;
  int areaX = 0, areaY = 0, areaW = 0, areaH = 0;  // 屏幕显示区
};

struct PdfViewMap {
  int dispW = 0, dispH = 0;  // 旋转后的显示尺寸（源像素）
  double scale = 1.0;        // 源像素 → 屏幕像素
  double offX = 0, offY = 0; // 内容左上角相对 area 原点
  int areaX = 0, areaY = 0, areaW = 0, areaH = 0;
  int rotate = 0;
  int cx0 = 0, cy0 = 0, cw = 0, ch = 0;  // 源裁剪区域（未旋转）

  // 屏幕 → 源像素；落在内容外返回 false。
  bool screenToSource(double sx, double sy, int *outX, int *outY) const;
  // 源像素中心 → 显示坐标（u,v，未加 offX/offY）。
  void sourceToDisplay(int srcX, int srcY, double *u, double *v) const;
  // 源像素中心 → 屏幕坐标。
  void sourceToScreen(int srcX, int srcY, double *sx, double *sy) const;
  double contentW() const { return dispW * scale; }
  double contentH() const { return dispH * scale; }
};

bool pdfComputeView(const PdfViewSpec &spec, PdfViewMap *out, PdfError *err, std::string *msg);

// 把 pan 限制在没有空边的范围内（内容小于显示区时归零）。
void pdfClampPan(const PdfViewMap &map, double *panX, double *panY);

// 让源像素 (srcX,srcY) 落在屏幕 (tapX,tapY)：返回新 pan。
void pdfPanForAnchor(const PdfViewMap &map, double tapX, double tapY, int srcX, int srcY,
                     double *panX, double *panY);

// 基准比例（不含用户 zoom），用于界面显示与双击基准还原。
double pdfBaseScale(const PdfViewSpec &spec);
