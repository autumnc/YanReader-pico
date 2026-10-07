#pragma once

#include <BidiUtils.h>
#include <EpdFontFamily.h>
#include <FontCacheManager.h>

#include <deque>
#include <string>

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}  // namespace BidiUtils

class GfxRenderer {
 public:
  FontCacheManager fontCache;
  FontCacheManager* getFontCacheManager() { return &fontCache; }
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  int getLineHeight(int, float = 1.0f) const { return 16; }
  int getFontAscenderSize(int) const { return 12; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 4; }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style) const {
    int width = 0;
    while (*text++) width += 8;
    return width;
  }
  bool isFontCacheScanning() const { return false; }
  int getTextWidth(int, const char* text, EpdFontFamily::Style,
                   BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO) const {
    int width = 0;
    while (*text++) width += 8;
    return width;
  }
  static constexpr int kWavyAmplitudePx = 2;
  void drawLine(int, int, int, int, bool = true) const {}
  void drawLine(int, int, int, int, int, bool) const {}
  void drawWavyLine(int, int, int, int, int, bool) const {}
  // 探针只跑解析/排版，不真的画字：渲染入口全部空实现。
  void drawText(int, int, int, const char*, bool = true,
                EpdFontFamily::Style = EpdFontFamily::REGULAR,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO) const {}
  int getKerning(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 0; }
  // 端口版加的 CSS 字号梯子面：探针只需要它们存在且返回合理值。
  int cssFontId(int baseFontId, float) const { return baseFontId; }
  uint32_t altFontFamilyHash() const { return 0; }
  void setAltFontFamilyHash(uint32_t) {}
  uint32_t alt2FontFamilyHash() const { return 0; }
  void setAlt2FontFamilyHash(uint32_t) {}
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 4; }
  bool isSdCardFont(int) const { return false; }
  void ensureSdCardFontReady(int, const std::deque<std::string>&, bool, uint8_t) const {}
};
