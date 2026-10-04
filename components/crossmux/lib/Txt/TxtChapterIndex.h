#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace txt_chapter_index {

constexpr size_t TITLE_CAPACITY = 192;

struct Record {
  uint32_t sourceOffset = 0;
  char title[TITLE_CAPACITY] = {};
};
static_assert(sizeof(Record) == 196);

// Returns the line when it is a supported chapter heading, or an empty view
// otherwise. The line is trimmed and a wrapping 【】「」『』()（） pair (one or
// more levels) is peeled off, so the returned view is what the TOC should show.
// The input must be UTF-8 (ASCII is valid UTF-8).
//
// Recognised: 第N章/回/节/讲/篇/话/集, 第N卷/部, 卷N (no 第), 序章/楔子/番外/…,
// Chapter/Part/Volume/Book/Act/Section + number, and bare short numeric titles
// ("1清和宫上"). Mirrors tools/ebook/epdbook/chapter_detect.py.
std::string_view chapterTitle(std::string_view line);

}  // namespace txt_chapter_index
