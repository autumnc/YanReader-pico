#pragma once

#include <HalStorage.h>

class Print;
class ZipFile;

class JpegToBmpConverter {
 public:
  enum class Output { Mono1, Gray2, Gray8 };

 private:
  static bool jpegFileToBmpStreamInternal(HalFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          Output output, bool crop = true);

 public:
  static bool jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop = true, Output output = Output::Gray2);
  // 自定义目标盒子 + 输出位深 + 是否裁切填充（默认值 = 老行为：2 位灰阶 + 裁切）。
  // 待机整屏封面（见 screen_reader.cpp 的 standbyCoverPathFor）走 Gray8 + crop=false：
  // 按待机框解一次、之后 1:1 上屏。那个 396×528 的固定上限是给书架格子的，整屏
  // 用它就等于拿缩略图放大 —— 所以尺寸必须能从外面给。fit 而不是 fill 是因为待机
  // 一屏只有这张封面，裁掉两边会把封面上的书名/作者切掉。
  // Convert with custom target size (thumbnails; the standby screen cover passes Gray8 + fit).
  static bool jpegFileToBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                          Output output = Output::Gray2, bool crop = true);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                              int targetMaxHeight);
};
