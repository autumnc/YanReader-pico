// Host-side stubs: SettingsManager, the embedded-dictionary symbols, and the
// candidate-width callback. See stubs.h. Host-test only.
//
// The dictionary blobs are the REAL files under main/ime/, embedded the same way
// ESP-IDF's EMBED_FILES does it, via .incbin in a top-level asm block. The .incbin
// paths are relative to the directory run.sh compiles from (the repo root), so the
// blobs are never copied or regenerated.

#include "stubs.h"
#include "settings_manager.h"  // the real header, from main/ via -I

#include <cstdint>
#include <cstring>
#include <string>

// ── embedded blobs (symbols IME.cpp declares) ────────────────────────────────
// _binary_ime_table_pinyin_bin_{start,end}  -> IME.cpp:1291-1292
// _binary_liangfen_bin_{start,end}          -> IME.cpp:1297-1298
// _binary_english_words_txt_{start,end}     -> IME.cpp:1305-1306
__asm__(
    ".pushsection .rodata\n"
    ".globl _binary_ime_table_pinyin_bin_start\n"
    "_binary_ime_table_pinyin_bin_start:\n"
    ".incbin \"main/ime/ime_table_pinyin.bin\"\n"
    ".globl _binary_ime_table_pinyin_bin_end\n"
    "_binary_ime_table_pinyin_bin_end:\n"
    ".globl _binary_liangfen_bin_start\n"
    "_binary_liangfen_bin_start:\n"
    ".incbin \"main/ime/liangfen.bin\"\n"
    ".globl _binary_liangfen_bin_end\n"
    "_binary_liangfen_bin_end:\n"
    ".globl _binary_english_words_txt_start\n"
    "_binary_english_words_txt_start:\n"
    ".incbin \"main/ime/english_words.txt\"\n"
    ".globl _binary_english_words_txt_end\n"
    "_binary_english_words_txt_end:\n"
    ".popsection\n");

// ── SettingsManager ─────────────────────────────────────────────────────────
// IME.cpp only ever calls these six getters (grep "g_settings\." main/ime/IME.cpp).
// The other declared methods are never referenced from the compiled TUs, so they
// need no definitions here.

static std::string g_fuzzy = "zcs";
static std::string g_predictMode = "always";
static bool g_sentence = true;
static bool g_docContext = true;
static bool g_candidateHighlight = false;
static bool g_debugLog = false;

SettingsManager g_settings;

std::string SettingsManager::imeFuzzy() { return g_fuzzy; }
bool SettingsManager::imeSentence() { return g_sentence; }
bool SettingsManager::imeDocContext() { return g_docContext; }
std::string SettingsManager::imePredictMode() { return g_predictMode; }
bool SettingsManager::imeCandidateHighlight() { return g_candidateHighlight; }
bool SettingsManager::imeDebug() { return g_debugLog; }

namespace hostime {

void setFuzzy(const std::string &csv) { g_fuzzy = csv; }
void setSentence(bool on) { g_sentence = on; }
void setDocContext(bool on) { g_docContext = on; }
void setPredictMode(const std::string &mode) { g_predictMode = mode; }
void setCandidateHighlight(bool on) { g_candidateHighlight = on; }
void setDebugLog(bool on) { g_debugLog = on; }

int textWidthApprox(const char *s) {
    // Mirrors FontRenderer::charWidth with the 22px content font (line_height_ == 22,
    // halfAdvance() == 11).
    const int kLineHeight = 22;
    const int kHalfAdvance = 11;
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if (*p < 0x80) {
            w += kHalfAdvance;
            p++;
        } else {
            w += kLineHeight;
            // Skip the rest of the UTF-8 sequence.
            if ((*p & 0xE0) == 0xC0) p += 2;
            else if ((*p & 0xF0) == 0xE0) p += 3;
            else if ((*p & 0xF8) == 0xF0) p += 4;
            else p += 1;
        }
    }
    return w;
}

// imeCandidateLineWidth() = SCREEN_W - 12，SCREEN_W 跟着当前方向走（面板 1216×684）。
int candidateLineWidth() { return 1216 - 12; }          // 横屏
int candidateLineWidthPortrait() { return 684 - 12; }   // 竖屏

}  // namespace hostime

// ── 长活/补采样桩（主机测试用）─────────────────────────────────────────────
// IME.cpp 的 saveUserDictFile 写循环里调这两个：longop 记账（固件里是关中断重活
// 探针）和 input_tick_throttled（按 12ms 放闸补采触摸）。主机上没有它们，给空桩
// 只为过链接 —— 不放进 hostime 命名空间，IME.cpp 是无限定名直接调的。
extern "C" void longop_begin(const char *) {}
extern "C" void longop_end(void) {}
void input_tick_throttled() {}
