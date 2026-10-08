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
//   ime_driver --enter <letters> [--highlight]        Enter 直接编码上屏的断言
//
// ENTER (--enter): 输入中按回车必须**原样上屏还没有选中的编码**（typing nihao + Enter
// → 正文里出现 "nihao"，不是换行、也不是第一个候选）。默认「候选高亮」关着，所以
// out 必须逐字节等于输入串；--highlight 把 imeCandidateHighlight 打开后，回车改成
// 上屏**高亮那个候选**（_highlightSelectMode 分支，见 IME.cpp:6132）。
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
    bool enter = false;      // --enter: 断言"输入中按回车直接编码上屏"
    bool highlight = false;  // --highlight: 打开「候选高亮」（回车改成上屏高亮候选）
    bool paging = false;     // --paging: 同一个编码在横/竖屏两档行宽下各分一次页，比个数
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

// ── 候选分页的行宽：一页装得下的个数随方向变，且一页绝不超过 9 个 ──────────────
// 候选行宽度是方向相关的（SCREEN_W 是当前方向的逻辑宽：横屏 1216−12=1204、竖屏
// 684−12=672），而分页(_pageStarts)是按像素宽切出来的。同一个编码在这两档宽度下各
// 分一次页，断言三件事：
//   1) 每页候选的实测总宽 ≤ 当次行宽（分页的契约：装不下就换页）；
//   2) 每页 ≤ 9 个 —— 页内编号 1..9，数字键刚好覆盖整页（用户 2026-10-08：横屏也
//      别超过 9 个，不然数字键选不到第 10 个以后）；
//   3) 横屏 ≥ 竖屏（宽的那一档不该反而装得少）。
// 这是**护栏**，不是拿 pre-fix 反证的那种：设备上"一页里塞了放不下的候选"是因为
// main.cpp 那时喂的是开机那一刻的快照（横屏），跟 IME 对"喂进来的宽度"的契约无关 ——
// 所以这条只用 setDisplayWidth()，pre-fix 的 IME.cpp 一样编得过、也一样过。
static void replay(IME &ime, const std::string &letters, int w) {
    ime.reset();
    ime.setDisplayWidth(w);   // = buildPage 每次现取到的那个行宽
    for (char ch : letters) {
        std::string out;
        ime.handleKey((unsigned char)ch, out);
    }
}

static int pagingMatrix(IME &ime, const std::string &letters) {
    struct Row { const char *name; int w; };
    const Row rows[] = {{"横屏", hostime::candidateLineWidth()},
                        {"竖屏", hostime::candidateLineWidthPortrait()}};
    int sizes[2] = {0, 0};
    int rc = 0;
    for (int r = 0; r < 2; r++) {
        replay(ime, letters, rows[r].w);
        // 与 buildPage 的分页口径逐字对齐：" N." 前缀（带前导空格）+ 候选文本，
        // 编号是页内序号（1 起）。差一个像素都会让"页宽 ≤ 行宽"这条断言失去意义。
        int pageW = 0;
        std::string page;
        for (size_t i = 0; i < ime._page.size(); i++) {
            char num[16];
            snprintf(num, sizeof(num), " %d.", (int)i + 1);
            pageW += ime._widthFn(num) + ime._widthFn(ime._page[i].c_str());
            if (!page.empty()) page += "  ";
            page += ime._page[i];
        }
        sizes[r] = (int)ime._page.size();
        const bool fits = pageW <= rows[r].w;
        const bool numKeys = sizes[r] <= 9;   // 数字键 1..9 必须覆盖整页
        if (!fits || !numKeys) rc = 1;
        printf("  %s %4dpx: 一页 %d 个，页宽 %4dpx%s\n", rows[r].name, rows[r].w, sizes[r], pageW,
               !fits ? "   ← 超出当次行宽"
                     : (numKeys ? "" : "   ← 多于 9 个，数字键够不着"));
        printf("           %s\n", page.c_str());
    }
    if (sizes[1] > sizes[0]) {
        printf("  FAIL: 竖屏一页 %d 个 > 横屏 %d 个 —— 宽的那一档反而装得少\n", sizes[1], sizes[0]);
        rc = 1;
    } else {
        printf("  PASS: 横屏 %d 个 ≥ 竖屏 %d 个，两页都 ≤ 9\n", sizes[0], sizes[1]);
    }
    return rc;
}

int main(int argc, char **argv) {
    Options opt;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--regress")) opt.regress = true;
        else if (!strcmp(argv[i], "--enter")) opt.enter = true;
        else if (!strcmp(argv[i], "--paging")) opt.paging = true;
        else if (!strcmp(argv[i], "--highlight")) opt.highlight = true;
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
    // _highlightSelectMode 是 begin() 里读的（IME.cpp:1356），必须赶在 initIme 之前设。
    hostime::setCandidateHighlight(opt.highlight);
    if (opt.paging) opt.widthPaging = true;   // 分页矩阵要按宽度分页，--fixed 不适用

    std::string tag = " mode=regress word=" + opt.word;
    printf("=== ime_driver: letters='%s' commitIdx=%d paging=%s%s ===\n", opt.letters.c_str(),
           opt.commitIdx, opt.widthPaging ? "width" : "fixed", opt.regress ? tag.c_str() : "");

    IME &ime = IME::getInstance();
    initIme(ime, opt.widthPaging);
    printf("begin(): loaded=%d scheme=%d\n", ime.loaded(), (int)ime.scheme());
    dumpState(ime, "after-init");

    if (opt.paging) {
        printf("\n=== 候选分页的宽度：横屏一页装得下的应当不少于竖屏 ===\n");
        int rc = pagingMatrix(ime, opt.letters);
        printf("=== done (rc=%d) ===\n", rc);
        return rc;
    }

    // The key sequence must be fed one key at a time: the stale _partialStart/_remainder
    // that this harness is about are planted by the *intermediate* keystroke whose
    // lookup ends with an empty candidate table (zheg / jiush), then survive the next
    // keystroke's early-returning lookup.
    std::vector<KeyTrace> trace;
    feedKeys(ime, opt.letters, trace);

    int rc = 0;
    if (opt.enter) {
        // 「输入法输入过程中按回车 = 编码原样上屏」。调用方（screen_editor /
        // screen_polish_prompt / 阅读模式的输入框）都是把 '\n' 直接喂给 handleKey，
        // 所以这里也走同一条路：喂一个真的 '\n'（虚拟键盘的回车键就是这个码，
        // 见 editor_vk.cpp 的 evkFinishKey('\n')；蓝牙键盘的 HID 0x28 在
        // bt_keyboard.cpp 的 s_asc_low 里也映射成 0x0A）。
        printf("\n=== Enter: 输入中直接编码上屏 ===\n");
        printf("  输入中: _code='%s' 候选 %zu 个 高亮模式=%d 高亮位=%d\n",
               ime._code.c_str(), ime._page.size(), opt.highlight ? 1 : 0, ime._sel);
        // 期望先取好：回车之后 _page/_sel 都被 reset 了。
        // 默认（高亮关）→ 逐字节等于输入串；高亮开 → 上屏**当前高亮位**那个候选。
        std::string want = opt.letters;
        if (opt.highlight && ime._sel >= 0 && ime._sel < (int)ime._page.size())
            want = ime._page[ime._sel];
        std::string out;
        bool handled = ime.handleKey('\n', out);
        printf("  handleKey('\\n') handled=%d out='%s'\n", handled ? 1 : 0, out.c_str());
        dumpState(ime, "after-enter");
        bool ok = handled && out == want && !leftoverComposition(ime);
        if (ok) {
            printf("  PASS: 回车%s → '%s'，没有残留编码\n",
                   opt.highlight ? "上屏高亮候选" : "原样上屏编码", out.c_str());
        } else {
            printf("  FAIL: 期望 out=='%s'（handled=1、_code/_prefix/_remainder 清空），"
                   "实际 out='%s' handled=%d _code='%s' _prefix='%s' _remainder='%s'\n",
                   want.c_str(), out.c_str(), handled ? 1 : 0, ime._code.c_str(),
                   ime._prefix.c_str(), ime._remainder.c_str());
            rc = 1;
        }
    } else if (!opt.regress) {
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
