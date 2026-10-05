#pragma once

#include <stdint.h>

// 4x4 Bayer matrix for ordered dithering
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// ── 16 级灰度（0 = 全墨 … 15 = 全白）───────────────────────────────────────
// 这块面板的 framebuffer 是 epdiy 原生 4bpp（一字节两像素、**偶列低半字节 /
// 奇列高半字节**，见 epd_draw_pixel 与 main/gfx/fb_fast.h），灰度是原生的，
// 不缺显示能力。这里是"8 位灰 → 0..15 一级"的**唯一**落点：两条写回路径
// （DirectPixelWriter 到 framebuffer、DirectCacheWriter 到 .pxc）共用它，
// 缓存读回来时也只是把这 4 位原样搬回 framebuffer，不再二次量化。
//
// 历史：以前这里只有 applyBayerDither4Level（Bayer → 4 级），然后 4 级又被
// DirectPixelWriter 的 BW 映射压成 1 位 —— 插图等于阈值 192 的二值化，所以
// 又黑又硬。4 级那套已删除（见 Jpeg/PngToFramebufferConverter 的 writeSample）。
enum class DitherMode : uint8_t {
  Ordered = 0,  // 4x4 Bayer 有序抖动：纯 (gray,x,y) 的函数，同一像素重绘逐值相同
  Row = 1,      // 误差只向右传到**连续**的下一个像素，行/块边界即断
  None = 2,     // 直接量化到最近一级
};

// Row 档的横向误差状态。判据取"列是否连续"而不是"行号是否变了"：JPEGDEC 按
// MCU **列带**交付（同一行的不同块列之间 x 会跳回去），行号判据会漏掉块边界，
// 让误差从上一块的最右列漏进下一块的最左列、叠出竖向条带。列不连续就归零，
// 于是误差既不跨行、也不跨块 —— 这正是行扩散相对 FS 全向扩散的意义（用户
// 明确否决 FS：设备端每次重绘图案会漂移）。
struct DitherRowState {
  int carry = 0;
  int nextX = -1;  // 期望的下一个连续列（-1 = 谁都不连续，等价于已重置）
};

// 255 / 15 = 17，即一个量化级对应的灰度步长。量化取四舍五入（+127）。
inline uint8_t quantizeToLevel16(int v) {
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  return static_cast<uint8_t>((v * 15 + 127) / 255);
}

// 8 位灰 → 0..15。st 只有 Row 档会用到（其余档每次进来都把它清干净，免得
// 换档时带着上一档的残余误差）。
inline uint8_t grayToLevel16(uint8_t gray, int x, int y, DitherMode mode, DitherRowState& st) {
  if (mode == DitherMode::Row) {
    if (x != st.nextX) st.carry = 0;  // 行首 / 块首：误差不跨行、不跨块
    st.nextX = x + 1;
    const int v = static_cast<int>(gray) + st.carry;
    const uint8_t lvl = quantizeToLevel16(v);
    int err = v - static_cast<int>(lvl) * 17;
    if (err < -8) err = -8;
    if (err > 8) err = 8;
    st.carry = err;
    return lvl;
  }

  st.carry = 0;
  st.nextX = -1;

  if (mode == DitherMode::None) return quantizeToLevel16(gray);

  // Ordered：bayer 0..15 → 偏移 -8..+7（半个量化步长，与旧 4 级式的 (bayer-8)*5
  // 同比例）。16 个偏移之和为 -8，均值 -0.5 —— 相对 17 的步长可以忽略，所以
  // 大面积的平均灰度与 None 档一致，不会整体偏黑/偏白。
  const int b = bayer4x4[y & 3][x & 3];
  const int dither = (b - 8) * 17 / 16;
  return quantizeToLevel16(static_cast<int>(gray) + dither);
}

// ── 4bpp 半字节打包（.pxc 缓存）───────────────────────────────────────────
// 与 framebuffer 同一约定：偶列在低半字节、奇列在高半字节。写侧（解码时）与
// 读侧（渲染时）共用这两个函数，免得两处各写一遍 bitShift、各错一遍。
inline void setNibble(uint8_t* row, int x, uint8_t level) {
  uint8_t& b = row[x >> 1];
  if (x & 1) {
    b = static_cast<uint8_t>((b & 0x0F) | static_cast<uint8_t>((level & 0x0F) << 4));
  } else {
    b = static_cast<uint8_t>((b & 0xF0) | (level & 0x0F));
  }
}

inline uint8_t getNibble(const uint8_t* row, int x) {
  const uint8_t b = row[x >> 1];
  return (x & 1) ? static_cast<uint8_t>(b >> 4) : static_cast<uint8_t>(b & 0x0F);
}
