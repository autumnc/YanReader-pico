// Host-side driver for the real pinyin IME (main/ime/IME.cpp).
//
// Goal: reproduce and instrument the "stray remainder" candidate/commit bug without
// flashing the device. It drives the IME exactly the way main.cpp/screen_*.cpp does —
// begin(), setPageSize(), setWidthFn()/setDisplayWidth(), setActive(true), then
// handleKey() per character — and then commits a chosen candidate index.
//
// The private members (_candLen, _pageStart, _prefix, _remainder, ...) are read via
// the classic host-test `#define private public` trick, applied to IME.h in THIS FILE
// ONLY. Nothing under main/ is modified. Access specifiers do not affect layout, so
// the driver and the verbatim IME.cpp see the same object.
//
// Usage:
//   ime_driver <letters> [commitIdx] [--fixed]        diagnostic dump + commit
//   ime_driver --regress <letters> <word> [--fixed]   regression assertion, exit!=0 on fail
//
// Both modes print, after every keystroke:
//   _code, _displayCode, _pageStart, _page.size(), _all.size(), _curPage,
//   _partialStart, _remainder, _prefix, _maxMatchLen
// and a table of every candidate with its index, its _candLen entry and its
// _predictCandidateKeys entry (the parallel arrays commit() indexes into).
//
// REGRESSION (--regress): feeds the code one key at a time (each keystroke must run a
// lookup; the intermediate `zheg`/`jiush` state is what plants the stale
// _partialStart/_remainder), finds the target word on the current page, commits that
// exact index, and asserts commit returned true with out == the whole word, leaving no
// live composition (_code/_prefix/_remainder empty, composing() false).

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Pre-include everything IME.h / yong_dict.h pull in, so that `#define private public`
// below cannot corrupt a standard header. All of them are #pragma once / include-guarded.
#include "ime_config.h"
#include "yong_dict.h"

#define private public
#include "IME.h"
#undef private

#include "stubs.h"

// ── observation helpers ─────────────────────────────────────────────────────

// "*" marks candidates that live on the current page (commit()'s idx space).
static const char *onPage(const IME &ime, size_t i) {
    size_t end = (size_t)ime._pageStart + ime._page.size();
    return (i >= (size_t)ime._pageStart && i < end) ? "*" : " ";
}

static void dumpCandidates(IME &ime, const char *label) {
    printf("  candidates (%s): _all=%zu _candLen=%zu _predictKeys=%zu _page=%zu\n",
           label, ime._all.size(), ime._candLen.size(), ime._predictCandidateKeys.size(),
           ime._page.size());
    printf("     i  p  all[i]            candLen  predKey  page[i]\n");
    for (size_t i = 0; i < ime._all.size(); i++) {
        char clbuf[16];
        if (i < ime._candLen.size()) snprintf(clbuf, sizeof(clbuf), "%d", ime._candLen[i]);
        else snprintf(clbuf, sizeof(clbuf), "<none>");
        const std::string &pk =
            (i < ime._predictCandidateKeys.size()) ? ime._predictCandidateKeys[i] : std::string();
        // page[i - _pageStart] is what commit(i - _pageStart) would return.
        std::string pageText = "-";
        long rel = (long)i - ime._pageStart;
        if (rel >= 0 && rel < (long)ime._page.size()) pageText = ime._page[(size_t)rel];
        printf("  %4zu %s  %-16s   %6s  %-8s %s\n", i, onPage(ime, i), ime._all[i].c_str(),
               clbuf, pk.c_str(), pageText.c_str());
    }
}

static void dumpState(IME &ime, const char *label) {
    printf("  state[%s]:\n", label);
    printf("    _code='%s' _displayCode='%s' _codeOrig='%s'\n", ime._code.c_str(),
           ime.displayCode().c_str(), ime._codeOrig.c_str());
    printf("    _pageStart=%d _curPage=%d _page.size()=%zu _all.size()=%zu "
           "_pageStarts=%zu _pageSize=%d _pageSizeBase=%d\n",
           ime._pageStart, ime._curPage, ime._page.size(), ime._all.size(),
           ime._pageStarts.size(), ime._pageSize, ime._pageSizeBase);
    printf("    _partialStart=%d _remainder='%s' _prefix='%s' _maxMatchLen=%d "
           "_widthFn=%s _displayWidth=%d _fixedCandidatePaging=%d\n",
           ime._partialStart, ime._remainder.c_str(), ime._prefix.c_str(), ime._maxMatchLen,
           ime._widthFn ? "set" : "null", ime._displayWidth, ime._fixedCandidatePaging ? 1 : 0);
}

static bool listContains(const IME &ime, const char *needle) {
    for (const std::string &s : ime._all)
        if (s == needle) return true;
    return false;
}

// "The IME still thinks part of the typed code is unconsumed" — the bug's signature.
// Deliberately NOT `composing()`: that also counts the prediction phase, which is a
// legitimate post-commit state (committing 九十 leaves _predicting set). A leftover,
// by contrast, is _code / _prefix / _remainder / _ambigCommitted still holding text.
static bool leftoverComposition(const IME &ime) {
    return !ime._code.empty() || !ime._prefix.empty() || !ime._remainder.empty() ||
           !ime._ambigCommitted.empty();
}

// Page-relative index of `word` on the current page, or -1.
static int findOnPage(IME &ime, const std::string &word) {
    for (size_t i = 0; i < ime._page.size(); i++)
        if (ime._page[i] == word) return (int)i;
    return -1;
}

// ── scenarios ───────────────────────────────────────────────────────────────

struct Options {
    std::string letters;
    std::string word;      // regression target ("" = diagnostic mode)
    int commitIdx = 0;
    bool regress = false;
    bool widthPaging = true;
};

static void initIme(IME &ime, bool widthPaging) {
    // Same sequence as main.cpp:686-702.
    if (!ime.begin()) {
        fprintf(stderr, "ime.begin() FAILED (dictionary not parsed)\n");
        exit(2);
    }
    ime.setPageSize(7);
    if (widthPaging) {
        ime.setWidthFn(&hostime::textWidthApprox);
        ime.setDisplayWidth(hostime::candidateLineWidth());
    }
    ime.setActive(true);  // ensureUserDictLoaded() + reset(), as the app calls on focus
}

// Per-keystroke record, kept so the regression failure explanation can show exactly
// which keystroke planted the stale _partialStart/_remainder.
struct KeyTrace {
    std::string code;
    int partialStart = 0;
    std::string remainder;
    std::string prefix;
    size_t all = 0;
    int pageStart = 0;
    size_t pageSize = 0;
};

static void feedKeys(IME &ime, const std::string &letters, std::vector<KeyTrace> &trace) {
    for (char ch : letters) {
        std::string out;
        bool handled = ime.handleKey((unsigned char)ch, out);
        printf("\n-- key '%c' handled=%d out='%s'\n", ch, handled ? 1 : 0, out.c_str());
        dumpState(ime, "per-key");
        dumpCandidates(ime, "per-key");
        trace.push_back({ime._code, ime._partialStart, ime._remainder, ime._prefix, ime._all.size(),
                         ime._pageStart, ime._page.size()});
    }
}

int main(int argc, char **argv) {
    Options opt;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--regress")) opt.regress = true;
        else if (!strcmp(argv[i], "--fixed") || !strcmp(argv[i], "--no-width"))
            opt.widthPaging = false;
        else if (!strcmp(argv[i], "--no-sentence")) hostime::setSentence(false);
        else if (!strcmp(argv[i], "--no-docctx")) hostime::setDocContext(false);
        else if (!strcmp(argv[i], "--fuzzy") && i + 1 < argc) hostime::setFuzzy(argv[++i]);
        else if (!strcmp(argv[i], "--predict") && i + 1 < argc) hostime::setPredictMode(argv[++i]);
        else pos.push_back(argv[i]);
    }
    if (opt.regress) {
        if (pos.size() < 2) {
            fprintf(stderr, "usage: ime_driver --regress <letters> <word> [--fixed]\n");
            return 2;
        }
        opt.letters = pos[0];
        opt.word = pos[1];
    } else {
        if (pos.empty()) pos.push_back("zhege");
        opt.letters = pos[0];
        if (pos.size() > 1) opt.commitIdx = atoi(pos[1].c_str());
    }

    std::string tag = " mode=regress word=" + opt.word;
    printf("=== ime_driver: letters='%s' commitIdx=%d paging=%s%s ===\n", opt.letters.c_str(),
           opt.commitIdx, opt.widthPaging ? "width" : "fixed", opt.regress ? tag.c_str() : "");

    IME &ime = IME::getInstance();
    initIme(ime, opt.widthPaging);
    printf("begin(): loaded=%d scheme=%d\n", ime.loaded(), (int)ime.scheme());
    dumpState(ime, "after-init");

    // The key sequence must be fed one key at a time: the stale _partialStart/_remainder
    // that this harness is about are planted by the *intermediate* keystroke whose
    // lookup ends with an empty candidate table (zheg / jiush), then survive the next
    // keystroke's early-returning lookup.
    std::vector<KeyTrace> trace;
    feedKeys(ime, opt.letters, trace);

    int rc = 0;
    if (!opt.regress) {
        printf("\n=== commit(%d) ===\n", opt.commitIdx);
        std::string out;
        bool ret = ime.commit(opt.commitIdx, out);
        printf("  commit(%d) returned %d, out='%s' (len=%zu)\n", opt.commitIdx, ret ? 1 : 0,
               out.c_str(), out.size());
        dumpState(ime, "after-commit");
        dumpCandidates(ime, "after-commit");
        printf("  post-commit composition still live: composing=%d _code='%s' _remainder='%s' "
               "_prefix='%s'\n",
               ime.composing() ? 1 : 0, ime._code.c_str(), ime._remainder.c_str(),
               ime._prefix.c_str());
        printf("  post-commit candidate list contains '是'=%d '个'=%d '就'=%d '这'=%d\n",
               listContains(ime, "是") ? 1 : 0, listContains(ime, "个") ? 1 : 0,
               listContains(ime, "就") ? 1 : 0, listContains(ime, "这") ? 1 : 0);
    } else {
        int idx = findOnPage(ime, opt.word);
        printf("\n=== regression: commit the candidate '%s' ===\n", opt.word.c_str());
        printf("  keystroke trace (_code, _partialStart, _remainder, _prefix, _all, "
               "_pageStart, _page):\n");
        for (const KeyTrace &t : trace) {
            printf("    %-8s partialStart=%-4d remainder='%s' prefix='%s' all=%zu pageStart=%d "
                   "page=%zu\n",
                   t.code.c_str(), t.partialStart, t.remainder.c_str(), t.prefix.c_str(), t.all,
                   t.pageStart, t.pageSize);
        }
        if (idx < 0) {
            printf("  FAIL: '%s' is not on the current page (_pageStart=%d, _page=%zu); "
                   "cannot select it\n",
                   opt.word.c_str(), ime._pageStart, ime._page.size());
            return 3;
        }
        printf("  '%s' is at page index %d (flat candidate index %d), _candLen=%d\n",
               opt.word.c_str(), idx, ime._pageStart + idx,
               (ime._pageStart + idx < (int)ime._candLen.size())
                   ? ime._candLen[ime._pageStart + idx] : -1);

        std::string out;
        bool ret = ime.commit(idx, out);
        printf("  commit(%d) returned %d, out='%s' (len=%zu)\n", idx, ret ? 1 : 0, out.c_str(),
               out.size());
        dumpState(ime, "after-commit");
        printf("  post-commit: composing=%d _code='%s' _codeOrig='%s' _remainder='%s' "
               "_prefix='%s' _all=%zu\n",
               ime.composing() ? 1 : 0, ime._code.c_str(), ime._codeOrig.c_str(),
               ime._remainder.c_str(), ime._prefix.c_str(), ime._all.size());

        const bool ok = ret && out == opt.word && !leftoverComposition(ime);
        if (ok) {
            printf("  PASS: '%s' committed whole in one step, no leftover composition\n",
                   opt.word.c_str());
        } else {
            printf("  FAIL: expected out=='%s' with empty _code/_prefix/_remainder/_ambigCommitted "
                   "(composing=%d _predicting=%d)\n",
                   opt.word.c_str(), ime.composing() ? 1 : 0, ime._predicting ? 1 : 0);
            if (!out.empty() || leftoverComposition(ime)) {
                printf("        commit left _code='%s' _prefix='%s' -> a second commit would "
                       "yield '%s%s...' (the 'stray remainder' bug)\n",
                       ime._code.c_str(), ime._prefix.c_str(), out.c_str(), ime._code.c_str());
            }
            rc = 1;
        }
    }
    printf("=== done (rc=%d) ===\n", rc);
    return rc;
}
