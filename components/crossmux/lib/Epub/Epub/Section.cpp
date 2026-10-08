#include "Section.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
// Keep separate cache-version sequences for the Latin and Chinese builds.
// The same built-in font IDs resolve to different font data and metrics in the
// two firmware flavors, so sharing a cache version could reuse pagination
// produced by the other flavor after reflashing.
//
// History:
//   34 / 35 - flat parsed-text arena and bounded HTML parsing spans
//   36 / 37 - unified CJK shaping
//   38 / 39 - line-through decoration and resumable HTML reading
//   40 / 41 - line-height rounding
//   42 / 43 - image hrefs and ruby annotations
//   44 / 45 - closed-tag pagination state
//   46 / 47 - UTF-8 emergency wrapping for oversized tokens
//   48 / 49 - source-space-aware CJK gaps, ruby continuation, and <br> margins
//   50 / 51 - per-page visible-text offset LUT
//   52 / 53 - ruby/CJK justification layout and 256-byte footnote hrefs
//   54 / 55 - one-shot soft-flush indentation and two-CJK-character defaults
//   56 / 57 - focus-word break opportunities, image viewport clamping, and extra-wide line spacing
//   58 / 59 - simple HTML table rows laid out as positioned columns
//   60 / 61 - touch-link rectangles, inline direction inheritance, block spacing, and linked sup/sub
//   62 / 63 - touch-link capability in the render spec
//   64 / 65 - bounded no-PSRAM soft-flush windows
//   66 / 67 - content-sniffed image decoders and the img2_ image cache prefix
//
// The 66/67 bump is not optional alongside the img2_ prefix in imageBasePath
// below: recognising an image by its content instead of its href extension means
// entries that used to be skipped now consume an imageCounter value, so a rebuilt
// chapter numbers its images differently. Reusing a pre-upgrade path would serve
// the wrong picture, because ImageBlock::ensureExtracted() accepts whatever file
// already sits at the path (and its .pxc pixel cache) without checking the source.
//   68 / 69 - first-line indent becomes a three-state control (Auto/Indent/NoIndent);
//             reserved to stay clear of the concurrent 66/67 layout change
//   70 / 71 - paragraph spacing stores levels 0..5 as a byte, not a boolean
//   72 / 73 - missing glyphs reserve space for a visible outline placeholder
//   74 / 75 - inline image boxes keep the source aspect ratio (contain)
//
// The 74/75 bump is what makes the contain fix reach books that were already
// opened: the box (ImageBlock::width/height) is serialized *inside the page*,
// and loadSectionFile only rejects a .bin on version or render-spec mismatch.
// A cached page therefore still carries the box the old code stretched to the
// CSS box, and no amount of parser fixing will change it. Bumping clears the
// layout caches; the pages are rebuilt with proportional boxes on next open.
// 75/74: TextBlock 多了一个字节的字体 id（书内 CSS font-size 按块选字号），老 .bin
// 里的块没有这一字节，读下去会整体错位。App 每次都重排（startBuild），旧文件本来
// 也不会被复用，这次 bump 是纪律性的保险。
// 76/75: 字号表从 5 档扩到 9 档、标题留出余量，CSS 阶梯的映射变了。缓存头存的是
// spec.fontId + embeddedStyle，两者在这个改动下**不变**（同一档位仍是同一个 id），
// 但同一把键现在会产出不同版式（标题字号大了）—— 不清就会静默复用旧 .bin、标题不更新。
// 77/76: 缓存头加了 spec.fontTag（书内嵌字体的身份）。这次是**字段本身**变了：
// fontId 只表示字号，换字面（用户字体 ↔ 书内字体）不影响它，没有 fontTag 就会拿
// 另一套字面排出来的旧 .bin 当自己的。旧文件少 4 字节，读下去整段错位，必须 bump。
// 78/77: 字号梯子砍掉最小的 3 档（34/40px 太小），梯子索引整体左移。缓存头存的只是
// spec.fontId（档位索引），**新旧头完全一致**，但同一个 id 现在解析成不同的像素高度 ——
// 这正是"头对得上、版式却变了"的情况，不清就会静默复用旧 .bin（老书一直是大字号）。
// （阅读线不在这里面：它是绘制层按行现算的叠加，页缓存里一个字节都没变。）
// 79/78: 锚点表每条多了一个 uint16（页内元素序号）。页缓存本身没动，但**锚点表的
// 记录形状变了**：老文件按 2 字节读会整段错位（拿页号的高位当元素序号、下一个锚点的
// 长度前缀跟着错），必须让老文件走重建。翻页/版式一字未变，纯粹是"表结构变了"。
// 80/79: 脚注登记口径变了 —— 回引识别开始认"指向本章自己文件名"的内链
// （`part0008.html#w7` 这类 calibre 老写法，以前只认裸 `#w7`）。同一页的**脚注表内容**
// 因此不同（注文页少掉一排指回正文的假条目），而表是被序列化进 .bin 的：老文件里那张
// 错表会被原样读回来继续弹错注。页缓存形状没动，纯粹是"表中内容变了"。
// 81/80: TextBlock 末尾多写一个字节的段落首行标记（isParagraphStart）。页缓存块的**形状
// 变了**：老文件读到块尾时下一个块的 numWords 就会串到这一字节上，整页解析错乱，必须 bump。
// 弹注取文现在靠它划界（注文正文里回引别的注号不再把正文腰斩）。
// 82/81: 夹在正文中间的 <img> 改排到**行内**（图片接在上一行文字右边，后面的字继续
// 跟在图片右边），页里多出一种"文字行 + 同一 y 上的 PageImage"的组合。页缓存块的形状
// 一个字节没变，变的是**同一段 HTML 排出来的页不同了**——旧 .bin 会原样复用那些"图片
// 独占好几行"的旧版式。按惯例 bump 清缓存，让书重新排一遍。
// 84/83: 脚注表的 href 从 char[256] 改成了 std::string（"alt:" 哨兵那条路要装整段注文，
// 256 会截断；见 FootnoteEntry.h），序列化从定长 write 换成 writeString 的**长度前缀**。
// 页缓存块的**字节形状变了**——旧 .bin 按定长读会把后面的内容读错位。这是纯粹的格式不
// 兼容，必须换号。两个档位各跳两格（而不是各 +1），免得新英文版(82)撞上旧中文版(82)：
// 缓存目录只按书的路径哈希，跨固件版本是同一个目录。
// 86/85: 每个词的样式位从 1 字节加宽到 2 字节，多了书名线的 WAVY_UNDERLINE 位
// （见 EpdFontFamily.h 与 TextBlock.h）。TextBlock 的 arena 是**逐字节落盘**的
// （TextBlock::serialize），所以页缓存块的形状真的变了：老 .bin 按 1 字节的
// styles[] 解释，后面所有数组和文本全错位。同时 CSS 侧也认了 border-bottom
// （CssParser::CSS_CACHE_VERSION 13→14），老 CSS 缓存里那些 span 没有下划线位。
// 同样各跳两格。
// 88/87: 书名线/专名线的解析规则变了（border-bottom 的线不再被 `text-decoration: none`
// 抹掉、虚/点线按波浪画，CssParser::CSS_CACHE_VERSION 14→15）。页缓存的**形状**没变，
// 变的是每个词样式位里有没有 UNDERLINE/WAVY_UNDERLINE：老 .bin 排出来是没线的，不清掉
// 的话已经建过缓存的古籍（含春秋左传注）打开仍然一条线都不画。同样各跳两格。
// 90/89: 回引链接（`<a class="…backref">`）不再被排除出"内链"这条路。以前 startTag 里
// 直接把它判成"非内链"，连 addLinkTarget 一起跳过了 —— 注文页因此一个链接矩形都没有，
// 读端只能走"按注号反查"那条路（要么靠开过弹注"学过"，要么靠往回扫页兜底），用户侧
// 就是"正常读到注文区点注号跳不回正文，只有从弹注跳过去才点得动"。页缓存块的**形状**
// 没变，变的是页内**链接表的内容**（注文页多出若干 rect，每个词还多一个 UNDERLINE 样式
// 位）—— 而链接表是逐字节序列化进 .bin 的，老文件里那张空表会被原样读回来继续点不动。
// 同样各跳两格。
#ifdef ENABLE_CHINESE_VERSION
constexpr uint8_t SECTION_FILE_VERSION = 90;
#else
constexpr uint8_t SECTION_FILE_VERSION = 89;
#endif
// Written into the version field while a build is in progress; patched to
// SECTION_FILE_VERSION only when the build is finalized. An abandoned /
// crash-interrupted .bin therefore carries version 0, which loadSectionFile rejects
// as unknown and clears -- so an incomplete file is never mistaken for a valid one.
constexpr uint8_t SECTION_FILE_INCOMPLETE_VERSION = 0;
// Written when a build is suspended partway (reader exited or device slept mid-build).
// The file carries valid pages 0..pageCount-1, all LUTs, and a trailer with the parse
// watermark (bytesConsumed, totalBytes) appended after the visible-offset LUT. loadSectionFile
// accepts it so a resume shows those pages instantly; the reader extends it by
// rebuilding in the background. Uses the same header layout as SECTION_FILE_VERSION,
// so finalized files are untouched by this feature; older firmware treats the sentinel
// as an unknown version and rebuilds, which is a safe downgrade.
// MUST change in lockstep with SECTION_FILE_VERSION: the sentinel IS the partial's
// format version, so a stale-format partial otherwise passes the header check and
// only fails (noisily, via the block-decode error path) when a page is loaded.
// Derived so the pairing can't be forgotten: 0xFE for v28, 0xFD for v29, ...
constexpr uint8_t SECTION_FILE_PARTIAL_VERSION = 0xFE - (SECTION_FILE_VERSION - 28);
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(uint8_t) + sizeof(uint8_t) +
                                 sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) +
                                 sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint32_t) * 6;
// Called only between layout/render operations; no borrowed glyph pointer is live.
void reclaimLayoutCaches(GfxRenderer& renderer, const char* stage) {
#ifndef BOARD_HAS_PSRAM
  if (ESP.getFreeHeap() >= 48 * 1024 && ESP.getMaxAllocHeap() >= 16 * 1024) return;
  auto* cache = renderer.getFontCacheManager();
  if (!cache) return;
  const auto before = ESP.getFreeHeap();
  cache->releaseSdFontCaches();
  if (ESP.getFreeHeap() > before) {
    LOG_DBG("SCT", "Reclaimed fonts at %s (free=%u->%u, min=%u, maxAlloc=%u)", stage, static_cast<unsigned>(before),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMinFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }
#endif
}
}  // namespace

// Out-of-line so the unique_ptr<ChapterHtmlSlimParser> in BuildContext can be
// constructed/destroyed where the parser's full definition is visible.
Section::Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
    : epub(epub),
      spineIndex(spineIndex),
      renderer(renderer),
      filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}

// Suspend any in-progress build so every section.reset() / navigation / sleep path
// persists the pages already laid out as a partial .bin instead of discarding them
// (no-op once a build has completed or never started).
Section::~Section() { suspendBuild(); }

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", builtPageCount_);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", builtPageCount_);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", builtPageCount_);

  builtPageCount_++;
  // pageCount is the pages available to read: a rebuild over a partial only raises it
  // once it has laid out more pages than the partial already covers.
  if (builtPageCount_ > pageCount) {
    pageCount = builtPageCount_;
  }
  return position;
}

void Section::writeSectionFileHeader(const ReaderRenderSpec& spec) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(spec.fontId) + sizeof(spec.fontTag) +
                                   sizeof(spec.lineCompression) + sizeof(spec.extraParagraphSpacing) +
                                   sizeof(spec.firstLineIndent) + sizeof(spec.paragraphAlignment) +
                                   sizeof(spec.viewportWidth) + sizeof(spec.viewportHeight) + sizeof(pageCount) +
                                   sizeof(spec.hyphenationEnabled) + sizeof(spec.embeddedStyle) +
                                   sizeof(spec.imageRendering) + sizeof(spec.focusReadingEnabled) +
                                   sizeof(spec.collectTouchLinks) + sizeof(uint32_t) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  // Written as the incomplete sentinel; finalizeBuild() patches it to
  // SECTION_FILE_VERSION as the last step, committing the file.
  serialization::writePod(file, SECTION_FILE_INCOMPLETE_VERSION);
  serialization::writePod(file, spec.fontId);
  serialization::writePod(file, spec.fontTag);
  serialization::writePod(file, spec.lineCompression);
  serialization::writePod(file, spec.extraParagraphSpacing);
  serialization::writePod(file, spec.firstLineIndent);
  serialization::writePod(file, spec.paragraphAlignment);
  serialization::writePod(file, spec.viewportWidth);
  serialization::writePod(file, spec.viewportHeight);
  serialization::writePod(file, spec.hyphenationEnabled);
  serialization::writePod(file, spec.embeddedStyle);
  serialization::writePod(file, spec.imageRendering);
  serialization::writePod(file, spec.focusReadingEnabled);
  serialization::writePod(file, spec.collectTouchLinks);
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for paragraph LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for li LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for visible-offset LUT (patched later)
}

bool Section::loadSectionFile(const ReaderRenderSpec& spec) {
  collectTouchLinks_ = spec.collectTouchLinks;
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  bool filePartial = false;
  {
    uint8_t version = 0;
    if (!serialization::readPod(file, version) ||
        (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION)) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      return false;
    }
    filePartial = (version == SECTION_FILE_PARTIAL_VERSION);

    int fileFontId = 0;
    uint32_t fileFontTag = 0;
    uint16_t fileViewportWidth = 0;
    uint16_t fileViewportHeight = 0;
    float fileLineCompression = 0;
    uint8_t fileExtraParagraphSpacing = 0;
    // FirstLineIndent::Auto(0)/Indent(1)/NoIndent(2). Read as a byte so the
    // three states round-trip; a bool would collapse Indent and NoIndent.
    uint8_t fileFirstLineIndent = 0;
    uint8_t fileParagraphAlignment = 0;
    bool fileHyphenationEnabled = false;
    bool fileEmbeddedStyle = false;
    uint8_t fileImageRendering = 0;
    bool fileFocusReadingEnabled = false;
    bool fileCollectTouchLinks = false;
    const bool headerValid =
        serialization::readPod(file, fileFontId) && serialization::readPod(file, fileFontTag) &&
        serialization::readPod(file, fileLineCompression) &&
        serialization::readPod(file, fileExtraParagraphSpacing) && serialization::readPod(file, fileFirstLineIndent) &&
        serialization::readPod(file, fileParagraphAlignment) && serialization::readPod(file, fileViewportWidth) &&
        serialization::readPod(file, fileViewportHeight) && serialization::readPod(file, fileHyphenationEnabled) &&
        serialization::readPod(file, fileEmbeddedStyle) && serialization::readPod(file, fileImageRendering) &&
        serialization::readPod(file, fileFocusReadingEnabled) && serialization::readPod(file, fileCollectTouchLinks);

    if (!headerValid || spec.fontId != fileFontId || spec.fontTag != fileFontTag ||
        spec.lineCompression != fileLineCompression ||
        spec.extraParagraphSpacing != fileExtraParagraphSpacing || spec.firstLineIndent != fileFirstLineIndent ||
        spec.paragraphAlignment != fileParagraphAlignment || spec.viewportWidth != fileViewportWidth ||
        spec.viewportHeight != fileViewportHeight || spec.hyphenationEnabled != fileHyphenationEnabled ||
        spec.embeddedStyle != fileEmbeddedStyle || spec.imageRendering != fileImageRendering ||
        spec.focusReadingEnabled != fileFocusReadingEnabled || spec.collectTouchLinks != fileCollectTouchLinks) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  if (!serialization::readPod(file, pageCount)) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: truncated page count");
    clearCache();
    pageCount = 0;
    return false;
  }

  if (filePartial) {
    // A partial's pageCount is the watermark of a suspended build. Read the watermark
    // trailer (appended after the visible-offset LUT) so estimatedTotalPages can extrapolate.
    uint32_t liLutOffset = 0;
    const bool liOffsetValid =
        file.seek(HEADER_SIZE - sizeof(uint32_t) * 2) && serialization::readPod(file, liLutOffset);
    uint32_t visibleLutOffset = 0;
    const bool visibleOffsetValid =
        file.seek(HEADER_SIZE - sizeof(uint32_t)) && serialization::readPod(file, visibleLutOffset);
    const uint64_t trailerOffset =
        static_cast<uint64_t>(visibleLutOffset) + static_cast<uint64_t>(pageCount) * sizeof(uint32_t);
    const bool trailerValid = liOffsetValid && visibleOffsetValid && pageCount > 0 && liLutOffset >= HEADER_SIZE &&
                              visibleLutOffset > liLutOffset && trailerOffset + 2 * sizeof(uint32_t) <= file.size();
    if (!trailerValid) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: malformed partial section");
      clearCache();
      pageCount = 0;
      return false;
    }
    if (!file.seek(static_cast<size_t>(trailerOffset)) || !serialization::readPod(file, partialBytesConsumed_) ||
        !serialization::readPod(file, partialTotalBytes_)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: truncated partial trailer");
      clearCache();
      pageCount = 0;
      return false;
    }
    partial_ = true;
    partialPageCount_ = pageCount;
  } else {
    // 整章已排完的 .bin：没有任何待续的活，直接标记完成。读端全靠这个判据决定"还要
    // 不要再 build"（翻页越过页数时要不要续排、空闲帧要不要补排版）——不标的话，一个
    // 既没有活构建、isBuildComplete() 又恒为 false 的 Section 会被那些循环当成"还在
    // 排，再等等"，在空闲帧里反复空转调 buildSomeMore（每次都打在"no active build"上）。
    // 之后真要继续排（比如版式变了要重排）走 startBuild()，它开头会把它清回 false。
    buildComplete_ = true;
  }

  // Explicit close() required: member variable persists beyond function scope
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages%s", pageCount, filePartial ? " (partial)" : "");
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  const std::string tmpBin = binTmpPath();
  if (Storage.exists(tmpBin.c_str())) {
    Storage.remove(tmpBin.c_str());
  }
  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::createSectionFile(const ReaderRenderSpec& spec, const std::function<void()>& popupFn) {
  // One-shot build: start, then lay out the whole section in a single pass.
  if (!startBuild(spec, popupFn)) {
    return false;
  }
  if (!buildSomeMore(0)) {  // 0 = build to completion
    return false;
  }
  return buildComplete_;
}

bool Section::startBuild(const ReaderRenderSpec& spec, const std::function<void()>& popupFn) {
  buildError_ = BuildError::Io;
  collectTouchLinks_ = spec.collectTouchLinks;
  if (build_) {
    LOG_ERR("SCT", "startBuild called while a build is already active");
    return false;
  }
  reclaimLayoutCaches(renderer, "start");
  buildComplete_ = false;
  builtPageCount_ = 0;
  // Pages from a loaded partial stay readable (from filePath) while this build writes
  // to the tmp .bin, so availability never drops below the partial's watermark.
  pageCount = partial_ ? partialPageCount_ : 0;

  // Remove a stale tmp .bin from a crash-interrupted build; this build recreates it.
  {
    const std::string staleTmp = binTmpPath();
    if (Storage.exists(staleTmp.c_str())) {
      Storage.remove(staleTmp.c_str());
    }
  }

  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto htmlDir = epub->getCachePath() + "/html";
  const auto htmlPath = htmlDir + "/" + std::to_string(spineIndex) + ".html";
  const auto tmpHtmlPath = htmlDir + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Reuse the previously unzipped HTML if we already have it. The unzipped HTML is keyed only on the
  // book (it lives in the per-book cache dir), not on render settings, so it survives the invalidation
  // that wipes the layout (.bin) caches when font/margin/orientation change -- rebuilds then skip zip
  // inflation entirely. It's promoted by an atomic rename as soon as the inflate succeeds (below), so
  // even a window-only giant spine -- whose .bin never finalizes -- still caches its HTML, letting a
  // reopen skip the multi-second inflate. If htmlPath exists it is known-complete.
  const bool reusedHtml = Storage.exists(htmlPath.c_str());
  bool htmlCached = reusedHtml;
  if (reusedHtml) {
    LOG_DBG("SCT", "Reusing cached HTML %s", htmlPath.c_str());
  } else {
    Storage.mkdir(htmlDir.c_str());

    // Retry logic for SD card timing issues
    bool streamed = false;
    uint32_t fileSize = 0;
    for (int attempt = 0; attempt < 3 && !streamed; attempt++) {
      if (attempt > 0) {
        LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
        delay(50);  // Brief delay before retry
      }

      // Remove any incomplete file from previous attempt before retrying
      if (Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
      }

      HalFile tmpHtml;
      if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
        continue;
      }
      // Larger chunks mean far fewer SD writes inflating the HTML; a 1KB chunk turned a 584KB
      // single-spine novel into ~570 tiny writes (multi-second). 8KB keeps the transient buffers
      // small while cutting the write count 8x.
      streamed = epub->readItemContentsToStream(localPath, tmpHtml, 8192);
      fileSize = tmpHtml.size();
      // Explicitly close() file before calling Storage.remove()
      tmpHtml.close();

      // If streaming failed, remove the incomplete file immediately
      if (!streamed && Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
        LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
      }
    }

    if (!streamed) {
      LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
      return false;
    }

    LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes)", tmpHtmlPath.c_str(), fileSize);

    // Promote to the persistent HTML cache immediately -- the inflate is complete and the bytes are
    // valid regardless of whether the layout build finishes, so reopening (even a window-only spine
    // that never finalizes its .bin) skips re-inflation. If the rename fails we just parse the temp.
    if (Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
      htmlCached = true;
    } else {
      LOG_DBG("SCT", "Failed to promote HTML cache; parsing from temp");
    }
  }

  if (!Storage.openFileForWrite("SCT", binTmpPath(), file)) {
    if (!reusedHtml) Storage.remove(tmpHtmlPath.c_str());
    return false;
  }
  // Header is written with the incomplete-version sentinel; finalizeBuild() commits it.
  writeSectionFileHeader(spec);

  build_ = makeUniqueNoThrow<BuildContext>();
  if (!build_) {
    LOG_ERR("SCT", "OOM: BuildContext");
    buildError_ = BuildError::OutOfMemory;
    releaseBuildResources();
    if (!reusedHtml) Storage.remove(tmpHtmlPath.c_str());
    return false;
  }
  auto& ctx = build_;
  // htmlCached == "htmlPath is the live cache" (reused, or just promoted). finalizeBuild/abandonBuild
  // then leave the cached HTML alone; only an un-promoted temp (rename failed) is theirs to clean up.
  ctx->reusedHtml = htmlCached;
  ctx->htmlPath = htmlPath;
  ctx->tmpHtmlPath = tmpHtmlPath;
  ctx->parsePath = htmlCached ? htmlPath : tmpHtmlPath;

  // Derive the content base directory and image cache path prefix for the parser
  const size_t lastSlash = localPath.find_last_of('/');
  ctx->contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  // The "img2_" prefix versions the extracted-image namespace. Content-based
  // decoder selection hands an imageCounter value to entries the old extension
  // gate skipped, which renumbers every later image in the chapter; without a new
  // prefix a rebuilt chapter would land on a pre-upgrade file (and reuse its .pxc)
  // that ImageBlock::ensureExtracted() accepts on existence alone. Bump it in
  // lockstep with SECTION_FILE_VERSION above. Pre-upgrade "img_*" files are simply
  // never referenced again.
  ctx->imageBasePath = epub->getCachePath() + "/img2_" + std::to_string(spineIndex) + "_";

  if (spec.embeddedStyle) {
    ctx->cssParser = epub->getCssParser();
    if (ctx->cssParser) {
      const CssParser::CacheLoadResult cacheResult = ctx->cssParser->loadFromCache();
      if (cacheResult == CssParser::CacheLoadResult::LowMemory) {
        LOG_ERR("SCT", "Insufficient heap to hydrate CSS");
        buildError_ = BuildError::OutOfMemory;
        releaseBuildResources();
        return false;
      }
      if (cacheResult == CssParser::CacheLoadResult::Invalid) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries
  std::vector<std::string> tocAnchors;
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  // The parser stores the path/contentBase/imageBasePath by reference, so they must
  // live in the BuildContext (which outlives the parser). The page-complete callback
  // captures the BuildContext pointer to append to its in-RAM LUT; build_ owns the
  // context for the parser's whole lifetime.
  BuildContext* ctxPtr = ctx.get();
  ctx->parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
      epub, ctxPtr->parsePath, renderer, spec.fontId, spec.lineCompression, spec.extraParagraphSpacing,
      spec.firstLineIndent, spec.paragraphAlignment, spec.viewportWidth, spec.viewportHeight, spec.hyphenationEnabled,
      spec.focusReadingEnabled,
      [this, ctxPtr](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex,
                     const uint32_t visibleTextOffset) {
        const uint32_t position = this->onPageComplete(std::move(page));
        if (position == 0) {
          buildError_ = BuildError::Io;
          ctxPtr->parser->failIo();
          return;
        }
        ctxPtr->lut.push_back({position, paragraphIndex, listItemIndex, visibleTextOffset});
      },
      spec.embeddedStyle, ctxPtr->contentBase, ctxPtr->imageBasePath, spec.imageRendering, std::move(tocAnchors),
      popupFn, ctxPtr->cssParser, spec.collectTouchLinks);
  if (!ctx->parser) {
    LOG_ERR("SCT", "OOM: ChapterHtmlSlimParser");
    buildError_ = BuildError::OutOfMemory;
    releaseBuildResources();
    return false;
  }

  Hyphenator::setPreferredLanguage(epub->getLanguage());

  if (!build_->parser->beginParse()) {
    LOG_ERR("SCT", "Failed to begin parse");
    if (build_->parser->allocationFailed()) buildError_ = BuildError::OutOfMemory;
    releaseBuildResources();
    return false;
  }
  build_->totalBytes = build_->parser->parseTotalBytes();
  buildError_ = BuildError::None;
  return true;
}

bool Section::buildSomeMore(const int maxPages) {
  if (!build_ || !build_->parser) {
    LOG_ERR("SCT", "buildSomeMore with no active build");
    return false;
  }
  // Pace on pages laid out by THIS build, not pageCount: during a rebuild over a partial,
  // pageCount stays pinned at the partial's watermark until the build passes it, which
  // would otherwise turn one "small" chunk into a blocking rebuild of the whole watermark.
  const int startCount = builtPageCount_;
  // 页数预算之外再加一个"源字节"预算。页面是在**块边界**产出的，遇到整章只有一个
  // 巨块（或畸形 HTML）时 maxPages 永远不满足，整章会在这一次调用里排完 —— 而这个
  // 函数是跑在主任务上的，表现就是几秒到几十秒完全无响应（且解析循环里没有 yield）。
  // 按已消费源字节封顶，保证单次调用有界；调用方本来就是 while 循环，多调几次即可。
  const size_t startBytes = build_->parser->parseBytesConsumed();
  const size_t maxBytesPerCall = 32 * 1024;
  for (;;) {
    reclaimLayoutCaches(renderer, "parse batch");
    const auto status = build_->parser->parseStep();
    switch (status) {
      case ChapterHtmlSlimParser::ParseStatus::OutOfMemory:
        buildError_ = BuildError::OutOfMemory;
        break;
      case ChapterHtmlSlimParser::ParseStatus::Error:
        buildError_ = build_->parser->ioFailed() ? BuildError::Io : BuildError::InvalidData;
        break;
      case ChapterHtmlSlimParser::ParseStatus::Done:
        return finalizeBuild();
      case ChapterHtmlSlimParser::ParseStatus::More:
        // Yield once we've laid out the requested number of pages.
        if (maxPages > 0 && (builtPageCount_ - startCount) >= maxPages) {
          build_->bytesConsumed = build_->parser->parseBytesConsumed();
          return true;
        }
        if (build_->parser->parseBytesConsumed() - startBytes >= maxBytesPerCall) {
          build_->bytesConsumed = build_->parser->parseBytesConsumed();
          return true;
        }
        continue;
    }
    LOG_ERR("SCT", "Parse error during incremental build");
    abandonBuild();
    return false;
  }
}

bool Section::hasHtmlCache() const {
  const std::string htmlPath = epub->getCachePath() + "/html/" + std::to_string(spineIndex) + ".html";
  return Storage.exists(htmlPath.c_str());
}

std::optional<uint16_t> Section::findAnchorDuringBuild(const std::string& anchor) const {
  if (!build_ || !build_->parser) return std::nullopt;  // 只看活构建那一份，不落盘
  for (const auto& rec : build_->parser->getAnchors()) {
    if (rec.id == anchor) return rec.page;
  }
  return std::nullopt;
}

std::optional<Section::AnchorPos> Section::findAnchorPos(const std::string& anchor) const {
  if (build_ && build_->parser) {
    for (const auto& rec : build_->parser->getAnchors()) {
      if (rec.id == anchor) return AnchorPos{rec.page, rec.element};
    }
  }
  // Fall back to the on-disk anchor map: a finalized section, or a partial whose map
  // covers everything up to its watermark (nullopt past it -- build further and retry).
  return getAnchorPosForAnchor(anchor);
}

std::optional<uint16_t> Section::findAnchor(const std::string& anchor) const {
  if (const auto pos = findAnchorPos(anchor)) return pos->page;
  return std::nullopt;
}

uint16_t Section::estimatedTotalPages() const {
  // Extrapolation from a suspended session's watermark trailer. A static snapshot, so no EMA
  // damping is needed. Also the best guess while a rebuild is running but hasn't laid out
  // enough pages yet to extrapolate from its own progress.
  const auto partialEstimate = [this]() -> uint16_t {
    if (!partial_ || partialBytesConsumed_ == 0 || partialTotalBytes_ <= partialBytesConsumed_) {
      return pageCount;
    }
    const uint64_t est = static_cast<uint64_t>(partialPageCount_) * partialTotalBytes_ / partialBytesConsumed_;
    if (est <= pageCount) return pageCount;
    return est > 60000 ? 60000 : static_cast<uint16_t>(est);
  };

  if (!build_) {
    return partial_ ? partialEstimate() : pageCount;  // partial -> extrapolate, finalized -> exact
  }
  const uint32_t consumed = build_->bytesConsumed;
  const uint32_t total = build_->totalBytes;
  if (builtPageCount_ == 0 || consumed == 0 || total <= consumed) return partialEstimate();

  // Raw extrapolation: scale the pages built so far by the fraction of HTML still unparsed. This
  // re-derives from a growing, non-uniform sample, so it jitters up and down as the build crosses
  // dense vs sparse regions of the chapter.
  const uint64_t raw = static_cast<uint64_t>(builtPageCount_) * total / consumed;

  // Damp that jitter with an exponential moving average. Step it once per build advance (keyed on
  // bytesConsumed) rather than per status-bar redraw, so the smoothing rate doesn't depend on how
  // often we repaint. As the build nears the end, consumed -> total and raw -> the built count, so
  // the average settles onto the true count (and finalizeBuild then returns the exact pageCount).
  constexpr float ALPHA = 0.25f;  // weight of each new sample; lower = steadier but slower to settle
  if (build_->smoothedEstimate <= 0) {
    build_->smoothedEstimate = static_cast<float>(raw);  // seed on the first estimate
  } else if (consumed != build_->smoothedAtConsumed) {
    build_->smoothedEstimate += ALPHA * (static_cast<float>(raw) - build_->smoothedEstimate);
  }
  build_->smoothedAtConsumed = consumed;

  const uint64_t est = static_cast<uint64_t>(build_->smoothedEstimate + 0.5f);
  if (est <= pageCount) return pageCount;  // never fewer than the pages already available
  return est > 60000 ? 60000 : static_cast<uint16_t>(est);
}

// Write the LUTs and anchor map into the open tmp .bin, patch the header with the built
// page count and table offsets, stamp `version` as the commit point, then swap the tmp
// file over filePath. For SECTION_FILE_PARTIAL_VERSION a watermark trailer
// (bytesConsumed, totalBytes) is appended after the li LUT so a later open can estimate
// the total page count. The parser must still be alive (anchors are read from it).
// On failure the tmp is removed and any pre-existing file at filePath is left intact.
bool Section::commitBuildFile(const uint8_t version, const uint32_t bytesConsumed, const uint32_t totalBytes) {
  const bool asPartial = (version == SECTION_FILE_PARTIAL_VERSION);

  const auto failCommit = [this]() {
    // Explicit close() required before remove (member variable, O_RDWR handle).
    file.close();
    Storage.remove(binTmpPath().c_str());
    return false;
  };

  const uint32_t lutOffset = file.position();
  for (const auto& entry : build_->lut) {
    if (entry.fileOffset == 0) {
      LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
      return failCommit();
    }
    serialization::writePod(file, entry.fileOffset);
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets). For a
  // partial, skip anchors that landed on the incomplete trailing page the suspend drops.
  // 每条 = 锚点 id + 页号 + **页内元素序号**（见 AnchorRecord：一页多条注文时靠它定位）。
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = build_->parser->getAnchors();
  uint16_t anchorCount = 0;
  for (const auto& rec : anchors) {
    if (!asPartial || rec.page < builtPageCount_) anchorCount++;
  }
  serialization::writePod(file, anchorCount);
  for (const auto& rec : anchors) {
    if (asPartial && rec.page >= builtPageCount_) continue;
    serialization::writeString(file, rec.id);
    serialization::writePod(file, rec.page);
    serialization::writePod(file, rec.element);
  }

  const uint32_t paragraphLutOffset = file.position();
  serialization::writePod(file, static_cast<uint16_t>(build_->lut.size()));
  for (const auto& entry : build_->lut) {
    serialization::writePod(file, entry.paragraphIndex);
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : build_->lut) {
    serialization::writePod(file, entry.listItemIndex);
  }

  const uint32_t visibleLutFileOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : build_->lut) {
    serialization::writePod(file, entry.visibleTextOffset);
  }

  if (asPartial) {
    // Watermark trailer, located on load immediately after the visible-offset LUT.
    serialization::writePod(file, bytesConsumed);
    serialization::writePod(file, totalBytes);
  }

  // Patch header with the built page count and section offsets...
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(builtPageCount_));
  serialization::writePod(file, builtPageCount_);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, paragraphLutOffset);
  serialization::writePod(file, liLutFileOffset);
  serialization::writePod(file, visibleLutFileOffset);
  // ...then commit by overwriting the sentinel version with the real one. Writing the
  // version last makes it the commit point: a crash before here leaves version 0.
  file.seek(0);
  serialization::writePod(file, version);
  // Explicit close() required: member variable persists beyond function scope
  file.close();

  // Swap into place. A crash between remove and rename loses the old file but keeps a
  // fully-committed tmp; the next build just removes it and rebuilds.
  if (Storage.exists(filePath.c_str())) {
    Storage.remove(filePath.c_str());
  }
  if (!Storage.rename(binTmpPath().c_str(), filePath.c_str())) {
    LOG_ERR("SCT", "Failed to move built section into place");
    Storage.remove(binTmpPath().c_str());
    return false;
  }
  return true;
}

bool Section::finalizeBuild() {
  // Flush the trailing page (emits the last page via the completePageFn into the LUT).
  if (!build_->parser->finishParse()) {
    LOG_ERR("SCT", "Failed to finish section parse");
    buildError_ = build_->parser->allocationFailed() ? BuildError::OutOfMemory : BuildError::Io;
    abandonBuild();
    return false;
  }

  if (!build_->reusedHtml) {
    // Parse succeeded: promote the freshly unzipped HTML to the persistent cache so future
    // rebuilds skip zip inflation. If promotion fails, drop the temp -- the build still succeeded.
    if (!Storage.rename(build_->tmpHtmlPath.c_str(), build_->htmlPath.c_str())) {
      LOG_DBG("SCT", "Failed to promote HTML cache, removing temp");
      Storage.remove(build_->tmpHtmlPath.c_str());
    }
  }

  const bool committed = commitBuildFile(SECTION_FILE_VERSION, 0, 0);
  releaseBuildResources();
  if (!committed) {
    buildError_ = BuildError::Io;
    // commitBuildFile removed filePath before the failed swap, so nothing valid remains.
    partial_ = false;
    partialPageCount_ = 0;
    pageCount = 0;
    builtPageCount_ = 0;
    return false;
  }
  buildComplete_ = true;
  partial_ = false;
  partialPageCount_ = 0;
  pageCount = builtPageCount_;
  return true;
}

void Section::suspendBuild() {
  if (!build_) return;

  // Only worth persisting if this build produced pages a pre-existing partial doesn't
  // already cover; otherwise keep the older (bigger) partial and just drop the tmp.
  const bool worthKeeping = builtPageCount_ > 0 && (!partial_ || builtPageCount_ > partialPageCount_);

  bool committed = false;
  if (worthKeeping) {
    // Capture the parse watermark and commit before tearing the parser down (the anchor
    // map is read from it). The incomplete trailing page is intentionally not flushed:
    // only fully laid-out pages are persisted, and the rebuild re-derives the rest.
    const uint32_t consumed = static_cast<uint32_t>(build_->parser->parseBytesConsumed());
    committed = commitBuildFile(SECTION_FILE_PARTIAL_VERSION, consumed, build_->totalBytes);
    if (committed) {
      partial_ = true;
      partialPageCount_ = builtPageCount_;
      partialBytesConsumed_ = consumed;
      partialTotalBytes_ = build_->totalBytes;
      LOG_INF("SCT", "Suspended build: %u pages persisted", builtPageCount_);
    } else {
      // A failed rename may have removed the old partial. Do not expose its
      // stale page count to a reader that continues after suspension.
      buildError_ = BuildError::Io;
      partial_ = false;
      partialPageCount_ = 0;
    }
  }

  releaseBuildResources();
  buildComplete_ = false;
  pageCount = partial_ ? partialPageCount_ : 0;
  builtPageCount_ = 0;
}

void Section::releaseBuildResources() {
  if (build_) {
    if (build_->parser) build_->parser->abortParse();
    if (build_->cssParser) build_->cssParser->clear();
    if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
      Storage.remove(build_->tmpHtmlPath.c_str());
    }
  }
  // The member file must be closed before removing an uncommitted build.
  if (file) file.close();
  if (Storage.exists(binTmpPath().c_str())) Storage.remove(binTmpPath().c_str());
  build_.reset();
}

void Section::abandonBuild() {
  if (!build_) return;
  releaseBuildResources();
  // A parse error would recur against the same HTML, so drop any partial too -- resuming
  // from it would just re-enter the failing build every open.
  if (Storage.exists(filePath.c_str())) {
    Storage.remove(filePath.c_str());
  }
  buildComplete_ = false;
  partial_ = false;
  partialPageCount_ = 0;
  pageCount = 0;
  builtPageCount_ = 0;
}

std::unique_ptr<Page> Section::loadPageDuringBuild(const int page) {
  if (!build_ || page < 0 || page >= static_cast<int>(build_->lut.size()) || !file) {
    return nullptr;
  }
  const uint32_t pos = build_->lut[page].fileOffset;
  if (pos == 0) {
    return nullptr;
  }
  // The .bin is open O_RDWR for the build. Read the already-written page, then restore
  // the write cursor so the next onPageComplete keeps appending where it left off.
  const uint32_t writePos = file.position();
  file.seek(pos);
  auto p = Page::deserialize(file, collectTouchLinks_);
  file.seek(writePos);
  if (p) {
    p->visibleTextOffset = build_->lut[page].visibleTextOffset;
  }
  return p;
}

// Read a page from the committed file at filePath (finalized section or partial from a
// previous session). Uses a local handle so it is safe while a build holds the member
// `file` open on the tmp .bin.
std::unique_ptr<Page> Section::loadPageAt(const int page) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return nullptr;
  }

  uint32_t lutOffset = 0;
  uint32_t pagePos = 0;
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 5) || !serialization::readPod(f, lutOffset) || lutOffset > f.size() ||
      (static_cast<uint64_t>(page) + 1) * sizeof(uint32_t) > f.size() - lutOffset ||
      !f.seek(lutOffset + sizeof(uint32_t) * page) || !serialization::readPod(f, pagePos) || pagePos >= f.size()) {
    return nullptr;
  }

  // Read this page's visible-codepoint start offset from the visible-offset LUT (last header slot)
  // in the same open handle, so the reader can persist progress without reopening the section file
  // on every page turn (see Page::visibleTextOffset). A malformed/old file leaves it at 0.
  uint32_t visibleLutOffset = 0;
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t)) || !serialization::readPod(f, visibleLutOffset)) return nullptr;
  uint32_t visibleTextOffset = 0;
  const uint64_t visibleEntry = static_cast<uint64_t>(visibleLutOffset) + sizeof(uint32_t) * page;
  if (visibleLutOffset >= HEADER_SIZE && visibleEntry + sizeof(uint32_t) <= f.size()) {
    if (!f.seek(static_cast<size_t>(visibleEntry)) || !serialization::readPod(f, visibleTextOffset)) return nullptr;
  }

  if (!f.seek(pagePos)) return nullptr;
  auto p = Page::deserialize(f, collectTouchLinks_);
  if (p) {
    p->visibleTextOffset = visibleTextOffset;
  }
  return p;
  // No f.close() needed -- DESTRUCTOR_CLOSES_FILE=1 handles it at scope exit
}

std::unique_ptr<Page> Section::loadPage(const int page) {
  if (page < 0) {
    return nullptr;
  }
  if (build_ && page < static_cast<int>(build_->lut.size())) {
    return loadPageDuringBuild(page);
  }
  // Not (yet) in the active build: serve from the file on disk -- a finalized section,
  // or a partial from a previous session whose pages the rebuild hasn't reached again.
  const int onDisk = partial_ ? partialPageCount_ : (build_ ? 0 : pageCount);
  if (page >= onDisk) {
    return nullptr;
  }
  return loadPageAt(page);
}

std::string Section::getTextFromSectionFile() {
  std::string fullText;
  auto p = loadPage(currentPage);
  if (p) {
    for (const auto& el : p->elements) {
      if (el->getTag() == TAG_PageLine) {
        const auto& line = static_cast<const PageLine&>(*el);
        if (line.getBlock()) {
          const auto& block = *line.getBlock();
          for (uint16_t i = 0; i < block.wordCount(); i++) {
            if (!fullText.empty()) fullText += " ";
            fullText += block.wordText(i);
          }
        }
      }
    }
  }
  return fullText;
}

std::optional<uint16_t> Section::getCachedPageCount() const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE) {
    return std::nullopt;
  }

  // Only a finalized section's count is the chapter total; a partial's count is just the
  // suspended build's watermark, which would skew progress mapping. Callers fall back to
  // their own estimates.
  uint8_t version = 0;
  if (!serialization::readPod(f, version) || version != SECTION_FILE_VERSION) {
    return std::nullopt;
  }

  uint16_t count = 0;
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(uint16_t)) || !serialization::readPod(f, count)) {
    return std::nullopt;
  }
  return count;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  if (const auto pos = getAnchorPosForAnchor(anchor)) return pos->page;
  return std::nullopt;
}

std::optional<Section::AnchorPos> Section::getAnchorPosForAnchor(const std::string& anchor) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  uint32_t anchorMapOffset = 0;
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 4) || !serialization::readPod(f, anchorMapOffset)) {
    return std::nullopt;
  }
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }

  uint16_t count = 0;
  if (!f.seek(anchorMapOffset) || !serialization::readPod(f, count)) return std::nullopt;
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page = 0;
    uint16_t element = 0;
    if (!serialization::readString(f, key, serialization::MAX_PATH_BYTES)) return std::nullopt;
    if (!serialization::readPod(f, page)) return std::nullopt;
    if (!serialization::readPod(f, element)) return std::nullopt;
    if (key == anchor) {
      return AnchorPos{page, element};
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  uint32_t paragraphLutOffset = 0;
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 3) || !serialization::readPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  uint16_t count = 0;
  if (!f.seek(paragraphLutOffset) || !serialization::readPod(f, count)) return std::nullopt;
  if (count == 0) {
    return std::nullopt;
  }

  const uint64_t lutEnd = static_cast<uint64_t>(paragraphLutOffset) + sizeof(uint16_t) + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx = 0;
    if (!serialization::readPod(f, pagePIdx)) return std::nullopt;
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  uint32_t paragraphLutOffset = 0;
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 3) || !serialization::readPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  uint16_t count = 0;
  if (!f.seek(paragraphLutOffset) || !serialization::readPod(f, count)) return std::nullopt;
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint64_t entryEnd =
      static_cast<uint64_t>(paragraphLutOffset) + sizeof(uint16_t) + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t pIdx = 0;
  if (!f.seek(paragraphLutOffset + sizeof(uint16_t) + page * sizeof(uint16_t)) || !serialization::readPod(f, pIdx)) {
    return std::nullopt;
  }
  return pIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  uint32_t liLutOffset = 0;
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2) || !serialization::readPod(f, liLutOffset)) {
    return std::nullopt;
  }
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  uint32_t paragraphLutOffset = 0;
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 3) || !serialization::readPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  uint16_t count = 0;
  if (!f.seek(paragraphLutOffset) || !serialization::readPod(f, count)) return std::nullopt;
  if (count == 0) {
    return std::nullopt;
  }

  const uint64_t lutEnd = static_cast<uint64_t>(liLutOffset) + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(liLutOffset)) return std::nullopt;
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx = 0;
    if (!serialization::readPod(f, pageLiIdx)) return std::nullopt;
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint32_t> Section::getVisibleTextOffsetForPage(const uint16_t page) const {
  if (build_ && page < build_->lut.size()) {
    return build_->lut[page].visibleTextOffset;
  }

  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f) || f.size() < HEADER_SIZE) {
    return std::nullopt;
  }

  uint8_t version = 0;
  if (!serialization::readPod(f, version) ||
      (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION)) {
    return std::nullopt;
  }

  uint16_t count = 0;
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(uint16_t)) || !serialization::readPod(f, count)) {
    return std::nullopt;
  }
  if (page >= count) {
    return std::nullopt;
  }

  uint32_t visibleLutOffset = 0;
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t)) || !serialization::readPod(f, visibleLutOffset)) {
    return std::nullopt;
  }
  const uint64_t entryOffset = static_cast<uint64_t>(visibleLutOffset) + static_cast<uint32_t>(page) * sizeof(uint32_t);
  if (visibleLutOffset < HEADER_SIZE || entryOffset + sizeof(uint32_t) > f.size()) {
    return std::nullopt;
  }

  uint32_t result = 0;
  if (!f.seek(static_cast<size_t>(entryOffset)) || !serialization::readPod(f, result)) return std::nullopt;
  return result;
}

std::optional<uint16_t> Section::getPageForVisibleTextOffset(const uint32_t offset,
                                                             const bool preferFirstAtOffset) const {
  const auto findInEntries = [offset, preferFirstAtOffset](const auto& entries) -> std::optional<uint16_t> {
    if (entries.empty()) return std::nullopt;
    uint16_t result = 0;
    for (size_t i = 0; i < entries.size(); i++) {
      const uint32_t pageStart = entries[i].visibleTextOffset;
      if (preferFirstAtOffset && pageStart == offset) {
        return static_cast<uint16_t>(i);
      }
      if (pageStart > offset) break;
      result = static_cast<uint16_t>(i);
    }
    return result;
  };

  if (build_ && !build_->lut.empty()) {
    // Resolve within the active build's known range. Later offsets may still be
    // covered by an on-disk partial that the resumed build has not reached yet.
    if (offset <= build_->lut.back().visibleTextOffset) {
      return findInEntries(build_->lut);
    }
  }

  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f) || f.size() < HEADER_SIZE) {
    return std::nullopt;
  }

  uint8_t version = 0;
  if (!serialization::readPod(f, version) ||
      (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION)) {
    return std::nullopt;
  }
  const bool partial = version == SECTION_FILE_PARTIAL_VERSION;

  uint16_t count = 0;
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(uint16_t)) || !serialization::readPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0) {
    return std::nullopt;
  }

  uint32_t visibleLutOffset = 0;
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t)) || !serialization::readPod(f, visibleLutOffset)) {
    return std::nullopt;
  }
  if (visibleLutOffset < HEADER_SIZE ||
      static_cast<uint64_t>(visibleLutOffset) + static_cast<uint32_t>(count) * sizeof(uint32_t) > f.size()) {
    return std::nullopt;
  }

  if (!f.seek(visibleLutOffset)) return std::nullopt;
  uint16_t result = 0;
  uint32_t lastPageStart = 0;
  for (uint16_t page = 0; page < count; page++) {
    uint32_t pageStart = 0;
    if (!serialization::readPod(f, pageStart)) return std::nullopt;
    lastPageStart = pageStart;
    if (preferFirstAtOffset && pageStart == offset) {
      return page;
    }
    if (pageStart > offset) break;
    result = page;
  }
  if (partial && offset > lastPageStart) {
    return std::nullopt;
  }
  return result;
}
