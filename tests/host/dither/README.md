# tests/host/dither — 16 级灰度写回核心的主机端校验

校验 `components/crossmux/lib/Epub/Epub/converters/DitherUtils.h` —— 「8 位灰 → 0..15 级」
的量化、三档抖动、以及 4bpp 半字节打包。纯 `g++ -std=c++17`，不需要 IDF、不需要桩件、
不需要设备、不需要 `builtin.ttf`。

## 为什么不需要桩件

`DitherUtils.h` 是 header-only、只 include `<stdint.h>`。把 `grayToLevel16` / `setNibble` /
`getNibble` 放进这个头（而不是放进 `DirectPixelWriter.h`）就是为了这一点 —— 后者会把
`GfxRenderer.h` / `HalDisplay.h` 拖进来，主机上根本编不过。

## 跑它

```sh
tests/host/dither/run.sh
```

失败时退出码非 0。产物只落在 `tests/host/dither/build/`（gitignored）。

## 断言分组

| # | 组 | 钉住什么 |
|---|---|---|
| 1 | 值域 | 256 灰 × 16×16 位置 × 三档，level 恒在 0..15 |
| 2 | 确定性 | Ordered 是纯 `(gray,x,y)` 的函数（同一像素重复调用逐值相同、与扫描顺序无关）——**这是"默认选有序"的立论依据** |
| 3 | None 档 | `level == (g*15+127)/255`；`\|level*17 − g\| ≤ 8`；g=0→0、g=255→15 |
| 4 | Ordered 零和 | 单个 4×4 周期内 16 个 level 的均值与 `g*15/255` 相差 ≤ 1 级（偏移比例写错会被抓） |
| 5 | Row 档 | 行 y 与 y+1 输出逐像素相同（误差不跨行）；`\|carry\| ≤ 8`；行首/块首像素 == None 档值 |
| 6 | 单调性 | 三档下 g 升序时 level 非降 |
| 7 | 打包 | `level ∈ [0,15]` 任意 x 往返一致；**偶列落低半字节**（epdiy 4bpp 的原生约定）；写 x 不串扰 x±1；行长恰好 `(w+1)/2` 字节 |

## 与固件的对应关系（改了哪边要一起想）

- `grayToLevel16` 是**唯一**的量化落点：两条写回路径
  （`DirectPixelWriter::writeGray16` → framebuffer、`DirectCacheWriter::writeGray16` → `.pxc`）
  共用它。改了公式，`.pxc` 缓存的口径就变了 —— **缓存名带档位字母**（`.g16o/.g16r/.g16n`），
  换档/换公式必然重解码，这条护栏在 `ImageBlock::getCachePath`。
- `setNibble` / `getNibble` 的"偶列低半字节"与 epdiy framebuffer 的
  `epd_draw_pixel` 一致。**改这里就等于改 `.pxc` 的落盘格式**，要同时 bump
  `PixelCache::kFormatVersion`。
- 设备侧的真机验收在「屏幕自检」页（阅读器 → 设置标签 → 屏幕自检）：①16 级灰阶梯
  （本屏 LUT 会并成约 11 级可分辨，能数出 ~11 根就是正常）、⑤抖动三档对比条。
