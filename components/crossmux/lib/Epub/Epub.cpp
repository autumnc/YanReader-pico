#include "Epub.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <Utf8.h>
#include <ZipFile.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/TocNavParser.h"
#include "Epub/parsers/TocNcxParser.h"

namespace {
enum class CoverImageType : uint8_t { None, Jpeg, Png };

CoverImageType coverImageType(const std::string& path) {
  HalFile file;
  uint8_t prefix[8] = {};
  if (!Storage.openFileForRead("EBP", path, file) || file.fileSize64() <= sizeof(prefix) ||
      file.read(prefix, sizeof(prefix)) != static_cast<int>(sizeof(prefix))) {
    return CoverImageType::None;
  }
  static constexpr uint8_t kPngMagic[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (memcmp(prefix, kPngMagic, sizeof(kPngMagic)) == 0) return CoverImageType::Png;
  return prefix[0] == 0xFF && prefix[1] == 0xD8 && prefix[2] == 0xFF ? CoverImageType::Jpeg : CoverImageType::None;
}

template <typename JpegConvert, typename PngConvert>
bool convertCoverFile(const std::string& sourcePath, const CoverImageType type, const std::string& outputPath,
                      const char* outputKind, JpegConvert jpegConvert, PngConvert pngConvert) {
  if (type == CoverImageType::None) return false;
  const char* format = type == CoverImageType::Jpeg ? "JPG" : "PNG";
  LOG_DBG("EBP", "Generating %s BMP from %s cover image", outputKind, format);

  bool success = false;
  {
    HalFile source;
    HalFile output;
    if (Storage.openFileForRead("EBP", sourcePath, source) && Storage.openFileForWrite("EBP", outputPath, output)) {
      success = type == CoverImageType::Jpeg ? jpegConvert(source, output) : pngConvert(source, output);
    }
  }
  if (!success) {
    LOG_ERR("EBP", "Failed to generate %s BMP from %s cover image", outputKind, format);
    Storage.remove(outputPath.c_str());
  }
  return success;
}

template <typename JpegConvert, typename PngConvert>
bool convertExtractedCover(const Epub& epub, const std::string& coverHref, const std::string& outputPath,
                           const char* outputKind, JpegConvert jpegConvert, PngConvert pngConvert) {
  const bool isJpeg = FsHelpers::hasJpgExtension(coverHref);
  const std::string tempPath = epub.getCachePath() + (isJpeg ? "/.cover.jpg" : "/.cover.png");

  if (!epub.extractItemToFile(coverHref, tempPath)) {
    LOG_ERR("EBP", "Failed to extract cover image for %s", outputKind);
    return false;
  }
  const ScopedCleanup removeTemp{[&tempPath] { Storage.remove(tempPath.c_str()); }};
  return convertCoverFile(tempPath, isJpeg ? CoverImageType::Jpeg : CoverImageType::Png, outputPath, outputKind,
                          jpegConvert, pngConvert);
}

template <typename JpegConvert, typename PngConvert>
bool convertOverrideCover(const Epub& epub, const std::string& outputPath, const char* outputKind,
                          JpegConvert jpegConvert, PngConvert pngConvert) {
  const std::string sourcePath = epub.getCoverOverridePath();
  return convertCoverFile(sourcePath, coverImageType(sourcePath), outputPath, outputKind, jpegConvert, pngConvert);
}

// ---------------------------------------------------------------------------
// Embedded-font discovery. Deliberately a standalone text scan over the book's
// stylesheets rather than a feature of CssParser: CssStyle is a fixed-size,
// memcmp-deduped POD with a fixed-length wire record and an already-full flags
// bitfield, so carrying family names through the rule cache would mean a new
// variable-length field, a cache-format bump, and a second resident string
// table — all to answer a question asked once per book.

bool isCssSpace(const char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string lowerAscii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::string trimAscii(const std::string& s) {
  size_t begin = 0;
  size_t end = s.size();
  while (begin < end && isCssSpace(s[begin])) ++begin;
  while (end > begin && isCssSpace(s[end - 1])) --end;
  return s.substr(begin, end - begin);
}

// Drop /* ... */ comments so a commented-out @font-face cannot win. An unterminated
// comment swallows the rest of the sheet, which is what a browser does too.
std::string stripCssComments(const std::string& css) {
  std::string out;
  out.reserve(css.size());
  for (size_t i = 0; i < css.size();) {
    if (css[i] == '/' && i + 1 < css.size() && css[i + 1] == '*') {
      const size_t end = css.find("*/", i + 2);
      if (end == std::string::npos) break;
      out += ' ';
      i = end + 2;
    } else {
      out += css[i++];
    }
  }
  return out;
}

// Last `prop: value` declaration in a declaration block, trimmed; "" when absent.
std::string cssDeclValue(const std::string& block, const char* prop) {
  const size_t propLen = strlen(prop);
  std::string value;
  size_t i = 0;
  while (i < block.size()) {
    const size_t colon = block.find(':', i);
    if (colon == std::string::npos) break;
    const size_t semi = block.find(';', colon);
    const size_t valueEnd = semi == std::string::npos ? block.size() : semi;
    const std::string name = trimAscii(block.substr(i, colon - i));
    if (name.size() == propLen && lowerAscii(name) == prop) {
      value = trimAscii(block.substr(colon + 1, valueEnd - colon - 1));
    }
    if (semi == std::string::npos) break;
    i = semi + 1;
  }
  return value;
}

// A CSS family list ("\"Songti\", 'STSong', serif") as normalised names in
// preference order. Quotes are optional in CSS and books use both.
std::vector<std::string> splitFamilyList(const std::string& value) {
  std::vector<std::string> families;
  size_t start = 0;
  while (start <= value.size()) {
    size_t comma = value.find(',', start);
    if (comma == std::string::npos) comma = value.size();
    std::string name = trimAscii(value.substr(start, comma - start));
    if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'') && name.back() == name.front()) {
      name = trimAscii(name.substr(1, name.size() - 2));
    }
    if (!name.empty()) families.push_back(lowerAscii(name));
    if (comma == value.size()) break;
    start = comma + 1;
  }
  return families;
}

bool isCssIdentChar(const char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; }

// Does this selector target the body or html *element*? ".body-text" and "bodyguard"
// must not count; "body p" and "html, body" must.
bool selectorMentionsElement(const std::string& selector, const char* name) {
  const size_t nameLen = strlen(name);
  for (size_t p = selector.find(name); p != std::string::npos; p = selector.find(name, p + 1)) {
    if (p > 0) {
      const char before = selector[p - 1];
      if (isCssIdentChar(before) || before == '.' || before == '#') continue;
    }
    if (isCssIdentChar(selector[p + nameLen])) continue;
    return true;
  }
  return false;
}

// Does this selector target a *bare* <name> element — one carrying no class/id of its
// own? "div.sgc-toc-title" and "p.duokan-image-subtitle" style one decorative thing
// (a TOC header, a caption), not the prose; a bare "p", "div p" or "p" in a comma list
// do. This is the guard that lets the p/div fallback tiers below exist without the
// book's heading/caption rules hijacking the whole novel.
// The caller passes an already lower-cased selector.
bool selectorHasBareElement(const std::string& selector, const char* name) {
  const size_t nameLen = strlen(name);
  for (size_t p = selector.find(name); p != std::string::npos; p = selector.find(name, p + 1)) {
    if (p > 0) {
      const char before = selector[p - 1];
      const bool atBoundary = before == ' ' || before == '\t' || before == '\n' || before == '\r' ||
                              before == ',' || before == '>' || before == '+' || before == '~';
      if (!atBoundary) continue;  // "span", ".p", "#page", ".pinyin"
    }
    const char after = selector[p + nameLen];  // std::string yields '\0' at size()
    const bool endsHere = after == '\0' || after == ' ' || after == '\t' || after == '\n' ||
                          after == '\r' || after == ',' || after == '>' || after == '+' ||
                          after == '~' || after == ':';
    if (!endsHere) continue;  // "pre", "div.sgc-toc-title", "p.duokan-image-subtitle"
    return true;
  }
  return false;
}

// Index of the '}' closing the '{' at `open`, honouring nesting; npos when unbalanced.
size_t findMatchingBrace(const std::string& css, const size_t open, const size_t end) {
  int depth = 0;
  for (size_t i = open; i < end; ++i) {
    if (css[i] == '{') {
      ++depth;
    } else if (css[i] == '}' && --depth == 0) {
      return i;
    }
  }
  return std::string::npos;
}

struct CssFontFace {
  std::string family;   // normalised family name
  std::string src;      // the raw src declaration value
  std::string cssHref;  // the stylesheet it came from; url() is relative to this
};

struct CssFontCandidates {
  std::vector<CssFontFace> faces;
  // Body-text family lists, one bucket per tier, in document order inside a bucket.
  // Tiers exist because a book almost never names its prose font the way we would:
  //   0  body / html   — the obvious one, but plenty of Chinese EPUBs have none at all
  //   1  bare <p>      — where the prose actually lives in most of those books
  //   2  bare <div>    — last resort for container-styled books
  // Keeping every rule (rather than "the winner") is deliberate: a footnote rule like
  // "li.duokan-footnote-item p" outranks the base "p" by specificity but not by our
  // flat document-order scan, and a rule whose first family is a font the archive does
  // not actually ship must not hide the next family that it does. resolveEmbeddedFont
  // flattens these and takes the first family that really resolves to a file.
  std::vector<std::vector<std::string>> tierFamilies[3];

  // 次家族候选：非正文三层的规则（.class / p.class / span.x …）里第一个家族名，以及它
  // 被引用了多少次。阅读器只养得起一个额外的字面，选谁就是"哪个家族在样式表里被用得
  // 最多"（祖堂集：正文 st，注文/引文 fs，fs 被 .jiazhu/.zhu/.author/.quote/.quoteright
  // 五条规则引用，压过 .kt 的四条）。**标题和行间注（h1..h6 / sup / sub / rt）不计**：
  // 那是一次性的短串，为它们多养一份字面（一份 CJK 字体常驻就是 1MB 上下）不划算，
  // 而真正值得多留一份字面的注文、引文、题名一律是段落正文。
  std::vector<std::pair<std::string, int>> altFamilies;
};

// 这条规则的家族值不值得为它多养一份字面（次家族候选）。排除三类：
//   1 标题 h1..h6、2 行间注/上标 sup/sub/rt、3 导航与图注（toc / image-subtitle / maintitle）
// —— 都是一次性短串或成不了气候的零碎，而一份 CJK 字面常驻就是 1MB 上下。
// 取反之后剩下的就是段落正文类（注文、引文、题名、说明文字），那才是第二个字面该去的地方。
// The caller passes an already lower-cased selector.
bool isDecorativeFamilySelector(const std::string& selector) {
  for (const char* heading : {"h1", "h2", "h3", "h4", "h5", "h6"}) {
    if (selectorMentionsElement(selector, heading)) return true;
  }
  for (const char* tiny : {"sup", "sub", "rt"}) {
    if (selectorHasBareElement(selector, tiny)) return true;
  }
  return selector.find("toc") != std::string::npos || selector.find("image-sub") != std::string::npos ||
         selector.find("image-main") != std::string::npos;
}

// Count one reference to `family`, keeping first-appearance order (the tie-break).
void tallyAltFamily(CssFontCandidates* out, const std::string& family) {
  for (auto& entry : out->altFamilies) {
    if (entry.first == family) {
      ++entry.second;
      return;
    }
  }
  out->altFamilies.emplace_back(family, 1);
}

void scanCssRules(const std::string& css, const size_t begin, const size_t end, const std::string& cssHref,
                  CssFontCandidates* out) {
  size_t i = begin;
  while (i < end) {
    const size_t brace = css.find('{', i);
    if (brace == std::string::npos || brace >= end) break;
    const size_t close = findMatchingBrace(css, brace, end);
    if (close == std::string::npos) break;

    // Statement at-rules (@charset, @import) end in ';' rather than a block, so they
    // land in the "selector" text of the rule that follows them. Keep only what comes
    // after the last ';' — otherwise "@charset "utf-8"; body" reads as a selector
    // starting with '@' and the body rule is dropped.
    std::string selector = trimAscii(css.substr(i, brace - i));
    const size_t lastSemi = selector.rfind(';');
    if (lastSemi != std::string::npos) selector = trimAscii(selector.substr(lastSemi + 1));
    selector = lowerAscii(selector);
    const std::string block = css.substr(brace + 1, close - brace - 1);

    if (!selector.empty() && selector[0] == '@') {
      if (selector.rfind("@font-face", 0) == 0) {
        const std::vector<std::string> families = splitFamilyList(cssDeclValue(block, "font-family"));
        const std::string src = cssDeclValue(block, "src");
        if (!families.empty() && !src.empty()) {
          out->faces.push_back({families.front(), src, cssHref});
        }
      } else if (selector.rfind("@media", 0) == 0 || selector.rfind("@supports", 0) == 0 ||
                 selector.rfind("@document", 0) == 0) {
        // Conditional group: the rules that matter are nested one level in.
        scanCssRules(css, brace + 1, close, cssHref, out);
      }
    } else {
      int tier = -1;
      if (selectorMentionsElement(selector, "body") || selectorMentionsElement(selector, "html")) {
        tier = 0;
      } else if (selectorHasBareElement(selector, "p")) {
        tier = 1;
      } else if (selectorHasBareElement(selector, "div")) {
        tier = 2;
      }
      const std::vector<std::string> families = splitFamilyList(cssDeclValue(block, "font-family"));
      if (families.empty()) {
        // nothing to record either way
      } else if (tier >= 0) {
        out->tierFamilies[tier].push_back(families);
      } else if (!isDecorativeFamilySelector(selector)) {
        tallyAltFamily(out, families.front());
      }
    }
    i = close + 1;
  }
}

// First URL in a src value that we can actually load. local() entries carry no URL,
// WOFF/WOFF2 are skipped outright (stb_truetype cannot read either, and extracting
// one would only produce a font that fails to open), and an extension-less URL is
// still accepted as a last resort.
bool pickFontUrl(const std::string& src, std::string* url) {
  std::string fallback;
  for (size_t i = src.find("url("); i != std::string::npos; i = src.find("url(", i)) {
    const size_t close = src.find(')', i + 4);
    if (close == std::string::npos) break;
    std::string candidate = trimAscii(src.substr(i + 4, close - i - 4));
    i = close + 1;
    if (candidate.size() >= 2 && (candidate.front() == '"' || candidate.front() == '\'') &&
        candidate.back() == candidate.front()) {
      candidate = trimAscii(candidate.substr(1, candidate.size() - 2));
    }
    const size_t cut = candidate.find_first_of("?#");
    if (cut != std::string::npos) candidate = candidate.substr(0, cut);
    if (candidate.empty()) continue;

    std::string ext;
    const size_t dot = candidate.rfind('.');
    const size_t slash = candidate.rfind('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
      ext = lowerAscii(candidate.substr(dot));
    }
    if (ext == ".woff" || ext == ".woff2") continue;
    if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") {
      *url = candidate;
      return true;
    }
    if (fallback.empty()) fallback = candidate;
  }
  if (fallback.empty()) return false;
  *url = fallback;
  return true;
}

// A ZIP URL is relative to the stylesheet that names it, and may be percent-encoded.
// extractFolderPath drops the trailing slash (and yields "/" at the archive root), so
// the separator has to be re-added; a leading "/" means archive-root-relative.
std::string resolveFontHref(const std::string& cssItemHref, const std::string& url) {
  const std::string decoded = FsHelpers::decodeUriEscapes(url);
  if (!decoded.empty() && decoded[0] == '/') {
    return FsHelpers::normalisePath(decoded);
  }
  return FsHelpers::normalisePath(FsHelpers::extractFolderPath(cssItemHref) + "/" + decoded);
}

}  // namespace

bool Epub::findContentOpfFile(std::string* contentOpfFile) const {
  const auto containerPath = "META-INF/container.xml";
  size_t containerSize;

  // Get file size without loading it all into heap
  if (!getItemSize(containerPath, &containerSize)) {
    LOG_ERR("EBP", "Could not find or size META-INF/container.xml");
    return false;
  }

  ContainerParser containerParser(containerSize);

  if (!containerParser.setup()) {
    return false;
  }

  // Stream read (reusing your existing stream logic)
  if (!readItemContentsToStream(containerPath, containerParser, 512)) {
    LOG_ERR("EBP", "Could not read META-INF/container.xml");
    return false;
  }

  // Extract the result
  if (containerParser.fullPath.empty()) {
    LOG_ERR("EBP", "Could not find valid rootfile in container.xml");
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);
  return true;
}

bool Epub::parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, const bool writeSpineEntries) {
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    LOG_ERR("EBP", "Could not find content.opf in zip");
    return false;
  }

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  LOG_DBG("EBP", "Parsing content.opf: %s", contentOpfFilePath.c_str());

  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    LOG_ERR("EBP", "Could not get size of content.opf");
    return false;
  }

  ContentOpfParser opfParser(getCachePath(), getBasePath(), contentOpfSize,
                             writeSpineEntries ? bookMetadataCache.get() : nullptr);
  if (!opfParser.setup()) {
    LOG_ERR("EBP", "Could not setup content.opf parser");
    return false;
  }

  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024)) {
    LOG_ERR("EBP", "Could not read content.opf");
    return false;
  }

  // Grab data from opfParser into epub. Normalize titles to NFC so NFD (combining
  // mark) text renders correctly — the device fonts have no mark positioning.
  bookMetadata.title = utf8ComposeNfc(opfParser.title);
  bookMetadata.author = opfParser.author;
  bookMetadata.language = opfParser.language;
  bookMetadata.coverItemHref = opfParser.coverItemHref;

  // Guide-based cover fallback: if no cover found via metadata/properties,
  // try extracting the image reference from the guide's cover page XHTML
  if (bookMetadata.coverItemHref.empty() && !opfParser.guideCoverPageHref.empty()) {
    LOG_DBG("EBP", "No cover from metadata, trying guide cover page: %s", opfParser.guideCoverPageHref.c_str());
    size_t coverPageSize;
    uint8_t* coverPageData = readItemContentsToBytes(opfParser.guideCoverPageHref, &coverPageSize, true);
    if (coverPageData) {
      const std::string coverPageHtml(reinterpret_cast<char*>(coverPageData), coverPageSize);
      free(coverPageData);

      // Determine base path of the cover page for resolving relative image references
      std::string coverPageBase;
      const auto lastSlash = opfParser.guideCoverPageHref.rfind('/');
      if (lastSlash != std::string::npos) {
        coverPageBase = opfParser.guideCoverPageHref.substr(0, lastSlash + 1);
      }

      // Search for image references: xlink:href="..." (SVG) and src="..." (img)
      std::string imageRef;
      for (const char* pattern : {"xlink:href=\"", "src=\""}) {
        auto pos = coverPageHtml.find(pattern);
        while (pos != std::string::npos) {
          pos += strlen(pattern);
          const auto endPos = coverPageHtml.find('"', pos);
          if (endPos != std::string::npos) {
            const auto ref = std::string_view{coverPageHtml}.substr(pos, endPos - pos);
            // Cover BMP generation supports JPG/PNG only; skip GIF so an unsupported wrapper image
            // does not block a later supported cover reference.
            if (FsHelpers::hasPngExtension(ref) || FsHelpers::hasJpgExtension(ref)) {
              imageRef = ref;
              break;
            }
          }
          pos = coverPageHtml.find(pattern, pos);
        }
        if (!imageRef.empty()) break;
      }

      if (!imageRef.empty()) {
        bookMetadata.coverItemHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(coverPageBase + imageRef));
        LOG_DBG("EBP", "Found cover image from guide: %s", bookMetadata.coverItemHref.c_str());
      }
    }
  }

  bookMetadata.textReferenceHref = opfParser.textReferenceHref;

  if (!opfParser.tocNcxPath.empty()) {
    tocNcxItem = opfParser.tocNcxPath;
  }

  if (!opfParser.tocNavPath.empty()) {
    tocNavItem = opfParser.tocNavPath;
  }

  if (!opfParser.cssFiles.empty()) {
    cssFiles = opfParser.cssFiles;
  }

  LOG_DBG("EBP", "Successfully parsed content.opf");
  return true;
}

bool Epub::parseTocNcxFile() const {
  // the ncx file should have been specified in the content.opf file
  if (tocNcxItem.empty()) {
    LOG_DBG("EBP", "No ncx file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc ncx file: %s", tocNcxItem.c_str());

  size_t ncxSize;
  if (!getItemSize(tocNcxItem, &ncxSize)) {
    LOG_ERR("EBP", "Could not get size of toc ncx file");
    return false;
  }

  TocNcxParser ncxParser(contentBasePath, ncxSize, bookMetadataCache.get());

  if (!ncxParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc ncx parser");
    return false;
  }

  // Stream the decompressed NCX straight into the parser instead of round-tripping
  // through a temp file on the SD card (decompress -> write -> reopen -> reread -> delete).
  if (!readItemContentsToStream(tocNcxItem, ncxParser, 1024)) {
    LOG_ERR("EBP", "Could not read toc ncx file");
    return false;
  }

  LOG_DBG("EBP", "Parsed TOC items");
  return true;
}

bool Epub::parseTocNavFile() const {
  // the nav file should have been specified in the content.opf file (EPUB 3)
  if (tocNavItem.empty()) {
    LOG_DBG("EBP", "No nav file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc nav file: %s", tocNavItem.c_str());

  size_t navSize;
  if (!getItemSize(tocNavItem, &navSize)) {
    LOG_ERR("EBP", "Could not get size of toc nav file");
    return false;
  }

  // Note: We can't use `contentBasePath` here as the nav file may be in a different folder to the content.opf
  // and the HTMLX nav file will have hrefs relative to itself
  const std::string navContentBasePath = tocNavItem.substr(0, tocNavItem.find_last_of('/') + 1);
  TocNavParser navParser(navContentBasePath, navSize, bookMetadataCache.get());

  if (!navParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc nav parser");
    return false;
  }

  // Stream the decompressed nav document straight into the parser instead of round-tripping
  // through a temp file on the SD card (decompress -> write -> reopen -> reread -> delete).
  if (!readItemContentsToStream(tocNavItem, navParser, 1024)) {
    LOG_ERR("EBP", "Could not read toc nav file");
    return false;
  }

  LOG_DBG("EBP", "Parsed TOC nav items");
  return true;
}

void Epub::discoverCssFilesFromZip() {
  const std::string& opfDir = contentBasePath;
  ZipFile zf(filepath);

  if (!zf.enumerateFilePaths([&](std::string_view filePath) {
        if (!opfDir.empty() && filePath.find(opfDir) != 0) {
          return;
        }

        if (!FsHelpers::hasCssExtension(filePath)) {
          return;
        }

        if (std::find(cssFiles.begin(), cssFiles.end(), filePath) != cssFiles.end()) {
          return;
        }

        LOG_DBG("EBP", "Discovered CSS file via ZIP enumeration: %.*s", (int)filePath.size(), filePath.data());
        cssFiles.push_back(std::string{filePath});
      })) {
    LOG_ERR("EBP", "Failed to enumerate ZIP file paths for CSS discovery");
  }
}

CssParser::ParseResult Epub::parseCssFiles(const CssParser::CacheStatus existingCacheStatus) const {
  // Maximum CSS file size we'll attempt to parse (uncompressed)
  // Larger files risk memory exhaustion on ESP32
  constexpr size_t MAX_CSS_FILE_SIZE = 128 * 1024;  // 128KB
  // Minimum heap required before attempting CSS parsing
  constexpr size_t MIN_HEAP_FOR_CSS_PARSING = 64 * 1024;  // 64KB

  if (cssFiles.empty()) {
    LOG_DBG("EBP", "No CSS files to parse, but CssParser created for inline styles");
  }

  LOG_DBG("EBP", "CSS files to parse: %zu", cssFiles.size());

  const bool hasPartialCache = existingCacheStatus == CssParser::CacheStatus::Partial;
  cssParser->clear();

  // Some converters emit one byte-identical stylesheet per chapter (100+ .css
  // entries), and each parse costs a zip locate plus an SD extract round-trip.
  // Match each normalized CSS path to its central-directory (CRC32,
  // compressed size) without throwing container allocations, then parse only
  // the first of each identical pair. If scratch allocation fails, parsing all
  // stylesheets is slower but remains correct.
  struct CssDedupEntry {
    uint64_t pathHash = 0;
    uint64_t contentKey = 0;
    size_t cssIndex = 0;
  };
  std::unique_ptr<CssDedupEntry[]> dedupEntries;
  if (cssFiles.size() > 1) {
    dedupEntries = makeUniqueNoThrow<CssDedupEntry[]>(cssFiles.size());
  }
  if (dedupEntries) {
    for (size_t i = 0; i < cssFiles.size(); i++) {
      dedupEntries[i].pathHash = ZipFile::fnvHash64(cssFiles[i].data(), cssFiles[i].size());
      dedupEntries[i].cssIndex = i;
    }
    std::sort(dedupEntries.get(), dedupEntries.get() + cssFiles.size(),
              [](const CssDedupEntry& lhs, const CssDedupEntry& rhs) { return lhs.pathHash < rhs.pathHash; });

    ZipFile(filepath).enumerateFileEntries([&](std::string_view entryPath, uint32_t crc32, uint32_t compressedSize) {
      if (!FsHelpers::hasCssExtension(entryPath)) {
        return;
      }

      const uint64_t pathHash = ZipFile::fnvHash64(entryPath.data(), entryPath.size());
      auto* match = std::lower_bound(
          dedupEntries.get(), dedupEntries.get() + cssFiles.size(), pathHash,
          [](const CssDedupEntry& candidate, const uint64_t hash) { return candidate.pathHash < hash; });
      for (const auto* end = dedupEntries.get() + cssFiles.size(); match != end && match->pathHash == pathHash;
           match++) {
        if (entryPath == cssFiles[match->cssIndex]) {
          match->contentKey = (static_cast<uint64_t>(crc32) << 32) | compressedSize;
          break;
        }
      }
    });
    std::sort(dedupEntries.get(), dedupEntries.get() + cssFiles.size(),
              [](const CssDedupEntry& lhs, const CssDedupEntry& rhs) { return lhs.cssIndex < rhs.cssIndex; });
  } else if (cssFiles.size() > 1) {
    LOG_ERR("EBP", "Insufficient heap for CSS deduplication; parsing every stylesheet");
  }

  size_t skippedDuplicates = 0;
  CssParser::ParseResult parseResult = CssParser::ParseResult::Complete;

  // No cache yet - parse CSS files
  for (size_t cssIndex = 0; cssIndex < cssFiles.size(); cssIndex++) {
    const auto& cssPath = cssFiles[cssIndex];
    const uint64_t dedupKey = dedupEntries ? dedupEntries[cssIndex].contentKey : 0;
    if (dedupKey != 0) {
      const bool seen =
          std::any_of(dedupEntries.get(), dedupEntries.get() + cssIndex,
                      [dedupKey](const CssDedupEntry& candidate) { return candidate.contentKey == dedupKey; });
      if (seen) {
        skippedDuplicates++;
        continue;
      }
    }
    LOG_DBG("EBP", "Parsing CSS file: %s", cssPath.c_str());

    // Check heap before parsing - CSS parsing allocates heavily
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_CSS_PARSING) {
      LOG_ERR("EBP", "Insufficient heap for CSS parsing (%u bytes free, need %zu), skipping: %s", freeHeap,
              MIN_HEAP_FOR_CSS_PARSING, cssPath.c_str());
      if (parseResult == CssParser::ParseResult::Complete) {
        parseResult = CssParser::ParseResult::Partial;
      }
      continue;
    }

    // Check CSS file size before decompressing - skip files that are too large
    size_t cssFileSize = 0;
    if (getItemSize(cssPath, &cssFileSize)) {
      if (cssFileSize > MAX_CSS_FILE_SIZE) {
        LOG_ERR("EBP", "CSS file too large (%zu bytes > %zu max), skipping: %s", cssFileSize, MAX_CSS_FILE_SIZE,
                cssPath.c_str());
        if (parseResult == CssParser::ParseResult::Complete) {
          parseResult = CssParser::ParseResult::Partial;
        }
        continue;
      }
    }

    // Extract CSS file to temp location
    const auto tmpCssPath = getCachePath() + "/.tmp.css";
    HalFile tempCssFile;
    if (!Storage.openFileForWrite("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not create temp CSS file");
      parseResult = CssParser::ParseResult::Error;
      continue;
    }
    if (!readItemContentsToStream(cssPath, tempCssFile, 1024)) {
      LOG_ERR("EBP", "Could not read CSS file: %s", cssPath.c_str());
      // Explicitly close() file before calling Storage.remove()
      tempCssFile.close();
      Storage.remove(tmpCssPath.c_str());
      parseResult = CssParser::ParseResult::Error;
      continue;
    }
    // Explicitly close() file before reopening for reading
    tempCssFile.close();

    // Parse the CSS file
    if (!Storage.openFileForRead("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not open temp CSS file for reading");
      Storage.remove(tmpCssPath.c_str());
      parseResult = CssParser::ParseResult::Error;
      continue;
    }
    const CssParser::ParseResult streamResult = cssParser->loadFromStream(tempCssFile);
    // Explicitly close() file before calling Storage.remove()
    tempCssFile.close();
    Storage.remove(tmpCssPath.c_str());
    if (streamResult == CssParser::ParseResult::Error) {
      parseResult = CssParser::ParseResult::Error;
    } else if (streamResult == CssParser::ParseResult::Partial && parseResult == CssParser::ParseResult::Complete) {
      parseResult = CssParser::ParseResult::Partial;
    }
  }

  if (parseResult == CssParser::ParseResult::Error) {
    LOG_ERR("EBP", "CSS parse failed; preserving any previous cache for a later retry");
    cssParser->clear();
    return parseResult;
  }

  if (parseResult == CssParser::ParseResult::Partial && cssParser->empty()) {
    LOG_ERR("EBP", "CSS parsing stopped before any usable rules were loaded; cache will not be replaced");
    cssParser->clear();
    return CssParser::ParseResult::Error;
  }

  if (parseResult == CssParser::ParseResult::Partial && hasPartialCache) {
    LOG_DBG("EBP", "CSS retry remained partial; preserving the previous partial cache");
    cssParser->clear();
    return parseResult;
  }

  // A partial cache remains useful for this session, but its header ensures a
  // later EPUB load retries the source stylesheets when more heap is available.
  if (!cssParser->saveToCache(parseResult == CssParser::ParseResult::Complete)) {
    LOG_ERR("EBP", "Failed to save CSS rules to cache");
    cssParser->clear();
    return CssParser::ParseResult::Error;
  }

  LOG_DBG("EBP", "Loaded %zu CSS style rules from %zu files (%zu identical duplicates skipped, %s)",
          cssParser->ruleCount(), cssFiles.size(), skippedDuplicates,
          parseResult == CssParser::ParseResult::Complete ? "complete" : "partial");
  cssParser->clear();
  return parseResult;
}

// load in the meta data for the epub file
bool Epub::load(const bool buildIfMissing, const bool skipLoadingCss) {
  LOG_DBG("EBP", "Loading ePub: %s", filepath.c_str());

  // Initialize spine/TOC cache
  bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  // Always create CssParser - needed for inline style parsing even without CSS files
  cssParser = makeUniqueNoThrow<CssParser>(cachePath);
  if (!bookMetadataCache || !cssParser) {
    LOG_ERR("EBP", "OOM: EPUB metadata helpers");
    return false;
  }

  // Try to load existing cache first
  if (bookMetadataCache->load()) {
    if (!skipLoadingCss) {
      const CssParser::CacheStatus cacheStatus = cssParser->inspectCache();
      CssParser::CacheLoadResult cacheLoadResult = CssParser::CacheLoadResult::Invalid;
      if (cacheStatus == CssParser::CacheStatus::Complete) {
        cacheLoadResult = cssParser->loadFromCache();
      }

      if (cacheLoadResult == CssParser::CacheLoadResult::LowMemory) {
        LOG_ERR("EBP", "Insufficient heap to load CSS cache; keeping it for a later retry");
      } else if (cacheLoadResult != CssParser::CacheLoadResult::Complete) {
        LOG_DBG("EBP", "CSS cache missing, partial, or invalid; attempting to parse source stylesheets");
        if (cacheStatus == CssParser::CacheStatus::Invalid ||
            (cacheStatus == CssParser::CacheStatus::Complete &&
             cacheLoadResult == CssParser::CacheLoadResult::Invalid)) {
          cssParser->deleteCache();
        }

        BookMetadataCache::BookMetadata cachedMetadata = bookMetadataCache->coreMetadata;
        CssParser::ParseResult cssParseResult = CssParser::ParseResult::Error;
        if (!parseContentOpf(cachedMetadata, /*writeSpineEntries=*/false)) {
          LOG_ERR("EBP", "Could not parse content.opf from cached bookMetadata for CSS files");
        } else {
          discoverCssFilesFromZip();
          bookMetadataCache.reset();
          cssParseResult = parseCssFiles(cacheStatus);
        }
        bookMetadataCache.reset();
        bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
        if (!bookMetadataCache) {
          LOG_ERR("EBP", "OOM: BookMetadataCache (%u bytes)", static_cast<unsigned>(sizeof(BookMetadataCache)));
          return false;
        }
        if (!bookMetadataCache->load()) {
          LOG_ERR("EBP", "Failed to reload cache after CSS rebuild");
          return false;
        }
        const bool cssCacheChanged =
            cssParseResult == CssParser::ParseResult::Complete ||
            (cssParseResult == CssParser::ParseResult::Partial && cacheStatus != CssParser::CacheStatus::Partial);
        if (cssCacheChanged) {
          // The CSS cache changed, so section caches must use the same rule set.
          Storage.removeDir((cachePath + "/sections").c_str());
        }
      }
    }
    // Release the resolved CSS rule map: it is only needed transiently while building
    // section caches, and createSectionFile reloads it from cache on demand. Holding it
    // resident pins tens of KB for the whole reading session (more on warm resume into
    // an already-cached chapter, where createSectionFile never runs to clear it).
    cssParser->clear();
    LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
    return true;
  }

  // If we didn't load from cache above and we aren't allowed to build, fail now
  if (!buildIfMissing) {
    return false;
  }

  // Cache doesn't exist or is invalid, build it
  LOG_DBG("EBP", "Cache not found, building spine/TOC cache");
  setupCacheDir();

  const uint32_t indexingStart = millis();

  // Begin building cache - stream entries to disk immediately
  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("EBP", "Could not begin writing cache");
    return false;
  }

  // OPF Pass
  const uint32_t opfStart = millis();
  BookMetadataCache::BookMetadata bookMetadata;
  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("EBP", "Could not begin writing content.opf pass");
    return false;
  }
  if (!parseContentOpf(bookMetadata)) {
    LOG_ERR("EBP", "Could not parse content.opf");
    return false;
  }
  discoverCssFilesFromZip();
  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("EBP", "Could not end writing content.opf pass");
    return false;
  }
  LOG_DBG("EBP", "OPF pass completed in %lu ms", millis() - opfStart);

  // TOC Pass - try EPUB 3 nav first, fall back to NCX
  const uint32_t tocStart = millis();
  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("EBP", "Could not begin writing toc pass");
    return false;
  }

  bool tocParsed = false;

  // Try EPUB 3 nav document first (preferred)
  if (!tocNavItem.empty()) {
    LOG_DBG("EBP", "Attempting to parse EPUB 3 nav document");
    tocParsed = parseTocNavFile();
  }

  // Fall back to NCX if nav parsing failed or wasn't available
  if (!tocParsed && !tocNcxItem.empty()) {
    LOG_DBG("EBP", "Falling back to NCX TOC");
    tocParsed = parseTocNcxFile();
  }

  if (!tocParsed) {
    LOG_ERR("EBP", "Warning: Could not parse any TOC format");
    // Continue anyway - book will work without TOC
  }

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("EBP", "Could not end writing toc pass");
    return false;
  }
  LOG_DBG("EBP", "TOC pass completed in %lu ms", millis() - tocStart);

  // Close the cache files
  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("EBP", "Could not end writing cache");
    return false;
  }

  // Build final book.bin
  const uint32_t buildStart = millis();
  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    LOG_ERR("EBP", "Could not update mappings and sizes");
    return false;
  }
  LOG_DBG("EBP", "buildBookBin completed in %lu ms", millis() - buildStart);
  LOG_DBG("EBP", "Total indexing completed in %lu ms", millis() - indexingStart);

  if (!bookMetadataCache->cleanupTmpFiles()) {
    LOG_DBG("EBP", "Could not cleanup tmp files - ignoring");
  }

  if (!skipLoadingCss) {
    // Parse CSS before reloading book.bin to leave more heap for CSS rule-table growth.
    bookMetadataCache.reset();
    if (parseCssFiles(cssParser->inspectCache()) != CssParser::ParseResult::Error) {
      Storage.removeDir((cachePath + "/sections").c_str());
    }
  }

  // Reload the cache from disk so it's in the correct state
  bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  if (!bookMetadataCache) {
    LOG_ERR("EBP", "OOM: BookMetadataCache (%u bytes)", static_cast<unsigned>(sizeof(BookMetadataCache)));
    return false;
  }
  if (!bookMetadataCache->load()) {
    LOG_ERR("EBP", "Failed to reload cache after writing");
    return false;
  }

  LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
  return true;
}

bool Epub::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("EPB", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("EPB", "Failed to clear cache");
    return false;
  }

  LOG_DBG("EPB", "Cache cleared successfully");
  return true;
}

void Epub::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  Storage.mkdir(cachePath.c_str());
}

const std::string& Epub::getCachePath() const { return cachePath; }

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.title;
}

const std::string& Epub::getAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.author;
}

const std::string& Epub::getLanguage() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.language;
}

std::string Epub::getCoverOverridePath() const { return cachePath + "/cover.override"; }

bool Epub::hasCoverOverride() const { return coverImageType(getCoverOverridePath()) != CoverImageType::None; }

std::string Epub::getCoverBmpPath(bool cropped) const {
  // v2 = 8 位灰阶封面（旧的是 2 位 + Atkinson 抖动）。名字必须换：generateCoverBmp()
  // 见到文件已存在就直接返回，不改名的话老机器上永远用着那张脏封面。
  // / v2 = 8-bit grayscale cover (the old one was 2-bit + Atkinson dithering). The name
  // has to change: generateCoverBmp() returns early when the file exists, so without a
  // bump an upgraded device would keep serving the old blotchy cover forever.
  const auto coverFileName = std::string("cover_v2") + (cropped ? "_crop" : "");
  return cachePath + "/" + coverFileName + ".bmp";
}

bool Epub::generateCoverBmp(bool cropped) const {
  // Already generated, return true
  if (Storage.exists(getCoverBmpPath(cropped).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate cover BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (FsHelpers::hasJpgExtension(coverImageHref) || FsHelpers::hasPngExtension(coverImageHref)) {
    // 一律 8 位灰阶：2 位 + 抖动的那张到货架上会被降采样成脏斑（见
    // JpegToBmpConverter::jpegFileToBmpStream 的注释）。
    // / Always 8-bit gray: the 2-bit dithered variant turns to blotches the moment the
    // shelf downsamples it (see JpegToBmpConverter::jpegFileToBmpStream).
    const std::string outputPath = getCoverBmpPath(cropped);
    if (convertExtractedCover(
            *this, coverImageHref, outputPath, cropped ? "cropped cover" : "cover",
            [cropped](HalFile& source, HalFile& output) {
              return JpegToBmpConverter::jpegFileToBmpStream(source, output, cropped, JpegToBmpConverter::Output::Gray8);
            },
            [cropped](HalFile& source, HalFile& output) {
              return PngToBmpConverter::pngFileToBmpStream(source, output, cropped);
            })) {
      return true;
    }
  }

  const std::string outputPath = getCoverBmpPath(cropped);
  return convertOverrideCover(
      *this, outputPath, cropped ? "cropped cover" : "cover",
      [cropped](HalFile& source, HalFile& output) {
        return JpegToBmpConverter::jpegFileToBmpStream(source, output, cropped, JpegToBmpConverter::Output::Gray8);
      },
      [cropped](HalFile& source, HalFile& output) {
        return PngToBmpConverter::pngFileToBmpStream(source, output, cropped);
      });
}

bool Epub::generateStandbyCoverBmp(const std::string& outputPath, int maxW, int maxH) const {
  if (outputPath.empty() || maxW <= 0 || maxH <= 0) return false;
  // 已经生成过就直接认（调用方也判，这里再判一次省得重复解一张大图）。
  if (Storage.exists(outputPath.c_str())) return true;

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate standby cover BMP, cache not loaded");
    return false;
  }

  // crop=false 是 fit：整张封面都要看得见。待机画面本来就是"一屏只放这张封面"，
  // 裁掉两边去填满屏幕会切掉封面的书名/作者。Gray8 与书架那张同一个口径。
  // / crop=false = fit: the standby screen shows the whole cover, and cropping to fill
  // would cut the title/author off the cover. Gray8, same as the shelf cover.
  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (FsHelpers::hasJpgExtension(coverImageHref) || FsHelpers::hasPngExtension(coverImageHref)) {
    if (convertExtractedCover(
            *this, coverImageHref, outputPath, "standby cover",
            [maxW, maxH](HalFile& source, HalFile& output) {
              return JpegToBmpConverter::jpegFileToBmpStreamWithSize(source, output, maxW, maxH,
                                                                    JpegToBmpConverter::Output::Gray8, false);
            },
            [maxW, maxH](HalFile& source, HalFile& output) {
              return PngToBmpConverter::pngFileToBmpStreamWithSize(source, output, maxW, maxH, true, false);
            })) {
      return true;
    }
  }
  return convertOverrideCover(
      *this, outputPath, "standby cover",
      [maxW, maxH](HalFile& source, HalFile& output) {
        return JpegToBmpConverter::jpegFileToBmpStreamWithSize(source, output, maxW, maxH,
                                                              JpegToBmpConverter::Output::Gray8, false);
      },
      [maxW, maxH](HalFile& source, HalFile& output) {
        return PngToBmpConverter::pngFileToBmpStreamWithSize(source, output, maxW, maxH, true, false);
      });
}

std::string Epub::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Epub::getThumbBmpPath(int height) const { return cachePath + "/thumb_" + std::to_string(height) + ".bmp"; }

bool Epub::generateThumbBmp(int height) const {
  // Already generated, return true
  if (Storage.exists(getThumbBmpPath(height).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate thumb BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  const int targetWidth = height * 3 / 5;
  const std::string outputPath = getThumbBmpPath(height);
  if (FsHelpers::hasJpgExtension(coverImageHref) || FsHelpers::hasPngExtension(coverImageHref)) {
    if (convertExtractedCover(
            *this, coverImageHref, outputPath, "thumbnail",
            [targetWidth, height](HalFile& source, HalFile& output) {
              return JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(source, output, targetWidth, height);
            },
            [targetWidth, height](HalFile& source, HalFile& output) {
              return PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(source, output, targetWidth, height);
            })) {
      return true;
    }
  }
  if (convertOverrideCover(
          *this, outputPath, "thumbnail",
          [targetWidth, height](HalFile& source, HalFile& output) {
            return JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(source, output, targetWidth, height);
          },
          [targetWidth, height](HalFile& source, HalFile& output) {
            return PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(source, output, targetWidth, height);
          })) {
    return true;
  }

  // Write an empty bmp file to avoid generation attempts in the future
  HalFile thumbBmp;
  Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp);
  return false;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, const bool trailingNullByte) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return nullptr;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);

  const auto content = ZipFile(filepath).readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    LOG_DBG("EBP", "Failed to read item %s", path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize,
                                    const bool allowEarlyStop) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return false;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).readFileToStream(path.c_str(), out, chunkSize, allowEarlyStop);
}

bool Epub::extractItemToFile(const std::string& itemHref, const std::string& destPath) const {
  HalFile out;
  if (!Storage.openFileForWrite("EBP", destPath, out)) {
    return false;
  }
  // Large images dominate lazy extraction. Match the section streamer size to
  // halve SD read/write calls while adding only 8 KB of transient ZIP buffers.
  const bool ok = readItemContentsToStream(itemHref, out, 8192);
  out.flush();
  out.close();
  if (!ok) {
    Storage.remove(destPath.c_str());
  }
  return ok;
}

Epub::EmbeddedFontSet Epub::resolveEmbeddedFonts() {
  EmbeddedFontSet set;
  EmbeddedFont& font = set.primary;
  setupCacheDir();
  const std::string cacheFile = cachePath + "/book_font.txt";

  // Ask the per-book cache first. This runs on every open and the answer is stable
  // for a given file, so the common cases — no font at all, or one already resolved
  // — must not pay for a ZIP enumeration. Format (v4):
  //   <href>\n<size>\n<family>\n  ×2  (primary, then alt)
  // with an empty primary href meaning the book ships no usable body font.
  // v1 -> v2: the selector tiers below changed which books resolve at all, so every
  // cached "none" from v1 is suspect and must be re-scanned (the entry is rewritten
  // in place on the next open).
  // v2 -> v3: the ZIP item lookup went case-insensitive-with-exact-preference, so
  // books whose CSS references a differently-cased path (e.g. ../Fonts/ vs the
  // archive's OEBPS/fonts/) now resolve — every cached "none" from v2 is likewise
  // suspect. Bump this whenever the matching rules change.
  // v3 -> v4: 多出一个次家族（见 isDecorativeFamilySelector/tallyAltFamily）。
  {
    bool cacheOk = false;
    const std::string cached = Storage.readFile(cacheFile.c_str(), &cacheOk);
    if (cacheOk && cached.rfind("v4\n", 0) == 0) {
      std::vector<std::string> lines;
      for (size_t start = 3; start <= cached.size();) {
        size_t nl = cached.find('\n', start);
        if (nl == std::string::npos) nl = cached.size();
        lines.push_back(cached.substr(start, nl - start));
        if (nl == cached.size()) break;
        start = nl + 1;
      }
      if (lines.size() >= 6) {
        font.itemHref = lines[0];
        font.size = static_cast<size_t>(strtoul(lines[1].c_str(), nullptr, 10));
        font.family = lines[2];
        set.alt.itemHref = lines[3];
        set.alt.size = static_cast<size_t>(strtoul(lines[4].c_str(), nullptr, 10));
        set.alt.family = lines[5];
        LOG_DBG("EBP", "Embedded body font (cached): %s%s%s", font.itemHref.empty() ? "none" : font.itemHref.c_str(),
                set.alt.itemHref.empty() ? "" : " + alt ", set.alt.itemHref.c_str());
        return set;
      }
    }
  }

  // Cache miss: scan the book's stylesheets. cssFiles is only filled by the CSS parse
  // path, which a warm book.bin load skips, so enumerate on demand.
  if (cssFiles.empty()) {
    discoverCssFilesFromZip();
  }

  CssFontCandidates candidates;
  constexpr size_t kMaxCssFiles = 8;  // bound the transient heap; real books ship one or two
  for (size_t i = 0; i < cssFiles.size() && i < kMaxCssFiles; ++i) {
    size_t cssSize = 0;
    uint8_t* raw = readItemContentsToBytes(cssFiles[i], &cssSize, /*trailingNullByte=*/false);
    if (!raw) continue;
    const ScopedCleanup releaseRaw{[raw] { free(raw); }};
    const std::string css = stripCssComments(std::string(reinterpret_cast<const char*>(raw), cssSize));
    scanCssRules(css, 0, css.size(), cssFiles[i], &candidates);
  }

  // Flatten the tiers into one preference-ordered family list: body/html rules first,
  // then bare <p>, then bare <div>, document order inside each. Duplicates collapse so
  // "p { st,st2,st3 }" followed by "p.sth { st }" does not retry st at the tail.
  std::vector<std::string> wantedFamilies;
  for (int tier = 0; tier < 3; ++tier) {
    for (const std::vector<std::string>& list : candidates.tierFamilies[tier]) {
      for (const std::string& name : list) {
        bool seen = false;
        for (const std::string& have : wantedFamilies) {
          if (have == name) {
            seen = true;
            break;
          }
        }
        if (!seen) wantedFamilies.push_back(name);
      }
    }
  }

  // Walk those families against the @font-face table in the book's own preference
  // order, so "«Songti», serif" and "serif, «Songti»" both land on Songti. A family
  // whose @font-face is missing, has no usable url(), or points at a file the archive
  // does not actually ship is skipped for the next one rather than aborting the search:
  // books routinely declare a dozen faces and only bundle two of them.
  const CssFontFace* matched = nullptr;
  for (const std::string& wanted : wantedFamilies) {
    for (const CssFontFace& face : candidates.faces) {
      if (face.family != wanted) continue;
      std::string url;
      if (!pickFontUrl(face.src, &url)) break;
      std::string href = resolveFontHref(face.cssHref, url);
      size_t size = 0;
      if (!href.empty() && getItemSize(href, &size) && size > 0) {
        font.itemHref = std::move(href);
        font.size = size;
        font.family = face.family;
        matched = &face;
      } else {
        // 家族在 CSS 里声明了、url 也解出来了，但包里没有这个文件。打一行：这是"书内
        // 字体没生效"最常见的成因（CSS 写 ../Fonts/ 而包里是 fonts/，或干脆没随书打包）。
        // 只在缓存未命中时发生（每本书一次），所以不会刷屏。
        // / The family has a face and a url, but the archive does not ship that path.
        LOG_INF("EBP", "font family '%s': '%s' not in archive", face.family.c_str(), href.c_str());
      }
      break;  // one face per family name; move on to the next family either way
    }
    if (matched) break;
  }

  // 次家族：候选按引用次数降序（同数保持先出现的在前），跳过正文家族、跳过解不出文件的、
  // 跳过跟正文指向同一个文件的 —— 同一份字面装两遍只是白占一份 PSRAM。
  std::vector<std::pair<std::string, int>> ranked = candidates.altFamilies;
  std::stable_sort(ranked.begin(), ranked.end(),
                   [](const std::pair<std::string, int>& a, const std::pair<std::string, int>& b) {
                     return a.second > b.second;
                   });
  for (const auto& entry : ranked) {
    if (entry.first == font.family) continue;  // 就是正文字面
    const CssFontFace* face = nullptr;
    for (const CssFontFace& candidate : candidates.faces) {
      if (candidate.family == entry.first) {
        face = &candidate;
        break;
      }
    }
    if (!face) continue;  // 只有家族名、没有 @font-face
    std::string url;
    if (!pickFontUrl(face->src, &url)) continue;
    std::string href = resolveFontHref(face->cssHref, url);
    size_t size = 0;
    if (href.empty() || href == font.itemHref || !getItemSize(href, &size) || size == 0) continue;
    set.alt.itemHref = std::move(href);
    set.alt.size = size;
    set.alt.family = face->family;
    LOG_INF("EBP", "次家族 '%s' → %s (%u refs, %u bytes)", set.alt.family.c_str(), set.alt.itemHref.c_str(),
            static_cast<unsigned>(entry.second), static_cast<unsigned>(set.alt.size));
    break;
  }

  if (font.itemHref.empty()) {
    LOG_DBG("EBP", "Embedded body font: none");
  } else {
    LOG_DBG("EBP", "Embedded body font: family '%s' -> %s (%u bytes)", font.family.c_str(), font.itemHref.c_str(),
            static_cast<unsigned>(font.size));
  }
  if (set.alt.itemHref.empty()) {
    LOG_DBG("EBP", "Embedded alt font: none");
  }

  std::string cacheContent = "v4\n";
  const auto appendFont = [&cacheContent](const EmbeddedFont& f) {
    cacheContent += f.itemHref;
    cacheContent += '\n';
    cacheContent += std::to_string(f.size);
    cacheContent += '\n';
    cacheContent += f.family;
    cacheContent += '\n';
  };
  appendFont(font);
  appendFont(set.alt);
  Storage.writeFile(cacheFile.c_str(), cacheContent);
  return set;
}

std::string Epub::extractEmbeddedFont(const EmbeddedFont& font, const char* fileName) const {
  if (font.itemHref.empty() || font.size == 0) return {};

  const std::string fontsDir = cachePath + "/fonts";
  Storage.mkdir(fontsDir.c_str(), true);
  const std::string localPath = fontsDir + "/" + fileName;

  // Re-extract only when the cached copy is missing or the wrong length, so reopening
  // a book (or reopening for a settings change) costs one stat, not a ZIP inflate.
  HalFile existing;
  if (Storage.openFileForRead("EBP", localPath, existing)) {
    const size_t have = existing.fileSize();
    existing.close();
    if (have == font.size) return localPath;
  }

  if (!extractItemToFile(font.itemHref, localPath)) {
    LOG_ERR("EBP", "Failed to extract embedded font %s", font.itemHref.c_str());
    return {};
  }
  LOG_DBG("EBP", "Extracted embedded font %s -> %s (%u bytes)", font.itemHref.c_str(), localPath.c_str(),
          static_cast<unsigned>(font.size));
  return localPath;
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).getInflatedFileSize(path.c_str(), size);
}

int Epub::getSpineItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  return bookMetadataCache->getSpineCount();
}

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  uint32_t cumulativeSize = 0;
  if (bookMetadataCache->getCumulativeSize(spineIndex, cumulativeSize)) return cumulativeSize;
  return getSpineItem(spineIndex).cumulativeSize;
}

BookMetadataCache::SpineEntry Epub::getSpineItem(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineItem called but cache not loaded");
    return {};
  }

  if (spineIndex < 0 || spineIndex >= bookMetadataCache->getSpineCount()) {
    LOG_ERR("EBP", "getSpineItem index:%d is out of range", spineIndex);
    return bookMetadataCache->getSpineEntry(0);
  }

  return bookMetadataCache->getSpineEntry(spineIndex);
}

BookMetadataCache::TocEntry Epub::getTocItem(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_DBG("EBP", "getTocItem called but cache not loaded");
    return {};
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_DBG("EBP", "getTocItem index:%d is out of range", tocIndex);
    return {};
  }

  return bookMetadataCache->getTocEntry(tocIndex);
}

int Epub::getTocItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }

  return bookMetadataCache->getTocCount();
}

// work out the section index for a toc index
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex called but cache not loaded");
    return 0;
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex: tocIndex %d out of range", tocIndex);
    return 0;
  }

  const int spineIndex = bookMetadataCache->getTocEntry(tocIndex).spineIndex;
  if (spineIndex < 0) {
    LOG_DBG("EBP", "Section not found for TOC index %d", tocIndex);
    return 0;
  }

  return spineIndex;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const { return getSpineItem(spineIndex).tocIndex; }

size_t Epub::getBookSize() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || bookMetadataCache->getSpineCount() == 0) {
    return 0;
  }
  return getCumulativeSpineItemSize(getSpineItemsCount() - 1);
}

int Epub::getSpineIndexForTextReference() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTextReference called but cache not loaded");
    return 0;
  }
  LOG_DBG("EBP", "Core Metadata: cover(%d)=%s, textReference(%d)=%s",
          bookMetadataCache->coreMetadata.coverItemHref.size(), bookMetadataCache->coreMetadata.coverItemHref.c_str(),
          bookMetadataCache->coreMetadata.textReferenceHref.size(),
          bookMetadataCache->coreMetadata.textReferenceHref.c_str());

  if (bookMetadataCache->coreMetadata.textReferenceHref.empty()) {
    // there was no textReference in epub, so we return 0 (the first chapter)
    return 0;
  }

  // loop through spine items to get the correct index matching the text href
  for (size_t i = 0; i < getSpineItemsCount(); i++) {
    if (getSpineItem(i).href == bookMetadataCache->coreMetadata.textReferenceHref) {
      LOG_DBG("EBP", "Text reference %s found at index %d", bookMetadataCache->coreMetadata.textReferenceHref.c_str(),
              i);
      return i;
    }
  }
  // This should not happen, as we checked for empty textReferenceHref earlier
  LOG_DBG("EBP", "Section not found for text reference");
  return 0;
}

// Calculate progress in book (returns 0.0-1.0)
float Epub::calculateProgress(const int currentSpineIndex, const float currentSpineRead) const {
  const size_t bookSize = getBookSize();
  if (bookSize == 0) {
    return 0.0f;
  }
  const size_t prevChapterSize = (currentSpineIndex >= 1) ? getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t curChapterSize = getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  const float sectionProgSize = currentSpineRead * static_cast<float>(curChapterSize);
  const float totalProgress = static_cast<float>(prevChapterSize) + sectionProgSize;
  return totalProgress / static_cast<float>(bookSize);
}

int Epub::resolveHrefToSpineIndex(const std::string& href) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return -1;

  // Split before decoding so escaped '#' characters in filenames stay part of the path.
  const size_t hashPos = href.find('#');
  const std::string rawTarget = hashPos != std::string::npos ? href.substr(0, hashPos) : href;
  const std::string target = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(rawTarget));

  // Same-file reference (anchor-only)
  if (target.empty()) return -1;

  // Extract just the filename for comparison
  size_t targetSlash = target.find_last_of('/');
  std::string targetFilename = (targetSlash != std::string::npos) ? target.substr(targetSlash + 1) : target;

  for (int i = 0; i < getSpineItemsCount(); i++) {
    const auto& spineHref = getSpineItem(i).href;
    // Try exact match first
    if (spineHref == target) return i;
    // Then filename-only match
    size_t spineSlash = spineHref.find_last_of('/');
    std::string spineFilename = (spineSlash != std::string::npos) ? spineHref.substr(spineSlash + 1) : spineHref;
    if (spineFilename == targetFilename) return i;
  }
  return -1;
}
