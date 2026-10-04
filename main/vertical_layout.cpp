#include "vertical_layout.h"
#include "font_renderer.h"
#include "ui_helpers.h"

#include <algorithm>
#include "u8g2_shim.h"

static int nextUtf8Byte(const std::string &line, int pos) {
    if (pos >= (int)line.size()) return (int)line.size();
    pos++;
    while (pos < (int)line.size() && ((unsigned char)line[pos] & 0xC0) == 0x80) pos++;
    return pos;
}

static std::string utf8FromCp(uint32_t cp) {
    char out[5] = {0, 0, 0, 0, 0};
    if (cp <= 0x7F) {
        out[0] = (char)cp;
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
    }
    return out;
}

static uint32_t verticalPunctCp(uint32_t cp) {
    switch (cp) {
    case 0x3001: return 0xFE11;  // 、 → ︑
    case 0x3002: return 0xFE12;  // 。 → ︒
    case 0xFF0C: return 0xFE10;  // ， → ︐
    case 0xFF61: return 0xFE12;  // ｡ → ︒
    case 0xFF62: return 0xFE41;  // ｢ → ﹁
    case 0xFF63: return 0xFE42;  // ｣ → ﹂
    case 0xFF64: return 0xFE11;  // ､ → ︑
    case 0xFF1A: return 0xFE13;  // ： → ︓
    case 0xFF1B: return 0xFE14;  // ； → ︔
    case 0xFF01: return 0xFE15;  // ！ → ︕
    case 0xFF1F: return 0xFE16;  // ？ → ︖
    case 0x2026: return 0xFE19;  // … → ︙
    case 0xFF08: return 0xFE35;  // （ → ︵
    case 0xFF09: return 0xFE36;  // ） → ︶
    case 0x3008: return 0xFE3F;  // 〈 → ︿
    case 0x3009: return 0xFE40;  // 〉 → ﹀
    case 0x300A: return 0xFE3D;  // 《 → ︽
    case 0x300B: return 0xFE3E;  // 》 → ︾
    case 0x300C: return 0xFE41;  // 「 → ﹁
    case 0x300D: return 0xFE42;  // 」 → ﹂
    case 0x300E: return 0xFE43;  // 『 → ﹃
    case 0x300F: return 0xFE44;  // 』 → ﹄
    case 0x201C: return 0xFE41;  // “ → ﹁
    case 0x201D: return 0xFE42;  // ” → ﹂
    case 0x2018: return 0xFE43;  // ‘ → ﹃
    case 0x2019: return 0xFE44;  // ’ → ﹄
    case 0x2014: return 0xFE31;  // — → ︱
    default: return 0;
    }
}

static uint32_t firstCodepoint(const std::string &s) {
    const char *p = s.c_str();
    return FontRenderer::utf8Decode(p);
}

static std::string verticalDisplayChar(const std::string &s) {
    uint32_t cp = firstCodepoint(s);
    uint32_t vcp = verticalPunctCp(cp);
    if (!vcp) return s;
    std::string mapped = utf8FromCp(vcp);
    // If the font does not contain the vertical presentation glyph, drawText()
    // would only advance by half a cell. Keep the original punctuation visible.
    return g_font.textWidth(mapped.c_str()) > g_font.halfAdvance() ? mapped : s;
}

static bool cellIsGlyph(const VerticalCell &c, const char *glyph) {
    return c.glyph == glyph;
}

static bool cellIsAsciiAlnum(const std::vector<VerticalCell> &cells, int idx) {
    if (idx < 0 || idx >= (int)cells.size()) return false;
    const std::string &g = cells[idx].glyph;
    if (g.size() != 1) return false;
    unsigned char c = (unsigned char)g[0];
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static std::string verticalGlyphOrFallback(uint32_t cp, const char *fallback) {
    std::string mapped = utf8FromCp(cp);
    return g_font.textWidth(mapped.c_str()) > g_font.halfAdvance() ? mapped : fallback;
}

static void normalizeVerticalQuoteCells(std::vector<VerticalCell> &cells) {
    bool doubleOpen = true;
    bool singleOpen = true;
    std::vector<VerticalCell> out;
    out.reserve(cells.size());
    for (int i = 0; i < (int)cells.size(); i++) {
        auto &c = cells[i];
        if (cellIsGlyph(c, "\"") || cellIsGlyph(c, "\xEF\xBC\x82")) {  // " / ＂
            c.glyph = verticalGlyphOrFallback(doubleOpen ? 0xFE41 : 0xFE42, c.glyph.c_str());
            doubleOpen = !doubleOpen;
        } else if (cellIsGlyph(c, "'") || cellIsGlyph(c, "\xEF\xBC\x87")) {  // ' / ＇
            if (cellIsGlyph(c, "'") && cellIsAsciiAlnum(cells, i - 1) &&
                cellIsAsciiAlnum(cells, i + 1))
                continue;
            c.glyph = verticalGlyphOrFallback(singleOpen ? 0xFE43 : 0xFE44, c.glyph.c_str());
            singleOpen = !singleOpen;
        }
        out.push_back(c);
    }
    cells.swap(out);
}

static bool drawVerticalPunctCell(int x, int y, uint32_t cp) {
    int lh = g_font.lineHeight();
    if (cp == 0x2026 || cp == 0xFE19) {  // … / ︙
        int px = x + lh / 2 - 1;
        int y1 = y + 3;
        int y2 = y + lh / 2 - 1;
        int y3 = y + lh - 5;
        u8g2_DrawBox(g_u8g2, px, y1, 3, 3);
        u8g2_DrawBox(g_u8g2, px, y2, 3, 3);
        u8g2_DrawBox(g_u8g2, px, y3, 3, 3);
        return true;
    }
    if (cp == 0x3002 || cp == 0xFE12 || cp == 0xFF61) {  // 。 / ︒ / ｡
        int px = x + lh - 6;
        int py = y + 3;
        u8g2_DrawBox(g_u8g2, px, py, 4, 1);
        u8g2_DrawBox(g_u8g2, px, py + 3, 4, 1);
        u8g2_DrawVLine(g_u8g2, px, py, 4);
        u8g2_DrawVLine(g_u8g2, px + 3, py, 4);
        return true;
    }
    if (cp == 0xFF0C || cp == 0x3001 || cp == 0xFE10 || cp == 0xFE11 || cp == 0xFF64) {
        int px = x + lh - 5;
        int py = y + 3;
        u8g2_DrawBox(g_u8g2, px, py, 2, 2);
        u8g2_DrawPixel(g_u8g2, px - 1, py + 2);
        u8g2_DrawPixel(g_u8g2, px - 2, py + 3);
        return true;
    }
    if (cp == 0xFF1A || cp == 0xFE13) {  // ： / ︓
        int px = x + lh - 5;
        u8g2_DrawBox(g_u8g2, px, y + 4, 3, 3);
        u8g2_DrawBox(g_u8g2, px, y + lh - 7, 3, 3);
        return true;
    }
    if (cp == 0xFF1B || cp == 0xFE14) {  // ； / ︔
        int px = x + lh - 5;
        int py = y + lh - 7;
        u8g2_DrawBox(g_u8g2, px, y + 4, 3, 3);
        u8g2_DrawBox(g_u8g2, px, py, 2, 2);
        u8g2_DrawPixel(g_u8g2, px - 1, py + 2);
        return true;
    }
    return false;
}

VerticalLayoutMetrics verticalMetrics(int x, int y, int w, int h) {
    VerticalLayoutMetrics m;
    m.x = x;
    m.y = y;
    m.w = w;
    m.h = h;
    m.rowAdvance = g_font.lineHeight() + 2;
    m.colAdvance = g_font.lineHeight() + 6;
    m.rows = std::max(1, h / m.rowAdvance);
    m.cols = std::max(1, w / m.colAdvance);
    return m;
}

VerticalData buildVerticalData(const std::vector<std::string> &lines, int rowsPerCol,
                               const std::vector<char> *hiddenLines,
                               const std::vector<MdLineInfo> *mdInfo,
                               const std::set<int> *foldedHeadings,
                               int cursorLineIdx, int cursorBytePos) {
    VerticalData data;
    data.cells.resize(lines.size());
    rowsPerCol = std::max(1, rowsPerCol);
    for (int li = 0; li < (int)lines.size(); li++) {
        if (hiddenLines && li < (int)hiddenLines->size() && (*hiddenLines)[li]) continue;
        const std::string &line = lines[li];
        auto &cs = data.cells[li];
        // 每格预留一格：UTF-8 一格 ≥1 字节，line.size() 一定是上界。不预留的话
        // 这个 vector 从 0 开始按倍增长，一行 N 个字要 N 次重分配＋搬移——竖排是
        // 每次重绘、每次方向键都全量重建，这一下把每行的分配次数从 O(logN) 降到 1。
        cs.reserve(line.size());
        if (mdInfo && li < (int)mdInfo->size()) {
            bool folded = foldedHeadings && foldedHeadings->count(li) > 0;
            int mdCursor = li == cursorLineIdx ? cursorBytePos : -1;
            for (const auto &vc : mdVerticalCells(line, (*mdInfo)[li], folded, mdCursor))
                cs.push_back({vc.start, vc.end, vc.glyph, vc.ts, vc.ts.bookTitle});
        } else {
            for (int p = 0; p < (int)line.size();) {
                int n = nextUtf8Byte(line, p);
                cs.push_back({p, n, line.substr(p, n - p), TextStyle{}, false});
                p = n;
            }
        }
        normalizeVerticalQuoteCells(cs);
        int start = 0, row = 0;
        for (int i = 0; i < (int)cs.size(); i++) {
            if (row == rowsPerCol) {
                data.cols.push_back({li, start, i});
                start = i;
                row = 0;
            }
            row++;
        }
        data.cols.push_back({li, start, (int)cs.size()});
    }
    if (data.cols.empty()) data.cols.push_back({0, 0, 0});
    return data;
}

int verticalCellRow(const std::vector<VerticalCell> &cells, int bytePos) {
    int row = 0;
    for (const auto &c : cells)
        if (c.end <= bytePos) row++;
    return row;
}

int verticalFindCol(const VerticalData &data, const std::vector<std::string> &lines,
                    int lineIdx, int bytePos) {
    (void)lines;
    if (lineIdx < 0 || lineIdx >= (int)data.cells.size()) return 0;
    const auto &cells = data.cells[lineIdx];
    int rowB = verticalCellRow(cells, bytePos);
    int fallback = -1;
    for (int i = 0; i < (int)data.cols.size(); i++) {
        const auto &c = data.cols[i];
        if (c.lineIdx != lineIdx) continue;
        fallback = i;
        if (rowB >= c.start && rowB < c.end) return i;
        if (rowB == c.end) {
            if (c.end == (int)cells.size()) return i;
            if (c.start == c.end) return i;
        }
    }
    return fallback >= 0 ? fallback : 0;
}

int verticalRowToByte(const std::vector<VerticalCell> &cells, int colStart, int colEnd, int row) {
    int idx = colStart + row;
    if (idx >= colStart && idx < colEnd && idx < (int)cells.size()) return cells[idx].start;
    if (colEnd > colStart && colEnd <= (int)cells.size()) return cells[colEnd - 1].end;
    return 0;
}

static void drawOneVerticalChar(int x, int y, const std::string &s, const TextStyle &ts) {
    TextStyle glyphStyle = ts;
    glyphStyle.underline = false;
    glyphStyle.italic = false;
    glyphStyle.strike = false;
    glyphStyle.emph = false;
    glyphStyle.invert = false;
    glyphStyle.bookTitle = false;

    u8g2_SetDrawColor(g_u8g2, 0);
    if (ts.invert) {
        u8g2_DrawBox(g_u8g2, x, y, g_font.lineHeight(), g_font.lineHeight());
        u8g2_SetDrawColor(g_u8g2, 1);
    }
    if (!drawVerticalPunctCell(x, y, firstCodepoint(s))) {
        std::string draw = verticalDisplayChar(s);
        int w = g_font.textWidth(draw.c_str());
        int dx = x + (g_font.lineHeight() - w) / 2;
        g_content_font.drawTextStyled(dx, y + g_font.ascent(), draw.c_str(), glyphStyle);
    }
    u8g2_SetDrawColor(g_u8g2, 0);
}

static void drawVerticalWave(int x, int y, int h) {
    if (h <= 0) return;
    for (int dy = 0; dy < h; dy++) {
        int phase = dy & 7;
        int px = x + (phase < 2 ? 0 : (phase < 4 ? 1 : (phase < 6 ? 2 : 1)));
        u8g2_DrawPixel(g_u8g2, px - 1, y + dy);
        u8g2_DrawPixel(g_u8g2, px, y + dy);
    }
}

static void drawVerticalDecorationRun(int x, const VerticalLayoutMetrics &m,
                                      int rowStart, int rowEnd, bool italic, bool underline,
                                      bool strike, bool emph, bool bookTitle) {
    if (rowStart >= rowEnd) return;
    int y0 = m.y + rowStart * m.rowAdvance + 2;
    int h = (rowEnd - rowStart - 1) * m.rowAdvance + g_font.lineHeight() - 4;
    if (h <= 0) return;
    u8g2_SetDrawColor(g_u8g2, 0);
    if (italic) {
        u8g2_DrawVLine(g_u8g2, x - 2, y0, h);
        u8g2_DrawVLine(g_u8g2, x - 1, y0, h);
    } else if (underline) {
        u8g2_DrawVLine(g_u8g2, x - 2, y0, h);
    }
    if (strike) {
        int sx = x + g_font.lineHeight() / 2;
        u8g2_DrawVLine(g_u8g2, sx, y0, h);
        u8g2_DrawVLine(g_u8g2, sx + 1, y0, h);
    }
    if (bookTitle) drawVerticalWave(x - 4, y0, h);
    if (emph) {
        for (int row = rowStart; row < rowEnd; row++) {
            int cy = m.y + row * m.rowAdvance + g_font.lineHeight() / 2 - 1;
            u8g2_DrawBox(g_u8g2, x - 6, cy, 3, 3);
        }
    }
}

static void drawVerticalDecorations(int x, const VerticalLayoutMetrics &m,
                                    const std::vector<VerticalCell> &cells,
                                    int start, int end) {
    int runStart = -1;
    bool ri = false, ru = false, rs = false, re = false, rb = false;
    auto flush = [&](int rowEnd) {
        if (runStart >= 0) drawVerticalDecorationRun(x, m, runStart, rowEnd, ri, ru, rs, re, rb);
        runStart = -1;
    };

    for (int i = start; i < end; i++) {
        const auto &cell = cells[i];
        bool blank = cell.glyph == " ";
        bool it = cell.ts.italic;
        bool u = cell.ts.underline;
        bool s = cell.ts.strike;
        bool e = cell.ts.emph && !blank;
        bool b = cell.bookTitle && !blank;
        int row = i - start;
        if (!it && !u && !s && !e && !b) {
            flush(row);
            continue;
        }
        if (runStart < 0 || it != ri || u != ru || s != rs || e != re || b != rb) {
            flush(row);
            runStart = row;
            ri = it;
            ru = u;
            rs = s;
            re = e;
            rb = b;
        }
    }
    flush(end - start);
}

static void drawVerticalGuideLineAt(int gx, const VerticalLayoutMetrics &m,
                                    VerticalGuideStyle style) {
    int y0 = m.y;
    int h = m.h;
    if (h <= 0) return;
    u8g2_SetDrawColor(g_u8g2, 0);
    if (style == VerticalGuideStyle::Solid) {
        u8g2_DrawVLine(g_u8g2, gx, y0, h);
    } else if (style == VerticalGuideStyle::Dash) {
        for (int y = y0; y < y0 + h; y += 8) {
            int seg = std::min(5, y0 + h - y);
            if (seg > 0) u8g2_DrawVLine(g_u8g2, gx, y, seg);
        }
    } else {
        for (int y = y0; y < y0 + h; y += 4)
            u8g2_DrawPixel(g_u8g2, gx, y);
    }
}

static void drawVerticalGuideLines(const VerticalLayoutMetrics &m, VerticalGuideStyle style) {
    int lh = g_font.lineHeight();
    int rightTextX = m.x + m.w - m.colAdvance;
    int rightGuideX = rightTextX + lh + 3;
    for (int i = 0; i <= m.cols; i++)
        drawVerticalGuideLineAt(rightGuideX - i * m.colAdvance, m, style);
}

void drawVerticalCols(const std::vector<std::string> &lines, const VerticalData &data,
                      int scrollCol, const VerticalLayoutMetrics &m,
                      bool guideLine, VerticalGuideStyle guideStyle) {
    (void)lines;
    int right = m.x + m.w - m.colAdvance;
    if (guideLine) drawVerticalGuideLines(m, guideStyle);
    for (int ci = 0; ci < m.cols; ci++) {
        int colIdx = scrollCol + ci;
        if (colIdx < 0 || colIdx >= (int)data.cols.size()) continue;
        const auto &col = data.cols[colIdx];
        if (col.lineIdx < 0 || col.lineIdx >= (int)data.cells.size()) continue;
        const auto &cells = data.cells[col.lineIdx];
        int x = right - ci * m.colAdvance;
        int row = 0;
        for (int i = col.start; i < col.end && row < m.rows; i++, row++)
            drawOneVerticalChar(x, m.y + row * m.rowAdvance, cells[i].glyph, cells[i].ts);
        drawVerticalDecorations(x, m, cells, col.start, col.end);
    }
}

void drawVerticalCursor(const std::vector<std::string> &lines, const VerticalData &data,
                        int scrollCol, const VerticalLayoutMetrics &m,
                        int lineIdx, int bytePos) {
    (void)lines;
    if (lineIdx < 0 || lineIdx >= (int)data.cells.size()) return;
    int colIdx = verticalFindCol(data, lines, lineIdx, bytePos);
    if (colIdx < scrollCol || colIdx >= scrollCol + m.cols) return;
    const auto &col = data.cols[colIdx];
    int row = verticalCellRow(data.cells[lineIdx], bytePos) - col.start;
    if (row >= m.rows) row = m.rows - 1;
    int ci = colIdx - scrollCol;
    int x = m.x + m.w - m.colAdvance - ci * m.colAdvance;
    int y = m.y + row * m.rowAdvance;
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, x + 2, y + m.rowAdvance - 4, g_font.lineHeight() - 4, 3);
    u8g2_SetDrawColor(g_u8g2, 1);
}
