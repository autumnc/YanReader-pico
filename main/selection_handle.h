#pragma once

// 选区两端手柄的**几何**：一颗 ⌀2r 的圆，实心饼或空心环（环宽 ring，0 = 实心）。
//
// 阅读模式（GfxRenderer）和写作模式（u8g2）各有一套完全不同的绘图接口，但"手柄长什么样"
// 该是同一件事 —— 所以形状只在这儿写一遍，两边各自把 put(x, y, w) 接到自己那句
// "从 x 起画一条 w 像素宽的横线"上。**只画属于圆/环的那些像素**，圆内部那一片原样留着：
// 手柄画在反白块外面、常常正压在上一行的字上，先铺一遍内部就把那些字擦掉了。
//
// 逐行整数栅格化，不调各自的圆弧接口：GfxRenderer 的 fillRoundedRect/drawRoundedRect
// 是空桩（cornerRadius 被 (void) 掉之后直接走 fillRect，拿到的是方块），drawArc 实为
// epd_draw_circle 的 Bresenham **描边**；u8g2 的 DrawCircle 只有 1px 线宽，放大到 ⌀24
// 就成了一根发丝。自己算还有个好处：所有像素最终都过各自的逐像素出口，屏幕外的静默丢弃。
//
// put 会被调用若干次，每次是同一行上的一段：put(x, y, w)。
template <typename Put>
inline void selHandleRaster(int cx, int cy, int r, int ring, bool solid, Put &&put) {
  if (r <= 0) return;
  int ir = r - ring;   // 内圆半径；<=0 等于实心
  for (int dy = -r; dy <= r; dy++) {
    int hw = 0;
    while ((hw + 1) * (hw + 1) + dy * dy <= r * r) hw++;   // 本行半宽（整数，无浮点）
    // |dy| >= ir 这两行内圆已经收到 0 宽（|dy| == ir 是内圆与这一行的切点），必须整行填满
    // —— 照公式算会得到内半宽 0，框出一格 1px 的假孔。
    if (solid || ir <= 0 || (dy < 0 ? -dy : dy) >= ir) {
      put(cx - hw, cy + dy, hw * 2 + 1);
      continue;
    }
    int ihw = 0;
    while ((ihw + 1) * (ihw + 1) + dy * dy <= ir * ir) ihw++;
    put(cx - hw, cy + dy, hw - ihw);          // 左半圈
    put(cx + ihw + 1, cy + dy, hw - ihw);     // 右半圈
  }
}
