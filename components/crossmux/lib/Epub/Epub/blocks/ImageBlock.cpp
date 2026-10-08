#include "ImageBlock.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <esp_heap_caps.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/PixelCache.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t version (= PixelCache::kFormatVersion, 3 = 16-level 4bpp)
// - uint8_t pixels[...] - 4 bits per pixel, packed (2 pixels per byte: even column
//   in the low nibble), row-major order
//
// 4bpp 而不是 2bpp：与 framebuffer 同一口径，16 级灰原样进出，渲染时不再
// 二次量化。

ImageBlock::ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height)
    : imagePath(imagePath), srcPath(srcPath), width(width), height(height) {}

void* ImageBlock::extractCtx = nullptr;
ImageBlock::ExtractFn ImageBlock::extractFn = nullptr;

// Reader-scoped image resampling choice. The library must not reach into the
// application's settings, so the reader pushes this in (same pattern as
// setExtractor()) and the decode path reads it here.
bool ImageBlock::bilinearScaling = false;

// 16 级量化的抖动档。库不读应用设置，由阅读器推入（同 setBilinearScaling）。
DitherMode ImageBlock::ditherMode = DitherMode::Ordered;

void ImageBlock::setExtractor(void* ctx, ExtractFn fn) {
  extractCtx = ctx;
  extractFn = fn;
}

void ImageBlock::setBilinearScaling(const bool enabled) {
  if (bilinearScaling == enabled) return;
  bilinearScaling = enabled;
  // Logged on every transition: the filter also changes the pixel-cache name, so
  // this line is what proves the reader actually re-decoded with the new setting
  // rather than serving the other variant's cache.
  LOG_INF("IMG", "Image resampling filter -> %s", enabled ? "bilinear" : "nearest");
}

void ImageBlock::setDitherMode(const DitherMode mode) {
  if (ditherMode == mode) return;
  ditherMode = mode;
  const char* name = mode == DitherMode::Row ? "row" : (mode == DitherMode::None ? "none" : "ordered");
  // Same reasoning as setBilinearScaling: the dither pattern is baked into the
  // cached pixels, so this line is the ground truth for "did the reader really
  // re-decode with the new mode".
  LOG_INF("IMG", "Image dither mode -> %s", name);
}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

std::string getCachePath(const std::string& imagePath) {
  // Replace extension with a cache name that encodes the payload format, the
  // resampling filter and the dither mode. The cached file holds *already
  // quantized* pixels, so any of the three changing must not serve the old file:
  //   g16 = 16-level 4bpp payload   (2bpp caches from older builds never match,
  //                                  and are simply never opened again)
  //   b   = bilinear resampling     (pixels are scaled before caching)
  //   o/r/n = Ordered / Row / None  (the dither pattern is baked in)
  // e.g. ".g16o.pxc", ".g16br.pxc". The version byte in the header is the second
  // line of defence; this one avoids even opening a mismatched file.
  const char* mode = "o";
  switch (ImageBlock::ditherModeEnabled()) {
    case DitherMode::Row: mode = "r"; break;
    case DitherMode::None: mode = "n"; break;
    case DitherMode::Ordered:
    default: break;
  }
  std::string suffix = ImageBlock::bilinearScalingEnabled() ? ".g16b" : ".g16";
  suffix += mode;
  suffix += ".pxc";
  const size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + suffix;
  }
  return imagePath + suffix;
}

// bytesPerRow of the 4bpp payload for a given image width.
constexpr size_t cacheRowBytes(int width) { return static_cast<size_t>((width + 1) / 2); }

bool readValidCacheHeader(HalFile& cacheFile, const int expectedWidth, const int expectedHeight, uint16_t& cachedWidth,
                          uint16_t& cachedHeight) {
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }
  uint8_t version = 0;
  if (cacheFile.read(&version, 1) != 1 || version != PixelCache::kFormatVersion) {
    return false;
  }

  const int widthDiff = abs(cachedWidth - expectedWidth);
  const int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    return false;
  }

  const size_t expectedSize = PixelCache::kHeaderBytes + cacheRowBytes(cachedWidth) * cachedHeight;
  return cacheFile.size() >= expectedSize;
}

// Pages are deserialized afresh on each visit. Keep a bounded, allocation-free
// record so an image that failed renders its placeholder directly for the rest
// of the reader session instead of paying another placeholder refresh and
// decode. The reader clears this on entry so transient memory/storage failures
// are retried.
constexpr size_t MAX_SESSION_IMAGE_FAILURES = 16;
uint64_t failedImageHashes[MAX_SESSION_IMAGE_FAILURES];
size_t failedImageCount = 0;

uint64_t imagePathHash(const std::string& path) {
  uint64_t hash = 14695981039346656037ull;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool imageFailedThisSession(const std::string& path) {
  const uint64_t hash = imagePathHash(path);
  for (size_t i = 0; i < failedImageCount; i++) {
    if (failedImageHashes[i] == hash) return true;
  }
  return false;
}

void rememberImageFailure(const std::string& path) {
  if (failedImageCount == MAX_SESSION_IMAGE_FAILURES || imageFailedThisSession(path)) return;
  failedImageHashes[failedImageCount++] = imagePathHash(path);
}

// --- Per-page-render RAM slot for the pixel cache ----------------------------
// A page render draws its image more than once (each full re-render of the page:
// opening a menu overlay, leaving the settings tab, ...), and each draw re-reads
// the whole .pxc off SD (~100 ms for a full-page image). Column clipping cannot
// reduce the SD traffic: the row stride is smaller than an SD sector, so every
// sector is touched regardless of the band window. Instead the first draw loads
// the payload into RAM and later draws render from it. Chunked allocation
// because a single full-image block (up to 192 KB at 4bpp) rarely fits the
// fragmented mid-render heap; all chunks are allocated before the file is read,
// and any failure falls back to streaming. The reader releases the slot when the
// page render completes, so nothing stays resident across page turns.
constexpr size_t PXC_CHUNK_SHIFT = 14;  // 16 KB chunks
constexpr size_t PXC_CHUNK_SIZE = 1u << PXC_CHUNK_SHIFT;
// 192 KB. Was 6 chunks (96 KB) at 2bpp: the payload doubles with the 4bpp cache,
// so keeping 6 would drop every medium illustration that used to fit into RAM
// (48-96 KB at 2bpp) back onto the streaming path. Full-page images never fit at
// either size and stream as before.
constexpr size_t PXC_MAX_CHUNKS = 12;
constexpr size_t PXC_HEAP_RESERVE = 24 * 1024;
constexpr size_t PXC_MAX_ALLOC_RESERVE = 8 * 1024;
// Rows can straddle a chunk boundary; they are reassembled into a stack buffer.
// 208 B was the 2bpp row of an 832px-wide image; the 4bpp equivalent (416 B) keeps
// the same coverage. Portrait body images are ~600px wide (300 B) and fit;
// landscape full-width ones (~1100px) exceeded the 2bpp limit already and still do.
constexpr int PXC_MAX_BYTES_PER_ROW = 420;

memory::ByteBuffer pxcChunks[PXC_MAX_CHUNKS];
uint64_t pxcSlotHash = 0;
uint16_t pxcSlotWidth = 0;
uint16_t pxcSlotHeight = 0;

// PSRAM 裸缓冲。**不要**用 memory::makePsramByteBuffer*：它挂在 `#if defined(BOARD_HAS_PSRAM)`
// 上，而本工程从不定义这个宏 —— 那两个 helper 恒返回空，编译器还会把下游的 `if (buf)`
// 整块当死代码删掉（症状：编译过了、刷机了、屏幕上一点变化都没有）。同样的坑与更长的说明
// 记在 JpegToBmpConverter.cpp 的 allocPsramBuffer 和 Epub/converters/JpegToFramebufferConverter.cpp。
// ByteBuffer 的释放是 free()，在 CONFIG_SPIRAM_USE_MALLOC 下与 heap_caps_malloc 配对。
memory::ByteBuffer allocPsramChunk(size_t size) {
  if (size == 0) return {};
  return memory::ByteBuffer{static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))};
}

// memory::psramHasHeadroom 是同一个空桩（恒 false），所以这道门也得自己算，否则下面
// allocateChunks(true) 永远轮不到。口径与 Memory.h 里那份逐字一致。
bool psramSlotHasHeadroom(size_t totalBytes, size_t contiguousBytes) {
  return memory::hasAllocationHeadroom(heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                                       heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), totalBytes,
                                       contiguousBytes, memory::PSRAM_FREE_RESERVE, PXC_MAX_ALLOC_RESERVE);
}

void releasePxcSlot() {
  for (auto& chunk : pxcChunks) chunk.reset();
  pxcSlotHash = 0;
  pxcSlotWidth = 0;
  pxcSlotHeight = 0;
}

const uint8_t* pxcRowPtr(size_t rowStart, int bytesPerRow, uint8_t* tempRow) {
  const size_t chunk = rowStart >> PXC_CHUNK_SHIFT;
  const size_t offset = rowStart & (PXC_CHUNK_SIZE - 1);
  if (offset + bytesPerRow <= PXC_CHUNK_SIZE) {
    return pxcChunks[chunk].get() + offset;
  }
  const size_t firstPart = PXC_CHUNK_SIZE - offset;
  memcpy(tempRow, pxcChunks[chunk].get() + offset, firstPart);
  memcpy(tempRow + firstPart, pxcChunks[chunk + 1].get(), bytesPerRow - firstPart);
  return tempRow;
}

// cacheFile is positioned just past the header. True when the slot holds the
// full pixel payload for this cache path afterward.
bool loadPxcSlot(uint64_t cacheHash, HalFile& cacheFile, uint16_t cachedWidth, uint16_t cachedHeight, int bytesPerRow) {
  releasePxcSlot();
  if (bytesPerRow > PXC_MAX_BYTES_PER_ROW) {
    return false;
  }
  const size_t payloadBytes = static_cast<size_t>(bytesPerRow) * cachedHeight;
  const size_t chunkCount = (payloadBytes + PXC_CHUNK_SIZE - 1) >> PXC_CHUNK_SHIFT;
  if (chunkCount == 0 || chunkCount > PXC_MAX_CHUNKS) {
    return false;
  }

  const size_t largestChunk = payloadBytes < PXC_CHUNK_SIZE ? payloadBytes : PXC_CHUNK_SIZE;
  const auto allocateChunks = [&](const bool usePsram) {
    size_t remaining = payloadBytes;
    for (size_t i = 0; i < chunkCount; i++) {
      const size_t want = remaining < PXC_CHUNK_SIZE ? remaining : PXC_CHUNK_SIZE;
      pxcChunks[i] = usePsram ? allocPsramChunk(want) : memory::makeInternalByteBufferNoThrow(want);
      if (!pxcChunks[i]) {
        releasePxcSlot();
        return false;
      }
      remaining -= want;
    }
    return true;
  };

  const bool internalFits = memory::hasAllocationHeadroom(ESP.getFreeHeap(), ESP.getMaxAllocHeap(), payloadBytes,
                                                          largestChunk, PXC_HEAP_RESERVE, PXC_MAX_ALLOC_RESERVE);
  if (!internalFits || !allocateChunks(false)) {
    if (!psramSlotHasHeadroom(payloadBytes, largestChunk) || !allocateChunks(true)) {
      return false;
    }
  }

  size_t remaining = payloadBytes;
  for (size_t i = 0; i < chunkCount; i++) {
    const size_t want = remaining < PXC_CHUNK_SIZE ? remaining : PXC_CHUNK_SIZE;
    if (cacheFile.read(pxcChunks[i].get(), want) != static_cast<int>(want)) {
      releasePxcSlot();
      return false;
    }
    remaining -= want;
  }
  pxcSlotHash = cacheHash;
  pxcSlotWidth = cachedWidth;
  pxcSlotHeight = cachedHeight;
  return true;
}

void renderRowsFromPxcSlot(GfxRenderer& renderer, int x, int y) {
  const int bytesPerRow = (pxcSlotWidth + 1) / 2;
  uint8_t tempRow[PXC_MAX_BYTES_PER_ROW];

  DirectPixelWriter pw;
  pw.init(renderer);

  for (int row = 0; row < pxcSlotHeight; row++) {
    const uint8_t* rowBuffer = pxcRowPtr((size_t)row * bytesPerRow, bytesPerRow, tempRow);
    pw.beginRow(y + row);
    int colStart, colEnd;
    pw.bandColRange(x, pxcSlotWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      pw.writeGray16(x + col, getNibble(rowBuffer, col));
    }
  }
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight, const ImageBlock::PixelCachePolicy cachePolicy) {
  // A later pass of the same page render: the payload is already in RAM, skip
  // the file entirely.
  const uint64_t cacheHash = imagePathHash(cachePath);
  if (cachePolicy == ImageBlock::PixelCachePolicy::LoadIntoRam && pxcSlotHash == cacheHash && pxcSlotWidth != 0) {
    renderRowsFromPxcSlot(renderer, x, y);
    return true;
  }

  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (!readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight)) {
    LOG_ERR("IMG", "Invalid image cache: %s", cachePath.c_str());
    return false;
  }

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  const int bytesPerRow = static_cast<int>(cacheRowBytes(cachedWidth));  // 4bpp: 2 pixels per byte
  // cachedWidth 来自 .pxc 头（缓存损坏/被手改时可能是 0），此时 bytesPerRow == 0：
  // 下面 4096 / bytesPerRow 会先除零崩（SIGFPE），轮不到 rowsPerRead < 1 那条兜底。
  if (bytesPerRow < 1) {
    LOG_ERR("IMG", "Invalid cached row width %u in %s", (unsigned)cachedWidth, cachePath.c_str());
    return false;
  }

  // First pass of a page render: try to pull the payload into the RAM slot so
  // the remaining ~12 passes skip SD entirely. Only an EMPTY slot is claimed:
  // the slot lives until the page render completes, so a populated slot with a
  // different hash means another image on this same page owns it. Evicting it
  // here would make 2+ image pages reload each other from SD on every pass
  // (all the SD traffic of streaming plus the slot alloc churn); instead later
  // images take the streaming path below, unchanged from pre-cache behavior.
  if (cachePolicy == ImageBlock::PixelCachePolicy::LoadIntoRam && pxcSlotHash == 0 &&
      loadPxcSlot(cacheHash, cacheFile, cachedWidth, cachedHeight, bytesPerRow)) {
    renderRowsFromPxcSlot(renderer, x, y);
    LOG_DBG("IMG", "Cache render complete (payload now in RAM)");
    return true;
  }

  // Streaming fallback (slot didn't fit). A failed slot load may have consumed
  // part of the payload; rewind to just past the header.
  cacheFile.seek(PixelCache::kHeaderBytes);

  // Read several rows per SD access. A one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat; batching
  // rows into a ~4KB buffer cuts that to ~20 reads per pass without holding the
  // whole image.
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > cachedHeight) rowsPerRead = cachedHeight;
  auto readBuffer = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(rowsPerRead) * bytesPerRow);
  if (!readBuffer) {
    // Fall back to a single-row buffer under memory pressure.
    rowsPerRead = 1;
    readBuffer = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = 0; row < cachedHeight; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (cachedHeight - row < rowsPerRead) ? (cachedHeight - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer.get(), bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer.get() + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    const int destY = y + row;
    pw.beginRow(destY);
    // On a grayscale strip pass only a narrow column window of the image is in
    // the active band; skip the rest instead of unpacking+clipping every pixel.
    int colStart, colEnd;
    pw.bandColRange(x, cachedWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      pw.writeGray16(x + col, getNibble(rowBuffer, col));
    }
  }

  LOG_DBG("IMG", "Cache render complete");
  return true;
}

}  // namespace

bool ImageBlock::hasValidCache() const {
  const auto cachePath = getCachePath(imagePath);
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  return readValidCacheHeader(cacheFile, width, height, cachedWidth, cachedHeight);
}

bool ImageBlock::needsDecode() const { return !imageFailedThisSession(imagePath) && !hasValidCache(); }

bool ImageBlock::ensureExtracted() {
  if (Storage.exists(imagePath.c_str())) return true;
  if (srcPath.empty() || !extractFn) {
    rememberImageFailure(imagePath);
    return false;
  }

  LOG_DBG("IMG", "Lazy-extracting %s -> %s", srcPath.c_str(), imagePath.c_str());
  if (extractFn(extractCtx, srcPath.c_str(), imagePath.c_str())) return true;

  LOG_ERR("IMG", "Lazy extraction failed: %s", srcPath.c_str());
  rememberImageFailure(imagePath);
  return false;
}

void ImageBlock::clearSessionRenderFailures() { failedImageCount = 0; }

void ImageBlock::releaseRenderCache() { releasePxcSlot(); }

void ImageBlock::renderPlaceholder(GfxRenderer& renderer, const int x, const int y) const {
  renderer.fillRect(x, y, width, height, true);
  if (width > 2 && height > 2) {
    renderer.fillRect(x + 1, y + 1, width - 2, height - 2, false);
  }
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  (void)render(renderer, x, y, PixelCachePolicy::LoadIntoRam);
}

bool ImageBlock::render(GfxRenderer& renderer, const int x, const int y, const PixelCachePolicy cachePolicy) {
  return renderInternal(renderer, x, y, cachePolicy, DecodeOutput::FrameBufferAndCache);
}

bool ImageBlock::cacheDecodedImage(GfxRenderer& renderer, const int x, const int y) {
  return renderInternal(renderer, x, y, PixelCachePolicy::Stream, DecodeOutput::CacheOnly);
}

bool ImageBlock::renderInternal(GfxRenderer& renderer, const int x, const int y, const PixelCachePolicy cachePolicy,
                                const DecodeOutput output) {
  const bool renderToFramebuffer = output == DecodeOutput::FrameBufferAndCache;

  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (renderToFramebuffer && fcm && fcm->isScanning()) return true;

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (x < 0 || y < 0 || x + width > screenWidth || y + height > screenHeight) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return false;
  }

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (renderToFramebuffer && !renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return true;
  }

  if (imageFailedThisSession(imagePath)) {
    if (renderToFramebuffer) renderPlaceholder(renderer, x, y);
    return false;
  }

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath);
  if (renderToFramebuffer && renderFromCache(renderer, cachePath, x, y, width, height, cachePolicy)) {
    return true;
  }
  if (!renderToFramebuffer && hasValidCache()) {
    return true;
  }

  // The build only header-probed the image for dimensions; pull the actual
  // file out of the book now, on first visit to the page.
  if (!srcPath.empty() && !ensureExtracted()) {
    if (renderToFramebuffer) renderPlaceholder(renderer, x, y);
    return false;
  }

  // No cache - need to decode the image
  // Check if image file exists
  size_t fileSize = 0;
  {
    HalFile file;
    if (!Storage.openFileForRead("IMG", imagePath, file)) {
      LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
      rememberImageFailure(imagePath);
      if (renderToFramebuffer) renderPlaceholder(renderer, x, y);
      return false;
    }
    fileSize = file.size();
  }

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    if (renderToFramebuffer) renderPlaceholder(renderer, x, y);
    return false;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.ditherMode = ditherMode;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  config.cachePath = cachePath;      // Enable caching during decode
  config.output = output;
  config.bilinearScaling = bilinearScalingEnabled();

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    if (renderToFramebuffer) renderPlaceholder(renderer, x, y);
    return false;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  // Hard guarantee that the picture keeps its own aspect ratio. The layout side
  // (ChapterHtmlSlimParser) already sizes the box proportionally, but the box is
  // serialized inside the page and a section .bin written by an older build
  // still carries the stretched one — and useExactDimensions makes the decoder
  // stretch the source to whatever box it is handed, independently per axis, so
  // a bad box shows up as a squashed picture. Correct it here, once, and write
  // the corrected size back: the .pxc header check, the placeholder and later
  // cache hits all then agree and the image is not re-decoded every visit.
  // The INFO line is the ground truth for what actually reaches the panel.
  ImageDimensions srcDims = {0, 0};
  if (height > 0 && decoder->getDimensions(imagePath, srcDims) && srcDims.width > 0 && srcDims.height > 0) {
    const float srcAspect = static_cast<float>(srcDims.width) / static_cast<float>(srcDims.height);
    const float boxAspect = static_cast<float>(width) / static_cast<float>(height);
    if (boxAspect > srcAspect * 1.02f || boxAspect < srcAspect * 0.98f) {
      const int fixedW = boxAspect > srcAspect ? static_cast<int>(height * srcAspect + 0.5f) : width;
      const int fixedH = boxAspect > srcAspect ? height : static_cast<int>(width / srcAspect + 0.5f);
      if (fixedW > 0 && fixedH > 0 && fixedW <= INT16_MAX && fixedH <= INT16_MAX) {
        LOG_INF("IMG", "box %dx%d vs source %dx%d aspect mismatch, shrink to %dx%d", width, height, srcDims.width,
                srcDims.height, fixedW, fixedH);
        width = static_cast<int16_t>(fixedW);
        height = static_cast<int16_t>(fixedH);
        config.maxWidth = width;
        config.maxHeight = height;
      }
    }
    LOG_INF("IMG", "decode %s: source %dx%d -> %dx%d", imagePath.c_str(), srcDims.width, srcDims.height, width,
            height);
  }

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    if (renderToFramebuffer) renderPlaceholder(renderer, x, y);
    return false;
  }

  LOG_DBG("IMG", "Decode successful");
  return true;
}

bool ImageBlock::serialize(HalFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writeString(file, srcPath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile& file) {
  std::string path;
  std::string src;
  if (!serialization::readString(file, path, serialization::MAX_PATH_BYTES) ||
      !serialization::readString(file, src, serialization::MAX_PATH_BYTES)) {
    LOG_ERR("IMG", "Deserialization failed: truncated or oversized image path");
    return nullptr;
  }
  int16_t w = 0;
  int16_t h = 0;
  if (!serialization::readPod(file, w) || !serialization::readPod(file, h) || w <= 0 || h <= 0) {
    LOG_ERR("IMG", "Deserialization failed: invalid image dimensions");
    return nullptr;
  }
  auto block = makeUniqueNoThrow<ImageBlock>(path, src, w, h);
  if (!block) LOG_ERR("IMG", "OOM: ImageBlock (%u bytes)", static_cast<unsigned>(sizeof(ImageBlock)));
  return block;
}
