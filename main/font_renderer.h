#pragma once

#include <cstdint>
#include <cstddef>

// 字体渲染器：官方 TTF 抗锯齿 (builtin.ttf / SD 外置字体) + NF-Propo 等宽图标字体。
//
// 三路字形路由：
//   - ASCII (<0x80)：NF-Propo 等宽 0.5em 字形；**装了外置字体时改走用户字体的
//     比例拉丁字形**（仍画进那个固定半格，见 drawCellGlyph）
//   - 图标 (PUA/全角/几何符号)：NF-Propo 子集按需光栅化 —— 特殊符号与图标恒用图标字体
//   - 其余 (CJK 等)：官方 ttf_font 层 (builtin.ttf / SD 字体) 抗锯齿
//
// 保留等宽 cell 模型：ASCII = 1 cell = halfAdvance()，CJK/图标 = 2 cells = cjkAdvance()。
//
// 字体面：role_ 是**本实例绘制文本字形时用哪个字面**（见 ttf_font.h 的 TTF_ROLE_*）。
// 界面文本也用用户选的字体，所以 g_font 的文本面就是内容面；"必须固定内置字体"的
// 只有虚拟键盘，它有自己的实例 g_vk_font。
struct TextStyle {
    bool bold = false;      // synthetic bold: glyph drawn twice, 1px offset
    bool underline = false; // line below the whole segment
    bool italic = false;    // vertical-only italic marker decoration
    bool strike = false;    // line through vertical center
    bool invert = false;    // reverse video: dark box + light glyphs
    bool emph = false;      // emphasis dot (着重号) under each character
    bool bookTitle = false; // vertical-only book title wave decoration
};

// 与 main/font/ttf_font.h 的 TTF_ROLE_* 同名同值。此处复制一份，避免让每个包含
// font_renderer.h 的 TU 都被拉进 epdiy.h（GfxRenderer.cpp 同样做法的同源理由）。
// 两处定义都是 `0`/`1`，重复定义完全一致 → 合法，无告警。
#ifndef TTF_ROLE_CONTENT
#define TTF_ROLE_CONTENT 0
#define TTF_ROLE_UI      1
#endif

class FontRenderer {
public:
    // 初始化并选定文本面(见 ttf_font.h 的 TTF_ROLE_*)。
    // CONTENT = 用户在设置里选的字体（界面文本、编辑器正文、输入法候选、日记查看器…）；
    // UI      = 内置 builtin.ttf。**只有虚拟键盘该传 UI**（它的字体固定，见 g_vk_font）。
    // 角色必须在这里定下：begin() 里的 ttf_font_init()/ttf_font_ready() 是内容固定的，
    // 之后每次绘制再由本实例的 role_ 选面。
    // ⚠ begin() 会跑一遍 setSize(20)，而格子模型是**所有实例共享的静态量** ——
    //   多调一次就是把用户选的字号冲回 20pt。所以一个进程里只由"主"实例调它
    //   （见 g_vk_font 的静态构造）。
    bool begin(int role);
    int  role() const { return role_; }
    // 构造时定文本面，供"不调 begin() 的从属实例"用（它们借用共享格子模型）。
    explicit FontRenderer(int role = TTF_ROLE_CONTENT) : role_(role) {}

    // Switch between available font sizes (18 | 20 | 22)
    bool setSize(int fontSize);

    // 换字体后强制重读当前字体的度量(ascender/descent)并清字形缓存。
    bool reloadFont();

    // Draw UTF-8 text at (x, y). y is baseline.
    // Returns width consumed.
    int drawText(int x, int y, const char *text, bool invert = false);

    // Draw UTF-8 text with style flags (bold/underline/strike/invert/emph).
    // Width is identical to drawText/textWidth — marker-free passthrough.
    int drawTextStyled(int x, int y, const char *text, const TextStyle &ts);

    // Get text width in pixels (UTF-8)
    int textWidth(const char *text);

    // Get glyph advance width (cell model)
    int charWidth(uint32_t codepoint);

    // Font metrics
    int lineHeight() const { return line_height_; }
    int ascent() const { return ascent_; }
    int descent() const { return descent_; }
    int fontSize() const { return font_size_; }

    // Cell-based layout helpers (monospace assumption)
    int cjkAdvance() const { return line_height_; }     // fullwidth advance (pixels)
    int halfAdvance() const { return line_height_ / 2; } // halfwidth advance (pixels)

    // Check if font is loaded
    bool loaded() const { return loaded_; }

    // Decode a single UTF-8 character from *str, advance str pointer
    static uint32_t utf8Decode(const char *&str);

private:
    // Draw a single glyph (any route) centered in its [cellW × line_height_] cell.
    // invert=true 填黑 cell 后画白字。bold=true 加描。
    void drawCellGlyph(int x, int y, int cellW, uint32_t cp, bool invert, bool bold);

    // 状态栏电池/蓝牙图标 (E001/E002/E018-E02D)：程序化绘制。
    void drawStatusSymbol(int x, int y, uint32_t cp, bool invert, int cellW);

    int role_ = TTF_ROLE_CONTENT;   // 本实例绘制**文本字形**时选用的字面（非共享）

    // ── 共享格子模型 ─────────────────────────────────────────────────────
    // 这些量**所有实例共用一份**（static）：格子几何由字号档决定，与"用户选了
    // 哪个字体"无关（ascent_ 也刻意锚在内置面上）。于是：
    //   * 任一实例 setSize() 即更新全局格子，g_content_font 根本不必 setSize()；
    //   * main.cpp/pjournal_app.cpp 那 15 处 g_font.setSize(n) 一行都不用改；
    //   * g_content_font.textWidth() 与 g_font.textWidth() 恒等 → 所有测量代码不动，
    //     只有**绘制**调用点要换成 g_content_font；
    //   * 换用户字体时格子纹丝不动，Markdown 记号盒 [y-ascent_, +line_height_) 也不漂移。
    static int font_size_;
    static int line_height_;   // 22pt → 50px, 20pt → 45px, 18pt → 41px
    static int ascent_;
    static int descent_;
    static int px_;            // TTF 光栅像素高 (= line_height_)
    static bool loaded_;
};

// 三个实例的差别**只有文本面**一处，格子模型/字号全是共享的（见下面的静态量）。
extern FontRenderer g_font;          // 界面文本：内容面（= 用户选的字体；没装外置字体时它就是内置）
extern FontRenderer g_content_font;  // 内容文本：编辑器正文 / 输入法候选 / 日记查看器（同上）
// 虚拟键盘专用（main/editor_vk.cpp）：**文本面钉在内置面上**，键盘字体固定，不跟
// 用户选的字体走。直接静态构造，**不调 begin()** —— begin() 里的 setSize(20) 会把
// 共享格子模型重置回 20pt，把用户选的字号冲掉；它借用的正是 g_font 建好的格子。
extern FontRenderer g_vk_font;
