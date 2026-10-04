#pragma once

#include <set>
#include <string>
#include <vector>

#include "font_renderer.h"

// Markdown live rendering for the editor.
// Block markers (#, -, >, task) are width-preserving: replaced by same-width
// symbols. Paired inline markers (** , ==, ~~, `, *) are hidden without taking
// space; mdVisualX/mdVrowX are marker-aware so cursor / selection / wrapped-row
// placement stay aligned with what's drawn.

struct MdLineInfo {
    int headingLevel = 0;  // 1..6, 0 = not a heading
    bool list = false;
    bool task = false;     // "- [ ]" / "- [x]"
    bool quote = false;
    bool inCodeBlock = false;  // between code fences
    bool hr = false;           // horizontal rule or code fence line
};

// List-family marker geometry (unordered `- `, ordered `1. `/`1、`, task `- [ ]`),
// including any leading whitespace (nested lists). ok=false if `line` isn't a
// list block. start = byte offset after leading whitespace; len = marker byte
// length (incl. trailing space); cells = visual cells the marker occupies, i.e.
// the content x offset (in cells) relative to start. Used by both the renderer
// and ui_helpers buildVrows so marker width / indent stay in sync.
struct MdListMarker {
    bool ok = false;
    bool task = false;
    bool ordered = false;
    int start = 0;
    int len = 0;
    int cells = 0;
};
MdListMarker mdListMarker(const std::string &line);

// Parse a leading Chinese numeral (零/一/二/…/十/十一/…) starting at line[from].
// Returns the value (0..9999) or -1 if `from` isn't a Chinese numeral char.
// numLen = consumed bytes (0 when -1).
int mdCnNumValue(const std::string &line, int from, int &numLen);

// 1..9999 → Chinese numeral string (一/二/…/十/十一/…); other values → decimal.
// Used by the editor's list auto-continuation.
std::string mdCnNumeral(int n);

// Classify every line of the document in one pass.
std::vector<MdLineInfo> mdClassifyLines(const std::vector<std::string> &lines);

// Shared heading-fold semantics: hidden[li]=1 for every line swallowed by a
// folded heading (nested folds, inner folded headings inside a fold region
// stay hidden). Returns all zeros when mdInfo is null or nothing is folded.
// Used by both horizontal buildVrows and vertical buildVerticalCols so the
// two layouts hide exactly the same lines.
std::vector<char> mdFoldHiddenLines(const std::vector<std::string> &lines,
                                    const std::vector<MdLineInfo> *mdInfo,
                                    const std::set<int> *foldedHeadings);

// Fold marker appended to a folded heading (uF09DA).
extern const char *kFoldMarker;

// Vertical-mode cell decomposition of a line. Block/inline markers become
// replacement glyph cells (heading icon, bullet, task box) or are dropped
// entirely — hidden marker bytes (** , ~~ , ` , >_ , link brackets) occupy no
// cell, so cursor mapping is cell-based (see vertical_layout.h). Inline style
// flags are kept per visible cell and rendered by vertical_layout. When
// cursorBytePos is inside an inline construct, that construct is emitted raw.
struct MdVCell {
    int start = 0, end = 0;  // byte range in the raw line
    std::string glyph;       // text drawn in this cell (usually one char)
    TextStyle ts;            // bold/underline/strike/invert/emph for this cell
};
std::vector<MdVCell> mdVerticalCells(const std::string &line, const MdLineInfo &info,
                                     bool folded = false, int cursorBytePos = -1);

// Draw the [start, end) byte slice of `line` (a vrow) at (x, y) with markdown
// styles. y is the text baseline. Byte offsets match buildVrows output.
// cursorBytePos >= 0 reveals the inline markdown construct containing that byte
// as raw text; the rest of the line keeps normal hidden-marker rendering.
void mdDrawVrow(int x, int y, const std::string &line, int start, int end,
                const MdLineInfo &info, int indentCells = 0, int cursorBytePos = -1,
                bool folded = false);

// Visual x (px from the line's left edge) at which raw byte bytePos renders.
// For heading/task/quote lines the content sits at a fixed prefix offset
// (2/3/4 cells) instead of its raw width. Cursor / selection must use this to
// stay aligned with draw. A folded heading widens its prefix to 4 cells
// (heading icon + uF09DA fold marker), shifting the title right 2 cells.
int mdVisualX(const std::string &line, const MdLineInfo &info, int bytePos,
              int cursorBytePos = -1, bool folded = false);

// Visual x (px) of bytePos within a vrow that starts at raw byte vrowStart.
// Continuation vrows repeat the prefix indent. Use for all vrow drawing /
// cursor / selection positioning.
int mdVrowX(const std::string &line, const MdLineInfo &info, int bytePos, int vrowStart,
            int indentCells = 0, int cursorBytePos = -1, bool folded = false);

// Enable/disable markdown rendering (Settings "Markdown渲染"). When disabled,
// every line is drawn as raw text with the marker characters untouched.
void mdSetRenderEnabled(bool on);
