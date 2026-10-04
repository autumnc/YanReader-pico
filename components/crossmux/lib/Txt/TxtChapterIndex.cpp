#include "TxtChapterIndex.h"

#include <array>
#include <cctype>

namespace txt_chapter_index {
namespace {

// 识别顺序即优先级：第N章/回/节… → 卷N → 序章/楔子… → 英文 → 纯数字短章名。
// 规则与 tools/ebook/epdbook/chapter_detect.py 对齐（设备侧无正则，手写码点匹配）。

constexpr std::string_view UTF8_BOM = "\xEF\xBB\xBF";

bool isAsciiSpace(const char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' || value == '\v';
}

std::string_view trim(std::string_view line) {
  while (!line.empty() && isAsciiSpace(line.front())) line.remove_prefix(1);
  while (!line.empty() && isAsciiSpace(line.back())) line.remove_suffix(1);
  if (line.compare(0, UTF8_BOM.size(), UTF8_BOM) == 0) line.remove_prefix(UTF8_BOM.size());
  return line;
}

// 取 pos 处的一个码点并前进（含全角标点）。截断/非法序列退化为单字节、返回 0xFFFD，
// 保证任何输入下都不会读越界、也不会原地打转。
uint32_t nextCodepoint(const std::string_view text, size_t& pos) {
  if (pos >= text.size()) return 0;
  const auto byteAt = [&text](const size_t index) { return static_cast<unsigned char>(text[index]); };
  const unsigned char lead = byteAt(pos);
  if (lead < 0x80) {
    ++pos;
    return lead;
  }
  int length = 0;
  uint32_t cp = 0;
  if ((lead & 0xE0) == 0xC0) {
    length = 2;
    cp = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3;
    cp = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    length = 4;
    cp = lead & 0x07;
  }
  if (length == 0 || pos + static_cast<size_t>(length) > text.size()) {
    ++pos;
    return 0xFFFD;
  }
  for (int i = 1; i < length; ++i) {
    const unsigned char cont = byteAt(pos + static_cast<size_t>(i));
    if ((cont & 0xC0) != 0x80) {
      ++pos;
      return 0xFFFD;
    }
    cp = (cp << 6) | (cont & 0x3F);
  }
  pos += static_cast<size_t>(length);
  return cp;
}

bool isSpace(const uint32_t cp) {
  return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == '\f' || cp == '\v' ||
         cp == 0x3000;  // 全角空格
}

bool isNumeral(const uint32_t cp) {
  if (cp >= '0' && cp <= '9') return true;
  switch (cp) {
    case 0x96F6:  // 零
    case 0x3007:  // 〇
    case 0x4E00:  // 一
    case 0x4E8C:  // 二
    case 0x4E09:  // 三
    case 0x56DB:  // 四
    case 0x4E94:  // 五
    case 0x516D:  // 六
    case 0x4E03:  // 七
    case 0x516B:  // 八
    case 0x4E5D:  // 九
    case 0x5341:  // 十
    case 0x767E:  // 百
    case 0x5343:  // 千
    case 0x4E07:  // 万
    case 0x4E24:  // 两
      return true;
    default:
      return false;
  }
}

// 章回节讲篇话集；卷/部单列，语义上是"卷"而不是"章"。
bool isChapterUnit(const uint32_t cp) {
  switch (cp) {
    case 0x7AE0:  // 章
    case 0x56DE:  // 回
    case 0x8282:  // 节
    case 0x8BB2:  // 讲
    case 0x7BC7:  // 篇
    case 0x8BDA:  // 话
    case 0x96C6:  // 集
      return true;
    default:
      return false;
  }
}

bool isVolumeUnit(const uint32_t cp) { return cp == 0x5377 /*卷*/ || cp == 0x90E8 /*部*/; }

// 跳过空格；返回新的 pos。
size_t skipSpaces(const std::string_view text, size_t pos) {
  while (pos < text.size()) {
    size_t next = pos;
    if (!isSpace(nextCodepoint(text, next))) break;
    pos = next;
  }
  return pos;
}

// 消耗一段数字（阿拉伯或中文），至少 1 个码点。返回消耗的字节数，失败 0。
size_t consumeNumerals(const std::string_view text, size_t pos) {
  const size_t start = pos;
  size_t count = 0;
  while (pos < text.size()) {
    size_t next = pos;
    if (!isNumeral(nextCodepoint(text, next))) break;
    pos = next;
    if (++count > 12) return 0;  // 超长数字串不是章号
  }
  return count > 0 ? pos - start : 0;
}

bool consume(const std::string_view text, size_t& pos, const std::string_view token) {
  if (text.compare(pos, token.size(), token) != 0) return false;
  pos += token.size();
  return true;
}

// ^第 <空白>? <数字> <空白>? <章|回|…|卷|部>
bool isDiNUnitHeading(const std::string_view title) {
  size_t pos = 0;
  if (!consume(title, pos, "第")) return false;
  pos = skipSpaces(title, pos);
  const size_t numerals = consumeNumerals(title, pos);
  if (numerals == 0) return false;
  pos += numerals;
  pos = skipSpaces(title, pos);
  size_t next = pos;
  const uint32_t unit = nextCodepoint(title, next);
  return isChapterUnit(unit) || isVolumeUnit(unit);
}

// ^卷 <空白>? <数字>（"卷三 风起" 这种前缀式，没有"第"）
bool isJuanNHeading(const std::string_view title) {
  size_t pos = 0;
  if (!consume(title, pos, "卷")) return false;
  pos = skipSpaces(title, pos);
  return consumeNumerals(title, pos) > 0;
}

bool isNameSeparator(const uint32_t cp) {
  return isSpace(cp) || cp == ':' || cp == 0xFF1A /*：*/ || cp == '-' || cp == 0x2014 /*—*/ ||
         cp == 0x2013 /*–*/ || cp == 0x3001 /*、*/ || cp == 0xFF0C /*，*/ || cp == ',';
}

// 整词前缀，后接空白/分隔符（或整行就是它）才算；单独的「序」只认整行。
bool isSpecialHeading(const std::string_view title) {
  constexpr std::array<std::string_view, 18> WORDS = {
      "番外篇", "内容简介", "序章", "序言", "楔子", "引子", "引言", "前言", "绪论",
      "后记",   "尾声",     "终章", "番外", "外传", "附录", "导读", "跋",   "序"};
  for (const auto word : WORDS) {
    if (title.compare(0, word.size(), word) != 0) continue;
    if (title.size() == word.size()) return true;
    size_t pos = word.size();
    if (isNameSeparator(nextCodepoint(title, pos))) return true;
  }
  return false;
}

bool equalsAsciiKeyword(const std::string_view text, const std::string_view keyword, size_t& pos) {
  if (text.size() < keyword.size()) return false;
  for (size_t i = 0; i < keyword.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(text[i])) !=
        std::tolower(static_cast<unsigned char>(keyword[i]))) {
      return false;
    }
  }
  pos = keyword.size();
  return true;
}

// Chapter/Part/Volume/Book/Act/Section + 空白 + 数字或罗马数字
bool isEnglishHeading(const std::string_view title) {
  constexpr std::array<std::string_view, 6> KEYWORDS = {"chapter", "part", "volume", "book", "act", "section"};
  size_t pos = 0;
  bool matched = false;
  for (const auto keyword : KEYWORDS) {
    size_t next = 0;
    if (equalsAsciiKeyword(title, keyword, next)) {
      pos = next;
      matched = true;
      break;
    }
  }
  if (!matched) return false;
  // 关键词必须独立成词（后面是空白），免得 "action"/"bookmark" 之类混进来。
  if (pos >= title.size() || !isAsciiSpace(title[pos])) return false;
  pos = skipSpaces(title, pos);
  if (pos >= title.size()) return false;
  const char first = title[pos];
  if (first >= '0' && first <= '9') return true;
  // 粗罗马数字：只要以这些字母开头就算（后面接不接后缀不再判断）。
  const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(first)));
  return lower == 'i' || lower == 'v' || lower == 'x' || lower == 'l' || lower == 'c' || lower == 'd' || lower == 'm';
}

// 纯数字短章名（"1清和宫上"、"11"）；排除 "1."、"1、" 这类编号说明行。
bool isNumericShortHeading(const std::string_view title) {
  if (title.size() > 60) return false;  // 长行几乎不可能是章名，压掉误报
  size_t pos = 0;
  size_t digits = 0;
  while (pos < title.size() && digits < 4 && title[pos] >= '0' && title[pos] <= '9') {
    ++pos;
    ++digits;
  }
  if (digits == 0) return false;
  if (pos >= title.size()) return true;
  size_t next = pos;
  const uint32_t cp = nextCodepoint(title, next);
  return cp != '.' && cp != 0x3001 /*、*/ && cp != 0xFF0E /*．*/;
}

// 去掉首尾成对的包裹符（【】「」『』()（）），逐层剥离后返回内部视图。
std::string_view stripWrap(const std::string_view title) {
  const auto peel = [](const std::string_view text, const uint32_t open, const uint32_t close) {
    size_t pos = 0;
    if (nextCodepoint(text, pos) != open) return text;
    // 找最后一个码点的起点
    size_t lastPos = std::string_view::npos;
    uint32_t lastCp = 0;
    size_t i = 0;
    while (i < text.size()) {
      const size_t start = i;
      lastCp = nextCodepoint(text, i);
      lastPos = start;
    }
    if (lastCp != close || lastPos == std::string_view::npos || lastPos < pos) return text;
    return text.substr(pos, lastPos - pos);
  };
  std::string_view result = title;
  result = peel(result, 0x3010, 0x3011);  // 【】
  result = peel(result, 0x300C, 0x300D);  // 「」
  result = peel(result, 0x300E, 0x300F);  // 『』
  result = peel(result, '(', ')');
  result = peel(result, 0xFF08, 0xFF09);  // （）
  return result;
}

}  // namespace

std::string_view chapterTitle(std::string_view line) {
  line = trim(line);
  if (line.empty() || line.size() >= TITLE_CAPACITY) return {};
  const std::string_view title = stripWrap(line);
  if (title.empty()) return {};
  return isDiNUnitHeading(title) || isJuanNHeading(title) || isSpecialHeading(title) ||
                 isEnglishHeading(title) || isNumericShortHeading(title)
             ? title
             : std::string_view{};
}

}  // namespace txt_chapter_index
