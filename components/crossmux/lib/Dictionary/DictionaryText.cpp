// StarDict 释义正文（.dict 里那段字节）**没有统一的格式**：按 .ifo 的 sametypesequence
// 可能是 h(HTML) 也可能是 m(纯文本)，而且相当一部分词典根本没写 .ifo。这里把 "HTML 那半边"
// 折成纯文本，交给 GfxRenderer::wrappedText 折行。
//
// 为什么不去走 Dictionary.h 里 `definitionsAreHtml()` 注释提到的"EPUB 渲染管线"：那条路要
// 起整本书的 CSS/章节解析链，代价远超一个释义窗口，而释义里的 HTML 翻来覆去也就
// div/span/font/br 这几样。等真遇到带样式表的词典再说。
//
// **保守是刻意的**：只剥白名单里的标签名。词典正文里 "<see also>"、数学写法 "5 < 10" 都不是
// 标签，误剥就是丢正文 —— 所以宁可漏剥（残留个别尖括号）也不多做。同理，**正文里一个标签都
// 没找到时原样返回**：那种输入本来就是纯文本，它的 '\n' 是硬换行（wrappedText 认），
// 不该按 HTML 的"源码空白无意义"规则压成空格。
#include "Dictionary.h"

#include <cctype>
#include <cstdint>
#include <string>

namespace {

bool asciiLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool asciiDigit(char c) { return c >= '0' && c <= '9'; }
bool asciiSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// 认识的标签名（小写）。不在名单里的一律当普通文字。
bool isKnownTag(const std::string& name) {
  static const char* const kTags[] = {"a",    "abbr",  "acronym", "b",     "big",   "blockquote",
                                      "br",   "center", "cite",   "code",  "dd",    "dfn",
                                      "div",  "dl",    "dt",      "em",    "font",  "h1",
                                      "h2",   "h3",    "h4",      "h5",    "h6",    "hr",
                                      "i",    "img",   "kbd",     "li",    "nobr",  "ol",
                                      "p",    "pre",   "q",       "rp",    "rt",    "ruby",
                                      "s",    "samp",  "script",  "small", "span",  "strike",
                                      "strong", "style", "sub",   "sup",   "table", "tbody",
                                      "td",   "tfoot", "th",      "thead", "tr",    "tt",
                                      "u",    "ul",    "var",     "wbr"};
  for (const char* t : kTags) {
    if (name == t) return true;
  }
  return false;
}

// 自带换行语义的标签。br/hr 是空元素，开标签就断行；其余容器在**闭**标签处断行，
// 于是 "<p>a</p><p>b</p>" 得到 "a\nb" 而不是 "a\n\n\nb" —— 小屏上双倍行距太散。
bool isLineBreaking(const std::string& name, const bool closing) {
  static const char* const kVoid[] = {"br", "hr"};
  static const char* const kBlock[] = {"p",    "div",  "li",   "tr",  "h1", "h2",
                                       "h3",   "h4",   "h5",   "h6",  "blockquote",
                                       "pre",  "table", "ul",  "ol",  "dl",
                                       "dd",   "dt"};
  for (const char* t : kVoid) {
    if (name == t) return true;
  }
  if (!closing) return false;
  for (const char* t : kBlock) {
    if (name == t) return true;
  }
  return false;
}

std::string utf8Encode(const uint32_t cp) {
  std::string s;
  if (cp < 0x80) {
    s.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return s;
}

// "&xxx;" 里的 xxx（不含 '&' 和 ';'）。认不出来返回空串，调用方原样保留。
std::string decodeEntity(const std::string& e) {
  if (e == "amp") return "&";
  if (e == "lt") return "<";
  if (e == "gt") return ">";
  if (e == "quot") return "\"";
  if (e == "apos" || e == "#39" || e == "#039") return "'";
  if (e == "nbsp" || e == "#160" || e == "#xa0" || e == "#xA0") return " ";
  if (e.size() > 1 && e[0] == '#') {
    uint32_t cp = 0;
    bool ok = true;
    if (e.size() > 2 && (e[1] == 'x' || e[1] == 'X')) {
      for (size_t k = 2; k < e.size() && ok; k++) {
        const char h = e[k];
        if (h >= '0' && h <= '9') cp = cp * 16 + static_cast<uint32_t>(h - '0');
        else if (h >= 'a' && h <= 'f') cp = cp * 16 + static_cast<uint32_t>(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') cp = cp * 16 + static_cast<uint32_t>(h - 'A' + 10);
        else ok = false;
      }
    } else {
      for (size_t k = 1; k < e.size() && ok; k++) {
        const char d = e[k];
        if (d >= '0' && d <= '9') cp = cp * 10 + static_cast<uint32_t>(d - '0');
        else ok = false;
      }
    }
    // 控制字符与代理区不还原 —— 它们画不出来，留着只会多一个空字形。
    if (ok && cp >= 0x20 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF)) return utf8Encode(cp);
  }
  return std::string();
}

// 收尾：行尾空白去掉、连续空行最多留一个、首尾空行删掉。
std::string tidy(std::string s) {
  std::string out;
  out.reserve(s.size());
  int newlines = 0;
  for (const char c : s) {
    if (c != '\n') {
      newlines = 0;
      out.push_back(c);
      continue;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    if (newlines >= 2) continue;  // 第三个及以后的 '\n' 丢掉 = 只留一个空行
    newlines++;
    out.push_back('\n');
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '\t' || out.back() == '\n')) out.pop_back();
  const size_t first = out.find_first_not_of('\n');
  return first == std::string::npos ? std::string() : out.substr(first);
}

}  // namespace

std::string Dictionary::htmlToPlainText(const std::string &html) {
  std::string out;
  out.reserve(html.size());
  const size_t n = html.size();
  size_t i = 0;
  bool pendingSpace = false;
  bool pendingSpaceIsFormatting = false;  // 这段空白里有 '\n'/'\t'（源码排版）而不是单个空格
  size_t markupSeen = 0;

  // 空格落后一格：真落了正文才补。这样"标签前的空白"不会变成行尾空格。
  const auto emit = [&out, &pendingSpace, &pendingSpaceIsFormatting](const std::string& t) {
    if (t.empty()) return;
    if (pendingSpace) {
      const unsigned char prev = out.empty() ? 0 : static_cast<unsigned char>(out.back());
      const unsigned char next = static_cast<unsigned char>(t[0]);
      // 源码换行造的空白落在两个多字节字符（CJK）之间时丢掉 —— 那是 HTML 排版，不是正文。
      // 正文里**明写**的空格（含 &nbsp;）不走这条路，照 HTML 规矩压成一个空格。
      const bool suppress = pendingSpaceIsFormatting && prev >= 0x80 && next >= 0x80;
      if (prev != 0 && out.back() != '\n' && !suppress) out.push_back(' ');
      pendingSpace = false;
      pendingSpaceIsFormatting = false;
    }
    out += t;
  };
  const auto breakLine = [&out, &pendingSpace, &pendingSpaceIsFormatting]() {
    pendingSpace = false;
    pendingSpaceIsFormatting = false;
    out.push_back('\n');
  };

  while (i < n) {
    const char c = html[i];

    if (c == '<' && html.compare(i, 4, "<!--") == 0) {  // 注释整段丢掉
      const size_t end = html.find("-->", i + 4);
      i = (end == std::string::npos) ? n : end + 3;
      markupSeen++;
      continue;
    }
    if (c == '<' && (html.compare(i, 2, "<!") == 0 || html.compare(i, 2, "<?") == 0)) {  // doctype / PI
      const size_t end = html.find('>', i);
      i = (end == std::string::npos) ? n : end + 1;
      markupSeen++;
      continue;
    }

    if (c == '<') {
      size_t j = i + 1;
      const bool closing = (j < n && html[j] == '/');
      if (closing) j++;
      const size_t nameStart = j;
      while (j < n && (asciiLetter(html[j]) || (j > nameStart && asciiDigit(html[j])))) j++;
      // 标签名后面必须跟空白 / '>' / '/'，且名字得在白名单里。不然 "<3"、"<see also>" 这类
      // 会被当成标签吃掉正文。
      const char after = (j < n) ? html[j] : '\0';
      const bool wellFormed = (j > nameStart) && (after == '\0' || asciiSpace(after) || after == '>' || after == '/');
      std::string name;
      if (wellFormed) {
        name.reserve(j - nameStart);
        for (size_t k = nameStart; k < j; k++) {
          name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(html[k]))));
        }
      }
      if (!wellFormed || !isKnownTag(name)) {
        emit(std::string(1, '<'));
        i++;
        continue;
      }
      // 扫到 '>'。属性值里可能有 '>'（如 title="a>b"），所以引号内不认 '>'。
      char quote = '\0';
      while (j < n) {
        const char d = html[j];
        if (quote != '\0') {
          if (d == quote) quote = '\0';
        } else if (d == '"' || d == '\'') {
          quote = d;
        } else if (d == '>') {
          break;
        }
        j++;
      }
      const size_t tagEnd = (j < n) ? j + 1 : n;
      markupSeen++;
      // <script>/<style> 的**内容**不是正文（词典里塞 CSS 的不少），整段吞掉。
      if (!closing && (name == "script" || name == "style")) {
        const size_t close = html.find("</" + name, tagEnd);
        i = (close == std::string::npos) ? n : close;
        continue;
      }
      // <img> 之类没有文字，但往往是一个义项的分隔点 —— 至少留一次换行。
      if (isLineBreaking(name, closing)) breakLine();
      i = tagEnd;
      continue;
    }

    if (c == '&') {
      const size_t semi = html.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 10) {
        const std::string rep = decodeEntity(html.substr(i + 1, semi - i - 1));
        if (!rep.empty()) {
          // &nbsp; 是作者**明写**的空格，不是源码排版 —— 按普通空格处理，也就能落在中文之间。
          if (rep == " ") {
            pendingSpace = true;
            pendingSpaceIsFormatting = false;
          } else {
            emit(rep);
          }
          i = semi + 1;
          continue;
        }
      }
      emit(std::string(1, '&'));
      i++;
      continue;
    }

    if (asciiSpace(c)) {  // HTML 源码里的空白（含换行）只是排版，不是断行
      pendingSpace = true;
      if (c != ' ') pendingSpaceIsFormatting = true;
      i++;
      continue;
    }
    emit(std::string(1, c));
    i++;
  }

  // 一个标签都没有：这本来就是纯文本，它的 '\n' 是硬换行（wrappedText 认），别按 HTML 的
  // "源码空白无意义"规则压掉 —— 压掉就正是"整条释义糊成一坨"那个 bug。
  if (markupSeen == 0) return html;
  return tidy(out);
}
