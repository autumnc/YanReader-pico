#include "GfxRenderer.h"

#include <FontCacheManager.h>
#include <Logging.h>

#include <cstring>
#include <string>

// 不 #include read_pico 的 epdiy.h：它会连带引入 epd_internals.h，其中 typedef 的
// EpdGlyph / EpdUnicodeInterval / EpdFont 与本移植的 EpdFontData.h / EpdFont.h 冲突
// （crossmux 的 EpdFont 是 class，epdiy 的同名是 struct）。这里按 ABI 手动声明本组件
// 用到的绘制原语与 EpdRect —— epdiy 为 C 函数，无 name mangling，结构布局一致即可链接。
extern "C" {
typedef struct {
  int x;
  int y;
  int width;
  int height;
} EpdRect;
// MAIN 组件 ttf_font.h 的 TtfDrawAlign 镜像（同名同值）。不能 include 那个头：
// main 依赖 crossmux，反向 includes 会成环。P1.3 之后 ttf_font.h 不再带 epdiy，
// 对齐标志也成了它自己的 TTF_ALIGN_*，这里镜像同一份；数值 = epdiy 的 EPD_DRAW_ALIGN_*。
enum TtfDrawAlign {
  TTF_ALIGN_LEFT = 0x2,
  TTF_ALIGN_RIGHT = 0x4,
  TTF_ALIGN_CENTER = 0x8,
};
void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* framebuffer);
void epd_draw_line(int x0, int y0, int x1, int y1, uint8_t color, uint8_t* framebuffer);
void epd_draw_circle(int x, int y, int r, uint8_t color, uint8_t* framebuffer);
void epd_draw_rect(EpdRect rect, uint8_t color, uint8_t* framebuffer);
void epd_fill_rect(EpdRect rect, uint8_t color, uint8_t* framebuffer);
void epd_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t color, uint8_t* framebuffer);
}

// ttf_font 位于 MAIN 组件（官方固件字体层），跨组件在最终链接时解析。
extern "C" {
// align 是 int：MAIN 组件的 ttf_font.h 刻意不带 epdiy（改成 int 就是为此，见那头的
// P1.3 说明），本组件又不能 include MAIN 的头（main 依赖 crossmux，反向会成环），
// 所以这里手工镜像同一份 ABI。传值仍是上面那几个 EPD_DRAW_ALIGN_* 标志位。
void ttf_draw_text_px(uint8_t* framebuffer, int x, int y, int pixel_height, const char* text,
                      int align, uint8_t fg, uint8_t bg);
int ttf_text_width_px(int pixel_height, const char* text);
int ttf_ascender_px(int pixel_height);
// 选定后续 ttf_* 绘制/度量作用于哪个字面（同 MAIN 组件 ttf_font.h）。UI_FONT_ID=0
// 是阅读器的标题/状态栏，恒为内置字体；正文 fontId 1..5 跟随字体设置。
void ttf_set_role(int role);
}

// 与 MAIN 组件 ttf_font.h 同名同值（见文件头关于"不 include epdiy.h"的说明）。
#define TTF_ROLE_CONTENT 0
#define TTF_ROLE_UI      1
#define TTF_ROLE_CONTENT_ALT 2
// fontId → 字面角色。阅读器约定：0 = UI(chrome)，1..5 = 正文。
// 正文词的 ALT_FONT 位 = "这一段用书内 CSS 的第二个家族"（祖堂集的仿宋注文），
// 切到次字面；次字面没打开时 ttf_set_role 自己退回内容面，所以这里不用判空。
inline int ttfRoleForFontId(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (fontId == 0) return TTF_ROLE_UI;
  if ((style & EpdFontFamily::ALT_FONT) != 0) return TTF_ROLE_CONTENT_ALT;
  return TTF_ROLE_CONTENT;
}

namespace {
constexpr uint8_t EPD_INK = 0x00;    // black
constexpr uint8_t EPD_WHITE = 0xF0;  // white
constexpr int DEFAULT_PIXEL_HEIGHT = 22;

// fontId → ttf 像素高：从 fontMap 里的 EpdFontData::advanceY 取（Phase 2 的 reader
// 在 insertFont 时构造带正确 metrics 的 EpdFontData），否则回落到默认字号。
int ttfPixelHeightFor(const std::map<int, EpdFontFamily>& fontMap, int fontId) {
  auto it = fontMap.find(fontId);
  if (it == fontMap.end()) return DEFAULT_PIXEL_HEIGHT;
  const EpdFontData* d = it->second.getData(EpdFontFamily::REGULAR);
  return (d && d->advanceY > 0) ? d->advanceY : DEFAULT_PIXEL_HEIGHT;
}

uint8_t toInk(bool state) { return state ? EPD_INK : EPD_WHITE; }

int utf8CharLen(unsigned char c) {
  if (c < 0x80) return 1;
  if (c < 0xE0) return 2;
  if (c < 0xF0) return 3;
  return 4;
}
}  // namespace

// ---- Setup ----

void GfxRenderer::begin() {
  panelWidth = display.getDisplayWidth();
  panelHeight = display.getDisplayHeight();
  panelWidthBytes = display.getDisplayWidthBytes();
  frameBufferSize = display.getBufferSize();
  frameBuffer = display.getFrameBuffer();
}

void GfxRenderer::insertFont(int fontId, EpdFontFamily font) { fontMap.insert_or_assign(fontId, font); }

void GfxRenderer::prewarmFallbackText(int fontId, TextGetter getter, const void* ctx, uint32_t textCount,
                                      EpdFontFamily::Style style) const {
  (void)fontId; (void)getter; (void)ctx; (void)textCount; (void)style;
}
void GfxRenderer::prewarmFallbackText(int fontId, const char* text, EpdFontFamily::Style style) const {
  (void)fontId; (void)text; (void)style;
}
bool GfxRenderer::isFontCacheScanning() const {
  return fontCacheManager_ ? fontCacheManager_->isScanning() : false;
}

// ---- Orientation / geometry ----

int GfxRenderer::getScreenWidth() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted: return panelHeight;
    case LandscapeClockwise:
    case LandscapeCounterClockwise: return panelWidth;
  }
  return panelWidth;
}
int GfxRenderer::getScreenHeight() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted: return panelWidth;
    case LandscapeClockwise:
    case LandscapeCounterClockwise: return panelHeight;
  }
  return panelHeight;
}

void GfxRenderer::tapToLogical(float nx, float ny, int& outX, int& outY) const {
  int phyX = static_cast<int>(nx * panelWidth);
  int phyY = static_cast<int>(ny * panelHeight);
  if (phyX < 0) phyX = 0;
  if (phyX > panelWidth - 1) phyX = panelWidth - 1;
  if (phyY < 0) phyY = 0;
  if (phyY > panelHeight - 1) phyY = panelHeight - 1;
  switch (orientation) {
    case Portrait:
      outX = panelHeight - 1 - phyY;
      outY = phyX;
      break;
    case PortraitInverted:
      outX = phyY;
      outY = panelWidth - 1 - phyX;
      break;
    case LandscapeClockwise:
      outX = panelWidth - 1 - phyX;
      outY = panelHeight - 1 - phyY;
      break;
    case LandscapeCounterClockwise:
    default:
      outX = phyX;
      outY = phyY;
      break;
  }
}

// ---- Refresh ----

void GfxRenderer::displayBuffer(HalDisplay::RefreshMode refreshMode, DisplayRefreshContext context) const {
  if (nextRefreshOverridePending) {
    refreshMode = nextRefreshOverride;
    nextRefreshOverridePending = false;
  }
  display.displayBuffer(refreshMode, false, context);
}
void GfxRenderer::displayBufferAsync(HalDisplay::RefreshMode refreshMode, DisplayRefreshContext context) const {
  if (nextRefreshOverridePending) {
    refreshMode = nextRefreshOverride;
    nextRefreshOverridePending = false;
  }
  display.displayBufferAsync(refreshMode, context);
}
void GfxRenderer::waitRefreshComplete() const { display.waitRefreshComplete(); }
bool GfxRenderer::supportsAsyncRefresh() const { return display.supportsAsyncRefresh(); }
bool GfxRenderer::isInverted() const { return display.isInverted(); }

void GfxRenderer::invertScreen() const {
  uint8_t* fb = frameBuffer;
  if (!fb) return;
  for (uint32_t i = 0; i < frameBufferSize; ++i) fb[i] = static_cast<uint8_t>(~fb[i]);
  display.displayBuffer(HalDisplay::FULL_REFRESH);
}
void GfxRenderer::clearScreen(uint8_t color) const {
  // 绘制目标被 setFrameBuffer 换走时（阅读器的空闲帧预渲染，见 screen_reader.cpp 的
  // rdPrerenderNextPage），要清的必须是**这一块**：display.clearScreen 清的永远是 display
  // 自己那块上屏缓冲（它不看 frameBuffer 指向谁），照旧调就会把面板正显示的那一帧擦白，
  // 而这一趟真正要画的备用缓冲一个字节都没清 —— 画出来是一页叠着上一趟的垃圾。
  // 指针没被换走（相等）时走原路：display 那条是整块缓冲 memset，与这里逐字节等价。
  if (frameBuffer != nullptr && frameBuffer != display.getFrameBuffer()) {
    memset(frameBuffer, color ? 0xFF : 0x00, frameBufferSize);
    return;
  }
  display.clearScreen(color);
}

void GfxRenderer::getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const {
  *outTop = VIEWABLE_MARGIN_TOP;
  *outRight = VIEWABLE_MARGIN_RIGHT;
  *outBottom = VIEWABLE_MARGIN_BOTTOM;
  *outLeft = VIEWABLE_MARGIN_LEFT;
}

// ---- Strip / clip ----

void GfxRenderer::beginStripTarget(uint8_t* scratch, int stripY0, int stripRows) const {
  _stripBuf = scratch;
  _stripY0 = stripY0;
  _stripRows = stripRows;
  _stripActive = true;
}
void GfxRenderer::endStripTarget() const {
  _stripBuf = nullptr;
  _stripActive = false;
}
bool GfxRenderer::glyphIntersectsStrip(int x0, int y0, int x1, int y1) const {
  (void)x0; (void)y0; (void)x1; (void)y1;
  return !_stripActive;  // 4bpp 原生灰度下无 strip，恒真
}

// ---- Pixel primitives ----

void GfxRenderer::drawPixel(int x, int y, bool state) const {
  if (!frameBuffer) return;
  if (clipActive && (x < clipX0 || x >= clipX1 || y < clipY0 || y >= clipY1)) return;
  epd_draw_pixel(x, y, toInk(state), frameBuffer);
}
void GfxRenderer::drawPixelInk(int x, int y, uint8_t ink) const {
  if (!frameBuffer) return;
  if (clipActive && (x < clipX0 || x >= clipX1 || y < clipY0 || y >= clipY1)) return;
  epd_draw_pixel(x, y, ink, frameBuffer);
}
void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, bool state) const {
  if (!frameBuffer) return;
  epd_draw_line(x1, y1, x2, y2, toInk(state), frameBuffer);
}
void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const {
  if (lineWidth <= 1) {
    drawLine(x1, y1, x2, y2, state);
    return;
  }
  for (int i = 0; i < lineWidth; ++i) drawLine(x1, y1 + i, x2, y2 + i, state);
}
void GfxRenderer::drawWavyLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const {
  if (!frameBuffer) return;
  // Normalize so the phase starts at the left end regardless of call order.
  if (x2 < x1) {
    const int tx = x1;
    x1 = x2;
    x2 = tx;
    const int ty = y1;
    y1 = y2;
    y2 = ty;
  }
  // 12 px 一周期、幅度 ±2 px 的折线波。取折线而非正弦：整数相位表零计算量（每条线就是
  // 几百次查表 + epd_draw_pixel），而且在 1bpp 抖动下不会出现正弦采样丢失波峰的问题。
  // 幅度/周期取头文件里的常量 —— TextBlock 用 kWavyAmplitudePx 给中线让位。
  static_assert(kWavyPeriodPx == 12, "kWave 表的长度必须等于 kWavyPeriodPx");
  static constexpr int8_t kWave[kWavyPeriodPx] = {0, -1, -2, -2, -1, 0, 0, 1, 2, 2, 1, 0};
  const uint8_t ink = toInk(state);
  const int span = (x2 > x1) ? (x2 - x1) : 1;
  const int dy = y2 - y1;
  for (int x = x1; x <= x2; ++x) {
    if (clipActive && (x < clipX0 || x >= clipX1)) continue;
    const int off = x - x1;
    const int yBase = y1 + dy * off / span + kWave[off % kWavyPeriodPx];
    for (int i = 0; i < lineWidth; ++i) {
      const int py = yBase + i;
      if (clipActive && (py < clipY0 || py >= clipY1)) continue;
      epd_draw_pixel(x, py, ink, frameBuffer);
    }
  }
}
void GfxRenderer::drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const {
  (void)xDir; (void)yDir; (void)lineWidth;
  if (!frameBuffer) return;
  epd_draw_circle(cx, cy, maxRadius, toInk(state), frameBuffer);
}
void GfxRenderer::drawRect(int x, int y, int width, int height, bool state) const {
  if (!frameBuffer) return;
  epd_draw_rect(EpdRect{x, y, width, height}, toInk(state), frameBuffer);
}
void GfxRenderer::drawRect(int x, int y, int width, int height, int lineWidth, bool state) const {
  if (lineWidth <= 1) {
    drawRect(x, y, width, height, state);
    return;
  }
  for (int i = 0; i < lineWidth; ++i) drawRect(x + i, y + i, width - 2 * i, height - 2 * i, state);
}
void GfxRenderer::drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius,
                                  bool state) const {
  (void)lineWidth; (void)cornerRadius;
  drawRect(x, y, width, height, state);
}
void GfxRenderer::drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius,
                                  bool roundTopLeft, bool roundTopRight, bool roundBottomLeft, bool roundBottomRight,
                                  bool state) const {
  (void)lineWidth; (void)cornerRadius; (void)roundTopLeft; (void)roundTopRight; (void)roundBottomLeft;
  (void)roundBottomRight;
  drawRect(x, y, width, height, state);
}
void GfxRenderer::maskRoundedRectOutsideCorners(int x, int y, int width, int height, int radius, Color color) const {
  (void)x; (void)y; (void)width; (void)height; (void)radius; (void)color;
}
void GfxRenderer::fillRect(int x, int y, int width, int height, bool state) const {
  if (!frameBuffer) return;
  epd_fill_rect(EpdRect{x, y, width, height}, toInk(state), frameBuffer);
}
void GfxRenderer::fillRectDither(int x, int y, int width, int height, Color color) const {
  bool state = (color == Color::Black);
  fillRect(x, y, width, height, state);
}
void GfxRenderer::fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const {
  (void)cornerRadius;
  fillRectDither(x, y, width, height, color);
}
void GfxRenderer::fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft,
                                  bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, Color color) const {
  (void)cornerRadius; (void)roundTopLeft; (void)roundTopRight; (void)roundBottomLeft; (void)roundBottomRight;
  fillRectDither(x, y, width, height, color);
}
void GfxRenderer::fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state) const {
  if (!frameBuffer || numPoints < 3) return;
  for (int i = 1; i < numPoints - 1; ++i) {
    epd_fill_triangle(xPoints[0], yPoints[0], xPoints[i], yPoints[i], xPoints[i + 1], yPoints[i + 1],
                      toInk(state), frameBuffer);
  }
}

// ---- Bitmap / image (Phase 1 封面解码为 stub，纯 1-bit 位图仍可画) ----

void GfxRenderer::drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const {
  if (!frameBuffer || !bitmap) return;
  for (int yy = 0; yy < height; ++yy) {
    for (int xx = 0; xx < width; ++xx) {
      int idx = yy * width + xx;
      bool on = bitmap[idx / 8] & (0x80 >> (idx % 8));
      epd_draw_pixel(x + xx, y + yy, on ? EPD_INK : EPD_WHITE, frameBuffer);
    }
  }
}
void GfxRenderer::drawIcon(const uint8_t bitmap[], int x, int y, int size) const {
  drawImage(bitmap, x, y, size, size);
}
void GfxRenderer::drawIconInverted(const uint8_t bitmap[], int x, int y, int size) const {
  if (!frameBuffer || !bitmap) return;
  for (int yy = 0; yy < size; ++yy) {
    for (int xx = 0; xx < size; ++xx) {
      int idx = yy * size + xx;
      bool on = bitmap[idx / 8] & (0x80 >> (idx % 8));
      epd_draw_pixel(x + xx, y + yy, on ? EPD_WHITE : EPD_INK, frameBuffer);
    }
  }
}
void GfxRenderer::drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX, float cropY,
                             bool preserveTransparency) const {
  (void)bitmap; (void)x; (void)y; (void)maxWidth; (void)maxHeight; (void)cropX; (void)cropY;
  (void)preserveTransparency;
}
bool GfxRenderer::drawBitmapCropToFill(const Bitmap& bitmap, int x, int y, int width, int height) const {
  (void)bitmap; (void)x; (void)y; (void)width; (void)height;
  return false;
}
void GfxRenderer::drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight) const {
  (void)bitmap; (void)x; (void)y; (void)maxWidth; (void)maxHeight;
}
void GfxRenderer::preserveImagePolarity(int x, int y, int width, int height) const {
  (void)x; (void)y; (void)width; (void)height;
}

// ---- Framebuffer region snapshot (Phase 1 stub) ----

size_t GfxRenderer::readFramebufferRegion(int x, int y, int w, int h, uint8_t* dst, size_t dstCapacity) const {
  (void)x; (void)y; (void)w; (void)h; (void)dst; (void)dstCapacity;
  return 0;
}
void GfxRenderer::writeFramebufferRegion(int x, int y, int w, int h, const uint8_t* src) {
  (void)x; (void)y; (void)w; (void)h; (void)src;
}

// ---- Text ----

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style,
                              const BidiUtils::BidiBaseDir baseDir) const {
  (void)baseDir;
  if (!text) return 0;
  ttf_set_role(ttfRoleForFontId(fontId, style));
  // SUP/SUB 画在 50% 字号上（drawText 里同一处减半），量宽必须跟着减半，
  // 否则排版（xposArr）与实画几何对不上：字按半宽画、却按全宽占位，
  // 上标后面会漏出半个字的空档、或和后一个词叠在一起。
  int px = ttfPixelHeightFor(getFontMap(), fontId);
  if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) px /= 2;
  return ttf_text_width_px(px, text);
}
void GfxRenderer::drawCenteredText(const int fontId, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style, const BidiUtils::BidiBaseDir baseDir) const {
  int w = getTextWidth(fontId, text, style, baseDir);
  int x = (getScreenWidth() - w) / 2;
  drawText(fontId, x, y, text, black, style, baseDir);
}
void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style, const BidiUtils::BidiBaseDir baseDir) const {
  (void)baseDir;
  if (!frameBuffer || !text || !*text) return;
  ttf_set_role(ttfRoleForFontId(fontId, style));
  int px = ttfPixelHeightFor(getFontMap(), fontId);
  // SUP/SUB：整串按半字号画（基线仍由 TextBlock 传入的 y 决定，它已按全字号
  // ascender 抬/压过）。半字号顺带把步进也减半 —— ttf_draw_text_px 用该字号
  // 自身的 advance —— 与 getTextWidth 的减半一致。
  if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) px /= 2;
  uint8_t fg = black ? 0x00 : 0x0F;
  uint8_t bg = black ? 0x0F : 0x00;
  bool bold = (style & EpdFontFamily::BOLD) || syntheticBoldPixels > 0;
  ttf_draw_text_px(frameBuffer, x, y, px, text, TTF_ALIGN_LEFT, fg, bg);
  if (bold) {
    ttf_draw_text_px(frameBuffer, x + 1, y, px, text, TTF_ALIGN_LEFT, fg, bg);
    ttf_draw_text_px(frameBuffer, x, y + 1, px, text, TTF_ALIGN_LEFT, fg, bg);
  }
}
int GfxRenderer::getSpaceWidth(const int fontId, const EpdFontFamily::Style style) const {
  // 必须与 getTextWidth/drawText 选同一面：它被 wrappedText 用来量空格却不出现在
  // drawText 里，漏了会让正文断行稳定地错（不崩、不报错）。SUP/SUB 同样减半，
  // 否则上标串里的空格按全宽算、与半宽的字对不齐。
  ttf_set_role(ttfRoleForFontId(fontId, style));
  int px = ttfPixelHeightFor(getFontMap(), fontId);
  if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) px /= 2;
  return ttf_text_width_px(px, " ");
}
int GfxRenderer::getSpaceAdvance(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                                 const EpdFontFamily::Style style) const {
  (void)leftCp; (void)rightCp;
  return getSpaceWidth(fontId, style);
}
int GfxRenderer::getKerning(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                            const EpdFontFamily::Style style) const {
  (void)fontId; (void)leftCp; (void)rightCp; (void)style;
  return 0;  // ttf_font 无字距对表
}
int GfxRenderer::getTextAdvanceX(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  return getTextWidth(fontId, text, style);
}
int GfxRenderer::getFontAscenderSize(const int fontId) const {
  ttf_set_role(ttfRoleForFontId(fontId));
  auto it = getFontMap().find(fontId);
  if (it != getFontMap().end()) {
    const EpdFontData* d = it->second.getData(EpdFontFamily::REGULAR);
    if (d && d->ascender > 0) return d->ascender;
  }
  return ttf_ascender_px(ttfPixelHeightFor(getFontMap(), fontId));
}
int GfxRenderer::getLineHeight(const int fontId) const { return ttfPixelHeightFor(getFontMap(), fontId); }
int GfxRenderer::getLineHeight(const int fontId, const float compression) const {
  return static_cast<int>(getLineHeight(fontId) * compression + 0.5f);
}
int GfxRenderer::getTextHeight(const int fontId) const { return getFontAscenderSize(fontId); }

int GfxRenderer::cssFontId(const int baseFontId, const float scale) const {
  if (cssFontLadder_[0] == 0) return baseFontId;   // 梯子没灌 = 未启用（强制指定模式）
  if (!(scale > 0.0f)) return baseFontId;          // 含 NaN：写反了比较就得原样返回
  int step;
  if (scale <= 0.80f) step = 0;
  else if (scale <= 0.95f) step = 1;
  else if (scale <= 1.10f) step = 2;               // 正文（±10% 内不折腾）
  else if (scale <= 1.40f) step = 3;
  else step = 4;
  const int id = cssFontLadder_[step];
  // 梯子被夹到边界时会有重复项，正常；但目标字体必须真注册过，否则宁可回退到
  // 正文字体——字号不对好过画不出来。
  if (id <= 0 || getFontMap().find(resolveFontFamilyId(id)) == getFontMap().end()) return baseFontId;
  return id;
}

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style) const {
  if (!text) return "";
  std::string s(text);
  if (getTextWidth(fontId, s.c_str(), style) <= maxWidth) return s;
  // 逐字累加宽度，而不是每加一个字就把整串重拼一遍再整串量宽。ttf 面量宽就是逐码点
  // advance 相加（measure_width），所以"整串宽 == 前缀宽之和"恒成立，切点逐字符不变。
  // 原来每个字符都要构造 cand(=out+ch) 并重扫整段，这里是 O(串长²)。
  std::string out;
  int outW = 0;
  for (size_t i = 0; i < s.size();) {
    const int len = utf8CharLen(static_cast<unsigned char>(s[i]));
    if (i + static_cast<size_t>(len) > s.size()) break;
    char cb[8];
    memcpy(cb, s.data() + i, static_cast<size_t>(len));
    cb[len] = '\0';
    const int cw = getTextWidth(fontId, cb, style);
    if (outW + cw > maxWidth) break;
    out.append(cb, static_cast<size_t>(len));
    outW += cw;
    i += static_cast<size_t>(len);
  }
  return out;
}
std::vector<std::string> GfxRenderer::wrappedText(const int fontId, const char* text, const int maxWidth,
                                                  const int maxLines, const EpdFontFamily::Style style) const {
  std::vector<std::string> lines;
  if (!text || maxLines <= 0) return lines;
  std::string s(text);
  std::string line;
  int lineW = 0;   // line 的当前宽度，随追加/新行增量维护（量宽逐码点可加）
  size_t i = 0;
  while (i < s.size() && static_cast<int>(lines.size()) < maxLines) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    // 硬换行。词典释义、笔记正文、脚注原文里的 '\n' 是"这里断行"，不是可折行的普通
    // 字符 —— 原来只按宽度折行，'\n' 既不断行又不显字形，整条释义就糊成一坨。
    // 空行照样入列（段落间距要留住）；"\r\n" 里的 '\r' 直接丢掉，免得多个空字形。
    if (c == '\n') {
      lines.push_back(line);
      line.clear();
      lineW = 0;
      i++;
      continue;
    }
    if (c == '\r') {
      i++;
      continue;
    }
    const int len = utf8CharLen(c);
    if (i + static_cast<size_t>(len) > s.size()) break;
    // 每个字符只量它自己，宽度累加 —— 原实现是 line + ch 重拼整行再整行量宽，
    // 行长 N 时整段 O(N²)。ttf 面量宽就是逐码点相加，累加与前缀宽之和逐位相同。
    char cb[8];
    memcpy(cb, s.data() + i, static_cast<size_t>(len));
    cb[len] = '\0';
    const int chW = getTextWidth(fontId, cb, style);
    if (lineW + chW > maxWidth && !line.empty()) {
      lines.push_back(line);
      line.assign(cb, static_cast<size_t>(len));
      lineW = chW;
    } else {
      line.append(cb, static_cast<size_t>(len));
      lineW += chW;
    }
    i += static_cast<size_t>(len);
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}
void GfxRenderer::drawTextRotated90CW(const int fontId, const int x, const int y, const char* text,
                                      const bool black, const EpdFontFamily::Style style) const {
  // Phase 1：侧边按钮竖排文字简化 —— 不旋转，水平绘制。
  drawText(fontId, x, y, text, black, style);
}

// ---- Grayscale (4bpp 原生灰度下为 no-op) ----

void GfxRenderer::preconditionGrayscale() const {}
void GfxRenderer::preconditionGrayscale(int x, int y, int w, int h) const { (void)x; (void)y; (void)w; (void)h; }
void GfxRenderer::displayGrayscaleBase(HalDisplay::RefreshMode fallback, DisplayRefreshContext context) const {
  display.displayGrayscaleBase(fallback, false, context);
}
void GfxRenderer::copyGrayscaleLsbBuffers() const {}
void GfxRenderer::copyGrayscaleMsbBuffers() const {}
void GfxRenderer::displayGrayBuffer() const {}
void GfxRenderer::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* scratch, int yStart, int numRows) const {
  (void)lsbPlane; (void)scratch; (void)yStart; (void)numRows;
}
bool GfxRenderer::supportsStripGrayscale() const { return false; }
bool GfxRenderer::combinesGrayscaleBase() const { return false; }
bool GfxRenderer::supportsTextOnlyCombinedBase() const { return false; }
bool GfxRenderer::supportsReaderTransitions() const { return false; }
bool GfxRenderer::supportsContinuousImageReading() const { return false; }
bool GfxRenderer::canUseTextTransition() const { return false; }
void GfxRenderer::cancelGrayscale() const {}

// ---- Font helpers ----

const uint8_t* GfxRenderer::getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const {
  (void)fontData; (void)glyph;
  return nullptr;
}

// ---- Framebuffer loan (Phase 1：不真正释放) ----

void GfxRenderer::releaseFrameBufferForBuild() {}
bool GfxRenderer::restoreFrameBufferAfterBuild() { return true; }

// ---- BW buffer store/restore（4bpp 单缓冲下为 no-op） ----

bool GfxRenderer::storeBwBuffer() { return false; }
void GfxRenderer::restoreBwBuffer(bool resyncPanelBaseline) { (void)resyncPanelBaseline; }
void GfxRenderer::cleanupGrayscaleWithFrameBuffer() const {}

// ---- Low level ----

uint8_t* GfxRenderer::getFrameBuffer() const { return frameBuffer; }
void GfxRenderer::setFrameBuffer(uint8_t* fb) { frameBuffer = fb; }
size_t GfxRenderer::getBufferSize() const { return frameBufferSize; }
uint8_t GfxRenderer::getGrayscaleLevels() const { return display.getGrayscaleLevels(); }
bool GfxRenderer::beginGrayscale16() { return false; }
bool GfxRenderer::commitGrayscale16() const { return false; }
void GfxRenderer::cancelGrayscale16() const {}
void GfxRenderer::drawGrayscale16Pixel(int x, int y, uint8_t gray) const {
  if (!frameBuffer) return;
  epd_draw_pixel(x, y, static_cast<uint8_t>(gray << 4), frameBuffer);
}
bool GfxRenderer::drawBitmapGrayscale16(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX,
                                        float cropY) const {
  (void)bitmap; (void)x; (void)y; (void)maxWidth; (void)maxHeight; (void)cropX; (void)cropY;
  return false;
}
size_t GfxRenderer::getRegionByteSize(int logicalX, int logicalY, int logicalW, int logicalH) const {
  (void)logicalX; (void)logicalY; (void)logicalW; (void)logicalH;
  return 0;
}
bool GfxRenderer::copyRegionToBuffer(int logicalX, int logicalY, int logicalW, int logicalH, uint8_t* buf,
                                     size_t bufSize) const {
  (void)logicalX; (void)logicalY; (void)logicalW; (void)logicalH; (void)buf; (void)bufSize;
  return false;
}
bool GfxRenderer::copyBufferToRegion(int logicalX, int logicalY, int logicalW, int logicalH, const uint8_t* buf,
                                     size_t bufSize) const {
  (void)logicalX; (void)logicalY; (void)logicalW; (void)logicalH; (void)buf; (void)bufSize;
  return false;
}

// ---- Private helpers ----

int GfxRenderer::resolveFontFamilyId(int fontId) const {
  auto it = preferredFontMap_.find(fontId);
  return it != preferredFontMap_.end() ? it->second : fontId;
}
int GfxRenderer::resolveTextFontId(int fontId, const char* text, EpdFontFamily::Style style) const {
  (void)text; (void)style;
  return resolveFontFamilyId(fontId);
}
void GfxRenderer::ensureSdGlyphsResident(int fontId, const char* text, EpdFontFamily::Style style,
                                         bool metadataOnly) const {
  (void)fontId; (void)text; (void)style; (void)metadataOnly;
}
void GfxRenderer::ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask) const {
  (void)fontId; (void)utf8Text; (void)styleMask;
}
void GfxRenderer::ensureSdCardFontReady(int fontId, const std::deque<std::string>& words, bool includeHyphen,
                                        uint8_t styleMask) const {
  (void)fontId; (void)words; (void)includeHyphen; (void)styleMask;
}
void GfxRenderer::renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, int* y, bool pixelState,
                             EpdFontFamily::Style style) const {
  (void)fontFamily; (void)cp; (void)x; (void)y; (void)pixelState; (void)style;
}
void GfxRenderer::freeBwBufferChunks() {
  for (uint8_t* chunk : bwBufferChunks) free(chunk);
  bwBufferChunks.clear();
}

// ---- FrameBufferLoan ----

GfxRenderer::FrameBufferLoan::FrameBufferLoan(GfxRenderer& renderer) : renderer_(renderer), active_(true) {
  renderer_.releaseFrameBufferForBuild();
}
void GfxRenderer::FrameBufferLoan::end() {
  if (active_) {
    renderer_.restoreFrameBufferAfterBuild();
    active_ = false;
  }
}
