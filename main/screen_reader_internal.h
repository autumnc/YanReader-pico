#pragma once
// ── 阅读模式内部共享层（screen_reader_internal.h）────────────────────────
// **不是对外接口**——对外只有 screen_reader.h 那几个函数（init/exit/handle + 待机封面
// 的两个取数口）。这一份是阅读模式**自己人**用的：阅读器外壳的状态量（RdState）、
// 外壳字体/排版几何的原语、几个跨子界面的动作。
//
// 存在的唯一理由是 screen_reader.cpp 太大（12343 行、561KB），按子应用拆文件时要有个
// 地方放"大家都要用的那部分"。第一个拆出去的是微信读书（screen_reader_weread.cpp）：
// 它要读 st.we*、要画标题栏/底栏、要 switchTab/openBook。见 docs/架构评审-2026-10-04.md 的 P3b。
//
// 拆的规矩（照着往下搬时别走样）：
//   · 状态量留在 RdState 里**一份**，不按子应用切分 —— 它们互相纠缠（微读下载完要
//     重扫书架并改 st.mode、设置页条目也要切 st.mode），切开会切出两套真相。
//   · 共享原语在这里只放**声明**，定义仍在 screen_reader.cpp（那些原本 static 的
//     符号因此去掉了 static）。不改成函数指针表：调用点原地不动才是这个重构的价值。
//   · 新拆一个子应用：把它的 render/handle 搬走，在这里补它用到的共享符号。

#include <cstdint>
#include <memory>
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

inline constexpr int MARGIN = 30;    // 正文/列表左右边距
inline constexpr int RD_TOP_INSET = 28;    // 顶部留白(标题起始)，原 8，整体下移 20px
inline constexpr int RD_BODY_TOP = 50;     // 正文起始，原 MARGIN(30)，整体下移 20px
inline constexpr int RD_BOTTOM_INSET = 4;  // 底部留白(状态栏下方)，原 24，状态栏下移 20px


// ── 阅读器状态 ──────────────────────────────────────────────────────────
enum class RdMode {
  Browser, Reading, Toc, Menu, LayoutMenu, Bookmarks, Footnotes, Percent, Qr, Dictionary, Weread, Wifi,
  ShelfMenu, ShelfInfo, Recent, FileBrowser, Image, Opds, NetShare, DictDl, ResDl, KeyMap, StatusBar, About,
  Settings, Notes, NoteEdit,
  WereadQr,    // 扫码登录
  WereadMenu,  // 选中书目的操作菜单
  WereadDl,    // 缓存进度
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
  RefreshTest   // 灰阶自检页（16 级梯 / 细线 / 抖动三档对比 / 各刷法耗时，自推屏）
};

// 主界面五个根标签：0=书架 1=文件 2=笔记 3=设置 4=统计。标签栏只画图标（tab_icons.h），
// 文字标签下屏——微信读书原本占 1 号位，现已挪进「设置」标签的条目表（MenuAct::Weread），
// 腾出来的位置给 SD 卡文件浏览器（就是原来的 RdMode::FileBrowser，现在直接当标签用）。
// 统计放最后一位：st.tab == 1/3 的判断遍布各处，插在中间要动的地方多。
inline constexpr int kTabCount = 5;
// 4 号位（统计）用 0 当哨兵：drawTabBar 见到 0 就走程序化的柱状图图标，不走字体。
// 理由见 drawStatsTabIcon —— 引进真字形要重裁 NF-Propo 子集，而当前环境没有 fontTools。
inline constexpr uint32_t kTabIcons[kTabCount] = {TAB_ICON_BOOKSHELF, TAB_ICON_FILES,
                                                  TAB_ICON_NOTES, TAB_ICON_SETTINGS, 0};

inline RdMode tabMode(int tab) {
  return tab == 1 ? RdMode::FileBrowser : tab == 2 ? RdMode::Notes
         : tab == 3 ? RdMode::Settings : tab == 4 ? RdMode::Stats : RdMode::Browser;
}

struct BookEntry {
  std::string path;
  std::string name;
  int kind;  // 0=epub 1=txt 2=xtc
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

  // 主界面根标签（书架/微读/设置）与「设置」标签的选中行
  int tab = 0;
  int setSel = 0;

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

  // 书内嵌字体：非空 = 内容面此刻装的是从当前这本书里解出来的字体（指向缓存目录里的
  // 落盘副本）。bookFontTag 跟着它进版式缓存键（见 ReaderRenderSpec::fontTag）；
  // 0 表示内容面是用户/内建字体。两者都在 openBook() 里换书时清掉。
  std::string bookFontLocal;
  uint32_t bookFontTag = 0;
  // 书内 CSS 的**次家族**（祖堂集：正文宋体、注文/引文仿宋）。跟 bookFontLocal 同生共死：
  // 换书/退出阅读/SD 挂起时一起关（次字面一份就占 ~1MB PSRAM，留着就是漏）。
  // 空 = 这本书没有可用的第二个家族，此时 g_rd 的次家族哈希是 0，排版不会给任何词
  // 打 ALT_FONT，绘制完全等价于次字面没打开。
  std::string bookFontAltLocal;
  uint32_t bookFontAltTag = 0;

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
  int menuSel = 0;
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
  int bookmarkSel = 0;

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
  int readingLine = 0;                      // 阅读线：0 无 1 虚线 2 点线 3 实线（正文行间引导线）
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
    int page = 0;        // 记下来时的页（跳转用，找不到再就近）
    std::string text;    // 选中的原文
    std::string note;    // 笔记正文（空 = 仅标注）
    int64_t time = 0;    // 记录时间
  };
  std::vector<RdNote> notes;
  int notesSel = 0;
  int notesScroll = 0;
  bool noteDelArm = false;    // 笔记列表里删除的二次确认已就位
  int noteDelIdx = -1;

  // 长按选中的词范围（当前页语言：words 下标）与浮层选择
  bool selActive = false;
  int selStart = 0, selEnd = 0;
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

  // 笔记编辑器
  std::string noteEditBuf;
  int noteEditIdx = -1;          // >=0 改这条；-1 = 新建（原文在 notePendingText）
  std::string notePendingText;   // 新建笔记待写入的原文
  std::string noteStatus;

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
  int weKind = 0;               // 0=登录/书架 1=缓存整本，用于判断完成事件
  bool weLoggedIn = false;      // 本地会话是否有效

  // 虚拟键盘
  bool vkVisible = false;

  // WiFi 管理
  int wifiField = 0;      // 0 SSID 1 密码 2 连接 3 断开
  bool wifiEditing = false;
  bool wifiBusy = false;
  std::string wifiSsidEdit;
  std::string wifiPassEdit;
  std::string wifiStatus;

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
void handleWeread(int key);
void handleWereadQr(int key);
void handleWereadMenu(int key);
void handleWereadDl(int key);
void weDrive();   // 推进一拍（联网请求同步阻塞，见 screen_reader_weread.cpp）

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
bool readerEnsureWifi(std::string &err);
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
void renderWifi();
void handleWifi(int key);
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
void renderAbout();
void handleAbout(int key);
void renderRefreshTest();   // 灰阶自检页（16 级梯 / 抖动对比 / 各刷法耗时，自推屏）
void handleRefreshTest(int key);
void netShareConnect();
void renderNetShare();
void handleNetShare(int key);

// ── 阅读器的 VK / IME 条（留在 screen_reader.cpp）────────────────────────
// 「设置」子界面（还有文件管理那一族）要在自己的输入框上打字，共用这份。
void drawVk();
void rdToggleVk();
void rdDrawVkIcon();
bool rdVkIconHit(int x, int y);
bool rdVkWantShow();
void drawRdImeBar();
void feedVkKey(int c);
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

// ── 「文件」根标签（screen_reader_files.cpp）─────────────────────────────
// 文件浏览器 + 文件操作菜单 + 图片查看器。主文件在 switchTab（进文件标签要先扫盘）、
// renderCurrent、按键分发里认它们；阅读菜单的"打开文件"用 rdEnterFileTab。
// fmTarget 给 fileMenuAction 之外没人用，但它是"文件菜单锁定那一项"的唯一取数口。
void fbScan(const std::string &dir);
void rdEnterFileTab();
void rdNetTick();          // 传书服务器运行时的每拍推进（主循环空转也要调）
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
  uint8_t style = 0;
  std::string text;
};

struct RdPageText {
  std::vector<RdWordHit> words;   // 阅读顺序：行序 → 行内词序
  std::vector<int> lineFirst;     // 每行首个词的下标，末尾补 words.size()
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
void drawFootnotePopup();
void closeFootnotePopup();

// 脚注引擎跨 TU 用到的阅读器原语（定义仍在 screen_reader.cpp）
void applyNightMode();
void buildToPage(int target);
bool openSpine(int idx);
std::string rdNormalizeNoteNumber(const std::string &s);
void rdOverlayRefresh();
int rdUtf8Len(unsigned char c);

extern RdPageText g_pageText;   // 当前排版页（脚注引擎读它）
