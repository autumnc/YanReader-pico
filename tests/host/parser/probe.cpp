// Host-side probe: run the REAL ChapterHtmlSlimParser on one .xhtml and dump, per page,
// the footnote table, the link rects and the anchor table.
//
//   probe <file.xhtml> [fontId] [width] [height]
//
// Answers "did this parser change silently alter link/footnote registration?" without a
// device. CWD-independent; the CssParser cache dir comes from $PROBE_CACHE_DIR (default
// "."), and no CSS is ever loaded (the parser is handed an empty stylesheet path), so no
// cache file is written in practice.
#include <Arduino.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "Epub/parsers/ChapterHtmlSlimParser.h"

extern std::vector<std::string> collectedFootnotes;
extern std::vector<std::string> collectedNoteLabels;
extern std::vector<std::string> collectedLinks;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: probe <file.xhtml> [fontId] [width] [height]\n");
    return 2;
  }
  const std::string filepath = argv[1];
  const int fontId = (argc > 2) ? std::atoi(argv[2]) : 0;
  GfxRenderer renderer;
  const char* cacheDir = std::getenv("PROBE_CACHE_DIR");
  CssParser cssParser{std::string(cacheDir ? cacheDir : ".")};
  const uint16_t w = (argc > 3) ? static_cast<uint16_t>(std::atoi(argv[3])) : 684;
  const uint16_t h = (argc > 4) ? static_cast<uint16_t>(std::atoi(argv[4])) : 1000;

  int pageNo = 0;
  size_t seenFn = 0, seenLk = 0;
  ChapterHtmlSlimParser parser{
      nullptr,
      filepath,
      renderer,
      fontId,
      1.0f,
      false,
      false,
      0,
      w,
      h,
      false,
      false,
      [&](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t) {
        std::printf("==== page %d ====\n", pageNo++);
        for (size_t i = seenFn; i < collectedFootnotes.size(); ++i)
          std::printf("   footnote  num='%s'  href='%s'\n", collectedNoteLabels[i].c_str(),
                      collectedFootnotes[i].c_str());
        for (size_t i = seenLk; i < collectedLinks.size(); ++i)
          std::printf("   link      href='%s'\n", collectedLinks[i].c_str());
        seenFn = collectedFootnotes.size();
        seenLk = collectedLinks.size();
      },
      true,
      "",
      "",
      0,
      {},
      nullptr,
      &cssParser,
      true};

  if (!parser.parseAndBuildPages()) {
    std::fprintf(stderr, "parse failed\n");
    return 1;
  }
  std::printf("==== tail (after last page) ====\n");
  for (size_t i = seenFn; i < collectedFootnotes.size(); ++i)
    std::printf("   footnote  num='%s'  href='%s'\n", collectedNoteLabels[i].c_str(), collectedFootnotes[i].c_str());
  for (size_t i = seenLk; i < collectedLinks.size(); ++i)
    std::printf("   link      href='%s'\n", collectedLinks[i].c_str());

  std::printf("\ntotal: %zu footnote entries, %zu link rects, %d pages\n", collectedFootnotes.size(),
              collectedLinks.size(), pageNo);

  std::printf("\n==== anchors (%zu) ====\n", parser.getAnchors().size());
  for (const auto& a : parser.getAnchors())
    std::printf("   anchor '%s' -> page %u element %u\n", a.id.c_str(), (unsigned)a.page, (unsigned)a.element);
  return 0;
}
