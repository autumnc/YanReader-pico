// screen_reader_stats.cpp — 「统计」标签与它的子界面（从 screen_reader.cpp 拆出来的第二块）。
//
// P3b 的第二个切片。这一块是个完整的内聚子应用：一个根标签（阅读统计）加它下面
// 8 个子界面（单本书 / 更多详情 / 热力图 / 某日 / 档案 / 调整 / 设置），状态全在
// st.stats* 那几个字段 + reading_stats 数据层里，不与翻页 / 排版 / 词典纠缠。
// 搬过来时**逻辑一行没改**：只动了 static、include，并把对外的入口
// （renderStats* / handleStats* / rdStatsDate）提到 screen_reader_internal.h。
//
// 版式基准与子界面的三联形状见下面那段原注释。

#include "screen_reader_internal.h"  // st / g_rd / 外壳原语（drawTitle、列表几何…）
#include "screen_reader.h"           // screen_reader_long_confirm_is_action
#include "reading_stats.h"           // 数据层（ReadingStats / ReadingBookStats）
#include "settings_manager.h"        // g_settings（每日目标）

#include <algorithm>
#include <cmath>   // 阅读档案的雷达图（cos/sin）
#include <cstdio>
#include <string>
#include <vector>

#include <GfxRenderer.h>
#include <HalStorage.h>

#include "ui/list_view.h"
#include "hw/input.h"

// ═══════════════════════════════════════════════════════════════════════════
// 阅读统计（第 5 个根标签）
//
// 数据层在 main/reading_stats.{h,cpp}（记录口径/常量与 crossmux 一致，见那边的注释），
// 这一段只画界面。版式基准：本移植里 GfxRenderer 的 fillRoundedRect / drawRoundedRect /
// fillRectDither / drawTextRotated90CW 全是 stub（圆角被忽略、灰阶退化成黑白、旋转不转），
// 所以：卡片一律直角描边；柱状图用描边矩形＋基线；需要"深浅"的地方（热力图）用
// drawGrayscale16Pixel 直接写 4bpp 灰阶——封面的面积平均缩放走的就是同一条路。
//
// 子界面全部照 renderAbout/handleAbout 的三联形状：st.retMode 单层回退 +
// st.fullRefresh/st.dirty 触发重绘，不发明新机制。
// ═══════════════════════════════════════════════════════════════════════════

// 概览卡张数。加"今日页数/总页数"后是 8 张：竖屏（W=684）2 列 4 行 = 416px，屏高
// 1216 绰绰有余；横屏（W=1216）3 列 3 行，书单还剩 4 行，也够。所有跟着卡片墙走的
// 几何（书单顶边 / 更多详情的滚动上限）都必须从 kStatsCards 算，别写死行数。
static const int kStatsCards = 8;
static const int kStatsCardH = 96;
static const int kStatsCardGap = 8;

// 按扩展名认书的类型（openBook 要 kind；统计里只存了 path）。
static int rdStatsKindFor(const std::string &path) {
  if (endsWith(path, ".epub")) return 0;
  if (endsWith(path, ".txt")) return 1;
  if (endsWith(path, ".xtc")) return 2;
  return -1;
}

// 时间戳 → "2026-10-03"（时钟不可信时给一句人话）。
std::string rdStatsDate(uint32_t epoch) {
  if (!RdTime::clockValid(epoch)) return "未记录";
  char buf[16];
  RdTime::formatOrdinal(RdTime::dayOrdinal(epoch), buf, sizeof(buf));
  return buf;
}

// 日序号 → "2026-10-03"，0 给空串（热力图里那些跨月的格子用不着日期）。
static std::string rdStatsOrdinalLabel(uint32_t ordinal) {
  if (ordinal == 0) return "";
  char buf[16];
  RdTime::formatOrdinal(ordinal, buf, sizeof(buf));
  return buf;
}

// 某一天的合计时长（聚合日表里查）。
static uint64_t rdStatsDayMs(uint32_t ordinal) {
  if (ordinal == 0) return 0;
  for (const auto &d : ReadingStats::readingDays()) {
    if (d.dayOrdinal == ordinal) return d.readingMs;
  }
  return 0;
}

// 某一天读过（≥3 分钟）的书，按时长降序。
struct RdDayBook {
  const ReadingBookStats *book;
  uint64_t ms;
};
static std::vector<RdDayBook> rdStatsBooksOnDay(uint32_t ordinal) {
  std::vector<RdDayBook> out;
  if (ordinal == 0) return out;
  const uint64_t minMs = 3ULL * 60ULL * 1000ULL;
  for (const auto &b : ReadingStats::books()) {
    for (const auto &d : b.readingDays) {
      if (d.dayOrdinal == ordinal && d.readingMs >= minMs) {
        out.push_back({&b, d.readingMs});
        break;
      }
    }
  }
  std::sort(out.begin(), out.end(), [](const RdDayBook &a, const RdDayBook &b) {
    if (a.ms != b.ms) return a.ms > b.ms;
    return a.book->title < b.book->title;
  });
  return out;
}

// 参考日序号：时钟可信就用今天；否则退回已有记录的最后一天；再没有就是 0。
static uint32_t rdStatsRefOrdinal() {
  const uint32_t today = RdTime::todayOrdinal();
  if (today != 0) return today;
  const auto &days = ReadingStats::readingDays();
  return days.empty() ? 0 : days.back().dayOrdinal;
}

// 一张概览卡：数值在上、标签在下，居中。非交互，选中态不落在卡片上。
static void rdStatsCard(int x, int y, int w, int h, const std::string &value, const std::string &label) {
  g_rd.drawRect(x, y, w, h, true);
  const int lh = uiLineHeight();
  const std::string v = fitWidth(value, w - 12);
  drawLineText(x + (w - g_rd.getTextWidth(uiFontId(), v.c_str())) / 2, y + 8, v.c_str(), true);
  const std::string l = fitWidth(label, w - 12);
  drawLineText(x + (w - g_rd.getTextWidth(uiFontId(), l.c_str())) / 2, y + h - lh - 6, l.c_str(), true);
}

// 概览卡的取值（主页和"更多详情"共用，以前是两处各写一遍）。
// 顺序 = 卡片墙顺序：连读 / 最长连读 / 今日目标 / 今日页数 / 总时长 / 总页数 / 读完 / 开始。
static void rdStatsOverviewCards(std::vector<std::string> &values, std::vector<std::string> &labels) {
  const bool goalMet = ReadingStats::todayReadingMs() >= ReadingStats::goalMs();
  values = {std::to_string(ReadingStats::currentStreakDays()) + "d",
            std::to_string(ReadingStats::maxStreakDays()) + "d",
            ReadingStats::formatDurationHm(ReadingStats::todayReadingMs()) + " / " +
                ReadingStats::formatDurationHm(ReadingStats::goalMs()),
            std::to_string(ReadingStats::todayPages()),
            ReadingStats::formatDurationHm(ReadingStats::totalReadingMs()),
            std::to_string(ReadingStats::totalPagesRead()),
            std::to_string(ReadingStats::booksFinished()), std::to_string(ReadingStats::booksStarted())};
  labels = {"连续阅读", "最长连续", goalMet ? "今日 / 目标 ✓" : "今日 / 目标", "今日页数", "阅读总时长", "总页数",
            "读完书籍", "开始书籍"};
}

// 一排概览卡（cols 由屏宽定），返回卡片区的下一个空位 y。
static int rdStatsCardCols() { return g_rd.getScreenWidth() >= 900 ? 3 : 2; }

// n 张卡占几行。卡片墙挪动位置的地方（书单顶边、更多详情滚动上限）一律问它，
// 免得再出现"render 加了张卡、命中还按老行数算"这种错位。
static int rdStatsCardRowsFor(int n) {
  const int cols = rdStatsCardCols();
  return (n + cols - 1) / cols;
}

static int rdStatsCardGrid(int top, const std::vector<std::string> &values,
                           const std::vector<std::string> &labels) {
  const int w = g_rd.getScreenWidth();
  const int cols = rdStatsCardCols();
  const int n = static_cast<int>(values.size());
  const int rows = (n + cols - 1) / cols;
  const int cardW = (w - 2 * MARGIN - (cols - 1) * kStatsCardGap) / cols;
  for (int i = 0; i < n; i++) {
    const int cx = MARGIN + (i % cols) * (cardW + kStatsCardGap);
    const int cy = top + (i / cols) * (kStatsCardH + kStatsCardGap);
    rdStatsCard(cx, cy, cardW, kStatsCardH, values[i], labels[i]);
  }
  return top + rows * (kStatsCardH + kStatsCardGap);
}

// 统计主页里卡片区占几行（render 与命中要算同一个数）。
static int rdStatsCardRows() { return rdStatsCardRowsFor(kStatsCards); }
// 主页交互列表的顶边。
static int rdStatsListTop() {
  return coverTop() + rdStatsCardRows() * (kStatsCardH + kStatsCardGap) + 6;
}

// ── 主页：概览卡 + 入口列表 + 已开始的书籍 ──────────────────────────────
struct RdStatRow {
  std::string label;
  std::string right;
  int act;             // 0 更多详情 1 热力图 2 档案 3 调整时长 4 统计设置 5 书籍 6 小标题
  std::string path;    // act==5 的书路径
};

static std::string rdStatsBookRowRight(const ReadingBookStats &b) {
  return ReadingStats::formatDurationHm(b.totalReadingMs) + " · " + std::to_string(b.lastProgressPercent) + "%";
}

static void rdStatsRows(std::vector<RdStatRow> &rows) {
  rows.clear();
  rows.push_back({"更多详情", "", 0, ""});
  rows.push_back({"阅读热力图", "", 1, ""});
  rows.push_back({"阅读档案", "", 2, ""});
  rows.push_back({"调整阅读时长", "", 3, ""});
  rows.push_back({"统计设置", "", 4, ""});
  rows.push_back({"已开始的书籍 (" + std::to_string(ReadingStats::booksStarted()) + ")", "", 6, ""});
  for (const auto &b : ReadingStats::books()) {
    rows.push_back({b.title.empty() ? b.path : b.title, rdStatsBookRowRight(b), 5, b.path});
  }
}

// 统计主页书单那一段的窗口几何（卡片墙之下）。渲染 / 点按命中 / 翻页步长共用 ——
// 以前 renderStatsTab 与 rdStatsRowAt 各算一遍同一个式子。注意**上下键不走
// listViewKey**：这里的选中要跳过 act==6 的小标题行并且首尾环绕（rdStatsMoveSel），
// 是这一屏特有的语义，不能换成通用的"逐行 + 夹边界"。翻页步长 = 一屏行数。
static ListView statsTabListView(const std::vector<RdStatRow> &rows) {
  ListView lv;
  lv.top = rdStatsListTop();
  lv.itemH = uiLineHeight() + 12;
  lv.count = static_cast<int>(rows.size());
  lv.rows = std::max(1, (tabBottom() - lv.top - 6) / lv.itemH);
  lv.page = lv.rows;
  lv.sel = st.statsSel;
  listViewCenter(lv);
  return lv;
}

// 选中行移动（跳过小标题行）。
static void rdStatsMoveSel(const std::vector<RdStatRow> &rows, int delta) {
  const int n = static_cast<int>(rows.size());
  if (n == 0) return;
  int i = clampI(st.statsSel, 0, n - 1);
  for (int k = 0; k < n; k++) {
    i = (i + delta + n) % n;
    if (rows[i].act != 6) break;
  }
  st.statsSel = i;
  st.dirty = 1;
}

static void rdStatsOpenBook(const std::string &path, RdMode ret) {
  if (path.empty()) return;
  st.statsBookPath = path;
  st.statsTop = 0;
  st.retMode = ret;
  st.mode = RdMode::StatsBook;
  st.fullRefresh = true;
  st.dirty = 1;
}

static void rdStatsHeatmapEnter();
static void rdStatsAdjustEnter(const std::string &path);

// 从主页进某个子界面。
static void rdStatsActivate(const std::vector<RdStatRow> &rows) {
  if (rows.empty()) return;
  const int i = clampI(st.statsSel, 0, static_cast<int>(rows.size()) - 1);
  const RdStatRow &r = rows[i];
  st.retMode = RdMode::Stats;
  switch (r.act) {
    case 0: st.statsTop = 0; st.mode = RdMode::StatsMore; st.fullRefresh = true; break;
    case 1: rdStatsHeatmapEnter(); return;
    case 2: st.statsTop = 0; st.mode = RdMode::StatsProfile; st.fullRefresh = true; break;
    case 3: rdStatsAdjustEnter(r.path); return;
    case 4: st.mode = RdMode::StatsSettings; st.fullRefresh = true; break;
    case 5: rdStatsOpenBook(r.path, RdMode::Stats); return;
    default: return;
  }
  st.dirty = 1;
}

void renderStatsTab() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  int top = drawTabBar();

  std::vector<std::string> values, labels;
  rdStatsOverviewCards(values, labels);
  const int listTop = rdStatsCardGrid(top, values, labels) + 6;
  g_rd.drawLine(MARGIN, listTop - 3, w - MARGIN, listTop - 3, true);

  std::vector<RdStatRow> rows;
  rdStatsRows(rows);
  const int n = static_cast<int>(rows.size());
  const ListView lv = statsTabListView(rows);  // top == listTop，几何与点按命中共用
  const int itemH = lv.itemH;
  const int maxRows = lv.rows;
  const int start = lv.first;
  for (int i = 0; i < maxRows && start + i < n; i++) {
    const RdStatRow &r = rows[start + i];
    const int y = lv.top + i * itemH;
    const int ty = y + (itemH - uiLineHeight()) / 2;
    if (r.act == 6) {
      drawLineText(MARGIN, ty, r.label.c_str(), true);
      continue;
    }
    const bool sel = (start + i) == st.statsSel;
    if (sel) g_rd.fillRect(0, y, w, itemH, true);
    std::string label = r.label;
    if (r.act == 5 && !st.statsDelPath.empty() && st.statsDelPath == r.path) label += "  再长按删除";
    const int rw = r.right.empty() ? 0 : g_rd.getTextWidth(uiFontId(), r.right.c_str());
    const std::string l = g_rd.truncatedText(uiFontId(), label.c_str(), w - 2 * MARGIN - rw - 16);
    drawLineText(MARGIN, ty, l.c_str(), !sel);
    if (rw > 0) drawLineText(w - MARGIN - rw, ty, r.right.c_str(), !sel);
  }
}

// 主页里 y 落在哪一行（触摸命中用；与 renderStatsTab 共用 statsTabListView 的几何）。
static int rdStatsRowAt(int y) {
  std::vector<RdStatRow> rows;
  rdStatsRows(rows);
  return listViewHitAt(statsTabListView(rows), y);
}

void handleStatsTab(int key) {
  std::vector<RdStatRow> rows;
  rdStatsRows(rows);
  const int n = static_cast<int>(rows.size());
  const int maxRows = statsTabListView(rows).rows;  // 翻页步长 = 一屏行数（同一个几何）

  // ←→ 切标签，Esc/长按回书架（与「设置」标签一致）。
  if (key == KEY_LEFT) { switchTab(st.tab - 1); return; }
  if (key == KEY_RIGHT) { switchTab(st.tab + 1); return; }
  if (key == 0x1B) { st.statsDelPath.clear(); switchTab(0); return; }
  if (key == KEY_UP) { st.statsDelPath.clear(); rdStatsMoveSel(rows, -1); return; }
  if (key == KEY_DOWN) { st.statsDelPath.clear(); rdStatsMoveSel(rows, +1); return; }
  if (key == KEY_PAGE_UP) { st.statsDelPath.clear(); st.statsSel = std::max(0, st.statsSel - maxRows); st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.statsDelPath.clear(); st.statsSel = std::min(n - 1, st.statsSel + maxRows); st.dirty = 1; return; }

  // 长按书籍行 = 从统计里删掉这本书（二次确认：第一次长按就位，第二次才真删）。
  if (key == KEY_LONG_CONFIRM || key == KEY_TOUCH_LONG) {
    const int i = clampI(st.statsSel, 0, std::max(0, n - 1));
    if (i < n && rows[i].act == 5) {
      if (st.statsDelPath == rows[i].path) {
        ReadingStats::removeBook(rows[i].path);
        st.statsDelPath.clear();
        st.statsSel = clampI(st.statsSel, 0, std::max(0, static_cast<int>(ReadingStats::booksStarted()) + 5 - 1));
        rdShowFloat("已从统计中删除", rows[i].label, 3000);
      } else {
        st.statsDelPath = rows[i].path;
      }
      st.fullRefresh = true;
      st.dirty = 1;
      return;
    }
    st.statsDelPath.clear();
    switchTab(0);
    return;
  }

  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      const int t = tabHit(x, y);
      if (t >= 0) { switchTab(t); return; }
      const int row = rdStatsRowAt(y);
      if (row >= 0) st.statsSel = row;
    }
    rdStatsActivate(rows);
    st.dirty = 1;
    return;
  }
}

// main.cpp 的全局「长按中间确认键 = 待机」要放行三个把它当**动作键**的子界面
// （见 pjournal_app.h 的声明）：
//   ① 词典管理：长按 = 删本地词典（二次确认）；
//   ② 按键映射：长按 = 解绑当前动作；
//   ③ 阅读统计主页：长按 = 从统计里删掉选中的那本书（二次确认）。
// ③ 只在这一行确实是一本书（rdStatsRows 里 act==5 的行）时才放行——选中卡片/占位行
// 时它的长按只是"回书架"的另一条路（Esc 也是同一条路），那种场合让给待机更合理。
static bool rdStatsLongConfirmIsDelete() {
  std::vector<RdStatRow> rows;
  rdStatsRows(rows);
  const int n = static_cast<int>(rows.size());
  const int i = clampI(st.statsSel, 0, std::max(0, n - 1));
  return i < n && rows[i].act == 5;
}

bool screen_reader_long_confirm_is_action() {
  switch (st.mode) {
    case RdMode::DictDl: return true;
    case RdMode::KeyMap: return true;
    case RdMode::Stats: return rdStatsLongConfirmIsDelete();
    default: return false;
  }
}

// ── 单本书的统计详情 ────────────────────────────────────────────────────
// 预计剩余时间：总时长 / 进度 × (100 − 进度)，向上圆整到 5 分钟。读得太少
// （不足 10 分钟或进度 < 5%）不估——那时候的线性外推纯属噪声。
static std::string rdStatsEstimate(const ReadingBookStats &b) {
  if (b.completed || b.lastProgressPercent >= 100) return "已读完";
  const uint64_t tenMin = 10ULL * 60ULL * 1000ULL;
  const uint64_t fiveMin = 5ULL * 60ULL * 1000ULL;
  if (b.totalReadingMs < tenMin || b.lastProgressPercent < 5) return "多读一会儿再估";
  const uint64_t est = (b.totalReadingMs * 100ULL + b.lastProgressPercent - 1) / b.lastProgressPercent;
  if (est <= b.totalReadingMs) return "多读一会儿再估";
  uint64_t remaining = est - b.totalReadingMs;
  remaining = ((remaining + fiveMin - 1) / fiveMin) * fiveMin;
  std::string s = "约 " + ReadingStats::formatDurationHm(remaining);
  if (b.sessions > 0) {
    const uint64_t avg = b.totalReadingMs / b.sessions;
    if (avg >= fiveMin) {
      const uint64_t left = (remaining + avg - 1) / avg;
      if (left > 0) s += " / " + std::to_string(left) + " 次";
    }
  }
  return s;
}

struct RdKvRow {
  std::string label;
  std::string value;
  int bar;   // -1 不画进度条，否则 0..100
};

static void rdStatsBookRows(const ReadingBookStats &b, std::vector<RdKvRow> &rows) {
  rows.clear();
  rows.push_back({"全书进度", std::to_string(b.lastProgressPercent) + "%", b.lastProgressPercent});
  rows.push_back({"章节进度", std::to_string(b.chapterProgressPercent) + "%", b.chapterProgressPercent});
  rows.push_back({"当前章节", b.chapterTitle.empty() ? "未记录" : b.chapterTitle, -1});
  rows.push_back({"阅读总时长", ReadingStats::formatDurationHm(b.totalReadingMs), -1});
  rows.push_back({"阅读页数", std::to_string(b.totalPages) + " 页", -1});
  rows.push_back({"阅读次数", std::to_string(b.sessions) + " 次", -1});
  rows.push_back({"上次阅读", b.lastSessionMs > 0 ? ReadingStats::formatDurationHm(b.lastSessionMs) : "—", -1});
  rows.push_back({"预计还需", rdStatsEstimate(b), -1});
  rows.push_back({"状态", b.completed ? "已读完" : "阅读中", -1});
  rows.push_back({"最后阅读", rdStatsDate(b.lastReadAt), -1});
  rows.push_back({"开始 → 读完", (RdTime::clockValid(b.firstReadAt) ? rdStatsDate(b.firstReadAt) : std::string("?")) +
                                      " → " + (b.completedAt ? rdStatsDate(b.completedAt) : std::string("?")) + " ",
                  -1});
}

// 单本书统计：纯滚动（statsTop 就是顶行下标）。渲染与按键共用同一个几何。
static ListView statsBookListView(int count) {
  ListView lv;
  lv.top = coverTop();  // == drawTitle 的返回值
  lv.itemH = uiLineHeight() + 12;
  lv.count = count;
  lv.rows = std::max(1, (statusTop() - lv.top - 8) / lv.itemH);
  lv.first = st.statsTop;
  listViewScroll(lv);
  return lv;
}

void renderStatsBook() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const ReadingBookStats *b = ReadingStats::findBook(st.statsBookPath);
  if (!b) {
    int top = drawTitle("书籍统计");
    drawCenteredLine(top + 20, "这本书已不在统计里");
    return;
  }
  const std::string title = b->title.empty() ? b->path : b->title;
  drawTitle(fitWidth(title, w - 2 * MARGIN).c_str());  // 返回值 == statsBookListView().top

  std::vector<RdKvRow> rows;
  rdStatsBookRows(*b, rows);
  const int n = static_cast<int>(rows.size());
  const ListView lv = statsBookListView(n);
  const int itemH = lv.itemH;
  const int maxRows = lv.rows;
  const int start = lv.first;

  for (int i = 0; i < maxRows && start + i < n; i++) {
    const RdKvRow &r = rows[start + i];
    const int y = lv.top + i * itemH;
    drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, r.label.c_str(), true);
    const int lw = g_rd.getTextWidth(uiFontId(), r.label.c_str());
    const std::string v = g_rd.truncatedText(uiFontId(), r.value.c_str(), w - 2 * MARGIN - lw - 16);
    if (!v.empty()) {
      const int vw = g_rd.getTextWidth(uiFontId(), v.c_str());
      drawLineText(w - MARGIN - vw, y + (itemH - uiLineHeight()) / 2, v.c_str(), true);
    }
    // 进度条：槽描边 + 已读实心，颜色用灰阶（和状态带那条进度条同一套画法）。
    if (r.bar >= 0) {
      const int barY = y + itemH - 8;
      const int barW = w - 2 * MARGIN;
      g_rd.drawRect(MARGIN, barY, barW, 5, true);
      const int fillW = barW * clampI(r.bar, 0, 100) / 100;
      if (fillW > 0) g_rd.fillRect(MARGIN + 1, barY + 1, std::max(0, fillW - 2), 3, true);
    }
  }
  drawFooter("↑↓ 滚动  Esc 返回  回车 打开");
}

void handleStatsBook(int key) {
  if (key == 0x1B) { st.mode = st.retMode; st.fullRefresh = true; st.dirty = 1; return; }
  const ReadingBookStats *b = ReadingStats::findBook(st.statsBookPath);
  if (!b) { st.mode = st.retMode; st.dirty = 1; return; }
  int n = 0;
  { std::vector<RdKvRow> rows; rdStatsBookRows(*b, rows); n = static_cast<int>(rows.size()); }
  const int maxRows = statsBookListView(n).rows;  // 一屏行数（与渲染同一个几何）
  const int maxTop = std::max(0, n - maxRows);
  if (key == KEY_UP) { st.statsTop = std::max(0, st.statsTop - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.statsTop = std::min(maxTop, st.statsTop + 1); st.dirty = 1; return; }
  if (key == KEY_PAGE_UP) { st.statsTop = std::max(0, st.statsTop - maxRows); st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.statsTop = std::min(maxTop, st.statsTop + maxRows); st.dirty = 1; return; }
  if (key == '\n') {
    // 回车 = 打开这本书接着读（统计里点开一本书，多半就是想读它）。
    const int kind = rdStatsKindFor(st.statsBookPath);
    if (kind >= 0 && Storage.exists(st.statsBookPath.c_str()) && openBook(st.statsBookPath, kind)) {
      st.mode = RdMode::Reading;
      st.fullRefresh = true;
    } else {
      rdShowFloat("打不开这本书", st.statsBookPath, 3000);
    }
    st.dirty = 1;
    return;
  }
}

// ── 柱状图 ──────────────────────────────────────────────────────────────
// 描边柱 + 基线刻度（0 不画柱）。柱顶一行数值、柱底一行标签，都是水平文字
// ——本移植里 drawTextRotated90CW 是 stub，转不了，所以标签一律控制在 2~3 字符。
static void rdStatsChart(int x, int y, int w, int h, const std::vector<uint64_t> &values,
                         const std::vector<std::string> &topLabels, const std::vector<std::string> &botLabels) {
  const int n = static_cast<int>(values.size());
  if (n == 0) return;
  uint64_t maxV = 1;
  for (uint64_t v : values) maxV = std::max(maxV, v);

  const int lh = uiLineHeight();
  const int baseY = y + h - lh - 4;
  const int chartH = std::max(10, baseY - y - lh);
  const int gap = (n <= 7) ? 12 : 6;
  const int slot = (w - (n - 1) * gap) / n;
  int barW = std::min(slot, (n <= 7) ? 72 : 44);
  if (barW < 6) barW = 6;
  const int totalW = n * barW + (n - 1) * gap;
  const int bx = x + (w - totalW) / 2;

  for (int i = 0; i < n; i++) {
    const int px = bx + i * (barW + gap);
    if (values[i] == 0) {
      g_rd.fillRect(px, baseY - 2, barW, 3, true);
    } else {
      int bh = static_cast<int>(values[i] * static_cast<uint64_t>(chartH) / maxV);
      if (bh < 6) bh = 6;
      g_rd.drawRect(px, baseY - bh, barW, bh, true);
    }
    if (i < static_cast<int>(topLabels.size()) && !topLabels[i].empty()) {
      const std::string t = fitWidth(topLabels[i], barW + gap - 2);
      const int tw = g_rd.getTextWidth(uiFontId(), t.c_str());
      drawLineText(px + (barW - tw) / 2, baseY - chartH - lh, t.c_str(), true);
    }
    if (i < static_cast<int>(botLabels.size()) && !botLabels[i].empty()) {
      const std::string t = fitWidth(botLabels[i], barW + gap - 2);
      const int tw = g_rd.getTextWidth(uiFontId(), t.c_str());
      drawLineText(px + (barW - tw) / 2, baseY + 4, t.c_str(), true);
    }
  }
  g_rd.drawLine(x, baseY, x + w, baseY, true);
}

// 柱顶数值的短标签："45m"；年度图按 crossmux 的规则取整（<1h 用分钟、<24h 用小时、否则用天）。
static std::string rdStatsMinutesLabel(uint64_t ms) {
  const uint64_t minutes = ms / 60000ULL;
  if (minutes == 0) return "";
  return std::to_string(minutes) + "m";
}
static std::string rdStatsRoundedLabel(uint64_t ms) {
  if (ms == 0) return "";
  const uint64_t minutes = ms / 60000ULL;
  if (minutes < 60) return std::to_string(std::max<uint64_t>(1, minutes)) + "m";
  const uint64_t hours = (ms + 30ULL * 60ULL * 1000ULL) / (60ULL * 60ULL * 1000ULL);
  if (hours < 24) return std::to_string(std::max<uint64_t>(1, hours)) + "h";
  const uint64_t days = (ms + 12ULL * 60ULL * 60ULL * 1000ULL) / (24ULL * 60ULL * 60ULL * 1000ULL);
  return std::to_string(std::max<uint64_t>(1, days)) + "d";
}

// ── 更多详情：两张区间卡 + 每日/年度柱状图 ───────────────────────────────
void renderStatsMore() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = drawTabBar();
  const uint32_t ref = rdStatsRefOrdinal();

  std::vector<std::string> values, labels;
  rdStatsOverviewCards(values, labels);
  int y = rdStatsCardGrid(top, values, labels);

  std::vector<std::string> rangeVals = {ReadingStats::formatDurationHm(ReadingStats::recentReadingMs(7)),
                                        ReadingStats::formatDurationHm(ReadingStats::recentReadingMs(30))};
  std::vector<std::string> rangeLabels = {"近 7 天", "近 30 天"};
  y = rdStatsCardGrid(y, rangeVals, rangeLabels) + 4;

  // 每日阅读：参考日往前 7 天（从旧到新）。表头带日期范围，柱子下面只写日号。
  const int dayAreaH = std::max(90, uiLineHeight() * 4);
  std::vector<uint64_t> dayVals;
  std::vector<std::string> dayTop, dayBot;
  for (int i = 6; i >= 0; i--) {
    const uint32_t ord = (ref >= static_cast<uint32_t>(i)) ? ref - i : 0;
    dayVals.push_back(rdStatsDayMs(ord));
    dayTop.push_back(rdStatsMinutesLabel(rdStatsDayMs(ord)));
    int yy = 0;
    unsigned mm = 0, dd = 0;
    if (ord != 0 && RdTime::dateFromOrdinal(ord, yy, mm, dd)) {
      dayBot.push_back(std::to_string(dd));  // 只写日号：竖屏一根柱不到 90px，放不下 "10/03"
    } else {
      dayBot.push_back("");
    }
  }
  std::string dayTitle = "每日阅读";
  if (ref != 0) {
    const uint32_t startOrd = (ref >= 6) ? ref - 6 : 0;
    dayTitle += "  " + rdStatsOrdinalLabel(startOrd).substr(5) + " ~ " + rdStatsOrdinalLabel(ref).substr(5);
  }
  drawLineText(MARGIN, y, dayTitle.c_str(), true);
  y += uiLineHeight() + 6;
  rdStatsChart(MARGIN, y, w - 2 * MARGIN, dayAreaH, dayVals, dayTop, dayBot);
  y += dayAreaH + 14;

  // 每日页数：和上面同一个 7 天窗口，柱顶写页数（没翻页的那天不画柱，只留基线）。
  std::vector<uint64_t> pageVals;
  std::vector<std::string> pageTop(7);
  for (int i = 6; i >= 0; i--) {
    const uint32_t ord = (ref >= static_cast<uint32_t>(i)) ? ref - i : 0;
    const uint32_t p = ReadingStats::pagesOnDay(ord);
    pageVals.push_back(p);
    pageTop[6 - i] = p == 0 ? "" : std::to_string(p);
  }
  drawLineText(MARGIN, y, "每日页数", true);
  y += uiLineHeight() + 6;
  rdStatsChart(MARGIN, y, w - 2 * MARGIN, dayAreaH, pageVals, pageTop, dayBot);
  y += dayAreaH + 14;

  // 年度阅读：参考年 12 个月。
  int year = 0;
  unsigned refM = 1, refD = 1;
  if (ref != 0) RdTime::dateFromOrdinal(ref, year, refM, refD);
  std::string yearTitle = "年度阅读";
  if (year != 0) yearTitle += "  " + std::to_string(year);
  drawLineText(MARGIN, y, yearTitle.c_str(), true);
  y += uiLineHeight() + 6;
  std::vector<uint64_t> monthVals(12, 0);
  std::vector<std::string> monthTop(12), monthBot(12);
  if (year != 0) {
    for (const auto &d : ReadingStats::readingDays()) {
      int dy = 0;
      unsigned dm = 0, dd = 0;
      if (!RdTime::dateFromOrdinal(d.dayOrdinal, dy, dm, dd)) continue;
      if (dy == year && dm >= 1 && dm <= 12) monthVals[dm - 1] += d.readingMs;
    }
  }
  for (int i = 0; i < 12; i++) {
    monthTop[i] = rdStatsRoundedLabel(monthVals[i]);
    char b[4];
    snprintf(b, sizeof(b), "%d", i + 1);
    monthBot[i] = b;
  }
  rdStatsChart(MARGIN, y, w - 2 * MARGIN, dayAreaH, monthVals, monthTop, monthBot);

  drawFooter("↑↓ 滚动  Esc 返回");
}

// 更多详情整页内容高度（滚动上限用）。
static int rdStatsMoreContentH() {
  const int lh = uiLineHeight();
  const int grid1 = rdStatsCardRowsFor(kStatsCards) * (kStatsCardH + kStatsCardGap);
  const int grid2 = 1 * (kStatsCardH + kStatsCardGap);  // 2 张区间卡：无论横竖都是 1 行
  const int area = std::max(90, lh * 4);
  // 三段图：每日阅读 / 每日页数 / 年度阅读，每段都是 "标题 + 图"。
  const int charts = 3 * (lh + 6 + area + 14);
  return grid1 + 4 + grid2 + 4 + charts + 8;
}

void handleStatsMore(int key) {
  if (key == 0x1B) { st.mode = st.retMode; st.fullRefresh = true; st.dirty = 1; return; }
  if (key == KEY_LEFT || key == KEY_RIGHT) { st.mode = st.retMode; st.fullRefresh = true; st.dirty = 1; return; }
  const int viewH = statusTop() - coverTop() - 8;
  const int maxTop = std::max(0, rdStatsMoreContentH() - viewH);
  const int step = uiLineHeight() + 12;
  if (key == KEY_UP || key == KEY_PAGE_UP) { st.statsTop = std::max(0, st.statsTop - step); st.dirty = 1; return; }
  if (key == KEY_DOWN || key == KEY_PAGE_DOWN) { st.statsTop = std::min(maxTop, st.statsTop + step); st.dirty = 1; return; }
}

// ── 阅读热力图（月历 6×7 热力格）─────────────────────────────────────────
// 强度档（分钟）：0 无 / <15 无 / <30 1 / <60 2 / <120 3 / <240 4 / ≥240 5。
// 本移植没有真的抖动绘制，档位直接映射成 4bpp 灰阶（0=最黑）。
static int rdStatsHeatLevel(uint64_t ms) {
  const uint64_t minutes = ms / 60000ULL;
  if (ms == 0 || minutes < 15) return 0;
  if (minutes < 30) return 1;
  if (minutes < 60) return 2;
  if (minutes < 120) return 3;
  if (minutes < 240) return 4;
  return 5;
}
static uint8_t rdStatsHeatGray(int level) {
  switch (level) {
    case 1: return 13;
    case 2: return 11;
    case 3: return 8;
    case 4: return 4;
    case 5: return 0;
    default: return 15;
  }
}

static void rdStatsFillGray(int x, int y, int w, int h, uint8_t gray) {
  if (gray == 15) return;  // 白 = 不画，省一遍整块写
  for (int yy = y; yy < y + h; yy++) {
    for (int xx = x; xx < x + w; xx++) g_rd.drawGrayscale16Pixel(xx, yy, gray);
  }
}

static void rdStatsHeatmapEnter() {
  uint32_t ref = rdStatsRefOrdinal();
  int y = 2026;
  unsigned m = 1, d = 1;
  if (ref != 0) RdTime::dateFromOrdinal(ref, y, m, d);
  st.statsMonthY = y;
  st.statsMonthM = static_cast<int>(m);
  st.statsDay = ref != 0 ? ref : RdTime::ordinalForDate(y, m, 1);
  st.statsTop = 0;
  st.retMode = RdMode::Stats;
  st.mode = RdMode::StatsHeatmap;
  st.fullRefresh = true;
  st.dirty = 1;
}

void renderStatsHeatmap() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = drawTabBar();
  const int lh = uiLineHeight();

  const int year = st.statsMonthY ? st.statsMonthY : 2026;
  const int month = (st.statsMonthM >= 1 && st.statsMonthM <= 12) ? st.statsMonthM : 1;
  const int firstOrd = static_cast<int>(RdTime::ordinalForDate(year, static_cast<unsigned>(month), 1));
  // 周一为一周之首：1970-01-01（序号 0）是周四 → (ordinal + 3) % 7。
  const int firstWeekday = ((firstOrd + 3) % 7 + 7) % 7;
  const int gridStart = firstOrd - firstWeekday;

  char mbuf[16];
  RdTime::formatMonth(year, static_cast<unsigned>(month), mbuf, sizeof(mbuf));
  drawLineText(MARGIN, top, mbuf, true);
  {
    const std::string sel = rdStatsOrdinalLabel(st.statsDay);
    if (!sel.empty()) {
      const int sw = g_rd.getTextWidth(uiFontId(), sel.c_str());
      drawLineText(w - MARGIN - sw, top, sel.c_str(), true);
    }
  }

  // 月份合计 / 阅读天数 / 最佳一天 / 连续
  uint64_t monthTotal = 0, bestDay = 0;
  int bestDom = 0, readDays = 0;
  for (const auto &d : ReadingStats::readingDays()) {
    int dy = 0;
    unsigned dm = 0, dd = 0;
    if (!RdTime::dateFromOrdinal(d.dayOrdinal, dy, dm, dd)) continue;
    if (dy != year || static_cast<int>(dm) != month) continue;
    monthTotal += d.readingMs;
    if (d.readingMs > 0) readDays++;
    if (d.readingMs > bestDay) { bestDay = d.readingMs; bestDom = static_cast<int>(dd); }
  }
  std::vector<std::string> sv = {ReadingStats::formatDurationHm(monthTotal), std::to_string(readDays),
                                 bestDay > 0 ? ReadingStats::formatDurationHm(bestDay) + " (" +
                                                   std::to_string(bestDom) + "日)"
                                             : "—",
                                 std::to_string(ReadingStats::currentStreakDays()) + "d"};
  std::vector<std::string> sl = {"本月合计", "阅读天数", "最佳一天", "连续阅读"};
  const int cardsBot = rdStatsCardGrid(top + lh + 6, sv, sl);

  // 图例（5 档 + 文字）
  int ly = cardsBot + 4;
  drawLineText(MARGIN, ly, "15m+", true);
  int lx = MARGIN + g_rd.getTextWidth(uiFontId(), "15m+") + 8;
  for (int level = 1; level <= 5; level++) {
    g_rd.fillRect(lx, ly + 4, 22, 22, false);
    rdStatsFillGray(lx + 1, ly + 5, 20, 20, rdStatsHeatGray(level));
    g_rd.drawRect(lx, ly + 4, 22, 22, true);
    lx += 30;
  }
  drawLineText(lx + 4, ly, "240m+", true);

  // 月历格：7 列 × 6 行
  const int gridTop = ly + lh + 8;
  const int gridW = w - 2 * MARGIN;
  int cellW = gridW / 7;
  const int availH = tabBottom() - gridTop - 6;
  int cellH = std::min(availH / 6, cellW * 3 / 2);
  if (cellH < 24) cellH = 24;
  const int gridX = MARGIN + (gridW - cellW * 7) / 2;
  const uint32_t todayOrd = RdTime::todayOrdinal();

  for (int i = 0; i < 42; i++) {
    const uint32_t ord = static_cast<uint32_t>(gridStart + i);
    const int gx = gridX + (i % 7) * cellW;
    const int gy = gridTop + (i / 7) * cellH;
    int dy = 0;
    unsigned dm = 0, dd = 0;
    const bool valid = ord != 0 && RdTime::dateFromOrdinal(ord, dy, dm, dd);
    if (!valid) continue;
    const bool inMonth = (dy == year && static_cast<int>(dm) == month);
    const uint64_t ms = rdStatsDayMs(ord);
    const int level = inMonth ? rdStatsHeatLevel(ms) : 0;
    if (inMonth && level > 0) rdStatsFillGray(gx + 1, gy + 1, cellW - 2, cellH - 2, rdStatsHeatGray(level));
    g_rd.drawRect(gx, gy, cellW, cellH, true);
    // 日号：深档反白。跨月的格子只写日号、不填色（灰淡一点不必，黑白本来就一样）
    char dbuf[4];
    snprintf(dbuf, sizeof(dbuf), "%u", dd);
    const bool white = inMonth && level >= 4;
    drawLineText(gx + 4, gy + 2, dbuf, !white);
    if (inMonth && ms > 0) {
      // 时长只写"分钟数"，字号放不下完整 "45m"
      const std::string m2 = std::to_string(ms / 60000ULL) + "m";
      const std::string t = g_rd.truncatedText(uiFontId(), m2.c_str(), cellW - 8);
      drawLineText(gx + 4, gy + cellH - lh - 2, t.c_str(), !white);
    }
    // 达标勾选 / 今天 / 选中
    if (inMonth && ms >= ReadingStats::goalMs()) {
      g_rd.fillRect(gx + cellW - 12, gy + 3, 8, 8, !white);
    }
    if (ord == todayOrd) g_rd.drawRect(gx + 2, gy + 2, cellW - 4, cellH - 4, true);
    if (ord == st.statsDay) g_rd.drawRect(gx + 1, gy + 1, cellW - 2, cellH - 2, 2, true);
  }

  drawFooter("←→ 换月  ↑↓ 选日  回车 当天进详情  Esc 返回");
}

void handleStatsHeatmap(int key) {
  if (key == 0x1B) { st.mode = st.retMode; st.fullRefresh = true; st.dirty = 1; return; }

  auto shiftMonth = [](int dir) {
    int y = st.statsMonthY ? st.statsMonthY : 2026;
    int m = st.statsMonthM;
    m += dir;
    if (m < 1) { m = 12; y--; }
    if (m > 12) { m = 1; y++; }
    st.statsMonthY = y;
    st.statsMonthM = m;
    // 选中日跟着挪到新月，日号超出当月天数就夹到月末。
    int sy = 0;
    unsigned sm = 0, sd = 0;
    if (st.statsDay != 0 && RdTime::dateFromOrdinal(st.statsDay, sy, sm, sd)) {
      const unsigned dim = RdTime::daysInMonth(y, static_cast<unsigned>(m));
      if (sd > dim) sd = dim;
      st.statsDay = RdTime::ordinalForDate(y, static_cast<unsigned>(m), sd);
    } else {
      st.statsDay = RdTime::ordinalForDate(y, static_cast<unsigned>(m), 1);
    }
    st.fullRefresh = true;
    st.dirty = 1;
  };

  if (key == KEY_LEFT) { shiftMonth(-1); return; }
  if (key == KEY_RIGHT) { shiftMonth(+1); return; }
  if (key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
    const int dir = (key == KEY_DOWN || key == KEY_PAGE_DOWN) ? +1 : -1;
    const uint32_t cur = st.statsDay ? st.statsDay : RdTime::ordinalForDate(st.statsMonthY, static_cast<unsigned>(st.statsMonthM), 1);
    const uint32_t next = (dir > 0) ? cur + 1 : (cur > 1 ? cur - 1 : cur);
    int ny = 0;
    unsigned nm = 0, nd = 0;
    if (RdTime::dateFromOrdinal(next, ny, nm, nd)) {
      st.statsDay = next;
      // 跨月：跟着把视图挪过去（和 crossmux 一样，选中日走出当月就换月）。
      if (ny != st.statsMonthY || static_cast<int>(nm) != st.statsMonthM) {
        st.statsMonthY = ny;
        st.statsMonthM = static_cast<int>(nm);
        st.fullRefresh = true;
      }
    }
    st.dirty = 1;
    return;
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      const int seg = tabHit(x, y);
      if (seg >= 0) { switchTab(seg); return; }
    }
    st.statsTop = 0;
    st.retMode = RdMode::StatsHeatmap;
    st.mode = RdMode::StatsDay;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
}

// ── 某一天的阅读详情 ────────────────────────────────────────────────────
// 日详情列表的顶边：概览卡一行 + 分隔 + "当天读过的书" 标题行。render 与触摸命中
// 必须用同一个数，否则点按会错行。
static int rdStatsDayListTop() {
  return coverTop() + rdStatsCardRowsFor(3) * (kStatsCardH + kStatsCardGap) + 6 + uiLineHeight() + 8;
}

// 当日书单：纯滚动（statsTop 就是顶行下标，没有选中项）。渲染 / 点按命中 / 翻页步长共用。
static ListView statsDayListView(int count) {
  ListView lv;
  lv.top = rdStatsDayListTop();
  lv.itemH = uiLineHeight() + 14;
  lv.count = count;
  lv.rows = std::max(1, (statusTop() - lv.top - 6) / lv.itemH);
  lv.first = st.statsTop;
  listViewScroll(lv);
  return lv;
}

void renderStatsDay() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = drawTitle(("阅读日 " + rdStatsOrdinalLabel(st.statsDay)).c_str());

  const auto books = rdStatsBooksOnDay(st.statsDay);
  std::vector<std::string> values = {ReadingStats::formatDurationHm(rdStatsDayMs(st.statsDay)),
                                     std::to_string(ReadingStats::pagesOnDay(st.statsDay)),
                                     std::to_string(books.size())};
  std::vector<std::string> labels = {"当日合计", "当日页数", "读过的书"};
  int y = rdStatsCardGrid(top, values, labels) + 6;
  drawLineText(MARGIN, y, "当天读过的书", true);
  y = rdStatsDayListTop();

  const int n = static_cast<int>(books.size());
  const ListView lv = statsDayListView(n);  // 顶行下标 = statsTop（渲染与点按命中共用）
  const int itemH = lv.itemH;
  const int maxRows = lv.rows;
  const int start = lv.first;
  if (n == 0) {
    drawCenteredLine(y + 20, "这一天没有阅读记录");
  }
  for (int i = 0; i < maxRows && start + i < n; i++) {
    const RdDayBook &db = books[start + i];
    const int ry = lv.top + i * itemH;
    std::string title = db.book->title.empty() ? db.book->path : db.book->title;
    const std::string right = ReadingStats::formatDurationHm(db.ms);
    const int rw = g_rd.getTextWidth(uiFontId(), right.c_str());
    title = g_rd.truncatedText(uiFontId(), title.c_str(), w - 2 * MARGIN - rw - 16);
    drawLineText(MARGIN, ry + (itemH - uiLineHeight()) / 2, title.c_str(), true);
    drawLineText(w - MARGIN - rw, ry + (itemH - uiLineHeight()) / 2, right.c_str(), true);
  }
  drawFooter("↑↓ 滚动  Esc 返回");
  (void)w;
}

void handleStatsDay(int key) {
  if (key == 0x1B) { st.mode = st.retMode; st.fullRefresh = true; st.dirty = 1; return; }
  const int n = static_cast<int>(rdStatsBooksOnDay(st.statsDay).size());
  const int maxRows = statsDayListView(n).rows;  // 一屏行数（与渲染同一个几何）
  const int maxTop = std::max(0, n - maxRows);
  if (key == KEY_UP) { st.statsTop = std::max(0, st.statsTop - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.statsTop = std::min(maxTop, st.statsTop + 1); st.dirty = 1; return; }
  if (key == KEY_PAGE_UP) { st.statsTop = std::max(0, st.statsTop - maxRows); st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.statsTop = std::min(maxTop, st.statsTop + maxRows); st.dirty = 1; return; }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      const int seg = tabHit(x, y);
      if (seg >= 0) { switchTab(seg); return; }
      const auto books = rdStatsBooksOnDay(st.statsDay);
      const int row = listViewHitAt(statsDayListView(static_cast<int>(books.size())), y);
      if (row >= 0) {
        rdStatsOpenBook(books[row].book->path, RdMode::StatsDay);
        return;
      }
    }
    return;
  }
}

// ── 阅读档案：4 轴雷达 + 总分 + 分轴指标 ────────────────────────────────
struct RdProfileAxis {
  std::string name;
  int score = 0;
  std::string m1Label, m1Value;
  std::string m2Label, m2Value;
};

static int rdRoundDiv(int n, int d) { return d == 0 ? 0 : (n + d / 2) / d; }
static int rdClampPct(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

// 近 7 天的档案评分，逐行对齐 crossmux 的 buildReadingProfileSummary（去掉成就）。
static void rdStatsProfileBuild(int &total, std::vector<RdProfileAxis> &axes) {
  axes.clear();
  total = 0;
  const uint32_t ref = rdStatsRefOrdinal();
  const int kDays = 7;
  RdProfileAxis habit, stability, engagement, depth;
  habit.name = "习惯";
  stability.name = "稳定";
  engagement.name = "投入";
  depth.name = "深度";
  if (ref == 0) {
    habit.m1Label = "读书天"; habit.m1Value = "0/7";
    habit.m2Label = "达标天"; habit.m2Value = "0/7";
    stability.m1Label = "连读"; stability.m1Value = "0d";
    stability.m2Label = "最佳占比"; stability.m2Value = "0%";
    engagement.m1Label = "次数"; engagement.m1Value = "0";
    engagement.m2Label = "次/读书天"; engagement.m2Value = "0";
    depth.m1Label = "<10m"; depth.m1Value = "0%";
    depth.m2Label = "10-29m"; depth.m2Value = "0%";
    axes = {habit, stability, engagement, depth};
    return;
  }

  const uint32_t startOrd = ref >= (uint32_t)(kDays - 1) ? ref - (kDays - 1) : 0;
  const uint64_t goal = ReadingStats::goalMs();
  uint64_t byDay[kDays] = {0};
  for (const auto &d : ReadingStats::readingDays()) {
    if (d.dayOrdinal < startOrd) continue;
    if (d.dayOrdinal > ref) continue;
    const size_t idx = static_cast<size_t>(d.dayOrdinal - startOrd);
    if (idx < kDays) byDay[idx] += d.readingMs;
  }

  uint64_t weekly = 0, maxDay = 0;
  int daysRead = 0, goalDays = 0, longestStreak = 0, run = 0;
  for (int i = 0; i < kDays; i++) {
    weekly += byDay[i];
    maxDay = std::max(maxDay, byDay[i]);
    if (byDay[i] > 0) {
      daysRead++;
      if (byDay[i] >= goal) {
        goalDays++;
        run++;
        longestStreak = std::max(longestStreak, run);
      } else {
        run = 0;
      }
    } else {
      run = 0;
    }
  }

  int bestDayShare = 0;
  if (weekly > 0) bestDayShare = rdRoundDiv(static_cast<int>(maxDay * 100ULL), static_cast<int>(weekly));

  // 近 7 天的会话日志（不足则退回"每天一场"的估算，和 crossmux 一致）。
  std::vector<uint32_t> sessions;
  const auto &log = ReadingStats::sessionLog();
  for (auto it = log.rbegin(); it != log.rend(); ++it) {
    if (it->dayOrdinal < startOrd) break;
    if (it->dayOrdinal <= ref) sessions.push_back(it->sessionMs);
  }
  if (sessions.empty() && daysRead > 0) {
    for (int i = 0; i < kDays; i++) {
      if (byDay[i] > 0) sessions.push_back(static_cast<uint32_t>(std::min<uint64_t>(byDay[i], 0xFFFFFFFFULL)));
    }
  }

  const uint32_t kTen = 10 * 60 * 1000, kThirty = 30 * 60 * 1000;
  int under10 = 0, mid = 0, over = 0;
  for (uint32_t s : sessions) {
    if (s < kTen) under10++;
    else if (s < kThirty) mid++;
    else over++;
  }
  const int nSessions = static_cast<int>(sessions.size());
  int perDayTenths = daysRead > 0 ? rdRoundDiv(nSessions * 10, daysRead) : 0;
  int pUnder = 0, pMid = 0, pOver = 0;
  if (nSessions > 0) {
    pUnder = rdRoundDiv(under10 * 100, nSessions);
    pMid = rdRoundDiv(mid * 100, nSessions);
    if (pUnder + pMid > 100) {
      if (pMid >= pUnder) pMid = 100 - pUnder;
      else pUnder = 100 - pMid;
    }
    pOver = rdClampPct(100 - pUnder - pMid);
  }

  const int habitScore = rdClampPct(rdRoundDiv(daysRead * 65 + goalDays * 35, kDays));
  const int streakScore = goalDays > 0 ? rdRoundDiv(longestStreak * 100, goalDays) : 0;
  int balanceScore = 0;
  if (daysRead > 1 && weekly > 0) {
    const double best = static_cast<double>(maxDay) / static_cast<double>(weekly);
    const double ideal = 1.0 / static_cast<double>(daysRead);
    const double norm = 1.0 - ((best - ideal) / (1.0 - ideal));
    balanceScore = rdClampPct(static_cast<int>(norm * 100.0 + 0.5));
  }
  const int stabilityScore = rdClampPct((streakScore + balanceScore + 1) / 2);
  const int sessionsScore = std::min(100, nSessions * 10);
  const int perDayScore = daysRead > 0 ? std::min(100, rdRoundDiv(nSessions * 100, daysRead * 3)) : 0;
  const int engagementScore = rdClampPct((sessionsScore * 60 + perDayScore * 40 + 50) / 100);
  const int depthScore = rdClampPct(rdRoundDiv(pMid * 50 + pOver * 100, 100));
  total = rdClampPct((habitScore + stabilityScore + engagementScore + depthScore + 2) / 4);

  auto pctLabel = [](int v) { return std::to_string(v) + "%"; };
  habit.score = habitScore;
  habit.m1Label = "读书天"; habit.m1Value = std::to_string(daysRead) + "/" + std::to_string(kDays);
  habit.m2Label = "达标天"; habit.m2Value = std::to_string(goalDays) + "/" + std::to_string(kDays);
  stability.score = stabilityScore;
  stability.m1Label = "连读"; stability.m1Value = std::to_string(longestStreak) + "d";
  stability.m2Label = "最佳占比"; stability.m2Value = pctLabel(bestDayShare);
  engagement.score = engagementScore;
  engagement.m1Label = "次数"; engagement.m1Value = std::to_string(nSessions);
  engagement.m2Label = "次/读书天";
  engagement.m2Value = std::to_string(perDayTenths / 10) + (perDayTenths % 10 ? "." + std::to_string(perDayTenths % 10) : "");
  depth.score = depthScore;
  depth.m1Label = "<10m"; depth.m1Value = pctLabel(pUnder);
  depth.m2Label = "10-29m"; depth.m2Value = pctLabel(pMid);
  axes = {habit, stability, engagement, depth};
}

void renderStatsProfile() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int h = g_rd.getScreenHeight();
  const int top = drawTabBar();

  int total = 0;
  std::vector<RdProfileAxis> axes;
  rdStatsProfileBuild(total, axes);

  // 雷达图：4 条轴在 45/135/225/315 度，环 25/50/75/100。
  const int cx = w / 2;
  const int cy = top + (tabBottom() - top) / 2 - uiLineHeight();
  int R = std::min(w / 4, (tabBottom() - top - uiLineHeight() * 4) / 3);
  if (R < 40) R = 40;
  const double kPi = 3.14159265358979;
  auto axisAngle = [&](int i) { return kPi * 0.25 + i * kPi * 0.5; };

  for (int ring = 1; ring <= 4; ring++) {
    int rx[4], ry[4];
    for (int i = 0; i < 4; i++) {
      const double a = axisAngle(i);
      rx[i] = cx + static_cast<int>(R * ring / 4 * std::cos(a));
      ry[i] = cy + static_cast<int>(R * ring / 4 * std::sin(a));
    }
    for (int i = 0; i < 4; i++) g_rd.drawLine(rx[i], ry[i], rx[(i + 1) % 4], ry[(i + 1) % 4], true);
  }
  int ax[4], ay[4];
  for (int i = 0; i < 4; i++) {
    const double a = axisAngle(i);
    ax[i] = cx + static_cast<int>(R * std::cos(a));
    ay[i] = cy + static_cast<int>(R * std::sin(a));
    g_rd.drawLine(cx, cy, ax[i], ay[i], true);
  }
  int px[4], py[4];
  for (int i = 0; i < 4; i++) {
    const double a = axisAngle(i);
    const int r = R * rdClampPct(axes[i].score) / 100;
    px[i] = cx + static_cast<int>(r * std::cos(a));
    py[i] = cy + static_cast<int>(r * std::sin(a));
  }
  g_rd.fillPolygon(px, py, 4, true);
  for (int i = 0; i < 4; i++) g_rd.fillRect(px[i] - 3, py[i] - 3, 6, 6, true);

  // 轴名 + 分数
  for (int i = 0; i < 4; i++) {
    const std::string t = axes[i].name + " " + std::to_string(axes[i].score);
    const int tw = g_rd.getTextWidth(uiFontId(), t.c_str());
    int tx = ax[i] + (ax[i] >= cx ? 6 : -tw - 6);
    tx = clampI(tx, MARGIN, std::max(MARGIN, w - MARGIN - tw));
    int ty = ay[i] + (ay[i] >= cy ? 0 : -uiLineHeight());
    drawLineText(tx, ty, t.c_str(), true);
  }

  // 总分（居中，压在雷达图下面）
  {
    const std::string t = "综合评分 " + std::to_string(total) + " / 100";
    drawLineText((w - g_rd.getTextWidth(uiFontId(), t.c_str())) / 2, std::min(tabBottom() - uiLineHeight() - 4, cy + R + 8),
                 t.c_str(), true);
  }

  // 分轴指标：每轴一行，四行排在雷达图下面（滚动看更多）
  const int itemH = uiLineHeight() + 10;
  int y = cy + R + uiLineHeight() + 14;
  for (int i = 0; i < 4 && y + itemH <= tabBottom(); i++) {
    const RdProfileAxis &a = axes[i];
    const std::string left =
        a.name + "  " + a.m1Label + " " + a.m1Value + "   " + a.m2Label + " " + a.m2Value;
    const std::string l = g_rd.truncatedText(uiFontId(), left.c_str(), w - 2 * MARGIN);
    drawLineText(MARGIN, y, l.c_str(), true);
    y += itemH;
  }
  (void)h;
  drawFooter("Esc 返回");
}

void handleStatsProfile(int key) {
  if (key == 0x1B || key == KEY_LEFT || key == KEY_RIGHT) {
    st.mode = st.retMode;
    st.fullRefresh = true;
    st.dirty = 1;
  }
}

// ── 调整某本书某一天的阅读时长 ──────────────────────────────────────────
static const int kStatsAddMinutes[4] = {15, 30, 45, 60};

static void rdStatsAdjustEnter(const std::string &path) {
  // 从主页进来时用当前选中的那本书；为空就用最近读的那本。
  std::string p = path;
  if (p.empty() && !ReadingStats::books().empty()) p = ReadingStats::books().front().path;
  if (p.empty()) {
    rdShowFloat("还没有可调整的书", "先读一会儿再来", 3000);
    return;
  }
  st.statsBookPath = p;
  st.statsAdjField = 0;
  st.statsAdjOp = 0;
  st.statsAdjAmt = 1;
  st.statsAdjFailed = false;
  const uint32_t ref = rdStatsRefOrdinal();
  st.statsAdjDay = ref;
  st.retMode = RdMode::Stats;
  st.mode = RdMode::StatsAdjust;
  st.fullRefresh = true;
  st.dirty = 1;
}

void renderStatsAdjust() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const ReadingBookStats *b = ReadingStats::findBook(st.statsBookPath);
  const int top = drawTitle("调整阅读时长");
  drawLineText(MARGIN, top, b ? (b->title.empty() ? b->path : b->title).c_str() : "（这本书已不在统计里）", true);

  const int itemH = uiLineHeight() + 16;
  int y = top + uiLineHeight() + 12;
  const char *names[3] = {"操作", "日期", "数量"};
  std::string vals[3];
  vals[0] = st.statsAdjOp == 0 ? "增加" : "减少";
  vals[1] = rdStatsOrdinalLabel(st.statsAdjDay);
  if (vals[1].empty()) vals[1] = "未设置";
  vals[2] = std::to_string(kStatsAddMinutes[clampI(st.statsAdjAmt, 0, 3)]) + " 分钟";

  for (int i = 0; i < 3; i++) {
    const int ry = y + i * itemH;
    if (i == st.statsAdjField) g_rd.fillRect(0, ry, w, itemH, true);
    const bool black = (i != st.statsAdjField);
    drawLineText(MARGIN, ry + (itemH - uiLineHeight()) / 2, names[i], black);
    const int vw = g_rd.getTextWidth(uiFontId(), vals[i].c_str());
    drawLineText(w - MARGIN - vw, ry + (itemH - uiLineHeight()) / 2, vals[i].c_str(), black);
    // 左右箭头提示（只有当前字段可调）
    if (i == st.statsAdjField) {
      drawLineText(MARGIN + g_rd.getTextWidth(uiFontId(), names[i]) + 12, ry + (itemH - uiLineHeight()) / 2, "◀ ▶", black);
    }
  }
  y += 3 * itemH + 8;

  // 预览：那天现在的合计 → 调整后；不足则明确说不给减。
  const uint64_t cur = rdStatsDayMs(st.statsAdjDay);
  const uint64_t delta = static_cast<uint64_t>(kStatsAddMinutes[clampI(st.statsAdjAmt, 0, 3)]) * 60ULL * 1000ULL;
  std::string preview;
  if (!b) {
    preview = "这本书已不在统计里";
  } else if (st.statsAdjDay == 0) {
    preview = "先设置日期";
  } else if (st.statsAdjOp == 0) {
    preview = "当日合计 " + ReadingStats::formatDurationHm(cur) + " → " + ReadingStats::formatDurationHm(cur + delta);
  } else if (cur < delta) {
    preview = "当日合计 " + ReadingStats::formatDurationHm(cur) + "（不够减）";
  } else {
    preview = "当日合计 " + ReadingStats::formatDurationHm(cur) + " → " + ReadingStats::formatDurationHm(cur - delta);
  }
  drawLineText(MARGIN, y, preview.c_str(), true);
  y += uiLineHeight() + 8;
  if (st.statsAdjFailed) drawLineText(MARGIN, y, "改不了：这一天没有这么多记录", true);

  drawFooter("↑↓ 选字段  ←→ 改值  回车 应用  Esc 返回");
}

void handleStatsAdjust(int key) {
  if (key == 0x1B) { st.mode = st.retMode; st.fullRefresh = true; st.dirty = 1; return; }
  if (key == KEY_UP) { st.statsAdjField = (st.statsAdjField + 2) % 3; st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.statsAdjField = (st.statsAdjField + 1) % 3; st.dirty = 1; return; }
  if (key == KEY_LEFT || key == KEY_RIGHT) {
    const int dir = (key == KEY_RIGHT) ? +1 : -1;
    if (st.statsAdjField == 0) st.statsAdjOp ^= 1;
    else if (st.statsAdjField == 1) {
      if (st.statsAdjDay == 0) st.statsAdjDay = rdStatsRefOrdinal();
      else {
        const uint32_t next = (dir > 0) ? st.statsAdjDay + 1 : (st.statsAdjDay > 1 ? st.statsAdjDay - 1 : st.statsAdjDay);
        st.statsAdjDay = next;
      }
    } else {
      st.statsAdjAmt = (st.statsAdjAmt + dir + 4) % 4;
    }
    st.statsAdjFailed = false;
    st.dirty = 1;
    return;
  }
  if (key == '\n') {
    const int64_t amount = static_cast<int64_t>(kStatsAddMinutes[clampI(st.statsAdjAmt, 0, 3)]) * 60LL * 1000LL;
    const int32_t delta = static_cast<int32_t>(st.statsAdjOp == 0 ? amount : -amount);
    if (st.statsAdjDay == 0 || !ReadingStats::adjustBookReadingTime(st.statsBookPath, st.statsAdjDay, delta)) {
      st.statsAdjFailed = true;
      st.dirty = 1;
      return;
    }
    rdShowFloat("已调整", ReadingStats::formatDurationHm(rdStatsDayMs(st.statsAdjDay)), 3000);
    st.mode = st.retMode;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
}

// ── 统计设置：每日目标 ──────────────────────────────────────────────────
static const int kStatsGoals[4] = {15, 30, 45, 60};

void renderStatsSettings() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = drawTitle("统计设置");
  drawLineText(MARGIN, top, "每日目标（达标的天才计入连续与热力图勾选）", true);

  const int cur = g_settings.dailyGoalMinutes();
  // 这一屏没有光标（高亮的是"当前值"），选中项传 0；几何与点按命中同源就好。
  const ListView lv = flatListViewAt(coverTop() + uiLineHeight() + 14, uiLineHeight() + 14, 4, 0);
  const int itemH = lv.itemH;
  for (int i = 0; i < 4; i++) {
    const int ry = lv.top + i * itemH;
    const bool sel = (kStatsGoals[i] == cur);
    if (sel) g_rd.fillRect(0, ry, w, itemH, true);
    const std::string label = std::to_string(kStatsGoals[i]) + " 分钟";
    drawLineText(MARGIN, ry + (itemH - uiLineHeight()) / 2, label.c_str(), !sel);
    if (sel) {
      const std::string mark = "当前";
      drawLineText(w - MARGIN - g_rd.getTextWidth(uiFontId(), mark.c_str()), ry + (itemH - uiLineHeight()) / 2, mark.c_str(),
                   false);
    }
  }
  drawFooter("回车 选定  Esc 返回");
}

void handleStatsSettings(int key) {
  if (key == 0x1B) { st.mode = st.retMode; st.fullRefresh = true; st.dirty = 1; return; }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      const int seg = tabHit(x, y);
      if (seg >= 0) { switchTab(seg); return; }
      const ListView lv = flatListViewAt(coverTop() + uiLineHeight() + 14, uiLineHeight() + 14, 4, 0);
      const int row = listViewHitAt(lv, y);
      if (row >= 0) {
        g_settings.setString("daily_goal", std::to_string(kStatsGoals[row]).c_str());
        ReadingStats::save();
        rdShowFloat("每日目标已更新", std::to_string(kStatsGoals[row]) + " 分钟", 3000);
        st.fullRefresh = true;
        st.dirty = 1;
      }
      return;
    }
    return;
  }
  // 数字键 1..4 直接选（和设置页的选项行同一个习惯）。
  if (key >= '1' && key <= '4') {
    const int row = key - '1';
    g_settings.setString("daily_goal", std::to_string(kStatsGoals[row]).c_str());
    ReadingStats::save();
    rdShowFloat("每日目标已更新", std::to_string(kStatsGoals[row]) + " 分钟", 3000);
    st.fullRefresh = true;
    st.dirty = 1;
  }
}
