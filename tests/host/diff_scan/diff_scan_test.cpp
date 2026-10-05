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

#include "../../../main/gfx/diff_scan.h"

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

  if (fails == 0) {
    printf("OK  diff scan equivalent (%ld cases checked)\n", cases);
  } else {
    printf("FAILED  %d mismatches\n", fails);
  }
  return fails ? 1 : 0;
}
