#pragma once

// 逻辑行段 → 物理扫描窗口：把"逻辑上第 [y0, y1) 行"翻译成"物理帧缓冲里要扫哪一段"。
//
// 给差分快路用（main/ui_render.cpp 的 diff_bounding_rect_rows）：凡是能证明**某一段
// 逻辑行一定没变**的地方，就只需要扫剩下的那一段。省下的必须是**取数指令本身**——
// 只把回调写成提前 return 是没用的，整屏扫描照旧 29ms（见 diff_scan.h 的说明）。
//
// 四种旋转下"逻辑行段"在物理内存里的形态不同：
//   LANDSCAPE(0)          ly == py           → 物理**行段** [y0, y1)
//   PORTRAIT(1)           ly == w-1-px       → 每个物理行的**列段** px ∈ [w-y1, w-y0)
//   INVERTED_LANDSCAPE(2) ly == h-1-py       → 物理行段 [h-y1, h-y0)
//   INVERTED_PORTRAIT(3)  ly == px           → 每个物理行的列段 px ∈ [y0, y1)
//   （方向定义与 main/gfx/fb_fast.h 的 fb_rot_from_phys 同源；本固件实际只用到 0 和 3，
//     见 hw/board.cpp 的 board_rotate_live —— 另外两档一并实现，免得留个只在特定姿势下
//     才走到、又没人验过的洞。）
//
// 列段 → 字节段按半字节**精确**取整（不是放宽）：一个字节装 px = 2*xb 与 2*xb+1 两个像素，
// 所以"这个字节里有像素落在段内" ⟺ xb ∈ [floor(段首/2), ceil(段尾/2))。端点若取整放宽，
// 多扫的那一两个字节不会出错图，但会把"省下来的量"悄悄吃掉一部分，不如算准。
//
// 只依赖 <stdint.h> 式的基本类型，主机端测试（tests/host/diff_scan/）能原样 include，
// 所以测的就是上了机的那份算术。
//
// rot 的取值就是 epdiy 的 EpdRotation；ui_render.cpp 用 static_assert 钉住一致性。

struct FbScanWindow {
    int row_off;   // 从帧缓冲的第几行开始扫（相对行号，调用方要自己加回 row_off）
    int n_rows;    // 扫几行
    int xb0, xb1;  // 每行只扫字节区间 [xb0, xb1)
};
typedef struct FbScanWindow FbScanWindow;

// y1 < 0 视为"到逻辑屏底"；y0 >= y1 返回空窗口（n_rows <= 0）。
// 逻辑屏高 = 竖屏两档是 fb_w、横屏两档是 fb_h（旋转把宽高换了个个儿）。
inline FbScanWindow fb_scan_window_for_rows(int rot, int fb_w, int fb_h, int y0, int y1) {
    const int row_bytes = fb_w / 2;
    const int logical_h = (rot == 1 || rot == 3) ? fb_w : fb_h;
    if (y1 < 0 || y1 > logical_h) y1 = logical_h;
    if (y0 < 0) y0 = 0;

    FbScanWindow w = {0, 0, 0, row_bytes};
    if (y0 >= y1) return w;

    switch (rot) {
        case 0:   // LANDSCAPE
            w.row_off = y0;
            w.n_rows = y1 - y0;
            break;
        case 2:   // INVERTED_LANDSCAPE
            w.row_off = fb_h - y1;
            w.n_rows = y1 - y0;
            break;
        case 1:   // PORTRAIT：列段 [w-y1, w-y0)
            w.n_rows = fb_h;
            w.xb0 = (fb_w - y1) / 2;
            w.xb1 = (fb_w - y0 + 1) / 2;
            break;
        default:  // 3 = INVERTED_PORTRAIT：列段 [y0, y1)
            w.n_rows = fb_h;
            w.xb0 = y0 / 2;
            w.xb1 = (y1 + 1) / 2;
            break;
    }
    if (w.xb0 < 0) w.xb0 = 0;
    if (w.xb1 > row_bytes) w.xb1 = row_bytes;
    return w;
}
