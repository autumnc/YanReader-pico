#pragma once
#include "EpdFont.h"

class EpdFontFamily {
 public:
  // Bitmask of text style flags carried per-word through layout and serialized in page cache.
  // Bits 0-1 select the font variant (BOLD/ITALIC); bits 2-5 are decoration/positioning overlays
  // applied at render time without changing the underlying font. getFont() ignores all bits
  // above bit 1 so decorations compose freely with bold/italic (e.g. BOLD | UNDERLINE | SUP).
  //
  // 2026-10-05：位宽从 uint8_t 加宽到 uint16_t。低 8 位当时已经**一个不剩**
  // （BOLD/ITALIC/UNDERLINE/STRIKETHROUGH/SUP/SUB/RUBY_CONTINUE/ALT_FONT），而古籍
  // EPUB 的书名线要跟专名线区分（波浪 vs 直线），必须再要一位。加宽的代价是每个词的
  // 样式从 1 字节变 2 字节（ParsedText::wordStyles 与 TextBlock 的 arena 各一份），
  // 并且 arena 是**逐字节落盘**的（TextBlock::serialize），所以必须同步 bump
  // Section.cpp 的 SECTION_FILE_VERSION。getFont() 只看低两位，新位不影响字体选择。
  enum Style : uint16_t {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,       // drawn as a line below baseline by TextBlock::render()
    STRIKETHROUGH = 8,   // drawn as a line through midline by TextBlock::render()
    SUP = 16,            // superscript: glyph scaled 50%, raised ~40% of ascender
    SUB = 32,            // subscript: glyph scaled 50%, lowered ~25% of ascender
    RUBY_CONTINUE = 64,  // Group ruby follower marker (used internally by Epub layout)
    // 这一段用书内 CSS 的**第二个家族**画（祖堂集：正文宋体、注文/引文仿宋）。
    // 只改选哪个字面(role)，不改字号、不加任何装饰 —— GfxRenderer 据此把 ttf 角色
    // 切到次字面；次字面没打开时静默回到内容面，绘制力与不加这一位时完全一致。
    ALT_FONT = 128,
    // 波浪下划线（书名线）。跟 UNDERLINE 是**互斥的两种画法**，不是叠加：两者都置位时
    // 按波浪画（见 TextBlock::render 的 tracker）。来自 CSS 的
    // `text-decoration: duokan-wavyline` / `text-decoration-style: wavy` / border-image。
    WAVY_UNDERLINE = 256,
  };
  static constexpr uint16_t TEXT_DECORATION_MASK = static_cast<uint16_t>(UNDERLINE | STRIKETHROUGH | WAVY_UNDERLINE);

  explicit EpdFontFamily(const EpdFont* regular, const EpdFont* bold = nullptr, const EpdFont* italic = nullptr,
                         const EpdFont* boldItalic = nullptr)
      : regular(regular), bold(bold), italic(italic), boldItalic(boldItalic) {}
  ~EpdFontFamily() = default;
  void getTextDimensions(const char* string, int* w, int* h, Style style = REGULAR) const;
  const EpdFontData* getData(Style style = REGULAR) const;
  const EpdGlyph* getGlyph(uint32_t cp, Style style = REGULAR) const;
  const EpdGlyph* getGlyph(uint32_t cp, Style style, bool* usedReplacement) const;
  /// Returns true if the resolved style's font can render `cp` directly
  /// (interval coverage only — see EpdFont::hasCodepoint).
  bool hasCodepoint(uint32_t cp, Style style = REGULAR) const;
  int8_t getKerning(uint32_t leftCp, uint32_t rightCp, Style style = REGULAR) const;
  uint32_t applyLigatures(uint32_t cp, const char*& text, Style style = REGULAR) const;
  static constexpr bool hasTextDecoration(const Style style) {
    return (static_cast<uint16_t>(style) & TEXT_DECORATION_MASK) != 0;
  }

 private:
  const EpdFont* regular;
  const EpdFont* bold;
  const EpdFont* italic;
  const EpdFont* boldItalic;

  const EpdFont* getFont(Style style) const;
};
