#include "ChapterHtmlSlimParser.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iterator>
#include <new>

#include "../../../../src/fontIds.h"
#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/VisibleTextUtils.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageDimsProbe.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "Epub/htmlEntities.h"

// Minimum file size (in bytes) to show indexing popup - smaller chapters don't benefit from it
// Minimum file size (in bytes) to show indexing popup - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_POPUP = 10 * 1024;  // 10KB
// 读块大小。取 4096：既是 64 的倍数（SDMMC 直接 DMA 的前提），又比原来的 1024 少 3/4 的
// SD 往返。用 XML_Parse 而不是 XML_GetBuffer —— 后者把 SD 读直接落在 expat 自己的缓冲区上，
// 而那块缓冲的地址/长度不满足 64 字节对齐，SDMMC 主机就会退回
// heap_caps_malloc(MALLOC_CAP_DMA) 的弹跳缓冲；本机内部 RAM 被 BLE/WiFi 挤到只剩几十 KB，
// 该分配会失败（"allocate_dma_buf: not enough mem"），整章解析随之中断、正文空白。
constexpr size_t PARSE_BUFFER_SIZE = 4096;

// Keep the larger basic-layout window on PSRAM targets. Constrained or styled
// parsing uses the smaller window, retaining the last line at each soft flush.
constexpr size_t TEXT_BLOCK_SOFT_FLUSH_WORDS = 750;
constexpr size_t TEXT_BLOCK_SOFT_FLUSH_WORDS_CONSTRAINED = 320;

// Hard cap on the number of anchor IDs recorded per chapter. Legitimate navigation
// anchors (TOC entries, footnotes, cross-references) rarely exceed a few hundred per
// chapter. A runaway count usually means a converter injected machine-generated IDs on
// every text fragment (e.g. Kobo KePub spans). The cap prevents unbounded heap growth
// on resource-constrained devices (~380KB heap). TOC anchors bypass this cap.
constexpr size_t MAX_ANCHORS_PER_CHAPTER = 1024;

// Reuse serializable PageLine/PageHorizontalRule elements for a small grid.
constexpr int16_t TABLE_CELL_HORIZONTAL_PADDING = 4;
constexpr int16_t TABLE_ROW_SEPARATOR_GAP = 4;
constexpr uint8_t TABLE_ROW_SEPARATOR_THICKNESS = 1;
constexpr int16_t TABLE_MIN_CELL_WIDTH_LINE_HEIGHTS = 3;

// expat 的解析缓冲区和内部对象一律从 PSRAM 分配。
//
// 起因：默认 malloc 走 MALLOC_CAP_INTERNAL 优先，内部 RAM 一旦碎片化就会兜底落到
// ESP32-S3 的 RTC fast RAM（8KB，地址 0x600FE000 起）。该区间在 IDF 里只登记
// MALLOC_CAP_INTERNAL，**不含 MALLOC_CAP_DMA**（见 heap/port/esp32s3/memory_layout.c
// 的 MALLOC_RTCRAM_BASE_CAPS，以及 esp_ptr_dma_capable() 只认 0x3FC88000..0x3FD00000）。
// 而 SDMMC 驱动决定"能否直接 DMA"时只检查地址对齐、不检查 DMA 能力
// （sd_host_check_buffer_alignment → is_aligned），于是 fread 会直接把 SD 数据 DMA 进
// 这块 RTC RAM：CPU 侧看到的是半旧的残留内容 → expat 在章节中途报错 →
// Section::abandonBuild() 把 pageCount 清零 → 整章正文空白。
// PSRAM 既宽裕、又是合法的 DMA 目标（驱动会做 cache msync），避开这个陷阱。
namespace {
void* expatPsramMalloc(size_t size) {
  void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  return p ? p : malloc(size);
}
void* expatPsramRealloc(void* ptr, size_t size) {
  void* p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
  return p ? p : realloc(ptr, size);
}
const XML_Memory_Handling_Suite kExpatPsramSuite = {expatPsramMalloc, expatPsramRealloc, free};
}  // namespace

constexpr const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
constexpr const char* BOLD_TAGS[] = {"b", "strong"};
constexpr const char* ITALIC_TAGS[] = {"i", "em"};
constexpr const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr const char* LINETHROUGH_TAGS[] = {"del", "s", "strike"};
constexpr const char* IMAGE_TAGS[] = {"img", "image"};
bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

// 弹注被做成小图时用的 class 名。QQ 阅读器是 "qqreader-footnote"，别的书也可能叫
// "footnote"/"note"/"fn"。判据宽松一点没关系：**必须同时带非空 alt** 才认（见调用处），
// 真正的插图通常没有 alt，所以不会把正经图片吞掉。
bool isFootnoteImageClass(const std::string& klass) {
  if (klass.empty()) return false;
  // 按非字母数字切 token，避免子串误伤（"definition" 里不该认出 "fn"）。
  // "qqreader-footnote" → {"qqreader","footnote"}；"footnote-ref" → {"footnote","ref"}。
  size_t i = 0;
  while (i < klass.size()) {
    while (i < klass.size() && !isalnum(static_cast<unsigned char>(klass[i]))) i++;
    size_t start = i;
    while (i < klass.size() && isalnum(static_cast<unsigned char>(klass[i]))) i++;
    if (i == start) break;
    std::string tok;
    tok.reserve(i - start);
    for (size_t j = start; j < i; j++) {
      char c = klass[j];
      tok.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    if (tok == "note" || tok == "footnote" || tok == "fn" || tok == "noteref") return true;
  }
  return false;
}

// 注号小图的判据，比 isFootnoteImageClass 再宽一档（**只在"它包在脚注内链里"时才用**，
// 见调用处）：class 里有 note/footnote/fn/noteref，或就是 sup（Duokan 把注号图写成
// class="sup"），或 src 基名里带 "note"（…/note.png）。正经插图三样都不占。
bool isFootnoteMarkerImage(const std::string& klass, const std::string& src) {
  if (isFootnoteImageClass(klass)) return true;
  size_t i = 0;
  while (i < klass.size()) {
    while (i < klass.size() && !isalnum(static_cast<unsigned char>(klass[i]))) i++;
    size_t start = i;
    while (i < klass.size() && isalnum(static_cast<unsigned char>(klass[i]))) i++;
    if (i == start) break;
    if (i - start == 3 && (klass[start] | 0x20) == 's' && (klass[start + 1] | 0x20) == 'u' &&
        (klass[start + 2] | 0x20) == 'p')
      return true;
  }
  const size_t slash = src.find_last_of("/\\");
  std::string base = slash == std::string::npos ? src : src.substr(slash + 1);
  for (char& c : base) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return base.find("note") != std::string::npos;
}

std::string trimAndNormalize(const std::string& str) {
  if (str.empty()) return "";
  size_t start = 0;
  while (start < str.size() && isWhitespace(str[start])) {
    start++;
  }
  if (start == str.size()) return "";
  size_t end = str.size() - 1;
  while (end > start && isWhitespace(str[end])) {
    end--;
  }
  std::string result;
  result.reserve(end - start + 1);
  bool inSpace = false;
  for (size_t i = start; i <= end; i++) {
    if (isWhitespace(str[i])) {
      if (!inSpace) {
        result.push_back(' ');
        inSpace = true;
      }
    } else {
      result.push_back(str[i]);
      inSpace = false;
    }
  }
  return result;
}

bool matches(const char* tag_name, const char* const* possible_tags, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

bool isNonVisibleTextTag(const char* name) { return VisibleTextUtils::isNonVisibleElement(name); }

const char* getAttribute(const XML_Char** atts, const char* attrName) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) return atts[i + 1];
  }
  return nullptr;
}

uint16_t parseTableSpan(const char* value) {
  if (!value || value[0] == '\0') return 1;

  uint32_t span = 0;
  for (const char* current = value; *current != '\0'; ++current) {
    if (*current < '0' || *current > '9') return 1;
    const uint32_t digit = static_cast<uint32_t>(*current - '0');
    if (span > (UINT16_MAX - digit) / 10) return UINT16_MAX;
    span = span * 10 + digit;
  }
  return span == 0 ? UINT16_MAX : static_cast<uint16_t>(span);
}

// Returns true if the HTML element is a purely inline, non-navigable wrapper.
// IDs on these elements are never meaningful navigation targets in epub content.
// Reading-system converters (Kobo KePub, Calibre, etc.) frequently inject thousands
// of such IDs for progress tracking or internal bookkeeping, and recording each one
// as a navigation anchor exhausts the heap on memory-constrained devices.
// Block-level, sectioning, and structural elements are always considered navigable.
bool isNonNavigableInlineElement(const char* name) { return strcmp(name, "span") == 0; }

bool isInternalEpubLink(const char* href) {
  if (!href || href[0] == '\0') return false;
  if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) return false;
  if (strncmp(href, "mailto:", 7) == 0) return false;
  if (strncmp(href, "ftp://", 6) == 0) return false;
  if (strncmp(href, "tel:", 4) == 0) return false;
  if (strncmp(href, "javascript:", 11) == 0) return false;
  return true;
}

// 取 href 的文件名部分（`text/part0008.html#m7` → `part0008.html`，`#x` → 空串）。
// 目录层级一律剥掉、URL 转义还原、大小写不敏感：href 是相对本章文件写的，而 filepath
// 是相对 zip 根的，两边的目录前缀对不上很正常（`../text/a.html` vs `OEBPS/text/a.html`）。
// 归一化文件名：剥掉目录前缀 + %XX 转义还原（带空格/中文的文件名常被转义，还原不成原样保留）
// + 大小写不敏感。两边都过这一道，`../text/a.html` 和 `OEBPS/text/a.html` 才比得起来。
static std::string normalizeFileName(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
  std::string out;
  out.reserve(name.size());
  for (size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (c == '%' && i + 2 < name.size() && isxdigit((unsigned char)name[i + 1]) &&
        isxdigit((unsigned char)name[i + 2])) {
      const std::string hex = name.substr(i + 1, 2);
      out.push_back(static_cast<char>(strtol(hex.c_str(), nullptr, 16)));
      i += 2;
    } else {
      out.push_back(c);
    }
  }
  for (char& c : out) c = static_cast<char>(tolower((unsigned char)c));
  return out;
}

static std::string hrefFileNameOf(const char* href) {
  if (!href) return std::string();
  const char* hash = strchr(href, '#');
  return normalizeFileName(std::string(href, hash ? static_cast<size_t>(hash - href) : strlen(href)));
}

// 纯页内锚点（`#noteref_1`）或**指向本章文件自己**的锚点（`part0008.html#m7`）→ 片段名。
// 指向别的文件的（`notes.xhtml#fn1`）返回空串：拿去跟本章的注号锚点比会张冠李戴。
//
// ⚠ 只认裸 `#` 是不够的：古典柏拉图主义哲学导论这一类的书（calibre 转换）**每一条内链
// 都带自己的文件名** —— 正文注号是 `<a id="w7"></a><a href="part0008.html#m7">`，注文
// 行首的回引是 `<a id="m7"></a><a href="part0008.html#w7">`。两条都因为"带文件名"被判成
// 跨文件 → 回引识别整个失效 → 注文的回引也被登记成脚注条目，注文那一页的脚注表里躺着
// 一排指回正文的假条目（点上去弹的是正文），而正文注号那一条反倒要靠锚点兜底跳转。
// 判据换成"文件名是不是本章这一份"，两种写法就都归位了。
std::string localAnchorOf(const char* href, const std::string& currentFilePath) {
  if (!href || href[0] == '\0') return std::string();
  const char* hash = strchr(href, '#');
  if (!hash) return std::string();
  const std::string file = hrefFileNameOf(href);
  if (!file.empty() && file != normalizeFileName(currentFilePath)) return std::string();
  return std::string(hash + 1);
}

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) || matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS));
}

bool isTableStructuralTag(const char* name) {
  return strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 || strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
}

void ChapterHtmlSlimParser::applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasDirection()) {
    entry.hasDirection = true;
    entry.direction = css.direction;
  }
}

EpdFontFamily::Style ChapterHtmlSlimParser::fontStyleForTextDecoration(const CssTextDecoration decoration) {
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  const bool underline = (decoration & CssTextDecoration::Underline) != CssTextDecoration::None;
  const bool wavy = (decoration & CssTextDecoration::Wavy) != CssTextDecoration::None;
  if (underline) {
    // Wavy 本身不画线，只是把线换成波浪笔法；单独一个 Wavy（没有 Underline）到此为止，
    // 与 `text-decoration-style: wavy` 后面没跟线的写法一致（见 CssStyle.h）。
    style = static_cast<EpdFontFamily::Style>(
        style | (wavy ? EpdFontFamily::WAVY_UNDERLINE : EpdFontFamily::UNDERLINE));
  }
  if ((decoration & CssTextDecoration::LineThrough) != CssTextDecoration::None) {
    style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::STRIKETHROUGH);
  }
  return style;
}

void ChapterHtmlSlimParser::applyTextDecorationToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasTextDecoration()) {
    entry.hasTextDecoration = true;
    entry.textDecoration = css.textDecoration;
  }
}

void ChapterHtmlSlimParser::applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (!css.hasVerticalAlign()) return;
  if (css.verticalAlign == CssVerticalAlign::Super) {
    entry.hasSup = true;
    entry.sup = true;
  } else if (css.verticalAlign == CssVerticalAlign::Sub) {
    entry.hasSub = true;
    entry.sub = true;
  }
}

// 这条 CSS 把文字交给了**哪个家族面**（书内 CSS 的第二 / 第三家族）。阅读器没开某个
// 家族面时它的哈希是 0，而 0 不是任何家族名的哈希、也不能等于 css.fontFamilyHash()
// （hash 非 0 才登记），所以恒不命中：关闭态下与"没有这条路径"逐像素等价。
//
// 判定顺序 = 优先级：同一段同时命中两个哈希（书里两个家族名指到同一个哈希时会这样）
// 时归**次家族**。两个哈希相等本来就该由调用方去重（见 Epub::resolveEmbeddedFonts
// 的挑选规则），这里只是兜底，保证"每个词只带一位"。
EpdFontFamily::Style ChapterHtmlSlimParser::familyBitOf(const CssStyle& css) const {
  if (!css.hasFontFamily()) return EpdFontFamily::REGULAR;
  const uint32_t altFamily = renderer.altFontFamilyHash();
  if (altFamily != 0 && css.fontFamilyHash == altFamily) return EpdFontFamily::ALT_FONT;
  const uint32_t alt2Family = renderer.alt2FontFamilyHash();
  if (alt2Family != 0 && css.fontFamilyHash == alt2Family) return EpdFontFamily::ALT2_FONT;
  return EpdFontFamily::REGULAR;
}

void ChapterHtmlSlimParser::applyFamilyBitToEntry(StyleStackEntry& entry, const CssStyle& css) {
  const EpdFontFamily::Style bit = familyBitOf(css);
  if (bit != EpdFontFamily::REGULAR) {
    entry.hasFamilyBit = true;
    entry.familyBit = bit;
  }
}

void ChapterHtmlSlimParser::pushTableTextStyleEntry(const CssStyle& cssStyle) {
  if (!cssStyle.hasFontWeight() && !cssStyle.hasFontStyle() && !cssStyle.hasTextDecoration() &&
      !cssStyle.hasDirection() && !cssStyle.hasTextAlign()) {
    return;
  }

  StyleStackEntry entry;
  entry.depth = depth;
  if (cssStyle.hasFontWeight()) {
    entry.hasBold = true;
    entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
  }
  if (cssStyle.hasFontStyle()) {
    entry.hasItalic = true;
    entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
  }
  applyTextDecorationToEntry(entry, cssStyle);
  applyDirectionToEntry(entry, cssStyle);
  applyFamilyBitToEntry(entry, cssStyle);
  entry.setsParagraphDirection = true;
  if (cssStyle.hasTextAlign()) {
    entry.hasTextAlign = true;
    entry.textAlign = cssStyle.textAlign;
  }
  inlineStyleStack.push_back(entry);
  updateEffectiveInlineStyle();
}

void ChapterHtmlSlimParser::pushDecorationStyleEntry(const CssTextDecoration defaultDecoration,
                                                     const CssStyle& cssStyle) {
  StyleStackEntry entry;
  entry.depth = depth;
  entry.hasTextDecoration = true;
  entry.textDecoration = cssStyle.hasTextDecoration() ? cssStyle.textDecoration : defaultDecoration;
  if (cssStyle.hasFontWeight()) {
    entry.hasBold = true;
    entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
  }
  if (cssStyle.hasFontStyle()) {
    entry.hasItalic = true;
    entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
  }
  applyDirectionToEntry(entry, cssStyle);
  applyFamilyBitToEntry(entry, cssStyle);
  inlineStyleStack.push_back(entry);
  updateEffectiveInlineStyle();
}

// Update effective bold/italic/decorations based on block style and inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveTextDecoration =
      currentCssStyle.hasTextDecoration() ? currentCssStyle.textDecoration : CssTextDecoration::None;
  bool paragraphDirectionDefined = false;
  bool paragraphIsRtl = false;
  if (!blockStyleStack.empty()) {
    const auto& blockStyle = blockStyleStack.back();
    paragraphDirectionDefined = blockStyle.directionDefined;
    paragraphIsRtl = blockStyle.isRtl;
  }
  effectiveDirectionDefined = paragraphDirectionDefined;
  effectiveDirection = paragraphIsRtl ? CssTextDirection::Rtl : CssTextDirection::Ltr;
  effectiveTextAlignDefined = currentCssStyle.hasTextAlign();
  effectiveTextAlign = currentCssStyle.textAlign;
  effectiveSup = false;
  effectiveSub = false;
  // 家族面：跟 bold 同一个来源（当前块的 CSS），比的是家族哈希。
  effectiveFamily = familyBitOf(currentCssStyle);

  // Apply inline style stack in order
  for (const auto& entry : inlineStyleStack) {
    if (entry.hasBold) {
      effectiveBold = entry.bold;
    }
    if (entry.hasItalic) {
      effectiveItalic = entry.italic;
    }
    // CSS line decorations propagate through descendants; child entries add
    // their own lines but cannot cancel an ancestor's already active line.
    if (entry.hasTextDecoration) {
      effectiveTextDecoration = effectiveTextDecoration | entry.textDecoration;
    }
    if (entry.hasDirection) {
      effectiveDirectionDefined = true;
      effectiveDirection = entry.direction;
      if (entry.setsParagraphDirection) {
        paragraphDirectionDefined = true;
        paragraphIsRtl = entry.direction == CssTextDirection::Rtl;
      }
    }
    if (entry.hasTextAlign) {
      effectiveTextAlignDefined = true;
      effectiveTextAlign = entry.textAlign;
    }
    if (entry.hasSup) {
      effectiveSup = entry.sup;
      if (entry.sup) effectiveSub = false;
    }
    if (entry.hasSub) {
      effectiveSub = entry.sub;
      if (entry.sub) effectiveSup = false;
    }
    if (entry.hasFamilyBit) {
      effectiveFamily = entry.familyBit;
    }
  }

  // Keep flow direction in the active empty text block. Inline direction remains
  // available for CSS inheritance without replacing the paragraph's base direction.
  if (currentTextBlock && currentTextBlock->isEmpty()) {
    auto& style = currentTextBlock->getBlockStyle();
    style.directionDefined = paragraphDirectionDefined;
    style.isRtl = paragraphIsRtl;
  }
}

void ChapterHtmlSlimParser::armInlineAnchor(const std::string& id, const char* elementName) {
  // 只有 `<a>` 是"行内、可导航"的边界情况。别的都走延后那条路：
  //   · `<p id=…>` / `<li id=…>` 这些块元素：延后到 startNewTextBlock 记，本来就准；
  //   · `<sup id=…>`（晋书）：不在块边界上，但现网一直这么记，读者侧的启发式也照它
  //     调过，不动它；
  //   · `<span id=…>`：在进这个函数之前就被 isNonNavigableInlineElement 挡掉了。
  if (elementName == nullptr || std::strcmp(elementName, "a") != 0) return;
  if (!currentTextBlock) return;
  // 上限只是防病态输入（一个 <p> 里挂几千个 id）。真到上限就退回延后那条路，宁可贵一点。
  if (inlineAnchorArms.size() >= 64) return;
  inlineAnchorArms.push_back(
      {currentTextBlock.get(), static_cast<int>(currentTextBlock->size()), id, partWordBufferIndex}
  );
}

void ChapterHtmlSlimParser::resolveInlineAnchors(TextBlock* line) {
  if (inlineAnchorArms.empty() || line == nullptr) return;
  const int lineEnd = wordsExtractedInBlock + static_cast<int>(line->wordCount());
  for (size_t i = 0; i < inlineAnchorArms.size();) {
    const InlineAnchorArm& arm = inlineAnchorArms[i];
    // 只结算**当前这一块**的：别的块要么还没排，要么是同一块更后面的锚点。
    // 同一条锚点只结算一次，结算完立刻从表里摘掉。
    if (arm.block == currentTextBlock.get() && arm.wordOffset < lineEnd) {
      anchorData.push_back({arm.id, static_cast<uint16_t>(completedPageCount),
                            static_cast<uint16_t>(currentPage ? currentPage->elements.size() : 0)});
      inlineAnchorArms.erase(inlineAnchorArms.begin() + static_cast<long>(i));
      continue;
    }
    ++i;
  }
}

void ChapterHtmlSlimParser::flushPendingAnchor() {
  if (hasFailed()) return;
  if (pendingAnchorId.empty()) return;

  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  if (std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
    if (currentPage && !currentPage->elements.empty()) {
      completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
      if (hasFailed()) return;
      completedPageCount++;
      if (!allocatePage()) return;
    }
  }

  // Record deferred anchor after previous block is flushed (and any TOC page break)
  anchorData.push_back(
      {std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount),
       static_cast<uint16_t>(currentPage ? currentPage->elements.size() : 0)}
  );
  pendingAnchorId.clear();
}

bool ChapterHtmlSlimParser::allocatePage() {
  if (hasFailed()) return false;
  auto page = makeUniqueNoThrow<Page>();
  if (!page) {
    LOG_ERR("EHP", "OOM: Page (%u bytes)", static_cast<unsigned>(sizeof(Page)));
    failAllocation("page layout");
    return false;
  }
  currentPage = std::move(page);
  currentPageNextY = 0;
  currentPageVisibleOffsetSet = false;
  // 上一页的最后一行记录作废。光靠 `lastLinePage == currentPage.get()` 比指针不够：
  // 上一页被 completePageFn 交出去后 currentPage 置空，但它那块内存会被下一页重新
  // 分配回来——地址一样，于是"这一行属于当前页"会误判成真，行内图片就按上一页的
  // y 落位。换页时显式清掉，指针相等才算数。
  lastLinePage = nullptr;
  return true;
}

void ChapterHtmlSlimParser::setCurrentPageVisibleOffset(const uint32_t offset) {
  if (currentPageVisibleOffsetSet) return;
  // The first page always begins at the start of the body, even when the XHTML
  // contains leading formatting whitespace before its first rendered word.
  currentPageVisibleOffset = completedPageCount == 0 ? 0 : offset;
  currentPageVisibleOffsetSet = true;
}

// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  if (hasFailed()) return;
  if (!currentTextBlock) {
    partWordBufferIndex = 0;
    nextWordContinues = false;
    return;
  }

  // Determine font style from depth-based tracking and CSS effective style
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | fontStyleForTextDecoration(effectiveTextDecoration));
  if (effectiveSup) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUP);
  } else if (effectiveSub) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUB);
  }
  // 家族面：只换画哪个字面，不动字号也不加装饰，所以跟上面几位正交。effectiveFamily
  // 本身就是那一位（0 / ALT_FONT / ALT2_FONT），"或"进去即可，不必再判。
  fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | effectiveFamily);

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  const size_t wordBytes = static_cast<size_t>(partWordBufferIndex);
  if (insideTableCell && !tableRowStacked && tableCellTextBytes + wordBytes > MAX_GRID_TABLE_CELL_BYTES) {
    fallbackTableRowToStacked();
    if (hasFailed()) return;
  }

  uint8_t linkId = 0;
  if (collectTouchLinks && insideFootnoteLink) {
    if (!currentTextBlock->linkTargetMatches(currentFootnoteLinkId, currentFootnote.href.c_str())) {
      currentFootnoteLinkId = currentTextBlock->addLinkTarget(currentFootnote.href.c_str());
    }
    linkId = currentFootnoteLinkId;
  } else if (collectTouchLinks && syntheticMarkerLinkId != 0) {
    // 合成注号：不是 <a> 包着的，但照样给它挂一个链接矩形，href 是私有 scheme
    // "fn:<序号>"。读端按这个 id 直取脚注，不必拿被点中的字形去比号码串。
    linkId = syntheticMarkerLinkId;
  }
  currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues, partWordVisibleOffset, linkId);
  if (insideTableCell && !tableRowStacked) {
    tableCellTextBytes += wordBytes;
    if (currentTextBlock->size() > MAX_GRID_TABLE_CELL_WORDS) {
      fallbackTableRowToStacked();
      if (hasFailed()) return;
    }
  }
  partWordBufferIndex = 0;
  nextWordContinues = false;
  listItemBulletOnly = false;
}

// start a new text block if needed
int ChapterHtmlSlimParser::resolvedFontId(const BlockStyle& blockStyle) const {
  if (!blockStyle.fontScaleDefined) return fontId;
  const int id = renderer.cssFontId(fontId, blockStyle.fontScale);
  // 临时诊断（字号锚定）：把"CSS 倍率 → 落到哪一档 id"打出来。同一章只打前 12 次且
  // 同一个倍率只打一次（书里一个 font-size 会重复出现几百次，全打会淹掉串口）。
  {
    static float seen[16];
    static int seenN = 0, logged = 0;
    bool dup = false;
    for (int i = 0; i < seenN; i++)
      if (seen[i] == blockStyle.fontScale) { dup = true; break; }
    if (!dup && logged < 12) {
      if (seenN < 16) seen[seenN++] = blockStyle.fontScale;
      logged++;
      LOG_INF("EHP", "字号锚定: 倍率 %.3f → id %d (正文 id %d)", (double)blockStyle.fontScale, id, fontId);
    }
  }
  return id;
}

void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  if (hasFailed()) return;
  nextWordContinues = false;  // New block = new paragraph, no continuation
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      // The stack accumulates horizontal margins and text properties from ancestors.
      // Vertical margins are per-element and not inherited through the stack, but
      // container elements deposit their vertical margins on the empty block when they
      // open. Merge those into the new style so the first child in a container inherits
      // the container's vertical spacing.
      const auto style = currentTextBlock->getBlockStyle();
      BlockStyle incoming = blockStyle;
      if (style.fromBrElement) {
        // The empty block was created by a <br> section separator. Inject a full line of
        // blank space before the following paragraph so the scene/section break is visible.
        // This only fires when the <br> block stayed empty (i.e. no inline text was added).
        const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(currentFontId, lineCompression));
        incoming.marginTop = static_cast<int16_t>(incoming.marginTop + lineHeight);
      }

      currentTextBlock->setBlockStyle(style.getCombinedBlockStyle(incoming, BlockStyle::CombineAxis::Vertical));

      flushPendingAnchor();
      return;
    }

    // <li> added a bullet as the first word, making the block non-empty. When a nested
    // block-level child (<p>, <div>, etc.) opens, reuse the block instead of flushing
    // the bullet to its own line. The bullet stays inline with the child's text.
    if (listItemBulletOnly) {
      const auto style = currentTextBlock->getBlockStyle();
      currentTextBlock->setBlockStyle(style.getCombinedBlockStyle(blockStyle, BlockStyle::CombineAxis::Vertical));
      listItemBulletOnly = false;
      flushPendingAnchor();
      return;
    }

    makePages();
    if (hasFailed()) return;
  }
  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  flushPendingAnchor();
  if (hasFailed()) return;
  // 上一个块刚刚在 makePages() 里排完，它的行内锚点该结算的都结算了 —— 剩下的都是排不到
  // 的（比如 id 落在被裁掉的内容里）。一并清掉：换块之后指针可能被复用，留着会张冠李戴。
  inlineAnchorArms.clear();
  currentTextBlock = makeUniqueNoThrow<ParsedText>(extraParagraphSpacing, firstLineIndent, hyphenationEnabled,
                                                   focusReadingEnabled, blockStyle, collectTouchLinks);
  if (!currentTextBlock) {
    LOG_ERR("EHP", "OOM: ParsedText (%u bytes)", static_cast<unsigned>(sizeof(ParsedText)));
    failAllocation("page layout");
    return;
  }
  wordsExtractedInBlock = 0;
  listItemBulletOnly = false;
}

void ChapterHtmlSlimParser::emitHorizontalRule(const BlockStyle& blockStyle) {
  if (hasFailed()) return;
  if (partWordBufferIndex > 0) {
    flushPartWordBuffer();
    if (hasFailed()) return;
  }

  if (currentTextBlock) {
    const BlockStyle parentBlockStyle = currentTextBlock->getBlockStyle();
    startNewTextBlock(parentBlockStyle);
    if (hasFailed()) return;
  }

  if (!currentPage) {
    if (!allocatePage()) return;
  }

  const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(currentFontId, lineCompression));
  const int16_t defaultVerticalSpacing = static_cast<int16_t>(lineHeight / 2);
  const int16_t topSpacing =
      static_cast<int16_t>((blockStyle.marginTop > 0 ? blockStyle.marginTop : defaultVerticalSpacing) +
                           (blockStyle.paddingTop > 0 ? blockStyle.paddingTop : 0));
  const int16_t bottomSpacing =
      static_cast<int16_t>((blockStyle.marginBottom > 0 ? blockStyle.marginBottom : defaultVerticalSpacing) +
                           (blockStyle.paddingBottom > 0 ? blockStyle.paddingBottom : 0));
  constexpr uint8_t ruleThickness = 2;
  const int16_t availableWidth =
      std::max<int16_t>(1, static_cast<int16_t>(viewportWidth - blockStyle.totalHorizontalInset()));
  const int16_t width = std::max<int16_t>(1, static_cast<int16_t>(availableWidth / 4));
  const int16_t xPos = static_cast<int16_t>(blockStyle.leftInset() + ((availableWidth - width) / 2));
  const int16_t totalHeight = static_cast<int16_t>(topSpacing + ruleThickness + bottomSpacing);

  if (!currentPage->elements.empty() && currentPageNextY + totalHeight > viewportHeight) {
    setCurrentPageVisibleOffset(visibleTextOffset);
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
    if (hasFailed()) return;
    completedPageCount++;
    if (!allocatePage()) return;
  }

  currentPageNextY += topSpacing;

  auto pageRule = makeUniqueNoThrow<PageHorizontalRule>(width, ruleThickness, xPos, currentPageNextY);
  if (!pageRule) {
    LOG_ERR("EHP", "Failed to create PageHorizontalRule");
    failAllocation("PageHorizontalRule");
    return;
  }
  currentPage->elements.push_back(std::move(pageRule));
  setCurrentPageVisibleOffset(visibleTextOffset);
  currentPageNextY = static_cast<int16_t>(currentPageNextY + ruleThickness + bottomSpacing);

  if (!pendingAnchorId.empty()) {
    // 锚在这儿的是**这个横线元素自己**（<div id=…><hr/></div> 那种），不是"下一个块"，
    // 所以元素序号指回刚推入的那一条；越界与否由读端夹。
    anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount),
                          static_cast<uint16_t>(currentPage->elements.empty()
                                                    ? 0
                                                    : currentPage->elements.size() - 1)});
    pendingAnchorId.clear();
  }
}

void ChapterHtmlSlimParser::fallbackTableRowToStacked() {
  if (hasFailed()) return;
  if (tableRowStacked) {
    return;
  }

  auto activeCell = std::move(currentTextBlock);
  tableRowStacked = true;

  for (auto& cell : tableRowCells) {
    currentTextBlock = std::move(cell);
    wordsExtractedInBlock = 0;
    if (currentTextBlock && !currentTextBlock->isEmpty()) {
      makePages();
      if (hasFailed()) return;
    }
  }
  tableRowCells.clear();
  currentTextBlock = std::move(activeCell);
  wordsExtractedInBlock = 0;
}

void ChapterHtmlSlimParser::closeTableCell() {
  if (hasFailed()) return;
  if (!insideTableCell) {
    return;
  }
  insideTableCell = false;

  if (!currentTextBlock) {
    return;
  }

  if (!tableRowStacked &&
      (tableRowCells.size() >= MAX_GRID_TABLE_COLUMNS || currentTextBlock->size() > MAX_GRID_TABLE_CELL_WORDS)) {
    fallbackTableRowToStacked();
    if (hasFailed()) return;
  }

  if (tableRowStacked) {
    wordsExtractedInBlock = 0;
    if (!currentTextBlock->isEmpty()) {
      makePages();
      if (hasFailed()) return;
    }
    currentTextBlock.reset();
    return;
  }

  tableRowCells.push_back(std::move(currentTextBlock));
}

void ChapterHtmlSlimParser::addTableRowSeparator() {
  if (hasFailed()) return;
  if (!currentPage || currentPage->elements.empty() || viewportWidth == 0 ||
      currentPageNextY + TABLE_ROW_SEPARATOR_GAP > viewportHeight) {
    return;
  }

  auto separator =
      makeUniqueNoThrow<PageHorizontalRule>(viewportWidth, TABLE_ROW_SEPARATOR_THICKNESS, 0, currentPageNextY + 1);
  if (!separator) {
    LOG_ERR("EHP", "OOM: table row separator");
    failAllocation("table row separator");
    return;
  }
  if (currentPage->elements.capacity() == currentPage->elements.size()) {
    currentPage->elements.reserve(currentPage->elements.size() + 1);
  }
  currentPage->elements.push_back(std::move(separator));
  currentPageNextY += TABLE_ROW_SEPARATOR_GAP;
}

void ChapterHtmlSlimParser::finishTableRow() {
  if (hasFailed()) return;
  closeTableCell();
  if (hasFailed()) return;

  if (tableRowCells.empty()) {
    if (tableRowStacked) {
      addTableRowSeparator();
      if (hasFailed()) return;
    }
    tableRowStacked = false;
    return;
  }

  const int16_t lineHeight =
      std::max<int16_t>(1, static_cast<int16_t>(renderer.getLineHeight(currentFontId) * lineCompression));
  const size_t columnCount = tableRowCells.size();
  const uint16_t cellWidth = static_cast<uint16_t>(viewportWidth / columnCount);

  // Keep enough width for a few glyphs while allowing ordinary three-column
  // tables to remain tabular at the default font size in portrait.
  if (columnCount < 2 || cellWidth <= TABLE_CELL_HORIZONTAL_PADDING * 2 ||
      cellWidth < lineHeight * TABLE_MIN_CELL_WIDTH_LINE_HEIGHTS) {
    fallbackTableRowToStacked();
    if (hasFailed()) return;
    addTableRowSeparator();
    if (hasFailed()) return;
    tableRowStacked = false;
    return;
  }

  const uint16_t textWidth = static_cast<uint16_t>(cellWidth - TABLE_CELL_HORIZONTAL_PADDING * 2);
  for (auto& lines : tableCellLines) {
    lines.clear();
  }
  tableLineVisibleOffsets.clear();
  if (tableLineVisibleOffsets.capacity() < MAX_GRID_TABLE_CELL_WORDS * 2) {
    tableLineVisibleOffsets.reserve(MAX_GRID_TABLE_CELL_WORDS * 2);
  }
  size_t maxLineCount = 0;
  const bool rowRtl = tableRowRtl;

  for (size_t column = 0; column < columnCount; ++column) {
    auto& lines = tableCellLines[column];
    // Two wrapped lines per buffered word avoids normal vector growth (max 64).
    if (lines.capacity() < MAX_GRID_TABLE_CELL_WORDS * 2) {
      lines.reserve(MAX_GRID_TABLE_CELL_WORDS * 2);
    }
    if (!tableRowCells[column]->layoutAndExtractLines(
            renderer, currentFontId, textWidth, [this, &lines](std::unique_ptr<TextBlock> line, const uint32_t offset) {
              const size_t lineIndex = lines.size();
              lines.push_back(std::move(line));
              if (tableLineVisibleOffsets.size() <= lineIndex) {
                tableLineVisibleOffsets.resize(lineIndex + 1, UINT32_MAX);
              }
              tableLineVisibleOffsets[lineIndex] = std::min(tableLineVisibleOffsets[lineIndex], offset);
              return true;
            })) {
      failAllocation("table layout");
      return;
    }
    maxLineCount = std::max(maxLineCount, lines.size());
  }
  tableRowCells.clear();
  const auto clearLayoutLines = [this]() {
    for (auto& lines : tableCellLines) {
      lines.clear();
    }
    tableLineVisibleOffsets.clear();
  };

  for (size_t lineIndex = 0; lineIndex < maxLineCount; ++lineIndex) {
    const uint32_t lineVisibleOffset =
        lineIndex < tableLineVisibleOffsets.size() ? tableLineVisibleOffsets[lineIndex] : visibleTextOffset;
    int16_t rowLineHeight = lineHeight;
    for (size_t column = 0; column < columnCount; ++column) {
      if (lineIndex < tableCellLines[column].size()) {
        rowLineHeight = std::max<int16_t>(
            rowLineHeight, static_cast<int16_t>(lineHeight + tableCellLines[column][lineIndex]->getRubyShift(
                                                                 renderer.getFontAscenderSize(currentFontId))));
      }
    }

    const bool pageFull =
        currentPage && !currentPage->elements.empty() && currentPageNextY + rowLineHeight > viewportHeight;
    if (!currentPage || pageFull) {
      if (pageFull) {
        setCurrentPageVisibleOffset(lineVisibleOffset);
        completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
        if (hasFailed()) return;
        completedPageCount++;
      }
      if (!allocatePage()) {
        LOG_ERR("EHP", "OOM: page for table row");
        clearLayoutLines();
        return;
      }
      currentPageNextY = 0;
      currentPageVisibleOffsetSet = false;
    }

    const int16_t rowY = currentPageNextY;
    const size_t requiredCapacity = currentPage->elements.size() + columnCount;
    if (currentPage->elements.capacity() < requiredCapacity) {
      const size_t linesThatFit =
          std::max<size_t>(1, static_cast<size_t>((viewportHeight - currentPageNextY) / rowLineHeight));
      const size_t linesToReserve = std::min(maxLineCount - lineIndex, linesThatFit);
      currentPage->elements.reserve(currentPage->elements.size() + linesToReserve * columnCount + 1);
    }
    for (size_t column = 0; column < columnCount; ++column) {
      if (lineIndex >= tableCellLines[column].size()) {
        continue;
      }

      auto& line = tableCellLines[column][lineIndex];
      auto style = line->getBlockStyle();
      const size_t physicalColumn = rowRtl ? columnCount - column - 1 : column;
      style.marginLeft = static_cast<int16_t>(physicalColumn * cellWidth + TABLE_CELL_HORIZONTAL_PADDING);
      style.paddingLeft = 0;
      line->setBlockStyle(style);

      // Reset Y so every cell in this slice shares one baseline.
      currentPageNextY = rowY;
      if (!addLineToPage(std::move(line), lineVisibleOffset)) return;
    }
    currentPageNextY = static_cast<int16_t>(rowY + rowLineHeight);
  }

  addTableRowSeparator();
  if (hasFailed()) return;
  tableRowStacked = false;
  clearLayoutLines();
}

// 「这张图小得不像插图吗」——只读图头，结果按 src 记账（见 .h 的说明）。
// 探不出尺寸（坏图、不认识的格式）一律算"不小"：宁可退回整块插图那条老路，也不要
// 把一张没读懂的图换成上标数字、把图本身弄丢。
bool ChapterHtmlSlimParser::isTinyMarkerImage(const std::string& resolvedSrc) {
  for (const auto& cached : tinyImageProbes) {
    if (cached.src == resolvedSrc) return cached.tiny;
  }
  bool tiny = false;
  if (epub && !resolvedSrc.empty()) {
    ImageDimsProbe probe;
    epub->readItemContentsToStream(resolvedSrc, probe, 1024, /*allowEarlyStop=*/true);
    ImageDimensions dims = {0, 0};
    if (probe.getDimensions(dims))
      tiny = dims.width > 0 && dims.height > 0 && dims.width <= FOOTNOTE_MARKER_MAX_PX &&
             dims.height <= FOOTNOTE_MARKER_MAX_PX;
  }
  // 记账上限只是防病态输入把内存吃干：正常一章里"行内小图 + alt"就那么几种 src
  // （斐洞整章 813 枚注号共用同一张 img001.png）。超了不再记账，那些 src 每次都会重探。
  if (tinyImageProbes.size() < 64) tinyImageProbes.push_back({resolvedSrc, tiny});
  return tiny;
}

// 几条判据（QQ 阅读器的 class 名、Duokan 的脚注内链、"裸 alt + 小图"）认出来之后走的是
// 同一段：不发图，改发一个上标序号词，注文整篇塞进 FootnoteEntry.href 的 "alt:" 哨兵前缀
// （这样不用动 Page 的缓存结构；href 已经是变长的 std::string，多长的注文都放得下）。
void ChapterHtmlSlimParser::appendAltFootnoteMarker(const std::string& altText) {
  if (partWordBufferIndex > 0) {
    flushPartWordBuffer();
    if (hasFailed()) return;
  }
  // 标记文字就是本条注的顺序号；push 一个 SUP 内联样式，让它以上标形态画出来。
  // 序号必须**本页唯一**：点注那套（rdTapOnFootnote）是拿按中的词去比**本页**注号列表的，
  // 重号的话点哪条都只会弹第一条。见 nextFootnoteMarker 的说明。
  const std::string marker = nextFootnoteMarker();
  StyleStackEntry supEntry;
  supEntry.depth = depth;
  supEntry.hasSup = true;
  supEntry.sup = true;
  inlineStyleStack.push_back(supEntry);
  updateEffectiveInlineStyle();

  // 给这颗注号字挂一条**私有链接**：href = "fn:<序号>"，序号与下面 FootnoteEntry.number
  // **同源**（都是 nextFootnoteMarker 发的那个号）。于是"上标字 → 脚注条目"是解析时就
  // 接好的一条边，读端点中这个矩形就顺着 id 直取，不用再拿被点中的字形去比号码串
  // （那套比对是"8 弹到 86"的根源，见 screen_reader.cpp::rdTapOnLink）。
  // 顺带解决"注号只有 1/3 字宽、手指点不中"：链接矩形走的是同一套放宽后的触摸容差。
  if (collectTouchLinks && currentTextBlock) {
    const std::string markerHref = "fn:" + marker;
    syntheticMarkerLinkId = currentTextBlock->addLinkTarget(markerHref.c_str());
  }

  syntheticCharacterData = true;
  characterData(this, marker.c_str(), marker.length());
  syntheticCharacterData = false;
  // 立刻落成词：linkId 只该跟着注号这一颗字，不能漏给后面的正文。
  if (syntheticMarkerLinkId != 0) {
    if (partWordBufferIndex > 0) flushPartWordBuffer();
    syntheticMarkerLinkId = 0;
  }
  inlineStyleStack.pop_back();
  updateEffectiveInlineStyle();
  if (hasFailed()) return;

  FootnoteEntry entry;
  strncpy(entry.number, marker.c_str(), sizeof(entry.number) - 1);
  entry.number[sizeof(entry.number) - 1] = '\0';
  entry.href = "alt:" + altText;
  const int wordIndex =
      wordsExtractedInBlock + (currentTextBlock ? static_cast<int>(currentTextBlock->size()) : 0);
  pendingFootnotes.push_back({wordIndex, entry});
  nextWordContinues = false;
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (!self->checkMemory()) return;
  if (strcasecmp(name, "body") == 0) {
    // Case-insensitive to match ParagraphStreamer's tag matching (ProgressMapper). A case
    // mismatch here would leave visibleTextOffset at 0 for the whole section, so every page
    // would record offset 0 while the sync resolver still counts a non-zero offset.
    self->insideBody = true;
  }
  if (self->insideBody && (self->nonVisibleTextDepth > 0 || isNonVisibleTextTag(name))) {
    self->nonVisibleTextDepth++;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  if (strcmp(name, "p") == 0) {
    self->xpathParagraphIndex++;
  }
  if (strcmp(name, "li") == 0) {
    self->xpathListItemIndex++;
  }

  // Extract class, style, id, and dir attributes for CSS/RTL processing
  std::string classAttr;
  std::string styleAttr;
  std::string dirAttr;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (self->cssParser && strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (self->cssParser && strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        // Defer both anchor recording and TOC page breaks until startNewTextBlock,
        // after the previous block is flushed to pages via makePages().
        //
        // Skip IDs on non-navigable inline elements (e.g. <span>): these are never
        // link targets in epub content, but reading-system converters can inject tens
        // of thousands of them per chapter, exhausting the heap. TOC anchors are
        // always recorded regardless of element type, since they drive page breaks.
        const char* idValue = atts[i + 1];
        const bool isTocAnchor =
            std::find(self->tocAnchors.begin(), self->tocAnchors.end(), idValue) != self->tocAnchors.end();
        if (isTocAnchor || (!isNonNavigableInlineElement(name) && self->anchorData.size() < MAX_ANCHORS_PER_CHAPTER)) {
          // Flush a displaced anchor before overwriting. Consecutive non-block elements
          // (e.g. <aside id="fn1">text</aside><aside id="fn2">) with no intervening block
          // never trigger startNewTextBlock, so fn1 gets silently overwritten. That leaves
          // fn1 missing from the anchor map -> getPageForAnchor returns nullopt -> reader
          // lands at page 0 (section start) instead of the footnote.
          if (!self->pendingAnchorId.empty()) {
            self->flushPendingAnchor();
            if (self->hasFailed()) return;
          }
          // id 挂在**行内**的 <a> 上（`<p><a id="10" href="#7">〔一〕</a>太阿：…`，
          // 趙州録校注整章都是这个形状）时不走 pendingAnchorId，改记"本块第几个词"，
          // 等这块排版时精确换算成页内元素序号。理由见 armInlineAnchor 的注释。
          // TOC 锚点例外：它要的是"另起一页"那套，必须留在延后那条路上。
          if (!isTocAnchor) {
            const size_t before = self->inlineAnchorArms.size();
            self->armInlineAnchor(idValue, name);
            if (self->inlineAnchorArms.size() != before) continue;  // 已按行内锚点收下
          }
          self->pendingAnchorId = idValue;
        }
      } else if (strcmp(atts[i], "dir") == 0) {
        dirAttr = atts[i + 1];
      }
    }
  }

  auto centeredBlockStyle = BlockStyle();
  centeredBlockStyle.textAlignDefined = true;
  centeredBlockStyle.alignment = CssTextAlign::Center;

  // Compute CSS style for this element early so display:none can short-circuit
  // before tag-specific branches emit any content or metadata.
  CssStyle cssStyle;
  if (self->cssParser) {
    cssStyle = self->cssParser->resolveStyle(name, classAttr);
    if (!styleAttr.empty()) {
      CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);
      cssStyle.applyOver(inlineStyle);
    }
  }

  // HTML dir attribute overrides CSS direction (case-insensitive per HTML spec)
  if (!dirAttr.empty()) {
    if (strcasecmp(dirAttr.c_str(), "rtl") == 0) {
      cssStyle.direction = CssTextDirection::Rtl;
      cssStyle.defined.direction = 1;
    } else if (strcasecmp(dirAttr.c_str(), "ltr") == 0) {
      cssStyle.direction = CssTextDirection::Ltr;
      cssStyle.defined.direction = 1;
    }
  }

  // Direction is inherited in HTML/CSS. If this element does not define one, carry
  // the currently active inherited direction into its computed style.
  if (!cssStyle.hasDirection() && self->effectiveDirectionDefined) {
    cssStyle.direction = self->effectiveDirection;
    cssStyle.defined.direction = 1;
  }

  // font-family 在 CSS 里也是继承属性：容器上写了家族（<div class="zhu">）、里面的
  // <p> 自己没写时，浏览器会让 <p> 继承它。引擎没有通用的属性继承（font-weight 等
  // 同样不继承，靠 inlineStyleStack 逐层传），但家族漏了这一条，容器式的多字体书
  // 就完全不生效（祖堂集恰好把 family 写在 <span> 上，走行内那一支，但 <div class="jiazhu">
  // 这种写法在中文 EPUB 里同样常见）。所以照 direction 的办法补一条：
  // 只继承**当前生效的那个家族**（哪一位生效就抄哪个哈希），别的家族继承下来也画不出
  // 区别。抄哈希而不是抄"位"，是因为下游认的是哈希（familyBitOf 再比一次）。
  if (!cssStyle.hasFontFamily() && self->effectiveFamily != EpdFontFamily::REGULAR) {
    cssStyle.fontFamilyHash = (self->effectiveFamily == EpdFontFamily::ALT2_FONT)
                                  ? self->renderer.alt2FontFamilyHash()
                                  : self->renderer.altFontFamilyHash();
    cssStyle.defined.fontFamily = 1;
  }

  // Skip elements with display:none before all fast paths (tables, links, etc.).
  if (cssStyle.hasDisplay() && cssStyle.display == CssDisplay::None) {
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Buffer one simple row; oversized rows fall back to full-width flow.
  if (strcmp(name, "table") == 0) {
    // Flatten nested content without allocating a recursive row buffer.
    if (self->tableDepth > 0) {
      if (self->tableDepth == 1 && self->insideTableCell && self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        if (self->hasFailed()) return;
      }
      self->nextWordContinues = false;
      self->tableDepth += 1;
      self->depth += 1;
      return;
    }

    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      if (self->hasFailed()) return;
    }
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
      if (self->hasFailed()) return;
      self->currentTextBlock.reset();
    }
    self->flushPendingAnchor();
    if (self->hasFailed()) return;
    self->pushTableTextStyleEntry(cssStyle);
    self->tableDepth = 1;
    self->insideTableCell = false;
    self->tableRowStacked = false;
    self->tableRowRtl = cssStyle.hasDirection() && cssStyle.direction == CssTextDirection::Rtl;
    self->tableRowsSpannedRemaining = 0;
    self->tableCellTextBytes = 0;
    self->tableRowCells.clear();
    self->tableRowCells.reserve(MAX_GRID_TABLE_COLUMNS);
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    self->finishTableRow();
    if (self->hasFailed()) return;
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      // Text before the first row is typically a <caption>.
      self->makePages();
      if (self->hasFailed()) return;
    }
    self->currentTextBlock.reset();
    self->tableRowStacked = self->tableRowsSpannedRemaining > 0;
    self->tableRowRtl = cssStyle.hasDirection() && cssStyle.direction == CssTextDirection::Rtl;
    if (self->tableRowsSpannedRemaining != UINT16_MAX && self->tableRowsSpannedRemaining > 0) {
      self->tableRowsSpannedRemaining--;
    }
    self->pushTableTextStyleEntry(cssStyle);
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      if (self->hasFailed()) return;
    }
    self->closeTableCell();
    if (self->hasFailed()) return;
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
      if (self->hasFailed()) return;
    }
    self->currentTextBlock.reset();

    const uint16_t columnSpan = parseTableSpan(getAttribute(atts, "colspan"));
    const uint16_t rowSpan = parseTableSpan(getAttribute(atts, "rowspan"));
    if (columnSpan > 1 || rowSpan > 1) {
      self->fallbackTableRowToStacked();
      if (self->hasFailed()) return;
    }
    if (rowSpan > 1) {
      const uint16_t remaining = rowSpan == UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(rowSpan - 1);
      self->tableRowsSpannedRemaining = std::max(self->tableRowsSpannedRemaining, remaining);
    }

    auto tableCellBlockStyle = BlockStyle();
    tableCellBlockStyle.textAlignDefined = true;
    tableCellBlockStyle.alignment =
        cssStyle.hasTextAlign()
            ? cssStyle.textAlign
            : (self->effectiveTextAlignDefined
                   ? self->effectiveTextAlign
                   : (cssStyle.hasDirection() && cssStyle.direction == CssTextDirection::Rtl ? CssTextAlign::Right
                                                                                             : CssTextAlign::Left));
    if (cssStyle.hasDirection()) {
      tableCellBlockStyle.directionDefined = true;
      tableCellBlockStyle.isRtl = cssStyle.direction == CssTextDirection::Rtl;
    }

    self->currentTextBlock =
        makeUniqueNoThrow<ParsedText>(self->extraParagraphSpacing, self->firstLineIndent, self->hyphenationEnabled,
                                      self->focusReadingEnabled, tableCellBlockStyle, self->collectTouchLinks);
    if (!self->currentTextBlock) {
      LOG_ERR("EHP", "OOM: table cell");
      self->failAllocation("table cell");
      return;
    }
    self->insideTableCell = true;
    self->tableCellTextBytes = 0;
    self->wordsExtractedInBlock = 0;
    self->flushPendingAnchor();
    if (self->hasFailed()) return;
    self->pushTableTextStyleEntry(cssStyle);

    if (strcmp(name, "th") == 0 && (!cssStyle.hasFontWeight() || cssStyle.fontWeight == CssFontWeight::Bold)) {
      self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    }

    self->depth += 1;
    return;
  }

  if (self->tableDepth >= 1 && strcmp(name, "hr") == 0) {
    self->depth += 1;
    return;
  }

  if (self->tableDepth >= 1 && self->insideTableCell && isHeaderOrBlock(name)) {
    // Collapse block markup inside a cell to a word boundary.
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      if (self->hasFailed()) return;
    }
    self->nextWordContinues = false;
    self->depth += 1;
    return;
  }

  if (self->tableDepth >= 1 && self->insideTableCell && matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    // Preserve alt text without allocating an image framebuffer in the row.
    const char* alt = getAttribute(atts, "alt");
    if (alt && alt[0] != '\0') {
      self->syntheticCharacterData = true;
      self->characterData(userData, alt, strlen(alt));
      if (self->hasFailed()) return;
      self->syntheticCharacterData = false;
    }
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    std::string src;
    std::string alt;
    std::string klass;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0) {
          src = atts[i + 1];
        } else if (src.empty() && (strcmp(atts[i], "href") == 0 || strcmp(atts[i], "xlink:href") == 0)) {
          src = atts[i + 1];
        } else if (strcmp(atts[i], "alt") == 0) {
          alt = atts[i + 1];
        } else if (strcmp(atts[i], "class") == 0) {
          klass = atts[i + 1];
        }
      }

      // QQ 阅读器等把弹注做成一张小图：
      //   <img class="qqreader-footnote" alt="注释正文" src="../Images/note.png"/>
      // 它既不是真图（渲染出来就是正文里一行小图标），alt 又被丢掉 —— 弹注就此消失。
      // 认出来：**不发图**，改发一个上标标记词，注释正文存进 FootnoteEntry.href 的
      // "alt:" 哨兵前缀里（这样不用动 FootnoteEntry 结构 / Page 缓存格式）。
      if (!alt.empty() && isFootnoteImageClass(klass)) {
        self->appendAltFootnoteMarker(alt);
        self->depth += 1;  // 与其它自闭合元素一致：闭合回调里再 -= 1
        return;
      }

      // Duokan 一系（祖堂集）也把注号做成小图，但**没有 alt**，外面还包着一条指向
      // #note_xxx 的页内内链：
      //   <sup><a class="duokan-footnote" href="#note_205"><img class="sup" src="…/note.png"/></a></sup>
      // 上面那条判据要求非空 alt，认不出它 → 图片被当正文块图排进去（"图片排版都错位"）；
      // 而链接里一个文字都没有 → currentFootnote.number 为空 → 闭合时不登记脚注，点哪
      // 都弹不出来。这里补一条：**在脚注内链里的**图（href 是纯页内锚点、图就是整条链接
      // 的标签、class/src 也像注号）不发图，改发一个上标序号 —— 闭合时照常登记脚注。
      // !currentFootnoteIsBackref：回引也是内链了（见 startTag 的说明），但注文侧的
      // 回引图绝不是"注号"，不能在这儿被合成成一条上标序号。
      if (self->insideFootnoteLink && !self->currentFootnoteIsBackref &&
          !self->currentFootnote.href.empty() && self->currentFootnote.href[0] == '#' &&
          self->currentFootnoteLinkTextLen == 0 && isFootnoteMarkerImage(klass, src)) {
        if (self->partWordBufferIndex > 0) {
          self->flushPartWordBuffer();
          if (self->hasFailed()) return;
        }
        // 序号取本章第几条脚注，与 QQ 阅读器那条路一致：注号得**本页唯一**，不然点哪条
        // 都只会弹第一条（rdTapOnFootnote 是拿按中的词去比本页注号列表的）。
        const std::string marker = self->nextFootnoteMarker();
        StyleStackEntry supEntry;
        supEntry.depth = self->depth;
        supEntry.hasSup = true;
        supEntry.sup = true;
        self->inlineStyleStack.push_back(supEntry);
        self->updateEffectiveInlineStyle();
        self->syntheticCharacterData = true;
        self->characterData(userData, marker.c_str(), marker.length());
        self->syntheticCharacterData = false;
        self->inlineStyleStack.pop_back();
        self->updateEffectiveInlineStyle();
        if (self->hasFailed()) return;

        self->depth += 1;  // 与其它自闭合元素一致：闭合回调里再 -= 1
        return;
      }

      // 第三条（最宽松的一条）：一枚**内联小图 + 一段 alt 正文**，既没有 class、
      // 也不在任何内链里。斐洞（希腊文、汉文对照）就是这样：整章 807 枚注号共用同一张
      // 48×48 的小图
      //   <img alt="斐洞（Φαίδων）来自伯罗奔尼撒半岛的城邦厄里斯……" src="../Images/img001.png"/>
      // 注文整篇就压在 alt 里（这本书的 <p class="note"> 只有 29 条，绝大多数注只有
      // 这一份 alt）。上面两条判据都认不出它（没 class、外面没链接），于是 807 枚注号
      // 被当 807 张插图排进去：正文被切成 807 段、每张还要往 SD 上抠一个 imgN.png，
      // 而点它不弹注 —— 点注那套是**按词**找的，图上没有词可点。
      //
      // 判据只能是"这张图小得不像插图"（isTinyMarkerImage，按 src 记账只探一次）。
      // 为什么不用 alt 判：这本书里最短的一条注是「见75d。」五个字，正经注文也有这么
      // 短的，按长度或字数筛会漏掉十几条；尺寸反而干净 —— 注号 48×48、分数图 18×54，
      // 真正要放的插图都是几百像素。
      //
      // 前面还要卡一道**行内**：注号是插在句子中间的（"斐洞啊<img/>，你本人…"），
      // 插图则总是自己独占一段。没有这道闸，任何"小图 + alt"都会被认成注号 —— 而 alt
      // 恰恰是**图注**最常见的位置；而且每张不同的 src 都要开一次 zip entry 去量尺寸，
      // 一本插图多的书能白探几百次。探图那一步（isTinyMarkerImage）在这道闸后面。
      const bool inlineInTextRun =
          self->partWordBufferIndex > 0 ||
          (self->currentTextBlock != nullptr && !self->currentTextBlock->isEmpty());
      if (inlineInTextRun && !alt.empty() && !src.empty()) {
        std::string probeSrc = src;
        const size_t probeHash = probeSrc.find('#');
        if (probeHash != std::string::npos) probeSrc.resize(probeHash);
        const std::string probeResolved =
            FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->contentBase + probeSrc));
        if (self->isTinyMarkerImage(probeResolved)) {
          self->appendAltFootnoteMarker(alt);
          self->depth += 1;  // 与其它自闭合元素一致：闭合回调里再 -= 1
          return;
        }
      }

      const size_t fragmentPos = src.find('#');
      if (fragmentPos != std::string::npos) {
        src.resize(fragmentPos);
      }

      // imageRendering: 0=display, 1=placeholder (alt text only), 2=suppress entirely
      if (self->imageRendering == 2) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }

      if (!src.empty() && self->imageRendering != 1) {
        LOG_DBG("EHP", "Found image: src=%s", src.c_str());

        {
          // Resolve the image path relative to the HTML file
          std::string resolvedPath = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->contentBase + src));

          // Read the entry's first bytes before deciding anything. They carry both
          // the dimensions AND the real format, and an EPUB may name an image
          // without an extension or with a misleading one — while the decoder
          // lookup is extension-based. Deciding on the href alone dropped those
          // images silently: no log, no placeholder, the picture just vanished.
          // The header therefore decides; the path extension is only the fallback.
          ImageDimensions dims = {0, 0};
          ImageDimsProbe headerProbe;
          self->epub->readItemContentsToStream(resolvedPath, headerProbe, 1024, /*allowEarlyStop=*/true);
          bool gotDimensions = headerProbe.getDimensions(dims);
          const char* sniffedExtension = ImageDimsProbe::extensionForFormat(headerProbe.detectedFormat());
          const bool recognised = sniffedExtension != nullptr || ImageDecoderFactory::isFormatSupported(resolvedPath);

          if (!recognised) {
            LOG_ERR("EHP", "Unsupported image entry (neither JPEG/PNG content nor a known extension): %s",
                    resolvedPath.c_str());
          } else {
            // Create a unique filename for the cached image. Prefer the sniffed
            // format so the cached file always carries an extension the decoder
            // can act on, whatever the href said.
            std::string ext;
            if (sniffedExtension != nullptr) {
              ext = sniffedExtension;
            } else {
              const size_t extPos = resolvedPath.rfind('.');
              if (extPos != std::string::npos) {
                ext = resolvedPath.substr(extPos);
              }
            }
            std::string cachedImagePath = self->imageBasePath + std::to_string(self->imageCounter++) + ext;

            {
              if (!gotDimensions) {
                // No header within the stream (rare) — fall back to extracting the
                // whole image and probing the file. That can take seconds, so
                // surface the indexing popup first (single-shot per parser).
                if (self->popupFn && !self->imagePopupFired) {
                  self->imagePopupFired = true;
                  self->popupFn();
                }
                const bool extractSuccess = self->epub->extractItemToFile(resolvedPath, cachedImagePath);
                if (extractSuccess) {
                  // Retry to absorb SD-card sync latency on slow cards, and to close
                  // the silent-drop bug where a single getDimensions failure was fatal.
                  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedImagePath);
                  for (int attempt = 0; attempt < 3 && !gotDimensions; attempt++) {
                    if (attempt > 0) {
                      delay(50);  // Give a slow SD card time to finish syncing before retrying
                    }
                    gotDimensions = decoder && decoder->getDimensions(cachedImagePath, dims);
                  }
                } else {
                  LOG_ERR("EHP", "Failed to extract image");
                }
              }

              if (gotDimensions) {
                LOG_DBG("EHP", "Image dimensions: %dx%d", dims.width, dims.height);

                int displayWidth = 0;
                int displayHeight = 0;
                const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
                const CssStyle& imgStyle = cssStyle;
                const bool hasCssHeight = imgStyle.hasImageHeight();
                const bool hasCssWidth = imgStyle.hasImageWidth();

                // Compute effective container width for percentage-based image sizes.
                // If the image is inside a block with horizontal margins/padding (e.g.
                // <div style="margin: 1em 40%">), percentage widths like width:100%
                // should resolve against the container width, not the full viewport.
                int containerWidth = self->viewportWidth;
                if (self->currentTextBlock) {
                  const int inset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
                  if (inset > 0 && inset < self->viewportWidth) {
                    containerWidth = self->viewportWidth - inset;
                  }
                }

                if (hasCssHeight && hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Both CSS height and width set: resolve both, then clamp to viewport preserving requested ratio
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  if (displayWidth < 1) displayWidth = 1;
                  if (displayWidth > containerWidth || displayHeight > self->viewportHeight) {
                    float scaleX =
                        (displayWidth > containerWidth) ? static_cast<float>(containerWidth) / displayWidth : 1.0f;
                    float scaleY = (displayHeight > self->viewportHeight)
                                       ? static_cast<float>(self->viewportHeight) / displayHeight
                                       : 1.0f;
                    float scale = (scaleX < scaleY) ? scaleX : scaleY;
                    displayWidth = static_cast<int>(displayWidth * scale + 0.5f);
                    displayHeight = static_cast<int>(displayHeight * scale + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  // CSS 同时给了宽和高，但那个盒子几乎从来不是图片自己的比例
                  // （`width:100%;height:100%` 的 svg 包装、`width:1em;height:1em` 的
                  // 字符图标、阅读器外壳给正文图统一套的 100% 盒子……）。解码器拿到
                  // useExactDimensions 后会把源图**独立地**拉伸到 (maxWidth,maxHeight)
                  // （JpegToFramebufferConverter 里 destWidth/destHeight 各算各的，
                  // 注释都写明了"输出的宽高比可能与源不同"），于是整本书的插图被压扁。
                  // 这里按 object-fit: contain 收进 CSS 盒子：保留源图宽高比，取能放
                  // 进去的最大尺寸。只会变小不会变大 —— 盒子比图小就是浏览器里的效果。
                  // / CSS gives both width and height, but that box is almost never the
                  // image's own ratio (100%/100% svg wrappers, 1em/1em glyph icons, a
                  // reader shell forcing 100% on body images...). With
                  // useExactDimensions the decoder stretches the source independently to
                  // (maxWidth, maxHeight) — its own comment says the output aspect may
                  // differ from the source — so every inline figure came out squashed.
                  // Contain it: keep the source ratio inside the CSS box, never enlarge.
                  if (dims.width > 0 && dims.height > 0) {
                    const float srcAspect = static_cast<float>(dims.width) / static_cast<float>(dims.height);
                    const float boxAspect = static_cast<float>(displayWidth) / static_cast<float>(displayHeight);
                    if (boxAspect > srcAspect) {
                      displayWidth = static_cast<int>(displayHeight * srcAspect + 0.5f);
                    } else if (boxAspect < srcAspect) {
                      displayHeight = static_cast<int>(displayWidth / srcAspect + 0.5f);
                    }
                    if (displayWidth < 1) displayWidth = 1;
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  LOG_INF("EHP", "图像(宽高都给了) 源 %dx%d → 显示 %dx%d", dims.width, dims.height, displayWidth,
                          displayHeight);
                } else if (hasCssHeight && !hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Use CSS height (resolve % against viewport height) and derive width from aspect ratio
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  displayWidth =
                      static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayWidth > containerWidth) {
                    displayWidth = containerWidth;
                    // Rescale height to preserve aspect ratio when width is clamped
                    displayHeight =
                        static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  if (displayWidth < 1) displayWidth = 1;
                  LOG_INF("EHP", "图像(只给高) 源 %dx%d → 显示 %dx%d", dims.width, dims.height, displayWidth,
                          displayHeight);
                } else if (hasCssWidth && !hasCssHeight && dims.width > 0 && dims.height > 0) {
                  // Use CSS width (resolve % against container width) and derive height from aspect ratio
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (displayWidth > containerWidth) displayWidth = containerWidth;
                  if (displayWidth < 1) displayWidth = 1;
                  displayHeight =
                      static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayHeight < 1) displayHeight = 1;
                  LOG_INF("EHP", "图像(只给宽) 源 %dx%d → 显示 %dx%d", dims.width, dims.height, displayWidth,
                          displayHeight);
                } else {
                  // Scale to fit container while maintaining aspect ratio
                  int maxWidth = containerWidth;
                  int maxHeight = self->viewportHeight;
                  float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;
                  float scaleY = (dims.height > maxHeight) ? (float)maxHeight / dims.height : 1.0f;
                  float scale = (scaleX < scaleY) ? scaleX : scaleY;
                  if (scale > 1.0f) scale = 1.0f;

                  displayWidth = (int)(dims.width * scale);
                  displayHeight = (int)(dims.height * scale);
                  LOG_INF("EHP", "图像(无 CSS 尺寸) 源 %dx%d → 显示 %dx%d (scale %.2f)", dims.width, dims.height,
                          displayWidth, displayHeight, scale);
                }

                // 判"这张图是不是夹在句子中间"必须在**冲刷之前**：冲刷会把攒着的正文
                // 全落到页上，之后再问 currentTextBlock 就永远是空的（见下面那段行内
                // 图片）。顺手把段落样式也留一份——冲刷之后 currentTextBlock 换成了新
                // 块，段落的对齐/右内边距还得按原文那段算。
                const bool pendingTextBeforeImage =
                    self->partWordBufferIndex > 0 ||
                    (self->currentTextBlock != nullptr && !self->currentTextBlock->isEmpty());
                const BlockStyle paragraphStyle =
                    self->currentTextBlock ? self->currentTextBlock->getBlockStyle() : BlockStyle();
                const uint32_t lineSerialBefore = self->lineSerial;

                // Flush any pending text block so it appears before the image
                if (self->partWordBufferIndex > 0) {
                  self->flushPartWordBuffer();
                  if (self->hasFailed()) return;
                }
                if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
                  const BlockStyle parentBlockStyle = self->currentTextBlock->getBlockStyle();
                  self->startNewTextBlock(parentBlockStyle);
                  if (self->hasFailed()) return;
                }

                // ── 行内图片：接在上一行文字的右边，同一行上 ──────────────────────
                // 原文里 `他说<img src="…"/>完了` 这种写法，图片以前一律另起几行居中
                // 放，一句话被切成三截，读起来是断的。这里改成：图片排在**上一行文字
                // 的右边**，后面的字继续跟在图片右边——续排块靠 BlockStyle::firstLineInset
                // 把首行整体右移（见 ParsedText::resolveFirstLineIndent）。
                //
                // 只在条件都成立时才走这条路，其余情况一个字都不变（整块插图）：
                //   · 图前面已经有正文：段落开头的图本来就是插图写法，不掺和；
                //   · 这次冲刷真的排出了一行（lineSerial 变了），别把上一段遗留的行尾
                //     当成落脚点；
                //   · 图的高度装得进那一行；
                //   · 那一行的行尾还放得下它；
                //   · 图和那一行在**同一页**上（换了页，记下的坐标就不属于这一页了）；
                //   · 段落是左对齐/两端对齐（居中、右对齐时“紧跟图片”和段落自身的对齐
                //     会互相打架）。
                {
                  const int lineH = self->lastLineBottom - self->lastLineY;
                  const int inlineGap = self->renderer.getSpaceWidth(self->currentFontId, EpdFontFamily::REGULAR);
                  const bool naturalAlign = paragraphStyle.alignment == CssTextAlign::Justify ||
                                            paragraphStyle.alignment == CssTextAlign::Left;
                  if (pendingTextBeforeImage && self->lineSerial != lineSerialBefore && lineH > 0 &&
                      displayHeight <= lineH && displayWidth > 0 && !paragraphStyle.isRtl && naturalAlign &&
                      self->lastLinePage == self->currentPage.get() &&
                      self->lastLineEndX + inlineGap + displayWidth <=
                          static_cast<int>(self->viewportWidth) - paragraphStyle.rightInset()) {
                    const int imgX = self->lastLineEndX + inlineGap;
                    const int imgY = self->lastLineY + (lineH - displayHeight) / 2;
                    auto inlineImage =
                        makeUniqueNoThrow<ImageBlock>(cachedImagePath, resolvedPath, displayWidth, displayHeight);
                    if (inlineImage) {
                      auto inlinePageImage = makeUniqueNoThrow<PageImage>(std::move(inlineImage),
                                                                          static_cast<int16_t>(imgX),
                                                                          static_cast<int16_t>(imgY));
                      if (inlinePageImage) {
                        self->currentPage->elements.push_back(std::move(inlinePageImage));
                        self->setCurrentPageVisibleOffset(self->visibleTextOffset);
                        // 续排的正文要躲开图片：把"左边被占了多宽"记到当前这个空块上，
                        // 它只作用于这个块的首行，也不会传给别的块（见 BlockStyle）。
                        //
                        // 同时把**纵向**段前距清零。续排的第一个词是接在上面那一行
                        // 右边的，不是新起一段；而 makePages() 会在排这个块之前把
                        // blockStyle.marginTop/paddingTop 加到 currentPageNextY 上
                        // （约 .cpp:2797），那会让半句话中间凭空空出一段——`p{margin:1em 0}`
                        // 这种书尤其明显。横向的 inset 和对齐要留着：续排得接着原行排。
                        if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                          BlockStyle contStyle = self->currentTextBlock->getBlockStyle();
                          const int inset = imgX + displayWidth + inlineGap - contStyle.leftInset();
                          contStyle.firstLineInset = static_cast<int16_t>(inset > 0 ? inset : 0);
                          contStyle.marginTop = 0;
                          contStyle.paddingTop = 0;
                          self->currentTextBlock->setBlockStyle(contStyle);
                        }
                        LOG_DBG("EHP", "行内图片: %dx%d 接在 y=%d x=%d 之后", displayWidth, displayHeight, imgY, imgX);
                        self->depth += 1;
                        return;
                      }
                    }
                    // 这里失败（内存不够）不致命：落到下面整块插图的路上。
                    LOG_ERR("EHP", "行内图片建块失败，退回整块插图");
                  }
                }

                // Apply vertical margins from the container to the image.
                // Top margin lives on the empty text block (deposited via vertical merge
                // in startNewTextBlock). Bottom margin was stripped by withoutBottom() for
                // deferred application at element close, so read it from the stack.
                int16_t imageMarginTop = 0;
                int16_t imageMarginBottom = 0;
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  const auto& bs = self->currentTextBlock->getBlockStyle();
                  imageMarginTop = bs.topInset();
                  if (self->blockStyleStack.size() > 1) {
                    imageMarginBottom = self->blockStyleStack.back().bottomInset();
                  }
                }

                // Create page for image - only break if image won't fit remaining space
                if (self->currentPage && !self->currentPage->elements.empty() &&
                    (self->currentPageNextY + imageMarginTop + displayHeight + imageMarginBottom >
                     self->viewportHeight)) {
                  self->completePageFn(std::move(self->currentPage), self->xpathParagraphIndex,
                                       self->xpathListItemIndex, self->currentPageVisibleOffset);
                  if (self->hasFailed()) return;
                  self->completedPageCount++;
                  if (!self->allocatePage()) return;
                } else if (!self->currentPage) {
                  if (!self->allocatePage()) return;
                }

                // Apply top margin from container block. Clamp it so the image never
                // overflows the page bottom: a full-viewport-height image leaves no room
                // for the margin, and the break above only fires on non-empty pages, so a
                // fresh page would otherwise place the image at y=marginTop and run
                // marginTop pixels past viewportHeight. A large bottom reserve (status
                // bar / big screen margin) absorbs that overflow silently, but with a
                // thin reserve it crosses the physical screen edge and fails
                // ImageBlock::render's bounds check, dropping the image entirely.
                if (self->currentPageNextY + imageMarginTop + displayHeight > self->viewportHeight) {
                  const int room = self->viewportHeight - displayHeight - self->currentPageNextY;
                  imageMarginTop = static_cast<int16_t>(room > 0 ? room : 0);
                }
                self->currentPageNextY += imageMarginTop;

                // Create ImageBlock and add to page
                auto imageBlock =
                    makeUniqueNoThrow<ImageBlock>(cachedImagePath, resolvedPath, displayWidth, displayHeight);
                if (!imageBlock) {
                  LOG_ERR("EHP", "Failed to create ImageBlock");
                  self->failAllocation("image block");
                  return;
                }
                int xPos = (self->viewportWidth - displayWidth) / 2;
                auto pageImage = makeUniqueNoThrow<PageImage>(std::move(imageBlock), xPos, self->currentPageNextY);
                if (!pageImage) {
                  LOG_ERR("EHP", "Failed to create PageImage");
                  self->failAllocation("PageImage");
                  return;
                }
                self->currentPage->elements.push_back(std::move(pageImage));
                self->setCurrentPageVisibleOffset(self->visibleTextOffset);
                self->currentPageNextY += displayHeight + imageMarginBottom;

                // The image consumed the empty block's accumulated vertical spacing.
                // Reset the block so the Vertical merge in startNewTextBlock doesn't
                // re-apply the same margins to the next text paragraph.
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  BlockStyle resetStyle;
                  resetStyle.alignment = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                             ? CssTextAlign::Justify
                                             : static_cast<CssTextAlign>(self->paragraphAlignment);
                  self->currentTextBlock->setBlockStyle(resetStyle);
                }

                self->depth += 1;
                return;
              } else {
                LOG_ERR("EHP", "Failed to get image dimensions");
                Storage.remove(cachedImagePath.c_str());
              }
            }
          }  // isFormatSupported
        }
      }

      // Fallback to alt text if image processing fails
      if (!alt.empty()) {
        alt = "[Image: " + alt + "]";
        self->startNewTextBlock(self->blockStyleStack.back()
                                    .getCombinedBlockStyle(centeredBlockStyle, BlockStyle::CombineAxis::Horizontal)
                                    .withoutBottom());
        if (self->hasFailed()) return;
        self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
        self->depth += 1;
        self->syntheticCharacterData = true;
        self->characterData(userData, alt.c_str(), alt.length());
        if (self->hasFailed()) return;
        self->syntheticCharacterData = false;
        // Skip any child content (skip until parent as we pre-advanced depth above)
        self->skipUntilDepth = self->depth - 1;
        return;
      }

      // No alt text, skip
      self->skipUntilDepth = self->depth;
      self->depth += 1;
      return;
    }
  }

  // Ruby tag handling
  if (strcmp(name, "ruby") == 0) {
    // <ruby> is an inline element: a base that follows text with no whitespace between them
    // continues the same visual word, exactly like <b>/<i> handling in endElement().
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      if (self->hasFailed()) return;
      self->nextWordContinues = true;
    }
    self->inRuby = true;
    self->rubyStartWordIndex = self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0;
    self->rubyTextBuffer.clear();
    self->depth += 1;
    return;
  }
  if (strcmp(name, "rt") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      if (self->hasFailed()) return;
    }
    self->collectingRubyText = true;
    self->depth += 1;
    return;
  }

  if (VisibleTextUtils::isNonVisibleElement(name)) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Skip blocks with role="doc-pagebreak" and epub:type="pagebreak"
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if ((strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0) ||
          (strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0)) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  // Detect internal <a href="..."> links (footnotes, cross-references)
  // Note: <aside epub:type="footnote"> elements are rendered as normal content
  // without special handling. Links pointing to them are collected as footnotes.
  if (strcmp(name, "a") == 0) {
    const char* href = getAttribute(atts, "href");

    bool isInternalLink = isInternalEpubLink(href);

    // Special case: javascript:void(0) links with data attributes
    // Example: <a href="javascript:void(0)"
    // data-xyz="{&quot;name&quot;:&quot;OPS/ch2.xhtml&quot;,&quot;frag&quot;:&quot;id46&quot;}">
    if (href && strncmp(href, "javascript:", 11) == 0) {
      isInternalLink = false;
      // TODO: Parse data-* attributes to extract actual href
    }

    // <a class="note-backref"> 是"从注文跳回正文"的反向链接（校勘记/脚注区常见）。
    // 正文里已经有注号在管弹注了，再把回跳链接登记成脚注，注文那一页就会凭空多出
    // 几条指向正文的"脚注"、注号也对不上。注意别误伤 note-ref（那才是正向注号）。
    //
    // **但它仍然是一条内链，链接矩形必须留。** 这里原来直接 `isInternalLink = false`，
    // 把整个 `if (isInternalLink)` 块（含 addLinkTarget）一起跳过了 —— 于是注文页一个
    // 链接矩形都没有（`$CLAUDE_JOB_DIR/tmp` 里那个主机端探针实测：晋书注文页 links = 0，
    // 正文页每条上标都有）。读端 `rdTapOnLink` 靠 rect 里的 href 精确跳转，没 rect 就
    // 只剩"按注号反查"那条路，而它要么靠开过弹注"学过"、要么靠往回扫页兜底 —— 用户侧
    // 就是"正常读到注文区点注号跳不回正文，只有从弹注跳过去才点得动"。
    // 现在改成：仍是内链（登记 rect、照常走内链那套），只在"算不算一条脚注"上区分
    // （class 命中回引 → currentFootnoteIsBackref，由 </a> 关闭处拦住登记）。
    bool linkClassBackref = false;
    if (isInternalLink) {
      const char* klass = getAttribute(atts, "class");
      if (klass && strstr(klass, "backref")) linkClassBackref = true;
    }

    if (isInternalLink) {
      // Footnote indices are block-relative, so linked rows use ordinary flow.
      if (self->tableDepth >= 1 && self->insideTableCell && !self->tableRowStacked) {
        self->fallbackTableRowToStacked();
        if (self->hasFailed()) return;
      }

      // 兜底第三种形态：注号 id 挂在**紧邻的前一个空 `<a>`** 上，本链接自己没有 id、
      // pendingAnchorId 也是空的
      //   <a id="w1"></a><a href="part0003.html#m1"><sup>[1]</sup></a>
      //   （《古典柏拉图主义哲学导论》整本都是两两一对：正文侧 `<a id="w1">`+`<a href="#m1">`，
      //     注文侧 `<a id="m1">`+`<a href="#w1">`。前两条 id 都没人登记，回引认不出来，
      //     每章凭空多出一倍的假脚注）。
      // 唯一线索是 inlineAnchorArms 里那条"刚 arm 过、之后一个字都没动过"的行内锚点。
      // 判据必须取在**下面这次 flush 之前**：arm 时尾部还有没折成词的文本，flush 一跑游标
      // 就往后走，取在后面永远比不上。两条同时成立才算紧邻 —— 块游标没动（没往块里加词）、
      // 待刷缓冲长度没动（两个 `<a>` 之间没有夹着文本）。少了后一条，`<a id="x"></a>文字<a href>`
      // 这种也会被误认成紧邻。
      std::string adjacentAnchorId;
      if (self->currentTextBlock && !self->inlineAnchorArms.empty()) {
        const InlineAnchorArm& last = self->inlineAnchorArms.back();
        if (last.block == self->currentTextBlock.get() &&
            last.wordOffset == static_cast<int>(self->currentTextBlock->size()) &&
            last.pendingAtArm == self->partWordBufferIndex) {
          adjacentAnchorId = last.id;
        }
      }

      // Flush buffer before style change
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        if (self->hasFailed()) return;
        self->nextWordContinues = true;
      }
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      self->currentFootnoteLinkId = self->currentTextBlock ? self->currentTextBlock->addLinkTarget(href) : 0;
      // 变长 href：不再有 256 字节的坎（原来超长就整条丢掉，注号点不出来）。
      // 只留一个损坏数据的闸：离谱长的 href 当没有处理，与旧行为一致。
      self->currentFootnote.href.clear();
      if (href != nullptr && strlen(href) <= FOOTNOTE_MAX_TEXT_BYTES) {
        self->currentFootnote.href = href;
      }
      self->currentFootnote.number[0] = '\0';
      self->currentFootnoteLinkTextLen = 0;

      // 回引识别（见头文件 footnoteMarkerAnchors 的说明）。两条信息都取在这里，因为
      // 此刻 pendingAnchorId 还没被 flushPendingAnchor 搬走 —— 它就是最近一层带 id 的
      // 自身/祖先元素：`<a id="noteref_1" href="#note_1">` 拿到 noteref_1，
      // `<sup id="ref-001"><a href="#note-001">` 拿到 ref-001，`<a id="1" href="#2">` 拿到 1。
      // 先比对（不含自己）再登记，免得自引用的链接把自己认成回引。
      const std::string localAnchor = localAnchorOf(href, self->filepath);
      // 两条信号取或：class 命中（calibre 那种 class="note-backref"）与 href 命中所见注号
      // 锚点（有些书的回引没有 class，只能靠"它指向的正是前面的上标"认出来）。
      self->currentFootnoteIsBackref =
          linkClassBackref ||
          (!localAnchor.empty() &&
           std::find(self->footnoteMarkerAnchors.begin(), self->footnoteMarkerAnchors.end(), localAnchor) !=
               self->footnoteMarkerAnchors.end());
      // 登记"注号锚点"（见头文件 footnoteMarkerAnchors 的说明）。注号 id 的来源**有两个**，
      // 按优先级取：
      //   · 链接**自己**的 id —— `<a id="noteref_1" href="#note_1">`（Duokan/Sigil 一系）、
      //     `<a id="1" href="#2">`（趙州録）。这类 id 在属性循环里已经被 armInlineAnchor 收进
      //     inlineAnchorArms 了，**不会**落到 pendingAnchorId，所以必须在这儿直接读自己的 id。
      //     原来只读 pendingAnchorId，于是这几种书一条注号锚点都没登记 —— 注文侧那条回引认不
      //     出来，全被当成脚注收下（《希腊人与非理性》每章凭空多出 116 条指向正文的假脚注，
      //     既挤占每页 16 条的 FootnoteList，点上去还弹错东西）。
      //   · 外层元素的 id —— `<sup id="ref-001"><a href="#note-001">`（晋书）才是
      //     pendingAnchorId 的用武之地。
      // 只有**非回引**才登记：回引的外层 id 是"注文那条"的 id
      // （<p id="note-013"><a class="note-backref" href="#ref-013">），把它也收进注号锚点表，
      // 之后任何指向该注文的链接都会被误判成回引、不再登记成脚注（一注多引的书就点不出弹注）。
      const char* ownId = getAttribute(atts, "id");
      std::string markerId = ownId != nullptr ? std::string(ownId) : self->pendingAnchorId;
      // 第三种形态（id 挂在紧邻的前一个空 `<a>` 上）—— 判据见上面取 adjacentAnchorId 处。
      if (markerId.empty()) markerId = adjacentAnchorId;
      if (!self->currentFootnoteIsBackref && !markerId.empty() &&
          std::find(self->footnoteMarkerAnchors.begin(), self->footnoteMarkerAnchors.end(), markerId) ==
              self->footnoteMarkerAnchors.end()) {
        self->footnoteMarkerAnchors.push_back(markerId);
      }

      // Apply underline style to visually indicate the link.
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasTextDecoration = true;
      entry.textDecoration = CssTextDecoration::Underline;
      applyDirectionToEntry(entry, cssStyle);
      applyVerticalAlignToEntry(entry, cssStyle);
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();

      // Skip CSS resolution — we already handled styling for this <a> tag
      self->depth += 1;
      return;
    }
  }

  // em 的折算基准固定用正文字体：块级字号不该把 margin/图片的 em 一起放大。
  const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
  const auto userAlignmentBlockStyle =
      BlockStyle::fromCssStyle(cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth,
                               self->cssBaseFontPx);

  if (strcmp(name, "hr") == 0) {
    auto hrBlockStyle =
        BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Left, self->viewportWidth, self->cssBaseFontPx);
    if (!self->embeddedStyle) {
      hrBlockStyle.marginLeft = 0;
      hrBlockStyle.marginRight = 0;
      hrBlockStyle.marginTop = 0;
      hrBlockStyle.marginBottom = 0;
      hrBlockStyle.paddingLeft = 0;
      hrBlockStyle.paddingRight = 0;
      hrBlockStyle.paddingTop = 0;
      hrBlockStyle.paddingBottom = 0;
      hrBlockStyle.textIndentDefined = false;
      hrBlockStyle.textIndent = 0;
    }
    self->emitHorizontalRule(hrBlockStyle);
    if (self->hasFailed()) return;
    self->depth += 1;
    return;
  }

  if (matches(name, HEADER_TAGS, std::size(HEADER_TAGS))) {
    self->currentCssStyle = cssStyle;
    auto headerBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Center, self->viewportWidth,
                                                     self->cssBaseFontPx);
    headerBlockStyle.textAlignDefined = true;
    if (self->embeddedStyle && cssStyle.hasTextAlign()) {
      headerBlockStyle.alignment = cssStyle.textAlign;
    }
    const auto accumulated =
        self->blockStyleStack.back().getCombinedBlockStyle(headerBlockStyle, BlockStyle::CombineAxis::Horizontal);
    self->blockStyleStack.push_back(accumulated);
    self->startNewTextBlock(accumulated.withoutBottom());
    if (self->hasFailed()) return;
    // 换字体必须排在 startNewTextBlock 之后：上一步会把上一个块排版出去，那时还得用
    // 上一个块自己的字体。
    self->currentFontId = self->resolvedFontId(accumulated);
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS))) {
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
        if (self->hasFailed()) return;
      }
      // A <br> after text is a line break: start the next block with the container's
      // vertical margins stripped, matching browsers, which never apply paragraph
      // margins at a <br>. This is what keeps <br>-per-paragraph books (common CJK
      // web-novel formatting) from re-adding container spacing at every paragraph
      // and collapsing page capacity.
      // A <br> on an empty block (consecutive <br>s, or a standalone <br> between
      // blocks) is a scene-break separator: keep the container margins so deposited
      // vertical spacing survives. Either way the block is tagged so that if it
      // stays empty, startNewTextBlock injects a full line-height gap when the next
      // block opens; once text follows the tag is inert.
      // Style comes from the block style stack, not the current block, so a closed
      // element's style can't leak through (#2679).
      BlockStyle brStyle = self->blockStyleStack.back();
      if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
        brStyle = brStyle.withoutTop().withoutBottom();
      }
      brStyle.fromBrElement = true;
      self->startNewTextBlock(brStyle);
      if (self->hasFailed()) return;
    } else {
      self->currentCssStyle = cssStyle;
      const auto accumulated = self->blockStyleStack.back().getCombinedBlockStyle(userAlignmentBlockStyle,
                                                                                  BlockStyle::CombineAxis::Horizontal);
      self->blockStyleStack.push_back(accumulated);
      self->startNewTextBlock(accumulated.withoutBottom());
      if (self->hasFailed()) return;
      self->currentFontId = self->resolvedFontId(accumulated);  // 同上：在 flush 之后
      self->updateEffectiveInlineStyle();

      if (strcmp(name, "li") == 0) {
        self->currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR, false, false, self->visibleTextOffset);
        self->listItemBulletOnly = true;
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      if (self->hasFailed()) return;
      self->nextWordContinues = true;
    }
    self->pushDecorationStyleEntry(CssTextDecoration::Underline, cssStyle);
  } else if (matches(name, LINETHROUGH_TAGS, std::size(LINETHROUGH_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      if (self->hasFailed()) return;
      self->nextWordContinues = true;
    }
    self->pushDecorationStyleEntry(CssTextDecoration::LineThrough, cssStyle);
  } else if (matches(name, BOLD_TAGS, std::size(BOLD_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      if (self->hasFailed()) return;
      self->nextWordContinues = true;
    }
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    // Push inline style entry for bold tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasBold = true;
    entry.bold = true;
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    applyTextDecorationToEntry(entry, cssStyle);
    applyDirectionToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      if (self->hasFailed()) return;
      self->nextWordContinues = true;
    }
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Push inline style entry for italic tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasItalic = true;
    entry.italic = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    applyTextDecorationToEntry(entry, cssStyle);
    applyDirectionToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "sup") == 0 || strcmp(name, "sub") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      if (self->hasFailed()) return;
      self->nextWordContinues = true;
    }
    StyleStackEntry entry;
    entry.depth = self->depth;
    if (strcmp(name, "sup") == 0) {
      entry.hasSup = true;
      entry.sup = true;
    } else {
      entry.hasSub = true;
      entry.sub = true;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Handle span and other inline elements for CSS styling.
    const bool inheritedTableTextAlign = self->tableDepth >= 1 && cssStyle.hasTextAlign();
    // 家族切换几乎只出现在行内（祖堂集：<span class="zhu"> 的仿宋注文），所以这一支
    // 必须认 font-family，否则家族字面永远不会被打开。只在"这个家族确实有字面"时才推
    // 一层栈 —— 别的家族（kt/ls/…）落到内容面，跟没写一样，不值得为它切断词缓冲。
    const EpdFontFamily::Style familyBitHere = self->familyBitOf(cssStyle);
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration() ||
        cssStyle.hasDirection() || cssStyle.hasVerticalAlign() || inheritedTableTextAlign ||
        familyBitHere != EpdFontFamily::REGULAR) {
      // Flush buffer before style change so preceding text gets current style
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        if (self->hasFailed()) return;
        self->nextWordContinues = true;
      }
      StyleStackEntry entry;
      entry.depth = self->depth;  // Track depth for matching pop
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      applyTextDecorationToEntry(entry, cssStyle);
      applyDirectionToEntry(entry, cssStyle);
      entry.setsParagraphDirection = strcmp(name, "html") == 0 || strcmp(name, "body") == 0;
      if (inheritedTableTextAlign) {
        entry.hasTextAlign = true;
        entry.textAlign = cssStyle.textAlign;
      }
      applyVerticalAlignToEntry(entry, cssStyle);
      if (familyBitHere != EpdFontFamily::REGULAR) {
        entry.hasFamilyBit = true;
        entry.familyBit = familyBitHere;
      }
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (!self->checkMemory()) return;
  const bool countVisibleOffsets = self->insideBody && self->nonVisibleTextDepth == 0 && !self->syntheticCharacterData;
  const uint32_t callbackVisibleOffset = self->visibleTextOffset;
  if (countVisibleOffsets) {
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(s);
    const unsigned char* end = ptr + len;
    while (ptr < end) {
      utf8NextCodepoint(&ptr);
      self->visibleTextOffset++;
    }
  }

  // Nested content needs an enclosing bounded cell collector.
  if (self->tableDepth > 1 && !self->insideTableCell) {
    return;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // Collect ruby text instead of normal word processing.
  if (self->collectingRubyText) {
    // 注音（ruby）本就该很短，但这是个 std::string：畸形 EPUB 里一个不闭合的
    // <ruby> 会把整章正文都攒进这里（clear 只发生在配对标记处）。封顶，超了就当
    // 普通文本丢弃余下部分——注音本来就画不下这么多字。
    static constexpr size_t kMaxRubyBytes = 4096;
    if (self->rubyTextBuffer.size() < kMaxRubyBytes) {
      const size_t room = kMaxRubyBytes - self->rubyTextBuffer.size();
      self->rubyTextBuffer.append(s, len < room ? len : room);
    }
    return;
  }

  if (self->tableDepth == 1 && !self->insideTableCell) {
    bool onlyWhitespace = true;
    for (int i = 0; i < len; ++i) {
      if (!isWhitespace(s[i])) {
        onlyWhitespace = false;
        break;
      }
    }
    if (onlyWhitespace) {
      return;
    }
  }

  // Recreate flow storage for valid text (for example a caption) after a row.
  if (!self->currentTextBlock) {
    const BlockStyle flowStyle =
        self->blockStyleStack.empty() ? BlockStyle() : self->blockStyleStack.back().withoutBottom();
    self->currentTextBlock =
        makeUniqueNoThrow<ParsedText>(self->extraParagraphSpacing, self->firstLineIndent, self->hyphenationEnabled,
                                      self->focusReadingEnabled, flowStyle, self->collectTouchLinks);
    if (!self->currentTextBlock) {
      LOG_ERR("EHP", "OOM: text block for character data");
      self->failAllocation("text block for character data");
      return;
    }
    self->wordsExtractedInBlock = 0;
  }

  // Collect footnote link display text (for the number label)
  // Skip whitespace and brackets to normalize noterefs like "[1]" → "1"
  if (self->insideFootnoteLink) {
    int start = 0;
    int end = len - 1;

    // Example input and output texts:
    // "     [  12  ]   " => "12"
    // "   turn to 256  " => "turn to 256"

    // Ignore leading whitespaces and left square brackets
    while (start < len && (isWhitespace(s[start]) || (s[start] == '['))) {
      ++start;
    }

    // Ignore trailing whitespaces and right square brackets
    while (end >= start && (isWhitespace(s[end]) || (s[end] == ']'))) {
      --end;
    }

    // Extract footnote link text
    for (int i = start; (self->currentFootnoteLinkTextLen < sizeof(self->currentFootnote.number) - 1) && (i <= end);
         ++i) {
      self->currentFootnote.number[self->currentFootnoteLinkTextLen++] = s[i];
    }
    self->currentFootnote.number[self->currentFootnoteLinkTextLen] = '\0';
  }

  uint32_t nextCodepointOffset = callbackVisibleOffset;
  for (int i = 0; i < len; i++) {
    // Only completed tokens are extracted; the pending UTF-8 word and last line
    // stay intact. Check inside the callback so a large text node cannot grow
    // the whole paragraph before yielding memory.
    if ((static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) {
      self->softFlushTextBlock();
      if (self->hasFailed()) return;
    }
    const uint32_t codepointOffset = nextCodepointOffset;
    if (countVisibleOffsets && (static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) {
      nextCodepointOffset++;
    }

    if (isWhitespace(s[i])) {
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        if (self->hasFailed()) return;
      }
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      // Skip the whitespace char
      continue;
    }

    // Detect U+00A0 (non-breaking space, UTF-8: 0xC2 0xA0) or
    //        U+202F (narrow no-break space, UTF-8: 0xE2 0x80 0xAF).
    //
    // Both are rendered as a visible space but must never allow a line break around them.
    // We split the no-break space into its own word token and link the surrounding words
    // with continuation flags so the layout engine treats them as an indivisible group.
    //
    // Example: "200&#xA0;Quadratkilometer" or "200&#x202F;Quadratkilometer"
    //   Input bytes:  "200\xC2\xA0Quadratkilometer"  (or 0xE2 0x80 0xAF for U+202F)
    //   Tokens produced:
    //     [0] "200"               continues=false
    //     [1] " "                 continues=true   (attaches to "200", no gap)
    //     [2] "Quadratkilometer"  continues=true   (attaches to " ", no gap)
    //
    //   The continuation flags prevent the line-breaker from inserting a line break
    //   between "200" and "Quadratkilometer". However, "Quadratkilometer" is now a
    //   standalone word for hyphenation purposes, so Liang patterns can produce
    //   "200 Quadrat-" / "kilometer" instead of the unusable "200" / "Quadratkilometer".
    if (static_cast<uint8_t>(s[i]) == 0xC2 && i + 1 < len && static_cast<uint8_t>(s[i + 1]) == 0xA0) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        if (self->hasFailed()) return;
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->partWordVisibleOffset = codepointOffset;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      self->flushPartWordBuffer();
      if (self->hasFailed()) return;

      self->nextWordContinues = true;  // Next real word attaches to this space (no break).

      i++;  // Skip the second byte (0xA0)
      continue;
    }

    // U+202F (narrow no-break space) — identical logic to U+00A0 above.
    if (static_cast<uint8_t>(s[i]) == 0xE2 && i + 2 < len && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
        static_cast<uint8_t>(s[i + 2]) == 0xAF) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        if (self->hasFailed()) return;
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->partWordVisibleOffset = codepointOffset;
      self->nextWordContinues = true;
      self->flushPartWordBuffer();
      if (self->hasFailed()) return;

      self->nextWordContinues = true;

      i += 2;  // Skip the remaining two bytes (0x80 0xAF)
      continue;
    }

    // Skip Zero Width No-Break Space / BOM (U+FEFF) = 0xEF 0xBB 0xBF
    const XML_Char FEFF_BYTE_1 = static_cast<XML_Char>(0xEF);
    const XML_Char FEFF_BYTE_2 = static_cast<XML_Char>(0xBB);
    const XML_Char FEFF_BYTE_3 = static_cast<XML_Char>(0xBF);

    if (s[i] == FEFF_BYTE_1) {
      // Check if the next two bytes complete the 3-byte sequence
      if ((i + 2 < len) && (s[i + 1] == FEFF_BYTE_2) && (s[i + 2] == FEFF_BYTE_3)) {
        // Sequence 0xEF 0xBB 0xBF found!
        i += 2;    // Skip the next two bytes
        continue;  // Move to the next iteration
      }
    }

    // If we're about to run out of space, then cut the word off and start a new one.
    // For CJK text (no spaces), this is the primary word-breaking mechanism.
    // We must avoid splitting multi-byte UTF-8 sequences across word boundaries,
    // otherwise the trailing bytes become orphaned continuation bytes that the
    // decoder can't interpret.
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      int safeLen = utf8SafeTruncateBuffer(self->partWordBuffer, self->partWordBufferIndex);

      if (safeLen < self->partWordBufferIndex && safeLen > 0) {
        // Incomplete UTF-8 sequence at the end — save it before flushing
        int overflow = self->partWordBufferIndex - safeLen;
        uint32_t overflowVisibleOffset = self->partWordVisibleOffset;
        const unsigned char* offsetPtr = reinterpret_cast<const unsigned char*>(self->partWordBuffer);
        const unsigned char* const safeEnd = offsetPtr + safeLen;
        while (offsetPtr < safeEnd) {
          utf8NextCodepoint(&offsetPtr);
          overflowVisibleOffset++;
        }
        char saved[4];
        for (int j = 0; j < overflow; j++) {
          saved[j] = self->partWordBuffer[safeLen + j];
        }
        self->partWordBufferIndex = safeLen;
        self->flushPartWordBuffer();
        if (self->hasFailed()) return;
        self->nextWordContinues = true;
        for (int j = 0; j < overflow; j++) {
          self->partWordBuffer[j] = saved[j];
        }
        self->partWordBufferIndex = overflow;
        self->partWordVisibleOffset = overflowVisibleOffset;
      } else {
        self->flushPartWordBuffer();
        if (self->hasFailed()) return;
        self->nextWordContinues = true;
      }
    }

    if (self->partWordBufferIndex == 0) {
      self->partWordVisibleOffset = codepointOffset;
    }
    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }

  self->softFlushTextBlock();
}

void ChapterHtmlSlimParser::softFlushTextBlock() {
  if (hasFailed() || !currentTextBlock || inRuby || (insideTableCell && !tableRowStacked)) return;
#ifdef BOARD_HAS_PSRAM
  const size_t threshold = embeddedStyle ? TEXT_BLOCK_SOFT_FLUSH_WORDS_CONSTRAINED : TEXT_BLOCK_SOFT_FLUSH_WORDS;
#else
  const size_t threshold = TEXT_BLOCK_SOFT_FLUSH_WORDS_CONSTRAINED;
#endif
  const size_t wordCount = currentTextBlock->size();
  if (wordCount <= threshold) return;
  LOG_DBG("EHP", "Text block soft flush (%u words)", static_cast<unsigned>(wordCount));
  const int inset = currentTextBlock->getBlockStyle().totalHorizontalInset();
  const uint16_t width = inset < viewportWidth ? static_cast<uint16_t>(viewportWidth - inset) : viewportWidth;
  if (!currentTextBlock->layoutAndExtractLines(
          renderer, currentFontId, width,
          [this](std::unique_ptr<TextBlock> line, const uint32_t offset) {
            return addLineToPage(std::move(line), offset);
          },
          false)) {
    failAllocation("page layout");
  }
}

void XMLCALL ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const XML_Char* s, const int len) {
  if (!static_cast<ChapterHtmlSlimParser*>(userData)->checkMemory()) return;
  // Check if this looks like an entity reference (&...;)
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      // Known entity: expand to its UTF-8 value
      characterData(userData, utf8Value, strlen(utf8Value));
      return;
    }
    // Unknown entity: preserve original &...; sequence
    characterData(userData, s, len);
    return;
  }
  // Not an entity we recognize - skip it
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (!self->checkMemory()) return;
  if (self->nonVisibleTextDepth > 0) {
    self->nonVisibleTextDepth--;
  }

  // Ruby text: </rt> distributes ruby to base words, </ruby> resets ruby state
  if (strcmp(name, "rt") == 0) {
    self->collectingRubyText = false;
    if (self->inRuby && self->currentTextBlock) {
      const int currentWordCount = static_cast<int>(self->currentTextBlock->size());
      const int baseWordCount = currentWordCount - self->rubyStartWordIndex;
      std::string cleanRuby = trimAndNormalize(self->rubyTextBuffer);
      if (!cleanRuby.empty()) {
        if (baseWordCount > 0) {
          self->currentTextBlock->setRubyGroupAt(self->rubyStartWordIndex, baseWordCount, cleanRuby);
          self->rubyStartWordIndex = currentWordCount;
        } else if (self->rubyStartWordIndex > 0) {
          int leaderIdx = self->rubyStartWordIndex - 1;
          while (leaderIdx >= 0 &&
                 (self->currentTextBlock->getWordStyleAt(leaderIdx) & EpdFontFamily::RUBY_CONTINUE) != 0) {
            leaderIdx--;
          }
          if (leaderIdx >= 0) {
            std::string prevRuby = self->currentTextBlock->getRubyTextAt(leaderIdx);
            self->currentTextBlock->setRubyForWordAt(leaderIdx, prevRuby + cleanRuby);
          }
        }
      }
    }
    self->rubyTextBuffer.clear();
    // Inline close: the next base (e.g. 字 in <ruby>漢<rt>かん</rt>字<rt>じ</rt></ruby>) joins the
    // preceding one with no space. Whitespace in the source resets this in characterData().
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->nextWordContinues = true;
    }
    self->depth -= 1;
    return;
  }
  if (strcmp(name, "ruby") == 0 && self->inRuby) {
    self->inRuby = false;
    self->rubyStartWordIndex = -1;
    self->rubyTextBuffer.clear();
    // Inline close: text following </ruby> joins the annotated base with no space.
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->nextWordContinues = true;
    }
    self->depth -= 1;
    return;
  }
  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      !self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;

  const bool styleWillChange = willPopStyleStack || willClearBold || willClearItalic;
  const bool headerOrBlockTag = isHeaderOrBlock(name);
  const bool tableStructuralTag = isTableStructuralTag(name);
  const bool insideSkippedSubtree = self->depth - 1 >= self->skipUntilDepth;

  if (!insideSkippedSubtree && self->tableDepth > 1 && strcmp(name, "table") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      if (self->hasFailed()) return;
    }
    self->nextWordContinues = false;
    self->tableDepth -= 1;
    self->depth -= 1;
    LOG_DBG("EHP", "nested table flattened into enclosing cell");
    return;
  }

  if (!insideSkippedSubtree && self->tableDepth >= 1 && self->insideTableCell && headerOrBlockTag) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      if (self->hasFailed()) return;
    }
    self->nextWordContinues = false;
    self->depth -= 1;
    return;
  }

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag = !headerOrBlockTag && !tableStructuralTag &&
                             !matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, std::size(BOLD_TAGS)) ||
                             matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS)) ||
                             matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS)) ||
                             matches(name, LINETHROUGH_TAGS, std::size(LINETHROUGH_TAGS)) || tableStructuralTag ||
                             matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) || self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      if (self->hasFailed()) return;
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;

  // Closing a footnote link — create entry from collected text and href
  if (self->insideFootnoteLink && self->depth == self->footnoteLinkDepth) {
    // 回引（注文 → 正文注号）不登记：它不是"某条注释的入口"，只是一条指回上标的链接。
    // 登记了就会在注文页多出几条指向正文的假脚注。链接矩形（addLinkTarget）照留 ——
    // 阅读器正是靠它按 href 跳回上标。
    if (!self->currentFootnoteIsBackref && self->currentFootnote.number[0] != '\0' &&
        !self->currentFootnote.href.empty()) {
      FootnoteEntry entry;
      strncpy(entry.number, self->currentFootnote.number, sizeof(entry.number) - 1);
      entry.number[sizeof(entry.number) - 1] = '\0';
      entry.href = self->currentFootnote.href;
      int wordIndex =
          self->wordsExtractedInBlock + (self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0);
      self->pendingFootnotes.push_back({wordIndex, entry});
    }
    self->insideFootnoteLink = false;
    self->currentFootnoteLinkId = 0;
    self->currentFootnoteIsBackref = false;
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  if (!insideSkippedSubtree && self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    self->closeTableCell();
    if (self->hasFailed()) return;
    self->nextWordContinues = false;
  }

  if (!insideSkippedSubtree && self->tableDepth == 1 && (strcmp(name, "tr") == 0)) {
    self->finishTableRow();
    if (self->hasFailed()) return;
    self->nextWordContinues = false;
  }

  if (!insideSkippedSubtree && self->tableDepth == 1 && strcmp(name, "table") == 0) {
    self->finishTableRow();
    if (self->hasFailed()) return;
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
      if (self->hasFailed()) return;
    }
    self->currentTextBlock.reset();
    self->tableDepth = 0;
    self->insideTableCell = false;
    self->tableRowStacked = false;
    self->tableRowsSpannedRemaining = 0;
    self->tableCellTextBytes = 0;
    self->tableRowCells.clear();
    self->nextWordContinues = false;

    const BlockStyle flowStyle =
        self->blockStyleStack.empty() ? BlockStyle() : self->blockStyleStack.back().withoutBottom();
    self->currentTextBlock =
        makeUniqueNoThrow<ParsedText>(self->extraParagraphSpacing, self->firstLineIndent, self->hyphenationEnabled,
                                      self->focusReadingEnabled, flowStyle, self->collectTouchLinks);
    if (!self->currentTextBlock) {
      LOG_ERR("EHP", "OOM: text block after table");
      self->failAllocation("text block after table");
      return;
    }
    self->wordsExtractedInBlock = 0;
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  // This handles all inline elements: b, i, u, span, etc.
  if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth) {
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag && !insideSkippedSubtree) {
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();

    // br is self-closing and not a container — it doesn't push/pop the stack.
    if (strcmp(name, "br") != 0 && self->blockStyleStack.size() > 1) {
      // Apply closing element's bottom margin to the current text block so
      // container spacing appears after the element's content (on the last child),
      // not on the first child via the empty-block merge in startNewTextBlock.
      if (self->currentTextBlock) {
        const auto style = self->currentTextBlock->getBlockStyle();
        self->currentTextBlock->setBlockStyle(style.addBottom(self->blockStyleStack.back()));
      }
      self->blockStyleStack.pop_back();
      // Start a new text block with the parent style to prevent subsequent bare text
      // from inheriting the closed block style (e.g. alignment or margins).
      // Vertical margins and paddings are stripped
      self->startNewTextBlock(self->blockStyleStack.back().withoutTop().withoutBottom());
      if (self->hasFailed()) return;
      self->currentFontId = self->resolvedFontId(self->blockStyleStack.back());  // 回到父块的字体
      self->updateEffectiveInlineStyle();
    }

    // </li> closes: if the bullet never got inline text (empty <li> or <li> with only
    // block children that were flushed), clear the flag so the next sibling doesn't
    // merge into this block.
    if (strcmp(name, "li") == 0) {
      self->listItemBulletOnly = false;
    }
  }
  if (strcmp(name, "body") == 0) {
    self->insideBody = false;
  }
  if (strcmp(name, "html") == 0) {
    self->htmlEnded_ = true;
  }
}

void ChapterHtmlSlimParser::stopParsing() {
  // Expat must not be freed inside a callback. It can still emit an empty
  // element's end callback after StopParser, so every callback checks failure.
  if (parseActive_ && xmlParser_) XML_StopParser(xmlParser_, XML_FALSE);
}

void ChapterHtmlSlimParser::failAllocation(const char* stage) {
  if (!hasFailed()) {
    LOG_ERR("EHP", "OOM: %s (words=%u, free=%u, min=%u, maxAlloc=%u)", stage,
            static_cast<unsigned>(currentTextBlock ? currentTextBlock->size() : 0),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMinFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    allocationFailed_ = true;
    stopParsing();
  }
}

bool ChapterHtmlSlimParser::checkMemory() {
#ifndef BOARD_HAS_PSRAM
  if (!hasFailed() && embeddedStyle && (ESP.getFreeHeap() < 48 * 1024 || ESP.getMaxAllocHeap() < 16 * 1024)) {
    failAllocation("styled parsing headroom");
  }
#endif
  return !hasFailed();
}

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() { abortParse(); }

bool ChapterHtmlSlimParser::beginParse() {
  allocationFailed_ = false;
  ioFailed_ = false;
  if (!checkMemory()) return false;
  htmlEnded_ = false;
  // Initialize block style stack with a root entry representing "no ancestor block elements".
  // The user's paragraph alignment is set as the default so child elements without explicit
  // text-align inherit it correctly through getCombinedBlockStyle.
  BlockStyle rootBlockStyle;
  rootBlockStyle.alignment = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                 ? CssTextAlign::Justify
                                 : static_cast<CssTextAlign>(this->paragraphAlignment);
  blockStyleStack.clear();
  blockStyleStack.reserve(8);
  blockStyleStack.push_back(rootBlockStyle);

  currentFontId = fontId;
  // CSS font-size 的折算基准。书自己在 body/html 上写死绝对字号时以它为准：很多书
  // `body{font-size:20px}`，拿阅读器的 34px 当分母会把整本书算成 0.6 倍而全篇掉到最小号，
  // 等于把阅读器的字号设置整个抹掉。em/%(相对) 的值这里不参与——分母得先有。
  cssBaseFontPx = static_cast<float>(renderer.getLineHeight(fontId));
  if (cssParser) {
    for (const char* tag : {"body", "html"}) {
      const CssStyle rootStyle = cssParser->resolveStyle(tag, {});
      const bool absolute =
          rootStyle.fontSize.unit == CssUnit::Pixels || rootStyle.fontSize.unit == CssUnit::Points;
      if (rootStyle.hasFontSize() && absolute && rootStyle.fontSize.value > 0.0f) {
        cssBaseFontPx = rootStyle.fontSize.unit == CssUnit::Points ? rootStyle.fontSize.value * 1.33f
                                                                   : rootStyle.fontSize.value;
        break;
      }
    }
  }
  // 临时诊断（字号锚定）：px/pt 的 CSS 字号按这个分母折算成倍率。如果它跟"用户选的
  // 字号"对不上（比如正文 64px 而这里因为书里 body{font-size:20px} 变成 20），
  // 那么书里所有 px 字号都会被算成 1.0 倍 → 全篇落到正文档，看起来就是"锚定没生效"。
  LOG_INF("EHP", "字号锚定: 正文 fontId=%d cssBaseFontPx=%.1f (行高口径)", fontId,
          static_cast<double>(cssBaseFontPx));

  tableDepth = 0;
  insideTableCell = false;
  tableRowStacked = false;
  tableRowsSpannedRemaining = 0;
  tableCellTextBytes = 0;
  tableRowCells.clear();
  for (auto& lines : tableCellLines) {
    lines.clear();
  }
  tableLineVisibleOffsets.clear();

  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  const auto align = rootBlockStyle.alignment;
  paragraphAlignmentBlockStyle.alignment = align;
  startNewTextBlock(paragraphAlignmentBlockStyle);
  if (hasFailed()) return false;

  xmlParser_ = XML_ParserCreate_MM(nullptr, &kExpatPsramSuite, nullptr);
  if (!xmlParser_) {
    failAllocation("XML parser");
    return false;
  }
  if (!readBuf_) {
    // 64 字节对齐（SDMMC 对 PSRAM 缓冲的对齐要求），长度也是 64 的倍数，
    // 这样每次 fread 都能直接 DMA，不需要任何弹跳缓冲。
    readBuf_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(64, PARSE_BUFFER_SIZE, MALLOC_CAP_SPIRAM));
    if (!readBuf_) readBuf_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(64, PARSE_BUFFER_SIZE, MALLOC_CAP_DMA));
    if (!readBuf_) {
      failAllocation("XML read buffer");
      destroyXmlParser(xmlParser_);
      xmlParser_ = nullptr;
      return false;
    }
  }

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE
  XML_SetDefaultHandlerExpand(xmlParser_, defaultHandlerExpand);

  if (!Storage.openFileForRead("EHP", filepath, parseFile_)) {
    ioFailed_ = true;
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
    return false;
  }

  // Get file size to decide whether to show indexing popup.
  if (popupFn && parseFile_.size() >= MIN_SIZE_FOR_POPUP) {
    popupFn();
  }

  XML_SetUserData(xmlParser_, this);
  XML_SetElementHandler(xmlParser_, startElement, endElement);
  XML_SetCharacterDataHandler(xmlParser_, characterData);

  parseStartTime_ = millis();
  return true;
}

ChapterHtmlSlimParser::ParseStatus ChapterHtmlSlimParser::parseStep() {
  if (!checkMemory()) return allocationFailed_ ? ParseStatus::OutOfMemory : ParseStatus::Error;
  if (!readBuf_) {
    failAllocation("XML read buffer");
    return ParseStatus::OutOfMemory;
  }

  const size_t len = parseFile_.read(readBuf_, PARSE_BUFFER_SIZE);

  if (len == 0 && parseFile_.available() > 0) {
    LOG_ERR("EHP", "File read error");
    failIo();
    return ParseStatus::Error;
  }

  const int done = parseFile_.available() == 0;

  parseActive_ = true;
  const auto result = XML_Parse(xmlParser_, reinterpret_cast<const char*>(readBuf_), static_cast<int>(len), done);
  parseActive_ = false;
  if (hasFailed()) return allocationFailed_ ? ParseStatus::OutOfMemory : ParseStatus::Error;
  if (result == XML_STATUS_ERROR) {
    if (XML_GetErrorCode(xmlParser_) == XML_ERROR_NO_MEMORY) {
      failAllocation("XML parsing");
      return ParseStatus::OutOfMemory;
    }
    if (htmlEnded_) {
      LOG_DBG("EHP", "Ignoring trailing data after </html>: %s", XML_ErrorString(XML_GetErrorCode(xmlParser_)));
      return ParseStatus::Done;
    }
    LOG_ERR("EHP", "Parse error at line %lu:\n%s", XML_GetCurrentLineNumber(xmlParser_),
            XML_ErrorString(XML_GetErrorCode(xmlParser_)));
    return ParseStatus::Error;
  }

  if (!checkMemory()) return allocationFailed_ ? ParseStatus::OutOfMemory : ParseStatus::Error;

  return done ? ParseStatus::Done : ParseStatus::More;
}

void ChapterHtmlSlimParser::abortParse() {
  if (xmlParser_) {
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
  }
  if (readBuf_) {
    heap_caps_free(readBuf_);
    readBuf_ = nullptr;
  }
  // Only close the file if it was successfully opened in beginParse()
  if (parseFile_.isOpen()) {
    parseFile_.close();
  }
}

bool ChapterHtmlSlimParser::finishParse() {
  if (!checkMemory()) {
    abortParse();
    return false;
  }
  if (xmlParser_) {
    LOG_DBG("EHP", "Parsed in %lu ms (tail words=%u, free=%u, min=%u, maxAlloc=%u)", millis() - parseStartTime_,
            static_cast<unsigned>(currentTextBlock ? currentTextBlock->size() : 0),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMinFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
  }
  parseFile_.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();
    if (hasFailed()) return false;
    if (!pendingAnchorId.empty()) {
      // 收尾这一条：makePages() 已经把块排完，记下的序号落在页尾之外 —— 读端越界就当
      // "没记"，退回按注号认整页。落在极少数"锚点在文件末尾、后面再没有块"的书上。
      anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount),
                            static_cast<uint16_t>(currentPage ? currentPage->elements.size() : 0)});
      pendingAnchorId.clear();
    }
    setCurrentPageVisibleOffset(visibleTextOffset);
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
    if (hasFailed()) return false;
    completedPageCount++;
    currentPage.reset();
    currentTextBlock.reset();
  }

  return true;
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  if (!beginParse()) {
    return false;
  }
  for (;;) {
    const ParseStatus status = parseStep();
    if (status == ParseStatus::Error || status == ParseStatus::OutOfMemory) {
      abortParse();
      return false;
    }
    if (status == ParseStatus::Done) {
      break;
    }
  }
  return finishParse();
}

bool ChapterHtmlSlimParser::addLineToPage(std::unique_ptr<TextBlock> line, const uint32_t visibleOffset) {
  if (hasFailed()) return false;
  // 行自己的字体：书内 CSS 字号让同一页里各行可能不同号，页内推进必须跟排版时的行高
  // 一致，否则行会叠字或留白。行上没戳字体时退回当前块字体。
  const int lineFontId = line->renderFontId() ? line->renderFontId() : currentFontId;
  const int lineHeight =
      renderer.getLineHeight(lineFontId, lineCompression) + line->getRubyShift(renderer.getFontAscenderSize(lineFontId));

  if (!currentPage) {
    if (!allocatePage()) return false;
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    setCurrentPageVisibleOffset(visibleOffset);
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
    if (hasFailed()) return false;
    completedPageCount++;
    if (!allocatePage()) return false;
  }
  setCurrentPageVisibleOffset(visibleOffset);

  // 结算落在这一行上的**行内**锚点（见 armInlineAnchor）。必须赶在 `wordsExtractedInBlock`
  // 累加这一行之前、且在这行的 PageLine 入页之前：此刻的 currentPage 就是它的页，
  // elements.size() 就是这一行即将占的那个序号。
  resolveInlineAnchors(line.get());

  // Track cumulative words to assign footnotes to the page containing their anchor
  wordsExtractedInBlock += line->wordCount();
  auto footnoteIt = pendingFootnotes.begin();
  while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
    currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
    ++footnoteIt;
  }
  pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

  // Apply horizontal left inset (margin + padding) as x position offset
  const int16_t xOffset = line->getBlockStyle().leftInset();
  const int rubyShift = line->getRubyShift(renderer.getFontAscenderSize(lineFontId));
  const int baseLineHeight = renderer.getLineHeight(lineFontId, lineCompression);
  for (const auto& link : line->takeLinkSpans()) {
    if (!currentPage->addLink(link.href, static_cast<int16_t>(xOffset + link.x),
                              static_cast<int16_t>(currentPageNextY + rubyShift - link.topLift), link.width,
                              static_cast<int16_t>(baseLineHeight + link.topLift))) {
      LOG_DBG("EHP", "Dropped page link: %.48s", link.href);
    }
  }
  // 记下这一行的几何，供行内图片（`<img>` 夹在正文里）接在它右边。最后一个字的
  // xpos 是排版时算好的（已含缩进/居中/两端对齐的拉伸），再加它自己的宽度就是行尾。
  // **这一段必须在下面 std::move(line) 之前**——移进 pageLine 之后 line 就成了空
  // 指针，再解引用就是拿 null+偏移 当地址读（EXCVADDR 0x2a，LoadProhibited）。
  const uint16_t lastWordIndex = line->wordCount();
  const int16_t lineEndX = [&] {
    if (lastWordIndex == 0) return xOffset;
    const uint16_t wi = static_cast<uint16_t>(lastWordIndex - 1);
    return static_cast<int16_t>(
        xOffset + line->wordXpos(wi) + renderer.getTextWidth(lineFontId, line->wordText(wi), line->wordStyle(wi)));
  }();

  auto pageLine = makeUniqueNoThrow<PageLine>(std::move(line), xOffset, currentPageNextY);
  if (!pageLine) {
    LOG_ERR("EHP", "OOM: PageLine (%u bytes)", static_cast<unsigned>(sizeof(PageLine)));
    failAllocation("page layout");
    return false;
  }
  // 这一行真的进页了，才把它记成"最后一行"——分配失败时上面已经返回，几何不动。
  lastLineEndX = lineEndX;
  lastLinePage = currentPage.get();
  lastLineY = currentPageNextY;
  lastLineBottom = static_cast<int16_t>(currentPageNextY + lineHeight);
  lineSerial++;
  currentPage->elements.push_back(std::move(pageLine));
  currentPageNextY += lineHeight;
  return true;
}

void ChapterHtmlSlimParser::makePages() {
  if (hasFailed()) return;
  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return;
  }

  if (!currentPage) {
    if (!allocatePage()) return;
  }

  const int lineHeight = renderer.getLineHeight(currentFontId, lineCompression);

  // Apply top spacing before the paragraph (stored in pixels)
  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  if (blockStyle.marginTop > 0) {
    currentPageNextY += blockStyle.marginTop;
  }
  if (blockStyle.paddingTop > 0) {
    currentPageNextY += blockStyle.paddingTop;
  }

  // Calculate effective width accounting for horizontal margins/padding
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  if (!currentTextBlock->layoutAndExtractLines(renderer, currentFontId, effectiveWidth,
                                               [this](std::unique_ptr<TextBlock> textBlock, const uint32_t offset) {
                                                 return addLineToPage(std::move(textBlock), offset);
                                               })) {
    failAllocation("page layout");
    return;
  }
  if (hasFailed()) return;

  // Fallback: transfer any remaining pending footnotes to current page.
  // Normally addLineToPage handles this via word-index tracking, but this catches
  // edge cases where a footnote's word index equals the exact block size.
  if (!pendingFootnotes.empty() && currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      currentPage->addFootnote(fn.number, fn.href);
    }
    pendingFootnotes.clear();
  }

  // Apply bottom spacing after the paragraph (stored in pixels)
  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  // Extra paragraph spacing: 0=off, else 0.5x/0.75x/1x/1.25x/1.5x line height.
  if (extraParagraphSpacing > 0) {
    constexpr float EXTRA_PARAGRAPH_SPACING_FACTORS[] = {0.0f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f};
    const float factor = extraParagraphSpacing < std::size(EXTRA_PARAGRAPH_SPACING_FACTORS)
                             ? EXTRA_PARAGRAPH_SPACING_FACTORS[extraParagraphSpacing]
                             : 1.5f;
    currentPageNextY += static_cast<int16_t>(lineHeight * factor);
  }
}
