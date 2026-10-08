// 主机探针：ProgressiveJpegScaled 的降尺度解 vs 参考解，逐像素对。
//
//   tests/host/jpeg/run.sh                    # 编 + 拿 fixture.jpg 按阈值判定
//   tests/host/jpeg/run.sh FILE.jpg [...]     # 只打统计（任意素材，不判定）
//   tests/host/jpeg/run.sh --bench            # 顺带报主机耗时
//   tests/host/jpeg/run.sh --ref-stb          # 参考换成 stb（没装 djpeg 时用）
//
// 参考取 **djpeg -scale 1/f -grayscale**，也就是 libjpeg 自己的降尺度解。为什么是它：
// 我们输出的是**亮度**（只折叠亮度分量的系数），djpeg 这条给的恰好是同一个量，所以
// 能逐像素对到 max ~2 —— 一对就说明折叠核、扫描同步、Q16 定标全对。
//
// 另一条路（stb 全解 → RGB → 77/151/28 → 按 f×f 箱平均，即 --ref-stb）看着更"独立"，
// 实际**不是同一个量**：它走的是 RGB→灰度换算，在彩色硬边处会掉几级。本 fixture 实测
// 差到 12 —— 逐点看，我们和 djpeg 只差 1，是 stb 那条离 djpeg 差了 11。所以默认不用它；
// 它只在没装 djpeg 时兜底，报出来的 mean 会虚高，别拿那些数下结论。
//
// 判定盯的是一个已经踩过的坑：**只留低频、丢掉折叠项**那一版。它每个像素看起来都
// "合理"，但整幅相当于多加了一道低通，实测 15%~22% 的像素差 >2 级、最大差 54。所以
// 看的是 mean 和 >5 的占比，不是"有没有解出来"。
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ProgressiveJpegScaled.h"
#include "stb_image.h"

namespace {

// ── 计数分配器：量真实峰值，顺便验 peakBytes() 是个上界 ────────────────────────
size_t g_live = 0, g_peak = 0;
void* cntAlloc(size_t n) {
  // 每块前面记一个长度头，只为了让峰值量得准一点（多算 16 字节，宁可偏大）。
  void* p = malloc(n + 16);
  if (!p) return nullptr;
  *static_cast<size_t*>(p) = n;
  g_live += n;
  if (g_live > g_peak) g_peak = g_live;
  return static_cast<char*>(p) + 16;
}
void cntFree(void* p) {
  if (!p) return;
  char* base = static_cast<char*>(p) - 16;
  g_live -= *reinterpret_cast<size_t*>(base);
  free(base);
}

std::vector<uint8_t> readFile(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return {};
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> v(static_cast<size_t>(n));
  if (fread(v.data(), 1, static_cast<size_t>(n), f) != static_cast<size_t>(n)) v.clear();
  fclose(f);
  return v;
}

// ── SOF 解析 ───────────────────────────────────────────────────────────────────
// peakBytes() 要的是源图的宽高和采样因子。设备上这些直接从 SOF 段读，这里也读真的，
// 免得"拿解出来的尺寸反推"把估算验成一句空话（反推会把源图算大，估算跟着变大，
// 上界检查就永远过了）。
struct Sof {
  int width = 0, height = 0, comps = 0;
  int yH = 1, yV = 1, hMax = 1, vMax = 1;
  bool progressive = false;
};

bool parseSof(const uint8_t* d, size_t n, Sof* s) {
  if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
  size_t i = 2;
  while (i + 3 < n) {
    if (d[i] != 0xFF) {
      i++;
      continue;
    }
    uint8_t m = d[i + 1];
    if (m == 0xFF) {
      i++;
      continue;
    }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
      i += 2;
      continue;
    }
    const size_t L = (static_cast<size_t>(d[i + 2]) << 8) | d[i + 3];
    if (L < 2 || i + 2 + L > n) return false;
    if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
      const uint8_t* p = d + i + 4;
      s->progressive = (m == 0xC2);
      s->height = (p[1] << 8) | p[2];
      s->width = (p[3] << 8) | p[4];
      s->comps = p[5];
      if (s->comps < 1) return false;
      // 第一个分量（亮度）的采样因子，以及全帧最大。
      s->yH = p[7] >> 4;
      s->yV = p[7] & 15;
      for (int c = 0; c < s->comps; c++) {
        const int h = p[7 + c * 3] >> 4, v = p[7 + c * 3] & 15;
        if (h > s->hMax) s->hMax = h;
        if (v > s->vMax) s->vMax = v;
      }
      return true;
    }
    if (m == 0xDA) return false;  // 走到 SOS 还没见 SOF，当解析失败
    i += 2 + L;
  }
  return false;
}

// 设备上那套灰度换算：单分量原样，彩色按 77/151/28（JPEG 的 YCbCr 亮度权重的整数版）。
inline uint8_t toGray(const uint8_t* p, int comp) {
  return comp == 1 ? p[0] : static_cast<uint8_t>((77u * p[0] + 151u * p[1] + 28u * p[2] + 128) >> 8);
}

// ── 参考一：djpeg -scale 1/f -grayscale（默认）──────────────────────────────────
// libjpeg 自己的降尺度解，出的就是这个尺度上的亮度 —— 和我们的输出是同一个量。
// PGM(P5) 从 stdout 读回来，头里的注释/空白按规范跳过。
bool refDjpeg(const char* path, int f, std::vector<uint8_t>* out, int* ow, int* oh) {
  char cmd[4096];
  snprintf(cmd, sizeof cmd, "djpeg -scale 1/%d -grayscale '%s' 2>/dev/null", f, path);
  FILE* p = popen(cmd, "r");
  if (!p) return false;
  auto nextInt = [&](int* v) -> bool {
    int c;
    // 跳空白与 '#' 注释行
    do {
      c = fgetc(p);
      if (c == '#')
        while (c != '\n' && c != EOF) c = fgetc(p);
    } while (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '#');
    if (c == EOF) return false;
    int n = 0;
    while (c >= '0' && c <= '9') {
      n = n * 10 + (c - '0');
      c = fgetc(p);
    }
    *v = n;
    return true;
  };
  char magic[3] = {0};
  if (fscanf(p, "%2s", magic) != 1 || strcmp(magic, "P5") != 0) {
    pclose(p);
    return false;
  }
  int w = 0, h = 0, maxv = 0;
  if (!nextInt(&w) || !nextInt(&h) || !nextInt(&maxv) || w <= 0 || h <= 0) {
    pclose(p);
    return false;
  }
  out->assign(static_cast<size_t>(w) * h, 0);
  const size_t got = fread(out->data(), 1, static_cast<size_t>(w) * h, p);
  pclose(p);
  if (got != static_cast<size_t>(w) * h) {
    out->clear();
    return false;
  }
  *ow = w;
  *oh = h;
  return true;
}

// ── 参考二：stb 全解 → 灰度 → 按 f×f 箱平均（--ref-stb，兜底）──────────────────
// 只精确到"另一套灰度口径"：彩色封面在硬色边处与本解码器（亮度）差好几级，见文件头。
std::vector<uint8_t> refStb(const uint8_t* data, size_t len, int f, int* ow, int* oh) {
  int w = 0, h = 0, comp = 0;
  uint8_t* src = stbi_load_from_memory(data, static_cast<int>(len), &w, &h, &comp, 0);
  if (!src) return {};
  const int rw = (w + f - 1) / f, rh = (h + f - 1) / f;
  std::vector<uint8_t> out(static_cast<size_t>(rw) * rh);
  for (int y = 0; y < rh; y++) {
    const int y0 = y * f, y1 = (y + 1) * f < h ? (y + 1) * f : h;
    for (int x = 0; x < rw; x++) {
      const int x0 = x * f, x1 = (x + 1) * f < w ? (x + 1) * f : w;
      uint32_t sum = 0, cnt = 0;
      for (int yy = y0; yy < y1; yy++) {
        const uint8_t* row = src + static_cast<size_t>(yy) * w * comp;
        for (int xx = x0; xx < x1; xx++) {
          sum += toGray(row + static_cast<size_t>(xx) * comp, comp);
          cnt++;
        }
      }
      out[static_cast<size_t>(y) * rw + x] = cnt ? static_cast<uint8_t>(sum / cnt) : 0;
    }
  }
  stbi_image_free(src);
  *ow = rw;
  *oh = rh;
  return out;
}

// 判定阈值。留了 2~4 倍余量：正常值见 README 的表（mean 0.1~0.6、>5 基本为 0），
// 而"丢掉折叠项"那一版是 mean 2~6、>2 有 15%~22%，怎么都盖不住。
constexpr double kMaxMean = 1.0;
constexpr double kMaxOver5Pct = 0.10;

struct Options {
  bool check = false;
  bool bench = false;
  bool quiet = false;
  bool refStb = false;
  const char* dumpDir = nullptr;
};

bool one(const char* path, const Sof& sof, int scaleLog2, const Options& opt, int* failures) {
  const std::vector<uint8_t> buf = readFile(path);
  if (buf.empty()) {
    printf("    read fail\n");
    return false;
  }
  const int f = 1 << scaleLog2;

  double bestMs = 1e9;
  size_t peak = 0;
  pjscaled::Plane pl;
  const int reps = opt.bench ? 4 : 1;
  for (int rep = 0; rep < reps; rep++) {
    g_live = g_peak = 0;
    pjscaled::Plane p;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = pjscaled::decodeGray(buf.data(), buf.size(), scaleLog2, {cntAlloc, cntFree}, &p);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!ok) {
      printf("    scale 1/%d : DECLINED\n", f);
      if (g_live != 0) {  // 拒绝也不能漏：内部缓冲得自己还回去
        printf("         !! 拒绝路上漏了 %zu B\n", g_live);
        (*failures)++;
      }
      return true;  // 拒绝不是失败：调用方会落回 JPEGDEC 的老路
    }
    if (rep == 0) {
      pl = p;  // 第一遍留着做逐像素比对
      peak = g_peak;
    } else {
      cntFree(p.pixels);
    }
    if (ms < bestMs) bestMs = ms;
  }

  int rw = 0, rh = 0;
  std::vector<uint8_t> ref;
  const char* which = opt.refStb ? "stb" : "djpeg";
  if (opt.refStb) {
    ref = refStb(buf.data(), buf.size(), f, &rw, &rh);
  } else {
    refDjpeg(path, f, &ref, &rw, &rh);
  }
  if (ref.empty()) {
    printf("    scale 1/%d : 我们的解出来了，%s 参考却解不出来\n", f, which);
    cntFree(pl.pixels);
    return false;
  }

  long long sum = 0, maxd = 0, over2 = 0, over5 = 0, n = 0;
  const int cw = pl.width < rw ? pl.width : rw;
  const int ch = pl.height < rh ? pl.height : rh;
  for (int y = 0; y < ch; y++) {
    const uint8_t* a = pl.pixels + static_cast<size_t>(y) * pl.stride;
    const uint8_t* b = ref.data() + static_cast<size_t>(y) * rw;
    for (int x = 0; x < cw; x++) {
      const long long d = std::labs(static_cast<long>(a[x]) - static_cast<long>(b[x]));
      sum += d;
      if (d > maxd) maxd = d;
      if (d > 2) {
        over2++;
        if (d > 5) over5++;
      }
      n++;
    }
  }
  const double mean = static_cast<double>(sum) / static_cast<double>(n);
  const double pct5 = 100.0 * static_cast<double>(over5) / static_cast<double>(n);

  printf("    scale 1/%d : %dx%d (参考 %dx%d，比 %dx%d)  mean=%.3f max=%lld  >2:%.3f%%  >5:%.3f%%\n", f,
         pl.width, pl.height, rw, rh, cw, ch, mean, maxd, 100.0 * static_cast<double>(over2) / n, pct5);
  if (opt.bench) printf("         主机最快 %.1f ms\n", bestMs);

  // peakBytes 只认 SOF 那几项，这里喂**真的**源图参数，验它是上界。
  const size_t est =
      pjscaled::peakBytes(sof.width, sof.height, sof.yH, sof.yV, sof.hMax, sof.vMax, scaleLog2);
  printf("         实测峰值 %zu B / 估算 %zu B %s\n", peak, est, peak <= est ? "OK" : "!! 估算偏低");

  // 书内插图那条路拿不到采样因子，走的是 peakBytesForImage（自己读 SOF）。两者必须一致，
  // 否则那条路要么白试一档、要么算小了。（判定放在下面 ok 声明之后。）
  const size_t estFromFile = pjscaled::peakBytesForImage(buf.data(), buf.size(), scaleLog2);

  if (opt.dumpDir) {
    char nm[512];
    char base[256];
    snprintf(base, sizeof base, "%s", path);
    for (char* q = base; *q; q++)
      if (*q == '/') *q = '_';
    snprintf(nm, sizeof nm, "%s/%s_%d.pgm", opt.dumpDir, base, f);
    if (FILE* fo = fopen(nm, "wb")) {
      fprintf(fo, "P5\n%d %d\n255\n", pl.width, pl.height);
      for (int y = 0; y < pl.height; y++)
        fwrite(pl.pixels + static_cast<size_t>(y) * pl.stride, 1, static_cast<size_t>(pl.width), fo);
      fclose(fo);
    }
    snprintf(nm, sizeof nm, "%s/%s_%d_ref.pgm", opt.dumpDir, base, f);
    if (FILE* fo = fopen(nm, "wb")) {
      fprintf(fo, "P5\n%d %d\n255\n", rw, rh);
      fwrite(ref.data(), 1, static_cast<size_t>(rw) * rh, fo);
      fclose(fo);
    }
  }

  bool ok = true;
  if (estFromFile != est) {
    printf("         !! peakBytesForImage 读出 %zu，与 peakBytes %zu 不一致\n", estFromFile, est);
    ok = false;
  }
  if (opt.check) {
    if ((pl.width != rw || pl.height != rh) && !opt.quiet)
      printf("         !! 尺寸与参考不一致\n");
    if (mean > kMaxMean) {
      printf("         !! mean %.3f > %.2f\n", mean, kMaxMean);
      ok = false;
    }
    if (pct5 > kMaxOver5Pct) {
      printf("         !! >5 占比 %.3f%% > %.2f%%\n", pct5, kMaxOver5Pct);
      ok = false;
    }
    if (peak > est) {
      printf("         !! peakBytes 估算偏低：实测 %zu > 估算 %zu\n", peak, est);
      ok = false;
    }
    if (!ok) (*failures)++;
  }
  cntFree(pl.pixels);
  if (g_live != 0) {  // 还完 plane 之后应该一分不剩（acc 等内部缓冲全靠解码器自己还）
    printf("         !! 解完漏了 %zu B\n", g_live);
    if (opt.check) (*failures)++;
    ok = false;
  }
  return ok;
}

// 降尺度档的选择阈值（chooseScaleForTarget）。纯算术，直接摆一张表对拍；
// 书内插图那条路就靠它决定"值不值得自己解"，判错会白花几倍时间或白糊。
void checkScalePolicy(int* failures) {
  struct Case {
    int sw, sh, dw, dh, want;
    const char* note;
  };
  const Case cases[] = {
      {1200, 1600, 500, 700, 1, "封面整页：源/目标=2，1/2"},          // fit=2
      {1200, 1600, 600, 800, 1, "正好 1/2：fit=2 仍不值得省"},
      {1200, 1600, 601, 800, 1, "fit=1（目标比源/2 还大）"},
      {1200, 1600, 300, 400, 2, "fit=4：1/4 就够"},
      {1200, 1600, 301, 400, 1, "fit=3：1/4 会放大，改用 1/2"},
      {1200, 1600, 150, 200, 0, "fit=8：1/8 本就够，别自己解"},
      {1200, 1600, 151, 200, 2, "fit=7：宽刚过 1/8 边界一格，就得自己解"},
      {2644, 3840, 396, 528, 2, "祖堂集封面缩略图：fit=6 → 1/4"},
      {400, 300, 400, 300, 1, "小图原尺寸：fit=1，仍走 1/2（比 1/8 强）"},
      {0, 0, 100, 100, 1, "退化输入不炸（fit=0）"},
      {1200, 1600, 0, 0, 0, "目标为 0 视为不需要"},
  };
  int bad = 0;
  for (const Case& c : cases) {
    const int got = pjscaled::chooseScaleForTarget(c.sw, c.sh, c.dw, c.dh);
    if (got != c.want) {
      printf("  scale policy: src %dx%d -> dst %dx%d 得 %d，应 %d  (%s)\n", c.sw, c.sh, c.dw, c.dh, got, c.want,
             c.note);
      bad++;
    }
  }
  if (bad) printf(">> FAIL: scale policy %d 项不符\n", bad);
  else printf(">> ok: scale policy %zu 例全对\n", sizeof(cases) / sizeof(cases[0]));
  *failures += bad;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  std::vector<const char*> files;
  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (!strcmp(a, "--check")) opt.check = true;
    else if (!strcmp(a, "--bench")) opt.bench = true;
    else if (!strcmp(a, "--quiet")) opt.quiet = true;
    else if (!strcmp(a, "--ref-stb")) opt.refStb = true;
    else if (!strcmp(a, "--dump") && i + 1 < argc) opt.dumpDir = argv[++i];
    else files.push_back(a);
  }
  if (files.empty()) {
    fprintf(stderr,
            "usage: probe [--check] [--bench] [--quiet] [--ref-stb] [--dump DIR] FILE.jpg [...]\n");
    return 2;
  }
  int failures = 0;
  if (opt.check) checkScalePolicy(&failures);
  for (const char* f : files) {
    const std::vector<uint8_t> buf = readFile(f);
    Sof sof;
    if (buf.empty() || !parseSof(buf.data(), buf.size(), &sof)) {
      printf("%s: 读不出 SOF，跳过\n", f);
      continue;
    }
    printf("%s  (%dx%d %s%s)\n", f, sof.width, sof.height,
           sof.progressive ? "渐进式" : "基线", sof.comps == 1 ? " 灰度" : "");
    one(f, sof, 1, opt, &failures);  // 1/2
    one(f, sof, 2, opt, &failures);  // 1/4
    one(f, sof, 3, opt, &failures);  // 1/8
  }
  if (opt.check && failures) {
    printf(">> FAIL: %d 项超出阈值\n", failures);
    return 1;
  }
  if (opt.check) printf(">> ok: 全部在阈值内\n");
  return 0;
}
