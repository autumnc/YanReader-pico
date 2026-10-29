#pragma once
// ── 阅读模式内部共享层（screen_reader_internal.h）────────────────────────
// **不是对外接口**——对外只有 screen_reader.h 那几个函数（init/exit/handle + 待机封面
// 的两个取数口）。这一份是阅读模式**自己人**用的：阅读器外壳的状态量（RdState）、
// 外壳字体/排版几何的原语、几个跨子界面的动作。
//
// 存在的唯一理由是 screen_reader.cpp 太大（12343 行、561KB），按子应用拆文件时要有个
// 地方放"大家都要用的那部分"。第一个拆出去的是微信读书（screen_reader_weread.cpp）：
// 它要读 st.we*、要画标题栏/底栏、要 switchTab/openBook。
//
// 拆的规矩（照着往下搬时别走样）：
//   · 状态量留在 RdState 里**一份**，不按子应用切分 —— 它们互相纠缠（微读下载完要
//     重扫书架并改 st.mode、设置页条目也要切 st.mode），切开会切出两套真相。
//   · 共享原语在这里只放**声明**，定义仍在 screen_reader.cpp（那些原本 static 的
//     符号因此去掉了 static）。不改成函数指针表：调用点原地不动才是这个重构的价值。
//   · 新拆一个子应用：把它的 render/handle 搬走，在这里补它用到的共享符号。

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <esp_heap_caps.h>
#include <HalStorage.h>
#include <Epub.h>
#include <Section.h>
#include <Xtc.h>
#include <Dictionary.h>
#include <WeReadClient.h>
#include <WeReadStore.h>

#include "dictionary_store.h"
#include "opds_client.h"
#include "tab_icons.h"
#include "ui/list_view.h"
#include "weread_skill_client.h"   // 发现页（搜索/推荐）的结果类型进了 RdState

// Operation 约 8KB（内含 4KB 收发缓冲），内部 RAM 紧张，优先放 PSRAM。
struct WeOpDeleter {
  void operator()(WeReadClient::Operation *op) const {
    if (!op) return;
    std::destroy_at(op);
    heap_caps_free(op);
  }
};

// ── 常量 ────────────────────────────────────────────────────────────────
#define UI_FONT_ID 0
#define BODY_FONT_ID_BASE 1  // 正文字体 id = BODY_FONT_ID_BASE + fontLevel (1..9)
// 「UI 尺寸、用户字体」的 id：插的是与 UI_FONT_ID **同一个** EpdFontFamily，所以字号/
// 度量完全相同，但 id != 0 → GfxRenderer 会选**内容面**（用户所选字体）。给底部状态栏、
// 笔记、微读书架这类"位置属于外壳、内容属于用户"的地方用：内置 builtin.ttf 是 7710 字
// 的子集，书名/笔记里出现子集外的字就是豆腐块，换成用户字体才显示得全。
// 用 20 而不是 6：正文字体 id 现在涨到 1..9，6 会被撞上。
// ⚠ 阅读器代码不要直接用这个宏，用下面的 uiFontId()：书内嵌字体模式会把内容面换成
//   本书字面，那时外壳必须退回内置字体（见 uiFontId 的注释）。
#define CONTENT_UI_FONT_ID 20
// 书架**列表样式**行内文字专用的小一号 id（比 uiFontId() 小 6px，见 shelfListFontId()）。
// 书架列表是"扫一眼挑书"的版面，菜单/对话框那档 UI 字号（42px）在列表里偏大。
// 21 / 22 都不被占用：正文字体是 1..9，UI 面只有 id 0、内容 UI 面是 20。
#define SHELF_LIST_FONT_ID 21
// 阅读页**状态栏**（时钟/书名/页码/百分比/电量）专用的小一号 id（比 uiFontId() 小 4px，
// 见 readerSbFontId()）。状态栏是常驻的一行小字，与正文/菜单同大偏抢眼。
// ⚠ 只缩字，**不缩带子**：footerH()/statusTop() 决定正文排版基准，带子高度不动，
//   所以每页行数、页码、书签一个都不变（文件分界线也照旧）。
#define READER_SB_FONT_ID 22


inline constexpr const char *CACHE_DIR = "/sdcard/.crossmux";
inline constexpr const char *DICT_ROOT = "/sdcard/dictionaries";

// 正文字号表（ttf 像素高，即 EpdFontData::advanceY）。前 kUserFontLevels 档菜单可选，
// 末尾两档是**标题专用**——CSS 阶梯的向上余量，菜单里选不到，保证正文选到最大档（64）时
// 标题仍能再大 1~2 档。全部注册为字体 id 1..N（dummy EpdFontData，不额外占内存；
// 字形缓存按字节计，clamp_px 上限 120 够用）。
// 26/30/34px 三档已删（实测太小、没法读）。注意这会把**档位下标整体左移**：老
// reader_font_level 存的是 9 档表的下标，直接沿用会静默换大小 —— 见 screen_reader_init()
// 里的 _v2 键迁移。
inline constexpr int kBodyPx[6] = {40, 46, 54, 64, 76, 88};
inline constexpr int kBodyPxCount = 6;
inline constexpr int kUserFontLevels = 4;  // 可选档位数：0..3 = 40..64px
inline constexpr int kDefaultFontLevel = 1;  // 46px（删档前的默认 34px 已不存在）
inline constexpr float kLineSpacings[5] = {1.0f, 1.2f, 1.4f, 1.6f, 1.8f};
inline constexpr float kDefaultLineSpacing = 1.2f;
// 段间距 6 档（0=关）。数值语义同 EPUB 引擎的 extraParagraphSpacing：
// "每段之后额外留 0.5/0.75/1/1.25/1.5 倍行高"。参与 Section 排版缓存键，改档会自动重排。
inline constexpr const char *kParaSpacingLabels[6] = {"关", "0.5x", "0.75x", "1x", "1.25x", "1.5x"};

// 正文左右边距三档（窄/标准/宽）。
inline constexpr int kMargins[3] = {20, 30, 45};
inline constexpr int kDefaultMarginIdx = 1;

// 字重：**只往粗的一边走**。0 档 = 不加粗（今天的观感，也是默认档），加粗档 +1…+9
// 一档 20（CSS 字重增量 20…180），一共 10 档。
// 不做变细：stb 没有轮廓腐蚀的公开 API，硬做只会啃掉细笔画，观感是"字发虚"而不是"变细"。
// 一档 20 而不是 KOReader 的 ±100 一档：那样一按就跳一大截，这里要的是每档只差一点、
// 由用户自己微调到顺手的那档。
//
// 0 档是**真的一个字节都不加**（合成半径 0，走单采样那条原路），所以老用户升级后默认
// 观感分毫不动；装了自带 wght 轴的字体也一样（0 不进"改字面字重"那一段）。加粗从 +1
// 起，+1/+2 在外号下还很轻，越往后笔画外扩越明显。
inline constexpr int kFontWeights[10] = {0, 20, 40, 60, 80, 100, 120, 140, 160, 180};
inline constexpr const char *kFontWeightLabels[10] = {"0", "+1", "+2", "+3", "+4",
                                                      "+5", "+6", "+7", "+8", "+9"};
inline constexpr int kFontWeightCount = 10;
inline constexpr int kDefaultFontWeightIdx = 0;  // 0 = 不加粗 = 今天的观感

// 对比度：抗锯齿覆盖率的 gamma。标准档 = TTF_COVER_GAMMA(0.6) = 今天的观感；
// 小 → 边缘更深（柔、墨重），大 → 更浅（锐、干净）。
inline constexpr float kContrastGammas[5] = {0.45f, 0.52f, 0.6f, 0.78f, 1.0f};
inline constexpr const char *kContrastLabels[5] = {"低", "偏低", "标准", "偏高", "高"};
inline constexpr int kContrastCount = 5;
inline constexpr int kDefaultContrastIdx = 2;

inline constexpr int MARGIN = 30;    // 正文/列表左右边距
inline constexpr int RD_TOP_INSET = 28;    // 顶部留白(标题起始)，原 8，整体下移 20px
inline constexpr int RD_BODY_TOP = 50;     // 正文起始，原 MARGIN(30)，整体下移 20px
inline constexpr int RD_BOTTOM_INSET = 4;  // 底部留白(状态栏下方)，原 24，状态栏下移 20px

// 阅读页右上角"这一页有书签"那枚实心书签（画在 [0, RD_BODY_TOP) 这条安全带里）。
// 字形来自 NF-Propo 子集，见 scripts/subset_icon_font.py 的 BOOKMARK 组与
// main/gfx/icon_font.h（不走字体 id 体系，直接喂给 icon_font_draw*）。
inline constexpr uint32_t RD_ICON_BOOKMARK = 0xF00C0;   // md-bookmark（实心）

// ── 阅读页底部状态栏：每一项的档位表 ────────────────────────────────────
// 设置页（RdMode::StatusBar，screen_reader_subpages.cpp）与弹层选择
// （screen_reader.cpp 的 openRdPick）**共用这一张**：加一项/加一档/改个标签只动
// 这里一处，两边不会各写一份而对不上。names[] 的下标就是存进 settings 的值。
//
// 第 0 行恒是"整条状态栏显示不显示"的开关，**必须排第一**：关掉之后设置页只留
// 这一行（底下那七项设置了也看不见，留着只会让人以为设置没生效），而"可见的恒是
// 表的前缀"这一点让行号与表下标始终相等，不必再维护一张映射表（见 sbRowCount）。
// 它也是唯一一项**改版式**的：关掉之后正文栏一直排到屏底（rdPaneBottom()），
// 所以 applyRdPick 里它单独走 reopenBook() 重排，其余七项只重画。
struct RdSbRow {
  const char *key;
  const char *label;
  int def;
  const char *const *names;
  int nameCount;
};
inline constexpr const char *kSbOnNames[2] = {"显示", "隐藏"};
inline constexpr const char *kSbTitleNames[3] = {"书名", "章节", "隐藏"};
inline constexpr const char *kSbPageNames[3] = {"章节", "全书", "隐藏"};
inline constexpr const char *kSbShowNames[2] = {"显示", "隐藏"};
inline constexpr const char *kSbBarNames[3] = {"全书", "章节", "隐藏"};
inline constexpr const char *kSbThickNames[3] = {"细", "中", "粗"};
inline constexpr const char *kSbClockNames[3] = {"隐藏", "右侧", "左侧"};
inline constexpr RdSbRow kSbRows[] = {
    {"reader_sb_on", "状态栏", 0, kSbOnNames, 2},
    {"reader_sb_title", "标题", 0, kSbTitleNames, 3},
    {"reader_sb_page", "页码", 1, kSbPageNames, 3},
    {"reader_sb_pct", "百分比", 0, kSbShowNames, 2},
    {"reader_sb_bar", "进度条", 2, kSbBarNames, 3},
    {"reader_sb_thick", "进度条粗细", 1, kSbThickNames, 3},
    {"reader_sb_batt", "电量", 0, kSbShowNames, 2},
    {"reader_sb_clock", "时钟", 0, kSbClockNames, 3},
};
inline constexpr int kSbRowCount = static_cast<int>(sizeof(kSbRows) / sizeof(kSbRows[0]));

// 「状态栏」设置页那几行也复用同一个弹层（st.pickAct）：act 取 9000 + 行号，故意
// 落在 MenuAct 之外 —— screen_reader.cpp 的 rdPickTable / rdPickCurValue / applyRdPick
// 三个 switch 都在 switch **之前**先判这一段，MenuAct 那批一行都不用改。
inline constexpr int kSbPickBase = 9000;


// ── 阅读器状态 ──────────────────────────────────────────────────────────
enum class RdMode {
  Browser, Reading, Toc, Menu, LayoutMenu, Footnotes, Percent, Qr, Dictionary, Weread, Wifi,
  Apps,        // 1 号根标签「应用」的图标入口页（文件管理 / 微信读书 / 字典）
  ShelfMenu, ShelfInfo, Recent, FileBrowser, Image, Opds, NetShare, DictDl, ResDl, KeyMap, StatusBar, About,
  Settings, Notes, NoteEdit, NoteDetail,
  WereadQr,    // 扫码登录
  WereadMenu,  // 选中书目的操作菜单
  WereadDl,    // 缓存进度
  WereadDetail,// 书籍详情（简介 / 出版社 / 分类 / 评分 / 字数）
  WereadNotes, // 微读笔记/划线/书评（三段：热门划线/我的划线/热门书评）
  WereadSync,  // 阅读进度同步（与本机/云端核对，需要选方向时给选择屏）
  WereadDiscover, // 发现：官方 Skill API 的搜索 / 推荐（结果可进详情、可缓存整本）
  ShelfSearch, // 书架搜索（书名关键字过滤）
  NotesSearch, // 笔记搜索（原文/笔记正文/书名过滤）
  FileMenu,    // 文件长按弹出：打开/重命名/删除/详情
  FileRename,  // 重命名（虚拟键盘编辑文件名）
  FileInfo,    // 文件详情（大小/时间/路径）
  Stats,       // 统计标签主页（概览卡 + 入口列表 + 已开始的书籍）
  StatsBook,   // 单本书的统计详情
  StatsMore,   // 更多详情（7/30 天卡 + 每日/年度柱状图）
  StatsHeatmap,// 阅读热力图（月历热力格）
  StatsDay,    // 某一天的阅读详情
  StatsProfile,// 阅读档案（4 轴雷达 + 总分）
  StatsAdjust, // 调整某本书某一天的阅读时长
  StatsSettings,// 统计设置（每日目标）
  RefreshTest,  // 灰阶自检页（16 级梯 / 细线 / 抖动三档对比 / 各刷法耗时，自推屏）
  AiChat        // AI 对话（两个入口：阅读页选区「问AI」带 ctx / 应用页「DeepSeek 对话」无 ctx）
};

// 主界面五个根标签：0=书架 1=应用 2=笔记 3=设置 4=统计。标签栏只画图标（tab_icons.h）。
// 1 号位原本直接是 SD 卡文件浏览器（RdMode::FileBrowser）；现在改成「应用」图标入口页
// （RdMode::Apps：文件管理 / 微信读书 / 字典），文件浏览器降级成它的子界面，
// 微信读书也一并从「设置」标签的条目表搬了过来（与「设置 → 词典下载」无关的那一批）。
// 统计放最后一位：st.tab == 1/3 的判断遍布各处，插在中间要动的地方多。
inline constexpr int kTabCount = 5;
// 图标表：0 和 1 是哨兵——0 = 统计的柱状图、1 = 应用的九宫格，都走程序化绘制（drawTabBar
// 分派到 drawStatsTabIcon / drawAppsTabIcon），不查字体。理由同 drawStatsTabIcon：
// 引进真字形要重裁 NF-Propo 子集，而当前环境没有 fontTools。
inline constexpr uint32_t kTabIcons[kTabCount] = {TAB_ICON_BOOKSHELF, TAB_ICON_APPS_SENTINEL,
                                                  TAB_ICON_NOTES, TAB_ICON_SETTINGS, 0};

inline RdMode tabMode(int tab) {
  return tab == 1 ? RdMode::Apps : tab == 2 ? RdMode::Notes
         : tab == 3 ? RdMode::Settings : tab == 4 ? RdMode::Stats : RdMode::Browser;
}

struct BookEntry {
  std::string path;
  std::string name;
  int kind;  // 0=epub 1=txt 2=xtc
};

// 浮动阅读菜单的一行。字体/排版两个 tab 是**滑块行**（[标签] [轨道 ●] [当前档名]），
// 目录/笔记是**列表行**（整行文字）。两种共用一个模型：count==0 就是列表/nav 行。
struct RdSheetRow {
  int act = -1;                       // MenuAct 的整数值（MenuAct 定义在后面，这里只能存 int）
  bool nav = false;                   // true = 不值调（nav 行 / 禁用的滑块行）：不画轨道
  bool disabled = false;              // nav 行的"置灰"（如 缩进/对齐 在"随书"时）
  std::string label;                  // 左端标签（滑块）/ 整行文字（列表行）
  std::string value;                  // 右端当前档名（滑块）/ 右端备注（nav 行）
  int idx = 0;                        // 滑块：当前档下标；列表行：条目下标（跳转用）
  int count = 0;                      // 滑块：档数（0 = 列表/nav 行）
  int level = 0;                      // 目录 tab：缩进级别；笔记 tab：0 = 书签 / 1 = 标注
  std::vector<std::string> names;     // 滑块档名（画两端与当前值）；列表行为空
};

struct RdState {
  RdMode mode = RdMode::Browser;
  int dirty = 1;
  bool fullRefresh = true;  // 进入新界面首帧用 GC16 清残影

  // 阻塞段的"正在…"浮层：打开大书（建元数据/解 zip/分章排版）前先刷一帧出去，
  // 用户才知道界面没卡死。只在 renderCurrent 里画，不当普通状态用。
  std::string busyMsg, busySub;

  // 瞬时浮动提示（居中黑底反白框，到点自动消失）。标签页的提示栏取消之后，那些
  // "刚做完了什么"的消息（服务地址、重命名结果、传完提示）改走这里——它们本来就
  // 是几秒钟的事，常驻一整行反而是浪费。
  std::string floatMsg, floatSub;
  int64_t floatUntilUs = 0;

  // 主界面根标签（书架/应用/笔记/设置/统计）与「设置」标签的选中行
  int tab = 0;
  int setSel = 0;

  // 「应用」标签（1 号位）里挑中的那枚图标入口：0=文件管理 1=微信读书 2=字典。
  // 单行三格，←→ 与 ↑↓ 都在这三个之间挪（这个标签像书架一样借用了左右键，见 handleApps）。
  int appSel = 0;

  // 「设置」标签的弹层选择（轮换制条目改成弹出式）。浮层盖在列表上，**不换 st.mode**
  // ——底图仍是设置标签，所以绘制走 renderCurrent 的浮层段、按键在分发前先被它拦下。
  // pickAct 存的是 MenuAct 的整数值（MenuAct 定义在本结构体后面，这里只能存 int）。
  bool pickOpen = false;
  int pickAct = -1;
  std::string pickTitle;
  std::vector<std::string> pickLabels;  // 显示用
  std::vector<std::string> pickValues;  // 落定后交给 applyRdPick 解释（不透明）
  int pickSel = 0;
  int pickScroll = 0;
  // 这个弹层是从**浮动阅读菜单**的滑块行开出来的：↑↓ 挪档要挂"停手 0.5s 才落地"
  // （见 rdSheetLiveSchedule）。设置标签里开的弹层置 false —— 那里改排版/方向立刻
  // 生效才是对的。
  bool pickLive = false;

  // ── 浮动阅读菜单（下半屏菜单卡 + 顶部三按钮）──────────────────────────────
  // 用户要的形态（`../阅读菜单.jpg`）：阅读页正中点一下浮出，再点一下（或 Esc）收起。
  // **模式仍是 RdMode::Reading** —— 两张卡是画在正文页之上的浮层（与脚注弹注同一层，
  // 见 renderCurrent 的浮层段）。正文像素本来就在屏上、常驻首帧（s_hold）与邻页留档
  // 照旧有效，所以开/合菜单只是"又一次正文页差分刷"，不整屏闪、不重排。
  // **三条硬不变量**（挪代码前先读 sheet 那一节的注释）：①卡片绝不进 renderReading
  // ②rdLiveLayerUp() 不许加 sheetOpen ③换模式/回书架/退出阅读器都要清 sheetOpen。
  bool sheetOpen = false;
  int  sheetTab = 0;                  // 0=目录 1=字体 2=笔记 3=排版
  int  sheetSel[4] = {0, 0, 0, 0};    // 每个 tab 各自记光标（切回来不跳回首行）
  int  sheetDragRow = -1;             // 正在拖的滑块行（-1 = 没在拖）
  bool sheetReturn = false;           // 从浮动菜单进的全屏页（书签/设置/排版设定页）Esc 回阅读页
  // 实时预览：改档**先只改眼前的画**，停手约 0.5s 才落地（落地 = reopenBook 重排 +
  // 一次整屏 GC16）。挂在阅读器空闲帧上，见 rdSheetLiveTick。dueUs==0 = 没有待落地的改动。
  int64_t sheetLiveDueUs = 0;
  int  sheetLiveAct = -1;             // 待落地的 MenuAct 整数值
  int  sheetLiveIdx = -1;             // 待落地的档位下标（画刀柄用；书等落地）
  std::string sheetLiveVal;           // 待落地的值字面量（交给 applyRdPick）
  // 当前 tab 的行表。**只在开菜单 / 切 tab / 落地之后重建**，每帧只画不建 ——
  // FontFamily 那张档位表会 ttf_font_scan() 扫 SD，每帧建一次就是每帧一次目录扫描。
  std::vector<RdSheetRow> sheetRows;



  // 子界面 Esc 的返回目标：从「阅读菜单」进 → Menu；从「设置」标签进 → Settings。
  // 这样同一批子界面（WiFi/OPDS/按键映射…）在两个入口下都能回到来处。
  RdMode retMode = RdMode::Menu;

  // 书架
  std::vector<BookEntry> books;
  int sel = 0;
  // 书架/笔记搜索：查询串 + 结果列表里的选中项（0 = 第一命中）。
  // 查询串留在这里而不是模式里，退出搜索不清空——再点一次放大镜能接着改。
  std::string shelfQuery;
  std::string notesQuery;
  int searchSel = 0;

  // 当前书
  int bookKind = -1;  // -1 无 0 epub 1 txt 2 xtc
  std::string bookPath;
  std::string bookTitle;

  // epub
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section;
  int spineIndex = 0;
  int page = 0;

  // 当前 epub 页的两条派生信息：「这一页有没有脚注」（决定菜单里出不出现「脚注」项）与
  // 「这一页的可见文本偏移」（书签的身份）。两条都要先把这一页读出来才知道，而
  // loadPage 是**开一次 section .bin + 反序列化整页**（另加一次开文件读 offset LUT）。
  // 菜单每帧都问一次（renderMenu → menuItems），也就是每帧一次 SD 读。
  //
  // 改成：渲染这一页时（renderEpubPage）顺手采一次，之后直接读缓存。
  // 键是 (章, 页)：页只由 turnBook 改、改完必然重渲染；重排一定新建 Section、必过
  // openSpine，那里显式作废（同 footnoteCache* 的规矩）。同一个 Section 里页内容不会
  // 变（构建只是往后追加），所以 (章, 页) 命中即可信。
  // pageInfoSpine < 0 = 无缓存，此时 getter 退回现算（宁可慢一拍也不能给错答案）。
  int pageInfoSpine = -1;
  int pageInfoPage = -1;
  bool pageInfoFootnotes = false;
  uint32_t pageInfoOffset = UINT32_MAX;  // UINT32_MAX = 该页没有可见偏移记录

  // 书内嵌字体：非空 = 内容面此刻装的是从当前这本书里解出来的字体（指向缓存目录里的
  // 落盘副本）。bookFontTag 跟着它进版式缓存键（见 ReaderRenderSpec::fontTag）；
  // 0 表示内容面是用户/内建字体。两者都在 openBook() 里换书时清掉。
  // **空 ≠ 这本书没装任何内嵌字体**：「内嵌字体=替换主字体」档下正文故意不装书里的
  // primary，这里就恒为空而次字面（下面那个）正装着。它是"内容面是不是本书字面"的判据，
  // 不是"这本书有没有内嵌字体"的判据。
  std::string bookFontLocal;
  uint32_t bookFontTag = 0;
  // 书内 CSS 的**家族字面**：次家族（祖堂集：正文宋体、注文/引文仿宋）与第三家族
  // （同一本书再指一个家族时的那份）。跟 bookFontLocal 同生共死：换书/退出阅读/SD 挂起
  // 时一起关（每份就占 ~1MB PSRAM，留着就是漏）。
  // **非空 = 那一面此刻装着的正是这本书的那个家族** —— openEpub() 拿它当"已就位"
  // 的判据（替换主字体档下那段字体会被每次 reopenBook() 重入，不判就位就要重读 SD）。
  // 空 = 没装成（这本书没有那么多家族 / 解压或打开失败），此时 g_rd 里对应的家族哈希是 0，
  // 排版不会给任何词打 ALT_FONT / ALT2_FONT，绘制完全等价于那一面没打开。
  // 两槽互不依赖：只有次家族的书写常（也应当）把 alt2 留空。
  std::string bookFontAltLocal;
  uint32_t bookFontAltTag = 0;
  std::string bookFontAlt2Local;
  uint32_t bookFontAlt2Tag = 0;

  // txt
  std::string txtUtf8;
  std::vector<size_t> txtLineStarts;  // 每行起始字节偏移（分页表）
  int txtPage = 0;
  // txt 目录：打开时扫一遍正文挑出章节标题行（TXT 没有 EPUB 的 ncx/spine）。
  // 两个表一一对应；章节 → 页码在跳转时按当前分页表换算（见 txtPageForOffset）。
  std::vector<size_t> txtChapterOffsets;
  std::vector<std::string> txtChapterTitles;

  // xtc
  std::unique_ptr<Xtc> xtc;
  int xtcPage = 0;

  // toc
  int tocSel = 0;

  // menu
  // 「书签」页（RdMode::Menu）的选中**行**：行表是「书签(N) 分组行 + 本书书签子行 + 工具项」
  // 拼出来的（见 rdBookMenuRows），所以它是行号不是条目号 —— 折叠一下行号的含义就变了。
  int menuSel = 0;
  int menuScroll = 0;      // 该页首个可见行（行高不一，只能按行滚，同 notesScroll）
  // 该页的分组行展开着没有。**默认展开**：这一页主要就是来看本书书签的，进来先给列表。
  // 不持久化 —— 每次 openMenu() 重置（折叠只是这一趟里的临时动作，不新增 settings 键）。
  bool bookMenuOpen = true;
  // 「排版设定」子菜单的选中行。和 menuSel 分开存：从子菜单退回来时父菜单的光标要落回
  // 「排版设定」那一行，两者同时有意义，共用一个变量就必然要丢一个。
  int layoutSel = 0;

  // 书架菜单
  int shelfMenuSel = 0;
  bool shelfDelArm = false;  // 删除二次确认已就位（再按一次才真删）
  int shelfIdx = -1;         // 长按锁定的书目下标（-1 = 无，动作回退到 sel）

  // 最近阅读（保存路径/标题，最多 kMaxRecent 条，最新在前）
  std::vector<BookEntry> recent;
  int recentSel = 0;

  // 文件浏览器
  std::string fbPath = "/sdcard";
  std::vector<BookEntry> fbEntries;  // kind=-1 目录 0 epub 1 txt 2 xtc 3 图片 4 其它文件
  int fbSel = 0;

  // 文件长按菜单（FileMenu/FileRename/FileInfo 共用）
  int fmIdx = -1;                  // 长按锁定的条目下标（-1 = 无）
  int fmSel = 0;                   // 菜单选中行
  bool fmDelArm = false;           // 删除二次确认已就位（再按一次才真删）
  bool fmMkdir = false;            // FileRename 复用为「新建文件夹」输入（无扩展名、提交走 mkdir）
  std::string fmRenameBuf;         // 重命名/新建文件夹的编辑缓冲
  std::string fmStatus;            // 操作结果提示（成功/失败）

  // 文件剪贴板（单个）：复制/剪切后暂存源路径，粘贴到当前目录。只存一个——一次复制
  // 一个文件或文件夹，再复制另一个就替换。剪切是「粘贴成功后删源」，粘之前源都在原地。
  std::string fmClipPath;          // 空 = 剪贴板空
  std::string fmClipName;          // 底栏/路径行显示用（源文件名）
  bool fmClipCut = false;          // true=剪切（粘贴成功后删源）

  // 图片查看器（同目录图片列表，左右翻页；源窗口缩放 + 拖动平移）
  std::vector<std::string> imgList;
  int imgSel = 0;
  float imgZoom = 1.0f;   // 1.0 = 适应屏幕
  // 源窗口左上角占源图的比例，取值 [0, 1 - 1/zoom]，由拖动更新。存比例而不是屏幕像素：
  // 捏合改了 zoom 之后不需要重新标定，夹取范围（1-1/zoom）自己会跟着缩。
  float imgPanX = 0.0f;
  float imgPanY = 0.0f;
  bool imgFailed = false;
  // 当前图的源尺寸（renderImage 里探到就记下）。拖动帧要拿它算"屏幕 1px = 源图多少比例"，
  // 每帧重开文件探尺寸太亏，而且拖动是每帧一发的。
  int imgSrcW = 0;
  int imgSrcH = 0;
  // 从阅读页长按插图进来的查看器：Esc 回阅读页（而不是文件浏览器），且不动标签高亮。
  bool imgFromReader = false;

  // 书签
  struct RdBookmark {
    std::string path;
    int kind;
    int spine;
    uint32_t offset;
    int page;
    float percent;
    std::string summary;
  };
  std::vector<RdBookmark> bookmarks;
  // 书签删除的二次确认（照笔记列表 noteDelArm 那套）：长按一次挂起、再长按同一条才真删，
  // 免得误触直接丢书签。「书签」菜单页一套、浮动菜单「笔记」tab 一套（两个不同的下标空间：
  // 前者是 rdBookMenuRows 造出来的**行号**，后者是 st.sheetRows 的下标，不能合并）。
  // 挂起态存的是行号而不是书签下标 —— 长按之后行表可能被重建（折叠/删除），按行号才对得上
  // 用户眼前看到的那一行。
  bool bookmarkDelArm = false;
  int bookmarkDelIdx = -1;
  bool sheetDelArm = false;
  int sheetDelIdx = -1;

  // 脚注
  std::vector<std::string> footnoteNums, footnoteHrefs;
  int footnoteSel = 0;
  // footnoteNums/Hrefs 装的是**哪一页**的脚注表。原来每次点注（含每一次"点链接试试
  // 是不是脚注"）都重新 loadPage 一次 —— 一次 SD 读 + 整页反序列化，几百微秒到几毫秒。
  // 按 (spine, page, 字号档) 记账：换页/换章/换字号才重读。字号换的是书的排版，
  // 同页的脚注表会变，所以它也得进键。
  int footnoteCacheSpine = -1;
  int footnoteCachePage = -1;
  int footnoteCacheFont = -1;
  std::string footnoteCacheBook;  // 换书后 spine/page 可能撞上，书路径也得进键
  // 挂起的弹注（异步）。大书（晋书/祖堂集）的注文整块压在**章末**，读者在章首点注号时
  // 要排几十页才够得着锚点 —— 同步排就是冻屏 2.5 秒，超时了还落进翻页（用户报的
  // "每次弹注都很慢"）。改成：按键那一拍只查不排，没查到就把这条挂起、起一个"正在取注…"
  // 浮层立刻返回；空闲帧继续排版（每帧限时），排到锚点再自动弹出来。
  int fnWaitIdx = -1;        // 待弹的脚注在本页脚注表里的下标，-1 = 无
  int fnWaitSpine = -1;      // 挂起时在第几章；换章即作废
  std::string fnWaitAnchor;  // 要等的锚点 id
  // 挂起弹注的**跨文件那一支**：注文不在本章，而在另一个 spine 里（calibre 把整本注释
  // 拆成 notes.xhtml 的书全是这样，href = "notes.xhtml#fn1"）。那种书本章的锚点表里
  // **永远**没有这个锚点，所以不能像本节那样"接着排本章" —— 得为目标节另开一份 Section
  // 把它排出来（见 rdMakeAltSection，**不动 st.section**：弹注是盖在当前页上的浮层，
  // 底下的正文页不能换，换了就落进"返回脚注跳转前"）。排整节要几秒，照样挂给空闲帧。
  // -1 = 挂起的不是跨文件那一支。
  int fnWaitAltSpine = -1;
  std::string fnWaitAltBook;  // 换书后 spine 号可能撞上，书路径也得进键
  // 挂起的**跨文件链接**落页（异步）。指向别的节的链接（calibre 的 notes.xhtml#fn1）
  // 要先开过去、再排到那个锚点才落页，而目标节常常从没排过 —— 几秒的排版绝不能塞进
  // "点下去到屏幕开始变"那一拍。与上面挂起弹注同一个套路：按键只开过节省下的交给
  // 空闲帧，见 rdLinkWaitTick。spineIndex = -1 = 无挂起。
  int linkWaitSpine = -1;      // 已经开过去的那一节；换章即作废
  std::string linkWaitAnchor;  // 要等的锚点 id
  std::string linkWaitBook;    // 换书后 spine 号可能撞上，书路径也得进键
  int64_t linkWaitUntilUs = 0; // 到点还没排到就放弃（用户早已翻走，别突然跳）
  // 本页的链接矩形（Page::links 抄过来的，坐标已加渲染偏移）。点按命中的**第一依据**：
  // 拿到 href 就能直接查到锚点 id，不必再去比注号（注号是猜出来的，href 是书里写死的）。
  // 数组小（每页最多 32 条，多数页 0~2 条），且是纯数据，跟着页重建。
  struct RdLinkRect { std::string href; int x, y, w, h; };
  std::vector<RdLinkRect> pageLinks;
  int footnoteRetSpine = 0, footnoteRetPage = 0;
  bool footnoteRetValid = false;
  // 注号 → 正文里那个**上标**所在的（章节, 页）。点注文条目行首的注号时靠它跳回上标处。
  // 一次一条学出来的（见 rdRememberNoteRef）：点开某条注、或从某条注跳注的那一刻，
  // 读者正待在那个上标所在的那一页上，顺手就记下了。学不到的下场是退化成往前扫页
  // （rdSearchNoteRefPage），所以这只是个"快表"，不是唯一数据源。
  struct RdNoteRef { std::string num; int spine; int page; };
  std::vector<RdNoteRef> noteRefs;
  // 脚注弹注（不切模式的浮层，盖在正文页上）。fnPopNum 非空 = 开着；
  // fnPopText 是注释正文原文，画的时候按弹窗宽度现断行，fnPopScroll 是首行偏移。
  // fnPopBox* 是上一帧画出来的框，触摸要靠它分"点框里=跳注 / 点框外=关闭"。
  std::string fnPopNum, fnPopText;
  int fnPopScroll = 0;
  int fnPopIdx = -1;   // 弹注对应脚注表里的第几条：弹注里的"跳注"要按它跳，不能按列表的选中项
  int fnPopBoxX = 0, fnPopBoxY = 0, fnPopBoxW = 0, fnPopBoxH = 0;

  // 百分比跳转
  int percentVal = 50;

  // 二维码
  std::string qrText;

  // 阅读设定（与写作模式共享 g_settings 存储，键名带 reader_ 前缀）
  int fontLevel = kDefaultFontLevel;
  float lineSpacing = kDefaultLineSpacing;
  int paraSpacing = 0;  // 段间距：0=关，1..5 = 0.5/0.75/1/1.25/1.5 倍行高
  int indentMode = 0;  // 0 自动 1 强制 2 取消
  int alignMode = 0;   // 0 两端 1 左 2 居中 3 书籍样式（跟随 CSS text-align）
  int marginIdx = kDefaultMarginIdx;        // 边距档位
  // 正文墨色：字重（kFontWeights 的下标）与对比度（kContrastGammas 的下标）。
  // 两者都**不改步进与度量**，所以改档只重画、不重排（见 ttf_font.h 的 ttf_body_ink_*）。
  int fontWeight = kDefaultFontWeightIdx;
  int contrast = kDefaultContrastIdx;
  int readingLine = 0;                      // 阅读线：0 无 1 虚线 2 点线 3 实线（正文行间引导线）
  // 标注风格（存下来的标注 + 正在选的选区都按它画；档位定义见 kAnnotStyleNames）：
  // 0 黑底反白 / 1 下划线 / 2 波浪线 / 3 灰底反白。与 readingLine 同一类 —— **只影响绘制**，
  // 不改版式，所以改档只重画当前页、不 reopenBook。像它一样，这一位也必须进 rdHoldKey。
  int annotStyle = 0;
  bool imageBilinear = true;                // 图片缩放：true 双线性 false 最近邻
  // 图片抖动档：0 有序 1 行扩散 2 关。存 int 而不是 DitherMode，这个头就不必
  // include crossmux 的 DitherUtils.h（档位序号与 kRdDitherKeys 一一对应）。
  int imageDither = 0;
  // 本帧内容是否带真中灰（插图页/图片查看器/灰阶自检页）。renderCurrent 每帧先清，
  // 三条路径自己置位；**推屏之后**用它记下"面板现在是什么"，供白底参考帧纪律消费
  // （中间灰不能当差分刷的参考帧，见 reader_refresh_bridge.h）。
  int frameGray = 0;
  bool night = false;                       // 夜间反色
  std::string orientation = "landscape";    // 阅读器方向

  // 词典
  Dictionary dict;
  bool dictOpen = false;
  std::string dictQuery;
  std::string dictResult;
  std::string dictHeadword;
  std::string dictStatus;
  // 释义正文的折行缓存与滚动。长释义（几 KB）逐帧重新量宽度会明显拖慢打字/滑动的响应，
  // 所以折行只算一次：dictLinesW 记下建它时的排版宽度，宽度一变（换字号/转屏）就重建；
  // 换词条时 doDictLookup 直接失效（dictLinesW = -1）。
  std::vector<std::string> dictLines;
  int dictLinesW = -1;      // -1 = 缓存失效，需要重建
  int dictScroll = 0;       // 正文首行偏移（行号），渲染时夹到 [0, dictScrollMax]
  int dictScrollMax = 0;    // 渲染时算出的最大首行偏移；翻页键的上限
  int dictPageLines = 1;    // 渲染时算出的一屏行数；翻页键的步长

  // ── 笔记 / 标注 ───────────────────────────────────────────────────────
  // 一条记录既是"标注"也是"笔记"：note 为空就是只划了重点。锚点是选中的原文，
  // 渲染时在当前页的词序列里按文本找回（重排换页也不至于错位，找不到就不画）。
  struct RdNote {
    std::string path;    // 书路径（笔记列表按它分组）
    std::string book;    // 书名（列表显示）
    int spine = 0;       // 章节
    int page = 0;        // 记下来时的页（跳转用，找不到再就近）—— 跨页标注记的是**起点页**
    // **跨页标注**的结束页（同一章内）。-1 = 老记录 / 单页标注：高亮走"整段文本在当页
    // 匹配"那条老路。>= page 时改用"起点页取尾部 / 终点页取头部 / 中间页整页"的拼法
    // （见 rdAnchorsOnPage），因为一段跨越几页的原文在**任何单独一页上都找不到**。
    int endPage = -1;
    std::string text;    // 选中的原文（跨页时是**整段**拼起来的全文）
    std::string note;    // 笔记正文（空 = 仅标注）
    int64_t time = 0;    // 记录时间
  };
  std::vector<RdNote> notes;
  int notesSel = 0;           // 当前笔记下标（详情页/搜索回填用；光标见 notesRowSel）
  int notesScroll = 0;
  bool noteDelArm = false;    // 笔记列表里删除的二次确认已就位
  int noteDelIdx = -1;
  // 笔记列表按书折叠。**存路径不存书名**：书名可能空、可能撞名（见 rdNoteRows）。
  // 空集合 = 全部折叠（默认态）。持久化在 g_settings 的 reader_notes_fold。
  std::set<std::string> notesOpen;
  int notesRowSel = 0;        // 行光标：**含书名分组行**，与 notesSel（笔记下标）分开

  // ── AI 对话（选段问答 / 普通聊天共用一屏）──────────────────────────────
  // 两个入口的唯一差别就是 aiCtx 空不空：非空 = 阅读页选区「问AI」，标题/system/正文抬头
  // 都带【选段】；(空) = 应用页「DeepSeek 对话」，纯聊天。选段原文**进入时就从调用方拷进来**：
  // g_pageText 是页内产物、换页就重建，晚了就没得拷。
  std::string aiCtx, aiInput, aiStatus;
  struct RdAiTurn {
    std::string q, a;
  };
  std::vector<RdAiTurn> aiTurns;      // **唯一真源**：多轮历史（有上限，见 screen_reader_ai.cpp）
  std::string aiTranscript;           // 由 aiTurns 合成的整篇文本（派生量，每次重建）
  std::vector<std::string> aiLines;   // 折行缓存（同 dictLines 那套）
  int aiLinesW = -1;                  // -1 = 缓存失效，需要重建
  int aiScroll = 0, aiScrollMax = 0, aiPageLines = 1;
  bool aiScrollToEnd = false;         // 下一帧渲染后滚到底（新提问/新回答落地）
  bool aiBusy = false;                // 请求在飞（除 Esc 外吞键）

  // 长按选中的词范围（当前页语言：words 下标）与浮层选择
  bool selActive = false;
  int selStart = 0, selEnd = 0;
  // "按字"粒度专用的**词内码点下标**（-1 = 整词条，即按句/按段落与所有老路径）。
  // 只在 selStart == selEnd 时有意义，表示"只选 words[selStart] 里的第 selCp 个码点"。
  // 中文一个词条常是"一个字 + 粘连的标点"（如 "好。"），所以只按词条框不出"好"这一个字。
  // 任何改变范围的路径（方向键/拖手柄/点选区内/重新锚定标注）都必须把它重置为 -1 ——
  // 否则会把上一次的码点下标套到一个新词条上，框到一半就断开。
  int selCp = -1;
  // 抓着哪个手柄（0=起点 1=终点 -1=没抓）。手柄画在行外，一次点按只能是"抓起来"，
  // 抓起来之后再点词，那一段才跳过去 —— 这样两端都能左右伸缩，而不是只有终点能动。
  int selGrab = -1;
  // 按住手柄拖动（KEY_TOUCH_DRAG）：selDrag = 正在被拖的那一端（0/1/-1=没在拖），
  // selDragX/Y 是拖动锚点（逻辑坐标，从被抓的那个手柄中心起算，逐帧加增量），
  // selDragActive 记"这一轮按下真的拖动过"——抬手那一下 hw/input 还会照常补一个
  // 方向键（拖到远处抬手 = 左右/上下滑），要让它把刚拖好的端点再挪一格。
  int selDrag = -1;
  int selDragX = 0, selDragY = 0;
  bool selDragActive = false;
  int selPopup = 0;        // 0 无浮层 1 未标注（标注/笔记/字典/取消） 2 已标注（删除/改笔记/字典）
  int selMenuSel = 0;
  int selNoteIdx = -1;     // 浮层对应的已有笔记下标（-1 = 新标注）
  int selPageSpine = -1;   // 选区所在章节（防止跨章误用）

  // ── 跨页选区（**限定在同一章 / 同一份 TXT 内**）──────────────────────────
  // 选区两端记的是绝对落点 (页, 页内词下标)：selLoPage/selLoWi ↔ selHiPage/selHiWi。
  // 上面那两个 selStart/selEnd 退化成它们在**当前页上的投影**，由 rdSelSyncEnds() 在每次
  // 渲染/按键前推出 —— 于是绘制、手柄、浮层、点词那些既有代码读的还是那两个下标，
  // 完全不必知道"跨页"这回事。
  // 页号口径与全仓别处一致：(bookKind == 0) ? st.page : st.txtPage（见 rdPosPage()）。
  // 不变量：selLoPage < selHiPage，或两页相同而 selLoWi <= selHiWi。
  int selLoPage = -1, selLoWi = 0;   // lo 端（起点）
  int selHiPage = -1, selHiWi = 0;   // hi 端（终点）
  // "拖出页边就能翻一页"这个动作的**待发位**：0 = 挂上了，非 0 = 刚翻过、还没重新挂上
  // （值本身是那次翻页的时刻，ms/esp_timer 口径，只用来写日志）。
  // 一次"拖到底"只翻一页：手指压在页脚上不动**不会**接着翻 —— 原来按时间冷却
  // （700ms）放行第二次，实际用起来是"手指在页脚停两秒吃掉三四页"，用户报"过选、
  // 选过页"。手指挪回正文栏里（或抬手重新拖一次）才重新挂上，见 rdSelDragStep。
  int64_t selEdgeMs = 0;
  // 从选区新建笔记时要把两端页号带到笔记编辑器那边去（那边已经离开阅读页，选区的
  // 状态已经没了）。见 rdSelActivate 的「笔记」分支与 rdNoteCommit。
  int notePendingPage = -1, notePendingEndPage = -1;

  // 笔记编辑器
  std::string noteEditBuf;
  int noteEditIdx = -1;          // >=0 改这条；-1 = 新建（原文在 notePendingText）
  std::string notePendingText;   // 新建笔记待写入的原文
  bool noteEditBackToDetail = false;  // 保存/取消后回笔记详情页（否则回正文）
  std::string noteStatus;

  // 笔记详情页：显示一条标注的原文全文 + 自己的注释。
  // 行表把"原文 / 小标题 / 注释"合成一张，共用一个滚动偏移（分两块滚就得判上下键归谁，
  // 而注释块又没有独立可视高度可言）；折行只在宽度或条目变了时重算一次（同词典页）。
  struct RdNoteDetailLine {
    std::string text;
    int kind = 0;   // 0=原文 1=小标题 2=注释 3=间隔
  };
  int noteDetailIdx = -1;              // 正在看的那条笔记（notes 下标）
  bool noteDetailFromSearch = false;   // 返回目标：true = 回 NotesSearch，false = 回 Notes
  bool noteDetailDelArm = false;       // 详情页删除的二次确认（与列表的 noteDelArm 独立）
  int noteDetailScroll = 0;
  int noteDetailScrollMax = 0;
  int noteDetailPageLines = 1;
  std::vector<RdNoteDetailLine> noteDetailLines;
  int noteDetailLinesW = -1;           // 建表时的排版宽度；-1 = 需重建（含换书/换字号）
  int noteDetailLinesIdx = -1;         // 建表时的 noteDetailIdx；对不上也要重建

  // ── 微信读书 ──────────────────────────────────────────────────────────
  // 复制一份原版的角色分工：状态机（WeReadClient::Operation）在 UI 循环里
  // 逐拍推进（step() 每次只做一小段，联网请求是同步阻塞的），UI 只读它暴露的
  // 进度/事件。登录、书架同步、整本缓存都走同一条驱动路径。
  std::unique_ptr<WeReadClient::Operation, WeOpDeleter> weOp;
  std::vector<WeReadStore::ShelfRecord> weShelf;
  bool weShelfLoaded = false;   // 书架是否已从 SD 读进内存
  int weSel = 0, weScroll = 0, weMenuSel = 0;
  std::string weQrUrl;          // 登录二维码内容
  std::string weStatus;         // 状态/错误提示
  std::string weJobTitle;       // 当前任务的书名
  int weKind = 0;               // 0=登录/书架 1=缓存整本 2=补封面 3=书籍详情 4=笔记/划线，判断完成事件
  bool weLoggedIn = false;      // 本地会话是否有效

  // 书籍详情页（RdMode::WereadDetail）：头部读一次摊进 weDetailHeader，简介按宽度折成
  // weDetailLines 缓存着滚（同笔记详情/词典页——逐帧重折会拖慢滚动）。
  WeReadStore::BookDetailHeader weDetailHeader = {};
  std::string weDetailIntro;                  // 头部之后那截简介（UTF-8，最多 64KB）
  bool weDetailLoaded = false;                // detail.bin 是否已读进来（没读进来就画"获取中"）
  int weDetailScroll = 0;                     // 简介滚动行偏移
  int weDetailLinesW = -1;                    // 建表时的排版宽度；-1 = 需重建
  std::vector<std::string> weDetailLines;     // 折好的简介行
  // 当前在看的这本书的 bookId —— 不再从书架选中项推。发现页（RdMode::WereadDiscover）
  // 也会进详情屏，那时这本多半根本不在书架上。
  std::string weDetailBookId;
  RdMode weDetailRet = RdMode::WereadMenu;    // 详情屏 Esc 回哪（书目菜单 / 发现页）

  // 微读笔记/划线页（RdMode::WereadNotes）：进来看本地 browse 缓存，没有就起一个 Browse
  // 任务（它一次抓 热门划线→我的划线→热门书评，且**只有 Complete 一个终止事件**，所以
  // 加载态是"正在获取…"而不是进度条）。当前 kind 的折行表缓存着滚，切 kind/换宽度才重建。
  bool weNotesLoaded = false;      // 缓存读进来了吗（没读进来就画加载态）
  bool weNotesCacheValid = false;  // 本地确实有可用的 browse 缓存
  int weNotesKind = 0;             // 0=热门划线 1=我的划线 2=热门书评
  int weNotesScroll = 0;           // 滚动行偏移
  int weNotesLinesW = -1;          // 建表时的排版宽度；-1 = 需重建
  int weNotesLinesKind = -1;       // 建表对应的 kind；变了也要重建
  uint32_t weNotesCounts[3] = {};  // 三类各自条数（tab 上显示）
  bool weNotesReviewsLimited = false;  // 书评被截到前 50 条（库里 kCacheReviewsLimited）
  // 一条笔记/划线/书评合成几行：头（章节名/作者）+ 正文折行 + 热度评分 + 间隔，
  // 全部共用一个滚动偏移（同笔记详情页）。
  struct RdWeNoteLine {
    std::string text;
    int kind = 0;   // 0=正文 1=条目头 2=热度/评分 3=间隔
  };
  std::vector<RdWeNoteLine> weNotesLines;  // 当前 kind 折好的行

  // 阅读进度同步页（RdMode::WereadSync）：本机进度取自 ReadingStats（不必打开书），
  // 云端进度由 Compare 那一拍回填。两端不同时给一个"用本机上传 / 用云端跳过去"的选择屏
  // ——库里没有任务内方向注入，只能选完再起第二拍（UploadLocal / ApplyRemote）。
  bool weSyncLoaded = false;        // 这一拍任务跑完了吗（没跑完画"核对中"）
  int weSyncStage = 0;              // 0=核对中 1=选方向 2=结果
  int weSyncSel = 0;                // 选择屏光标 0=用本机 1=用云端
  int weSyncLocalPercent = 0;       // 本机进度百分比
  int weSyncRemotePercent = 0;      // 云端进度百分比
  WeReadClient::ProgressSyncMode weSyncMode = WeReadClient::ProgressSyncMode::Compare;
  std::string weSyncStatus;         // 结果文案

  // ── 发现页（RdMode::WereadDiscover）──────────────────────────────────────
  // 官方 Skill/Gateway（i.weread.qq.com，API Key 在设置键 weread_api_key）的搜索与推荐。
  // 两个 tab，各自的结果各自留；跑请求是**同步阻塞**的（同微读其它请求），发起前先刷一帧
  // "正在搜索…"，回来再重画。结果项回车 → 动作菜单（看详情 / 缓存整本并阅读）。
  std::string weDiscQuery;        // 搜索关键字（vkTargetString 的编辑目标）
  int weDiscTab = 0;              // 0=搜索 1=推荐
  int weDiscSel = 0;              // 光标所在的**虚拟行**：搜索 tab 的第 0 行是查询行，
                                  // 结果从第 1 行起；推荐 tab 没有查询行，结果从第 0 行起
  bool weDiscBusy = false;        // 请求在飞（画"正在…"，也是"这次按键别再发一次"的闸）
  std::string weDiscStatus;       // 空结果 / 错误文案
  int weDiscMenuSel = 0;          // 动作菜单光标
  int weDiscMenuFor = -1;         // 动作菜单挂在第几条结果上（-1 = 菜单关着）
  bool weDiscSearchLoaded = false;  // 搜索 tab 拉过了吗（没拉过画"输入关键字后回车"）
  bool weDiscRecLoaded = false;     // 推荐 tab 拉过了吗（没拉过画"回车获取推荐"）
  std::vector<WereadSkillClient::SearchItem> weDiscSearch;       // 搜索 tab 的结果
  std::vector<WereadSkillClient::RecommendItem> weDiscRecommend; // 推荐 tab 的结果
  // 任务从发现页起的时候，结束后回发现页而不是书目菜单（weAfterJobMode 读它）。
  RdMode weJobRet = RdMode::WereadMenu;

  // 虚拟键盘
  bool vkVisible = false;

  // WiFi 管理（界面本体在 screen_reader_wifi.cpp，状态在那边的 g_wui 里）。
  // 这里只留 **VK/IME 胶水**要用的三个：vkTargetString 按 wifiField 选编辑目标、
  // rdTypingScreen 看 wifiEditing、rdSelfTickingProgress 看 wifiBusy。
  int wifiField = 0;      // 密码表单的光标：0 网络名 1 密码 2 连接
  bool wifiEditing = false;
  bool wifiBusy = false;
  std::string wifiSsidEdit;
  std::string wifiPassEdit;

  // 资源下载（词典清单地址 / 字体下载地址 / 下载字体）。这三项原本挂在写作模式的
  // 设置页（screen_settings.cpp 的「资源下载」分类）下，但下载出来的词典和字体都是
  // 给阅读用的，整类搬到阅读设置标签下，写作设置里不再出现。
  int resField = 0;  // 0 词典清单地址 1 字体下载地址 2 下载字体
  bool resEditing = false;
  bool resBusy = false;
  int resPct = 0;
  std::string resDictEdit;
  std::string resFontEdit;
  std::string resStatus;

  // OPDS 书库
  std::string opdsUrl;       // 当前目录地址（持久化键 opds_url）
  std::string opdsUrlEdit;   // 编辑中的地址
  std::vector<OpdsEntry> opdsEntries;
  std::vector<std::string> opdsStack;  // 上级目录地址栈（Esc 逐级返回）
  int opdsSel = 0;
  bool opdsEditing = false;
  bool opdsBusy = false;
  std::string opdsStatus;

  // WiFi 传书（复用 file_manager_server，浏览器上下载/上传）
  std::string netStatus;
  bool netBusy = false;
  bool netServerUp = false;
  int netSel = 0;

  // 词典下载（清单 → /sdcard/dictionaries/<id>/，与「词典」查询共用目录）
  DictCatalog dictCat;
  int dictDlSel = 0;
  bool dictDlBusy = false;
  std::string dictDlStatus;   // 列表页的状态行
  std::string dictDlDelArm;   // 待确认删除的 id（长按一次后置位）
  // 安装进度（dictDlBusy 时由 renderDictDl 走进度版面）
  std::string dictDlPhase;
  int dictDlFileIdx = 0, dictDlFileCount = 0;
  size_t dictDlDone = 0, dictDlTotal = 0;
  size_t dictDlFileGot = 0, dictDlFileTotal = 0;

  // BLE 按键映射
  int keyMapSel = 0;
  int keyMapCapture = -1;  // ≥0 = 正在等待按键的动作序号（-1 不在捕获）
  std::string keyMapStatus;

  // 自定义状态栏
  int sbSel = 0;

  // 关于页滚动位置
  int aboutTop = 0;

  // 灰阶自检页：选中动作（0..4，见 reader_refresh_test_present 的 which）+ 每个动作的
  // 上次耗时 ms（-1 = 没跑过）。自检页自己推屏，所以 renderCurrent 的推屏那一段要
  // 绕过；rtPushed 就是"本帧已经推过了"的一次性标志（照 s_vk_incr_ok 的先例）。
  int rtSel = 0;
  int rtMs[5] = {-1, -1, -1, -1, -1};
  int rtPending = -1;   // >= 0：这一帧不走走常规推屏，改用该动作的刷法自推（见 renderCurrent）

  // ── 阅读统计（第 5 个根标签）─────────────────────────────────────────
  int statsSel = 0;       // 主页交互列表的选中行
  int statsTop = 0;       // 子界面（更多详情/档案/调整）的滚动位置
  std::string statsBookPath;   // 子界面正在看的那本书（主页/日详情点进来的）
  uint32_t statsDay = 0;       // 热力图选中的日序号 / 日详情看的那一天
  int statsMonthY = 0, statsMonthM = 0;  // 热力图当前显示的月份（0 = 用参考日）
  std::string statsDelPath;    // 主页里长按删除已就位的书（二次确认；空 = 未就位）
  // 调整阅读时长（操作/日期/数量三个字段）
  int statsAdjField = 0;       // 0=操作 1=日期 2=数量
  int statsAdjOp = 0;          // 0=增加 1=减少
  int statsAdjAmt = 1;         // 下标 → 15/30/45/60 分钟
  uint32_t statsAdjDay = 0;
  bool statsAdjFailed = false; // 上一次应用失败（数量超过那天记录的数）
};

// ── 本模块的两个全局（定义在 screen_reader.cpp）─────────────────────────
extern GfxRenderer g_rd;
extern RdState st;

// ── 微信读书子应用（screen_reader_weread.cpp）───────────────────────────
// 四个渲染入口 + 四个按键入口，加一个每拍推进状态机的泵。主文件的分发只认这几个名字。
std::unique_ptr<WeReadClient::Operation, WeOpDeleter> weMakeOperation();

void renderWeread();
void renderWereadQr();
void renderWereadMenu();
void renderWereadDl();
void renderWereadDetail();
void renderWereadNotes();
void renderWereadSync();
void renderWereadDiscover();
void handleWeread(int key);
void handleWereadQr(int key);
void handleWereadMenu(int key);
void handleWereadDl(int key);
void handleWereadDetail(int key);
void handleWereadNotes(int key);
void handleWereadSync(int key);
void handleWereadDiscover(int key);
void weDrive();   // 推进一拍（联网请求同步阻塞，见 screen_reader_weread.cpp）

// 微读「重新获取封面」把 epub 在原地换掉之后调它：把这本书的封面产物（书架缩略图
// cover_v3.bmp、待机封面 standby_v5.bmp、以及内存里那份缩略图缓存）全部作废重建。
// 定义在 screen_reader.cpp（封面那套私有原语都在那儿）。
void rdRebuildBookCoverArtifacts(const std::string &path, int kind);

// ── 阅读器外壳的共享原语（定义在 screen_reader.cpp）──────────────────────
// 几何：顶栏底边 / 列表首行 / 页脚上沿 / 标签页底边。渲染与点按命中共用这几个值。
int rdHeadBottom();
int coverTop();      // == drawTitle / drawTabBar 的返回值
int statusTop();     // 底部提示栏（有提示行的子界面用）
int tabBottom();     // 一屏到底的标签页用
int footerH();
int uiLineHeight();
int uiAsc();
int uiFontId();      // 外壳字体 id（书内嵌字面在位时退回内置字体）
// 当前是不是"宽屏"（横屏 1216×684）。AI 对话的横屏版式按它分支；其余界面
// 仍按老版式走。用屏幕逻辑宽高判，不看 st.orientation —— 竖屏反过来也是同一回事。
bool rdWideLayout();

int clampI(int v, int lo, int hi);

// 画：一行文字（top 是文字上缘，不是基线）/ 居中一行 / 底栏提示 / 标题栏 / 标签栏
void drawLineText(int x, int top, const char *s, bool black = true, int fontId = uiFontId());
void drawCenteredLine(int top, const char *s, bool black = true);
void drawFooter(const char *hint);
int drawTitle(const char *title);
int drawTabBar();

// 标签栏命中与切换
int tabHit(int x, int y);
void switchTab(int tab);

// 平铺菜单（无窗口）的几何：渲染与点按共用
ListView flatMenuListView(int count, int sel, int top = coverTop());

// 动作：打开一本书 / 确保联网 / 重扫书架 / 整屏重绘
bool openBook(const std::string &path, int kind);
// 跳到全书的某百分比处（微读「用云端进度」用；内部就是 jumpToPercent，会把 mode 设成
// Reading）。要求书已经用 openBook 打开。
void rdApplyProgressPercent(int pct);
bool readerEnsureWifi(std::string &err);
// 阻塞式连接 + 屏上"正在连接… N 秒"进度反馈（连接最长 10 秒，不给反馈用户会以为死机）。
// 所有"先连 WiFi 再干活"的路径都该走它，不要直接调 g_wifi.connect()。
bool readerWifiConnect(const char *ssid, const char *pass);
void scanBooks();
void renderCurrent();

// 小工具：这几个原本是 screen_reader.cpp 的 file-static，拆出「统计」TU
// （screen_reader_stats.cpp）后它也要用，所以去掉 static 收在这里。
bool endsWith(const std::string &s, const char *suf);
std::string fitWidth(const std::string &s, int maxW);   // 按像素宽截断/加省略号
ListView flatListViewAt(int top, int itemH, int count, int sel);
void rdShowFloat(const std::string &msg, const std::string &sub, int ms);  // 瞬时浮动提示

// ── 「统计」标签（screen_reader_stats.cpp）───────────────────────────────
// 一个根标签 + 8 个子界面。主文件只在 renderCurrent 与按键分发里认这几个入口；
// rdStatsDate 另外给书架详情弹窗（renderShelfInfo）算"最近阅读"那一行用。
void renderStatsTab();
void handleStatsTab(int key);
void renderStatsBook();
void handleStatsBook(int key);
void renderStatsMore();
void handleStatsMore(int key);
void renderStatsHeatmap();
void handleStatsHeatmap(int key);
void renderStatsDay();
void handleStatsDay(int key);
void renderStatsProfile();
void handleStatsProfile(int key);
void renderStatsAdjust();
void handleStatsAdjust(int key);
void renderStatsSettings();
void handleStatsSettings(int key);
std::string rdStatsDate(uint32_t epoch);

// ── 「设置」标签下那一族叶子界面（screen_reader_subpages.cpp）──────────────
// 主文件只在分发（renderCurrent 的 case 与 screen_reader_handle 的 mode 分发）里认它们，
// 另外 doMenuAction（主文件）要调 dictDlLoadCatalog / opdsBeginLoad / netShareConnect；
// renderShelfInfo 后那条"最近阅读"用不上这几个。全部只出不进（没人回头调它们）。
// 「词典页」也是这一族：它由阅读菜单的"查词典"进（doMenuAction），只出不进。
void renderDict();
void rdDictResetScroll();   // 换了词条/改了字号：作废折行缓存并回到首行
void rdDictCacheClear();    // 词典文件变了（安装/删除）：作废查询缓存
// WiFi 管理（安卓式：开关 / 扫描 / 连接 / 已保存）。界面本体与界面态在
// screen_reader_wifi.cpp；主文件只认 renderWifi/handleWifi 两个入口，外加下面三个钩子：
//   rdWifiEnter     —— 从阅读菜单进本页（复位 + 起扫描），替代原来在 MenuAct::Wifi 里
//                      手写的那几行状态复位；
//   wifiCommitEdit  —— VK/IME 的回车提交（vkEnter 调）；
//   rdWifiIdleTick  —— 空闲帧推进（扫描结果落地 / 连接态变化），返回"该重绘"。
void renderWifi();
void handleWifi(int key);
void rdWifiEnter();
void wifiCommitEdit();
bool rdWifiIdleTick();
void opdsBeginLoad(const std::string &url);
void renderOpds();
void handleOpds(int key);
void dictDlLoadCatalog();
void renderDictDl();
void handleDictDl(int key);
void renderResDl();
void handleResDl(int key);
void renderKeyMap();
void handleKeyMap(int key);
void renderStatusBarSet();
void handleStatusBarSet(int key);
// 「状态栏」设置页在另一个 TU，开弹层（openRdPick，定义在 screen_reader.cpp）要转一手。
// act 用 kSbPickBase + 行号。同文件里的调用直接走 openRdPick，不绕这一层。
void rdOpenPick(int act);
void renderAbout();
void handleAbout(int key);
void renderRefreshTest();   // 灰阶自检页（16 级梯 / 抖动对比 / 各刷法耗时，自推屏）
void handleRefreshTest(int key);
void netShareConnect();
void renderNetShare();
void handleNetShare(int key);

// ── AI 对话（screen_reader_ai.cpp）────────────────────────────────────────
// **一个屏、两个入口**：阅读页选区动作条的「问AI」（ctx = 选中原文）与「应用」页的
// 「DeepSeek 对话」（ctx 空，纯聊天）。ctx 空不空只影响标题 / system prompt / 正文抬头。
// Esc 回哪由 from 决定（存进 st.retMode，回程按它三元）。
// 未配 DeepSeek Key 时 rdAiChatBegin 返回 false（弹提示、不进屏），调用方据此决定是否清选区。
bool rdAiChatBegin(const std::string &ctx, RdMode from);
void rdAiChatSubmit();      // vkEnter 的回车提交（多轮连问，**不收键盘**）
bool rdAiChatIdleTick();    // 空闲帧取件（返回是否该重绘）
void renderAiChat();
void handleAiChat(int key);

// 「这一拍别走虚拟键盘增量帧，把整页重画一遍」（实现在 screen_reader.cpp 的 s_vkFullDrawReq）。
// 异步内容（AI 的回答/错误）落地时由 aiPollResult 置位：增量帧的判据只看"键盘的目标串
// 变没变"，看不见上半屏别处发生的变化，不置这一位就会出现"答案到了、屏上还挂着
// 正在思考…"（而且 st.dirty 已被清，没有第二拍来补画）。
void rdRequestWholeRepaint();

// ── 阅读器的 VK / IME 条（留在 screen_reader.cpp）────────────────────────
// 「设置」子界面（还有文件管理那一族）要在自己的输入框上打字，共用这份。
void drawVk();
void rdToggleVk();
void rdDrawVkIcon();
bool rdVkIconHit(int x, int y);
bool rdVkWantShow();
// 词典 / AI 对话这两个"整屏都在等输入"的应用专用（其余输入框仍走 rdVkWantShow）：
// 蓝牙键盘连着时**默认不弹**虚拟键盘，把整屏让给释义/对话正文（2026-10-29 用户要求）。
//   rdTypingAppVkDefault —— 进屏那一刻定默认态：连着实体键盘就收起面板（并给一次
//                            "右下角图标可展开"的浮动提示），没连则等同 rdVkWantShow。
//   rdTypingAppTapInput —— 点输入框：连着实体键盘时不再弹面板，只留同一句提示；
//                            没连则等同 rdVkWantShow。
void rdTypingAppVkDefault();
void rdTypingAppTapInput();
void drawRdImeBar();
// fromVk = 这个字符是屏幕键盘点出来的（不是实体键盘敲的）：会撤掉 Win 键的「字母键选字」
// 待选，让那条功能只作用于实体键盘。物理键盘那几处调用点不传，默认 false。
void feedVkKey(int c, bool fromVk = false);
void feedVkBackspace();
void vkEnter();
void vkTap(int x, int y);
int vkVkTop();

// 杂项小工具（原来都是 file-static，被上面两个 TU 用到了）
int utf8Len(unsigned char c);
int rdBodyBottom();   // 正文可用底边（虚拟键盘 / 输入法条 / 提示行三选一）
void rdSyncVk();      // st.vkVisible → editor_vk 的可见性同步
bool rdImeBarOn();    // 实体键盘打字时的输入法条是否该占位
void rdShowBusy(const char *msg, const std::string &sub);   // 打开大书前刷的那一帧"正在…"
void renderNoteEdit();  // 笔记编辑页（重命名/改文件名共用同一个输入形态）

// ── 文件浏览器（screen_reader_files.cpp）──────────────────────────────────
// 文件浏览器 + 文件操作菜单 + 图片查看器。它是「应用」根标签（1 号位）下面的子界面：
// 三个入口（应用页的文件夹图标、书架菜单的「文件浏览」、恢复上次界面）都调
// rdEnterFileTab（扫盘 + st.tab=1 + mode=FileBrowser）；卡根按 Esc 退回应用页。
// fmTarget 给 fileMenuAction 之外没人用，但它是"文件菜单锁定那一项"的唯一取数口。
void fbScan(const std::string &dir);
void rdEnterFileTab();
bool rdNetTick();          // 传书服务器运行时的每拍推进（主循环空转也要调）；返回是否该重绘
bool rdFileScanIdleTick(); // 空闲帧取回异步扫描的结果（返回是否该重绘当前界面）
void renderFileBrowser();
void handleFileBrowser(int key);
const BookEntry *fmTarget();
void renderFileMenu();
void handleFileMenu(int key);
void renderFileRename();
void handleFileRename(int key);
void renderFileInfo();
void handleFileInfo(int key);
std::string humanSize(long long bytes);
void renderImage();
void handleImage(int key);
// 图片解码被按键中止的那一帧只有半张图，别推屏。定义随图片查看器走，renderCurrent
// （主文件）在推屏前要看一眼。
extern bool s_imgPresentSkipped;
int sbCount(const char *key, int def, int n);
int rdListTop();
int dictQueryH();
std::string rdNetXferText();
bool rdWifiEnsure();
void prepareQr();

// ── 当前页"文字地图"（长按选词/标注/脚注共用）─────────────────
// P3b：原来定义在 screen_reader.cpp，脚注引擎拆出后要跨 TU 共用，遂上移到本头。
// ── 笔记/标注：当前页的"文字地图" ────────────────────────────────────────
// 长按选词、标注高亮都要回答两个问题：屏幕上的某个点落在哪个词上、某段文字在
// 这一页的哪几个词上。这里把当前页的词连同像素坐标采下来，后面都在这张表上做。
// 采集时必须和 Page::render 用同一套 xOffset/yOffset/字重，否则高亮会画歪。
struct RdWordHit {
  int x = 0, w = 0, y = 0;   // 像素：起点、宽、基线
  // 与 EpdFontFamily::Style 同宽（uint16_t）。**别收窄成 uint8_t**：ALT_FONT(128) 塞得进，
  // 但 WAVY_UNDERLINE(256) / ALT2_FONT(512) 会被砍 —— 后果是 rdWarmPageText 永远读不到
  // ALT2 位（第三家族的字形从不预热），命中框宽度也会按错的面量。
  uint16_t style = 0;
  // 这个词条用的**实际字体 id**（标题/正文/脚注可能不是同一个面，见 rdBuildPageText 里
  // 的 lineFontId）。存下来是为了"按字"能把词条拆成单个码点的子矩形：量前缀宽必须跟
  // 渲染同一个面同一个样式，否则字宽会和实际错开。从前只有绘制时才拿它，现在数据层也留一份。
  int fontId = 0;
  std::string text;
};

struct RdPageText {
  std::vector<RdWordHit> words;   // 阅读顺序：行序 → 行内词序
  std::vector<int> lineFirst;     // 每行首个词的下标，末尾补 words.size()
  // 每行的**基线** y（与"行"一一对应：长度 = lineFirst.size() - 1，末尾**不**补）。
  // 为什么不让阅读线自己从词表推：空行（TXT 的空行、段距补出来的那一行）没有任何词，
  // lineFirst 对它只能给出首尾相等，事后反推只能靠"上一行 + 标称行距"——连着两个空行
  // 或者空行后面跟着段距就反推不出来。排版那一刻这个 y 是现成的（PageLine::yPos 加上
  // ascender / TXT 的 y 推进），顺手存一份最省事也最准。
  std::vector<int> lineBase;
  // 每个词条所属的**段落块**编号（与 words 等长）。EPUB 用 Block 的同一性，TXT 用空行/
  // 首行缩进切。只被"按段落"选字用；其它粒度不看它。空 vector = 这一页建不出来段落归属
  // （老路径/异常），此时按段落退回"只选点中的那一个词条"。
  std::vector<int> wordBlock;
  bool valid = false;
};

// ── 正文脚注引擎（screen_reader_footnotes.cpp）───────────────────────────────
// P3b：从 screen_reader.cpp 拆出的脚注一整套。主文件的分发（handleReading / 点按
// 那条路）与 renderCurrent 只认下面这些入口；跨 TU 用到的阅读器原语也一并声明在这里。
bool loadCurrentFootnotes();
std::optional<Section::AnchorPos> rdFindFootnotePage(const std::string &anchor, int64_t budgetUs = 2500 * 1000);
void jumpToFootnote(int idx);
void footnoteReturn();
void rdRememberNoteRef(const std::string &num, int spine, int page);
bool rdGotoNoteRef(const std::string &num);
void rdLeadingNoteMarker(const std::string &text, std::string *out);
bool rdStartsBracketed(const std::string &text);
bool rdHeadBracketClosed(const std::string &text);
bool rdOpenFootnote(int idx, bool allowBuild);
void rdFootnoteWaitCancel();
void rdFootnoteWaitTick();
void rdLinkWaitCancel();
void rdLinkWaitTick();
void drawFootnotePopup();
void closeFootnotePopup();

// 脚注引擎跨 TU 用到的阅读器原语（定义仍在 screen_reader.cpp）
void applyNightMode();
void buildToPage(int target);
bool openSpine(int idx);
// 跨文件弹注为**目标节**另开一份 Section（与 openSpine 同一套 spec 与字号梯子），
// **不碰 st.section**（见 RdState::fnWaitAltSpine）。失败返回 nullptr；调用方持有它，
// 排到锚点就把正文取出来，然后放掉 —— 析构会自动 suspendBuild（把已排的页落成
// partial），所以半途放掉不丢活。
std::unique_ptr<Section> rdMakeAltSection(int spine);
// 跳到**别的节**里的一个锚点（href 是带文件名的跨文件引用，如 notes.xhtml#fn1）。
// 命中就开过去落页，还没排过就开过去 + 挂给空闲帧（rdLinkWaitTick）；返回点在两支里
// 都记好。返回 false = 这个 href 指不到任何**别的**节。点跨文件链接与脚注的"→"共用。
bool rdJumpAcrossSpine(const std::string &href, const std::string &anchor);
std::string rdNormalizeNoteNumber(const std::string &s);
void rdOverlayRefresh();
int rdUtf8Len(unsigned char c);

extern RdPageText g_pageText;   // 当前排版页（脚注引擎读它）
