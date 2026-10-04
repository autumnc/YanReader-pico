#pragma once

#include <HalStorage.h>

class Print;

class PngToBmpConverter {
  // gray8: 输出 8 位灰阶 BMP（封面走这条），否则按 oneBit 出 1/2 位。
  // / gray8: emit an 8-bit grayscale BMP (the cover path); otherwise 1/2-bit per oneBit.
  static bool pngFileToBmpStreamInternal(HalFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight,
                                         bool oneBit, bool crop = true, bool preserveTransparency = false,
                                         bool gray8 = false);
  static bool pngFileToBmpFileInternal(const char* pngPath, const char* bmpPath, bool crop, bool preserveTransparency);

 public:
  static bool pngFileToBmpFile(const char* pngPath, const char* bmpPath, bool crop = true);
  static bool pngFileToTransparentBmpFile(const char* pngPath, const char* bmpPath, bool crop = true);
  static bool pngFileToBmpStream(HalFile& pngFile, Print& bmpOut, bool crop = true);
  // gray8/crop 的理由同 JpegToBmpConverter::jpegFileToBmpStreamWithSize：默认值 =
  // 老行为（2 位 + 裁切），待机整屏封面传 gray8=true、crop=false，按待机框 fit 解一次。
  static bool pngFileToBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                         bool gray8 = false, bool crop = true);
  static bool pngFileTo1BitBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
};
