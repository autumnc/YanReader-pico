// 主机端校验「真 16 级灰度写回」的量化/打包核心：DitherUtils.h。
//
// DitherUtils.h 是 header-only、只 include <stdint.h>，所以这里不需要任何桩件 —— 这正是
// 把 grayToLevel16 / setNibble / getNibble 放进那个头（而不是放进 DirectPixelWriter.h）
// 的理由：DirectPixelWriter.h 会把 GfxRenderer.h / HalDisplay.h 拖进来，主机上编不过。
//
// 覆盖的 7 组断言（见 tests/host/dither/README.md）：
//   [1] 值域        [2] 确定性      [3] None 档      [4] Ordered 档零和
//   [5] Row 档      [6] 单调性      [7] 4bpp 打包 round-trip
//
// 纯 g++ -std=c++17，不需要 IDF 环境、不需要设备。
#include <DitherUtils.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_checks = 0;
static int g_fail = 0;

#define CHECK(cond, ...)                                            \
  do {                                                              \
    g_checks++;                                                     \
    if (!(cond)) {                                                  \
      g_fail++;                                                     \
      std::printf("  FAIL %s:%d  ", __FILE__, __LINE__);            \
      std::printf(__VA_ARGS__);                                     \
      std::printf("\n");                                            \
    }                                                               \
  } while (0)

static const DitherMode kModes[3] = {DitherMode::Ordered, DitherMode::Row, DitherMode::None};
static const char *kModeNames[3] = {"Ordered", "Row", "None"};

// [1] 值域：任何灰值、任何坐标、三档，结果都必须是 0..15。
static void test_range() {
  for (int m = 0; m < 3; m++) {
    for (int g = 0; g < 256; g++) {
      for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
          DitherRowState st;
          const uint8_t lvl = grayToLevel16((uint8_t)g, x, y, kModes[m], st);
          if (lvl > 15) {
            CHECK(false, "%s g=%d x=%d y=%d -> %d", kModeNames[m], g, x, y, lvl);
            return;
          }
        }
      }
    }
  }
  CHECK(true, "range");
}

// [2] 确定性：Ordered 是纯 (gray,x,y) 的函数 —— 这是"默认选有序"的立论依据（设备端
// 每次重绘逐值相同，图案不会漂移），必须钉死。逐行正序扫与逐点乱序扫结果一致。
static void test_determinism() {
  // 同一 (gray,x,y) 重复调用逐值相同。
  for (int g = 0; g < 256; g += 7) {
    for (int y = 0; y < 8; y++) {
      for (int x = 0; x < 8; x++) {
        DitherRowState a, b;
        const uint8_t la = grayToLevel16((uint8_t)g, x, y, DitherMode::Ordered, a);
        // 中间插一堆别的调用，逼出"状态被带着走"的写法。
        DitherRowState noise;
        grayToLevel16((uint8_t)(g ^ 0x5A), x + 3, y + 1, DitherMode::Ordered, noise);
        const uint8_t lb = grayToLevel16((uint8_t)g, x, y, DitherMode::Ordered, b);
        CHECK(la == lb, "ordered not deterministic at g=%d x=%d y=%d (%d vs %d)", g, x, y, la, lb);
      }
    }
  }

  // 正序扫 vs 乱序扫：Ordered 档不该有任何跨像素状态。
  const int kSeq[][2] = {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {2, 1}, {0, 3}, {3, 3}, {1, 2}};
  DitherRowState fwd;
  std::vector<uint8_t> a;
  for (auto &p : kSeq) a.push_back(grayToLevel16(137, p[0], p[1], DitherMode::Ordered, fwd));
  for (int i = 7; i >= 0; i--) {
    DitherRowState rev;
    const uint8_t lvl = grayToLevel16(137, kSeq[i][0], kSeq[i][1], DitherMode::Ordered, rev);
    CHECK(lvl == a[i], "ordered scan-order dependent at idx %d", i);
  }
}

// [3] None 档：离最近一级不超过半个步长，端点钳死。
static void test_none() {
  for (int g = 0; g < 256; g++) {
    DitherRowState st;
    const uint8_t lvl = grayToLevel16((uint8_t)g, 5, 5, DitherMode::None, st);
    const int want = (g * 15 + 127) / 255;
    CHECK(lvl == want, "none g=%d -> %d want %d", g, lvl, want);
    const int diff = lvl * 17 - g;
    CHECK(diff >= -8 && diff <= 8, "none g=%d -> lvl %d (err %d)", g, lvl, diff);
  }
  DitherRowState st;
  CHECK(grayToLevel16(0, 0, 0, DitherMode::None, st) == 0, "none(0) != 0");
  CHECK(grayToLevel16(255, 0, 0, DitherMode::None, st) == 15, "none(255) != 15");
}

// [4] Ordered 档零和：一个 4x4 周期内 16 个 level 的均值与"理想级"相差不超过 1 级。
// 偏移写错比例（比如 (bayer-8)*5 那种 4 级时代的系数）会被这一条抓出来。
static void test_ordered_mean() {
  for (int g = 0; g < 256; g++) {
    DitherRowState st;
    int sum = 0;
    for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++)
        sum += grayToLevel16((uint8_t)g, x, y, DitherMode::Ordered, st);
    const double mean = sum / 16.0;
    const double ideal = g * 15.0 / 255.0;
    CHECK(mean >= ideal - 1.0 && mean <= ideal + 1.0, "ordered g=%d mean %.3f ideal %.3f", g, mean,
          ideal);
  }
}

// [5] Row 档：误差只向右、逐行/逐块自重置。
static void test_row() {
  const uint8_t seq[24] = {30, 130, 200, 12, 250, 90, 90, 90, 77, 240, 5, 111,
                           60,  180, 25,  220, 3,  199, 44, 133, 210, 8,  99, 170};

  // 同一灰序列在行 y 与 y+1 输出逐像素相同（误差绝不跨行）。
  for (int y = 0; y < 6; y++) {
    DitherRowState sa, sb;
    for (int x = 0; x < 24; x++) {
      const uint8_t la = grayToLevel16(seq[x], x, y, DitherMode::Row, sa);
      const uint8_t lb = grayToLevel16(seq[x], x, y + 1, DitherMode::Row, sb);
      CHECK(la == lb, "row y=%d vs y+1 differ at x=%d (%d vs %d)", y, x, la, lb);
      CHECK(sa.carry >= -8 && sa.carry <= 8, "row carry out of range: %d", sa.carry);
    }
  }

  // 行首像素 == None 档值（carry 从 0 起）；列不连续（块边界）处同样归零。
  for (int i = 0; i < 24; i++) {
    {
      DitherRowState rowSt, noneSt;
      const uint8_t a = grayToLevel16(seq[i], 0, 0, DitherMode::Row, rowSt);
      const uint8_t b = grayToLevel16(seq[i], 0, 0, DitherMode::None, noneSt);
      CHECK(a == b, "row row-start x=0 g=%d -> %d, none -> %d", seq[i], a, b);
    }
    {
      // 先在一个不相邻的列上踩一脚，模拟 JPEGDEC 的块列跳变。
      DitherRowState st;
      grayToLevel16(240, 0, 0, DitherMode::Row, st);
      grayToLevel16(240, 1, 0, DitherMode::Row, st);
      const uint8_t a = grayToLevel16(seq[i], 9, 0, DitherMode::Row, st);
      DitherRowState noneSt;
      const uint8_t b = grayToLevel16(seq[i], 9, 0, DitherMode::None, noneSt);
      CHECK(a == b, "row block-start x=9 g=%d -> %d, none -> %d (carry leaked)", seq[i], a, b);
    }
  }
}

// [6] 单调性：灰值升序时 level 非降（三档）。用"每步全新状态 + 行首"取纯函数的那个面。
static void test_monotonic() {
  for (int m = 0; m < 3; m++) {
    int prev = -1;
    for (int g = 0; g < 256; g++) {
      DitherRowState st;
      const int lvl = grayToLevel16((uint8_t)g, 0, 0, kModes[m], st);
      CHECK(lvl >= prev, "%s not monotonic at g=%d (%d < %d)", kModeNames[m], g, lvl, prev);
      prev = lvl;
    }
  }
}

// [7] 4bpp 半字节打包：往返一致、互不串扰、偶列落**低**半字节（与 epdiy framebuffer 的
// 约定一致 —— 这块屏的 4bpp 是 epdiy 原生的，不是自定的）。
static void test_packing() {
  for (int step = 1; step <= 3; step++) {
    for (int w = 1; w <= 64; w++) {
      std::vector<uint8_t> row((w + 1) / 2 + 1, 0xAA);
      for (int x = 0; x < w; x++) setNibble(row.data(), x, (uint8_t)((x * step) & 0x0F));
      for (int x = 0; x < w; x++) {
        const uint8_t want = (uint8_t)((x * step) & 0x0F);
        const uint8_t got = getNibble(row.data(), x);
        CHECK(got == want, "roundtrip w=%d x=%d -> %d want %d", w, x, got, want);
      }
      // 行占用的字节数 == (w+1)/2：最后一字节（w 为奇数时只用低半字节）之外不许被碰。
      const int bytes = (w + 1) / 2;
      CHECK(row[bytes] == 0xAA, "row overrun: byte %d touched (w=%d)", bytes, w);
    }
  }

  // 偶列低半字节 / 奇列高半字节，且写 x 不串扰 x±1。
  std::vector<uint8_t> row(4, 0x00);
  setNibble(row.data(), 0, 0xF);
  CHECK(row[0] == 0x0F, "even column must land in the LOW nibble (got 0x%02X)", row[0]);
  setNibble(row.data(), 1, 0xA);
  CHECK(row[0] == 0xAF, "odd column must land in the HIGH nibble (got 0x%02X)", row[0]);
  setNibble(row.data(), 2, 0x5);
  CHECK(row[1] == 0x05, "x=2 must write byte 1 low nibble (got 0x%02X)", row[1]);
  CHECK(getNibble(row.data(), 0) == 0xF && getNibble(row.data(), 1) == 0xA &&
            getNibble(row.data(), 2) == 0x5,
        "neighbour crosstalk");
}

int main() {
  std::printf("== dither / 16-level gray write-back ==\n");
  test_range();
  test_determinism();
  test_none();
  test_ordered_mean();
  test_row();
  test_monotonic();
  test_packing();
  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "PASS", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
