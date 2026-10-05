#pragma once

#include <cstdint>

// Matches order of PARAGRAPH_ALIGNMENT in CrossPointSettings
enum class CssTextAlign : uint8_t { Justify = 0, Left = 1, Center = 2, Right = 3, None = 4 };
enum class CssUnit : uint8_t { Pixels = 0, Em = 1, Rem = 2, Points = 3, Percent = 4 };
enum class CssTextDirection : uint8_t { Ltr = 0, Rtl = 1 };

// Represents a CSS length value with its unit, allowing deferred resolution to pixels
struct CssLength {
  float value = 0.0f;
  CssUnit unit = CssUnit::Pixels;

  CssLength() = default;
  CssLength(const float v, const CssUnit u) : value(v), unit(u) {}

  // Convenience constructor for pixel values (most common case)
  explicit CssLength(const float pixels) : value(pixels) {}

  // Returns true if this length can be resolved to pixels with the given context.
  // Percentage units require a non-zero containerWidth to resolve.
  [[nodiscard]] bool isResolvable(const float containerWidth = 0) const {
    return unit != CssUnit::Percent || containerWidth > 0;
  }

  // Resolve to pixels given the current em size (font line height)
  // containerWidth is needed for percentage units (e.g. viewport width)
  [[nodiscard]] float toPixels(const float emSize, const float containerWidth = 0) const {
    switch (unit) {
      case CssUnit::Em:
      case CssUnit::Rem:
        return value * emSize;
      case CssUnit::Points:
        return value * 1.33f;  // Approximate pt to px conversion
      case CssUnit::Percent:
        return value * containerWidth / 100.0f;
      default:
        return value;
    }
  }

  // Resolve to int16_t pixels (for BlockStyle fields)
  [[nodiscard]] int16_t toPixelsInt16(const float emSize, const float containerWidth = 0) const {
    return static_cast<int16_t>(toPixels(emSize, containerWidth));
  }
};

// Font style options matching CSS font-style property
enum class CssFontStyle : uint8_t { Normal = 0, Italic = 1 };

// Font weight options - CSS supports 100-900, we simplify to normal/bold
enum class CssFontWeight : uint8_t { Normal = 0, Bold = 1 };

// Text decoration options. Values are bit flags so CSS can combine multiple line decorations.
// Wavy = 波浪下划线（古籍书名线）：本身**不**画线，只是给 Underline 换个笔法；单独出现
// （没有 Underline）时应当什么都不画，所以 ChapterHtmlSlimParser 的映射里
// `Underline|Wavy → 波浪`、`Wavy 单独 → 无`。值域仍在 uint8_t 内，CssStyle 布局不变，
// 所以 CSS 缓存不需要 version bump。
//
// BorderLine = 8 也**不画线**，它记的是"这条线的出处是 `border-bottom`"——见下面的
// mergeBorderLine()。只查 Underline/Wavy/LineThrough 位的消费端（fontStyleForTextDecoration）
// 看不见它，无害。
enum class CssTextDecoration : uint8_t { None = 0, Underline = 1, LineThrough = 2, Wavy = 4, BorderLine = 8 };

constexpr CssTextDecoration operator|(const CssTextDecoration a, const CssTextDecoration b) {
  return static_cast<CssTextDecoration>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr CssTextDecoration operator&(const CssTextDecoration a, const CssTextDecoration b) {
  return static_cast<CssTextDecoration>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

constexpr uint8_t CSS_TEXT_DECORATION_MASK = static_cast<uint8_t>(CssTextDecoration::Underline) |
                                             static_cast<uint8_t>(CssTextDecoration::LineThrough) |
                                             static_cast<uint8_t>(CssTextDecoration::Wavy) |
                                             static_cast<uint8_t>(CssTextDecoration::BorderLine);

// `border-bottom` 和 `text-decoration` 在 CSS 里是**两个独立的属性**：后者写 `none` 只能取消
// 自己那条线，抹不掉前者的边框线。春秋左传注的 `span.q` / `u` 正是同一块里先写
// `border-bottom: 1px …`、再写 `text-decoration: none`（作者只想声明"别给我加文字装饰线"），
// 浏览器照样画边框 —— 而这个移植版把两者折进同一个位段，朴素的整体赋值就把线抹没了，
// 那本书的书名线/专名线一条都画不出来。
//
// 所以在**任何要整体赋值 textDecoration 的地方**（CSS 的 text-decoration 分支、以及
// CssStyle::applyOver 的级联）都过一遍这个函数：把 carried 里来自边框的那一份
// （BorderLine 标记 + 它带的 Wavy 笔法）并回 incoming。边框线本身必须是实线，
// 所以 BorderLine 一定连带 Underline。
constexpr CssTextDecoration mergeBorderLine(const CssTextDecoration incoming, const CssTextDecoration carried) {
  if ((carried & CssTextDecoration::BorderLine) == CssTextDecoration::None) return incoming;
  CssTextDecoration result = incoming | CssTextDecoration::BorderLine | CssTextDecoration::Underline;
  if ((carried & CssTextDecoration::Wavy) != CssTextDecoration::None) {
    result = result | CssTextDecoration::Wavy;
  }
  return result;
}

// Display options - only None and Block are relevant for e-ink rendering
enum class CssDisplay : uint8_t { Block = 0, None = 1 };

// Vertical alignment options for inline elements (e.g. superscript/subscript)
enum class CssVerticalAlign : uint8_t { Baseline = 0, Super = 1, Sub = 2 };

// Bitmask for tracking which properties have been explicitly set
struct CssPropertyFlags {
  uint16_t textAlign : 1;
  uint16_t fontStyle : 1;
  uint16_t fontWeight : 1;
  uint16_t fontSize : 1;
  uint16_t textDecoration : 1;
  uint16_t textIndent : 1;
  uint16_t marginTop : 1;
  uint16_t marginBottom : 1;
  uint16_t marginLeft : 1;
  uint16_t marginRight : 1;
  uint16_t paddingTop : 1;
  uint16_t paddingBottom : 1;
  uint16_t paddingLeft : 1;
  uint16_t paddingRight : 1;
  uint16_t imageHeight : 1;
  uint16_t imageWidth : 1;
  uint16_t display : 1;
  uint16_t direction : 1;
  uint16_t verticalAlign : 1;
  uint16_t fontFamily : 1;

  CssPropertyFlags()
      : textAlign(0),
        fontStyle(0),
        fontWeight(0),
        fontSize(0),
        textDecoration(0),
        textIndent(0),
        marginTop(0),
        marginBottom(0),
        marginLeft(0),
        marginRight(0),
        paddingTop(0),
        paddingBottom(0),
        paddingLeft(0),
        paddingRight(0),
        imageHeight(0),
        imageWidth(0),
        display(0),
        direction(0),
        verticalAlign(0),
        fontFamily(0) {}

  [[nodiscard]] bool anySet() const {
    return textAlign || fontStyle || fontWeight || fontSize || textDecoration || textIndent || marginTop ||
           marginBottom || marginLeft || marginRight || paddingTop || paddingBottom || paddingLeft || paddingRight ||
           imageHeight || imageWidth || display || direction || verticalAlign || fontFamily;
  }

  void clearAll() {
    textAlign = fontStyle = fontWeight = fontSize = textDecoration = textIndent = 0;
    marginTop = marginBottom = marginLeft = marginRight = 0;
    paddingTop = paddingBottom = paddingLeft = paddingRight = 0;
    imageHeight = imageWidth = display = direction = verticalAlign = fontFamily = 0;
  }
};

// Cache serializes defined flags as uint32_t with bit indices 0..19.
static_assert(sizeof(CssPropertyFlags) <= sizeof(uint32_t),
              "CssPropertyFlags exceeds 32 bits; update cache read/write in CssParser.cpp");

// Represents a collection of CSS style properties
// Only stores properties relevant to e-ink text rendering
// Length values are stored as CssLength (value + unit) for deferred resolution
struct CssStyle {
  CssTextAlign textAlign = CssTextAlign::Left;
  CssFontStyle fontStyle = CssFontStyle::Normal;
  CssFontWeight fontWeight = CssFontWeight::Normal;
  CssTextDecoration textDecoration = CssTextDecoration::None;
  CssTextDirection direction = CssTextDirection::Ltr;

  CssLength textIndent;     // First-line indent (deferred resolution)
  // Requested font size. Only the *relative* magnitude survives into layout
  // (BlockStyle::fontScale → a font from the reader's ladder): the engine ships
  // a fixed set of fonts, so an absolute px value is turned into a ratio
  // against the body font. Only meaningful when embedded styling is on.
  CssLength fontSize;
  CssLength marginTop;      // Vertical spacing before block
  CssLength marginBottom;   // Vertical spacing after block
  CssLength marginLeft;     // Horizontal spacing left of block
  CssLength marginRight;    // Horizontal spacing right of block
  CssLength paddingTop;     // Padding before
  CssLength paddingBottom;  // Padding after
  CssLength paddingLeft;    // Padding left
  CssLength paddingRight;   // Padding right
  CssLength imageHeight;    // Height for img (e.g. 2em) – width derived from aspect ratio when only height set
  CssLength imageWidth;     // Width for img when both or only width set
  CssDisplay display = CssDisplay::Block;                       // display property (Block or None)
  CssVerticalAlign verticalAlign = CssVerticalAlign::Baseline;  // vertical-align (super/sub positioning)

  // 这条规则把文字交给**书内 CSS 的哪个家族**。存的是家族名归一化后的 32 位哈希
  // （0 = 没写）。不建家族表、不做多字面引擎：设备上同时只可能常驻两个书内字面
  // （正文家族 + 次家族，见 ttf_font.c 的 role），所以排版时只需要回答"是不是次家族"
  // 这一个问题 —— 拿它跟 Epub::resolveEmbeddedFonts() 定出的次家族哈希比一比即可
  // （ChapterHtmlSlimParser::updateEffectiveInlineStyle）。第 3 个及以后的家族照旧
  // 落到正文字面，与"只认一个家族"的旧行为一致。
  uint32_t fontFamilyHash = 0;

  CssPropertyFlags defined;  // Tracks which properties were explicitly set

  // Apply properties from another style, only overwriting if the other style
  // has that property explicitly defined
  void applyOver(const CssStyle& base) {
    if (base.hasTextAlign()) {
      textAlign = base.textAlign;
      defined.textAlign = 1;
    }
    if (base.hasFontStyle()) {
      fontStyle = base.fontStyle;
      defined.fontStyle = 1;
    }
    if (base.hasFontWeight()) {
      fontWeight = base.fontWeight;
      defined.fontWeight = 1;
    }
    if (base.hasTextDecoration()) {
      // base 优先级更高，整个接管 —— 但自己那条**边框线**不能被它的 `text-decoration: none`
      // 抹掉（两个属性互不相干），见 mergeBorderLine。
      textDecoration = mergeBorderLine(base.textDecoration, textDecoration);
      defined.textDecoration = 1;
    }
    if (base.hasFontSize()) {
      fontSize = base.fontSize;
      defined.fontSize = 1;
    }
    if (base.hasTextIndent()) {
      textIndent = base.textIndent;
      defined.textIndent = 1;
    }
    if (base.hasMarginTop()) {
      marginTop = base.marginTop;
      defined.marginTop = 1;
    }
    if (base.hasMarginBottom()) {
      marginBottom = base.marginBottom;
      defined.marginBottom = 1;
    }
    if (base.hasMarginLeft()) {
      marginLeft = base.marginLeft;
      defined.marginLeft = 1;
    }
    if (base.hasMarginRight()) {
      marginRight = base.marginRight;
      defined.marginRight = 1;
    }
    if (base.hasPaddingTop()) {
      paddingTop = base.paddingTop;
      defined.paddingTop = 1;
    }
    if (base.hasPaddingBottom()) {
      paddingBottom = base.paddingBottom;
      defined.paddingBottom = 1;
    }
    if (base.hasPaddingLeft()) {
      paddingLeft = base.paddingLeft;
      defined.paddingLeft = 1;
    }
    if (base.hasPaddingRight()) {
      paddingRight = base.paddingRight;
      defined.paddingRight = 1;
    }
    if (base.hasImageHeight()) {
      imageHeight = base.imageHeight;
      defined.imageHeight = 1;
    }
    if (base.hasImageWidth()) {
      imageWidth = base.imageWidth;
      defined.imageWidth = 1;
    }
    if (base.hasDisplay()) {
      display = base.display;
      defined.display = 1;
    }
    if (base.hasDirection()) {
      direction = base.direction;
      defined.direction = 1;
    }
    if (base.hasVerticalAlign()) {
      verticalAlign = base.verticalAlign;
      defined.verticalAlign = 1;
    }
    if (base.hasFontFamily()) {
      fontFamilyHash = base.fontFamilyHash;
      defined.fontFamily = 1;
    }
  }

  [[nodiscard]] bool hasTextAlign() const { return defined.textAlign; }
  [[nodiscard]] bool hasFontStyle() const { return defined.fontStyle; }
  [[nodiscard]] bool hasFontWeight() const { return defined.fontWeight; }
  [[nodiscard]] bool hasFontSize() const { return defined.fontSize; }
  [[nodiscard]] bool hasTextDecoration() const { return defined.textDecoration; }
  [[nodiscard]] bool hasTextIndent() const { return defined.textIndent; }
  [[nodiscard]] bool hasMarginTop() const { return defined.marginTop; }
  [[nodiscard]] bool hasMarginBottom() const { return defined.marginBottom; }
  [[nodiscard]] bool hasMarginLeft() const { return defined.marginLeft; }
  [[nodiscard]] bool hasMarginRight() const { return defined.marginRight; }
  [[nodiscard]] bool hasPaddingTop() const { return defined.paddingTop; }
  [[nodiscard]] bool hasPaddingBottom() const { return defined.paddingBottom; }
  [[nodiscard]] bool hasPaddingLeft() const { return defined.paddingLeft; }
  [[nodiscard]] bool hasPaddingRight() const { return defined.paddingRight; }
  [[nodiscard]] bool hasImageHeight() const { return defined.imageHeight; }
  [[nodiscard]] bool hasImageWidth() const { return defined.imageWidth; }
  [[nodiscard]] bool hasDisplay() const { return defined.display; }
  [[nodiscard]] bool hasDirection() const { return defined.direction; }
  [[nodiscard]] bool hasVerticalAlign() const { return defined.verticalAlign; }
  [[nodiscard]] bool hasFontFamily() const { return defined.fontFamily; }

  void reset() {
    textAlign = CssTextAlign::Left;
    fontStyle = CssFontStyle::Normal;
    fontWeight = CssFontWeight::Normal;
    textDecoration = CssTextDecoration::None;
    direction = CssTextDirection::Ltr;
    fontFamilyHash = 0;
    textIndent = CssLength{};
    fontSize = CssLength{};
    marginTop = marginBottom = marginLeft = marginRight = CssLength{};
    paddingTop = paddingBottom = paddingLeft = paddingRight = CssLength{};
    imageHeight = imageWidth = CssLength{};
    display = CssDisplay::Block;
    verticalAlign = CssVerticalAlign::Baseline;
    defined.clearAll();
  }
};
