#pragma once

// 两块同尺寸帧缓冲的差分扫描：找出逐字节不同的位置，回调给调用方。
//
// 为什么单独抽出来：整屏 415,872 字节（1216/2 × 684），每个 UI 帧、每个阅读器虚拟
// 键盘按键都要跑一遍（ui_render.cpp 的 diff_bounding_rect）。逐字节比较是纯带宽浪费
// —— 内层先按 32 位字比，字不同才落回那 4 个字节逐个报。
//
// **契约**：回调报出的 (xb, y) 序列与"逐字节扫描"逐位相同（顺序也相同，xb 单调递增）。
// 换句话说这只是一次纯提速，不改变任何调用方能观察到的结果。主机端对拍见
// tests/host/diff_scan/。
//
// 头文件只依赖 <stdint.h> / <string.h>，为的是主机端测试能原样 include（不拖 epdiy
// 或本工程的任何头）。用 memcpy 取字，既不要求 a/b 对齐，也不碰严格别名。

#include <stdint.h>
#include <string.h>

// 对 a、b 的每一行（每行 row_bytes 字节、共 rows 行）扫描，凡 ra[xb] != rb[xb] 就调
// on_diff(xb, y)。a 与 b 的行距都假定为 row_bytes。
template <typename OnDiff>
inline void fb_scan_diff_bytes(const uint8_t* a, const uint8_t* b, int row_bytes, int rows, OnDiff on_diff) {
  for (int y = 0; y < rows; y++) {
    const uint8_t* ra = a + (size_t)y * (size_t)row_bytes;
    const uint8_t* rb = b + (size_t)y * (size_t)row_bytes;
    int xb = 0;
    for (; xb + 4 <= row_bytes; xb += 4) {
      uint32_t wa, wb;
      memcpy(&wa, ra + xb, sizeof wa);
      memcpy(&wb, rb + xb, sizeof wb);
      if (wa == wb) continue;
      // 字不同才展开成字节：报出的位置与逐字节扫描完全一致，不多也不少。
      for (int k = 0; k < 4; k++) {
        if (ra[xb + k] != rb[xb + k]) on_diff(xb + k, y);
      }
    }
    for (; xb < row_bytes; xb++) {
      if (ra[xb] != rb[xb]) on_diff(xb, y);
    }
  }
}
