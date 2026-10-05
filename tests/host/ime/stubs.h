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

// The exact pixel width main.cpp hands the IME in landscape (SCREEN_W - 12, and
// imeCandidateLineWidth() == SCREEN_W - 12 on the 1216px panel).
int candidateLineWidth();

// Settings the IME snapshots in begin() (see IME.cpp:1345-1349). Defaults mirror
// settings_manager.cpp's real defaults.
void setFuzzy(const std::string &csv);            // imeFuzzy(),        default "zcs"
void setSentence(bool on);                        // imeSentence(),     default true
void setDocContext(bool on);                      // imeDocContext(),   default true
void setPredictMode(const std::string &mode);     // imePredictMode(),  default "always"
void setCandidateHighlight(bool on);              // imeCandidateHighlight(), default false
void setDebugLog(bool on);                        // imeDebug(),        default false

}  // namespace hostime
