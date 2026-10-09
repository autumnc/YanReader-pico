// 主机端等价性对拍：main/gfx/diff_scan.h 的 fb_scan_diff_bytes（按 32 位字比、字不同才
// 展开成字节）与"逐字节扫描"逐位、逐序等价。
//
// 直接 include 真头文件 —— 它只依赖 <stdint.h>/<string.h>，所以测的就是上了机的那份代码，
// 不是抄一份。diff_bounding_rect 只是把这个位置序列再折成包围盒，位置一致则矩形一致。
//
// 编译运行：tests/host/diff_scan/run.sh（不需要 IDF 环境）。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include <array>

#include "../../../main/gfx/diff_scan.h"
#include "../../../main/gfx/fb_scan_window.h"

static int fails = 0;

// 参考实现：逐字节扫描，报告顺序（xb 递增、行递增）就是契约里承诺的顺序。
static void ref_scan(const uint8_t *a, const uint8_t *b, int row_bytes, int rows,
                     std::vector<std::pair<int, int>> &out) {
  for (int y = 0; y < rows; y++) {
    const uint8_t *ra = a + (size_t)y * (size_t)row_bytes;
    const uint8_t *rb = b + (size_t)y * (size_t)row_bytes;
    for (int xb = 0; xb < row_bytes; xb++) {
      if (ra[xb] != rb[xb]) out.emplace_back(xb, y);
    }
  }
}

// mode: 0 = 完全相同, 1 = 少量差异, 2 = 处处不同
static void check_case(int row_bytes, int rows, int mode, unsigned seed) {
  const size_t n = (size_t)row_bytes * (size_t)rows;
  std::vector<uint8_t> a(n), b(n);
  srand(seed);
  for (size_t i = 0; i < n; i++) a[i] = (uint8_t)(rand() & 0xFF);
  if (mode == 0) {
    b = a;
  } else if (mode == 1) {
    b = a;
    if (n > 0) {
      const int ndiff = 1 + (int)(rand() % 32);
      for (int k = 0; k < ndiff; k++) {
        const size_t p = (size_t)(rand() % (int)n);
        // 异或 1..255：结果必然 != a[p]，保证是真差异
        b[p] = (uint8_t)(a[p] ^ (1 + rand() % 255));
      }
    }
  } else {
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(rand() & 0xFF);
  }

  std::vector<std::pair<int, int>> got, want;
  fb_scan_diff_bytes(a.data(), b.data(), row_bytes, rows, [&](int xb, int y) { got.emplace_back(xb, y); });
  ref_scan(a.data(), b.data(), row_bytes, rows, want);

  if (got != want) {
    if (++fails <= 5) {
      printf("  FAIL row_bytes=%d rows=%d mode=%d seed=%u: got %zu 处, want %zu 处\n", row_bytes, rows,
             mode, seed, got.size(), want.size());
    }
  }
}

// 参考实现（区间版）：每行只扫 [xb0, xb1)，y 报的是**相对**行号（与真实现同口径）。
static void ref_scan_range(const uint8_t *a, const uint8_t *b, int row_bytes, int rows, int xb0,
                           int xb1, std::vector<std::pair<int, int>> &out) {
  if (xb0 < 0) xb0 = 0;
  if (xb1 > row_bytes) xb1 = row_bytes;
  for (int y = 0; y < rows; y++) {
    const uint8_t *ra = a + (size_t)y * (size_t)row_bytes;
    const uint8_t *rb = b + (size_t)y * (size_t)row_bytes;
    for (int xb = xb0; xb < xb1; xb++) {
      if (ra[xb] != rb[xb]) out.emplace_back(xb, y);
    }
  }
}

static void check_range_case(int row_bytes, int rows, int xb0, int xb1, unsigned seed) {
  const size_t n = (size_t)row_bytes * (size_t)rows;
  std::vector<uint8_t> a(n), b(n);
  srand(seed);
  for (size_t i = 0; i < n; i++) a[i] = (uint8_t)(rand() & 0xFF);
  b = a;
  for (int k = 0; k < 1 + (int)(rand() % 40); k++) {
    const size_t p = (size_t)(rand() % (int)n);
    b[p] = (uint8_t)(a[p] ^ (1 + rand() % 255));
  }

  std::vector<std::pair<int, int>> got, want;
  fb_scan_diff_bytes_range(a.data(), b.data(), row_bytes, rows, xb0, xb1,
                           [&](int xb, int y) { got.emplace_back(xb, y); });
  ref_scan_range(a.data(), b.data(), row_bytes, rows, xb0, xb1, want);

  if (got != want) {
    if (++fails <= 5) {
      printf("  FAIL range row_bytes=%d rows=%d [%d,%d) seed=%u: got %zu 处, want %zu 处\n",
             row_bytes, rows, xb0, xb1, seed, got.size(), want.size());
    }
  }
}

// ── 逻辑行段 → 物理扫描窗口（fb_scan_window.h）穷举对拍 ──────────────────────
// 判据是**双向**的：窗口里的字节 = "含至少一个段内像素的字节"的**精确**集合。
//   * 漏一个字节（段内像素没被扫到）→ 差分看不见 → 该驱动的区域不驱动 → 画面残留；
//   * 多一个字节（段外像素被扫到）→ 不错图，但把省下来的量吃掉一部分。
// 参考实现自己写一遍四种旋转的"物理 → 逻辑"映射（与 fb_scan_window.h 里的**逆**运算
// 是不同的算式，所以不是自证），逐像素判它在不在段内。
static void ref_phys_to_logical(int rot, int fb_w, int fb_h, int px, int py, int *lx, int *ly) {
  switch (rot) {
    case 0:  *lx = px;             *ly = py;             break;   // LANDSCAPE
    case 1:  *lx = py;             *ly = fb_w - 1 - px;  break;   // PORTRAIT
    case 2:  *lx = fb_w - 1 - px;  *ly = fb_h - 1 - py;  break;   // INVERTED_LANDSCAPE
    default: *lx = fb_h - 1 - py;  *ly = px;             break;   // INVERTED_PORTRAIT
  }
}

static long window_cases = 0;
static void check_window(int rot, int fb_w, int fb_h, int y0, int y1) {
  const int logical_h = (rot == 1 || rot == 3) ? fb_w : fb_h;
  const int yy1 = (y1 < 0 || y1 > logical_h) ? logical_h : y1;
  const int yy0 = y0 < 0 ? 0 : y0;
  const FbScanWindow w = fb_scan_window_for_rows(rot, fb_w, fb_h, y0, y1);
  window_cases++;
  for (int py = 0; py < fb_h; py++) {
    for (int xb = 0; xb < fb_w / 2; xb++) {
      bool want = false;
      if (yy0 < yy1) {
        for (int k = 0; k < 2 && !want; k++) {
          int lx, ly;
          ref_phys_to_logical(rot, fb_w, fb_h, xb * 2 + k, py, &lx, &ly);
          if (ly >= yy0 && ly < yy1) want = true;
        }
      }
      const bool got = py >= w.row_off && py < w.row_off + w.n_rows &&
                       xb >= w.xb0 && xb < w.xb1;
      if (got != want) {
        if (++fails <= 5) {
          printf("  FAIL window rot=%d fb=%dx%d [%d,%d): 字节(%d,%d) got=%d want=%d\n",
                 rot, fb_w, fb_h, y0, y1, xb, py, (int)got, (int)want);
        }
        return;   // 一个 band 报一次就够，别刷屏
      }
    }
  }
}

int main(void) {
  // 行长覆盖：1..8（含非 4 倍数、小于一个字长的）、以及实机两种物理行长。
  // 1216/2=608（横屏，4 的倍数）、684/2=342（竖屏，不是 4 的倍数 —— 尾部字节循环必须走到）。
  const int ROW_BYTES[] = {1, 2, 3, 4, 5, 6, 7, 8, 341, 342, 343, 608};
  const int ROWS[] = {1, 3, 7};

  long cases = 0;
  for (int rb : ROW_BYTES) {
    for (int rows : ROWS) {
      for (int mode = 0; mode < 3; mode++) {
        for (unsigned seed = 1; seed <= 4; seed++) {
          check_case(rb, rows, mode, seed);
          cases++;
        }
      }
    }
  }

  // 实机两种整屏尺寸各扫一遍（内容为"少量差异"，正是打字/翻页的真实形态）。
  check_case(608, 684, 1, 0xC0FFEE);
  check_case(342, 1216, 1, 0xBEEF01);
  check_case(608, 684, 2, 0x12345);
  cases += 3;

  // 区间版：空区间、越界夹取、非 8 倍数端点、整行、以及"面板顶线那段"几种典型形态。
  const int RB = 342;   // 竖屏行长（非 4 的倍数，尾部收尾会走到）
  for (int xb0 : {0, 1, 6, 7, 8, 100, 341, 342, 400, -5}) {
    for (int xb1 : {0, 1, 6, 8, 9, 100, 341, 342, 500, -1}) {
      check_range_case(RB, 5, xb0, xb1, 0x1000u + (unsigned)(xb0 * 7919 + xb1));
      cases++;
    }
  }
  check_range_case(608, 684, 0, 608, 0xABCD01);   // 整行 = 与整屏扫描同口径
  check_range_case(608, 684, 92, 304, 0xABCD02);  // 竖屏列段（px 184..608）
  cases += 2;

  // 小画幅穷举**所有**行段（端点取整最容易错的地方：段首/段尾奇偶、1 像素宽的段、
  // 贴边的段、空段），大画幅按代表性行段抽查（面板顶线 521 附近正是实机用到的那条）。
  for (int rot = 0; rot < 4; rot++) {
    const int W[] = {16, 18};
    const int H[] = {8, 7};
    for (int fi = 0; fi < 2; fi++) {
      const int fb_w = W[fi], fb_h = H[fi];
      const int logical_h = (rot == 1 || rot == 3) ? fb_w : fb_h;
      for (int y0 = 0; y0 <= logical_h; y0++) {
        for (int y1 = 0; y1 <= logical_h; y1++) check_window(rot, fb_w, fb_h, y0, y1);
      }
    }
    const std::array<std::array<int, 2>, 2> SIZES = {{{1216, 684}, {684, 1216}}};
    for (const std::array<int, 2> &fb : SIZES) {
      const int fb_w = fb[0], fb_h = fb[1];
      const int logical_h = (rot == 1 || rot == 3) ? fb_w : fb_h;
      for (int y0 : {0, 1, 2, 3, 4, 5, 520, 521, 522, 523, 600, logical_h - 2, logical_h - 1,
                     logical_h}) {
        for (int y1 : {0, 1, 2, 5, 6, 521, 522, 523, 524, 601, logical_h - 1, logical_h, -1}) {
          check_window(rot, fb_w, fb_h, y0, y1);
        }
      }
    }
  }

  if (fails == 0) {
    printf("OK  diff scan equivalent (%ld cases checked, %ld window cases)\n", cases, window_cases);
  } else {
    printf("FAILED  %d mismatches\n", fails);
  }
  return fails ? 1 : 0;
}
