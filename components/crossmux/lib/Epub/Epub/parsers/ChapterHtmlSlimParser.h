#pragma once

#include <HalStorage.h>
#include <expat.h>

#include <array>
#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/css/CssParser.h"
#include "Epub/css/CssStyle.h"

class Page;
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)> completePageFn;
  std::function<void()> popupFn;  // Popup callback
  bool imagePopupFired = false;   // popupFn fired for the first image probe (single-shot)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  // Ruby text state
  bool inRuby = false;
  int rubyStartWordIndex = -1;
  bool collectingRubyText = false;
  std::string rubyTextBuffer;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  // 最近一行正文落在**哪一页**的什么位置。行内图片（`<img>` 夹在句子中间）要接在它
  // 右边同一行上，就得知道这一行在哪儿结束——而结束位置只有在那一行排版出来之后才
  // 知道。存页面指针是为了换页后自动失效：翻页会换掉 currentPage，指针对不上就说明
  // 记下的那一行已经不在当前页上了，行内插入必须放弃、退回整块插图。
  const Page* lastLinePage = nullptr;
  int16_t lastLineY = 0;
  int16_t lastLineBottom = 0;  // 这一行自己的下沿（行高用的是行自己的字体，不是当前块的）
  int16_t lastLineEndX = 0;    // 绝对 x（含块左内边距）：行内最后一个字排到哪儿
  // 每落一行 +1。图片处理前先记一个号，冲刷之后再比：号没变就说明"这行"其实是上一段
  // 遗留的（中间只有空白），不能拿来当行内图片的落脚点。
  uint32_t lineSerial = 0;
  int fontId;
  // 当前正在排的块用的字体 id。书内 CSS 的 font-size 会让标题/注释落到别的字号档上
  // （GfxRenderer::cssFontId，倍率被吸附到阅读器注册的 5 档之一）；书里没写 font-size
  // 时它恒等于 fontId。跟着 blockStyleStack 一起换，且必须**在 startNewTextBlock
  // 之后**再换——那一步会把上一个块排版出去，用的还得是上一个块自己的字体。
  int currentFontId = 0;
  // CSS 的 `100%` / `1em` 折成多少 px。优先用书自己在 body/html 上声明的**绝对**字号
  // （很多书 body{font-size:20px}；不认它的话整本书都会被算成 20/34≈0.6 而全篇掉到最小号，
  // 把阅读器选的字号整个抹掉），书没声明就退到阅读器正文字号。
  float cssBaseFontPx = 0.0f;
  /// 块字体：CSS 有 font-size 就吸附到梯子，否则就是 reading 的 fontId。
  [[nodiscard]] int resolvedFontId(const BlockStyle& blockStyle) const;
  float lineCompression;
  uint8_t extraParagraphSpacing;  // 0=off, 1..5=0.5x/0.75x/1x/1.25x/1.5x line height
  uint8_t firstLineIndent;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  const CssParser* cssParser;
  bool embeddedStyle;
  bool collectTouchLinks;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasTextDecoration = false;
    CssTextDecoration textDecoration = CssTextDecoration::None;
    bool hasDirection = false;
    CssTextDirection direction = CssTextDirection::Ltr;
    bool setsParagraphDirection = false;
    bool hasTextAlign = false;
    CssTextAlign textAlign = CssTextAlign::Left;
    bool hasSup = false, sup = false;
    bool hasSub = false, sub = false;
    // 次家族（EpdFontFamily::ALT_FONT）。跟 bold/italic 不同，它不来自标签本身，
    // 而来自这一层的 CSS：解析器先把家族哈希跟 GfxRenderer 里的次家族哈希比一比，
    // 相等才置位。这样 <span class="zhu"> 这类**行内**家族切换也能一路传到词上。
    bool hasAltFont = false, altFont = false;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  std::vector<BlockStyle> blockStyleStack;  // accumulated block styles from open ancestor elements
  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  CssTextDecoration effectiveTextDecoration = CssTextDecoration::None;
  bool effectiveDirectionDefined = false;
  CssTextDirection effectiveDirection = CssTextDirection::Ltr;
  bool effectiveTextAlignDefined = false;
  CssTextAlign effectiveTextAlign = CssTextAlign::Left;
  bool effectiveSup = false;
  bool effectiveSub = false;
  // 这一处的字用次字面画（书内 CSS 的第二个家族）。判据只有一个：当前生效的 CSS
  // 家族哈希 == GfxRenderer::altFontFamilyHash()，见 updateEffectiveInlineStyle。
  bool effectiveAltFont = false;
  static constexpr size_t MAX_GRID_TABLE_COLUMNS = 4;
  static constexpr size_t MAX_GRID_TABLE_CELL_WORDS = 32;
  static constexpr size_t MAX_GRID_TABLE_CELL_BYTES = 512;
  int tableDepth = 0;
  bool insideTableCell = false;
  bool tableRowStacked = false;
  bool tableRowRtl = false;
  uint16_t tableRowsSpannedRemaining = 0;
  size_t tableCellTextBytes = 0;
  std::vector<std::unique_ptr<ParsedText>> tableRowCells;
  std::array<std::vector<std::unique_ptr<TextBlock>>, MAX_GRID_TABLE_COLUMNS> tableCellLines;
  std::vector<uint32_t> tableLineVisibleOffsets;
  bool listItemBulletOnly = false;  // true when currentTextBlock has only the <li> bullet

  // 锚点记录：id → 落在哪一页、页内第几个元素。
  // 光有页码不够用：注文常常一页里挤着好几条（祖堂集一个 <li> 一条校勘记、晋书一个
  // <p> 一条），只有页码时弹注只能把整页捞出来，前后几条一起显示。`element` 在
  // "锚点所在块开始排版"的那一刻记下（= 当时该页已有的元素个数），读端按同样次序
  // 就能精确挑出那一段。越界（块换页）时读端会忽略它，退回按注号认。
  struct AnchorRecord {
    std::string id;
    uint16_t page = 0;
    uint16_t element = 0;
  };
  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  std::vector<AnchorRecord> anchorData;
  std::string pendingAnchorId;          // deferred until after previous text block is flushed
  std::vector<std::string> tocAnchors;  // the list of anchors that are TOC chapter boundaries
  uint16_t xpathParagraphIndex = 0;
  uint16_t xpathListItemIndex = 0;
  // Canonical reading-position counter: zero-based Unicode codepoints in visible
  // <body> text. Token offsets flow through line breaking so every completed page
  // records the first source character it renders.
  uint32_t visibleTextOffset = 0;
  uint32_t partWordVisibleOffset = 0;
  uint32_t currentPageVisibleOffset = 0;
  bool currentPageVisibleOffsetSet = false;
  bool allocationFailed_ = false;
  bool ioFailed_ = false;
  bool parseActive_ = false;
  void stopParsing();
  void failAllocation(const char* stage);
  bool checkMemory();
  bool insideBody = false;
  bool htmlEnded_ = false;
  bool syntheticCharacterData = false;
  uint16_t nonVisibleTextDepth = 0;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  uint8_t currentFootnoteLinkId = 0;
  FootnoteEntry currentFootnote = {};
  int currentFootnoteLinkTextLen = 0;
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;

  // 回引（注文 → 正文注号）识别。注号链接**自己的**锚点 id 存在这里：Duokan 是
  // `<a id="noteref_1" href="#note_1">` 的 noteref_1，趙州録是 `<a id="1" href="#2">` 的 1，
  // 晋书是外层 `<sup id="ref-001">` 的 ref-001。之后任何一条内链的 href 指回这些 id，
  // 就是"从注文跳回正文"的回引 —— 不该再登记成脚注条目，否则注文那一页会凭空多出几条
  // 指向正文的"脚注"（点上去弹错东西），还挤占每页 16 条的 FootnoteList。
  // 只在同一章内比对：回引都是页内锚点。
  std::vector<std::string> footnoteMarkerAnchors;
  bool currentFootnoteIsBackref = false;

  // Resumable parse state. The one-shot parseAndBuildPages() drives these
  // internally; the incremental section builder drives them across render ticks
  // so a large single chapter can yield between pages instead of blocking the UI
  // until the whole thing is laid out. parseFile_ and the expat parser stay alive
  // for the lifetime of the parse so it can be paused and resumed at buffer
  // boundaries.
  XML_Parser xmlParser_ = nullptr;
  HalFile parseFile_;
  // 64 字节对齐的读缓冲（见 .cpp 里的说明）。不再借用 expat 的 XML_GetBuffer，
  // 因为那块的地址/长度都不满足 SDMMC 的直接 DMA 条件，会被迫走弹跳缓冲。
  uint8_t* readBuf_ = nullptr;
  uint32_t parseStartTime_ = 0;

  void updateEffectiveInlineStyle();
  void startNewTextBlock(const BlockStyle& blockStyle);
  void flushPendingAnchor();
  void flushPartWordBuffer();
  void softFlushTextBlock();
  void fallbackTableRowToStacked();
  void closeTableCell();
  void finishTableRow();
  void addTableRowSeparator();
  void setCurrentPageVisibleOffset(uint32_t offset);
  bool allocatePage();
  void makePages();
  static EpdFontFamily::Style fontStyleForTextDecoration(CssTextDecoration decoration);
  static void applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyTextDecorationToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& css);
  /// 这条 CSS 是不是把文字交给了书内 CSS 的第二个家族（次字面）。见 .cpp。
  [[nodiscard]] bool isAltFamily(const CssStyle& css) const;
  void applyAltFontToEntry(StyleStackEntry& entry, const CssStyle& css);
  void pushTableTextStyleEntry(const CssStyle& cssStyle);
  void pushDecorationStyleEntry(CssTextDecoration defaultDecoration, const CssStyle& cssStyle);
  void emitHorizontalRule(const BlockStyle& blockStyle);
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(
      std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer, const int fontId,
      const float lineCompression, const uint8_t extraParagraphSpacing, const uint8_t firstLineIndent,
      const uint8_t paragraphAlignment, const uint16_t viewportWidth, const uint16_t viewportHeight,
      const bool hyphenationEnabled, const bool focusReadingEnabled,
      const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)>& completePageFn,
      const bool embeddedStyle, const std::string& contentBase, const std::string& imageBasePath,
      const uint8_t imageRendering = 0, std::vector<std::string> tocAnchors = {},
      const std::function<void()>& popupFn = nullptr, const CssParser* cssParser = nullptr,
      const bool collectTouchLinks = false)

      : epub(epub),
        filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        currentFontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        firstLineIndent(firstLineIndent),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        completePageFn(completePageFn),
        popupFn(popupFn),
        cssParser(embeddedStyle ? cssParser : nullptr),
        embeddedStyle(embeddedStyle),
        collectTouchLinks(collectTouchLinks),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath),
        tocAnchors(std::move(tocAnchors)) {}

  ~ChapterHtmlSlimParser();

  // One-shot parse: builds every page before returning (begin + step* + finish).
  bool parseAndBuildPages();

  // Resumable parse, for the incremental section builder. Drive as:
  //   if (!beginParse()) fail;
  //   More: keep going / yield; Done: finishParse(); Error / OutOfMemory: abortParse().
  // Pages are emitted via completePageFn as they complete during parseStep(), so
  // the caller can stop once enough pages are built and resume on a later tick.
  enum class ParseStatus { More, Done, Error, OutOfMemory };
  bool allocationFailed() const { return allocationFailed_; }
  bool ioFailed() const { return ioFailed_; }
  bool hasFailed() const { return allocationFailed_ || ioFailed_; }
  void failIo() {
    ioFailed_ = true;
    stopParsing();
  }
  bool beginParse();
  ParseStatus parseStep();
  bool finishParse();  // flush the trailing page and tear down; returns true
  void abortParse();   // tear down without flushing (error / abandon)

  bool addLineToPage(std::unique_ptr<TextBlock> line, uint32_t visibleOffset);
  const std::vector<AnchorRecord>& getAnchors() const { return anchorData; }

  // Byte progress of the in-flight parse, used to estimate a still-building section's total page
  // count (a giant single-spine book never fully lays out, so its real count is unknown). Valid
  // between beginParse() and finishParse()/abortParse().
  size_t parseBytesConsumed() { return parseFile_ ? parseFile_.position() : 0; }
  size_t parseTotalBytes() { return parseFile_ ? parseFile_.size() : 0; }
};
