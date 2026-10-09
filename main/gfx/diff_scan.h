#pragma once

// 两块同尺寸帧缓冲的差分扫描：找出逐字节不同的位置，回调给调用方。
//
// 为什么单独抽出来：整屏 415,872 字节（1216/2 × 684），每个 UI 帧、每个阅读器虚拟
// 键盘按键都要跑一遍（ui_render.cpp 的 diff_bounding_rect）。逐字节比较是纯带宽浪费
// —— 内层先按 64 位字比，字不同才落回那 8 个字节逐个报。
//
// **为什么是 64 位而不是 32 位**（2026-10-09 实机微基准，ESP32-S3 + PSRAM）：
// 整屏 831KB（两块 415,872B）扫字实测 32 位/次 **41ms**、64 位/次 **22ms**、
// 16 字节/次（4 条独立 32 位装填）**40ms**。16 字节档没比 32 位快，说明瓶颈不是
// "在飞的请求数"而是**每条取数指令都要付满一次 PSRAM 延迟**（≈200ns，两路完全不
// 重叠）；取宽一倍就正好省一半。别再往上加宽，实测无效。
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
//
// 每行只扫字节区间 [xb0, xb1)。**这不是给回调加过滤**：瓶颈既然是"每条取数指令付满一次
// PSRAM 延迟"，那就必须真的**不碰**区间外的字节，否则一次省钱都省不到 —— 只把回调写成
// 提前 return，取数照旧发生，时间一分不少（2026-10-09 实测：同样的行段过滤写在回调里，
// 整屏扫描仍是 29ms）。
template <typename OnDiff>
inline void fb_scan_diff_bytes_range(const uint8_t* a, const uint8_t* b, int row_bytes, int rows,
                                     int xb0, int xb1, OnDiff on_diff) {
  if (xb0 < 0) xb0 = 0;
  if (xb1 > row_bytes) xb1 = row_bytes;
  if (xb0 >= xb1) return;
  for (int y = 0; y < rows; y++) {
    const uint8_t* ra = a + (size_t)y * (size_t)row_bytes;
    const uint8_t* rb = b + (size_t)y * (size_t)row_bytes;
    int xb = xb0;
    for (; xb + 8 <= xb1; xb += 8) {
      uint64_t wa, wb;
      memcpy(&wa, ra + xb, sizeof wa);
      memcpy(&wb, rb + xb, sizeof wb);
      if (wa == wb) continue;
      // 字不同才展开成字节：报出的位置与逐字节扫描完全一致，不多也不少。
      for (int k = 0; k < 8; k++) {
        if (ra[xb + k] != rb[xb + k]) on_diff(xb + k, y);
      }
    }
    // 尾部：先 32 位（竖屏行长 342 = 8*42+6，省下 4 个字节的逐字节），再逐字节收尾。
    for (; xb + 4 <= xb1; xb += 4) {
      uint32_t wa, wb;
      memcpy(&wa, ra + xb, sizeof wa);
      memcpy(&wb, rb + xb, sizeof wb);
      if (wa == wb) continue;
      for (int k = 0; k < 4; k++) {
        if (ra[xb + k] != rb[xb + k]) on_diff(xb + k, y);
      }
    }
    for (; xb < xb1; xb++) {
      if (ra[xb] != rb[xb]) on_diff(xb, y);
    }
  }
}

// 整行扫描（区间 = 整行）。位置序列与逐字节扫描逐位相同。
template <typename OnDiff>
inline void fb_scan_diff_bytes(const uint8_t* a, const uint8_t* b, int row_bytes, int rows, OnDiff on_diff) {
  fb_scan_diff_bytes_range(a, b, row_bytes, rows, 0, row_bytes, on_diff);
}
