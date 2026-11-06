#include "PdfJbig2.h"

#include <cstring>

// PdfScratch.h 末尾会 #undef 这个宏；这里自己再探一次，否则大图日志和
// 内存不足诊断会在设备上被静默编掉（现象是设备日志里看不到 PdfJbig2 字样）。
#if !defined(SIMULATOR) && !defined(CROSSPOINT_EMULATED) && __has_include(<esp_heap_caps.h>)
#include <esp_log.h>
#define PDF_JBIG2_LOG 1
#endif

namespace {

// 上限：坏流最先撞到这些数，而不是 OOM 或死循环。
constexpr uint32_t kMaxSegments = 4096;
constexpr uint32_t kMaxSymbols = 65536;          // 单条流的新符号数
constexpr uint32_t kMaxInstances = 1000000;      // 文本区域实例数
constexpr uint64_t kMaxGlyphPixels = 32ull * 1024 * 1024;  // 全部符号位图像素
constexpr uint32_t kMaxNodesBeforeAbort = 1000;  // 连续空导出段的护栏

void setErr(PdfError *err, std::string *msg, PdfError e, const char *m) {
  if (err) *err = e;
  if (msg) *msg = m ? m : "";
}

inline uint32_t rd32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

inline uint16_t rd16(const uint8_t *p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline int bitAt(const uint8_t *row, int x) {
  return (row[x >> 3] >> (7 - (x & 7))) & 1;
}

// 行内取位，越界返回 0（与 jbig2_image_get_pixel 一致）。
inline int rowBit(const uint8_t *row, int x, int w) {
  if (!row || x < 0 || x >= w) return 0;
  return (row[x >> 3] >> (7 - (x & 7))) & 1;
}

// ── MQ 算术解码器（jbig2_arith.c 逐位镜像）──────────────────────────────
struct QeRow {
  uint16_t Qe;
  uint8_t mps_xor, lps_xor;
};

#define MPS(i, n) ((i) ^ (n))
#define LPS(i, n, s) ((i) ^ (n) ^ ((s) << 7))

const QeRow kQe[47] = {
    {0x5601, MPS(0, 1), LPS(0, 1, 1)},     {0x3401, MPS(1, 2), LPS(1, 6, 0)},
    {0x1801, MPS(2, 3), LPS(2, 9, 0)},     {0x0AC1, MPS(3, 4), LPS(3, 12, 0)},
    {0x0521, MPS(4, 5), LPS(4, 29, 0)},    {0x0221, MPS(5, 38), LPS(5, 33, 0)},
    {0x5601, MPS(6, 7), LPS(6, 6, 1)},     {0x5401, MPS(7, 8), LPS(7, 14, 0)},
    {0x4801, MPS(8, 9), LPS(8, 14, 0)},    {0x3801, MPS(9, 10), LPS(9, 14, 0)},
    {0x3001, MPS(10, 11), LPS(10, 17, 0)}, {0x2401, MPS(11, 12), LPS(11, 18, 0)},
    {0x1C01, MPS(12, 13), LPS(12, 20, 0)}, {0x1601, MPS(13, 29), LPS(13, 21, 0)},
    {0x5601, MPS(14, 15), LPS(14, 14, 1)}, {0x5401, MPS(15, 16), LPS(15, 14, 0)},
    {0x5101, MPS(16, 17), LPS(16, 15, 0)}, {0x4801, MPS(17, 18), LPS(17, 16, 0)},
    {0x3801, MPS(18, 19), LPS(18, 17, 0)}, {0x3401, MPS(19, 20), LPS(19, 18, 0)},
    {0x3001, MPS(20, 21), LPS(20, 19, 0)}, {0x2801, MPS(21, 22), LPS(21, 19, 0)},
    {0x2401, MPS(22, 23), LPS(22, 20, 0)}, {0x2201, MPS(23, 24), LPS(23, 21, 0)},
    {0x1C01, MPS(24, 25), LPS(24, 22, 0)}, {0x1801, MPS(25, 26), LPS(25, 23, 0)},
    {0x1601, MPS(26, 27), LPS(26, 24, 0)}, {0x1401, MPS(27, 28), LPS(27, 25, 0)},
    {0x1201, MPS(28, 29), LPS(28, 26, 0)}, {0x1101, MPS(29, 30), LPS(29, 27, 0)},
    {0x0AC1, MPS(30, 31), LPS(30, 28, 0)}, {0x09C1, MPS(31, 32), LPS(31, 29, 0)},
    {0x08A1, MPS(32, 33), LPS(32, 30, 0)}, {0x0521, MPS(33, 34), LPS(33, 31, 0)},
    {0x0441, MPS(34, 35), LPS(34, 32, 0)}, {0x02A1, MPS(35, 36), LPS(35, 33, 0)},
    {0x0221, MPS(36, 37), LPS(36, 34, 0)}, {0x0141, MPS(37, 38), LPS(37, 35, 0)},
    {0x0111, MPS(38, 39), LPS(38, 36, 0)}, {0x0085, MPS(39, 40), LPS(39, 37, 0)},
    {0x0049, MPS(40, 41), LPS(40, 38, 0)}, {0x0025, MPS(41, 42), LPS(41, 39, 0)},
    {0x0015, MPS(42, 43), LPS(42, 40, 0)}, {0x0009, MPS(43, 44), LPS(43, 41, 0)},
    {0x0005, MPS(44, 45), LPS(44, 42, 0)}, {0x0001, MPS(45, 45), LPS(45, 43, 0)},
    {0x5601, MPS(46, 46), LPS(46, 46, 0)},
};

struct WordStream {
  const uint8_t *data = nullptr;
  size_t size = 0;
  // 从 off 起最多取 4 字节拼成大端 word；返回实际字节数。
  int get(size_t off, uint32_t *word) const {
    uint32_t v = 0;
    int ret = 0;
    if (off < size) {
      v |= static_cast<uint32_t>(data[off]) << 24;
      ret++;
    }
    if (off + 1 < size) {
      v |= static_cast<uint32_t>(data[off + 1]) << 16;
      ret++;
    }
    if (off + 2 < size) {
      v |= static_cast<uint32_t>(data[off + 2]) << 8;
      ret++;
    }
    if (off + 3 < size) {
      v |= data[off + 3];
      ret++;
    }
    *word = v;
    return ret;
  }
};

struct MQ {
  WordStream ws;
  uint32_t C = 0, A = 0;
  int CT = 0;
  uint32_t nextWord = 0;
  size_t nextWordBytes = 0;
  bool failed = false;
  size_t offset = 0;

  int bytein() {
    if (failed) return -1;
    if (nextWordBytes == 0) return -1;
    uint8_t B = static_cast<uint8_t>((nextWord >> 24) & 0xFF);
    if (B == 0xFF) {
      if (nextWordBytes <= 1) {
        uint32_t w = 0;
        const int ret = ws.get(offset, &w);
        if (ret < 0) {
          failed = true;
          return -1;
        }
        nextWord = w;
        nextWordBytes = static_cast<size_t>(ret);
        if (nextWordBytes == 0) {
          nextWord = 0xFF900000;
          nextWordBytes = 2;
          C += 0xFF00;
          CT = 8;
          return 0;
        }
        offset += nextWordBytes;
        const uint8_t B1 = static_cast<uint8_t>((nextWord >> 24) & 0xFF);
        if (B1 > 0x8F) {
          CT = 8;
          nextWord = 0xFF000000u | (nextWord >> 8);
          nextWordBytes = 2;
          offset--;
        } else {
          C += 0xFE00 - (static_cast<uint32_t>(B1) << 9);
          CT = 7;
        }
      } else {
        const uint8_t B1 = static_cast<uint8_t>((nextWord >> 16) & 0xFF);
        if (B1 > 0x8F) {
          CT = 8;
        } else {
          nextWordBytes--;
          nextWord <<= 8;
          C += 0xFE00 - (static_cast<uint32_t>(B1) << 9);
          CT = 7;
        }
      }
    } else {
      nextWord <<= 8;
      nextWordBytes--;
      if (nextWordBytes == 0) {
        uint32_t w = 0;
        const int ret = ws.get(offset, &w);
        if (ret < 0) {
          failed = true;
          return -1;
        }
        nextWord = w;
        nextWordBytes = static_cast<size_t>(ret);
        if (nextWordBytes == 0) {
          nextWord = 0xFF900000;
          nextWordBytes = 2;
          C += 0xFF00;
          CT = 8;
          return 0;
        }
        offset += nextWordBytes;
      }
      B = static_cast<uint8_t>((nextWord >> 24) & 0xFF);
      C += 0xFF00 - (static_cast<uint32_t>(B) << 8);
      CT = 8;
    }
    return 0;
  }

  __attribute__((always_inline)) inline int renormd() {
    do {
      if (CT == 0 && bytein() < 0) return -1;
      A <<= 1;
      C <<= 1;
      CT--;
    } while ((A & 0x8000) == 0);
    return 0;
  }

  void init() {
    offset = 0;
    failed = false;
    uint32_t w = 0;
    const int ret = ws.get(0, &w);
    nextWord = w;
    nextWordBytes = static_cast<size_t>(ret);
    if (nextWordBytes == 0) {
      failed = true;
      return;
    }
    offset += nextWordBytes;
    C = (~(nextWord >> 8)) & 0xFF0000;
    if (bytein() < 0) {
      failed = true;
      return;
    }
    C <<= 7;
    CT -= 7;
    A = 0x8000;
  }

  __attribute__((always_inline)) inline int decode(uint8_t *pcx) {
    uint8_t cx = *pcx;
    const unsigned idx = cx & 0x7f;
    if (idx >= 47) return -1;
    const QeRow *q = &kQe[idx];
    const uint32_t qe = q->Qe;
    int D;
    A -= qe;
    if ((C >> 16) < A) {
      if ((A & 0x8000) == 0) {
        if (A < qe) {
          D = 1 - (cx >> 7);
          *pcx ^= q->lps_xor;
        } else {
          D = cx >> 7;
          *pcx ^= q->mps_xor;
        }
        if (renormd() < 0) return -1;
        return D;
      }
      return cx >> 7;
    }
    C -= A << 16;
    if (A < qe) {
      A = qe;
      D = cx >> 7;
      *pcx ^= q->mps_xor;
    } else {
      A = qe;
      D = 1 - (cx >> 7);
      *pcx ^= q->lps_xor;
    }
    if (renormd() < 0) return -1;
    return D;
  }
};

// ── 算术整数 / IAID（Annex A）────────────────────────────────────────────
struct ArithIntCtx {
  uint8_t IAx[512];
  ArithIntCtx() { memset(IAx, 0, sizeof(IAx)); }
};

// -1 = 出错，0 = 正常值，1 = OOB。
int intDecode(MQ &mq, ArithIntCtx &ai, int32_t *out) {
  uint8_t *IAx = ai.IAx;
  int PREV = 1;
  int S = mq.decode(&IAx[PREV]);
  if (S < 0) return -1;
  PREV = (PREV << 1) | S;
  int bit = mq.decode(&IAx[PREV]);
  if (bit < 0) return -1;
  PREV = (PREV << 1) | bit;
  int n_tail, offset;
  if (bit) {
    bit = mq.decode(&IAx[PREV]);
    if (bit < 0) return -1;
    PREV = (PREV << 1) | bit;
    if (bit) {
      bit = mq.decode(&IAx[PREV]);
      if (bit < 0) return -1;
      PREV = (PREV << 1) | bit;
      if (bit) {
        bit = mq.decode(&IAx[PREV]);
        if (bit < 0) return -1;
        PREV = (PREV << 1) | bit;
        if (bit) {
          bit = mq.decode(&IAx[PREV]);
          if (bit < 0) return -1;
          PREV = (PREV << 1) | bit;
          if (bit) {
            n_tail = 32;
            offset = 4436;
          } else {
            n_tail = 12;
            offset = 340;
          }
        } else {
          n_tail = 8;
          offset = 84;
        }
      } else {
        n_tail = 6;
        offset = 20;
      }
    } else {
      n_tail = 4;
      offset = 4;
    }
  } else {
    n_tail = 2;
    offset = 0;
  }

  int32_t V = 0;
  for (int i = 0; i < n_tail; i++) {
    bit = mq.decode(&IAx[PREV]);
    if (bit < 0) return -1;
    PREV = ((PREV << 1) & 511) | (PREV & 256) | bit;
    V = static_cast<int32_t>((static_cast<uint32_t>(V) << 1) | static_cast<uint32_t>(bit));
  }
  if (V > INT32_MAX - offset) V = INT32_MAX;
  else V += offset;
  V = S ? -V : V;
  *out = V;
  return (S && V == 0) ? 1 : 0;
}

struct ArithIaidCtx {
  pdfscratch::ByteVec x;
  int len = 0;
  bool init(int n) {
    if (n < 0 || n > 24) return false;
    len = n;
    x.assign(static_cast<size_t>(1) << n, 0);
    return true;
  }
  int decode(MQ &mq, uint32_t *out) {
    int PREV = 1;
    for (int i = 0; i < len; i++) {
      const int D = mq.decode(&x[static_cast<size_t>(PREV)]);
      if (D < 0) return -1;
      PREV = (PREV << 1) | D;
    }
    *out = static_cast<uint32_t>(PREV - (1 << len));
    return 0;
  }
};

int ceilLog2(uint32_t n) {
  int l = 0;
  uint64_t v = 1;
  while (v < static_cast<uint64_t>(n)) {
    v <<= 1;
    l++;
  }
  return l;
}

bool imageAlloc(PdfJbig2Image *img, int w, int h, const char *what, PdfError *err,
                std::string *msg) {
  if (w <= 0 || h <= 0) {
    setErr(err, msg, PdfError::Decode, what);
    return false;
  }
  if (static_cast<int64_t>(w) * h > PdfLimits::kMaxImagePixels) {
    setErr(err, msg, PdfError::TooLarge, "PDF 图像像素过多");
    return false;
  }
  img->width = w;
  img->height = h;
  img->stride = ((w - 1) >> 3) + 1;
  const uint64_t bytes = static_cast<uint64_t>(img->stride) * static_cast<uint64_t>(h);
  if (bytes > static_cast<uint64_t>(PdfLimits::kMaxStreamBytes)) {
    setErr(err, msg, PdfError::TooLarge, "PDF 图像数据过大");
    return false;
  }
  // pdfscratch::alloc 拿不到内存时会 abort；大块（页面/区域）先探一下，
  // 变成可显示的「内存不足」，也顺手把当时的账打出来。
  if (bytes > pdfscratch::largestFree()) {
#if defined(PDF_JBIG2_LOG)
    ESP_LOGE("PdfJbig2", "image %dx%d bytes=%u > psram free=%u largest=%u", w, h,
             static_cast<unsigned>(bytes), static_cast<unsigned>(pdfscratch::freeBytes()),
             static_cast<unsigned>(pdfscratch::largestFree()));
#endif
    setErr(err, msg, PdfError::TooLarge, "PDF 图像内存不足");
    return false;
  }
#if defined(PDF_JBIG2_LOG)
  if (bytes >= 256 * 1024) {
    ESP_LOGI("PdfJbig2", "image %dx%d bytes=%u psram free=%u largest=%u", w, h,
             static_cast<unsigned>(bytes), static_cast<unsigned>(pdfscratch::freeBytes()),
             static_cast<unsigned>(pdfscratch::largestFree()));
  }
#endif
  img->bits.assign(static_cast<size_t>(bytes), 0);
  return true;
}

void imageClear(PdfJbig2Image *img, int v) {
  memset(img->bits.data(), v ? 0xFF : 0x00, img->bits.size());
}

enum ComposeOp { kOpOr = 0, kOpAnd = 1, kOpXor = 2, kOpXnor = 3, kOpReplace = 4 };

inline void composePixel(uint8_t *drow, int64_t dxi, const uint8_t *srow, int64_t sxi, int op) {
  const int d = bitAt(drow, static_cast<int>(dxi));
  const int s = bitAt(srow, static_cast<int>(sxi));
  int r;
  switch (op) {
    case kOpOr: r = d | s; break;
    case kOpAnd: r = d & s; break;
    case kOpXor: r = d ^ s; break;
    case kOpXnor: r = (d ^ s) ^ 1; break;
    default: r = s; break;
  }
  const uint8_t m = static_cast<uint8_t>(1u << (7 - (dxi & 7)));
  uint8_t &byte = drow[dxi >> 3];
  if (r) byte = static_cast<uint8_t>(byte | m);
  else byte = static_cast<uint8_t>(byte & static_cast<uint8_t>(~m));
}

// 按 jbig2_image_compose 语义把 src 合成到 dst 的 (x,y)。
// 主体按“目的字节”一次搬 8 像素：源位偏移固定，用相邻两字节拼出 8 位，
// 目标字节对齐后再做按位运算。首尾不满一字节的像素仍走逐像素，语义不变。
void composeImage(PdfJbig2Image &dst, const PdfJbig2Image *src, int64_t x, int64_t y, int op) {
  if (!src || src->empty()) return;
  int64_t sx0 = 0, sy0 = 0;
  if (x < 0) sx0 = -x;
  if (y < 0) sy0 = -y;
  int64_t sx1 = src->width;
  if (x + sx1 > dst.width) sx1 = dst.width - x;
  int64_t sy1 = src->height;
  if (y + sy1 > dst.height) sy1 = dst.height - y;
  if (sx1 <= sx0 || sy1 <= sy0) return;

  uint8_t *dBase = dst.bits.data();
  const uint8_t *sBase = src->bits.data();
  const int dStride = dst.stride;
  const int sStride = src->stride;

  for (int64_t sy = sy0; sy < sy1; sy++) {
    uint8_t *drow = dBase + static_cast<size_t>(y + sy) * static_cast<size_t>(dStride);
    const uint8_t *srow = sBase + static_cast<size_t>(sy) * static_cast<size_t>(sStride);
    const int64_t dx0 = x + sx0;
    const int64_t n = sx1 - sx0;

    int64_t k = 0;
    const int64_t head = (8 - (dx0 & 7)) & 7;
    const int64_t nHead = head < n ? head : n;
    for (; k < nHead; k++) composePixel(drow, dx0 + k, srow, sx0 + k, op);
    while (k + 8 <= n) {
      const int64_t sb = sx0 + k;
      const int boff = static_cast<int>(sb & 7);
      const int64_t bb = sb >> 3;
      uint32_t pair = static_cast<uint32_t>(srow[bb]) << 8;
      if (boff && bb + 1 < sStride) pair |= srow[bb + 1];
      const uint8_t sbyte = static_cast<uint8_t>((pair >> (8 - boff)) & 0xFF);
      uint8_t &d = drow[(dx0 + k) >> 3];
      switch (op) {
        case kOpOr: d = static_cast<uint8_t>(d | sbyte); break;
        case kOpAnd: d = static_cast<uint8_t>(d & sbyte); break;
        case kOpXor: d = static_cast<uint8_t>(d ^ sbyte); break;
        case kOpXnor: d = static_cast<uint8_t>((d ^ sbyte) ^ 0xFF); break;
        default: d = sbyte; break;
      }
      k += 8;
    }
    for (; k < n; k++) composePixel(drow, dx0 + k, srow, sx0 + k, op);
  }
}

// ── 通用区域，模板 0 ─────────────────────────────────────────────────────
bool decodeGenericT0(MQ &mq, PdfJbig2Image &img, const int8_t gbat[8], uint8_t *stats,
                     PdfJbig2ProgressFn progress, void *pctx, PdfError *err, std::string *msg) {
  const int w = img.width;
  const int h = img.height;
  const int stride = img.stride;
  uint8_t *base = img.bits.data();
  const int at[8] = {gbat[0], gbat[1], gbat[2], gbat[3], gbat[4], gbat[5], gbat[6], gbat[7]};

  for (int y = 0; y < h; y++) {
    uint8_t *cur = base + static_cast<size_t>(y) * static_cast<size_t>(stride);
    const uint8_t *p1 = y >= 1 ? cur - stride : nullptr;
    const uint8_t *p2 = y >= 2 ? cur - static_cast<size_t>(2) * static_cast<size_t>(stride) : nullptr;
    // 4 个 AT 像素的行指针每行只算一次（原实现每像素重算 4 次）。
    const uint8_t *atRow[4];
    for (int i = 0; i < 4; i++) {
      const int yy = y + at[2 * i + 1];
      atRow[i] = (yy >= 0 && yy < h) ? base + static_cast<size_t>(yy) * stride : nullptr;
    }
    // 上下文模板 0 的固定位用滑窗维护：当前行 x-1..x-4、上一行 x+2..x-2
    // （5 位）、上上行 x+1..x-1（3 位）。窗口 bit0 = 最靠右的像素。
    uint32_t cur4 = 0;
    uint32_t p1w = static_cast<uint32_t>(rowBit(p1, 2, w)) |
                   static_cast<uint32_t>(rowBit(p1, 1, w)) << 1 |
                   static_cast<uint32_t>(rowBit(p1, 0, w)) << 2;
    uint32_t p2w = static_cast<uint32_t>(rowBit(p2, 1, w)) |
                   static_cast<uint32_t>(rowBit(p2, 0, w)) << 1;

    uint8_t out = 0;
    for (int x = 0; x < w; x++) {
      uint32_t ctx = cur4 | (p1w << 5) | (p2w << 12);
      ctx |= static_cast<uint32_t>(rowBit(atRow[0], x + at[0], w)) << 4;
      ctx |= static_cast<uint32_t>(rowBit(atRow[1], x + at[2], w)) << 10;
      ctx |= static_cast<uint32_t>(rowBit(atRow[2], x + at[4], w)) << 11;
      ctx |= static_cast<uint32_t>(rowBit(atRow[3], x + at[6], w)) << 15;
      const int bit = mq.decode(&stats[ctx]);
      if (bit < 0) {
        setErr(err, msg, PdfError::Decode, "PDF JBIG2 数据损坏");
        return false;
      }
      // 攒满一个字节再写回：设备上内存在 PSRAM，逐像素读改写很贵。
      out = static_cast<uint8_t>((out << 1) | bit);
      if ((x & 7) == 7) cur[x >> 3] = out;
      cur4 = ((cur4 << 1) | static_cast<uint32_t>(bit)) & 0x0F;
      p1w = ((p1w << 1) | static_cast<uint32_t>(rowBit(p1, x + 3, w))) & 0x1F;
      p2w = ((p2w << 1) | static_cast<uint32_t>(rowBit(p2, x + 2, w))) & 0x07;
    }
    if (w & 7) {
      const int rem = w & 7;
      const uint8_t mask = static_cast<uint8_t>(0xFFu << (8 - rem));
      uint8_t &byte = cur[w >> 3];
      byte = static_cast<uint8_t>((out << (8 - rem)) | (byte & static_cast<uint8_t>(~mask)));
    }
    if (progress && (y & 63) == 0 && !progress(pctx)) {
      setErr(err, msg, PdfError::Decode, "PDF 解码已取消");
      return false;
    }
  }
  return true;
}

// ── 符号字典 ─────────────────────────────────────────────────────────────
struct SymDict {
  // 本段新解的符号（拥有位图缓冲）；导出表只放指针，不复制位图。
  pdfscratch::Vec<PdfJbig2Image> own;
  pdfscratch::Vec<const PdfJbig2Image *> glyphs;  // 本字典对外导出的符号（按顺序）
};

bool decodeSymbolDict(const uint8_t *data, size_t len,
                      const pdfscratch::Vec<const SymDict *> &refs, SymDict *out,
                      PdfJbig2ProgressFn progress, void *pctx, PdfError *err, std::string *msg) {
  if (len < 10) {
    setErr(err, msg, PdfError::Decode, "PDF JBIG2 符号字典过短");
    return false;
  }
  const uint16_t flags = rd16(data);
  const bool SDHUFF = (flags & 0x1) != 0;
  const bool SDREFAGG = (flags & 0x2) != 0;
  const int SDTEMPLATE = (flags >> 10) & 3;
  if (SDHUFF) {
    setErr(err, msg, PdfError::Unsupported, "PDF JBIG2 Huffman 符号字典不支持");
    return false;
  }
  if (SDREFAGG) {
    setErr(err, msg, PdfError::Unsupported, "PDF JBIG2 细化符号不支持");
    return false;
  }
  if (SDTEMPLATE != 0) {
    setErr(err, msg, PdfError::Unsupported, "PDF JBIG2 符号模板不支持");
    return false;
  }

  int8_t gbat[8];
  for (int i = 0; i < 8; i++) gbat[i] = static_cast<int8_t>(data[2 + i]);
  size_t off = 10;
  if (off + 8 > len) {
    setErr(err, msg, PdfError::Decode, "PDF JBIG2 符号字典过短");
    return false;
  }
  const uint32_t SDNUMEX = rd32(data + off);
  const uint32_t SDNUMNEW = rd32(data + off + 4);
  off += 8;
  if (SDNUMNEW > kMaxSymbols || SDNUMEX > kMaxSymbols) {
    setErr(err, msg, PdfError::TooLarge, "PDF JBIG2 符号过多");
    return false;
  }

  uint32_t SDNUMINSYMS = 0;
  for (const SymDict *r : refs) SDNUMINSYMS += static_cast<uint32_t>(r->glyphs.size());

  MQ mq;
  mq.ws.data = data + off;
  mq.ws.size = len - off;
  mq.init();
  if (mq.failed) {
    setErr(err, msg, PdfError::Decode, "PDF JBIG2 数据不足");
    return false;
  }

  ArithIntCtx IADH, IADW, IAEX;
  pdfscratch::ByteVec statsBuf(65536, 0);
  uint8_t *stats = statsBuf.data();

  out->own.clear();
  out->own.reserve(SDNUMNEW);
  uint32_t HCHEIGHT = 0;
  uint32_t NSYMSDECODED = 0;
  uint32_t SYMWIDTH = 0;
  uint64_t glyphPixels = 0;

  while (NSYMSDECODED < SDNUMNEW) {
    int32_t HCDH = 0;
    if (intDecode(mq, IADH, &HCDH) != 0) {
      setErr(err, msg, PdfError::Decode, "PDF JBIG2 符号高度解码失败");
      return false;
    }
    HCHEIGHT = static_cast<uint32_t>(static_cast<int32_t>(HCHEIGHT) + HCDH);
    if (static_cast<int32_t>(HCHEIGHT) < 0) {
      setErr(err, msg, PdfError::Decode, "PDF JBIG2 符号高度非法");
      return false;
    }
    SYMWIDTH = 0;
    for (;;) {
      int32_t DW = 0;
      const int rc = intDecode(mq, IADW, &DW);
      if (rc < 0) {
        setErr(err, msg, PdfError::Decode, "PDF JBIG2 符号宽度解码失败");
        return false;
      }
      if (rc > 0) break;               // 本高度类结束
      if (NSYMSDECODED >= SDNUMNEW) break;
      if (DW < 0 && SYMWIDTH < static_cast<uint32_t>(-DW)) {
        setErr(err, msg, PdfError::Decode, "PDF JBIG2 符号宽度非法");
        return false;
      }
      SYMWIDTH = static_cast<uint32_t>(static_cast<int32_t>(SYMWIDTH) + DW);
      if (SYMWIDTH == 0 || HCHEIGHT == 0) {
        setErr(err, msg, PdfError::Decode, "PDF JBIG2 符号尺寸为零");
        return false;
      }
      glyphPixels += static_cast<uint64_t>(SYMWIDTH) * HCHEIGHT;
      if (glyphPixels > kMaxGlyphPixels) {
        setErr(err, msg, PdfError::TooLarge, "PDF JBIG2 符号数据过大");
        return false;
      }
      PdfJbig2Image im;
      if (!imageAlloc(&im, static_cast<int>(SYMWIDTH), static_cast<int>(HCHEIGHT),
                      "PDF JBIG2 符号尺寸非法", err, msg)) {
        return false;
      }
      if (!decodeGenericT0(mq, im, gbat, stats, progress, pctx, err, msg)) return false;
      out->own.push_back(std::move(im));
      NSYMSDECODED++;
    }
  }

  // 6.5.10 导出表：引用字典的导出符号在前，本段新符号在后。
  const uint32_t limit = SDNUMINSYMS + SDNUMNEW;
  pdfscratch::Vec<const PdfJbig2Image *> combined;
  combined.reserve(limit);
  for (const SymDict *r : refs) {
    for (const PdfJbig2Image *g : r->glyphs) combined.push_back(g);
  }
  for (const auto &g : out->own) combined.push_back(&g);

  out->glyphs.clear();
  uint32_t i = 0, j = 0;
  int exflag = 0;
  uint32_t emptyruns = 0;
  while (i < limit) {
    int32_t runS = 0;
    const int rc = intDecode(mq, IAEX, &runS);
    if (rc < 0) {
      setErr(err, msg, PdfError::Decode, "PDF JBIG2 导出表解码失败");
      return false;
    }
    if (rc > 0) {
      setErr(err, msg, PdfError::Decode, "PDF JBIG2 导出表越界");
      return false;
    }
    uint32_t run = static_cast<uint32_t>(runS);
    if (run == 0) {
      if (++emptyruns == kMaxNodesBeforeAbort) {
        setErr(err, msg, PdfError::Decode, "PDF JBIG2 导出表异常");
        return false;
      }
    } else {
      emptyruns = 0;
    }
    if (run > limit - i) run = limit - i;
    if (exflag && j + run > SDNUMEX) run = SDNUMEX - j;
    for (uint32_t k = 0; k < run; k++) {
      if (exflag) {
        if (i < combined.size()) out->glyphs.push_back(combined[i]);
        j++;
      }
      i++;
    }
    exflag = !exflag;
  }
  return true;
}

// ── 文本区域 ─────────────────────────────────────────────────────────────
struct TextParams {
  bool SBHUFF = false, SBREFINE = false, TRANSPOSED = false, SBDEFPIXEL = false;
  int LOGSBSTRIPS = 0, SBSTRIPS = 1, REFCORNER = 0, SBCOMBOP = 0, SBDSOFFSET = 0;
  bool SBRTEMPLATE = false;
};

bool decodeTextRegion(const uint8_t *data, size_t len,
                      const pdfscratch::Vec<const SymDict *> &dicts, const TextParams &p,
                      PdfJbig2Image *image, PdfJbig2ProgressFn progress, void *pctx,
                      PdfError *err, std::string *msg) {
  size_t off = 0;
  uint32_t SBNUMSYMS = 0;
  for (const SymDict *d : dicts) SBNUMSYMS += static_cast<uint32_t>(d->glyphs.size());

  if (len < 2) {
    setErr(err, msg, PdfError::Decode, "PDF JBIG2 文本区域过短");
    return false;
  }
  off += 2;  // flags 已在段头解析
  if (p.SBHUFF) {
    setErr(err, msg, PdfError::Unsupported, "PDF JBIG2 Huffman 文本区域不支持");
    return false;
  }
  if (p.SBREFINE && !p.SBRTEMPLATE) off += 4;  // sbrat
  if (off + 4 > len) {
    setErr(err, msg, PdfError::Decode, "PDF JBIG2 文本区域过短");
    return false;
  }
  const uint32_t SBNUMINSTANCES = rd32(data + off);
  off += 4;
  if (SBNUMINSTANCES > kMaxInstances) {
    setErr(err, msg, PdfError::TooLarge, "PDF JBIG2 实例过多");
    return false;
  }

  imageClear(image, p.SBDEFPIXEL ? 1 : 0);

  MQ mq;
  mq.ws.data = data + off;
  mq.ws.size = len - off;
  mq.init();
  if (mq.failed) {
    setErr(err, msg, PdfError::Decode, "PDF JBIG2 数据不足");
    return false;
  }

  ArithIntCtx IADT, IAFS, IADS, IAIT;
  ArithIaidCtx IAID;
  if (!IAID.init(ceilLog2(SBNUMSYMS))) {
    setErr(err, msg, PdfError::Decode, "PDF JBIG2 符号数非法");
    return false;
  }

  int32_t STRIPT = 0;
  if (intDecode(mq, IADT, &STRIPT) != 0) {
    setErr(err, msg, PdfError::Decode, "PDF JBIG2 条带解码失败");
    return false;
  }
  STRIPT *= -p.SBSTRIPS;
  int32_t FIRSTS = 0;
  uint32_t NINSTANCES = 0;

  while (NINSTANCES < SBNUMINSTANCES) {
    int32_t DT = 0;
    if (intDecode(mq, IADT, &DT) != 0) {
      setErr(err, msg, PdfError::Decode, "PDF JBIG2 条带解码失败");
      return false;
    }
    STRIPT += DT * p.SBSTRIPS;

    bool firstSymbol = true;
    // CURS 必须跨实例保持（jbig2dec 里是函数级变量）：同一符号在条带内
    // 相对上一个符号递增，只有条带第一个符号才用 FIRSTS 重置。
    int32_t CURS = 0;
    for (;;) {
      if (firstSymbol) {
        int32_t DFS = 0;
        if (intDecode(mq, IAFS, &DFS) != 0) {
          setErr(err, msg, PdfError::Decode, "PDF JBIG2 实例位置解码失败");
          return false;
        }
        FIRSTS += DFS;
        CURS = FIRSTS;
        firstSymbol = false;
      } else {
        if (NINSTANCES > SBNUMINSTANCES) break;
        int32_t IDS = 0;
        const int rc = intDecode(mq, IADS, &IDS);
        if (rc < 0) {
          setErr(err, msg, PdfError::Decode, "PDF JBIG2 实例位置解码失败");
          return false;
        }
        if (rc > 0) break;  // 条带结束
        CURS += IDS + p.SBDSOFFSET;
      }

      int32_t CURT = 0;
      if (p.SBSTRIPS != 1) {
        if (intDecode(mq, IAIT, &CURT) != 0) {
          setErr(err, msg, PdfError::Decode, "PDF JBIG2 条带位置解码失败");
          return false;
        }
      }
      const int32_t T = STRIPT + CURT;

      uint32_t ID = 0;
      if (IAID.decode(mq, &ID) < 0) {
        setErr(err, msg, PdfError::Decode, "PDF JBIG2 符号编号解码失败");
        return false;
      }
      const PdfJbig2Image *IB = nullptr;
      if (ID < SBNUMSYMS) {
        uint32_t id = ID;
        for (const SymDict *d : dicts) {
          if (id < d->glyphs.size()) {
            IB = d->glyphs[id];
            break;
          }
          id -= static_cast<uint32_t>(d->glyphs.size());
        }
      }  // ID 越界：jbig2dec 只告警并跳过

      if (!p.TRANSPOSED && p.REFCORNER > 1 && IB) CURS += IB->width - 1;
      const int32_t S = CURS;
      int64_t x = 0, y = 0;
      if (!p.TRANSPOSED) {
        switch (p.REFCORNER) {
          case 0:  // bottom-left
            x = S;
            y = T + 1 - (IB ? IB->height : 0);
            break;
          case 1:  // top-left
            x = S;
            y = T;
            break;
          case 2:  // bottom-right
            x = IB ? S - IB->width + 1 : S + 1;
            y = T + 1 - (IB ? IB->height : 0);
            break;
          default:  // top-right
            x = IB ? S - IB->width + 1 : S + 1;
            y = IB ? T - IB->height + 1 : T + 1;
            break;
        }
      } else {
        switch (p.REFCORNER) {
          case 0:
          case 1:
            x = T;
            y = S;
            break;
          case 2:
            x = IB ? T - IB->width + 1 : T + 1;
            y = S;
            break;
          default:
            x = IB ? T - IB->width + 1 : T + 1;
            y = IB ? S - IB->height + 1 : S + 1;
            break;
        }
      }
      composeImage(*image, IB, x, y, p.SBCOMBOP);
      if (IB && !p.TRANSPOSED && p.REFCORNER < 2) CURS += IB->width - 1;
      NINSTANCES++;
      if (progress && (NINSTANCES & 255) == 0 && !progress(pctx)) {
        setErr(err, msg, PdfError::Decode, "PDF 解码已取消");
        return false;
      }
    }
  }
  return true;
}

// ── 段流 ─────────────────────────────────────────────────────────────────
struct Segment {
  uint32_t number = 0;
  int type = 0;
  pdfscratch::Vec<uint32_t> refs;
  const uint8_t *data = nullptr;
  size_t len = 0;
};

bool parseSegments(const uint8_t *file, size_t size, pdfscratch::Vec<Segment> *segs,
                   PdfError *err, std::string *msg) {
  size_t p = 0;
  while (p + 11 <= size) {
    if (segs->size() >= kMaxSegments) {
      setErr(err, msg, PdfError::TooLarge, "PDF JBIG2 段过多");
      return false;
    }
    Segment s;
    s.number = rd32(file + p);
    const uint8_t flags = file[p + 4];
    s.type = flags & 0x3f;
    const uint8_t rtscarf = file[p + 5];
    uint32_t count;
    size_t off;
    if ((rtscarf & 0xe0) == 0xe0) {
      const uint32_t v = rd32(file + p + 5);
      count = v & 0x1fffffff;
      off = p + 5 + 4 + (static_cast<size_t>(count) + 1) / 8;
    } else {
      count = rtscarf >> 5;
      off = p + 5 + 1;
    }
    const uint32_t refSize = s.number <= 256 ? 1 : s.number <= 65536 ? 2 : 4;
    const size_t paSize = (flags & 0x40) ? 4 : 1;
    if (count > kMaxSegments ||
        off + static_cast<size_t>(count) * refSize + paSize + 4 > size) {
      setErr(err, msg, PdfError::Decode, "PDF JBIG2 段头损坏");
      return false;
    }
    s.refs.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
      uint32_t r;
      if (refSize == 1) r = file[off];
      else if (refSize == 2) r = rd16(file + off);
      else r = rd32(file + off);
      s.refs.push_back(r);
      off += refSize;
    }
    off += paSize;
    const uint32_t dlen = rd32(file + off);
    off += 4;
    if (off + dlen > size) {
      setErr(err, msg, PdfError::Decode, "PDF JBIG2 段数据截断");
      return false;
    }
    s.data = file + off;
    s.len = dlen;
    segs->push_back(std::move(s));
    p = off + dlen;
    if ((flags & 0x3f) == 51) break;  // 文件结束段
  }
  return true;
}

// 把某个段的引用解析成符号字典指针（按引用顺序，跳过非字典/缺失项）。
void resolveDictRefs(const Segment &s, const pdfscratch::Vec<Segment> &segs,
                     const pdfscratch::Vec<int> &dictOfSeg,
                     const pdfscratch::Vec<SymDict *> &dicts,
                     pdfscratch::Vec<const SymDict *> *out) {
  for (uint32_t r : s.refs) {
    for (size_t k = 0; k < segs.size(); k++) {
      if (segs[k].number == r && dictOfSeg[k] >= 0) {
        out->push_back(dicts[static_cast<size_t>(dictOfSeg[k])]);
        break;
      }
    }
  }
}

}  // namespace

bool pdfJbig2Decode(const uint8_t *data, size_t len, int expectedWidth, int expectedHeight,
                    PdfJbig2Image *out, PdfJbig2ProgressFn progress, void *ctx, PdfError *err,
                    std::string *msg) {
  if (!data || len < 11) {
    setErr(err, msg, PdfError::Decode, "PDF JBIG2 数据过短");
    return false;
  }
  pdfscratch::Vec<Segment> segs;
  if (!parseSegments(data, len, &segs, err, msg)) return false;

  PdfJbig2Image page;
  bool havePageInfo = false;
  int pageFill = 0;  // 页面初值（页信息段 flags.bit2）
  pdfscratch::Vec<SymDict *> dicts;
  pdfscratch::Vec<int> dictOfSeg(segs.size(), -1);

  auto fail = [&](const char *m) {
    setErr(err, msg, PdfError::Decode, m);
    for (SymDict *d : dicts) delete d;
    return false;
  };

  for (size_t si = 0; si < segs.size(); si++) {
    const Segment &s = segs[si];
    switch (s.type) {
      case 0: {  // 符号字典
        pdfscratch::Vec<const SymDict *> refs;
        resolveDictRefs(s, segs, dictOfSeg, dicts, &refs);
        SymDict *d = new SymDict();
        if (!d) return fail("PDF JBIG2 内存不足");
        if (!decodeSymbolDict(s.data, s.len, refs, d, progress, ctx, err, msg)) {
          delete d;
          for (SymDict *x : dicts) delete x;
          return false;
        }
        dicts.push_back(d);
        dictOfSeg[si] = static_cast<int>(dicts.size()) - 1;
        break;
      }
      case 48: {  // 页面信息
        if (s.len < 19) return fail("PDF JBIG2 页面信息过短");
        const uint32_t w = rd32(s.data);
        const uint32_t h = rd32(s.data + 4);
        const uint8_t pflags = s.data[16];
        if (!imageAlloc(&page, static_cast<int>(w), static_cast<int>(h),
                        "PDF JBIG2 页面尺寸非法", err, msg)) {
          for (SymDict *x : dicts) delete x;
          return false;
        }
        pageFill = (pflags & 4) ? 1 : 0;
        imageClear(&page, pageFill);
        havePageInfo = true;
        break;
      }
      case 4:  // 中间文本区域
        return fail("PDF JBIG2 中间文本区域不支持");
      case 6:
      case 7: {  // 立即文本区域
        if (s.len < 19) return fail("PDF JBIG2 文本区域过短");
        const uint32_t rw = rd32(s.data);
        const uint32_t rh = rd32(s.data + 4);
        const uint32_t rx = rd32(s.data + 8);
        const uint32_t ry = rd32(s.data + 12);
        const uint8_t rflags = s.data[16];
        if (rflags & 8) return fail("PDF JBIG2 彩色区域不支持");
        const int op = rflags & 7;
        const uint16_t tflags = rd16(s.data + 17);
        TextParams p;
        p.SBHUFF = (tflags & 0x0001) != 0;
        p.SBREFINE = (tflags & 0x0002) != 0;
        p.LOGSBSTRIPS = (tflags & 0x000c) >> 2;
        p.SBSTRIPS = 1 << p.LOGSBSTRIPS;
        p.REFCORNER = (tflags & 0x0030) >> 4;
        p.TRANSPOSED = (tflags & 0x0040) != 0;
        p.SBCOMBOP = (tflags & 0x0180) >> 7;
        p.SBDEFPIXEL = (tflags & 0x0200) != 0;
        p.SBDSOFFSET = (tflags & 0x7C00) >> 10;
        if (p.SBDSOFFSET > 0x0f) p.SBDSOFFSET -= 0x20;
        p.SBRTEMPLATE = (tflags & 0x8000) != 0;
        if (p.SBHUFF) return fail("PDF JBIG2 Huffman 文本区域不支持");

        pdfscratch::Vec<const SymDict *> rdicts;
        resolveDictRefs(s, segs, dictOfSeg, dicts, &rdicts);

        if (!havePageInfo) {
          if (expectedWidth <= 0 || expectedHeight <= 0) {
            return fail("PDF JBIG2 缺少页面信息");
          }
          if (!imageAlloc(&page, expectedWidth, expectedHeight, "PDF JBIG2 页面尺寸非法", err,
                          msg)) {
            for (SymDict *x : dicts) delete x;
            return false;
          }
          imageClear(&page, 0);
          havePageInfo = true;
        }

        // 扫描件几乎都是「整页一个文本区域、左上角、OR 合成」，且页面初值为 0。
        // 这时区域图完全可以就解在页面图上，省掉一整张全页位图（本项目 PSRAM
        // 只有 ~4MB 空闲，全页 ~1.3MB 是实打实的一半预算）。
        // 等价条件：位置/尺寸完全覆盖页面、区域初值与页面初值相同、
        // 合成为 OR（初值 0 时 0|r=r）或 REPLACE。
        const int regionFill = p.SBDEFPIXEL ? 1 : 0;
        const bool direct =
            rx == 0 && ry == 0 && rw == static_cast<uint32_t>(page.width) &&
            rh == static_cast<uint32_t>(page.height) && pageFill == regionFill &&
            (op == kOpReplace || (op == kOpOr && regionFill == 0));
        PdfJbig2Image region;
        PdfJbig2Image *target = &region;
        if (direct) {
          target = &page;
        } else {
          if (!imageAlloc(&region, static_cast<int>(rw), static_cast<int>(rh),
                          "PDF JBIG2 文本区域尺寸非法", err, msg)) {
            for (SymDict *x : dicts) delete x;
            return false;
          }
        }
        if (!decodeTextRegion(s.data + 17, s.len - 17, rdicts, p, target, progress, ctx, err,
                              msg)) {
          for (SymDict *x : dicts) delete x;
          return false;
        }
        if (!direct) composeImage(page, &region, rx, ry, op);
        break;
      }
      case 49:  // 页结束
      case 50:  // 条带结束
      case 51:  // 文件结束
      case 52:  // 配置段：忽略
      case 62:  // 扩展段：忽略
        break;
      default:
        // 其它段（细部/半色调/中间通用区域等）与本子集无关；不认识就跳过，
        // 页面信息缺失或页面为空时会由下面的检查兜底报错。
        break;
    }
  }

  for (SymDict *x : dicts) delete x;

  if (!havePageInfo || page.empty()) {
    setErr(err, msg, PdfError::Decode, "PDF JBIG2 页面为空");
    return false;
  }
  *out = std::move(page);
  return true;
}
