// screen_reader.cpp — 阅读模式（阅读器 + 微信读书）移植。
//
// 用 read_pico 官方固件的 ui 库与底层驱动（epdiy 4bpp framebuffer + ttf_font），
// 逻辑层复用 crossmux 的 lib/（Epub/Txt/Xtc/Section/Page/GfxRenderer/HalDisplay/
// HalStorage/Dictionary）。本模块只重建阅读器的 UI 外壳：书架、翻页、目录、
// 阅读设定、词典、虚拟键盘（接系统拼音输入法）。
//
// 注意：本 TU 不能 #include read_pico 的 epdiy.h —— 它会连带 epd_internals.h，
// 其中 typedef 的 EpdGlyph/EpdUnicodeInterval/EpdFont 与 crossmux 的
// EpdFontData.h/EpdFont.h（EpdFont 是 class）冲突。逻辑尺寸用 HalDisplay 的
// getDisplayWidth/Height（内部已调 epd_rotated_*），字体度量经 GfxRenderer 取得。

#include "screen_reader.h"
#include "reader_page_turn.h"  // 揭页提示：只有裸声明，不会拖进 epdiy.h
#include "ui_render.h"   // ui_render_drain：进阅读器前等在飞的 UI 推屏收尾

#include <cmath>  // 阅读档案的雷达图（cos/sin）

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <HalDisplay.h>
#include <HalStorage.h>
#include <GfxRenderer.h>
#include <EpdFont.h>
#include <EpdFontData.h>
#include <EpdFontFamily.h>
#include <Epub.h>
#include <Section.h>
#include <Page.h>
#include <ReaderRenderSpec.h>
#include <FirstLineIndent.h>
#include <Txt.h>
#include <TxtEncoding.h>
#include <TxtParagraph.h>
#include <Xtc.h>
#include <Dictionary.h>
#include <Bitmap.h>
#include <ImageBlock.h>
#include <ImageDecoderFactory.h>

#include "qrcodegen.h"

#include "bt_keyboard.h"
#include "editor_vk.h"  // 虚拟键盘：写作/计划/阅读三个模式共用同一套（drawVk/vkTap 是薄适配）
#include "icon_font.h"  // 标签栏/搜索入口图标（NF-Propo 子集）
#include "tab_icons.h"
#include "reading_stats.h"  // 阅读统计数据层（计时/落盘/查询），界面见文件末尾统计区
#include "settings_manager.h"
#include "settings.h"  // app_settings_set_font_path

// 字体模块的最小外部声明。不能直接 #include "font/ttf_font.h"：它会 #include epdiy.h，
// 连带 epd_internals.h 的 typedef EpdFont 与本 TU 里 crossmux 的 EpdFont class 冲突
// （同本文件开头的说明）。这里只重复声明用到的几个接口与条目结构。
extern "C" {
#define RD_TTF_FONT_NAME_MAX 64
#define RD_TTF_FONT_PATH_MAX 160
typedef struct {
  char name[RD_TTF_FONT_NAME_MAX];
  char path[RD_TTF_FONT_PATH_MAX];
} rd_ttf_font_item_t;
int ttf_font_scan(void);
const rd_ttf_font_item_t *ttf_font_item(int index);
bool ttf_font_path_is_builtin(const char *path);
bool ttf_font_is_builtin(void);
const char *ttf_font_path(void);
const char *ttf_font_display_name(void);
int ttf_font_open(const char *path);
int ttf_font_open_builtin(void);
// 字形缓存的埋点（ttf_font.c 里本来就有，只是没人调用过）。结构体布局与
// ttf_font.h 的 ttf_bench_stats_t 逐字段一致——这里不能直接 include 那个头
// （会和 crossmux 的 EpdFont class 撞名，见本文件开头），所以照抄一份。
typedef struct {
  uint32_t glyphs;
  uint32_t hits;
  uint32_t misses;
  int64_t read_us;
  int64_t raster_us;
  int64_t total_us;
  int64_t seek_us;
  uint32_t read_calls;
  uint32_t read_bytes;
  uint32_t io_blocks;
  uint32_t io_runs;
  uint32_t io_span_min;
  uint32_t io_span_max;
  uint32_t cache_kb;
  uint32_t cache_cap_kb;
} rd_ttf_bench_t;
void ttf_bench_begin(void);
void ttf_bench_end(rd_ttf_bench_t *out);
// 整页预取：把这一页所有字的轮廓块一次读进块缓存，避免逐词预取时
// 每次只能看见一个词、把随机小读摊到整页。ttf_font.h 里的角色枚举值。
#define RD_TTF_ROLE_CONTENT 0
#define RD_TTF_ROLE_CONTENT_ALT 2
void ttf_set_role(int role);
void ttf_warm_text_px(int pixel_height, const char *text);
int ttf_font_open_alt(const char *path);
void ttf_font_close_alt(void);
bool ttf_font_alt_ready(void);
int ttf_get_role(void);
}
#include "wifi_manager.h"
#include "opds_client.h"
#include "dictionary_store.h"
#include "ble_keymap.h"
#include "pcf85063.h"  // g_rtc：状态栏时钟
#include "file_manager_server.h"
#include "standby_clock.h"
#include "clipboard.h"  // 选区「复制」：跨模式粘贴板（写作/阅读/计划共享一份）
#include "ime/IME.h"
#include "hw/input.h"
#include "hw/board_reader.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <algorithm>
#include <cctype>  // tolower：书架去重键按全小写路径折叠（FATFS 大小写不敏感）
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>  // strcasecmp（下载字体的 .ttf 后缀判断）
#include <memory>
#include <new>
#include <set>
#include <string>
#include <vector>

// 微信读书：整库移植自 crossmux 的 WeReadWebApi（登录/书架/章节下载/EPUB 打包）。
#include <WeReadClient.h>
#include <WeReadStore.h>

static const char *TAG = "Reader";

// Operation 约 8KB（内含 4KB 收发缓冲），内部 RAM 紧张，优先放 PSRAM。
struct WeOpDeleter {
  void operator()(WeReadClient::Operation *op) const {
    if (!op) return;
    std::destroy_at(op);
    heap_caps_free(op);
  }
};
static std::unique_ptr<WeReadClient::Operation, WeOpDeleter> weMakeOperation() {
  void *raw = heap_caps_malloc(sizeof(WeReadClient::Operation), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!raw) raw = heap_caps_malloc(sizeof(WeReadClient::Operation), MALLOC_CAP_8BIT);
  if (!raw) return nullptr;
  return std::unique_ptr<WeReadClient::Operation, WeOpDeleter>(new (raw) WeReadClient::Operation());
}

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


static const char *CACHE_DIR = "/sdcard/.crossmux";
static const char *DICT_ROOT = "/sdcard/dictionaries";

// 正文字号表（ttf 像素高，即 EpdFontData::advanceY）。前 kUserFontLevels 档菜单可选，
// 末尾两档是**标题专用**——CSS 阶梯的向上余量，菜单里选不到，保证正文选到最大档（64）时
// 标题仍能再大 1~2 档。全部注册为字体 id 1..N（dummy EpdFontData，不额外占内存；
// 字形缓存按字节计，clamp_px 上限 120 够用）。
// 26/30/34px 三档已删（实测太小、没法读）。注意这会把**档位下标整体左移**：老
// reader_font_level 存的是 9 档表的下标，直接沿用会静默换大小 —— 见 screen_reader_init()
// 里的 _v2 键迁移。
static const int kBodyPx[6] = {40, 46, 54, 64, 76, 88};
static const int kBodyPxCount = 6;
static const int kUserFontLevels = 4;  // 可选档位数：0..3 = 40..64px
static const int kDefaultFontLevel = 1;  // 46px（删档前的默认 34px 已不存在）
static const float kLineSpacings[5] = {1.0f, 1.2f, 1.4f, 1.6f, 1.8f};
static const float kDefaultLineSpacing = 1.2f;
// 段间距 6 档（0=关）。数值语义同 EPUB 引擎的 extraParagraphSpacing：
// "每段之后额外留 0.5/0.75/1/1.25/1.5 倍行高"。参与 Section 排版缓存键，改档会自动重排。
static const char *kParaSpacingLabels[6] = {"关", "0.5x", "0.75x", "1x", "1.25x", "1.5x"};

// 正文左右边距三档（窄/标准/宽）。
static const int kMargins[3] = {20, 30, 45};
static const int kDefaultMarginIdx = 1;

static const int MARGIN = 30;    // 正文/列表左右边距
static const int RD_TOP_INSET = 28;    // 顶部留白(标题起始)，原 8，整体下移 20px
static const int RD_BODY_TOP = 50;     // 正文起始，原 MARGIN(30)，整体下移 20px
static const int RD_BOTTOM_INSET = 4;  // 底部留白(状态栏下方)，原 24，状态栏下移 20px

// ── 小工具 ──────────────────────────────────────────────────────────────
static int clampI(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int utf8Len(unsigned char c) {
  if (c < 0x80) return 1;
  if (c < 0xE0) return 2;
  if (c < 0xF0) return 3;
  return 4;
}
static bool endsWith(const std::string &s, const char *suf) {
  size_t n = strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

// ── 字体：dummy EpdFontData 仅 advanceY/ascender 生效 ────────────────────
// GfxRenderer 的 getTextWidth/drawText/getLineHeight 只读 advanceY（经
// ttfPixelHeightFor），getFontAscenderSize 读 ascender；glyph/bitmap/intervals
// 对 ttf 渲染路径从不使用。所以一个只填 advanceY+ascender 的空 EpdFontData 即可
// 驱动 ttf_font 光栅化。
struct RdFont {
  EpdFontData data{};
  EpdFont font{&data};
  EpdFontFamily family{&font};
  void set(int advanceY, int ascender) {
    data = EpdFontData{};
    data.advanceY = static_cast<uint8_t>(advanceY);
    data.ascender = ascender;
  }
};

static GfxRenderer g_rd(display);
static RdFont g_uiFont;       // id 0
static RdFont g_bodyFont[9];  // id 1..9

// ── 阅读器状态 ──────────────────────────────────────────────────────────
enum class RdMode {
  Browser, Reading, Toc, Menu, Bookmarks, Footnotes, Percent, Qr, Dictionary, Weread, Wifi,
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
  StatsSettings// 统计设置（每日目标）
};

// 主界面五个根标签：0=书架 1=文件 2=笔记 3=设置 4=统计。标签栏只画图标（tab_icons.h），
// 文字标签下屏——微信读书原本占 1 号位，现已挪进「设置」标签的条目表（MenuAct::Weread），
// 腾出来的位置给 SD 卡文件浏览器（就是原来的 RdMode::FileBrowser，现在直接当标签用）。
// 统计放最后一位：st.tab == 1/3 的判断遍布各处，插在中间要动的地方多。
static const int kTabCount = 5;
// 4 号位（统计）用 0 当哨兵：drawTabBar 见到 0 就走程序化的柱状图图标，不走字体。
// 理由见 drawStatsTabIcon —— 引进真字形要重裁 NF-Propo 子集，而当前环境没有 fontTools。
static const uint32_t kTabIcons[kTabCount] = {TAB_ICON_BOOKSHELF, TAB_ICON_FILES,
                                              TAB_ICON_NOTES, TAB_ICON_SETTINGS, 0};
static RdMode tabMode(int tab) {
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
  bool night = false;                       // 夜间反色
  std::string orientation = "landscape";    // 阅读器方向

  // 词典
  Dictionary dict;
  bool dictOpen = false;
  std::string dictQuery;
  std::string dictResult;
  std::string dictHeadword;
  std::string dictStatus;

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

static RdState st;

// 阅读器**外壳**（菜单/对话框/列表/状态栏/按钮/文件管理器…）用的字体 id。
//
// 平时是 uiFontId() —— 走内容面拿用户所选字体的字形，繁体/生僻字才不是豆腐块。
// 但「书内嵌字体」模式会把**内容面**换成这本书自己的字面，而那种字面是按本书正文子集化
// 出来的：正文里的字它都有，外壳的字（目录/书签/排版/返回/第 N 页…）它多半没有 ——
// 一开菜单就是一排缺字。所以内嵌字面在位时，外壳退回内置 builtin.ttf（7710 字，
// 与设置/GTD/写作等其它界面同一套），内嵌字面只负责正文。
// 判据用 st.bookFontLocal：它非空 ⇔ 内容面此刻装的正是这本书的字面（装载成功才赋值）。
static int uiFontId() { return st.bookFontLocal.empty() ? CONTENT_UI_FONT_ID : UI_FONT_ID; }

// 把当前行距吸附到 kLineSpacings 下标（容差匹配，避免 atof 浮点误差）。
static int spacingIdx() {
  float v = st.lineSpacing;
  int best = 1;
  float bd = 1e9f;
  for (int i = 0; i < 5; i++) {
    float d = v - kLineSpacings[i];
    if (d < 0) d = -d;
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}

static int uiLineHeight() { return g_rd.getLineHeight(UI_FONT_ID); }
static int uiAsc() { return g_rd.getFontAscenderSize(UI_FONT_ID); }

// 底部状态栏/提示栏高度与上边界。留 RD_BOTTOM_INSET 底部物理留白，
// 避免提示文字贴到屏幕底边被边框遮挡（原版固定 44px 时文字基线落到底边被截断）。
static int footerH() { return uiLineHeight() + 8; }
static int statusTop() { return g_rd.getScreenHeight() - footerH() - RD_BOTTOM_INSET; }

// 根标签页（书架/文件/笔记/设置/统计）**不再画底部提示栏**，内容一直排到物理底边。
// 阅读页的状态带、目录/菜单/词典的提示行都还在用 statusTop()——那是各子界面的排版基准，
// 全局改它会连环炸，所以这里只给标签页单开一个底边，两者互不影响。
static int tabBottom() { return g_rd.getScreenHeight() - RD_BOTTOM_INSET; }

// 顶栏底边（分隔线所在）。文字标题栏和图标标签栏共用同一条线，所以 coverTop()
// 只有一个值，两种顶栏可以互换着用。取两者里更靠下的那个：标签图标（56px）比
// UI 行高（42px）大，靠这条把正文起点一起往下让，图标才不会被分隔线切到。
static int rdHeadBottom() {
  return std::max(RD_TOP_INSET + uiLineHeight() + 6, TAB_BAND_BOTTOM);
}

// 以 top 为文字上缘画一行 UI 文本（drawText 的 y 是基线）。
static void drawLineText(int x, int top, const char *s, bool black = true,
                         int fontId = uiFontId()) {
  g_rd.drawText(fontId, x, top + g_rd.getFontAscenderSize(fontId), s, black);
}

// 居中一行 UI 文本。
static void drawCenteredLine(int top, const char *s, bool black = true) {
  int x = (g_rd.getScreenWidth() - g_rd.getTextWidth(uiFontId(), s)) / 2;
  drawLineText(x, top, s, black);
}

static std::string fitWidth(const std::string &s, int maxW);  // 定义在文件后段
static std::string humanSize(long long bytes);                // 同上（文件浏览器要用来报进度）

// 打开大书前先刷一帧"正在…"（定义在 renderCurrent 之后）。最近阅读/文件浏览器这两个
// 打开入口都在文件前段，所以声明要放在这里。
static void rdShowBusy(const char *msg, const std::string &sub);

// 瞬时浮动提示：居中黑底反白框，ms 毫秒后自己消失。只置状态 + 标脏，不立刻重绘
// （调用点都在按键处理里，随后 renderCurrent 那一趟就画出来了）。
static void rdShowFloat(const std::string &msg, const std::string &sub, int ms);

// 「设置」标签的弹层选择：openRdPick 由 doMenuAction 的轮换条目调（那些条目现在
// 不再原地循环，改成弹层里挑），drawSettingPicker 由 renderCurrent 的浮层段画，
// 按键由 screen_reader_handle 在分发前先喂给 handleSettingPicker。三处都在文件后段。
static void openRdPick(int act);
static void drawSettingPicker();
static void handleSettingPicker(int key);

// 「想要虚拟键盘」的统一入口：点输入框、进编辑态、按确认键想唤出键盘，全走这里。
//
// 蓝牙键盘连着的时候按设计**不弹**虚拟键盘（免得白挡半屏正文），原来那些位置写的是
// 光秃秃的 `st.vkVisible = !g_bt.isConnected();` —— 于是"点输入框一点反应都没有"。
// 用户看到的只有键盘弹不出来，看不出原因，也没人知道状态栏右端那个图标能强制打开。
// 这里在"没弹"的时候给一条浮动提示，把原因和出路说清楚；提示只出一次（蓝牙断开再
// 连上会重新计一次），不要每点一下都冒出来。
//
// 返回 true = 键盘已展开（调用方标脏重绘即可）；false = 蓝牙键盘连着，没弹。
static bool rdVkWantShow() {
  static bool hinted = false;
  if (!g_bt.isConnected()) {
    hinted = false;
    st.vkVisible = true;
    return true;
  }
  if (!hinted) {
    hinted = true;
    rdShowFloat("蓝牙键盘已连接", "虚拟键盘不自动弹出；点状态栏右端的键盘图标可强制打开", 5000);
  }
  return false;
}

// 底部提示行。
// 右端要给虚拟键盘的开关图标让位（editorVkDrawIcon 画在同一行的最右端）。短提示照旧
// 整屏居中——文字在中间，本来就够不到极右端的图标；只有长到真会压上去时，才按让位后
// 的宽度截断并在这段里重新居中，免得白截掉半句话。
static void drawFooter(const char *hint) {
  int w = g_rd.getScreenWidth();
  int y = statusTop();
  g_rd.drawLine(0, y, w, y, true);
  const int slot = editorVkIconSlotW();
  int avail = w - 2 * MARGIN;
  std::string s = fitWidth(hint, avail);
  int tw = g_rd.getTextWidth(uiFontId(), s.c_str());
  int x = (w - tw) / 2;
  if (slot > 0 && x + tw > w - slot) {
    avail = w - 2 * MARGIN - slot;
    s = fitWidth(hint, avail);
    tw = g_rd.getTextWidth(uiFontId(), s.c_str());
    x = MARGIN + (avail - tw) / 2;
  }
  drawLineText(x, y + 4, s.c_str(), true);
}

// 顶部标题栏，返回正文可用 top。
static int drawTitle(const char *title) {
  int y = RD_TOP_INSET;
  drawLineText(MARGIN, y, title, true);
  g_rd.drawLine(0, rdHeadBottom(), g_rd.getScreenWidth(), rdHeadBottom(), true);
  return rdHeadBottom() + 8;
}

// 统计标签的图标：三根高低不同的竖条（柱状图）。没有对应的字体码点，直接画。
// 走真字形要重跑 scripts/subset_icon_font.py 重裁 NF-Propo.ttf，而当前环境没有
// fontTools，还得踩 ICON_MAX_PX 那条"超限静默丢字形"的坑——几十行代码更划算，
// 思路和 icon_font.c 里 U+25D0/U+25B8 那几个几何字形一样。
// invert = 活动标签（黑底反白）。
static void drawStatsTabIcon(int x, int y, int box, bool invert) {
  const int barW = box / 5;
  const int gap = (box - 3 * barW) / 2;
  const int heights[3] = {box * 45 / 100, box * 72 / 100, box};
  for (int i = 0; i < 3; i++) {
    const int bh = heights[i];
    g_rd.fillRect(x + i * (barW + gap), y + box - bh, barW, bh, !invert);
  }
}

// ── 主界面标签栏（书架 / 文件 / 笔记 / 设置 / 统计）──────────────────────
// 刻意与 drawTitle 占用同一条顶栏、返回同一个正文 top（都取 rdHeadBottom()），
// 标签化不需要另做一套内容区排版。
static int drawTabBar() {
  int w = g_rd.getScreenWidth();
  int seg = w / kTabCount;
  // 图标尺寸/离顶边距离走 tab_icons.h 的共用常量，和计划模式完全一致；底边交给
  // rdHeadBottom()，coverTop() 等既有版式跟着它一起走（见那边的注释）。
  uint8_t *fb = g_rd.getFrameBuffer();
  const int box = TAB_ICON_PX + 2 * TAB_BOX_PAD;
  const int boxY = TAB_ICON_INSET - TAB_BOX_PAD;
  for (int i = 0; i < kTabCount; i++) {
    if (i == st.tab) {
      // 活动标签：黑底 + 反白图标。原来是一条下划线，图标放大到 56px 之后字形
      // 下沿到分隔线只剩几个像素，塞不下；改成和计划模式同一套黑底，顺带把
      // 两个模式的观感统一。
      g_rd.fillRect(i * seg + (seg - box) / 2, boxY, box, box, true);
    }
    const int ix = i * seg + (seg - TAB_ICON_PX) / 2;
    if (kTabIcons[i] == 0) {
      drawStatsTabIcon(ix, TAB_ICON_INSET, TAB_ICON_PX, i == st.tab);
    } else if (fb) {
      icon_font_draw_sized(fb, ix, TAB_ICON_INSET, TAB_ICON_PX, TAB_ICON_PX, kTabIcons[i],
                           i == st.tab, TAB_ICON_PX);
    }
  }
  g_rd.drawLine(0, rdHeadBottom(), w, rdHeadBottom(), true);
  return rdHeadBottom() + 8;
}

// 点按坐标落在哪个标签上（不在标签栏内返回 -1）。命中带故意不跟着字形走：
// 从屏幕上沿一路给到标签栏下方一点，整条横带按等分列切——e-ink 上手指落点很糙，
// 用户瞄的是大概位置，不是那个 56px 的方块。
static int tabHit(int x, int y) {
  int w = g_rd.getScreenWidth();
  if (y < 0 || y > TAB_BAND_BOTTOM + 6) return -1;
  int i = (x < 0) ? 0 : (x >= w ? kTabCount - 1 : x * kTabCount / w);
  return (i >= 0 && i < kTabCount) ? i : -1;
}

// ── 搜索入口（书架 / 笔记）──────────────────────────────────────────────
// 搜索入口（两个列表页共用）。定义在文件后段，这里先声名，因为书架菜单要用。
static void rdEnterSearch(RdMode m);
static void renderShelfSearch();
static void renderNotesSearch();
static void handleShelfSearch(int key);
static void handleNotesSearch(int key);
// 文件标签的进入动作（重扫目录）。定义在文件浏览器那一段，这里先声明给 switchTab 用。
static void rdEnterFileTab();

// 切根标签。文件标签进来要额外扫一遍 SD（见 rdEnterFileTab），所以它单独走一条分支。
static void switchTab(int tab) {
  tab = clampI(tab, 0, kTabCount - 1);
  if (tab == st.tab && st.mode == tabMode(tab)) return;
  st.pickOpen = false;   // 切标签一定收起设置弹层（防它被带进别的标签）
  if (tab == 1) { rdEnterFileTab(); return; }
  st.tab = tab;
  st.mode = tabMode(tab);
  st.fullRefresh = true;
  st.dirty = 1;
}

// ── 渲染规格 ────────────────────────────────────────────────────────────
static int bodyMargin() { return kMargins[st.marginIdx]; }

// 段落对齐档位 → CssTextAlign 取值（见 blocks/BlockStyle.h::fromCssStyle）。
// None 表示"不覆盖，按书籍 CSS 的 text-align 来"；其余强制覆盖全书。
// 注意 paragraphAlignment 参与 Section 的排版缓存键，改档位会自动重排。
static const uint8_t kAlignSpec[4] = {
    0,  // Justify（两端对齐）
    1,  // Left
    2,  // Center
    4,  // None → 书籍样式
};
static const char *kAlignLabels[4] = {"两端", "左", "居中", "书籍样式"};
// 阅读线（正文行间引导线）的四种线型。0 就是关，菜单标签直接用它。
// 新加的「实线」排在末位，是为了让老设置里存着的 0/1/2 含义不变。
static const char *kReadingLineNames[4] = {"无", "虚线", "点线", "实线"};

// ── 样式解析：书籍内嵌 / 强制指定（设置 → 样式解析）────────────────────────
// 书籍内嵌（默认）= 按书里 CSS 排：对齐、缩进、字号都听书的（用户在这三项上的
//   选择退居其次，"对齐/缩进"两档显示成"随书"）。
// 强制指定 = 完全忽略书内 CSS，全按阅读器设置排。引擎在 embeddedStyle=false 时直接
//   把 cssParser 置空（见 ChapterHtmlSlimParser.h），class= 与 style="" 一概不解析；
//   HTML 标签语义（<b>/<i>/<h1> 的粗斜体）不受影响，那些走标签分支。
static const char *kStyleSrcKeys[] = {"embedded", "fixed"};
static const char *kStyleSrcNames[] = {"书籍内嵌", "强制指定"};
static const int kStyleSrcCount = 2;
static int styleSource() {
  const std::string k = g_settings.getString("reader_style_source", "embedded");
  for (int i = 0; i < kStyleSrcCount; i++) {
    if (k == kStyleSrcKeys[i]) return i;
  }
  return 0;  // 书籍内嵌
}
static bool styleEmbedded() { return styleSource() == 0; }

// ── 内嵌字体：随书 / 关闭（设置 → 内嵌字体）────────────────────────────────
// 随书（默认）= 书里 @font-face 标了、并且被 body/html 规则点名的字体，读这本书时
//   装到内容面；换书/退出自动还原用户字体。关闭 = 永远用用户字体（老行为）。
// 只认"整本书一个家族"这一层：按元素混排要同时驻留多个字体面（每面一套字形缓存
// 加 128+32KB io 缓冲），而字体面是全局单例、不能嵌套，先不做。
static const char *kEmbFontKeys[] = {"on", "off"};
static const char *kEmbFontNames[] = {"随书", "关闭"};
static const int kEmbFontCount = 2;
static int embeddedFontMode() {
  const std::string k = g_settings.getString("reader_embedded_font", "on");
  for (int i = 0; i < kEmbFontCount; i++) {
    if (k == kEmbFontKeys[i]) return i;
  }
  return 0;  // 随书
}
static bool embeddedFontEnabled() { return embeddedFontMode() == 0; }

// 版式缓存键用的字面身份。FNV-1a 而不是 std::hash：后者不保证跨版本稳定，
// 换了实现就会把一整批 .bin 白白判废。
static uint32_t fontTagFor(const std::string &localPath, size_t size) {
  uint32_t h = 2166136261u;
  const std::string key = localPath + "#" + std::to_string(size);
  for (unsigned char c : key) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

// 把内容面切回用户的全局字体（没选过就是内建）。TXT/XTC 没有书内字体、退出阅读器
// 也要还回去 —— 否则写作模式里呈现的还是上一本书的字面。ttf_font_open(""/NULL)
// 就是内建，不用另判；打开失败时 ttf_font_open 自己会退回上一个字体。
// 内容面已经是目标字面时直接返回：reopenBook() 每次改字号都会走到这里，而
// ttf_font_open 是"先卸面再重读"，大 CJK 字体一次就是几百毫秒。
static void applyUserContentFont() {
  const char *path = app_settings_font_path();
  if (ttf_font_path_is_builtin(path)) {
    if (ttf_font_is_builtin()) return;
  } else if (strcmp(ttf_font_path(), path) == 0) {
    return;
  }
  if (ttf_font_open(path) != 0) {
    ESP_LOGW(TAG, "用户字体打开失败，回落内建: %s", (path && *path) ? path : "(builtin)");
    (void)ttf_font_open_builtin();
  }
}

// ── 次字面（书内 CSS 的第二个家族）────────────────────────────────────────
// 祖堂集：正文家族 st(宋体)，注文/引文家族 fs(仿宋)。次字面是**独立**于内容面的一份
// 字面：装不上不影响正文，只是那些段落照旧用正文字面画。它占 PSRAM（一份 CJK 字面
// 常驻 ~1MB），所以换书/退出阅读必须连同内容面一起还回去 —— 见 releaseBookFonts()。
//
// 装载成功后把家族名哈希灌进 g_rd：排版期 ChapterHtmlSlimParser 拿它跟每个元素算出来的
// CSS 家族哈希比，相等就给这个词打 EpdFontFamily::ALT_FONT，绘制时切到次字面。
// 没装成则灌 0（0 不是任何家族名的哈希）—— 恒不相等，整本书一个字都不走这条路。
static void loadAltEmbeddedFont(const Epub::EmbeddedFont &alt) {
  ttf_font_close_alt();
  st.bookFontAltLocal.clear();
  g_rd.setAltFontFamilyHash(0);
  st.bookFontAltTag = 0;
  if (alt.itemHref.empty()) return;  // 这本书只有一个家族（绝大多数书）
  const std::string local = st.epub ? st.epub->extractEmbeddedFont(alt, "book_alt.ttf") : std::string();
  if (local.empty() || ttf_font_open_alt(local.c_str()) != 0) {
    ESP_LOGW(TAG, "次家族装载失败，注文/引文回落正文字面: %s",
             local.empty() ? alt.itemHref.c_str() : local.c_str());
    return;
  }
  st.bookFontAltLocal = local;
  // 这里只需要一个"变没变"的指纹：哈希本身跟着家族名走，家族名没变就没必要重排。
  st.bookFontAltTag = fontTagFor(local, alt.size);
  g_rd.setAltFontFamilyHash(CssParser::fontFamilyHash(alt.family));
  ESP_LOGI(TAG, "次家族: '%s' → %s", alt.family.c_str(), local.c_str());
}

// 内容面 + 次字面一起还回去。任何清 st.bookFontLocal 的地方都必须走这里，否则次字面
// 那份 ~1MB 会一直挂在 PSRAM 上（换一本书才发现，且表现为"内存莫名其妙少了 1MB"）。
static void releaseBookFonts() {
  ttf_font_close_alt();
  st.bookFontAltLocal.clear();
  st.bookFontAltTag = 0;
  g_rd.setAltFontFamilyHash(0);
}

static ReaderRenderSpec makeSpec() {
  ReaderRenderSpec spec;
  int w = g_rd.getScreenWidth();
  spec.fontId = BODY_FONT_ID_BASE + st.fontLevel;
  // fontId 只挑字号，挑不出字面：换字面（用户字体 ↔ 书内字体）后 fontId 可能一字不变，
  // 不带上 fontTag 就会拿另一套字面排出来的旧 .bin 直接显示。次字面同理：它的哈希
  // 只在装了次字面时才非零，装/不装会改变每个词的 ALT_FONT 位，也就改变版式。
  spec.fontTag = st.bookFontTag ^ st.bookFontAltTag;
  spec.lineCompression = st.lineSpacing;
  spec.extraParagraphSpacing = static_cast<uint8_t>(clampI(st.paraSpacing, 0, 5));
  // 内嵌模式下让对齐/缩进也随书（引擎的 None/Auto 就是"不覆盖书籍样式"）；
  // 强制指定模式下这两项才由使用者的档位说了算。
  const bool embedded = styleEmbedded();
  spec.firstLineIndent = embedded ? 0 : static_cast<uint8_t>(st.indentMode);
  spec.paragraphAlignment = embedded ? static_cast<uint8_t>(CssTextAlign::None) : kAlignSpec[clampI(st.alignMode, 0, 3)];
  spec.viewportWidth = w - 2 * bodyMargin();
  spec.viewportHeight = statusTop() - RD_BODY_TOP - MARGIN;
  spec.hyphenationEnabled = false;
  spec.embeddedStyle = embedded;
  spec.imageRendering = 0;
  spec.focusReadingEnabled = false;
  // 打开每页的链接矩形表（Page::links，带 href + 坐标）。弹注/回跳从此按链接 id 走：
  // 正文注号 → href `#note_1`，注文行首的 ※ → href `#noteref_1`，都是书里写死的，不用猜。
  // 代价：这项进了 section 文件头并参与缓存比对（Section.cpp:181/240），**旧排版缓存全部
  // 作废，每本书首次打开重排一次**；之后每页最多多存 32 条矩形（264 B 一条，只有含链的页占）。
  spec.collectTouchLinks = true;
  return spec;
}

// CSS 字号梯子：书里写了 font-size 时把倍率吸附到的正文字号 id（5 个比例桶各一个）。
// 梯子以用户当前的字号档 L 为基准：id[档] = 1 + clamp(L + 档 - 2, 0, kBodyPxCount-1)。
// 上限放到全表 9 档（而不是用户可选的 7 档）：正文选到最大档 64px 时，标题（比例桶 ≥1.4）
// 仍能落到 76/88 两档，不会被夹成与正文同号。9 档字体在 screen_reader_init() 里本来就全部
// 注册着（id 1..9），按块选字号不额外占内存。
// 强制指定模式下全填 0 = 关闭（引擎不解析 CSS，本来也用不上）。
static void applyCssFontLadder() {
  const int L = clampI(st.fontLevel, 0, kUserFontLevels - 1);
  int8_t ids[5];
  if (!styleEmbedded()) {
    for (auto &id : ids) id = 0;
  } else {
    for (int step = 0; step < 5; step++) {
      ids[step] = static_cast<int8_t>(1 + clampI(L + step - 2, 0, kBodyPxCount - 1));
    }
  }
  g_rd.setCssFontLadder(ids);
  // 临时诊断（字号锚定）：梯子到底灌了什么、每个 id 对应多少 px。正文 id = 1+L 这一档
  // 就是"用户选的字号"，书里 1em 的块必须落到它上面。
  {
    char px[160] = {0};
    for (int i = 0; i < 5; i++) {
      char t[32];
      snprintf(t, sizeof(t), "%s%d:%.0fpx", i == 0 ? "" : " ", (int)ids[i],
               (double)g_rd.getLineHeight(ids[i]));
      strncat(px, t, sizeof(px) - strlen(px) - 1);
    }
    ESP_LOGW(TAG, "字号锚定: 档L=%d 正文id=%d 梯子[%d,%d,%d,%d,%d] px[%s] 内嵌解析=%d",
             L, BODY_FONT_ID_BASE + L, (int)ids[0], (int)ids[1], (int)ids[2], (int)ids[3],
             (int)ids[4], px, (int)styleEmbedded());
  }
}

static int linesPerPage() {
  int lh = static_cast<int>(g_rd.getLineHeight(BODY_FONT_ID_BASE + st.fontLevel) * st.lineSpacing + 0.5f);
  int usable = statusTop() - RD_BODY_TOP - MARGIN;
  return std::max(1, usable / lh);
}

static int totalPages() {
  if (st.bookKind == 0) return st.section ? static_cast<int>(st.section->estimatedTotalPages()) : 1;
  if (st.bookKind == 1) {
    int lpp = linesPerPage();
    return std::max(1, static_cast<int>((st.txtLineStarts.size() + lpp - 1) / lpp));
  }
  if (st.bookKind == 2) return st.xtc ? static_cast<int>(st.xtc->getPageCount()) : 1;
  return 1;
}

// ── 书架扫描 ────────────────────────────────────────────────────────────
// 扫描时装作看不见的文件：应用自己的数据也是 .txt，落在 /sdcard 根目录下会被
// 当成一本书摆上书架（"阅读进度"reader_progress.txt 就这么上架的）。
//   reader_progress.txt  阅读位置记忆（见 kProgressPath）
//   reader_notes.txt     阅读笔记（见 kNotesPath）
//   N.txt (0..9)         快捷编辑的草稿（quick_edit.cpp 的 quickEditFilePath）
// 只按文件名判、不改这些文件的落盘位置：位置一挪，老设备上的记录就找不回来了。
static bool isInternalShelfFile(const std::string &name) {
  if (name == "reader_progress.txt" || name == "reader_notes.txt") return true;
  if (name.size() == 5 && name[1] == '.' && name[0] >= '0' && name[0] <= '9' &&
      name.compare(2, 3, "txt") == 0)
    return true;  // 0.txt..9.txt：快捷编辑草稿，不是书
  return false;
}

static void scanBooks() {
  st.books.clear();
  // /sdcard/WeRead 是微信读书缓存整本后的落盘目录（finalBookPath 前缀）。
  // books/Books 两种大小写都列是因为卡上的目录名不统一；但 FATFS 的 LFN 查找是
  // 大小写不敏感的（没有 CONFIG_FATFS_CASE_SENSITIVE），两者打开的是**同一个**目录，
  // 条目名也一模一样，只是拼出来的路径字符串不同 → 同一本书在书架上变成两行。
  // 所以去重的键必须是**全小写路径**：/sdcard/books/X.epub 与 /sdcard/Books/X.epub
  // 折叠成同一个键，而 /sdcard/X.epub 与 /sdcard/books/X.epub（不同文件）仍是两个键。
  const char *dirs[] = {"/sdcard", "/sdcard/books", "/sdcard/Books", "/sdcard/WeRead", nullptr};
  std::set<std::string> seen;
  for (int d = 0; dirs[d]; d++) {
    DIR *dp = opendir(dirs[d]);
    if (!dp) continue;
    struct dirent *e;
    while ((e = readdir(dp)) != nullptr) {
      std::string name = e->d_name;
      if (name == "." || name == "..") continue;
      if (isInternalShelfFile(name)) continue;  // 应用自己的数据文件，不是书
      std::string full = std::string(dirs[d]) + "/" + name;
      int kind = -1;
      if (endsWith(name, ".epub")) kind = 0;
      else if (endsWith(name, ".txt")) kind = 1;
      else if (endsWith(name, ".xtc")) kind = 2;
      if (kind < 0) continue;
      std::string key = full;
      for (char &c : key) c = (char)tolower((unsigned char)c);
      if (!seen.insert(key).second) continue;  // 同一个文件（books/Books 是同一个目录）
      st.books.push_back({full, name, kind});
    }
    closedir(dp);
  }
}

// ── 打开书籍 ────────────────────────────────────────────────────────────
static bool openSpine(int idx);

static bool openEpub(const std::string &path) {
  auto epub = std::make_shared<Epub>(path, CACHE_DIR);
  if (!epub->load()) {
    // 这本书没打开：内容面可能还装着上一本的内嵌字体（openBook 已经把
    // st.bookFontLocal 清了，两者必须一致），还回用户字体。
    applyUserContentFont();
    return false;
  }
  st.epub = epub;
  // 内联插图是**延迟提取**的：解析章节时只读图片头部拿尺寸，真正的图片数据要等第一
  // 次翻到那一页才从 zip 里抠出来（ImageBlock::ensureExtracted）。抠的那一步走的是
  // 这里注册的回调 —— 不注册的话它判失败直接画空框，而且**不打任何日志**（那条分支
  // 只 rememberImageFailure），表现就是"书里所有插图都只剩一个框"。crossmux 在加载
  // EPUB 的同一位置做同样的事。
  // ctx 是裸指针：Epub 由 st.epub 持有，换书时先换回调和 st.epub 再销毁旧的，
  // 退出阅读器时清掉（见 screen_reader_exit）。
  ImageBlock::clearSessionRenderFailures();
  ImageBlock::setExtractor(epub.get(), [](void *ctx, const char *src, const char *dest) {
    return static_cast<Epub *>(ctx)->extractItemToFile(src, dest);
  });
  st.bookTitle = epub->getTitle().empty() ? path : epub->getTitle();

  // 书内嵌字体：装到内容面。reopenBook() 会重入这里（改字号/行距/边距都走它），
  // 那时内容面已经装着这本书的字面，st.bookFontLocal 非空即为"同一本书、字面已就位"，
  // 不能再解压+重读一遍 SD（大字体一次几百毫秒）；换书时 openBook() 会先清空它。
  if (st.bookFontLocal.empty()) {
    bool loaded = false;
    if (embeddedFontEnabled()) {
      const Epub::EmbeddedFontSet fonts = epub->resolveEmbeddedFonts();
      const Epub::EmbeddedFont &font = fonts.primary;
      const std::string local = font.itemHref.empty() ? std::string() : epub->extractEmbeddedFont(font);
      if (!local.empty() && ttf_font_open(local.c_str()) == 0) {
        st.bookFontLocal = local;
        st.bookFontTag = fontTagFor(local, font.size);
        loaded = true;
        ESP_LOGI(TAG, "内嵌字体: '%s' → %s", font.family.c_str(), local.c_str());
      } else if (!font.itemHref.empty()) {
        ESP_LOGW(TAG, "内嵌字体装载失败，回落用户字体: %s",
                 local.empty() ? font.itemHref.c_str() : local.c_str());
      } else {
        // 空 itemHref = 这本书没有我们认得的正文家族（或者声明的字体根本不在归档里）。
        // 这条路以前完全静默，"内嵌字体没生效"就只能靠猜，所以补一行 INFO。
        ESP_LOGI(TAG, "内嵌字体: 本书没有可用的正文家族声明，用用户字体");
      }
      loadAltEmbeddedFont(fonts.alt);
    } else {
      ESP_LOGI(TAG, "内嵌字体: 设置=关闭，用用户字体");
    }
    // 没有内嵌字体（绝大多数书）、装载失败、或用户在设置里关掉了：内容面必须是
    // 用户字体。这一支也是"上一本书的内嵌字体"唯一的还原点。
    if (!loaded) applyUserContentFont();
  }

  st.spineIndex = 0;
  st.page = 0;
  return openSpine(0);
}

static bool openSpine(int idx) {
  if (!st.epub) return false;
  int n = st.epub->getSpineItemsCount();
  if (n <= 0) return false;
  idx = clampI(idx, 0, n - 1);
  st.spineIndex = idx;
  st.section = std::make_unique<Section>(st.epub, idx, g_rd);
  // 字号梯子必须在 startBuild 之前灌：排版期就要按它把 CSS font-size 吸附到某一档。
  applyCssFontLadder();
  if (!st.section->startBuild(makeSpec())) return false;
  st.page = 0;
  st.section->buildSomeMore(2);  // 先排出前几页，立即可读
  return true;
}

static size_t measureTxtLine(const std::string &t, size_t start, int fontId, int maxW) {
  size_t i = start;
  std::string acc;
  acc.reserve(64);
  while (i < t.size()) {
    unsigned char c = static_cast<unsigned char>(t[i]);
    if (c == '\n') return i + 1;  // 换行结束本行
    int clen = utf8Len(c);
    if (i + clen > t.size()) break;
    acc.append(t.data() + i, clen);
    if (g_rd.getTextWidth(fontId, acc.c_str()) > maxW) return i;  // 溢出：本字符起换行
    i += clen;
  }
  return i;
}

// ── 段间距（TXT 侧）─────────────────────────────────────────────────────
// 与 EPUB 的 extraParagraphSpacing 同一语义（0=关，1..5 = 0.5/0.75/1/1.25/1.5 行高），
// 但 TXT 的行高是整行的、分页表也按行记，所以只能按整行补空行，粒度到"行"。
static const int kTxtParaGapRows[6] = {0, 1, 1, 2, 2, 3};

// 当前行是不是一个段落的最后一行：非空，且下一行是空行 / 缩进行 / 章节标题行。
// 判定偏保守 —— 中文 TXT 靠"全角空格缩进"或空行分段；一条条硬折行的文件没有这些
// 特征，就不补（补了会满屏空行）。
static bool isTxtParagraphEnd(const std::string &t, size_t lineStart, size_t lineEnd) {
  const auto kindAt = [&t](size_t start, size_t end) {
    while (end > start && (t[end - 1] == '\n' || t[end - 1] == '\r')) --end;
    return txt_paragraph::analyzeLine(reinterpret_cast<const uint8_t *>(t.data()) + start, end - start).kind;
  };
  if (kindAt(lineStart, lineEnd) == txt_paragraph::LineKind::Blank) return false;
  if (lineEnd >= t.size()) return false;
  size_t nextEnd = t.find('\n', lineEnd);
  if (nextEnd == std::string::npos) nextEnd = t.size();
  const auto nextKind = kindAt(lineEnd, nextEnd);
  if (nextKind == txt_paragraph::LineKind::Blank || nextKind == txt_paragraph::LineKind::Indented) return true;
  return std::binary_search(st.txtChapterOffsets.begin(), st.txtChapterOffsets.end(), lineEnd);
}

static void paginateTxt() {
  st.txtLineStarts.clear();
  const std::string &t = st.txtUtf8;
  if (t.empty()) return;
  int fontId = BODY_FONT_ID_BASE + st.fontLevel;
  int maxW = g_rd.getScreenWidth() - 2 * bodyMargin();
  const int gapRows = kTxtParaGapRows[clampI(st.paraSpacing, 0, 5)];
  st.txtLineStarts.push_back(0);
  size_t pos = 0;
  while (pos < t.size()) {
    size_t next = measureTxtLine(t, pos, fontId, maxW);
    if (next <= pos) next = pos + 1;
    if (next > t.size()) next = t.size();
    st.txtLineStarts.push_back(next);
    // 段后补空行：重复同一个偏移 → 渲染时是空行（le==ls），不会重复文字。
    // 分页仍是"每行一格"的整齐表（页 = 行索引 / lpp），书签/百分比都不用改。
    if (gapRows > 0 && next < t.size() && isTxtParagraphEnd(t, pos, next)) {
      for (int i = 0; i < gapRows; i++) st.txtLineStarts.push_back(next);
    }
    pos = next;
  }
}

// 扫一遍正文挑章节标题行，填 txtChapterOffsets/Titles。8MB 上限下这是一次线性扫描。
static void buildTxtChapters() {
  st.txtChapterOffsets.clear();
  st.txtChapterTitles.clear();
  const std::string &t = st.txtUtf8;
  size_t pos = 0;
  while (pos < t.size()) {
    size_t end = t.find('\n', pos);
    if (end == std::string::npos) end = t.size();
    auto title = txt_chapter_index::chapterTitle(std::string_view(t.data() + pos, end - pos));
    if (!title.empty()) {
      st.txtChapterOffsets.push_back(pos);
      st.txtChapterTitles.emplace_back(title);
    }
    if (end >= t.size()) break;
    pos = end + 1;
  }
}

// 章节字节偏移 → 页码。分页表里每行一个起点，标题行自己就是一行起点（标题不会被
// 折行拆开），所以偏移必然命中表里某一项。
static int txtPageForOffset(size_t offset) {
  if (st.txtLineStarts.empty()) return 0;
  auto it = std::lower_bound(st.txtLineStarts.begin(), st.txtLineStarts.end(), offset);
  size_t line = static_cast<size_t>(it - st.txtLineStarts.begin());
  if (line >= st.txtLineStarts.size()) line = st.txtLineStarts.size() - 1;
  return static_cast<int>(line / static_cast<size_t>(linesPerPage()));
}

// 进目录时把选中行定位到"当前页所属的那一章"。几千章的 TXT 没有这个会很难用。
static void selectTxtChapterForCurrentPage() {
  st.tocSel = 0;
  if (st.txtChapterOffsets.empty() || st.txtLineStarts.empty()) return;
  size_t line = static_cast<size_t>(std::max(0, st.txtPage)) * static_cast<size_t>(linesPerPage());
  if (line >= st.txtLineStarts.size()) line = st.txtLineStarts.size() - 1;
  auto it = std::upper_bound(st.txtChapterOffsets.begin(), st.txtChapterOffsets.end(), st.txtLineStarts[line]);
  st.tocSel = (it == st.txtChapterOffsets.begin()) ? 0 : static_cast<int>(it - st.txtChapterOffsets.begin() - 1);
}

static bool openTxt(const std::string &path) {
  applyUserContentFont();  // TXT 没有书内字体；也可能是从带内嵌字体的 EPUB 换过来的
  Txt txt(path, CACHE_DIR);
  if (!txt.load()) return false;
  size_t fsize = txt.getFileSize();
  if (fsize == 0 || fsize > 8 * 1024 * 1024) return false;  // 首版上限 8MB
  // 峰值：工作缓冲 2×f（GBK→UTF8 最多翻倍）+ 行表 ~0.4×f。本工程 C++ 异常是**关闭**的，
  // std::string 分配失败不是抛异常而是 abort —— 也就是重启。所以宁可在这里判"放不下"、
  // 干净地返回 false，也不能让它分配下去再说。标称的 8MB 上限在只有 ~2.6MB PSRAM 的机器上
  // 根本达不到：真正能开的书受这条判据限制（约 800KB 上下）。
  const size_t cap = fsize * 2 + 64;  // 2× 容纳 GBK→UTF8 扩张
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < cap + fsize + 65536) {
    ESP_LOGE(TAG, "TXT %s too large: need ~%u B, PSRAM free %u B", path.c_str(),
             (unsigned)(cap + fsize + 65536),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return false;
  }
  // 直接读进正文缓冲（原来先读进一个临时 vector 再 assign 一份，峰值 4×f）。读入和
  // 原地转码都在这块上做，转完 resize 收口——比原来多开一倍的余量。
  st.txtUtf8.resize(cap);
  if (!txt.readContent(reinterpret_cast<uint8_t *>(st.txtUtf8.data()), 0, fsize)) {
    st.txtUtf8.clear();
    return false;
  }
  txt_encoding::Encoding enc =
      txt_encoding::detect(reinterpret_cast<uint8_t *>(st.txtUtf8.data()), fsize, true);
  size_t ulen = fsize;
  if (enc == txt_encoding::Encoding::Gbk) {
    auto r = txt_encoding::transcodeGbkInPlace(reinterpret_cast<uint8_t *>(st.txtUtf8.data()),
                                               fsize, cap, true);
    ulen = r.utf8Length;
  }
  st.txtUtf8.resize(ulen);
  st.bookTitle = txt.getTitle().empty() ? path : txt.getTitle();
  st.txtPage = 0;
  buildTxtChapters();  // 先扫章节：paginateTxt 要拿它判断"段末"（章节标题前留空行）
  paginateTxt();
  return true;
}

static bool openXtc(const std::string &path) {
  applyUserContentFont();  // 同上：XTC 也没有书内字体
  auto xtc = std::make_unique<Xtc>(path, CACHE_DIR);
  if (!xtc->load()) return false;
  st.xtc = std::move(xtc);
  st.bookTitle = st.xtc->getTitle().empty() ? path : st.xtc->getTitle();
  st.xtcPage = 0;
  return true;
}

static void generateCoverForOpenedBook();
static void generateStandbyCoverForOpenedBook();   // 定义在 generateCoverForOpenedBook 之后
static void pushRecent(const std::string &path, int kind, const std::string &title);
// 阅读位置落盘/恢复：定义在 buildToPage 之后（恢复要靠它跳页），这里先声明。
static void rdRememberProgress(bool force);
static void rdRestoreProgress();
static void rdStatsBeginSession();  // 定义在 chapterPage() 之后（要用到章节进度）
static void buildToPage(int target);  // 同上：reopenBook 重排后要靠它跳回原页

static bool openBook(const std::string &path, int kind) {
  // 换书之前先把上一本读到哪落盘：下面 openEpub/openTxt 会把书对象整个换掉，
  // 换完 st.spineIndex/st.page 就属于新书了，想记也没得记。
  if (!st.bookPath.empty() && st.bookPath != path) rdRememberProgress(true);
  st.bookKind = kind;
  st.bookPath = path;
  // 换书 = 上一本的内嵌字体作废。放在这里（而不是 openEpub 里）是因为 openEpub 也会
  // 被 reopenBook() 重入，重入时要保住它才能跳过重复解压/重读。次字面一起还
  // （releaseBookFonts 里就是 close_alt + 清哈希），两本书的家族往往不是一回事。
  releaseBookFonts();
  st.bookFontLocal.clear();
  st.bookFontTag = 0;
  bool ok = false;
  if (kind == 0) ok = openEpub(path);
  else if (kind == 1) ok = openTxt(path);
  else if (kind == 2) ok = openXtc(path);
  if (ok) {
    // 读到哪就从哪继续。显式跳页的调用方（书签/返回点）随后会再跳一次覆盖掉，
    // 那条路径不受影响。
    rdRestoreProgress();
    generateCoverForOpenedBook();
    pushRecent(path, kind, st.bookTitle);
    // 统计会话：同一本书因为改字号/行距重排会再走一次这里（reopenBook），
    // rdStatsBeginSession 内部认活跃路径，同书不重开会话。
    rdStatsBeginSession();
  }
  return ok;
}

// ── 封面 ────────────────────────────────────────────────────────────────
// 一本书的缓存目录（与 crossmux 一致：缓存目录 + 类型前缀 + path 哈希）。
static std::string bookCacheDirFor(const std::string &path, int kind) {
  const char *pfx = (kind == 0) ? "epub" : (kind == 1) ? "txt" : "xtc";
  return std::string(CACHE_DIR) + "/" + pfx + "_" + std::to_string(std::hash<std::string>{}(path));
}
// 封面缓存路径。v2 = 8 位灰阶封面，与 Epub/Txt/Xtc::getCoverBmpPath() 必须一致 ——
// 这里是读、那边是写，名字对不上书架就永远只有占位框。改名同时让老机器上那张 2 位
// 抖动封面自然失效（生成端见文件存在就跳过，不改名永远换不掉）。
static std::string coverBmpPathFor(const std::string &path, int kind) {
  return bookCacheDirFor(path, kind) + "/cover_v2.bmp";
}
static std::string coverBmpPathFor(const BookEntry &b) { return coverBmpPathFor(b.path, b.kind); }

// 待机「书籍封面」表盘用的整屏封面缓存：**原图**按待机框解出来的那一张，与书架那张
// 396×528 的 cover_v2.bmp 分开存。名字带版本（v1）：以后改了解析口径/框尺寸，改个
// 名字就自然作废重生成，不必写迁移。生成端见 generateCoverForOpenedBook。
// / The full-screen standby-face cover: the book's ORIGINAL image decoded to the standby
// box, cached separately from the 396×528 shelf cover. Versioned name so a later change
// in box or decoding invalidates it without a migration.
static std::string standbyCoverPathFor(const std::string &path, int kind) {
  return bookCacheDirFor(path, kind) + "/standby_v1.bmp";
}

// 待机封面表盘的目标框（整屏减一圈页边距）。**生成端（本文件）与绘制端
// （standby_clock.cpp 的 drawCoverFace）都调这一份**：框只有一个来源，改了不会一边
// 变一边不变 —— 那正是"生成时按 A 尺寸解、画的时候按 B 尺寸又缩一遍"的糊法。
void readerStandbyCoverBox(int &x, int &y, int &w, int &h) {
  const int W = SCREEN_W, H = SCREEN_H;
  const int m = (W < H ? W : H) / 40;   // 短边的 2.5%：面板本身盖边 3~4px，留一点就够
  x = m;
  y = m;
  w = W - 2 * m;
  h = H - 2 * m;
}

// 打开书后即时生成封面（用已加载对象，避免二次解压）。
static void rdCoverThumbForget(const std::string &bmpPath);  // 定义在封面缩放那一段
static void generateCoverForOpenedBook() {
  // 顺手清掉 v1 的封面：改名之后它再也不会被读到，留着白占卡（每本约 170KB，
  // 几百本就是几十 MB）。删失败也无所谓，下次打开再试。
  const std::string legacy = bookCacheDirFor(st.bookPath, st.bookKind) + "/cover.bmp";
  if (Storage.exists(legacy.c_str())) Storage.remove(legacy.c_str());
  if (st.bookKind == 0) { if (st.epub) st.epub->generateCoverBmp(); }
  else if (st.bookKind == 1) { Txt t(st.bookPath, CACHE_DIR); if (t.load()) (void)t.generateCoverBmp(); }
  else if (st.bookKind == 2) { if (st.xtc) st.xtc->generateCoverBmp(); }
  // 新封面写完了 → 作废缩略图缓存里这本的旧条目（否则书架上还挂着上一版封面）。
  rdCoverThumbForget(coverBmpPathFor(st.bookPath, st.bookKind));
  generateStandbyCoverForOpenedBook();
}

// 待机整屏封面：打开书时顺手留一份，供「书籍封面」表盘 1:1 上屏。
//
// 为什么不在待机时现做：待机是 light sleep 前的最后一屏，那时书对象已经不在手上
// （screen_reader_exit 会释放），要现做就得重新解压 epub 找封面 —— 而且慢。所以按
// **原图**在开书这一趟解好，一本书只做一次（文件在就跳过）。
//
// 为什么不用书架那张 cover_v2.bmp：那是 396×528 的格子缩略图，待机整屏的框是
// 650×1182 上下，拿它上屏就是"缩略图放大"——用户报的"待机封面糊"就是它。
//
// XTC 不做：它的 cover_v2.bmp 本来就是第 0 页原分辨率（见 Xtc.cpp 的注释），已经
// 是能拿到的最好一版，readerLastBookCover 会退回用它。
static void generateStandbyCoverForOpenedBook() {
  if (st.bookPath.empty() || st.bookKind == 2) return;
  const std::string out = standbyCoverPathFor(st.bookPath, st.bookKind);
  if (Storage.exists(out.c_str())) return;   // 一书一次

  int bx = 0, by = 0, bw = 0, bh = 0;
  readerStandbyCoverBox(bx, by, bw, bh);
  bool ok = false;
  if (st.bookKind == 0) {
    if (st.epub) ok = st.epub->generateStandbyCoverBmp(out, bw, bh);
  } else if (st.bookKind == 1) {
    Txt t(st.bookPath, CACHE_DIR);
    if (t.load()) ok = t.generateStandbyCoverBmp(out, bw, bh);
  }
  ESP_LOGI(TAG, "待机封面: %s (%dx%d) %s", out.c_str(), bw, bh, ok ? "已生成" : "无原图，退回书架封面");
}

// 重排当前这本书（改字号/行距/段距/边距/方向/字体/样式解析都走这里）。
// openEpub/openTxt/openXtc 给的是"刚打开"的位置（第 0 章第 0 页），所以先把"读到哪"
// 记下来、重排完再跳回去 —— 改了字号却被弹回书首是没人想要的。页号会随重排变化
// （字大了页就多），这里只保证落在同一章里，不追锚点：追锚点要动 Section 的接口。
// 不直接复用 openBook()：那会再 pushRecent/生成封面/重开统计会话，都不是重排该干的事。
static void reopenBook() {
  if (st.bookPath.empty()) return;
  const int wantSpine = st.spineIndex;
  const int wantPage = st.page;
  const int wantTxt = st.txtPage;
  const int wantXtc = st.xtcPage;
  if (st.bookKind == 0) openEpub(st.bookPath);
  else if (st.bookKind == 1) openTxt(st.bookPath);
  else if (st.bookKind == 2) openXtc(st.bookPath);

  if (st.bookKind == 0 && st.epub && st.section) {
    const int n = st.epub->getSpineItemsCount();
    const int sp = clampI(wantSpine, 0, std::max(0, n - 1));
    if (sp != st.spineIndex) openSpine(sp);
    if (st.section) buildToPage(wantPage);
  } else if (st.bookKind == 1 && !st.txtLineStarts.empty()) {
    st.txtPage = clampI(wantTxt, 0, std::max(0, totalPages() - 1));
  } else if (st.bookKind == 2 && st.xtc) {
    st.xtcPage = clampI(wantXtc, 0, std::max(0, static_cast<int>(st.xtc->getPageCount()) - 1));
  }
}

// ── 错相揭页（翻页动画，设置 → 显示与版式 → 翻页动画）──────────────────
// turnBook() 成功翻页后记下方向，renderAndPresent() 在决定刷新档位时消费它：
// 只有"这一帧确实是翻页、且选中的是差分正文刷"才让推屏层走揭页动画。
// 换章/全刷频率到点/进新界面首帧都是整屏 GC16 重画，方向会被丢掉（也不该动画）。
static int s_pendingTurn = 0;  // 0=无，+1=向后翻，-1=向前翻
static bool pageTurnAnimOn() { return g_settings.getString("page_turn_anim", "1") != "0"; }
// e0470_turn_dir_t 的取值。本 TU 不能 include e0470_page_turn.h（它会拖进 epdiy.h），
// 所以这边只能用裸数字；两边靠这条注释对齐。
// 枚举顺序 ltr/rtl/ttb/btt = 0/1/2/3；现在只用前两个（横向扫），后两个留着对齐枚举。
static const int kTurnLtr = 0, kTurnRtl = 1;

// 揭页方向**一律横向**，不看排版方向：竖屏下书是纵向分页（一屏接一屏往下读），
// 早先按排版方向选 TTB/BTT，看着是"一条带从下往上掀"——不像翻书页。用户要的是
// 左右方向：往后翻 = 新页从右边揭过来（RTL），往前翻 = 从左边揭回来（LTR）。
// 方向报错揭页会朝相反一侧扫，看着像"往回翻"，所以 delta 的符号别调反。
static int turnDirFor(int delta) { return delta > 0 ? kTurnRtl : kTurnLtr; }

// ── 翻页 ────────────────────────────────────────────────────────────────
static bool turnEpub(int dir) {
  if (!st.section) return false;
  int np = st.page + dir;
  if (np < 0) {
    if (st.spineIndex > 0) { openSpine(st.spineIndex - 1); st.page = st.section->pageCount - 1; return true; }
    return false;
  }
  if (np < static_cast<int>(st.section->pageCount)) { st.page = np; return true; }
  if (!st.section->isBuildComplete()) {
    st.section->buildSomeMore(4);
    if (np < static_cast<int>(st.section->pageCount)) { st.page = np; return true; }
    if (!st.section->isBuildComplete()) return false;  // 还在排，等下一拍
  }
  if (st.spineIndex + 1 < st.epub->getSpineItemsCount()) { openSpine(st.spineIndex + 1); st.page = 0; return true; }
  return false;
}

static bool turnBook(int dir) {
  st.selActive = false;   // 翻页即放弃选区（选区只对当前页有效）
  bool ok = false;
  if (st.bookKind == 0) ok = turnEpub(dir);
  else if (st.bookKind == 1) {
    int np = st.txtPage + dir;
    int tp = totalPages();
    if (np >= 0 && np < tp) { st.txtPage = np; ok = true; }
  } else if (st.bookKind == 2) {
    int np = st.xtcPage + dir;
    if (np >= 0 && np < static_cast<int>(st.xtc->getPageCount())) { st.xtcPage = np; ok = true; }
  }
  if (ok) st.dirty = 1;
  // 记下方向给推屏用（err 时清掉：这一帧什么都没翻，动画没有意义）。
  s_pendingTurn = ok ? (dir < 0 ? -1 : +1) : 0;
  return ok;
}

// ── 状态栏内容（自定义状态栏）───────────────────────────────────────────
// 对应 crossmux 的 StatusBarSettingsActivity：阅读页底部那一条显示些什么。
// 取值存在 settings（reader_sb_*），drawStatusBand() 按这些值排版。
static int curPage();  // 定义在下面（书签/百分比共用）

static int sbGet(const char *key, int def) {
  return atoi(g_settings.getString(key, std::to_string(def).c_str()).c_str());
}
static int sbCount(const char *key, int def, int n) {
  int v = sbGet(key, def);
  return (v < 0 || v >= n) ? def : v;
}

// ── TOC 索引表（章节名 / 章节跳转共用）──────────────────────────────────
// ⚠ 这里曾经是"翻一页等 8 秒"的元凶（实测，不是推测）。
//   Epub::getTocItem() 每取一条 TOC 条目 = **一次真实的 SD 随机读**：先 seek 到 TOC
//   LUT 读数，再 seek 到条目本体读串（BookMetadataCache.cpp 的 getTocEntry，:650）。
//   章节名那个循环对**每一个** TOC 条目都调它两遍（getSpineIndexForTocIndex + getTocItem
//   各一次），TOC 上千条 → 每取一次章节名就是上千次 SD 随机读。
//   而它在**每次按键**（rdStatsNoteActivity）和**每帧状态栏**（titleMode==1 那条）
//   里都会被调到，于是每翻一页白白多花 7.7 秒（日志 `分发探针: … 统计 7706ms`）。
//
// 修法：章节名只在换章时变，所以把整张 TOC 的 (spineIndex, title) 一次性读进 RAM
// （每本书一份，第一次用到时建；getTocItem 返回的条目本身就带 spineIndex，原来多算的
// 那遍是白读）。**章节跳转（双击左右电容键）要的正是同一张表**，所以一并放这里，
// 别让它再扫一遍 SD——那张表就是"哪条目录在哪个 spine"。
struct TocIndex {
  std::string path;                  // 缓存归属：按书路径判，避免 Epub 指针被复用
  std::vector<int> spine;
  std::vector<std::string> title;
};

static const TocIndex &tocIndex() {
  static TocIndex s;
  if (st.bookKind == 0 && st.epub && s.path != st.bookPath) {
    const int n = st.epub->getTocItemsCount();
    const int64_t t0 = esp_timer_get_time();
    s.path = st.bookPath;
    s.spine.clear();
    s.title.clear();
    s.spine.reserve(n);
    s.title.reserve(n);
    for (int i = 0; i < n; i++) {
      auto e = st.epub->getTocItem(i);     // 一次 SD 读拿全：spineIndex + title
      s.spine.push_back(e.spineIndex);
      s.title.push_back(e.title);
    }
    ESP_LOGI(TAG, "章节名索引: %d 条 TOC 一次读入 RAM 表 %lldms",
             n, (esp_timer_get_time() - t0) / 1000);
  }
  return s;
}

// 当前章节名（EPUB；否则退回书名）。语义逐字不变：TOC 序里最后一个
// spineIndex ≤ 当前 spine 的条目。
static std::string currentChapterTitle() {
  if (st.bookKind == 0 && st.epub) {
    const TocIndex &ti = tocIndex();
    // 记下标而不是"边扫边拷字符串"：这个函数每帧都会跑（状态栏 titleMode==1），
    // TOC 上千条时每命中一次拷一个 std::string 是白白的堆分配。
    int best = -1;
    for (size_t i = 0; i < ti.spine.size(); i++) {
      if (ti.spine[i] >= 0 && ti.spine[i] <= st.spineIndex) best = static_cast<int>(i);
    }
    if (best >= 0 && !ti.title[best].empty()) return ti.title[best];
  }
  return st.bookTitle;
}

// ── 章节跳转（阅读页双击右/左电容键）────────────────────────────────────
// 有目录就按目录找：**章节 = 目录条目**，取当前 spine 之后/之前最近的那条的 spine，
// 落到那一节第 0 页——粒度和目录菜单跳转（handleToc）一致。目录条目可能"一章一
// 文件"，也可能几章挤在一个文件里，按目录才对得上人眼里的"章"。
// 目录里没有更前/更后的条目（或压根没有目录）就退回相邻 spine，走一节算一章。
// TXT/XTC 没有章节概念，不动。
static bool jumpChapter(int dir) {
  if (st.bookKind != 0 || !st.epub) return false;
  const int cur = st.spineIndex;
  int target = -1;
  const TocIndex &ti = tocIndex();
  for (size_t i = 0; i < ti.spine.size(); i++) {
    const int sp = ti.spine[i];
    if (sp < 0) continue;
    if (dir > 0) { if (sp > cur && (target < 0 || sp < target)) target = sp; }
    else         { if (sp < cur && sp > target) target = sp; }
  }
  if (target < 0) {
    const int nx = cur + dir;
    if (nx < 0 || nx >= st.epub->getSpineItemsCount()) return false;  // 首章之前 / 末章之后
    target = nx;
  }
  st.selActive = false;      // 换章即放弃选区（和翻页同一条）
  if (!openSpine(target)) return false;
  st.page = 0;               // openSpine 自己也会置 0，写一次是明示
  st.fullRefresh = true;     // 整屏重画（目录跳转也是这么做的）
  st.dirty = 1;
  ESP_LOGI(TAG, "章节跳转: %s spine %d → %d", dir > 0 ? "下一章" : "上一章", cur, target);
  return true;
}

// 章节内页码（EPUB；否则退回全书页）。
static void chapterPage(int &cur, int &tot) {
  if (st.bookKind == 0 && st.section && st.section->pageCount > 0) {
    cur = st.page + 1;
    tot = static_cast<int>(st.section->pageCount);
  } else {
    cur = curPage() + 1;
    tot = totalPages();
  }
}

// ── 阅读统计接线 ────────────────────────────────────────────────────────
// 数据层在 main/reading_stats.{h,cpp}（口径/常量与 crossmux 一致），这里只决定
// **什么时候记一笔**。记时逻辑是"把距上次交互的时间记进当时的本地日桶"，所以入口
// 只有两个：有按键的那一帧（分发之后）和空闲心跳（没按键但人还在看）。
//
// 会话的语义：打开一本书 = beginSession，回到书架/退出阅读器 = endSession（短于
// 3 分钟的会话不计"次数"，但时长照记）。中途去菜单/目录/词典再回来不补记空档
// ——那段时间人在看别的界面，resumeSession 把间隙丢掉。
static RdMode s_statsPrevMode = RdMode::Browser;

static void rdStatsBeginSession() {
  if (st.bookPath.empty()) return;
  // 同书重入（改字号/行距触发的 reopenBook）不重开会话，只续上心跳。
  if (ReadingStats::hasActiveSession() && ReadingStats::activePath() == st.bookPath) {
    ReadingStats::resumeSession();
    return;
  }
  int cur = 0, tot = 0;
  chapterPage(cur, tot);
  const int bookPct = totalPages() > 0 ? (curPage() + 1) * 100 / totalPages() : 0;
  const int chapPct = tot > 0 ? cur * 100 / tot : 0;
  std::string author;
  if (st.bookKind == 0 && st.epub) author = st.epub->getAuthor();
  ReadingStats::beginSession(st.bookPath, st.bookTitle, author, static_cast<uint8_t>(clampI(bookPct, 0, 100)),
                             currentChapterTitle(), static_cast<uint8_t>(clampI(chapPct, 0, 100)));
}

// 有按键的一帧：同步进度 + 记一笔。全书进度用状态栏上那个同一个数字
// （(curPage()+1)*100/totalPages），章节进度用章节内页码。
static void rdStatsNoteActivity() {
  if (!ReadingStats::hasActiveSession()) return;
  int cur = 0, tot = 0;
  chapterPage(cur, tot);
  const int bookPct = totalPages() > 0 ? (curPage() + 1) * 100 / totalPages() : 0;
  const int chapPct = tot > 0 ? cur * 100 / tot : 0;
  ReadingStats::updateProgress(static_cast<uint8_t>(clampI(bookPct, 0, 100)), false, currentChapterTitle(),
                               static_cast<uint8_t>(clampI(chapPct, 0, 100)));
  ReadingStats::noteActivity();
}

// 空闲帧：①模式从别处回到阅读页的那一帧丢弃空档（菜单/目录/词典十几处返回点都在
// 这里一处收口，不用各自改）；②阅读页按 60 秒心跳补时；③有脏数据且攒够 10 分钟
// 就落一次盘，掉电不丢。
static void rdStatsIdleTick() {
  if (st.mode != s_statsPrevMode) {
    if (st.mode == RdMode::Reading) ReadingStats::resumeSession();
    s_statsPrevMode = st.mode;
  }
  if (st.mode != RdMode::Reading) return;
  ReadingStats::tickActiveSession();
  if (ReadingStats::shouldCheckpoint() && ReadingStats::save()) {
    ESP_LOGI(TAG, "阅读统计检查点已落盘");
  }
}

// ── 排版余量 ────────────────────────────────────────────────────────────
// 章节排版本来就是增量的（Section::buildSomeMore，单次还有 32KB 源字节上界），
// 真正的问题在**谁**来做这一步：turnEpub 把"翻到水位之外的那一翻"和排版捆在一拍里，
// 于是长章节每翻过一页水位就多等一次排版 —— 日志里同一本书，跨水位的翻页
// 「合计 1420ms」，水位内的「合计 606ms」，差值就是这一拍里塞的排版。
// 短章节没这个问题：buildSomeMore(2) 在 openSpine 里就把整章排完了，之后每一翻都
// 只是取页+绘制+推屏。
//
// 所以把排版的节拍**从翻页那一拍挪到空闲**，并且给一个固定余量：始终保持排版
// 跑在读者前面 kPrebuildAhead 页，一旦领先够多就停下。这样排版量随阅读推进平移
// （全书/整章的排版总量不变），既不会在翻页时卡住，也不会出现"一翻开就把整章
// 排完"——后者只在畸形 HTML（整章一个巨块）时靠 buildSomeMore 的字段上界兜底。
//
// 余量取 5：读者连翻 5 页都不会碰到水位，第 6 页时空闲帧早就把水位又推上去了。
// 这也是当前行为的上界（turnEpub 每次跨水位排 4 页，本来就领先 3~4 页），只是
// 把这份工作量从"按键那一拍"挪到了"读者盯着这一页看"的空档里。
static const int kPrebuildAhead = 5;

static void rdPrebuildAhead() {
  if (st.bookKind != 0 || st.mode != RdMode::Reading || !st.section) return;
  if (st.section->isBuildComplete()) return;
  // 单次调用有界：一次空闲帧最多花 kPrebuildBudgetUs 做排版，超了就留给下一帧。
  // 空闲帧每 ~80ms 一拍（main.cpp 的 idleWaitWithTouch(80)），几帧就能把余量补满；
  // 按键随时会来，所以这里宁可少排一点也不能让触摸轮询等太久。
  const int64_t kPrebuildBudgetUs = 30 * 1000;
  const int64_t deadline = esp_timer_get_time() + kPrebuildBudgetUs;
  while (!st.section->isBuildComplete() &&
         static_cast<int>(st.section->pageCount) < st.page + 1 + kPrebuildAhead) {
    st.section->buildSomeMore(1);
    if (esp_timer_get_time() >= deadline) break;
  }
}

// 本地时间（跟随设置的 TZ）。未对时（RTC 无效）返回 false。
static bool readerLocalNow(struct tm &out) {
  time_t t = g_rtc.getTime();
  if (t <= 0) t = time(nullptr);
  if (t < 1600000000) return false;  // 早于 2020 年视为未对时
  localtime_r(&t, &out);
  return true;
}

// 阅读页底部状态栏：分隔线 + 左（时钟/标题）+ 右（页码/百分比/时钟/电量）
// + 屏底进度条（占 RD_BOTTOM_INSET 留白，不与文字打架）。
static void drawStatusBand(int y) {
  int w = g_rd.getScreenWidth();
  g_rd.drawLine(0, y, w, y, true);

  int textY = y + 4;
  // 状态栏的字（书名/章节名/时钟/页码）用**用户字体**：内置子集只有 7710 字，
  // 书名里的生僻字会变豆腐块、长书名截断得也早。asc 仍取 uiAsc()，两 id 同一
  // 个 family，度量完全相同，版式不动。
  int asc = uiAsc();
  char buf[64];

  int titleMode = sbCount("reader_sb_title", 0, 3);
  int pageMode = sbCount("reader_sb_page", 1, 3);
  // 「显示/隐藏」两态项存的是标签下标（kSbShowNames[0]=="显示"），不是布尔值。
  // 写成 if (pctOn) 就正好反了：设置页显示「显示」的那个值（0）反倒一行都不画，
  // 表现就是电量/百分比怎么改都看不到——改成 == 0 才与设置页的标签一致。
  int pctOn = sbCount("reader_sb_pct", 0, 2);
  int barMode = sbCount("reader_sb_bar", 2, 3);  // 默认隐藏，保持原有观感
  int thick = sbCount("reader_sb_thick", 1, 3);
  int battOn = sbCount("reader_sb_batt", 0, 2);
  int clockMode = sbCount("reader_sb_clock", 0, 3);

  struct tm lt;
  std::string clock;
  if (readerLocalNow(lt)) {
    snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    clock = buf;
  } else {
    // 还没对时（首次联网校时之前 RTC 无效）也把位置占住：否则把时钟设成
    // 左/右侧照样一片空白，看着像设置没生效。crossmux 同样退化画 --:--。
    clock = "--:--";
  }

  // 右侧组：从右缘往左依次排「电量 → 时钟 → 百分比 → 页码」。
  int rx = w - MARGIN;
  auto pushRight = [&](const std::string &s) {
    if (s.empty()) return;
    int tw = g_rd.getTextWidth(uiFontId(), s.c_str());
    rx -= tw;
    if (rx < MARGIN) return;  // 放不下就整体不画，别和左侧叠字
    g_rd.drawText(uiFontId(), rx, textY + asc, s.c_str(), true);
    rx -= 12;
  };

  if (battOn == 0) {
    int pct = battery_pct();
    if (pct >= 0) {
      snprintf(buf, sizeof(buf), "%d%%", pct);
      pushRight(buf);
    }
  }
  if (clockMode == 1 && !clock.empty()) pushRight(clock);
  if (pctOn == 0) {
    int tp = totalPages();
    int p = tp > 1 ? clampI((curPage() + 1) * 100 / tp, 0, 100) : 100;
    snprintf(buf, sizeof(buf), "%d%%", p);
    pushRight(buf);
  }
  if (pageMode != 2) {
    int c, t;
    if (pageMode == 0) chapterPage(c, t);
    else { c = curPage() + 1; t = totalPages(); }
    snprintf(buf, sizeof(buf), "%d/%d", c, t);
    pushRight(buf);
  }

  // 左侧：时钟（可选）+ 标题（可选）。
  int lx = MARGIN;
  if (clockMode == 2 && !clock.empty()) {
    g_rd.drawText(uiFontId(), lx, textY + asc, clock.c_str(), true);
    lx += g_rd.getTextWidth(uiFontId(), clock.c_str()) + 12;
  }
  if (titleMode != 2) {
    std::string t = (titleMode == 1) ? currentChapterTitle() : st.bookTitle;
    int avail = rx - lx;
    if (avail > 0) t = g_rd.truncatedText(uiFontId(), t.c_str(), avail);
    if (!t.empty() && lx < rx) g_rd.drawText(uiFontId(), lx, textY + asc, t.c_str(), true);
  }

  // 底部进度条：描边槽 + 槽内浅灰 + 已读实心黑。
  // 原来只描个空心矩形：上下两条全宽细线落在屏底最后几行，看着就是屏幕本来自带的
  // 一道边；已读部分按比例只有几个像素宽（第一页 p/总页数），整条跟没读一样，所以
  // 「进度条」这个设置看着毫无反应。槽内铺浅灰（10/15）之后，条有多长、读到哪儿
  // 一眼可辨；灰阶封面缩略图/XTC 页已经在用，刷新链路是通的。极速(DU)刷会把灰底
  // 推成纯白，这时描边还在，条仍然看得出范围。
  if (barMode != 2) {
    const int th = (thick == 0) ? 2 : (thick == 1) ? 3 : 4;
    // 离屏底 2px：贴着最后一行时细条在屏边上看不真切。
    const int barY = g_rd.getScreenHeight() - th - 2;
    int num, den;
    if (barMode == 1) chapterPage(num, den);
    else { num = curPage() + 1; den = totalPages(); }
    if (den <= 0) den = 1;
    int fw = static_cast<int>(static_cast<int64_t>(w) * num / den);
    if (fw < 0) fw = 0;
    if (fw > w) fw = w;
    g_rd.drawRect(0, barY, w, th, true);  // 槽的描边：界定整条范围
    if (th > 2) {                         // 细(2px)没有内部可铺，保持纯描边
      for (int yy = barY + 1; yy < barY + th - 1; yy++) {
        for (int xx = 1; xx < w - 1; xx++) g_rd.drawGrayscale16Pixel(xx, yy, 10);
      }
    }
    if (fw > 0) g_rd.fillRect(1, barY, fw, th, true);  // 已读：实心黑盖在槽上
  }
}

// ── 渲染：各界面 ────────────────────────────────────────────────────────
static void drawReaderStatus() { drawStatusBand(statusTop()); }

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

// 当前页的文字地图。渲染时采一次，长按命中直接查它，避免再 loadPage 一遍。
static RdPageText g_pageText;

static RdPageText rdBuildPageText(const Page &page, int fontId, int xOffset, int yOffset) {
  RdPageText pt;
  pt.valid = true;
  for (const auto &el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto &line = static_cast<const PageLine &>(*el);
    const auto &blk = line.getBlock();
    if (!blk || !blk->valid()) continue;
    pt.lineFirst.push_back(static_cast<int>(pt.words.size()));
    const int baseY = yOffset + el->yPos;
    for (uint16_t i = 0; i < blk->wordCount(); i++) {
      RdWordHit wh;
      wh.x = xOffset + el->xPos + blk->wordXpos(i);
      wh.style = static_cast<uint8_t>(blk->wordStyle(i));
      wh.y = baseY;
      wh.text = blk->wordText(i);
      wh.w = g_rd.getTextWidth(fontId, wh.text.c_str(), static_cast<EpdFontFamily::Style>(wh.style));
      pt.words.push_back(std::move(wh));
    }
  }
  pt.lineFirst.push_back(static_cast<int>(pt.words.size()));
  return pt;
}

// TXT 没有排版块，只能按行自己切：行内容按字符拆开，x 用前缀宽度量出来。
// 字符级粒度让"长按选一句"在 TXT 上同样可用。
static RdPageText rdBuildPageTextTxt(const std::vector<std::string> &lines, int fontId, int x, int y0,
                                     int lineH) {
  RdPageText pt;
  pt.valid = true;
  int y = y0 + g_rd.getFontAscenderSize(fontId);
  for (const auto &seg : lines) {
    pt.lineFirst.push_back(static_cast<int>(pt.words.size()));
    int px = x;
    size_t i = 0;
    while (i < seg.size()) {
      int n = utf8Len(static_cast<unsigned char>(seg[i]));
      if (n < 1 || i + n > seg.size()) n = 1;
      RdWordHit wh;
      wh.x = px;
      wh.y = y;
      wh.text = seg.substr(i, n);
      int pw = g_rd.getTextWidth(fontId, seg.substr(0, i + n).c_str());
      wh.w = pw - (px - x);
      if (wh.w < 0) wh.w = 0;
      px = x + pw;
      pt.words.push_back(std::move(wh));
      i += n;
    }
    y += lineH;
  }
  pt.lineFirst.push_back(static_cast<int>(pt.words.size()));
  return pt;
}

// 整页预取：把这一页要用的字形块一次性读进 ttf_font.c 的 PSRAM 块缓存。
//
// 为什么值得单独干这件事：ttf_draw_text_px 自己按词预热，一次只看得到下一个词的
// 几个字，于是整页摊出几十次互不相邻的小读，每次还都得重新 lseek —— 30MB 字体上
// FATFS 走一遍簇链，实测占单次读的一半时间。整页先把所有字收齐再排块，ttf_font.c
// 才能把相邻块并成顺序大读（io_flush_touches 的间隙合并）。
//
// 必须在内容字面下调用：角色不同，loca/glyf 的基址就不同，拿错字面等于白读一遍。
// 内建字体/已整表映射时 ttf_warm_text_px 直接返回，这里是空操作。
// 传的是去重前的整段文本，去重与"已缓存则跳过"都在 font 侧做。
static void rdWarmStrings(int role, const std::string &all) {
  if (all.empty()) return;
  ttf_set_role(role);
  ttf_warm_text_px(kBodyPx[st.fontLevel], all.c_str());
}

static void rdWarmPageText(const RdPageText &pt) {
  if (!pt.valid) return;
  // 按字面分两拨：一页里正文（内容面）和注文/引文（次字面）混排是常态，
  // 而 ttf_warm_text_px 只作用于"当前字面"，混在一起喂等于拿内容面的 loca/glyf
  // 基址去读次字面该用的字形块 —— 白读一遍，那些注文还是一个冷字形一次 SD 读。
  // 次字面没打开时 ttf_set_role 会静默退回内容面，所以不必在这儿判空。
  std::string main, alt;
  for (const auto &w : pt.words) {
    if (w.text.empty()) continue;
    if ((w.style & EpdFontFamily::ALT_FONT) != 0) {
      alt += w.text;
    } else {
      main += w.text;
    }
  }
  rdWarmStrings(RD_TTF_ROLE_CONTENT_ALT, alt);
  rdWarmStrings(RD_TTF_ROLE_CONTENT, main);  // 最后把角色留在内容面，跟改之前一致
}

// 词序列 → 展示文本。中日韩之间不加空格（原文本来就没有），拉丁词之间补一个空格。
static bool rdAsciiWordChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static std::string rdJoinWords(const RdPageText &pt, int a, int b) {
  std::string s;
  for (int i = a; i <= b && i < static_cast<int>(pt.words.size()); i++) {
    const std::string &w = pt.words[i].text;
    if (!s.empty() && !w.empty() && rdAsciiWordChar(s.back()) && rdAsciiWordChar(w.front())) s += ' ';
    s += w;
  }
  return s;
}

// 在当页找回一段文字，返回起始词下标（找不到 -1），end 输出结束词下标。
// 只做当页匹配：跨页的标注这一页就不画高亮，绝不画到别的句子上。
static int rdFindSpan(const RdPageText &pt, const std::string &target, int &end) {
  if (target.empty()) return -1;
  const int n = static_cast<int>(pt.words.size());
  for (int a = 0; a < n; a++) {
    std::string s;
    for (int b = a; b < n; b++) {
      const std::string &w = pt.words[b].text;
      if (!s.empty() && !w.empty() && rdAsciiWordChar(s.back()) && rdAsciiWordChar(w.front())) s += ' ';
      s += w;
      if (s.size() > target.size()) break;
      if (s.size() == target.size()) {
        if (s == target) { end = b; return a; }
        break;   // 同长度但不等：往后只会更长
      }
    }
  }
  return -1;
}

// 词尾是不是句读（。！？；…!?;.）
static bool rdWordEndsSentence(const std::string &w) {
  if (w.empty()) return false;
  size_t i = w.size() - 1;
  while (i > 0 && (static_cast<unsigned char>(w[i]) & 0xC0) == 0x80) i--;
  const unsigned char c = static_cast<unsigned char>(w[i]);
  unsigned cp = c;
  if (c >= 0x80) {
    if ((c & 0xE0) == 0xC0) return false;                      // 2 字节：无句读
    if ((c & 0xF0) == 0xE0 && i + 2 < w.size()) {
      cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(w[i + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(w[i + 2]) & 0x3F);
    } else {
      return false;
    }
  }
  switch (cp) {
    case 0x3002:   // 。
    case 0xFF01:   // ！
    case 0xFF1F:   // ？
    case 0xFF1B:   // ；
    case 0x2026:   // …
    case '!':
    case '?':
    case ';':
    case '.':
      return true;
    default:
      return false;
  }
}

// 点到的词所在的那一句（前后各自找到句读为界）。整页都没句读时按 idx 前后各留 30 词，
// 免得一按就选中半页。
static void rdSentenceAt(const RdPageText &pt, int idx, int &a, int &b) {
  const int n = static_cast<int>(pt.words.size());
  if (n <= 0) { a = b = -1; return; }
  idx = clampI(idx, 0, n - 1);
  a = 0;
  for (int i = idx - 1; i >= 0; i--) {
    if (rdWordEndsSentence(pt.words[i].text)) { a = i + 1; break; }
  }
  b = n - 1;
  for (int i = idx; i < n; i++) {
    if (rdWordEndsSentence(pt.words[i].text)) { b = i; break; }
  }
  if (b - a + 1 > 120) {
    a = (a > idx - 30) ? a : idx - 30;
    b = (b < idx + 30) ? b : idx + 30;
  }
  if (a < 0) a = 0;
  if (b >= n) b = n - 1;
}

// 屏幕点 → 词下标（同一行里挑最近的；行外返回 -1）。
static int rdWordAtPoint(const RdPageText &pt, int fontId, int x, int y) {
  const int asc = g_rd.getFontAscenderSize(fontId);
  int best = -1, bestScore = 0;
  for (int i = 0; i < static_cast<int>(pt.words.size()); i++) {
    const RdWordHit &w = pt.words[i];
    const int dy = std::abs(y - (w.y - asc / 2));
    if (dy > asc) continue;
    int dx = 0;
    if (x < w.x) dx = w.x - x;
    else if (x >= w.x + w.w) dx = x - (w.x + w.w);
    const int score = dy * 4 + dx;
    if (best < 0 || score < bestScore) { bestScore = score; best = i; }
  }
  return best;
}

// 把 [a,b] 词范围画成反白高亮（黑底白字）：逐行找连续段，整段铺黑再把字重画成白的。
static void rdDrawSpan(const RdPageText &pt, int fontId, int a, int b) {
  const int n = static_cast<int>(pt.words.size());
  if (a < 0 || b < a || b >= n) return;
  const int asc = g_rd.getFontAscenderSize(fontId);
  const int lh = g_rd.getLineHeight(fontId);
  int i = a;
  while (i <= b) {
    int j = i;
    while (j + 1 <= b && pt.words[j + 1].y == pt.words[i].y) j++;
    const int x0 = pt.words[i].x;
    const int x1 = pt.words[j].x + pt.words[j].w;
    if (x1 > x0) {
      g_rd.fillRect(x0 - 1, pt.words[i].y - asc - 2, x1 - x0 + 2, lh + 2, true);
      for (int k = i; k <= j; k++)
        g_rd.drawText(fontId, pt.words[k].x, pt.words[k].y, pt.words[k].text.c_str(), false,
                      static_cast<EpdFontFamily::Style>(pt.words[k].style));
    }
    i = j + 1;
  }
}

// ── 笔记存储 ────────────────────────────────────────────────────────────
// 存 <SD>/reader_notes.txt，一行一条，字段用 \x1f 分隔（笔记正文里的换行转义成 \n）。
// 用文件而不是 g_settings：笔记会长，NVS 单条有大小上限。
static const char *kNotesPath = "/sdcard/reader_notes.txt";
#define RD_NOTE_SEP '\x1f'

static std::string rdNoteEscape(const std::string &s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    if (c == '\n') o += "\\n";
    else if (c == '\r') continue;
    else if (c == RD_NOTE_SEP) o += ' ';
    else o += c;
  }
  return o;
}

static std::string rdNoteUnescape(const std::string &s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { o += '\n'; i++; }
    else o += s[i];
  }
  return o;
}

static void saveNotes() {
  HalFile f;
  if (!Storage.openFileForWrite(TAG, kNotesPath, f)) return;
  for (const auto &n : st.notes) {
    std::string line = rdNoteEscape(n.path) + RD_NOTE_SEP + rdNoteEscape(n.book) + RD_NOTE_SEP +
                       std::to_string(n.spine) + RD_NOTE_SEP + std::to_string(n.page) + RD_NOTE_SEP +
                       std::to_string(static_cast<long long>(n.time)) + RD_NOTE_SEP +
                       rdNoteEscape(n.text) + RD_NOTE_SEP + rdNoteEscape(n.note) + "\n";
    f.write(reinterpret_cast<const uint8_t *>(line.data()), line.size());
  }
  f.close();
}

static void loadNotes() {
  st.notes.clear();
  HalFile f;
  if (!Storage.openFileForRead(TAG, kNotesPath, f)) return;
  std::string all;
  all.resize(f.size());
  if (!all.empty()) {
    const int got = f.read(&all[0], all.size());
    all.resize(got > 0 ? got : 0);
  }
  f.close();
  size_t pos = 0;
  while (pos < all.size()) {
    size_t nl = all.find('\n', pos);
    if (nl == std::string::npos) nl = all.size();
    std::string line = all.substr(pos, nl - pos);
    pos = nl + 1;
    if (line.empty()) continue;
    std::vector<std::string> parts;
    size_t p = 0;
    while (p <= line.size()) {
      size_t sp = line.find(RD_NOTE_SEP, p);
      if (sp == std::string::npos) { parts.push_back(line.substr(p)); break; }
      parts.push_back(line.substr(p, sp - p));
      p = sp + 1;
    }
    if (parts.size() < 7) continue;
    RdState::RdNote n;
    n.path = rdNoteUnescape(parts[0]);
    n.book = rdNoteUnescape(parts[1]);
    n.spine = atoi(parts[2].c_str());
    n.page = atoi(parts[3].c_str());
    n.time = strtoll(parts[4].c_str(), nullptr, 10);
    n.text = rdNoteUnescape(parts[5]);
    n.note = rdNoteUnescape(parts[6]);
    st.notes.push_back(std::move(n));
  }
}

// 当前页上的所有标注 → 词范围（找不到的不画，绝不画到别的句子上）。
struct RdAnchor {
  int noteIdx;
  int a, b;
};
static std::vector<RdAnchor> rdAnchorsOnPage(const RdPageText &pt) {
  std::vector<RdAnchor> out;
  for (int i = 0; i < static_cast<int>(st.notes.size()); i++) {
    const auto &n = st.notes[i];
    if (n.path != st.bookPath || n.spine != st.spineIndex) continue;
    int b = -1;
    const int a = rdFindSpan(pt, n.text, b);
    if (a >= 0) out.push_back({i, a, b});
  }
  return out;
}

// 选区动作浮层：贴着屏底的一条，上一行是选中的原文，下一行是五个按钮。
// 点按与键盘都按同一套下标走，按钮文案随"是不是已有标注"切换。
// 下标固定为 0..4（两种浮层同构）：复制固定在第 4 格、取消固定在最末格，
// 于是 rdSelActivate 可以按显式下标分派，不再靠"不是 0 也不是 1 就是取消"的兜底。
static int rdPopupH() { return uiLineHeight() * 2 + 18; }
static int rdPopupButtonCount() { return 5; }
static const char *rdPopupButtonLabel(int popup, int i) {
  static const char *lNew[] = {"标注", "笔记", "字典", "复制", "取消"};
  static const char *lOld[] = {"删除标注", "改笔记", "字典", "复制", "取消"};
  const char **l = (popup == 2) ? lOld : lNew;
  return (i >= 0 && i < 5) ? l[i] : "";
}
static void rdPopupRect(int *bx, int *by, int *bw, int *bh) {
  *bw = g_rd.getScreenWidth();
  *bh = rdPopupH();
  *bx = 0;
  *by = g_rd.getScreenHeight() - *bh - 6;
}
// 浮层里命中的按钮下标（-1 = 不在按钮行上）。
static int rdPopupButtonAt(int px, int py) {
  int bx, by, bw, bh;
  rdPopupRect(&bx, &by, &bw, &bh);
  if (py < by || py >= by + bh) return -1;
  const int rowY = by + uiLineHeight() + 8;
  if (py < rowY) return -1;
  const int n = rdPopupButtonCount();
  const int seg = bw / n;
  int i = (px - bx) / seg;
  if (i < 0) i = 0;
  if (i >= n) i = n - 1;
  return i;
}

static void rdDrawSelPopup(const RdPageText &pt, int fontId) {
  (void)fontId;
  int bx, by, bw, bh;
  rdPopupRect(&bx, &by, &bw, &bh);
  g_rd.fillRect(bx, by, bw, bh, false);
  g_rd.drawRect(bx, by, bw, bh, true);

  std::string t = rdJoinWords(pt, st.selStart, st.selEnd);
  t = g_rd.truncatedText(uiFontId(), t.c_str(), bw - 2 * MARGIN);
  // 抬头居中（同下面几个按钮）：浮层里的文字一律居中，别一左一中。
  const int tw = g_rd.getTextWidth(uiFontId(), t.c_str());
  drawLineText(bx + (bw - tw) / 2, by + 4, t.c_str(), true);

  const int n = rdPopupButtonCount();
  const int seg = bw / n;
  const int y = by + uiLineHeight() + 8;
  const int h = bh - (y - by) - 4;
  for (int i = 0; i < n; i++) {
    const int x = bx + i * seg;
    g_rd.drawRect(x + 2, y, seg - 4, h, true);
    const bool on = (i == st.selMenuSel);
    if (on) g_rd.fillRect(x + 3, y + 1, seg - 6, h - 2, true);
    const char *lb = rdPopupButtonLabel(st.selPopup, i);
    const int tw = g_rd.getTextWidth(uiFontId(), lb);
    g_rd.drawText(uiFontId(), x + (seg - tw) / 2, y + h / 2 + uiAsc() / 2, lb, !on);
  }
}

// 选区两端的"手柄"：手指点不准字边，给两个看得见也抓得着的端点。
// 都画在反白块**外面**（起点手柄在首行上沿之上、终点手柄在末行下沿之下），所以是
// 白底黑块——落在反白块里就会被白字搅成一团，也分不出是哪一端。
static constexpr int RD_SEL_HANDLE = 5;   // 手柄半边长（像素），成品是 10×10

static void rdSelHandleBoxes(const RdPageText &pt, int fontId, int *sx, int *sy, int *ex,
                             int *ey) {
  // 返回两个手柄的左上角坐标；调用前必须确认选区有效。
  const int asc = g_rd.getFontAscenderSize(fontId);
  const int lh = g_rd.getLineHeight(fontId);
  const RdWordHit &a = pt.words[st.selStart];
  const RdWordHit &b = pt.words[st.selEnd];
  *sx = a.x - 1;
  *sy = a.y - asc - 2 - RD_SEL_HANDLE * 2;   // 首行上沿再往上一个手柄高
  *ex = b.x + b.w - RD_SEL_HANDLE * 2 + 1;
  *ey = b.y + (lh - asc) + 2;                // 末行下沿再往下一个手柄高
}

// 命中哪个手柄：0=起点 1=终点 -1=都不是。容差比手柄本身再放大一圈，手指才够用。
static int rdSelHandleAt(const RdPageText &pt, int fontId, int x, int y) {
  const int n = static_cast<int>(pt.words.size());
  if (!st.selActive || !pt.valid || st.selStart < 0 || st.selEnd >= n ||
      st.selStart > st.selEnd)
    return -1;
  int sx, sy, ex, ey;
  rdSelHandleBoxes(pt, fontId, &sx, &sy, &ex, &ey);
  constexpr int SLOP = 8;
  const int hs = RD_SEL_HANDLE * 2;
  if (x >= sx - SLOP && x <= sx + hs + SLOP && y >= sy - SLOP && y <= sy + hs + SLOP) return 0;
  if (x >= ex - SLOP && x <= ex + hs + SLOP && y >= ey - SLOP && y <= ey + hs + SLOP) return 1;
  return -1;
}

static void rdDrawSelHandles(const RdPageText &pt, int fontId) {
  const int n = static_cast<int>(pt.words.size());
  if (st.selStart < 0 || st.selEnd >= n || st.selStart > st.selEnd) return;
  int sx, sy, ex, ey;
  rdSelHandleBoxes(pt, fontId, &sx, &sy, &ex, &ey);
  const int hs = RD_SEL_HANDLE * 2;
  // 抓着的那一端画成空心（黑框白心）——跟"没抓"的实心块区分开，用户能看出`下一次
  // 点词会挪哪一端`。正在被拖的那一端同样算"抓着"。
  const bool gs = (st.selGrab == 0 || st.selDrag == 0), ge = (st.selGrab == 1 || st.selDrag == 1);
  g_rd.fillRect(sx, sy, hs, hs, !gs);
  if (gs) g_rd.drawRect(sx, sy, hs, hs, true);
  g_rd.fillRect(ex, ey, hs, hs, !ge);
  if (ge) g_rd.drawRect(ex, ey, hs, hs, true);
}

// 按住的那一下抓的是哪一端：先认"手指就在手柄上/旁边"，再认"手指落在反白块里"
// （这时取近的那端）。认不出来返回 -1 —— 在正文上随便划一下不该把选区改掉。
static int rdSelDragGrab(const RdPageText &pt, int fontId, int x, int y) {
  const int n = static_cast<int>(pt.words.size());
  if (!st.selActive || !pt.valid || st.selStart < 0 || st.selEnd >= n ||
      st.selStart > st.selEnd)
    return -1;
  int sx, sy, ex, ey;
  rdSelHandleBoxes(pt, fontId, &sx, &sy, &ex, &ey);
  const int scx = sx + RD_SEL_HANDLE, scy = sy + RD_SEL_HANDLE;   // 手柄中心
  const int ecx = ex + RD_SEL_HANDLE, ecy = ey + RD_SEL_HANDLE;
  // 手柄本体只有 10px，手指点不了那么准：命中圈放到 ~56px 宽、~64px 高。
  const bool onS = std::abs(x - scx) <= 28 && std::abs(y - scy) <= 32;
  const bool onE = std::abs(x - ecx) <= 28 && std::abs(y - ecy) <= 32;
  if (onS && !onE) return 0;
  if (onE && !onS) return 1;
  if (onS && onE) return (st.selEnd > st.selStart) ? 1 : 0;   // 单行里两柄叠在一起：拖终点
  // 不靠近手柄：落在反白块里（首行上沿到末行下沿、首词左沿到末词右沿）就取近的那端。
  const RdWordHit &a = pt.words[st.selStart];
  const RdWordHit &b = pt.words[st.selEnd];
  const int asc = g_rd.getFontAscenderSize(fontId);
  const int lh = g_rd.getLineHeight(fontId);
  const bool inside = y >= a.y - asc && y <= b.y + (lh - asc) && x >= a.x - 4 &&
                      x <= b.x + b.w + 4;
  if (!inside) return -1;
  const int dS = std::abs(x - scx) + std::abs(y - scy);
  const int dE = std::abs(x - ecx) + std::abs(y - ecy);
  return dS <= dE ? 0 : 1;
}

// 拖动一帧（KEY_TOUCH_DRAG）：把被拖的那一端挪到手指底下的词上，返回 0/1；
// 这一帧没在拖（没抓到手柄/没有增量）返回 -1。
// 锚点用**按下点**而不是手柄坐标：hw/input 的第一帧增量就是"按下点→当前点"，
// 从按下点起算，加完这一帧正好落在手指现在的位置。
static int rdSelDragStep(int fontId) {
  int ddx = 0, ddy = 0;
  if (!input_drag_xy(&ddx, &ddy)) return -1;
  if (st.selDrag < 0) {
    int px = 0, py = 0;
    if (!input_press_xy(&px, &py)) return -1;   // 认不出按下点就宁可不拖
    st.selDrag = rdSelDragGrab(g_pageText, fontId, px, py);
    if (st.selDrag < 0) return -1;              // 这一轮拖动整个丢掉
    st.selDragX = px;
    st.selDragY = py;
  }
  st.selDragX += ddx;
  st.selDragY += ddy;
  const int wi = rdWordAtPoint(g_pageText, fontId, st.selDragX, st.selDragY);
  if (wi >= 0) {
    if (st.selDrag == 0) st.selStart = std::min(wi, st.selEnd);
    else st.selEnd = std::max(wi, st.selStart);
  }
  return st.selDrag;
}

// 正文之后画标注层：已有标注的反白高亮 + 正在选择的选区 + 动作浮层。
static void rdDrawOverlays(const RdPageText &pt, int fontId) {
  if (!pt.valid) return;
  for (const auto &an : rdAnchorsOnPage(pt)) {
    if (st.selActive && an.noteIdx == st.selNoteIdx) continue;  // 正在操作的那条改由选区画
    rdDrawSpan(pt, fontId, an.a, an.b);
  }
  if (st.selActive) {
    rdDrawSpan(pt, fontId, st.selStart, st.selEnd);
    rdDrawSelHandles(pt, fontId);
    if (st.selPopup) rdDrawSelPopup(pt, fontId);
  }
}

// ── 翻页性能埋点 ────────────────────────────────────────────────────────
// renderCurrent 末尾那条"翻页耗时"只把一页拆成 绘制/决策/刷屏 三段，而绘制那
// 七八百毫秒里到底是取页、栅格化还是文字地图，从外面看不出来。下面再拆一层，
// 顺带记下这一页有几个图片元素、调用了多少个字形、字形缓存命中多少（ttf_font.c
// 里的 ttf_bench_* 本来就统计这些，只是一直没人调用），以及正文字面是内建还是
// SD 卡上的外部字体——后者会让每个冷字形都真读一次盘，是"发闷变慢"的头号嫌疑。
//
// 开销：ttf_bench 打开时每个字形多两次 esp_timer_get_time()，一页 ~600 字形约
// 1ms，相对 1300ms 可以忽略，所以常开，不做开关。
struct RdPageProf {
  int64_t loadUs = 0;     // Section::loadPage：SD 打开 + 反序列化
  int64_t renderUs = 0;   // page->render：字形栅格化 + 写进帧缓冲
  int64_t textmapUs = 0;  // rdBuildPageText：长按选词用的文字地图
  uint32_t glyphs = 0, hits = 0, misses = 0;
  int64_t rasterUs = 0, fontReadUs = 0;
  // 字库 SD I/O 细分：fontReadUs 里有多少花在 lseek(大文件上 FATFS 要重走簇链)、
  // 多少次读、多少字节。用来分辨"寻道开销"还是"吞吐瓶颈"。
  int64_t fontSeekUs = 0;
  uint32_t fontReadCalls = 0, fontReadBytes = 0;
  // 整页预取（rdWarmPageText）：单独记一笔，好和下面"绘制期间"的 I/O 分开看。
  // 预取做得对的话，这里的读次数应当远小于字形数、而绘制期间的读次数接近 0。
  int64_t warmUs = 0;
  int64_t warmReadUs = 0, warmSeekUs = 0;
  uint32_t warmCalls = 0, warmBytes = 0;
  uint32_t ioBlocks = 0, ioRuns = 0;  // 预取涉及的块数(去重) / 实际发起的读次数
  int64_t ioSpan = 0;                 // 这些块在字体文件里的跨度（字节）
  // 字形缓存占用/额度（KB）。顶到 cache_cap_kb 还大量未命中 → 内存已给足，
  // 瓶颈在别处；占用 < 额度 → 是 PSRAM 余量卡住了，不是静态上限。
  uint32_t cacheKB = 0, cacheCapKB = 0;
  int images = 0;  // 本页图片元素个数（>0 会强制整屏全刷，解码也算在 renderUs 里）
  int valid = 0;   // 本帧是阅读页并且填过数据
};
static RdPageProf s_prof;

// ── 阅读线（正文行间引导线）──────────────────────────────────────────────
// 每行正文下面一条 1px 中灰线，垫在**这一行的字形盒底**和**下一行的盒顶**之间，
// 给横向阅读一个落点参照（抄自参考固件 book_layout_draw_page 的同名功能）。
// 线拉满整个正文栏宽，不随行内文字长短伸缩 —— 参考线的作用是"这一行读到哪"，不是
// 标出文字范围。虚线 19 实 12 空、点线 2 实 11 空，与参考固件同参数。
//
// 用真灰度 0x80（GfxRenderer::drawPixelInk）而不是布尔黑：阅读线不该抢正文的对比度。
// GL16/GC16/8 灰阶三张表都带灰度阶；只有"快刷 DU"的相位簿灰阶少，那一档下它会重一点。
//
// 纵向位置用**实际**行距（下一行基线 − 本行基线）算，不直接用标称值：书里 CSS 的
// line-height、段距都会改它，用标称值会在这些书上歪到贴着字。差值大得离谱（跨块/
// 跨图片/最后一行）时退回标称行距。
static void rdDrawReadingLines(int fontId) {
  if (st.readingLine <= 0 || st.bookKind == 2) return;  // XTC 是整页位图，没有"行"
  if (!g_pageText.valid || g_pageText.words.empty()) return;
  const int asc = g_rd.getFontAscenderSize(fontId);
  const int boxH = g_rd.getLineHeight(fontId);  // 字体自带行盒高（asc + desc）
  if (boxH <= 0) return;
  const int pitch = static_cast<int>(boxH * st.lineSpacing + 0.5f);
  const int desc = std::max(0, boxH - asc);     // 基线 → 行盒底
  const int x0 = bodyMargin();
  const int x1 = g_rd.getScreenWidth() - bodyMargin();
  // 线型：虚线 19 实 12 空、点线 2 实 11 空（与参考固件同参数），实线则整栏一笔到底
  // ——周期取整栏宽度，循环只跑一轮，落进的 run 就覆盖 x0..x1。
  const int span = x1 - x0;
  const int dash = st.readingLine == 1 ? 19 : (st.readingLine == 3 ? span : 2);
  const int period = st.readingLine == 1 ? 31 : (st.readingLine == 3 ? span : 13);
  // 1px 细线的墨量得给足：0x80（16 级里的正中间）在 GL16 之后被压得很淡，1px 行
  // 更是只剩一丝，"几乎看不清"。0x40 是明显更深的中灰，跟正文的纯黑还差好几级，
  // 不会喧宾夺主，但压得住底噪。
  const uint8_t ink = 0x40;
  const int lines = static_cast<int>(g_pageText.lineFirst.size()) - 1;
  for (int li = 0; li < lines; li++) {
    const int a = g_pageText.lineFirst[li], b = g_pageText.lineFirst[li + 1];
    if (b <= a) continue;  // 空行（TXT 的空段落）没有词，也就没有线
    const int baseY = g_pageText.words[a].y;
    int gap = pitch;
    if (li + 1 < lines) {
      const int nextBase = g_pageText.words[g_pageText.lineFirst[li + 1]].y;
      const int d = nextBase - baseY;
      if (d > 0 && d <= boxH * 3) gap = d;
    }
    const int gy = baseY + desc + std::max(0, gap - boxH) / 2;
    if (gy < 0 || gy >= g_rd.getScreenHeight()) continue;
    for (int x = x0; x < x1; x += period) {
      const int run = std::min(dash, x1 - x);
      for (int k = 0; k < run; k++) g_rd.drawPixelInk(x + k, gy, ink);
    }
  }
}

static void renderEpubPage() {
  g_rd.clearScreen();
  const int fontId = BODY_FONT_ID_BASE + st.fontLevel;
  g_pageText = RdPageText();
  st.pageLinks.clear();
  s_prof = RdPageProf();
  s_prof.valid = 1;
  if (st.section) {
    int64_t tA = esp_timer_get_time();
    auto page = st.section->loadPage(st.page);
    s_prof.loadUs = esp_timer_get_time() - tA;
    if (page) {
      for (const auto &el : page->elements)
        if (el->getTag() == TAG_PageImage) s_prof.images++;
      if (page->hasImages()) st.fullRefresh = true;
      // 先把文字地图建出来（它只查排版块，与帧缓冲无关），再用它做整页预取，
      // 最后才画。顺序不能反：预取必须整页一次性做，逐词做就没意义了。
      tA = esp_timer_get_time();
      g_pageText = rdBuildPageText(*page, fontId, bodyMargin(), RD_BODY_TOP);
      s_prof.textmapUs = esp_timer_get_time() - tA;
      // 页面链接矩形跟着页一起抄下来：PageLink 存的是页内坐标（解析时已含 leftInset），
      // 与 PageLine 同一套，所以屏幕坐标 = 页内坐标 + 渲染偏移（bodyMargin / RD_BODY_TOP）。
      st.pageLinks.clear();
      for (const auto &lk : page->links) {
        st.pageLinks.push_back({std::string(lk.href), lk.x + bodyMargin(), lk.y + RD_BODY_TOP, lk.width, lk.height});
      }
      rd_ttf_bench_t ws;
      ttf_bench_begin();
      tA = esp_timer_get_time();
      rdWarmPageText(g_pageText);
      s_prof.warmUs = esp_timer_get_time() - tA;
      ttf_bench_end(&ws);
      s_prof.warmReadUs = ws.read_us;
      s_prof.warmSeekUs = ws.seek_us;
      s_prof.warmCalls = ws.read_calls;
      s_prof.warmBytes = ws.read_bytes;
      s_prof.ioBlocks = ws.io_blocks;
      s_prof.ioRuns = ws.io_runs;
      s_prof.ioSpan = ws.io_span_max > ws.io_span_min
                          ? (int64_t)(ws.io_span_max - ws.io_span_min) + 4096
                          : 0;
      rd_ttf_bench_t bs;
      ttf_bench_begin();
      tA = esp_timer_get_time();
      page->render(g_rd, fontId, bodyMargin(), RD_BODY_TOP);
      s_prof.renderUs = esp_timer_get_time() - tA;
      ttf_bench_end(&bs);
      s_prof.glyphs = bs.glyphs;
      s_prof.hits = bs.hits;
      s_prof.misses = bs.misses;
      s_prof.rasterUs = bs.raster_us;
      s_prof.fontReadUs = bs.read_us;
      s_prof.fontSeekUs = bs.seek_us;
      s_prof.fontReadCalls = bs.read_calls;
      s_prof.fontReadBytes = bs.read_bytes;
      s_prof.cacheKB = bs.cache_kb;
      s_prof.cacheCapKB = bs.cache_cap_kb;
      // 图片解码缓存(.pxc 像素)在 RAM 里的那份副本：整页渲染期间留着，好让同一页
      // 的多趟绘制不再读 SD；这一页画完就还回去。它最大 96KB PSRAM，跨页持有没意义。
      ImageBlock::releaseRenderCache();
    }
  }
  // 阅读线画在正文之上、叠加层之下：标注底色/选词高亮要能盖住它，否则划线穿过高亮块。
  rdDrawReadingLines(fontId);
  drawReaderStatus();
  rdDrawOverlays(g_pageText, fontId);
}

static void renderTxtPage() {
  g_rd.clearScreen();
  const std::string &t = st.txtUtf8;
  int fontId = BODY_FONT_ID_BASE + st.fontLevel;
  int lh = static_cast<int>(g_rd.getLineHeight(fontId) * st.lineSpacing + 0.5f);
  int lpp = linesPerPage();
  int tp = totalPages();
  if (st.txtPage >= tp) st.txtPage = tp - 1;
  if (st.txtPage < 0) st.txtPage = 0;
  size_t base = static_cast<size_t>(st.txtPage) * lpp;
  int y = RD_BODY_TOP;
  int asc = g_rd.getFontAscenderSize(fontId);
  std::vector<std::string> segs;   // 同时留一份给"文字地图"（长按选词用）
  segs.reserve(static_cast<size_t>(lpp));
  s_prof = RdPageProf();
  s_prof.valid = 1;
  // 1) 先切好这一页的行（不画）。原文是边切边画，但整页预取必须先把整页的字收齐，
  //    所以这里拆成"收集 → 预取 → 绘制"三步，绘制结果与原顺序逐像素一致。
  for (int ln = 0; ln < lpp; ln++) {
    size_t li = base + ln;
    if (li >= st.txtLineStarts.size()) break;
    size_t ls = st.txtLineStarts[li];
    size_t le = (li + 1 < st.txtLineStarts.size()) ? st.txtLineStarts[li + 1] : t.size();
    if (le > t.size()) le = t.size();
    if (ls >= le) { segs.emplace_back(); continue; }
    std::string seg = t.substr(ls, le - ls);
    if (!seg.empty() && seg.back() == '\n') seg.pop_back();
    segs.push_back(std::move(seg));
  }
  // 2) 整页预取（TXT 是按行切的，没有词表，直接把行拼起来）。
  {
    size_t total = 0;
    for (const auto &s : segs) total += s.size();
    std::string all;
    if (total > 0) {
      all.reserve(total + 8);
      for (const auto &s : segs) all += s;
    }
    rd_ttf_bench_t ws;
    ttf_bench_begin();
    int64_t tw = esp_timer_get_time();
    rdWarmStrings(RD_TTF_ROLE_CONTENT, all);  // TXT 没有样式，只有内容面
    s_prof.warmUs = esp_timer_get_time() - tw;
    ttf_bench_end(&ws);
    s_prof.warmReadUs = ws.read_us;
    s_prof.warmSeekUs = ws.seek_us;
    s_prof.warmCalls = ws.read_calls;
    s_prof.warmBytes = ws.read_bytes;
    s_prof.ioBlocks = ws.io_blocks;
    s_prof.ioRuns = ws.io_runs;
    s_prof.ioSpan = ws.io_span_max > ws.io_span_min
                        ? (int64_t)(ws.io_span_max - ws.io_span_min) + 4096
                        : 0;
  }
  // 3) 画。y 的推进与收集循环一一对应（每个 seg 一行，含空行）。
  rd_ttf_bench_t bs;
  ttf_bench_begin();
  int64_t tA = esp_timer_get_time();
  for (const auto &seg : segs) {
    if (!seg.empty()) g_rd.drawText(fontId, bodyMargin(), y + asc, seg.c_str(), true);
    y += lh;
  }
  s_prof.renderUs = esp_timer_get_time() - tA;
  ttf_bench_end(&bs);
  s_prof.glyphs = bs.glyphs;
  s_prof.hits = bs.hits;
  s_prof.misses = bs.misses;
  s_prof.rasterUs = bs.raster_us;
  s_prof.fontReadUs = bs.read_us;
  s_prof.fontSeekUs = bs.seek_us;
  s_prof.fontReadCalls = bs.read_calls;
  s_prof.fontReadBytes = bs.read_bytes;
  s_prof.cacheKB = bs.cache_kb;
  s_prof.cacheCapKB = bs.cache_cap_kb;
  tA = esp_timer_get_time();
  g_pageText = rdBuildPageTextTxt(segs, fontId, bodyMargin(), RD_BODY_TOP, lh);
  s_prof.textmapUs = esp_timer_get_time() - tA;
  rdDrawReadingLines(fontId);  // 与 EPUB 同一条阅读线（TXT 的 y 推进就是标称行距）
  drawReaderStatus();
  rdDrawOverlays(g_pageText, fontId);
}

static void renderXtcPage() {
  g_rd.clearScreen();
  s_prof = RdPageProf();
  s_prof.valid = 1;
  if (!st.xtc) { drawCenteredLine(200, "XTC 打开失败"); drawReaderStatus(); return; }
  uint32_t w = st.xtc->getPageWidth();
  uint32_t h = st.xtc->getPageHeight();
  uint8_t depth = st.xtc->getBitDepth();
  if (w == 0 || h == 0) { drawCenteredLine(200, "空页面"); drawReaderStatus(); return; }
  size_t stride = (depth == 1) ? (w + 7) / 8 : (w + 3) / 4;
  size_t bufsize = stride * h;
  // w/h 是从文件头读出来的 uint16，损坏的文件能给出任意大的值；这里的分配失败在
  // 异常关闭的配置下就是 abort(重启)。先按 PSRAM 余量判一次，放不下就按"读不出来"处理。
  if (bufsize == 0 || bufsize > 4 * 1024 * 1024 ||
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < bufsize + 65536) {
    drawCenteredLine(200, "页面过大");
    drawReaderStatus();
    return;
  }
  // 复用同一块缓冲：翻一页就是一次重绘，每帧新开一块 100~200KB 只是白白给堆添乱
  // （分配-释放-再分配，PSRAM 迟早被切碎）。resize 不缩容，容量稳定在一页大小。
  static std::vector<uint8_t> buf;
  buf.resize(bufsize);
  int64_t tA = esp_timer_get_time();
  int rd = st.xtc->loadPage(st.xtcPage, buf.data(), bufsize);
  s_prof.loadUs = esp_timer_get_time() - tA;
  if (rd == 0) {
    drawCenteredLine(200, "页面读取失败");
    drawReaderStatus();
    return;
  }
  int vw = g_rd.getScreenWidth() - 2 * bodyMargin();
  int vh = statusTop() - RD_BODY_TOP - MARGIN;
  float scale = std::min(static_cast<float>(vw) / w, static_cast<float>(vh) / h);
  int dw = static_cast<int>(w * scale), dh = static_cast<int>(h * scale);
  int ox = (g_rd.getScreenWidth() - dw) / 2, oy = RD_BODY_TOP;
  tA = esp_timer_get_time();
  for (int yy = 0; yy < dh; yy++) {
    int sy = static_cast<int>(yy / scale);
    if (sy >= static_cast<int>(h)) sy = h - 1;
    for (int xx = 0; xx < dw; xx++) {
      int sx = static_cast<int>(xx / scale);
      if (sx >= static_cast<int>(w)) sx = w - 1;
      uint8_t v;
      if (depth == 1) v = (buf[sy * stride + sx / 8] >> (7 - (sx % 8))) & 1;
      else v = (buf[sy * stride + sx / 4] >> ((3 - (sx % 4)) * 2)) & 3;
      if (depth == 1) g_rd.drawPixel(ox + xx, oy + yy, v == 0);
      else g_rd.drawGrayscale16Pixel(ox + xx, oy + yy, v == 0 ? 0 : v == 1 ? 5 : v == 2 ? 10 : 15);
    }
  }
  s_prof.renderUs = esp_timer_get_time() - tA;
  drawReaderStatus();
}

static void renderReading() {
  if (st.bookKind == 0) renderEpubPage();
  else if (st.bookKind == 1) renderTxtPage();
  else renderXtcPage();
}

// ── 书架封面网格 ─────────────────────────────────────────────────────────
static int coverTop() { return rdHeadBottom() + 8; }  // == drawTitle / drawTabBar 返回

// ── 顶部搜索栏（书架 / 笔记共用）─────────────────────────────────────────
// 顶标签栏之下一条横栏：左端是占位提示，右端是动作图标（书架 3 个：搜索/刷新/微读；
// 笔记暂时只放搜索）。书籍封面 / 笔记列表都从栏下方起排（rdBarContentTop）。
// 图标都是 5 位 PUA，icon_font_is_icon() 不认，所以直接调 icon_font_draw_sized。
static int rdBarTop() { return coverTop(); }
static int rdBarH() { return uiLineHeight() + 14; }
static int rdBarBottom() { return rdBarTop() + rdBarH(); }
static int rdBarContentTop() { return rdBarBottom() + 6; }

static int rdBarIconPx() { return uiLineHeight(); }
static int rdBarSlotW() { return rdBarIconPx() + 12; }
static int rdBarIconY() { return rdBarTop() + (rdBarH() - rdBarIconPx()) / 2; }
// 第 i 个动作图标（从左往右数）的左上角 x；count = 该界面一共几个。
static int rdBarIconX(int i, int count) {
  return g_rd.getScreenWidth() - MARGIN - (count - i) * rdBarSlotW() + (rdBarSlotW() - rdBarIconPx()) / 2;
}
static int rdBarIconCount(bool shelf) { return shelf ? 3 : 1; }
static uint32_t rdBarIconCp(int i, bool shelf) {
  static const uint32_t kShelf[3] = {TAB_ICON_SEARCH, BAR_ICON_REFRESH, BAR_ICON_WEREAD};
  return shelf ? kShelf[i] : TAB_ICON_SEARCH;
}
// 命中判定放宽一圈：e-ink 没有实时反馈，指尖落点差几个像素很常见。
static int rdBarIconHit(int x, int y, bool shelf) {
  if (y < rdBarTop() - 6 || y > rdBarBottom() + 6) return -1;
  const int count = rdBarIconCount(shelf);
  for (int i = 0; i < count; i++) {
    const int ix = rdBarIconX(i, count);
    if (x >= ix - 8 && x <= ix + rdBarIconPx() + 8) return i;
  }
  return -1;
}
// 栏体（提示文字那一段）命中：点它等价于点放大镜，进搜索。图标那一带不算栏体。
static bool rdBarBodyHit(int x, int y, bool shelf) {
  const int iconsLeft = rdBarIconX(0, rdBarIconCount(shelf)) - 8;
  return y >= rdBarTop() - 6 && y <= rdBarBottom() + 6 && x >= MARGIN && x < iconsLeft;
}

static void rdDrawSearchBar(bool shelf) {
  const int w = g_rd.getScreenWidth();
  const int top = rdBarTop(), h = rdBarH();
  g_rd.drawRect(MARGIN, top, w - 2 * MARGIN, h, true);
  drawLineText(MARGIN + 12, top + (h - uiLineHeight()) / 2, shelf ? "搜索书名" : "搜索笔记", true);
  uint8_t *fb = g_rd.getFrameBuffer();
  const int count = rdBarIconCount(shelf);
  for (int i = 0; i < count; i++) {
    if (fb) icon_font_draw_sized(fb, rdBarIconX(i, count), rdBarIconY(), rdBarIconPx(), rdBarIconPx(),
                                 rdBarIconCp(i, shelf), false, rdBarIconPx());
  }
}

// ── 旧版底部放大镜已下岗：搜索入口挪到上面的搜索栏 ───────────────────────

// 书架风格（设置 → 书架风格）：「自动」是历史行为（横屏 6×2、竖屏 2×2），
// 另外给显式的 2×2 / 3×3 / 列表。每页本数、每行本数都从设置查，不再写死。
enum class ShelfStyle { Auto, G2x2, G3x3, List };
static const char *kShelfStyleKeys[] = {"auto", "2x2", "3x3", "list"};
static const char *kShelfStyleNames[] = {"自动", "2x2", "3x3", "列表"};
static const int kShelfStyleCount = 4;
static ShelfStyle shelfStyle() {
  std::string k = g_settings.getString("reader_shelf_style", "auto");
  for (int i = 0; i < kShelfStyleCount; i++) {
    if (k == kShelfStyleKeys[i]) return static_cast<ShelfStyle>(i);
  }
  return ShelfStyle::Auto;
}
// 书架主界面底部：底部那条状态栏（分隔线 + 选中书名 + 页码）已经去掉，封面/列表一直
// 排到屏幕底边，只留 RD_BOTTOM_INSET 的物理留白。列表行数、网格高度、点按命中都从
// 这里取值，三处必须一致，否则点按会跟画面错行。
static int shelfBottom() { return g_rd.getScreenHeight() - RD_BOTTOM_INSET; }

static int shelfCols() {
  switch (shelfStyle()) {
    case ShelfStyle::G2x2: return 2;
    case ShelfStyle::G3x3: return 3;
    case ShelfStyle::List: return 1;
    default: return g_rd.getScreenWidth() >= 1000 ? 6 : 2;
  }
}
// 列表一行的高度：左侧缩略图约两行 UI 字高，且不低于 64px（再小封面就糊了）。
static int shelfListRowH() {
  int rh = uiLineHeight() * 2 + 10;
  return rh < 64 ? 64 : rh;
}
static int shelfRows() {
  switch (shelfStyle()) {
    case ShelfStyle::G2x2: return 2;
    case ShelfStyle::G3x3: return 3;
    case ShelfStyle::List: {
      int contentH = shelfBottom() - coverTop() - 4;
      return std::max(1, contentH / shelfListRowH());
    }
    default: return 2;
  }
}
static int coverPerPage() { return shelfCols() * shelfRows(); }

static int browserPageCount() {
  int n = static_cast<int>(st.books.size());
  return n == 0 ? 1 : (n + coverPerPage() - 1) / coverPerPage();
}

// 把封面 BMP 按面积平均（box filter）缩到 (dx0,dy0,dw,dh)。drawBitmap 系在本移植中
// 仍是 stub，这里用 Bitmap::readNextRow(Gray8) 直接解码 + drawGrayscale16Pixel 绘制。
// 以前这里是最近邻取样：3~15 个源像素里只留一个 —— 封面上那层抖动图案被采成一粒粒
// 噪点，整体明暗也跟着抖，看着就是"清晰度和对比度都很差"。面积平均让每个源像素按
// 覆盖权重都算进去，降采样越狠（列表模式 45×60）收益越大。
// 行只能顺序读（Bitmap 是流式接口，不能回跳），所以按源行推进：归属同一个输出行的
// 源行累积进 acc/cnt，跨到下一个输出行时求平均并画出。Y 放大时相邻源行之间会空出
// 输出行，用刚画完的那一行补（沿用累积器，不额外读盘）。
// / Area-average (box filter) the cover BMP down to (dx0,dy0,dw,dh). It used to take
// one source pixel out of every 3~15 (nearest neighbour), which sampled the cover's
// dither pattern into speckle and made the tone jump pixel to pixel — exactly the
// "sharpness and contrast both bad" report. Every source pixel now contributes by
// area, so the harder the reduction (45×60 in list layout) the more it pays off.
// Rows can only be read forward (Bitmap streams, no seeking), so the loop walks
// source rows: those mapping to the same output row accumulate, and the average is
// drawn on the transition. Upscaling in Y leaves gaps between source rows; the row
// just drawn fills them (the accumulator is reused, no extra reads).
// 封面提对比度。墨水屏那 16 级灰阶的中间调本来就发闷（波形表里 16 档去重后只剩
// 11 级可分辨），封面又多是浅底细线，上架后就是"整体发灰、看不清细节"。这里在
// 0..255 的域上做一条以 128 为支点的线性拉伸：中间调往两头拉开，亮部压到纯白、
// 暗部压到纯黑 —— 在 4bpp 量化之前做，量化后的档位差也跟着变大。
// 增益写成百分数常量，改一个数就能调：现在 1.45×，仍觉淡就往上加（155/170），
// 觉得暗部糊了就往回退（130）。
static const int kCoverGainPercent = 145;
static inline uint8_t rdCoverContrast(uint8_t v) {
  int g = 128 + (static_cast<int>(v) - 128) * kCoverGainPercent / 100;
  if (g < 0) g = 0;
  if (g > 255) g = 255;
  return static_cast<uint8_t>(g);
}

// 把 bmp 缩放成 dw×dh 的 0..15 灰度写进 out（行优先，dw*dh 字节）。**只算不画** ——
// 画（drawGrayscale16Pixel）交给调用方，因为结果要进 PSRAM 缓存复用（见
// drawCoverThumb）。以前这个函数直接画到帧缓冲，也就没法缓存。
static bool rdBuildCoverThumb(Bitmap &bmp, int dw, int dh, uint8_t *out) {
  const int sw = bmp.getWidth(), sh = bmp.getHeight();
  if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return false;
  const int rowBytes = bmp.getRowBytes();
  std::vector<uint8_t> rowBuf(rowBytes);
  std::vector<uint8_t> data(sw);
  std::vector<uint8_t> opacity(sw);
  // 每列一个和 + 一个计数（uint32 装"亮度×像素数"，够 396×528 用）。
  std::vector<uint32_t> acc(dw, 0), cnt(dw, 0);

  // 平均把边缘糊掉了，这里补一遍**非锐化掩模**（unsharp mask）把边缘还回来。
  // 关键是**在 0..255 的域里做**：原先算完平均就直接 >>4 量化成 0..15 画出去，
  // 16 级本来就离散，再对着它锐化只会放大条带。现在是「平均 → 盒式去噪 → 掩模 →
  // 提对比 → 量化」。掩模用拉普拉斯（看上下左右四个邻居），所以每根输出行都得
  // **晚一行**才画 —— 要等下一行的平均值算出来，才有"下邻居"。
  const int areaRatio = (sw * sh) / std::max(1, dw * dh);
  // 面积平均只把噪声除到 1/√N。缩得不够狠时（4:1、5:1 这种）剩下的 JPEG 噪声还骑在
  // 13/14/15 的量化边界上，纸面就成了"白底撒了一层灰点"——用户说的"底色不干净"。
  // 量化前先做一遍 3×3 盒式平均把它抹掉：主机端拿六张真封面跑同款管线量过，孤立
  // 灰点 27→4、109→1、1152→54 个；而本来就没噪声的（8:1 以上）只差十来个像素，
  // 可忽略。代价是每行三遍 3 抽头平均，且这份结果进缩略图缓存、一本书只算一次。
  std::vector<uint8_t> hbox[3];  // 三行各自的行内 3 抽头平均
  for (int i = 0; i < 3; i++) hbox[i].assign(dw, 255);
  auto hboxRow = [&](std::vector<uint8_t> &dst, const std::vector<uint8_t> &src) {
    for (int x = 0; x < dw; x++) {
      const int a = src[x > 0 ? x - 1 : x];
      const int b = src[x + 1 < dw ? x + 1 : x];
      dst[x] = static_cast<uint8_t>((a + static_cast<int>(src[x]) + b) / 3);
    }
  };
  // 缩得越狠糊得越厉害，力度随面积比加大。**放大（面积比 <1）不锐化**：那种情况下
  // Y 方向是"重复上一行"、X 方向是"沿用左列"，本来就没有真高频，再锐化等于把这两级
  // 阶梯放大成满屏横竖条带（448×612 放大到 492×673 那张封面就是竖纹）。
  const int kSharpen = (areaRatio < 1) ? 0 : 2 + std::min(3, areaRatio / 20);  // 2/8 ～ 5/8

  std::vector<uint8_t> ring[3];  // 三行滚动：0..255 的平均值
  for (int i = 0; i < 3; i++) ring[i].assign(dw, 255);
  int ringDy[3] = {-1, -1, -1};
  int nRaw = 0;    // 已算出的源行组数（= ring 里的行号）
  int nextDy = 0;  // 下一个待画的输出行
  std::vector<uint8_t> prevOut(dw, 255);  // 最近画出去的那一行（gap 要用它补）

  auto put = [&](int y, const std::vector<uint8_t> &row) {
    uint8_t *dst = out + static_cast<size_t>(y) * dw;
    for (int x = 0; x < dw; x++) {
      int g = (row[x] + 8) >> 4;  // 四舍五入到 0..15（>15 夹住；15=白）
      if (g > 15) g = 15;
      dst[x] = static_cast<uint8_t>(g);
    }
  };

  // 画第 k 组源行：它上/下邻居已在 ring 里。hasDown=false 只出现在最后一行。
  auto emit = [&](int k, bool hasDown) {
    const std::vector<uint8_t> &mid = ring[k % 3];
    const std::vector<uint8_t> &up = (k >= 1) ? ring[(k - 1) % 3] : mid;
    const std::vector<uint8_t> &dn = hasDown ? ring[(k + 1) % 3] : mid;
    const int dy = ringDy[k % 3];
    // 三行各做一遍行内平均，再把三行合成 3×3 盒式平均（可分离，所以每像素只有
    // 一次除以 3）；上/下邻居缺失时退化成它自己，与原来的"边界补自己"一致。
    hboxRow(hbox[0], up);
    hboxRow(hbox[1], mid);
    hboxRow(hbox[2], dn);
    const std::vector<uint8_t> &bx = hbox[1], &bu = hbox[0], &bd = hbox[2];
    std::vector<uint8_t> out(dw);
    for (int x = 0; x < dw; x++) {
      const int c = (bu[x] + bx[x] + bd[x]) / 3;
      const int l = (x > 0) ? (bu[x - 1] + bx[x - 1] + bd[x - 1]) / 3 : c;
      const int r = (x + 1 < dw) ? (bu[x + 1] + bx[x + 1] + bd[x + 1]) / 3 : c;
      const int lap = 4 * c - (l + r + bu[x] + bd[x]);
      int v = c + (lap * kSharpen) / 8;
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      out[x] = rdCoverContrast(static_cast<uint8_t>(v));
    }
    for (; nextDy < dy && nextDy < dh; nextDy++) put(nextDy, prevOut);  // Y 放大：重复上一行
    if (nextDy < dh) {
      put(nextDy, out);
      nextDy++;
      prevOut = out;
    }
  };

  int curDy = -1;
  auto finishGroup = [&]() {  // 当前这一组的平均值已定：收进 ring，回头补画它前一组
    std::vector<uint8_t> &raw = ring[nRaw % 3];
    uint8_t gray = 255;  // 没有源列覆盖时沿用的值（X 放大）；列 0 没有则留白
    for (int x = 0; x < dw; x++) {
      if (cnt[x] != 0) gray = static_cast<uint8_t>(acc[x] / cnt[x]);
      raw[x] = gray;
    }
    ringDy[nRaw % 3] = curDy;
    nRaw++;
    if (nRaw >= 2) emit(nRaw - 2, true);
  };

  for (int sy = 0; sy < sh; sy++) {
    if (bmp.readNextRow(data.data(), rowBuf.data(), opacity.data(), Bitmap::RowOutput::Gray8) != BmpReaderError::Ok)
      return false;
    const int dy = sy * dh / sh;  // 本行归属的输出行（随 sy 单调不减）
    if (dy != curDy) {
      if (curDy >= 0) finishGroup();
      std::fill(acc.begin(), acc.end(), 0u);
      std::fill(cnt.begin(), cnt.end(), 0u);
      curDy = dy;
    }
    // X 轴同样按"源像素归属哪个输出列"来累加（与 Y 轴同一套映射，放大时同一个
    // 输出列会收到多个源列，缩小时一个源列只进一列），这样两边不会出现空档。
    for (int sx = 0; sx < sw; sx++) {
      if (opacity[sx] == 0) continue;  // 透明像素不参与平均
      const int dx = sx * dw / sw;
      acc[dx] += data[sx];
      cnt[dx]++;
    }
  }
  if (curDy >= 0) finishGroup();
  if (nRaw > 0) emit(nRaw - 1, false);  // 最后一行没有下邻居，按它自己补
  for (; nextDy < dh; nextDy++) put(nextDy, prevOut);
  return true;
}

// ── 书架封面缩略图缓存 ────────────────────────────────────────────────────
// 「进阅读模式先卡一下」的病根就在上面这套缩放：renderCurrent() 每次都要把整架封面
// 重读盘 + 面积平均 + 非锐化 + 提对比。横屏一页 6×2 = 12 本，每本 cover_v2.bmp 约
// 170KB，实测量出来书架渲染 3.0s（刷屏另算 0.4s）。可这套像素在源封面不变时是**不变
// 的** —— 降采样结果存下来，第二次起直接 blit，SD 一个字节都不用读。
//
// 键取 (源路径, 目标格子宽, 目标格子高)：同一本书在网格（约 168×224）和列表
// （45×60）里目标尺寸不同，各存一份。封面被重新生成时（generateCoverForOpenedBook）
// 显式作废对应条目，见 rdCoverThumbForget。
//
// 像素放 PSRAM：一页 12 本 × 约 37KB ≈ 450KB，内部 RAM 经不起这么占（见内部 RAM
// 挤压那条）。超预算就整个清掉重建 —— 清一次也就是再花 3 秒，比做 LRU 简单得多，
// 正常使用根本碰不到。
struct RdCoverThumb {
  std::string path;
  int boxW = 0, boxH = 0;
  int dw = 0, dh = 0, ox = 0, oy = 0;  // 缩略图尺寸 + 在格子里的居中偏移
  uint8_t *pix = nullptr;              // dw*dh，逐字节 0..15；PSRAM
  RdCoverThumb() = default;
  RdCoverThumb(const RdCoverThumb &) = delete;
  RdCoverThumb &operator=(const RdCoverThumb &) = delete;
  RdCoverThumb(RdCoverThumb &&o) noexcept { take(o); }
  RdCoverThumb &operator=(RdCoverThumb &&o) noexcept {
    if (this != &o) {
      release();
      take(o);
    }
    return *this;
  }
  ~RdCoverThumb() { release(); }

 private:
  void release() {
    if (pix) heap_caps_free(pix);
    pix = nullptr;
  }
  void take(RdCoverThumb &o) {
    path = std::move(o.path);
    boxW = o.boxW; boxH = o.boxH;
    dw = o.dw; dh = o.dh; ox = o.ox; oy = o.oy;
    pix = o.pix; o.pix = nullptr;
  }
};
static std::vector<RdCoverThumb> s_coverThumbs;
static size_t s_coverThumbBytes = 0;
static const size_t kCoverThumbBudget = 3u * 1024 * 1024;  // PSRAM 预算

// 封面被重新生成后作废对应的缓存条目（路径是 <book cache dir>/cover_v2.bmp）。
static void rdCoverThumbForget(const std::string &bmpPath) {
  for (size_t i = 0; i < s_coverThumbs.size();) {
    if (s_coverThumbs[i].path == bmpPath) {
      s_coverThumbBytes -= static_cast<size_t>(s_coverThumbs[i].dw) * s_coverThumbs[i].dh;
      s_coverThumbs.erase(s_coverThumbs.begin() + i);
    } else {
      ++i;
    }
  }
}

// 取（必要时算）path 这本封面缩到"格子大小 boxW×boxH"里的缩略图，然后 blit 到
// (boxX,boxY) 那个格子左上角。返回是否画出了真实封面（false=交给占位/空处理）。
static bool drawCoverThumb(const std::string &path, int boxX, int boxY, int boxW, int boxH) {
  if (path.empty() || boxW <= 0 || boxH <= 0) return false;
  auto blit = [&](const RdCoverThumb &t) {
    for (int y = 0; y < t.dh; y++) {
      const uint8_t *row = t.pix + static_cast<size_t>(y) * t.dw;
      for (int x = 0; x < t.dw; x++) {
        g_rd.drawGrayscale16Pixel(boxX + t.ox + x, boxY + t.oy + y, row[x]);
      }
    }
  };
  for (const auto &t : s_coverThumbs) {
    if (t.pix && t.boxW == boxW && t.boxH == boxH && t.path == path) {
      blit(t);
      return true;
    }
  }
  if (!Storage.exists(path.c_str())) return false;
  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) return false;
  Bitmap bmp(f);
  if (bmp.parseHeaders() != BmpReaderError::Ok || bmp.getWidth() <= 0 || bmp.getHeight() <= 0) return false;
  const int sw = bmp.getWidth(), sh = bmp.getHeight();
  const float scale = std::min(static_cast<float>(boxW) / sw, static_cast<float>(boxH) / sh);
  int dw = static_cast<int>(sw * scale), dh = static_cast<int>(sh * scale);
  if (dw < 1) dw = 1;
  if (dh < 1) dh = 1;
  const size_t need = static_cast<size_t>(dw) * dh;
  uint8_t *pix = static_cast<uint8_t *>(heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!pix) pix = static_cast<uint8_t *>(heap_caps_malloc(need, MALLOC_CAP_8BIT));  // PSRAM 不够退内部
  if (!pix) return false;  // 算不出来就什么都不画（占位分支会兜底）
  if (!rdBuildCoverThumb(bmp, dw, dh, pix)) {
    heap_caps_free(pix);
    return false;
  }
  if (s_coverThumbBytes + need > kCoverThumbBudget) {  // 满了整个清掉（不是 LRU，够用）
    s_coverThumbs.clear();
    s_coverThumbBytes = 0;
  }
  RdCoverThumb t;
  t.path = path;
  t.boxW = boxW; t.boxH = boxH;
  t.dw = dw; t.dh = dh; t.ox = (boxW - dw) / 2; t.oy = (boxH - dh) / 2;
  t.pix = pix;
  s_coverThumbBytes += need;
  s_coverThumbs.push_back(std::move(t));
  blit(s_coverThumbs.back());
  return true;
}

// 画一个封面单元，返回是否画出了真实封面（false=占位）。
static bool drawCoverCell(const BookEntry &b, int cx, int cy, int cellW, int cellH, bool selected) {
  // 格内留白取格子的 8%（上下限 8..20px）：封面比"顶满格子"略小一圈，
  // 一页（竖屏 2×2 / 横屏 6×2）都能完整显示，封面不贴格线。只改封面大小，
  // 不改每页本数。
  int pad = std::min(cellW, cellH) * 8 / 100;
  if (pad < 8) pad = 8;
  if (pad > 20) pad = 20;
  // 封面 3:4（宽:高）：在可用区域内取最大 3:4 矩形并居中。
  int availW = cellW - 2 * pad, availH = cellH - 2 * pad;
  int k = std::min(availW / 3, availH / 4);
  if (k < 1) k = 1;
  int boxW = 3 * k, boxH = 4 * k;
  int boxX = cx + (cellW - boxW) / 2, boxY = cy + (cellH - boxH) / 2;
  const bool drew = drawCoverThumb(coverBmpPathFor(b), boxX, boxY, boxW, boxH);
  if (!drew) {
    g_rd.drawRect(boxX, boxY, boxW, boxH, true);
    std::string nm = b.name;
    size_t dot = nm.rfind('.');
    if (dot != std::string::npos) nm = nm.substr(0, dot);
    auto lines = g_rd.wrappedText(uiFontId(), nm.c_str(), boxW - 12, 3);
    int lh = uiLineHeight() + 2;
    int ty = boxY + (boxH - static_cast<int>(lines.size()) * lh) / 2;
    for (size_t i = 0; i < lines.size(); i++) {
      int tw = g_rd.getTextWidth(uiFontId(), lines[i].c_str());
      drawLineText(boxX + (boxW - tw) / 2, ty + static_cast<int>(i) * lh, lines[i].c_str(), true);
    }
  }
  if (selected) {
    // 选中样式：封面外描 2px 黑边 + 封面下沿一条反白标题条。网格样式本来整个格子里
    // 只有封面、看不到书名（drawCoverCell 只在封面位图缺失时才拿名字充数），这条标题
    // 条就是"当前选中的是哪本"的唯一文字反馈——点一下选中、再点一下打开的交互要靠它。
    g_rd.drawRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, true);
    g_rd.drawRect(boxX - 1, boxY - 1, boxW + 2, boxH + 2, true);
    std::string nm = b.name;
    size_t dot = nm.rfind('.');
    if (dot != std::string::npos) nm = nm.substr(0, dot);
    const int barH = uiLineHeight() + 4;
    const int barY = boxY + boxH - barH;
    g_rd.fillRect(boxX, barY, boxW, barH, true);
    const std::string shown = g_rd.truncatedText(uiFontId(), nm.c_str(), boxW - 8);
    drawLineText(boxX + 4, barY + 2, shown.c_str(), false);
  }
  return drew;
}

// 列表样式的一行：左侧 3:4 缩略图，右侧书名（最多两行）+ 类型后缀。
static void drawListRow(const BookEntry &b, int cx, int cy, int cw, int ch, bool selected) {
  const int pad = 6;
  if (selected) g_rd.drawRect(cx + 1, cy + 1, cw - 2, ch - 2, true);
  const int innerX = cx + pad + (selected ? 2 : 0);
  int thumbH = ch - 2 * pad - (selected ? 4 : 0);
  if (thumbH < 24) thumbH = 24;
  int thumbW = thumbH * 3 / 4;
  int ty = cy + (ch - thumbH) / 2;
  drawCoverThumb(coverBmpPathFor(b), innerX, ty, thumbW, thumbH);
  g_rd.drawRect(innerX, ty, thumbW, thumbH, true);  // 描边：弱封面/占位都看得清边界

  int textX = innerX + thumbW + 12;
  int textW = cx + cw - pad - textX;
  if (textW < 16) textW = 16;
  std::string nm = b.name;
  size_t dot = nm.rfind('.');
  if (dot != std::string::npos) nm = nm.substr(0, dot);
  auto lines = g_rd.wrappedText(uiFontId(), nm.c_str(), textW, 2);
  const int lh = uiLineHeight() + 2;
  const char *kindName = b.kind == 0 ? "EPUB" : (b.kind == 1 ? "TXT" : "XTC");
  int blockH = static_cast<int>(lines.size()) * lh + uiLineHeight();
  int ty0 = cy + (ch - blockH) / 2;
  for (size_t i = 0; i < lines.size(); i++) {
    drawLineText(textX, ty0 + static_cast<int>(i) * lh, lines[i].c_str(), true);
  }
  drawLineText(textX, ty0 + static_cast<int>(lines.size()) * lh, kindName, true);
}

static void renderBrowser() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  drawTabBar();  // 主界面根标签：书架 / 文件 / 笔记 / 设置
  rdDrawSearchBar(true);   // 顶部搜索栏（搜索 / 刷新 / 微读）
  int top = rdBarContentTop();
  if (st.books.empty()) {
    drawCenteredLine(g_rd.getScreenHeight() / 2, "未找到电子书");
    drawCenteredLine(g_rd.getScreenHeight() / 2 + uiLineHeight() + 4, "请将 .epub/.txt/.xtc 放入 SD 卡");
  } else if (shelfStyle() == ShelfStyle::List) {
    int contentW = w - 2 * MARGIN;
    int rowH = shelfListRowH();
    int page = clampI(st.sel / coverPerPage(), 0, browserPageCount() - 1);
    int base = page * coverPerPage();
    for (int r = 0; r < shelfRows(); r++) {
      int idx = base + r;
      if (idx >= static_cast<int>(st.books.size())) break;
      drawListRow(st.books[idx], MARGIN, top + r * rowH, contentW, rowH - 4, idx == st.sel);
    }
  } else {
    int contentW = w - 2 * MARGIN;
    int contentH = shelfBottom() - top - 4;
    int cellW = contentW / shelfCols();
    int cellH = contentH / shelfRows();
    int page = clampI(st.sel / coverPerPage(), 0, browserPageCount() - 1);
    int base = page * coverPerPage();
    for (int r = 0; r < shelfRows(); r++) {
      for (int c = 0; c < shelfCols(); c++) {
        int idx = base + r * shelfCols() + c;
        if (idx >= static_cast<int>(st.books.size())) break;
        int cx = MARGIN + c * cellW;
        int cy = top + r * cellH;
        drawCoverCell(st.books[idx], cx, cy, cellW, cellH, idx == st.sel);
      }
    }
  }
  // 底部状态栏（原来的"选中书名 + 第 X/Y 页"）已去掉：书名在封面选中框里看得到，
  // 页码对书架没意义。腾出的高度让封面直接排到屏幕底边（见 shelfBottom）。
}

// 书架搜索栏右端的动作：0=搜索 1=刷新（重扫书库）2=微读。
// 微读那条与设置页「微信读书」入口同一套状态准备；handleWeread 的 Esc 是 switchTab(0)，
// 所以从书架进来回退也直接落回书架（不依赖 retMode）。
static void rdShelfBarAction(int i) {
  if (i == 0) { rdEnterSearch(RdMode::ShelfSearch); return; }
  if (i == 1) {
    scanBooks();
    st.sel = clampI(st.sel, 0, std::max(0, static_cast<int>(st.books.size()) - 1));
    st.dirty = 1;
    return;
  }
  if (i == 2) {
    st.vkVisible = false;
    st.weSel = 0;
    st.weScroll = 0;
    st.weStatus.clear();
    st.weShelfLoaded = false;
    st.mode = RdMode::Weread;
    st.fullRefresh = true;
    st.dirty = 1;
  }
}

// ── 最近阅读 ─────────────────────────────────────────────────────────────
// 存到 settings（每键一文件），每行 "kind|title|path"，最新在前，最多 kMaxRecent 条。
static const int kMaxRecent = 10;

static void saveRecent() {
  std::string s;
  for (auto &b : st.recent) {
    std::string t = b.name;
    for (auto &c : t) if (c == '|' || c == '\n') c = ' ';
    s += std::to_string(b.kind) + "|" + t + "|" + b.path + "\n";
  }
  g_settings.setString("reader_recent", s);
}

static void loadRecent() {
  st.recent.clear();
  std::string s = g_settings.getString("reader_recent");
  size_t pos = 0;
  while (pos < s.size()) {
    size_t nl = s.find('\n', pos);
    if (nl == std::string::npos) nl = s.size();
    std::string line = s.substr(pos, nl - pos);
    pos = nl + 1;
    if (line.empty()) continue;
    size_t p1 = line.find('|');
    if (p1 == std::string::npos) continue;
    size_t p2 = line.find('|', p1 + 1);
    if (p2 == std::string::npos) continue;
    BookEntry b;
    b.kind = atoi(line.substr(0, p1).c_str());
    b.name = line.substr(p1 + 1, p2 - p1 - 1);
    b.path = line.substr(p2 + 1);
    if (!b.path.empty()) st.recent.push_back(std::move(b));
  }
}

static void pushRecent(const std::string &path, int kind, const std::string &title) {
  if (path.empty()) return;
  // 已存在则移到最前（去重）。
  for (auto it = st.recent.begin(); it != st.recent.end(); ++it) {
    if (it->path == path) { st.recent.erase(it); break; }
  }
  st.recent.insert(st.recent.begin(), BookEntry{path, title.empty() ? path : title, kind});
  if (static_cast<int>(st.recent.size()) > kMaxRecent) st.recent.resize(kMaxRecent);
  saveRecent();
}

static void renderRecent() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int top = drawTitle("最近阅读");
  int n = static_cast<int>(st.recent.size());
  if (n == 0) {
    drawCenteredLine(g_rd.getScreenHeight() / 2, "暂无阅读记录");
    drawFooter("Esc 返回");
    return;
  }
  int itemH = uiLineHeight() + 8;
  int viewH = statusTop() - top - 8;
  int maxRows = std::max(1, viewH / itemH);
  int start = clampI(st.recentSel - maxRows / 2, 0, std::max(0, n - maxRows));
  for (int i = 0; i < maxRows && start + i < n; i++) {
    int idx = start + i;
    std::string label = g_rd.truncatedText(uiFontId(), st.recent[idx].name.c_str(), w - 2 * MARGIN);
    int y = top + i * itemH;
    if (idx == st.recentSel) { g_rd.fillRect(0, y, w, itemH, true); drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, label.c_str(), false); }
    else drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, label.c_str(), true);
  }
  drawFooter("↑↓ 选择  Enter 打开  Esc 返回");
}

static void handleRecent(int key) {
  int n = static_cast<int>(st.recent.size());
  if (key == 0x1B) { st.mode = RdMode::Browser; st.fullRefresh = true; st.dirty = 1; return; }
  if (key == KEY_LONG_CONFIRM) { st.mode = RdMode::Browser; st.fullRefresh = true; st.dirty = 1; return; }
  if (n == 0) return;
  if (key == KEY_UP) { st.recentSel = std::max(0, st.recentSel - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.recentSel = std::min(n - 1, st.recentSel + 1); st.dirty = 1; return; }
  if (key == KEY_PAGE_UP) { st.recentSel = std::max(0, st.recentSel - 8); st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.recentSel = std::min(n - 1, st.recentSel + 8); st.dirty = 1; return; }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      int top = coverTop();
      int itemH = uiLineHeight() + 8;
      int viewH = statusTop() - top - 8;
      int maxRows = std::max(1, viewH / itemH);
      int start = clampI(st.recentSel - maxRows / 2, 0, std::max(0, n - maxRows));
      int row = start + (y - top) / itemH;
      if (row >= 0 && row < n) st.recentSel = row;
    }
    BookEntry b = st.recent[st.recentSel];
    rdShowBusy("正在打开…", b.name);
    if (openBook(b.path, b.kind)) {
      st.mode = RdMode::Reading;
      st.fullRefresh = true;
    } else {
      ESP_LOGE(TAG, "最近阅读打开失败: %s", b.path.c_str());
    }
    st.dirty = 1;
    return;
  }
}

// ── 文件浏览器 ───────────────────────────────────────────────────────────
static bool rdIsImageName(const std::string &n);      // 定义见下方「图片查看器」
static void imgBuildList(const std::string &path);

static void fbScan(const std::string &dir) {
  st.fbPath = dir;
  st.fbEntries.clear();
  st.fbSel = 0;
  st.fmStatus.clear();   // 进新目录/重扫：清掉上一条"已重命名/已删除"提示
  DIR *dp = opendir(dir.c_str());
  if (!dp) return;
  std::vector<BookEntry> dirs, files;
  struct dirent *e;
  while ((e = readdir(dp)) != nullptr) {
    std::string name = e->d_name;
    if (name.empty() || name[0] == '.') continue;  // 跳过隐藏项（含 .crossmux）
    std::string full = dir + "/" + name;
    struct stat sb;
    if (stat(full.c_str(), &sb) != 0) continue;
    if (S_ISDIR(sb.st_mode)) {
      dirs.push_back({full, name, -1});
    } else if (S_ISREG(sb.st_mode)) {
      int kind = -1;
      if (endsWith(name, ".epub")) kind = 0;
      else if (endsWith(name, ".txt")) kind = 1;
      else if (endsWith(name, ".xtc")) kind = 2;
      else if (rdIsImageName(name)) kind = 3;
      else kind = 4;   // 其它任何普通文件：列出来、能改名/删除/复制/剪切，但打不开（见 fbOpenEntry）
      files.push_back({full, name, kind});
    }
  }
  closedir(dp);
  auto byName = [](const BookEntry &a, const BookEntry &b) { return a.name < b.name; };
  std::sort(dirs.begin(), dirs.end(), byName);
  std::sort(files.begin(), files.end(), byName);
  st.fbEntries = std::move(dirs);
  st.fbEntries.insert(st.fbEntries.end(), files.begin(), files.end());
}

// 进入文件标签：重扫当前目录。三个入口（标签栏点按、←→ 换标签、恢复上次标签）都得过
// 这一道。原先只有书架菜单的「文件浏览」那条路会扫描，从标签直接进来（或重启后恢复到
// 文件标签）时 st.fbEntries 是空的 —— 列表空白，而且空列表会提前 return 吃掉点按，
// 连标签都切不走，看上去就是"卡死"。
static void rdEnterFileTab() {
  const int keep = st.fbSel;   // 换标签来回切时保住光标位置
  fbScan(st.fbPath.empty() ? std::string("/sdcard") : st.fbPath);
  st.fbSel = clampI(keep, 0, std::max(0, static_cast<int>(st.fbEntries.size()) - 1));
  st.tab = 1;
  st.mode = RdMode::FileBrowser;
  st.fullRefresh = true;
  st.dirty = 1;
}

// ── 文件浏览页的浮动按钮：网络文件管理 ───────────────────────────────────
// 右下角一个地球按钮：点一下把 web 文件管理服务立起来（手机/电脑浏览器打开
// http://<IP>/ 就能看/传 /sdcard 下的文件），再点一下停掉。服务跑在 httpd 自己的
// 任务里，界面不会被它占住 —— 但**屏幕会一直不动**，传大文件时看着就像死机，
// 所以底栏会跟着报传输进度（进度源在 file_manager_server.cpp，见 rdNetTick）。
static int fbFabBoxPx() { return TAB_ICON_PX + 12; }   // 按钮外框比字形大一圈
static int fbFabGlyphPx() { return TAB_ICON_PX; }
static int fbFabX() { return g_rd.getScreenWidth() - MARGIN - fbFabBoxPx(); }
static int fbFabY() { return tabBottom() - 14 - fbFabBoxPx(); }

// 命中区比按钮放宽 8px（e-ink 手指落点糙）。
static bool fbFabHit(int x, int y) {
  const int pad = 8, b = fbFabBoxPx();
  return x >= fbFabX() - pad && x <= fbFabX() + b + pad &&
         y >= fbFabY() - pad && y <= fbFabY() + b + pad;
}

// 「新建文件夹」浮动按钮：叠在网络按钮正上方（右下角竖着一列，都靠右对齐）。
static int fbNewFabY() { return fbFabY() - fbFabBoxPx() - 12; }
static bool fbNewFabHit(int x, int y) {
  const int pad = 8, b = fbFabBoxPx();
  const int bx = fbFabX(), by = fbNewFabY();
  return x >= bx - pad && x <= bx + b + pad && y >= by - pad && y <= by + b + pad;
}

// 程序化画一个 "+"：图标子集里没有 plus 字形，为它重裁一次字体不划算，两条实心矩形更省事。
static void fbDrawNewFab() {
  const int b = fbFabBoxPx();
  const int x = fbFabX(), y = fbNewFabY();
  g_rd.drawRect(x, y, b, b, true);
  const int cx = x + b / 2, cy = y + b / 2;
  const int arm = b / 2 - 10;                 // 笔画半长
  const int t = std::max(2, b / 12);          // 笔画粗细
  g_rd.fillRect(cx - arm, cy - t / 2, 2 * arm, t, true);
  g_rd.fillRect(cx - t / 2, cy - arm, t, 2 * arm, true);
}

// 「+」的动作定义在后面的「文件长按菜单」段（那边才拿到 FileRename 的输入页设施）。
static void fmBeginMkdir();

// 「刷新」浮动按钮：叠在「+」之上，三个按钮在右下角竖成一列。
// 为什么要它：目录里列的是 SD 卡上的实体文件，用读卡器、或者用右下角那个地球按钮
// （网络文件管理）从手机/电脑上增删过之后，本机这份 st.fbEntries 还是旧的，别的界面
// 也没有"重新扫描"的入口。点一下重扫当前目录即可。
static int fbRefreshFabY() { return fbNewFabY() - fbFabBoxPx() - 12; }
static bool fbRefreshFabHit(int x, int y) {
  const int pad = 8, b = fbFabBoxPx();
  const int bx = fbFabX(), by = fbRefreshFabY();
  return x >= bx - pad && x <= bx + b + pad && y >= by - pad && y <= by + b + pad;
}

// 图标复用顶部搜索栏那个 md-refresh（BAR_ICON_REFRESH，已在图标子集里）。
static void fbDrawRefreshFab() {
  const int b = fbFabBoxPx(), g = fbFabGlyphPx();
  const int x = fbFabX(), y = fbRefreshFabY();
  g_rd.drawRect(x, y, b, b, true);
  uint8_t *fb = g_rd.getFrameBuffer();
  if (fb) icon_font_draw_sized(fb, x + (b - g) / 2, y + (b - g) / 2, g, g, BAR_ICON_REFRESH, false, g);
}

// 重扫当前目录。fbScan 会把 fbSel 归零，所以先记下选中项的路径、扫完再选回去——
// 刷新只该更新列表内容，不该把用户的位置弄丢。
static void fbRefreshAction() {
  std::string keep;
  if (st.fbSel >= 0 && st.fbSel < static_cast<int>(st.fbEntries.size()))
    keep = st.fbEntries[st.fbSel].path;
  const std::string dir = st.fbPath;
  const int before = static_cast<int>(st.fbEntries.size());
  fbScan(dir);
  if (!keep.empty()) {
    for (size_t i = 0; i < st.fbEntries.size(); i++)
      if (st.fbEntries[i].path == keep) { st.fbSel = static_cast<int>(i); break; }
  }
  ESP_LOGI(TAG, "文件刷新: %s (%d → %d 项)", dir.c_str(), before,
           static_cast<int>(st.fbEntries.size()));
  rdShowFloat("已刷新", dir, 1500);
  st.dirty = 1;
}

// 服务开着就画成实心反白，一眼能看出状态；关着是线框。
static void fbDrawFab() {
  const int b = fbFabBoxPx(), g = fbFabGlyphPx();
  const int x = fbFabX(), y = fbFabY();
  const bool on = st.netServerUp;
  if (on) g_rd.fillRect(x, y, b, b, true);
  else g_rd.drawRect(x, y, b, b, true);
  uint8_t *fb = g_rd.getFrameBuffer();
  if (fb) icon_font_draw_sized(fb, x + (b - g) / 2, y + (b - g) / 2, g, g, FAB_ICON_WEB, on, g);
}

// 传输进度文案；没在传就返回空串。总数未知（打包下载算不出来）时只报已传字节。
static std::string rdNetXferText() {
  const FmXfer *x = file_manager_get_xfer();
  if (!x->active) return std::string();
  // 名字是 httpd 任务写的，读的时候再复制一份并强制结尾，防止恰好读到改名中途。
  char nm[sizeof(x->name) + 1];
  memcpy(nm, x->name, sizeof(x->name));
  nm[sizeof(x->name)] = '\0';
  const char *verb = (x->kind == 0) ? "接收" : (x->kind == 1) ? "发送" : "打包发送";
  std::string s = std::string(verb) + " " + nm + "  " + humanSize(x->done);
  if (x->total) {
    int pct = (int)((uint64_t)x->done * 100 / (uint64_t)x->total);
    if (pct > 100) pct = 100;
    s += " / " + humanSize(x->total) + "  " + std::to_string(pct) + "%";
  }
  return s;
}

// 确保 WiFi 已连上；失败时把原因写进 st.netStatus。文件浏览页的按钮和
// 「WiFi 传书」界面共用这一套（原来这段逻辑内联在 netShareConnect 里）。
static bool rdWifiEnsure() {
  if (g_wifi.isConnected()) return true;
  std::string ssid = g_settings.wifiSsid();
  std::string pass = g_settings.wifiPassword();
  if (ssid.empty()) {
    st.netStatus = "未配置 WiFi（先到 WiFi 管理填写）";
    return false;
  }
  g_wifi.begin();
  if (!g_wifi.connect(ssid.c_str(), pass.c_str())) {
    st.netStatus = "WiFi 连接失败";
    return false;
  }
  st.netStatus.clear();
  return true;
}

// 起服务后把地址浮出来几秒。地址是给手机/电脑敲的，浮在屏幕正中最显眼，而且
// 到点自己消失，不像常驻一行那样白占地方。
static void fbFabShowAddress() {
  rdShowFloat("网络文件管理已开启", "http://" + g_wifi.getIp() + "/", 8000);
}

static void fbFabAction() {
  const FmXfer *x = file_manager_get_xfer();
  if (x->active) {
    // httpd_stop 是调用方忙等，传到一半停会把主循环冻住几十秒（见 file_manager_server.cpp
    // 里 s_shutdown 的注释），所以传输中拒绝关闭。
    rdShowFloat("正在传输", "传完才能停服务", 3000);
    return;
  }
  if (st.netServerUp) {
    // 服务已经在跑：这一下就是"再看一眼地址"。要停服务用长按（见 handleFileBrowser
    // 的 KEY_TOUCH_LONG）—— 把停服务挂在同一下点按上，想看地址的人会不小心把服务关掉。
    fbFabShowAddress();
    return;
  }
  rdShowBusy("正在启动服务…", std::string());
  if (!rdWifiEnsure()) {
    rdShowFloat(st.netStatus, "", 4000);
    return;
  }
  if (file_manager_server_start(80)) {
    st.netServerUp = true;
    fbFabShowAddress();
  } else {
    rdShowFloat("服务启动失败", "端口被占用?", 5000);
  }
}

// 长按浮动按钮 = 停服务。返回真表示这一下被 FAB 吃掉了（别再当列表长按处理）。
static bool fbFabLongPress() {
  if (!st.netServerUp) return false;
  const FmXfer *x = file_manager_get_xfer();
  if (x->active) {
    rdShowFloat("正在传输", "传完才能停服务", 3000);
    return true;
  }
  file_manager_server_stop();
  st.netServerUp = false;
  rdShowFloat("网络文件管理已停止", "", 3000);
  return true;
}

// 网络传输的心跳。httpd 在自己的任务里收发，主任务这边屏幕一动不动，传大文件时
// 看着像死机 —— 空闲帧里轮询进度，**文案变了才重绘**，并且限流：阅读器的推屏一次
// 几百毫秒，刷太勤会把主循环按住。只在文件浏览 / WiFi 传书两个界面轮询。
static void rdNetTick() {
  if (!st.netServerUp) return;
  if (st.mode != RdMode::FileBrowser && st.mode != RdMode::NetShare) return;
  const FmXfer *x = file_manager_get_xfer();
  const int64_t now = esp_timer_get_time();
  static std::string s_drawn;
  static bool s_wasActive = false;
  static int64_t s_last_us = 0;
  if (s_wasActive && !x->active) {
    // 结束那一拍给条短提示：进度行直接消失会让人以为传失败了。
    s_wasActive = false;
    s_drawn.clear();
    char nm[sizeof(x->name) + 1];
    memcpy(nm, x->name, sizeof(x->name));
    nm[sizeof(x->name)] = '\0';
    rdShowFloat(x->kind == 0 ? "已接收" : "已发送", nm, 4000);
    return;
  }
  if (!x->active) return;
  s_wasActive = true;
  const std::string s = rdNetXferText();
  if (s == s_drawn) return;
  if (now - s_last_us < 1000000) return;   // 限流 1s
  s_last_us = now;
  s_drawn = s;
  st.dirty = 1;
}

// 文件标签：路径面包屑 + 目录/文件列表。以前它是从书架菜单进来的子界面（用 drawTitle
// 占顶栏），现在是 1 号根标签，顶栏换成标签栏，路径下移成正文第一行（fbListTop()）。
static int fbListTop() { return coverTop() + uiLineHeight() + 6; }

// 列表行 ↔ y 的换算。绘制、点按、长按三处共用同一套公式，避免各自算错位。
// 参数 clampI 见文件前段的 clamp 工具。返回 -1 = 不在任何一行（面包屑/空白/越界）。
static int fbRowAtY(int y) {
  const int top = fbListTop();
  const int itemH = uiLineHeight() + 6;
  const int n = static_cast<int>(st.fbEntries.size());
  const int viewH = tabBottom() - top - 8;
  const int maxRows = std::max(1, viewH / itemH);
  const int start = clampI(st.fbSel - maxRows / 2, 0, std::max(0, n - maxRows));
  const int row = start + (y - top) / itemH;
  if (y < top || row < 0 || row >= n) return -1;
  return row;
}

// 打开一个条目：目录往下走、图片进看图器、书进阅读页。列表回车和长按菜单的「打开」
// 共用同一条路径，免得两处行为漂移。
static void fbOpenEntry(const BookEntry &e) {
  if (e.kind < 0) {
    fbScan(e.path);
  } else if (e.kind == 3) {
    imgBuildList(e.path);
    st.imgZoom = 1.0f;
    st.imgPanX = 0.0f;
    st.imgPanY = 0.0f;
    st.imgFromReader = false;  // 从文件标签打开的：Esc 回文件浏览器
    st.mode = RdMode::Image;
    st.fullRefresh = true;
  } else if (e.kind == 4) {
    // 打不开的普通文件（.pdf/.zip/.md…）：直接进详情页，顺带告诉用户长按能改名/复制/删除。
    for (size_t i = 0; i < st.fbEntries.size(); i++)
      if (st.fbEntries[i].path == e.path) { st.fmIdx = static_cast<int>(i); break; }
    st.mode = RdMode::FileInfo;
    st.fullRefresh = true;
    st.dirty = 1;
  } else {
    rdShowBusy("正在打开…", e.name);
    if (openBook(e.path, e.kind)) {
      st.mode = RdMode::Reading;
      st.fullRefresh = true;
    } else {
      ESP_LOGE(TAG, "文件浏览打开失败: %s", e.path.c_str());
    }
  }
}

static void renderFileBrowser() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  drawTabBar();
  // 剪贴板状态跟路径行挤在一行：右边留给「已复制/已剪切: 名字」，左边路径相应收窄。
  std::string clipTag;
  if (!st.fmClipPath.empty()) clipTag = (st.fmClipCut ? "已剪切: " : "已复制: ") + st.fmClipName;
  int clipW = clipTag.empty() ? 0 : g_rd.getTextWidth(uiFontId(), clipTag.c_str()) + 16;
  if (clipW > w / 2) { clipTag = g_rd.truncatedText(uiFontId(), clipTag.c_str(), w / 2); clipW = w / 2; }
  std::string pathLabel = g_rd.truncatedText(uiFontId(), st.fbPath.c_str(), w - 2 * MARGIN - clipW);
  drawLineText(MARGIN, coverTop(), pathLabel.c_str(), true);
  if (!clipTag.empty())
    drawLineText(w - MARGIN - g_rd.getTextWidth(uiFontId(), clipTag.c_str()), coverTop(), clipTag.c_str(), true);
  int top = fbListTop();
  int n = static_cast<int>(st.fbEntries.size());
  if (n == 0) drawCenteredLine(g_rd.getScreenHeight() / 2, "空目录");
  int itemH = uiLineHeight() + 6;
  int viewH = tabBottom() - top - 8;
  int maxRows = std::max(1, viewH / itemH);
  int start = clampI(st.fbSel - maxRows / 2, 0, std::max(0, n - maxRows));
  for (int i = 0; i < maxRows && start + i < n; i++) {
    int idx = start + i;
    const BookEntry &e = st.fbEntries[idx];
    std::string label = (e.kind < 0) ? ("[" + e.name + "]") : e.name;
    int y = top + i * itemH;
    if (idx == st.fbSel) { g_rd.fillRect(0, y, w, itemH, true); drawLineText(MARGIN, y + 3, label.c_str(), false); }
    else drawLineText(MARGIN, y + 3, label.c_str(), true);
  }
  // 传输进度是唯一留在底边的常驻读数（其余瞬时提示都走中间的浮动框）：它每秒都在变，
  // 浮在正中会挡住列表，压在最后一行上反而顺眼，而且**不画分隔线**——标签页下面没有
  // 提示栏了，多一条横线只会让人以为那里还有内容。画在 FAB 之前，FAB 才压得住它。
  if (const std::string xf = rdNetXferText(); !xf.empty()) {
    const int y = tabBottom() - uiLineHeight();
    g_rd.fillRect(0, y, w, uiLineHeight(), false);   // 先擦白，免得和末行文字叠字
    drawLineText(MARGIN, y, xf.c_str(), true);
  }
  // 浮动按钮画在列表之后（压住右下角那格的一部分，这是 FAB 的常态）。
  fbDrawFab();
  fbDrawNewFab();
  fbDrawRefreshFab();
}

static void handleFileBrowser(int key) {
  int n = static_cast<int>(st.fbEntries.size());
  // 点按坐标只读一次（input_tap_xy 读完即清），标签命中和下面的列表行命中共用这一份。
  int tapX = 0, tapY = 0;
  const bool tapped = (key == '\n') && input_tap_xy(&tapX, &tapY);
  // 标签栏命中要放在"空目录提前返回"**前面**：目录空时也得能点标签切走，否则空列表页
  // 会困住用户——点哪儿都没反应，只剩物理键/边缘划能退出去。
  if (tapped) {
    const int t = tabHit(tapX, tapY);
    if (t >= 0) { switchTab(t); return; }
    if (fbNewFabHit(tapX, tapY)) { fmBeginMkdir(); return; }
    if (fbRefreshFabHit(tapX, tapY)) { fbRefreshAction(); return; }
    if (fbFabHit(tapX, tapY)) { fbFabAction(); return; }
  }
  // ←→ 换标签：这个界面现在是 1 号根标签，和其他三个标签的左右键行为一致。
  if (key == KEY_LEFT) { switchTab(st.tab - 1); return; }
  if (key == KEY_RIGHT) { switchTab(st.tab + 1); return; }
  if (key == 0x1B || key == KEY_LONG_CONFIRM || key == KEY_BACK) {
    if (st.fbPath != "/sdcard" && !st.fbPath.empty()) {
      size_t slash = st.fbPath.rfind('/');
      fbScan(slash == 0 ? "/" : st.fbPath.substr(0, slash));
    } else {
      // 已经在卡根：退到书架标签（原来退到 RdMode::Browser，现在标签化后等价于
      // switchTab(0)，顺带把标签高亮也摆正）。
      st.fbSel = 0;
      switchTab(0);
      return;
    }
    st.dirty = 1;
    return;
  }
  if (n == 0) return;
  // 长按列表项 → 弹出上下文菜单（打开/重命名/删除/详情）。长按落点在哪一行就作用于哪
  // 一行：先把 st.fbSel 指过去，菜单里的动作走同一套入口（与键盘"先选中再操作"等价）。
  // 这个键能到达这里，是因为 screen_reader_handle 顶部把 KEY_TOUCH_LONG 展平成 0x1B 的
  // 例外名单里加了 RdMode::FileBrowser（否则长按会被当成"回上一级"）。
  if (key == KEY_TOUCH_LONG) {
    int lx = 0, ly = 0;
    if (input_tap_xy(&lx, &ly)) {
      // 长按右下角浮动按钮 = 停掉网络文件管理（点按是"起服务/再看一眼地址"）。
      if (fbNewFabHit(lx, ly)) return;   // 长按「+」不弹行菜单（点按才建文件夹）
      if (fbRefreshFabHit(lx, ly)) return;   // 长按「刷新」同理，别弹行菜单
      if (fbFabHit(lx, ly) && fbFabLongPress()) return;
      int row = fbRowAtY(ly);
      if (row >= 0) {
        st.fbSel = row;
        st.fmIdx = row;
        st.fmSel = 0;
        st.fmDelArm = false;
        st.fmStatus.clear();
        st.mode = RdMode::FileMenu;
        st.fullRefresh = true;
        st.dirty = 1;
      }
    }
    return;
  }
  if (key == KEY_UP) { st.fbSel = std::max(0, st.fbSel - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.fbSel = std::min(n - 1, st.fbSel + 1); st.dirty = 1; return; }
  if (key == KEY_PAGE_UP) { st.fbSel = std::max(0, st.fbSel - 8); st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.fbSel = std::min(n - 1, st.fbSel + 8); st.dirty = 1; return; }
  if (key == KEY_HOME) { st.fbSel = 0; st.dirty = 1; return; }
  if (key == KEY_END) { st.fbSel = n - 1; st.dirty = 1; return; }
  if (key == '\n') {
    if (tapped) {
      int row = fbRowAtY(tapY);
      if (row >= 0) st.fbSel = row;
      else { st.dirty = 1; return; }   // 点在面包屑/空白：不打开当前项，别误触
    }
    fbOpenEntry(st.fbEntries[st.fbSel]);
    st.dirty = 1;
    return;
  }
}

// ── 文件长按菜单 / 重命名 / 详情 ────────────────────────────────────────
// 菜单作用于 st.fmIdx 锁定的条目（长按哪一行就是哪一行）。删除沿用书架菜单的「两次确认」
// 规矩；重命名走虚拟键盘；详情只读。三者都以 FileBrowser 为返回目标。
// 这一段排在虚拟键盘那套 helper 定义之前，得自己前置声明（renderNoteEdit 在更后面，
// 反过来依赖这里的顺序没关系）。
static void drawVk();
static void vkTap(int x, int y);
static int vkVkTop();
static void rdDrawVkIcon();
static bool rdVkIconHit(int x, int y);
static void feedVkKey(int c);
static void feedVkBackspace();

static const int kFileMenuItemMax = 8;

static const BookEntry *fmTarget() {
  if (st.fmIdx < 0 || st.fmIdx >= static_cast<int>(st.fbEntries.size())) return nullptr;
  return &st.fbEntries[st.fmIdx];
}

// 名字里去掉扩展名 / 取出扩展名（含点）。重命名时只让用户改主干，扩展名照旧保留，
// 免得改成 .epub 之外的尾巴后打开器认不出来。
static std::string nameNoExt(const std::string &n) {
  size_t dot = n.rfind('.');
  return (dot == std::string::npos || dot == 0) ? n : n.substr(0, dot);
}
static std::string nameExt(const std::string &n) {
  size_t dot = n.rfind('.');
  return (dot == std::string::npos || dot == 0) ? std::string() : n.substr(dot);
}

// 菜单动作（顺序即显示顺序）。「打开」只对能打开的条目出现；「粘贴」只在剪贴板非空时出现。
enum { FM_OPEN = 0, FM_COPY, FM_CUT, FM_PASTE, FM_RENAME, FM_DELETE, FM_INFO };
static int fileMenuActions(int *acts, int maxN) {
  const BookEntry *e = fmTarget();
  int n = 0;
  if (e && e->kind != 4) acts[n++] = FM_OPEN;   // kind 4（其它文件）没有可打开的动作
  acts[n++] = FM_COPY;
  acts[n++] = FM_CUT;
  if (!st.fmClipPath.empty()) acts[n++] = FM_PASTE;
  acts[n++] = FM_RENAME;
  acts[n++] = FM_DELETE;
  acts[n++] = FM_INFO;
  (void)maxN;
  return n;
}

// 标签从动作派生：两处都只认 fileMenuActions 的返回，绝不会出现「第 3 行显示剪切、按下去
// 却是粘贴」这种错位。
static void fileMenuLabels(std::vector<std::string> &out) {
  out.clear();
  int acts[kFileMenuItemMax];
  const int n = fileMenuActions(acts, kFileMenuItemMax);
  const BookEntry *e = fmTarget();
  const bool isDir = e && e->kind < 0;
  for (int i = 0; i < n; i++) {
    switch (acts[i]) {
      case FM_OPEN: out.push_back("打开"); break;
      case FM_COPY: out.push_back("复制"); break;
      case FM_CUT: out.push_back("剪切"); break;
      case FM_PASTE: out.push_back(st.fmClipCut ? "粘贴到此处（剪切）" : "粘贴到此处（复制）"); break;
      case FM_RENAME: out.push_back("重命名"); break;
      case FM_DELETE: out.push_back(st.fmDelArm ? "确认删除" : isDir ? "删除文件夹" : "删除"); break;
      case FM_INFO: out.push_back("详情"); break;
      default: break;
    }
  }
}

static void fmBackToBrowser() {
  st.fmDelArm = false;
  st.fmMkdir = false;
  // 从长按菜单做完的动作（已删除/已重命名/失败原因）在这里转成中间的浮动提示：文件
  // 标签页已经不画提示栏了，而这条消息本来就是"刚发生了什么"，看一眼就够。转完就清
  // 掉——重命名输入页还会拿 fmStatus 画自己的错误行，留着会被下一次进来时重复显示。
  if (!st.fmStatus.empty()) {
    rdShowFloat(st.fmStatus, "", 4000);
    st.fmStatus.clear();
  }
  st.mode = RdMode::FileBrowser;
  st.fullRefresh = true;
  st.dirty = 1;
}

static void renderFileMenu() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const BookEntry *e = fmTarget();
  std::string title = e ? g_rd.truncatedText(uiFontId(), e->name.c_str(), w - 2 * MARGIN) : "文件菜单";
  const int top = drawTitle(title.c_str());
  std::vector<std::string> items;
  fileMenuLabels(items);
  const int n = static_cast<int>(items.size());
  const int itemH = uiLineHeight() + 12;
  for (int i = 0; i < n; i++) {
    int y = top + i * itemH;
    if (i == st.fmSel) { g_rd.fillRect(0, y, w, itemH, true); drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, items[i].c_str(), false); }
    else drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, items[i].c_str(), true);
  }
  if (st.fmDelArm) drawFooter("再按一次确认删除（其余键取消）");
  else drawFooter("↑↓ 选择  Enter 确认  Esc 返回");
}

// 从菜单里执行「打开」时先退回文件列表，再走 fbOpenEntry：目录/图片/书各自的模式
// 切换由它负责，省得在这里再判断一遍。
static void fmOpen() {
  const BookEntry e = *fmTarget();
  fmBackToBrowser();
  fbOpenEntry(e);
}

// 进入文件名输入页时把虚拟键盘摆出来（没接蓝牙键盘才摆，有物理键盘时摆出来只是挡屏）。
// 注意不能只置 st.vkVisible 位：可见性的**唯一真相**是 st.vkVisible，但真正决定画不画
// 的是 editor_vk 的 s_visible，两者靠 rdSyncVk() 在绘制前对齐。这里显式调
// editorVkAutoShow() 有三个好处：
//   1) 它顺手做 evkLoadLayout()，把用户上次选的键位布局读回来（阅读模式从不调
//      editorVkInit，只有这条路能拿回布局）；
//   2) 它把 s_userOverride 复原成 false，保留"蓝牙键盘一连上就自动收起"的行为
//      （若改用 editorVkSetVisible(true)，s_userOverride 会被置真，之后再也收不起来）；
//   3) 它清掉上一次输入会话的按键状态——新建文件夹是空串，挂着上一段（比如重命名）的
//      拼音组合/候选会直接往空名字里塞字，所以组合也一并 cancel。
static void fmAutoShowVk() {
  IME::getInstance().cancelComposition();
  rdVkWantShow();
  if (st.vkVisible) editorVkAutoShow();
  else editorVkSetVisible(false);
}

static void fmBeginRename() {
  const BookEntry *e = fmTarget();
  if (!e) { fmBackToBrowser(); return; }
  st.fmMkdir = false;
  st.fmRenameBuf = nameNoExt(e->name);
  st.fmStatus.clear();
  fmAutoShowVk();   // 没接蓝牙键盘就自动弹虚拟键盘
  st.mode = RdMode::FileRename;
  st.fullRefresh = true;
  st.dirty = 1;
}

// 正在读的书落在被移动/删除的路径下时把它放掉（句柄/内存还挂着已不在原处的文件）。
// 删除给的是文件本身，剪切给的是被剪走的目录——两种情况都按「等于或在其子树里」判。
static void fmCloseBookIf(const std::string &pathOrDir) {
  if (st.bookPath.empty()) return;
  const bool inside = (st.bookPath == pathOrDir) ||
                      (st.bookPath.rfind(pathOrDir + "/", 0) == 0);
  if (!inside) return;
  st.epub.reset(); st.section.reset(); st.xtc.reset();
  st.txtUtf8.clear(); st.txtLineStarts.clear();
  st.txtChapterOffsets.clear(); st.txtChapterTitles.clear();
  st.bookPath.clear(); st.bookTitle.clear(); st.bookKind = -1;
}

// 删除是不可逆的：菜单里第一次 Enter 只置位（标签变「确认删除」），再按一次才真删。
static void fmDeleteConfirmed() {
  const BookEntry e = *fmTarget();
  if (e.kind < 0) Storage.removeDir(e.path.c_str());
  else Storage.remove(e.path.c_str());

  fmCloseBookIf(e.path);   // 删的正好是当前打开的书（或它所在的目录）→ 先放掉
  ESP_LOGI(TAG, "文件菜单删除: %s", e.path.c_str());
  fbScan(st.fbPath);
  st.fmStatus = "已删除";
  fmBackToBrowser();
}

// 复制一个文件（4KB 弹跳缓冲；没有现成的 copy 原语，HalStorage 只有 rename/remove）。
static bool rdCopyFile(const std::string &src, const std::string &dst) {
  HalFile in, out;
  if (!Storage.openFileForRead("FBM", src, in)) return false;
  if (!Storage.openFileForWrite("FBM", dst, out)) { in.close(); return false; }
  uint8_t *buf = static_cast<uint8_t *>(malloc(4096));
  if (!buf) { in.close(); out.close(); Storage.remove(dst.c_str()); return false; }
  bool ok = true;
  int r;
  while ((r = in.read(buf, 4096)) > 0) {
    if (out.write(buf, static_cast<size_t>(r)) != static_cast<size_t>(r)) { ok = false; break; }
  }
  if (r < 0) ok = false;
  free(buf);
  in.close();
  out.close();
  if (!ok) Storage.remove(dst.c_str());   // 半截文件不比没有更安全
  return ok;
}

// 递归复制文件/目录。隐藏文件也照搬（列表里看不见，但目录整体搬走时应一起走）。
static bool rdCopyTree(const std::string &src, const std::string &dst) {
  struct stat sb;
  if (stat(src.c_str(), &sb) != 0) return false;
  if (!S_ISDIR(sb.st_mode)) return rdCopyFile(src, dst);
  if (!Storage.mkdir(dst.c_str(), true)) return false;
  DIR *dp = opendir(src.c_str());
  if (!dp) return false;
  bool ok = true;
  struct dirent *e;
  while (ok && (e = readdir(dp)) != nullptr) {
    const std::string nm = e->d_name;
    if (nm == "." || nm == "..") continue;
    ok = rdCopyTree(src + "/" + nm, dst + "/" + nm);
  }
  closedir(dp);
  return ok;
}

static void fmCopy(bool cut) {
  const BookEntry *e = fmTarget();
  if (!e) { fmBackToBrowser(); return; }
  st.fmClipPath = e->path;
  st.fmClipName = e->name;
  st.fmClipCut = cut;
  st.fmStatus = cut ? "已剪切" : "已复制";
  ESP_LOGI(TAG, "文件菜单%s: %s", cut ? "剪切" : "复制", e->path.c_str());
  fmBackToBrowser();
}

// 粘贴到当前目录。剪切优先走 rename（同一张 SD，瞬间完成，还保住 inode）；rename 失败
// 才退回复制 + 删源。复制则一律递归拷贝。粘完清空剪贴板（单个剪贴板的语义）。
static void fmPaste() {
  if (st.fmClipPath.empty()) { fmBackToBrowser(); return; }
  const std::string src = st.fmClipPath;
  // 长按的是一行目录 → 粘进那个目录；长按的是文件/空白 → 粘进当前目录。
  const BookEntry *e = fmTarget();
  const std::string dir = (e && e->kind < 0) ? e->path : st.fbPath;
  const std::string dst = dir + "/" + st.fmClipName;
  if (dst == src) { st.fmStatus = "就是它自己"; fmBackToBrowser(); return; }
  if (dst.rfind(src + "/", 0) == 0) { st.fmStatus = "不能粘进自己的子目录"; fmBackToBrowser(); return; }
  if (Storage.exists(dst.c_str())) { st.fmStatus = "同名已存在"; fmBackToBrowser(); return; }

  bool ok;
  // rename 报成功不等于文件真到了目的地：本机报过"剪切粘贴成功、源没了、目标目录里
  // 却什么都没有"（见日志 万象指掌_stardict.*）。粘完当场 stat 一次对账——**对不上
  // 也不要退回复制**（源这时已经被 rename 删了，复制只会再失败一次），直接报失败，
  // 免得又静默丢一批文件。
  const bool renamed = st.fmClipCut && Storage.rename(src.c_str(), dst.c_str());
  if (renamed) {
    ok = Storage.exists(dst.c_str());
    if (!ok) ESP_LOGE(TAG, "粘贴: rename 报成功但目标不存在: %s", dst.c_str());
  } else {
    ok = rdCopyTree(src, dst);
    if (ok && st.fmClipCut) {
      struct stat sb;
      if (stat(src.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) Storage.removeDir(src.c_str());
      else Storage.remove(src.c_str());
    }
  }
  ESP_LOGI(TAG, "文件粘贴: %s -> %s (%d)", src.c_str(), dst.c_str(), static_cast<int>(ok));
  if (ok) {
    if (st.fmClipCut) fmCloseBookIf(src);
    st.fmClipPath.clear();
    st.fmClipName.clear();
    st.fmClipCut = false;
    fbScan(st.fbPath);   // 刷新当前目录（粘进子目录时列表不变，但重扫代价可忽略）
    st.fmStatus = "已粘贴";
  } else {
    st.fmStatus = "粘贴失败";
  }
  fmBackToBrowser();
}

// 「+」浮动按钮：复用 FileRename 的输入页（fmMkdir 置位后标题/提交行为都不同）。
static void fmBeginMkdir() {
  st.fmMkdir = true;
  st.fmRenameBuf.clear();
  st.fmStatus.clear();
  fmAutoShowVk();
  ESP_LOGI(TAG, "新建文件夹: 虚拟键盘 %s", st.vkVisible ? "自动弹出" : "不弹(蓝牙键盘已连接)");
  st.mode = RdMode::FileRename;
  st.fullRefresh = true;
  st.dirty = 1;
}

static void fmCommitRename() {
  // 去掉首尾空白，避免生成 " 名.epub" 这种看不见的脏名字。
  std::string stem = st.fmRenameBuf;
  size_t b = stem.find_first_not_of(" \t");
  size_t en = stem.find_last_not_of(" \t");
  stem = (b == std::string::npos) ? std::string() : stem.substr(b, en - b + 1);

  if (stem.empty()) { st.fmStatus = "名字不能为空"; fmBackToBrowser(); return; }
  if (stem.find('/') != std::string::npos) { st.fmStatus = "名字不能含 /"; fmBackToBrowser(); return; }

  if (st.fmMkdir) {
    const std::string dir = st.fbPath + "/" + stem;
    if (Storage.exists(dir.c_str())) { st.fmStatus = "同名已存在"; fmBackToBrowser(); return; }
    const bool ok = Storage.mkdir(dir.c_str(), true);
    ESP_LOGI(TAG, "文件菜单新建文件夹: %s (%d)", dir.c_str(), static_cast<int>(ok));
    if (ok) { fbScan(st.fbPath); st.fmStatus = "已新建文件夹"; }
    else st.fmStatus = "新建失败";
    fmBackToBrowser();
    return;
  }

  const BookEntry *e = fmTarget();
  if (!e) { fmBackToBrowser(); return; }
  const std::string oldPath = e->path;
  const std::string oldName = e->name;
  const std::string newName = stem + nameExt(oldName);
  if (newName == oldName) { fmBackToBrowser(); return; }   // 没改，直接回去
  const std::string newPath = st.fbPath + "/" + newName;
  if (Storage.exists(newPath.c_str())) { st.fmStatus = "同名文件已存在"; fmBackToBrowser(); return; }

  bool ok = Storage.rename(oldPath.c_str(), newPath.c_str());
  ESP_LOGI(TAG, "文件菜单重命名: %s -> %s (%d)", oldPath.c_str(), newPath.c_str(), static_cast<int>(ok));
  if (ok) {
    // 正在读的就是这本书 → 跟着改名，否则进度/书签按旧路径存，重开对不上。
    if (st.bookKind == e->kind && st.bookPath == oldPath) st.bookPath = newPath;
    fbScan(st.fbPath);
    st.fmStatus = "已重命名";
  } else {
    st.fmStatus = "重命名失败";
  }
  fmBackToBrowser();
}

static void fileMenuAction(int i) {
  int acts[kFileMenuItemMax];
  const int n = fileMenuActions(acts, kFileMenuItemMax);
  if (i < 0 || i >= n) { fmBackToBrowser(); return; }
  switch (acts[i]) {
    case FM_OPEN: fmOpen(); break;
    case FM_COPY: fmCopy(false); break;
    case FM_CUT: fmCopy(true); break;
    case FM_PASTE: fmPaste(); break;
    case FM_RENAME: fmBeginRename(); break;
    case FM_DELETE:
      if (!st.fmDelArm) { st.fmDelArm = true; st.dirty = 1; break; }
      fmDeleteConfirmed();
      break;
    case FM_INFO:
      st.mode = RdMode::FileInfo;
      st.fullRefresh = true;
      st.dirty = 1;
      break;
    default: fmBackToBrowser(); break;
  }
}

static void handleFileMenu(int key) {
  std::vector<std::string> items;
  fileMenuLabels(items);
  const int n = static_cast<int>(items.size());
  if (key == 0x1B || key == KEY_LONG_CONFIRM || key == KEY_BACK) { fmBackToBrowser(); return; }
  if (key == KEY_UP) { st.fmDelArm = false; st.fmSel = std::max(0, st.fmSel - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.fmDelArm = false; st.fmSel = std::min(n - 1, st.fmSel + 1); st.dirty = 1; return; }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      const int top = coverTop();
      const int itemH = uiLineHeight() + 12;
      const int row = (y - top) / itemH;
      if (row >= 0 && row < n) {
        if (row != st.fmSel) st.fmDelArm = false;   // 换了行就取消待确认
        st.fmSel = row;
      }
    }
    fileMenuAction(st.fmSel);
    return;
  }
}

// ── 文件详情（只读）────────────────────────────────────────────────────
static std::string humanSize(long long bytes) {
  char b[48];
  if (bytes < 1024) snprintf(b, sizeof(b), "%lld B", bytes);
  else if (bytes < 1024 * 1024) snprintf(b, sizeof(b), "%.1f KB", bytes / 1024.0);
  else snprintf(b, sizeof(b), "%.1f MB", bytes / (1024.0 * 1024.0));
  return b;
}

static void renderFileInfo() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = drawTitle("文件详情");
  const BookEntry *e = fmTarget();
  if (!e) { drawFooter("Esc 返回"); return; }

  struct stat sb;
  const bool ok = (stat(e->path.c_str(), &sb) == 0);
  const int lh = uiLineHeight();
  int y = top;
  auto row = [&](const char *k, const std::string &v) {
    drawLineText(MARGIN, y, k, true);
    const int kx = MARGIN + g_rd.getTextWidth(uiFontId(), k) + 12;
    drawLineText(kx, y, g_rd.truncatedText(uiFontId(), v.c_str(), w - kx - MARGIN).c_str(), true);
    y += lh + 8;
  };
  const char *typeStr = e->kind < 0 ? "文件夹" : e->kind == 0 ? "EPUB 电子书"
                        : e->kind == 1 ? "TXT 文本" : e->kind == 2 ? "XTC 漫画"
                        : e->kind == 3 ? "图片" : "文件";
  row("名称", e->name);
  row("类型", typeStr);
  row("大小", ok ? humanSize(static_cast<long long>(sb.st_size)) : "未知");
  if (ok) {
    struct tm tm; localtime_r(&sb.st_mtime, &tm);
    char tbuf[32]; strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", &tm);
    row("修改", tbuf);
  }
  // 路径可能很长，单独占一段，按宽度折行显示。
  y += 4;
  g_rd.drawLine(MARGIN, y, w - MARGIN, y, true);
  y += 8;
  auto pl = g_rd.wrappedText(uiFontId(), e->path.c_str(), w - 2 * MARGIN, 4);
  for (auto &ln : pl) {
    if (y + lh > statusTop()) break;
    drawLineText(MARGIN, y, ln.c_str(), true);
    y += lh + 2;
  }
  drawFooter("Esc 返回");
}

static void handleFileInfo(int key) {
  (void)key;   // 只读：任意键都退回文件列表
  fmBackToBrowser();
}

// ── 重命名输入（虚拟键盘）──────────────────────────────────────────────
static void renderFileRename() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const BookEntry *e = fmTarget();
  const int top = drawTitle(st.fmMkdir ? "新建文件夹" : "重命名");
  const std::string ext = (!st.fmMkdir && e) ? nameExt(e->name) : std::string();

  int y = top;
  drawLineText(MARGIN, y, st.fmMkdir ? "文件夹名" : "新名字", true);
  y += uiLineHeight() + 5;
  // 输入行：主干 + 固定的扩展名 + 光标。扩展名是灰色概念，这里用普通文字标出来即可。
  std::string shown = st.fmRenameBuf.empty() ? "点下方键盘输入" : st.fmRenameBuf;
  drawLineText(MARGIN, y, (shown + "|").c_str(), true);
  const int tx = MARGIN + g_rd.getTextWidth(uiFontId(), st.fmRenameBuf.c_str());
  if (!ext.empty()) drawLineText(tx + g_rd.getTextWidth(uiFontId(), "|"), y, ext.c_str(), true);
  y += uiLineHeight() + 8;
  g_rd.drawLine(MARGIN, y, w - MARGIN, y, true);

  if (!st.fmStatus.empty()) {
    y += 8;
    drawLineText(MARGIN, y, st.fmStatus.c_str(), true);
  }

  if (st.vkVisible) drawVk();
  else drawFooter("回车保存   Esc 取消");
  rdDrawVkIcon();
}

static void handleFileRename(int key) {
  if (key == 0x1B || key == KEY_LONG_CONFIRM) {   // 取消
    IME::getInstance().cancelComposition();
    st.fmRenameBuf.clear();
    st.vkVisible = false;
    fmBackToBrowser();
    return;
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      if (rdVkIconHit(x, y)) { st.vkVisible = !st.vkVisible; st.dirty = 1; return; }
      if (st.vkVisible && y >= vkVkTop()) { vkTap(x, y); st.dirty = 1; return; }
      if (st.vkVisible) { st.dirty = 1; return; }   // 键盘开着时点正文不提交
      fmCommitRename();
      return;
    }
    // 无坐标回车（蓝牙键盘/KEY2）：先把组合落地，再存。
    std::string out;
    IME::getInstance().handleKey('\n', out);
    if (!out.empty() && out != "\n") st.fmRenameBuf += out;
    fmCommitRename();
    return;
  }
  if (key == 0x08) { feedVkBackspace(); st.dirty = 1; return; }
  if (key >= 0x20 && key <= 0x7E) { feedVkKey(key); st.dirty = 1; return; }
}

// ── 图片查看器 ───────────────────────────────────────────────────────────
// 用 vendored 的 PNG/JPEG 解码器直接铺 framebuffer。放大是「取源图上的一个矩形窗口
// （由 imgZoom 定大小、imgPan* 定位置）+ 按窗口重新适配屏幕」，平移靠改窗口原点。
// 两个解码器都实现了 RenderConfig::sourceWindow*：PNG 拿它当 cropLeft/visibleWidth，
// JPEG 把块坐标平移成窗口内坐标（见各自解码器里的注释）。
static bool rdIsImageName(const std::string &n) {
  static const char *exts[] = {".png", ".jpg", ".jpeg"};
  for (const char *x : exts) {
    size_t l = strlen(x);
    if (n.size() > l && endsWith(n, x)) return true;
  }
  return false;
}

// 扫描图片所在目录，填充 imgList 并选中 path。
static void imgBuildList(const std::string &path) {
  st.imgList.clear();
  st.imgSel = 0;
  size_t slash = path.rfind('/');
  std::string dir = (slash == std::string::npos) ? "/sdcard" : path.substr(0, slash);
  DIR *dp = opendir(dir.c_str());
  if (dp) {
    struct dirent *e;
    while ((e = readdir(dp)) != nullptr) {
      std::string name = e->d_name;
      if (name.empty() || name[0] == '.' || !rdIsImageName(name)) continue;
      struct stat sb;
      std::string full = dir + "/" + name;
      if (stat(full.c_str(), &sb) == 0 && S_ISREG(sb.st_mode)) st.imgList.push_back(full);
    }
    closedir(dp);
  }
  std::sort(st.imgList.begin(), st.imgList.end());
  for (size_t i = 0; i < st.imgList.size(); i++) {
    if (st.imgList[i] == path) { st.imgSel = static_cast<int>(i); break; }
  }
}

// ── 图片解码中止 ────────────────────────────────────────────────────────
// 解码期间主循环完全不采样输入，而 cst836u 只返回"当前"按下状态（没有锁存），一次
// 快速滑动会整个落进解码窗口里被丢掉 —— 用户侧就是"划了没反应"。所以让解码内核回调
// 顺便补采一次（input_pending_key() 采到的键暂存，下一次 input_poll() 取走）：一有键
// 就中止这张图的解码，让主循环立刻处理它。8MP 上限下单张解码是秒级，差别能感觉到。
// 触摸是 I²C 读，不能每个 MCU 块都问一遍，限流 50ms（响应仍在一帧之内）。
static bool s_imgDecodeAborted = false;
static bool s_imgPresentSkipped = false;  // 中止的那一帧是半张图，别推屏

static bool imgDecodeAbortPoll(void *) {
  static int64_t s_lastPollUs = 0;
  const int64_t now = esp_timer_get_time();
  if (now - s_lastPollUs < 50000) return false;
  s_lastPollUs = now;
  if (input_pending_key() == 0) return false;
  s_imgDecodeAborted = true;
  return true;
}

// 当前查看器的窗口：源图上可见的那个矩形（比例）。大小由 zoom 定，位置由 imgPan* 定，
// 并夹住不让窗口跑出源图（zoom=1 时窗口=整图，夹取范围 0，自然没有平移余地）。
static void imgWindowRect(float *x, float *y, float *w, float *h) {
  float z = st.imgZoom;
  if (z < 1.0f) z = 1.0f;
  const float s = 1.0f / z;
  float maxOff = 1.0f - s;
  if (maxOff < 0.0f) maxOff = 0.0f;
  float px = st.imgPanX, py = st.imgPanY;
  if (px < 0.0f) px = 0.0f;
  if (px > maxOff) px = maxOff;
  if (py < 0.0f) py = 0.0f;
  if (py > maxOff) py = maxOff;
  *x = px;
  *y = py;
  *w = s;
  *h = s;
}

// 窗口铺到屏幕上占多少像素（居中于内容区）。renderImage 与拖动/捏合共用，
// 免得两边各算一套、比例对不上。
static void imgDstSize(int srcW, int srcH, int *outW, int *outH) {
  int sw = g_rd.getScreenWidth();
  int areaH = g_rd.getScreenHeight() - footerH();
  if (areaH < 1) areaH = 1;
  float wx, wy, ww, wh;
  imgWindowRect(&wx, &wy, &ww, &wh);
  int visW = static_cast<int>(srcW * ww);
  int visH = static_cast<int>(srcH * wh);
  if (visW < 1) visW = 1;
  if (visH < 1) visH = 1;
  float s = std::min(static_cast<float>(sw) / visW, static_cast<float>(areaH) / visH);
  int dstW = static_cast<int>(visW * s);
  int dstH = static_cast<int>(visH * s);
  *outW = (dstW < 1) ? 1 : dstW;
  *outH = (dstH < 1) ? 1 : dstH;
}

static void renderImage() {
  g_rd.clearScreen();
  int sw = g_rd.getScreenWidth();
  int sh = g_rd.getScreenHeight();
  int fh = footerH();
  int areaH = sh - fh;
  if (areaH < 1) areaH = 1;
  st.imgFailed = false;
  s_imgDecodeAborted = false;

  if (st.imgList.empty()) {
    drawCenteredLine(sh / 2, "无图片");
    drawFooter("Esc 返回");
    return;
  }
  const std::string &path = st.imgList[st.imgSel];

  ImageToFramebufferDecoder *dec = ImageDecoderFactory::getDecoder(path);
  ImageDimensions dim = {};
  bool ok = (dec != nullptr) && dec->getDimensions(path, dim) && dim.width > 0 && dim.height > 0;
  if (ok) {
    st.imgSrcW = dim.width;
    st.imgSrcH = dim.height;
    float wx, wy, ww, wh;
    imgWindowRect(&wx, &wy, &ww, &wh);
    int dstW, dstH;
    imgDstSize(dim.width, dim.height, &dstW, &dstH);

    RenderConfig cfg;
    cfg.x = (sw - dstW) / 2;
    cfg.y = (areaH - dstH) / 2;
    cfg.maxWidth = dstW;
    cfg.maxHeight = dstH;
    cfg.useGrayscale = true;
    cfg.useDithering = true;
    cfg.performanceMode = false;
    cfg.useExactDimensions = true;
    cfg.bilinearScaling = st.imageBilinear;
    cfg.sourceWindowX = wx;
    cfg.sourceWindowY = wy;
    cfg.sourceWindowW = ww;
    cfg.sourceWindowH = wh;
    cfg.cachePath.clear();    // 查看器不写像素缓存
    cfg.abortPoll = imgDecodeAbortPoll;  // 解码途中按了键就收手（见上方说明）
    ok = dec->decodeToFramebuffer(path, g_rd, cfg);
    if (!ok && s_imgDecodeAborted) {
      // 用户已经按下了下一个键：这一帧只是半张图，不推屏也不报错，
      // 交给主循环把这个键处理掉、下一轮重新画。
      s_imgPresentSkipped = true;
      return;
    }
    if (!ok) ESP_LOGE(TAG, "图片解码失败: %s", path.c_str());
  }
  st.imgFailed = !ok;
  if (!ok) drawCenteredLine(areaH / 2, "无法解码此图片");

  // 页脚：索引/文件名/缩放
  size_t slash = path.rfind('/');
  std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
  char info[256];
  snprintf(info, sizeof(info), "%d/%d %s  %d%%", st.imgSel + 1,
           static_cast<int>(st.imgList.size()), name.c_str(),
           static_cast<int>(st.imgZoom * 100.0f + 0.5f));
  drawFooter(info);
}

static void imgSet(int idx) {
  int n = static_cast<int>(st.imgList.size());
  if (n <= 0) return;
  st.imgSel = clampI(idx, 0, n - 1);
  st.imgPanX = 0.0f;  // 换图回到画面中心
  st.imgPanY = 0.0f;
  st.dirty = 1;
  st.fullRefresh = true;  // 换图整块重画，全刷清残影
}

// 缩放一档。回到 1×（适应屏幕）时平移归零——窗口=整图时本来也无处可平移，
// 留着旧偏移只会让下一次放大从一个莫名其妙的位置开始。
static void imgZoomBy(float factor) {
  float z = st.imgZoom * factor;
  if (z < 1.0f) z = 1.0f;
  if (z > 8.0f) z = 8.0f;
  st.imgZoom = z;
  if (z <= 1.01f) {
    st.imgZoom = 1.0f;
    st.imgPanX = 0.0f;
    st.imgPanY = 0.0f;
  }
  st.dirty = 1;
  st.fullRefresh = true;
}

static void handleImage(int key) {
  if (key == 0x1B || key == KEY_LONG_CONFIRM) {
    const bool fromReader = st.imgFromReader;
    st.imgFromReader = false;
    st.imgZoom = 1.0f;
    st.imgPanX = 0.0f;
    st.imgPanY = 0.0f;
    if (fromReader) {
      // 从阅读页长按插图进来的：回**原阅读页**（页/章都没动过），不去文件浏览器。
      st.mode = RdMode::Reading;
    } else {
      // 从文件标签打开的：回退时把标签高亮一起摆正。
      st.tab = 1;
      st.mode = RdMode::FileBrowser;
    }
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (st.imgList.empty()) return;

  // 拖动平移。放大后单指的左右划在抬手时会变成 KEY_LEFT/RIGHT（见 hw/input 的
  // 滑动判定）——那会被当成"换下一张"，所以下面把翻页键限定在 1× 时才受理。
  if (key == KEY_TOUCH_DRAG) {
    int ddx = 0, ddy = 0;
    if (!input_drag_xy(&ddx, &ddy)) return;
    if (st.imgZoom <= 1.01f || (ddx == 0 && ddy == 0)) return;
    if (st.imgSrcW <= 0 || st.imgSrcH <= 0) return;
    // 窗口铺满 dstW 像素，而窗口本身是源图的 1/zoom → 屏幕 1px = (1/zoom)/dstW。
    // 内容跟着手指走，所以窗口原点反向移动。
    int dstW = 0, dstH = 0;
    imgDstSize(st.imgSrcW, st.imgSrcH, &dstW, &dstH);
    const float perPxX = (1.0f / st.imgZoom) / (dstW > 0 ? dstW : 1);
    const float perPxY = (1.0f / st.imgZoom) / (dstH > 0 ? dstH : 1);
    st.imgPanX = std::clamp(st.imgPanX - ddx * perPxX, 0.0f, 1.0f - 1.0f / st.imgZoom);
    st.imgPanY = std::clamp(st.imgPanY - ddy * perPxY, 0.0f, 1.0f - 1.0f / st.imgZoom);
    st.dirty = 1;
    st.fullRefresh = true;
    return;
  }

  if (key == KEY_PINCH_IN) { imgZoomBy(1.25f); return; }
  if (key == KEY_PINCH_OUT) { imgZoomBy(1.0f / 1.25f); return; }
  if (key == KEY_UP) { imgZoomBy(1.25f); return; }
  if (key == KEY_DOWN) { imgZoomBy(1.0f / 1.25f); return; }
  if (key == '\n') {  // 点按/确认：适应屏幕 ↔ 2× 切换
    if (st.imgZoom > 1.01f) {
      st.imgZoom = 1.0f;
      st.imgPanX = 0.0f;
      st.imgPanY = 0.0f;
      st.dirty = 1; st.fullRefresh = true;
    } else {
      st.imgZoom = 2.0f;
      st.dirty = 1; st.fullRefresh = true;
    }
    return;
  }
  // 翻页只在 1× 时受理：放大状态下横向拖动结束会甩出一个 KEY_LEFT/RIGHT，
  // 那时用户想的是"看看图的那一边"，不是"换下一张"。
  if (st.imgZoom > 1.01f) return;
  if (key == KEY_LEFT) { imgSet(st.imgSel - 1); return; }
  if (key == KEY_RIGHT) { imgSet(st.imgSel + 1); return; }
  if (key == KEY_HOME) { imgSet(0); return; }
  if (key == KEY_END) { imgSet(static_cast<int>(st.imgList.size()) - 1); return; }
}

// ── 清理缓存 ─────────────────────────────────────────────────────────────
// 删除 .crossmux 下所有书缓存（封面/排版/书签索引），再重建空目录。
static void clearReadingCache() {
  int removed = 0;
  DIR *dp = opendir(CACHE_DIR);
  if (dp) {
    struct dirent *e;
    std::vector<std::string> subs;
    while ((e = readdir(dp)) != nullptr) {
      std::string name = e->d_name;
      if (name == "." || name == "..") continue;
      subs.push_back(std::string(CACHE_DIR) + "/" + name);
    }
    closedir(dp);
    for (auto &p : subs) {
      struct stat sb;
      if (stat(p.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) { if (Storage.removeDir(p.c_str())) removed++; }
      else { if (Storage.remove(p.c_str())) removed++; }
    }
  }
  Storage.mkdir(CACHE_DIR, true);
  ESP_LOGI(TAG, "已清理阅读缓存: %d 项", removed);
}

// ── 书架菜单 ─────────────────────────────────────────────────────────────
// 菜单项：0 搜索 1 最近阅读 2 文件浏览 3 图书详情 4 删除本书 5 重新扫描 6 清理缓存 7 返回。
static const int kShelfMenuItemCount = 8;

// 长按锁定目标书的辅助函数，照搬文件浏览器的 fmTarget()：菜单里的删除/详情都读它，
// 而不是 st.sel——"长按哪本弹的菜单就作用于哪本"全靠这个。shelfIdx 为 -1（没锁，
// 比如键盘路径或长按落在空白处）时回退到当前选中。
static const BookEntry *shelfTarget() {
  const int n = static_cast<int>(st.books.size());
  if (n <= 0) return nullptr;
  const int idx = (st.shelfIdx >= 0) ? clampI(st.shelfIdx, 0, n - 1) : clampI(st.sel, 0, n - 1);
  return &st.books[idx];
}

static int shelfTargetIdx() {
  const int n = static_cast<int>(st.books.size());
  if (n <= 0) return -1;
  return (st.shelfIdx >= 0) ? clampI(st.shelfIdx, 0, n - 1) : clampI(st.sel, 0, n - 1);
}

// 删掉书架上一本书：书文件 + 该书缓存目录（封面/排版）+ 最近阅读记录。
// 删除是不可逆操作，界面上用「按两次」确认（第一次 Enter 只是置位，见 handleShelfMenu）。
static void deleteBookAt(int idx) {
  if (idx < 0 || idx >= static_cast<int>(st.books.size())) return;
  const BookEntry b = st.books[idx];

  // 正打开的就是这本 → 先放掉书对象，免得删了文件还占着句柄/内存。
  if (st.bookKind == b.kind && st.bookPath == b.path) {
    st.epub.reset();
    st.section.reset();
    st.xtc.reset();
    st.txtUtf8.clear();
    st.txtLineStarts.clear();
    st.txtChapterOffsets.clear();
    st.txtChapterTitles.clear();
    st.bookPath.clear();
    st.bookTitle.clear();
    st.bookKind = -1;
  }

  bool fileOk = Storage.remove(b.path.c_str());
  // 缓存目录删不掉不算失败（可能本来就没有）；留着它反而会在同名文件放回来时读到旧缓存。
  Storage.removeDir(bookCacheDirFor(b.path, b.kind).c_str());

  for (auto it = st.recent.begin(); it != st.recent.end();) {
    if (it->path == b.path) it = st.recent.erase(it);
    else ++it;
  }
  saveRecent();

  ESP_LOGI(TAG, "删除书籍: %s (file=%d)", b.path.c_str(), static_cast<int>(fileOk));
  scanBooks();
  // 光标停在被删的那一格（表短了，原下标就是"下一本"），而不是 clamp 旧选中——
  // 长按删的可能是别处的书，选中不该还赖在原地。
  st.sel = clampI(idx, 0, std::max(0, static_cast<int>(st.books.size()) - 1));
  st.shelfIdx = -1;
  st.shelfDelArm = false;
}

static void shelfMenuLabels(std::vector<std::string> &out) {
  out.clear();
  out.push_back("搜索");     // 键盘/无触摸进入搜索的入口，与底栏放大镜同一件事
  out.push_back("最近阅读");
  out.push_back("文件浏览");
  out.push_back("图书详情");   // 作用于长按锁定的那一本（shelfTarget）
  const BookEntry *t = shelfTarget();
  if (st.shelfDelArm && t) {
    out.push_back("确认删除《" + t->name + "》");
  } else {
    out.push_back("删除本书");
  }
  out.push_back("重新扫描");
  out.push_back("清理缓存");
  out.push_back("返回");
}

static void renderShelfMenu() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int top = drawTitle("书架菜单");
  std::vector<std::string> items;
  shelfMenuLabels(items);
  int n = static_cast<int>(items.size());
  int itemH = uiLineHeight() + 12;
  for (int i = 0; i < n; i++) {
    int y = top + i * itemH;
    if (i == st.shelfMenuSel) { g_rd.fillRect(0, y, w, itemH, true); drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, items[i].c_str(), false); }
    else drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, items[i].c_str(), true);
  }
  if (st.shelfDelArm) drawFooter("再按一次确认删除（其余键取消）");
  else drawFooter("↑↓ 选择  Enter 确认  Esc 返回");
}

static void doShelfAction(int i) {
  switch (i) {
    case 0:
      rdEnterSearch(RdMode::ShelfSearch);
      break;
    case 1:
      st.recentSel = 0;
      st.mode = RdMode::Recent;
      break;
    case 2:
      // 文件浏览现在也是 1 号根标签，走和标签栏同一条进入路径（含目录重扫）。
      rdEnterFileTab();
      break;
    case 3:
      // 图书详情：盖在书架上的居中弹窗，任意键关掉（动作只读，不动任何状态）。
      if (st.books.empty()) break;
      st.mode = RdMode::ShelfInfo;
      break;
    case 4:
      // 两步删除：第一次只是置位（菜单随即显示「确认删除《…》」），再按一次才真删。
      // 删的是**长按锁定的那一本**，不是当前选中（shelfTargetIdx 负责回退）。
      if (st.books.empty()) break;
      if (!st.shelfDelArm) { st.shelfDelArm = true; break; }
      deleteBookAt(shelfTargetIdx());
      st.mode = RdMode::Browser;
      break;
    case 5:
      scanBooks();
      st.sel = clampI(st.sel, 0, std::max(0, static_cast<int>(st.books.size()) - 1));
      st.shelfIdx = -1;
      st.mode = RdMode::Browser;
      break;
    case 6:
      clearReadingCache();
      st.mode = RdMode::Browser;
      break;
    default:
      st.mode = RdMode::Browser;
      break;
  }
}

static void handleShelfMenu(int key) {
  const int n = kShelfMenuItemCount;
  if (key == 0x1B || key == KEY_LONG_CONFIRM) {
    st.shelfDelArm = false;
    st.mode = RdMode::Browser;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (key == KEY_UP) { st.shelfDelArm = false; st.shelfMenuSel = std::max(0, st.shelfMenuSel - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.shelfDelArm = false; st.shelfMenuSel = std::min(n - 1, st.shelfMenuSel + 1); st.dirty = 1; return; }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      int top = coverTop();
      int itemH = uiLineHeight() + 12;
      int row = (y - top) / itemH;
      if (row >= 0 && row < n) {
        if (row != st.shelfMenuSel) st.shelfDelArm = false;  // 换了行就取消待确认
        st.shelfMenuSel = row;
      }
    }
    doShelfAction(st.shelfMenuSel);
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
}

// ── 图书详情弹窗 ─────────────────────────────────────────────────────────
// 把书架原样画出来当背景（四周还能看到这是哪一页、选中框在哪），中间叠一个黑底白字
// 的居中框，风格照 ui_draw_confirm_dialog（黑底 + 1px 白内框 + 白字）。只读，任意键
// 关掉（handleShelfInfo）。内容全部来自 shelfTarget() 锁定的那一本——不是 st.sel。
static std::string rdStatsDate(uint32_t epoch);   // 定义见「统计」一节

struct ShelfInfoRow {
  std::string label;
  std::string value;
};

static void renderShelfInfo() {
  renderBrowser();   // 背景：书架本身
  const BookEntry *b = shelfTarget();
  if (!b) return;

  // 统计记录可能没有（从没打开过的书），那不是错误，按"未读"展示。
  const ReadingBookStats *rs = ReadingStats::findBook(b->path);
  const int lh = uiLineHeight();
  const int lineH = lh + 6;
  const int padX = 24, padY = 14;
  const int barH = lh + 12;

  std::vector<ShelfInfoRow> rows;
  rows.push_back({"书名", b->name});
  rows.push_back({"作者", (rs && !rs->author.empty()) ? rs->author : std::string("未知")});
  const char *typeStr = b->kind == 0 ? "EPUB 电子书"
                        : b->kind == 1 ? "TXT 文本"
                        : b->kind == 2 ? "XTC 漫画" : "文件";
  rows.push_back({"类型", typeStr});
  struct stat sb;
  rows.push_back({"大小", (stat(b->path.c_str(), &sb) == 0)
                              ? humanSize(static_cast<long long>(sb.st_size)) : std::string("未知")});
  rows.push_back({"进度", rs ? (std::to_string(static_cast<int>(rs->lastProgressPercent)) + "% · 章节 " +
                                std::to_string(static_cast<int>(rs->chapterProgressPercent)) + "%")
                             : std::string("未读")});
  rows.push_back({"状态", !rs ? "未读" : (rs->completed ? "已读完" : "阅读中")});
  if (rs && rs->totalReadingMs > 0)
    rows.push_back({"总时长", ReadingStats::formatDurationHm(rs->totalReadingMs)});
  if (rs && RdTime::clockValid(rs->lastReadAt))
    rows.push_back({"最后阅读", rdStatsDate(rs->lastReadAt)});

  const int w = g_rd.getScreenWidth(), h = g_rd.getScreenHeight();
  const int boxW = w - 2 * MARGIN;
  auto pl = g_rd.wrappedText(uiFontId(), b->path.c_str(), boxW - 2 * padX, 3);

  auto boxHeightOf = [&](int nRows, int nPath) {
    return padY + barH + nRows * lineH + 10 + nPath * lineH + lineH + padY;
  };
  // 逻辑高只有 684 的横屏、或把界面字号调得很大时，"全部字段 + 3 行路径"会装不进一屏。
  // 从尾部依次砍：先砍路径折行，再砍可选行（总时长/最后阅读——它们本来就 push 在末尾，
  // 前 6 行是必留的）。这样框永远在屏内，不会一半掉到屏幕外。
  const int availH = h - 2 * RD_TOP_INSET;
  while (!pl.empty() && boxHeightOf(static_cast<int>(rows.size()), static_cast<int>(pl.size())) > availH)
    pl.pop_back();
  while (static_cast<int>(rows.size()) > 6 &&
         boxHeightOf(static_cast<int>(rows.size()), static_cast<int>(pl.size())) > availH)
    rows.pop_back();
  const int boxH = boxHeightOf(static_cast<int>(rows.size()), static_cast<int>(pl.size()));

  int labelW = 0;
  for (auto &r : rows) labelW = std::max(labelW, g_rd.getTextWidth(uiFontId(), r.label.c_str()));
  const int valX = padX + labelW + 16;

  const int bx = (w - boxW) / 2;
  int by = (h - boxH) / 2;
  if (by < RD_TOP_INSET) by = RD_TOP_INSET;

  g_rd.fillRect(bx, by, boxW, boxH, true);              // 黑底
  g_rd.drawRect(bx + 2, by + 2, boxW - 4, boxH - 4, false);   // 1px 白内框
  int y = by + padY;
  drawCenteredLine(y, "图书详情", false);
  y += barH;
  g_rd.drawLine(bx + padX, y - 6, bx + boxW - padX, y - 6, false);
  for (auto &r : rows) {
    drawLineText(bx + padX, y, r.label.c_str(), false);
    drawLineText(bx + valX, y,
                 g_rd.truncatedText(uiFontId(), r.value.c_str(), boxW - padX - valX).c_str(), false);
    y += lineH;
  }
  y += 4;
  g_rd.drawLine(bx + padX, y, bx + boxW - padX, y, false);
  y += 6;
  for (auto &ln : pl) {
    drawLineText(bx + padX, y, ln.c_str(), false);
    y += lineH;
  }
  drawCenteredLine(y + 2, "任意键关闭", false);
}

static void handleShelfInfo(int key) {
  (void)key;   // 只读弹窗：任意键/点按都关掉，回书架
  st.mode = RdMode::Browser;
  st.fullRefresh = true;
  st.dirty = 1;
}

// 目录一屏能排几行。renderToc（绘制）、handleToc（翻页/点按）共用同一份几何，
// 免得"翻一页"和"画一屏"各算各的。
static int tocRowsPerPage() {
  const int top = coverTop();
  const int itemH = uiLineHeight() + 6;
  const int viewH = statusTop() - top - 8;
  return std::max(1, viewH / itemH);
}

static void renderToc() {
  g_rd.clearScreen();
  int top = drawTitle("目录");
  std::vector<std::string> items;
  if (st.bookKind == 0 && st.epub) {
    for (int i = 0; i < st.epub->getTocItemsCount(); i++) {
      items.push_back(st.epub->getTocItem(i).title);
    }
  } else if (st.bookKind == 1) {
    items = st.txtChapterTitles;
  }
  if (items.empty()) {
    drawCenteredLine(g_rd.getScreenHeight() / 2, "本书无目录");
  } else {
    int itemH = uiLineHeight() + 6;
    int maxRows = tocRowsPerPage();
    int start = clampI(st.tocSel - maxRows / 2, 0, std::max(0, static_cast<int>(items.size()) - maxRows));
    // 条目走**内容面**（CONTENT_UI_FONT_ID），不走外壳面。目录条目是书里的字——卷名、
    // 回目、繁体书名——而内嵌字体在位时 uiFontId() 退回的 builtin.ttf 只有常用简繁字，
    // 繁体书一开目录就是一片豆腐块。内容面在内嵌字体在位时是**这本书的字面**，没内嵌
    // 字体时是用户所选字体，两者覆盖都远好于 builtin。
    // id 20 与 id 0 度量完全一致（同一个 g_uiFont.data），所以 itemH/maxRows/缩进全都
    // 不用动，只换字面；抬头「目录」「本书无目录」和底部提示仍是外壳词，照旧走 builtin。
    for (int i = 0; i < maxRows && start + i < static_cast<int>(items.size()); i++) {
      int idx = start + i;
      int y = top + i * itemH;
      if (idx == st.tocSel) {
        g_rd.fillRect(0, y, g_rd.getScreenWidth(), itemH, true);
        drawLineText(MARGIN, y + 3, items[idx].c_str(), false, CONTENT_UI_FONT_ID);
      } else {
        drawLineText(MARGIN, y + 3, items[idx].c_str(), true, CONTENT_UI_FONT_ID);
      }
    }
  }
  drawFooter("↑↓ 选择  上下滑翻页  Enter 跳转  Esc 返回");
}

// ── 阅读菜单：动作 + 动态条目 ───────────────────────────────────────────
enum class MenuAct {
  Toc, Font, FontFamily, LineSpacing, ParaSpacing, Indent, Align, Margin, Image, ReadingLine, Night, Orient,
  ToggleBookmark, Bookmarks, Footnotes, FootnoteBack, Percent, Qr,
  Dict, DictDl, ResDl, Weread, Wifi, Opds, NetShare, KeyMap, StatusBar, About, Standby,
  ClockFace, ShelfStyle, RefreshStrategy, FullEvery, TurnAnim, StyleSource, EmbeddedFont, AutoStandby,
  ToShelf, Back
};
struct MenuItem { std::string label; MenuAct act; };

// 当前页号（书签/百分比/二维码通用）。
static int curPage() {
  if (st.bookKind == 0) return st.page;
  if (st.bookKind == 1) return st.txtPage;
  return st.xtcPage;
}

// epub 当前页可见文本偏移（跨重排的书签标识）。txt/xtc 返回 UINT32_MAX。
static uint32_t currentVisibleOffset() {
  if (st.bookKind == 0 && st.section) {
    auto o = st.section->getVisibleTextOffsetForPage(static_cast<uint16_t>(st.page));
    if (o) return *o;
  }
  return UINT32_MAX;
}

static bool currentPageHasFootnotes() {
  if (st.bookKind != 0 || !st.section) return false;
  auto page = st.section->loadPage(st.page);
  return page && !page->footnotes.empty();
}

static bool isCurrentBookmarked() {
  if (st.bookPath.empty()) return false;
  uint32_t off = (st.bookKind == 0) ? currentVisibleOffset() : 0;
  for (auto &b : st.bookmarks) {
    if (b.path != st.bookPath || b.kind != st.bookKind) continue;
    if (st.bookKind == 0) { if (b.spine == st.spineIndex && b.offset == off) return true; }
    else if (b.page == curPage()) return true;
  }
  return false;
}

static std::vector<MenuItem> menuItems() {
  char buf[64];
  std::vector<MenuItem> m;
  m.push_back({"目录", MenuAct::Toc});
  snprintf(buf, sizeof(buf), "字号: %d", kBodyPx[st.fontLevel]);
  m.push_back({buf, MenuAct::Font});
  // 书内嵌字体生效时内容面装的不是用户选的那个字体，标签就如实写"书内嵌"——
  // 否则显示成用户字体名会让人以为设置没生效（按下时的处理见 MenuAct::FontFamily）。
  m.push_back({st.bookFontLocal.empty() ? std::string("字体: ") + ttf_font_display_name()
                                        : std::string("字体: 书内嵌"),
               MenuAct::FontFamily});
  snprintf(buf, sizeof(buf), "行距: %.1f", st.lineSpacing);
  m.push_back({buf, MenuAct::LineSpacing});
  m.push_back({std::string("段距: ") + kParaSpacingLabels[clampI(st.paraSpacing, 0, 5)], MenuAct::ParaSpacing});
  // 样式解析选「书籍内嵌」时这两项被书里的 CSS 接管，标签直接标出来，免得以为
  // 调了没反应（值本身保留着，切回「强制指定」立刻按它生效）。
  // TXT/XTC 没有书内 CSS，这个开关对它们没有意义，照旧显示自己的档位。
  const bool embedded = styleEmbedded() && st.bookKind == 0;
  m.push_back({std::string("缩进: ") +
                   (embedded ? "随书" : (st.indentMode == 0 ? "自动" : st.indentMode == 1 ? "强制" : "取消")),
               MenuAct::Indent});
  m.push_back({std::string("对齐: ") + (embedded ? "随书" : kAlignLabels[clampI(st.alignMode, 0, 3)]), MenuAct::Align});
  m.push_back({std::string("边距: ") + (st.marginIdx == 0 ? "窄" : st.marginIdx == 1 ? "标准" : "宽"), MenuAct::Margin});
  m.push_back({std::string("图片: ") + (st.imageBilinear ? "双线性" : "最近邻"), MenuAct::Image});
  m.push_back({std::string("阅读线: ") + kReadingLineNames[clampI(st.readingLine, 0, 3)], MenuAct::ReadingLine});
  // 「夜间」「方向」已搬到主界面设置（夜间模式 / 阅读器方向）——它们本来就是设备级
  // 设定，放在"当前这本书"的菜单里不对路。
  snprintf(buf, sizeof(buf), "书签(%d)", static_cast<int>(st.bookmarks.size()));
  m.push_back({buf, MenuAct::Bookmarks});
  m.push_back({isCurrentBookmarked() ? "删除书签" : "添加书签", MenuAct::ToggleBookmark});
  if (currentPageHasFootnotes()) m.push_back({"脚注", MenuAct::Footnotes});
  if (st.footnoteRetValid) m.push_back({"返回脚注跳转前", MenuAct::FootnoteBack});
  m.push_back({"跳转百分比", MenuAct::Percent});
  m.push_back({"二维码", MenuAct::Qr});
  m.push_back({"词典", MenuAct::Dict});
  // 系统级条目（WiFi/传书/OPDS/词典下载/按键映射/状态栏/方向/待机/关于）都移到了
  // 主界面的「设置」标签，这里只留与「当前这本书」有关的阅读操作。
  m.push_back({"返回书架", MenuAct::ToShelf});
  m.push_back({"返回阅读", MenuAct::Back});
  return m;
}

static void renderMenu() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int top = drawTitle("阅读菜单");
  auto items = menuItems();
  int itemH = uiLineHeight() + 12;
  int viewH = statusTop() - top - 8;
  int maxRows = std::max(1, viewH / itemH);
  int start = clampI(st.menuSel - maxRows / 2, 0, std::max(0, static_cast<int>(items.size()) - maxRows));
  for (int i = 0; i < maxRows && start + i < static_cast<int>(items.size()); i++) {
    int idx = start + i;
    int y = top + i * itemH;
    if (idx == st.menuSel) { g_rd.fillRect(0, y, w, itemH, true); drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, items[idx].label.c_str(), false); }
    else drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, items[idx].label.c_str(), true);
  }
  drawFooter("↑↓ 选择  Enter 确认  Esc 返回");
}

// ── 词典 ────────────────────────────────────────────────────────────────
// 查询框的矩形必须**和 renderDict 画出来的一模一样**：renderDict 用
// `top = drawTitle("词典")` 当 qy，而 drawTitle 返回的是 rdHeadBottom() + 8
// （rdHeadBottom 里还夹着 TAB_BAND_BOTTOM=84，字号小的时候它才是大的那个）。
// 以前这里硬算 RD_TOP_INSET + uiLineHeight + 14，字号 <=20 时比画出来的框
// **高 12px**，点在框上沿到框底那一带就落不进命中区，直接被当成"没点输入框"
// → 走了查词 → 状态栏报"未找到词典"，虚拟键盘也就永远弹不出来。
// 现在直接取同一个来源，绘制和命中不可能再对不上。
static int dictQueryTop() { return coverTop(); }
static int dictQueryH() { return uiLineHeight() + 12; }

// 上一次发现失败时 /sdcard/dictionaries 里到底有什么（空串=还没探过）。串口日志里本来
// 就有这条（见 openDictionary 的 ESP_LOGW），但**用户看得到的是屏幕上的那一句** ——
// 报障时只能得到"我把文件拷进去了，还是说没有词典"，谁都判断不出是没拷进去、名字不对，
// 还是目录枚举出了问题。把它一并写进屏幕上的状态行，一眼就能看出来。
static std::string s_dictDirSeen;

static bool openDictionary() {
  if (st.dictOpen) return true;
  // 优先用设置里的目录名。
  std::string folder = g_settings.getString("reader_dict");
  if (!folder.empty() && st.dict.open(folder.c_str())) {
    st.dictOpen = true;
    return true;
  }
  // 否则扫描 /sdcard/dictionaries 下第一个含 .idx 的目录。
  DIR *dp = opendir(DICT_ROOT);
  if (!dp) {
    ESP_LOGW(TAG, "词典: %s 打不开（SD 卡没挂载？）", DICT_ROOT);
    s_dictDirSeen = "目录打不开（SD 卡没挂载？）";
    return false;
  }
  s_dictDirSeen.clear();
  struct dirent *e;
  bool ok = false;
  // 把根目录里到底有些什么打出来：以前这条链路全是静默失败，用户看到的现象只有
  // 一句"未找到词典"，没法分辨是文件根本没拷进去、名字不对，还是目录枚举有问题。
  std::string seen;
  while ((e = readdir(dp)) != nullptr) {
    std::string name = e->d_name;
    // 跳过 . / .. 以及下载器的半成品目录（.<id>.staging / .<id>.bak）。
    if (name.empty() || name[0] == '.') continue;
    std::string full = std::string(DICT_ROOT) + "/" + name;
    struct stat s;
    const bool isDir = stat(full.c_str(), &s) == 0 && S_ISDIR(s.st_mode);
    if (seen.size() < 300) seen += (seen.empty() ? "" : ", ") + name + (isDir ? "/" : "");
    if (isDir && st.dict.open(name.c_str())) { st.dictOpen = true; ok = true; break; }
  }
  closedir(dp);
  if (ok) return true;
  // 再认一种摆法：三件套（.idx/.dict[.dz]）不放进子目录，直接摊在词典根目录下。
  // 手拷 StarDict 词典的人多半就是这么放的 —— 少建一层目录，也就少一个出错的地方。
  if (st.dict.open(".")) {
    ESP_LOGI(TAG, "词典: 用根目录 %s 下摊放的词典", DICT_ROOT);
    st.dictOpen = true;
    return true;
  }
  ESP_LOGW(TAG, "词典: %s 下没有可用词典；entries=[%s]", DICT_ROOT, seen.c_str());
  s_dictDirSeen = seen;
  return false;
}

static void doDictLookup() {
  if (!openDictionary()) {
    st.dictStatus = "未找到词典：请到「设置→词典下载」安装";
    // 发现阶段失败时，把**看到的目录内容**和合格摆法一起写到正文区（正文是折行画的，
    // 状态行只有一行放不下）。用户拍的屏幕照片就够判断是"根本没拷进去"、"名字不对"
    // 还是"目录枚举出问题"—— 以前这里只有一句"未找到词典"，报障信息量为零。
    st.dictResult = std::string("目录 ") + DICT_ROOT + " 内：";
    st.dictResult += s_dictDirSeen.empty() ? "（空的，或只有隐藏目录）" : s_dictDirSeen;
    st.dictResult += "。词典要三件套同名：<名>.idx 加 <名>.dict（或 <名>.dict.dz），"
                     "放进该目录下的任意子文件夹，或直接摊在根目录。";
    st.dictHeadword.clear();
    return;
  }
  if (st.dictQuery.empty()) { st.dictStatus.clear(); return; }
  if (st.dict.needsIndex()) {
    st.dictStatus = "首次建立词典索引...";
    st.dict.buildIndex();
  }
  Dictionary::LookupResult r;
  if (st.dict.lookup(st.dictQuery.c_str(), st.dictResult, st.dictHeadword, &r)) {
    st.dictStatus = "词条: " + st.dictHeadword;
  } else {
    st.dictResult.clear();
    st.dictHeadword.clear();
    switch (r) {
      case Dictionary::LookupResult::NotFound: st.dictStatus = "未找到该词条"; break;
      case Dictionary::LookupResult::LowMemory: st.dictStatus = "内存不足"; break;
      case Dictionary::LookupResult::Decompress: st.dictStatus = "词典数据解压失败"; break;
      case Dictionary::LookupResult::ReadError: st.dictStatus = "词典读取错误"; break;
      default: st.dictStatus = "查找失败"; break;
    }
  }
}

// ── 虚拟键盘：三个模式共用 editor_vk，这里只剩一层薄适配 ──────────────────
// 阅读模式原来另有一套自己的键盘绘制（走 GfxRenderer g_rd），几何是单独一份常量：
// 候选条 50、键行高 46、总高 240px。写作/计划模式的 editor_vk 是候选条 FONT_H+10、
// 字母行 FONT_H+40，横屏下总高 400px 出头 —— 同一块屏幕上两个键盘一大一小，连
// 键位都不一样（editor_vk 多一行 Ctrl/方向/Shift/⌫），看着就是两套东西。
//
// 现在这里不再画键盘：**绘制、命中、按下反馈全部转给 editor_vk**。两者写的是同一块
// epdiy 帧缓冲（u8g2 shim 绑的 s_u8g2.fb 与 GfxRenderer::frameBuffer 都是
// HalDisplay::getFrameBuffer()，见 hw/board.cpp:39 与 GfxRenderer.cpp:74），坐标同为
// 旋转后的逻辑坐标，所以 editor_vk 在阅读模式里可以直接用，不需要任何坐标换算。
// 布局、键高、候选条、Shift/⌫ 图标字形、键位切换因此只有一份实现，三个模式不会走样。
//
// 键位语义也一并统一：阅读模式跟着有了 Ctrl/方向键/Shift 那一行。方向键在输入法有
// 编码时由 IME 内部映射成候选翻页，无编码时 IME 放行——阅读模式没有光标可移，这里
// 忽略即可（下面 vkTap 的 default 分支）。Shift 在阅读模式同样作用于下一个字母。
//
// st.vkVisible 仍是可见性的唯一真相（阅读模式有 20 多处地方在翻它），每次绘制/命中
// 前同步给 editor_vk，避免两边各记一份再慢慢不一致。

static void rdSyncVk() {
  if (editorVkVisible() != st.vkVisible) editorVkSetVisible(st.vkVisible);
}

// 键盘面板顶边 y。阅读模式原来用固定的 VK_H 算，现在统一问 editor_vk（面板高度
// 跟随当前字号，横竖屏都现算）。
static int vkVkTop() { return editorVkTop(); }

static void drawVk() {
  rdSyncVk();
  editorVkDraw();
}

// 状态栏右端的键盘开关图标。绘制与命中都沿用 editor_vk 那一份几何（STATUS_BAR_Y /
// STATUS_BAR_H / FONT_H 槽宽）——阅读模式的提示栏 statusTop() 与它只差 2px，同一行，
// 不必再算一遍。**必须在状态栏画完之后调用**，否则会被提示栏的白底盖掉。
// 反白态取 editor_vk 的 s_visible，所以画之前先把可见性同步过去。
static void rdDrawVkIcon() {
  rdSyncVk();
  editorVkDrawIcon();
}

static bool rdVkIconHit(int x, int y) { return editorVkIconHit(x, y); }

// 前向声明（OPDS/传书界面在文件后段定义，但 VK 的回车提交需要调用它们）。
static void renderCurrent();
static void renderSettingsTab();  // 「设置」标签（定义在菜单动作之后）
// 「统计」标签与它的子界面（定义在 handleSettingsTab 之后的一大段里）。
static void renderStatsTab();
static void handleStatsTab(int key);
static void renderStatsBook();
static void handleStatsBook(int key);
static void renderStatsMore();
static void handleStatsMore(int key);
static void renderStatsHeatmap();
static void handleStatsHeatmap(int key);
static void renderStatsDay();
static void handleStatsDay(int key);
static void renderStatsProfile();
static void handleStatsProfile(int key);
static void renderStatsAdjust();
static void handleStatsAdjust(int key);
static void renderStatsSettings();
static void handleStatsSettings(int key);
static void opdsBeginLoad(const std::string &url);
static void netShareConnect();
static void prepareQr();
static void rdNoteCommit();      // 笔记编辑器保存（定义在文件后段）
static void renderNotes();       // 「笔记」标签（定义在文件后段）
static void renderNoteEdit();
static void rdShelfSearchOpen();  // 打开书架搜索的当前命中（定义在文件后段）
static void rdNotesSearchOpen();  // 打开笔记搜索的当前命中

// 当前被 VK/IME 编辑的文本字段（nullptr=无）。
static std::string *vkTargetString() {
  if (st.mode == RdMode::NoteEdit) return &st.noteEditBuf;
  if (st.mode == RdMode::FileRename) return &st.fmRenameBuf;
  if (st.mode == RdMode::ShelfSearch) return &st.shelfQuery;
  if (st.mode == RdMode::NotesSearch) return &st.notesQuery;
  if (st.mode == RdMode::Dictionary) return &st.dictQuery;
  if (st.mode == RdMode::Wifi) return (st.wifiField == 0) ? &st.wifiSsidEdit : &st.wifiPassEdit;
  if (st.mode == RdMode::ResDl) return &((st.resField == 0) ? st.resDictEdit : st.resFontEdit);
  if (st.mode == RdMode::Opds) return &st.opdsUrlEdit;
  return nullptr;
}

static void commitVk(const std::string &s) {
  if (s.empty()) return;
  std::string *t = vkTargetString();
  if (t) *t += s;
  // 搜索框里每敲一个字，命中表就变一次；选中项跟着回到第一命中，免得停在
  // 已经被筛掉的第 N 项上（列表本身每帧也会 clamp，这里只是让它更直觉）。
  if (st.mode == RdMode::ShelfSearch || st.mode == RdMode::NotesSearch) st.searchSel = 0;
}

static void popUtf8(std::string &s) {
  if (s.empty()) return;
  size_t i = s.size() - 1;
  while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i--;
  s.erase(i);
}

static void feedVkKey(int c) {
  std::string out;
  if (IME::getInstance().handleKey(c, out)) {
    commitVk(out);
    return;
  }
  // 输入法没接这个键（未组合时的数字与半角标点、英文态下的字母）：直接当裸字符落进
  // 目标串。写作模式有 screen_editor 的 "ASCII printable" 分支兜底，阅读模式没有，
  // 少了这一段，数字面板上的 1..9/0 就一个也打不出来。
  if (c >= 0x20 && c <= 0x7E) {
    std::string *t = vkTargetString();
    if (t) {
      t->push_back(static_cast<char>(c));
      st.dirty = 1;
    }
  }
}

static void feedVkBackspace() {
  auto &ime = IME::getInstance();
  if (ime.composing()) {
    std::string out;
    ime.handleKey('\b', out);
    commitVk(out);
  } else {
    std::string *t = vkTargetString();
    if (t) popUtf8(*t);
  }
}

static void vkEnter() {
  auto &ime = IME::getInstance();
  if (st.mode == RdMode::NoteEdit) {
    // 笔记编辑器：回车=保存。组合中的编码先落地，但换行本身不写进笔记。
    std::string out;
    ime.handleKey('\n', out);
    if (!out.empty() && out != "\n") commitVk(out);
    rdNoteCommit();
    return;
  }
  std::string out;
  ime.handleKey('\n', out);
  commitVk(out);
  if (st.mode == RdMode::Dictionary) {
    doDictLookup();
    st.vkVisible = false;
  } else if (st.mode == RdMode::Wifi) {
    g_settings.setWifiSsid(st.wifiSsidEdit);
    g_settings.setWifiPassword(st.wifiPassEdit);
    st.vkVisible = false;
  } else if (st.mode == RdMode::Opds) {
    st.opdsEditing = false;
    st.vkVisible = false;
    if (!st.opdsUrlEdit.empty()) {
      g_settings.setString("opds_url", st.opdsUrlEdit);
      opdsBeginLoad(st.opdsUrlEdit);
    }
  } else if (st.mode == RdMode::ResDl) {
    // 空串就是要"回到内置默认"（词典下载那边按空串回落 DICT_MANIFEST_DEFAULT），
    // 所以这里照写不误，别拿 if (!empty) 保护掉。
    g_settings.setString("dict_manifest_url", st.resDictEdit);
    g_settings.setString("font_dl_url", st.resFontEdit);
    st.resEditing = false;
    st.vkVisible = false;
  } else if (st.mode == RdMode::ShelfSearch) {
    // 搜索框里的回车（虚拟键盘/蓝牙键盘都一样）：组合中的编码已经落在上一行 commitVk
    // 里了，这里打开当前命中的那本。
    st.vkVisible = false;
    rdShelfSearchOpen();
  } else if (st.mode == RdMode::NotesSearch) {
    st.vkVisible = false;
    rdNotesSearchOpen();
  }
}

// 点按 → editor_vk 命中 → 翻译成"普通键码" → 走阅读模式既有的输入逻辑。
// 和写作模式一样，键盘不重复实现任何输入逻辑：候选返回 '1'+i（交给 IME 完成选词）、
// 字母返回 'a'..'z' 或一键多字母布局的组码、空格/退格/回车返回 ' ' / '\b' / '\n'，
// 其余（翻页/中英/布局/Shift/Ctrl）在 editorVkHitTest 内部已经翻转了自身状态，
// 这里重绘即可。
static void vkTap(int x, int y) {
  rdSyncVk();
  EditorVkHit hit;
  const int k = editorVkHitTest(x, y, &hit);
  if (k == EVK_NONE) return;  // 死区/空白：吞掉本次点按，不发生任何动作（防误触）
  editorVkMarkPressed(hit);   // 反色由本次动作的重绘一起画出去（不额外推屏）
  // 这几类键的动作会把键面重画成新状态（换标签/翻反白），按下前那份几何再叠上去
  // 就成了两个标签摞一起（中英切换时"拼""英"叠字，按别的键才刷新）。取消补画。
  // 候选数字键也在内：上屏后候选栏换新，旧候选几何补画上去就是清不掉的残留。
  if (k == EVK_PAGE || k == EVK_LANG || k == EVK_LAYOUT || k == EVK_CTRL || k == EVK_SHIFT ||
      (k >= '1' && k <= '9'))
    editorVkClearPressed();
  switch (k) {
    case EVK_PAGE:  // 换面板（字母/符号/数字）：命中测试内已切好
      // 键区整块换图样 → 和布局切换一样要一次整屏 GC16，局刷的残影擦不干净。
      st.fullRefresh = true;
      return;
    case EVK_LAYOUT:  // 键位布局循环 26→14→18→9（命中测试内已翻转并写设置）
      // 整块键位换图样，局刷的残影最明显（旧的键框擦不干净）→ 要一次整屏 GC16。
      st.fullRefresh = true;
      return;
    case EVK_T9:  // T9 候选面板开/关，或面板里换了读音（命中测试内已处理）
      // 同样是整块键区换图样 → 全刷。
      st.fullRefresh = true;
      return;
    case EVK_CTRL:    // 组合键待发。阅读模式没有可组合的动作，让它纯当待发状态，
      return;         // 下一个普通键被翻译成控制码后由下面的守卫丢弃。
    case EVK_SHIFT:   // 大写待发：命中测试内已翻转，下一个字母由它翻译成大写
      return;
    case EVK_SEP:     // 九宫格「1 分词」：命中测试内已确定下一个音节
    case EVK_CLEAR:   // 九宫格「重输」：命中测试内已清空当前拼音组合
    case EVK_NOOP:    // 九宫格组合中的「0」：故意无动作，只给反色反馈
      // 三者都不改键面图样；调用方本就在 vkTap 之后置了 st.dirty，重绘即反馈。
      return;
    case EVK_LANG: {
      auto &ime = IME::getInstance();
      // 未开输入法 → 开中文；已开 → 拼音/英文互切（与写作模式键盘同一套语义）
      if (!ime.active()) ime.setActive(true);
      else ime.toggleEnglish();
      // 中/英切换会连带换掉键面标签、候选条、状态栏标记，同样是整块重画 → 全刷。
      st.fullRefresh = true;
      return;
    }
    default:
      // 普通字符、一键多字母组码(IME::KEY_AMBIG_BASE+组)、以及上面那行方向键
      // (KEY_LEFT/UP/DOWN/RIGHT) 全走同一条：交给 IME。方向键在输入法有编码时
      // 被 IME 内部当候选翻页消费；无编码时 IME 放行，feedVkKey 也就什么都不做
      // ——阅读模式没有光标可移，方向键在这里自然退化成无操作。
      if (k == '\n') { vkEnter(); return; }
      if (k == '\b') { feedVkBackspace(); return; }
      // Ctl 待发时命中测试会把下一个普通键翻译成控制码(0x01..0x1A、KEY_SEARCH…)。
      // 写作模式拿它们当快捷键，阅读模式没有对应动作——在门口丢掉，别让它们
      // 落到输入法的特殊分支上(如预测模式的 0x04 是"拒绝候选")。
      if (k < 0x20) return;
      feedVkKey(k);
      return;
  }
}

// 候选行左右划翻页（实现在 editor_vk 的 editorVkSwipePage）。抬手时 input.cpp 会把
// 一次横滑补成一个离散的 KEY_LEFT/RIGHT——在阅读页那是翻书、在词典/设置里是切标签。
// 滑动的起点(input_press_xy)落在候选行时，这个键属于键盘，必须从这里吃掉。
static bool rdVkSwipePage(int x, int y, int dir) {
  rdSyncVk();   // 可见性的唯一真相是 st.vkVisible，判候选行之前先同步过去
  return editorVkSwipePage(x, y, dir);
}

// T9 候选面板里的上下滑（实现在 editor_vk 的 editorVkSwipeScroll）。同样：抬手时
// input.cpp 把一次竖滑补成离散的 KEY_PAGE_UP/DOWN，起点落在面板上时归键盘。
static bool rdVkSwipeScroll(int x, int y, int dir) {
  rdSyncVk();
  return editorVkSwipeScroll(x, y, dir);
}

// ── 渲染：词典 / 微信读书 ───────────────────────────────────────────────
static void renderDict() {
  g_rd.clearScreen();
  int top = drawTitle("词典");
  int qy = top;
  int qh = dictQueryH();
  g_rd.drawRect(MARGIN, qy, g_rd.getScreenWidth() - 2 * MARGIN, qh, true);
  std::string q = st.dictQuery.empty() ? "点击输入单词/拼音" : st.dictQuery;
  drawLineText(MARGIN + 8, qy + 6, q.c_str(), true);
  int ry = qy + qh + 12;
  int bottom = st.vkVisible ? vkVkTop() : statusTop();
  if (!st.dictStatus.empty()) { drawLineText(MARGIN, ry, st.dictStatus.c_str(), true); ry += uiLineHeight() + 6; }
  if (!st.dictResult.empty()) {
    int maxW = g_rd.getScreenWidth() - 2 * MARGIN;
    int maxLines = std::max(1, (bottom - ry) / (uiLineHeight() + 4));
    auto lines = g_rd.wrappedText(uiFontId(), st.dictResult.c_str(), maxW, maxLines);
    for (auto &ln : lines) {
      if (ry + uiLineHeight() > bottom) break;
      drawLineText(MARGIN, ry, ln.c_str(), true);
      ry += uiLineHeight() + 4;
    }
  }
  if (st.vkVisible) drawVk();
  else drawFooter("点查询框输入  Enter 查词  Esc 返回");
  rdDrawVkIcon();
}

// ── 微信读书 ────────────────────────────────────────────────────────────
// 状态机由 weDrive() 在按键循环里逐拍推进；这里只做四件事：
// 书架列表（RdMode::Weread）、扫码（WereadQr）、书目菜单（WereadMenu）、缓存进度（WereadDl）。

static const char *weErrorText(WeReadClient::Error e) {
  switch (e) {
    case WeReadClient::Error::Ok: return "完成";
    case WeReadClient::Error::Cancelled: return "已取消";
    case WeReadClient::Error::Network: return "网络不可用（先连 WiFi）";
    case WeReadClient::Error::SessionExpired: return "登录已失效，请重新扫码";
    case WeReadClient::Error::LoginFailed: return "扫码登录超时或失败";
    case WeReadClient::Error::Protocol: return "接口返回异常（服务可能已变更）";
    case WeReadClient::Error::SdCard: return "SD 卡读写失败";
    case WeReadClient::Error::Integrity: return "数据校验失败";
    case WeReadClient::Error::Unavailable: return "该书暂不可缓存";
    case WeReadClient::Error::Clock: return "时钟未同步";
    case WeReadClient::Error::OutOfMemory: return "内存不足（重启后再试）";
    case WeReadClient::Error::WholeBookOnly: return "该书只能整本缓存";
  }
  return "未知错误";
}

static const char *weStageName(WeReadClient::Operation::ProgressStage s) {
  switch (s) {
    case WeReadClient::Operation::ProgressStage::Chapters: return "缓存章节";
    case WeReadClient::Operation::ProgressStage::Preparing: return "整理资源";
    case WeReadClient::Operation::ProgressStage::Images: return "下载图片";
    case WeReadClient::Operation::ProgressStage::Packaging: return "生成图书";
  }
  return "处理中";
}

static bool weSessionValid() {
  WeReadStore::Session s;
  return WeReadStore::loadSession(s);
}

static void weLoadShelf() {
  st.weShelf.clear();
  st.weShelfLoaded = true;
  HalFile file;
  uint32_t count = 0;
  if (!WeReadStore::openShelf(file, count)) return;
  st.weShelf.reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    WeReadStore::ShelfRecord rec;
    if (!WeReadStore::readShelfRecord(file, i, rec)) break;
    st.weShelf.push_back(rec);
  }
  file.close();
  st.weSel = clampI(st.weSel, 0, std::max(0, static_cast<int>(st.weShelf.size()) - 1));
}

static bool readerEnsureWifi(std::string &err);   // 定义在词典下载一节

// 起一个新任务（登录/书架同步 kind=0，整本缓存 kind=1）。
static void weBeginJob(int kind, const WeReadStore::ShelfRecord *book) {
  if (!st.weOp) st.weOp = weMakeOperation();
  if (!st.weOp) { st.weStatus = "内存不足，无法启动"; return; }
  // 没联网就先用设置里的 SSID/密码连一次（与词典下载同一条路子）。微读这边所有请求
  // 都是同步阻塞的，不先连上只会从 HTTP 层拿到一句含糊的"网络错误"；这里先连、
  // 连不上才报错，用户看到的是"正在连接 WiFi..."→ 具体失败原因。
  if (!g_wifi.isConnected()) {
    st.weStatus = "正在连接 WiFi...";
    st.fullRefresh = true;
    st.dirty = 1;
    renderCurrent();
    st.fullRefresh = false;
    std::string werr;
    if (!readerEnsureWifi(werr)) {
      st.weStatus = werr;
      st.dirty = 1;
      return;
    }
    st.weStatus.clear();
  }
  WeReadClient::DownloadOptions options;
  options.imagePolicy = WeReadStore::ImagePolicy::Embed;
  options.chapterScope = WeReadClient::DownloadOptions::ChapterScope::WholeBook;
  const WeReadClient::Operation::Kind k =
      (kind == 1) ? WeReadClient::Operation::Kind::Download : WeReadClient::Operation::Kind::Sync;
  st.weKind = kind;
  st.weStatus.clear();
  st.weJobTitle = book ? book->title : "";
  if (!st.weOp->begin(k, book, options)) {
    st.weStatus = weErrorText(st.weOp->error());
    st.dirty = 1;
    return;
  }
  st.fullRefresh = true;
  st.dirty = 1;
}

// 推进状态机并处理事件。联网请求是同步阻塞的——刷屏已经先画好，用户看到的是
// 上一次的进度画面，请求返回后立刻重画。
static void weDrive() {
  if (!st.weOp) return;
  const WeReadClient::Operation::Event ev = st.weOp->step();
  switch (ev) {
    case WeReadClient::Operation::Event::QrReady:
      st.weQrUrl = st.weOp->qrUrl();
      st.mode = RdMode::WereadQr;
      st.fullRefresh = true;
      st.dirty = 1;
      break;
    case WeReadClient::Operation::Event::Authenticated:
      st.weStatus = "已扫码，正在同步书架...";
      st.fullRefresh = true;
      st.dirty = 1;
      break;
    case WeReadClient::Operation::Event::Complete:
      if (st.weKind == 1) {
        const std::string path = st.weOp->finalPath();
        st.weStatus.clear();
        // 新书刚落进 /sdcard/WeRead，书架列表还是进阅读模式时扫的那一份：
        // 重扫一遍并夹住选中项，否则读完退出回书架看不到刚缓存的书。
        scanBooks();
        if (st.sel >= static_cast<int>(st.books.size())) {
          st.sel = st.books.empty() ? 0 : static_cast<int>(st.books.size()) - 1;
        }
        if (!path.empty() && openBook(path, 0)) {
          st.mode = RdMode::Reading;
        } else {
          st.weStatus = "缓存完成，但打不开这本书";
          st.mode = RdMode::Weread;
        }
      } else {
        weLoadShelf();
        st.weStatus.clear();
        st.mode = RdMode::Weread;
      }
      st.fullRefresh = true;
      st.dirty = 1;
      break;
    case WeReadClient::Operation::Event::Cancelled:
      st.weStatus = "已取消";
      st.mode = (st.weKind == 1) ? RdMode::WereadMenu : RdMode::Weread;
      st.fullRefresh = true;
      st.dirty = 1;
      break;
    case WeReadClient::Operation::Event::Failed:
      st.weStatus = weErrorText(st.weOp->error());
      st.mode = (st.weKind == 1) ? RdMode::WereadMenu : RdMode::Weread;
      st.fullRefresh = true;
      st.dirty = 1;
      break;
    case WeReadClient::Operation::Event::DetailReady:
    case WeReadClient::Operation::Event::ChapterRangeReady:
    case WeReadClient::Operation::Event::ChapterComplete:
      st.dirty = 1;   // 进度画面刷新
      break;
    case WeReadClient::Operation::Event::None:
      if (st.mode == RdMode::WereadDl && st.weOp->active()) st.dirty = 1;
      break;
  }
}

// 书架行（两行文字：书名 + 作者/本地状态）。
static int weItemH() { return uiLineHeight() * 2 + 8; }

static int weBookIndex() {
  return (st.weSel >= 0 && st.weSel < static_cast<int>(st.weShelf.size())) ? st.weSel : -1;
}

// 书目菜单：本地已有缓存就给"打开/重新缓存/删除"，否则只有"缓存整本并阅读"。
static std::vector<std::string> weMenuItems() {
  const int idx = weBookIndex();
  bool cached = false;
  if (idx >= 0) cached = Storage.exists(WeReadStore::finalBookPath(st.weShelf[idx]).c_str());
  if (cached) return {"打开本书", "重新缓存", "删除本地缓存", "取消"};
  return {"缓存整本并阅读", "取消"};
}

// 书目菜单的行高与首行 y（renderWereadMenu 与命中测试共用）。
static int weMenuItemH() { return uiLineHeight() + 12; }
static int weMenuFirstY() { return coverTop() + uiLineHeight() + 14; }

static void renderWeread() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = drawTabBar();

  // 还没有登录：先免责声明，再引导扫码。
  if (!weSessionValid()) {
    if (!WeReadStore::hasAcceptedDisclaimer()) {
      drawCenteredLine(top + 24, "微信读书");
      drawCenteredLine(top + 24 + (uiLineHeight() + 8), "非官方第三方功能");
      drawCenteredLine(top + 24 + 2 * (uiLineHeight() + 8), "与腾讯/微信读书无关");
      drawCenteredLine(top + 24 + 3 * (uiLineHeight() + 8), "接口可能随服务端变更失效");
      drawCenteredLine(top + 24 + 4 * (uiLineHeight() + 8), "账号与数据风险自负");
      drawFooter("回车 同意并继续   Esc 回书架");
      return;
    }
    drawCenteredLine(top + 40, "未登录");
    drawCenteredLine(top + 40 + uiLineHeight() + 8, "回车 扫码登录（手机微信扫码）");
    if (!st.weStatus.empty()) drawCenteredLine(statusTop() - uiLineHeight() - 12, st.weStatus.c_str());
    drawFooter("回车 扫码登录   ←→ 换标签   Esc 回书架");
    return;
  }

  if (!st.weShelfLoaded) weLoadShelf();
  if (st.weShelf.empty()) {
    drawCenteredLine(top + 40, "书架是空的");
    drawCenteredLine(top + 40 + uiLineHeight() + 8, st.weStatus.empty() ? "回车 重新同步" : st.weStatus.c_str());
    drawFooter("回车 重新同步   ←→ 换标签   Esc 回书架");
    return;
  }

  const int bottom = statusTop();
  const int itemH = weItemH();
  const int rows = std::max(1, (bottom - top) / itemH);
  st.weSel = clampI(st.weSel, 0, static_cast<int>(st.weShelf.size()) - 1);
  st.weScroll = clampI(st.weSel - rows / 2, 0, std::max(0, static_cast<int>(st.weShelf.size()) - rows));

  int y = top;
  for (int i = st.weScroll; i < static_cast<int>(st.weShelf.size()) && y + itemH <= bottom; i++) {
    const auto &b = st.weShelf[i];
    const bool sel = (i == st.weSel);
    if (sel) g_rd.fillRect(2, y, w - 4, itemH - 2, true);
    std::string title = b.title[0] ? b.title : "(无书名)";
    title = g_rd.truncatedText(uiFontId(), title.c_str(), w - 2 * MARGIN, EpdFontFamily::REGULAR);
    drawLineText(MARGIN, y + 2, title.c_str(), !sel, uiFontId());
    // 第二行：作者 + 本地是否已有缓存（只查可见行，书架可能有几百本）。
    const std::string finalPath = WeReadStore::finalBookPath(b);
    const bool cached = Storage.exists(finalPath.c_str());
    std::string sub = b.author;
    if (cached) sub += sub.empty() ? "已缓存" : "  ·  已缓存";
    sub = g_rd.truncatedText(uiFontId(), sub.c_str(), w - 2 * MARGIN, EpdFontFamily::REGULAR);
    drawLineText(MARGIN, y + 2 + uiLineHeight() + 2, sub.c_str(), !sel,
                 uiFontId());
    y += itemH;
  }
  if (!st.weStatus.empty()) drawCenteredLine(statusTop() - uiLineHeight() - 12, st.weStatus.c_str());
  drawFooter("↑↓ 选择  Enter 打开  ←→ 换标签  Esc 回书架");
}

static void renderWereadQr() {
  g_rd.clearScreen();
  drawTitle("微信读书 扫码登录");
  const int w = g_rd.getScreenWidth();
  const int top = coverTop();
  const int availH = statusTop() - top - 12;
  const int box = std::min(w - 2 * MARGIN, availH);
  const int cap = qrcodegen_BUFFER_LEN_FOR_VERSION(40);
  std::vector<uint8_t> temp(cap), qr(cap);
  if (box <= 0 || st.weQrUrl.empty() ||
      !qrcodegen_encodeText(st.weQrUrl.c_str(), temp.data(), qr.data(), qrcodegen_Ecc_LOW, 4, 40,
                            qrcodegen_Mask_AUTO, true)) {
    drawCenteredLine(top + 40, "等待登录二维码...");
    if (!st.weStatus.empty()) drawCenteredLine(top + 40 + uiLineHeight() + 8, st.weStatus.c_str());
    drawFooter("Esc 取消");
    return;
  }
  const int size = qrcodegen_getSize(qr.data());
  const int px = std::max(1, box / size);
  const int dim = size * px;
  const int x0 = (w - dim) / 2;
  const int y0 = top + (availH - dim) / 2;
  for (int cy = 0; cy < size; cy++)
    for (int cx = 0; cx < size; cx++)
      if (qrcodegen_getModule(qr.data(), cx, cy)) g_rd.fillRect(x0 + px * cx, y0 + px * cy, px, px, true);
  drawFooter("用微信扫描二维码  Esc 取消");
}

static void renderWereadMenu() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = drawTitle("微信读书");
  const int bi = weBookIndex();
  if (bi < 0) { drawCenteredLine(top + 20, "书目已失效，返回书架重选"); drawFooter("Esc 返回"); return; }
  const auto &b = st.weShelf[bi];
  drawLineText(MARGIN, top, b.title[0] ? b.title : "(无书名)", true);
  const int ly = top + uiLineHeight() + 6;
  g_rd.drawLine(0, ly, w, ly, true);

  const std::vector<std::string> items = weMenuItems();
  const int itemH = weMenuItemH();
  int y = ly + 8;
  for (int i = 0; i < static_cast<int>(items.size()); i++) {
    if (i == st.weMenuSel) {
      g_rd.fillRect(2, y, w - 4, itemH - 2, true);
      drawLineText(MARGIN, y + 6, items[i].c_str(), false);
    } else {
      drawLineText(MARGIN, y + 6, items[i].c_str(), true);
    }
    y += itemH;
  }
  if (!st.weStatus.empty()) drawCenteredLine(statusTop() - uiLineHeight() - 12, st.weStatus.c_str());
  drawFooter("↑↓ 选择  Enter 确定  Esc 返回");
}

static void renderWereadDl() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = drawTitle("微信读书 缓存图书");
  std::string title = st.weJobTitle.empty() ? "正在准备" : st.weJobTitle;
  title = g_rd.truncatedText(uiFontId(), title.c_str(), w - 2 * MARGIN, EpdFontFamily::REGULAR);
  drawLineText(MARGIN, top + 8, title.c_str(), true);

  uint32_t done = 0, total = 0;
  const char *stage = "准备中";
  if (st.weOp) {
    done = st.weOp->progressCompleted();
    total = st.weOp->progressTotal();
    stage = weStageName(st.weOp->progressStage());
  }
  int y = top + 8 + uiLineHeight() + 16;
  g_rd.drawText(uiFontId(), MARGIN, y + uiAsc(), stage, true);
  y += uiLineHeight() + 10;

  // 进度条（20 段）
  const int barW = w - 2 * MARGIN;
  const int barH = uiLineHeight();
  g_rd.drawRect(MARGIN, y, barW, barH, true);
  const int pct = (total > 0) ? static_cast<int>((static_cast<uint64_t>(done) * 100) / total) : 0;
  g_rd.fillRect(MARGIN + 1, y + 1, (barW - 2) * pct / 100, barH - 2, true);
  y += barH + 8;
  char buf[64];
  if (total > 0) snprintf(buf, sizeof(buf), "%u / %u  (%d%%)", static_cast<unsigned>(done),
                          static_cast<unsigned>(total), pct);
  else snprintf(buf, sizeof(buf), "处理中...");
  drawLineText(MARGIN, y, buf, true);

  drawFooter("Esc 取消");
}

static void handleWeread(int key) {
  if (key == KEY_LEFT) { switchTab(st.tab - 1); return; }
  if (key == KEY_RIGHT) { switchTab(st.tab + 1); return; }
  if (key == 0x1B || key == KEY_LONG_CONFIRM) { switchTab(0); return; }
  if (key == KEY_UP || key == KEY_DOWN) {
    if (!st.weShelf.empty()) {
      st.weSel = clampI(st.weSel + (key == KEY_DOWN ? 1 : -1), 0, static_cast<int>(st.weShelf.size()) - 1);
    }
    st.dirty = 1;
    return;
  }
  if (key != '\n') return;
  // 触摸点按只读一次：input_tap_xy 读后即清，读第二次一定是空的。
  int x = 0, y = 0;
  const bool tapped = input_tap_xy(&x, &y);
  if (tapped) {
    const int t = tabHit(x, y);
    if (t >= 0) { switchTab(t); return; }
  }
  if (!weSessionValid()) {
    if (!WeReadStore::hasAcceptedDisclaimer()) {
      WeReadStore::acceptDisclaimer();
      st.dirty = 1;
      return;
    }
    weBeginJob(0, nullptr);   // 登录 + 同步书架
    return;
  }
  if (st.weShelf.empty()) { weBeginJob(0, nullptr); return; }
  // 点按落到哪一行就选哪一行，再进书目菜单。
  if (tapped && y >= coverTop()) {
    const int idx = st.weScroll + (y - coverTop()) / weItemH();
    if (idx >= 0 && idx < static_cast<int>(st.weShelf.size())) st.weSel = idx;
  }
  st.weMenuSel = 0;
  st.weStatus.clear();
  st.mode = RdMode::WereadMenu;
  st.fullRefresh = true;
  st.dirty = 1;
}

static void handleWereadQr(int key) {
  if (key == 0x1B || key == KEY_LONG_CONFIRM) {
    if (st.weOp) st.weOp->cancel();
    return;
  }
}

static void handleWereadMenu(int key) {
  const auto items = weMenuItems();
  const int n = static_cast<int>(items.size());
  if (key == KEY_UP || key == KEY_DOWN) {
    st.weMenuSel = clampI(st.weMenuSel + (key == KEY_DOWN ? 1 : -1), 0, n - 1);
    st.dirty = 1;
    return;
  }
  if (key == 0x1B || key == KEY_LONG_CONFIRM) {
    st.weStatus.clear();
    st.mode = RdMode::Weread;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (key != '\n') return;
  int tx = 0, ty = 0;
  if (input_tap_xy(&tx, &ty)) {
    // 点在哪一行就选哪一行；点在菜单外（标题/空白）忽略。
    const int hit = (ty - weMenuFirstY()) / weMenuItemH();
    if (ty >= weMenuFirstY() && hit >= 0 && hit < n) st.weMenuSel = hit;
    else return;
  }
  const int idx = weBookIndex();
  if (idx < 0 || idx >= static_cast<int>(st.weShelf.size())) return;
  const auto &b = st.weShelf[idx];
  const std::string finalPath = WeReadStore::finalBookPath(b);
  const bool cached = Storage.exists(finalPath.c_str());
  const std::string label = items[clampI(st.weMenuSel, 0, n - 1)];

  if (label == "打开本书") {
    if (openBook(finalPath, 0)) { st.mode = RdMode::Reading; st.fullRefresh = true; st.dirty = 1; }
    else { st.weStatus = "打不开已缓存的书"; st.dirty = 1; }
    return;
  }
  if (label == "删除本地缓存") {
    if (Storage.exists(finalPath.c_str())) Storage.remove(finalPath.c_str());
    st.weStatus = "已删除本地缓存";
    st.dirty = 1;
    return;
  }
  if (label == "取消") {
    st.mode = RdMode::Weread;
    st.dirty = 1;
    return;
  }
  // 缓存整本 / 重新缓存
  (void)cached;
  weBeginJob(1, &b);
  if (st.weKind == 1 && st.weStatus.empty()) {
    st.mode = RdMode::WereadDl;
    st.fullRefresh = true;
    st.dirty = 1;
  }
}

static void handleWereadDl(int key) {
  if ((key == 0x1B || key == KEY_LONG_CONFIRM) && st.weOp) st.weOp->cancel();
}

// ── WiFi 管理 ────────────────────────────────────────────────────────────
static const int kWifiRows = 4;  // SSID / 密码 / 连接 / 断开
static int wifiListTop() { return coverTop() + (uiLineHeight() + 12); }  // 状态行 + 分隔线

static void renderWifi() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int top = drawTitle("WiFi 管理");
  int itemH = uiLineHeight() + 12;
  // 状态行
  std::string status;
  if (st.wifiBusy) status = "连接中...";
  else if (!st.wifiStatus.empty()) status = st.wifiStatus;
  else if (g_wifi.isConnected()) { std::string ip = g_wifi.getIp(); status = ip.empty() ? "已连接" : ("已连接  IP " + ip); }
  else status = "未连接";
  drawLineText(MARGIN, top, status.c_str(), true);
  int ly = top + itemH;
  g_rd.drawLine(0, ly, w, ly, true);
  // 行
  std::string rows[4];
  rows[0] = "SSID: " + st.wifiSsidEdit;
  rows[1] = "密码: " + std::string(st.wifiPassEdit.size(), '*');
  rows[2] = "连接";
  rows[3] = "断开";
  for (int i = 0; i < 4; i++) {
    int ry = ly + i * itemH;
    if (i == st.wifiField) {
      g_rd.fillRect(0, ry, w, itemH, true);
      drawLineText(MARGIN, ry + 6, rows[i].c_str(), false);
    } else {
      drawLineText(MARGIN, ry + 6, rows[i].c_str(), true);
    }
  }
  if (st.vkVisible) drawVk();
  else drawFooter(st.wifiEditing ? "输入中  Enter 完成  Esc 取消" : "↑↓ 选择  Enter 编辑/执行  Esc 返回");
  if (st.wifiEditing) rdDrawVkIcon();  // 只有在编辑字段（键盘可用）时才给开关
}

static void handleWifi(int key) {
  if (key == 0x1B) {
    if (st.wifiEditing) {
      IME::getInstance().cancelComposition();
      st.wifiEditing = false;
      st.vkVisible = false;
      st.dirty = 1;
    } else {
      st.mode = st.retMode;
      st.fullRefresh = true;
      st.dirty = 1;
    }
    return;
  }
  if (st.wifiEditing) {
    if (key == '\n') {
      int x, y;
      if (input_tap_xy(&x, &y)) {
        // 状态栏右端的键盘图标（编辑态才画）：点一下开/收键盘。
        if (rdVkIconHit(x, y)) {
          st.vkVisible = !st.vkVisible;
          st.dirty = 1;
          return;
        }
        if (st.vkVisible) {
          vkTap(x, y);
          st.dirty = 1;
          return;
        }
      }
      vkEnter();  // BLE/KEY3 回车 → 完成；键盘收起时的点按同样走"完成"
      st.dirty = 1;
      return;
    }
    if (key == 0x08) { feedVkBackspace(); st.dirty = 1; return; }
    if (key >= 0x20 && key <= 0x7E) { feedVkKey(key); st.dirty = 1; return; }
    return;
  }
  if (key == KEY_UP) { st.wifiField = std::max(0, st.wifiField - 1); st.wifiStatus.clear(); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.wifiField = std::min(kWifiRows - 1, st.wifiField + 1); st.wifiStatus.clear(); st.dirty = 1; return; }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      int itemH = uiLineHeight() + 12;
      int row = (y - wifiListTop()) / itemH;
      if (row >= 0 && row < kWifiRows) st.wifiField = row;
      st.wifiStatus.clear();
    }
    if (st.wifiField == 0 || st.wifiField == 1) {
      // 进入编辑
      st.wifiEditing = true;
      rdVkWantShow();
      st.dirty = 1;
    } else if (st.wifiField == 2) {
      // 连接（阻塞式，最多 10s，与写作模式设置页一致）
      if (st.wifiSsidEdit.empty()) { st.wifiStatus = "SSID 为空"; st.dirty = 1; return; }
      g_settings.setWifiSsid(st.wifiSsidEdit);
      g_settings.setWifiPassword(st.wifiPassEdit);
      st.wifiBusy = true;
      st.fullRefresh = true;
      st.dirty = 1;
      g_wifi.begin();
      bool ok = g_wifi.connect(st.wifiSsidEdit.c_str(), st.wifiPassEdit.c_str());
      st.wifiBusy = false;
      // 失败时把 reason 码翻成人话贴出来：最常见的是"密码错"与"找不到该 SSID"，两者
      // 的处理办法完全不同，笼统写"检查 SSID/密码"帮不上忙。
      st.wifiStatus = ok ? ("已连接  IP " + g_wifi.getIp())
                         : (std::string("连接失败: ") + g_wifi.lastReasonText());
      st.dirty = 1;
    } else if (st.wifiField == 3) {
      g_wifi.disconnect();
      st.wifiStatus = "已断开";
      st.dirty = 1;
    }
    return;
  }
}

// ── OPDS 书库 ───────────────────────────────────────────────────────────
// 列表条目来自当前 feed；末行固定为“目录地址”（Enter 进入编辑）。Esc 沿导航栈
// 逐级返回上一目录，栈空则回阅读菜单。
static int opdsListTop() { return coverTop() + (uiLineHeight() + 12); }  // 状态行 + 分隔线

// 按屏幕宽度截断（超出加省略号），避免 e-ink 上文字画出屏外。
static std::string fitWidth(const std::string &s, int maxW) {
  if (g_rd.getTextWidth(uiFontId(), s.c_str()) <= maxW) return s;
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    int n = utf8Len(static_cast<unsigned char>(s[i]));
    if (i + n > s.size()) break;
    std::string t = out + s.substr(i, n);
    if (g_rd.getTextWidth(uiFontId(), (t + "…").c_str()) > maxW) break;
    out = t;
    i += n;
  }
  return out + "…";
}

static std::string opdsRowText(int idx) {
  if (idx >= static_cast<int>(st.opdsEntries.size())) return "目录地址: " + st.opdsUrl;
  const OpdsEntry &e = st.opdsEntries[idx];
  if (e.type == OpdsEntryType::BOOK) {
    std::string s = "[书] " + e.title;
    if (!e.author.empty()) s += "  — " + e.author;
    return s;
  }
  return "[目录] " + e.title;
}

// 阻塞拉取一个 feed 并替换当前列表（导航栈不变）。
static void opdsLoadFeed(const std::string &url) {
  st.opdsBusy = true;
  st.opdsStatus = "加载中...";
  st.fullRefresh = true;
  st.dirty = 1;
  renderCurrent();  // 先刷出“加载中”，随后是阻塞式的网络请求
  st.fullRefresh = false;

  std::vector<OpdsEntry> entries;
  std::string next, err;
  bool ok = opdsFetchFeed(url, entries, next, err);
  st.opdsBusy = false;
  if (!ok) {
    st.opdsStatus = "加载失败: " + err;
    st.dirty = 1;
    return;
  }
  st.opdsUrl = url;
  st.opdsEntries = std::move(entries);
  st.opdsSel = 0;
  st.opdsStatus = std::to_string(st.opdsEntries.size()) + " 条";
  st.dirty = 1;
}

static void opdsBeginLoad(const std::string &url) {
  st.opdsStack.clear();
  opdsLoadFeed(url);
}

static void renderOpds() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int itemH = uiLineHeight() + 12;

  if (st.opdsEditing) {
    int top = drawTitle("OPDS 目录地址");
    drawLineText(MARGIN, top, "地址:", true);
    std::string shown = st.opdsUrlEdit.empty() ? "（空）" : st.opdsUrlEdit;
    drawLineText(MARGIN, top + itemH, fitWidth(shown, w - 2 * MARGIN).c_str(), true);
    drawLineText(MARGIN, top + 2 * itemH, "例: http://主机:端口/opds", true);
    if (st.vkVisible) drawVk();
    else drawFooter("输入中  Enter 完成  Esc 取消");
    rdDrawVkIcon();
    return;
  }

  int top = drawTitle("OPDS 书库");
  std::string status;
  if (st.opdsBusy) status = "网络请求中...";
  else if (!st.opdsStatus.empty()) status = st.opdsStatus;
  else if (!g_wifi.isConnected()) status = "未连接 WiFi（先到 WiFi 管理连接）";
  else status = "未设置目录地址";
  drawLineText(MARGIN, top, fitWidth(status, w - 2 * MARGIN).c_str(), true);
  int ly = top + itemH;
  g_rd.drawLine(0, ly, w, ly, true);

  int n = static_cast<int>(st.opdsEntries.size()) + 1;  // 末行 = 地址行
  int viewH = statusTop() - ly - 8;
  int maxRows = std::max(1, viewH / itemH);
  int start = clampI(st.opdsSel - maxRows / 2, 0, std::max(0, n - maxRows));
  for (int i = 0; i < maxRows && start + i < n; i++) {
    int idx = start + i;
    int y = ly + i * itemH;
    std::string txt = fitWidth(opdsRowText(idx), w - 2 * MARGIN);
    if (idx == st.opdsSel) {
      g_rd.fillRect(0, y, w, itemH, true);
      drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, txt.c_str(), false);
    } else {
      drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, txt.c_str(), true);
    }
  }
  drawFooter("↑↓ 选择  Enter 打开/下载  Esc 返回");
}

static void handleOpds(int key) {
  if (st.opdsEditing) {
    if (key == 0x1B) {
      IME::getInstance().cancelComposition();
      st.opdsEditing = false;
      st.vkVisible = false;
      st.dirty = 1;
      return;
    }
    if (key == '\n') {
      int x, y;
      if (input_tap_xy(&x, &y)) {
        // 状态栏右端的键盘图标：点一下开/收键盘。
        if (rdVkIconHit(x, y)) {
          st.vkVisible = !st.vkVisible;
          st.dirty = 1;
          return;
        }
        if (st.vkVisible) {
          vkTap(x, y);
          st.dirty = 1;
          return;
        }
      }
      vkEnter();  // 非点按回车（BLE/KEY3）→ 保存并加载；键盘收起时的点按同样走"保存"
      st.dirty = 1;
      return;
    }
    if (key == 0x08) { feedVkBackspace(); st.dirty = 1; return; }
    if (key >= 0x20 && key <= 0x7E) { feedVkKey(key); st.dirty = 1; return; }
    return;
  }

  if (st.opdsBusy) return;  // 阻塞请求期间不接受输入

  int n = static_cast<int>(st.opdsEntries.size()) + 1;

  if (key == 0x1B) {
    if (!st.opdsStack.empty()) {
      std::string prev = st.opdsStack.back();
      st.opdsStack.pop_back();
      opdsLoadFeed(prev);
    } else {
      st.mode = st.retMode;
      st.fullRefresh = true;
      st.dirty = 1;
    }
    return;
  }
  if (key == KEY_UP) { st.opdsSel = std::max(0, st.opdsSel - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.opdsSel = std::min(n - 1, st.opdsSel + 1); st.dirty = 1; return; }
  if (key == KEY_PAGE_UP) { st.opdsSel = std::max(0, st.opdsSel - 8); st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.opdsSel = std::min(n - 1, st.opdsSel + 8); st.dirty = 1; return; }
  if (key != '\n') return;

  int x, y;
  if (input_tap_xy(&x, &y)) {
    int itemH = uiLineHeight() + 12;
    int ly = opdsListTop();
    int viewH = statusTop() - ly - 8;
    int maxRows = std::max(1, viewH / itemH);
    int start = clampI(st.opdsSel - maxRows / 2, 0, std::max(0, n - maxRows));
    int row = start + (y - ly) / itemH;
    if (row >= 0 && row < n) st.opdsSel = row;
    st.dirty = 1;
    return;
  }

  if (st.opdsSel >= static_cast<int>(st.opdsEntries.size())) {
    st.opdsUrlEdit = st.opdsUrl;  // 地址行 → 编辑
    st.opdsEditing = true;
    rdVkWantShow();
    st.dirty = 1;
    return;
  }

  const OpdsEntry &e = st.opdsEntries[st.opdsSel];
  if (e.type == OpdsEntryType::NAVIGATION) {
    st.opdsStack.push_back(st.opdsUrl);
    opdsLoadFeed(e.href);
    return;
  }

  // 电子书：下载到 /sdcard/books/
  std::string fname = opdsFilenameFromUrl(e.href);
  if (fname.empty()) fname = "opds_book.epub";
  Storage.mkdir("/sdcard/books", true);
  std::string dest = std::string("/sdcard/books/") + fname;
  std::string href = e.href;  // 拷贝：下面的 renderCurrent 不再改动 vector，但保险起见

  st.opdsBusy = true;
  st.opdsStatus = "准备下载 " + fname;
  st.fullRefresh = true;
  st.dirty = 1;
  renderCurrent();
  st.fullRefresh = false;

  int lastPct = -1;
  size_t lastBytes = 0;
  auto progress = [&](size_t got, size_t total) {
    char b[96];
    if (total > 0) {
      int pct = static_cast<int>(got * 100 / total);
      if (pct < lastPct + 5) return;
      lastPct = pct;
      snprintf(b, sizeof(b), "下载 %d%%  %u/%u KB", pct, static_cast<unsigned>(got / 1024),
               static_cast<unsigned>(total / 1024));
    } else {
      if (got < lastBytes + 256 * 1024) return;
      lastBytes = got;
      snprintf(b, sizeof(b), "下载 %u KB", static_cast<unsigned>(got / 1024));
    }
    st.opdsStatus = b;
    st.fullRefresh = false;
    st.dirty = 1;
    renderCurrent();
  };

  std::string err;
  bool ok = opdsDownloadFile(href, dest, progress, err);
  st.opdsBusy = false;
  if (ok) {
    st.opdsStatus = "已下载 " + fname;
    scanBooks();
  } else {
    st.opdsStatus = "下载失败: " + err;
  }
  st.fullRefresh = true;
  st.dirty = 1;
}

// ── 词典下载 ────────────────────────────────────────────────────────────
// 清单来自 crossmux 的词典 API（JSON，schema 见 dictionary_store.h），下载后装到
// /sdcard/dictionaries/<id>/，与阅读菜单里的「词典」查询共用同一个目录。
// 下载是阻塞式的（44MB 的大词典要几分钟），期间走进度版面。
static int dictDlRowCount() {
  return static_cast<int>(st.dictCat.items.size()) + 1;  // 末行 = 重新获取清单
}

// 确保 WiFi 已连（未连则按设置里的 SSID/密码连一次）。
static bool readerEnsureWifi(std::string &err) {
  if (g_wifi.isConnected()) return true;
  std::string ssid = g_settings.wifiSsid();
  std::string pass = g_settings.wifiPassword();
  if (ssid.empty()) {
    err = "未配置 WiFi（先到 WiFi 管理填写）";
    return false;
  }
  g_wifi.begin();
  if (!g_wifi.connect(ssid.c_str(), pass.c_str())) {
    err = "WiFi 连接失败";
    return false;
  }
  return true;
}

// 用本地目录里的安装状态刷新清单条目。
static void dictDlRefreshInstalled() {
  std::vector<DictLocalItem> local;
  dictListLocal(local);
  for (auto &it : st.dictCat.items) {
    it.installedRevision = 0;
    for (const auto &l : local)
      if (l.id == it.id) { it.installedRevision = l.revision; break; }
  }
  st.dictOpen = false;  // 目录内容变了，查询界面下次重新解析
}

static std::string dictDlRowText(int idx) {
  if (idx >= static_cast<int>(st.dictCat.items.size())) return "重新获取清单";
  const DictCatalogItem &it = st.dictCat.items[idx];
  std::string s;
  if (it.updateAvailable()) s = "↑ ";
  else if (it.installed()) s = "✓ ";
  else s = "· ";
  s += it.name;
  s += "  " + dictFormatSize(it.totalSize);
  if (it.updateAvailable()) s += " 有更新";
  return s;
}

static void dictDlLoadCatalog() {
  st.dictDlBusy = true;
  st.dictDlStatus = "加载中...";
  st.fullRefresh = true;
  st.dirty = 1;
  renderCurrent();  // 先刷出“加载中”，随后是阻塞式请求
  st.fullRefresh = false;

  std::string url = g_settings.getString("dict_manifest_url");
  if (url.empty()) url = DICT_MANIFEST_DEFAULT;

  // 先连 WiFi 再拉清单：原来这里**不连**，只有下面的下载步骤才连——没网时清单请求
  // 直接死在 DNS（日志：esp-tls couldn't get hostname … getaddrinfo() returns 202），
  // 用户看到的是"清单获取失败: ESP_ERR_HTTP_CONNECT"，完全不知道是没联网。
  if (!g_wifi.isConnected()) {
    st.dictDlStatus = "正在连接 WiFi...";
    st.dirty = 1;
    renderCurrent();
    std::string werr;
    if (!readerEnsureWifi(werr)) {
      st.dictDlBusy = false;
      st.dictDlStatus = werr;
      st.dirty = 1;
      return;
    }
    st.dictDlStatus = "加载中...";
    st.dirty = 1;
  }

  DictCatalog cat;
  std::string err;
  bool ok = dictCatalogFetch(url, cat, err);
  st.dictDlBusy = false;
  if (!ok) {
    st.dictDlStatus = "清单获取失败: " + err;
    st.dirty = 1;
    return;
  }
  st.dictCat = std::move(cat);
  st.dictDlSel = 0;
  st.dictDlDelArm.clear();
  dictDlRefreshInstalled();
  st.dictDlStatus = std::to_string(st.dictCat.items.size()) + " 个词典  rev " +
                    std::to_string(st.dictCat.revision);
  st.dirty = 1;
}

// 进度条：边框 + 按比例填充。
static void drawProgressBar(int x, int y, int w, int h, size_t got, size_t total) {
  g_rd.drawRect(x, y, w, h, true);
  if (total == 0 || w <= 6 || h <= 6) return;
  int fw = static_cast<int>(static_cast<uint64_t>(w - 4) * got / total);
  if (fw > w - 4) fw = w - 4;
  if (fw > 0) g_rd.fillRect(x + 2, y + 2, fw, h - 4, true);
}

static void renderDictDlProgress() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int top = drawTitle("词典下载");
  int idx = clampI(st.dictDlSel, 0, static_cast<int>(st.dictCat.items.size()) - 1);
  const char *name = idx >= 0 ? st.dictCat.items[idx].name.c_str() : "";

  drawLineText(MARGIN, top, fitWidth(name, w - 2 * MARGIN).c_str(), true);
  int y = top + uiLineHeight() + 6;

  char line[128];
  snprintf(line, sizeof(line), "%s  (%d/%d)", st.dictDlPhase.c_str(), st.dictDlFileIdx + 1,
           st.dictDlFileCount);
  drawLineText(MARGIN, y, fitWidth(line, w - 2 * MARGIN).c_str(), true);
  y += uiLineHeight() + 4;

  int barH = uiLineHeight();
  drawProgressBar(MARGIN, y, w - 2 * MARGIN, barH, st.dictDlDone, st.dictDlTotal);
  y += barH + 10;

  size_t pct = st.dictDlTotal ? st.dictDlDone * 100 / st.dictDlTotal : 0;
  snprintf(line, sizeof(line), "总进度 %u%%   %s / %s", static_cast<unsigned>(pct),
           dictFormatSize(st.dictDlDone).c_str(), dictFormatSize(st.dictDlTotal).c_str());
  drawLineText(MARGIN, y, fitWidth(line, w - 2 * MARGIN).c_str(), true);
  y += uiLineHeight() + 4;

  if (st.dictDlFileTotal) {
    snprintf(line, sizeof(line), "本文件 %s / %s", dictFormatSize(st.dictDlFileGot).c_str(),
             dictFormatSize(st.dictDlFileTotal).c_str());
    drawLineText(MARGIN, y, fitWidth(line, w - 2 * MARGIN).c_str(), true);
  }

  drawFooter("下载中… 请勿断电");
}

static void renderDictDl() {
  if (st.dictDlBusy && st.dictDlTotal > 0) {
    renderDictDlProgress();
    return;
  }
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int itemH = uiLineHeight() + 12;
  int top = drawTitle("词典下载");

  std::string status;
  if (st.dictDlBusy) status = "网络请求中...";
  else if (!st.dictDlStatus.empty()) status = st.dictDlStatus;
  else if (!g_wifi.isConnected()) status = "未连接 WiFi（先到 WiFi 管理连接）";
  else status = "正在获取清单...";
  drawLineText(MARGIN, top, fitWidth(status, w - 2 * MARGIN).c_str(), true);
  int ly = top + itemH;
  g_rd.drawLine(0, ly, w, ly, true);

  int n = dictDlRowCount();
  int viewH = statusTop() - ly - 8;
  int maxRows = std::max(1, viewH / itemH);
  int start = clampI(st.dictDlSel - maxRows / 2, 0, std::max(0, n - maxRows));
  for (int i = 0; i < maxRows && start + i < n; i++) {
    int idx = start + i;
    int y = ly + i * itemH;
    std::string txt = fitWidth(dictDlRowText(idx), w - 2 * MARGIN);
    if (idx == st.dictDlSel) {
      g_rd.fillRect(0, y, w, itemH, true);
      drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, txt.c_str(), false);
    } else {
      drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, txt.c_str(), true);
    }
  }
  drawFooter("↑↓ 选择  Enter 下载/更新  长按 删除  Esc 返回");
}

static void dictDlInstall() {
  int idx = clampI(st.dictDlSel, 0, static_cast<int>(st.dictCat.items.size()) - 1);
  if (idx < 0) return;
  const DictCatalogItem &item = st.dictCat.items[idx];

  if (!g_wifi.isConnected()) {
    st.dictDlStatus = "正在连接 WiFi...";
    st.fullRefresh = true;
    st.dirty = 1;
    renderCurrent();
    st.fullRefresh = false;
    std::string werr;
    if (!readerEnsureWifi(werr)) {
      st.dictDlStatus = werr;
      st.dirty = 1;
      return;
    }
  }

  st.dictDlBusy = true;
  st.dictDlPhase = "准备中";
  st.dictDlDone = 0;
  st.dictDlTotal = item.totalSize;
  st.dictDlFileIdx = 0;
  st.dictDlFileCount = item.fileCount();
  st.dictDlFileGot = st.dictDlFileTotal = 0;
  st.fullRefresh = true;
  st.dirty = 1;
  renderCurrent();
  st.fullRefresh = false;

  int lastBucket = -1;
  std::string id = item.id;
  auto progress = [&](int fi, int fc, size_t got, size_t total, const char *phase) {
    size_t done = got;
    for (int k = 0; k < fi && k < item.fileCount(); k++) done += item.files[k].size;
    size_t all = item.totalSize ? item.totalSize : 1;
    st.dictDlPhase = phase;
    st.dictDlFileIdx = fi;
    st.dictDlFileCount = fc;
    st.dictDlFileGot = got;
    st.dictDlFileTotal = total;
    st.dictDlDone = done;
    int bucket = static_cast<int>(done * 20 / all);  // 5% 一档，避免每 4KB 重刷一次
    if (bucket == lastBucket) return;
    lastBucket = bucket;
    st.fullRefresh = false;
    st.dirty = 1;
    renderCurrent();
  };

  std::string err;
  bool ok = dictInstall(st.dictCat, idx, progress, err);
  st.dictDlBusy = false;
  st.dictDlTotal = 0;
  if (ok) {
    // 首次安装时把它设为当前词典，省得再去设置里选。
    if (g_settings.getString("reader_dict").empty()) g_settings.setString("reader_dict", id);
    dictDlRefreshInstalled();
    st.dictDlStatus = "已安装 " + item.name;
  } else {
    st.dictDlStatus = "下载失败: " + err;
  }
  st.fullRefresh = true;
  st.dirty = 1;
}

static void handleDictDl(int key) {
  if (st.dictDlBusy) return;  // 阻塞请求期间不接受输入
  int n = dictDlRowCount();

  if (key == 0x1B) {
    st.dictDlDelArm.clear();
    st.mode = st.retMode;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (key == KEY_UP) { st.dictDlSel = std::max(0, st.dictDlSel - 1); st.dictDlDelArm.clear(); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.dictDlSel = std::min(n - 1, st.dictDlSel + 1); st.dictDlDelArm.clear(); st.dirty = 1; return; }
  if (key == KEY_PAGE_UP) { st.dictDlSel = std::max(0, st.dictDlSel - 8); st.dictDlDelArm.clear(); st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.dictDlSel = std::min(n - 1, st.dictDlSel + 8); st.dictDlDelArm.clear(); st.dirty = 1; return; }

  // 长按中间键删除本地词典：第一次置位并提示，再长按一次才真删。
  if (key == KEY_LONG_CONFIRM) {
    if (st.dictDlSel >= static_cast<int>(st.dictCat.items.size())) return;
    const DictCatalogItem &it = st.dictCat.items[st.dictDlSel];
    if (!it.installed()) {
      st.dictDlStatus = "未安装，无需删除";
      st.dirty = 1;
      return;
    }
    if (st.dictDlDelArm != it.id) {
      st.dictDlDelArm = it.id;
      st.dictDlStatus = "再长按一次删除 " + it.name;
      st.dirty = 1;
      return;
    }
    std::string err;
    bool ok = dictDelete(it.id, err);
    st.dictDlDelArm.clear();
    if (ok) {
      dictDlRefreshInstalled();
      st.dictDlStatus = "已删除 " + it.name;
    } else {
      st.dictDlStatus = "删除失败: " + err;
    }
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }

  if (key != '\n') return;

  int x, y;
  if (input_tap_xy(&x, &y)) {
    int itemH = uiLineHeight() + 12;
    int ly = coverTop() + itemH;
    int viewH = statusTop() - ly - 8;
    int maxRows = std::max(1, viewH / itemH);
    int start = clampI(st.dictDlSel - maxRows / 2, 0, std::max(0, n - maxRows));
    int row = start + (y - ly) / itemH;
    if (row >= 0 && row < n) st.dictDlSel = row;
    st.dictDlDelArm.clear();
    st.dirty = 1;
    return;
  }

  if (st.dictDlSel >= static_cast<int>(st.dictCat.items.size())) {
    dictDlLoadCatalog();
    return;
  }
  dictDlInstall();
}

// ── 资源下载 ────────────────────────────────────────────────────────────
// 原写作模式设置的「资源下载」分类（词典清单地址 / 字体下载地址 / 下载字体），
// 按用户要求整类搬到阅读设置的标签下。清单地址就是「词典下载」读取的那一个键
// （dict_manifest_url，空=内置默认），所以放这儿正对着它的使用方；字体下完落到
// /sdcard/fonts/，再回「字体」设置里挑。布局与 WiFi 管理同构（状态行 + 字段行）。
static const int kResRows = 3;                                          // 清单地址 / 字体地址 / 下载
static int resListTop() { return coverTop() + (uiLineHeight() + 12); }  // 状态行 + 分隔线

// 编辑中的字段显示尾巴（正在输入的是末尾），其余字段显示开头并截断——地址很长时
// 两者都不至于把行画到屏外。
static std::string resRowText(const std::string &s, bool tail) {
  const int maxW = g_rd.getScreenWidth() - 2 * MARGIN;
  if (g_rd.getTextWidth(uiFontId(), s.c_str()) <= maxW) return s;
  std::string out;
  if (!tail) {
    size_t i = 0;
    while (i < s.size()) {
      int n = utf8Len(static_cast<unsigned char>(s[i]));
      if (i + n > s.size()) break;
      std::string t = out + s.substr(i, n);
      if (g_rd.getTextWidth(uiFontId(), (t + "…").c_str()) > maxW) break;
      out = t;
      i += n;
    }
    return out + "…";
  }
  size_t i = s.size();
  while (i > 0) {
    size_t p = i - 1;
    while (p > 0 && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80) p--;
    std::string t = "…" + s.substr(p);
    if (g_rd.getTextWidth(uiFontId(), t.c_str()) > maxW) break;
    out = s.substr(p);
    i = p;
  }
  return "…" + out;
}

static void renderResDl() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int top = drawTitle("资源下载");
  int itemH = uiLineHeight() + 12;
  std::string status;
  if (st.resBusy) status = "正在下载字体 " + std::to_string(st.resPct) + "%";
  else if (!st.resStatus.empty()) status = st.resStatus;
  else if (!g_wifi.isConnected()) status = "未连接 WiFi（下载前会先连）";
  else { std::string ip = g_wifi.getIp(); status = ip.empty() ? "已连接" : ("已连接  IP " + ip); }
  drawLineText(MARGIN, top, fitWidth(status, w - 2 * MARGIN).c_str(), true);
  int ly = top + itemH;
  g_rd.drawLine(0, ly, w, ly, true);
  std::string rows[kResRows];
  rows[0] = "词典清单地址: " + (st.resDictEdit.empty() ? std::string("(内置默认)") : st.resDictEdit);
  rows[1] = "字体下载地址: " + (st.resFontEdit.empty() ? std::string("(未设置)") : st.resFontEdit);
  rows[2] = "下载字体";
  for (int i = 0; i < kResRows; i++) {
    int ry = ly + i * itemH;
    const bool sel = (i == st.resField);
    std::string t = (i == 2) ? rows[i]
                             : resRowText(rows[i], sel && st.resEditing);  // 编辑时看尾巴
    if (sel) {
      g_rd.fillRect(0, ry, w, itemH, true);
      drawLineText(MARGIN, ry + 6, t.c_str(), false);
    } else {
      drawLineText(MARGIN, ry + 6, t.c_str(), true);
    }
  }
  if (st.vkVisible) drawVk();
  else drawFooter(st.resEditing ? "输入中  Enter 完成  Esc 取消"
                                : "↑↓ 选择  Enter 编辑/下载  Esc 返回");
  if (st.resEditing) rdDrawVkIcon();  // 只有在编辑字段（键盘可用）时才给开关
}

// 下载字体：地址→文件名（强制 .ttf，ttf_font_scan 只认它）→ 必要时先连 WiFi →
// 阻塞下载（每 20% 刷一次）→ 校验 sfnt 头再收下。逻辑与写作设置里那份一致。
static void resDlInstallFont() {
  // 顺手把用户刚改过的两个地址落盘：下面要用到字体地址，而清单地址与它同屏。
  g_settings.setString("dict_manifest_url", st.resDictEdit);
  g_settings.setString("font_dl_url", st.resFontEdit);
  const std::string url = st.resFontEdit;
  if (url.empty()) { st.resStatus = "请先填字体下载地址"; st.dirty = 1; return; }

  std::string fname = opdsFilenameFromUrl(url);
  if (fname.empty()) fname = "font";
  if (fname.size() < 4 || strcasecmp(fname.c_str() + fname.size() - 4, ".ttf") != 0) fname += ".ttf";
  Storage.mkdir("/sdcard/fonts", true);  // 已存在则忽略
  const std::string dest = std::string("/sdcard/fonts/") + fname;

  if (!g_wifi.isConnected()) {
    st.resStatus = "正在连接 WiFi...";
    st.fullRefresh = true;
    st.dirty = 1;
    renderCurrent();
    st.fullRefresh = false;
    std::string werr;
    if (!readerEnsureWifi(werr)) { st.resStatus = werr; st.dirty = 1; return; }
  }

  st.resBusy = true;
  st.resPct = 0;
  st.resStatus.clear();
  st.fullRefresh = true;
  st.dirty = 1;
  renderCurrent();
  st.fullRefresh = false;

  int lastPct = -20;
  auto progress = [&](size_t got, size_t total) {
    if (total == 0) return;
    int pct = static_cast<int>(got * 100 / total);
    if (pct < lastPct + 20) return;  // 每 20% 刷一次，别把 e-ink 刷爆
    lastPct = pct;
    st.resPct = pct;
    st.dirty = 1;
    renderCurrent();
  };
  std::string err;
  bool ok = opdsDownloadFile(url, dest, progress, err);
  st.resBusy = false;
  std::string msg;
  if (ok) {
    // 只认四种 sfnt 头，避免把 HTML 错误页存成 .ttf 后扫描器读崩。
    FILE *fp = fopen(dest.c_str(), "rb");
    uint8_t magic[4] = {0};
    if (fp) { size_t n = fread(magic, 1, 4, fp); fclose(fp); (void)n; }
    uint32_t m = (uint32_t)magic[0] << 24 | magic[1] << 16 | magic[2] << 8 | magic[3];
    bool isFont = (m == 0x00010000u) || (m == 0x74727565u) ||  // \x00\x01\x00\x00 / "true"
                  (m == 0x4F54544Fu) || (m == 0x74797031u);   // "OTTO" / "typ1"
    if (!isFont) {
      remove(dest.c_str());
      msg = "不是有效的 TTF 文件";
    } else {
      ttf_font_scan();
      msg = "已下载 " + fname + "（到字体设置里选）";
    }
  } else {
    msg = "下载失败: " + err;
  }
  st.resStatus = msg;
  st.fullRefresh = true;
  st.dirty = 1;
}

static void handleResDl(int key) {
  if (st.resBusy) return;  // 阻塞下载期间不接受输入
  if (key == 0x1B) {
    if (st.resEditing) {
      IME::getInstance().cancelComposition();
      st.resEditing = false;
      st.vkVisible = false;
      st.dirty = 1;
    } else {
      st.mode = st.retMode;
      st.fullRefresh = true;
      st.dirty = 1;
    }
    return;
  }
  if (st.resEditing) {
    if (key == '\n') {
      int x, y;
      if (input_tap_xy(&x, &y)) {
        if (rdVkIconHit(x, y)) { st.vkVisible = !st.vkVisible; st.dirty = 1; return; }
        if (st.vkVisible) { vkTap(x, y); st.dirty = 1; return; }
      }
      vkEnter();  // 提交并退出编辑（vkEnter 的 ResDl 分支里已落盘、已清 resEditing）
      st.dirty = 1;
      return;
    }
    if (key == 0x08) { feedVkBackspace(); st.dirty = 1; return; }
    if (key >= 0x20 && key <= 0x7E) { feedVkKey(key); st.dirty = 1; return; }
    return;
  }
  if (key == KEY_UP) { st.resField = std::max(0, st.resField - 1); st.resStatus.clear(); st.dirty = 1; return; }
  if (key == KEY_DOWN) {
    st.resField = std::min(kResRows - 1, st.resField + 1);
    st.resStatus.clear();
    st.dirty = 1;
    return;
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      int itemH = uiLineHeight() + 12;
      int row = (y - resListTop()) / itemH;
      if (row >= 0 && row < kResRows) st.resField = row;
      st.resStatus.clear();
    }
    if (st.resField == 0 || st.resField == 1) {
      st.resEditing = true;
      rdVkWantShow();
      st.dirty = 1;
    } else if (st.resField == 2) {
      resDlInstallFont();
    }
    return;
  }
}

// ── BLE 按键映射 ────────────────────────────────────────────────────────
// 把蓝牙键盘/遥控器上的任意一个键绑到阅读动作（翻页/确认/返回/上下左右）。
// 廉价遥控器/翻页器只发消费类(HID Consumer Control)报告，键码是
// KEY_CONSUMER_BASE|usage（见 bt_keyboard.cpp），不映射就完全没反应。
// 映射只在阅读模式内生效——翻译发生在 screen_reader_handle 顶部，写作模式不受影响。
static void renderKeyMap() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int itemH = uiLineHeight() + 12;
  int top = drawTitle("按键映射");

  std::string status;
  if (st.keyMapCapture >= 0) status = "请按遥控器/键盘上要绑定的键…（Esc 取消）";
  else if (!st.keyMapStatus.empty()) status = st.keyMapStatus;
  else if (!g_bt.isConnected()) status = "未连接蓝牙设备（先到「蓝牙管理」配对）";
  else status = "选中一行按 Enter，再按要绑定的键";
  drawLineText(MARGIN, top, fitWidth(status, w - 2 * MARGIN).c_str(), true);
  int ly = top + itemH;
  g_rd.drawLine(0, ly, w, ly, true);

  for (int i = 0; i < BLE_ACT_COUNT; i++) {
    int y = ly + i * itemH;
    BleAct a = static_cast<BleAct>(i);
    int src = bleKeymapSrcOf(a);
    std::string txt = std::string(bleKeymapActLabel(a)) + "  →  " +
                      (src ? bleKeymapKeyName(src) : std::string("未绑定"));
    txt = fitWidth(txt, w - 2 * MARGIN);
    if (i == st.keyMapSel) {
      g_rd.fillRect(0, y, w, itemH, true);
      drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, txt.c_str(), false);
    } else {
      drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, txt.c_str(), true);
    }
  }
  drawFooter(st.keyMapCapture >= 0 ? "按遥控器/键盘上的键绑定  Esc 取消"
                                   : "↑↓ 选择  Enter 绑定  长按 解绑  Esc 返回");
}

static void handleKeyMap(int key) {
  // 捕获中：下一个来自蓝牙的键就是要绑的源键。
  if (st.keyMapCapture >= 0) {
    if (key == 0x1B) { st.keyMapCapture = -1; st.keyMapStatus.clear(); st.fullRefresh = true; st.dirty = 1; return; }
    if (!g_key_from_ble) return;  // 触摸点按（以 '\n' 到达）不参与绑定
    BleAct a = static_cast<BleAct>(st.keyMapCapture);
    bool ok = bleKeymapBind(key, a);
    st.keyMapStatus = ok ? (std::string("已绑定 ") + bleKeymapActLabel(a) + " → " + bleKeymapKeyName(key))
                         : std::string("绑定失败（写入 SD 卡出错）");
    st.keyMapCapture = -1;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }

  // 长按中间键解绑当前动作。
  if (key == KEY_LONG_CONFIRM) {
    BleAct a = static_cast<BleAct>(st.keyMapSel);
    if (bleKeymapSrcOf(a) == 0) { st.keyMapStatus = "未绑定，无需解绑"; st.dirty = 1; return; }
    bleKeymapClear(a);
    st.keyMapStatus = std::string("已解绑 ") + bleKeymapActLabel(a);
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }

  if (key == 0x1B) { st.keyMapStatus.clear(); st.mode = st.retMode; st.fullRefresh = true; st.dirty = 1; return; }
  if (key == KEY_UP) { st.keyMapSel = std::max(0, st.keyMapSel - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.keyMapSel = std::min(BLE_ACT_COUNT - 1, st.keyMapSel + 1); st.dirty = 1; return; }
  if (key == KEY_PAGE_UP) { st.keyMapSel = 0; st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.keyMapSel = BLE_ACT_COUNT - 1; st.dirty = 1; return; }

  if (key != '\n') return;

  int x, y;
  if (input_tap_xy(&x, &y)) {
    int itemH = uiLineHeight() + 12;
    int ly = coverTop() + itemH;
    int row = (y - ly) / itemH;
    if (row >= 0 && row < BLE_ACT_COUNT) st.keyMapSel = row;
    st.dirty = 1;
    return;
  }
  st.keyMapCapture = st.keyMapSel;  // 开始捕获
  st.keyMapStatus.clear();
  st.fullRefresh = true;
  st.dirty = 1;
}

// ── 自定义状态栏（设置界面）──────────────────────────────────────────────
// 逐行选择阅读页底部状态栏显示的内容。左右键（或 Enter）轮转取值，Esc 存盘返回——
// 每改一项立刻写 settings，回到阅读页即生效。
static const char *const kSbTitleNames[] = {"书名", "章节", "隐藏"};
static const char *const kSbPageNames[] = {"章节", "全书", "隐藏"};
static const char *const kSbShowNames[] = {"显示", "隐藏"};
static const char *const kSbBarNames[] = {"全书", "章节", "隐藏"};
static const char *const kSbThickNames[] = {"细", "中", "粗"};
static const char *const kSbClockNames[] = {"隐藏", "右侧", "左侧"};

struct SbRow {
  const char *key;
  const char *label;
  int def;
  const char *const *names;
  int nameCount;
};
static const SbRow kSbRows[] = {
    {"reader_sb_title", "标题", 0, kSbTitleNames, 3},
    {"reader_sb_page", "页码", 1, kSbPageNames, 3},
    {"reader_sb_pct", "百分比", 0, kSbShowNames, 2},
    {"reader_sb_bar", "进度条", 2, kSbBarNames, 3},
    {"reader_sb_thick", "进度条粗细", 1, kSbThickNames, 3},
    {"reader_sb_batt", "电量", 0, kSbShowNames, 2},
    {"reader_sb_clock", "时钟", 0, kSbClockNames, 3},
};
static const int kSbRowCount = static_cast<int>(sizeof(kSbRows) / sizeof(kSbRows[0]));

static void renderStatusBarSet() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int itemH = uiLineHeight() + 12;
  int top = drawTitle("状态栏");

  int n = kSbRowCount;
  int viewH = statusTop() - top - 8;
  int maxRows = std::max(1, viewH / itemH);
  int start = clampI(st.sbSel - maxRows / 2, 0, std::max(0, n - maxRows));
  for (int i = 0; i < maxRows && start + i < n; i++) {
    int idx = start + i;
    const SbRow &r = kSbRows[idx];
    int v = sbCount(r.key, r.def, r.nameCount);
    int y = top + i * itemH;
    std::string txt = fitWidth(std::string(r.label) + ":  " + r.names[v], w - 2 * MARGIN);
    if (idx == st.sbSel) {
      g_rd.fillRect(0, y, w, itemH, true);
      drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, txt.c_str(), false);
    } else {
      drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, txt.c_str(), true);
    }
  }
  drawFooter("↑↓ 选择  ←→/Enter 修改  Esc 返回阅读");
}

static void handleStatusBarSet(int key) {
  // Esc 直接回阅读页：这些是显示项，回不去就看不到效果，就地返回最顺手。
  // 但若是从「设置」标签进来的（retMode==Settings），那里没有阅读页可回，退回设置标签。
  if (key == 0x1B) {
    st.mode = (st.retMode == RdMode::Settings) ? RdMode::Settings : RdMode::Reading;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (key == KEY_UP) { st.sbSel = std::max(0, st.sbSel - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.sbSel = std::min(kSbRowCount - 1, st.sbSel + 1); st.dirty = 1; return; }
  if (key == KEY_PAGE_UP) { st.sbSel = 0; st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.sbSel = kSbRowCount - 1; st.dirty = 1; return; }

  if (key != KEY_LEFT && key != KEY_RIGHT && key != '\n') return;

  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {  // 点按：先选中该行，不直接改值
      int itemH = uiLineHeight() + 12;
      int ly = coverTop();
      int row = (y - ly) / itemH;
      if (row >= 0 && row < kSbRowCount) st.sbSel = row;
      st.dirty = 1;
      return;
    }
  }

  const SbRow &r = kSbRows[st.sbSel];
  int v = sbCount(r.key, r.def, r.nameCount);
  int d = (key == KEY_LEFT) ? -1 : 1;  // Enter 等同 +1（轮转）
  v = (v + d + r.nameCount) % r.nameCount;
  g_settings.setString(r.key, std::to_string(v));
  st.dirty = 1;
}

// ── 关于 ────────────────────────────────────────────────────────────────
// 设备信息页（对应 crossmux 的 AboutActivity）：固件/版本/芯片/MAC/运行时间/内存/SD/电量。
// 行数固定，值在渲染时现取（SD 用量、电量这类要读硬件）。
static const int kAboutRowCount = 10;

struct AboutRow {
  const char *label;
  std::string value;
};

static void fillAboutRows(std::vector<AboutRow> &rows) {
  rows.clear();
  auto add = [&](const char *l, const std::string &v) { rows.push_back({l, v}); };
  char buf[128];

  add("固件", "Yan Reader");
  add("主题", "研读 | 研墨 | 研虑");
  add("版本", PJOURNAL_VERSION "  (" __DATE__ " " __TIME__ ")");

  esp_chip_info_t ci;
  esp_chip_info(&ci);
  snprintf(buf, sizeof(buf), "ESP32-S3 v%d.%d · %d 核", ci.revision / 100, ci.revision % 100, ci.cores);
  add("芯片", buf);

  uint8_t mac[6] = {0};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    add("WiFi MAC", buf);
  } else {
    add("WiFi MAC", "不可用");
  }

  long long up = static_cast<long long>(esp_timer_get_time() / 1000000);
  snprintf(buf, sizeof(buf), "%lld 时 %lld 分", up / 3600, (up / 60) % 60);
  add("运行时间", buf);

  add("内部 RAM 空闲", dictFormatSize(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  add("PSRAM 空闲", dictFormatSize(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

  struct statvfs sv;
  if (statvfs("/sdcard", &sv) == 0 && sv.f_blocks > 0) {
    uint64_t total = static_cast<uint64_t>(sv.f_blocks) * sv.f_frsize;
    uint64_t avail = static_cast<uint64_t>(sv.f_bavail) * sv.f_frsize;
    snprintf(buf, sizeof(buf), "%s / %s", dictFormatSize(total - avail).c_str(), dictFormatSize(total).c_str());
    add("SD 卡", buf);
  } else {
    add("SD 卡", "未挂载");
  }

  int pct = battery_pct();
  if (pct >= 0) {
    snprintf(buf, sizeof(buf), "%d%%", pct);
    add("电量", buf);
  } else {
    add("电量", "未知");
  }
}

static void renderAbout() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int itemH = uiLineHeight() + 8;
  int top = drawTitle("关于");

  std::vector<AboutRow> rows;
  fillAboutRows(rows);
  int n = static_cast<int>(rows.size());
  int viewH = statusTop() - top - 8;
  int maxRows = std::max(1, viewH / itemH);
  int start = clampI(st.aboutTop, 0, std::max(0, n - maxRows));

  for (int i = 0; i < maxRows && start + i < n; i++) {
    const AboutRow &r = rows[start + i];
    int y = top + i * itemH;
    drawLineText(MARGIN, y, r.label, true);
    int lw = g_rd.getTextWidth(uiFontId(), r.label);
    int avail = w - 2 * MARGIN - lw - 16;
    std::string v = (avail > 0) ? g_rd.truncatedText(uiFontId(), r.value.c_str(), avail) : std::string();
    if (!v.empty()) {
      int vw = g_rd.getTextWidth(uiFontId(), v.c_str());
      drawLineText(w - MARGIN - vw, y, v.c_str(), true);
    }
  }
  drawFooter("↑↓ 滚动  Esc 返回");
}

static void handleAbout(int key) {
  if (key == 0x1B) { st.mode = st.retMode; st.fullRefresh = true; st.dirty = 1; return; }
  int itemH = uiLineHeight() + 8;
  int maxRows = std::max(1, (statusTop() - coverTop() - 8) / itemH);
  int maxTop = std::max(0, kAboutRowCount - maxRows);
  if (key == KEY_UP) { st.aboutTop = std::max(0, st.aboutTop - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.aboutTop = std::min(maxTop, st.aboutTop + 1); st.dirty = 1; return; }
  if (key == KEY_PAGE_UP) { st.aboutTop = std::max(0, st.aboutTop - maxRows); st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.aboutTop = std::min(maxTop, st.aboutTop + maxRows); st.dirty = 1; return; }
}

// ── WiFi 传书 ───────────────────────────────────────────────────────────
// 复用写作模式的 web 文件管理器（main/file_manager_server.cpp）：手机/电脑浏览器
// 打开 http://<IP>/ 即可浏览 /sdcard，把 epub/txt 传进 /sdcard/books。
static const int kNetRows = 3;  // 启动 / 停止 / 二维码
static int netListTop() { return coverTop() + (uiLineHeight() + 12); }

static void netShareConnect() {
  st.netBusy = true;
  st.netStatus = "处理中...";
  st.fullRefresh = true;
  st.dirty = 1;
  renderCurrent();
  st.fullRefresh = false;

  // 连 WiFi（与文件浏览页浮动按钮共用 rdWifiEnsure，失败原因它已经写进 st.netStatus）。
  if (!rdWifiEnsure()) {
    st.netBusy = false;
    st.dirty = 1;
    return;
  }
  bool ok = file_manager_server_start(80);
  st.netServerUp = ok;
  st.netBusy = false;
  st.netStatus = ok ? ("服务已启动  " + g_wifi.getIp()) : "服务启动失败(端口被占用?)";
  if (ok && !g_wifi.getIp().empty()) rdShowFloat("网络文件管理已开启", "http://" + g_wifi.getIp() + "/", 8000);
  st.dirty = 1;
}

static void renderNetShare() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int itemH = uiLineHeight() + 12;
  int top = drawTitle("WiFi 传书");

  std::string ip = g_wifi.isConnected() ? g_wifi.getIp() : std::string();
  std::string status;
  if (st.netBusy) status = "处理中...";
  else if (!st.netStatus.empty()) status = st.netStatus;
  else if (g_wifi.isConnected()) status = ip.empty() ? "WiFi 已连接" : ("WiFi 已连接  " + ip);
  else status = "未连接 WiFi";
  drawLineText(MARGIN, top, fitWidth(status, w - 2 * MARGIN).c_str(), true);
  int ly = top + itemH;
  g_rd.drawLine(0, ly, w, ly, true);

  const char *rows[kNetRows] = {"启动服务（浏览器传书）", "停止服务", "显示地址二维码"};
  for (int i = 0; i < kNetRows; i++) {
    int ry = ly + i * itemH;
    if (i == st.netSel) {
      g_rd.fillRect(0, ry, w, itemH, true);
      drawLineText(MARGIN, ry + (itemH - uiLineHeight()) / 2, rows[i], false);
    } else {
      drawLineText(MARGIN, ry + (itemH - uiLineHeight()) / 2, rows[i], true);
    }
  }
  if (st.netServerUp && !ip.empty()) {
    std::string url = "http://" + ip + "/";
    drawCenteredLine(ly + kNetRows * itemH + 10, fitWidth(url, w - 2 * MARGIN).c_str(), true);
  }
  // 传输进度顶掉底栏提示：这个界面待着不动的时候，只有进度能说明"还在动"。
  // 传输结束/刚起服务的短提示走中间的浮动框（与文件浏览页一致），不占底栏。
  if (const std::string xf = rdNetXferText(); !xf.empty()) { drawFooter(xf.c_str()); return; }
  drawFooter("↑↓ 选择  Enter 执行  Esc 返回");
}

static void handleNetShare(int key) {
  if (st.netBusy) return;
  if (key == 0x1B) { st.mode = st.retMode; st.fullRefresh = true; st.dirty = 1; return; }
  if (key == KEY_UP) { st.netSel = std::max(0, st.netSel - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.netSel = std::min(kNetRows - 1, st.netSel + 1); st.dirty = 1; return; }
  if (key != '\n') return;

  int x, y;
  if (input_tap_xy(&x, &y)) {
    int itemH = uiLineHeight() + 12;
    int row = (y - netListTop()) / itemH;
    if (row >= 0 && row < kNetRows) st.netSel = row;
    st.dirty = 1;
    return;
  }

  if (st.netSel == 0) {
    netShareConnect();
  } else if (st.netSel == 1) {
    file_manager_server_stop();
    st.netServerUp = false;
    st.netStatus = "服务已停止";
    st.dirty = 1;
  } else {
    if (!g_wifi.isConnected()) {
      st.netStatus = "未连接 WiFi";
      st.dirty = 1;
      return;
    }
    st.qrText = "http://" + g_wifi.getIp() + "/";
    prepareQr();
    st.mode = RdMode::Qr;
    st.fullRefresh = true;
    st.dirty = 1;
  }
}

// ── 分发渲染 ────────────────────────────────────────────────────────────
// ── 方向切换 ─────────────────────────────────────────────────────────────
static void applyReaderOrientation() {
  if (st.orientation == "portrait") {
    board_force_portrait();
    g_rd.setOrientation(GfxRenderer::PortraitInverted);
  } else {
    board_force_landscape();
    g_rd.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  }
}

// ── 夜间反色 ─────────────────────────────────────────────────────────────
// 夜间模式现在是**全设备**开关：取反搬到了推屏唯一出口
// HalDisplay::displayBuffer()（见那里的注释），一次罩住所有界面。这里只负责把
// 全局设置同步到那个标志。**绝不能**再自己逐字节取反 fb —— 会和出口处的取反叠加，
// 两次相消等于没开。读 settings 而非 st.night：设置界面里改的键才是唯一真源。
static void applyNightMode() {
  display.setInverted(g_settings.nightMode());
}

// ── 书签 ────────────────────────────────────────────────────────────────
static std::string currentPageSummary() {
  char buf[32];
  if (st.bookKind == 1) {
    int lpp = linesPerPage();
    size_t li = static_cast<size_t>(st.txtPage) * lpp;
    if (li < st.txtLineStarts.size()) {
      size_t ls = st.txtLineStarts[li];
      size_t le = (li + 1 < st.txtLineStarts.size()) ? st.txtLineStarts[li + 1] : st.txtUtf8.size();
      if (le > ls + 48) le = ls + 48;
      std::string s = st.txtUtf8.substr(ls, le - ls);
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
      for (auto &ch : s) if (ch == '|') ch = ' ';
      return s;
    }
  }
  snprintf(buf, sizeof(buf), "%d/%d", curPage() + 1, totalPages());
  return buf;
}

static void saveBookmarks() {
  std::string s;
  for (auto &b : st.bookmarks) {
    s += std::to_string(b.kind) + "|" + std::to_string(b.spine) + "|" + std::to_string(b.offset) + "|" +
         std::to_string(b.page) + "|" + std::to_string(b.percent) + "|" + b.summary + "|" + b.path + "\n";
  }
  g_settings.setString("reader_bookmarks", s);
}

static void loadBookmarks() {
  st.bookmarks.clear();
  std::string s = g_settings.getString("reader_bookmarks");
  size_t pos = 0;
  while (pos < s.size()) {
    size_t nl = s.find('\n', pos);
    if (nl == std::string::npos) nl = s.size();
    std::string line = s.substr(pos, nl - pos);
    pos = nl + 1;
    if (line.empty()) continue;
    RdState::RdBookmark b;
    std::vector<std::string> parts;
    size_t p = 0;
    while (p <= line.size()) {
      size_t bar = line.find('|', p);
      if (bar == std::string::npos) { parts.push_back(line.substr(p)); break; }
      parts.push_back(line.substr(p, bar - p));
      p = bar + 1;
    }
    if (parts.size() < 7) continue;
    b.kind = atoi(parts[0].c_str());
    b.spine = atoi(parts[1].c_str());
    b.offset = static_cast<uint32_t>(strtoul(parts[2].c_str(), nullptr, 10));
    b.page = atoi(parts[3].c_str());
    b.percent = static_cast<float>(atof(parts[4].c_str()));
    b.summary = parts[5];
    b.path = parts[6];
    st.bookmarks.push_back(b);
  }
}

static void toggleBookmark() {
  if (st.bookPath.empty()) return;
  uint32_t off = (st.bookKind == 0) ? currentVisibleOffset() : 0;
  for (auto it = st.bookmarks.begin(); it != st.bookmarks.end(); ++it) {
    if (it->path == st.bookPath && it->kind == st.bookKind &&
        ((st.bookKind == 0) ? (it->spine == st.spineIndex && it->offset == off) : (it->page == curPage()))) {
      st.bookmarks.erase(it);
      saveBookmarks();
      return;
    }
  }
  RdState::RdBookmark b;
  b.path = st.bookPath;
  b.kind = st.bookKind;
  b.spine = st.spineIndex;
  b.offset = off;
  b.page = curPage();
  b.percent = totalPages() > 1 ? static_cast<float>(curPage()) / (totalPages() - 1) : 0.0f;
  b.summary = currentPageSummary();
  st.bookmarks.push_back(b);
  saveBookmarks();
}

// 把 epub 当前 spine 排到 target 页为止（跳转书签/脚注/百分比用）。
static void buildToPage(int target) {
  if (!st.section) return;
  while (static_cast<int>(st.section->pageCount) < target + 1 && !st.section->isBuildComplete()) {
    st.section->buildSomeMore(16);
    // 让出 CPU。单次 buildSomeMore 现在有界（Section 里按源字节封顶），但这个循环
    // 可能要跑很多轮；主任务是 core0 上唯一采样按键/触摸的地方，一直不让出就等于把
    // 输入通道掐了（看门狗也不会复位：阻塞/不让出的是 idle 之外的活儿）。
    vTaskDelay(1);
  }
  st.page = clampI(target, 0, static_cast<int>(st.section->pageCount) - 1);
}

// ── 阅读位置记忆 ────────────────────────────────────────────────────────
// 存 <SD>/reader_progress.txt，一行一本书，字段用 \x1f 分隔 —— 与笔记同一套文件格式
// （见 kNotesPath），路径里混进分隔符时替换成空格。
//
// 为什么要单独落盘：st(screen_reader.cpp)是纯 RAM 的，退出阅读模式时书对象就释放了；
// s_return 只覆盖"同一次开机内切走再切回来"。掉电/重启位置就丢了，必须写文件。
// 也不能塞 g_settings(NVS)：为每次翻页写 NVS 磨损太快，SD 上写一行几百字节正合适。
static const char *kProgressPath = "/sdcard/reader_progress.txt";
// 最多记这么多本；超出丢最久没读的。够用且把文件大小封在上限（约十几 KB）。
static const int kProgressMax = 200;

struct RdProgress {
  std::string path;
  int kind = 0;
  int spine = 0;
  int page = 0;
  int txtPage = 0;
  int xtcPage = 0;
};

static std::vector<RdProgress> s_progress;  // 最新在前
static bool s_progressLoaded = false;

static void loadProgress() {
  if (s_progressLoaded) return;
  s_progressLoaded = true;
  s_progress.clear();
  HalFile f;
  if (!Storage.openFileForRead(TAG, kProgressPath, f)) return;
  std::string all;
  all.resize(f.size());
  if (!all.empty()) {
    const int got = f.read(&all[0], all.size());
    all.resize(got > 0 ? got : 0);
  }
  f.close();
  size_t pos = 0;
  while (pos < all.size()) {
    size_t nl = all.find('\n', pos);
    if (nl == std::string::npos) nl = all.size();
    std::string line = all.substr(pos, nl - pos);
    pos = nl + 1;
    if (line.empty()) continue;
    std::vector<std::string> parts;
    size_t p = 0;
    while (p <= line.size()) {
      size_t sp = line.find(RD_NOTE_SEP, p);
      if (sp == std::string::npos) { parts.push_back(line.substr(p)); break; }
      parts.push_back(line.substr(p, sp - p));
      p = sp + 1;
    }
    if (parts.size() < 6) continue;
    RdProgress r;
    r.path = rdNoteUnescape(parts[0]);
    r.kind = atoi(parts[1].c_str());
    r.spine = atoi(parts[2].c_str());
    r.page = atoi(parts[3].c_str());
    r.txtPage = atoi(parts[4].c_str());
    r.xtcPage = atoi(parts[5].c_str());
    if (s_progress.size() < static_cast<size_t>(kProgressMax)) s_progress.push_back(std::move(r));
  }
}

static void saveProgress() {
  HalFile f;
  if (!Storage.openFileForWrite(TAG, kProgressPath, f)) return;
  for (const auto &r : s_progress) {
    std::string line = rdNoteEscape(r.path) + RD_NOTE_SEP + std::to_string(r.kind) + RD_NOTE_SEP +
                       std::to_string(r.spine) + RD_NOTE_SEP + std::to_string(r.page) + RD_NOTE_SEP +
                       std::to_string(r.txtPage) + RD_NOTE_SEP + std::to_string(r.xtcPage) + "\n";
    f.write(reinterpret_cast<const uint8_t *>(line.data()), line.size());
  }
  f.close();
}

// ── 待机表盘「书籍封面」的出口（声明见 screen_reader.h）────────────────────
bool readerLastBookCover(std::string &coverBmp, std::string &title, int &percent) {
  loadProgress();
  if (s_progress.empty()) return false;
  const RdProgress &r = s_progress.front();   // "最新在前"：表头就是最后读的那本
  // 优先给待机专用的整屏封面（原图按待机框解的，1:1 上屏）。没有才退回书架那张
  // 396×528 的缩略封面：老书（这份缓存是后加的，之前打开过的书没生成）、刚打开就
  // 待机（生成还没跑到）、以及 XTC（它的封面本来就是原分辨率，没必要另存一份）。
  const std::string sb = standbyCoverPathFor(r.path, r.kind);
  coverBmp = Storage.exists(sb.c_str()) ? sb : coverBmpPathFor(r.path, r.kind);
  title.clear();
  percent = 0;
  if (const ReadingBookStats *b = ReadingStats::findBook(r.path)) {
    title = b->title;
    percent = b->lastProgressPercent;
  }
  if (title.empty()) {
    // 统计里还没有这本（刚打开就待机 / 统计未落盘）：退回文件名，去掉目录与扩展名。
    const size_t sl = r.path.find_last_of('/');
    std::string base = (sl == std::string::npos) ? r.path : r.path.substr(sl + 1);
    const size_t dot = base.rfind('.');
    if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
    title = base;
  }
  return true;
}

bool readerCoverScale(const std::string &bmpPath, int boxW, int boxH, uint8_t *out, size_t outCap,
                      int &dw, int &dh, int &ox, int &oy) {
  dw = dh = ox = oy = 0;
  if (bmpPath.empty() || boxW <= 0 || boxH <= 0 || out == nullptr) return false;
  if (!Storage.exists(bmpPath.c_str())) return false;
  HalFile f;
  if (!Storage.openFileForRead(TAG, bmpPath, f)) return false;
  Bitmap bmp(f);
  if (bmp.parseHeaders() != BmpReaderError::Ok || bmp.getWidth() <= 0 || bmp.getHeight() <= 0) return false;
  const int sw = bmp.getWidth(), sh = bmp.getHeight();
  float scale = std::min(static_cast<float>(boxW) / sw, static_cast<float>(boxH) / sh);
  // **只缩不放**：源图比框小就按原尺寸画（下面 ox/oy 照样把它居中）。
  // 放大出来的像素要么是复制的、要么是插值的，细节全是编的 —— 待机封面表盘要的是
  // "最清晰的那一版"，留白居中比糊掉强。（rdBuildCoverThumb 在放大时是按最近源像素
  // 取样再上锐化，方块会被锐化放大，尤其难看。）
  if (scale > 1.0f) scale = 1.0f;
  int w = static_cast<int>(sw * scale), h = static_cast<int>(sh * scale);
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (static_cast<size_t>(w) * h > outCap) return false;
  // 1:1（源图正好是这个尺寸）→ 逐行直拷，**不走 rdBuildCoverThumb**。
  // 待机那张缓存就是按这个框解出来的原图，再经一遍 3×3 盒式平均 + 非锐化纯属白糊：
  // 面积比 = 1 时 kSharpen 仍然会加 2/8 的拉普拉斯，等于把刚解出来的像素又抹一遍。
  // 这里只做"提对比 + 量化到 0..15"，和缩放路径的最后一步同一个口径。
  // / 1:1 source → straight row copy. Running it through rdBuildCoverThumb would apply
  // a 3×3 box blur and a 2/8 Laplacian to pixels that were just decoded at this exact
  // size. Contrast + quantization only, same as the tail of the scaling path.
  if (w == sw && h == sh) {
    const int rowBytes = bmp.getRowBytes();
    std::vector<uint8_t> rowBuf(rowBytes), data(sw), opacity(sw);
    for (int y = 0; y < sh; y++) {
      if (bmp.readNextRow(data.data(), rowBuf.data(), opacity.data(), Bitmap::RowOutput::Gray8) != BmpReaderError::Ok)
        return false;
      uint8_t *dst = out + static_cast<size_t>(y) * w;
      for (int x = 0; x < sw; x++) {
        if (opacity[x] == 0) { dst[x] = 15; continue; }   // 透明 = 纸白
        int g = (rdCoverContrast(data[x]) + 8) >> 4;
        dst[x] = static_cast<uint8_t>(g > 15 ? 15 : g);
      }
    }
  } else if (!rdBuildCoverThumb(bmp, w, h, out)) {
    return false;
  }
  dw = w;
  dh = h;
  ox = (boxW - w) / 2;
  oy = (boxH - h) / 2;
  return true;
}

// 把"现在读到哪"记进表。位置没变就整个返回，不碰 SD——所以翻目录、重绘、按键空转
// 这些高频调用都是零成本；真正翻页时才写一次（几百字节到几 KB，相对一次 e-ink 刷新的
// 几百毫秒可以忽略）。force=true 用于退出/换书：那之后书就释放了，没机会再记。
static void rdRememberProgress(bool force) {
  if (st.bookPath.empty() || st.bookKind < 0) return;

  RdProgress cur;
  cur.path = st.bookPath;
  cur.kind = st.bookKind;
  cur.spine = st.spineIndex;
  cur.page = st.page;
  cur.txtPage = st.txtPage;
  cur.xtcPage = st.xtcPage;

  loadProgress();
  int idx = -1;
  for (size_t i = 0; i < s_progress.size(); i++) {
    if (s_progress[i].path == cur.path && s_progress[i].kind == cur.kind) { idx = static_cast<int>(i); break; }
  }
  if (idx >= 0) {
    const RdProgress &old = s_progress[idx];
    const bool same = old.spine == cur.spine && old.page == cur.page && old.txtPage == cur.txtPage &&
                      old.xtcPage == cur.xtcPage;
    if (same && !force) return;
    s_progress.erase(s_progress.begin() + idx);
  }
  // 最新在前；满了就从尾部丢最久没读的。
  while (s_progress.size() >= static_cast<size_t>(kProgressMax)) s_progress.pop_back();
  s_progress.insert(s_progress.begin(), std::move(cur));
  saveProgress();
}

// 打开书之后调用：这本书上次读到哪就跳到哪。没有记录（第一次读）保持原样——
// openEpub/openTxt/openXtc 给的首屏。
static void rdRestoreProgress() {
  loadProgress();
  if (st.bookPath.empty()) return;
  for (const auto &r : s_progress) {
    if (r.path != st.bookPath || r.kind != st.bookKind) continue;
    if (st.bookKind == 0 && st.epub) {
      const int spines = st.epub->getSpineItemsCount();
      const int sp = clampI(r.spine, 0, std::max(0, spines - 1));
      if (sp != st.spineIndex) openSpine(sp);
      buildToPage(r.page);
    } else if (st.bookKind == 1 && !st.txtLineStarts.empty()) {
      st.txtPage = clampI(r.txtPage, 0, std::max(0, totalPages() - 1));
    } else if (st.bookKind == 2 && st.xtc) {
      st.xtcPage = clampI(r.xtcPage, 0, std::max(0, static_cast<int>(st.xtc->getPageCount()) - 1));
    }
    return;
  }
}

static void gotoBookmark(int idx) {
  if (idx < 0 || idx >= static_cast<int>(st.bookmarks.size())) return;
  auto &b = st.bookmarks[idx];
  if (b.path != st.bookPath) {
    if (!openBook(b.path, b.kind)) return;
  }
  if (st.bookKind == 0) { if (b.spine != st.spineIndex) openSpine(b.spine); buildToPage(b.page); }
  else if (st.bookKind == 1) st.txtPage = clampI(b.page, 0, std::max(0, totalPages() - 1));
  else if (st.bookKind == 2) st.xtcPage = clampI(b.page, 0, st.xtc ? static_cast<int>(st.xtc->getPageCount()) - 1 : 0);
  st.mode = RdMode::Reading;
  st.fullRefresh = true;
  st.dirty = 1;
}

// ── 脚注 ────────────────────────────────────────────────────────────────
static bool loadCurrentFootnotes() {
  st.footnoteNums.clear();
  st.footnoteHrefs.clear();
  if (st.bookKind != 0 || !st.section) return false;
  auto page = st.section->loadPage(st.page);
  if (!page) return false;
  for (const auto &fn : page->footnotes) {
    st.footnoteNums.push_back(fn.number[0] ? fn.number : "[链接]");
    st.footnoteHrefs.push_back(fn.href);
  }
  if (st.footnoteHrefs.empty()) {
    // 临时诊断：页里明明有上标号，脚注表却是空的。两种可能——(a) 解析时注号被记到了
    // 邻页（pendingFootnotes 的页归属差一行）；(b) 这一页是旧版解析器落下的页缓存。
    // 打页码+章节号，配合 `点注:` 那行就能分辨。
    ESP_LOGW(TAG, "点注诊断: 第%d页(spine %d)脚注表为空，本页词 %u 个",
             st.page, st.spineIndex, (unsigned)(g_pageText.valid ? g_pageText.words.size() : 0));
  }
  return !st.footnoteHrefs.empty();
}

// 锚点 → 页码，查不到就把本节多排一会儿再查。
// 为什么需要：本节是**惰性排版**的（空闲帧只领先读者 kPrebuildAhead 页），而注释正文
// 常常压在**本节末尾**——晋书的校勘记就是正文后一整块 `<p id="note-001">`，最后一处
// 正文引用在源文件的 83% 处。读者停在第 4 页点注号时，本节只排到第 9 页，锚点那一页
// 压根没排出来 → 锚点表里没有 → 原来 getPageForAnchor 直接返回空，弹注/跳转全落空
// （日志 `注号对上了但取不到注文`，然后按键落进左右 1/3 翻页，这正是用户看到的"点注
// 变成翻页"）。
// 这里先查一次（活构建 + 磁盘锚点表），没查到且本节还在排就有界地继续排，边排边查。
// 预算按**时间**封顶而不是按页数：一次翻页的排版量级是几十毫秒，这个预算足够覆盖
// "注释就在后面几页"的常见情形；真遇到超长章节也只是退化回原来的行为，不会把按键
// 处理卡到没法用。
static std::optional<Section::AnchorPos> rdFindFootnotePage(const std::string &anchor) {
  if (st.bookKind != 0 || !st.section || anchor.empty()) return std::nullopt;
  auto pos = st.section->findAnchorPos(anchor);  // 活构建优先，其次磁盘锚点表
  if (pos) return pos;
  if (!st.section->isBuilding() || st.section->isBuildComplete()) return std::nullopt;
  constexpr int64_t kBudgetUs = 2500 * 1000;
  const int64_t deadline = esp_timer_get_time() + kBudgetUs;
  while (esp_timer_get_time() < deadline) {
    if (!st.section->buildSomeMore(4)) break;
    pos = st.section->findAnchorPos(anchor);
    if (pos) return pos;
    if (st.section->isBuildComplete()) break;
  }
  return pos;
}

// 注号归一化区的两个工具函数（定义在本文件后半段，这一段先用）。
static int rdUtf8Len(unsigned char c);
static std::string rdNormalizeNoteNumber(const std::string &s);
static void rdRememberNoteRef(const std::string &num, int spine, int page);  // 定义在 footnoteReturn 之后

static void jumpToFootnote(int idx) {
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

static void footnoteReturn() {
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

static void rdRememberNoteRef(const std::string &num, int spine, int page) {
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
static bool rdGotoNoteRef(const std::string &num) {
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
static void rdLeadingNoteMarker(const std::string &text, std::string *out) {
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
static bool rdStartsBracketed(const std::string &text) {
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
static bool rdHeadBracketClosed(const std::string &text) {
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
static std::string rdNoteText(Section &sec, int pageIdx, int elementIdx, int fontId,
                              const std::string &noteNum) {
  constexpr int kMaxExtraPages = 3;  // 锚点页之外最多再读 3 页
  constexpr int kMaxLines = 240;     // 总行数上限，防跑飞

  // 先定位"注文从哪一页、哪一行开始"。锚点序号指的是**锚点页**；行内锚点会被解析器记到
  // 后一段（见 rdFindNoteStartLine 的说明），极端情况下本段正排在页尾、整段被推到下一页，
  // 那锚点页上就一行都对不上、而注文留在**上一页**。所以只有"锚点行在、号码却对不上"
  // 这一种确凿的错位才退一页再找；那一趟不带锚点序号（它属于下一页），只按注号唯一命中。
  int startPage = pageIdx, startCur = -1;
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
    } else if (!late) {
      break;  // 锚点页上不是"错位"（只是认不出来）：退一页也找不着
    }
  }
  if (startCur < 0) {  // 认不出来：退回锚点页整页（老行为），总比什么都不给强
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
    if (!page) break;
    std::vector<std::string> lines;
    std::vector<int> lineStart;
    std::vector<uint8_t> lineParaStart;
    rdSplitPageLines(*page, fontId, &lines, &lineStart, &lineParaStart);
    if (lines.empty()) break;

    int cur = 0;
    if (needStart) {
      cur = startCur;
      if (cur >= static_cast<int>(lines.size())) break;
      if (lines.size() == 1) return lines[cur];  // 整页就这一行：没有"下一行"可判终止
      out = lines[cur];
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

// 取出第 idx 条脚注的正文并开弹注。返回 false = 这条注释不在本章里（calibre 常见的
// notes.xhtml#fn1 那种跨文件的注释就是这种），调用方退回"直接跳转"那条老路。
// 判据现成：锚点表只覆盖当前这一章，跨章/跨文件的 href 在这儿查不到。
static bool openFootnotePopup(int idx) {
  if (idx < 0 || idx >= static_cast<int>(st.footnoteHrefs.size())) return false;
  if (st.bookKind != 0 || !st.section) return false;
  const std::string &href = st.footnoteHrefs[idx];

  // QQ 阅读器的弹注**没有正文段落**，注释文字就藏在 <img alt="…"> 里。解析器把它
  // 以 "alt:" 哨兵前缀塞进 href（见 ChapterHtmlSlimParser 的 IMAGE_TAGS 分支），
  // 这里直接取用 —— 不用去锚点表里找。
  if (href.rfind("alt:", 0) == 0) {
    if (href.size() <= 4) return false;
    st.fnPopNum = st.footnoteNums[idx];
    st.fnPopText = href.substr(4);
    st.fnPopScroll = 0;
    st.fnPopIdx = idx;
    return true;
  }

  const size_t hash = href.rfind('#');
  if (hash == std::string::npos) return false;
  const std::string anchor = href.substr(hash + 1);
  if (anchor.empty()) return false;

  std::string text;
  auto pg = rdFindFootnotePage(anchor);
  if (pg)
    text = rdNoteText(*st.section, pg->page, pg->element, BODY_FONT_ID_BASE + st.fontLevel,
                      st.footnoteNums[idx]);

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
  if (text.empty()) return false;

  st.fnPopNum = st.footnoteNums[idx];
  st.fnPopText = std::move(text);
  st.fnPopScroll = 0;
  st.fnPopIdx = idx;
  return true;
}

// 弹注浮层。画在正文页之上、busy/瞬时浮层之下（后两个都在正中间，优先级更高），
// 且在 applyNightMode() 之前 —— 夜间模式连它一起反色，不会留一块刺眼的白。
// 白底 + 双线边框 + 现行宽现断行，非交互部分全在这里，滚动窗口由 fnPopScroll 定。
static void drawFootnotePopup() {
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

// 关掉弹注。改回正文页需要整屏重绘（弹窗底下那页要恢复原样），所以置 fullRefresh。
static void closeFootnotePopup() {
  st.fnPopNum.clear();
  st.fnPopText.clear();
  st.fnPopScroll = 0;
  st.fnPopIdx = -1;
  st.fullRefresh = true;
  st.dirty = 1;
}

// ── 百分比跳转 ───────────────────────────────────────────────────────────
static void jumpToPercent(int pct) {
  pct = clampI(pct, 0, 100);
  if (st.bookKind == 1) {
    int tp = totalPages();
    st.txtPage = tp > 1 ? static_cast<int>(static_cast<long long>(tp - 1) * pct / 100) : 0;
  } else if (st.bookKind == 2) {
    int tp = st.xtc ? static_cast<int>(st.xtc->getPageCount()) : 1;
    st.xtcPage = tp > 1 ? static_cast<int>(static_cast<long long>(tp - 1) * pct / 100) : 0;
  } else if (st.bookKind == 0 && st.epub) {
    size_t bookSize = st.epub->getBookSize();
    if (bookSize == 0) return;
    size_t target = (pct >= 100) ? bookSize - 1 : static_cast<size_t>(static_cast<double>(bookSize) * pct / 100);
    int spineCount = st.epub->getSpineItemsCount();
    int targetSpine = spineCount - 1;
    size_t prev = 0;
    for (int i = 0; i < spineCount; i++) {
      size_t cum = st.epub->getCumulativeSpineItemSize(i);
      if (target <= cum) { targetSpine = i; prev = (i > 0) ? st.epub->getCumulativeSpineItemSize(i - 1) : 0; break; }
    }
    size_t cum = st.epub->getCumulativeSpineItemSize(targetSpine);
    float frac = (cum > prev) ? static_cast<float>(target - prev) / static_cast<float>(cum - prev) : 0.0f;
    if (targetSpine != st.spineIndex) openSpine(targetSpine);
    int ep = st.section ? static_cast<int>(st.section->estimatedTotalPages()) : 1;
    buildToPage(static_cast<int>(frac * ep));
  }
  st.mode = RdMode::Reading;
  st.fullRefresh = true;
  st.dirty = 1;
}

// ── 二维码 ───────────────────────────────────────────────────────────────
static void prepareQr() {
  st.qrText.clear();
  if (st.bookKind == 0 && st.section) st.qrText = st.section->getTextFromSectionFile();
  else if (st.bookKind == 1) st.qrText = st.txtUtf8.substr(0, 4000);
  else st.qrText = st.bookTitle;
  // 截断到 QR 字节模式容量内（v40 L 约 2953 字节），按 UTF-8 边界切。
  const size_t MAXQ = 2500;
  if (st.qrText.size() > MAXQ) {
    size_t cut = MAXQ;
    while (cut > 0 && (static_cast<unsigned char>(st.qrText[cut]) & 0xC0) == 0x80) cut--;
    st.qrText.resize(cut);
  }
}

static void renderQr() {
  g_rd.clearScreen();
  drawTitle("二维码");
  int w = g_rd.getScreenWidth();
  int top = coverTop();
  int availH = statusTop() - top - 12;
  int box = std::min(w - 2 * MARGIN, availH);
  if (box <= 0) { drawFooter("任意键返回"); return; }
  const int cap = qrcodegen_BUFFER_LEN_FOR_VERSION(40);
  std::vector<uint8_t> temp(cap), qr(cap);
  if (!qrcodegen_encodeText(st.qrText.c_str(), temp.data(), qr.data(), qrcodegen_Ecc_LOW, 4, 40, qrcodegen_Mask_AUTO, true)) {
    drawCenteredLine(g_rd.getScreenHeight() / 2, "文本过长,二维码生成失败");
    drawFooter("任意键返回");
    return;
  }
  int size = qrcodegen_getSize(qr.data());
  int px = std::max(1, box / size);
  int dim = size * px;
  int x0 = (w - dim) / 2;
  int y0 = top + (availH - dim) / 2;
  for (int cy = 0; cy < size; cy++)
    for (int cx = 0; cx < size; cx++)
      if (qrcodegen_getModule(qr.data(), cx, cy))
        g_rd.fillRect(x0 + px * cx, y0 + px * cy, px, px, true);
  drawFooter("任意键返回");
}

// ── 书签/脚注/百分比/二维码 界面 ─────────────────────────────────────────
static void renderBookmarks() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int top = drawTitle("书签");
  if (st.bookmarks.empty()) {
    drawCenteredLine(g_rd.getScreenHeight() / 2, "暂无书签");
    drawFooter("Esc 返回");
    return;
  }
  int itemH = uiLineHeight() + 8;
  int viewH = statusTop() - top - 8;
  int maxRows = std::max(1, viewH / itemH);
  int start = clampI(st.bookmarkSel - maxRows / 2, 0, std::max(0, static_cast<int>(st.bookmarks.size()) - maxRows));
  for (int i = 0; i < maxRows && start + i < static_cast<int>(st.bookmarks.size()); i++) {
    int idx = start + i;
    auto &b = st.bookmarks[idx];
    char line[160];
    snprintf(line, sizeof(line), "%d%%  %s", static_cast<int>(b.percent * 100), b.summary.c_str());
    int y = top + i * itemH;
    if (idx == st.bookmarkSel) { g_rd.fillRect(0, y, w, itemH, true); drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, line, false); }
    else drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, line, true);
  }
  drawFooter("↑↓ 选择  Enter 跳转  Esc 返回");
}

static void handleBookmarks(int key) {
  int n = static_cast<int>(st.bookmarks.size());
  if (key == 0x1B) { st.mode = RdMode::Menu; st.fullRefresh = true; st.dirty = 1; return; }
  if (n == 0) return;
  if (key == KEY_UP) { st.bookmarkSel = std::max(0, st.bookmarkSel - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.bookmarkSel = std::min(n - 1, st.bookmarkSel + 1); st.dirty = 1; return; }
  if (key == KEY_PAGE_UP) { st.bookmarkSel = std::max(0, st.bookmarkSel - 8); st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.bookmarkSel = std::min(n - 1, st.bookmarkSel + 8); st.dirty = 1; return; }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      int top = coverTop();
      int itemH = uiLineHeight() + 8;
      int viewH = statusTop() - top - 8;
      int maxRows = std::max(1, viewH / itemH);
      int start = clampI(st.bookmarkSel - maxRows / 2, 0, std::max(0, n - maxRows));
      int row = start + (y - top) / itemH;
      if (row >= 0 && row < n) st.bookmarkSel = row;
    }
    gotoBookmark(st.bookmarkSel);
    st.dirty = 1;
    return;
  }
}

static void renderFootnotes() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int top = drawTitle("脚注");
  if (st.footnoteNums.empty()) {
    drawCenteredLine(g_rd.getScreenHeight() / 2, "本页无脚注");
    drawFooter("Esc 返回");
    return;
  }
  int itemH = uiLineHeight() + 8;
  int viewH = statusTop() - top - 8;
  int maxRows = std::max(1, viewH / itemH);
  int start = clampI(st.footnoteSel - maxRows / 2, 0, std::max(0, static_cast<int>(st.footnoteNums.size()) - maxRows));
  for (int i = 0; i < maxRows && start + i < static_cast<int>(st.footnoteNums.size()); i++) {
    int idx = start + i;
    int y = top + i * itemH;
    if (idx == st.footnoteSel) { g_rd.fillRect(0, y, w, itemH, true); drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, st.footnoteNums[idx].c_str(), false); }
    else drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, st.footnoteNums[idx].c_str(), true);
  }
  drawFooter("↑↓ 选择  Enter 弹注  → 跳转  Esc 返回");
}

static void handleFootnotes(int key) {
  int n = static_cast<int>(st.footnoteNums.size());
  if (key == 0x1B) { st.mode = RdMode::Menu; st.fullRefresh = true; st.dirty = 1; return; }
  if (n == 0) return;
  if (key == KEY_UP) { st.footnoteSel = std::max(0, st.footnoteSel - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.footnoteSel = std::min(n - 1, st.footnoteSel + 1); st.dirty = 1; return; }
  if (key == KEY_PAGE_UP) { st.footnoteSel = std::max(0, st.footnoteSel - 8); st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.footnoteSel = std::min(n - 1, st.footnoteSel + 8); st.dirty = 1; return; }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      int top = coverTop();
      int itemH = uiLineHeight() + 8;
      int viewH = statusTop() - top - 8;
      int maxRows = std::max(1, viewH / itemH);
      int start = clampI(st.footnoteSel - maxRows / 2, 0, std::max(0, n - maxRows));
      int row = start + (y - top) / itemH;
      if (row >= 0 && row < n) st.footnoteSel = row;
    }
    // 默认真弹注：在书里就地看一眼注释，不跳走（跳走再回来得走"返回脚注跳转前"）。
    // 注释不在本章（跨文件的 notes.xhtml）时弹不出来，退回老行为直接跳。
    const int sel = st.footnoteSel;
    if (openFootnotePopup(sel)) {
      st.mode = RdMode::Reading;  // 弹注是盖在**正文页**上的浮层，先回正文
      st.fullRefresh = true;
    } else {
      jumpToFootnote(sel);
    }
    st.dirty = 1;
    return;
  }
  // → 保留老行为：直接跳到注释原文页（想连读注释、不弹窗的走这条）。
  if (key == KEY_RIGHT) {
    jumpToFootnote(st.footnoteSel);
    st.dirty = 1;
    return;
  }
}

static void renderPercent() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int h = g_rd.getScreenHeight();
  drawTitle("跳转百分比");
  int barX = MARGIN, barW = w - 2 * MARGIN;
  int barY = h / 2 - 40, barH = 30;
  g_rd.drawRect(barX, barY, barW, barH, true);
  g_rd.fillRect(barX + 1, barY + 1, (barW - 2) * st.percentVal / 100, barH - 2, true);
  char buf[64];
  snprintf(buf, sizeof(buf), "%d%%", st.percentVal);
  drawCenteredLine(barY + barH + 20, buf, true);
  drawFooter("← → 调整  Enter 跳转  Esc 返回");
}

static void handlePercent(int key) {
  if (key == 0x1B) { st.mode = RdMode::Menu; st.fullRefresh = true; st.dirty = 1; return; }
  if (key == KEY_LEFT || key == KEY_UP) { st.percentVal = std::max(0, st.percentVal - 5); st.dirty = 1; return; }
  if (key == KEY_RIGHT || key == KEY_DOWN) { st.percentVal = std::min(100, st.percentVal + 5); st.dirty = 1; return; }
  if (key == KEY_PAGE_UP) { st.percentVal = std::max(0, st.percentVal - 20); st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.percentVal = std::min(100, st.percentVal + 20); st.dirty = 1; return; }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      int barX = MARGIN, barW = g_rd.getScreenWidth() - 2 * MARGIN;
      int barY = g_rd.getScreenHeight() / 2 - 40, barH = 30;
      if (y >= barY && y < barY + barH && barW > 0)
        st.percentVal = clampI((x - barX) * 100 / barW, 0, 100);
    }
    jumpToPercent(st.percentVal);
    st.dirty = 1;
    return;
  }
}

static void handleQr(int key) {
  (void)key;
  st.mode = RdMode::Menu; st.fullRefresh = true; st.dirty = 1;
}

// 阅读器此刻是不是"能打字的界面"：WiFi/OPDS 地址输入、笔记编辑、词典查询。只有这
// 几个界面才谈得上"打字刷新"；翻页/书架/菜单用极速刷只是白白牺牲画质。
static bool rdTypingScreen() {
  if (st.mode == RdMode::NoteEdit || st.mode == RdMode::Dictionary) return true;
  if (st.mode == RdMode::ShelfSearch || st.mode == RdMode::NotesSearch) return true;
  if (st.mode == RdMode::Wifi && st.wifiEditing) return true;
  if (st.mode == RdMode::Opds && st.opdsEditing) return true;
  if (st.mode == RdMode::ResDl && st.resEditing) return true;
  return false;
}

// 阅读模式里挂的是同一个系统级输入法，只是以前从没把它打开过——于是虚拟键盘敲拼音
// 一个字都上不了屏（handleKey 在 !active 时直接返回 false）。这里按"是不是打字界面"
// 开关它：进入笔记/词典/地址输入就开中文，离开就关。
// 只在状态**变化**时切：每次按键都 setActive() 会把正在输入的编码 reset 掉。离开时
// 的 setActive(false) 顺带把用户词典刷盘（IME::setActive 内部会 flushUserDictSaves(true)）。
static void rdSyncImeActive() {
  auto &ime = IME::getInstance();
  const bool want = rdTypingScreen();
  if (want == ime.active()) return;   // 已经是想要的状态，别 setActive(会把编码 reset)
  ime.setActive(want);
}

// ── 翻页刷新策略（设置 → 刷新策略）──────────────────────────────────────
// 0=全局 每次 GC16 全刷：零残影，但最慢也最闪；1=局刷 GL16 差分（默认）；
// 2=快刷 DU 极速：最快、残影最重，靠 display 侧每 APP_GC16_EVERY 次自动升一次
// GC16 兜底；3=自适应：抽样比对前后两帧，按变化量自动选波形（见下面
// rdFrameChangePermille）。只作用于**阅读页翻页**；菜单/列表/WiFi 等界面仍按原规则
// （进入首帧全刷、实体键盘打字走极速），免得为了翻页手感把设置界面一起拖慢。
static const char *kRdRefreshKeys[] = {"full", "partial", "fast", "auto"};
static const char *kRdRefreshNames[] = {"全局", "局刷", "快刷", "自适应"};
static const int kRdRefreshCount = 4;
static int refreshStrategy() {
  std::string k = g_settings.getString("reader_refresh", "partial");
  for (int i = 0; i < kRdRefreshCount; i++) {
    if (k == kRdRefreshKeys[i]) return i;
  }
  return 1;  // 局刷
}

// ── 全刷频率（设置 → 全刷频率）──────────────────────────────────────────
// 每翻多少页强制一次全刷，0 表示"跟随策略"（由刷新策略自己决定，即现在的行为：
// 局刷/快刷不主动全刷，自适应按残影预算攒够了才清）。选 5/10/15/20 就是不论策略
// 每翻够这么多页清一次。**策略选"全局"时这一项无效**——本来就每页都是全刷。
static const int kRdFullEveryValues[] = {0, 5, 10, 15, 20};
static const char *kRdFullEveryKeys[] = {"0", "5", "10", "15", "20"};
static const char *kRdFullEveryNames[] = {"跟随策略", "5 页", "10 页", "15 页", "20 页"};
static const int kRdFullEveryCount = 5;
static int fullRefreshEvery() {
  std::string k = g_settings.getString("reader_full_every", "0");
  for (int i = 0; i < kRdFullEveryCount; i++) {
    if (k == kRdFullEveryKeys[i]) return kRdFullEveryValues[i];
  }
  return 0;  // 跟随策略
}
// 自上次全刷以来推过的阅读页数（只数阅读页翻页，菜单/列表不算）。
static int s_pagesSinceFull = 0;

// ── 换章检测（强制全刷，与策略无关）─────────────────────────────────────
// EPUB 每个 spine 项就是一个章节文件，所以 spine 变了就是换章。换章时整屏内容全换、
// 上一章的残影最脏，这里记下上一次推屏时的章节，翻页/跳目录/跳脚注/跳书签只要跨了章，
// 都走一次 GC16 —— 不管用户选的是局刷还是快刷。
static std::string s_chapterKey;
static bool s_chapterKnown = false;

// ── 自适应刷新的"变化量"测量 ────────────────────────────────────────────
// 抽样比对上一帧推屏时的 framebuffer：2048 个样本、步长 203（质数，与一行 608 字节
// 互质，样本不会规律地落进同一列），覆盖整屏且无偏。2KB 静态数组，比整帧快照(406KB)
// 便宜得多，统计误差 ±1~2‰，足够区分"文字翻页/图片页/几乎没变"三档。
#define RD_SAMPLE_COUNT 2048
#define RD_SAMPLE_STRIDE 203
static uint8_t s_prevSample[RD_SAMPLE_COUNT];
static bool s_prevSampleValid = false;
// 自上次 GC16 以来攒下的"残影预算"（累加每次的变化千分比）。
static int s_ghostAccum = 0;

// 与上一帧抽样比对，返回变化千分比（0..1000），顺手把当前帧存为新基准。
static int rdFrameChangePermille() {
  const uint8_t *fb = g_rd.getFrameBuffer();
  if (fb == nullptr) return 0;
  const uint32_t size = g_rd.getBufferSize();
  int changed = 0;
  for (int i = 0; i < RD_SAMPLE_COUNT; i++) {
    const uint32_t off = static_cast<uint32_t>(i) * RD_SAMPLE_STRIDE;
    if (off >= size) break;
    const uint8_t v = fb[off];
    if (s_prevSampleValid && v != s_prevSample[i]) changed++;
    s_prevSample[i] = v;
  }
  const bool hadBaseline = s_prevSampleValid;
  s_prevSampleValid = true;
  return hadBaseline ? changed * 1000 / RD_SAMPLE_COUNT : 1000;
}

static void renderCurrent() {
  // 翻页手感排查用的分段计时：绘制（含栅格化） / 决策+测量 / 刷屏 三段。
  // 只在阅读页打，菜单/列表页刷屏快慢与翻页无关，不值得占日志。
  const int64_t s_t0 = esp_timer_get_time();
  switch (st.mode) {
    case RdMode::Browser: renderBrowser(); break;
    case RdMode::Reading: renderReading(); break;
    case RdMode::Toc: renderToc(); break;
    case RdMode::Menu: renderMenu(); break;
    case RdMode::Bookmarks: renderBookmarks(); break;
    case RdMode::Footnotes: renderFootnotes(); break;
    case RdMode::Percent: renderPercent(); break;
    case RdMode::Qr: renderQr(); break;
    case RdMode::Dictionary: renderDict(); break;
    case RdMode::Weread: renderWeread(); break;
    case RdMode::WereadQr: renderWereadQr(); break;
    case RdMode::WereadMenu: renderWereadMenu(); break;
    case RdMode::WereadDl: renderWereadDl(); break;
    case RdMode::Wifi: renderWifi(); break;
    case RdMode::ShelfMenu: renderShelfMenu(); break;
    case RdMode::ShelfInfo: renderShelfInfo(); break;
    case RdMode::Recent: renderRecent(); break;
    case RdMode::FileBrowser: renderFileBrowser(); break;
    case RdMode::FileMenu: renderFileMenu(); break;
    case RdMode::FileRename: renderFileRename(); break;
    case RdMode::FileInfo: renderFileInfo(); break;
    case RdMode::Image: renderImage(); break;
    case RdMode::Opds: renderOpds(); break;
    case RdMode::NetShare: renderNetShare(); break;
    case RdMode::DictDl: renderDictDl(); break;
    case RdMode::ResDl: renderResDl(); break;
    case RdMode::KeyMap: renderKeyMap(); break;
    case RdMode::StatusBar: renderStatusBarSet(); break;
    case RdMode::About: renderAbout(); break;
    case RdMode::Settings: renderSettingsTab(); break;
    case RdMode::Notes: renderNotes(); break;
    case RdMode::NoteEdit: renderNoteEdit(); break;
    case RdMode::ShelfSearch: renderShelfSearch(); break;
    case RdMode::NotesSearch: renderNotesSearch(); break;
    case RdMode::Stats: renderStatsTab(); break;
    case RdMode::StatsBook: renderStatsBook(); break;
    case RdMode::StatsMore: renderStatsMore(); break;
    case RdMode::StatsHeatmap: renderStatsHeatmap(); break;
    case RdMode::StatsDay: renderStatsDay(); break;
    case RdMode::StatsProfile: renderStatsProfile(); break;
    case RdMode::StatsAdjust: renderStatsAdjust(); break;
    case RdMode::StatsSettings: renderStatsSettings(); break;
  }
  // 脚注弹注：盖在正文页上的一层（自己就是内容，不是提示），所以在所有别的浮层之前画。
  drawFootnotePopup();
  // 设置标签的选择弹层：也是"盖在底图上的一层"，底图由上面那个 case 画好，这里叠上去。
  drawSettingPicker();
  // 阻塞前的"正在…"浮层。画在模式内容之上、夜间反色之前，于是夜间模式下它也跟着
  // 反色，不会有一块白得刺眼的方块（见 rdShowBusy）。
  if (!st.busyMsg.empty()) {
    const int w = g_rd.getScreenWidth(), h = g_rd.getScreenHeight();
    const int lh = uiLineHeight();
    int bw = g_rd.getTextWidth(uiFontId(), st.busyMsg.c_str());
    if (!st.busySub.empty()) bw = std::max(bw, g_rd.getTextWidth(uiFontId(), st.busySub.c_str()));
    bw = std::min(bw + 64, w - 2 * MARGIN);
    const int bh = st.busySub.empty() ? lh + 32 : lh * 2 + 40;
    const int bx = (w - bw) / 2, by = (h - bh) / 2;
    g_rd.fillRect(bx, by, bw, bh, true);
    drawCenteredLine(by + 16, st.busyMsg.c_str(), false);
    if (!st.busySub.empty())
      drawCenteredLine(by + 16 + lh + 4,
                       g_rd.truncatedText(uiFontId(), st.busySub.c_str(), bw - 24).c_str(), false);
  }
  // 瞬时浮动提示。和 busy 浮层同款（同一套居中黑底反白框的算法），区别是它到点自己
  // 消失、且不在阻塞路径上。busy 在的时候不画（两者都在正中间，会叠字）。
  if (st.busyMsg.empty() && !st.floatMsg.empty() && esp_timer_get_time() < st.floatUntilUs) {
    const int w = g_rd.getScreenWidth(), h = g_rd.getScreenHeight();
    const int lh = uiLineHeight();
    int bw = g_rd.getTextWidth(uiFontId(), st.floatMsg.c_str());
    if (!st.floatSub.empty()) bw = std::max(bw, g_rd.getTextWidth(uiFontId(), st.floatSub.c_str()));
    bw = std::min(bw + 64, w - 2 * MARGIN);
    const int bh = st.floatSub.empty() ? lh + 32 : lh * 2 + 40;
    const int bx = (w - bw) / 2, by = (h - bh) / 2;
    g_rd.fillRect(bx, by, bw, bh, true);
    drawCenteredLine(by + 16, st.floatMsg.c_str(), false);
    if (!st.floatSub.empty())
      drawCenteredLine(by + 16 + lh + 4,
                       g_rd.truncatedText(uiFontId(), st.floatSub.c_str(), bw - 24).c_str(), false);
  }
  if (s_imgPresentSkipped) {
    // 图片解码被用户按键中止：这一帧只有半张图，推上去就是闪一下再换。
    // 什么都不推，屏幕保持上一张；下一轮那个键被处理掉后重新画。
    s_imgPresentSkipped = false;
    st.dirty = 1;
    return;
  }
  applyNightMode();
  const int64_t tDraw = esp_timer_get_time();
  // 极速刷(DU)只给**用实体键盘打字**的场景：界面上能打字（WiFi/OPDS 地址、笔记、
  // 词典查询），而且键盘不是虚拟键盘（蓝牙键盘连上了才自动收起）。虚拟键盘是手点的，
  // 一键之间有整段等待，抢不到那点刷新时间，DU 的低画质反而把残影留在屏上；那种场景
  // 与其他界面一样走常规局刷(GL16)。
  const bool physTyping = rdTypingScreen() && !st.vkVisible && g_bt.isConnected();
  // 阅读页：先量一下和上一帧的差异（顺便把基准刷成当前帧，别的策略下也保持新鲜，
  // 这样临时切到"自适应"不会因为基准过时而误判一次大变化）。
  int frameChange = -1;
  bool chapterChanged = false;
  if (st.mode == RdMode::Reading) {
    frameChange = rdFrameChangePermille();
    const std::string key =
        st.bookPath + "#" + std::to_string(st.bookKind == 0 ? st.spineIndex : -1);
    chapterChanged = !s_chapterKnown || key != s_chapterKey;
    s_chapterKey = key;
    s_chapterKnown = true;
  }
  // 这一帧是不是"翻了一页"（错相揭页只认这个）。在这里就取走，因为下面判定刷新档位
  // 时要拿它决定"临时降档"，而真正消费 s_pendingTurn 在更后面。
  const int pendingTurn = s_pendingTurn;
  bool stratAuto = false;        // 本帧用的是"自适应"档（见下面的降档逻辑）
  bool fullByCount = false;      // 本帧是"翻够页数强制全刷"那一页（下面降档要排除它）
  HalDisplay::RefreshMode m;
  if (st.fullRefresh) {
    m = HalDisplay::FULL_REFRESH;          // 进入新界面首帧：清掉上一屏的残影
  } else if (st.mode == RdMode::Reading) {
    const int strat = refreshStrategy();
    stratAuto = (strat == 3);
    // 全刷频率：选了 5/10/15/20 就每翻够这么多页清一次（与策略无关，但策略是
    // "全局"时本来就每页全刷，这一项没有意义，也不计数）。
    const int every = fullRefreshEvery();
    fullByCount = (strat != 0 && every > 0 && ++s_pagesSinceFull >= every);
    if (fullByCount) {
      s_pagesSinceFull = 0;
      ESP_LOGI(TAG, "全刷频率: 每 %d 页 → 全刷", every);
    }
    if (chapterChanged) {
      // 换章：整屏内容全换，残影最脏，用全刷清干净 —— 与用户选的策略无关。
      m = HalDisplay::FULL_REFRESH;
      ESP_LOGI(TAG, "换章强制全刷: %s spine=%d", st.bookTitle.c_str(), st.spineIndex);
    } else if (fullByCount) {
      m = HalDisplay::FULL_REFRESH;
    } else if (strat == 3) {
      // 自适应：按刚量出的变化量分档。
      //   ≥300‰   图片页/版式巨变 → 16 灰阶 GC16 全刷（要 16 级灰才不出带状）。
      //   ≥120‰   整页文字翻页、表格图表 → 8 灰阶 GL16 差分：30 相而非 37 相，
      //           每屏快约 80ms，不变的白像素不驱动所以不闪。正文是黑白像素，
      //           灰阶 16→8 看不出来。
      //   ≤40‰    空白页之类几乎没变 → 极速 DU。
      //   其余    默认局刷 GL16，同时把变化量累加成残影预算，攒够 2600‰ 用一次
      //           8 灰阶 GC16 全刷清账（该驱动不变像素、本来就要闪一次，
      //           用 30 相替 36 相省 ~70ms）。
      if (frameChange >= 300) {
        m = HalDisplay::FULL_REFRESH;
      } else if (frameChange >= 120) {
        m = HalDisplay::GRAY8_TEXT_REFRESH;
      } else if (frameChange <= 40) {
        m = HalDisplay::FAST_REFRESH;
      } else if (s_ghostAccum + frameChange >= 2600) {
        m = HalDisplay::GRAY8_REFRESH;
      } else {
        s_ghostAccum += frameChange;
        m = HalDisplay::HALF_REFRESH;
      }
      ESP_LOGI(TAG, "自适应: 变化 %d‰ 累计 %d‰ → %s", frameChange, s_ghostAccum,
               m == HalDisplay::FULL_REFRESH       ? "全刷16灰阶"
               : m == HalDisplay::GRAY8_REFRESH    ? "全刷8灰阶(清账)"
               : m == HalDisplay::GRAY8_TEXT_REFRESH ? "8灰阶正文刷"
               : m == HalDisplay::FAST_REFRESH     ? "极速"
                                                   : "局刷");
    } else {
      // 翻页走用户选的策略（设置 → 刷新策略）。
      switch (strat) {
        case 0:  m = HalDisplay::FULL_REFRESH; break;
        case 2:  m = HalDisplay::FAST_REFRESH; break;
        default:
          // 局刷：正文页走「8 灰阶正文刷」——和 GL16 同一条差分路（同样整屏压残影、
          // 同样计残影预算），只是 30 相替 37 相，每屏省约 80ms；正文是黑白像素，
          // 灰阶 16→8 看不出来。图片页（变化 ≥300‰）留着 GL16：16 级灰才不出带状。
          // 这不是偷偷改用户的选择：自动档从 120‰ 起本来就把正文页判给 8 灰阶正文刷，
          // 这里只是让"局刷"在正文页上与它对齐；真正需要 16 灰阶的图像页（≥300‰）
          // 两个档都会留着 GL16。要每页都黑白闪烁清账的人选"全局"。
          m = (frameChange >= 0 && frameChange < 300) ? HalDisplay::GRAY8_TEXT_REFRESH
                                                      : HalDisplay::HALF_REFRESH;
          break;
      }
    }
  } else {
    m = physTyping ? HalDisplay::FAST_REFRESH : HalDisplay::HALF_REFRESH;
  }
  // 错相揭页只替得掉差分刷（局刷 / 8 灰阶正文刷）。自适应档下正文翻页的变化量正好骑在
  // 300‰ 的"全刷"分界上（实测 215–330‰），于是同一本书一页能动、下一页不能动——用户
  // 侧就是"揭页时灵时不灵"。既然这一帧确实要翻页、也确实开着动画，就把档位降到可动画
  // 的那一档：全刷 → 8 灰阶正文刷（同样是整屏差分、同样扫掉残影，只是 8 灰阶），
  // 极速 DU → 局刷。**只动自适应**：全局/快刷是用户在设置里明选的档，不该被动画悄悄改写
  // （明选"全局"的人要的就是每页全刷，揭页动画本来就不适合全刷）。
  // 换章、全刷计数这两条走的是"该清就清"，不参与降档。
  if (pendingTurn != 0 && stratAuto && !st.fullRefresh && !chapterChanged && !fullByCount &&
      pageTurnAnimOn()) {
    if (m == HalDisplay::FULL_REFRESH) m = HalDisplay::GRAY8_TEXT_REFRESH;
    else if (m == HalDisplay::FAST_REFRESH) m = HalDisplay::HALF_REFRESH;
  }
  // 两种全刷都是整屏全像素过 LUT，残影一并清掉，预算与页数计数归零。
  if (m == HalDisplay::FULL_REFRESH || m == HalDisplay::GRAY8_REFRESH) {
    s_ghostAccum = 0;
    s_pagesSinceFull = 0;
  }
  const int64_t tPrep = esp_timer_get_time();
  // 错相揭页：这一帧是翻页、用户开着动画、且档位是差分正文刷（局刷/8 灰阶正文刷）时，
  // 把方向交给推屏层，让它用 16 条带依次入相的揭页替代这一次普通差分刷。
  // 其它档位（全刷/8 灰阶清账）与其它来由的帧（换章、进菜单、跳目录）都不动画 ——
  // 前者动画替不掉清残影，后者本来就不是"翻一页"。
  const int turn = pendingTurn;
  s_pendingTurn = 0;
  if (turn != 0 && pageTurnAnimOn() &&
      (m == HalDisplay::HALF_REFRESH || m == HalDisplay::GRAY8_TEXT_REFRESH)) {
    // 梯子：正文页用短梯（跟随表 DU 8 相），图片页用长梯（默认表 GL16 37 相）。
    // 判据和上面选档位用的是同一个变化量——≥300‰ 就是"有图/版式巨变"，短梯只有黑白
    // 两级会把图压成硬边。短梯其实比它替掉的那次 GL16 局刷还快（38 拍 × 7ms 扫描
    // ≈ 0.27s，而 GL16 局刷是 37 相 × 11.09ms 帧周期 ≈ 0.41s），所以动画不欠速度；
    // 长梯（97 拍 ≈ 0.68s）才是"为了不出带状宁可慢一点"的那一档。
    const bool textLike = (frameChange >= 0 && frameChange < 300);
    reader_hint_page_turn(turnDirFor(turn), textLike ? 1 : 0);
  }
  g_rd.displayBuffer(m);
  if (st.mode == RdMode::Reading) {
    const int64_t tEnd = esp_timer_get_time();
    ESP_LOGI(TAG, "翻页耗时: 绘制 %lldms 决策 %lldms 刷屏 %lldms 合计 %lldms",
             (tDraw - s_t0) / 1000, (tPrep - tDraw) / 1000, (tEnd - tPrep) / 1000,
             (tEnd - s_t0) / 1000);
    // 明细只解读"绘制"那一段。取页+栅格化+文字图 与 绘制 的差额是 clearScreen、
    // 状态栏、浮层、夜间反色——若它也很大，说明瓶颈在状态栏而不在正文。
    // 命中/未命中直接回答"字形缓存是不是在抖"：一页几百个字里未命中占大头，
    // 就该抬 TTF_CACHE_LIMIT（现在 1.5MB，PSRAM 还空着 4MB 多）。
    // 字体读：正文字面若是 SD 上的外部字体，冷字形会真读盘，这里会明显非零。
    if (s_prof.valid) {
      ESP_LOGI(TAG, "翻页明细: %s%s 取页 %lldms 栅格化 %lldms 文字图 %lldms 其余 %lldms | "
                    "图 %d 个 字形 %u 命中 %u 未命中 %u 光栅 %lldms 字体读 %lldms | "
                    "字库缓存 %u/%uKB | 面 %s",
               st.bookKind == 0 ? "EPUB" : st.bookKind == 1 ? "TXT" : "XTC",
               (st.bookKind == 0 && st.section && !st.section->isBuildComplete()) ? "(排版未完成)"
                                                                                  : "",
               s_prof.loadUs / 1000, s_prof.renderUs / 1000, s_prof.textmapUs / 1000,
               (tDraw - s_t0 - s_prof.loadUs - s_prof.renderUs - s_prof.textmapUs) / 1000,
               s_prof.images, (unsigned)s_prof.glyphs, (unsigned)s_prof.hits,
               (unsigned)s_prof.misses, s_prof.rasterUs / 1000, s_prof.fontReadUs / 1000,
               (unsigned)s_prof.cacheKB, (unsigned)s_prof.cacheCapKB,
               ttf_font_display_name());
      // 字库 I/O 细分（这是**绘制期间**的读，预取的读记在下面那条里）。
      // 整页预取做对的话，这里的次数应该接近 0；若仍很大，说明预取漏了字
      // （比如字形在 render 里又变了字号/字重，或 opentype 变量轴走了另外的偏移）。
      if (s_prof.fontReadCalls > 0) {
        const int64_t calls = (int64_t)s_prof.fontReadCalls;
        ESP_LOGI(
            TAG, "字库I/O: 读 %u 次 %uKB 共 %lldms(其中 seek %lldms) 均 %uB/次 %lldus/次 %lldKB/s",
            (unsigned)s_prof.fontReadCalls, (unsigned)(s_prof.fontReadBytes / 1024),
            s_prof.fontReadUs / 1000, s_prof.fontSeekUs / 1000,
            (unsigned)(s_prof.fontReadBytes / (uint32_t)calls), s_prof.fontReadUs / calls,
            s_prof.fontReadUs > 0 ? (int64_t)s_prof.fontReadBytes * 1000 / s_prof.fontReadUs
                                  : 0
        );
      }
      // 整页预取的效果。看三个数：
      //  · 块数→读次数 = 合并倍率（合并前每次读≈5KB，等于一次随机小读）；
      //  · 跨度 = 这一页的字形在字体表里的分布范围 —— 只有它接近整表大小时，
      //    "按章建字形 arena"那种大改才值得做；跨度小就说明本页的字本来就聚在一起，
      //    靠块缓存+顺序读就够了；
      //  · warmUs 是这次预取的总耗时，应该被后面的"字体读"降下来对冲掉。
      if (s_prof.ioBlocks > 0) {
        ESP_LOGI(TAG, "整页预取: %u 块→%u 次读 %uKB 跨度 %lldKB 耗时 %lldms(seek %lldms)",
                 (unsigned)s_prof.ioBlocks, (unsigned)s_prof.ioRuns,
                 (unsigned)(s_prof.warmBytes / 1024), (long long)(s_prof.ioSpan / 1024),
                 s_prof.warmUs / 1000, s_prof.warmSeekUs / 1000);
      } else {
        ESP_LOGI(TAG, "整页预取: 无块可读(字形已缓存/内建字体) 遍历 %lldus",
                 s_prof.warmUs);
      }
    }
  }
  st.fullRefresh = false;
  st.dirty = 0;
  // 这一帧已经推出去了，用户正盯着新页看 —— 正是把排版余量补回来的空档。
  // 放在计时之后：翻页明细/耗时那两条日志量的还是"翻页本身"，不掺预排版的账。
  rdPrebuildAhead();
}

// 先刷一帧"正在…"（当前界面 + 居中浮层）再进阻塞段。openBook 会建元数据、解 zip、
// 分章排版，大书要好几秒；e-ink 上这几秒整屏不动，用户会以为死机。
// renderCurrent 收尾已经把 fullRefresh/dirty 复位，这里不用再管。
static void rdShowBusy(const char *msg, const std::string &sub) {
  st.busyMsg = msg;
  st.busySub = sub;
  st.fullRefresh = true;   // 浮层首帧走全刷，免得和上一屏的残影叠在一起
  renderCurrent();
  st.busyMsg.clear();
  st.busySub.clear();
}

static void rdShowFloat(const std::string &msg, const std::string &sub, int ms) {
  if (msg.empty()) return;
  st.floatMsg = msg;
  st.floatSub = sub;
  st.floatUntilUs = esp_timer_get_time() + (int64_t)ms * 1000;
  st.dirty = 1;
}

// ── 各界面按键 ──────────────────────────────────────────────────────────
// 书架落点 (x,y) → 书目下标（不在任何封面上返回 -1）。点按（打开）和长按（锁定
// 目标）共用同一份几何：两处各算一遍迟早会走偏。列表样式一行一本，col 恒为 0。
static int shelfCellAt(int x, int y) {
  const int n = static_cast<int>(st.books.size());
  if (n <= 0) return -1;
  const int top = rdBarContentTop();
  const int contentW = g_rd.getScreenWidth() - 2 * MARGIN;
  const bool isList = shelfStyle() == ShelfStyle::List;
  const int cellW = isList ? contentW : contentW / shelfCols();
  const int cellH = isList ? shelfListRowH() : (shelfBottom() - top - 4) / shelfRows();
  if (cellW <= 0 || cellH <= 0) return -1;
  const int col = (x - MARGIN) / cellW;
  const int row = (y - top) / cellH;
  if (col < 0 || col >= shelfCols() || row < 0 || row >= shelfRows()) return -1;
  if (x < MARGIN || x >= MARGIN + contentW) return -1;
  const int base = (st.sel / coverPerPage()) * coverPerPage();
  const int idx = base + row * shelfCols() + col;
  return (idx >= 0 && idx < n) ? idx : -1;
}

static void handleBrowser(int key) {
  int n = static_cast<int>(st.books.size());
  auto setSel = [&](int v) { st.sel = clampI(v, 0, std::max(0, n - 1)); st.dirty = 1; };
  auto openSel = [&](int idx) {
    if (idx < 0 || idx >= n) return;
    rdShowBusy("正在打开…", st.books[idx].name);   // 大书开得慢，先给个视觉反馈
    if (openBook(st.books[idx].path, st.books[idx].kind)) {
      st.mode = RdMode::Reading;
      st.fullRefresh = true;
      st.dirty = 1;
    } else {
      ESP_LOGE(TAG, "打开失败: %s", st.books[idx].path.c_str());
    }
  };
  // 顶栏切换标签要在"书架为空"之前处理：没书时也得能切到微读/设置，否则会困在书架。
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      int t = tabHit(x, y);
      if (t >= 0) { switchTab(t); return; }
      // 顶部搜索栏：右端动作图标（搜索/刷新/微读），或点栏体（等价于点放大镜）。
      const int bar = rdBarIconHit(x, y, true);
      if (bar >= 0) { rdShelfBarAction(bar); return; }
      if (rdBarBodyHit(x, y, true)) { rdEnterSearch(RdMode::ShelfSearch); return; }
      if (n <= 0) return;  // 空书架：点标签以外的地方无事可做
      // 点按命中封面单元：第一下只选中，再点同一下才打开（大书打开要几十秒，
      // 命中即打开的话点错了代价太大）。键盘 Enter/KEY3 仍是"立即打开选中"。
      const int idx = shelfCellAt(x, y);
      if (idx >= 0) {
        if (st.sel == idx) {
          openSel(idx);
        } else {
          st.sel = idx;
          st.shelfIdx = -1;   // 手点选的：长按锁定的目标作废
          st.dirty = 1;
        }
      }
      return;
    }
    if (n <= 0) return;
    openSel(st.sel);  // 非点按回车(BLE/KEY3)：打开选中
    return;
  }

  if (n <= 0) {
    // 空书架：←→ 拿来切标签（有书时它们要换封面），否则空卡上会困在书架。
    if (key == KEY_LEFT) { switchTab(st.tab - 1); return; }
    if (key == KEY_RIGHT) { switchTab(st.tab + 1); return; }
    return;
  }

  // 列表样式 shelfCols()==1，↑↓ 退化成逐本移动，正是想要的行为。
  if (key == KEY_UP) { setSel(st.sel - shelfCols()); return; }
  if (key == KEY_DOWN) { setSel(st.sel + shelfCols()); return; }
  if (key == KEY_LEFT) { setSel(st.sel - 1); return; }
  if (key == KEY_RIGHT) { setSel(st.sel + 1); return; }
  if (key == KEY_PAGE_UP) { setSel(st.sel - coverPerPage()); return; }
  if (key == KEY_PAGE_DOWN) { setSel(st.sel + coverPerPage()); return; }
  if (key == KEY_HOME) { setSel(0); return; }
  if (key == KEY_END) { setSel(n - 1); return; }

  // 长按(触摸 KEY_TOUCH_LONG / Esc / 中间键 KEY2) → 书架菜单（含删除本书/图书详情）。
  // 关键在于**锁定长按的那一本**：菜单里的删除/详情都作用于 st.shelfIdx，不再是
  // st.sel——"长按 A 删掉 B"就是这么来的。这个键能原样到达这里，是因为
  // screen_reader_handle 顶部展平 KEY_TOUCH_LONG 的例外名单里加了 RdMode::Browser。
  // 落点在封面之外（搜索栏/标签栏/空白）时退回当前选中，与改动前一致。
  if (key == KEY_TOUCH_LONG || key == KEY_LONG_CONFIRM || key == 0x1B) {
    int idx = st.sel;
    if (key == KEY_TOUCH_LONG) {
      int x = 0, y = 0;
      if (input_tap_xy(&x, &y)) {
        const int hit = shelfCellAt(x, y);
        if (hit >= 0) idx = hit;
      }
    }
    st.sel = idx;   // 选中跟着长按走（与文件浏览器的长按一致）
    st.shelfIdx = idx;
    st.shelfMenuSel = 0;
    st.shelfDelArm = false;
    st.mode = RdMode::ShelfMenu;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
}

static void openMenu() {
  st.mode = RdMode::Menu;
  st.menuSel = 0;
  st.fullRefresh = true;
  st.dirty = 1;
}

// 返回书架（阅读菜单里的「返回书架」走这里）。当前书对象不释放，方便快速重新打开。
static void gotoBookshelf() {
  // 回书架 = 这次阅读会话结束（时长已经按心跳记进日桶了，这里只是收尾：够 3 分钟
  // 才 +1 次"阅读次数"，并产出 lastSessionSnapshot 供子界面显示）。
  ReadingStats::endSession();
  // 弹注是"阅读页之上的临时层"，回书架时一并收掉，否则下次进阅读页会冒出一个陈旧的框
  // （书本对象不释放，正文页也还是原来那页）。
  st.fnPopNum.clear();
  st.fnPopText.clear();
  st.fnPopScroll = 0;
  // 书内嵌字面还给用户字体：**这是阅读器里最大的一块常驻 PSRAM** —— 字形缓存上限就有
  // 3MB（ttf_font.c 的 TTF_CACHE_LIMIT），外加 glyf 映射/工作字库/IO 块。书本对象本身
  // 故意留着（下次点开同一本书不用重新解压分章），但字面没必要陪着留在内存里：书架和其它
  // 界面用的本来也是用户字体（uiFontId() 取内容面的字形）。
  // 代价：再点进这本书要重新解压 + 重读一次内嵌字体（大字体几百毫秒），换回的是整段
  // 书架时间的 PSRAM。缓存键里的 fontTag 不变，所以已排好的 .bin 版式照旧命中，不会重排。
  // 顺序：先还字体面，再清 bookFontLocal —— openEpub() 是用 bookFontLocal 是否为空
  // 判断"字面是否已就位"的，先清它会让重入时跳过装载。bookFontTag 一起清，跟 openBook 一致。
  applyUserContentFont();
  releaseBookFonts();  // 次字面那份 ~1MB 同理，不能陪在书架上
  st.bookFontLocal.clear();
  st.bookFontTag = 0;
  st.tab = 0;
  st.mode = RdMode::Browser;
  st.fullRefresh = true;
  st.dirty = 1;
}

static void gotoBookStart() {
  if (st.bookKind == 0) { openSpine(0); st.page = 0; }
  else if (st.bookKind == 1) st.txtPage = 0;
  else if (st.bookKind == 2) st.xtcPage = 0;
  st.dirty = 1;
}

static void gotoBookEnd() {
  if (st.bookKind == 0) {
    if (st.epub) {
      int n = st.epub->getSpineItemsCount();
      if (n > 0) { openSpine(n - 1); st.page = st.section ? st.section->pageCount - 1 : 0; }
    }
  } else if (st.bookKind == 1) {
    st.txtPage = std::max(0, totalPages() - 1);
  } else if (st.bookKind == 2) {
    if (st.xtc) st.xtcPage = std::max(0, static_cast<int>(st.xtc->getPageCount()) - 1);
  }
  st.dirty = 1;
}

// ── 选中 / 标注 ─────────────────────────────────────────────────────────
// 长按正文的某个词 → 选中它所在的那一句（直接可用的粒度，缩放由 ◀▶/点词微调），
// 同时在屏底弹出动作条。点已标注的文字 → 弹同一条动作条（删/改/查）。
static void rdBeginSelection(int wi) {
  int a = wi, b = wi;
  rdSentenceAt(g_pageText, wi, a, b);
  st.selActive = true;
  st.selStart = a;
  st.selEnd = b;
  st.selGrab = -1;
  st.selDrag = -1;
  st.selDragActive = false;
  st.selPageSpine = st.spineIndex;
  st.selPopup = 1;
  st.selMenuSel = 0;
  st.selNoteIdx = -1;
  // 选中的句子若正好就是某条已有标注，直接进"已标注"那一套（可以删/改/查）。
  const std::string sel = rdJoinWords(g_pageText, a, b);
  for (const auto &an : rdAnchorsOnPage(g_pageText)) {
    if (st.notes[an.noteIdx].text == sel) {
      st.selNoteIdx = an.noteIdx;
      st.selPopup = 2;
      break;
    }
  }
  st.fullRefresh = true;   // 反白块整片翻转，局刷残影太明显
  st.dirty = 1;
}

// 点按落在已有标注上 → 选中它并弹"已标注"菜单。
static bool rdTapOnAnnotation(int x, int y) {
  const int fontId = BODY_FONT_ID_BASE + st.fontLevel;
  const int wi = rdWordAtPoint(g_pageText, fontId, x, y);
  if (wi < 0) return false;
  for (const auto &an : rdAnchorsOnPage(g_pageText)) {
    if (wi >= an.a && wi <= an.b) {
      st.selActive = true;
      st.selStart = an.a;
      st.selEnd = an.b;
      st.selGrab = -1;
      st.selDrag = -1;
      st.selDragActive = false;
      st.selPageSpine = st.spineIndex;
      st.selNoteIdx = an.noteIdx;
      st.selPopup = 2;
      st.selMenuSel = 0;
      st.fullRefresh = true;
      st.dirty = 1;
      return true;
    }
  }
  return false;
}

// UTF-8 首字节 → 该字符字节数（非法字节按 1 计，够用）。
static int rdUtf8Len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

// 注号归一化：把各种写法的注号压成同一串再比。
// 晋书是 〔一〕、Duokan 是 [1]/1、有的书是 (1) / （1）/ 1. —— 去掉空白与各类括号/
// 句点后：〔一〕→"一"、[1]→"1"、1.→"1"。比对时两边都过一遍这个函数。
static std::string rdNormalizeNoteNumber(const std::string &s) {
  static const char *kDrop[] = {"〔", "〕", "（", "）", "【", "】", "｛", "｝", "「", "」", "　", "《", "》"};
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      // ASCII：空白与常见标点全丢，数字/字母留。
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '[' || c == ']' || c == '(' || c == ')' ||
          c == '.' || c == ',' || c == ':' || c == ';' || c == '-' || c == '\'')
        { i++; continue; }
      out.push_back(static_cast<char>(c));
      i++;
      continue;
    }
    const int len = rdUtf8Len(c);
    if (i + static_cast<size_t>(len) > s.size()) break;
    const std::string ch = s.substr(i, len);
    bool drop = false;
    for (const char *d : kDrop) if (ch == d) { drop = true; break; }
    if (!drop) out += ch;
    i += len;
  }
  return out;
}

// 点链接 → 按链接自己的 href 解析。**弹注/回跳的第一判据**，比 rdTapOnFootnote 那套号码
// 比对可靠：号码是解析器**猜**出来的（多看/QQ 阅读器的注号是张 `<img>`，链接里一个字都
// 没有，只能伪造成章节序号），href 是书里写死的。命中用页面链接矩形（Page::links），
// 顺带把"注号只有 1/3 字宽、手指点不中"一并解决。
//
// 三种去处：
//   ① 本页脚注表里有同名 href → 正向注号（注号和它那条脚注条目共用同一个 href）→ 弹注；
//   ② 纯页内锚点、但没有同名条目 → 回引（注文行首的 ※ 指回上标）或交叉引用 → 直达锚点；
//   ③ href 带文件名 → 别的 spine 里的注（calibre 的 notes.xhtml#fn1）→ 定位过去再查锚点。
//
// 返回 false = 这一下没落在链接上、或链接解不出目标，让下层照旧处理（翻页/菜单）。
static bool rdTapOnLink(int x, int y) {
  if (st.bookKind != 0 || !st.section || st.pageLinks.empty()) return false;
  // 手指容差：与 rdTapOnFootnote 同一套（那里有详细说明）。链接矩形就是字的框，
  // 注号那种半个字高的上标必须给足面积。
  constexpr int TOUCH_SLOP = 24;
  constexpr int MIN_TOUCH_WIDTH = 40;

  const RdState::RdLinkRect *hit = nullptr;
  long bestD2 = 0;
  for (const auto &lk : st.pageLinks) {
    // 与 PageLink::contains 同一套放宽法：窄链接按最小触摸宽度撑开，左右再各放 TOUCH_SLOP。
    const int boxW = std::max(lk.w, MIN_TOUCH_WIDTH);
    const int slopX = std::max(TOUCH_SLOP, (MIN_TOUCH_WIDTH - lk.w) / 2);
    if (x < lk.x - slopX || x > lk.x + boxW + slopX) continue;
    if (y < lk.y - TOUCH_SLOP || y > lk.y + lk.h + TOUCH_SLOP) continue;
    // 框重叠时取中心最近的那条（注文页一行一条，链接框会互相挨着）。
    const long dx = lk.x + boxW / 2 - x, dy = lk.y + lk.h / 2 - y;
    const long d2 = dx * dx + dy * dy;
    if (!hit || d2 < bestD2) { hit = &lk; bestD2 = d2; }
  }
  if (!hit || hit->href.empty()) return false;
  const std::string href = hit->href;

  // ① 正向注号：本页脚注表里有同一条 href。
  if (loadCurrentFootnotes()) {
    for (int i = 0; i < static_cast<int>(st.footnoteHrefs.size()); i++) {
      if (st.footnoteHrefs[i] != href) continue;
      if (!openFootnotePopup(i)) break;  // 条目在、正文取不到 → 落回下面按锚点跳
      // 记下上标位置，注文区那边按注号回跳时就能直接命中（见 rdGotoNoteRef）。
      rdRememberNoteRef(st.footnoteNums[i], st.spineIndex, st.page);
      st.mode = RdMode::Reading;
      st.fullRefresh = true;
      st.dirty = 1;
      ESP_LOGI(TAG, "点链接: '%s' → 弹注第 %d 条 (注号 '%s')", href.c_str(), i, st.footnoteNums[i].c_str());
      return true;
    }
  }

  // ②③ 回引/交叉引用：按锚点直达。锚点可能在本节后面还没排到的页上，rdFindFootnotePage
  // 自带"活构建优先 + 有界续排"。
  const size_t hash = href.rfind('#');
  if (hash == std::string::npos) return false;
  const std::string anchor = href.substr(hash + 1);
  if (anchor.empty()) return false;
  if (auto pg = rdFindFootnotePage(anchor)) {
    st.footnoteRetValid = false;  // 手已经落在目标上，旧的一次性返回点过期（同 rdGotoNoteRef）
    buildToPage(static_cast<int>(pg->page));
    st.mode = RdMode::Reading;
    st.fullRefresh = true;
    st.dirty = 1;
    ESP_LOGI(TAG, "点链接: '%s' → 锚点 '%s' 第 %d 页", href.c_str(), anchor.c_str(), (int)pg->page);
    return true;
  }
  // 跨文件：本节锚点表里没有，按文件名定位到别的 spine 再查一次（calibre 常见的
  // notes.xhtml#fn1 那种）。查不到就老实认输，交给下面更老的兜底。
  if (st.epub) {
    const int target = st.epub->resolveHrefToSpineIndex(href);
    if (target >= 0 && target != st.spineIndex) {
      auto sec = std::make_unique<Section>(st.epub, target, g_rd);
      if (auto pg = sec->getAnchorPosForAnchor(anchor)) {
        st.footnoteRetSpine = st.spineIndex;
        st.footnoteRetPage = st.page;
        st.footnoteRetValid = true;
        openSpine(target);
        buildToPage(static_cast<int>(pg->page));
        st.mode = RdMode::Reading;
        st.fullRefresh = true;
        st.dirty = 1;
        ESP_LOGI(TAG, "点链接: '%s' → 跨节 %d 第 %d 页", href.c_str(), target, (int)pg->page);
        return true;
      }
    }
  }
  ESP_LOGW(TAG, "点链接: '%s' 命中了链接框但解析不出目标", href.c_str());
  return false;
}

// 点正文里的上标注号 → 弹出对应脚注。
// 用现成的 g_pageText（每词带 style 字节）做命中测试，**不去开 collectTouchLinks**：
// 那会改 Section 缓存里 Page::links 的内容，代价大，而且这里要的「哪个词上了标」
// 信息词表里本来就有。
//
// ⚠ 实测结论（上机日志，不是推测）：**注号在词表里不是一个词**。
//   晋书的 `<sup><a href="#note-015">〔一五〕</a></sup>` 排完版被切成若干段——
//   `〕` 是不许起行的标点会粘在前一个 CJK 字上，于是词表里是 `〔` / `一` / `五〕`。
//   原来"拿被点中的那个词去比注号"因此**永远比不中**：点 `五〕` 归一化得 "五"，
//   而注号是 "一五"（日志 `点注: 点中上标词 '五〕' … 但注号列表 … '〔一五〕'`）。
//   顺带一提，单字命中框只有注号的 1/3 宽，所以"点上标"还常常点不中。
//
// 修法：先把这个词所在的那**一整段连续上标**并成一个"注号框"，用整段文本归一化比对，
//   命中判定也用整段的框。比对和命中面积一起修好。合并只在词表里**连续**的上标词之间
//   发生，所以正文里两个相隔的注号不会被并成一个（中间夹一个非上标词就断）。
static bool rdTapOnFootnote(int x, int y) {
  // 临时诊断（点注排查用，验收后删）：把每一次点按的判定依据打出来。
  const auto noHit = [&](const char *why) {
    ESP_LOGW(TAG, "点注: (%d,%d) 未命中 — %s", x, y, why);
    return false;
  };
  if (st.bookKind != 0 || !g_pageText.valid || !st.section)
    return noHit("前置不满足");
  if (!loadCurrentFootnotes())
    return noHit("本页没有脚注条目");
  const int fontId = BODY_FONT_ID_BASE + st.fontLevel;
  const int asc = g_rd.getFontAscenderSize(fontId);
  // 命中容差：注号字形缩到 50% 画（〔一〕这种只有半个字高、两个字宽），手指根本
  // 描不准那几像素。上游 EpubReaderUtils::linkAtPoint 的 6px/28px 是给鼠标/触控笔
  // 定的，在手指上明显不够——用户反馈"点不到"。所以这里按**手指**放宽：
  //   最小宽度 40px（约 9mm）＋左右各 24px 许差：一个字宽的注号手里有 ~88px 的宽带。
  //   纵向取**整行**（上沿到基线下的降部）再上下各放 24px：注号是上标，词表里记的
  //   仍是行基线，手指按在字上时落点常在基线上下飘。
  // 放宽不会误伤正文：这个函数只认 SUP/SUB 词，普通词根本不参与判定；代价只是注号
  // 附近那几十像素不再落进翻页/菜单分区（注号紧贴屏幕边缘时才有感）。
  constexpr int TOUCH_SLOP = 24;
  constexpr int MIN_TOUCH_WIDTH = 40;
  const int lineDesc = std::max(0, g_rd.getLineHeight(fontId) - asc);

  const int n = static_cast<int>(g_pageText.words.size());
  int supRuns = 0;
  // 命中不再是"扫到第一段就弹"。放宽容差之后，相邻两行的注号框会互相重叠（注文页
  // 一行一条 〔一〕〔二〕…，注号都在同一个 x 上，行距又只有 ~50px），先命中谁就弹谁
  // 会变成"点二弹一"。所以整页扫完，取**框中心离手指最近**的那一段。
  int bestFound = -1;
  long bestDist2 = 0;
  std::string mismatch;   // 最后一段"对上了注号却不在本页注号表里"的串（日志用）
  for (int i = 0; i < n;) {
    const RdWordHit &w0 = g_pageText.words[i];
    // 只认上标/下标：正文里一个普通的"1"不该把注弹出来。三种弹注写法都是上标
    // （晋书 <sup><a>、Duokan <a><sup>、QQ Reader 的 img-alt 由解析器补的 SUP）。
    if ((w0.style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) == 0) {
      i++;
      continue;
    }
    // 合并从 i 起的连续上标词：同一行（y 相同）且在词表里首尾相接。间隙上限取半个
    // 字高——上标字形缩到 50% 画，词与词之间可能留一点空档，但绝不该有半个字宽。
    int j = i + 1;
    while (j < n) {
      const RdWordHit &wj = g_pageText.words[j];
      if ((wj.style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) == 0) break;
      if (wj.y != w0.y) break;
      const RdWordHit &wp = g_pageText.words[j - 1];
      if (wj.x - (wp.x + wp.w) > asc / 2) break;
      j++;
    }
    const int runEnd = j;                      // [i, runEnd) 是一整段注号
    const RdWordHit &wLast = g_pageText.words[runEnd - 1];
    supRuns++;

    const int boxX = w0.x;
    const int boxW = std::max(wLast.x + wLast.w - boxX, MIN_TOUCH_WIDTH);
    if (x >= boxX - TOUCH_SLOP && x <= boxX + boxW + TOUCH_SLOP &&
        y >= w0.y - asc - TOUCH_SLOP && y <= w0.y + lineDesc + TOUCH_SLOP) {
      std::string marker;
      for (int k = i; k < runEnd; k++) marker += g_pageText.words[k].text;
      const std::string norm = rdNormalizeNoteNumber(marker);
      if (!norm.empty()) {
        int found = -1;
        for (int k = 0; k < static_cast<int>(st.footnoteNums.size()); k++) {
          if (rdNormalizeNoteNumber(st.footnoteNums[k]) == norm) { found = k; break; }
        }
        if (found < 0) {
          // 兜底：合并后仍不相等时，允许"唯一一个注号以本串结尾"。只兜排版把段首
          // 丢掉之类的残例；有歧义（≥2 个候选）就放弃，宁可不动也不能弹错注。
          int cand = -1, cnt = 0;
          const size_t nl = norm.size();
          for (int k = 0; k < static_cast<int>(st.footnoteNums.size()); k++) {
            const std::string q = rdNormalizeNoteNumber(st.footnoteNums[k]);
            if (q.size() >= nl && q.compare(q.size() - nl, nl, norm) == 0) { cand = k; cnt++; }
          }
          if (cnt == 1) found = cand;
        }
        if (found >= 0) {
          // 注号框中心：横向取合并段中点；纵向取行上半（注号是上标，画在基线上方）。
          const long cx = boxX + boxW / 2;
          const long cy = w0.y - asc / 2;
          const long dx = x - cx, dy = y - cy;
          const long d2 = dx * dx + dy * dy;
          if (bestFound < 0 || d2 < bestDist2) { bestDist2 = d2; bestFound = found; }
        } else {
          mismatch = marker + "' (norm='" + norm + "')";
        }
      }
    }
    i = runEnd;
  }
  if (bestFound >= 0) {
    if (openFootnotePopup(bestFound)) {
      // 点中的就是正文里的上标，此刻这一页就是它的位置 —— 记下来，好让注文区那边
      // 点行首注号能跳回来（见 rdGotoNoteRef）。
      rdRememberNoteRef(st.footnoteNums[bestFound], st.spineIndex, st.page);
      st.mode = RdMode::Reading;  // 弹注是盖在正文页上的浮层
      st.fullRefresh = true;
      st.dirty = 1;
      return true;
    }
    return noHit("注号对上了但取不到注文");
  }
  if (!mismatch.empty()) {
    std::string all;
    for (const auto &q : st.footnoteNums) { all += "'"; all += q; all += "' "; }
    ESP_LOGW(TAG, "点注: 点中注号段 '%s' 但注号列表 %u 条对不上: %s", mismatch.c_str(),
             (unsigned)st.footnoteNums.size(), all.c_str());
    return false;
  }
  char buf[128];
  snprintf(buf, sizeof(buf), "注号段 %d 个（词表 %u），点没落在任何注号框内", supRuns,
           (unsigned)g_pageText.words.size());
  return noHit(buf);
}

// 点"注文条目行首的注号" → 跳到正文里对应的那个**上标**处。
//
// 早先只认**刚跳过的那一条**（把它的注号存在 st 里对比），跳完就失效——在注文区多翻
// 两页、去点别的注文条目的注号就什么都不认（用户反馈"翻页后跳转功能就失效了"）。
// 现在按**注号**查表：认出行首的注号就交给 rdGotoNoteRef 去反查上标位置，任何时候点
// 任何一条注文条目的注号都能跳回去。
//
// 命中测试跟 rdTapOnFootnote 反过来：那边点的是正文里的**上标**注号（SUP/SUB），
// 这边点的是注文段落开头的**正文号**（"〔一〕"是正常字号的普通词）。所以判据换成
// "行首 + 注号 + 归一化后能反查到上标"，不去看 style 位。
static bool rdTapOnNoteBack(int x, int y) {
  if (st.bookKind != 0 || !st.section) return false;
  if (!g_pageText.valid) return false;
  const int fontId = BODY_FONT_ID_BASE + st.fontLevel;
  const int asc = g_rd.getFontAscenderSize(fontId);
  const int lineDesc = std::max(0, g_rd.getLineHeight(fontId) - asc);
  // 与 rdTapOnFootnote 同一套放宽后的手指容差（见那处的说明）。注文行首的注号
  // （〔一〕）是正文大小，但只有两个字宽，一样得给足面积。
  constexpr int TOUCH_SLOP = 24;
  constexpr int MIN_TOUCH_WIDTH = 40;

  // 注号可能被排版切成几个词（跟 rdTapOnFootnote 那边同理），所以按"行"扫：
  // 每行取头几个词拼出开头文本，认出注号后再用这几个词的合并框做命中判定。
  const int n = static_cast<int>(g_pageText.words.size());
  for (int li = 0; li + 1 < static_cast<int>(g_pageText.lineFirst.size()); li++) {
    const int begin = g_pageText.lineFirst[li];
    const int end = std::min(g_pageText.lineFirst[li + 1], n);
    if (begin >= end) continue;
    const int y0 = g_pageText.words[begin].y;
    // 拼注号：**不能**见到第一个"号码词"就停 —— CJK 排版单字成词，〔一二〕会被切成
    // 〔/一/二/〕四个词，停下来只会拿到"一"，回跳于是永远落在第 1 条上（二〇~二九
    // 落在第 2 条）。规则改成"拼到号码不再变长为止"：括号注号拼到闭括号收全，
    // 纯数字注号（1.）拼到号码不再增长（下一个词是正文）就停。
    std::string head, mark;
    int stop = begin;   // 注号覆盖到的词下标上界（不含）
    for (int k = begin; k < end && k < begin + 8; k++) {
      const RdWordHit &wk = g_pageText.words[k];
      if (wk.y != y0) break;
      const std::string next = head + wk.text;
      std::string nm;
      rdLeadingNoteMarker(next, &nm);
      const bool closed = rdHeadBracketClosed(next);
      // 号码不再增长、又没读到闭括号 → 上一词已经把注号收全，这一词属于正文。
      if (!closed && !mark.empty() && nm.size() <= mark.size()) break;
      head = next;
      if (!nm.empty()) mark = nm;
      stop = k + 1;
      if (closed) break;             // 〔一二〕/（1）已收全
      if (head.size() > 24) break;   // 注号不会这么长，防止把整行都拼进来
    }
    if (mark.empty()) continue;
    // 只认**像注号的行首**：括号包着的（〔一〕/（1）/[1]）或纯阿拉伯数字（1.）。光秃秃的
    // 中文数字开头（"十三州…"这类正常正文）不算——不然正文里随便一行都成了跳转键。
    const std::string norm = rdNormalizeNoteNumber(mark);
    if (norm.empty()) continue;
    const bool digitOnly = norm.find_first_not_of("0123456789") == std::string::npos;
    if (!rdStartsBracketed(head) && !digitOnly) continue;
    const RdWordHit &w0 = g_pageText.words[begin];
    const RdWordHit &wLast = g_pageText.words[stop - 1];
    const int boxX = w0.x;
    const int boxW = std::max(wLast.x + wLast.w - boxX, MIN_TOUCH_WIDTH);
    if (x >= boxX - TOUCH_SLOP && x <= boxX + boxW + TOUCH_SLOP &&
        y >= y0 - asc - TOUCH_SLOP && y <= y0 + lineDesc + TOUCH_SLOP) {
      if (!rdGotoNoteRef(mark)) continue;   // 反查不到上标就不认这一下，让下层照常处理
      ESP_LOGI(TAG, "点注: 点中注文行首 '%s' → 跳回正文上标", mark.c_str());
      return true;
    }
  }
  return false;
}

// 写/改笔记：Enter 保存，Esc 取消。新建时原文存在 notePendingText 里。
static void rdNoteCommit() {
  if (st.noteEditIdx >= 0 && st.noteEditIdx < static_cast<int>(st.notes.size())) {
    st.notes[st.noteEditIdx].note = st.noteEditBuf;
  } else if (!st.notePendingText.empty()) {
    RdState::RdNote n;
    n.path = st.bookPath;
    n.book = st.bookTitle;
    n.spine = st.spineIndex;
    n.page = st.page;
    n.text = st.notePendingText;
    n.note = st.noteEditBuf;
    n.time = static_cast<int64_t>(time(nullptr));
    st.notes.push_back(std::move(n));
  }
  saveNotes();
  st.notePendingText.clear();
  st.noteEditIdx = -1;
  st.vkVisible = false;
  st.mode = RdMode::Reading;
  st.fullRefresh = true;
  st.noteStatus = "已保存笔记";
  st.dirty = 1;
}

static void rdSelActivate() {
  const int sel = st.selMenuSel;
  const std::string selText = rdJoinWords(g_pageText, st.selStart, st.selEnd);

  // 3=复制 / 4=取消：两种浮层同一位置（见 rdPopupButtonLabel），所以先处理，
  // 不必再跟着 popup 分支走一遍。
  if (sel == 4) {                              // 取消
    st.selActive = false;
    st.dirty = 1;
    return;
  }
  if (sel == 3) {                              // 复制到跨模式粘贴板
    clipboardPush(selText);
    st.noteStatus = clipboardLastTruncated() ? "已复制（超长已截断）" : "已复制";
    st.selActive = false;
    st.dirty = 1;
    return;
  }
  if (st.selPopup == 2 && sel == 0) {          // 删除标注
    if (st.selNoteIdx >= 0 && st.selNoteIdx < static_cast<int>(st.notes.size())) {
      st.notes.erase(st.notes.begin() + st.selNoteIdx);
      saveNotes();
    }
    st.selActive = false;
    st.selNoteIdx = -1;
    st.noteStatus = "已删除标注";
    st.dirty = 1;
    return;
  }
  if (st.selPopup == 2 && sel == 1) {          // 改笔记
    st.noteEditIdx = st.selNoteIdx;
    st.noteEditBuf = (st.selNoteIdx >= 0 && st.selNoteIdx < static_cast<int>(st.notes.size()))
                         ? st.notes[st.selNoteIdx].note
                         : std::string();
    st.selActive = false;
    st.mode = RdMode::NoteEdit;
    rdVkWantShow();
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (sel == 2) {                              // 查字典（两种浮层都有）
    st.dictQuery = selText;
    st.retMode = RdMode::Reading;
    st.selActive = false;
    openDictionary();
    doDictLookup();
    st.mode = RdMode::Dictionary;
    st.vkVisible = false;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  // 未标注浮层：0=标注（只划线） 1=笔记
  if (sel == 0) {
    RdState::RdNote n;
    n.path = st.bookPath;
    n.book = st.bookTitle;
    n.spine = st.spineIndex;
    n.page = st.page;
    n.text = selText;
    n.time = static_cast<int64_t>(time(nullptr));
    st.notes.push_back(std::move(n));
    saveNotes();
    st.selActive = false;
    st.noteStatus = "已标注";
    st.dirty = 1;
    return;
  }
  st.noteEditIdx = -1;
  st.noteEditBuf.clear();
  st.notePendingText = selText;
  st.selActive = false;
  st.mode = RdMode::NoteEdit;
  rdVkWantShow();
  st.fullRefresh = true;
  st.dirty = 1;
}

// ── 从阅读页打开插图查看器 ──────────────────────────────────────────────
// 查看器的图片列表 = **当前节**的插图（左右翻页只在这一节里走）。从第 0 页起按文档序
// 扫，遇到含图的页就把该页的图依次收进来——和 rdSearchNoteRefPage 一样有页数/时间预算，
// 扫不动就退化成"只有长按的这一张"。目标路径一定在列表里（找不到就单张成列），
// 否则查看器会停在错的那张图上。
static void rdCollectSectionImages(const std::string &target) {
  st.imgList.clear();
  st.imgSel = 0;
  if (st.section) {
    constexpr int kMaxPages = 200;
    constexpr int64_t kBudgetUs = 400 * 1000;
    const int64_t deadline = esp_timer_get_time() + kBudgetUs;
    const int total = std::min<int>(st.section->pageCount, kMaxPages);
    for (int p = 0; p < total; p++) {
      if (esp_timer_get_time() > deadline) break;
      auto page = st.section->loadPage(p);
      if (!page) continue;
      for (const auto &el : page->elements) {
        if (el->getTag() != TAG_PageImage) continue;
        const std::string &ip = static_cast<const PageImage &>(*el).getImageBlock().getImagePath();
        if (!ip.empty()) st.imgList.push_back(ip);
      }
    }
  }
  for (size_t i = 0; i < st.imgList.size(); i++)
    if (st.imgList[i] == target) { st.imgSel = static_cast<int>(i); return; }
  // 扫不到目标（预算用完、或被扫的页还没排出来）：只放这一张，免得显示错图。
  st.imgList.assign(1, target);
  st.imgSel = 0;
}

// 长按点是否落在一张插图的矩形上？是就打开查看器并返回 true，否则 false（调用方
// 继续走选词）。矩形 = 元素页内坐标 + 渲染偏移，与 PageLine/链接矩形同一套换算。
static bool rdOpenImageAt(int x, int y) {
  if (!st.section) return false;
  auto page = st.section->loadPage(st.page);
  if (!page) return false;
  const int bm = bodyMargin();
  for (const auto &el : page->elements) {
    if (el->getTag() != TAG_PageImage) continue;
    ImageBlock &img = static_cast<PageImage &>(*el).getImageBlock();
    const int rx = el->xPos + bm;
    const int ry = el->yPos + RD_BODY_TOP;
    const int rw = img.getWidth(), rh = img.getHeight();
    if (x < rx - 12 || x > rx + rw + 12 || y < ry - 12 || y > ry + rh + 12) continue;
    // 插图是延迟提取的：文件还没从 zip 里抠出来先抠（失败就退回占位框，别弹空查看器）。
    if (!img.ensureExtracted() || img.getImagePath().empty()) continue;
    rdCollectSectionImages(img.getImagePath());
    st.imgFromReader = true;
    st.imgZoom = 1.0f;
    st.imgPanX = 0.0f;
    st.imgPanY = 0.0f;
    st.mode = RdMode::Image;
    st.fullRefresh = true;
    st.dirty = 1;
    return true;
  }
  return false;
}

static void handleReading(int key) {
  int w = g_rd.getScreenWidth();
  const int fontId = BODY_FONT_ID_BASE + st.fontLevel;
  const int nw = static_cast<int>(g_pageText.words.size());

  // ── 脚注弹注：浮层最上层，开着的时候键全归它管（正文不翻页、菜单不开） ──
  if (!st.fnPopNum.empty()) {
    if (key == 0x1B) { closeFootnotePopup(); return; }
    if (key == KEY_UP) { st.fnPopScroll = std::max(0, st.fnPopScroll - 1); st.dirty = 1; return; }
    if (key == KEY_DOWN) { st.fnPopScroll++; st.dirty = 1; return; }  // 上限在画的时候夹住
    if (key == KEY_PAGE_UP) { st.fnPopScroll = std::max(0, st.fnPopScroll - 8); st.dirty = 1; return; }
    if (key == KEY_PAGE_DOWN) { st.fnPopScroll += 8; st.dirty = 1; return; }
    if (key == '\n') {
      int x = 0, y = 0;
      // 触摸：点框里 = 看注释原文（原来的"跳转"），点框外 = 收起弹注。没坐标的 Enter
      // （实体键 KEY2）按"跳转"算——键盘上 Esc 就在旁边，不需要另一个"关闭"键。
      if (input_tap_xy(&x, &y)) {
        const bool inside = x >= st.fnPopBoxX && x < st.fnPopBoxX + st.fnPopBoxW &&
                            y >= st.fnPopBoxY && y < st.fnPopBoxY + st.fnPopBoxH;
        if (!inside) { closeFootnotePopup(); return; }
      }
      // 跳的必须是**这个弹注显示的那一条**。原来是拿列表的选中项 st.footnoteSel，
      // 而点正文上标直接开弹注时根本没动过它 —— 跳到的是列表上一次选中的那条（多半
      // 是 0）。用弹注自己记下的下标才对得上。
      const int idx = (st.fnPopIdx >= 0) ? st.fnPopIdx : st.footnoteSel;
      closeFootnotePopup();
      jumpToFootnote(idx);  // 它自己会置 mode/fullRefresh
      return;
    }
    return;
  }

  // ── 选中态：键先给选区/浮层用，翻页等一概不理会 ─────────────────────
  if (st.selActive) {
    // 按住手柄拖动：把被拖的那一端挪到手指底下的词上。放在最前面——拖动帧不是
    // "命令"，不该落进下面的取消/微调里。
    if (key == KEY_TOUCH_DRAG) {
      if (rdSelDragStep(fontId) >= 0) {
        st.selDragActive = true;
        st.dirty = 1;
      }
      return;
    }
    if (key == KEY_TOUCH_LONG || key == KEY_LONG_CONFIRM || key == 0x1B) {
      st.selActive = false;
      st.selDrag = -1;
      st.selDragActive = false;
      st.dirty = 1;
      return;
    }
    // 拖完抬手，hw/input 照常补一个键（拖到远处 = 左右/上下滑，拖回原处 = 点按）：
    // 那是同一个手势的尾巴，不是新命令，吃掉它 —— 否则刚拖到位的端点又被 ±1/±5
    // 挪一格，或者顺手把浮层上的按钮按了。
    if (st.selDragActive) {
      st.selDrag = -1;
      st.selDragActive = false;
      if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN ||
          key == '\n')
        return;
    }
    // ←→ 微调一个词，↑↓ 一次挪 5 个词。动的是**抓起来的那一端**（没抓手柄时是终点，
    // 与以前一致）——手柄和键盘因此走同一条通路：抓了起点再按 ← 缩的就是起点。
    if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN) {
      const int step = (key == KEY_LEFT)    ? -1
                       : (key == KEY_RIGHT) ? +1
                       : (key == KEY_UP)    ? -5
                                            : +5;
      if (st.selGrab == 0) st.selStart = clampI(st.selStart + step, 0, st.selEnd);
      else st.selEnd = clampI(st.selEnd + step, st.selStart, std::max(0, nw - 1));
      st.dirty = 1;
      return;
    }
    if (key == '\n') {
      int x, y;
      if (input_tap_xy(&x, &y)) {
        const int btn = rdPopupButtonAt(x, y);
        if (btn >= 0) { st.selMenuSel = btn; rdSelActivate(); return; }
        // ① 点到手柄：抓起那一段（再点同一个 = 放下）。手柄画在行外，rdWordAtPoint
        //    在那里量不到任何词，所以必须单独判、判中就直接返回。
        const int h = rdSelHandleAt(g_pageText, fontId, x, y);
        if (h >= 0) {
          st.selGrab = (st.selGrab == h) ? -1 : h;
          st.dirty = 1;
          return;
        }
        // ② 点到词：把端点挪过去。抓着手柄时就挪那一段；没抓就按词的位置就近吸——
        //    在选区左 → 起点，右 → 终点，落在选区**里面** → 近的那一端（用来往里收）。
        //    两端始终 clamp 住不交叉，区间可双向伸缩（原来只动终点、起点钉死，
        //    用户侧就是"起始位置是固定的，只能选结束位置"）。
        const int wi = rdWordAtPoint(g_pageText, fontId, x, y);
        if (wi >= 0) {
          if (st.selGrab == 0) st.selStart = std::min(wi, st.selEnd);
          else if (st.selGrab == 1) st.selEnd = std::max(wi, st.selStart);
          else if (wi <= st.selStart) st.selStart = std::min(wi, st.selEnd);
          else if (wi >= st.selEnd) st.selEnd = std::max(wi, st.selStart);
          else if (wi - st.selStart <= st.selEnd - wi) st.selStart = wi;
          else st.selEnd = wi;
          st.selGrab = -1;
          st.dirty = 1;
        }
        return;
      }
      // 无坐标回车（KEY2）：展开动作条
      st.selPopup = (st.selNoteIdx >= 0) ? 2 : 1;
      st.selMenuSel = 0;
      st.dirty = 1;
      return;
    }
    return;
  }

  // ── 长按：先看是不是插图（图片页长按 = 全屏看图），不是再选中该词所在的一句 ──
  if (key == KEY_TOUCH_LONG) {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      // input_tap_xy 是一次性的：取到就先用它判图片，判不出再拿同一份坐标去选词
      // （所以这里不能像上面那样在 rdWordAtPoint 里再调一次）。
      if (rdOpenImageAt(x, y)) return;
      if (g_pageText.valid) {
        const int wi = rdWordAtPoint(g_pageText, fontId, x, y);
        if (wi >= 0) { rdBeginSelection(wi); return; }
      }
    }
    return;   // 没落到文字/图片上（空白）：什么也不做
  }

  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      // 点已标注的文字 → 弹出这条标注的动作菜单
      if (rdTapOnAnnotation(x, y)) return;
      // 点链接（注号 / 注文行首的 ※ / 交叉引用）→ 按链接 href 解析。放在最前面：
      // 有了本节链接矩形就不必再靠"注号长得像什么"去猜（见 rdTapOnLink）。
      if (rdTapOnLink(x, y)) return;
      // 兜底：链接矩形认不出时的老路——点上标注号 → 按注号比对本页脚注表
      if (rdTapOnFootnote(x, y)) return;
      // 点"注文条目行首的注号" → 跳到正文里那个上标处（任何时候、任何一条都行）
      if (rdTapOnNoteBack(x, y)) return;
      // 临时诊断：点按走到了这里说明上面两条都没认，落进了翻页/菜单分区。
      ESP_LOGW(TAG, "点注: 点按 (%d,%d) 未被上标/标注认领 → 落进 %s", x, y,
               x < w / 3 ? "左1/3翻页" : (x > w * 2 / 3 ? "右1/3翻页" : "中间菜单"));
      // crossmux 阅读器触摸分区：左右 1/3 翻页，中间 1/3 菜单
      if (x < w / 3) turnBook(-1);
      else if (x > w * 2 / 3) turnBook(+1);
      else openMenu();
      return;
    }
    // 临时诊断：收到回车却没有点按坐标（BLE/KEY3 的 Enter）—— 说明触摸那一下
    // 根本没变成 '\n'（被当成滑动/长按/边缘返回吃掉了）。
    ESP_LOGW(TAG, "点注: 收到 '\\n' 但无点按坐标 → 当菜单键");
    openMenu();  // 非点按回车(BLE/KEY3)
    return;
  }
  // 双击左右电容键 = 跳上/下一章（由 main.cpp 的电容键双击分支产生）。
  if (key == KEY_NEXT_CHAPTER) { jumpChapter(+1); return; }
  if (key == KEY_PREV_CHAPTER) { jumpChapter(-1); return; }
  if (key == KEY_UP || key == KEY_LEFT || key == KEY_PAGE_UP) { turnBook(-1); return; }
  if (key == KEY_DOWN || key == KEY_RIGHT || key == KEY_PAGE_DOWN) { turnBook(+1); return; }
  if (key == KEY_HOME) { gotoBookStart(); return; }
  if (key == KEY_END) { gotoBookEnd(); return; }
  // 长按中间确认键不在这里：它现在是全局的"待机"（main.cpp），只有词典管理/按键映射/
  // 统计删书这三个把它当动作键的子界面才被放行（screen_reader_long_confirm_is_action）。
  // 原来的"长按 = 全屏刷新"改由**晃动机身**触发，落点见 screen_reader_handle 顶部的
  // KEY_SHAKE 分支。
  if (key == 0x1B) { openMenu(); return; }
}

static void handleToc(int key) {
  const int n = (st.bookKind == 1) ? static_cast<int>(st.txtChapterTitles.size())
                                   : (st.epub ? st.epub->getTocItemsCount() : 0);
  if (key == 0x1B) { st.mode = RdMode::Menu; st.fullRefresh = true; st.dirty = 1; return; }
  if (key == KEY_UP) { st.tocSel = std::max(0, st.tocSel - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.tocSel = std::min(n - 1, st.tocSel + 1); st.dirty = 1; return; }
  // 上下滑 = 目录整屏翻。选区每帧都被拉回屏幕正中（见 renderToc 的 start 计算），
  // 所以按一屏走一格，列表就正好前进/后退一屏，跟翻书一样。
  if (key == KEY_PAGE_UP) { st.tocSel = std::max(0, st.tocSel - tocRowsPerPage()); st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.tocSel = std::min(n - 1, st.tocSel + tocRowsPerPage()); st.dirty = 1; return; }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y) && n > 0) {
      int top = coverTop();
      int itemH = uiLineHeight() + 6;
      int maxRows = tocRowsPerPage();
      int start = clampI(st.tocSel - maxRows / 2, 0, std::max(0, n - maxRows));
      int row = start + (y - top) / itemH;
      if (row >= 0 && row < n) st.tocSel = row;
    }
    if (n > 0) {
      st.tocSel = clampI(st.tocSel, 0, n - 1);
      if (st.bookKind == 1) {
        st.txtPage = txtPageForOffset(st.txtChapterOffsets[static_cast<size_t>(st.tocSel)]);
      } else if (st.epub) {
        openSpine(st.epub->getSpineIndexForTocIndex(st.tocSel));
        st.page = 0;
      }
      st.mode = RdMode::Reading;
      st.fullRefresh = true;
      st.dirty = 1;
    }
    return;
  }
}

static void doMenuAction(MenuAct act) {
  switch (act) {
    case MenuAct::Toc:
      if (st.bookKind == 1) {
        if (!st.txtChapterTitles.empty()) {
          selectTxtChapterForCurrentPage();  // 几千章的 TXT，进目录先落在当前章
          st.mode = RdMode::Toc;
          st.fullRefresh = true;
        }
      } else if (st.epub && st.epub->getTocItemsCount() > 0) {
        // 进目录先落在**正在读的这一章**上（TXT 那条路一直是这样，见
        // selectTxtChapterForCurrentPage；EPUB 这边原来写死 0，几十上百条的目录要从头
        // 往下翻好几屏才找得到当前位置）。getTocIndexForSpineIndex 取的就是当前 spine
        // 对应的目录项：构建元数据时对"没有直接目录项"的 spine 做过"继承上一条"的回填
        // （BookMetadataCache 的 lastSpineTocIndex），所以它落在正确的章节上而不是 -1。
        // 渲染那边本来就以 tocSel 为中心开窗（renderToc 的 start = tocSel - maxRows/2），
        // 所以设了它，目录也跟着滚到当前章。
        st.tocSel =
            clampI(st.epub->getTocIndexForSpineIndex(st.spineIndex), 0, st.epub->getTocItemsCount() - 1);
        st.mode = RdMode::Toc;
        st.fullRefresh = true;
      }
      break;
    case MenuAct::Font:
      st.fontLevel = (st.fontLevel + 1) % kUserFontLevels;
      g_settings.setString("reader_font_level_v2", std::to_string(st.fontLevel));
      reopenBook();
      st.fullRefresh = true;
      break;
    case MenuAct::FontFamily: {
      // 本书在用内嵌字体时，这个键改的是**用户全局字体**，对当前这本没有可见效果
      // （书内字体优先，见 openEpub）。与其让用户按了没反应、以为设置坏了，不如直说
      // 并且指路到那本书的开关上。
      if (!st.bookFontLocal.empty()) {
        rdShowFloat("本书使用内嵌字体", "设置 → 内嵌字体 可关闭", 3000);
        break;
      }
      // 内建 → SD 扫描到的字体 → 回到内建（与写作模式设置里的列表一致）。
      // GfxRenderer 文本路径直接调 ttf_font_*，故 ttf_font_open 后即生效；
      // 需重排正文（字形度量变了）。
      int n = ttf_font_scan();
      const char *cur = ttf_font_path();
      int curIdx = 0;
      if (cur && !ttf_font_path_is_builtin(cur)) {
        for (int i = 0; i < n; i++)
          if (strcmp(cur, ttf_font_item(i)->path) == 0) { curIdx = i + 1; break; }
      }
      int nextIdx = (curIdx + 1) % (n + 1);
      const char *nextPath = (nextIdx == 0) ? "" : ttf_font_item(nextIdx - 1)->path;
      app_settings_set_font_path(nextPath);
      if (ttf_font_open(nextPath) != 0) {
        ESP_LOGW(TAG, "字体打开失败，回落内建: %s", nextPath ? nextPath : "(builtin)");
        (void)ttf_font_open_builtin();
        app_settings_set_font_path("");
      }
      reopenBook();
      st.fullRefresh = true;
      break;
    }
    case MenuAct::LineSpacing:
      st.lineSpacing = kLineSpacings[(spacingIdx() + 1) % 5];
      g_settings.setString("reader_line_spacing", std::to_string(st.lineSpacing));
      reopenBook();
      st.fullRefresh = true;
      break;
    case MenuAct::ParaSpacing:
      st.paraSpacing = (st.paraSpacing + 1) % 6;
      g_settings.setString("reader_para_spacing", std::to_string(st.paraSpacing));
      reopenBook();  // EPUB 要让 Section 按新档位重排；TXT 要重扫分页表
      st.fullRefresh = true;
      break;
    case MenuAct::Indent:
      // 内嵌模式下缩进由书里的 CSS 说了算，这一档按下无效；标签上已经写着"随书"，
      // 值保留着，切回「强制指定」立刻按它生效。
      if (styleEmbedded() && st.bookKind == 0) break;
      st.indentMode = (st.indentMode + 1) % 3;
      g_settings.setString("reader_indent", std::to_string(st.indentMode));
      reopenBook();
      st.fullRefresh = true;
      break;
    case MenuAct::Align:
      if (styleEmbedded() && st.bookKind == 0) break;   // 同理：对齐也随书
      st.alignMode = (st.alignMode + 1) % 4;
      g_settings.setString("reader_align", std::to_string(st.alignMode));
      reopenBook();
      st.fullRefresh = true;
      break;
    case MenuAct::Margin:
      st.marginIdx = (st.marginIdx + 1) % 3;
      g_settings.setString("reader_margin", std::to_string(st.marginIdx));
      reopenBook();
      st.fullRefresh = true;
      break;
    case MenuAct::Image:
      st.imageBilinear = !st.imageBilinear;
      ImageBlock::setBilinearScaling(st.imageBilinear);
      g_settings.setString("reader_image_scaling", std::to_string(st.imageBilinear ? 1 : 0));
      reopenBook();
      st.fullRefresh = true;
      break;
    case MenuAct::ReadingLine:
      // 无 → 虚线 → 点线 → 无。只是绘制层的叠加，不动版式：重排一次反而会丢掉
      // 当前页（reopenBook 回到本节开头），所以这里只重画当前页。
      st.readingLine = (clampI(st.readingLine, 0, 3) + 1) % 4;
      g_settings.setString("reader_reading_line", std::to_string(st.readingLine));
      st.fullRefresh = true;
      break;
    case MenuAct::Night:  // 菜单项已移除（夜间在 主界面设置）；保留分支供旧路径兜底
      st.night = !g_settings.nightMode();
      g_settings.setNightMode(st.night);
      display.setInverted(st.night);
      st.fullRefresh = true;
      break;
    case MenuAct::Orient:
      st.orientation = (st.orientation == "portrait") ? "landscape" : "portrait";
      g_settings.setString("reader_orientation", st.orientation);
      applyReaderOrientation();
      reopenBook();
      st.fullRefresh = true;
      break;
    case MenuAct::ToggleBookmark:
      toggleBookmark();
      st.fullRefresh = true;
      break;
    case MenuAct::Bookmarks:
      st.bookmarkSel = 0;
      st.mode = RdMode::Bookmarks;
      st.fullRefresh = true;
      break;
    case MenuAct::Footnotes:
      if (loadCurrentFootnotes()) {
        st.footnoteSel = 0;
        st.mode = RdMode::Footnotes;
        st.fullRefresh = true;
      }
      break;
    case MenuAct::FootnoteBack:
      footnoteReturn();
      break;
    case MenuAct::Percent:
      st.percentVal = totalPages() > 1 ? clampI((curPage() + 1) * 100 / totalPages(), 0, 100) : 50;
      st.mode = RdMode::Percent;
      st.fullRefresh = true;
      break;
    case MenuAct::Qr:
      prepareQr();
      st.mode = RdMode::Qr;
      st.fullRefresh = true;
      break;
    case MenuAct::Dict:
      st.dictQuery.clear();
      st.dictResult.clear();
      st.dictHeadword.clear();
      st.dictStatus.clear();
      rdVkWantShow();
      st.mode = RdMode::Dictionary;
      st.fullRefresh = true;
      break;
    case MenuAct::DictDl:
      st.dictDlSel = 0;
      st.dictDlBusy = false;
      st.dictDlTotal = 0;
      st.dictDlDelArm.clear();
      st.dictDlStatus.clear();
      st.vkVisible = false;
      st.mode = RdMode::DictDl;
      st.fullRefresh = true;
      dictDlLoadCatalog();  // 阻塞拉清单；失败时列表页显示原因，仍可返回
      break;
    case MenuAct::Weread:
      st.vkVisible = false;
      st.weSel = 0;
      st.weScroll = 0;
      st.weStatus.clear();
      st.weShelfLoaded = false;
      // 微读不再占标签位（1 号位给了文件浏览器），只能从「设置」标签进来。**不要**
      // 再设 st.tab=1，否则微读界面画出来的标签栏会高亮"文件"，回退也会走错标签；
      // 保持 st.tab 停在设置（3），handleWeread 的 Esc 借 retMode 也回设置。
      st.mode = RdMode::Weread;
      st.fullRefresh = true;
      break;
    case MenuAct::ResDl:
      st.resField = 0;
      st.resEditing = false;
      st.resBusy = false;
      st.resPct = 0;
      st.resStatus.clear();
      st.resDictEdit = g_settings.getString("dict_manifest_url");
      st.resFontEdit = g_settings.getString("font_dl_url");
      st.vkVisible = false;
      st.mode = RdMode::ResDl;
      st.fullRefresh = true;
      break;
    case MenuAct::Wifi:
      st.wifiField = 0;
      st.wifiEditing = false;
      st.wifiBusy = false;
      st.wifiStatus.clear();
      st.wifiSsidEdit = g_settings.wifiSsid();
      st.wifiPassEdit = g_settings.wifiPassword();
      st.vkVisible = false;
      st.mode = RdMode::Wifi;
      st.fullRefresh = true;
      break;
    case MenuAct::Opds:
      st.opdsEditing = false;
      st.opdsBusy = false;
      st.opdsSel = 0;
      st.opdsStatus.clear();
      st.opdsStack.clear();
      st.opdsEntries.clear();
      st.vkVisible = false;
      st.opdsUrl = g_settings.getString("opds_url");
      st.mode = RdMode::Opds;
      st.fullRefresh = true;
      if (st.opdsUrl.empty()) {
        st.opdsUrlEdit.clear();
        st.opdsEditing = true;
        rdVkWantShow();
      } else if (g_wifi.isConnected()) {
        opdsBeginLoad(st.opdsUrl);
      } else {
        st.opdsStatus = "未连接 WiFi（先到 WiFi 管理连接）";
      }
      break;
    case MenuAct::NetShare:
      st.netSel = 0;
      st.netBusy = false;
      st.netServerUp = file_manager_server_get_port() != 0;
      st.netStatus = st.netServerUp ? ("服务已启动  " + g_wifi.getIp()) : "";
      st.mode = RdMode::NetShare;
      st.fullRefresh = true;
      break;
    case MenuAct::KeyMap:
      st.keyMapSel = 0;
      st.keyMapCapture = -1;
      st.keyMapStatus.clear();
      st.vkVisible = false;
      st.mode = RdMode::KeyMap;
      st.fullRefresh = true;
      break;
    case MenuAct::StatusBar:
      st.sbSel = 0;
      st.vkVisible = false;
      st.mode = RdMode::StatusBar;
      st.fullRefresh = true;
      break;
    case MenuAct::About:
      st.aboutTop = 0;
      st.vkVisible = false;
      st.mode = RdMode::About;
      st.fullRefresh = true;
      break;
    // 轮换制条目：全部改成弹出式选择（openRdPick 弹层，落定后走 applyRdPick）。
    // 弹层比循环好在"有哪些档位、现在是哪档"一眼看清，也不用按好几次才跳到目标档。
    case MenuAct::ShelfStyle:
    case MenuAct::StyleSource:
    case MenuAct::EmbeddedFont:
    case MenuAct::RefreshStrategy:
    case MenuAct::FullEvery:
    case MenuAct::TurnAnim:
    case MenuAct::ClockFace:
    case MenuAct::AutoStandby:
      openRdPick(static_cast<int>(act));
      break;
    case MenuAct::Standby:
      // 立即进入待机：按「设置 → 待机表盘」绘制表盘并 light sleep；任意键唤醒。
      // 未选表盘时仍进入休眠（只打底部提示条），行为与自动休眠一致。
      app_enterStandby();
      st.fullRefresh = true;
      st.dirty = 1;
      break;
    case MenuAct::ToShelf:
      gotoBookshelf();
      break;
    case MenuAct::Back:
      st.mode = RdMode::Reading;
      st.fullRefresh = true;
      break;
  }
}

static void handleMenu(int key) {
  auto items = menuItems();
  int n = (int)items.size();
  if (key == 0x1B) { st.mode = RdMode::Reading; st.fullRefresh = true; st.dirty = 1; return; }
  if (key == KEY_UP) { st.menuSel = std::max(0, st.menuSel - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.menuSel = std::min(n - 1, st.menuSel + 1); st.dirty = 1; return; }
  if (key == KEY_PAGE_UP) { st.menuSel = std::max(0, st.menuSel - 8); st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.menuSel = std::min(n - 1, st.menuSel + 8); st.dirty = 1; return; }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      int top = coverTop();
      int itemH = uiLineHeight() + 12;
      int viewH = statusTop() - top - 8;
      int maxRows = std::max(1, viewH / itemH);
      int start = clampI(st.menuSel - maxRows / 2, 0, std::max(0, n - maxRows));
      int row = start + (y - top) / itemH;
      if (row >= 0 && row < n) st.menuSel = row;
    }
    st.retMode = RdMode::Menu;  // 从这里进的子界面，Esc 回阅读菜单
    doMenuAction(items[st.menuSel].act);
    st.dirty = 1;
    return;
  }
}

// ── 「设置」标签（系统级设置集合）────────────────────────────────────────
// 这些条目原本散落在阅读菜单里，现在统一收到主界面的「设置」标签；阅读菜单只留
// 与「当前这本书」有关的操作。条目全部复用阅读菜单已有的 MenuAct 动作，所以新增
// 系统设置只需在这里加一行 + 在 doMenuAction 里加一个 case（"后续有其它的再添加"）。

// 自动待机档位：关 / 5 / 10 / 15 / 20 分钟。**计时是全局的**（三个模式同一份空闲计时，
// 见 main.cpp 的 checkLightSleep），设置入口放在这里只是因为「待机」相关的都在这一屏。
static const char *kAutoStandbyValues[] = {"0", "5", "10", "15", "20"};
static const char *kAutoStandbyLabels[] = {"关", "5 分钟", "10 分钟", "15 分钟", "20 分钟"};
static const int kAutoStandbyCount = 5;

static std::string autoStandbyLabel(int minutes) {
  for (int i = 0; i < kAutoStandbyCount; i++)
    if (atoi(kAutoStandbyValues[i]) == minutes) return kAutoStandbyLabels[i];
  return kAutoStandbyLabels[2];  // 兜底 = 10 分钟（settings 的默认值）
}

static std::vector<MenuItem> settingsItems() {
  std::vector<MenuItem> m;
  m.push_back({"WiFi 管理", MenuAct::Wifi});
  m.push_back({"WiFi 传书", MenuAct::NetShare});
  // 微信读书原来是主界面 1 号根标签，现在收进设置（和 OPDS 一起算"书的来源"）。
  m.push_back({"微信读书", MenuAct::Weread});
  m.push_back({"OPDS 书库", MenuAct::Opds});
  m.push_back({"词典下载", MenuAct::DictDl});
  m.push_back({"资源下载", MenuAct::ResDl});
  m.push_back({"按键映射", MenuAct::KeyMap});
  m.push_back({"状态栏", MenuAct::StatusBar});
  m.push_back({std::string("书架风格: ") + kShelfStyleNames[static_cast<int>(shelfStyle())], MenuAct::ShelfStyle});
  m.push_back({std::string("样式解析: ") + kStyleSrcNames[styleSource()], MenuAct::StyleSource});
  m.push_back({std::string("内嵌字体: ") + kEmbFontNames[embeddedFontMode()], MenuAct::EmbeddedFont});
  m.push_back({std::string("刷新策略: ") + kRdRefreshNames[refreshStrategy()], MenuAct::RefreshStrategy});
  // 全刷频率：策略选「全局」时它不起作用，标签上直接标出来，免得以为设置了没反应。
  {
    const int every = fullRefreshEvery();
    std::string label = std::string("全刷频率: ");
    if (every == 0) {
      label += kRdFullEveryNames[0];
    } else if (refreshStrategy() == 0) {
      label += "不适用（全局）";   // 标签别太长，竖屏一行放不下
    } else {
      label += std::string("每 ") + std::to_string(every) + " 页";
    }
    m.push_back({label, MenuAct::FullEvery});
  }
  // 错相揭页开关。主界面的「设置 → 显示与版式 → 翻页动画」已经有这一项，但那个入口
  // 要先退出阅读器；挑翻页动画来试的时候人就该在读书的地方，所以在设置标签里再放一份，
  // 两边写同一个键（page_turn_anim），改哪儿都算数。
  m.push_back({std::string("翻页动画: ") + (pageTurnAnimOn() ? "开" : "关"), MenuAct::TurnAnim});
  m.push_back({std::string("阅读器方向: ") + (st.orientation == "portrait" ? "竖屏" : "横屏"), MenuAct::Orient});
  m.push_back({std::string("待机表盘: ") +
                   standbyFaceLabel(standbyFaceFromKey(g_settings.getString("clock_face").c_str())),
               MenuAct::ClockFace});
  m.push_back({std::string("自动待机: ") + autoStandbyLabel(g_settings.autoStandbyMinutes()),
               MenuAct::AutoStandby});
  m.push_back({"待机时钟", MenuAct::Standby});
  m.push_back({"关于本机", MenuAct::About});
  return m;
}

static void renderSettingsTab() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int top = drawTabBar();
  auto items = settingsItems();
  int n = static_cast<int>(items.size());
  int itemH = uiLineHeight() + 12;
  int viewH = tabBottom() - top - 8;
  int maxRows = std::max(1, viewH / itemH);
  int start = clampI(st.setSel - maxRows / 2, 0, std::max(0, n - maxRows));
  for (int i = 0; i < maxRows && start + i < n; i++) {
    int idx = start + i;
    int y = top + i * itemH;
    if (idx == st.setSel) { g_rd.fillRect(0, y, w, itemH, true); drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, items[idx].label.c_str(), false); }
    else drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, items[idx].label.c_str(), true);
  }
  // 标签页不再画底部提示栏（见 tabBottom 的说明），列表一直排到屏幕底边。
}

static void handleSettingsTab(int key) {
  auto items = settingsItems();
  int n = static_cast<int>(items.size());
  // 微读/设置标签里 ←→ 是空的，顺手用来切标签（书架标签要用它们换封面，故不占用）。
  if (key == KEY_LEFT) { switchTab(st.tab - 1); return; }
  if (key == KEY_RIGHT) { switchTab(st.tab + 1); return; }
  if (key == 0x1B || key == KEY_LONG_CONFIRM) { switchTab(0); return; }
  if (key == KEY_UP) { st.setSel = std::max(0, st.setSel - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.setSel = std::min(n - 1, st.setSel + 1); st.dirty = 1; return; }
  if (key == KEY_PAGE_UP) { st.setSel = std::max(0, st.setSel - 8); st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.setSel = std::min(n - 1, st.setSel + 8); st.dirty = 1; return; }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      int t = tabHit(x, y);
      if (t >= 0) { switchTab(t); return; }
      int top = coverTop();
      int itemH = uiLineHeight() + 12;
      int viewH = tabBottom() - top - 8;
      int maxRows = std::max(1, viewH / itemH);
      int start = clampI(st.setSel - maxRows / 2, 0, std::max(0, n - maxRows));
      int row = start + (y - top) / itemH;
      if (row >= 0 && row < n) st.setSel = row;
    }
    st.retMode = RdMode::Settings;  // 从这里进的子界面，Esc 回设置标签
    doMenuAction(items[st.setSel].act);
    st.dirty = 1;
    return;
  }
}

// ── 设置弹层：轮换制条目改成弹出式选择 ──────────────────────────────────
// 几何照写作模式设置的选择器（screen_settings.cpp 的 pickerBoxRect/drawPicker），
// 版式基准换成阅读器自己的 uiLineHeight()/uiAsc()（这套在阅读器里已经统一）。
// 存的是「标签 / 值」两张平行表：值是给 applyRdPick 的不透明串（设置键的原始取值），
// 所以弹层自己不需要知道任何一项的含义，加档位只动 rdPickFill 那一处。
static int rdPickStep() { return uiLineHeight() + 8; }

static void rdPickBox(int *bx, int *by, int *bw, int *bh, int *rows) {
  const int lh = uiLineHeight();
  const int sw = g_rd.getScreenWidth(), sh = g_rd.getScreenHeight();
  int maxRows = (sh - 4 * lh) / rdPickStep();   // 上下各留两行余量
  if (maxRows > 10) maxRows = 10;
  if (maxRows < 3) maxRows = 3;
  const int n = static_cast<int>(st.pickLabels.size());
  int r = n < maxRows ? n : maxRows;
  if (r < 1) r = 1;
  *rows = r;

  int w = g_rd.getTextWidth(uiFontId(), st.pickTitle.c_str()) + 2 * lh;
  for (const auto &s : st.pickLabels) {
    const int tw = g_rd.getTextWidth(uiFontId(), s.c_str()) + 3 * lh;
    if (tw > w) w = tw;
  }
  if (w < 240) w = 240;
  if (w > sw - 24) w = sw - 24;
  *bw = w;
  *bh = 8 + lh + 6 + r * rdPickStep() + 8;
  *bx = (sw - w) / 2;
  *by = (sh - *bh) / 2;
}

// 第 i 行文字的上缘（绘制与命中共用；下面那 8 = 分隔线到首行的间距）。
static int rdPickRowTop(int by, int i) {
  return by + 8 + uiLineHeight() + 8 + i * rdPickStep();
}

// 每个轮换条目的档位表（标签 + 值两张平行表）。
static void rdPickFill(int act) {
  st.pickLabels.clear();
  st.pickValues.clear();
  auto add = [](const char *label, const std::string &value) {
    st.pickLabels.push_back(label);
    st.pickValues.push_back(value);
  };
  switch (static_cast<MenuAct>(act)) {
    case MenuAct::ShelfStyle:
      st.pickTitle = "书架风格";
      for (int i = 0; i < kShelfStyleCount; i++) add(kShelfStyleNames[i], kShelfStyleKeys[i]);
      break;
    case MenuAct::StyleSource:
      st.pickTitle = "样式解析";
      for (int i = 0; i < kStyleSrcCount; i++) add(kStyleSrcNames[i], kStyleSrcKeys[i]);
      break;
    case MenuAct::EmbeddedFont:
      st.pickTitle = "内嵌字体";
      for (int i = 0; i < kEmbFontCount; i++) add(kEmbFontNames[i], kEmbFontKeys[i]);
      break;
    case MenuAct::RefreshStrategy:
      st.pickTitle = "刷新策略";
      for (int i = 0; i < kRdRefreshCount; i++) add(kRdRefreshNames[i], kRdRefreshKeys[i]);
      break;
    case MenuAct::FullEvery:
      st.pickTitle = "全刷频率";
      for (int i = 0; i < kRdFullEveryCount; i++) add(kRdFullEveryNames[i], kRdFullEveryKeys[i]);
      break;
    case MenuAct::TurnAnim:
      st.pickTitle = "翻页动画";
      add("开", "1");
      add("关", "0");
      break;
    case MenuAct::ClockFace:
      st.pickTitle = "待机表盘";
      add(standbyFaceLabel(StandbyFace::Off), standbyFaceKey(StandbyFace::Off));
      add(standbyFaceLabel(StandbyFace::Clock), standbyFaceKey(StandbyFace::Clock));
      add(standbyFaceLabel(StandbyFace::Almanac), standbyFaceKey(StandbyFace::Almanac));
      add(standbyFaceLabel(StandbyFace::Cover), standbyFaceKey(StandbyFace::Cover));
      break;
    case MenuAct::AutoStandby:
      st.pickTitle = "自动待机";
      for (int i = 0; i < kAutoStandbyCount; i++) add(kAutoStandbyLabels[i], kAutoStandbyValues[i]);
      break;
    default:
      st.pickTitle.clear();
      break;
  }
}

// 这一项现在是什么值（用来把弹层的选中行落在当前档上，并画那个 ●）。
static std::string rdPickCurValue(int act) {
  switch (static_cast<MenuAct>(act)) {
    case MenuAct::ShelfStyle: return kShelfStyleKeys[clampI(static_cast<int>(shelfStyle()), 0, kShelfStyleCount - 1)];
    case MenuAct::StyleSource: return kStyleSrcKeys[clampI(styleSource(), 0, kStyleSrcCount - 1)];
    case MenuAct::EmbeddedFont: return kEmbFontKeys[clampI(embeddedFontMode(), 0, kEmbFontCount - 1)];
    case MenuAct::RefreshStrategy: return kRdRefreshKeys[clampI(refreshStrategy(), 0, kRdRefreshCount - 1)];
    case MenuAct::FullEvery: {
      const int v = fullRefreshEvery();
      for (int i = 0; i < kRdFullEveryCount; i++)
        if (kRdFullEveryValues[i] == v) return kRdFullEveryKeys[i];
      return kRdFullEveryKeys[0];
    }
    case MenuAct::TurnAnim: return pageTurnAnimOn() ? "1" : "0";
    case MenuAct::ClockFace:
      return standbyFaceKey(standbyFaceFromKey(g_settings.getString("clock_face").c_str()));
    case MenuAct::AutoStandby: return std::to_string(g_settings.autoStandbyMinutes());
    default: return "";
  }
}

static void openRdPick(int act) {
  st.pickAct = act;
  rdPickFill(act);
  if (st.pickLabels.empty()) { st.pickOpen = false; return; }
  const std::string cur = rdPickCurValue(act);
  int sel = 0;
  for (size_t i = 0; i < st.pickValues.size(); i++)
    if (st.pickValues[i] == cur) { sel = static_cast<int>(i); break; }
  st.pickSel = sel;
  st.pickScroll = 0;
  st.pickOpen = true;
  st.dirty = 1;
}

static void closeRdPick() {
  st.pickOpen = false;
  st.pickAct = -1;
  st.pickTitle.clear();
  st.pickLabels.clear();
  st.pickValues.clear();
  st.pickSel = 0;
  st.pickScroll = 0;
  st.dirty = 1;
}

// 落定后按条目把值写回去。动作照搬原来 doMenuAction 里循环版本的收尾
// （改值 → 落盘 → reopenBook/标全刷），只是现在是"一次跳到目标档"。
static void applyRdPick(int act, const std::string &value) {
  switch (static_cast<MenuAct>(act)) {
    case MenuAct::ShelfStyle:
      // 自动 → 2x2 → 3x3 → 列表；选中项由 st.sel 派生页码，改完不会跑出屏。
      g_settings.setString("reader_shelf_style", value);
      st.fullRefresh = true;
      break;
    case MenuAct::StyleSource:
      // 书籍内嵌 ↔ 强制指定。改完当前这本书立刻重排（对齐/缩进/字号全变）。
      g_settings.setString("reader_style_source", value);
      reopenBook();
      st.fullRefresh = true;
      break;
    case MenuAct::EmbeddedFont:
      // 随书 ↔ 关闭。关掉时当前这本书立刻换回用户字体，打开时立刻换成书里的
      // （reopenBook 走 openEpub → 重新解析/解压/装载，所以在这里先把已装载的
      // 书内字体作废，否则 openEpub 会认为"字面已就位"而跳过）。
      g_settings.setString("reader_embedded_font", value);
      releaseBookFonts();  // 次字面同理：openEpub 会按新设置重装（关了就没有）
      st.bookFontLocal.clear();
      st.bookFontTag = 0;
      reopenBook();
      st.fullRefresh = true;
      break;
    case MenuAct::RefreshStrategy:
      // 只影响阅读页翻页，改完立刻生效。
      g_settings.setString("reader_refresh", value);
      s_pagesSinceFull = 0;
      st.fullRefresh = true;
      break;
    case MenuAct::FullEvery:
      // 计数从改动这一刻重新开始。
      g_settings.setString("reader_full_every", value);
      s_pagesSinceFull = 0;
      st.fullRefresh = true;
      break;
    case MenuAct::TurnAnim: {
      const bool on = (value == "1");
      g_settings.setString("page_turn_anim", on ? "1" : "0");
      rdShowFloat(on ? "翻页动画: 开" : "翻页动画: 关", "错相揭页 · 慢约 1 秒", 1500);
      st.fullRefresh = true;
      break;
    }
    case MenuAct::ClockFace:
      g_settings.setString("clock_face", value);
      st.fullRefresh = true;
      break;
    case MenuAct::AutoStandby:
      g_settings.setString("reader_auto_standby", value);
      rdShowFloat(std::string("自动待机: ") + autoStandbyLabel(atoi(value.c_str())), "", 1500);
      st.fullRefresh = true;
      break;
    default:
      break;
  }
}

// 弹层盖在设置标签上（不换 st.mode），所以在 renderCurrent 的浮层段画。
static void drawSettingPicker() {
  if (!st.pickOpen || st.pickLabels.empty()) return;
  int bx, by, bw, bh, rows;
  rdPickBox(&bx, &by, &bw, &bh, &rows);
  const int n = static_cast<int>(st.pickLabels.size());
  if (st.pickSel < st.pickScroll) st.pickScroll = st.pickSel;
  if (st.pickSel >= st.pickScroll + rows) st.pickScroll = st.pickSel - rows + 1;

  const int lh = uiLineHeight(), asc = uiAsc();
  g_rd.fillRect(bx, by, bw, bh, false);   // 白底：盖住底下的设置列表
  g_rd.drawRect(bx, by, bw, bh, true);    // 黑框
  drawCenteredLine(by + 8, st.pickTitle.c_str(), true);
  const int sepY = by + 8 + lh + 2;
  g_rd.drawLine(bx + 4, sepY, bx + bw - 4, sepY, true);

  const std::string cur = rdPickCurValue(st.pickAct);
  for (int i = 0; i < rows && st.pickScroll + i < n; i++) {
    const int idx = st.pickScroll + i;
    const int top = rdPickRowTop(by, i);
    const bool sel = (idx == st.pickSel);
    // 现用值前面点一个 ●：弹层比循环式好在"有哪些值、现在是哪个"一眼看清。
    const std::string lb = std::string(st.pickValues[idx] == cur ? "\xe2\x97\x8f " : "  ") +
                           fitWidth(st.pickLabels[idx], bw - 3 * lh);
    const int tx = bx + (bw - g_rd.getTextWidth(uiFontId(), lb.c_str())) / 2;
    if (sel) {
      g_rd.fillRect(bx + 4, top - 2, bw - 8, lh + 4, true);
      g_rd.drawText(uiFontId(), tx, top + asc, lb.c_str(), false);
    } else {
      g_rd.drawText(uiFontId(), tx, top + asc, lb.c_str(), true);
    }
  }
}

// 弹层的按键：↑↓/点按移动，回车落定，Esc（含边缘返回手势）/点浮层外取消。
// 模态：这一键一定被吃掉，底下的设置列表看不见它。
static void handleSettingPicker(int key) {
  const int n = static_cast<int>(st.pickLabels.size());
  if (n <= 0) { closeRdPick(); return; }
  int bx, by, bw, bh, rows;
  rdPickBox(&bx, &by, &bw, &bh, &rows);

  if (key == KEY_UP) {
    if (st.pickSel > 0) st.pickSel--;
  } else if (key == KEY_DOWN) {
    if (st.pickSel < n - 1) st.pickSel++;
  } else if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
    st.pickSel = clampI(st.pickSel + (key == KEY_PAGE_DOWN ? rows : -rows), 0, n - 1);
  } else if (key == '\n') {
    int tx, ty;
    if (input_tap_xy(&tx, &ty)) {
      if (tx < bx || tx >= bx + bw || ty < by || ty >= by + bh) {   // 点浮层外 = 取消
        closeRdPick();
        return;
      }
      const int r = (ty - rdPickRowTop(by, 0)) / rdPickStep();
      if (ty < rdPickRowTop(by, 0) || r < 0 || r >= rows || st.pickScroll + r >= n) {
        st.dirty = 1;   // 点在标题/分隔线上：忽略这一次点按
        return;
      }
      st.pickSel = st.pickScroll + r;
    }
    const int act = st.pickAct;
    const std::string value = st.pickValues[st.pickSel];
    closeRdPick();
    applyRdPick(act, value);   // 应用后列表标签变了，closeRdPick 已标脏 → 底图重画
    return;
  } else if (key == 0x1B) {
    closeRdPick();
    return;
  } else {
    return;   // 其余键一概吃掉（模态）
  }
  st.dirty = 1;
}

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

static const int kStatsCards = 6;
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
static std::string rdStatsDate(uint32_t epoch) {
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

// 一排概览卡（cols 由屏宽定），返回卡片区的下一个空位 y。
static int rdStatsCardGrid(int top, const std::vector<std::string> &values,
                           const std::vector<std::string> &labels) {
  const int w = g_rd.getScreenWidth();
  const int cols = (w >= 900) ? 3 : 2;
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
static int rdStatsCardRows() {
  const int cols = (g_rd.getScreenWidth() >= 900) ? 3 : 2;
  return (kStatsCards + cols - 1) / cols;
}
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

static void renderStatsTab() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  int top = drawTabBar();

  const bool goalMet = ReadingStats::todayReadingMs() >= ReadingStats::goalMs();
  std::vector<std::string> values = {
      std::to_string(ReadingStats::currentStreakDays()) + "d", std::to_string(ReadingStats::maxStreakDays()) + "d",
      ReadingStats::formatDurationHm(ReadingStats::todayReadingMs()) + " / " +
          ReadingStats::formatDurationHm(ReadingStats::goalMs()),
      ReadingStats::formatDurationHm(ReadingStats::totalReadingMs()),
      std::to_string(ReadingStats::booksFinished()), std::to_string(ReadingStats::booksStarted())};
  std::vector<std::string> labels = {"连续阅读", "最长连续", goalMet ? "今日 / 目标 ✓" : "今日 / 目标", "阅读总时长",
                                     "读完书籍", "开始书籍"};
  const int listTop = rdStatsCardGrid(top, values, labels) + 6;
  g_rd.drawLine(MARGIN, listTop - 3, w - MARGIN, listTop - 3, true);

  std::vector<RdStatRow> rows;
  rdStatsRows(rows);
  const int n = static_cast<int>(rows.size());
  const int itemH = uiLineHeight() + 12;
  const int maxRows = std::max(1, (tabBottom() - listTop - 6) / itemH);
  const int start = clampI(st.statsSel - maxRows / 2, 0, std::max(0, n - maxRows));
  for (int i = 0; i < maxRows && start + i < n; i++) {
    const RdStatRow &r = rows[start + i];
    const int y = listTop + i * itemH;
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

// 主页里 y 落在哪一行（触摸命中用；与 renderStatsTab 同一套数学）。
static int rdStatsRowAt(int y) {
  std::vector<RdStatRow> rows;
  rdStatsRows(rows);
  const int n = static_cast<int>(rows.size());
  const int itemH = uiLineHeight() + 12;
  const int listTop = rdStatsListTop();
  const int maxRows = std::max(1, (tabBottom() - listTop - 6) / itemH);
  const int start = clampI(st.statsSel - maxRows / 2, 0, std::max(0, n - maxRows));
  if (y < listTop) return -1;
  const int row = start + (y - listTop) / itemH;
  return (row >= 0 && row < n) ? row : -1;
}

static void handleStatsTab(int key) {
  std::vector<RdStatRow> rows;
  rdStatsRows(rows);
  const int n = static_cast<int>(rows.size());
  const int itemH = uiLineHeight() + 12;
  const int listTop = rdStatsListTop();
  const int maxRows = std::max(1, (tabBottom() - listTop - 6) / itemH);

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
  rows.push_back({"阅读次数", std::to_string(b.sessions) + " 次", -1});
  rows.push_back({"上次阅读", b.lastSessionMs > 0 ? ReadingStats::formatDurationHm(b.lastSessionMs) : "—", -1});
  rows.push_back({"预计还需", rdStatsEstimate(b), -1});
  rows.push_back({"状态", b.completed ? "已读完" : "阅读中", -1});
  rows.push_back({"最后阅读", rdStatsDate(b.lastReadAt), -1});
  rows.push_back({"开始 → 读完", (RdTime::clockValid(b.firstReadAt) ? rdStatsDate(b.firstReadAt) : std::string("?")) +
                                      " → " + (b.completedAt ? rdStatsDate(b.completedAt) : std::string("?")) + " ",
                  -1});
}

static void renderStatsBook() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const ReadingBookStats *b = ReadingStats::findBook(st.statsBookPath);
  if (!b) {
    int top = drawTitle("书籍统计");
    drawCenteredLine(top + 20, "这本书已不在统计里");
    return;
  }
  const std::string title = b->title.empty() ? b->path : b->title;
  const int top = drawTitle(fitWidth(title, w - 2 * MARGIN).c_str());

  std::vector<RdKvRow> rows;
  rdStatsBookRows(*b, rows);
  const int n = static_cast<int>(rows.size());
  const int itemH = uiLineHeight() + 12;
  const int maxRows = std::max(1, (statusTop() - top - 8) / itemH);
  const int start = clampI(st.statsTop, 0, std::max(0, n - maxRows));

  for (int i = 0; i < maxRows && start + i < n; i++) {
    const RdKvRow &r = rows[start + i];
    const int y = top + i * itemH;
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

static void handleStatsBook(int key) {
  if (key == 0x1B) { st.mode = st.retMode; st.fullRefresh = true; st.dirty = 1; return; }
  const ReadingBookStats *b = ReadingStats::findBook(st.statsBookPath);
  if (!b) { st.mode = st.retMode; st.dirty = 1; return; }
  int n = 0;
  { std::vector<RdKvRow> rows; rdStatsBookRows(*b, rows); n = static_cast<int>(rows.size()); }
  const int itemH = uiLineHeight() + 12;
  const int maxRows = std::max(1, (statusTop() - coverTop() - 8) / itemH);
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
static void renderStatsMore() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = drawTabBar();
  const uint32_t ref = rdStatsRefOrdinal();

  std::vector<std::string> values = {
      std::to_string(ReadingStats::currentStreakDays()) + "d", std::to_string(ReadingStats::maxStreakDays()) + "d",
      ReadingStats::formatDurationHm(ReadingStats::todayReadingMs()) + " / " +
          ReadingStats::formatDurationHm(ReadingStats::goalMs()),
      ReadingStats::formatDurationHm(ReadingStats::totalReadingMs()),
      std::to_string(ReadingStats::booksFinished()), std::to_string(ReadingStats::booksStarted())};
  std::vector<std::string> labels = {"连续阅读", "最长连续", "今日 / 目标", "阅读总时长", "读完书籍", "开始书籍"};
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
  const int cardRowsFor = (g_rd.getScreenWidth() >= 900) ? 2 : 3;  // 6 张卡
  const int grid1 = cardRowsFor * (kStatsCardH + kStatsCardGap);
  const int grid2 = 1 * (kStatsCardH + kStatsCardGap);  // 2 张区间卡：无论横竖都是 1 行
  const int area = std::max(90, lh * 4);
  return grid1 + 4 + grid2 + 4 + lh + 6 + area + 14 + lh + 6 + area + 8;
}

static void handleStatsMore(int key) {
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

static void renderStatsHeatmap() {
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

static void handleStatsHeatmap(int key) {
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
  return coverTop() + (kStatsCardH + kStatsCardGap) + 6 + uiLineHeight() + 8;
}

static void renderStatsDay() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = drawTitle(("阅读日 " + rdStatsOrdinalLabel(st.statsDay)).c_str());

  const auto books = rdStatsBooksOnDay(st.statsDay);
  std::vector<std::string> values = {ReadingStats::formatDurationHm(rdStatsDayMs(st.statsDay)),
                                     std::to_string(books.size())};
  std::vector<std::string> labels = {"当日合计", "读过的书"};
  int y = rdStatsCardGrid(top, values, labels) + 6;
  drawLineText(MARGIN, y, "当天读过的书", true);
  y = rdStatsDayListTop();

  const int itemH = uiLineHeight() + 14;
  const int maxRows = std::max(1, (statusTop() - y - 6) / itemH);
  const int n = static_cast<int>(books.size());
  const int start = clampI(st.statsTop, 0, std::max(0, n - maxRows));
  if (n == 0) {
    drawCenteredLine(y + 20, "这一天没有阅读记录");
  }
  for (int i = 0; i < maxRows && start + i < n; i++) {
    const RdDayBook &db = books[start + i];
    const int ry = y + i * itemH;
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

static void handleStatsDay(int key) {
  if (key == 0x1B) { st.mode = st.retMode; st.fullRefresh = true; st.dirty = 1; return; }
  const int n = static_cast<int>(rdStatsBooksOnDay(st.statsDay).size());
  const int itemH = uiLineHeight() + 14;
  const int listTop = rdStatsDayListTop();
  const int maxRows = std::max(1, (statusTop() - listTop - 6) / itemH);
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
      if (!books.empty() && y >= listTop) {
        const int row = st.statsTop + (y - listTop) / itemH;
        if (row >= 0 && row < static_cast<int>(books.size())) {
          rdStatsOpenBook(books[row].book->path, RdMode::StatsDay);
          return;
        }
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

static void renderStatsProfile() {
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

static void handleStatsProfile(int key) {
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

static void renderStatsAdjust() {
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

static void handleStatsAdjust(int key) {
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

static void renderStatsSettings() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = drawTitle("统计设置");
  drawLineText(MARGIN, top, "每日目标（达标的天才计入连续与热力图勾选）", true);

  const int cur = g_settings.dailyGoalMinutes();
  const int itemH = uiLineHeight() + 14;
  int y = top + uiLineHeight() + 14;
  for (int i = 0; i < 4; i++) {
    const int ry = y + i * itemH;
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

static void handleStatsSettings(int key) {
  if (key == 0x1B) { st.mode = st.retMode; st.fullRefresh = true; st.dirty = 1; return; }
  const int itemH = uiLineHeight() + 14;
  const int top = coverTop() + uiLineHeight() + 14;
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      const int seg = tabHit(x, y);
      if (seg >= 0) { switchTab(seg); return; }
      if (y >= top) {
        const int row = (y - top) / itemH;
        if (row >= 0 && row < 4) {
          g_settings.setString("daily_goal", std::to_string(kStatsGoals[row]).c_str());
          ReadingStats::save();
          rdShowFloat("每日目标已更新", std::to_string(kStatsGoals[row]) + " 分钟", 3000);
          st.fullRefresh = true;
          st.dirty = 1;
        }
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

static void handleDict(int key) {
  if (key == 0x1B) {
    IME::getInstance().cancelComposition();
    st.vkVisible = false;
    st.mode = RdMode::Menu;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      // 状态栏右端的键盘图标：先判它。它在键盘面板底边之下一点点，面板收起时才露出来；
      // 面板展开时点它则收起键盘（与写作模式键盘的开关同序，见 screen_editor.cpp）。
      if (rdVkIconHit(x, y)) {
        st.vkVisible = !st.vkVisible;
      } else if (y >= dictQueryTop() && y < dictQueryTop() + dictQueryH()) {
        rdVkWantShow();   // 点到查询框：展开键盘（蓝牙键盘连着时给一次浮动提示）
      } else if (st.vkVisible) {
        vkTap(x, y);
      } else {
        doDictLookup();
      }
      st.dirty = 1;
      return;
    }
    vkEnter();  // 非点按回车（BLE/KEY3）
    st.dirty = 1;
    return;
  }
  if (key == 0x08) { feedVkBackspace(); st.dirty = 1; return; }
  if (key >= 0x20 && key <= 0x7E) { feedVkKey(key); st.dirty = 1; return; }
}

// ── 笔记标签 ────────────────────────────────────────────────────────────
// 列表按书分组：一条书名行 + 该书的每条笔记（原文一行、笔记正文一行）。
// 笔记行高两行文字，书名行只有一行，所以行高不固定；滚动按"行号"记。
struct RdNoteRow {
  int noteIdx;   // -1 = 书名分组行
  std::string label;
};

static std::vector<RdNoteRow> rdNoteRows() {
  std::vector<RdNoteRow> rows;
  std::vector<std::string> seen;
  for (int i = 0; i < static_cast<int>(st.notes.size()); i++) {
    std::string bk = st.notes[i].book.empty() ? st.notes[i].path : st.notes[i].book;
    if (bk.empty()) bk = "未知书籍";
    bool first = true;
    for (auto &s : seen) {
      if (s == bk) { first = false; break; }
    }
    if (first) {
      seen.push_back(bk);
      RdNoteRow h;
      h.noteIdx = -1;
      h.label = "《" + bk + "》";
      rows.push_back(h);
    }
    RdNoteRow r;
    r.noteIdx = i;
    r.label = st.notes[i].text;
    rows.push_back(r);
  }
  return rows;
}

static int rdNoteRowH(const RdNoteRow &r) {
  return (r.noteIdx < 0) ? (uiLineHeight() + 10) : (uiLineHeight() * 2 + 8);
}

// 点按落在哪条笔记上（书名行/列表外返回 -1）。滚动状态和渲染时一致。
static int rdNoteRowAtY(int ty) {
  auto rows = rdNoteRows();
  int y = rdBarContentTop();   // 列表从搜索栏下方起排（与 renderNotes 的 top 同源）
  for (int i = st.notesScroll; i < static_cast<int>(rows.size()); i++) {
    int h = rdNoteRowH(rows[i]);
    if (ty >= y && ty < y + h) return rows[i].noteIdx;
    y += h;
    if (y > tabBottom()) break;   // 与 renderNotes 的 bottom 同源：标签页排到物理底边
  }
  return -1;
}

static void rdGotoNote(int idx) {
  if (idx < 0 || idx >= static_cast<int>(st.notes.size())) return;
  const RdState::RdNote &n = st.notes[idx];
  if (n.path.empty()) { st.noteStatus = "该笔记没有关联文件"; st.dirty = 1; return; }
  if (n.path != st.bookPath) {
    std::string low;
    for (char c : n.path) low += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    int kind = 0;
    if (low.size() >= 4 && low.compare(low.size() - 4, 4, ".txt") == 0) kind = 1;
    else if (low.size() >= 4 && low.compare(low.size() - 4, 4, ".xtc") == 0) kind = 2;
    if (!openBook(n.path, kind)) { st.noteStatus = "打不开这本书（文件可能已移走）"; st.dirty = 1; return; }
  }
  if (st.bookKind == 0) {
    if (n.spine != st.spineIndex) openSpine(n.spine);
    buildToPage(n.page);
  } else if (st.bookKind == 1) {
    st.txtPage = clampI(n.page, 0, std::max(0, totalPages() - 1));
  }
  st.selActive = false;
  st.mode = RdMode::Reading;
  st.fullRefresh = true;
  st.noteStatus = "已跳到笔记位置";
  st.dirty = 1;
}

static void renderNotes() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  drawTabBar();
  rdDrawSearchBar(false);   // 笔记搜索栏（暂时只有搜索图标）
  int top = rdBarContentTop();
  auto rows = rdNoteRows();
  if (rows.empty()) {
    drawCenteredLine(top + 40, "还没有笔记");
    drawCenteredLine(top + 40 + uiLineHeight() + 8, "阅读时长按文字即可标注/写笔记");
    return;
  }
  // 选中行：第 st.notesSel 条笔记（不含书名行）。
  int selRow = -1, k = 0;
  for (int i = 0; i < static_cast<int>(rows.size()); i++) {
    if (rows[i].noteIdx < 0) continue;
    if (k == st.notesSel) { selRow = i; break; }
    k++;
  }
  if (selRow < 0) selRow = 0;

  int bottom = tabBottom();
  if (st.notesScroll < 0) st.notesScroll = 0;
  if (st.notesScroll > selRow) st.notesScroll = selRow;
  // 向下滚到选中行完全可见为止（行高不一，只能逐行累加）。
  while (st.notesScroll < selRow) {
    int y = top;
    for (int i = st.notesScroll; i <= selRow; i++) y += rdNoteRowH(rows[i]);
    if (y <= bottom) break;
    st.notesScroll++;
  }

  int y = top;
  for (int i = st.notesScroll; i < static_cast<int>(rows.size()); i++) {
    int h = rdNoteRowH(rows[i]);
    if (y + h > bottom) break;
    if (rows[i].noteIdx < 0) {
      // 分组行就是书名，同样交给用户字体（理由与状态栏一致）。
      drawLineText(MARGIN, y + 4, rows[i].label.c_str(), true, uiFontId());
      g_rd.drawLine(MARGIN, y + h - 3, w - MARGIN, y + h - 3, true);
    } else {
      const RdState::RdNote &n = st.notes[rows[i].noteIdx];
      const bool sel = (i == selRow);
      if (sel) g_rd.fillRect(2, y, w - 4, h - 2, true);
      const int maxW = w - 2 * MARGIN - 12;
      std::string l1 = g_rd.truncatedText(uiFontId(), n.text.c_str(), maxW, EpdFontFamily::REGULAR);
      drawLineText(MARGIN + 10, y + 3, l1.c_str(), !sel, uiFontId());
      std::string l2 = g_rd.truncatedText(uiFontId(), n.note.empty() ? "（仅标注）" : n.note.c_str(),
                                          maxW, EpdFontFamily::REGULAR);
      drawLineText(MARGIN + 10, y + 3 + uiLineHeight() + 2, l2.c_str(), !sel,
                   uiFontId());
    }
    y += h;
  }
  // 标签页没有底部提示栏：二次确认的提示改由 handleNotes 在置位那一下弹浮动框。
}

static void handleNotes(int key) {
  if (key == KEY_LEFT) { switchTab(st.tab - 1); return; }
  if (key == KEY_RIGHT) { switchTab(st.tab + 1); return; }
  if (key == 0x1B) { switchTab(0); return; }
  if (key == KEY_LONG_CONFIRM) { switchTab(0); return; }
  const int n = static_cast<int>(st.notes.size());

  if (key == KEY_UP || key == KEY_DOWN) {
    if (n > 0) {
      st.noteDelArm = false;
      st.notesSel = clampI(st.notesSel + (key == KEY_DOWN ? 1 : -1), 0, n - 1);
    }
    st.dirty = 1;
    return;
  }
  if (key == KEY_TOUCH_LONG) {   // 长按 = 删除（二次确认，误触不至于直接丢笔记）
    if (n > 0) {
      if (st.noteDelArm && st.noteDelIdx == st.notesSel) {
        st.notes.erase(st.notes.begin() + st.notesSel);
        saveNotes();
        st.notesSel = clampI(st.notesSel, 0, std::max(0, static_cast<int>(st.notes.size()) - 1));
        st.noteDelArm = false;
        st.noteStatus = "已删除笔记";
      } else {
        st.noteDelArm = true;
        st.noteDelIdx = st.notesSel;
        rdShowFloat("再按一次删除该笔记", "", 3000);   // 标签页没有提示栏了，确认提示走浮动框
      }
      st.dirty = 1;
    }
    return;
  }
  st.noteDelArm = false;
  if (key == '\n') {
    int x, yy;
    if (input_tap_xy(&x, &yy)) {
      int t = tabHit(x, yy);
      if (t >= 0) { switchTab(t); return; }
      if (rdBarIconHit(x, yy, false) >= 0 || rdBarBodyHit(x, yy, false)) {
        rdEnterSearch(RdMode::NotesSearch);
        return;
      }
      int idx = rdNoteRowAtY(yy);
      if (idx >= 0) { st.notesSel = idx; rdGotoNote(idx); return; }
      st.dirty = 1;
      return;
    }
    rdGotoNote(st.notesSel);
    return;
  }
  st.dirty = 1;
}

// ── 笔记编辑器 ──────────────────────────────────────────────────────────
// 上半屏显示选中的原文，下半屏是笔记正文；蓝牙键盘没连时自动弹虚拟键盘。
static void renderNoteEdit() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  const bool editing = (st.noteEditIdx >= 0 && st.noteEditIdx < static_cast<int>(st.notes.size()));
  int top = drawTitle(editing ? "修改笔记" : "写笔记");
  const int fontId = BODY_FONT_ID_BASE + st.fontLevel;
  const int asc = g_rd.getFontAscenderSize(fontId);
  const int lh = g_rd.getLineHeight(fontId);
  const std::string src = editing ? st.notes[st.noteEditIdx].text : st.notePendingText;

  // 原文（缩进块，最多 3 行，超出截断）
  int y = top;
  auto lines = g_rd.wrappedText(fontId, src.c_str(), w - 2 * MARGIN - 14, 3);
  for (auto &ln : lines) {
    g_rd.drawText(fontId, MARGIN + 14, y + asc, ln.c_str(), true);
    y += lh;
  }
  g_rd.drawLine(MARGIN + 6, top, MARGIN + 6, y, true);

  y += 8;
  g_rd.drawLine(MARGIN, y, w - MARGIN, y, true);
  y += 12;

  const int bottom = st.vkVisible ? (vkVkTop() - 6) : statusTop();
  if (st.noteEditBuf.empty()) {
    drawLineText(MARGIN, y, "点下方键盘输入，或连蓝牙键盘直接打字", true);
  } else {
    auto bl = g_rd.wrappedText(fontId, st.noteEditBuf.c_str(), w - 2 * MARGIN, 128);
    int maxLines = std::max(1, (bottom - y) / lh);
    int start = std::max(0, static_cast<int>(bl.size()) - maxLines);  // 长笔记跟到最后
    for (int i = start; i < static_cast<int>(bl.size()); i++) {
      if (y + lh > bottom) break;
      g_rd.drawText(fontId, MARGIN, y + asc, bl[i].c_str(), true);
      y += lh;
    }
  }
  if (st.vkVisible) drawVk();
  else drawFooter("回车保存   Esc 取消");
  rdDrawVkIcon();
}

static void handleNoteEdit(int key) {
  if (key == 0x1B || key == KEY_LONG_CONFIRM) {   // 取消
    IME::getInstance().cancelComposition();
    st.noteEditBuf.clear();
    st.notePendingText.clear();
    st.noteEditIdx = -1;
    st.vkVisible = false;
    st.mode = RdMode::Reading;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      // 状态栏右端的键盘图标：点一下开/收键盘（收起来之后接着点正文才提交）。
      if (rdVkIconHit(x, y)) { st.vkVisible = !st.vkVisible; st.dirty = 1; return; }
      if (st.vkVisible && y >= vkVkTop()) { vkTap(x, y); st.dirty = 1; return; }
      if (st.vkVisible) { st.dirty = 1; return; }   // 键盘开着时点正文不提交
      rdNoteCommit();
      return;
    }
    if (!st.vkVisible && rdVkWantShow()) { st.dirty = 1; return; }
    rdNoteCommit();
    return;
  }
  if (key == 0x08) { feedVkBackspace(); st.dirty = 1; return; }
  if (key >= 0x20 && key <= 0x7E) { feedVkKey(key); st.dirty = 1; return; }
}

// ── 搜索（书架 / 笔记）──────────────────────────────────────────────────
// 共用一套版式：标题栏 → 查询行（图标+查询串+光标）→ 分隔线 → 命中列表 → 提示行/键盘。
// 匹配逻辑一律"先算命中表，选中项是命中表的下标"，所以边打边筛时列表和选中项永远一致。
static std::string rdLower(const std::string &s) {
  std::string o = s;
  for (char &c : o) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return o;
}

// 查询行的绘制。返回命中列表的起点 y（点按命中按同一公式算行号，两处必须同源）。
static int rdDrawSearchHeader(const char *title, const std::string &q, const char *hint) {
  const int w = g_rd.getScreenWidth();
  const int top = drawTitle(title);
  const int iconPx = uiLineHeight();
  uint8_t *fb = g_rd.getFrameBuffer();
  if (fb) icon_font_draw_sized(fb, MARGIN, top, iconPx, iconPx, TAB_ICON_SEARCH, false, iconPx);
  const int qx = MARGIN + iconPx + 8;
  if (q.empty()) {
    drawLineText(qx, top, hint, true);
  } else {
    drawLineText(qx, top, q.c_str(), true);
    // 光标：查询串后面的竖条，打一个字就往右挪一格（e-ink 上没有其它"活着"的反馈）。
    const int qw = g_rd.getTextWidth(uiFontId(), q.c_str());
    if (qx + qw + 4 < w - MARGIN) g_rd.fillRect(qx + qw + 2, top + 2, 2, uiLineHeight() - 6, true);
  }
  const int sep = top + uiLineHeight() + 6;
  g_rd.drawLine(MARGIN, sep, w - MARGIN, sep, true);
  return top + uiLineHeight() + 12;
}
static int rdSearchListTop() { return coverTop() + uiLineHeight() + 12; }

// ── 书架搜索 ────────────────────────────────────────────────────────────
// 书名子串匹配（ASCII 大小写不敏感，中文不受影响）。空查询=全部命中，这样刚进搜索
// 时列表就是整个书架，删字也能自然回到全集，不会突然空屏。
static std::vector<int> rdShelfHits() {
  std::vector<int> out;
  const std::string q = rdLower(st.shelfQuery);
  for (int i = 0; i < static_cast<int>(st.books.size()); i++) {
    if (q.empty() || rdLower(st.books[i].name).find(q) != std::string::npos) out.push_back(i);
  }
  return out;
}

static void rdShelfSearchOpen() {
  auto hits = rdShelfHits();
  if (hits.empty()) return;
  const int h = clampI(st.searchSel, 0, static_cast<int>(hits.size()) - 1);
  const BookEntry &b = st.books[hits[h]];
  if (openBook(b.path, b.kind)) {
    st.sel = hits[h];   // 书架上的选中项跟着走，Esc 回到书架时停在刚打开的那本
    st.mode = RdMode::Reading;
    st.fullRefresh = true;
    st.dirty = 1;
  } else {
    ESP_LOGE(TAG, "搜索打开失败: %s", b.path.c_str());
  }
}

static void renderShelfSearch() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = rdDrawSearchHeader("搜索书架", st.shelfQuery, "输入书名关键字");
  auto hits = rdShelfHits();
  const int n = static_cast<int>(hits.size());
  const int bottom = st.vkVisible ? (vkVkTop() - 6) : statusTop();
  const int itemH = uiLineHeight() + 6;
  if (n == 0) {
    drawCenteredLine(top + 30, "没有匹配的书");
  } else {
    st.searchSel = clampI(st.searchSel, 0, n - 1);
    const int maxRows = std::max(1, (bottom - top) / itemH);
    const int start = clampI(st.searchSel - maxRows / 2, 0, std::max(0, n - maxRows));
    for (int i = 0; i < maxRows && start + i < n; i++) {
      const int idx = hits[start + i];
      const int y = top + i * itemH;
      const bool sel = (start + i == st.searchSel);
      if (sel) g_rd.fillRect(0, y, w, itemH, true);
      // 命中结果用 UI 字体画（书名可能含 builtin 子集外的字，但这里和书架列表一致，
      // 保持同一套排版；换字体反而会让搜索结果和书架看着不一样）。
      std::string nm = g_rd.truncatedText(uiFontId(), st.books[idx].name.c_str(), w - 2 * MARGIN);
      drawLineText(MARGIN, y + 3, nm.c_str(), !sel);
    }
  }
  if (st.vkVisible) drawVk();
  else drawFooter("↑↓ 选择  Enter 打开  Esc 取消");
  rdDrawVkIcon();
}

static void handleShelfSearch(int key) {
  if (key == 0x1B || key == KEY_LONG_CONFIRM || key == KEY_BACK) {
    IME::getInstance().cancelComposition();
    st.vkVisible = false;
    st.mode = RdMode::Browser;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
    const int n = static_cast<int>(rdShelfHits().size());
    if (n <= 0) return;
    int step = (key == KEY_UP) ? -1 : (key == KEY_DOWN) ? 1 : (key == KEY_PAGE_DOWN ? 8 : -8);
    st.searchSel = clampI(st.searchSel + step, 0, n - 1);
    st.dirty = 1;
    return;
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      if (rdVkIconHit(x, y)) { st.vkVisible = !st.vkVisible; st.dirty = 1; return; }
      if (st.vkVisible && y >= vkVkTop()) { vkTap(x, y); st.dirty = 1; return; }
      // 键盘面板之上（或键盘收起时）点结果行 → 直接打开那一本
      auto hits = rdShelfHits();
      const int n = static_cast<int>(hits.size());
      const int top = rdSearchListTop();
      const int itemH = uiLineHeight() + 6;
      const int bottom = st.vkVisible ? (vkVkTop() - 6) : statusTop();
      const int maxRows = std::max(1, (bottom - top) / itemH);
      const int start = n > 0 ? clampI(st.searchSel - maxRows / 2, 0, std::max(0, n - maxRows)) : 0;
      const int row = start + (y - top) / itemH;
      if (y >= top && row >= 0 && row < n) {
        st.searchSel = row;
        rdShelfSearchOpen();
        return;
      }
      st.dirty = 1;
      return;
    }
    // 非点按回车：蓝牙键盘连上时键盘面板是收起的，这里就是唯一的"确认"。
    if (!st.vkVisible && rdVkWantShow()) { st.dirty = 1; return; }
    rdShelfSearchOpen();
    return;
  }
  if (key == 0x08) { feedVkBackspace(); st.searchSel = 0; st.dirty = 1; return; }
  if (key >= 0x20 && key <= 0x7E) { feedVkKey(key); st.searchSel = 0; st.dirty = 1; return; }
}

// ── 笔记搜索 ────────────────────────────────────────────────────────────
// 匹配原文、笔记正文、书名三处（用户记不住自己搜的是哪一段）。命中直接给笔记下标，
// 打开就复用笔记列表那套 rdGotoNote（跳回书里的位置）。
static std::vector<int> rdNotesHits() {
  std::vector<int> out;
  const std::string q = rdLower(st.notesQuery);
  for (int i = 0; i < static_cast<int>(st.notes.size()); i++) {
    const RdState::RdNote &n = st.notes[i];
    if (q.empty() || rdLower(n.text).find(q) != std::string::npos ||
        rdLower(n.note).find(q) != std::string::npos ||
        rdLower(n.book).find(q) != std::string::npos) {
      out.push_back(i);
    }
  }
  return out;
}

static void rdNotesSearchOpen() {
  auto hits = rdNotesHits();
  if (hits.empty()) return;
  const int h = clampI(st.searchSel, 0, static_cast<int>(hits.size()) - 1);
  st.notesSel = hits[h];   // 笔记列表的选中项也跟着走
  rdGotoNote(hits[h]);
}

static void renderNotesSearch() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = rdDrawSearchHeader("搜索笔记", st.notesQuery, "输入关键字（原文/笔记/书名）");
  auto hits = rdNotesHits();
  const int n = static_cast<int>(hits.size());
  const int bottom = st.vkVisible ? (vkVkTop() - 6) : statusTop();
  const int itemH = uiLineHeight() * 2 + 6;   // 两行：原文 / 《书名》+笔记正文
  if (n == 0) {
    drawCenteredLine(top + 30, "没有匹配的笔记");
  } else {
    st.searchSel = clampI(st.searchSel, 0, n - 1);
    const int maxRows = std::max(1, (bottom - top) / itemH);
    const int start = clampI(st.searchSel - maxRows / 2, 0, std::max(0, n - maxRows));
    const int maxW = w - 2 * MARGIN - 12;
    for (int i = 0; i < maxRows && start + i < n; i++) {
      const RdState::RdNote &nt = st.notes[hits[start + i]];
      const int y = top + i * itemH;
      const bool sel = (start + i == st.searchSel);
      if (sel) g_rd.fillRect(2, y, w - 4, itemH - 2, true);
      std::string l1 = g_rd.truncatedText(uiFontId(), nt.text.c_str(), maxW, EpdFontFamily::REGULAR);
      std::string bk = nt.book.empty() ? nt.path : nt.book;
      std::string l2 = "《" + bk + "》 " + (nt.note.empty() ? std::string("（仅标注）") : nt.note);
      l2 = g_rd.truncatedText(uiFontId(), l2.c_str(), maxW, EpdFontFamily::REGULAR);
      drawLineText(MARGIN + 10, y + 2, l1.c_str(), !sel, uiFontId());
      drawLineText(MARGIN + 10, y + 2 + uiLineHeight() + 2, l2.c_str(), !sel, uiFontId());
    }
  }
  if (st.vkVisible) drawVk();
  else drawFooter("↑↓ 选择  Enter 跳转  Esc 取消");
  rdDrawVkIcon();
}

static void handleNotesSearch(int key) {
  if (key == 0x1B || key == KEY_LONG_CONFIRM || key == KEY_BACK) {
    IME::getInstance().cancelComposition();
    st.vkVisible = false;
    st.mode = RdMode::Notes;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
    const int n = static_cast<int>(rdNotesHits().size());
    if (n <= 0) return;
    int step = (key == KEY_UP) ? -1 : (key == KEY_DOWN) ? 1 : (key == KEY_PAGE_DOWN ? 8 : -8);
    st.searchSel = clampI(st.searchSel + step, 0, n - 1);
    st.dirty = 1;
    return;
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      if (rdVkIconHit(x, y)) { st.vkVisible = !st.vkVisible; st.dirty = 1; return; }
      if (st.vkVisible && y >= vkVkTop()) { vkTap(x, y); st.dirty = 1; return; }
      auto hits = rdNotesHits();
      const int n = static_cast<int>(hits.size());
      const int top = rdSearchListTop();
      const int itemH = uiLineHeight() * 2 + 6;
      const int bottom = st.vkVisible ? (vkVkTop() - 6) : statusTop();
      const int maxRows = std::max(1, (bottom - top) / itemH);
      const int start = n > 0 ? clampI(st.searchSel - maxRows / 2, 0, std::max(0, n - maxRows)) : 0;
      const int row = start + (y - top) / itemH;
      if (y >= top && row >= 0 && row < n) {
        st.searchSel = row;
        rdNotesSearchOpen();
        return;
      }
      st.dirty = 1;
      return;
    }
    if (!st.vkVisible && rdVkWantShow()) { st.dirty = 1; return; }
    rdNotesSearchOpen();
    return;
  }
  if (key == 0x08) { feedVkBackspace(); st.searchSel = 0; st.dirty = 1; return; }
  if (key >= 0x20 && key <= 0x7E) { feedVkKey(key); st.searchSel = 0; st.dirty = 1; return; }
}

// 进搜索：查询串保留（再点一次放大镜能接着改上次的关键字），选中项归零。
static void rdEnterSearch(RdMode m) {
  st.searchSel = 0;
  rdVkWantShow();
  st.mode = m;
  st.fullRefresh = true;
  st.dirty = 1;
}

// ── 模式切换返回点 ──────────────────────────────────────────────────────
// 电源键短按切到写作/计划再切回来时，默认会一路重建成书架首页——正在读的书、翻到第
// 几页、停在哪个标签全丢。这里在退出前记下"当时在哪一屏"，重新进入时回到那一屏。
// 只记能重建的东西（书按路径重开、页按序号重排），所以不影响退出时释放大对象的做法。
// 子界面（菜单/目录/WiFi/OPDS/下载…）一律退回它的入口界面：那些界面的临时状态
// （编辑缓冲、下载进度、URL 栈）本来就不该跨模式存活。
struct RdReturn {
  bool valid = false;
  RdMode mode = RdMode::Browser;
  int tab = 0;
  int sel = 0, setSel = 0, notesSel = 0;
  std::string bookPath;
  int bookKind = -1;
  int spine = 0, page = 0, txtPage = 0, xtcPage = 0;
  std::string fbPath;
  int fbSel = 0;
  int recentSel = 0;
};
static RdReturn s_return;

static void rdSaveReturnPoint() {
  RdReturn r;
  r.tab = st.tab;
  r.sel = st.sel;
  r.setSel = st.setSel;
  r.notesSel = st.notesSel;
  r.bookPath = st.bookPath;
  r.bookKind = st.bookKind;
  r.spine = st.spineIndex;
  r.page = st.page;
  r.txtPage = st.txtPage;
  r.xtcPage = st.xtcPage;
  r.fbPath = st.fbPath;
  r.fbSel = st.fbSel;
  r.recentSel = st.recentSel;

  const bool hasBook = st.bookKind >= 0 && !st.bookPath.empty();
  switch (st.mode) {
    case RdMode::Reading:     r.mode = RdMode::Reading; break;
    // 目录 / 书签 / 百分比定位都是"依附于当前书"的界面：重建路径与阅读页完全相同
    // （重开那本书 + 跳回原页），只是最后停在的界面不同，所以原样记下来。
    // 选中行（tocSel / bookmarkSel）本来就在 st 静态里，退出时没被清，回来直接续上。
    case RdMode::Toc:         r.mode = RdMode::Toc; break;
    case RdMode::Bookmarks:   r.mode = RdMode::Bookmarks; break;
    case RdMode::Percent:     r.mode = RdMode::Percent; break;
    case RdMode::Recent:      r.mode = RdMode::Recent; break;
    case RdMode::FileBrowser: r.mode = RdMode::FileBrowser; break;
    // 四个根标签各有自己的常驻界面，直接按标签回来。
    case RdMode::Browser:
    case RdMode::Weread:
    case RdMode::Notes:
    case RdMode::Settings:    r.mode = tabMode(st.tab); break;
    // 两个搜索界面是书架/笔记的子界面，回来时回各自的列表（查询串本来就留着）。
    case RdMode::ShelfSearch: r.mode = RdMode::Browser; break;
    case RdMode::NotesSearch: r.mode = RdMode::Notes; break;
    // 其余是子界面（书内菜单/弹注/查看器/网络/下载/统计下钻…）：回入口界面——
    // 正在读的书回阅读页，否则回原根标签。这些界面的临时状态（弹注表、下载进度、
    // URL 栈、解码出来的图）本来就不该跨模式存活，退不回去是刻意的。
    default:                  r.mode = hasBook ? RdMode::Reading : tabMode(st.tab); break;
  }
  if (r.mode == RdMode::Reading && !hasBook) r.mode = tabMode(st.tab);
  s_return = r;
  s_return.valid = true;
  ESP_LOGI(TAG, "记录返回点 mode=%d tab=%d book=%s spine=%d page=%d", (int)s_return.mode,
           s_return.tab, s_return.bookPath.empty() ? "-" : s_return.bookPath.c_str(),
           s_return.spine, s_return.page);
}

static void rdRestoreReturnPoint() {
  if (!s_return.valid) return;
  const RdReturn r = s_return;

  // 有书：重开后跳回原来的章节/页。跳页复用书签那套 buildToPage。
  // 目录/书签/百分比定位依附于当前书，走同一条重建路径，只是最后停的界面交给 r.mode。
  const bool needBook = r.mode == RdMode::Reading || r.mode == RdMode::Toc ||
                        r.mode == RdMode::Bookmarks || r.mode == RdMode::Percent;
  if (needBook && r.bookKind >= 0 && !r.bookPath.empty() &&
      openBook(r.bookPath, r.bookKind)) {
    if (r.bookKind == 0) {
      if (r.spine != st.spineIndex) openSpine(r.spine);
      buildToPage(r.page);
      // 目录选中行夹一下界就够（条目本身按书重建，行号在原书里没变）。
      if (st.epub) st.tocSel = clampI(st.tocSel, 0, std::max(0, st.epub->getTocItemsCount() - 1));
      st.bookmarkSel = clampI(st.bookmarkSel, 0,
                              std::max(0, static_cast<int>(st.bookmarks.size()) - 1));
    } else if (r.bookKind == 1) {
      st.txtPage = clampI(r.txtPage, 0, std::max(0, totalPages() - 1));
    } else if (r.bookKind == 2) {
      st.xtcPage = clampI(r.xtcPage, 0,
                          st.xtc ? static_cast<int>(st.xtc->getPageCount()) - 1 : 0);
    }
    st.mode = r.mode;   // Reading / Toc / Bookmarks / Percent
    st.fullRefresh = true;
    st.dirty = 1;
    ESP_LOGI(TAG, "返回界面 %d: %s spine=%d page=%d", (int)st.mode, r.bookPath.c_str(),
             st.spineIndex, st.page);
    return;   // 打开失败（文件被删/损坏）就往下走，退回书架，别卡住进入阅读模式
  }

  if (r.mode == RdMode::Recent) {
    st.recentSel = clampI(r.recentSel, 0, std::max(0, static_cast<int>(st.recent.size()) - 1));
    st.mode = RdMode::Recent;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (r.mode == RdMode::FileBrowser) {
    st.tab = 1;   // 文件浏览 = 1 号根标签
    fbScan(r.fbPath.empty() ? std::string("/sdcard") : r.fbPath);
    st.fbSel = clampI(r.fbSel, 0, std::max(0, static_cast<int>(st.fbEntries.size()) - 1));
    st.mode = RdMode::FileBrowser;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }

  st.tab = clampI(r.tab, 0, kTabCount - 1);
  st.sel = clampI(r.sel, 0, std::max(0, static_cast<int>(st.books.size()) - 1));
  st.setSel = r.setSel;
  st.notesSel = r.notesSel;
  st.mode = tabMode(st.tab);
  st.fullRefresh = true;
  st.dirty = 1;
  // 恢复到文件标签时必须重扫目录：进程重启后 st.fbEntries 是空的，只把 mode 设成
  // FileBrowser 会得到一张空列表（详见 rdEnterFileTab 的注释）。
  if (st.tab == 1) rdEnterFileTab();
  ESP_LOGI(TAG, "返回标签 %d，选中 %d", st.tab, st.sel);
}

// ── 公开入口 ────────────────────────────────────────────────────────────

void screen_reader_init() {
  // 阅读器整条绘制路径**绕过** core1 的渲染任务：GfxRenderer 画的就是 epdiy 的
  // front_fb，刷屏直接调 epd_hl_update_screen（都是本任务、同步）。所以进阅读器前
  // 必须等在飞的那一帧推完 —— 否则 core0 一边画 front_fb、core1 一边在
  // epd_hl_update_* 里读它，既会撕裂画面，也会踩坏 epdiy 的单份全局 render_context。
  ui_render_drain();

  // 阅读器方向：默认横屏 1216×684，可在菜单里切竖屏；退出时 board_restore_orientation
  // 恢复全局方向设置。applyReaderOrientation 同时设 epd 旋转与渲染器方向标志。
  st.orientation = g_settings.getString("reader_orientation", "landscape");
  if (st.orientation != "portrait" && st.orientation != "landscape") st.orientation = "landscape";

  // 关键：g_rd.begin() 在 LANDSCAPE 旋转下捕获 panelWidth/panelHeight（物理 1216×684），
  // 方向标志据此映射逻辑宽高。若在竖屏下 begin()，panelWidth/Height 会互换、逻辑尺寸错乱
  // （内容只画在半屏）。所以先强制横屏再 begin()，之后 applyReaderOrientation 才切方向。
  board_force_landscape();

  // 确保 .crossmux 缓存目录存在：Epub::setupCacheDir() 建每本书的子目录，
  // 父目录缺失时写 book.bin/spine.bin 会失败导致无法打开书。
  bool mkdirOk = Storage.mkdir(CACHE_DIR, true);
  ESP_LOGI(TAG, "缓存目录 %s mkdir=%d exists=%d", CACHE_DIR, (int)mkdirOk, (int)Storage.exists(CACHE_DIR));

  // 显示 HAL + 渲染器初始化（display 由 main 经 crossmux_platform_set_display 注入）。
  g_rd.begin();
  applyReaderOrientation();

  // UI 字体（菜单/书架/词典）跟随写作模式共享字号；正文字体 5 档独立。
  // 像素高 ≈ 字号 * 2.1：22pt→46 / 20pt→42 / 18pt→38。阅读器的整套界面版式都从
  // uiLineHeight() 派（footerH/statusTop/coverTop/drawTitle），所以这一个值定了全缩放。
  const int uiFs = g_settings.fontSize();
  int uiPx = uiFs >= 22 ? 46 : (uiFs >= 20 ? 42 : 38);
  g_uiFont.set(uiPx, uiPx * 4 / 5);
  g_rd.insertFont(UI_FONT_ID, g_uiFont.family);
  g_rd.insertFont(CONTENT_UI_FONT_ID, g_uiFont.family);  // 度量同 UI，角色为内容
  for (int i = 0; i < kBodyPxCount; i++) {
    g_bodyFont[i].set(kBodyPx[i], kBodyPx[i] * 4 / 5);
    g_rd.insertFont(BODY_FONT_ID_BASE + i, g_bodyFont[i].family);
  }

  // 恢复阅读设定（与写作模式共享 settings 存储，键名带 reader_ 前缀）。
  // 字号档换过表：26/30/34px 三档删掉后，老下标在新表里是另一个大小（老 6=64px 会变
  // 成新 6 → 越界夹到 3=64px 恰好对，但老 4=46px 会变成 64px）。所以换 _v2 键并平移：
  // 新档 = 老档 - 3，像素大小保持不变。老键迁移后擦掉，只做一次。
  {
    const std::string lvl = g_settings.getString("reader_font_level_v2");
    if (!lvl.empty()) {
      st.fontLevel = clampI(atoi(lvl.c_str()), 0, kUserFontLevels - 1);
    } else {
      const std::string legacy = g_settings.getString("reader_font_level");
      st.fontLevel = legacy.empty()
          ? kDefaultFontLevel
          : clampI(atoi(legacy.c_str()) - 3, 0, kUserFontLevels - 1);
      g_settings.setString("reader_font_level_v2", std::to_string(st.fontLevel));
      g_settings.erase("reader_font_level");
    }
  }

  // BLE 按键映射表（<SD>/settings/bt_keymap），供本模式的键改道用。
  bleKeymapLoad();
  st.lineSpacing = static_cast<float>(atof(g_settings.getString("reader_line_spacing", "1.2").c_str()));
  st.lineSpacing = kLineSpacings[spacingIdx()];  // 把历史浮点值吸附到档位
  st.paraSpacing = clampI(atoi(g_settings.getString("reader_para_spacing", "0").c_str()), 0, 5);
  st.indentMode = clampI(atoi(g_settings.getString("reader_indent", "0").c_str()), 0, 2);
  st.alignMode = clampI(atoi(g_settings.getString("reader_align", "0").c_str()), 0, 3);
  st.marginIdx = clampI(atoi(g_settings.getString("reader_margin", "1").c_str()), 0, 2);
  st.readingLine = clampI(atoi(g_settings.getString("reader_reading_line", "0").c_str()), 0, 3);
  st.imageBilinear = g_settings.getString("reader_image_scaling", "1") != "0";
  ImageBlock::setBilinearScaling(st.imageBilinear);
  st.night = g_settings.nightMode();  // 全设备夜间（旧键 reader_night 由访问器迁移）
  display.setInverted(st.night);      // 进阅读模式时套用一次，保证与其他界面同向
  loadBookmarks();
  loadRecent();
  loadNotes();
  // 阅读统计：一开机读一次。统计文件在 SD 根（和 reader_progress.txt/reader_notes.txt
  // 放一起），时钟没同步时只有总时长、没有日桶（见 reading_stats.cpp 的 clockValid）。
  ReadingStats::load();
  st.fbPath = "/sdcard";

  st.mode = RdMode::Browser;
  st.tab = 0;
  st.retMode = RdMode::Menu;
  st.shelfDelArm = false;
  st.dirty = 1;
  st.fullRefresh = true;
  st.sel = 0;
  st.wifiField = 0;
  st.wifiEditing = false;
  st.wifiBusy = false;
  st.wifiSsidEdit.clear();
  st.wifiPassEdit.clear();
  st.wifiStatus.clear();
  st.opdsUrl = g_settings.getString("opds_url");
  st.opdsUrlEdit.clear();
  st.opdsEntries.clear();
  st.opdsStack.clear();
  st.opdsSel = 0;
  st.opdsEditing = false;
  st.opdsBusy = false;
  st.opdsStatus.clear();
  st.netSel = 0;
  st.netBusy = false;
  st.netServerUp = false;
  st.netStatus.clear();
  st.floatMsg.clear();
  st.floatSub.clear();
  st.floatUntilUs = 0;
  st.dictOpen = false;
  st.vkVisible = false;
  st.bookKind = -1;
  st.bookPath.clear();
  st.epub.reset();
  st.section.reset();
  st.xtc.reset();
  st.txtUtf8.clear();
  st.txtLineStarts.clear();
  st.txtChapterOffsets.clear();
  st.txtChapterTitles.clear();
  scanBooks();
  // 从其它模式切回来时回到切换出去前那一屏（书的位置一起带回）。开机首次进入时
  // s_return 无效，保持上面的"书架首页"默认。
  rdRestoreReturnPoint();
  ESP_LOGI(TAG, "阅读模式初始化: %d 本书, ui=%dpx, body=%dpx, 行距档=%d, 缩进=%d",
           (int)st.books.size(), uiPx, kBodyPx[st.fontLevel],
           spacingIdx(), st.indentMode);
  renderCurrent();
}

void screen_reader_exit() {
  ESP_LOGI(TAG, "阅读模式退出");
  // 退出前强制记一次：下面马上要把书对象释放掉，之后就没得记了。同一次开机内切走
  // 再切回来靠 s_return，这次落盘是为了掉电/重启后还能接上。
  rdRememberProgress(true);
  // 统计收尾：结束会话（补记最后一段时间）再落盘。放在这里是因为下面就要把书对象
  // 释放掉了——不过统计的数据都是值拷贝，不引用书对象，顺序上只是"别漏了"。
  ReadingStats::endSession();
  ReadingStats::save();
  // 脚注弹注是"阅读页之上的一层"：换模式出去就得收掉，下次进来 st 里不留残影。
  st.fnPopNum.clear();
  st.fnPopText.clear();
  st.fnPopScroll = 0;
  // 设置弹层同理（它也是静态 st 上的一层）。
  st.pickOpen = false;
  st.pickAct = -1;
  st.pickLabels.clear();
  st.pickValues.clear();
  // 刷新侧的跨屏状态一并作废：下次进来重新建立基准、重新记章节。
  s_chapterKnown = false;
  s_prevSampleValid = false;
  s_ghostAccum = 0;
  s_pagesSinceFull = 0;
  s_pendingTurn = 0;
  // 错相揭页的 37KB 相位表只在阅读时按需分配，退出时还回 PSRAM。
  // 留在原地也不会坏（下次进来会复用），但写作模式正缺 PSRAM。
  reader_release_page_turn();
  // 先记下"现在在哪一屏"，下面就要把书和列表都释放掉了。
  rdSaveReturnPoint();
  // 恢复全局方向设置（写作模式可能为竖屏），由 main 的 ui_invalidate_snapshot
  // 触发整屏重刷。
  board_restore_orientation();
  IME::getInstance().cancelComposition();
  // 内容面还给用户字体：字体引擎是写作模式共用的，留着上一本书的内嵌字面，
  // 写作界面会整片变成那本书的字形。放在释放书对象之前更保险。
  applyUserContentFont();
  releaseBookFonts();
  st.bookFontLocal.clear();
  st.bookFontTag = 0;
  // 释放大对象，避免占用 PSRAM 影响写作模式。
  // 插图提取回调是裸 ctx 指向 st.epub 的，必须先解除注册再销毁那本书。
  ImageBlock::setExtractor(nullptr, nullptr);
  ImageBlock::releaseRenderCache();
  st.epub.reset();
  st.section.reset();
  st.xtc.reset();
  st.txtUtf8.clear();
  st.txtUtf8.shrink_to_fit();
  st.txtLineStarts.clear();
  st.txtLineStarts.shrink_to_fit();
  st.txtChapterOffsets.clear();
  st.txtChapterOffsets.shrink_to_fit();
  st.txtChapterTitles.clear();
  st.txtChapterTitles.shrink_to_fit();
  st.books.clear();
  st.books.shrink_to_fit();
  // 若在本模式里起过“WiFi 传书”服务，退出时关掉，避免端口残留。
  if (st.netServerUp) {
    file_manager_server_stop();
    st.netServerUp = false;
  }
  st.opdsEntries.clear();
  st.opdsEntries.shrink_to_fit();
  st.opdsStack.clear();
  st.opdsStack.shrink_to_fit();
}

AppState screen_reader_handle(int key, ScreenContext &ctx) {
  (void)ctx;

  // ── 临时帧探针（定位"翻页后 ~8s 无响应"，定位到就删） ──────────────────
  // 把一次 dispatch 切成 前置/分发/渲染/进度/统计 五段，任一段超阈值就整行打出，
  // 这样"卡在哪一段"是量出来的。stage 变量在下面各段之间赋值。
  const int64_t rdT0 = esp_timer_get_time();
  int64_t rdTa = rdT0, rdTb = rdT0, rdTc = rdT0, rdTd = rdT0;

  // 微信读书任务在跑时独占输入：空转 tick（key==0）推进状态机，其余按键里
  // 只有 Esc/长按有意义（取消）。状态机是同步阻塞的——这一步进去前先把画面
  // 刷出去，用户看到的是上一拍画好的进度。
  if (st.weOp && st.weOp->active()) {
    // 先刷出上一拍画面（renderCurrent 自己清 dirty），再进这一拍的阻塞请求；
    // 这一拍产生的新画面留到下一拍开头刷——每拍恰好一次刷新。
    if (st.dirty) renderCurrent();
    if (key == 0x1B || key == KEY_LONG_CONFIRM || key == KEY_BACK) st.weOp->cancel();
    weDrive();
    // 任务在这一拍结束（完成/失败/取消）：收尾画面必须马上刷出来，因为下一拍
    // 就不再进这个分支了。
    if (st.dirty && !(st.weOp && st.weOp->active())) renderCurrent();
    return APP_READER;
  }

  // 打字界面（笔记/词典/WiFi/OPDS 地址）才开系统输入法。放在 key==0 空转之前，
  // 这样键盘上的中/英标签在第一次按键前就是对的。
  rdSyncImeActive();

  if (key == 0) {
    // 空闲帧：浮动提示到点就清掉。这里必须**当场重绘**——空闲帧没有按键，主循环不会
    // 替我们推屏，只标脏的话那个框会一直挂在屏幕上直到用户再按一下。
    if (!st.floatMsg.empty() && esp_timer_get_time() >= st.floatUntilUs) {
      st.floatMsg.clear();
      st.floatSub.clear();
      st.dirty = 1;
      renderCurrent();
    }
    // 统计的会话心跳/检查点落盘全挂在这里（空闲帧是它唯一的节拍源）。
    rdStatsIdleTick();
    // 排版余量也挂这里：空闲帧是"读者在看书、没按任何键"的唯一节拍源，正好用来
    // 把排版推到当前页前面 kPrebuildAhead 页，翻页那一拍就只剩取页+绘制+推屏。
    rdPrebuildAhead();
    return APP_READER;
  }

  // 晃动机身 = 一次全刷（hw/input.cpp 读加速度计判定，见那边的 shake_poll）。
  // **所有阅读器子界面通用**：只置"下一帧走全刷档"这一对标记，画面由各子界面自己
  // 照常重绘——这样清的是残影，不会把用户正在看的界面/浮层状态清掉。原来这个动作
  // 挂在"长按中间确认键"上，那个键位现在改成待机了。
  if (key == KEY_SHAKE) {
    st.fullRefresh = true;
    st.dirty = 1;
    key = 0;
  }

  // 屏幕边缘向中间横划 = 返回（hw/input.cpp 产生 KEY_BACK，主循环原样放行到这里）。
  // **阅读页且没有任何浮层**（没有选区标注面板、没弹菜单）→ 直接回书架，这是用户
  // 要的语义；书本对象不释放，回书架再点开还是这一本。其余情况（弹了菜单/目录/
  // 书签/设置等子界面，或阅读页正开着选区）一律按 Esc 退一层——**不能**换成
  // KEY_LONG_CONFIRM，它在阅读器里的语义是「删词典 / 解绑按键 / 删统计里的书」，
  // 会误删（其余场合它已经被 main.cpp 拿去当"待机"了）。
  if (key == KEY_BACK) {
    // 脚注弹注开着时它也算"一层浮层"：划回来先收起弹注（转成 Esc，由弹注自己吃），
    // 而不是一路滑回书架。
    if (st.mode == RdMode::Reading && !st.selActive && st.fnPopNum.empty()) {
      gotoBookshelf();
      key = 0;      // 不再往下分发（下一次空转 tick 正常走书架的重绘）
    } else {
      key = 0x1B;
    }
  }

  // BLE 按键映射：把绑定的源键改道成对应动作键。只在阅读模式内做，写作模式不受影响
  // （写作模式的键分发在 main.cpp，不经过这里）。
  // 跳过两种情形：①「按键映射」界面本身要拿到原始键码（捕获/重绑都需要）；
  // ②虚拟键盘/OPDS 编辑中——那些状态需要原样的 ASCII 输入，不能被改道。
  if (st.mode != RdMode::KeyMap && !st.vkVisible && !st.opdsEditing) {
    int t = bleKeymapTranslate(key);
    if (t) key = t;
  }

  // 电容键的"哪一侧"→ 本模式的动作。**阅读页**要的是"右侧键=下一页、左侧键=上一页"
  // （用户点名的顺序），和全设备的"右侧键=上移、左侧键=下移"正好相反，所以 main.cpp
  // 没有提前翻，把"哪一侧"一路带到这里由阅读页自己判；其余界面（目录/菜单/设置/
  // 脚注弹注/选区微调…）照旧翻回 KEY_UP/KEY_DOWN，跟别处的列表一致，不动。
  // 逐层判定用阅读页自己那三样状态：有弹注或正拖着选区时键盘归浮层（滚注文/挪端点），
  // 那时也该是标准的 上/下，不翻。
  if (key == KEY_CAP_RIGHT || key == KEY_CAP_LEFT) {
    const bool right = (key == KEY_CAP_RIGHT);
    const bool readingPage = (st.mode == RdMode::Reading && !st.selActive && st.fnPopNum.empty());
    key = readingPage ? (right ? KEY_DOWN : KEY_UP)    // 右侧=下一页 / 左侧=上一页
                      : (right ? KEY_UP : KEY_DOWN);   // 其它界面照旧
  }

  // 触摸长按：阅读页（选词/标注）、笔记列表（删除）、文件浏览器（弹上下文菜单）、
  // 书架（弹出菜单并锁定长按的那一本）要拿到原始长按键——只有原始键才带落点
  // （input_tap_xy），展平成 0x1B 就分不出"长按的是哪一本"了。其余界面保持
  // "长按=返回"的老语义。文件菜单/重命名/详情这三个界面里的长按仍按 Esc 处理
  // （取消），所以只在 FileBrowser / Browser 这两处放行。
  if (key == KEY_TOUCH_LONG && st.mode != RdMode::Reading && st.mode != RdMode::Notes &&
      st.mode != RdMode::FileBrowser && st.mode != RdMode::Browser && st.mode != RdMode::Stats)
    key = 0x1B;

  // 虚拟键盘候选行左右划翻页。放在分发之前：起点落在候选行的横滑是"划候选"，
  // 这个 KEY_LEFT/RIGHT 归键盘，不能落进下面的阅读页/标签页动作里。
  // input_press_xy() 只在触摸手势抬手的那一帧有值，BLE 键盘的左右键读不到，不会误吃。
  if (key == KEY_LEFT || key == KEY_RIGHT) {
    int px = 0, py = 0;
    if (input_press_xy(&px, &py) && rdVkSwipePage(px, py, key == KEY_RIGHT ? +1 : -1)) {
      st.dirty = 1;
      renderCurrent();
      return APP_READER;
    }
  }

  // T9 候选面板里的上下滑：左列滚读音、宫格翻候选页。同样放在分发之前——起点落在
  // 面板上的竖滑是"滚面板"，这个键不能落进下面的翻页动作里。
  // 认 KEY_UP/DOWN 而不是 KEY_PAGE_UP/DOWN：触摸竖滑本来是后者，主循环(main.cpp)
  // 会把非设置/计划界面的 PAGE 键回退成单步 UP/DOWN，走到这儿已经是 KEY_UP/DOWN。
  if (key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
    int px = 0, py = 0;
    const bool next = (key == KEY_DOWN || key == KEY_PAGE_DOWN);
    if (input_press_xy(&px, &py) &&
        rdVkSwipeScroll(px, py, next ? +1 : -1)) {
      // 面板整块换内容，和点开面板/换读音一样走全刷（局刷的残影在宫格线上最明显）。
      st.fullRefresh = true;
      st.dirty = 1;
      renderCurrent();
      return APP_READER;
    }
  }

  // 触摸上下滑的翻页键（KEY_PAGE_UP/DOWN，主循环不再替阅读模式回退成单步）：
  // 目录和**书架**认它——目录几千章、书架一屏四本（横屏十二本），"一格一格选"或
  // "一行一行挪"都走不动，上下滑要整页翻。其余子界面（正文/菜单/词典/书签/笔记…）
  // 保持原来的单步上下语义。
  if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
    if (st.mode != RdMode::Toc && st.mode != RdMode::Browser)
      key = (key == KEY_PAGE_UP) ? KEY_UP : KEY_DOWN;
  }

  // 网络传输心跳（按键帧和空闲帧都过这里）。放分发之前：它只置 st.dirty，不碰键。
  rdNetTick();

  rdTa = esp_timer_get_time();   // 探针：前置段结束

  // 统计：记下分发前的模式。这一键哪怕把界面切走了（打开菜单/目录），
  // 按下它的那一刻人还在阅读页，也算一次阅读交互。
  const RdMode statsModeBefore = st.mode;

  // 设置弹层是模态的：它盖在设置标签上，这一键只喂给它，底下的模式看不见。
  if (st.pickOpen) {
    handleSettingPicker(key);
  } else switch (st.mode) {
    case RdMode::Browser: handleBrowser(key); break;
    case RdMode::Reading: handleReading(key); break;
    case RdMode::Toc: handleToc(key); break;
    case RdMode::Menu: handleMenu(key); break;
    case RdMode::Bookmarks: handleBookmarks(key); break;
    case RdMode::Footnotes: handleFootnotes(key); break;
    case RdMode::Percent: handlePercent(key); break;
    case RdMode::Qr: handleQr(key); break;
    case RdMode::Dictionary: handleDict(key); break;
    case RdMode::Weread: handleWeread(key); break;
    case RdMode::WereadQr: handleWereadQr(key); break;
    case RdMode::WereadMenu: handleWereadMenu(key); break;
    case RdMode::WereadDl: handleWereadDl(key); break;
    case RdMode::ShelfSearch: handleShelfSearch(key); break;
    case RdMode::NotesSearch: handleNotesSearch(key); break;
    case RdMode::Wifi: handleWifi(key); break;
    case RdMode::ShelfMenu: handleShelfMenu(key); break;
    case RdMode::ShelfInfo: handleShelfInfo(key); break;
    case RdMode::Recent: handleRecent(key); break;
    case RdMode::FileBrowser: handleFileBrowser(key); break;
    case RdMode::FileMenu: handleFileMenu(key); break;
    case RdMode::FileRename: handleFileRename(key); break;
    case RdMode::FileInfo: handleFileInfo(key); break;
    case RdMode::Image: handleImage(key); break;
    case RdMode::Opds: handleOpds(key); break;
    case RdMode::NetShare: handleNetShare(key); break;
    case RdMode::DictDl: handleDictDl(key); break;
    case RdMode::ResDl: handleResDl(key); break;
    case RdMode::KeyMap: handleKeyMap(key); break;
    case RdMode::StatusBar: handleStatusBarSet(key); break;
    case RdMode::About: handleAbout(key); break;
    case RdMode::Settings: handleSettingsTab(key); break;
    case RdMode::Notes: handleNotes(key); break;
    case RdMode::NoteEdit: handleNoteEdit(key); break;
    case RdMode::Stats: handleStatsTab(key); break;
    case RdMode::StatsBook: handleStatsBook(key); break;
    case RdMode::StatsMore: handleStatsMore(key); break;
    case RdMode::StatsHeatmap: handleStatsHeatmap(key); break;
    case RdMode::StatsDay: handleStatsDay(key); break;
    case RdMode::StatsProfile: handleStatsProfile(key); break;
    case RdMode::StatsAdjust: handleStatsAdjust(key); break;
    case RdMode::StatsSettings: handleStatsSettings(key); break;
  }

  rdTb = esp_timer_get_time();   // 探针：分发段结束

  if (st.dirty) renderCurrent();
  rdTc = esp_timer_get_time();   // 探针：渲染段结束
  // 阅读页的位置一有变化就落盘（位置没变时 rdRememberProgress 自己会早退，不写 SD）。
  if (st.mode == RdMode::Reading) rdRememberProgress(false);
  rdTd = esp_timer_get_time();   // 探针：进度落盘段结束
  // 统计记时：一次按键只记一笔（翻页/滚动/弹菜单都算），不在 turnBook 里另记。
  if (statsModeBefore == RdMode::Reading) rdStatsNoteActivity();
  // 临时帧探针：任一段超 300ms（正常每段都是 0~几十 ms）就整行打出。
  {
    const int64_t tEnd = esp_timer_get_time();
    if (tEnd - rdT0 > 300000)
      ESP_LOGW(TAG, "分发探针: 前置 %lldms 分发 %lldms 渲染 %lldms 进度 %lldms 统计 %lldms 共 %lldms "
                    "key=%d mode=%d dirty=%d",
               (rdTa - rdT0) / 1000, (rdTb - rdTa) / 1000, (rdTc - rdTb) / 1000,
               (rdTd - rdTc) / 1000, (tEnd - rdTd) / 1000, (tEnd - rdT0) / 1000,
               key, static_cast<int>(statsModeBefore), (int)st.dirty);
  }
  return APP_READER;
}
