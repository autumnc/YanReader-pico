// stb_image 的唯一实现 TU（STB_IMAGE_IMPLEMENTATION 只能出现在一个 TU 里）。
//
// 仓库里只为一件事引它进来：**渐进式 JPEG 的全解码**。JPEGDEC 对渐进式（SOF2）
// 只解第一条扫描的 DC 系数 —— 那固定是 1/8 分辨率（jpeg.inl 把 scaleOption 钉在
// JPEG_SCALE_EIGHTH），封面盒子才 396×528，源图一窄就糊。stb_image 会把所有扫描
// 都解出来，清晰度与基线路径同级。背景见 JpegToBmpConverter.cpp 里的长注释。
//
// 版本与 read_pico_firmware/main/book/vendor/stb_image.h 同为 v2.30（仓库里
// stb_truetype.h 也是同样的 vendor 先例）；升级请两边一起动。那份用默认 malloc、
// 还带 zlib，这份钉 PSRAM、只编 JPEG —— 两边是不同二进制，宏不同没关系。
//
// 编译面按最小配置：
//   STBI_ONLY_JPEG —— 别的格式不编（这条定义了，stb 自己会把其余 STBI_NO_* 补上）
//   STBI_NO_STDIO  —— 只走 stbi_load_from_memory，不碰文件 API
//   STBI_NO_LINEAR —— 不要 stbi_loadf 那套线性转换
//
// 关键的一条是**把 stb 自己的分配钉在 PSRAM**：阅读模式下内部 RAM 只剩几十 KB，
// 而渐进式一条 800×1200 的彩色封面光系数就要好几个 MB，走默认 malloc 会先拿内部
// 堆去试（CONFIG_SPIRAM_USE_MALLOC 下大块最终也会落到 PSRAM，但小分配会持续啃内部
// 堆、留下碎片）。释放照旧用 stbi_image_free() —— 它展开的就是这里的 STBI_FREE，
// 天生配对，调用方不必知道分配器是谁。
//
// / The single implementation TU for stb_image (STB_IMAGE_IMPLEMENTATION may only
// appear once). Vendored for exactly one job: full decoding of progressive JPEGs,
// which JPEGDEC can only give at 1/8 resolution. Same v2.30 as the copy under
// read_pico_firmware/main/book/vendor/, except that this one pins all of stb's
// own allocations to PSRAM (internal RAM has only tens of KB free while reading)
// and compiles only the JPEG path. Release with stbi_image_free() as usual — it
// expands to the matching STBI_FREE, so callers never need to know the allocator.

#include <esp_heap_caps.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR

#define STBI_MALLOC(sz) heap_caps_malloc((sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_REALLOC(p, sz) heap_caps_realloc((p), (sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_FREE(p) heap_caps_free(p)

#include "stb_image.h"
