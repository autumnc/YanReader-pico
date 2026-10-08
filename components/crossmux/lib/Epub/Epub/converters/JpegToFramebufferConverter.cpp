#include "JpegToFramebufferConverter.h"

#include <BuildScratch.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Logging.h>
#include <Memory.h>
#include <ProgressiveJpegScaled.h>
#include <esp_heap_caps.h>

#include <cstdlib>
#include <memory>
#include <new>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"
#include "SampleRowClamp.h"

namespace {

// Context struct passed through JPEGDEC callbacks to avoid global mutable state.
// The draw callback receives this via pDraw->pUser (set by setUserPointer()).
// The file I/O callbacks receive the HalFile* via pFile->fHandle (set by jpegOpen()).
struct JpegContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  // 源窗口的左上角（缩放后的源坐标，与 pDraw->x/y 同一空间）。窗口外的 MCU 块映射出的
  // dst 区间会整个落在 0 左侧/上侧而被提前 return，所以解码器老实解完全图也白干不了多少活。
  int windowSrcX{0};
  int windowSrcY{0};

  // Source dimensions after JPEGDEC's built-in scaling — 有窗口时就是**窗口**的尺寸，
  // 不是整图尺寸：回调里 scaledSrcWidth 只用于"块是否越过源边界"的判定。
  int scaledSrcWidth{0};
  int scaledSrcHeight{0};

  // Final output dimensions
  int dstWidth{0};
  int dstHeight{0};

  // Fine scale in 16.16 fixed-point (ESP32-C3 has no FPU).
  // X and Y axes use separate scale factors: the aspect ratio of the output (dstWidth/dstHeight)
  // may differ from the source (srcWidth/srcHeight) due to integer rounding of displayHeight.
  // Using a single (X-based) scale for both axes causes the wrong srcRow to be skipped
  // during nearest-neighbor downscaling, potentially losing critical image content.
  int32_t fineScaleFPX{1 << 16};  // X: src -> dst column mapping
  int32_t invScaleFPX{1 << 16};   // X: dst -> src column mapping
  int32_t fineScaleFPY{1 << 16};  // Y: src -> dst row mapping
  int32_t invScaleFPY{1 << 16};   // Y: dst -> src row mapping

  PixelCache cache;
  bool caching{false};

  // 16 级量化的横向误差状态（只有 DitherMode::Row 档用）。
  DitherRowState ditherState;

  uint32_t lastYieldMs{0};  // throttle state for yieldDuringDecode()
  bool aborted{false};      // config.abortPoll asked us to stop mid-decode
};

// File I/O callbacks use pFile->fHandle to access the HalFile*,
// avoiding the need for global file state.
void* jpegOpen(const char* filename, int32_t* size) {
  // JPEGDEC owns the callback handle until jpegClose(), which deletes it.
  HalFile* f = new (std::nothrow) HalFile();
  if (!f) {
    LOG_ERR("JPG", "Failed to allocate JPEG file handle");
    return nullptr;
  }
  if (!Storage.openFileForRead("JPG", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}

void jpegClose(void* handle) {
  HalFile* f = reinterpret_cast<HalFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}

// JPEGDEC tracks file position via pFile->iPos internally (e.g. JPEGGetMoreData
// checks iPos < iSize to decide whether more data is available). The callbacks
// MUST maintain iPos to match the actual file position, otherwise progressive
// JPEGs with large headers fail during parsing.
int32_t jpegRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return 0;
  int32_t bytesRead = f->read(pBuf, len);
  if (bytesRead < 0) return 0;
  pFile->iPos += bytesRead;
  return bytesRead;
}

int32_t jpegSeek(JPEGFILE* pFile, int32_t pos) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return -1;
  if (!f->seek(pos)) return -1;
  pFile->iPos = pos;
  return pos;
}

// JPEGDEC carries its decode buffers inline. Allocate it only during active
// decode and require both aggregate headroom and one sufficiently large block.
constexpr size_t JPEG_DECODER_SIZE = sizeof(JPEGDEC);
constexpr size_t MIN_FREE_HEAP_FOR_JPEG = JPEG_DECODER_SIZE + 16 * 1024;

bool hasHeapForJpegDecoder(const char* operation) {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxAlloc = ESP.getMaxAllocHeap();
  if (freeHeap >= MIN_FREE_HEAP_FOR_JPEG && maxAlloc >= JPEG_DECODER_SIZE) return true;
  LOG_ERR("JPG", "Not enough heap for JPEG %s (free=%u need=%u, maxAlloc=%u need=%u)", operation, freeHeap,
          MIN_FREE_HEAP_FOR_JPEG, maxAlloc, JPEG_DECODER_SIZE);
  return false;
}

// Choose JPEGDEC's built-in scale factor for coarse downscaling.
// Returns the scale denominator (1, 2, 4, or 8) and sets jpegScaleOption.
int chooseJpegScale(float targetScale, int& jpegScaleOption) {
  if (targetScale <= 0.125f) {
    jpegScaleOption = JPEG_SCALE_EIGHTH;
    return 8;
  }
  if (targetScale <= 0.25f) {
    jpegScaleOption = JPEG_SCALE_QUARTER;
    return 4;
  }
  if (targetScale <= 0.5f) {
    jpegScaleOption = JPEG_SCALE_HALF;
    return 2;
  }
  jpegScaleOption = 0;
  return 1;
}

// 渐进式降尺度解（见本文件 decodeToFramebuffer 的说明）的分配器：整幅 plane 与折叠
// 累加器都放 PSRAM。跟 JpegToBmpConverter 里那份同一个路子。
const pjscaled::Alloc kPsramAlloc = {
    [](size_t bytes) -> void* { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); },
    [](void* p) { heap_caps_free(p); }};

// PSRAM 上的整幅**文件**缓冲（渐进式那条要先读进来才谈得上解）。
//
// **不要**用 memory::makePsramByteBuffer*：它挂在 `#if defined(BOARD_HAS_PSRAM)` 上，而
// 本工程从来没定义过这个宏，所以那个 helper 恒返回空 —— 编译器据此认定 progFile 是空的，
// 把整个 `if (progFile) { …peakBytes/decodeGray… }` 当死代码删掉。症状极具迷惑性：代码改了、
// 编译过了、刷进去了，屏幕上**一点变化都没有**（书内封面照样糊），因为那段压根没进固件。
// JpegToBmpConverter 的 allocPsramBuffer 踩过同一个坑，注释写在那儿。
// ByteBuffer 的释放是 free()，在 CONFIG_SPIRAM_USE_MALLOC 下与 heap_caps_malloc 配对。
memory::ByteBuffer allocPsramBuffer(const size_t size) {
  if (size == 0) return {};
  return memory::ByteBuffer{static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))};
}

// Fixed-point 16.16 arithmetic avoids software float emulation on ESP32-C3 (no FPU).
constexpr int FP_SHIFT = 16;
constexpr int32_t FP_ONE = 1 << FP_SHIFT;
constexpr int32_t FP_MASK = FP_ONE - 1;

int jpegDrawCallback(JPEGDRAW* pDraw) {
  JpegContext* ctx = reinterpret_cast<JpegContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer) return 0;

  ImageToFramebufferDecoder::yieldDuringDecode(ctx->lastYieldMs);

  // JPEGDEC stops as soon as the draw callback returns 0, but reports the abort
  // as a *successful* decode (iErr is untouched), so the flag is what tells the
  // caller apart.
  if (ctx->config->abortPoll && ctx->config->abortPoll(ctx->config->abortPollCtx)) {
    ctx->aborted = true;
    return 0;
  }

  // In EIGHT_BIT_GRAYSCALE mode, pPixels contains 8-bit grayscale values
  // Buffer is densely packed: stride = pDraw->iWidth, valid columns = pDraw->iWidthUsed
  uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;

  if (stride <= 0 || blockH <= 0 || validW <= 0) return 1;

  const DitherMode ditherMode = ctx->config->ditherMode;
  const bool writeFramebuffer = ctx->config->output == DecodeOutput::FrameBufferAndCache;
  bool caching = ctx->caching;
  const int32_t fineScaleFPX = ctx->fineScaleFPX;
  const int32_t invScaleFPX = ctx->invScaleFPX;
  const int32_t fineScaleFPY = ctx->fineScaleFPY;
  const int32_t invScaleFPY = ctx->invScaleFPY;
  GfxRenderer& renderer = *ctx->renderer;
  const int cfgX = ctx->config->x;
  const int cfgY = ctx->config->y;
  // 块在**窗口内**的源坐标（可能为负）。解码器给的行缓冲是块自己的，下面所有行/列索引
  // 都按"窗口内源坐标 - 块原点"来算，所以块原点必须与 dst 用同一套原点
  // （窗口左上角 = dst 的 (0,0)）。
  const int blockX = pDraw->x - ctx->windowSrcX;
  const int blockY = pDraw->y - ctx->windowSrcY;

  // Determine destination pixel range covered by this source block
  const int srcYEnd = blockY + blockH;
  const int srcXEnd = blockX + validW;

  int dstYStart = (int)((int64_t)blockY * fineScaleFPY >> FP_SHIFT);
  int dstYEnd = (srcYEnd >= ctx->scaledSrcHeight) ? ctx->dstHeight : (int)((int64_t)srcYEnd * fineScaleFPY >> FP_SHIFT);
  int dstXStart = (int)((int64_t)blockX * fineScaleFPX >> FP_SHIFT);
  int dstXEnd = (srcXEnd >= ctx->scaledSrcWidth) ? ctx->dstWidth : (int)((int64_t)srcXEnd * fineScaleFPX >> FP_SHIFT);

  // Pre-clamp destination ranges to screen bounds (eliminates per-pixel screen checks).
  // 先夹屏幕（cfgX/cfgY 可能为负：图比屏幕宽时），再夹 0——dst 坐标以窗口左上角为原点，
  // 负值意味着源像素在窗口外，索引到块缓冲里就是越界读。跨窗口边缘的那一块只画落在
  // 窗口里的那截（窗口内坐标仍是对的，因为索引都减了块原点）。
  int clampYMax = ctx->dstHeight;
  if (ctx->screenHeight - cfgY < clampYMax) clampYMax = ctx->screenHeight - cfgY;
  if (dstYStart < -cfgY) dstYStart = -cfgY;
  if (dstYStart < 0) dstYStart = 0;
  if (dstYEnd > clampYMax) dstYEnd = clampYMax;

  int clampXMax = ctx->dstWidth;
  if (ctx->screenWidth - cfgX < clampXMax) clampXMax = ctx->screenWidth - cfgX;
  if (dstXStart < -cfgX) dstXStart = -cfgX;
  if (dstXStart < 0) dstXStart = 0;
  if (dstXEnd > clampXMax) dstXEnd = clampXMax;

  if (dstYStart >= dstYEnd || dstXStart >= dstXEnd) return 1;

  // Pre-compute orientation and render-mode state once per callback invocation
  DirectPixelWriter pw;
  if (writeFramebuffer) pw.init(renderer);

  // The cache streams to disk one MCU-row band at a time. Flushing rows below
  // this block (raster order guarantees they are final) repositions the band;
  // cacheOriginY then maps screen rows to the band-local buffer rows. If a flush
  // write fails, stop caching for the rest of this decode (and let finalize drop
  // the partial file) rather than writing past the band buffer.
  DirectCacheWriter cw;
  int cacheOriginY = 0;
  if (caching) {
    if (!ctx->cache.advanceTo(dstYStart)) {
      caching = false;
      ctx->caching = false;
    } else {
      cw.init(ctx->cache.buffer, ctx->cache.bytesPerRow, ctx->cache.bandRows, ctx->cache.originX);
      cacheOriginY = ctx->config->y + ctx->cache.bandStart;
    }
  }

  // 两个写回目标拿到的是**同一个** level：framebuffer 与 .pxc 都是 4bpp、
  // 每像素 0..15，缓存读回来时原样搬进 framebuffer，不再二次量化。
  const auto writeSample = [&](int outX, int outY, uint8_t gray) {
    const uint8_t level = grayToLevel16(gray, outX, outY, ditherMode, ctx->ditherState);
    if (writeFramebuffer) pw.writeGray16(outX, level);
    if (caching) cw.writeGray16(outX, level);
  };

  // === 1:1 fast path: no scaling math ===
  if (fineScaleFPX == FP_ONE && fineScaleFPY == FP_ONE) {
    for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
      const int outY = cfgY + dstY;
      if (writeFramebuffer) pw.beginRow(outY);
      if (caching) cw.beginRow(outY, cacheOriginY);
      const uint8_t* row = &pixels[(dstY - blockY) * stride];
      for (int dstX = dstXStart; dstX < dstXEnd; dstX++) {
        const int outX = cfgX + dstX;
        uint8_t gray = row[dstX - blockX];
        writeSample(outX, outY, gray);
      }
    }
    return 1;
  }

  // === Bilinear interpolation ===
  // Used for upscaling (smooths the block boundaries that a progressive JPEG's
  // DC-only 1/8 decode would otherwise band), and for downscaling when the
  // reader asks for it — nearest neighbour drops source detail and produces
  // stair-step edges on scaled artwork.
  const bool bilinearRequested = ctx->config != nullptr && ctx->config->bilinearScaling;
  if ((fineScaleFPX > FP_ONE && fineScaleFPY > FP_ONE) || bilinearRequested) {
    // Pre-compute safe X range where lx0 and lx0+1 are both in [0, validW-1].
    // Only the left/right edge pixels (typically 0-2 and 1-8 respectively) need clamping.
    int safeXStart = (int)(((int64_t)blockX * fineScaleFPX + FP_MASK) >> FP_SHIFT);
    int safeXEnd = (int)((int64_t)(blockX + validW - 1) * fineScaleFPX >> FP_SHIFT);
    if (safeXStart < dstXStart) safeXStart = dstXStart;
    if (safeXEnd > dstXEnd) safeXEnd = dstXEnd;
    if (safeXStart > safeXEnd) safeXEnd = safeXStart;

    for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
      const int outY = cfgY + dstY;
      if (writeFramebuffer) pw.beginRow(outY);
      if (caching) cw.beginRow(outY, cacheOriginY);
      const int32_t srcFyFP = dstY * invScaleFPY;
      const int32_t fy = srcFyFP & FP_MASK;
      const int32_t fyInv = FP_ONE - fy;
      // Both sample rows are clamped inside the block; clamping only the lower
      // bound of ly0 leaves ly1 at -1 for the first row of a shifted block.
      const SampleRows rows = sampleRowsFor(dstY, invScaleFPY, blockY, blockH);

      const uint8_t* row0 = &pixels[rows.row0 * stride];
      const uint8_t* row1 = &pixels[rows.row1 * stride];

      // Left edge: the source column falls before the block, so both sampled
      // columns come from the shared clamp instead of local bounds logic.
      for (int dstX = dstXStart; dstX < safeXStart; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        const SampleCols cols = sampleColsFor(dstX, invScaleFPX, blockX, validW);

        int top = ((int)row0[cols.col0] * fxInv + (int)row0[cols.col1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[cols.col0] * fxInv + (int)row1[cols.col1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        writeSample(outX, outY, gray);
      }

      // Interior. The range split above is an optimisation, not a guarantee: it
      // is computed from fineScaleFPX while the samples below come from
      // invScaleFPX, and both divide with truncation, so the split start can be
      // one destination column too early. For 100x100 -> 60x60 the block at
      // x = 80 starts its interior at dstX = 48, whose first sample is
      // (48 * 109226) >> 16 - 80 = -1. Sample through the same clamp as the
      // edges rather than trusting the split.
      for (int dstX = safeXStart; dstX < safeXEnd; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        const SampleCols cols = sampleColsFor(dstX, invScaleFPX, blockX, validW);

        int top = ((int)row0[cols.col0] * fxInv + (int)row0[cols.col1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[cols.col0] * fxInv + (int)row1[cols.col1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        writeSample(outX, outY, gray);
      }

      // Right edge: the source column runs past the block's valid columns.
      for (int dstX = safeXEnd; dstX < dstXEnd; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        const SampleCols cols = sampleColsFor(dstX, invScaleFPX, blockX, validW);

        int top = ((int)row0[cols.col0] * fxInv + (int)row0[cols.col1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[cols.col0] * fxInv + (int)row1[cols.col1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        writeSample(outX, outY, gray);
      }
    }
    return 1;
  }

  // === Nearest-neighbor (downscale: fineScale < 1.0) ===
  for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
    const int outY = cfgY + dstY;
    if (writeFramebuffer) pw.beginRow(outY);
    if (caching) cw.beginRow(outY, cacheOriginY);
    const int32_t srcFyFP = dstY * invScaleFPY;
    const uint8_t* row = &pixels[clampSampleIndex((srcFyFP >> FP_SHIFT) - blockY, blockH) * stride];

    for (int dstX = dstXStart; dstX < dstXEnd; dstX++) {
      const int outX = cfgX + dstX;
      const int32_t srcFxFP = dstX * invScaleFPX;
      const int lx = clampSampleIndex((srcFxFP >> FP_SHIFT) - blockX, validW);
      uint8_t gray = row[lx];

      writeSample(outX, outY, gray);
    }
  }

  return 1;
}

}  // namespace

bool JpegToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  if (!hasHeapForJpegDecoder("dimensions")) return false;

  auto jpeg = makeUniqueNoThrow<JPEGDEC>();
  if (!jpeg) {
    LOG_ERR("JPG", "Failed to allocate JPEG decoder for dimensions");
    return false;
  }

  int rc = jpeg->open(imagePath.c_str(), jpegOpen, jpegClose, jpegRead, jpegSeek, nullptr);
  const ScopedCleanup cleanup{[&jpeg]() { jpeg->close(); }};
  if (rc != 1) {
    LOG_ERR("JPG", "Failed to open JPEG for dimensions (err=%d): %s", jpeg->getLastError(), imagePath.c_str());
    return false;
  }

  const int width = jpeg->getWidth();
  const int height = jpeg->getHeight();
  if (!validateAndStoreDimensions(width, height, out, "JPEG")) return false;
  LOG_DBG("JPG", "Image dimensions: %dx%d", width, height);

  return true;
}

bool JpegToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                     const RenderConfig& config) {
  LOG_DBG("JPG", "Decoding JPEG: %s", imagePath.c_str());

  const bool cacheOnly = config.output == DecodeOutput::CacheOnly;
  if (cacheOnly && config.cachePath.empty()) {
    LOG_ERR("JPG", "Cache-only decode requires a cache path");
    return false;
  }

  uint8_t* decoderScratch = cacheOnly ? buildscratch::claim(JPEG_DECODER_SIZE) : nullptr;
  if (!decoderScratch && !hasHeapForJpegDecoder("decode")) return false;

  std::unique_ptr<JPEGDEC> heapJpeg;
  JPEGDEC* jpeg = nullptr;
  if (decoderScratch) {
    LOG_DBG("JPG", "Using framebuffer scratch for cache-only decode");
    jpeg = ::new (static_cast<void*>(decoderScratch)) JPEGDEC();
  } else {
    heapJpeg = makeUniqueNoThrow<JPEGDEC>();
    jpeg = heapJpeg.get();
  }
  if (!jpeg) {
    LOG_ERR("JPG", "Failed to allocate JPEG decoder");
    return false;
  }
  const ScopedCleanup releaseDecoderScratch{[jpeg, decoderScratch]() {
    if (!decoderScratch) return;
    jpeg->~JPEGDEC();
    buildscratch::release(decoderScratch);
  }};
  const ScopedCleanup closeJpeg{[jpeg]() { jpeg->close(); }};

  JpegContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();

  int rc = jpeg->open(imagePath.c_str(), jpegOpen, jpegClose, jpegRead, jpegSeek, jpegDrawCallback);
  if (rc != 1) {
    LOG_ERR("JPG", "Failed to open JPEG (err=%d): %s", jpeg->getLastError(), imagePath.c_str());
    return false;
  }

  ImageDimensions sourceDimensions;
  if (!validateAndStoreDimensions(jpeg->getWidth(), jpeg->getHeight(), sourceDimensions, "JPEG")) return false;
  const int srcWidth = sourceDimensions.width;
  const int srcHeight = sourceDimensions.height;

  bool isProgressive = jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE;
  if (isProgressive) {
    LOG_INF("JPG", "Progressive JPEG detected - may decode DC-only (1/8) or reduced-scale via pjscaled");
  }

  // Calculate overall target scale
  float targetScale;
  int destWidth, destHeight;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    destWidth = config.maxWidth;
    destHeight = config.maxHeight;
    targetScale = (float)destWidth / srcWidth;
  } else {
    float scaleX = (config.maxWidth > 0 && srcWidth > config.maxWidth) ? (float)config.maxWidth / srcWidth : 1.0f;
    float scaleY = (config.maxHeight > 0 && srcHeight > config.maxHeight) ? (float)config.maxHeight / srcHeight : 1.0f;
    targetScale = (scaleX < scaleY) ? scaleX : scaleY;
    if (targetScale > 1.0f) targetScale = 1.0f;

    destWidth = (int)(srcWidth * targetScale);
    destHeight = (int)(srcHeight * targetScale);
  }

  // 源窗口（放大看图的平移+缩放）。窗口按源图比例给出，先落到整图像素坐标，
  // 再换算成"缩放后的源坐标"——JPEGDEC 的回调给的就是后者。
  const bool windowed = config.sourceWindowW > 0.0f && config.sourceWindowH > 0.0f;
  int winX = 0, winY = 0, winW = srcWidth, winH = srcHeight;
  if (windowed) {
    winX = static_cast<int>(srcWidth * std::clamp(config.sourceWindowX, 0.0f, 1.0f));
    winY = static_cast<int>(srcHeight * std::clamp(config.sourceWindowY, 0.0f, 1.0f));
    winW = static_cast<int>(srcWidth * std::clamp(config.sourceWindowW, 0.0f, 1.0f));
    winH = static_cast<int>(srcHeight * std::clamp(config.sourceWindowH, 0.0f, 1.0f));
    if (winX < 0) winX = 0;
    if (winY < 0) winY = 0;
    if (winX + winW > srcWidth) winW = srcWidth - winX;
    if (winY + winH > srcHeight) winH = srcHeight - winY;
  }

  // ---- 渐进式：绕开 JPEGDEC 的 1/8，自己按降尺度 IDCT 解 ---------------------------
  //
  // JPEGDEC 对渐进式（SOF2）只解第一扫描，那条固定是 DC —— 也就是 1/8。1200 宽的封面
  // 出来才 150px，铺到整页就是用户报的"书里面的封面也糊"。这里把整个文件读进来，用
  // pjscaled 解 1/2（PSRAM 放不下退 1/4）的精确箱平均，稍后按行喂进**同一套**目标映射
  // （复用 jpegDrawCallback，窗口/缓存/抖动都不用改）。解不出来或放不下就保持 0，
  // 落回 JPEGDEC 的 1/8 老路（略软，但不会错）。
  memory::ByteBuffer progFile;
  pjscaled::Plane progPlane;
  int progressiveScaleLog2 = 0;
  if (isProgressive) {
    // 只在"JPEGDEC 的 1/8 会被明显放大"时才值得自己解：目标尺寸已经不大于源/8 的话，
    // 1/8 本来就够细，自己解不会更清楚、还多花几倍时间（插图页的翻页延迟就是这么来的）。
    // 阈值集中在 chooseScaleForTarget 里（纯函数，tests/host/jpeg 对拍）。
    const int bestLog2 = pjscaled::chooseScaleForTarget(srcWidth, srcHeight, destWidth, destHeight);

    if (bestLog2 > 0) {
      constexpr size_t MAX_FILE_BYTES = 4u << 20;  // 与 JpegToBmpConverter 同限
      size_t fileBytes = 0;
      HalFile f;
      if (Storage.openFileForRead("JPG", imagePath, f)) {
        fileBytes = f.size();
        if (fileBytes >= 16 && fileBytes <= MAX_FILE_BYTES) {
          progFile = allocPsramBuffer(fileBytes);
          if (progFile) {
            size_t got = 0;
            while (got < fileBytes) {
              const int n = f.read(progFile.get() + got, fileBytes - got);
              if (n <= 0) break;
              got += static_cast<size_t>(n);
            }
            if (got != fileBytes) progFile.reset();
          }
        }
        f.close();
      }
      if (progFile) {
        const size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        // 从 bestLog2 起只往粗走：比它再细就是白花时间（目标尺寸吃不下那么多细节）。
        for (int s = bestLog2; s <= 2; s++) {
          const size_t need = pjscaled::peakBytesForImage(progFile.get(), fileBytes, s);
          if (need == 0) break;  // 不是我们能认的渐进式：别试了，落回 JPEGDEC
          if (freePsram < need + memory::PSRAM_FREE_RESERVE) {
            LOG_INF("JPG", "插图 %dx%d 渐进式降尺度 1/%d 约需 %u KB > PSRAM 空闲 %u KB，退一档", srcWidth, srcHeight,
                    1 << s, static_cast<unsigned>((need + 1023) / 1024), static_cast<unsigned>(freePsram / 1024));
            continue;
          }
          if (pjscaled::decodeGray(progFile.get(), fileBytes, s, kPsramAlloc, &progPlane)) {
            progressiveScaleLog2 = s;
            break;
          }
        }
        progFile.reset();  // 解完就能放：后面的大头是解出来的像素
        if (progressiveScaleLog2 == 0)
          LOG_INF("JPG", "插图 %dx%d：渐进式降尺度解不可用，落回 JPEGDEC 1/8", srcWidth, srcHeight);
        else
          LOG_INF("JPG", "插图 %dx%d → 目标 %dx%d：渐进式自解 1/%d（JPEGDEC 只能出 1/8）", srcWidth, srcHeight,
                  destWidth, destHeight, 1 << progressiveScaleLog2);
      }
    }
  }
  const ScopedCleanup freeProgPlane{[&progPlane]() {
    if (progPlane.pixels) kPsramAlloc.release(progPlane.pixels);
  }};

  // Choose JPEGDEC built-in scaling for coarse downscaling.
  // Progressive JPEGs: JPEGDEC forces JPEG_SCALE_EIGHTH internally (DC-only
  // decode produces 1/8 resolution). We must match this to avoid the if/else
  // priority chain in DecodeJPEG selecting a different scale.
  // 有窗口时一律不降采样（除非是 progressive，那由库自己决定）：放大本来就是要看细节，
  // JPEGDEC 再按 targetScale（那个比例是拿整图算的，跟窗口没关系）砍一刀就白放大了。
  int jpegScaleOption;
  int jpegScaleDenom;
  if (isProgressive) {
    if (progressiveScaleLog2 > 0) {
      // 自己解，比例就是降尺度倍数；下面 windowSrc*/scaledSrc* 全按它换算。
      jpegScaleOption = 0;
      jpegScaleDenom = 1 << progressiveScaleLog2;
    } else {
      jpegScaleOption = JPEG_SCALE_EIGHTH;
      jpegScaleDenom = 8;
    }
  } else if (windowed) {
    jpegScaleOption = 0;
    jpegScaleDenom = 1;
  } else {
    jpegScaleDenom = chooseJpegScale(targetScale, jpegScaleOption);
  }

  if (destWidth <= 0 || destHeight <= 0) {
    LOG_ERR("JPG", "Degenerate output dimensions %dx%d for %s, skipping render", destWidth, destHeight,
            imagePath.c_str());
    return false;
  }

  // 窗口在缩放后源空间里的位置与大小（scaledSrc* 就是窗口尺寸，见 JpegContext 的说明）。
  ctx.windowSrcX = winX / jpegScaleDenom;
  ctx.windowSrcY = winY / jpegScaleDenom;
  ctx.scaledSrcWidth = (winW + jpegScaleDenom - 1) / jpegScaleDenom;
  ctx.scaledSrcHeight = (winH + jpegScaleDenom - 1) / jpegScaleDenom;
  if (ctx.scaledSrcWidth < 1) ctx.scaledSrcWidth = 1;
  if (ctx.scaledSrcHeight < 1) ctx.scaledSrcHeight = 1;
  ctx.dstWidth = destWidth;
  ctx.dstHeight = destHeight;
  ctx.fineScaleFPX = (int32_t)((int64_t)destWidth * FP_ONE / ctx.scaledSrcWidth);
  ctx.invScaleFPX = (int32_t)((int64_t)ctx.scaledSrcWidth * FP_ONE / destWidth);
  ctx.fineScaleFPY = (int32_t)((int64_t)destHeight * FP_ONE / ctx.scaledSrcHeight);
  ctx.invScaleFPY = (int32_t)((int64_t)ctx.scaledSrcHeight * FP_ONE / destHeight);

  LOG_DBG("JPG", "JPEG %dx%d -> %dx%d (scale %.2f, jpegScale 1/%d, fineScale %.2f)%s", srcWidth, srcHeight, destWidth,
          destHeight, targetScale, jpegScaleDenom, (float)destWidth / ctx.scaledSrcWidth,
          isProgressive ? " [progressive]" : "");

  // Set pixel type to 8-bit grayscale (must be after open())
  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);

  // Start streaming the pixel cache to disk. The band only needs to hold the
  // tallest single decode block: a JPEGDEC MCU cell is at most 16 scaled-source
  // rows tall, which our fine scale maps to this many output rows.
  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    const int maxBlockDstRows = (int)(((int64_t)16 * ctx.fineScaleFPY) >> FP_SHIFT) + 2;
    if (!ctx.cache.begin(config.cachePath, destWidth, destHeight, config.x, config.y, maxBlockDstRows)) {
      LOG_ERR("JPG", "Failed to start cache stream%s", cacheOnly ? "" : ", continuing without caching");
      ctx.caching = false;
      if (cacheOnly) return false;
    }
  }

  unsigned long decodeStart = millis();
  ctx.lastYieldMs = decodeStart;
  if (progressiveScaleLog2 > 0) {
    // 把整幅降尺度 plane 按"每段 16 个缩放后源行"喂给现成的 jpegDrawCallback —— 它本来
    // 就是"一段灰度行 + 块原点 → 目标像素"这个契约，窗口、分带缓存、抖动全在里面。
    // 段高取 16，是为了每段映射出的目标行数不超过缓存单带高度（跟 JPEGDEC 一条 MCU 行
    // 等价；间隔由 advanceTo 负责冲掉）。
    const int planeX = winX / jpegScaleDenom;  // 窗口左上角在 plane 里的坐标
    const int planeY = winY / jpegScaleDenom;
    const int winCols = ctx.scaledSrcWidth;  // = ceil(winW / denom)，就是 plane 的有效列
    const int winRows = ctx.scaledSrcHeight;
    for (int row0 = 0; row0 < winRows; row0 += 16) {
      const int rows = (winRows - row0 < 16) ? (winRows - row0) : 16;
      JPEGDRAW d{};
      d.pUser = &ctx;
      // pPixels 声明成 uint16_t*（JPEGDEC 内部当 16 位用），但回调立刻 reinterpret 回
      // uint8_t* —— 我们喂的本来就是 8 位灰度，只是地址，不解引用成 16 位。
      d.pPixels = reinterpret_cast<uint16_t*>(progPlane.pixels +
                                               static_cast<size_t>(planeY + row0) * progPlane.stride + planeX);
      d.iWidth = progPlane.stride;
      d.iWidthUsed = winCols;
      d.iHeight = rows;
      d.x = ctx.windowSrcX;          // ⇒ 回调里 blockX = 0（这段从窗口左边界起）
      d.y = ctx.windowSrcY + row0;   // ⇒ blockY = row0（本段在窗口内的行偏移）
      if (jpegDrawCallback(&d) == 0) break;  // 0 只可能是 abortPoll 让收手
    }
    rc = ctx.aborted ? 0 : 1;
  } else {
    rc = jpeg->decode(0, 0, jpegScaleOption);
  }
  unsigned long decodeTime = millis() - decodeStart;

  if (ctx.aborted) {
    LOG_DBG("JPG", "Decode abandoned after %lu ms (abort requested)", decodeTime);
    if (ctx.caching || cacheOnly) ctx.cache.abort();
    return false;
  }

  if (rc != 1) {
    LOG_ERR("JPG", "Decode failed (rc=%d, lastError=%d)", rc, jpeg->getLastError());
    if (ctx.caching || cacheOnly) ctx.cache.abort();
    return false;
  }

  LOG_DBG("JPG", "JPEG decoding complete - render time: %lu ms", decodeTime);

  // Finalize the streamed cache file. Note: a flush failure mid-decode clears
  // ctx.caching (the partial file is dropped), so re-read the flag here.
  if (cacheOnly) {
    if (!ctx.caching) {
      ctx.cache.abort();
      return false;
    }
    return ctx.cache.finalize();
  }
  if (ctx.caching) ctx.cache.finalize();

  return true;
}

bool JpegToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasJpgExtension(extension);
}
