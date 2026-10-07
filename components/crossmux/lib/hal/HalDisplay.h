#pragma once
#include <cstdint>

#include <DisplayRefreshContext.h>

// 阅读模式显示 HAL：把 crossmux 的 1-bit + 灰度平面抽象映射到 read_pico 的
// epdiy 4bpp 单缓冲（每字节两像素，高半字节=左像素，0=黑/15=白）。
// 公共接口与 reference crossmux 的 HalDisplay 完全一致，逻辑层无需改动。
// main 在 board_init 后调用 crossmux_platform_set_display() 注入 epdiy 句柄。
class HalDisplay {
 public:
  HalDisplay();
  ~HalDisplay();

  enum RefreshMode {
    FULL_REFRESH,  // GC16
    HALF_REFRESH,  // GL16
    FAST_REFRESH,  // DU
    GRAY8_REFRESH,     // 30 相 8 灰阶 GC16 全刷：比 FULL 快，残影一样清净（清账档）
    GRAY8_TEXT_REFRESH, // 30 相 8 灰阶 GL16 差分：不闪、比 HALF 每屏快约 80ms（正文档）
    STATUS_REFRESH     // 同 GRAY8_TEXT 的刷法，但**不计入残影预算**：过渡屏（缓存进度 /
                       // 词典下载 / "正在连接…"）按秒重画，记账会每 14 次升一次整屏
                       // GC16 —— 那就是"缓存时隔几秒闪一下"。见 display.c
  };

  void begin(bool seamless = false);

  // Read Pico 面板：1216 列 × 684 门线；4bpp 每行 608 字节。
  static constexpr uint16_t DISPLAY_WIDTH = 1216;
  static constexpr uint16_t DISPLAY_HEIGHT = 684;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 2;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  // Frame buffer operations
  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 bool fromProgmem = false) const;
  void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            bool fromProgmem = false) const;

  void displayBuffer(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false,
                     DisplayRefreshContext context = DisplayRefreshContext::Normal);
  // 阅读模式虚拟键盘的"打字帧"：只驱动与上一帧有差异的那块矩形（判据与波形见
  // crossmux_platform.h 的 vk_present，实现在 ui_render.cpp 的 reader_vk_present）。
  // panel_top = 键盘面板顶边，cand_h = 编码/候选两行总高（逻辑像素）。夜间反色与本类
  // 其它推屏出口一致，由这里罩住；没注册钩子时退回 displayBuffer(HALF_REFRESH)。
  void displayBufferVk(int panel_top, int cand_h);
  void displayBufferAsync(RefreshMode mode = RefreshMode::FAST_REFRESH,
                          DisplayRefreshContext context = DisplayRefreshContext::Normal);
  void waitRefreshComplete();
  bool supportsAsyncRefresh() const;
  void refreshDisplay(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);

  void setInverted(bool inverted);
  bool toggleInverted();
  bool isInverted() const;

  void deepSleep();

  uint8_t* getFrameBuffer() const;

  uint8_t* lendFrameBufferStorage(uint32_t* sizeOut);
  void returnFrameBufferStorage();

  // 灰度预调/灰度平面接口：4bpp 原生灰度下为 no-op。
  void preconditionGrayscale();
  void preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
  void displayGrayscaleBase(RefreshMode fallback = HALF_REFRESH, bool turnOffScreen = false,
                            DisplayRefreshContext context = DisplayRefreshContext::Normal);
  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);
  void displayGrayBuffer(bool turnOffScreen = false);
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows);
  bool supportsStripGrayscale() const;
  bool combinesGrayscaleBase() const;
  bool supportsTextOnlyCombinedBase() const;
  bool supportsReaderTransitions() const;
  bool supportsContinuousImageReading() const;
  bool canUseTextTransition() const;
  void cancelGrayscale();

  // Runtime geometry passthrough
  uint16_t getDisplayWidth() const;
  uint16_t getDisplayHeight() const;
  uint8_t getGrayscaleLevels() const;
  uint8_t* beginGrayscale16();
  bool commitGrayscale16();
  void cancelGrayscale16();
  uint16_t getDisplayWidthBytes() const;
  uint32_t getBufferSize() const;
};

extern HalDisplay display;
