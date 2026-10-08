// Host-side stubs for the bits of the firmware environment that IME.cpp needs.
//
// Everything here exists only to satisfy the linker and to let the driver poke the
// few settings the IME reads. No IME logic is reimplemented: the real main/ime/IME.cpp,
// yong_dict.cpp and yong_pinyin.cpp are compiled verbatim.
#pragma once

#include <string>

namespace hostime {

// Candidate width callback (the analogue of main.cpp's setWidthFn lambda). The device
// measures with FontRenderer::charWidth: ASCII -> half advance, everything else ->
// line height. Reproduced here with the 22px content font that drawIMEUI uses.
int textWidthApprox(const char *s);

// 候选行宽度 = SCREEN_W - 12，而 SCREEN_W 是**当前方向**的逻辑宽（面板物理
// 1216×684）：横屏 1216-12、竖屏 684-12。main.cpp 现在把 imeCandidateLineWidth()
// 这个函数本身作为"现取行宽"的回调交给 IME（setDisplayWidthFn），不再是开机一次的
// 常数 —— 所以两档都要在这里量得到，见 ime_driver 的 --paging。
int candidateLineWidth();          // 横屏：1216 - 12
int candidateLineWidthPortrait();  // 竖屏：684  - 12

// Settings the IME snapshots in begin() (see IME.cpp:1345-1349). Defaults mirror
// settings_manager.cpp's real defaults.
void setFuzzy(const std::string &csv);            // imeFuzzy(),        default "zcs"
void setSentence(bool on);                        // imeSentence(),     default true
void setDocContext(bool on);                      // imeDocContext(),   default true
void setPredictMode(const std::string &mode);     // imePredictMode(),  default "always"
void setCandidateHighlight(bool on);              // imeCandidateHighlight(), default false
void setDebugLog(bool on);                        // imeDebug(),        default false

}  // namespace hostime
