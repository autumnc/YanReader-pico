#include "ProgressiveJpegScaled.h"

#include <cmath>
#include <cstring>

namespace pjscaled {
namespace {

// zigzag 序 → 自然序（第 k 个 zigzag 位置对应哪个 (u,v)）。和 libjpeg 的
// jpeg_natural_order 一致：自然序 = u + 8*v，zigzag 从 (0,0) 出发沿反对角线走。
const uint8_t kDezigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// 降尺度核**不截断**：8 点核按箱平均缩到 S 点之后，一个 8×8 块的 64 个系数会"折叠"回
// 这 S² 个输出点上 —— u 和 2S-u 落到同一个模式（符号相反），u = S 那一项正负相消为零。
// 所以每块要留的不是"低频若干个系数"，而是**先把 u 这一维折掉**的中间量：
//
//   acc[v][x] = Σ_{u=0}^{7} F(u,v)·W[u][x]        v ∈ 0..7，x ∈ 0..S-1
//   out(x,y)  = Σ_{v=0}^{7} acc[v][x]·W[v][y]
//
// 每块 8·S 个 short（S=4 是 64 字节，S=2 是 32 字节）——比"留 64 个系数"省一半到四分之三，
// 而且出来的是**精确的箱平均**（对拍 djpeg -scale，mean ≤0.3 / max 3，见 tests/host/jpeg）。
//
// 早先那版只留 zigzag 前缀（1/2 留 25 个、1/4 留 5 个）是错的：折叠项恰好全落在前缀外，
// 丢掉它们等于额外加了个低通，实测 15%~22% 的像素差 >2 级、最大差 54 —— 本来就是冲着
// "封面糊"来的，再送一个低通就本末倒置了。
constexpr int kAccRows = 8;  // acc 的行数 = v 的取值个数（折叠后仍是 8 行）
constexpr int kMaxComps = 4;

// scaleLog2 → 输出块边长 S。S=1（1/8）那档：c_1(u>0) 在整周期上求和恒为 0，折叠退化成
// "只留 DC"，正好是 8×8 的块平均，和 `djpeg -scale 1/8` 一致。
inline int sizeForScale(int scaleLog2) {
  return scaleLog2 == 3 ? 1 : (scaleLog2 == 1 ? 4 : 2);
}

struct HuffTable {
  bool present = false;
  uint8_t values[256] = {};
  int32_t mincode[17] = {};
  int32_t maxcode[17] = {};
  int32_t valptr[17] = {};
};

// DQT 里的数就是 zigzag 序，index 直接用 zigzag 位号。
struct QuantTable {
  bool present = false;
  uint16_t v[64] = {};
};

struct Component {
  int id = 0;
  int h = 1, v = 1;  // 采样因子
  int tq = 0, td = 0, ta = 0;
  int compW = 0, compH = 0;    // 采样平面尺寸
  int blocksX = 0, blocksY = 0;  // **按 MCU 对齐**的块数（交错扫描要用这个格子）
  short* acc = nullptr;          // 每块 kAccRows·S 个 short 的折叠累加器（只有亮度要）
};

// 熵解码的位读入器。遇到 marker（0xFF 后非 0x00 / 非 RSTn）就停在原地并持续吐 1 位
// —— 这是 libjpeg 的做法：截断的数据流不至于让 huffman 解码跑飞。
class BitReader {
 public:
  BitReader(const uint8_t* data, size_t len, size_t pos)
      : data_(data), len_(len), pos_(pos) {
    refill(8);
  }

  uint32_t getBits(int n) {
    if (n == 0) return 0;
    refill(n);
    bits_ -= n;
    return (buf_ >> bits_) & ((1u << n) - 1u);
  }

  // 攒够 n 位。**bits_ 不能超过 32** —— buf_ 是 32 位，多攒的位会从左边掉出去，
  // 之后 `buf_ >> bits_` 就成了移位量 ≥ 32 的未定义行为（x86 上移位量按 5 位取模，
  // 于是"丢掉前 4 个字节、后 4 个字节在低位"这种错法，解出来是前几块正常、后面全花）。
  // 我们的 n 最大 16，攒到 24 位就够用，所以卡在 bits_ <= 24 再吞一个字节。
  void refill(int n) {
    while (bits_ < n && !hitMarker_ && bits_ <= 24) {
      buf_ = (buf_ << 8) | static_cast<uint32_t>(nextByte());
      bits_ += 8;
    }
    if (bits_ < n) {  // 流到头 / 撞上 marker：按 libjpeg 的惯例补 1
      const int pad = n - bits_;
      buf_ = (buf_ << pad) | ((1u << pad) - 1u);
      bits_ = n;
    }
  }

  // 丢掉 n 位（精化扫描里用不上，但 EOB 后的位要跳过时用得到）。
  void skipBits(int n) {
    while (n >= 16) { getBits(16); n -= 16; }
    if (n) getBits(n);
  }

  // 只解 huffman 码（精化扫描里"读一位"也在码流里）。
  int huffDecode(const HuffTable& t);

  // 跳到重启间隔标记之后：丢掉字节缓冲，重新对齐到下一个 RSTn 的后面。
  // 缓冲里可能已经把 RSTn 吞进来了（那时 hitMarker_ 立在 0xFF 上，pos_ 还指着它）。
  void restart() {
    buf_ = 0;
    bits_ = 0;
    hitMarker_ = false;
    if (pos_ + 1 < len_ && data_[pos_] == 0xFF && data_[pos_ + 1] >= 0xD0 &&
        data_[pos_ + 1] <= 0xD7) {
      pos_ += 2;
    }
    refill(8);
  }

  size_t pos() const { return pos_; }

 private:
  uint8_t nextByte() {
    if (hitMarker_ || pos_ >= len_) {
      // 截断：吐 0（libjpeg 的"补零"，不是补 1 —— 补 1 会把 trailing 的
      // huffman 码解成大数，补 0 更接近"多解了几个 EOB"的安全侧）。
      return 0;
    }
    uint8_t b = data_[pos_++];
    if (b == 0xFF) {
      // 0xFF00 = 数据里的 0xFF；0xFFD0..D7 = 重启标记；别的都是真 marker。
      while (pos_ < len_ && data_[pos_] == 0xFF) pos_++;  // 填充字节
      if (pos_ >= len_) {
        hitMarker_ = true;
        return 0xFF;
      }
      uint8_t n = data_[pos_];
      if (n == 0x00) {
        pos_++;
        return 0xFF;
      }
      hitMarker_ = true;  // 真 marker：停在这儿，别再动 pos_
      return 0xFF;
    }
    return b;
  }

  const uint8_t* data_;
  size_t len_;
  size_t pos_;
  uint32_t buf_ = 0;
  int bits_ = 0;
  bool hitMarker_ = false;
};

// JPEG 规范图 F.16 的逐位走法。别图快去改成"先读 8 位再比" —— maxcode[] 是按**码长**
// 分档的，拿 8 位的值去比 1 位的档是两回事（试过，第一个块就解挂）。
// count[l]==0 的档 maxcode[l] = -1，`code <= -1` 永远不成立，自然就往下走了。
int BitReader::huffDecode(const HuffTable& t) {
  int code = 0;
  for (int l = 1; l <= 16; l++) {
    code = (code << 1) | getBits(1);
    if (code <= t.maxcode[l]) return t.values[t.valptr[l] + code - t.mincode[l]];
  }
  return -1;  // 表不对 / 流坏了
}

// 从 pos 出发找这一条扫描的熵数据在哪里结束（下一个真 marker 的 0xFF 处）。
size_t scanEnd(const uint8_t* d, size_t len, size_t pos) {
  while (pos + 1 < len) {
    if (d[pos] != 0xFF) {
      pos++;
      continue;
    }
    uint8_t n = d[pos + 1];
    if (n == 0x00 || (n >= 0xD0 && n <= 0xD7)) {
      pos += 2;  // 填充 / 重启标记，都还算扫描数据
      continue;
    }
    return pos;  // 真 marker
  }
  return len;
}

class Decoder {
 public:
  Decoder(const uint8_t* data, size_t len, int scaleLog2, const Alloc& mem)
      : data_(data), len_(len), scaleLog2_(scaleLog2), mem_(mem), S_(sizeForScale(scaleLog2)) {}

  ~Decoder() {
    // acc 只归 Decoder 所有（交给调用方的只有 plane），所以不管 run() 从哪条路返回，
    // 都在这里还回去。少了这个析构，每次解码漏掉整块 acc —— 1200×1600 的封面 1/2 约
    // 1.9 MB，几本书就把 PSRAM 啃光，后续封面会静默退回 JPEGDEC 的 1/8（又糊回去）。
    for (int i = 0; i < kMaxComps; i++) {
      if (comp_[i].acc) mem_.release(comp_[i].acc);
    }
  }

  bool run(Plane* out) {
    if (!data_ || len_ < 4 || !mem_.alloc || !mem_.release) return false;
    if (scaleLog2_ < 1 || scaleLog2_ > 3) return false;
    if (data_[0] != 0xFF || data_[1] != 0xD8) return false;  // SOI
    accStride_ = kAccRows * S_;
    buildBasis();
    if (!scanMarkers()) return false;
    if (!sofSeen_ || !dcScanDone_) return false;
    if (oom_) return false;
    if (!runIdct(out)) return false;
    if (!out->pixels) return false;
    return true;
  }

  // 给 peakBytes 用：只算账，不解码。
  static size_t estimate(int width, int height, int yH, int yV, int hMax, int vMax,
                         int scaleLog2) {
    if (width <= 0 || height <= 0 || hMax <= 0 || vMax <= 0 || yH <= 0 || yV <= 0) return 0;
    const int S = sizeForScale(scaleLog2);
    const int mcuW = (width + hMax * 8 - 1) / (hMax * 8);
    const int mcuH = (height + vMax * 8 - 1) / (vMax * 8);
    const size_t bw = static_cast<size_t>(mcuW) * yH;
    const size_t bh = static_cast<size_t>(mcuH) * yV;
    const size_t acc = bw * bh * kAccRows * S * sizeof(short);
    const size_t plane = bw * S * bh * S;
    return acc + plane + 64 * 1024;  // 表 + 对齐余量
  }

 private:
  // ---- 降尺度核：W[u][x] = A(u)·c_S(u)·cos((2x+1)uπ/(2S))，u ∈ 0..7，x ∈ 0..S-1 ------
  //
  // A(u) = C(u)/2，C(0)=1/√2，C(u>0)=1（JPEG 的归一化）。
  // c_S(u) = [1/d]·Σ_{j<d} cos((2j+1)uπ/16)（d = 8/S）—— 把 8 点核沿一个方向按 d 个源点做
  // 箱平均得到的常数，等比求和后 ≡ sin(d·uπ/16)/(d·sin(uπ/16))，u=0 取极限 1。
  //
  // u 必须跑到 7（不能只到 S-1）：u ∈ (S, 2S) 的项折回模式 2S-u，u = S 的项折成 0。
  // 漏掉它们就是上面说的那个多余的低通。
  //
  // DC 校验：(A(0)c_S(0))² = (1/(2√2))² = 1/8 —— 正好 DC/8，8 个点平均到 S² 个输出的正确缩放。
  void buildBasis() {
    // d = 8/S：一个降尺度输出点吃掉几个源点（S=4 吃 2，S=2 吃 4，S=1 吃 8）。
    const double d = 8.0 / S_;
    for (int u = 0; u < 8; u++) {
      // u=0 时是 0/0，取极限 = 1。
      const double cu = (u == 0) ? 1.0
                                 : std::sin(d * u * M_PI / 16.0) /
                                       (d * std::sin(u * M_PI / 16.0));
      const double a = (u == 0 ? 1.0 / std::sqrt(2.0) : 1.0) * 0.5 * cu;
      for (int x = 0; x < S_; x++) {
        const double w = a * std::cos((2 * x + 1) * u * M_PI / (2.0 * S_));
        W_[u][x] = static_cast<float>(w);
        // Q16 定点：折叠的内层循环里全是整数乘加，没浮点也没除法。
        Wi_[u][x] = static_cast<int32_t>(std::lround(w * 65536.0));
      }
    }
  }

  Component* compById(int id) {
    for (int i = 0; i < ncomp_; i++)
      if (comp_[i].id == id) return &comp_[i];
    return nullptr;
  }

  // ---- marker 主循环 ------------------------------------------------------------
  bool scanMarkers() {
    size_t pos = 2;
    while (pos + 1 < len_) {
      if (data_[pos] != 0xFF) {
        pos++;
        continue;
      }
      if (data_[pos + 1] == 0xFF) {  // 填充
        pos++;
        continue;
      }
      const uint8_t marker = data_[pos + 1];
      pos += 2;
      if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;  // TEM / RST
      if (marker == 0xD9) break;  // EOI
      if (pos + 2 > len_) return false;
      const size_t segLen = (static_cast<size_t>(data_[pos]) << 8) | data_[pos + 1];
      if (segLen < 2 || pos + segLen > len_) return false;
      const uint8_t* seg = data_ + pos + 2;
      const size_t segBytes = segLen - 2;

      switch (marker) {
        case 0xDB:  // DQT
          if (!parseDqt(seg, segBytes)) return false;
          pos += segLen;
          break;
        case 0xC4:  // DHT
          if (!parseDht(seg, segBytes)) return false;
          pos += segLen;
          break;
        case 0xC2:  // SOF2（渐进）
          if (sofSeen_ || !parseSof(seg, segBytes)) return false;
          pos += segLen;
          break;
        case 0xDD:  // DRI
          if (segBytes < 2) return false;
          restartInterval_ = (seg[0] << 8) | seg[1];
          pos += segLen;
          break;
        case 0xC0: case 0xC1: case 0xC3: case 0xC5: case 0xC6: case 0xC7:
        case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
          return false;  // 不是 SOF2 渐进：别碰，交给调用方的老路
        case 0xDC:  // DNL：会改高度，直接放弃
          return false;
        case 0xDA: {  // SOS —— 熵数据紧跟在段后面
          if (!sofSeen_ || !parseSosHeader(seg, segBytes)) return false;
          const size_t start = pos + segLen;
          if (!runScan(start)) return false;
          pos = scanEnd(data_, len_, start);  // 跳到扫描数据之后（忽略缓冲预留）
          break;
        }
        default:
          pos += segLen;
          break;
      }
    }
    // 收尾：把没写完的块补成 0（系数数组开头就清零了，所以什么都不用做）。
    return true;
  }

  bool parseDqt(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
      const uint8_t pq = p[i] >> 4;
      const uint8_t tq = p[i] & 0x0F;
      i++;
      if (tq >= 4 || pq > 1) return false;
      const size_t need = pq ? 128 : 64;
      if (i + need > n) return false;
      for (int k = 0; k < 64; k++) {
        qt_[tq].v[k] = pq ? static_cast<uint16_t>((p[i] << 8) | p[i + 1]) : p[i];
        i += pq ? 2 : 1;
      }
      qt_[tq].present = true;
    }
    return true;
  }

  bool parseDht(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
      if (i + 17 > n) return false;
      const uint8_t tc = p[i] >> 4;
      const uint8_t th = p[i] & 0x0F;
      i++;
      if (tc > 1 || th >= 4) return false;
      int total = 0;
      int counts[17];
      counts[0] = 0;
      for (int l = 1; l <= 16; l++) {
        counts[l] = p[i + l - 1];
        if (l < 16) total += counts[l];
      }
      total += counts[16];
      if (i + 16 + static_cast<size_t>(total) > n) return false;
      HuffTable& t = tc ? huffAc_[th] : huffDc_[th];
      std::memset(&t, 0, sizeof(t));
      int k = 0;
      for (int l = 1; l <= 16; l++)
        for (int j = 0; j < counts[l]; j++) {
          t.values[k] = p[i + 16 + k];
          k++;
        }
      // 标准码表：mincode / maxcode / valptr 按位长 L 累出来
      int code = 0, idx = 0;
      for (int l = 1; l <= 16; l++) {
        if (counts[l]) {
          t.valptr[l] = idx;
          t.mincode[l] = code;
          code += counts[l];
          idx += counts[l];
          t.maxcode[l] = code - 1;
        } else {
          t.maxcode[l] = -1;
        }
        code <<= 1;
      }
      t.present = true;
      i += 16 + static_cast<size_t>(total);
    }
    return true;
  }

  bool parseSof(const uint8_t* p, size_t n) {
    if (n < 6) return false;
    if (p[0] != 8) return false;  // 8 位精度
    height_ = (p[1] << 8) | p[2];
    width_ = (p[3] << 8) | p[4];
    ncomp_ = p[5];
    if (height_ <= 0 || width_ <= 0 || ncomp_ < 1 || ncomp_ > kMaxComps) return false;
    if (n < 6 + static_cast<size_t>(ncomp_) * 3) return false;
    for (int i = 0; i < ncomp_; i++) {
      const uint8_t* c = p + 6 + i * 3;
      comp_[i].id = c[0];
      comp_[i].h = c[1] >> 4;
      comp_[i].v = c[1] & 0x0F;
      comp_[i].tq = c[2];
      if (comp_[i].h < 1 || comp_[i].h > 4 || comp_[i].v < 1 || comp_[i].v > 4) return false;
      if (comp_[i].tq >= 4) return false;
    }
    hMax_ = 1;
    vMax_ = 1;
    for (int i = 0; i < ncomp_; i++) {
      if (comp_[i].h > hMax_) hMax_ = comp_[i].h;
      if (comp_[i].v > vMax_) vMax_ = comp_[i].v;
    }
    // 我们只出亮度。要求 0 号分量就是全分辨率那个（所有实测封面都是），否则认不出来。
    if (comp_[0].h != hMax_ || comp_[0].v != vMax_) return false;
    mcuW_ = (width_ + hMax_ * 8 - 1) / (hMax_ * 8);
    mcuH_ = (height_ + vMax_ * 8 - 1) / (vMax_ * 8);
    for (int i = 0; i < ncomp_; i++) {
      Component& c = comp_[i];
      c.compW = (width_ * c.h + hMax_ - 1) / hMax_;
      c.compH = (height_ * c.v + vMax_ - 1) / vMax_;
      c.blocksX = mcuW_ * c.h;
      c.blocksY = mcuH_ * c.v;
    }
    sofSeen_ = true;
    return true;
  }

  bool parseSosHeader(const uint8_t* p, size_t n) {
    if (n < 1) return false;
    const int ns = p[0];
    if (ns < 1 || ns > kMaxComps || n < 1 + static_cast<size_t>(ns) * 2 + 3) return false;
    scanComps_ = ns;
    for (int i = 0; i < ns; i++) {
      Component* c = compById(p[1 + i * 2]);
      if (!c) return false;
      scanComp_[i] = c;
      c->td = p[2 + i * 2] >> 4;
      c->ta = p[2 + i * 2] & 0x0F;
      if (c->td >= 4 || c->ta >= 4) return false;
    }
    const uint8_t* t = p + 1 + ns * 2;
    ss_ = t[0];
    se_ = t[1];
    ahal_ = t[2];
    ah_ = ahal_ >> 4;
    al_ = ahal_ & 0x0F;
    if (ss_ > 63 || se_ > 63 || ss_ > se_) return false;
    return true;
  }

  bool allTablesPresent() {
    for (int i = 0; i < scanComps_; i++) {
      Component* c = scanComp_[i];
      if (!qt_[c->tq].present) return false;
      if (ss_ == 0) {
        if (!huffDc_[c->td].present) return false;
      } else {
        if (!huffAc_[c->ta].present) return false;
      }
    }
    return true;
  }

  // 这条扫描里有没有亮度分量？
  bool scanHasLuma() {
    for (int i = 0; i < scanComps_; i++)
      if (scanComp_[i] == &comp_[0]) return true;
    return false;
  }

  // 这条扫描要不要真的解？折叠要用**全部** 64 个系数，所以 DC 波段和每条 AC 波段都留；
  // 只有精化扫描（Ah>0）跳过 —— 它只补系数的低位，且实测十本书的渐进式封面全是 Ah=0。
  bool scanNeeded() {
    return ah_ == 0;
  }

  // 把整个扫描熵数据解出来（或者整条跳过去）。
  bool runScan(size_t start) {
    const bool need = scanHasLuma() && scanNeeded();
    if (!need) return true;  // 色度 / 精化 / 高波段：整条跳过

    if (!tablesReady_) {
      // 表按需分配（只有真要解码时才分配）
      if (!allocCoeffs()) return false;
      tablesReady_ = true;
    }
    if (!allTablesPresent()) return false;

    BitReader br(data_, len_, start);
    eobRun_ = 0;

    if (scanComps_ > 1) {
      // 交错扫描：按 MCU 走，每个分量每 MCU 出 h*v 个块
      for (int my = 0; my < mcuH_; my++) {
        for (int mx = 0; mx < mcuW_; mx++) {
          for (int ci = 0; ci < scanComps_; ci++) {
            Component* c = scanComp_[ci];
            for (int yy = 0; yy < c->v; yy++) {
              for (int xx = 0; xx < c->h; xx++) {
                const int bx = mx * c->h + xx;
                const int by = my * c->v + yy;
                // 只要亮度：色度照解（位流得推进），但系数不落盘。
                const bool keep = (c == &comp_[0]);
                short* acc = keep ? c->acc + (static_cast<size_t>(by) * c->blocksX + bx) * accStride_
                                  : nullptr;
                if (!decodeBlock(br, *c, acc, keep ? qt_[c->tq].v : nullptr)) {
                  truncated_ = true;  // 码流到头，剩下的块留 0
                  return true;
                }
              }
            }
          }
          if (!endOfInterval(br)) return false;
        }
      }
    } else {
      // 非交错：单一分量的块格子（和 stb 一样用**未对齐**的 ceil(compW/8)）——
      // 编码器在非交错扫描里就只写这么多块，多算会读到别的 marker。
      Component* c = scanComp_[0];
      const int bw = (c->compW + 7) / 8;
      const int bh = (c->compH + 7) / 8;
      for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
          const bool keep = (c == &comp_[0]);
          short* acc = keep ? c->acc + (static_cast<size_t>(by) * c->blocksX + bx) * accStride_
                            : nullptr;
          if (!decodeBlock(br, *c, acc, keep ? qt_[c->tq].v : nullptr)) {
            truncated_ = true;
            return true;
          }
          if (!endOfInterval(br)) return false;
        }
      }
    }
    if (scanHasLuma() && ss_ == 0) dcScanDone_ = true;
    return true;
  }

  bool endOfInterval(BitReader& br) {
    if (restartInterval_ == 0) return true;
    if (++sinceRestart_ < restartInterval_) return true;
    sinceRestart_ = 0;
    br.restart();
    // 重启后 DC 预测值清零（AVOID: 只对 DC 扫描有意义，但清零无害）
    for (int i = 0; i < ncomp_; i++) dcPred_[i] = 0;
    return true;
  }

  // 解一个块，把系数**折进** acc（kAccRows·S 个 short）。acc 为 null 表示这个分量不要
  // （色度照解是为了推进位流，数值全扔）。
  //
  // acc 是 short：libjpeg 存系数也是 short 且饱和，这里折的是"u 维求和后的中间量"，
  // 实测十本书一次饱和都没碰到（真碰上也只是那一项削顶，不会跑飞）。
  //
  // 返回 false 只表示"这条扫描的码流到此为止"，不是错误 ——（下面这段是原注释，保留）
  // 编码器常常把队尾那些全 EOB 的块整个省掉（实测十本书里 1200×1687 那张，最后 6 个块
  // 就没写），libjpeg / stb 在扫描末尾遇到坏码也是告警了事。停下来、剩下的块留 0 就行。
  bool decodeBlock(BitReader& br, const Component& c, short* acc, const uint16_t* quant) {
    const int ci = static_cast<int>(&c - comp_);
    if (ss_ == 0) {
      const HuffTable& t = huffDc_[c.td];
      int s = br.huffDecode(t);
      if (s < 0 || s > 15) return false;
      int diff = s ? br.getBits(s) : 0;
      if (s && (diff & (1 << (s - 1))) == 0) diff -= (1 << s) - 1;  // extend
      int dc = dcPred_[ci] + diff;
      dcPred_[ci] = dc;
      if (acc) accAdd(acc, dequant(dc, quant[0]), 0);  // DC = F(0,0)，只折进 acc[0][*]
      return true;
    }

    // AC（Ah=0）。**别**在这儿清零整块 —— acc[0][*] 里已经有上一条 DC 扫描折进去的量了。
    const HuffTable& t = huffAc_[c.ta];

    // EOB 游程：`0b1rrr_0000` 里的 r > 0 表示"本块之后还有 2^r + 额外位 个块整个是 EOB"，
    // 那些块**一个比特都不占**。必须照着跳过，不能照常去解一个 huffman 码 —— 少了这一步
    // 位流立刻错位，而且是"解得出、数值全错"那种（早先只解低频波段时误打误撞躲过去了；
    // 一解到 6..63 波段，误差立刻从 mean 1.6 涨到 6.1）。
    if (eobRun_ > 0) {
      eobRun_--;
      return true;
    }

    int k = ss_;
    while (k <= se_) {
      int rs = br.huffDecode(t);
      if (rs < 0) return false;
      const int s = rs & 0x0F;
      const int r = rs >> 4;
      if (s == 0) {
        if (r != 15) {
          eobRun_ = 1 << r;            // r=0 时就是 1，减掉后为 0，等于不跳
          if (r) eobRun_ += static_cast<int>(br.getBits(r));
          eobRun_--;
          break;
        }
        k += 16;  // ZRL：跳过 16 个 0
        continue;
      }
      k += r;
      if (k > se_) break;  // 越界系数：丢掉它、本块到此为止（libjpeg 的 for 循环同此）
      int val = br.getBits(s);
      if ((val & (1 << (s - 1))) == 0) val -= (1 << s) - 1;  // extend
      if (acc) {
        // 系数在自然序里的位置决定它折到哪一行哪一列：v = nat>>3 选行，u = nat&7 选核。
        const int nat = kDezigzag[k];
        accAdd(acc + (nat >> 3) * S_, dequant(val, quant[k]), nat & 7);
      }
      k++;
    }
    return true;
  }

  // 量化后的系数 = 熵解码出来的值 × 量化表，中间要把渐进式的"尺度"还原回去。
  //
  // 第一条扫描送的是 (系数 >> Al)，后面还有精化扫描补低 Al 位 —— 我们跳过精化扫描，
  // 所以这里左移 Al 还原到正确量级（低 Al 位当 0，误差 ≤ 量化台阶的 2^Al-1 倍）。
  // **不左移就是"褪色"**：Al>0 时每个系数都只有真值的 1/2^Al，整幅会塌向中灰。
  // 仓里十本书的渐进式封面实测全是 Al=0（只有频段划分、没有逐次逼近），在那些书上
  // 这条不生效；留着是为了别在别的书上踩到。
  //
  // 顺带按 libjpeg 的 JCOEF 惯例饱和到 short：一来这本就是它的语义，二来后面
  // f·Q16 核（|核| ≤ 2^15）才保证乘积落在 int32 里。全程走 int64，别在中间截。
  inline int32_t dequant(const int32_t v, const uint16_t q) const {
    const int64_t scaled = al_ ? (static_cast<int64_t>(v) << al_) : v;
    const int64_t t = scaled * q;
    if (t > 32767) return 32767;
    if (t < -32768) return -32768;
    return static_cast<int32_t>(t);
  }

  // acc[0..S-1] += F·W[u][0..S-1]（Q16 定点，四舍五入后饱和到 short）。
  void accAdd(short* row, int32_t f, int u) {
    const int32_t* w = Wi_[u];
    for (int x = 0; x < S_; x++) {
      int32_t t = row[x] + ((f * w[x] + 32768) >> 16);
      if (t > 32767) t = 32767;
      else if (t < -32768) t = -32768;
      row[x] = static_cast<short>(t);
    }
  }

  bool allocCoeffs() {
    Component& y = comp_[0];
    const size_t n = static_cast<size_t>(y.blocksX) * y.blocksY * accStride_;
    short* buf = static_cast<short*>(mem_.alloc(n * sizeof(short)));
    if (!buf) {
      oom_ = true;
      return false;
    }
    std::memset(buf, 0, n * sizeof(short));  // 编码器没写的块就留 0（折叠后的 0 也是 0）
    y.acc = buf;
    return true;
  }

  // ---- IDCT + 出图 ---------------------------------------------------------------
  bool runIdct(Plane* out) {
    const Component& y = comp_[0];
    if (!y.acc) return false;

    planeStride_ = y.blocksX * S_;
    const size_t planeBytes = static_cast<size_t>(planeStride_) * y.blocksY * S_;
    uint8_t* plane = static_cast<uint8_t*>(mem_.alloc(planeBytes));
    if (!plane) {
      oom_ = true;
      return false;
    }
    // 有效尺寸：一个 8×8 源块出 S×S 个输出点，所以每点吃 8/S 个源像素。
    // 别写成 ceil(w/S) —— S 是**输出**块边长，不是缩放分母（1/4 那档会差一倍）。
    const int d = 8 / S_;
    const int validW = (width_ + d - 1) / d;
    const int validH = (height_ + d - 1) / d;
    std::memset(plane, 128, planeBytes);

    for (int by = 0; by < y.blocksY; by++) {
      const int y0 = by * S_;
      if (y0 >= validH) break;  // 下面的块全在有效区外，不用算了
      for (int bx = 0; bx < y.blocksX; bx++) {
        const int x0 = bx * S_;
        if (x0 >= validW) break;
        // acc 里 u 那一维已经折完了，这里只剩 v 那一维：out(x,y) = Σ_v acc[v][x]·W[v][y]。
        const short* acc = y.acc + (static_cast<size_t>(by) * y.blocksX + bx) * accStride_;
        for (int yy = 0; yy < S_; yy++) {
          const int py = y0 + yy;
          if (py >= validH) break;
          uint8_t* row = plane + static_cast<size_t>(py) * planeStride_;
          for (int xx = 0; xx < S_; xx++) {
            const int px = x0 + xx;
            if (px >= validW) break;
            float s = 0.0f;
            for (int v = 0; v < kAccRows; v++) s += acc[v * S_ + xx] * W_[v][yy];
            int val = static_cast<int>(std::lrint(s)) + 128;
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            row[px] = static_cast<uint8_t>(val);
          }
        }
      }
    }

    out->pixels = plane;
    out->width = validW;
    out->height = validH;
    out->stride = planeStride_;
    return true;
  }

  const uint8_t* data_;
  size_t len_;
  int scaleLog2_;
  Alloc mem_;
  int S_;
  int accStride_ = 0;  // 每块的折叠累加器长度 = kAccRows·S_

  int ncomp_ = 0;
  Component comp_[kMaxComps];
  HuffTable huffDc_[4], huffAc_[4];
  QuantTable qt_[4];
  int width_ = 0, height_ = 0;
  int hMax_ = 1, vMax_ = 1;
  int mcuW_ = 0, mcuH_ = 0;
  bool sofSeen_ = false;
  bool dcScanDone_ = false;
  bool oom_ = false;
  bool tablesReady_ = false;
  bool truncated_ = false;  // 有条扫描在队尾被省掉了几块（见 decodeBlock 的说明）

  // 当前扫描
  int scanComps_ = 0;
  Component* scanComp_[kMaxComps] = {};
  int ss_ = 0, se_ = 0, ah_ = 0, al_ = 0, ahal_ = 0;
  int restartInterval_ = 0;
  int sinceRestart_ = 0;
  int eobRun_ = 0;  // EOB 游程剩余块数，每条扫描开头清零（见 decodeBlock 的说明）

  // DC 预测值（重启间隔清零）
  int dcPred_[kMaxComps] = {};

  // 降尺度核：W_ 给最后那趟二维反变换用（浮点，够精），Wi_ 是同一张表的 Q16 定点版，
  // 折叠的内层循环只用它。
  float W_[8][4] = {};
  int32_t Wi_[8][4] = {};
  int planeStride_ = 0;
};

}  // namespace

size_t peakBytes(int width, int height, int yH, int yV, int hMax, int vMax, int scaleLog2) {
  return Decoder::estimate(width, height, yH, yV, hMax, vMax, scaleLog2);
}

// 从文件里自己读 SOF 拿采样因子，再算 peakBytes。给"手头只有文件、想先判放不放得下"的
// 调用方用（书内插图那条路就是：它拿 JPEGDEC 只解出宽高，拿不到采样因子）。
// 认不出来（非 JPEG / 不是 SOF2 / 截断）返回 0。这里只是复用同一套段扫描逻辑，
// 真正的解析仍以 decodeGray 为准 —— 估偏了顶多是多试/少试一档，不会解错。
size_t peakBytesForImage(const uint8_t* data, size_t len, int scaleLog2) {
  if (!data || len < 4 || data[0] != 0xFF || data[1] != 0xD8) return 0;
  size_t pos = 2;
  while (pos + 3 < len) {
    if (data[pos] != 0xFF) {
      pos++;
      continue;
    }
    if (data[pos + 1] == 0xFF) {  // 填充
      pos++;
      continue;
    }
    const uint8_t marker = data[pos + 1];
    pos += 2;
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;  // TEM / RST
    if (marker == 0xD9) return 0;                                        // EOI：没见过 SOF
    if (pos + 2 > len) return 0;
    const size_t segLen = (static_cast<size_t>(data[pos]) << 8) | data[pos + 1];
    if (segLen < 2 || pos + segLen > len) return 0;
    if (marker == 0xC2) {  // SOF2
      const uint8_t* p = data + pos + 2;
      const size_t n = segLen - 2;
      if (n < 6) return 0;
      const int height = (p[1] << 8) | p[2];
      const int width = (p[3] << 8) | p[4];
      const int ncomp = p[5];
      if (ncomp < 1 || ncomp > kMaxComps || n < 6 + static_cast<size_t>(ncomp) * 3) return 0;
      const int yH = p[7] >> 4, yV = p[7] & 0x0F;
      int hMax = 1, vMax = 1;
      for (int c = 0; c < ncomp; c++) {
        const int h = p[7 + c * 3] >> 4, v = p[7 + c * 3] & 0x0F;
        if (h > hMax) hMax = h;
        if (v > vMax) vMax = v;
      }
      if (yH < 1 || yV < 1 || hMax > 4 || vMax > 4) return 0;
      return Decoder::estimate(width, height, yH, yV, hMax, vMax, scaleLog2);
    }
    // 别的 SOFn（基线等）不归我们管，别往下猜。
    if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) return 0;
    if (marker == 0xDA) return 0;  // 进了扫描还没见 SOF2
    pos += segLen;
  }
  return 0;
}

int chooseScaleForTarget(int srcWidth, int srcHeight, int dstWidth, int dstHeight) {
  const int fitByW = dstWidth > 0 ? srcWidth / dstWidth : 8;
  const int fitByH = dstHeight > 0 ? srcHeight / dstHeight : 8;
  const int fit = fitByW < fitByH ? fitByW : fitByH;
  if (fit >= 8) return 0;  // 1/8 本来就够细
  return fit >= 4 ? 2 : 1;
}

bool decodeGray(const uint8_t* data, size_t len, int scaleLog2, const Alloc& mem, Plane* out) {
  if (!out) return false;
  Decoder dec(data, len, scaleLog2, mem);
  return dec.run(out);
}

}  // namespace pjscaled
