// screen_reader_footnotes.cpp — 阅读模式的正文脚注引擎（P3b 从 screen_reader.cpp 拆出）。
//
// 这里装的是 EPUB/TXT 正文脚注这一整套：本章注号表（loadCurrentFootnotes）、跨文件
// 注号定位（rdFindFootnotePage / rdLookupNoteRef / rdGotoNoteRef / rdRememberNoteRef）、
// 页内注文切分与抽取（rdSplitPageLines / rdFindNoteStartLine / rdStripLeadingNoteNumber /
// rdNoteText）、弹注浮层（openFootnotePopup / drawFootnotePopup / closeFootnotePopup）与
// "正在取注…"的异步泵（rdFootnoteWaitTick / rdFootnoteWaitCancel）。
//
// 搬过来时**逻辑一行没改**：只动了 static 与 include；14 个入口提到
// screen_reader_internal.h，用到的阅读器原语（renderCurrent / rdShowFloat / buildToPage…）
// 由同一份内部头声明。FnPop 只在本族内用，跟着搬。

#include "screen_reader_internal.h"

static const char *TAG = "Reader";

#include "hw/input.h"
#include "ui/ime_field.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <esp_log.h>
#include <esp_timer.h>

#include <GfxRenderer.h>
#include <Page.h>
#include <Section.h>


bool loadCurrentFootnotes() {
  if (st.bookKind != 0 || !st.section) {
    st.footnoteNums.clear();
    st.footnoteHrefs.clear();
    st.footnoteCacheSpine = st.footnoteCachePage = st.footnoteCacheFont = -1;
    st.footnoteCacheBook.clear();
    return false;
  }
  // 同一页的脚注表只从磁盘读一次。点链接的那条路每按一次就要问一次"这是不是脚注"，
  // 原来每次都整页 loadPage（SD 读 + 反序列化，几毫秒），是"点注很慢"的一个大头。
  if (st.footnoteCacheSpine == st.spineIndex && st.footnoteCachePage == st.page &&
      st.footnoteCacheFont == st.fontLevel && st.footnoteCacheBook == st.bookPath) {
    return !st.footnoteHrefs.empty();
  }
  st.footnoteNums.clear();
  st.footnoteHrefs.clear();
  auto page = st.section->loadPage(st.page);
  if (!page) return false;  // 读失败不记账，下次再试
  st.footnoteCacheSpine = st.spineIndex;
  st.footnoteCachePage = st.page;
  st.footnoteCacheFont = st.fontLevel;
  st.footnoteCacheBook = st.bookPath;
  for (const auto &fn : page->footnotes) {
    st.footnoteNums.push_back(fn.number[0] ? fn.number : "[链接]");
    st.footnoteHrefs.push_back(fn.href);
  }
  return !st.footnoteHrefs.empty();
}

// 锚点 → 页码，查不到就把本节多排一会儿再查。
// 为什么需要：本节是**惰性排版**的（空闲帧只领先读者 kPrebuildAhead 页），而注释正文
// 常常压在**本节末尾**——晋书的校勘记就是正文后一整块 `<p id="note-001">`，最后一处
// 正文引用在源文件的 83% 处。读者停在第 4 页点注号时，本节只排到第 9 页，锚点那一页
// 压根没排出来 → 锚点表里没有 → 原来 getPageForAnchor 直接返回空，弹注/跳转全落空，
// 然后按键落进左右 1/3 翻页，这正是用户看到的"点注变成翻页"。
// 这里先查一次（活构建 + 磁盘锚点表），没查到且本节还在排就有界地继续排，边排边查。
// 预算按**时间**封顶而不是按页数：一次翻页的排版量级是几十毫秒，这个预算足够覆盖
// "注释就在后面几页"的常见情形；真遇到超长章节也只是退化回原来的行为，不会把按键
// 处理卡到没法用。
std::optional<Section::AnchorPos> rdFindFootnotePage(const std::string &anchor,
                                                            int64_t budgetUs) {
  if (st.bookKind != 0 || !st.section || anchor.empty()) return std::nullopt;
  auto pos = st.section->findAnchorPos(anchor);  // 活构建优先，其次磁盘锚点表
  if (pos || budgetUs <= 0) return pos;          // budgetUs=0：只查不排（按键那一拍）
  if (!st.section->isBuilding() || st.section->isBuildComplete()) return std::nullopt;
  const int64_t start = esp_timer_get_time();
  const int64_t deadline = start + budgetUs;
  int steps = 0;
  while (esp_timer_get_time() < deadline) {
    if (!st.section->buildSomeMore(4)) break;
    steps++;
    pos = st.section->findAnchorPos(anchor);
    if (pos) break;
    if (st.section->isBuildComplete()) break;
  }
  // 这一行专治"每次弹注都很慢"：慢的到底是这里（现排章节直到锚点出现），
  // 还是页面反序列化、还是强制全刷。steps=0 表示锚点表本来就有、这个循环没跑。
  ESP_LOGI(TAG, "弹注计时: 现排锚点 '%s' %d 轮 %lldms %s", anchor.c_str(), steps,
           (long long)((esp_timer_get_time() - start) / 1000), pos ? "命中" : "未命中");
  return pos;
}

// 注号归一化区的两个工具函数（定义在本文件后半段，这一段先用）。

// 弹注的三种结果。
//   Opened  —— 正文取到了，浮层已经就位。
//   NotHere —— 这条注释不在本章（calibre 那种 notes.xhtml#fn1 跨文件引用），调用方
//              退回"按锚点直接跳过去"的老路。
//   Pending —— 锚点还没排到。allowBuild=false 那一趟（按键处理里）**不会**去排，
//              只把这条挂在 st.fnWait* 上、起一个浮层就返回；空闲帧接着把它排出来
//              （见 rdFootnoteWaitTick）。这是"弹注很慢"的正面解法：原来这里同步排
//              2.5 秒，屏幕整个冻住；大书注文压在章末，动辄要排几十页。
enum class FnPop { Opened, NotHere, Pending };
static FnPop openFootnotePopup(int idx, bool allowBuild);

void jumpToFootnote(int idx) {
  if (idx < 0 || idx >= static_cast<int>(st.footnoteHrefs.size())) return;
  std::string href = st.footnoteHrefs[idx];
  // 跳走之前先把"上标在哪"记下来：此刻读者正待在那个上标所在的那一页上。
  // 有了这条，到了注文区点行首的注号就能跳回来（见 rdGotoNoteRef）。
  rdRememberNoteRef(st.footnoteNums[idx], st.spineIndex, st.page);
  size_t hash = href.rfind('#');
  if (hash == std::string::npos || !st.section) return;
  std::string anchor = href.substr(hash + 1);
  auto pg = rdFindFootnotePage(anchor);
  if (!pg) return;
  st.footnoteRetSpine = st.spineIndex;
  st.footnoteRetPage = st.page;
  st.footnoteRetValid = true;
  buildToPage(static_cast<int>(pg->page));
  st.mode = RdMode::Reading;
  st.fullRefresh = true;
  st.dirty = 1;
}

void footnoteReturn() {
  if (!st.footnoteRetValid) return;
  if (st.spineIndex != st.footnoteRetSpine) openSpine(st.footnoteRetSpine);
  buildToPage(st.footnoteRetPage);
  st.footnoteRetValid = false;
  st.mode = RdMode::Reading;
  st.fullRefresh = true;
  st.dirty = 1;
}

// ── 注文 → 正文上标 的反查（点注文条目行首的注号跳回上标处） ──────────────
//
// 为什么不能只靠 footnoteRet*：那套是"刚跳过来的那一条"的**一次性**返回点，跳完就清，
// 而且只认那一个注号 —— 在注文区多翻两页、去点别的注文条目的注号，就什么都不认了
// （用户反馈"翻页后跳转功能就失效了"）。这里换成按**注号**查表：任何一条注文条目的
// 行首注号，只要认得出注号，就能跳到它对应的那个上标。
//
// 表是**学**出来的，分两处学（都满足"此刻读者正站在上标那一页上"）：
//   * 点上标开弹注      → rdTapOnFootnote 的命中处
//   * 从注文跳注（列表/弹注的 Enter）→ jumpToFootnote 里，跳走之前
// 学不到的（比如从目录直接翻到注文区）就往前扫本节的页脚注表兜底，见 rdSearchNoteRefPage。
static constexpr int kMaxNoteRefs = 64;

void rdRememberNoteRef(const std::string &num, int spine, int page) {
  const std::string norm = rdNormalizeNoteNumber(num);
  if (norm.empty()) return;
  for (auto &r : st.noteRefs)
    if (r.num == norm) { r.spine = spine; r.page = page; return; }   // 只更新，不重复记
  if (static_cast<int>(st.noteRefs.size()) >= kMaxNoteRefs)
    st.noteRefs.erase(st.noteRefs.begin());   // 满了丢最旧的：走扫页兜底照样能找到
  st.noteRefs.push_back({norm, spine, page});
}

static bool rdLookupNoteRef(const std::string &norm, int *spine, int *page) {
  for (const auto &r : st.noteRefs)
    if (r.num == norm) { *spine = r.spine; *page = r.page; return true; }
  return false;
}

// 兜底：没学过这条注的上标位置，就从**当前页往前**扫本节的页脚注表，找第一个记着同一
// 注号的页 —— 上标永远排在注文区**前面**，所以从当前页往回走，撞见的第一个就是它
// （各章的注号会重复，取"离得最近的前一个"才落在本章）。
// 只扫当前页及之前：这些页读者是一路看过来的，早就排好了，命中通常只要翻几页。
// 时间和页数都封顶，扫不到就老实认输（调用方退回不跳），绝不把界面卡住。
static bool rdSearchNoteRefPage(const std::string &norm, int *outPage) {
  if (!st.section || norm.empty()) return false;
  constexpr int kMaxPages = 120;
  constexpr int64_t kBudgetUs = 300 * 1000;
  const int64_t deadline = esp_timer_get_time() + kBudgetUs;
  const int last = std::max(0, st.page - kMaxPages);
  for (int p = st.page; p >= last; p--) {
    if (esp_timer_get_time() > deadline) break;
    auto page = st.section->loadPage(p);
    if (!page) continue;
    for (const auto &fn : page->footnotes)
      if (rdNormalizeNoteNumber(fn.number) == norm) { *outPage = p; return true; }
  }
  return false;
}

// 按注号跳到正文里的那个上标处。返回 false = 这条注号反查不到（调用方照旧往下走）。
// 认出来就把位置记下（下次同一注号直接命中），然后跟 jumpToFootnote 一样翻页过去。
bool rdGotoNoteRef(const std::string &num) {
  const std::string norm = rdNormalizeNoteNumber(num);
  if (norm.empty()) return false;
  int spine = st.spineIndex, page = 0;
  if (!rdLookupNoteRef(norm, &spine, &page)) {
    if (!rdSearchNoteRefPage(norm, &page)) return false;
    rdRememberNoteRef(norm, spine, page);
  }
  if (st.spineIndex != spine) openSpine(spine);
  buildToPage(page);
  st.footnoteRetValid = false;   // 手已经落在上标上了，旧的一次性返回点就过期了
  st.mode = RdMode::Reading;
  st.fullRefresh = true;
  st.dirty = 1;
  return true;
}

// ── 脚注弹注（弹窗显示注释正文，不跳走） ─────────────────────────────────
// FootnoteEntry 里只有序号和 href，**没有注释正文**，所以正文得自己在目标锚点所在的
// 那一页上捞回来：href 的锚点 → getPageForAnchor → 该页的 PageLine 逐个取词拼成一段。
//
// 拼词时空格靠**版式量出来的间距**判断，不靠字符集猜：CJK 的"词"是单字紧挨着排的
// （wordXpos 首尾相接，一个缝都没有），英文才有真实的空格缝。规则是"缝够大、或换了行，
// 且缝两边都是 ASCII"才补一个空格——中文行尾换行因此不会凭空多出一个空格。
static bool rdAsciiByte(char c) { return (static_cast<unsigned char>(c) & 0x80) == 0; }

// 段首的"号码串"：从段首起，跳过括号/空白/句点，收下连续的号码字符（〇一二三…/0-9），
// 遇到第一个真正的字就停。`〔一〕以義熙元年…` → "一"；`1. 說明…` → "1"；`　　注文…` → ""。
// 只用来**选段**，不做精确解析，所以号码表取得保守（罗马数字、天干地支一律不认）。
void rdLeadingNoteMarker(const std::string &text, std::string *out) {
  static const char *kNum[] = {"〇", "零", "一", "二", "三", "四", "五", "六",
                               "七", "八", "九", "十", "百", "千"};
  // 收尾括号**终止**号码串，不能当普通标点跳过：`〔一〕三世必大昌` 跳过 `〕` 会接着
  // 把正文里的"三"吞进来算成"一三"，跟真正的 `〔一三〕` 撞上（离线仿真 4638 条注文里
  // 有 24 条就栽在这）。开括号前面没有号码，跳过它不影响。
  static const char *kClose[] = {"〕", "]", ")", "）", "】", "｝", "」", "》"};
  out->clear();
  for (size_t i = 0; i < text.size();) {
    const int len = rdUtf8Len(static_cast<unsigned char>(text[i]));
    if (i + static_cast<size_t>(len) > text.size()) break;
    const std::string ch = text.substr(i, len);
    i += len;
    for (const char *c : kClose)
      if (ch == c) return;
    const std::string n = rdNormalizeNoteNumber(ch);
    if (n.empty()) continue;  // 开括号/空白/句点：既不属于号码串，也不终止它
    bool numeral = n.size() == 1 && n[0] >= '0' && n[0] <= '9';
    for (const char *d : kNum)
      if (!numeral && n == d) numeral = true;
    if (!numeral) break;
    *out += n;
  }
}

// 段首是不是"括号开头的号码"（〔一〕/ [1] / （1）/ 【一】）。正文段落极少这么开头，
// 所以这一档几乎不会误伤——用来在多个同号段之间先挑最像注文的那一个。
bool rdStartsBracketed(const std::string &text) {
  static const char *kOpen[] = {"〔", "[", "(", "（", "【", "｛", "「", "《"};
  for (size_t i = 0; i < text.size();) {
    const int len = rdUtf8Len(static_cast<unsigned char>(text[i]));
    if (i + static_cast<size_t>(len) > text.size()) break;
    const std::string ch = text.substr(i, len);
    i += len;
    if (ch == " " || ch == "\t" || ch == "\r" || ch == "\n" || ch == "　") continue;
    for (const char *o : kOpen)
      if (ch == o) return true;
    return false;
  }
  return false;
}

// 已拼出的注号是不是**读到闭括号**了（〔一二〕/（1）/[1]）。回跳拼词时用它判断
// 括号注号收全没有 —— 闭括号本身也可能单独成词（见 rdTapOnNoteBack）。
bool rdHeadBracketClosed(const std::string &text) {
  static const char *kClose[] = {"〕", "]", ")", "）", "】", "｝", "」", "》"};
  for (size_t i = 0; i < text.size();) {
    const int len = rdUtf8Len(static_cast<unsigned char>(text[i]));
    if (i + static_cast<size_t>(len) > text.size()) break;
    const std::string ch = text.substr(i, len);
    i += len;
    for (const char *c : kClose)
      if (ch == c) return true;
  }
  return false;
}

// 页面拆成"行"。**一行 = 同一 yPos 的一串 PageLine**（ParsedText::extractLine 每行
// makeUniqueNoThrow 一个新 TextBlock，所以同一段的每行各是一个块、但 yPos 相同或递增）。
// lines[i] 是这行拼出来的文字，lineStart[i] 是这行第一个 PageLine 在 page.elements 里的
// 下标（给下面 elementIdx 定位用）。
//
// 拼词时空格靠**版式量出来的间距**判断，不靠字符集猜：CJK 的"词"是单字紧挨着排的
// （wordXpos 首尾相接，一个缝都没有），英文才有真实的空格缝。规则是"缝够大 且 缝两边
// 都是 ASCII"才补一个空格——中文行尾换行因此不会凭空多出一个空格。
//
// `lineParaStart`（可空）逐行回传 TextBlock::isParagraphStart()：**这一行是不是源段落的首行**。
// 排版时每个 <p>/<li>/<div>/<br> 都新起一个 ParsedText（ChapterHtmlSlimParser::
// startNewTextBlock），其第一行才带这个标记；段内折行、以及跨页后的续行都不带。弹注取文
// 就靠它划界（见 rdNoteText）。
static void rdSplitPageLines(const Page &page, int fontId, std::vector<std::string> *lines,
                             std::vector<int> *lineStart, std::vector<uint8_t> *lineParaStart = nullptr) {
  lines->clear();
  if (lineStart) lineStart->clear();
  if (lineParaStart) lineParaStart->clear();
  int prevLineY = 0, prevRight = 0;
  for (size_t ei = 0; ei < page.elements.size(); ei++) {
    const auto &el = page.elements[ei];
    if (el->getTag() != TAG_PageLine) continue;
    const auto &line = static_cast<const PageLine &>(*el);
    const auto &blk = line.getBlock();
    if (!blk || !blk->valid()) continue;
    if (lines->empty() || el->yPos != prevLineY) {  // 换行：起一条新的，间距状态归零
      lines->emplace_back();
      if (lineStart) lineStart->push_back(static_cast<int>(ei));
      if (lineParaStart) lineParaStart->push_back(blk->isParagraphStart() ? 1 : 0);
      prevRight = 0;
    }
    std::string &out = lines->back();
    for (uint16_t i = 0; i < blk->wordCount(); i++) {
      const char *word = blk->wordText(i);
      if (!word || !word[0]) continue;
      const int x = el->xPos + blk->wordXpos(i);
      const char prevCh = out.empty() ? '\0' : out[out.size() - 1];
      if (!out.empty() && rdAsciiByte(prevCh) && rdAsciiByte(word[0]) && x - prevRight > 2) out += ' ';
      out += word;
      prevRight = x + g_rd.getTextWidth(fontId, word, blk->wordStyle(i));
    }
    prevLineY = el->yPos;
  }
}

// 【已废弃】"这一行是不是下一条注文的开头"曾经是个**行首启发式**，两条路：
//   ① 行首够格的注号写法：号码串带括号收尾（〔二〕 / （2） / [2]）或紧跟顿号句点（1. / 一、 / 二：）
//      —— 晋书/趙州録式。光秃秃一个"一"开头的续行不算（离线统计《老子想尔注》1100 条注文，
//      有 38 条栽在"…尽得楚国货赂。”一是指贿赂…"这种折行上）；
//   ② 行首的"回引号" ※ ＊ * ↑ —— 祖堂集/多看的 `<p class="footnote"><a href="#noteref_N">※</a>順之：…</p>`。
// 这套启发式认不出"行首信号 ≠ 段落边界"：注文正文里回引别的注号（「…参见前注〔三〕…」）只要
// 折行到行首，就被当成下一条注文，正文在那儿被腰斩——这正是用户报的那个 bug。根子在于页面模型
// 里根本没有段落界限，只能拿行首长相猜。
//
// 现在 TextBlock::isParagraphStart() 把这个界限补上了（排版时每个 <p>/<li>/<div>/<br> 新起一个
// ParsedText，只有它的首行带标记；段内折行与跨页续行都不带），于是终止判定改由**源段落**说话，
// 行首启发式整体退役。上面的 ① ② 两条判据保留在此仅作背景——rdFindNoteStartLine 定位**起点**
// 时仍在用同一族的 rdLeadingNoteMarker/rdStartsBracketed。
// 在一页的行里找"本条注文从哪一行开始"。返回 -1 = 本页认不出来（调用方退一页再找 /
// 退回整页）。`anchorLate`（可空）回传"锚点行在、但它的段首号码跟要取的注号对不上"——
// 调用方靠这个决定要不要退到上一页去找。
//
// 两条线索，按可靠度排序：
//  1) **锚点记下的页内元素序号**（elementIdx）：解析器在"锚点所在块开始排版"那一刻
//     记下该页已有几个元素，正是这一段第一行在页 elements 里的下标。直接命中，唯一说话。
//  2) 段首号码：拿被点的注号去比段首的号码串。两级，先只用"括号开头的号码"（晋书
//     〔一〕、Duokan [1]/（1）），正文段落极少这么开头；一个都没认着再放宽到任何行首号码。
//
// **但 1) 有个例外：行内锚点。** id 落在 <a> 上时（趙州録校注全是
// `<p><span><a id="10" href="#7">〔一〕</a>太阿：…`），解析器只能等**下一个块边界**
// 才把这条待记锚点 flush 掉（flushPendingAnchor 是在 startNewTextBlock 里、makePages()
// **之后**调的），而那时本段的行早排进页里了 —— 记下的序号于是落到**后一段**。用户侧
// 看到的就是"点〔四〕弹出来的却是〔五〕的注文"，一整组注释错位一格。晋书/Duokan 的 id
// 都挂在 <p>/<li> 这些块元素上，跟 startNewTextBlock 是同一次调用，不受影响——所以只有
// 这本来报错。
//
// 因此这里不无条件相信 1)：锚点行的段首号码**明确和要取的不一样**时，改取它**前面**
// 最近的一条同号行（错位一格时那正是注文自己那一行）。锚点行没有号码可认（Duokan 的
// ※ 注文）时只在近处（12 行内）回溯 —— 免得把远处正文里形似号码的段首误当成注文；
// 号码对得上、或什么都认不出，仍旧信锚点行。
static int rdFindNoteStartLine(const std::vector<std::string> &lines,
                               const std::vector<int> &lineStart, int elementIdx,
                               const std::string &noteNum, int elementCount,
                               bool *anchorLate = nullptr) {
  if (anchorLate) *anchorLate = false;
  const std::string want = rdNormalizeNoteNumber(noteNum);

  int anchorLine = -1;
  if (!lineStart.empty() && elementIdx >= 0 && elementIdx < elementCount) {
    for (size_t i = 0; i < lines.size() && i < lineStart.size(); i++) {
      const int s = lineStart[i];
      const int e = (i + 1 < lineStart.size()) ? lineStart[i + 1] : elementCount;
      if (elementIdx >= s && elementIdx < e) { anchorLine = static_cast<int>(i); break; }
    }
  }
  bool late = false;  // 锚点行在，但它的段首号码不是我们要的那个 → 锚点记晚了一段
  if (anchorLine >= 0 && !want.empty()) {
    std::string mark;
    rdLeadingNoteMarker(lines[anchorLine], &mark);
    late = !mark.empty() && rdNormalizeNoteNumber(mark) != want;
  }
  if (anchorLate) *anchorLate = late;

  if (!want.empty()) {
    constexpr int kNearLines = 12;  // 锚点行没号码可认时允许回溯的行数
    for (int strict = 1; strict >= 0; strict--) {
      int before = -1, first = -1, hits = 0;
      for (size_t i = 0; i < lines.size(); i++) {
        if (strict && !rdStartsBracketed(lines[i])) continue;
        std::string mark;
        rdLeadingNoteMarker(lines[i], &mark);
        if (mark.empty() || rdNormalizeNoteNumber(mark) != want) continue;
        const int idx = static_cast<int>(i);
        ++hits;
        if (anchorLine < 0) {
          if (first < 0) first = idx;
        } else if (idx <= anchorLine && idx > before) {
          before = idx;  // 离锚点行最近的、不晚于它的同号行
        }
      }
      if (anchorLine < 0) {
        if (hits == 1) return first;  // 没有锚点可依：必须唯一命中，撞多条宁可退回整页
        if (hits > 1) break;          // 这一档就撞了，放宽一档只会更多
        continue;
      }
      if (before >= 0 && (late || anchorLine - before <= kNearLines)) return before;
    }
  }

  if (anchorLine >= 0 && !late) return anchorLine;  // 号码帮不上忙：仍旧信锚点行
  return -1;                                        // 锚点行号码对不上又找不到注文
}

// 注文首行行首的**注号**去掉：抬头已经写了「脚注 〔一五〕」，正文再带一遍就是重了
// （用户口径："定位到那一行，整行去掉序号剩下的就是注文正文"）。只在行首号码归一化后
// **与注号相等**时才动手 —— 认不出号码的行（锚点落在注文中段、或这本书的行首根本不带
// 号码）原样返回，不做任何猜测。分隔符（〔〕（）【】「」顿号句点空格）连在号码两侧一起吃掉。
static bool rdIsNumeralStr(const std::string &n) {
  if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') return true;
  static const char *kCn[] = {"〇", "零", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十", "百", "千"};
  for (const char *d : kCn)
    if (n == d) return true;
  return false;
}
static bool rdIsNoteDelim(const std::string &ch) {
  static const char *kDelim[] = {"〔", "〕", "（", "）", "【", "】", "｛", "｝", "「", "」", "　",
                                 "[",  "]",  "(",  ")",  "｟",  "｠",  "《",  "》",  "、",  "。",
                                 ".",  ",",  ":",  "：",  "；",  "·",  "—",  "-",  " "};
  for (const char *d : kDelim)
    if (ch == d) return true;
  return false;
}
// "证据"：号码左边有左括号，或右边跟了个**实打实的分隔符**（句点/顿号/冒号/逗号/右括号）。
// 只跟着一个空格的"5 注文"不算 —— 宁可留下号码，也不要把「5月4日…」这种正文开头的
// 数字当成注号吃掉。
static bool rdIsOpenBracket(const std::string &ch) {
  static const char *kOpen[] = {"〔", "（", "【", "｛", "「", "[", "(", "｟", "《"};
  for (const char *d : kOpen)
    if (ch == d) return true;
  return false;
}
static bool rdIsStrongDelim(const std::string &ch) {
  static const char *kStrong[] = {"〕", "）", "】", "｝", "」", "]", ")", "｠", "》", "、", "。",
                                  ".",  "．", "：", ":",  "，", ",",  "；", ";",  "·"};
  for (const char *d : kStrong)
    if (ch == d) return true;
  return false;
}
static std::string rdStripLeadingNoteNumber(const std::string &line, const std::string &noteNum) {
  const std::string want = rdNormalizeNoteNumber(noteNum);
  if (want.empty() || line.empty()) return line;
  size_t i = 0;
  bool hadOpen = false;
  // 1) 吃掉号码左边的分隔符（〔一五〕 / （1） / [2]）
  while (i < line.size()) {
    const int len = rdUtf8Len(static_cast<unsigned char>(line[i]));
    if (i + static_cast<size_t>(len) > line.size()) break;
    const std::string ch = line.substr(i, len);
    if (!rdIsNoteDelim(ch)) break;
    if (rdIsOpenBracket(ch)) hadOpen = true;
    i += static_cast<size_t>(len);
  }
  // 2) 攒号码
  std::string numeral;
  while (i < line.size()) {
    const int len = rdUtf8Len(static_cast<unsigned char>(line[i]));
    if (i + static_cast<size_t>(len) > line.size()) break;
    const std::string n = rdNormalizeNoteNumber(line.substr(i, len));
    if (n.empty() || !rdIsNumeralStr(n)) break;
    numeral += n;
    i += static_cast<size_t>(len);
  }
  if (numeral.empty() || numeral != want) return line;   // 号码对不上：整行原样
  // 3) 吃掉号码右边的分隔符（〕 / ） / ．/ 空格），并看有没有"实打实的分隔符"
  size_t after = i;
  bool hadStrong = false;
  while (after < line.size()) {
    const int len = rdUtf8Len(static_cast<unsigned char>(line[after]));
    if (after + static_cast<size_t>(len) > line.size()) break;
    const std::string ch = line.substr(after, len);
    if (!rdIsNoteDelim(ch)) break;
    if (rdIsStrongDelim(ch)) hadStrong = true;
    after += static_cast<size_t>(len);
  }
  // 号码既没被括号包着、后面也没跟分隔符：多半是正文自己以数字开头（"5月4日" / "三國志…"），
  // 整行原样。唯一的例外是"纯汉字、两个字以上"的号码（晋书注文就是「一七姚興載記上…」这种
  // 裸号码起头）——单个 ASCII 数字或单个汉字没有分隔符时一律不动。
  if (!hadOpen && !hadStrong) {
    if (numeral.size() < 6) return line;
    for (char c : numeral)
      if (c >= '0' && c <= '9') return line;
  }
  return line.substr(after);
}

// 取一条注释的**全部**正文。核心是**跨页**：注释条目常常正好排在翻页处，只读锚点那一页，
// 正文会在页边界处被硬生生截断——这正是"同一条注文有的显示完整、有的只剩两行"的区别所在
// （少的就是落在下一页的那几行）。所以这里从锚点那一行起一路读下去，直到**撞上源段落的边界**
// （TextBlock::isParagraphStart，见 rdSplitPageLines）或空行，就接着读下一页，直到终止或到达
// 页数上限（注释再长也不会跨 4 页，上限纯粹是防跑飞）。
//
// 终止判据曾经是"行首长相"（够格的注号写法 / 回引号 ※，见上面那段【已废弃】的说明）——那只是
// 段落界限的替代品，且会把注文正文里折行到行首的回引注号（「…参见前注〔三〕…」）误当成下一条
// 注文，正文在那儿被腰斩。现在段落界限是排版时记下的真数据，标准电子书里每条注文各自一个
// <p>/<br>，段首即注文边界；段内折行与跨页续行都不是段首，天然不会被误判。
//
// 参数用 Section& 而不是 Page&：跨页要继续 loadPage 下一页，所以非拿这一节不可。
//
// incomplete 出参：**交回的文本可能是半截**。惰性排版下"下一页还没排出来"与"真的到章末了"
// 都表现为 loadPage 返回空（build_->lut[page].fileOffset 还是 0），这里分不清 —— 只要本节
// 还在排、又需要下一页却拿不到，就置位让调用方挂起重试（见 openFootnotePopup）。这正是
// 用户侧"注文有时被截断、有时又完整"的由来：注文跨页时撞上没排完的窗口就截断。
// 排完之后（isBuildComplete）再拿不到就是真的没有了，那时 incomplete 保持 false，半截
// 就是全部。
static std::string rdNoteText(Section &sec, int pageIdx, int elementIdx, int fontId,
                              const std::string &noteNum, bool *incomplete = nullptr) {
  constexpr int kMaxExtraPages = 3;  // 锚点页之外最多再读 3 页
  constexpr int kMaxLines = 240;     // 总行数上限，防跑飞

  if (incomplete) *incomplete = false;
  const auto needPageFail = [&]() {
    if (incomplete && sec.isBuilding() && !sec.isBuildComplete()) *incomplete = true;
  };

  // 先定位"注文从哪一页、哪一行开始"。锚点序号指的是**锚点页**；行内锚点会被解析器记到
  // 后一段（见 rdFindNoteStartLine 的说明），极端情况下本段正排在页尾、整段被推到下一页，
  // 那锚点页上就一行都对不上、而注文留在**上一页**。所以只有"锚点行在、号码却对不上"
  // 这一种确凿的错位才退一页再找；那一趟不带锚点序号（它属于下一页），只按注号唯一命中。
  int startPage = pageIdx, startCur = -1;
  bool anchorPastPageEnd = false;  // 锚点序号指着"锚点页末尾之外"，见下面第三趟
  for (int probe = 0; probe < 2 && startCur < 0; probe++) {
    const int p = (probe == 0) ? pageIdx : pageIdx - 1;
    if (p < 0) break;
    auto page = sec.loadPage(p);
    if (!page) continue;
    std::vector<std::string> lines;
    std::vector<int> lineStart;
    rdSplitPageLines(*page, fontId, &lines, &lineStart);
    if (lines.empty()) continue;
    bool late = false;
    const int cur = rdFindNoteStartLine(lines, lineStart, probe == 0 ? elementIdx : -1, noteNum,
                                        static_cast<int>(page->elements.size()), &late);
    if (cur >= 0) {
      startPage = p;
      startCur = cur;
    } else {
      if (probe == 0) {
        // 锚点序号 >= 本页元素总数 ⇒ 解析器把这条锚点记在了**页尾之后**。它在
        // startNewTextBlock 里记的是"此刻当前页已有的元素个数"，而"这一行装不下、该翻页"
        // 是等这一行真要入页时才判的（addLineToPage）。锚点段落的首行正好压在页边界上
        // 时，记下的就是这个满页的元素总数，正文却落在**下一页**——锚点页上按注号一个也
        // 找不着（那一页根本没这条注文）。晋书校勘记里每章约 1~2 条注文是这个形状
        // （离线扫全书 4638 条注文命中 77 条；柏拉图 0 条），症状就是这一条取不出注文、
        // 退回整页倒出。
        anchorPastPageEnd = elementIdx >= static_cast<int>(page->elements.size());
      }
      if (!late) break;  // 锚点页上不是"错位"（只是认不出来）：退一页也找不着
    }
  }
  if (startCur < 0 && anchorPastPageEnd) {
    // 第三趟：去**下一页**。锚点块既然没能开始于锚点页，那它整个是新段落、从下一页的第一
    // 行开始排（页满换页后第一个入页的元素就是它）。先认这一行的段首号码，对得上就一定是
    // 它——比"整页按注号找"稳：正文里行首的引号/引用号偶尔会冒充注号（`「三月」…` 被
    // rdStartsBracketed 当成带括号的注号，一页上撞出两条同号行，那个函数就撒手了）。
    // 首行号码认不出（锚点块是空块、或真被推到更后面）再退回按注号**唯一**命中。
    auto page = sec.loadPage(pageIdx + 1);
    if (!page) needPageFail();  // 下一页还没排完（或真没有下一页）：先记账，别当成"找不到"
    if (page) {
      std::vector<std::string> lines;
      std::vector<int> lineStart;
      rdSplitPageLines(*page, fontId, &lines, &lineStart);
      if (!lines.empty()) {
        const std::string want = rdNormalizeNoteNumber(noteNum);
        std::string mark;
        rdLeadingNoteMarker(lines[0], &mark);
        if (!want.empty() && rdNormalizeNoteNumber(mark) == want) {
          startPage = pageIdx + 1;
          startCur = 0;
        } else {
          const int cur = rdFindNoteStartLine(lines, lineStart, -1, noteNum,
                                              static_cast<int>(page->elements.size()), nullptr);
          if (cur >= 0) {
            startPage = pageIdx + 1;
            startCur = cur;
          }
        }
      }
    }
  }
  if (startCur < 0) {
    // 取不出起点时先退一步：**锚点元素序号**本身还指着"这一段从哪一行起"。号码串认不出来
    // （注号写法怪、或锚点记偏了）不等于元素序号也废了 —— 用它当起点，下面的收集循环照样
    // 按段落界限收在**这一条**注文上。
    // 原来这里直接把整页倒出来，用户侧就是"点第 5 条，弹出的文本里第 3、4、5 条全在里面"。
    auto page = sec.loadPage(pageIdx);
    if (page) {
      std::vector<std::string> lines;
      std::vector<int> lineStart;
      rdSplitPageLines(*page, fontId, &lines, &lineStart);
      const int elementCount = static_cast<int>(page->elements.size());
      if (elementIdx >= 0 && elementIdx < elementCount) {
        for (size_t i = 0; i < lines.size() && i < lineStart.size(); i++) {
          const int s = lineStart[i];
          const int e = (i + 1 < lineStart.size()) ? lineStart[i + 1] : elementCount;
          if (elementIdx >= s && elementIdx < e) { startCur = static_cast<int>(i); break; }
        }
      }
    }
  }
  if (startCur < 0) {  // 连元素序号都对不上：退回锚点页整页（老行为），总比什么都不给强
    auto page = sec.loadPage(pageIdx);
    if (!page) return std::string();
    std::vector<std::string> lines;
    std::vector<int> lineStart;
    rdSplitPageLines(*page, fontId, &lines, &lineStart);
    std::string all;
    for (const std::string &p : lines) all += p;
    return all;
  }

  std::string out;
  int collected = 0;
  bool needStart = true;  // 还在第一页、还没定位到注文首行

  for (int extra = 0; extra <= kMaxExtraPages; extra++) {
    auto page = sec.loadPage(startPage + extra);
    if (!page) { needPageFail(); break; }
    std::vector<std::string> lines;
    std::vector<int> lineStart;
    std::vector<uint8_t> lineParaStart;
    rdSplitPageLines(*page, fontId, &lines, &lineStart, &lineParaStart);
    if (lines.empty()) break;

    int cur = 0;
    if (needStart) {
      cur = startCur;
      if (cur >= static_cast<int>(lines.size())) break;
      // 整页就这一行：没有"下一行"可判终止 —— 得看下一页的首行是不是新段落。下一页还没
      // 排出来时也判不了，先记账挂起重试。
      if (lines.size() == 1) {
        needPageFail();
        return rdStripLeadingNoteNumber(lines[cur], noteNum);
      }
      out = rdStripLeadingNoteNumber(lines[cur], noteNum);
      collected = 1;
      cur++;
      needStart = false;
    }

    // 收续行：下一行**起新段**（= 下一条注文）或空行就收工，否则是本条注文的续行。
    // 注意这里只看段落界限，不再看行首长相 —— 见函数头的说明。
    bool terminated = false;
    for (; cur < static_cast<int>(lines.size()); cur++) {
      if (lines[cur].empty() || (cur < static_cast<int>(lineParaStart.size()) && lineParaStart[cur])) {
        terminated = true;
        break;
      }
      if (!out.empty() && rdAsciiByte(out[out.size() - 1]) && rdAsciiByte(lines[cur][0])) out += ' ';
      out += lines[cur];
      if (++collected >= kMaxLines) { terminated = true; break; }
    }
    if (terminated) return out;
    // 本页读完还没见着结尾 → 注文跨页了，接着读下一页（下一轮 cur 从 0 起）
  }
  return out;
}

// 取出第 idx 条脚注的正文并开弹注。三种结果见 enum FnPop 的说明。
// allowBuild=false 是**按键那一拍**走的：锚点没排到就只挂起（Pending），绝不在这里
// 排页 —— 排页在空闲帧里做（rdFootnoteWaitTick），否则就是用户报的"弹注很慢"。
static FnPop openFootnotePopup(int idx, bool allowBuild) {
  if (idx < 0 || idx >= static_cast<int>(st.footnoteHrefs.size())) return FnPop::NotHere;
  if (st.bookKind != 0 || !st.section) return FnPop::NotHere;
  const int64_t t0 = esp_timer_get_time();
  const std::string &href = st.footnoteHrefs[idx];

  // QQ 阅读器的弹注**没有正文段落**，注释文字就藏在 <img alt="…"> 里。解析器把它
  // 以 "alt:" 哨兵前缀塞进 href（见 ChapterHtmlSlimParser 的 IMAGE_TAGS 分支），
  // 这里直接取用 —— 不用去锚点表里找。
  if (href.rfind("alt:", 0) == 0) {
    if (href.size() <= 4) return FnPop::NotHere;
    st.fnPopNum = st.footnoteNums[idx];
    st.fnPopText = href.substr(4);
    st.fnPopScroll = 0;
    st.fnPopIdx = idx;
    ESP_LOGI(TAG, "弹注计时: 第%d条 alt 哨兵 %u 字 共 %lldms", idx,
             (unsigned)st.fnPopText.size(), (long long)((esp_timer_get_time() - t0) / 1000));
    return FnPop::Opened;
  }

  const size_t hash = href.rfind('#');
  if (hash == std::string::npos) return FnPop::NotHere;
  const std::string anchor = href.substr(hash + 1);
  if (anchor.empty()) return FnPop::NotHere;

  std::string text;
  bool truncated = false;  // 注文跨页、而下一页还没排完 → text 是半截（见 rdNoteText）
  auto pg = rdFindFootnotePage(anchor, allowBuild ? 2500 * 1000 : 0);
  if (pg)
    text = rdNoteText(*st.section, pg->page, pg->element, BODY_FONT_ID_BASE + st.fontLevel,
                      st.footnoteNums[idx], &truncated);

  // **"锚点登记了" ≠ "正文取得到"**，这一条是晋书"点注号一直报取不到注文"的根子：
  // 锚点在 startNewTextBlock 里、**块刚要排版那一刻**就登记了（flushPendingAnchor），
  // 它记下的那一页这时往往还没排完 —— build_->lut[page].fileOffset 仍是 0，
  // loadPage 直接返回空，rdNoteText 交回空串。原来这里只在 allowBuild=false（按键那一拍）
  // 才保持挂起，而空闲帧重试走的正是 allowBuild=true：一取不到就判 NotHere → 报错，
  // 其实它只是慢了一页。改成只要本节还在排就继续挂起，下一空闲帧再试。
  // 排完了还取不出来才是真的取不到（那样下面的跨 spine 兜底与 NotHere 照旧生效）。
  //
  // 同一个道理还有第二种面孔：**锚点排到了、注文却跨页**，下一页同样可能还没排 ——
  // rdNoteText 那时只能交回当前页的半截注文（它置 truncated）。它同样是"慢了一页"，
  // 不是"就这样了"，所以一起挂起。用户侧的症状正是"注文有时被截断、有时又完整"：
  // 截不截断取决于点的那一下撞没撞上排版窗口。
  if ((text.empty() || truncated) && st.section->isBuilding() && !st.section->isBuildComplete()) {
    st.fnWaitIdx = idx;
    st.fnWaitSpine = st.spineIndex;
    st.fnWaitAnchor = anchor;
    ESP_LOGI(TAG, "弹注计时: 第%d条 锚点 '%s' %s → 继续挂起", idx, anchor.c_str(),
             text.empty() ? "在表里但正文还取不出" : "注文跨页但下一页还没排完");
    return FnPop::Pending;
  }

  // 本章查不到 → 注释可能在**别的 spine**（calibre 常见的 notes.xhtml#fn1 那种）。
  // 用现成的 resolveHrefToSpineIndex（它只对"带文件名"的跨文件 href 返回有效值，
  // 纯 #anchor 的同文件引用返回 -1）定位，单独开一节把锚点取回来。这条只在本节
  // 失败时才走，所以既慢不了日常阅读，也不会碰到用户的三种书。
  if (text.empty() && st.epub) {
    const int target = st.epub->resolveHrefToSpineIndex(href);
    if (target >= 0 && target != st.spineIndex) {
      auto sec = std::make_unique<Section>(st.epub, target, g_rd);
      auto pg2 = sec->getAnchorPosForAnchor(anchor);
      if (pg2)
        text = rdNoteText(*sec, pg2->page, pg2->element, BODY_FONT_ID_BASE + st.fontLevel,
                          st.footnoteNums[idx]);
    }
  }
  if (text.empty()) {
    ESP_LOGW(TAG, "弹注计时: 第%d条 取不到正文 共 %lldms", idx,
             (long long)((esp_timer_get_time() - t0) / 1000));
    return FnPop::NotHere;
  }

  st.fnPopNum = st.footnoteNums[idx];
  st.fnPopText = std::move(text);
  st.fnPopScroll = 0;
  st.fnPopIdx = idx;
  // 走到这儿还 truncated，说明本章**排完了**也还是拿不到下一页 —— 那要么注文真到章末了，
  // 要么跨页数超过了 kMaxExtraPages。两种都不该再等，照常弹，但留一条日志好区分。
  if (truncated)
    ESP_LOGW(TAG, "弹注计时: 第%d条 取文到章末为止（%u 字，可能不全）", idx,
             (unsigned)st.fnPopText.size());
  ESP_LOGI(TAG, "弹注计时: 第%d条 锚点取文 %u 字 共 %lldms", idx, (unsigned)st.fnPopText.size(),
           (long long)((esp_timer_get_time() - t0) / 1000));
  return FnPop::Opened;
}

// 「开弹注」这一步的公共壳：三种结果里能就地处理的两种（Opened / Pending）都在这里
// 摆好界面状态 —— 回正文模式、按刷新策略标脏；挂起态再挂上那个"正在取注…"浮层。
// 返回 false = NotHere，调用方自己走退路（跳转 / 报未命中）。
bool rdOpenFootnote(int idx, bool allowBuild) {
  const FnPop r = openFootnotePopup(idx, allowBuild);
  if (r == FnPop::NotHere) return false;
  st.mode = RdMode::Reading;  // 弹注是盖在正文页上的浮层
  if (r == FnPop::Pending) {
    // 空闲帧排到锚点后自己清掉这个浮层并把注文弹出来（见 rdFootnoteWaitTick）。
    st.busyMsg = "正在取注…";
    st.busySub.clear();
  }
  rdOverlayRefresh();
  return true;
}

// 撤掉挂起的弹注：清状态 + 抹掉那个"正在取注…"浮层（它就是 st.busyMsg，正常路径上
// 只在 rdShowBusy 里短暂出现，这里之外恒为空，所以清它不会碰到别的流程）。
void rdFootnoteWaitCancel() {
  st.fnWaitIdx = -1;
  st.fnWaitSpine = -1;
  st.fnWaitAnchor.clear();
  if (!st.busyMsg.empty()) {
    st.busyMsg.clear();
    st.busySub.clear();
    st.dirty = 1;  // 浮层得擦掉
  }
}

// 空闲帧推进一步挂起的弹注。
//   ① 状态对不上（换了书/换了章/脚注表换了）→ 作废。
//   ② 只查不排地问一次锚点：到了就弹出来。**空闲帧没人推屏**，所以这里必须自己
//      重绘一次 —— 和上面 floatMsg 到点自清那一处是同一个道理。
//      锚点在表里但正文还取不出来（锚点页还没排完）时 openFootnotePopup 回 Pending，
//      此时**不作废**，接着往下排，下一空闲帧再试。
//   ③ 还没到就继续排一段（限时），下一空闲帧接着来。
// 判不出来（整章排完仍没有这个锚点）就作废并给个提示，不会无限等下去。
void rdFootnoteWaitTick() {
  if (st.fnWaitIdx < 0) return;
  if (st.bookKind != 0 || !st.section || st.spineIndex != st.fnWaitSpine ||
      st.fnWaitIdx >= static_cast<int>(st.footnoteNums.size())) {
    rdFootnoteWaitCancel();
    return;
  }
  if (rdFindFootnotePage(st.fnWaitAnchor, 0)) {  // 只查不排
    const std::string anchor = st.fnWaitAnchor;
    const FnPop r = openFootnotePopup(st.fnWaitIdx, true);
    if (r != FnPop::Pending) {
      rdFootnoteWaitCancel();
      if (r == FnPop::Opened) {
        ESP_LOGI(TAG, "挂起弹注: 锚点 '%s' 排到了 → 弹出", anchor.c_str());
        rdOverlayRefresh();
      } else {
        rdShowFloat("取不到注文", std::string(), 1500);
        st.dirty = 1;
      }
      renderCurrent();
      return;
    }
    // Pending：锚点在表里，但正文这一刻还取不出来（锚点页还没排完，见 openFootnotePopup）。
    // **不能在这里 cancel** —— 那会把挂起状态连同"正在取注…"浮层一起清掉。落到下面接着排，
    // 下一空闲帧再试。openFootnotePopup 只在"本节还在排"时才返回 Pending，排完那一帧它会
    // 改口成 Opened 或 NotHere，所以这里不会无限等下去。
  }
  if (st.section->isBuildComplete() || !st.section->isBuilding()) {
    rdFootnoteWaitCancel();
    rdShowFloat("没有找到这条注文", std::string(), 1500);
    st.dirty = 1;
    renderCurrent();
    return;
  }
  // 还没排到：接着排。一次一层 build 有界（一页），限时 60ms —— 空闲帧的轮询周期是
  // 80ms（main.cpp 的 idleWaitWithTouch(80)），留一点余量给下一次触摸轮询。
  const int64_t deadline = esp_timer_get_time() + 60 * 1000;
  do {
    st.section->buildSomeMore(1);
  } while (!st.section->isBuildComplete() && esp_timer_get_time() < deadline);
}

// 弹注浮层。画在正文页之上、busy/瞬时浮层之下（后两个都在正中间，优先级更高），
// 且在 applyNightMode() 之前 —— 夜间模式连它一起反色，不会留一块刺眼的白。
// 白底 + 双线边框 + 现行宽现断行，非交互部分全在这里，滚动窗口由 fnPopScroll 定。
void drawFootnotePopup() {
  if (st.fnPopNum.empty()) return;
  const int w = g_rd.getScreenWidth(), h = g_rd.getScreenHeight();
  // 注释正文是**书里的文字**，必须跟正文用同一套字面：内嵌字体的书，字面是按本书
  // 正文子集化出来的，拿外壳的内置字体去画就是整段豆腐块（"弹注用的不是内嵌字体…
  // 有太多缺字"）。抬头/底部提示相反 —— 那几个是 UI 词（脚注、翻页、关闭），书的
  // 子集里多半没有，仍旧走外壳字体。
  const int bodyId = BODY_FONT_ID_BASE + st.fontLevel;
  const int lh = std::max(1, g_rd.getLineHeight(bodyId)), gap = 4, pad = 14;
  const int boxX = MARGIN, boxW = w - 2 * MARGIN, bodyW = boxW - 2 * pad;
  const int headH = std::max(uiLineHeight(), lh) + 12, hintH = uiLineHeight() + 12;
  // 弹窗最高占屏幕 3/4，其余留给"底下还是那页正文"的观感。
  const int maxBody = std::max(1, (h * 3 / 4 - headH - hintH - 2 * pad) / (lh + gap));
  auto lines = g_rd.wrappedText(bodyId, st.fnPopText.c_str(), bodyW, 4096);
  const int total = static_cast<int>(lines.size());
  st.fnPopScroll = clampI(st.fnPopScroll, 0, std::max(0, total - maxBody));  // 越界在这里夹回
  const int scroll = st.fnPopScroll;
  const int shown = std::min(maxBody, total - scroll);
  const int boxH = pad + headH + shown * (lh + gap) + hintH + pad;
  const int boxY = std::max(RD_TOP_INSET, (h - boxH) / 2);
  st.fnPopBoxX = boxX;
  st.fnPopBoxY = boxY;
  st.fnPopBoxW = boxW;
  st.fnPopBoxH = boxH;
  g_rd.fillRect(boxX, boxY, boxW, boxH, false);  // 先铺白：把底下的正文盖掉
  g_rd.drawRect(boxX, boxY, boxW, boxH, true);
  g_rd.drawRect(boxX + 3, boxY + 3, boxW - 6, boxH - 6, true);  // 双线，像一张浮起来的纸片
  int y = boxY + pad;
  // "脚注"走外壳字体、编号走书字体（编号是从书里抓的，比如"〔一〕"），两者**共用一条
  // 基线**才不会一个高一个低 —— drawText 的 y 是基线，不能各按自己的 ascender 顶对齐。
  // 标题整条居中：「脚注 」+ 编号 两段字体不同，各量各的宽再加起来算左端。
  const char *headLabel = "脚注 ";
  const int baseY = y + g_rd.getFontAscenderSize(uiFontId());
  const int labW = g_rd.getTextWidth(uiFontId(), headLabel);
  const int numW = g_rd.getTextWidth(bodyId, st.fnPopNum.c_str());
  int hx = boxX + (boxW - (labW + numW)) / 2;
  if (hx < boxX + pad) hx = boxX + pad;
  g_rd.drawText(uiFontId(), hx, baseY, headLabel, true);
  hx += labW;
  g_rd.drawText(bodyId, hx, baseY, st.fnPopNum.c_str(), true);
  y += headH;
  for (int i = 0; i < shown; i++) {
    drawLineText(boxX + pad, y, lines[scroll + i].c_str(), true, bodyId);
    y += lh + gap;
  }
  char hint[80];
  if (total > maxBody)
    snprintf(hint, sizeof(hint), "↑↓ 翻页 %d-%d/%d   Enter 跳注  Esc 关闭", scroll + 1, scroll + shown, total);
  else
    snprintf(hint, sizeof(hint), "Enter 跳注  Esc 关闭");
  drawLineText(boxX + pad, boxY + boxH - pad - lh, hint, true);
}

// 关掉弹注。改回正文页需要整屏重绘（弹窗底下那页要恢复原样）——但那是**差分**能做的
// 事，不必全刷，见 rdOverlayRefresh。
void closeFootnotePopup() {
  st.fnPopNum.clear();
  st.fnPopText.clear();
  st.fnPopScroll = 0;
  st.fnPopIdx = -1;
  rdOverlayRefresh();
}
