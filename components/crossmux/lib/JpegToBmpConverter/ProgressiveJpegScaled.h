#pragma once

#include <cstddef>
#include <cstdint>

// 渐进式 JPEG 的**降尺度解码**：直接把 8×8 块解成 1/2 或 1/4 的灰度图。
//
// 为什么再写一个解码器。封面糊的根子是 JPEGDEC 对渐进式（SOF2）只解**第一条扫描**，
// 那条固定是 DC，也就是 1/8 —— 1200 宽的封面出来只有 150px，要塞进书架 396×528 的
// 框里得放大 2.5 倍，糊是必然的。stb 的全解能出全分辨率，但它要 3 字节/系数像素，
// 1200×1687 的彩色封面实测要 24 MB，8 MB PSRAM 上永远走不通（那条路只在源图很小时
// 才够，所以一直是死路）。
//
// 这里的取法：把 8 点 IDCT 核按箱平均缩到 S 点（S=4 出 1/2，S=2 出 1/4），并且**把高
// 频系数折回去**——u ∈ (S, 2S) 的项折进模式 2S-u，u = S 的项正负相消为零。所以解出来
// 的就是精确的箱平均，和 `djpeg -scale 1/2` 几乎逐像素全等（mean ≤0.3 级、max 3）。
// 只留低频、丢掉折叠项那一版**是错的**：相当于额外加了道低通，实测 15%~22% 的像素
// 差 >2 级。内存随**目标盒子**走、不随源图尺寸走：1200×1600 的封面 1/2 约 2.5 MB，
// 1/4 约 1.2 MB。
//
// 与"全解"的差别只有一条：**跳过 successive approximation 的精化扫描（Ah>0）**。
// 精化只补系数的低 Al 位，所以第一条扫描（Ah=0）送的是 (系数 >> Al)，解码时左移 Al
// 还原（见 .cpp 的 dequant），低 Al 位当 0 —— 误差 ≤ 量化台阶的 2^Al-1 倍。
// 仓里十本书的渐进式封面实测**全是 Ah=0/Al=0**（只有频段划分，没有逐次逼近），在这
// 些书上连这点差别都没有。真正在跑的对照见 tests/host/jpeg 那套探针。
//
// 本文件不依赖任何平台：内存从外面注入，没有 ESP 头。主机上原样编译，和 stb 逐像素对拍。
// 认不出来的一律**拒绝**（返回 false），由调用方落回 JPEGDEC 老路 —— 本文件只负责
// "我确定能正确解出来的那些"。
namespace pjscaled {

// 分配器：设备上是 heap_caps_malloc(MALLOC_CAP_SPIRAM)，主机上是 malloc。
struct Alloc {
  void* (*alloc)(size_t bytes) = nullptr;
  void (*release)(void* p) = nullptr;
};

struct Plane {
  uint8_t* pixels = nullptr;  // 行优先 8 位灰度；用 Alloc::release 释放
  int width = 0;              // 有效宽度 = ceil(源宽 / scale)
  int height = 0;             // 有效高度 = ceil(源高 / scale)
  int stride = 0;             // 行距（按 8×8 块对齐，≥ width，多余的列也解出来了但别用）
};

// 峰值内存：只要 SOF 里读得到的几项就能算（只许偏大）。
// yH/yV 是亮度分量的采样因子，hMax/vMax 是全帧最大采样因子。
size_t peakBytes(int width, int height, int yH, int yV, int hMax, int vMax, int scaleLog2);

// 同一笔账，但采样因子从文件里的 SOF2 段自己读。给拿不到采样因子的调用方用；
// 认不出来返回 0。
size_t peakBytesForImage(const uint8_t* data, size_t len, int scaleLog2);

// 目标尺寸既定，选**最省**的降尺度档 —— 只在 JPEGDEC 的 1/8 会被放大时才值得自己解。
// fit = 不需放大的最大分母（宽、高取小的那个）：fit ≥ 8 → 返回 0（1/8 已经够细，别自己解）；
// fit ≥ 4 → 2（1/4 够）；否则 → 1（1/2）。返回 0 表示"不用降尺度解，交给 JPEGDEC"。
// 纯函数，主机上直接对拍。
int chooseScaleForTarget(int srcWidth, int srcHeight, int dstWidth, int dstHeight);

// scaleLog2：1 → 1/2，2 → 1/4，3 → 1/8。成功才写 out；失败不分配、不写 out。
// （1/8 那档折叠退化成"只留 DC"，与 JPEGDEC 的 DC 解等价，留着只为对拍。）
bool decodeGray(const uint8_t* data, size_t len, int scaleLog2, const Alloc& mem, Plane* out);

}  // namespace pjscaled
