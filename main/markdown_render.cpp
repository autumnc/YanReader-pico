#include "markdown_render.h"
#include "font_renderer.h"
#include "ui_helpers.h"

#include <algorithm>

extern "C" {
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
}
struct u8g2_struct;
typedef struct u8g2_struct u8g2_t;
extern u8g2_t *g_u8g2;

namespace {

struct MdSeg {
    int start, end;         // byte range in the raw line
    TextStyle ts;           // style to render
    std::string drawText;   // replacement text for [start,end), possibly empty
};

bool s_mdEnabled = true;  // cleared when "Markdown渲染" setting is off

bool cursorInMarker(int cursorBytePos, int start, int end) {
    return cursorBytePos >= start && cursorBytePos < end;
}

bool cursorInConstruct(int cursorBytePos, int start, int end) {
    return cursorBytePos >= start && cursorBytePos <= end;
}

bool shouldRevealInline(int cursorBytePos, int openStart, int openEnd,
                        int closeStart, int closeEnd, bool verticalMode) {
    if (!verticalMode) return cursorInConstruct(cursorBytePos, openStart, closeEnd);
    return cursorInMarker(cursorBytePos, openStart, openEnd) ||
           cursorInMarker(cursorBytePos, closeStart, closeEnd);
}

// Spaces whose width matches the raw byte range [from,to). Keeps the width
// invariant even if the raw range contains non-ASCII bytes.
std::string spacesForWidth(const std::string &line, int from, int to) {
    std::string sp;
    int target = g_font.textWidth(line.substr(from, to - from).c_str());
    while (g_font.textWidth(sp.c_str()) < target) sp += ' ';
    return sp;
}

// Emit [from,to) as text, turning backslash escapes into a space plus the literal
// next char (`\*` → " *") so an escaped marker doesn't trigger styling and the
// backslash itself is hidden. Escapes apply to single-byte ASCII only (CommonMark
// style); `\中` keeps the backslash. Either way the emitted width equals the raw
// width, so mdContentWidth (which counts escapes at raw width via nextMarker's
// skip) stays in sync and cursor/selection remain aligned.
void emitPlain(const std::string &line, int from, int to, const TextStyle &base,
               std::vector<MdSeg> &segs) {
    int p = from;
    while (p < to) {
        if (line[p] == '\\' && p + 1 < to && (unsigned char)line[p + 1] < 0x80) {
            if (p > from) segs.push_back({from, p, base, line.substr(from, p - from)});
            segs.push_back({p, p + 1, base, " "});
            segs.push_back({p + 1, p + 2, base, line.substr(p + 1, 1)});
            p += 2;
            from = p;
        } else {
            p++;
        }
    }
    if (from < to) segs.push_back({from, to, base, line.substr(from, to - from)});
}

// Next inline marker byte, skipping escaped characters. Returns len if none.
bool isBookOpen(const std::string &line, int at) {
    return at + 3 <= (int)line.size() &&
           (line.compare(at, 3, "\xE3\x80\x8A") == 0 ||
            line.compare(at, 3, "\xE3\x80\x88") == 0);  // 《 / 〈
}

bool isBookClose(const std::string &line, int at) {
    return at + 3 <= (int)line.size() &&
           (line.compare(at, 3, "\xE3\x80\x8B") == 0 ||
            line.compare(at, 3, "\xE3\x80\x89") == 0);  // 》 / 〉
}

int nextMarker(const std::string &line, int from, int len, bool verticalMode = false) {
    for (int i = from; i < len; i++) {
        char c = line[i];
        if (c == '\\') { i++; continue; }
        if (c == '*' || c == '`' || c == '[' || c == '~' || c == '=') return i;
        if (verticalMode && c == '_') return i;
        if (verticalMode && (unsigned char)c == 0xE3 && isBookOpen(line, i)) return i;
    }
    return len;
}

// True if line[at] is the start of a Chinese numeral char (零一二三四五六七八九十百千).
bool isCnNumChar(const std::string &line, int at) {
    static const char *kCn[13] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十", "百", "千"};
    if (at + 3 > (int)line.size()) return false;
    for (int k = 0; k < 13; k++)
        if (line.compare(at, 3, kCn[k]) == 0) return true;
    return false;
}

}  // namespace

// List-family marker geometry. Skips leading whitespace so nested lists work.
// cells = content offset in cells from marker start: unordered/task bullet is 3
// cells (" • "/" ○ "/" ☐ ", marker glyph is 1 cell), ordered keeps its
// digits+separator width (bytes==cells for ASCII `.`/`)`, `、` is 2 cells, 3 bytes).
MdListMarker mdListMarker(const std::string &line) {
    MdListMarker m;
    int len = (int)line.size();
    int i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    m.start = i;
    int rest = len - i;

    if (rest >= 5 && (line.compare(i, 5, "- [ ]") == 0 ||
                      line.compare(i, 5, "- [x]") == 0 ||
                      line.compare(i, 5, "- [X]") == 0)) {
        m.ok = m.task = true;
        m.len = (i + 5 < len && line[i + 5] == ' ') ? 6 : 5;
        m.cells = 3;
        return m;
    }
    if (rest >= 2 && (line[i] == '-' || line[i] == '*' || line[i] == '+') && line[i + 1] == ' ') {
        m.ok = true;
        m.len = 2;
        m.cells = 3;
        return m;
    }
    int d = i;
    while (d < len && line[d] >= '0' && line[d] <= '9') d++;
    int nd = d - i;
    if (nd >= 1 && d + 2 < len && (unsigned char)line[d] == 0xE3 &&
        (unsigned char)line[d + 1] == 0x80 && (unsigned char)line[d + 2] == 0x81) {  // 、
        m.len = nd + 3;                     // digits + 、 (3 bytes)
        m.cells = nd + 3;                   // digits + 、(1+2格) + 1格右移,与无序一致
        if (d + 3 < len && line[d + 3] == ' ') { m.len++; m.cells++; }
        m.ordered = m.ok = true;
        return m;
    }
    if (nd >= 1 && d < len && (line[d] == '.' || line[d] == ')')) {
        if (d + 1 >= len) m.len = nd + 1;              // "1." at EOL
        else if (line[d + 1] == ' ') m.len = nd + 2;   // "1. "
        else return m;                                  // "1.5" → not a list
        m.cells = m.len + 1;   // 数字+分隔符后补1格右移内容,与无序列表一致
        m.ordered = m.ok = true;
    }
    // 中文序号 + 顿号:一、二、十、十一、… 原文渲染,前导1空格缩进,序号+顿号加粗
    int c = i;
    int nchars = 0;
    while (c + 3 <= len && isCnNumChar(line, c)) { c += 3; nchars++; }
    if (nchars >= 1 && c + 2 < len && (unsigned char)line[c] == 0xE3 &&
        (unsigned char)line[c + 1] == 0x80 && (unsigned char)line[c + 2] == 0x81) {  // 、
        m.len = (c - i) + 3;                // 序号字节 + `、`(3 bytes)
        m.cells = 1 + 2 * nchars + 2;       // 前导1格 + 每字2格 + `、`2格
        if (c + 3 < len && line[c + 3] == ' ') { m.len++; m.cells++; }
        m.ordered = m.ok = true;
    }
    return m;
}

// Parse a Chinese numeral starting at line[from] (零一二三四五六七八九十百千).
int mdCnNumValue(const std::string &line, int from, int &numLen) {
    static const char *kDg[10] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    int len = (int)line.size();
    int value = 0, section = 0;
    int i = from;
    while (i + 3 <= len) {
        const std::string c = line.substr(i, 3);
        int dv = -1, mult = 0;
        for (int k = 0; k < 10; k++)
            if (c == kDg[k]) { dv = k; break; }
        if (dv < 0) {
            if (c == "十") mult = 10;
            else if (c == "百") mult = 100;
            else if (c == "千") mult = 1000;
            else break;
        }
        if (mult) { value += (section == 0 ? 1 : section) * mult; section = 0; }
        else section = dv;
        i += 3;
    }
    numLen = i - from;
    if (numLen == 0) return -1;
    return value + section;
}

std::string mdCnNumeral(int n) {
    static const char *kCN[10] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    static const char *kUnit[4] = {"", "十", "百", "千"};
    if (n < 0 || n > 9999) return std::to_string(n);
    if (n < 10) return kCN[n];
    std::string out;
    int digits[4] = {0, 0, 0, 0};
    int len = 0;
    for (int t = n; t > 0; t /= 10) digits[len++] = t % 10;
    bool zeroPending = false;
    for (int i = len - 1; i >= 0; i--) {
        if (digits[i] == 0) {
            if (i > 0) zeroPending = true;
        } else {
            if (zeroPending) { out += kCN[0]; zeroPending = false; }
            out += kCN[digits[i]];
            out += kUnit[i];
        }
    }
    if (n >= 10 && n < 20) out.erase(0, 3);  // "一十"→"十"
    return out;
}

namespace {

int findStars(const std::string &line, int from, int len, int n) {
    for (int i = from; i <= len - n; i++) {
        bool ok = true;
        for (int k = 0; k < n; k++) if (line[i + k] != '*') { ok = false; break; }
        if (ok) return i;
    }
    return -1;
}

int findRun(const std::string &line, int from, int len, char marker, int n) {
    for (int i = from; i <= len - n; i++) {
        bool ok = true;
        for (int k = 0; k < n; k++) if (line[i + k] != marker) { ok = false; break; }
        if (ok) return i;
    }
    return -1;
}

int findBookClose(const std::string &line, int from, int len) {
    for (int i = from; i <= len - 3; i++) {
        if (line[i] == '\\') { i++; continue; }
        if (isBookClose(line, i)) return i;
    }
    return -1;
}

// Marker-aware visual width (px) of line[from,to): paired inline markers are
// hidden without taking space. Links and escapes stay width-neutral. Mirrors
// mdParseInline pairing so the cursor/selection match what's drawn.
static int mdContentWidth(const std::string &line, int from, int to) {
    int len = (int)line.size();
    if (to > len) to = len;
    if (from >= to) return 0;
    int px = 0;
    int i = from;
    while (i < to) {
        int m = nextMarker(line, i, to);
        if (m >= to) { px += g_font.textWidth(line.substr(i, to - i).c_str()); break; }
        if (m > i) { px += g_font.textWidth(line.substr(i, m - i).c_str()); i = m; }
        char c = line[m];
        int openLen = 1, closeIdx = -1, closeLen = 1, contentStart = m + 1;
        if (c == '*') {
            openLen = 1;
            while (m + openLen < len && line[m + openLen] == '*') openLen++;
            if (openLen > 3) openLen = 3;
            int cc = findStars(line, m + openLen, len, openLen);
            if (cc >= 0) { closeIdx = cc; closeLen = openLen; contentStart = m + openLen; }
        } else if (c == '`') {
            int cc = (int)line.find('`', m + 1);
            if (cc >= 0) { closeIdx = cc; contentStart = m + 1; }
        } else if (c == '~' && m + 1 < len && line[m + 1] == '~') {
            int cc = (int)line.find("~~", m + 2);
            if (cc >= 0) { closeIdx = cc; closeLen = 2; contentStart = m + 2; openLen = 2; }
        } else if (c == '=' && m + 1 < len && line[m + 1] == '=') {
            int cc = (int)line.find("==", m + 2);
            if (cc >= 0) { closeIdx = cc; closeLen = 2; contentStart = m + 2; openLen = 2; }
        } else if (c == '[') {
            int p = (int)line.find("](", m + 1);
            if (p >= 0 && p < len) {
                int cp = (int)line.find(')', p + 2);
                if (cp >= 0 && cp < len) {  // links stay width-neutral
                    int linkEnd = cp + 1;
                    if (linkEnd >= to) { px += g_font.textWidth(line.substr(m, to - m).c_str()); break; }
                    px += g_font.textWidth(line.substr(m, linkEnd - m).c_str());
                    i = linkEnd;
                    continue;
                }
            }
        }
        if (closeIdx < 0) { px += g_font.halfAdvance(); i = m + 1; continue; }  // unmatched literal
        int openEnd = m + openLen;
        if (to <= openEnd) break;  // inside hidden open marker
        int closeEnd = closeIdx + closeLen;
        if (to <= closeIdx) {  // inside styled content
            px += g_font.textWidth(line.substr(contentStart, to - contentStart).c_str());
            break;
        }
        px += g_font.textWidth(line.substr(contentStart, closeIdx - contentStart).c_str());
        i = closeEnd;
    }
    return px;
}

// Scan [from, len) for paired inline markers.
void mdParseInline(const std::string &line, int from, const TextStyle &base,
                   std::vector<MdSeg> &segs, int cursorBytePos = -1,
                   bool verticalMode = false) {
    int len = (int)line.size();
    int plainStart = from;
    while (plainStart < len) {
        int m = nextMarker(line, plainStart, len, verticalMode);
        if (m == len) break;
        if (m > plainStart)
            emitPlain(line, plainStart, m, base, segs);

        char c = line[m];
        if (c == '*') {
            int n = 1;
            while (m + n < len && line[m + n] == '*') n++;
            if (n > 3) n = 3;
            int cclose = findStars(line, m + n, len, n);
            if (cclose < 0) {  // unmatched → literal star
                segs.push_back({m, m + 1, base, line.substr(m, 1)});
                plainStart = m + 1;
                continue;
            }
            if (shouldRevealInline(cursorBytePos, m, m + n, cclose, cclose + n, verticalMode)) {
                segs.push_back({m, cclose + n, base, line.substr(m, cclose + n - m)});
                plainStart = cclose + n;
                continue;
            }
            TextStyle st = base;
            if (n >= 3) { st.bold = true; st.underline = true; }
            else if (n == 2) st.bold = true;
            else if (verticalMode) st.italic = true;  // vertical: single * = shifted underline
            else st.underline = true;
            segs.push_back({m, m + n, base, ""});
            if (cclose > m + n)
                emitPlain(line, m + n, cclose, st, segs);
            segs.push_back({cclose, cclose + n, base, ""});
            plainStart = cclose + n;
            continue;
        }
        if (verticalMode && c == '_' && m + 1 < len && line[m + 1] == '_') {
            int cclose = findRun(line, m + 2, len, '_', 2);
            if (cclose < 0) {
                segs.push_back({m, m + 1, base, line.substr(m, 1)});
                plainStart = m + 1;
                continue;
            }
            if (shouldRevealInline(cursorBytePos, m, m + 2, cclose, cclose + 2, verticalMode)) {
                segs.push_back({m, cclose + 2, base, line.substr(m, cclose + 2 - m)});
                plainStart = cclose + 2;
                continue;
            }
            TextStyle st = base;
            st.italic = true;
            segs.push_back({m, m + 2, base, ""});
            if (cclose > m + 2)
                emitPlain(line, m + 2, cclose, st, segs);
            segs.push_back({cclose, cclose + 2, base, ""});
            plainStart = cclose + 2;
            continue;
        }
        if (verticalMode && (unsigned char)c == 0xE3 && isBookOpen(line, m)) {
            int cclose = findBookClose(line, m + 3, len);
            if (cclose < 0) {
                int n = std::min(3, len - m);
                segs.push_back({m, m + n, base, line.substr(m, n)});
                plainStart = m + n;
                continue;
            }
            if (shouldRevealInline(cursorBytePos, m, m + 3, cclose, cclose + 3, verticalMode)) {
                segs.push_back({m, cclose + 3, base, line.substr(m, cclose + 3 - m)});
                plainStart = cclose + 3;
                continue;
            }
            TextStyle st = base;
            st.bookTitle = true;
            segs.push_back({m, m + 3, base, ""});
            if (cclose > m + 3)
                emitPlain(line, m + 3, cclose, st, segs);
            segs.push_back({cclose, cclose + 3, base, ""});
            plainStart = cclose + 3;
            continue;
        }
        if (c == '`') {
            int cclose = (int)line.find('`', m + 1);
            if (cclose < 0 || cclose >= len) {  // unmatched → literal
                segs.push_back({m, m + 1, base, line.substr(m, 1)});
                plainStart = m + 1;
                continue;
            }
            if (shouldRevealInline(cursorBytePos, m, m + 1, cclose, cclose + 1, verticalMode)) {
                segs.push_back({m, cclose + 1, base, line.substr(m, cclose + 1 - m)});
                plainStart = cclose + 1;
                continue;
            }
            TextStyle st = base;
            st.invert = true;
            segs.push_back({m, m + 1, base, ""});
            emitPlain(line, m + 1, cclose, st, segs);
            segs.push_back({cclose, cclose + 1, base, ""});
            plainStart = cclose + 1;
            continue;
        }
        if (c == '~' && m + 1 < len && line[m + 1] == '~') {
            int cclose = (int)line.find("~~", m + 2);
            if (cclose < 0 || cclose >= len) {  // unmatched → literal
                segs.push_back({m, m + 1, base, line.substr(m, 1)});
                plainStart = m + 1;
                continue;
            }
            if (shouldRevealInline(cursorBytePos, m, m + 2, cclose, cclose + 2, verticalMode)) {
                segs.push_back({m, cclose + 2, base, line.substr(m, cclose + 2 - m)});
                plainStart = cclose + 2;
                continue;
            }
            TextStyle st = base;
            st.strike = true;
            segs.push_back({m, m + 2, base, ""});
            emitPlain(line, m + 2, cclose, st, segs);
            segs.push_back({cclose, cclose + 2, base, ""});
            plainStart = cclose + 2;
            continue;
        }
        if (c == '=' && m + 1 < len && line[m + 1] == '=') {
            int cclose = (int)line.find("==", m + 2);
            if (cclose < 0 || cclose >= len) {  // unmatched → literal
                segs.push_back({m, m + 1, base, line.substr(m, 1)});
                plainStart = m + 1;
                continue;
            }
            if (shouldRevealInline(cursorBytePos, m, m + 2, cclose, cclose + 2, verticalMode)) {
                segs.push_back({m, cclose + 2, base, line.substr(m, cclose + 2 - m)});
                plainStart = cclose + 2;
                continue;
            }
            TextStyle st = base;
            st.emph = true;
            segs.push_back({m, m + 2, base, ""});
            emitPlain(line, m + 2, cclose, st, segs);
            segs.push_back({cclose, cclose + 2, base, ""});
            plainStart = cclose + 2;
            continue;
        }
        if (c == '[') {
            int p = (int)line.find("](", m + 1);
            if (p >= 0 && p < len) {
                int cp = (int)line.find(')', p + 2);
                if (cp >= 0 && cp < len) {
                    if (!verticalMode && cursorInConstruct(cursorBytePos, m, cp + 1)) {
                        segs.push_back({m, cp + 1, base, line.substr(m, cp + 1 - m)});
                        plainStart = cp + 1;
                        continue;
                    }
                    TextStyle st = base;
                    st.invert = true;
                    st.underline = true;
                    segs.push_back({m, m + 1, base, " "});
                    emitPlain(line, m + 1, p, st, segs);
                    segs.push_back({p, cp + 1, base, spacesForWidth(line, p, cp + 1)});
                    plainStart = cp + 1;
                    continue;
                }
            }
        }
        plainStart = m + 1;  // unhandled marker char → emit as literal content
        segs.push_back({m, m + 1, base, line.substr(m, 1)});
    }
    if (plainStart < len)
        emitPlain(line, plainStart, len, base, segs);
}

void mdParseLine(const std::string &line, const MdLineInfo &info, std::vector<MdSeg> &segs,
                 int cursorBytePos = -1, bool folded = false, bool verticalMode = false) {
    int len = (int)line.size();
    if (!s_mdEnabled) {
        segs.push_back({0, len, TextStyle{}, line});
        return;
    }
    TextStyle base;
    if (info.headingLevel > 0) { base.bold = true; base.underline = true; }

    if (info.inCodeBlock) {
        TextStyle st = base;
        st.invert = true;
        segs.push_back({0, len, st, line});
        return;
    }
    if (info.hr) {
        segs.push_back({0, len, base, spacesForWidth(line, 0, len)});
        return;
    }

    int pos = 0;
    if (info.headingLevel > 0) {
        int n = info.headingLevel;
        static const char *kLevelGlyph[6] = {
            "\xF3\xB0\x8E\xA4", "\xF3\xB0\x8E\xA7", "\xF3\xB0\x8E\xAA",
            "\xF3\xB0\x8E\xAD", "\xF3\xB0\x8E\xB1", "\xF3\xB0\x8E\xB3",
        };
        if (cursorInMarker(cursorBytePos, 0, n)) {
            segs.push_back({0, n, TextStyle{}, line.substr(0, n)});
        } else if (folded) {
            // 保留级别图标,紧跟 uF09DA 折叠标志(各 advance 2 格)→ 前缀共 4 格,
            // 标题内容右移 2 格。mdVisualX/mdVrowX/mdContinuationPrefix 同按折叠态
            // 输出 4 格前缀,光标/选中/折行定位与此一致。
            std::string glyph(kLevelGlyph[info.headingLevel - 1]);
            glyph += kFoldMarker;
            segs.push_back({0, n, base, glyph});
        } else {
            // Heading glyph advance is 2 cells; content starts right after it.
            segs.push_back({0, n, base, kLevelGlyph[info.headingLevel - 1]});
        }
        pos = n;
    } else if (info.list || info.task) {
        MdListMarker m = mdListMarker(line);
        if (m.ok) {
            int mend = m.start + m.len;
            if (cursorInMarker(cursorBytePos, 0, mend)) {
                segs.push_back({0, mend, base, line.substr(0, mend)});
            } else if (m.task) {
                bool checked = (m.start + 3 < len) &&
                               (line[m.start + 3] == 'x' || line[m.start + 3] == 'X');
                // U+F0132 完成(实心带勾) / U+F0131 待办(空框), content after marker
                std::string repl = checked ? " \xF3\xB0\x84\xB2 " : " \xF3\xB0\x84\xB1 ";
                segs.push_back({0, mend, base, line.substr(0, m.start) + repl});
            } else if (m.ordered) {
                TextStyle nst = base;
                nst.bold = true;
                // 前导补1格:序号(阿拉伯或中文)随整体右移1格,与无序列表一致(无序子弹在1格处)
                segs.push_back({0, mend, nst, std::string(" ") + line.substr(0, mend)});
            } else {
                // 次级列表(缩进)用空心圆 U+1F786,顶级用实心圆点 U+2022
                const char *glyph = m.start > 0 ? " \xF0\x9F\x9E\x86 " : " \xE2\x80\xA2 ";
                segs.push_back({0, mend, base, line.substr(0, m.start) + glyph});
            }
            pos = mend;
        }
    } else if (info.quote) {
        if (cursorInMarker(cursorBytePos, 0, 2))
            segs.push_back({0, 2, base, line.substr(0, 2)});
        else
            segs.push_back({0, 2, base, "    "});  // bar at cell 4, 1-space gap, content at cell 5
        pos = 2;
    }

    mdParseInline(line, pos, base, segs, cursorBytePos, verticalMode);
}

std::string sliceDraw(const MdSeg &seg, int s, int e) {
    if (s == seg.start && e == seg.end) return seg.drawText;
    int so = s - seg.start;
    int n = e - s;
    if ((int)seg.drawText.size() >= so + n) return seg.drawText.substr(so, n);
    return seg.drawText;  // symbol segments are never partially sliced
}

int segWidthToByte(const MdSeg &seg, int bytePos) {
    if (bytePos <= seg.start) return 0;
    if (bytePos >= seg.end) return g_font.textWidth(seg.drawText.c_str());
    return g_font.textWidth(sliceDraw(seg, seg.start, bytePos).c_str());
}

int mdParsedX(const std::string &line, const MdLineInfo &info, int bytePos,
              int cursorBytePos, bool folded = false) {
    std::vector<MdSeg> segs;
    mdParseLine(line, info, segs, cursorBytePos, folded);
    int px = 0;
    for (auto &seg : segs) {
        if (bytePos >= seg.end) px += g_font.textWidth(seg.drawText.c_str());
        else return px + segWidthToByte(seg, bytePos);
    }
    return px;
}

int mdContinuationPrefix(const std::string &line, const MdLineInfo &info, int cursorBytePos,
                         bool folded = false) {
    if (info.headingLevel > 0) return mdParsedX(line, info, info.headingLevel, cursorBytePos, folded);
    if (info.quote) return mdParsedX(line, info, 2, cursorBytePos);
    if (info.list || info.task) {
        MdListMarker m = mdListMarker(line);
        if (m.ok) return mdParsedX(line, info, m.start + m.len, cursorBytePos);
    }
    return 0;
}

}  // namespace

// U+F09DA 折叠标志(NF-Mono 圆角方框内含 >),折叠标题行时紧跟级别图标之后,
// 与级别图标同几何(advance 2 格),合起来前缀占 4 格。编辑器竖排也用它。
const char *kFoldMarker = "\xF3\xB0\xA7\x9A";

// 竖排单元格分解。块级/行内标记替换为符号格(标题图标/列表符号/任务框)或整段
// 隐藏(**、~~、`、>、链接括号不留空格),因此竖排的光标映射按格而非按字节。
std::vector<MdVCell> mdVerticalCells(const std::string &line, const MdLineInfo &info,
                                     bool folded, int cursorBytePos) {
    std::vector<MdVCell> cells;
    int len = (int)line.size();
    if (len == 0) return cells;
    auto nextChar = [](const std::string &s, int p) {
        p++;
        while (p < (int)s.size() && ((unsigned char)s[p] & 0xC0) == 0x80) p++;
        return p;
    };
    auto pushRaw = [&](int from, int to, const TextStyle &ts) {
        for (int p = from; p < to;) {
            int n = nextChar(line, p);
            cells.push_back({p, n, line.substr(p, n - p), ts});
            p = n;
        }
    };

    if (info.hr) {  // 水平线/围栏行:原样字符竖排成虚线列
        pushRaw(0, len, TextStyle{});
        return cells;
    }
    if (info.inCodeBlock) {
        TextStyle st;
        st.invert = true;
        pushRaw(0, len, st);  // 围栏内整列反白
        return cells;
    }

    std::vector<MdSeg> segs;
    mdParseLine(line, info, segs, cursorBytePos, folded, true);
    for (auto &seg : segs) {
        if (seg.drawText.empty()) continue;  // 隐藏的成对标记(**、`、~~、==)
        std::string raw = line.substr(seg.start, seg.end - seg.start);
        if (seg.drawText == raw) {
            pushRaw(seg.start, seg.end, seg.ts);
            continue;
        }
        bool allSpace = true;
        for (char c : seg.drawText)
            if (c != ' ') { allSpace = false; break; }
        if (allSpace) {
            // 引用行水平画左侧竖线,竖排改为一个竖线格(︱)标记引用
            if (info.quote && seg.start == 0 && seg.end == 2)
                cells.push_back({0, 2, "\xEF\xB8\xB1", TextStyle{}});
            continue;  // 链接括号/引用线等占位空白不留格
        }
        // 替换型标记(标题图标/折叠标志/列表符号):尾部空格去掉,前导空格
        // (嵌套列表缩进/序号右移)保留为空白格,其余逐字符成格
        int s = 0, e = (int)seg.drawText.size();
        while (e > s && seg.drawText[e - 1] == ' ') e--;
        for (int p = s; p < e;) {
            int n = nextChar(seg.drawText, p);
            cells.push_back({seg.start, seg.end, seg.drawText.substr(p, n - p), seg.ts});
            p = n;
        }
    }
    return cells;
}

void mdSetRenderEnabled(bool on) { s_mdEnabled = on; }

std::vector<MdLineInfo> mdClassifyLines(const std::vector<std::string> &lines) {
    std::vector<MdLineInfo> out(lines.size());
    bool inCode = false;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string &ln = lines[i];
        MdLineInfo &info = out[i];
        int end = (int)ln.size();
        while (end > 0 && (ln[end - 1] == ' ' || ln[end - 1] == '\t')) end--;

        if (ln.compare(0, 3, "```") == 0) {
            info.hr = true;  // fence line renders as a horizontal rule
            inCode = !inCode;
            continue;
        }
        if (inCode) { info.inCodeBlock = true; continue; }

        int ws = 0;
        while (ws < end && (ln[ws] == ' ' || ln[ws] == '\t')) ws++;

        // heading / quote only at column 0; horizontal rule tolerates leading ws
        if (ws == 0) {
            int h = 0;
            while (h < end && ln[h] == '#') h++;
            if (h >= 1 && h <= 6 && h < end && ln[h] == ' ') {
                info.headingLevel = h;
                continue;
            }
        }
        if (end >= 3) {  // horizontal rule: only - * _ (and whitespace), >= 3
            bool hr = true;
            int cnt = 0;
            for (int k = 0; k < end; k++) {
                char c = ln[k];
                if (c == ' ' || c == '\t') continue;
                if (c == '-' || c == '*' || c == '_') { cnt++; continue; }
                hr = false;
                break;
            }
            if (hr && cnt >= 3) { info.hr = true; continue; }
        }
        if (ws == 0 && end >= 2 && ln[0] == '>' && ln[1] == ' ') { info.quote = true; continue; }

        // list family (unordered/ordered/task), allowed after leading ws = nested
        MdListMarker lm = mdListMarker(ln);
        if (lm.ok) { info.task = lm.task; info.list = true; }
    }
    return out;
}

std::vector<char> mdFoldHiddenLines(const std::vector<std::string> &lines,
                                    const std::vector<MdLineInfo> *mdInfo,
                                    const std::set<int> *foldedHeadings) {
    std::vector<char> hidden(lines.size(), 0);
    if (!mdInfo || !foldedHeadings || foldedHeadings->empty() ||
        mdInfo->size() < lines.size())
        return hidden;
    int hideLevel = 0;
    for (size_t li = 0; li < lines.size(); li++) {
        int lvl = (*mdInfo)[li].headingLevel;
        bool inCode = (*mdInfo)[li].inCodeBlock;
        bool isH = lvl > 0 && !inCode;
        if (hideLevel != 0) {
            // 折叠区内部:只有同级或更高级的标题能结束折叠
            if (!(isH && lvl <= hideLevel)) {
                hidden[li] = 1;
                continue;
            }
        }
        if (isH) hideLevel = foldedHeadings->count((int)li) ? lvl : 0;
    }
    return hidden;
}

int mdVisualX(const std::string &line, const MdLineInfo &info, int bytePos,
              int cursorBytePos, bool folded) {
    if (!s_mdEnabled) return g_font.textWidth(line.substr(0, bytePos).c_str());
    if (cursorBytePos >= 0) return mdParsedX(line, info, bytePos, cursorBytePos, folded);
    if (info.inCodeBlock || info.hr) return g_font.textWidth(line.substr(0, bytePos).c_str());
    int cell = g_font.halfAdvance();
    if (info.headingLevel > 0) {
        int prefixCells = folded ? 4 : 2;  // 折叠时:级别图标 + uF09DA 折叠标志
        if (bytePos >= info.headingLevel)
            return prefixCells * cell + mdContentWidth(line, info.headingLevel, bytePos);
        return 0;
    }
    if (info.list || info.task) {
        MdListMarker m = mdListMarker(line);
        if (m.ok) {
            int mend = m.start + m.len;
            if (bytePos >= mend)
                return (m.start + m.cells) * cell + mdContentWidth(line, mend, bytePos);
            if (bytePos <= m.start)
                return mdContentWidth(line, 0, bytePos);
            return (m.start + (bytePos - m.start)) * cell;  // inside marker (approx)
        }
        return mdContentWidth(line, 0, bytePos);
    }
    if (info.quote) {
        if (bytePos >= 2)
            return 4 * cell + 2 + mdContentWidth(line, 2, bytePos);
        return mdContentWidth(line, 0, bytePos);
    }
    return mdContentWidth(line, 0, bytePos);
}

// Visual x (px) of raw bytePos within a vrow that starts at raw byte vrowStart.
// Continuation vrows (vrowStart > 0) of heading/task/quote/list lines repeat the
// prefix indent so wrapped text stays aligned under the first line. Without
// this, wrapped lines were placed at their whole-line x, i.e. off-screen.
int mdVrowX(const std::string &line, const MdLineInfo &info, int bytePos, int vrowStart,
            int indentCells, int cursorBytePos, bool folded) {
    int extra = indentCells * g_font.halfAdvance();
    if (!s_mdEnabled)
        return extra + g_font.textWidth(line.substr(vrowStart, bytePos - vrowStart).c_str());
    if (cursorBytePos >= 0) {
        int prefix = vrowStart > 0 ? mdContinuationPrefix(line, info, cursorBytePos, folded) : 0;
        return extra + mdVisualX(line, info, bytePos, cursorBytePos, folded) -
               mdVisualX(line, info, vrowStart, cursorBytePos, folded) + prefix;
    }
    int cell = g_font.halfAdvance();
    int prefix = 0;
    if (vrowStart > 0) {
        if (info.headingLevel > 0) prefix = (folded ? 4 : 2) * cell;
        else if (info.quote) prefix = 4 * cell + 2;
        else if (info.list || info.task) {
            MdListMarker m = mdListMarker(line);
            if (m.ok) prefix = (m.start + m.cells) * cell;
        }
    }
    return extra + mdVisualX(line, info, bytePos, -1, folded) -
           mdVisualX(line, info, vrowStart, -1, folded) + prefix;
}

void mdDrawVrow(int x, int y, const std::string &line, int start, int end,
                const MdLineInfo &info, int indentCells, int cursorBytePos, bool folded) {
    std::vector<MdSeg> segs;
    mdParseLine(line, info, segs, cursorBytePos, folded);
    int len = (int)line.size();
    if (end > len) end = len;

    u8g2_SetDrawColor(g_u8g2, 0);  // text draws in ink; overlays must not leak color 1

    for (auto &seg : segs) {
        if (seg.start >= end) break;
        if (seg.end <= start) continue;
        int s = std::max(seg.start, start);
        int e = std::min(seg.end, end);
        int cx = x + mdVrowX(line, info, s, start, indentCells, cursorBytePos, folded);
        std::string draw = sliceDraw(seg, s, e);
        if (!draw.empty()) g_content_font.drawTextStyled(cx, y, draw.c_str(), seg.ts);
    }

    if (info.quote && !cursorInMarker(cursorBytePos, 0, 2)) {
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, x + 3 * g_font.halfAdvance(), y - g_font.ascent(), 2, g_font.lineHeight());
        // do NOT flip color back to 1 here: the next vrow's text would then be
        // drawn light-on-light and vanish (wrapped quote lines showed no text)
    } else if (info.hr) {
        int hy = y - g_font.ascent() + g_font.lineHeight() / 2;
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawHLine(g_u8g2, x, hy, SCREEN_W - 2 * x);
    }
}
