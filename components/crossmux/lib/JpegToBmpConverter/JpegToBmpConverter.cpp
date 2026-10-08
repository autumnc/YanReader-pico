#include "JpegToBmpConverter.h"

#include <BuildScratch.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "BitmapHelpers.h"
// 只要声明（实现在 StbImageImpl.cpp，那里把 stb 的分配钉在 PSRAM）。
// / Declarations only; the implementation lives in StbImageImpl.cpp.
#include "stb_image.h"
#include "ProgressiveJpegScaled.h"

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
// Dithering method selection (only one should be true, or all false for simple quantization):
constexpr bool USE_ATKINSON = true;          // Atkinson dithering (cleaner than F-S, less error diffusion)
constexpr bool USE_FLOYD_STEINBERG = false;  // Floyd-Steinberg error diffusion (can cause "worm" artifacts)
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering (good for downsampling)
// Pre-resize to target display size (CRITICAL: avoids dithering artifacts from post-downsampling)
constexpr bool USE_PRESCALE = true;  // true: scale image to target size before dithering
// ============================================================================

// Stack-only adapter catches short header, palette and row writes alike.
class CheckedBmpOutput final : public Print {
 public:
  explicit CheckedBmpOutput(Print& output) : output(output) {}
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t size) override {
    const size_t written = output.write(bytes, size);
    failed = failed || written != size;
    return written;
  }
  bool failed = false;

 private:
  Print& output;
};

inline void write16(Print& out, const uint16_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

inline void write32(Print& out, const uint32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

inline void write32Signed(Print& out, const int32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

// Helper function: Write BMP header with 8-bit grayscale (256 levels)
void writeBmpHeader8bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 3) / 4 * 4;  // 8 bits per pixel, padded
  const int imageSize = bytesPerRow * height;
  const uint32_t paletteSize = 256 * 4;  // 256 colors * 4 bytes (BGRA)
  const uint32_t fileSize = 14 + 40 + paletteSize + imageSize;

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);                      // Reserved
  write32(bmpOut, 14 + 40 + paletteSize);  // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 8);              // Bits per pixel (8 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 256);   // colorsUsed
  write32(bmpOut, 256);   // colorsImportant

  // Color Palette (256 grayscale entries x 4 bytes = 1024 bytes)
  for (int i = 0; i < 256; i++) {
    bmpOut.write(static_cast<uint8_t>(i));  // Blue
    bmpOut.write(static_cast<uint8_t>(i));  // Green
    bmpOut.write(static_cast<uint8_t>(i));  // Red
    bmpOut.write(static_cast<uint8_t>(0));  // Reserved
  }
}

// Helper function: Write BMP header with 1-bit color depth (black and white)
static void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 31) / 32 * 4;  // 1 bit per pixel, round up to 4-byte boundary
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;  // 14 (file header) + 40 (DIB header) + 8 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 62);        // Offset to pixel data (14 + 40 + 8)

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 1);              // Bits per pixel (1 bit)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 2);     // colorsUsed
  write32(bmpOut, 2);     // colorsImportant

  // Color Palette (2 colors x 4 bytes = 8 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  // Note: In 1-bit BMP, palette index 0 = black, 1 = white
  uint8_t palette[8] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0xFF, 0xFF, 0xFF, 0x00   // Color 1: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

// Helper function: Write BMP header with 2-bit color depth
static void writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;  // 2 bits per pixel, round up
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;  // 14 (file header) + 40 (DIB header) + 16 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 70);        // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 2);              // Bits per pixel (2 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 4);     // colorsUsed
  write32(bmpOut, 4);     // colorsImportant

  // Color Palette (4 colors x 4 bytes = 16 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  uint8_t palette[16] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0x55, 0x55, 0x55, 0x00,  // Color 1: Dark gray (85)
      0xAA, 0xAA, 0xAA, 0x00,  // Color 2: Light gray (170)
      0xFF, 0xFF, 0xFF, 0x00   // Color 3: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

namespace {

// Max MCU height supported by any JPEG (4:2:0 chroma = 16 rows, 4:4:4 = 8 rows)
constexpr int MAX_MCU_HEIGHT = 16;
constexpr size_t JPEG_DECODER_SIZE = sizeof(JPEGDEC);
constexpr size_t MIN_FREE_HEAP = JPEG_DECODER_SIZE + 16 * 1024;
constexpr uint32_t FP_ONE = 1UL << 16;

constexpr int chooseJpegScale(const int srcWidth, const int srcHeight, const int outWidth, const int outHeight) {
  if ((srcWidth + 7) / 8 >= outWidth && (srcHeight + 7) / 8 >= outHeight) return JPEG_SCALE_EIGHTH;
  if ((srcWidth + 3) / 4 >= outWidth && (srcHeight + 3) / 4 >= outHeight) return JPEG_SCALE_QUARTER;
  if ((srcWidth + 1) / 2 >= outWidth && (srcHeight + 1) / 2 >= outHeight) return JPEG_SCALE_HALF;
  return 0;
}

static_assert(chooseJpegScale(600, 800, 112, 149) == JPEG_SCALE_QUARTER);
static_assert(chooseJpegScale(1600, 2400, 109, 164) == JPEG_SCALE_EIGHTH);
static_assert(chooseJpegScale(300, 400, 112, 149) == JPEG_SCALE_HALF);

// Static file pointer for JPEGDEC open callback.
// Safe in single-threaded embedded context; never accessed concurrently.
static HalFile* s_jpegFile = nullptr;
static uint8_t s_jpegIoSinceYield = 0;

static void yieldToIdle() { vTaskDelay(1); }

static void yieldDuringJpegIo() {
  if (++s_jpegIoSinceYield < 4) return;
  s_jpegIoSinceYield = 0;
  yieldToIdle();
}

void* bmpJpegOpen(const char* /*filename*/, int32_t* size) {
  if (!s_jpegFile || !*s_jpegFile) return nullptr;
  s_jpegIoSinceYield = 0;
  s_jpegFile->seek(0);
  *size = static_cast<int32_t>(s_jpegFile->size());
  yieldDuringJpegIo();
  return s_jpegFile;
}

void bmpJpegClose(void* /*handle*/) {
  // Caller owns the file — do not close it here
}

int32_t bmpJpegRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
  auto* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return 0;
  int32_t n = f->read(pBuf, len);
  if (n < 0) n = 0;
  pFile->iPos += n;
  yieldDuringJpegIo();
  return n;
}

int32_t bmpJpegSeek(JPEGFILE* pFile, int32_t pos) {
  auto* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f || !f->seek(pos)) return -1;
  pFile->iPos = pos;
  yieldDuringJpegIo();
  return pos;
}

// Context passed to the JPEGDEC draw callback via setUserPointer()
struct BmpConvertCtx {
  Print* bmpOut;
  int srcWidth;
  int srcHeight;
  int outWidth;
  int outHeight;
  JpegToBmpConverter::Output output;
  int bytesPerRow = 0;
  bool needsScaling;
  uint32_t scaleX_fp;  // source pixels per output pixel, 16.16 fixed-point
  uint32_t scaleY_fp;
  bool smoothUpscale;
  uint32_t smoothScaleX_fp;
  uint32_t smoothScaleY_fp;

  // Accumulates one MCU row (up to MAX_MCU_HEIGHT source rows × srcWidth pixels)
  // Filled column-by-column as JPEGDEC callbacks arrive for the same MCU row
  std::unique_ptr<uint8_t[]> mcuBuf;

  // Y-axis area averaging accumulators (needsScaling only)
  int currentOutY;
  uint32_t nextOutY_srcStart;  // 16.16 fixed-point boundary for the next output row
  std::unique_ptr<uint32_t[]> rowAccum;
  std::unique_ptr<uint32_t[]> rowCount;

  int smoothNextOutY;
  int smoothPrevY;
  std::unique_ptr<uint8_t[]> smoothRows;
  uint8_t* smoothPrevRow;
  uint8_t* smoothCurrRow;
  uint8_t* smoothOutRow;

  std::unique_ptr<uint8_t[]> bmpRow;

  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<FloydSteinbergDitherer> fsDitherer;
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitDitherer;

  uint8_t rowsSinceYield;
  uint8_t blocksSinceYield;
  bool error;
};

static void yieldDuringDecode(BmpConvertCtx* ctx) {
  if (++ctx->rowsSinceYield < 8) return;
  ctx->rowsSinceYield = 0;
  yieldToIdle();
}

static void yieldDuringDecodeBlock(BmpConvertCtx* ctx) {
  if (++ctx->blocksSinceYield < 16) return;
  ctx->blocksSinceYield = 0;
  yieldToIdle();
}

// Write a fully-assembled output row (grayscale bytes, length outWidth) to BMP
static void writeOutputRow(BmpConvertCtx* ctx, const uint8_t* srcRow, int outY) {
  memset(ctx->bmpRow.get(), 0, ctx->bytesPerRow);

  if (ctx->output == JpegToBmpConverter::Output::Gray8) {
    for (int x = 0; x < ctx->outWidth; x++) {
      ctx->bmpRow[x] = srcRow[x];
    }
  } else if (ctx->output == JpegToBmpConverter::Output::Mono1) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(srcRow[x], x)
                                                    : quantize1bit(srcRow[x], x, outY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel(srcRow[x]);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, outY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  if (ctx->bmpOut->write(ctx->bmpRow.get(), ctx->bytesPerRow) != static_cast<size_t>(ctx->bytesPerRow)) {
    ctx->error = true;
  }
  yieldDuringDecode(ctx);
}

// Matches the progressive-JPEG smoothing used by JpegToFramebufferConverter, but stays
// local because cover generation streams dithered BMP rows instead of framebuffer pixels.
static uint32_t interpolationStep(const int srcSize, const int outSize) {
  if (srcSize <= 1 || outSize <= 1) return 0;
  return (static_cast<uint32_t>(srcSize - 1) << 16) / static_cast<uint32_t>(outSize - 1);
}

static uint32_t interpolatedSourceFp(const int outIndex, const int outSize, const int srcSize, const uint32_t step) {
  if (srcSize <= 1 || outSize <= 1) return 0;
  if (outIndex >= outSize - 1) return static_cast<uint32_t>(srcSize - 1) << 16;
  return static_cast<uint32_t>(outIndex) * step;
}

static void scaleRowLinear(BmpConvertCtx* ctx, const uint8_t* srcRow, uint8_t* dstRow) {
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    const uint32_t srcX_fp = interpolatedSourceFp(outX, ctx->outWidth, ctx->srcWidth, ctx->smoothScaleX_fp);
    const int x0 = srcX_fp >> 16;
    const int x1 = (x0 + 1 < ctx->srcWidth) ? (x0 + 1) : x0;
    const uint32_t fx = srcX_fp & (FP_ONE - 1);
    dstRow[outX] = static_cast<uint8_t>((srcRow[x0] * (FP_ONE - fx) + srcRow[x1] * fx) >> 16);
  }
}

static void writeBlendedRow(BmpConvertCtx* ctx, const uint8_t* row0, const uint8_t* row1, const uint32_t fy,
                            const int outY) {
  const uint32_t invFy = FP_ONE - fy;
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    ctx->smoothOutRow[outX] = static_cast<uint8_t>((row0[outX] * invFy + row1[outX] * fy) >> 16);
  }
  writeOutputRow(ctx, ctx->smoothOutRow, outY);
}

static void processSmoothSourceRow(BmpConvertCtx* ctx, const uint8_t* srcRow, const int srcY) {
  scaleRowLinear(ctx, srcRow, ctx->smoothCurrRow);

  if (ctx->smoothPrevY < 0) {
    uint8_t* tmp = ctx->smoothPrevRow;
    ctx->smoothPrevRow = ctx->smoothCurrRow;
    ctx->smoothCurrRow = tmp;
    ctx->smoothPrevY = srcY;
    if (ctx->srcHeight <= 1) {
      while (ctx->smoothNextOutY < ctx->outHeight) {
        writeOutputRow(ctx, ctx->smoothPrevRow, ctx->smoothNextOutY);
        ctx->smoothNextOutY++;
      }
      return;
    }
    return;
  }

  while (ctx->smoothNextOutY < ctx->outHeight) {
    const uint32_t srcY_fp =
        interpolatedSourceFp(ctx->smoothNextOutY, ctx->outHeight, ctx->srcHeight, ctx->smoothScaleY_fp);
    const int y0 = srcY_fp >> 16;
    const int y1 = (y0 + 1 < ctx->srcHeight) ? (y0 + 1) : y0;
    if (y1 > srcY) break;

    const uint8_t* row0 = (y0 == srcY) ? ctx->smoothCurrRow : ctx->smoothPrevRow;
    const uint8_t* row1 = (y1 == srcY) ? ctx->smoothCurrRow : ctx->smoothPrevRow;
    writeBlendedRow(ctx, row0, row1, srcY_fp & (FP_ONE - 1), ctx->smoothNextOutY);
    ctx->smoothNextOutY++;
  }

  uint8_t* tmp = ctx->smoothPrevRow;
  ctx->smoothPrevRow = ctx->smoothCurrRow;
  ctx->smoothCurrRow = tmp;
  ctx->smoothPrevY = srcY;
}

static void finishSmoothUpscale(BmpConvertCtx* ctx) {
  if (ctx->smoothPrevY < 0) {
    LOG_ERR("JPG", "No progressive rows decoded for smoothing");
    ctx->error = true;
    return;
  }

  while (ctx->smoothNextOutY < ctx->outHeight) {
    writeOutputRow(ctx, ctx->smoothPrevRow, ctx->smoothNextOutY);
    ctx->smoothNextOutY++;
  }
}

// Flush one scaled output row from Y-axis accumulators and advance currentOutY
static void flushScaledRow(BmpConvertCtx* ctx) {
  memset(ctx->bmpRow.get(), 0, ctx->bytesPerRow);

  if (ctx->output == JpegToBmpConverter::Output::Gray8) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      ctx->bmpRow[x] = gray;
    }
  } else if (ctx->output == JpegToBmpConverter::Output::Mono1) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(gray, x)
                                                    : quantize1bit(gray, x, ctx->currentOutY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel((ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, ctx->currentOutY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  if (ctx->bmpOut->write(ctx->bmpRow.get(), ctx->bytesPerRow) != static_cast<size_t>(ctx->bytesPerRow)) {
    ctx->error = true;
  }
  ctx->currentOutY++;
  yieldDuringDecode(ctx);
}

// JPEGDEC draw callback — receives one MCU-width × MCU-height block at a time,
// in left-to-right, top-to-bottom order (baseline JPEG).
// Accumulates columns into mcuBuf; once the last column arrives (completing the MCU
// row), applies scaling + dithering and writes packed BMP rows to bmpOut.
int bmpDrawCallback(JPEGDRAW* pDraw) {
  auto* ctx = reinterpret_cast<BmpConvertCtx*>(pDraw->pUser);
  if (!ctx || ctx->error) return 0;
  yieldDuringDecodeBlock(ctx);

  const uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;
  const int blockX = pDraw->x;
  const int blockY = pDraw->y;

  // Guard against unexpected callback geometry so we never index past row buffers.
  if (blockX < 0 || blockY < 0 || blockX >= ctx->srcWidth || blockY >= ctx->srcHeight) {
    LOG_ERR("JPG", "Unexpected JPEG block origin (%d,%d) for decode grid %dx%d", blockX, blockY, ctx->srcWidth,
            ctx->srcHeight);
    ctx->error = true;
    return 0;
  }

  // Copy block pixels into MCU row buffer
  for (int r = 0; r < blockH && r < MAX_MCU_HEIGHT; r++) {
    const int copyW = (blockX + validW <= ctx->srcWidth) ? validW : (ctx->srcWidth - blockX);
    if (copyW <= 0) continue;
    memcpy(ctx->mcuBuf.get() + r * ctx->srcWidth + blockX, pixels + r * stride, copyW);
  }

  // Wait for the last MCU column before processing any rows
  if (blockX + validW < ctx->srcWidth) return 1;

  // Process each complete source row in this MCU row
  const int endRow = blockY + blockH;

  for (int y = blockY; y < endRow && y < ctx->srcHeight; y++) {
    const uint8_t* srcRow = ctx->mcuBuf.get() + (y - blockY) * ctx->srcWidth;

    if (ctx->smoothUpscale) {
      processSmoothSourceRow(ctx, srcRow, y);
    } else if (!ctx->needsScaling) {
      // 1:1 — outWidth == srcWidth, write directly
      writeOutputRow(ctx, srcRow, y);
    } else {
      // Fixed-point area averaging on X axis
      for (int outX = 0; outX < ctx->outWidth; outX++) {
        const int srcXStart = (static_cast<uint32_t>(outX) * ctx->scaleX_fp) >> 16;
        const int srcXEnd = (static_cast<uint32_t>(outX + 1) * ctx->scaleX_fp) >> 16;
        int sum = 0;
        int count = 0;
        for (int srcX = srcXStart; srcX < srcXEnd && srcX < ctx->srcWidth; srcX++) {
          sum += srcRow[srcX];
          count++;
        }
        if (count == 0 && srcXStart < ctx->srcWidth) {
          sum = srcRow[srcXStart];
          count = 1;
        }
        ctx->rowAccum[outX] += sum;
        ctx->rowCount[outX] += count;
      }

      // Flush output row(s) whose Y boundary we've crossed
      const uint32_t srcY_fp = static_cast<uint32_t>(y + 1) << 16;
      while (srcY_fp >= ctx->nextOutY_srcStart && ctx->currentOutY < ctx->outHeight) {
        flushScaledRow(ctx);
        ctx->nextOutY_srcStart = static_cast<uint32_t>(ctx->currentOutY + 1) * ctx->scaleY_fp;
        if (srcY_fp >= ctx->nextOutY_srcStart) continue;
        memset(ctx->rowAccum.get(), 0, ctx->outWidth * sizeof(uint32_t));
        memset(ctx->rowCount.get(), 0, ctx->outWidth * sizeof(uint32_t));
      }
    }
  }

  return ctx->error ? 0 : 1;
}

}  // namespace

// =============================================================================
// 渐进式 JPEG 封面：换一个能**全解**的解码器（stb_image）
// =============================================================================
// JPEGDEC 对渐进式（SOF2）只解第一条扫描的 DC 系数 —— 那固定是 1/8 分辨率，也是它
// 能给出的最好结果（jpeg.inl 把 scaleOption 钉成 JPEG_SCALE_EIGHTH 的原因）。封面
// 盒子 396×528（待机整屏 593×890），所以糊多少完全由源图多宽决定：600px 的封面退化
// 到 75px 再双线性放大 5 倍，糊成一团；2644px 的扫描件退化到 330px 只放大 1.2 倍，
// 只是略软。同一格书架上"有的清楚有的糊"就是这么来的 —— 基线与 PNG 走的是
// chooseJpegScale 的 DCT 缩放 + 区域平均，干净利落。
//
// 真修：渐进式来源交给 stb_image（公有领域单头文件；实现 TU 见 StbImageImpl.cpp，
// 兄弟仓库 read_pico_firmware 对同一个问题用的也是它）。全部扫描解出来之后再按区域
// 平均缩到目标尺寸，采样口径与基线那条路的 flushScaledRow 一致，所以两条路清晰度同级。
//
// 代价是内存：渐进式的系数必须整幅留在 RAM 里（每个系数像素 raw_data 1 + raw_coeff
// 2 字节，彩色还要按分量采样比累加），出图再要 3 字节/像素。所以这条路只走得起中小
// 源图（8 MB PSRAM 上大约 800×1200 以内）—— 而更大的源图本来就只是略软（1/8 也够
// 396px 的盒子用），落回 JPEGDEC 老路没有损失。够不够是**按 SOF 算出来的峰值**当场
// 判的，不是拍一个像素上限：判完写一行 INFO，设备自己就把"库里渐进式占多少、够不够
// 全解"报出来。
//
// 位置：在 JPEGDEC 打开**之前**。一是成功时根本不建那套解码器（省 ~18 KB scratch），
// 二是失败时必须能干净回退 —— JPEGDEC 的读取是顺序的、自己记 iPos，中途被我们
// seek(0) 读一遍会把它的状态搞乱，所以只能在 `jpeg->open()` 之前动文件。
// =============================================================================

// SOF 段里能读到的、决定"要不要走 stb"的全部信息。
struct JpegSofInfo {
  int width = 0;
  int height = 0;
  int components = 0;
  uint8_t marker = 0;        // 帧段标记（0xC0/0xC1 基线，0xC2 渐进式）
  uint8_t sampling[4] = {};  // 高 4 位水平、低 4 位垂直采样因子
};

// 沿段结构走到 SOF（不解码、不解熵）。失败一律返回 false —— 判不出来就不接管，
// 让 JPEGDEC 那条老路去处理。
static bool jpegScanSof(const uint8_t* buf, const size_t len, JpegSofInfo* out) {
  if (len < 4 || buf[0] != 0xFF || buf[1] != 0xD8) return false;  // 不是 JPEG
  size_t pos = 2;
  while (pos + 4 <= len) {
    if (buf[pos] != 0xFF) return false;  // 段边界对不上，别猜
    const uint8_t marker = buf[pos + 1];
    if (marker == 0xFF) {  // 段间填充
      pos++;
      continue;
    }
    pos += 2;
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;  // 无载荷
    if (marker == 0xD9 || marker == 0xDA) return false;                  // 到 SOS/EOI 还没见 SOF
    if (pos + 2 > len) return false;
    const size_t segLen = (static_cast<size_t>(buf[pos]) << 8) | buf[pos + 1];
    if (segLen < 2 || pos + segLen > len) return false;
    // SOF0..SOF15，去掉 DHT(0xC4) / JPG(0xC8) / DAC(0xCC) 三个非帧段。
    const bool isSof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
    if (isSof) {
      const uint8_t* p = buf + pos + 2;  // 精度(1) 高(2) 宽(2) 分量数(1) 各分量(3)
      if (segLen < 8) return false;
      out->height = (static_cast<int>(p[1]) << 8) | p[2];
      out->width = (static_cast<int>(p[3]) << 8) | p[4];
      out->components = p[5];
      if (out->width <= 0 || out->height <= 0) return false;
      if (out->components < 1 || out->components > 4) return false;
      if (segLen < 8 + 3 * static_cast<size_t>(out->components)) return false;
      for (int i = 0; i < out->components; i++) out->sampling[i] = p[7 + 3 * i];
      out->marker = marker;
      return true;
    }
    pos += segLen;
  }
  return false;
}

// 渐进式全解的峰值内存（照 stb 的分配算：每个分量 raw_data 1 + raw_coeff 2 字节，
// 尺寸按 MCU 对齐后的 w2×h2；再算上出图的 img_n 通道整幅）。
// / Peak bytes for a full progressive decode, from stb's own allocation pattern.
static size_t progressiveDecodeBytes(const JpegSofInfo& sof) {
  int hMax = 1, vMax = 1;
  for (int i = 0; i < sof.components; i++) {
    const int h = sof.sampling[i] >> 4, v = sof.sampling[i] & 0x0F;
    if (h > hMax) hMax = h;
    if (v > vMax) vMax = v;
  }
  if (hMax <= 0 || vMax <= 0) return SIZE_MAX;
  const size_t mcuX = (static_cast<size_t>(sof.width) + hMax * 8 - 1) / (hMax * 8);
  const size_t mcuY = (static_cast<size_t>(sof.height) + vMax * 8 - 1) / (vMax * 8);
  size_t bytes = 0;
  for (int i = 0; i < sof.components; i++) {
    const int h = sof.sampling[i] >> 4, v = sof.sampling[i] & 0x0F;
    if (h <= 0 || v <= 0) return SIZE_MAX;
    bytes += mcuX * h * 8 * mcuY * v * 8 * 3;
  }
  // 出图那一幅：彩色源我们直接要 3 通道（避免 stb 转换时两幅并存），灰度源 1 通道。
  bytes += static_cast<size_t>(sof.width) * sof.height * (sof.components > 1 ? 3 : 1);
  // stb 自己的固定开销（霍夫曼表、行缓冲、那两三个内部缓冲）实测约 20 KB，跟图大小
  // 基本无关（主机端拿计数分配器量过 200×300 / 600×800 / 900×1200 / 1600×2000 四张，
  // 差值稳定在 19~23 KB）。这里多留一点：峰值估计只许偏大，偏小就是 OOM。
  bytes += 64 * 1024;
  return bytes;
}

// PSRAM 上的裸缓冲。用 memory::ByteBuffer（它的释放是 free()，在
// CONFIG_SPIRAM_USE_MALLOC 下与 heap_caps_malloc 配对，Memory.h 自己的
// makePsramByteBuffer* 就是这么用的）。**不用**那个 helper：它要求在编译期定义
// BOARD_HAS_PSRAM，本工程没有这个宏，它会一律返回空。
// stb 那边的分配器同样钉在 PSRAM（StbImageImpl.cpp）—— 阅读模式下内部堆只剩
// 几十 KB，几百 KB 的文件缓冲进去就是灾难。
static memory::ByteBuffer allocPsramBuffer(const size_t size) {
  if (size == 0) return {};
  return memory::ByteBuffer{static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))};
}

// 输出尺寸：没给目标尺寸就是源尺寸（渐进式沿用 1/8 的旧口径），给了就按 fit/fill 缩放并
// 夹到 1 像素以上。抽出来给两条路共用 —— 渐进式那条要在 JPEGDEC 打开之前就算出与老路
// 一模一样的 outWidth/outHeight。
// / Output dimensions, shared by both decoders so they agree exactly.
static bool computeOutputDims(const int srcWidth, const int srcHeight, const int targetWidth, const int targetHeight,
                              const bool crop, const bool progressiveDecode, int* outWidth, int* outHeight) {
  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  int w = srcWidth;
  int h = srcHeight;
  if (targetWidth <= 0 || targetHeight <= 0) {
    w = progressiveDecode ? (srcWidth + 7) / 8 : srcWidth;
    h = progressiveDecode ? (srcHeight + 7) / 8 : srcHeight;
  } else if (srcWidth != targetWidth || srcHeight != targetHeight) {
    const float scaleToFitWidth = static_cast<float>(targetWidth) / srcWidth;
    const float scaleToFitHeight = static_cast<float>(targetHeight) / srcHeight;
    const float scale = crop ? (scaleToFitWidth > scaleToFitHeight ? scaleToFitWidth : scaleToFitHeight)
                             : (scaleToFitWidth < scaleToFitHeight ? scaleToFitWidth : scaleToFitHeight);
    w = static_cast<int>(srcWidth * scale);
    h = static_cast<int>(srcHeight * scale);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
  }
  if (w <= 0 || h <= 0 || w > MAX_IMAGE_WIDTH || h > MAX_IMAGE_HEIGHT) return false;
  *outWidth = w;
  *outHeight = h;
  return true;
}

enum class ProgressiveResult {
  NotApplicable,  // 不是渐进式 / 内存不够 / 认不出来 —— 交给 JPEGDEC，没有任何输出
  Done,           // 已经写出完整 BMP
  Failed,         // 已经开始写 BMP 却失败了，调用方不要再接着写第二份
};

// 降尺度解那棵树里的分配全走 PSRAM：这本来就是内存最紧的时候（DRAM 那边还要留给
// 解码器和帧缓冲），而且解出来的平面会一直活到写完 BMP。
const pjscaled::Alloc kPsramAlloc = {
    [](size_t bytes) -> void* { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); },
    [](void* p) { heap_caps_free(p); }};

// 两条解码路共用的"源图"。stb 解出来的是 srcW×srcH、每像素 comp（1 或 3）通道；降尺度解
// 出来的是一张 1 通道、行距可能大于宽度的灰度平面（块边界对齐的那种多余列别去读）。
struct GraySource {
  const uint8_t* pixels = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;  // 每行字节数（≥ width·comp）
  int comp = 1;    // 1 = 已是灰度；3 = RGB，需按 77/151/28 转
};

// 按区域平均缩到 outW×outH → 写 8bit BMP。两条路共用同一套取样口径，出来的尺寸和亮度
// 换算与老路逐像素一致。
//
// 三个小缓冲先要下来再说 —— 一旦 header 写出去就只能一路写到底，那之后失败只能是
// Failed（回退给 JPEGDEC 会写出两份 BMP）。所以这函数要么在动笔之前干净地返回
// NotApplicable，要么返回 Done / Failed。
static ProgressiveResult writeScaledGrayBmp(Print& bmpOut, const GraySource& src, const int outW,
                                            const int outH) {
  const int bytesPerRow = (outW + 3) / 4 * 4;
  auto row = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
  auto accum = makeUniqueNoThrow<uint32_t[]>(outW);
  auto xEdges = makeUniqueNoThrow<uint32_t[]>(2 * static_cast<size_t>(outW));  // [0,outW) 起 / [outW,..) 止
  if (!row || !accum || !xEdges) {
    LOG_ERR("JPG", "OOM: 渐进式缩放缓冲");
    return ProgressiveResult::NotApplicable;
  }
  CheckedBmpOutput checked(bmpOut);
  writeBmpHeader8bit(checked, outW, outH);
  if (checked.failed) return ProgressiveResult::Failed;

  // 每个输出列的源列区间，与基线路径的 rowAccum/rowCount 是同一套区域平均。
  // **两端都向下取整**（瓦片互不重叠、恰好铺满 0..srcW）：早期版本把末端写成 ceil，
  // 相邻两列会重叠一列源像素，左边缘被重复计权，整幅输出均值会系统性漂一点
  // （主机端拿 600×800 渐变实测均值掉 0.42 级），做的是缩放不是平移 —— 别改成 ceil。
  uint32_t* xHi = xEdges.get() + outW;
  for (int ox = 0; ox < outW; ox++) {
    uint32_t lo = static_cast<uint32_t>((static_cast<uint64_t>(ox) * src.width) / outW);
    uint32_t hi = static_cast<uint32_t>((static_cast<uint64_t>(ox + 1) * src.width) / outW);
    if (hi <= lo) hi = lo + 1;  // 放大时也要至少吃掉一个源像素
    if (lo >= static_cast<uint32_t>(src.width)) lo = static_cast<uint32_t>(src.width) - 1;
    if (hi > static_cast<uint32_t>(src.width)) hi = static_cast<uint32_t>(src.width);
    xEdges[ox] = lo;
    xHi[ox] = hi;
  }

  for (int oy = 0; oy < outH; oy++) {
    const uint32_t y0 = static_cast<uint32_t>((static_cast<uint64_t>(oy) * src.height) / outH);
    uint32_t y1 = static_cast<uint32_t>((static_cast<uint64_t>(oy + 1) * src.height) / outH);  // 同 x：两端都向下取整
    if (y1 <= y0) y1 = y0 + 1;
    if (y1 > static_cast<uint32_t>(src.height)) y1 = static_cast<uint32_t>(src.height);
    memset(accum.get(), 0, static_cast<size_t>(outW) * sizeof(uint32_t));
    for (uint32_t y = y0; y < y1; y++) {
      const uint8_t* srcRow = src.pixels + static_cast<size_t>(y) * src.stride;
      uint32_t* acc = accum.get();
      for (int ox = 0; ox < outW; ox++, acc++) {
        const uint8_t* p = srcRow + static_cast<size_t>(xEdges[ox]) * src.comp;
        uint32_t sum = 0;
        for (uint32_t x = xEdges[ox]; x < xHi[ox]; x++, p += src.comp) {
          // 灰度源直接用；彩色按 stb 同一套亮度权重（77/151/28，和为 256）转一趟。
          sum += src.comp == 1 ? p[0] : static_cast<uint32_t>((77u * p[0] + 151u * p[1] + 28u * p[2] + 128u) >> 8);
        }
        *acc += sum;
      }
    }
    const uint32_t rows = y1 - y0;
    uint8_t* dst = row.get();
    for (int ox = 0; ox < outW; ox++) {
      const uint32_t v = accum[ox] / ((xHi[ox] - xEdges[ox]) * rows);
      dst[ox] = v > 255 ? 255 : static_cast<uint8_t>(v);
    }
    memset(dst + outW, 0, static_cast<size_t>(bytesPerRow - outW));
    checked.write(dst, bytesPerRow);
    if ((oy & 15) == 0) vTaskDelay(1);  // 与 IO 路径同样的礼让
  }
  if (checked.failed) {
    LOG_ERR("JPG", "封面 BMP 写入失败（渐进式）");
    return ProgressiveResult::Failed;
  }
  return ProgressiveResult::Done;
}

// 读整个文件 → stb 全解灰度 → 区域平均缩到目标尺寸 → 写 8bit BMP。
// 只有"源是渐进式 JPEG + 目标 Gray8"才可能返回 Done；其余情况返回 NotApplicable，
// 且保证没往 bmpOut 写过任何字节。
static ProgressiveResult progressiveJpegToGrayBmp(HalFile& file, Print& bmpOut, const int targetWidth,
                                                  const int targetHeight, const bool crop) {
  constexpr size_t MAX_FILE_BYTES = 4u << 20;  // stbi_load_from_memory 的长度参数是 int
  const size_t fileBytes = file.size();
  if (fileBytes < 16 || fileBytes > MAX_FILE_BYTES) return ProgressiveResult::NotApplicable;

  // 整个文件读进来：渐进式的解码要来回跳扫描，边读边解不划算（而且我们反正要把
  // 全部扫描喂给 stb）。放不下就安静回退 —— 大文件对应的源图本来就只是略软。
  memory::ByteBuffer fileBuf = allocPsramBuffer(fileBytes);
  if (!fileBuf) return ProgressiveResult::NotApplicable;
  if (!file.seek(0)) return ProgressiveResult::NotApplicable;
  size_t got = 0;
  while (got < fileBytes) {
    const int n = file.read(fileBuf.get() + got, fileBytes - got);
    if (n <= 0) break;
    got += static_cast<size_t>(n);
  }
  if (got != fileBytes) {
    LOG_WRN("JPG", "封面文件只读到 %u/%u 字节，落回 JPEGDEC", static_cast<unsigned>(got),
            static_cast<unsigned>(fileBytes));
    return ProgressiveResult::NotApplicable;
  }

  JpegSofInfo sof;
  if (!jpegScanSof(fileBuf.get(), got, &sof)) {
    LOG_INF("JPG", "封面：段结构不认识（非 JPEG 或截断），走 JPEGDEC");
    return ProgressiveResult::NotApplicable;
  }
  if (sof.marker != 0xC2) {
    LOG_INF("JPG", "封面源 %dx%d / %d 分量：非渐进式（SOF 0x%02X），走 JPEGDEC（DCT 缩放 + 区域平均）", sof.width,
            sof.height, sof.components, sof.marker);
    return ProgressiveResult::NotApplicable;
  }

  // 目标尺寸先定下来：两条解码路共用，而且要在解码之前就算得出来（越界就早退，别白解）。
  int outW = 0, outH = 0;
  if (!computeOutputDims(sof.width, sof.height, targetWidth, targetHeight, crop, true, &outW, &outH)) {
    LOG_WRN("JPG", "封面源 %dx%d：目标尺寸越界", sof.width, sof.height);
    return ProgressiveResult::NotApplicable;
  }

  const size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t decodeBytes = progressiveDecodeBytes(sof);

  // ---- 路一：stb 全解（源够小、PSRAM 放得下时质量最好：真·全分辨率）--------------------
  // fileBuf 已经在账上了，这里只判解码要的那份。
  if (decodeBytes != SIZE_MAX && freePsram >= decodeBytes + memory::PSRAM_FREE_RESERVE) {
    // 彩色直接要 3 通道、灰度要 1 通道：与源通道数一致时 stb 不必再分配一幅转换结果
    // （3→1 的转换会让两幅整图同时活着）。
    const int reqComp = sof.components > 1 ? 3 : 1;
    int srcW = 0, srcH = 0, comp = 0;
    const int64_t startedUs = esp_timer_get_time();
    stbi_uc* src = stbi_load_from_memory(fileBuf.get(), static_cast<int>(got), &srcW, &srcH, &comp, reqComp);
    if (src) {
      const ScopedCleanup freeSrc{[src]() { stbi_image_free(src); }};
      fileBuf.reset();  // 解完就能放：后面的大头是解出来的那幅像素
      LOG_DBG("JPG", "Progressive full decode: %dx%d -> %dx%d", srcW, srcH, outW, outH);
      const GraySource gs{src, srcW, srcH, srcW * reqComp, reqComp};
      const ProgressiveResult r = writeScaledGrayBmp(bmpOut, gs, outW, outH);
      if (r == ProgressiveResult::Done) {
        LOG_INF("JPG", "封面源 %dx%d / %d 分量：渐进式 → stb 全解 %dx%d，峰值约 %u KB，%u ms", srcW, srcH, comp,
                outW, outH, static_cast<unsigned>(decodeBytes / 1024),
                static_cast<unsigned>((esp_timer_get_time() - startedUs) / 1000));
      }
      return r;
    }
    LOG_WRN("JPG", "封面源 %dx%d：渐进式全解失败（%s），改走降尺度解", sof.width, sof.height,
            stbi_failure_reason());
  } else {
    LOG_INF("JPG", "封面源 %dx%d / %d 分量：渐进式，全解约需 %u KB > PSRAM 空闲 %u KB，改走降尺度解",
            sof.width, sof.height, sof.components, static_cast<unsigned>(decodeBytes / 1024),
            static_cast<unsigned>(freePsram / 1024));
  }

  // ---- 路二：降尺度解（渐进式专用）------------------------------------------------------
  //
  // 全解放不下时走这条：只解低频、把 8×8 块的 64 个系数折叠回 S×S，内存随**目标盒子**走
  // （1200×1600 的封面 1/2 约 2.5 MB、1/4 约 1.2 MB），所以大封面也进得来，而且出来的就是
  // 精确的箱平均（对拍 djpeg -scale，mean ≤0.3 级）。
  //   · 1/2（S=4）优先 —— "糊"就糊在 JPEGDEC 的渐进式被钉死在 1/8 上，1/2 比它细四倍；
  //   · 放不下才退 1/4（S=2）；
  //   · 1/8 不做：它和 JPEGDEC 的 DC 解完全等价（都是 8×8 块平均），再读一遍文件不划算。
  int hMax = 1, vMax = 1;
  for (int i = 0; i < sof.components; i++) {
    const int h = sof.sampling[i] >> 4, v = sof.sampling[i] & 0x0F;
    if (h > hMax) hMax = h;
    if (v > vMax) vMax = v;
  }
  int yH = sof.sampling[0] >> 4, yV = sof.sampling[0] & 0x0F;  // 0 号分量就是亮度
  if (yH < 1) yH = 1;
  if (yV < 1) yV = 1;

  for (int scaleLog2 = 1; scaleLog2 <= 2; scaleLog2++) {
    const size_t need = pjscaled::peakBytes(sof.width, sof.height, yH, yV, hMax, vMax, scaleLog2);
    if (need == 0 || freePsram < need + memory::PSRAM_FREE_RESERVE) {
      LOG_INF("JPG", "封面源 %dx%d：降尺度 1/%d 约需 %u KB > PSRAM 空闲 %u KB，退一档", sof.width, sof.height,
              1 << scaleLog2, static_cast<unsigned>((need + 1023) / 1024),
              static_cast<unsigned>(freePsram / 1024));
      continue;
    }
    pjscaled::Plane plane;
    const int64_t startedUs = esp_timer_get_time();
    if (!pjscaled::decodeGray(fileBuf.get(), got, scaleLog2, kPsramAlloc, &plane)) {
      LOG_INF("JPG", "封面源 %dx%d：降尺度 1/%d 认不出来（段结构不认识 / 截断），退一档", sof.width, sof.height,
              1 << scaleLog2);
      continue;
    }
    const ScopedCleanup freePlane{[&plane]() { kPsramAlloc.release(plane.pixels); }};
    const GraySource gs{plane.pixels, plane.width, plane.height, plane.stride, 1};
    const ProgressiveResult r = writeScaledGrayBmp(bmpOut, gs, outW, outH);
    if (r == ProgressiveResult::Done) {
      LOG_INF("JPG", "封面源 %dx%d / %d 分量：渐进式 → 降尺度 1/%d 解出 %dx%d，峰值约 %u KB，%u ms", sof.width,
              sof.height, sof.components, 1 << scaleLog2, plane.width, plane.height,
              static_cast<unsigned>(need / 1024),
              static_cast<unsigned>((esp_timer_get_time() - startedUs) / 1000));
    }
    return r;
  }

  LOG_INF("JPG", "封面源 %dx%d：降尺度解也放不下，落回 JPEGDEC 的 1/8（略软）", sof.width, sof.height);
  return ProgressiveResult::NotApplicable;
}

// Internal implementation with configurable target size and bit depth
bool JpegToBmpConverter::jpegFileToBmpStreamInternal(HalFile& jpegFile, Print& bmpOut, int targetWidth,
                                                     int targetHeight, Output output, bool crop) {
  LOG_DBG("JPG", "Converting JPEG to %s BMP (target: %dx%d)",
          output == Output::Mono1   ? "1-bit"
          : output == Output::Gray8 ? "8-bit"
                                    : "2-bit",
          targetWidth, targetHeight);

  // 渐进式灰度封面先试 stb 全解（见上面的长注释）。必须在 JPEGDEC 打开之前 ——
  // 我们这里要 seek(0) 把文件整个读一遍，落在那之后会把解码器的读取状态搞乱。
  if (output == Output::Gray8) {
    switch (progressiveJpegToGrayBmp(jpegFile, bmpOut, targetWidth, targetHeight, crop)) {
      case ProgressiveResult::Done:
        return true;
      case ProgressiveResult::Failed:
        return false;  // 已经写了一半 BMP，绝不能再让下面接第二份
      case ProgressiveResult::NotApplicable:
        break;  // 老路（对基线本来就够好，对渐进式是 1/8 兜底）
    }
  }

  // Cover generation already lends the 48KB framebuffer. Reuse it for the
  // 17.9KB decoder after ZIP extraction releases its inflate claim; callers
  // without a loan retain the existing fallible heap path.
  uint8_t* decoderScratch = buildscratch::claim(JPEG_DECODER_SIZE);
  if (!decoderScratch && (ESP.getFreeHeap() < MIN_FREE_HEAP || ESP.getMaxAllocHeap() < JPEG_DECODER_SIZE)) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (free=%u need=%u, maxAlloc=%u need=%u)", ESP.getFreeHeap(),
            MIN_FREE_HEAP, ESP.getMaxAllocHeap(), JPEG_DECODER_SIZE);
    return false;
  }

  s_jpegFile = &jpegFile;

  std::unique_ptr<JPEGDEC> heapJpeg;
  JPEGDEC* jpeg = nullptr;
  if (decoderScratch) {
    jpeg = ::new (static_cast<void*>(decoderScratch)) JPEGDEC();
  } else {
    heapJpeg = makeUniqueNoThrow<JPEGDEC>();
    jpeg = heapJpeg.get();
  }
  if (!jpeg) {
    LOG_ERR("JPG", "OOM: JPEG decoder");
    return false;
  }
  const ScopedCleanup releaseDecoderScratch{[jpeg, decoderScratch]() {
    if (!decoderScratch) return;
    jpeg->~JPEGDEC();
    buildscratch::release(decoderScratch);
  }};
  const ScopedCleanup closeJpeg{[jpeg]() {
    jpeg->close();
    // s_jpegFile 指向本函数栈上的 HalFile（jpegFile）。函数一返回它就是野指针，
    // 现在只有 bmpJpegOpen 在本次调用内解引用、且带 !*s_jpegFile 兜底，所以还没出事；
    // 但留着就是给将来埋雷，退出时清掉。
    s_jpegFile = nullptr;
  }};

  int rc = jpeg->open("", bmpJpegOpen, bmpJpegClose, bmpJpegRead, bmpJpegSeek, bmpDrawCallback);
  if (rc != 1) {
    LOG_ERR("JPG", "JPEG open failed (err=%d)", jpeg->getLastError());
    return false;
  }

  const int srcWidth = jpeg->getWidth();
  const int srcHeight = jpeg->getHeight();
  const bool progressiveDecode = (jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE);

  LOG_DBG("JPG", "JPEG dimensions: %dx%d", srcWidth, srcHeight);

  // 源图尺寸这里只挡病态文件；**真正的内存约束是缩放后的宽度**，见下面 scaleSrcWidth
  // 那处检查。原来这里直接拿 2048×3072 卡源尺寸，比真约束严得多：2644×3840 的扫描封面
  // 走 1/8 缩放后一行只有 331 px，离解码器内部像素缓冲（JPEGDEC 的 MAX_BUFFERED_PIXELS
  // = 2048）还差得远，却会被当成 "too large" 整本丢掉。而且原来那句是 LOG_DBG，默认日志
  // 级别下连一行提示都没有，用户侧的表现就是"这本书没有封面"。实测（主机端跑同一份
  // JPEGDEC）：2644×3840 渐进式封面在 1/8 缩放下 rc=1，解码完全正常。
  constexpr int MAX_SOURCE_DIM = 16384;  // JPEG 规格上限 65535，取 2^14 做兜底
  if (srcWidth <= 0 || srcHeight <= 0 || srcWidth > MAX_SOURCE_DIM || srcHeight > MAX_SOURCE_DIM) {
    LOG_WRN("JPG", "Image has invalid or absurd dimensions (%dx%d)", srcWidth, srcHeight);
    return false;
  }

  // 输出侧的尺寸与上限（BMP 行缓冲 / 缩放后 MCU 行）都在 computeOutputDims 里，
  // 渐进式那条路在 JPEGDEC 打开之前算的就是同一个函数，两条路的 outWidth/outHeight
  // 必须逐字节一致（否则同一本书会因为走了哪条路而尺寸不同）。
  int outWidth = 0;
  int outHeight = 0;
  if (!computeOutputDims(srcWidth, srcHeight, targetWidth, targetHeight, crop, progressiveDecode, &outWidth,
                         &outHeight)) {
    LOG_WRN("JPG", "Image has absurd output dimensions (%dx%d) from source %dx%d", outWidth, outHeight, srcWidth,
            srcHeight);
    return false;
  }

  uint32_t scaleX_fp = 65536;  // 1.0 in 16.16 fixed point
  uint32_t scaleY_fp = 65536;
  bool needsScaling = false;

  // Use JPEGDEC's reduced DCT output before the line-buffered fine scaling.
  // This bounds a 2048-pixel source's MCU buffer to 4 KB at 1/8 scale instead
  // of allocating 32 KB just to produce a 112x164 shelf cover.
  const int scaleOption =
      progressiveDecode ? JPEG_SCALE_EIGHTH : chooseJpegScale(srcWidth, srcHeight, outWidth, outHeight);
  const int scaleDenominator = scaleOption == 0 ? 1 : scaleOption;
  const int scaleSrcWidth = (srcWidth + scaleDenominator - 1) / scaleDenominator;
  const int scaleSrcHeight = (srcHeight + scaleDenominator - 1) / scaleDenominator;
  LOG_DBG("JPG", "JPEG decoder scale: 1/%d (%dx%d -> %dx%d), output=%dx%d", scaleDenominator, srcWidth, srcHeight,
          scaleSrcWidth, scaleSrcHeight, outWidth, outHeight);

  // 解码器内部只有一行 MAX_BUFFERED_PIXELS 宽的像素缓冲，>会写穿，<不会。这才是"图片
  // 太大"的真正判据，也是 MCU 行缓冲（MAX_MCU_HEIGHT × scaleSrcWidth）的实际上限：
  // 卡在 2048 时最坏 16×2048 = 32 KB，与旧代码（源宽 ≤ 2048 时 scaleSrcWidth 也 ≤ 2048）
  // 的峰值完全一致 —— 所以放宽源尺寸不会让内存占用变大，只是不再误杀大扫描件。
  if (scaleSrcWidth > MAX_BUFFERED_PIXELS) {
    LOG_WRN("JPG", "Scaled width %d exceeds decoder buffer (%d): src %dx%d at 1/%d", scaleSrcWidth,
            MAX_BUFFERED_PIXELS, srcWidth, srcHeight, scaleDenominator);
    return false;
  }

  if (scaleSrcWidth != outWidth || scaleSrcHeight != outHeight) {
    scaleX_fp = (static_cast<uint32_t>(scaleSrcWidth) << 16) / outWidth;
    scaleY_fp = (static_cast<uint32_t>(scaleSrcHeight) << 16) / outHeight;
    needsScaling = true;
  }

  const bool smoothUpscale =
      progressiveDecode && needsScaling && scaleSrcWidth <= outWidth && scaleSrcHeight <= outHeight;

  CheckedBmpOutput checkedOutput(bmpOut);
  // Write BMP header with output dimensions（尺寸与上限已经由 computeOutputDims 把关）
  int bytesPerRow = 0;
  switch (output) {
    case Output::Gray8:
      writeBmpHeader8bit(checkedOutput, outWidth, outHeight);
      bytesPerRow = (outWidth + 3) / 4 * 4;
      break;
    case Output::Mono1:
      writeBmpHeader1bit(checkedOutput, outWidth, outHeight);
      bytesPerRow = (outWidth + 31) / 32 * 4;
      break;
    case Output::Gray2:
      writeBmpHeader2bit(checkedOutput, outWidth, outHeight);
      bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
      break;
  }

  BmpConvertCtx ctx = {};
  if (checkedOutput.failed || bytesPerRow == 0) return false;
  ctx.bmpOut = &checkedOutput;
  ctx.srcWidth = scaleSrcWidth;
  ctx.srcHeight = scaleSrcHeight;
  ctx.outWidth = outWidth;
  ctx.outHeight = outHeight;
  ctx.output = output;
  ctx.bytesPerRow = bytesPerRow;
  ctx.needsScaling = needsScaling;
  ctx.scaleX_fp = scaleX_fp;
  ctx.scaleY_fp = scaleY_fp;
  ctx.smoothUpscale = smoothUpscale;
  ctx.smoothScaleX_fp = interpolationStep(ctx.srcWidth, outWidth);
  ctx.smoothScaleY_fp = interpolationStep(ctx.srcHeight, outHeight);
  ctx.smoothNextOutY = 0;
  ctx.smoothPrevY = -1;
  ctx.rowsSinceYield = 0;
  ctx.blocksSinceYield = 0;
  ctx.error = false;

  // MCU row buffer: MAX_MCU_HEIGHT rows × decoded srcWidth columns of grayscale
  ctx.mcuBuf = makeUniqueNoThrow<uint8_t[]>(MAX_MCU_HEIGHT * ctx.srcWidth);
  if (!ctx.mcuBuf) {
    LOG_ERR("JPG", "OOM: MCU buffer (%d bytes)", MAX_MCU_HEIGHT * ctx.srcWidth);
    return false;
  }
  memset(ctx.mcuBuf.get(), 0, MAX_MCU_HEIGHT * ctx.srcWidth);

  ctx.bmpRow = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
  if (!ctx.bmpRow) {
    LOG_ERR("JPG", "OOM: BMP row buffer");
    return false;
  }

  if (smoothUpscale) {
    // One contiguous allocation avoids three heap blocks while keeping smoothing line-buffered.
    const size_t smoothRowsBytes = static_cast<size_t>(outWidth) * 3;
    ctx.smoothRows = makeUniqueNoThrow<uint8_t[]>(smoothRowsBytes);
    if (!ctx.smoothRows) {
      LOG_ERR("JPG", "OOM: progressive smoothing buffers");
      return false;
    }
    ctx.smoothPrevRow = ctx.smoothRows.get();
    ctx.smoothCurrRow = ctx.smoothPrevRow + outWidth;
    ctx.smoothOutRow = ctx.smoothCurrRow + outWidth;
    LOG_DBG("JPG", "Progressive smoothing: %dx%d -> %dx%d, buffers=%u bytes", ctx.srcWidth, ctx.srcHeight, outWidth,
            outHeight, static_cast<unsigned>(smoothRowsBytes));
  } else if (needsScaling) {
    ctx.rowAccum = makeUniqueNoThrow<uint32_t[]>(outWidth);
    ctx.rowCount = makeUniqueNoThrow<uint32_t[]>(outWidth);
    if (!ctx.rowAccum || !ctx.rowCount) {
      LOG_ERR("JPG", "OOM: scaling buffers");
      return false;
    }
    ctx.nextOutY_srcStart = scaleY_fp;
  }

  if (output == Output::Mono1) {
    ctx.atkinson1BitDitherer = makeUniqueNoThrow<Atkinson1BitDitherer>(outWidth);
    if (!ctx.atkinson1BitDitherer || !ctx.atkinson1BitDitherer->valid()) {
      LOG_ERR("JPG", "OOM: Atkinson1BitDitherer");
      return false;
    }
  } else if (output == Output::Gray2) {
    if (USE_ATKINSON) {
      ctx.atkinsonDitherer = makeUniqueNoThrow<AtkinsonDitherer>(outWidth);
      if (!ctx.atkinsonDitherer || !ctx.atkinsonDitherer->valid()) {
        LOG_ERR("JPG", "OOM: AtkinsonDitherer");
        return false;
      }
    } else if (USE_FLOYD_STEINBERG) {
      ctx.fsDitherer = makeUniqueNoThrow<FloydSteinbergDitherer>(outWidth);
      if (!ctx.fsDitherer || !ctx.fsDitherer->valid()) {
        LOG_ERR("JPG", "OOM: FloydSteinbergDitherer");
        return false;
      }
    }
  }

  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);

  rc = jpeg->decode(0, 0, scaleOption);

  if (rc == 1 && ctx.smoothUpscale && !ctx.error) {
    finishSmoothUpscale(&ctx);
  }

  if (rc != 1 || ctx.error || checkedOutput.failed) {
    LOG_ERR("JPG", "JPEG decode failed (rc=%d, err=%d)", rc, jpeg->getLastError());
    return false;
  }

  LOG_DBG("JPG", "Successfully converted JPEG to BMP");
  return true;
}

// 封面 BMP 的目标尺寸。原来用的是屏幕的旋转尺寸（竖屏 684×1216），那是照整屏算的；
// 封面唯一的消费者是货架格子 —— 横屏 6×2 最大 168×224、竖屏 2×2 最大 195×260
// （3:4，见 screen_reader.cpp 的 drawCoverCell）。按 2 倍采样密度取 396×528 已经超过
// 缩略图所需，而且货架每移动一次光标就整屏重画一遍（封面没有任何内存缓存，每次都
// 逐行读盘），尺寸直接决定翻书架的跟手感。调用方一律 crop=false，输出不会超过这个
// 盒子，所以宽扁封面也不会撑出大文件。
// / Cover BMP target size. It used to be the rotated screen size (684×1216), sized for
// a whole page; the only consumer is the shelf cell (at most 195×260, 3:4 — see
// drawCoverCell). 396×528 is already 2× the sampling density a thumbnail needs, and
// the shelf re-reads every cover off SD on each redraw with no in-RAM cache, so the
// size directly sets how the shelf feels to navigate. Callers pass crop=false, so the
// output never exceeds this box even for a wide cover.
constexpr int COVER_BMP_MAX_WIDTH = 396;
constexpr int COVER_BMP_MAX_HEIGHT = 528;

// Core function: Convert JPEG file to BMP (uses the cover target size above)
bool JpegToBmpConverter::jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop, Output output) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, COVER_BMP_MAX_WIDTH, COVER_BMP_MAX_HEIGHT, output, crop);
}

// 自定义目标尺寸 / 位深 / 裁切。默认值就是老行为（2 位灰阶 + 裁切），改动只是把这两个
// 参数放出来给"待机整屏封面"用（Gray8 + crop=true）。
bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight, Output output, bool crop) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, output, crop);
}

// Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                         int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, Output::Mono1, true);
}
