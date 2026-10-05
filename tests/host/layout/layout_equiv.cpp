// 主机端验证：「让 ASCII 按字体真实字宽排版」（废弃「ASCII = 半格」）。
//
// 被测的是**折行几何**：charAdvancePx / byteToX / xToByte / mdIndentPx / buildVrows
// （工作区版，从 main/ui_helpers.cpp 原样抄来），对照物是改造前那版
// （gen_old.py 从 git HEAD 抽的 old_*，见 build/old_layout.inc）。
//
// 为什么不直接编译 main/ui_helpers.cpp：那份 TU 拖着 wifi/bt/display/epdiy/FreeRTOS
// 一大串，桩件的量级远超被测代码本身。这与 tests/host/rotation/rot_equiv.cpp 同一套
// 做法（那里也是为了不引 epdiy 才把旋转映射抄进桩 TU）——**几何算法可以整段搬**，
// 搬过来的字节与原文件一致，改了正文忘了同步这里，[4] 会先亮（见那条的说明）。
//
// 七件事：
//   [1] 量画自洽：Σ charAdvancePx(cp) == g_font.textWidth(s)。自由函数 charAdvancePx
//       是"规则的第二份写法"（它没有 FontRenderer 实例），这一条钉住两份写法不许分家
//       —— `g_font.charWidth` 的桩是 main/font_renderer.cpp 里**逐字抄的正文**，所以
//       这是真检查，不是自证。
//   [2] 往返：每个 UTF-8 边界 k 上 xToByte(line, 0, len, byteToX(line, k)) == k。
//   [3] 折行不变量：每条 vrow 装得下、且**再多吞一个字就越界**（行是极大的）；vrow
//       必须首尾相接铺满整行（不重不漏、pos 一定前进 = 没有死循环）。
//   [4] 回归护栏：**内容面是内置字体时**（= 绝大多数用户），新 buildVrows 与旧版输出的
//       vrow 列表逐条相同（indentPx == 旧格数 × 半格）—— 但**只在格高是偶数时严格成立**。
//       旧口径 CJK = 2 格 = 2×⌊h/2⌋px，新口径 CJK = cjkAdvance() = h px，h 为奇数时每个
//       汉字差 1px（45/41 两档都是奇数，50 是偶数）。所以奇数档的断言是"只许变紧、
//       不许变松" + 打印差异行数；纯 ASCII 行任何 h 都严格相同。详见那条函数的注释 ——
//       这是把**旧代码里就有的**「wrap 按 44px/汉字、draw 按 45px/汉字」收拢成同值，
//       代价是极少数贴着右缘的行会提前一个字符换行。
//   [5] 边界：空行 / 纯空格 / 一个字就比整行宽的 CJK 与比例拉丁 / pos 前进性。
//   [6] 改动确实生效：外置字体下新旧**必须**有分歧 —— 否则说明改动被谁还原了，
//       而 [4] 会照样全绿（护栏只证明"没坏"，不证明"改了"，两条都要）。
//   [7] 输入法编码行的例外仍成立：latin_builtin_ 的实例在装了外置字体时拉丁仍走半格。
//
// 编译运行：tests/host/layout/run.sh（纯 g++，不需要 IDF 环境、不需要 builtin.ttf）。
// 退出码 0 = 全部通过。

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

// ── 被测环境的桩 ────────────────────────────────────────────────────────────
// 真身里 SCREEN_W 是 ui_screen_w()（由 epd 旋转定），这里给个可调的整数。
static int g_screen_w = 960;
#define SCREEN_W (g_screen_w)

// 角色常量，值取自 main/font/ttf_font.h。
#define TTF_ROLE_CONTENT 0

// 「装没装外置字体」。内容面 = 内置字体时，内部函数里 ttf_font_is_builtin() 为真。
static bool g_builtin_face = true;
// 当前格子像素高（= FontRenderer 的 px_ = line_height_）。
static int g_px = 45;

// 桩版外置字体的 ASCII 步进表。真身是 main/font/ttf_font.c 里每字面 128 项的
// f_ascii_adv（由 cmap + hmtx 量出来）。这里给一张**按宽度分类**的表：只要它非等宽，
// 就能把"比例拉丁"与"半格"的区别测出来，也不必依赖 SD 上的字体文件。
// 值以 px=45 为准，按字号等比缩放（真身的步进也是随 px 线性缩放的）。
static int fakeAsciiAdvance(uint8_t c) {
    if (c == 0) return 0;
    static const char *kNarrow = " .,:;'\"|!ilj[]()ftr-";
    static const char *kWide   = "MW@%mw";
    if (std::strchr(kNarrow, (char)c)) return 12;
    if (std::strchr(kWide, (char)c)) return 34;
    return 22;
}

bool ttf_font_is_builtin(void) { return g_builtin_face; }

int ttf_char_advance_px(int role, int pixel_height, uint32_t codepoint) {
    (void)role;
    (void)pixel_height;
    return fakeAsciiAdvance((uint8_t)codepoint) * g_px / 45;
}

// 桩版 FontRenderer。halfAdvance/cjkAdvance/pxHeight 是 font_renderer.h 里的内联体；
// charWidth/textWidth 是 main/font_renderer.cpp 里的**正文**（只去掉 isStatusSymbol 那
// 两支：那是程序化状态图标，ASCII/CJK 走不到，被测代码也用不到）。
struct StubFont {
    int line_height_ = 45;
    int px_ = 45;
    bool latin_builtin_ = false;   // 输入法编码行那个实例钉的是 true

    int lineHeight() const { return line_height_; }
    int cjkAdvance() const { return line_height_; }      // fullwidth advance
    int halfAdvance() const { return line_height_ / 2; } // 设计用的半格
    int pxHeight() const { return px_; }

    // main/font_renderer.cpp:210 —— 逐字抄（role_ == TTF_ROLE_CONTENT 在本桩恒真）
    bool usesProportionalLatin() const {
        return !latin_builtin_ && !ttf_font_is_builtin();
    }

    // main/font_renderer.cpp:214 —— 逐字抄
    int charWidth(uint32_t cp) {
        if (cp < 0x80) {
            if (!usesProportionalLatin()) return halfAdvance();
            return ttf_char_advance_px(TTF_ROLE_CONTENT, px_, cp);
        }
        return line_height_;
    }

    // main/font_renderer.cpp:228 —— 逐字抄
    int textWidth(const char *text) {
        int w = 0;
        while (*text) {
            uint32_t cp = utf8Decode(text);
            if (cp == 0) continue;
            w += charWidth(cp);
        }
        return w;
    }

    // main/font_renderer.cpp:171 —— 逐字抄
    static uint32_t utf8Decode(const char *&str) {
        if (!str || !*str) return 0;
        uint8_t c = (uint8_t)*str;
        if (c < 0x80) { str++; return c; }
        if ((c & 0xE0) == 0xC0) {
            if ((str[1] & 0xC0) != 0x80) { str++; return 0; }
            uint32_t cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(str[1] & 0x3F);
            str += 2; return cp;
        }
        if ((c & 0xF0) == 0xE0) {
            if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80) { str++; return 0; }
            uint32_t cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(str[1] & 0x3F) << 6) |
                          (uint32_t)(str[2] & 0x3F);
            str += 3; return cp;
        }
        if ((c & 0xF8) == 0xF0) {
            if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80 ||
                (str[3] & 0xC0) != 0x80) { str++; return 0; }
            uint32_t cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(str[1] & 0x3F) << 12) |
                          ((uint32_t)(str[2] & 0x3F) << 6) | (uint32_t)(str[3] & 0x3F);
            str += 4; return cp;
        }
        str++;
        return 0;
    }
};

static StubFont g_font;

// 桩版 settings：只给 buildVrows 要的那一个开关。
struct StubSettings {
    bool firstLineIndent() const { return fi; }
    bool fi = false;
};
static StubSettings g_settings;

// ── 从 main/markdown_render.h 抄的结构体（真身那份 include font_renderer.h，
//    会拖进 FontRenderer 的完整类定义；这里只要字段名一致）─────────────────────
struct MdLineInfo {
    int headingLevel = 0;  // 1..6, 0 = not a heading
    bool list = false;
    bool task = false;     // "- [ ]" / "- [x]"
    bool quote = false;
    bool inCodeBlock = false;  // between code fences
    bool hr = false;           // horizontal rule or code fence line
};

struct MdListMarker {
    bool ok = false;
    bool task = false;
    bool ordered = false;
    int start = 0;
    int len = 0;
    int cells = 0;
};

// 从 main/ui_helpers.h 抄（字段名必须一致：buildVrows 用位置初始化，靠顺序对齐）。
struct VRow { int lineIdx; int start; int end; int indentPx = 0; };

// ══ markdown 侧：从 main/markdown_render.cpp 原样抄来的几支 ═══════════════════

// :90 逐字抄
bool isCnNumChar(const std::string &line, int at) {
    static const char *kCn[13] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十", "百", "千"};
    if (at + 3 > (int)line.size()) return false;
    for (int k = 0; k < 13; k++)
        if (line.compare(at, 3, kCn[k]) == 0) return true;
    return false;
}

// :104 逐字抄
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

// :698 逐字抄（折叠标题的隐藏行判定；测试一律传 foldedHeadings=nullptr，但
// buildVrows 无条件调它，所以必须有定义）
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
            if (!(isH && lvl <= hideLevel)) {
                hidden[li] = 1;
                continue;
            }
        }
        if (isH) hideLevel = foldedHeadings->count((int)li) ? lvl : 0;
    }
    return hidden;
}

// :649 的**桩**（真身那支还判 hr / 行内标记 / 代码围栏细节；这里只要 buildVrows 会
// 用到的几项判对即可 —— 新旧两版共用这支，所以等价性对拍不受影响）。
std::vector<MdLineInfo> mdClassifyLines(const std::vector<std::string> &lines) {
    std::vector<MdLineInfo> out(lines.size());
    bool inCode = false;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string &l = lines[i];
        size_t s = l.find_first_not_of(" \t");
        std::string t = (s == std::string::npos) ? std::string() : l.substr(s);
        if (t.rfind("```", 0) == 0) { out[i].inCodeBlock = true; inCode = !inCode; continue; }
        if (inCode) { out[i].inCodeBlock = true; continue; }
        int h = 0;
        while (h < (int)t.size() && h < 6 && t[h] == '#') h++;
        if (h >= 1 && h < (int)t.size() && t[h] == ' ') { out[i].headingLevel = h; continue; }
        if (t.rfind("> ", 0) == 0 || t == ">") { out[i].quote = true; continue; }
        MdListMarker m = mdListMarker(l);
        if (m.ok) { out[i].task = m.task; out[i].list = !m.task; }
    }
    return out;
}

// :727 逐字抄 —— 「有序记号前缀该占多宽」的唯一真相（原文照原样画，所以量原文）。
int mdRawMarkerIndentPx(const std::string &line, const MdListMarker &m) {
    int mend = m.start + m.len;
    if (mend > (int)line.size()) mend = (int)line.size();
    return g_font.textWidth((" " + line.substr(0, mend)).c_str());
}

// :740 逐字抄 —— 渲染前缀的像素宽（内容真正开始的那个 x 偏移）。
int mdPrefixAdvancePx(const std::string &line, const MdLineInfo &info, bool folded) {
    const int cell = g_font.halfAdvance();
    if (info.headingLevel > 0) return (folded ? 4 : 2) * cell;
    if (info.quote) return 4 * cell + 2;   // 引用条在 3 格处，内容从 4 格 + 2px 起
    if (info.list || info.task) {
        MdListMarker m = mdListMarker(line);
        if (!m.ok) return 0;
        if (m.ordered) return mdRawMarkerIndentPx(line, m);
        return (m.start + m.cells) * cell;
    }
    return 0;
}

// ══ 被测对象：从 main/ui_helpers.cpp 原样抄来（行号见每支的注）═══════════════

// :164 逐字抄
static int charAdvancePx(unsigned char c) {
    if (c >= 0x80) return g_font.cjkAdvance();
    if (ttf_font_is_builtin()) return g_font.halfAdvance();
    return ttf_char_advance_px(TTF_ROLE_CONTENT, g_font.pxHeight(), (uint32_t)c);
}

// :170 逐字抄
static int utf8CharLen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// :179 逐字抄
int byteToX(const std::string &line, int byteOffset) {
    if (byteOffset > (int)line.size()) byteOffset = (int)line.size();
    int x = 0;
    for (int i = 0; i < byteOffset; ) {
        x += charAdvancePx((unsigned char)line[i]);
        i += utf8CharLen((unsigned char)line[i]);
    }
    return x;
}

// :191 逐字抄
int xToByte(const std::string &line, int start, int end, int targetX) {
    if (start < 0) start = 0;
    if (end > (int)line.size()) end = (int)line.size();
    int x = byteToX(line, start);
    for (int ci = start; ci < end; ) {
        unsigned char c = (unsigned char)line[ci];
        int cc = charAdvancePx(c);
        if (x + cc > targetX) {
            return (targetX - x <= x + cc - targetX) ? ci : ci + utf8CharLen(c);
        }
        x += cc;
        ci += utf8CharLen(c);
    }
    return end;
}

// :203 逐字抄
static int mdPrefixLen(const std::string &line) {
    MdListMarker m = mdListMarker(line);
    if (m.ok) return m.start + m.len;
    int len = (int)line.size();
    if (len >= 2 && line[0] == '>' && line[1] == ' ') return 2;
    int h = 0;
    while (h < len && h < 6 && line[h] == '#') h++;
    if (h >= 1 && h < len && line[h] == ' ') return h + 1;
    return 0;
}

// :233 逐字抄
static int mdIndentPx(const std::string &line) {
    const int cell = g_font.halfAdvance();
    MdListMarker m = mdListMarker(line);
    if (m.ok) {
        if (m.ordered) return mdRawMarkerIndentPx(line, m);
        return (m.start + m.cells) * cell;
    }
    int len = (int)line.size();
    if (len >= 2 && line[0] == '>' && line[1] == ' ') return 4 * cell;
    return mdPrefixLen(line) > 0 ? 2 * cell : 0;  // heading
}

// :245 逐字抄
std::vector<VRow> buildVrows(const std::vector<std::string> &lines,
                             const std::vector<MdLineInfo> *mdInfoIn,
                             const std::set<int> *foldedHeadings) {
    std::vector<VRow> vrows;
    bool firstLineIndent = g_settings.firstLineIndent();
    std::vector<MdLineInfo> localInfo;
    const std::vector<MdLineInfo> *mdInfoPtr = mdInfoIn;
    if (!mdInfoPtr && firstLineIndent) {
        localInfo = mdClassifyLines(lines);
        mdInfoPtr = &localInfo;
    }
    std::vector<char> hidden = mdFoldHiddenLines(lines, mdInfoPtr, foldedHeadings);
    bool folding = foldedHeadings && !foldedHeadings->empty();
    for (int li = 0; li < (int)lines.size(); li++) {
        if (folding && hidden[li]) continue;
        const auto &line = lines[li];
        int len = (int)line.length();
        if (len == 0) {
            vrows.push_back({li, 0, 0});
            continue;
        }
        const int cellw = g_font.halfAdvance();
        const int maxpx = (SCREEN_W / cellw) * cellw;
        int indentPx = mdIndentPx(line);
        if (folding && mdInfoPtr && (*mdInfoPtr)[li].headingLevel > 0 &&
            !(*mdInfoPtr)[li].inCodeBlock && foldedHeadings->count(li)) {
            indentPx = 4 * cellw;
        }
        int prefixEnd = mdPrefixLen(line);
        int firstIndentPx = 0;
        if (firstLineIndent && mdInfoPtr) {
            const MdLineInfo &info = (*mdInfoPtr)[li];
            if (info.headingLevel == 0 && !info.list && !info.task &&
                !info.quote && !info.inCodeBlock && !info.hr) {
                firstIndentPx = 4 * cellw;  // two Chinese-width characters
            }
        }
        int pos = 0;
        while (pos < len) {
            int used = 0;
            int end = pos;
            int lastBreak = -1;
            int pe = (pos == 0) ? prefixEnd : 0;  // only the first vrow has the marker
            int rowIndentPx = (pos == 0) ? firstIndentPx : 0;
            int cap = maxpx - indentPx - rowIndentPx + ((pos == 0) ? byteToX(line, prefixEnd) : 0);
            if (cap > maxpx) cap = maxpx;
            int firstAdv = charAdvancePx((unsigned char)line[pos]);
            if (cap < firstAdv) cap = firstAdv;
            while (end < len) {
                unsigned char c = (unsigned char)line[end];
                int cc = charAdvancePx(c);
                if (used + cc > cap) break;
                used += cc;
                int clen = utf8CharLen(c);
                if (c == ' ' && end >= pe) {
                    lastBreak = end + 1;
                } else if (c >= 0x80 && end >= pe) {
                    lastBreak = end + clen;
                }
                end += clen;
            }
            if (end >= len) {
                vrows.push_back({li, pos, len, rowIndentPx});
                break;
            }
            if (lastBreak > pos) {
                vrows.push_back({li, pos, lastBreak, rowIndentPx});
                pos = lastBreak;
                while (pos < len && line[pos] == ' ') pos++;
            } else {
                vrows.push_back({li, pos, end, rowIndentPx});
                pos = end;
            }
        }
    }
    return vrows;
}

// ══ 对照物：改造前那版（gen_old.py 从 git HEAD 抽）════════════════════════════
#if __has_include("old_layout.inc")
#include "old_layout.inc"
#define HAVE_OLD 1
#else
#define HAVE_OLD 0
#endif

// ── 断言 ────────────────────────────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;
__attribute__((format(printf, 2, 3)))
static void check(bool ok, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (ok) { g_pass++; }
    else { g_fail++; printf("  FAIL %s\n", buf); }
}
__attribute__((format(printf, 1, 2)))
static void note(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("  ... %s\n", buf);
}

// 语料：纯 ASCII / 纯 CJK / 中英混排 / 各种 markdown 记号。
static std::vector<std::string> corpus() {
    return {
        "Hello, world! This MWm line mixes iiij with wide letters.",
        "这是一段纯中文测试文本用于验证折行与像素换算是否都正确无误",
        "中文 English 混排 with some words 和汉字 test.",
        "# 标题 Heading with latin",
        "## 二级标题",
        "- 列表项 item one",
        "  - 嵌套 nested item",
        "1. 有序 ordered item",
        "10. 两位数字 two-digit ordinal",
        "一、中文序号条目",
        "- [ ] 待办任务 task",
        "- [x] 已完成 done",
        "> 引用 quote with latin words",
        "   开头有四个空格的行",
        "a",
        "中",
        "",
        "     ",
        "ThisIsOneVeryLongUnbrokenLatinTokenThatMustWrapSomewhereBecauseItIsLong",
    };
}

// ── [1] 量画自洽 ────────────────────────────────────────────────────────────
static void testMeasureDrawAgree(const std::vector<std::string> &lines) {
    printf("[1] 量画自洽：Σ charAdvancePx == g_font.textWidth\n");
    // 自由函数 charAdvancePx 是规则的第二份写法，必须与实例版逐值相同。
    for (int c = 0x20; c < 0x80; c++)
        check(charAdvancePx((unsigned char)c) == g_font.charWidth((uint32_t)c),
              "ASCII 0x%02X: charAdvancePx=%d charWidth=%d", c,
              charAdvancePx((unsigned char)c), g_font.charWidth((uint32_t)c));
    // 全角侧走的是另一支（cjkAdvance == line_height），也钉一下。
    check(charAdvancePx(0xE4) == g_font.charWidth(0x4E2D),
          "CJK: charAdvancePx(首字节)=%d charWidth(中)=%d",
          charAdvancePx(0xE4), g_font.charWidth(0x4E2D));
    // 整串：逐字节累加 == 实例逐码点累加。
    for (const std::string &l : lines)
        check(byteToX(l, (int)l.size()) == g_font.textWidth(l.c_str()),
              "串 [%s]: byteToX=%d textWidth=%d", l.c_str(), byteToX(l, (int)l.size()),
              g_font.textWidth(l.c_str()));
    note("内容面 %s，px=%d", g_builtin_face ? "内置" : "外置", g_px);
}

// ── [2] 往返 ────────────────────────────────────────────────────────────────
static void testRoundTrip(const std::vector<std::string> &lines) {
    printf("[2] 往返：xToByte(byteToX(k)) == k（每个 UTF-8 边界）\n");
    for (const std::string &l : lines) {
        int n = (int)l.size();
        for (int k = 0; k <= n; k++) {
            // 只测**字符边界**（落在续字节上本来就没有对应关系）
            if (k < n && ((unsigned char)l[k] & 0xC0) == 0x80) continue;
            int back = xToByte(l, 0, n, byteToX(l, k));
            check(back == k, "行 [%s] 偏移 %d 往返成了 %d", l.c_str(), k, back);
        }
        // 带窗口的同一断言：调用点全是 xToByte(line, vrow.start, vrow.end, x)，
        // 起点的偏移（`int x = byteToX(line, start)`）写错只有这里才现形。
        bool ok = true;
        for (int s = 0; s <= n && ok; s++) {
            if (s < n && ((unsigned char)l[s] & 0xC0) == 0x80) continue;
            for (int e = s; e <= n && ok; e++) {
                if (e < n && ((unsigned char)l[e] & 0xC0) == 0x80) continue;
                ok = (xToByte(l, s, e, byteToX(l, s)) == s);
            }
        }
        check(ok, "行 [%s] 在任意 start/end 窗口下起点往返都成立", l.c_str());
    }
}

// ── [3] 折行不变量 ──────────────────────────────────────────────────────────
static void testWrapInvariants(const std::vector<std::string> &lines) {
    printf("[3] 折行不变量：装得下、极大、铺满整行\n");
    std::vector<MdLineInfo> info = mdClassifyLines(lines);
    std::vector<VRow> vr = buildVrows(lines, &info, nullptr);
    const int cellw = g_font.halfAdvance();
    const int maxpx = (SCREEN_W / cellw) * cellw;
    size_t i = 0;
    for (int li = 0; li < (int)lines.size(); li++) {
        const std::string &line = lines[li];
        int len = (int)line.size();
        // 收集这一行的 vrow
        std::vector<VRow> row;
        while (i < vr.size() && vr[i].lineIdx == li) row.push_back(vr[i++]);
        if (len == 0) {
            // 空行 = 一条 0..0 的 vrow（渲染时它就是"一个空行"）
            check(row.size() == 1 && row[0].start == 0 && row[0].end == 0,
                  "行 %d 是空行，应为单条 0..0 的 vrow（实际 %zu 条）", li, row.size());
            continue;
        }
        check(!row.empty(), "行 %d [%s] 一条 vrow 都没有", li, line.c_str());
        if (row.empty()) continue;
        check(row.front().start == 0, "行 %d 首条 vrow 不从 0 起（%d）", li, row.front().start);
        check(row.back().end == len, "行 %d 末条 vrow 不收到行尾（%d != %d）", li,
              row.back().end, len);
        for (size_t r = 0; r < row.size(); r++) {
            const VRow &v = row[r];
            check(v.start < v.end, "行 %d 第 %zu 条 vrow 是空的（%d..%d）—— pos 没前进",
                  li, r, v.start, v.end);
            if (r) check(v.start >= row[r - 1].end, "行 %d 第 %zu 条 vrow 与上一条重叠",
                         li, r);
            // 预算：与 buildVrows 内层同一个算式（这里独立复述一遍，正是测试的意义）
            int indentPx = mdIndentPx(line);
            int firstIndentPx = (!info[li].headingLevel && !info[li].list && !info[li].task &&
                                 !info[li].quote && !info[li].inCodeBlock && !info[li].hr &&
                                 g_settings.firstLineIndent()) ? 4 * cellw : 0;
            int prefixEnd = mdPrefixLen(line);
            int rowIndentPx = (v.start == 0) ? firstIndentPx : 0;
            int cap = maxpx - indentPx - rowIndentPx +
                      ((v.start == 0) ? byteToX(line, prefixEnd) : 0);
            if (cap > maxpx) cap = maxpx;
            int firstAdv = charAdvancePx((unsigned char)line[v.start]);
            if (cap < firstAdv) cap = firstAdv;
            int used = byteToX(line, v.end) - byteToX(line, v.start);
            check(used <= cap, "行 %d 第 %zu 条 vrow 超出了预算（用 %d > 容 %d）", li, r, used, cap);
            // 极大性：还没到行尾、且**不是**在空格处按词界断的行，再吞一个字一定越界。
            // 空格断行是**故意提前**的（buildVrows 的 lastBreak 分支），不是"还能吞"。
            // CJK 那边不会有这个豁免：每个汉字都是断点，lastBreak 落在最后一个装得下的
            // 汉字后面，与"装到满"同一个位置，所以它照样要过极大性。
            if (v.end < len && line[v.end - 1] != ' ')
                check(used + charAdvancePx((unsigned char)line[v.end]) > cap,
                      "行 %d 第 %zu 条 vrow 不是极大的（还能再吞一个 %dpx 的字）", li, r,
                      charAdvancePx((unsigned char)line[v.end]));
            // 屏幕级：预留 + 用完的总宽不许越过右缘
            int total = indentPx + rowIndentPx + used -
                        ((v.start == 0) ? byteToX(line, prefixEnd) : 0);
            check(total <= maxpx, "行 %d 第 %zu 条 vrow 总宽 %d 越过屏宽 %d", li, r, total, maxpx);
        }
    }
    check(i == vr.size(), "还有 %zu 条 vrow 没被归到任何一行", vr.size() - i);
}

// ── [4] 内置面下的等价护栏 ──────────────────────────────────────────────────
#if HAVE_OLD
// 内置面下的新旧对拍。**这里有一个必须说清楚的算术事实**：
//
//   旧口径是"格"：ASCII=1 格、CJK=2 格，1 格 = halfAdvance() = ⌊line_height/2⌋ px。
//   新口径是"像素"：ASCII=halfAdvance()、CJK=cjkAdvance()=**line_height**。
//
// 两者只在 line_height 是**偶数**时逐像素相同（2×⌊h/2⌋ == h）。h 为奇数时每个汉字差
// 1px（h=45 的默认档、h=41 的备用档都是奇数），屏幕预算 946px，所以**折行点会偶尔
// 提前一个字符**：一行里汉字越多、累计差越大，越接近右缘的行越容易少排一个字。
//
// 这不是新引入的偏差，而是把一处在**旧代码里就存在**的量画分家收拢了：旧代码
// **画**汉字时用的就是 charWidth(CJK) = line_height = 45px，而**量**折行时按 2 格 = 44px
// 算 —— 也就是说旧 wrap 每个汉字少数 1px，满行时能放行的行其实比预算宽。改完两边
// 同值。所以这条对拍在奇数档上"允许有差异"，而[3]负责保证新口径下的行确实不越界。
//
// 纯 ASCII 行不受影响（1 格 == halfAdvance，任何 h 都严格相等），所以那一档无论
// 奇偶都做严格断言。
static void testBuiltinEquivalence(const std::vector<std::string> &lines) {
    printf("[4] 内置面下：新 buildVrows 与旧 buildVrows 对拍（格制 vs 像素制）\n");
    if (!g_builtin_face) { note("外置面，跳过"); return; }
    const int h = g_font.line_height_;
    const int half = g_font.halfAdvance();
    const bool exact = (h == 2 * half);   // 偶数格高 → 两支口径逐像素相同
    std::vector<MdLineInfo> info = mdClassifyLines(lines);
    std::vector<VRow> a = buildVrows(lines, &info, nullptr);
    std::vector<VRow> b = old::old_buildVrows(lines, &info, nullptr);

    int diffLines = 0, shrunk = 0;
    size_t ia = 0, ib = 0;
    for (int li = 0; li < (int)lines.size(); li++) {
        const std::string &line = lines[li];
        std::vector<VRow> ra, rb;
        while (ia < a.size() && a[ia].lineIdx == li) ra.push_back(a[ia++]);
        while (ib < b.size() && b[ib].lineIdx == li) rb.push_back(b[ib++]);
        bool asciiOnly = true;
        for (size_t k = 0; k < line.size(); k++)
            if ((unsigned char)line[k] >= 0x80) { asciiOnly = false; break; }
        bool same = (ra.size() == rb.size());
        for (size_t k = 0; same && k < ra.size(); k++)
            same = ra[k].start == rb[k].start && ra[k].end == rb[k].end &&
                   ra[k].indentPx == rb[k].indentPx * half;
        if (same) continue;
        diffLines++;
        if (exact || asciiOnly) {
            check(false, "行 %d [%s] 新旧折行不同：新 %zu 条 旧 %zu 条（%s）", li,
                  line.c_str(), ra.size(), rb.size(),
                  exact ? "格高是偶数，两支口径本该逐像素相同" : "纯 ASCII 行，本该完全相同");
        } else {
            // 奇数档：只许"变紧"（新的容量只可能更小），不许变松。
            if (ra.size() < rb.size()) shrunk++;
            check(ra.size() >= rb.size(),
                  "行 %d [%s] 新折行比旧**少**了（%zu < %zu）—— 奇数档的漂移只该让容量变小",
                  li, line.c_str(), ra.size(), rb.size());
        }
    }
    check(ia == a.size() && ib == b.size(), "对拍时漏掉了 vrow（%zu/%zu 未归行）",
          a.size() - ia, b.size() - ib);
    if (exact) {
        check(diffLines == 0, "内置面下与改造前有 %d 行不同（格高偶数，应为 0）", diffLines);
        note("格高 %d 是偶数：%zu 条 vrow 与改造前**逐条相同**", h, a.size());
    } else {
        check(shrunk == 0, "有 %d 行反而排得更松了", shrunk);
        note("格高 %d 是奇数：%zu 条 vs 旧 %zu 条，%d 行折行点提前（每个汉字 1px 的"
             "量画收敛，见本函数注释）", h, a.size(), b.size(), diffLines);
    }
    note("对照物来自 git HEAD 的 ui_helpers.cpp（gen_old.py 每次重抽）");
}
#endif

// ── [5] 边界 ────────────────────────────────────────────────────────────────
static void testEdges() {
    printf("[5] 边界：空行 / 纯空格 / 单字比整行还宽 / pos 前进性\n");
    {
        std::vector<std::string> L = {""};
        std::vector<VRow> v = buildVrows(L, nullptr, nullptr);
        check(v.size() == 1 && v[0].start == 0 && v[0].end == 0, "空行应是一条 0..0 的 vrow");
    }
    {
        std::vector<std::string> L = {"     "};
        std::vector<VRow> v = buildVrows(L, nullptr, nullptr);
        check(!v.empty() && v.front().start == 0 && v.back().end == 5, "纯空格行应铺满");
        for (const VRow &r : v) check(r.start < r.end || r.end == 0, "纯空格行出现空 vrow");
    }
    // 一个字比整行还宽：old 的 `if (cap < 1) cap = 1`（格制）在这个屏宽下会
    // end==pos → 外层不前进 → **死循环**；新代码的 `cap < firstAdv` 是为此加的。
    // 这里只跑新版（旧版会把测试挂住），断言它**返回**且每行都排得下。
    {
        int saved = g_screen_w;
        g_screen_w = 30;   // 半格 22px > maxpx=22px… 见下：maxpx = (30/22)*22 = 22
        std::vector<std::string> L = {"中文字", "MWW"};
        std::vector<VRow> v = buildVrows(L, nullptr, nullptr);
        check(!v.empty(), "屏宽 30px 下建不出 vrow");
        for (const VRow &r : v) check(r.start < r.end, "窄屏下出现空 vrow（pos 没前进）");
        int cnt = 0;
        for (const VRow &r : v) if (r.lineIdx == 0) cnt++;
        check(cnt == 3, "窄屏下 3 个汉字应各占一行（实际 %d 行）", cnt);
        note("屏宽 30px：CJK 一字一行（旧版在这里会死循环，见 buildVrows 的 firstAdv 注释）");
        g_screen_w = saved;
    }
    // 首行缩进打开时也要铺满（走 mdClassifyLines 那条路）
    {
        g_settings.fi = true;
        std::vector<std::string> L = {"正文段落 with latin", "- 列表项", "# 标题"};
        std::vector<VRow> v = buildVrows(L, nullptr, nullptr);
        check(!v.empty(), "首行缩进打开时建不出 vrow");
        for (const VRow &r : v)
            check(r.start < r.end, "首行缩进模式下出现空 vrow");
        check(v.front().lineIdx == 0 && v.front().start == 0, "首行缩进模式下首条 vrow 不对");
        g_settings.fi = false;
    }
}

// ── [6] 改动确实生效 ────────────────────────────────────────────────────────
#if HAVE_OLD
static void testChangeIsEffective(const std::vector<std::string> &lines) {
    printf("[6] 外置面下：新旧**必须**有分歧（护栏只证「没坏」，这条证「改了」）\n");
    if (g_builtin_face) { note("内置面，跳过"); return; }
    std::vector<MdLineInfo> info = mdClassifyLines(lines);
    std::vector<VRow> a = buildVrows(lines, &info, nullptr);
    std::vector<VRow> b = old::old_buildVrows(lines, &info, nullptr);
    bool differ = a.size() != b.size();
    for (size_t k = 0; !differ && k < a.size() && k < b.size(); k++)
        differ = (a[k].lineIdx != b[k].lineIdx || a[k].start != b[k].start ||
                  a[k].end != b[k].end);
    check(differ, "外置面下新旧输出完全一致 —— 改动没生效（被还原了？）");
    if (differ) note("新 %zu 条 vrow vs 旧 %zu 条：比例拉丁真的改变了折行", a.size(), b.size());
}
#endif

// ── [7] IME 编码行的例外 ────────────────────────────────────────────────────
static void testImeEncodingLineStaysMonospace() {
    printf("[7] 输入法编码行：装了外置字体，拉丁仍走半格\n");
    g_builtin_face = false;   // 这一条只在"装了外置字体"时才有意义
    StubFont ime = g_font;
    ime.latin_builtin_ = true;
    for (int c = 0x20; c < 0x80; c++)
        check(ime.charWidth((uint32_t)c) == ime.halfAdvance(),
              "编码行 ASCII 0x%02X 步进 %d != 半格 %d", c, ime.charWidth((uint32_t)c),
              ime.halfAdvance());
    check(ime.textWidth("nihao") == 5 * ime.halfAdvance(), "编码行 nihao 宽度不是 5 个半格");
    // 反证：同一个字体，内容面实例（钉住的开关关掉）就必须是比例拉丁
    check(g_font.textWidth("nihao") != 5 * g_font.halfAdvance(),
          "外置面下内容面的 nihao 竟然还是 5 个半格 —— 比例拉丁没生效");
}

// ── [8] 记号 lockstep ───────────────────────────────────────────────────────
static void testMarkerLockstep() {
    printf("[8] 记号：折行预留(mdIndentPx) == 渲染前缀(mdPrefixAdvancePx)\n");
    std::vector<std::string> L = {
        "# 标题", "## 二级", "- 无序项", "  - 嵌套", "- [ ] 待办", "- [x] 完成",
        "1. 有序", "10. 两位", "一、中文序号", "> 引用",
    };
    for (const std::string &l : L) {
        MdListMarker m = mdListMarker(l);
        MdLineInfo info;
        info.headingLevel = (l[0] == '#') ? (l[1] == '#' ? 2 : 1) : 0;
        info.quote = (l[0] == '>');
        info.list = m.ok && !m.task;
        info.task = m.ok && m.task;
        int reserve = mdIndentPx(l);
        int drawn = mdPrefixAdvancePx(l, info, false);
        if (info.headingLevel > 0) {
            check(reserve == 2 * g_font.halfAdvance(), "标题预留 %d != 2 格", reserve);
        } else if (info.quote) {
            // 引用：预留刻意 4 格、内容从 4 格 + 2px 起（改造前就有的 2px 差，保持不动）
            check(drawn - reserve == 2, "引用条的前缀与预留应差 2px（实际 %d）", drawn - reserve);
        } else {
            check(reserve == drawn, "行 [%s] 预留 %d != 画出来的前缀 %d（记号会压字/留缝）",
                  l.c_str(), reserve, drawn);
        }
    }
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--px") && i + 1 < argc) g_px = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screen") && i + 1 < argc) g_screen_w = atoi(argv[++i]);
    }
    g_font.line_height_ = g_px;
    g_font.px_ = g_px;

    std::vector<std::string> lines = corpus();

    for (int face = 0; face < 2; face++) {
        g_builtin_face = (face == 0);
        printf("\n════════ 内容面：%s（px=%d 屏宽=%d）════════\n",
               g_builtin_face ? "内置字体" : "外置字体（比例拉丁）", g_px, SCREEN_W);
        testMeasureDrawAgree(lines);
        testRoundTrip(lines);
        testWrapInvariants(lines);
#if HAVE_OLD
        testBuiltinEquivalence(lines);
        testChangeIsEffective(lines);
#else
        printf("[4][6] 没有 build/old_layout.inc（git 不可用），跳过新旧对拍\n");
#endif
        testEdges();
    }
    testImeEncodingLineStaysMonospace();
    testMarkerLockstep();

    printf("\n%s（通过 %d，失败 %d）\n", g_fail ? "有失败" : "全部通过", g_pass, g_fail);
    if (g_fail) {
        printf("\n提示：[4] 挂 = 内置面下的版式被改动了（奇数格高时允许 1px/汉字的收紧，"
               "但不许变松）；\n      [6] 挂 = 外置面下新旧一样（比例拉丁没生效）。\n");
    }
    return g_fail ? 1 : 0;
}
