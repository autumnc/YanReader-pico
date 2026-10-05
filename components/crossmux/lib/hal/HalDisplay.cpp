#include "HalDisplay.h"

#include <string.h>

#include "crossmux_platform.h"
#include "epd_highlevel.h"
#include "epdiy.h"

HalDisplay display;

namespace {
EpdiyHighlevelState* s_hl = nullptr;
bool s_inverted = false;
crossmux_full_refresh_fn s_full_refresh = nullptr;
crossmux_full_refresh_fn s_gray8_refresh = nullptr;
crossmux_full_refresh_fn s_gray8_text_refresh = nullptr;
crossmux_mode_refresh_fn s_mode_refresh = nullptr;
crossmux_vk_present_fn s_vk_present = nullptr;

enum EpdDrawMode toEpdMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH: return MODE_GC16;
    case HalDisplay::HALF_REFRESH: return MODE_GL16;
    case HalDisplay::GRAY8_REFRESH: return MODE_GC16;       // 未注册钩子时的退路
    case HalDisplay::GRAY8_TEXT_REFRESH: return MODE_GL16;  // 同上
    case HalDisplay::FAST_REFRESH:
    default: return MODE_DU;
  }
}
}  // namespace

extern "C" void crossmux_platform_set_display(EpdiyHighlevelState* hl) { s_hl = hl; }
extern "C" void crossmux_platform_set_full_refresh(crossmux_full_refresh_fn fn) {
  s_full_refresh = fn;
}
extern "C" void crossmux_platform_set_gray8_refresh(crossmux_full_refresh_fn fn) {
  s_gray8_refresh = fn;
}
extern "C" void crossmux_platform_set_gray8_text_refresh(crossmux_full_refresh_fn fn) {
  s_gray8_text_refresh = fn;
}
extern "C" void crossmux_platform_set_mode_refresh(crossmux_mode_refresh_fn fn) {
  s_mode_refresh = fn;
}
extern "C" void crossmux_platform_set_vk_present(crossmux_vk_present_fn fn) {
  s_vk_present = fn;
}

HalDisplay::HalDisplay() {}
HalDisplay::~HalDisplay() {}

void HalDisplay::begin(bool seamless) { (void)seamless; }

void HalDisplay::clearScreen(uint8_t color) const {
  uint8_t* fb = getFrameBuffer();
  if (!fb) return;
  // 整屏填色直接按字节 memset：逐像素走 epd_fill_rect 要对 83 万个像素做一次
  // 旋转映射 + 半字节读改写，实测约 100ms（一次翻页里最贵的一步）。整屏都是同
  // 一种颜色，两个半字节同值（0/15 重复），与逐像素填充逐位等价；旋转 90° 时
  // 逻辑/物理缓冲的字节总数相同、且覆盖全部像素，所以按整块缓冲 memset 就行。
  memset(fb, color ? 0xFF : 0x00, getBufferSize());
}

// 1-bit (MSB-first) 位图 → 4bpp 像素（首版不用于封面，仅保接口）。
void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  (void)fromProgmem;
  uint8_t* fb = getFrameBuffer();
  if (!fb || !imageData) return;
  // 位序号按 size_t 算：yy*w+xx 在 int 下会溢出（w、h 都是 uint16，65535² > INT_MAX），
  // 溢出即 UB，可能算出负下标。另注：本函数没有位图长度参数，调用前请自备
  // ceil(w*h/8) 字节的缓冲（当前树内无调用者）。
  for (uint16_t yy = 0; yy < h; ++yy) {
    for (uint16_t xx = 0; xx < w; ++xx) {
      const size_t bitIdx = static_cast<size_t>(yy) * w + xx;
      const uint8_t byte = imageData[bitIdx >> 3];
      const uint8_t bit = byte & (0x80 >> (bitIdx & 7));
      epd_draw_pixel(x + xx, y + yy, bit ? 0x00 : 0xF0, fb);
    }
  }
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  // 透明位图（0 = 透明）与不透明版本在 4bpp 下近似等价，直接复用。
  drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen, DisplayRefreshContext context) {
  (void)turnOffScreen;
  (void)context;
  if (!s_hl) return;
  // ── 夜间反色不在这里 ──────────────────────────────────────────────────
  // 这里曾经是"所有推屏的唯一出口"，但那是错的：core1 的渲染任务（书架/菜单/设置/
  // 写作/GTD/待机）直呼 main 的 display.c，根本不经过本类 —— 反色挂在这里只会覆盖
  // 阅读器一族，用户侧就是"夜间模式只对阅读模式生效"。
  // 现在唯一实施点在 display.c 的推屏处（front/back 成对取反 + 推完翻回来），本类
  // 只保留 s_inverted 标志（isInverted() 的语义不变）。
  auto push = [&]() {
    // 统一出口优先：main 的 update_display_reader() 把五个档位全收口到 display.c，
    // 那里才有 GL16 全像素与"软刷攒够升 GC16"的计数。见 crossmux_platform.h 的说明。
    // Unified landing point first: main's update_display_reader() funnels all five
    // tiers into display.c, which is where the GL16-full-pixel rule and the
    // soft-refresh counter live. See crossmux_platform.h.
    if (s_mode_refresh) {
      s_mode_refresh(s_hl, static_cast<int>(mode));
      return;
    }
    // FULL_REFRESH 必须整屏全像素过一次 LUT。epd_hl_update_screen() 是差分刷：它只驱动
    // 与上一帧不同的像素，屏上内容没变时（长按确认键手动全局刷新、菜单里按返回等）差分
    // 图为空 → 直接返回，一个字都不刷。交给 main 的全刷出口去做。
    if (mode == FULL_REFRESH && s_full_refresh) {
      s_full_refresh(s_hl);
      return;
    }
    // 8 灰阶整屏：也要全像素过一次 LUT（同 FULL_REFRESH 的理由），走 main 的出口。
    if (mode == GRAY8_REFRESH && s_gray8_refresh) {
      s_gray8_refresh(s_hl);
      return;
    }
    if (mode == GRAY8_TEXT_REFRESH && s_gray8_text_refresh) {
      s_gray8_text_refresh(s_hl);
      return;
    }
    epd_hl_update_screen(s_hl, toEpdMode(mode), 25);
  };
  push();
}

// 阅读模式虚拟键盘的打字帧。夜间反色同样不在本类做（钩子最终落到 display.c 的推屏处）。
void HalDisplay::displayBufferVk(int panel_top, int cand_h) {
  if (!s_hl) return;
  if (!s_vk_present) {
    // 没注册钩子（理论上只有单测/裁剪构建会这样）：退回老行为 —— 整屏局刷。
    displayBuffer(HALF_REFRESH);
    return;
  }
  s_vk_present(panel_top, cand_h);
}
void HalDisplay::displayBufferAsync(RefreshMode mode, DisplayRefreshContext context) {
  displayBuffer(mode, false, context);
}
void HalDisplay::waitRefreshComplete() {}
bool HalDisplay::supportsAsyncRefresh() const { return false; }
void HalDisplay::refreshDisplay(RefreshMode mode, bool turnOffScreen) { displayBuffer(mode, turnOffScreen); }

void HalDisplay::setInverted(bool inverted) { s_inverted = inverted; }
bool HalDisplay::toggleInverted() {
  s_inverted = !s_inverted;
  return s_inverted;
}
bool HalDisplay::isInverted() const { return s_inverted; }

void HalDisplay::deepSleep() {}

uint8_t* HalDisplay::getFrameBuffer() const {
  return s_hl ? epd_hl_get_framebuffer(s_hl) : nullptr;
}

uint8_t* HalDisplay::lendFrameBufferStorage(uint32_t* sizeOut) {
  if (sizeOut) *sizeOut = getBufferSize();
  return getFrameBuffer();
}
void HalDisplay::returnFrameBufferStorage() {}

void HalDisplay::preconditionGrayscale() {}
void HalDisplay::preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  (void)x; (void)y; (void)w; (void)h;
}
void HalDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOffScreen, DisplayRefreshContext context) {
  displayBuffer(fallback, turnOffScreen, context);
}
void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  (void)lsbBuffer; (void)msbBuffer;
}
void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { (void)lsbBuffer; }
void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { (void)msbBuffer; }
void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { (void)bwBuffer; }
void HalDisplay::displayGrayBuffer(bool turnOffScreen) { displayBuffer(HALF_REFRESH, turnOffScreen); }
void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  (void)lsbPlane; (void)rows; (void)yStart; (void)numRows;
}
bool HalDisplay::supportsStripGrayscale() const { return false; }
bool HalDisplay::combinesGrayscaleBase() const { return false; }
bool HalDisplay::supportsTextOnlyCombinedBase() const { return false; }
bool HalDisplay::supportsReaderTransitions() const { return false; }
bool HalDisplay::supportsContinuousImageReading() const { return false; }
bool HalDisplay::canUseTextTransition() const { return false; }
void HalDisplay::cancelGrayscale() {}

uint16_t HalDisplay::getDisplayWidth() const { return static_cast<uint16_t>(epd_rotated_display_width()); }
uint16_t HalDisplay::getDisplayHeight() const { return static_cast<uint16_t>(epd_rotated_display_height()); }
uint8_t HalDisplay::getGrayscaleLevels() const { return 16; }
uint8_t* HalDisplay::beginGrayscale16() { return nullptr; }
bool HalDisplay::commitGrayscale16() { return false; }
void HalDisplay::cancelGrayscale16() {}
uint16_t HalDisplay::getDisplayWidthBytes() const { return getDisplayWidth() / 2; }
uint32_t HalDisplay::getBufferSize() const {
  return static_cast<uint32_t>(getDisplayWidthBytes()) * getDisplayHeight();
}
