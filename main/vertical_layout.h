#pragma once

#include <string>
#include <vector>

#include "markdown_render.h"

// 竖排以「格」为最小单位:一行先分解为格子(md 关闭时每 UTF-8 字符一格,
// 开启时由 mdVerticalCells 替换/隐藏标记),列 = 连续 rowsPerCol 个格。
// 光标/选区/导航全部按格索引映射,字节只存在于格的 [start,end) 区间内。
struct VerticalCell {
    int start = 0;          // 原始行内字节起点
    int end = 0;            // 原始行内字节终点
    std::string glyph;      // 本格绘制的文本(通常一个字符)
    TextStyle ts;           // Markdown 行内样式
    bool bookTitle = false;  // 竖排书名号内容:左侧波浪线
};

struct VerticalCol {
    int lineIdx = 0;
    int start = 0;          // 行内格起始索引
    int end = 0;            // 行内格结束索引(不含)
};

struct VerticalData {
    std::vector<std::vector<VerticalCell>> cells;  // 每行格子(被折叠隐藏的行保持为空)
    std::vector<VerticalCol> cols;
};

struct VerticalLayoutMetrics {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int rowAdvance = 0;
    int colAdvance = 0;
    int rows = 1;
    int cols = 1;
};

enum class VerticalGuideStyle {
    Solid,
    Dash,
    Dot,
};

VerticalLayoutMetrics verticalMetrics(int x, int y, int w, int h);

// Chunk visible lines into columns of rowsPerCol cells. hiddenLines marks
// lines swallowed by folded headings; mdInfo (optional) enables Markdown
// marker cells via mdVerticalCells (foldedHeadings add the fold-marker cell).
VerticalData buildVerticalData(const std::vector<std::string> &lines, int rowsPerCol,
                               const std::vector<char> *hiddenLines,
                               const std::vector<MdLineInfo> *mdInfo = nullptr,
                               const std::set<int> *foldedHeadings = nullptr,
                               int cursorLineIdx = -1, int cursorBytePos = -1);

// bytePos 落在行内第几个格之前/所在格(全局格索引)。隐藏字节映射到其后
// 第一个可见格;行尾映射到格总数。
int verticalCellRow(const std::vector<VerticalCell> &cells, int bytePos);

int verticalFindCol(const VerticalData &data, const std::vector<std::string> &lines,
                    int lineIdx, int bytePos);

// 列内第 row 格对应的字节位置(越界时截到列尾字节)。
int verticalRowToByte(const std::vector<VerticalCell> &cells, int colStart, int colEnd, int row);

void drawVerticalCols(const std::vector<std::string> &lines, const VerticalData &data,
                      int scrollCol, const VerticalLayoutMetrics &m,
                      bool guideLine = false,
                      VerticalGuideStyle guideStyle = VerticalGuideStyle::Solid);

void drawVerticalCursor(const std::vector<std::string> &lines, const VerticalData &data,
                        int scrollCol, const VerticalLayoutMetrics &m,
                        int lineIdx, int bytePos);
