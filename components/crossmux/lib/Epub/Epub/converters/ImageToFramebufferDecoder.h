#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>

#include "DitherUtils.h"

class GfxRenderer;

struct ImageDimensions {
  int16_t width;
  int16_t height;
};

enum class DecodeOutput : uint8_t {
  FrameBufferAndCache,
  CacheOnly,
};

struct RenderConfig {
  int x, y;
  int maxWidth, maxHeight;
  bool useGrayscale = true;
  // 8 位灰 → 16 级灰的抖动档。阅读器把用户设置推给 ImageBlock::setDitherMode()
  // 后落到这里；独立图片查看器直接填。
  DitherMode ditherMode = DitherMode::Ordered;
  bool performanceMode = false;
  bool useExactDimensions = false;  // If true, use maxWidth/maxHeight as exact output size (no recalculation)
  float sourceCropX = 0.0f;         // Fraction cropped equally from the left and right edges
  float sourceCropY = 0.0f;         // Fraction cropped equally from the top and bottom edges
  // 源窗口：取源图上的任意矩形（左上角比例 + 宽高比例），用来做"放大 + 平移"——
  // 上面的 sourceCrop* 只能**居中对称**裁剪，放大后想挪到画面别处就没辙。
  // sourceWindowW/H 为 0（默认）时退回 sourceCrop* 的对称裁剪，两个解码器都照旧。
  float sourceWindowX = 0.0f;
  float sourceWindowY = 0.0f;
  float sourceWindowW = 0.0f;
  float sourceWindowH = 0.0f;
  bool preserveAlpha = false;       // Skip transparent pixels instead of compositing them against white
  // Resampling filter for the scale step: false = nearest neighbour (historical
  // behaviour, one source pixel per output pixel), true = bilinear blend of the
  // source neighbourhood. Callers that must keep the cheap path leave it false.
  bool bilinearScaling = false;
  std::string cachePath;  // If non-empty, decoder will write pixel cache to this path
  DecodeOutput output = DecodeOutput::FrameBufferAndCache;
  // Optional "give up" poll, called from the decoder's per-row/per-MCU callback at
  // the same cadence as yieldDuringDecode. Returning true stops the decode and
  // decodeToFramebuffer() returns false. The image viewer uses it so a slow decode
  // can be dropped when the user turns the page mid-decode (otherwise the queued
  // key would only be seen after a multi-second decode finishes). Leave null when
  // a partial image would be a problem - callers that poll know a false return may
  // mean "aborted", not "broken file".
  bool (*abortPoll)(void* ctx) = nullptr;
  void* abortPollCtx = nullptr;
};

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual const char* getFormatName() const = 0;

  // Call from per-row/per-MCU decode callbacks (free functions, hence public):
  // yields one tick at most every 250 ms so multi-second decodes keep the idle
  // task (and its watchdog) fed. `lastYieldMs` is caller-held state,
  // initialized to the decode start time.
  static void yieldDuringDecode(uint32_t& lastYieldMs);

  // Validate decoder/header dimensions before narrowing them into the layout
  // representation. Shared by header probing and decoder fallbacks.
  static bool validateAndStoreDimensions(int64_t width, int64_t height, ImageDimensions& out, const char* format);

 protected:
  // Size validation helpers. The cap bounds decode TIME, not memory: both decoders
  // stream (JPEG in MCU bands at 1/2..1/8 coarse scale, PNG scanline-by-scanline
  // with its own width-based row-buffer guard), so RAM never scales with source
  // area. 8 MP admits real-world ebook covers (KDP recommends 1600x2560 and
  // 2000x3000) while keeping a worst-case single decode in single-digit seconds;
  // the row callbacks yield periodically so a long decode cannot starve the idle
  // task's watchdog.
  static constexpr int64_t MAX_SOURCE_DIMENSION = INT16_MAX;
  static constexpr int64_t MAX_SOURCE_PIXELS = 8388608;  // 8 MP (e.g. 2048 * 4096)

  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
