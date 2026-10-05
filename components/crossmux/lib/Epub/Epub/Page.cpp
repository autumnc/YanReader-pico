#include "Page.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <new>

namespace {

template <typename Predicate>
void renderFilteredPageElements(const std::vector<std::unique_ptr<PageElement>>& elements, GfxRenderer& renderer,
                                const int fontId, const int xOffset, const int yOffset, Predicate&& predicate) {
  for (const auto& element : elements) {
    if (predicate(*element)) {
      element->render(renderer, fontId, xOffset, yOffset);
    }
  }
}

}  // namespace

void Page::addFootnote(const char* number, const std::string& href) {
  if (footnotes.size() >= MAX_FOOTNOTES_PER_PAGE || footnotes.allocationFailed()) return;

  auto* entry = footnotes.append();
  if (!entry) {
    LOG_ERR("PGE", "OOM: footnote storage (%u bytes)",
            static_cast<unsigned>(FootnoteList::MAX_SIZE * sizeof(FootnoteEntry)));
    return;
  }
  strncpy(entry->number, number, sizeof(entry->number) - 1);
  entry->number[sizeof(entry->number) - 1] = '\0';
  // 不截断：href 是 std::string（普通锚点几十字节，"alt:" 哨兵的整段注文最长见过 ~2.8 KB）
  entry->href = href;
}

bool Page::addLink(const char* href, const int16_t x, const int16_t y, const int16_t width, const int16_t height) {
  if (!href || width <= 0 || height <= 0 || links.size() >= MAX_LINKS_PER_PAGE) return false;
  const size_t hrefLen = strnlen(href, sizeof(PageLink::href));
  if (hrefLen == 0 || hrefLen == sizeof(PageLink::href)) return false;

  auto* link = links.append();
  if (!link) {
    if (links.allocationFailed()) {
      LOG_ERR("PGE", "OOM: page link storage (%u bytes)",
              static_cast<unsigned>(PageLinkList::MAX_SIZE * sizeof(PageLink)));
    }
    return false;
  }
  memcpy(link->href, href, hrefLen + 1);
  link->x = x;
  link->y = y;
  link->width = width;
  link->height = height;
  return true;
}

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  // yPos 是**行顶**（排版游标 currentPageNextY：按行高逐行推进、页高预算按它算、PageLink
  // 矩形也以它为上沿），而 TextBlock::render 收的 y 是**基线** —— 本移植版的 ttf 渲染器
  // 直接把它当基线（`ttf_draw_text_px`，同 TextBlock.cpp 装饰线那段注释）。所以这里必须
  // 补一跳 ascender，少了它整页文字被抬高一整个 ascender：正文只表现为"顶边留白比标称的
  // 少 40px"（不显眼），字号大的标题就直接顶出屏幕被切掉 —— 晋书 h1 经 CSS 阶梯顶到
  // 64px（ascender≈56），首行基线 50 再减 56 就是 -6，那 6px 正是被截掉的部分。
  // 用**这一行自己的**字体号：CSS 放大过的标题 ascender 更大，差这一档就白修了。
  const int lineFontId = block->renderFontId() ? block->renderFontId() : fontId;
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset + renderer.getFontAscenderSize(lineFontId));
}

bool PageLine::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(HalFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  if (!serialization::readPod(file, xPos) || !serialization::readPod(file, yPos)) return nullptr;

  auto tb = TextBlock::deserialize(file);
  if (!tb) {
    LOG_ERR("PGE", "Deserialization failed: null TextBlock");
    return nullptr;
  }

  auto line = makeUniqueNoThrow<PageLine>(std::move(tb), xPos, yPos);
  if (!line) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageLine");
    return nullptr;
  }
  return line;
}

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  // Images don't use fontId or text rendering
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset);
}

void PageImage::renderPlaceholder(GfxRenderer& renderer, const int xOffset, const int yOffset) const {
  imageBlock->renderPlaceholder(renderer, xPos + xOffset, yPos + yOffset);
}

bool PageImage::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize ImageBlock
  return imageBlock->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(HalFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  if (!serialization::readPod(file, xPos) || !serialization::readPod(file, yPos)) return nullptr;

  auto ib = ImageBlock::deserialize(file);
  if (!ib) return nullptr;
  auto image = makeUniqueNoThrow<PageImage>(std::move(ib), xPos, yPos);
  if (!image) LOG_ERR("PGE", "Deserialization failed: could not allocate PageImage");
  return image;
}

void PageHorizontalRule::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  (void)fontId;
  if (width == 0 || thickness == 0) {
    return;
  }

  renderer.drawLine(xPos + xOffset, yPos + yOffset, xPos + xOffset + width - 1, yPos + yOffset, thickness, true);
}

bool PageHorizontalRule::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  serialization::writePod(file, width);
  serialization::writePod(file, thickness);
  return true;
}

std::unique_ptr<PageHorizontalRule> PageHorizontalRule::deserialize(HalFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint8_t thickness = 0;
  if (!serialization::readPod(file, xPos) || !serialization::readPod(file, yPos) ||
      !serialization::readPod(file, width) || !serialization::readPod(file, thickness)) {
    return nullptr;
  }

  if (width == 0 || thickness == 0) {
    LOG_ERR("PGE", "Deserialization failed: invalid horizontal rule metadata (width=%u thickness=%u)", width,
            thickness);
    return nullptr;
  }

  auto rule = makeUniqueNoThrow<PageHorizontalRule>(width, thickness, xPos, yPos);
  if (!rule) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageHorizontalRule");
    return nullptr;
  }
  return rule;
}

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset, [](const PageElement&) { return true; });
}

void Page::renderImages(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset,
                             [](const PageElement& element) { return element.getTag() == TAG_PageImage; });
}

void Page::renderWithImagePlaceholders(GfxRenderer& renderer, const int fontId, const int xOffset,
                                       const int yOffset) const {
  for (const auto& element : elements) {
    if (element->getTag() == TAG_PageImage) {
      static_cast<const PageImage&>(*element).renderPlaceholder(renderer, xOffset, yOffset);
    } else {
      element->render(renderer, fontId, xOffset, yOffset);
    }
  }
}

void Page::extractImagesNeedingDecode() {
  for (auto& element : elements) {
    if (element->getTag() != TAG_PageImage) continue;
    auto& image = static_cast<PageImage&>(*element).getImageBlock();
    if (image.needsDecode()) image.ensureExtracted();
  }
}

void Page::cacheImagesNeedingDecode(GfxRenderer& renderer, const int xOffset, const int yOffset) {
  for (auto& element : elements) {
    if (element->getTag() != TAG_PageImage) continue;
    auto& image = static_cast<PageImage&>(*element);
    if (image.getImageBlock().needsDecode()) {
      image.getImageBlock().cacheDecodedImage(renderer, image.xPos + xOffset, image.yPos + yOffset);
    }
  }
}

bool Page::serialize(HalFile& file) const {
  const uint16_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    // Use getTag() method to determine type
    serialization::writePod(file, static_cast<uint8_t>(el->getTag()));

    if (!el->serialize(file)) {
      return false;
    }
  }

  // Serialize footnotes (clamp to MAX_FOOTNOTES_PER_PAGE to match addFootnote/deserialize limits)
  const uint16_t fnCount = std::min<uint16_t>(footnotes.size(), MAX_FOOTNOTES_PER_PAGE);
  serialization::writePod(file, fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    const auto& fn = footnotes[i];
    // number 定长，href 变长（uint32 长度前缀 + 正文，见 serialization::writeString）
    if (file.write(fn.number, sizeof(fn.number)) != sizeof(fn.number)) {
      LOG_ERR("PGE", "Failed to write footnote");
      return false;
    }
    serialization::writeString(file, fn.href);
  }

  const uint16_t linkCount = std::min<uint16_t>(links.size(), MAX_LINKS_PER_PAGE);
  serialization::writePod(file, linkCount);
  for (uint16_t i = 0; i < linkCount; i++) {
    const auto& link = links[i];
    if (file.write(link.href, sizeof(link.href)) != sizeof(link.href)) {
      LOG_ERR("PGE", "Failed to write link %u", i);
      return false;
    }
    serialization::writePod(file, link.x);
    serialization::writePod(file, link.y);
    serialization::writePod(file, link.width);
    serialization::writePod(file, link.height);
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(HalFile& file, const bool collectTouchLinks) {
  auto page = makeUniqueNoThrow<Page>();
  if (!page) {
    LOG_ERR("PGE", "OOM: Page (%u bytes)", static_cast<unsigned>(sizeof(Page)));
    return nullptr;
  }

  uint16_t count = 0;
  if (!serialization::readPod(file, count)) return nullptr;

  static constexpr uint16_t MAX_PAGE_ELEMENTS = 256;
  if (count > MAX_PAGE_ELEMENTS) {
    LOG_ERR("PGE", "Deserialization failed: page element count %u exceeds maximum", count);
    return nullptr;
  }
  page->elements.reserve(count);

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag = 0;
    if (!serialization::readPod(file, tag)) return nullptr;

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      if (!pl) {
        return nullptr;
      }
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      if (!pi) {
        return nullptr;
      }
      page->elements.push_back(std::move(pi));
    } else if (tag == TAG_PageHorizontalRule) {
      auto rule = PageHorizontalRule::deserialize(file);
      if (!rule) {
        return nullptr;
      }
      page->elements.push_back(std::move(rule));
    } else {
      LOG_ERR("PGE", "Deserialization failed: Unknown tag %u", tag);
      return nullptr;
    }
  }

  // Deserialize footnotes
  uint16_t fnCount = 0;
  if (!serialization::readPod(file, fnCount)) return nullptr;
  if (fnCount > MAX_FOOTNOTES_PER_PAGE) {
    LOG_ERR("PGE", "Invalid footnote count %u", fnCount);
    return nullptr;
  }
  if (!page->footnotes.resize(fnCount)) {
    // 分配失败（页脚注表要 16 条，见 FootnoteList）。href 变长之后不能再按固定尺寸往后跳，
    // 只能逐条读**长度前缀**、跳过正文；这条路上不分配，所以依旧不会 OOM。
    LOG_ERR("PGE", "OOM: dropping %u footnotes", fnCount);
    const size_t fileSize = file.size();
    for (uint16_t i = 0; i < fnCount; i++) {
      uint32_t len = 0;
      if (!serialization::readPod(file, len)) {
        LOG_ERR("PGE", "Failed to skip footnote %u after OOM", i);
        return nullptr;
      }
      const size_t position = file.position();
      if (len > FOOTNOTE_MAX_TEXT_BYTES || position > fileSize || len > fileSize - position ||
          !file.seek(position + len)) {
        LOG_ERR("PGE", "Failed to skip footnote %u after OOM", i);
        return nullptr;
      }
    }
  }
  for (uint16_t i = 0; i < page->footnotes.size(); i++) {
    auto& entry = page->footnotes[i];
    if (file.read(entry.number, sizeof(entry.number)) != sizeof(entry.number)) {
      LOG_ERR("PGE", "Failed to read footnote %u", i);
      return nullptr;
    }
    entry.number[sizeof(entry.number) - 1] = '\0';
    if (!serialization::readString(file, entry.href, FOOTNOTE_MAX_TEXT_BYTES)) {
      LOG_ERR("PGE", "Failed to read footnote %u", i);
      return nullptr;
    }
  }

  uint16_t linkCount = 0;
  if (!serialization::readPod(file, linkCount)) return nullptr;
  if (linkCount > MAX_LINKS_PER_PAGE) {
    LOG_ERR("PGE", "Invalid link count %u", linkCount);
    return nullptr;
  }
  if (!collectTouchLinks || !page->links.resize(linkCount)) {
    constexpr size_t SERIALIZED_LINK_SIZE = FOOTNOTE_HREF_LEN + 4 * sizeof(int16_t);
    const size_t bytesToSkip = static_cast<size_t>(linkCount) * SERIALIZED_LINK_SIZE;
    const size_t position = file.position();
    const size_t fileSize = file.size();
    if (collectTouchLinks) {
      LOG_ERR("PGE", "OOM: dropping %u page links (%u bytes)", linkCount, static_cast<unsigned>(bytesToSkip));
    }
    if (position > fileSize || bytesToSkip > fileSize - position || !file.seek(position + bytesToSkip)) {
      LOG_ERR("PGE", "Failed to skip page links");
      return nullptr;
    }
    return page;
  }
  for (uint16_t i = 0; i < linkCount; i++) {
    auto& link = page->links[i];
    if (file.read(link.href, sizeof(link.href)) != sizeof(link.href)) {
      LOG_ERR("PGE", "Failed to read link %u", i);
      return nullptr;
    }
    link.href[sizeof(link.href) - 1] = '\0';
    if (!serialization::readPod(file, link.x) || !serialization::readPod(file, link.y) ||
        !serialization::readPod(file, link.width) || !serialization::readPod(file, link.height)) {
      return nullptr;
    }
    if (link.href[0] == '\0' || link.width <= 0 || link.height <= 0) {
      LOG_ERR("PGE", "Invalid link geometry %u", i);
      return nullptr;
    }
  }

  return page;
}
