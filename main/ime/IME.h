#pragma once

#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include "ime_config.h"
#include "yong_dict.h"

class IME {
public:
    enum Scheme { WUBI = 0, PINYIN = 1, SHUANGPIN = 2 };

    bool begin();
    bool loaded() const { return _loaded; }
    Scheme scheme() const { return _scheme; }

    bool active() const { return _active; }
    void setActive(bool on);
    void toggle() { setActive(!_active); }

    bool fullwidth() const { return _fullwidth; }
    void toggleFullwidth() { _fullwidth = !_fullwidth; }
    void setFullwidth(bool on) { _fullwidth = on; }

    bool trad() const { return _trad; }
    void toggleTrad() { _trad = !_trad; buildPage(); }
    void setTrad(bool on) { _trad = on; buildPage(); }

    bool english() const { return _english; }
    void toggleEnglish() { setEnglish(!_english); }
    void setEnglish(bool on) { _english = on; if (on) reset(); }

    bool handleKey(int key, std::string &out);

    // ── 一键多字母布局（9/14/18 键）歧义输入 ─────────────────────────────
    // 虚拟键盘的一键多字母布局按下的不是字母而是一组字母。这类键的键码走
    // KEY_AMBIG_BASE + 组号，在这里展开成若干"具体拼音码"，再逐个借用既有
    // lookup() 得到候选、按轮转顺序合并 —— lookup() 那套打分/词频/学习逻辑一行不改。
    // 只由虚拟键盘使用；实体键盘继续走 handleKey，两条路径互不影响。
    static const int KEY_AMBIG_BASE = 0x200;
    // 键位表按布局切换（三张表见 IME.cpp，取自万象拼音的三条 xlit）。
    enum AmbigLayout { AMBIG_26 = 0, AMBIG_14, AMBIG_18, AMBIG_9 };
    // 换布局。会清掉偏好表：同一串字节在不同布局下读出的音节完全不同，留着是错的。
    static void setAmbigLayout(int layout);
    static int ambigLayout();
    // 布局总数（26/14/18/9 共 4 个）。UI 的循环切换读它，不自己再写一个 4。
    static int ambigLayoutCount();
    static int ambigGroupCount();
    // 组号 → 该键包含的字母（'\0' 结尾的 1-4 个小写字母）。展开与"退化成首字母"共用。
    static const char *ambigGroupLetters(int group);
    // 组号 → 键面标签。14/18 键就是字母本身；9 键带上数字（"2 abc"），数字才是九宫格的
    // 记忆锚点，只写字母反而认不出按的是哪个键。
    static const char *ambigGroupLabel(int group);
    // 键位的**行分组**：三行各放哪几个组、每行几个键。两个虚拟键盘——写作/计划模式的
    // editor_vk 与阅读模式的 screen_reader——都读这一份，不会各写一张表再慢慢漂移。
    // 三种布局都是"组按顺序切成三段"，段长记在各自的行长表里，不另存二维表。
    // ambigRowGroup() 越界返回 -1。
    static int ambigRowCount();
    static int ambigRowKeys(int row);
    static int ambigRowGroup(int row, int k);
    // 某一行的列数（键宽基准）：14/18 键 = 本行组数（整行撑满）；9 键恒为 3（九宫格，
    // 末行只有 2 个键时居中留白，不把两个键拉成满宽大条）。绘制与命中测试共用。
    static int ambigRowCols(int row);
    bool handleAmbigKey(int group, std::string &out);

    // ── T9 候选面板（虚拟键盘点开"编码"行）───────────────────────────────
    // 面板左列 = **分音节选择**：剩下的键还能拼出哪些音节，按词库支持度排序。
    // 点一个就把这个音节接到已选前缀上（见 selectAmbigCode），候选随即按新的前缀
    // 重算。这样左侧永远是"下一个分词能用哪几个音节"这么几行，不会随按键数涨成长表；
    // 而且每个音节由用户显式确认，不会出现"正确读音名次靠后被截断"的问题
    // （tigao 曾掉在第 7 名被 kLookups 砍掉，就是这么丢的）。
    // 非歧义编码、或剩余键拼不出完整音节（如只剩 "gh"）时为空。
    const std::vector<std::string> &ambigSyllables() const { return _ambigSyllables; }
    // 已经挑过音节（挑之后展开集就被这个前缀收窄了）。
    bool ambigSegmented() const { return _ambigSegKeys > 0; }
    // 歧义键串还按着（有等待解释的键）。和 composing() 的区别：那个把所有"有东西要
    // 上屏"的状态都算进来（全拼、英文态、V 模式…），这里只问 _ambigKeys 在不在——
    // ambigSyllables() 只有在这一刻才是**当前**的，否则可能是上一轮留下的旧表
    // （reset() 不清它），照旧表画出来的读音列就是错的。
    bool ambigActive() const { return !_ambigKeys.empty(); }
    // 选中左列第 i 个"下一个音节"：接到已选前缀上并重算候选与分页。第一次选中时
    // 顺手把"这串键 → 这个音节"记进偏好表（下次同一串键它排最前）。返回 false =
    // 下标越界 / 当前没有可挑的音节。
    bool selectAmbigCode(int i);
    // 九宫格的「1 分词」键：把"下一个音节"里最可能的那一个确定下来。语义就是手机上
    // T9 键盘那个 1 键——显式断一个音，后面的键按新音节重新解释（等价于在 T9 候选
    // 面板左列点第一项，见 selectAmbigCode）。没有可挑的音节（还没按键 / 键已用完 /
    // 剩下的键拼不出任何音节）返回 false，调用方据此不做任何事。
    bool ambigCommitSyllable();

    std::string displayCode() const {
        if (_displayCodeDirty) {
            // 分音节选择进行中：已经挑定的音节之间**用 ' 隔开**（挑过 "ti" 就显示
            // "ti'gao"），后面再跟上剩下那串键的首选读法。' 就是拼音分音节的固定写法
            // （xi'an、tian'anmen），一眼看得出用户断在哪儿——连成一串 "tigao" 的话，
            // 挑过和没挑过长得一模一样，用户不知道自己那一下到底生效没有。
            if (_ambigSegKeys > 0) {
                std::string s;
                if (_ambigSegPick.empty()) {
                    s = _ambigSegText;   // 兜底：正常不会走到（挑选与退格都同步维护两边）
                } else {
                    size_t pos = 0;
                    for (size_t i = 0; i < _ambigSegPick.size() && pos < _ambigSegText.size(); i++) {
                        if (i) s.push_back('\'');
                        size_t len = _ambigSegPick[i];
                        if (pos + len > _ambigSegText.size()) len = _ambigSegText.size() - pos;
                        s.append(_ambigSegText, pos, len);
                        pos += len;
                    }
                    if (pos < _ambigSegText.size()) s.append(_ambigSegText, pos, std::string::npos);
                }
                // 还没挑的那几个键照显示（用展开表首位的读法）。不显示等于把它们藏起来：
                // 它们还占着候选项，藏了用户就无从解释候选为什么是这些。
                // _code 一定以 _ambigSegText 开头（展开就是从它接上去的），所以从
                // _ambigSegKeys 处切开正好是剩下那几个键的字母。
                if (_code.size() > (size_t)_ambigSegKeys) {
                    s.push_back('\'');
                    s.append(_code, (size_t)_ambigSegKeys, std::string::npos);
                }
                // 续接后（先上屏了「分」）再回来挑音节：已上屏那截照挂前面，别让它从
                // 编码行上消失——它还在正文里挂着没落屏。
                const std::string committed = _prefix + _ambigCommitted;
                if (!committed.empty() && !s.empty()) s = committed + "'" + s;
                else s = committed + s;
                _displayCodeCache = s;
            } else {
                // 自动分词：拼音态下把编码按音节切开（"fenxi" → "fen'xi"），一眼看得出
                // 断在哪儿。已经上屏的字（歧义布局续接，如选了「分」）连同 ' 一起挂在
                // 前面，显示成 "分'xi"——用户据此知道剩下的码还留在组合里没丢。
                // 两分/英文/v 模式/删除/联想态里的编码不是拼音读音，一律原样显示。
                const bool pinyin = !_lfMode && !_englishCompose && !_vMode && !_deleteMode &&
                                    !_predicting && !_english;
                const std::string committed = _prefix + _ambigCommitted;   // 正常只有一个非空
                const std::string seg = pinyin ? segmentedCode(_code) : _code;
                if (!committed.empty() && !seg.empty()) {
                    _displayCodeCache = committed + "'" + seg;
                } else {
                    _displayCodeCache = committed + seg;
                }
            }
            _displayCodeDirty = false;
        }
        return _displayCodeCache;
    }
    const std::string &composition() const { return _code; }
    const std::vector<std::string> &candidates() const { return _page; }
    // 页内高亮候选下标(v 模式始终可用; 普通候选由设置控制)
    int highlightIdx() const {
        return (_vMode || (_highlightSelectMode && !_englishCompose && !_predicting)) ? _sel : -1;
    }
    // 一键多字母布局的歧义编码也算"正在组合"：那时 _code 里放的是首选读音，但待确认的是
    // _ambigKeys 这串键。把它算进来，退格才会被正确地退给输入法（弹掉一个键）而不是
    // 落到正文上退字；候选条绘制也才会跟着出。
    bool composing() const {
        return _code.length() > 0 || !_ambigKeys.empty() || _predicting || _lfMode || _deleteMode || _vMode ||
               _englishCompose;
    }

    bool isLfMode() const { return _lfMode; }
    bool isDeleteMode() const { return _deleteMode; }
    // 上屏计数器: commit() 每成功一次 +1。推屏侧拿它认"这一帧是上屏那一拍" ——
    // 编码区/候选区只在那时清一遍(打字中途一次全刷都不做,见 ui_render.cpp)。
    uint32_t commitSeq() const { return _commitSeq; }
    std::string modeLabel() const;
    void clearLearningContext();
    // 文档级上下文: 编辑器喂入正文尾部(约 200 字), 已出现在正文里的词在小范围加分
    void setDocumentContext(const std::string &text);
    void toggleDeleteMode();
    void setDeleteMode(bool on);
    std::string takeStatusMessage() {
        std::string msg = _statusMessage;
        _statusMessage.clear();
        return msg;
    }

    void beginPredict(const std::string &text, bool afterSpaceCommit = false);
    void endPredict() { _predicting = false; _predChar = ""; }
    bool predicting() const { return _predicting; }
    void handleHostBackspace();
    void cancelComposition() { reset(); }
    void flushUserDictSavesNow() { flushUserDictSaves(true); }

    enum UserDictKind { FIXED_DICT = 0, DYNAMIC_DICT = 1, PREDICT_DICT = 2 };
    struct UserEntryView { std::string code; std::string word; int count; bool trad = false; };
    const std::vector<UserEntryView> userDictEntries(UserDictKind kind) const;
    bool addUserDictEntry(UserDictKind kind, const std::string &code, const std::string &word);
    bool addUserDictEntry(UserDictKind kind, const std::string &code, const std::string &word,
                          int count, bool trad);
    int addUserDictEntries(UserDictKind kind, const std::vector<UserEntryView> &items,
                           int *skipped = nullptr);
    void removeUserDictEntries(UserDictKind kind, const std::vector<int> &indices);
    void clearUserDict(UserDictKind kind);
    size_t userDictSize(UserDictKind kind) const;
    void ensureUserDictLoaded();

    void removeUserWord(const std::string &code, const std::string &word);
    void clearUserDict();
    void pruneUserDict(int minCount = 0);
    size_t userDictSize() const { return _dynamicUserWords.size(); }

    static IME &getInstance() {
        static IME instance;
        return instance;
    }
    IME(const IME &) = delete;
    IME &operator=(const IME &) = delete;

    // 各界面按自己的候选条宽度定每页数量(写作/阅读 7，设置 7，小字号 5…)，而且是在
    // 主循环里每帧无脑重设的。宫格展开期间这个值归 setGridPaging 管，外来的一律先
    // 存起来，等面板收起再还回去 —— 否则每帧被改回 7，宫格就永远只有 3/3/1 个。
    void setPageSize(int n) {
        if (_gridPaging) { _pageSizeBase = n; return; }
        _pageSize = n;
    }
    // 返回当前页实际候选数量(界面用 (i % pageSize)+1 编号, 页内从 1 起)
    int pageSize() const { int n = (int)_page.size(); return n >= 1 ? n : 1; }
    int totalCandidates() const { return (int)_all.size(); }
    int totalPages() const { return _pageStarts.empty() ? 1 : (int)_pageStarts.size(); }
    int currentPage() const { return _curPage + 1; }

    // ── 候选翻页（虚拟键盘在候选行上左右划）─────────────────────────────
    // 与方向键 ↑/↓ 翻页是同一份实现——页内编号、宽度分页规则都走 pagePrev/pageNext，
    // 不会出现"按键翻一页、手划翻另一页"两套页码。左右划是抬手一次性手势，不走
    // handleKey 那套按键流，所以在这里单开一个口子给虚拟键盘调。返回 false = 没得翻
    // （只有一页 / 已经在头尾），调用方据此决定要不要重绘。
    bool prevPage() { return pagePrev(); }
    bool nextPage() { return pageNext(); }

    using WidthFn = int (*)(const char *text);
    void setWidthFn(WidthFn fn) { _widthFn = fn; }
    // 候选行可用像素宽度(与各界面渲染 curW+partW+8>SCREEN_W 的 8px 余量一致)
    void setDisplayWidth(int w) { _displayWidth = w; }
    // 候选宽度是按字符串缓存的(_candidateWidths)。测宽回调本身可变时(虚拟键盘开的
    // 时候按"候选字大小"档位量、关掉时按界面字号量)，换量法就得把缓存清掉，否则
    // 沿用上一次的宽度，分页会按旧字号算——大了就截尾、小了就留空。
    void invalidateCandidateWidths() { _candidateWidths.clear(); }

    // T9 宫格用：宫格是 3×3 一整块，一页放 9 个；候选条是单行，按实测宽度分页在竖屏
    // 只放得下 5~7 个。展开面板期间切成"固定每页 9 个"，翻页按宫格整块换，编号 1..9
    // 也正好对上宫格顺序；关掉面板恢复宽度分页（候选条就那么宽）。
    void setGridPaging(bool on) {
        if (_gridPaging == on) return;
        _gridPaging = on;
        if (on) {
            _pageSizeBase = _pageSize;   // 备份各界面的每页数量，收起时原样还回去
            _pageSize = GRID_PAGE_SIZE;
        } else {
            _pageSize = _pageSizeBase;
        }
        _pageAnchor = _pageStart;   // 锚定当前页首个候选：换分页规则后仍停在同一批字上
        buildPage();
    }

    static const int GRID_PAGE_SIZE = 9;   // 3 列 × 3 行，见 editor_vk 的 EVK_T9_COLS

private:
    IME() {}

    static const int HEADER_SIZE = 12;
    static const int HANZI_SIZE = 3;
    static const int FLAG_SIZE = 1;
    int _codeLen = 6;
    int _recordSize = 6 + HANZI_SIZE + FLAG_SIZE;
    int _maxCode = 4;
    Scheme _scheme = WUBI;

    static const int INDEX_ENTRIES = 26 * 26 + 1; // 677
    static const int MAX_CODE_LEN = 6;
    static const int MAX_CANDIDATES = 300;

    bool _loaded = false;
    bool _active = false;

    const uint8_t *_blob = nullptr;
    size_t _blobSize = 0;
    ime::Im3Dictionary _dict;
    uint32_t _count = 0;
    size_t _recordBase = HEADER_SIZE + INDEX_ENTRIES * 4;

    bool _predicting = false;
    std::string _predChar;
    std::string _lastCommitChar;
    std::string _lastCommitText;
    uint32_t _commitSeq = 0;   // 见 commitSeq()
    std::string _statusMessage;
    int _partialStart = 0;
    int _maxMatchLen = 0;
    std::string _prefix;
    std::string _remainder;
    std::string _codeOrig;

    struct UserEntry { std::string code; std::string word; int count; bool trad = false; std::string initial; };
    struct PendingJournalEntry { std::string path; UserEntry entry; };
    std::vector<UserEntry> _fixedUserWords;
    std::vector<UserEntry> _dynamicUserWords;
    std::vector<UserEntry> _userPredictWords;
    std::vector<UserEntry> _userPredictRejectWords;
    std::vector<PendingJournalEntry> _pendingUserDictJournal;
    std::unordered_map<int, std::vector<uint16_t>> _fixedUserCodeIndex;
    std::unordered_map<int, std::vector<uint16_t>> _dynamicUserCodeIndex;
    std::unordered_map<int, std::vector<uint16_t>> _fixedUserInitialIndex;
    std::unordered_map<int, std::vector<uint16_t>> _dynamicUserInitialIndex;
    std::unordered_map<std::string, std::vector<uint16_t>> _fixedUserCodePrefixIndex;
    std::unordered_map<std::string, std::vector<uint16_t>> _dynamicUserCodePrefixIndex;
    std::unordered_map<std::string, std::vector<uint16_t>> _fixedUserInitialPrefixIndex;
    std::unordered_map<std::string, std::vector<uint16_t>> _dynamicUserInitialPrefixIndex;
    std::unordered_map<std::string, std::vector<uint16_t>> _userPredictIndex;
    bool _userWordIndexesDirty = true;
    // Last caller to invalidate the positional lookup indexes; reported by the
    // "perf rebuild" log so a stray invalidation is attributable. PERF_LOG only.
    const char *_indexDirtyReason = nullptr;
    bool _userPredictIndexDirty = true;
    bool _fixedUserDirty = false;
    bool _dynamicUserDirty = false;
    bool _userPredictDirty = false;
    bool _userPredictRejectDirty = false;
    bool _userDictLoaded = false;
    int64_t _deferredUserDictSinceUs = 0;
    int64_t _pendingUserDictJournalSinceUs = 0;
    void loadUserDict();
    bool loadUserDictFile(const char *path, std::vector<UserEntry> &entries, bool &dirty, size_t maxEntries);
    void saveUserDictFile(const char *path, std::vector<UserEntry> &entries, bool &dirty);
    void loadUserDictJournal(const char *path, std::vector<UserEntry> &entries,
                             bool &dirty, size_t maxEntries);
    void appendUserDictJournal(const char *path, const UserEntry &entry);
    void queueUserDictJournal(const char *path, const UserEntry &entry);
    void flushUserDictJournal(bool force);
    void clearUserDictJournal(const char *path);
    void markUserDictDirty(bool &dirty, const char *path = nullptr, const UserEntry *entry = nullptr);
    void flushUserDictSaves(bool force);
    void markUserWordIndexesDirty(const char *reason = nullptr);
    void rebuildUserWordIndexes();
    void rebuildUserPredictIndex();
    // Push one entry's keys into the four lookup maps (push-only, no sort/clear).
    static void indexUserWordEntry(uint16_t idx, const UserEntry &entry,
                                   std::unordered_map<int, std::vector<uint16_t>> &codeIndex,
                                   std::unordered_map<int, std::vector<uint16_t>> &initialIndex,
                                   std::unordered_map<std::string, std::vector<uint16_t>> &codePrefixIndex,
                                   std::unordered_map<std::string, std::vector<uint16_t>> &initialPrefixIndex);
    static void sortUserIndexMaps(const std::vector<UserEntry> &entries,
                                  std::unordered_map<int, std::vector<uint16_t>> &codeIndex,
                                  std::unordered_map<int, std::vector<uint16_t>> &initialIndex,
                                  std::unordered_map<std::string, std::vector<uint16_t>> &codePrefixIndex,
                                  std::unordered_map<std::string, std::vector<uint16_t>> &initialPrefixIndex);
    // Append-path variant of the above. A bucket is count-sorted after the last full
    // rebuild and appends only ever push onto its tail, so the only possible disorder
    // is that tail pair. Re-sorting just those buckets skips a std::stable_sort (which
    // allocates a temporary buffer even for 2-element buckets) on every other bucket.
    static void sortUserIndexMapsAfterAppend(const std::vector<UserEntry> &entries,
                                             std::unordered_map<int, std::vector<uint16_t>> &codeIndex,
                                             std::unordered_map<int, std::vector<uint16_t>> &initialIndex,
                                             std::unordered_map<std::string, std::vector<uint16_t>> &codePrefixIndex,
                                             std::unordered_map<std::string, std::vector<uint16_t>> &initialPrefixIndex);
    // Index one already-appended dynamic entry without a full rebuild. Falls back to
    // a full rebuild (leaving the dirty flag set) if the maps are stale.
    void appendUserWordIndexEntry(size_t entryIdx);
    void bumpFrequency(const std::string &code, const std::string &word, int weight = 1);
    bool penalizeUserWord(const std::string &code, const std::string &word, int weight = 2);
    // 删掉动态词库下标 i 的条目: 末尾条目顶上来(swap-and-pop)并把索引里的下标就地改写,
    // 不做整表重建。penalize 与 delete 模式删词都走这里。
    void removeDynamicUserWordAt(size_t i);
    void bumpPredictFrequency(const std::string &key, const std::string &word, bool saveNow = true, int weight = 1);
    bool penalizePredictWord(const std::string &word, int weight = 2);
    bool rejectedPredictWord(const std::string &key, const std::string &word) const;
    void rejectPredictWord(const std::string &key, const std::string &word);
    void learnPredictPairs(const std::string &text);
    void learnAutoPhraseFromSingle(const std::string &code, const std::string &word);
    void rememberCommittedText(const std::string &text);
    void rememberRecentCommit(const std::string &code, const std::string &word);
    int recentCommitBoost(const std::string &code, const std::string &word) const;
    void appendRecentCommitCandidates(const std::string &code,
                                      const std::vector<std::string> &aliasCodes,
                                      int typedLen);
    bool recentlyDeletedWord(const std::string &word) const;
    bool recentlyDeletedWordHash(uint32_t hash) const;
    void rememberDeletedWord(const std::string &word);
    bool confirmNewUserWordLearning(const std::string &code, const std::string &word, int weight);
    void rememberLastLearning(const std::string &code, const std::string &word);
    void rememberReplacementPreference(const std::string &code, const std::string &word);
    int contextCandidateBoost(const std::string &word);
    void rebuildContextBoostScores();
    void appendEnglishInlineCandidates(const std::string &code);
    void appendShortcutSymbol(const char *code, int len);
    int stableCandidateBoost(const std::string &word) const;
    void rememberCandidateStability();
    void rebuildRecentBoostIndex();
    void logCandidateDebug(const char *stage, const std::string &word, int score, int context, int stable) const;
    static bool compactUserEntries(std::vector<UserEntry> &entries, size_t limit);

    bool _deleteMode = false;
    bool _vMode = false;
    std::vector<std::pair<std::string, std::string>> _pendingUserWordLearns;
    std::vector<std::pair<std::string, std::string>> _recentSingleCommits;
    std::vector<std::pair<std::string, std::string>> _recentCommittedWords;
    // word -> indices into _recentCommittedWords, ascending. Rebuilt only when the
    // list changes (on commit), so recentCommitBoost() is an O(1) lookup per candidate.
    std::unordered_map<std::string, std::vector<uint8_t>> _recentBoostByWord;
    std::vector<std::string> _recentDeletedWords;
    std::vector<uint32_t> _recentDeletedHashes;
    std::string _lastLearningCode;
    std::string _lastLearningWord;
    std::string _lastLearningContext;
    int64_t _lastLearningUs = 0;
    std::string _lastRejectedCode;
    std::string _lastRejectedWord;
    std::string _lastRejectedContext;
    int64_t _lastRejectedUs = 0;
    // Previous screen candidates, kept as hashes so per-keystroke rebuild does not
    // churn the heap (a map/vector of strings would malloc+free every lookup).
    static const int STABLE_BOOST_SLOTS = 24;
    uint32_t _stableBoostHashes[STABLE_BOOST_SLOTS] = {};
    int16_t _stableBoostValues[STABLE_BOOST_SLOTS] = {};
    int _stableBoostCount = 0;
    int64_t _lastAsciiCommitUs = 0;
    int _sel = 0;  // 页内高亮候选(左右键移动)
    bool _englishCompose = false;
    bool _englishDictLoaded = false;
    std::vector<std::string> _englishWords;
    // Snapshot of the hidden ime_debug setting, taken once in begin() while the SD
    // card is already mounted. logCandidateDebug() runs per candidate inside the
    // hottest scan loops, where a settings mutex + map lookup is measurable (and its
    // first call otherwise paid three stat() plus a failed fopen on the SD card).
    // Enabling the setting therefore requires a reboot.
    bool _imeDebugLog = false;
    // Snapshot of the ime_sentence toggle, taken in begin() for the same reason as
    // _imeDebugLog: the phase runs inside the hot lookup path.
    bool _sentenceMode = true;
    bool _highlightSelectMode = false;  // begin() 快照的 ime_candidate_highlight
#if PJOURNAL_IME_ENABLE_LIANGFEN
    bool _lfMode = false;
    const uint8_t *_lfBlob = nullptr;
    uint32_t _lfCount = 0;
    size_t _lfRecordBase = 0;
    std::vector<uint16_t> _lfIndex;
    void loadLfDict();
    void searchLfWindow(const char *code, int len, uint32_t &lo, uint32_t &hi);
    bool readLfCode(uint16_t i, char out[13]);
    bool readLfHanzi(uint16_t i, char out[4]);
#else
    bool _lfMode = false;
    void loadLfDict() {}
#endif

    void searchWindow(const char *code, int len, uint32_t &lo, uint32_t &hi);
    void wordWindowCached(const char *code, int len, size_t &lo, size_t &hi);
    static int pinyinPrefixLen(const std::string &code);
    bool parseHeader(const uint8_t *hdrIndex, size_t total);
    bool readCode(uint32_t i, char out[MAX_CODE_LEN + 1]);
    bool readHanzi(uint32_t i, char out[HANZI_SIZE + 1]);
    uint8_t readRecordFlag(uint32_t i);
#if PJOURNAL_IME_ENABLE_LIANGFEN
    uint8_t readLfFlag(uint16_t i);
#endif

    std::string _code;
    // 一键多字母布局的歧义编码：按下的组号序列。非空时 _code 里放的是首选"具体拼音码"
    // （候选条直接显示它，用户能看出输入法猜的是哪个读音）。
    std::vector<uint8_t> _ambigKeys;
    // 键序列 → 上次从中选词的具体码。同一串键第二次输入时该码排到展开表首位，
    // 于是用户选过的词自动回到候选第 1 位（比任何静态词频先验都准，且零成本）。
    std::unordered_map<std::string, std::string> _ambigPreferred;
    // 键序列 → 上次在左列挑的第一个音节。**与上面那张表分开**：一张存的是完整码
    // （"tigao"），一张存的是音节（"ti"），写同一个 key 会互相覆盖，两条"记忆"
    // 就只剩后写的那条。左列的置顶读这张。
    std::unordered_map<std::string, std::string> _ambigPrefSyl;
    // 合并候选时记录的来源展开下标，与 _all 平行。选词时据此把"键序列 → 具体码"
    // 记进 _ambigPreferred。
    std::vector<uint8_t> _ambigSrc;
    // 当前键序列展开出的具体码（已排序、已截断），下标与 _ambigSrc 对应。数字键选词
    // 时靠它把 _code 换回这条候选所属的那个码（合并候选来自不同的展开）。**不是**
    // 面板左列——左列是 _ambigSyllables。
    std::vector<std::string> _ambigCodes;
    // 分音节选择：已确认的音节串（正好是键序列前 _ambigSegKeys 个键所指的字母），以及
    // 每个已选音节的字母数（= 消耗的键数），退格按音节回退。空 = 还没挑，走整串展开。
    std::string _ambigSegText;
    int _ambigSegKeys = 0;
    std::vector<uint8_t> _ambigSegPick;
    // 歧义布局下"已经上屏、但整串还没组合完"的字：选的是前半个码（fenxi 里选「分」），
    // 剩下的键留在 _ambigKeys/_code 里继续匹配。**不与 _prefix 合用**：lookupAmbiguous()
    // 会无条件清 _prefix（它属于逐读音 lookup() 的语义），而且这边要的是"键已退到 xi"，
    // 编码行据此显示成 "分'xi"（见 displayCode）。
    std::string _ambigCommitted;
    // 左列选项（"下一个音节"），见 ambigSyllables()。
    std::vector<std::string> _ambigSyllables;
    std::vector<std::string> _all;
    uint32_t _candidateHashes[MAX_CANDIDATES] = {};
    size_t _candidateHashCount = 0;
    std::vector<int> _candLen;  // code length per candidate in _all
    std::vector<int> _candidateWidths;  // cached text width parallel to _all (-1=unknown)
    std::vector<std::string> _predictCandidateKeys;  // prediction key parallel to _all in predict mode
    // Scratch for scoring dictionary single chars before they are appended. Held as
    // a member so the pass adds no per-lookup heap traffic (capacity survives clear).
    struct SingleScratchEntry { std::string word; int codeLen = 0; int score = 0; };
    std::vector<SingleScratchEntry> _singleScratch;
    std::vector<std::string> _page;
    int _pageStart = 0;
    int _pageSize = 9;
    int _pageSizeBase = 9;               // 宫格分页接管前各界面的每页数量(收起时还原)
    int _curPage = 0;                    // 当前页索引(第 _curPage+1 页)
    std::vector<int> _pageStarts;        // 每页起始候选索引; 按实测宽度分页时由 buildPage 重建
    int _pageAnchor = -1;                // 翻页时锚定目标候选, 宽度分页重建后仍停在包含它的页
    WidthFn _widthFn = nullptr;          // 候选文本宽度测量回调
    int _displayWidth = 0;               // 候选行可用像素宽度(0=退化为固定 _pageSize 分页)
    bool _fixedCandidatePaging = false;  // 短辅音输入走固定分页, 避免热路径反复测字宽
    bool _gridPaging = false;            // T9 宫格展开中: 强制固定每页 _pageSize 个(见 setGridPaging)
    size_t _candidateLimit = MAX_CANDIDATES;
    std::string _singleWindowCacheCode;
    int _singleWindowCacheLen = 0;
    uint32_t _singleWindowCacheLo = 0;
    uint32_t _singleWindowCacheHi = 0;
    std::string _wordWindowCacheCode;
    int _wordWindowCacheLen = 0;
    size_t _wordWindowCacheLo = 0;
    size_t _wordWindowCacheHi = 0;
    std::string _fuzzyConfigCache;
    int64_t _fuzzyConfigCacheUs = 0;
    std::string _contextBoostScoresContext;
    std::unordered_map<std::string, int> _contextBoostScores;

    // 文档级上下文(#13): 只保留正文里 2-3 字 CJK 片段的出现次数, 用固定槽位哈希表
    // (线性探测)存。候选打分循环里按候选调用, 所以查找必须是零分配的——冲突只意味着
    // 某个无关词多拿一点小加分, 与 _stableBoostHashes 的处理同理。
    // 槽位数按 200 字正文最坏 ~400 个片段(2 字 + 3 字各一遍)取 512, 装载因子 0.78,
    // 16 步探测足够(平均成功探测 ~2 步)。只收 2-3 字: 中文词绝大多数是这两个长度。
    static const int DOC_CTX_SLOTS = 512;   // 2 的幂, 掩码即取模
    static const int DOC_CTX_MAX_N = 3;
    uint32_t _docCtxHashes[DOC_CTX_SLOTS] = {};
    uint8_t _docCtxCounts[DOC_CTX_SLOTS] = {};
    bool _docCtxMode = true;  // begin() 快照的 ime_doc_context; 改设置要重启才生效
    void rebuildDocumentContext(const std::string &text);
    int documentContextBoost(const std::string &word) const;
    static uint32_t docCtxHash(const char *p, size_t n);

    // 整句覆盖词图的暂存区。全部做成长期成员, 容量在多次 lookup 间复用, 整句路径
    // 本身就不再产生查找期堆分配(除每条弧的候选文本字符串)。
    struct SentenceArc { uint8_t lo = 0; uint8_t hi = 0; int32_t score = 0; std::string word; };
    struct SentenceNode { int16_t parent = -1; int16_t arc = -1; int32_t score = 0; };
    struct SentenceCand { int16_t parent = -1; int16_t arc = -1; int32_t score = 0; };
    std::vector<SentenceArc> _sentenceArcs;    // 按 lo 升序(每个起点收集一遍); DP 另按 hi 分桶
    std::vector<SentenceNode> _sentenceNodes;  // 已定型的 beam 节点, 按位置切分
    std::vector<int> _sentenceNodeOff;
    std::vector<SentenceCand> _sentenceCands;  // 单个位置收候选时的临时表
    std::vector<int16_t> _sentenceByHi;        // 弧下标按终点分桶(计数排序)
    std::vector<int> _sentenceByHiOff;
    int _sentenceGroupsScanned = 0;

    mutable std::string _displayCodeCache;
    mutable bool _displayCodeDirty = true;

    bool _singleQuoteOpen = false;  // Track single quote pairing state
    bool _doubleQuoteOpen = false;  // Track double quote pairing state
    bool _fullwidth = false;        // Fullwidth character mode
    bool _trad = false;             // Traditional mode: hide simplified-only, show trad counterparts
    bool _english = false;          // Temp English mode: pass keys through as ASCII

    void reset();
    void lookup();
    void lookupSegmented();  // 单引号分词编码的查词路径
    // 编码行的自动分词显示（displayCode() 用）：把 "fenxi" 切成 "fen'xi"。见 IME.cpp。
    std::string segmentedCode(const std::string &code) const;
    void lookupAmbiguous();  // 一键多字母布局的查词路径（展开 + 轮转合并）
    void buildAmbigNextSyllables();  // 左列：剩余键还能拼出的音节（分音节选择）
    // 歧义展开的排序/剪枝依据：只查词库(单字表 + 词表)的"匹配程度"，不跑 lookup()。
    // 整码就是一个有字的完整音节 > 各音节切分都有字 > 成词条数 > 同码字数。
    int dictSupportScore(const std::string &code);
    int dictSyllableCharCount(const std::string &code);  // 该码作完整音节的同码字数(0=没有)
    int dictWordCountForCode(const std::string &code);   // 该码作完整词组码的词条数(0=没有)
    void lookupVMode();
    void lookupKaomoji(const std::string &query);  // v/编码 拼音/声母搜索文字表情
    void lookupEnglishMode();
    void loadEnglishDict();
    bool hasCandidate(const std::string &text, uint32_t hash) const;
    void clearCandidates();
    void rebuildCandidateHashes();
    bool appendCandidate(const std::string &text, int candLen);
    void appendScoredSingleChars(const char *prefix, int qlen, int scanBudget,
                                 bool codeLenFromRecord, int fixedCandLen, size_t cap);
    void appendSingleCharCandidates(const std::string &prefix, int candLen,
                                    size_t cap = 0);  // cap=0 用 _candidateLimit
    void collectSentenceArcs(int pos, const char *code, int len);
    void appendSentenceCandidates(const char *code, int len);
    void buildPage();
    bool pagePrev();
    bool pageNext();
    bool commit(int idx, std::string &out, bool bySpace = false);
    bool handleFullwidthPunct(int key, std::string &out);
    bool handleFullwidthChar(int key, std::string &out);
};
