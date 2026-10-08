#pragma once

#include <Print.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class ZipFile;

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;

  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  void discoverCssFilesFromZip();
  CssParser::ParseResult parseCssFiles(CssParser::CacheStatus existingCacheStatus) const;

 public:
  // A font the book itself ships and its CSS asks for. See resolveEmbeddedFonts for
  // which families get picked (the body-text one, plus one more per-element family).
  struct EmbeddedFont {
    std::string itemHref;  // path inside the ZIP; empty when the book supplies no usable font
    std::string family;    // the CSS family name, for logging
    size_t size = 0;       // inflated size of the font file, 0 when itemHref is empty
  };

  // 一本书最多认三份字面：primary 画正文，alt / alt2 画样式表里另指了家族的段落
  // （祖堂集：正文宋体 st，注文/引文仿宋 fs）。每份都是一整套字面 + 自己的字形缓存
  // （设备上每份净增几十 KB 常驻 + ≤384KB 按需字形位图，见 ttf_font.h 的角色说明），
  // 所以到第三份为止；第四个及以后的家族照旧落到 primary。
  struct EmbeddedFontSet {
    EmbeddedFont primary;
    EmbeddedFont alt;
    EmbeddedFont alt2;
  };

  explicit Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
    // create a cache key based on the filepath
    cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  std::string getCoverOverridePath() const;
  bool hasCoverOverride() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  bool generateCoverBmp(bool cropped = false) const;
  // 待机整屏封面：拿书里的**原图**按 (maxW,maxH) 解一次（8 位灰阶 + fit，只缩不放），
  // 输出路径由调用方给（阅读器的待机缓存路径，见 screen_reader.cpp 的
  // standbyCoverPathFor）。和 generateCoverBmp 的区别只有两点：目标尺寸不再是那个
  // 396×528 的书架盒子，名字也不再是 cover_v3.bmp —— 那张是给货架格子用的缩略图，
  // 待机整屏拿它放大就是用户说的"待机封面糊"。
  // / Full-screen standby cover generated from the book's ORIGINAL cover image at
  // (maxW,maxH): 8-bit gray, fit, never upscaled. Separate from generateCoverBmp
  // because that one targets the 396×528 shelf box and its cover_v3.bmp name.
  bool generateStandbyCoverBmp(const std::string& outputPath, int maxW, int maxH) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  bool generateThumbBmp(int height) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize,
                                bool allowEarlyStop = false) const;
  // Extract an item to a file on SD. On failure the partial file is removed.
  bool extractItemToFile(const std::string& itemHref, const std::string& destPath) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  // Find the fonts the book's own CSS nominates, for the reader to load while this
  // book is open. The answer is cached per book (see the .cache dir), so repeat opens
  // — and the far more common "book ships no font" case — cost one small file read
  // instead of a ZIP scan.
  //
  // primary: only @font-face families actually named by a body-text `font-family`
  // rule — body/html first, then a bare <p>, then a bare <div> (plenty of Chinese
  // EPUBs style the prose on `p` and never mention body at all). Rules that put a
  // class/id on the element ("p.duokan-image-subtitle", "div.sgc-toc-title") are
  // ignored here: a decorative face used for captions or a TOC must not be applied
  // to the whole book.
  //
  // alt / alt2: the next faces, taken from the per-element rules the body tiers ignore
  // — the families referenced most often by them, minus the genuinely one-off bits
  // (sup/sub/rt, toc, captions; see isNegligibleFamilySelector). Headings (h1..h6) ARE
  // counted: a typical book is exactly body + notes/quotes + headings, which fills all
  // three slots. 祖堂集 is the case that motivated alt: body 宋体, annotations/quotes
  // 仿宋; the heading family fills alt2 the same way. Rendering one means a per-word face switch
  // (EpdFontFamily::ALT_FONT / ALT2_FONT), not another layout engine, so exactly two
  // extra families can be honoured; a fourth falls back to primary. The two are
  // always distinct families resolving to distinct files (see the ranking below).
  // (not const: a cache miss populates cssFiles from the ZIP, same as load() does)
  EmbeddedFontSet resolveEmbeddedFonts();
  // Copy the resolved font out of the ZIP into the book's cache dir and return the
  // path on SD, or an empty string on failure (the caller keeps the user's font).
  // fileName distinguishes the faces: primary/alt/alt2 must land in different files
  // (they are different fonts) and each must be length-stable across opens so the
  // re-extract short-circuit works for each.
  std::string extractEmbeddedFont(const EmbeddedFont& font, const char* fileName = "book.ttf") const;

  size_t getBookSize() const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;
};
