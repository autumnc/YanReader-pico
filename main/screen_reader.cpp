// screen_reader.cpp — 阅读模式（阅读器）移植。
//
// 按子应用拆文件（P3b）：微信读书已搬到 screen_reader_weread.cpp，共享的状态量与
// 外壳原语在 screen_reader_internal.h。这个 TU 仍是阅读器本体（书架/翻页/目录/笔记/
// 词典/设置/统计/文件管理）——往下拆的下一块见 docs/架构评审-2026-10-04.md 的 P3b。
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
#include "screen_reader_internal.h"  // RdState/共享原语（P3b 拆文件后的内部层）
#include "reader_page_turn.h"  // 揭页提示：只有裸声明，不会拖进 epdiy.h
#include "reader_refresh_bridge.h"  // 白底纪律记账 / 自检页自推屏（同样只有裸声明）
#include "ui_render.h"   // ui_render_drain：进阅读器前等在飞的 UI 推屏收尾
#include "ui_helpers.h"  // drawIMEUI / imeBarPanelH：实体键盘打字时的输入法条（见 drawRdImeBar）
#include "usb_msc.h"     // U盘模式：SD 卡整卡经 TinyUSB MSC 暴露给电脑

#include <algorithm>  // std::sort（导出标注按时序排）
#include <cmath>  // 阅读档案的雷达图（cos/sin）
#include <ctime>  // 导出文件名的年月日时分秒
#include <atomic>  // s_sbPending：网页线程投递"设为待机画面"，主任务取件

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
// 待机「图片」表盘：把用户选的 JPG/PNG 解成 Gray8 BMP 缓存。走的是封面那条
// "文件→文件"的流式转换器（不是 ImageDecoderFactory 那条"直接铺 framebuffer"的路），
// 因为缓存要留到待机那一刻用，那时书对象/渲染器都不在手上。
#include <JpegToBmpConverter.h>
#include <PngToBmpConverter.h>

#include "qrcodegen.h"

#include "bt_keyboard.h"
#include "editor_vk.h"  // 虚拟键盘：写作/计划/阅读三个模式共用同一套（drawVk/vkTap 是薄适配）
#include "icon_font.h"  // 标签栏/搜索入口图标（NF-Propo 子集）
#include "u8g2_shim.h"  // u8g2_set_fb：把 shim 钉回阅读器直画的 front_fb（见 rdPinShimFb）
#include "tab_icons.h"
#include "reading_stats.h"  // 阅读统计数据层（计时/落盘/查询），界面见文件末尾统计区
#include "settings_manager.h"
#include "font_store.h"  // font_store_set_path

// 字体模块接口：现在可以直接包含真头了。P1.3 之后 ttf_font.h 不再 #include epdiy.h
// （它把唯一依赖的 enum EpdFontFlags 降级成了 int），所以 epd_internals.h 的 EpdFont
// 不会再被拖进来跟本 TU 里 crossmux 的 EpdFont class 撞名（同本文件开头的说明）。
// 以前这里整段照抄接口，连 ttf_font_item_t 都字段级复制了一份 ——
// 漂移已经发生了：照抄的 ttf_font_open 返回 int，真头返回 esp_err_t。
#include "font/ttf_font.h"
#include "wifi_manager.h"
#include "opds_client.h"
#include "dictionary_store.h"
#include "ble_keymap.h"
#include "pcf85063.h"  // g_rtc：状态栏时钟
#include "file_manager_server.h"
#include "standby_clock.h"
#include "clipboard.h"  // 选区「复制」：跨模式粘贴板（写作/阅读/计划共享一份）
#include "ime/IME.h"
#include "ui/ime_field.h"  // 输入框绑定：落串/退格/光标的 UTF-8 算术（与写作/计划共用一份）
#include "ui/list_view.h"  // 列表选择/滚动/命中（选中项夹边界、窗口公式、tap→行号）
#include "hw/input.h"
#include "hw/board.h"

#include <dirent.h>
#include <sys/stat.h>

#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <algorithm>
#include <map>
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

// 微信读书（WeReadClient/WeReadStore）已随子应用拆到 screen_reader_weread.cpp：
// 本文件只剩分发里那几行 case（renderWeread/handleWeread…），类型经
// screen_reader_internal.h 可见。

static const char *TAG = "Reader";

// ── 小工具 ──────────────────────────────────────────────────────────────
int clampI(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
int utf8Len(unsigned char c) {
  if (c < 0x80) return 1;
  if (c < 0xE0) return 2;
  if (c < 0xF0) return 3;
  return 4;
}
bool endsWith(const std::string &s, const char *suf) {
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

GfxRenderer g_rd(display);
static RdFont g_uiFont;       // id 0
static RdFont g_bodyFont[9];  // id 1..9

RdState st;

// 阅读器**外壳**（菜单/对话框/列表/状态栏/按钮/文件管理器…）用的字体 id。
//
// 平时是 uiFontId() —— 走内容面拿用户所选字体的字形，繁体/生僻字才不是豆腐块。
// 但「书内嵌字体」模式会把**内容面**换成这本书自己的字面，而那种字面是按本书正文子集化
// 出来的：正文里的字它都有，外壳的字（目录/书签/排版/返回/第 N 页…）它多半没有 ——
// 一开菜单就是一排缺字。所以内嵌字面在位时，外壳退回内置 builtin.ttf（7710 字，
// 与设置/GTD/写作等其它界面同一套），内嵌字面只负责正文。
// 判据用 st.bookFontLocal：它非空 ⇔ 内容面此刻装的正是这本书的字面（装载成功才赋值）。
int uiFontId() { return st.bookFontLocal.empty() ? CONTENT_UI_FONT_ID : UI_FONT_ID; }

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

int uiLineHeight() { return g_rd.getLineHeight(UI_FONT_ID); }
int uiAsc() { return g_rd.getFontAscenderSize(UI_FONT_ID); }

// 底部状态栏/提示栏高度与上边界。留 RD_BOTTOM_INSET 底部物理留白，
// 避免提示文字贴到屏幕底边被边框遮挡（原版固定 44px 时文字基线落到底边被截断）。
int footerH() { return uiLineHeight() + 8; }
int statusTop() { return g_rd.getScreenHeight() - footerH() - RD_BOTTOM_INSET; }

// 根标签页（书架/文件/笔记/设置/统计）**不再画底部提示栏**，内容一直排到物理底边。
// 阅读页的状态带、目录/菜单/词典的提示行都还在用 statusTop()——那是各子界面的排版基准，
// 全局改它会连环炸，所以这里只给标签页单开一个底边，两者互不影响。
int tabBottom() { return g_rd.getScreenHeight() - RD_BOTTOM_INSET; }

// 顶栏底边（分隔线所在）。文字标题栏和图标标签栏共用同一条线，所以 coverTop()
// 只有一个值，两种顶栏可以互换着用。取两者里更靠下的那个：标签图标（56px）比
// UI 行高（42px）大，靠这条把正文起点一起往下让，图标才不会被分隔线切到。
int rdHeadBottom() {
  return std::max(RD_TOP_INSET + uiLineHeight() + 6, TAB_BAND_BOTTOM);
}

// 以 top 为文字上缘画一行 UI 文本（drawText 的 y 是基线）。
// 默认实参（black=true / fontId=uiFontId()）在 screen_reader_internal.h 里给，
// 这里不能再写一遍——C++ 只认一次。
void drawLineText(int x, int top, const char *s, bool black, int fontId) {
  g_rd.drawText(fontId, x, top + g_rd.getFontAscenderSize(fontId), s, black);
}

// 居中一行 UI 文本。（默认实参在 screen_reader_internal.h。）
void drawCenteredLine(int top, const char *s, bool black) {
  int x = (g_rd.getScreenWidth() - g_rd.getTextWidth(uiFontId(), s)) / 2;
  drawLineText(x, top, s, black);
}

std::string fitWidth(const std::string &s, int maxW);  // 定义在文件后段（stats TU 也调）
std::string humanSize(long long bytes);                       // 同上（文件浏览器/详情页报大小）

// 打开大书前先刷一帧"正在…"（定义在 renderCurrent 之后）。最近阅读/文件浏览器这两个
// 打开入口都在文件前段，所以声明要放在这里。
void rdShowBusy(const char *msg, const std::string &sub);

// 挂起的弹注（锚点还没排到）每空闲帧推进一步：接着排、排到了就弹出来。
// 定义在 openFootnotePopup 之后；rdPrebuildAhead（文件前段）是它的唯一调用者。

// 瞬时浮动提示：居中黑底反白框，ms 毫秒后自己消失。只置状态 + 标脏，不立刻重绘
// （调用点都在按键处理里，随后 renderCurrent 那一趟就画出来了）。
void rdShowFloat(const std::string &msg, const std::string &sub, int ms);

// 通用弹层选择（「设置」标签 + 阅读菜单的字号/字体都走它）：openRdPick 由 doMenuAction
// 的轮换条目调（那些条目不再原地循环，改成弹层里挑），drawSettingPicker 由 renderCurrent
// 的浮层段画（对所有 RdMode 生效，所以阅读菜单上也能盖），按键由 screen_reader_handle
// 在分发前先喂给 handleSettingPicker。三处都在文件后段。
static void openRdPick(int act);
static void drawSettingPicker();
static void handleSettingPicker(int key);

// 「应用」标签（1 号位）的图标入口页。渲染/按键定义在文件后段（设置标签那一节之后），
// 这里先声明，供 renderCurrent / screen_reader_handle 的分派用。
static void renderApps();
static void handleApps(int key);

// 「想要虚拟键盘」的统一入口：点输入框、进编辑态、按确认键想唤出键盘，全走这里。
//
// 这几处以前写的是光秃秃的 `st.vkVisible = !g_bt.isConnected();` —— 蓝牙键盘一连上，
// "点输入框一点反应都没有"。中间改成一版"不弹 + 浮动提示指路"，但只要用户找不着/点不中
// 那个小图标，症状与之前一模一样（本轮报障就是）。现在一律弹，蓝牙键盘连着时附一条
// 提示（只出一次，断开重连会重新计）说明：键盘能用、右下角的图标能收起。
//
// 返回 true = 键盘已展开（调用方标脏重绘即可）。恒为 true，保留返回值是为了调用点
// 不用改（有"弹出来了才 return"的写法，见 handleNoteEdit / handleShelfSearch）。
bool rdVkWantShow() {
  static bool hinted = false;
  // 蓝牙键盘连着时**照样弹**（2026-10-05 改）。原来是学写作模式那条"有物理键盘就别
  // 弹"的规矩，可写作模式的编辑器有**实体键盘的输入法条**（drawIMEUI，编码行+候选行
  // 一直画着），阅读模式没有——于是"点了输入框什么都没发生"，用户只能看见一行小字
  // 让他去点右下角那个小图标。这个函数本来也只由"明确想在这儿打字"的手势调用
  // （点输入框 / 进搜索 / 确认键唤键盘），弹出来是符合预期的：面板上的候选条还能点
  // 字，蓝牙键盘同时照常可用，不想要就点同一个图标收回去（面板不盖那一行）。
  if (g_bt.isConnected()) {
    if (!hinted) {
      hinted = true;
      rdShowFloat("蓝牙键盘已连接", "虚拟键盘照常可用；点右下角的键盘图标可收起", 4000);
    }
  } else {
    hinted = false;
  }
  // 从"没弹"变成"弹着"也是一次整块换图样（同 rdToggleVk 的理由）：面板顶上来、
  // 正文重排，置一次全刷。已经弹着时不置 —— 这个入口在每次点输入框时都会被调用。
  if (!st.vkVisible) st.fullRefresh = true;
  st.vkVisible = true;
  return true;
}

// 底部提示行。
// 右端要给虚拟键盘的开关图标让位（editorVkDrawIcon 画在同一行的最右端）。短提示照旧
// 整屏居中——文字在中间，本来就够不到极右端的图标；只有长到真会压上去时，才按让位后
// 的宽度截断并在这段里重新居中，免得白截掉半句话。
void drawFooter(const char *hint) {
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
int drawTitle(const char *title) {
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

// 「应用」标签的图标：3×3 九宫格。和统计的柱状图一样是程序化画的 —— 图标子集里没有
// md-apps / md-view-grid 这类码点，而当前环境装不了 fontTools 裁新字形（见 tab_icons.h）。
// 九宫格是"应用抽屉"的通用写法，扫一眼就知道"东西都收在这儿"。
static void drawAppsTabIcon(int x, int y, int box, bool invert) {
  const int gap = box / 10;               // 格间距
  const int cell = (box - 2 * gap) / 3;   // 单格边长
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      g_rd.fillRect(x + c * (cell + gap), y + r * (cell + gap), cell, cell, !invert);
    }
  }
}

// 「字典」入口的图标：一本摊开的书（外框 + 书脊 + 三行字）。同样没有现成字形可用
// （子集里没有 md-book-search / md-dictionary），照上面两枚的先例程序化画。
// 尺寸全按 box 等比推，和旁边两枚真字形图标排在一行不会显得矮一截。
static void drawDictAppIcon(int x, int y, int box, bool invert) {
  const bool fg = !invert;      // 选中时整枚反白（黑底上的白书）
  const int pad = box / 8;
  const int w = box - 2 * pad;
  const int h = (box * 3) / 4;  // 书是扁的：高三 quarter、宽整幅
  const int top = y + (box - h) / 2;
  const int t = std::max(2, box / 20);   // 线宽
  // 四条边（画成外框）
  g_rd.fillRect(x + pad, top, w, t, fg);
  g_rd.fillRect(x + pad, top + h - t, w, t, fg);
  g_rd.fillRect(x + pad, top, t, h, fg);
  g_rd.fillRect(x + pad + w - t, top, t, h, fg);
  // 书脊（靠左一竖）+ 右边三行"字"
  const int spineX = x + pad + w / 4;
  g_rd.fillRect(spineX, top, t, h, fg);
  for (int i = 0; i < 3; i++) {
    const int ly = top + h / 2 - box / 8 + i * (box / 6);
    g_rd.fillRect(spineX + 2 * t, ly, w - w / 4 - 4 * t, std::max(2, t / 2), fg);
  }
}

// ── 主界面标签栏（书架 / 应用 / 笔记 / 设置 / 统计）──────────────────────
// 刻意与 drawTitle 占用同一条顶栏、返回同一个正文 top（都取 rdHeadBottom()），
// 标签化不需要另做一套内容区排版。
int drawTabBar() {
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
    } else if (kTabIcons[i] == TAB_ICON_APPS_SENTINEL) {
      drawAppsTabIcon(ix, TAB_ICON_INSET, TAB_ICON_PX, i == st.tab);
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
int tabHit(int x, int y) {
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
// 文件标签的进入动作（重扫目录）现在定义在 screen_reader_files.cpp，声明见内部头。

// 切根标签。1 号位（应用）以前在这一处特判成"进文件浏览器 + 扫 SD"，现在它就是个普通的
// 图标入口页 —— 扫 SD 那一步挪到「文件管理」那枚图标按下时（rdEnterFileTab），
// 于是五个标签走同一条路，切标签不再有副作用。
void switchTab(int tab) {
  tab = clampI(tab, 0, kTabCount - 1);
  if (tab == st.tab && st.mode == tabMode(tab)) return;
  st.pickOpen = false;   // 切标签一定收起设置弹层（防它被带进别的标签）
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
  const char *path = font_store_get_path();
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

// 书架排序（按最近阅读）。定义在后面（要用 reader_progress.txt 的进度表），这里先声明，
// 因为 scanBooks() 扫完就要用它把列表排好。
static void rdSortShelfByRecency();

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

// ── 书架空闲预建的状态（实现见 fdShelfIdlePrebuild，几百行之后）──────────────
// 放在这里而不是实现旁边：scanBooks() 得能把"全都建好了"这条结论作废，而它在前面。
// 三件事：用户最后一次按键的时刻（空闲窗口的锚）、这个窗口是否已经做过一本、
// 以及"书架上一本待建的都没有了"。
static int64_t s_rdLastInputUs = 0;
static bool s_rdSawKey = false;          // 本次开机进阅读器后有没有过按键（见 kShelfIdleColdUs）
static int64_t s_rdPrebuiltForInputUs = -1;
static bool s_rdShelfPrebuildExhausted = false;

void scanBooks() {
  st.books.clear();
  s_rdShelfPrebuildExhausted = false;   // 书目变了，重新找一遍待建的书
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
  rdSortShelfByRecency();   // 越近读的排越前面
}

// ── 打开书籍 ────────────────────────────────────────────────────────────

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

bool openSpine(int idx) {
  if (!st.epub) return false;
  int n = st.epub->getSpineItemsCount();
  if (n <= 0) return false;
  idx = clampI(idx, 0, n - 1);
  st.spineIndex = idx;
  st.section = std::make_unique<Section>(st.epub, idx, g_rd);
  // 换了 Section 就是换了排版，脚注表缓存的键（书/章/页/字号）虽然够用，但 Section
  // 重建后页内容可能整个变了而键恰好没变（比如改了行距又翻回同一页），这里一并清掉。
  st.footnoteCacheSpine = st.footnoteCachePage = st.footnoteCacheFont = -1;
  st.footnoteCacheBook.clear();
  // 页信息缓存（章/页/脚注/书签偏移）同理，而且**只在这里**作废：它的键是 (章, 页)，
  // 同一个章号换一份 Section 就是换了一套排版，键可能恰好没变。任何新建 Section 的
  // 路都必须过这里（openBook / 翻章 / 链接跳章 / reopenBook 重排都过）。
  st.pageInfoSpine = -1;
  st.pageInfoPage = -1;
  // 字号梯子必须在 startBuild 之前灌：排版期就要按它把 CSS font-size 吸附到某一档。
  applyCssFontLadder();
  if (!st.section->startBuild(makeSpec())) return false;
  st.page = 0;
  st.section->buildSomeMore(2);  // 先排出前几页，立即可读
  return true;
}

// 打开 spine 并落到它最后一页（回翻上一章、跳书末用）。openSpine 之后只排出了前几页
// （增量排版），pageCount 是水位不是章总页数 —— 直接 pageCount-1 会落在章首附近，而不是
// 末页。这里排完整章再取末页；buildSomeMore 有 32KB 源字节上界所以要循环到 isBuildComplete，
// 让出 CPU 的写法和 buildToPage 同一套。回翻跨章 / 跳书末是低频操作，一次排完可接受。
static bool openSpineLast(int idx) {
  if (!openSpine(idx)) return false;
  if (!st.section) return false;
  while (!st.section->isBuildComplete()) {
    if (!st.section->buildSomeMore(16)) break;  // 出错停在已排到的末尾
    vTaskDelay(1);
  }
  st.page = static_cast<int>(st.section->pageCount) > 0 ? static_cast<int>(st.section->pageCount) - 1 : 0;
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
static void pushRecent(const std::string &path, int kind, const std::string &title);
// 阅读位置落盘/恢复：定义在 buildToPage 之后（恢复要靠它跳页），这里先声明。
static void rdRememberProgress(bool force);
static void rdRestoreProgress();
static void rdStatsBeginSession();  // 定义在 chapterPage() 之后（要用到章节进度）
void buildToPage(int target);  // 同上：reopenBook 重排后要靠它跳回原页

bool openBook(const std::string &path, int kind) {
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

// 待机「书籍封面」表盘用的封面缓存：**原图**按封面框解出来的那一张，与书架那张
// 396×528 的 cover_v2.bmp 分开存。名字带版本（v2）：改了解析口径/框尺寸就改个名字，
// 自然作废重生成，不必写迁移。v1 是"整屏一张封面"那版的框（近整屏），已不用，见
// generateCoverForOpenedBook 里的顺手清理。生成端见 generateCoverForOpenedBook。
// / The standby-face cover: the book's ORIGINAL image decoded to the cover box, cached
// separately from the 396×528 shelf cover. Versioned name so a later change in box or
// decoding invalidates it without a migration. v1 was the full-screen box of the
// cover-only layout and is cleaned up opportunistically.
static std::string standbyCoverPathFor(const std::string &path, int kind) {
  return bookCacheDirFor(path, kind) + "/standby_v2.bmp";
}

// 待机封面表盘的**封面框**（版式的上半部分：封面在上，时刻/日期/书名+进度在下）。
// **生成端（本文件）与绘制端（standby_clock.cpp 的 drawCoverFace）都调这一份**：框只有
// 一个来源，改了不会一边变一边不变 —— 那正是"生成时按 A 尺寸解、画的时候按 B 尺寸又
// 缩一遍"的糊法。
//
// 尺寸口径：左右各留 1/6（框宽 = 屏宽 2/3 ≈ 456@684）、高取 55%（竖屏）；横屏时屏是
// 宽扁的，封面占到 55% 就没地方放下面那三行，改成 45%。456:668 ≈ 0.68，与常见封面
// 的 2:3 几乎一致 —— 框越贴合原图，下采样比越小、糊的越少。
void readerStandbyCoverBox(int &x, int &y, int &w, int &h) {
  const int W = SCREEN_W, H = SCREEN_H;
  const int m = (W < H ? W : H) / 40;   // 短边的 2.5%：面板本身盖边 3~4px，留一点就够
  const bool portrait = W < H;
  x = W / 6;
  y = m;
  w = W - 2 * (W / 6);
  h = portrait ? (H * 55 / 100) : (H * 45 / 100);
}

// 打开书后即时生成封面（用已加载对象，避免二次解压）。
static void rdCoverThumbForget(const std::string &bmpPath);  // 定义在封面缩放那一段
static void generateCoverForOpenedBook() {
  // 顺手清掉两代老封面：改名之后它们再也不会被读到，留着白占卡（每本一两百 KB，
  // 几百本就是几十 MB）。删失败也无所谓，下次打开再试。
  const std::string legacy = bookCacheDirFor(st.bookPath, st.bookKind) + "/cover.bmp";      // 更早的整屏封面
  const std::string legacyStandby = bookCacheDirFor(st.bookPath, st.bookKind) + "/standby_v1.bmp";  // 单图版表盘的封面
  if (Storage.exists(legacy.c_str())) Storage.remove(legacy.c_str());
  if (Storage.exists(legacyStandby.c_str())) Storage.remove(legacyStandby.c_str());
  if (st.bookKind == 0) { if (st.epub) st.epub->generateCoverBmp(); }
  else if (st.bookKind == 1) { Txt t(st.bookPath, CACHE_DIR); if (t.load()) (void)t.generateCoverBmp(); }
  else if (st.bookKind == 2) { if (st.xtc) st.xtc->generateCoverBmp(); }
  // 新封面写完了 → 作废缩略图缓存里这本的旧条目（否则书架上还挂着上一版封面）。
  rdCoverThumbForget(coverBmpPathFor(st.bookPath, st.bookKind));
  // 待机整屏封面**故意不在这里做**：它跟"把这本书打开"没有半点关系，纯粹是待机表盘
  // 的素材，而做它要"解原图 + 缩放 + 写盘"（实测 ~3.0s，开书路径上第三大的一块）。
  // 两个补做的时机见 rdBuildStandbyCoverForOpenBook 的头注释。
}

// 待机整屏封面（「书籍封面」表盘 1:1 上屏的那张 650×1182）。
//
// 为什么不在待机时现做：待机是 light sleep 前的最后一屏，那时书对象已经不在手上
// （screen_reader_exit 会释放），要现做就得重新解压 epub 找封面 —— 而且慢。所以按
// **原图**提前解好，一本书只做一次（文件在就跳过）。
//
// 为什么不用书架那张 cover_v2.bmp：那是 396×528 的格子缩略图，待机整屏的框是
// 650×1182 上下，拿它上屏就是"缩略图放大"——用户报的"待机封面糊"就是它。
//
// XTC 不做：它的 cover_v2.bmp 本来就是第 0 页原分辨率（见 Xtc.cpp 的注释），已经
// 是能拿到的最好一版，readerLastBookCover 会退回用它。
//
// **它不挂在开书路径上**（原来挂，改掉了）：这本书有没有待机封面，跟"用户此刻要
// 不要读它"无关，而 3 秒的开销在开书这一趟是实打实的等待。改由这两个时机补：
//   · 表盘是「书籍封面」→ 首页推上屏后的空闲帧（rdPrebuildAhead 开头那段）；
//   · 用户后来才把表盘切成「书籍封面」→ 设置动的那一下（MenuAct::ClockFace）。
// 没补上的窗口很短：drawCoverFace 会退回书架封面兜底，不会画不出来。
static void rdBuildStandbyCoverForOpenBook() {
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

// ── 待机表盘「图片」：用户自己选的那张图 ─────────────────────────────────
// 与「书籍封面」同一条路子（见 screen_reader.h 的接口说明）：**不**在待机那一刻解码
// 原图，而是选中时就按当前屏尺寸解成一张 Gray8 BMP 缓存，待机只做"读 BMP + 铺屏"。
// 缓存名带屏尺寸 —— 横竖屏的表盘尺寸不同，转了屏自然换一张，转回来还能命中老的。
static std::string standbyImageCacheFor(int w, int h) {
  return std::string(CACHE_DIR) + "/standby/image_" + std::to_string(w) + "x" + std::to_string(h) + ".bmp";
}

bool readerStandbyImage(std::string &bmpPath, std::string &srcPath) {
  srcPath = g_settings.getString("standby_image");
  bmpPath = standbyImageCacheFor(SCREEN_W, SCREEN_H);
  return !srcPath.empty();
}

// 后缀判断（大小写不敏感——相册/网友上传来的图常是 .JPG）。
static bool rdImageExtIs(const std::string &name, const char *ext) {
  const size_t l = strlen(ext);
  if (name.size() <= l) return false;
  const size_t off = name.size() - l;
  for (size_t i = 0; i < l; i++) {
    char a = name[off + i], b = ext[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

// 把 src 解成当前屏尺寸的待机缓存（Gray8 BMP，同封面那套口径）。失败填 err。
static bool rdBuildStandbyImageCache(const std::string &src, std::string &err) {
  err.clear();
  if (src.empty()) { err = "还没选图片"; return false; }
  if (!Storage.exists(src.c_str())) { err = "图片不在卡上"; return false; }
  const bool isPng = rdImageExtIs(src, ".png");
  const bool isJpg = rdImageExtIs(src, ".jpg") || rdImageExtIs(src, ".jpeg");
  if (!isPng && !isJpg) { err = "只支持 JPG / PNG"; return false; }

  // fit（crop=false）+ 整屏盒子：待机要的是"整张图都看得见"，裁掉两边去填满会把
  // 画面切掉一块。留白由绘制端居中处理（readerCoverScale 的 ox/oy）。
  const int w = SCREEN_W, h = SCREEN_H;
  const std::string out = standbyImageCacheFor(w, h);
  Storage.mkdir((std::string(CACHE_DIR) + "/standby").c_str(), true);
  bool ok = false;
  {
    HalFile in, outf;
    if (Storage.openFileForRead(TAG, src, in) && Storage.openFileForWrite(TAG, out, outf)) {
      ok = isPng ? PngToBmpConverter::pngFileToBmpStreamWithSize(in, outf, w, h, /*gray8=*/true, /*crop=*/false)
                 : JpegToBmpConverter::jpegFileToBmpStreamWithSize(in, outf, w, h,
                                                                   JpegToBmpConverter::Output::Gray8, false);
    }
  }
  if (!ok) {
    Storage.remove(out.c_str());   // 半截 BMP 不比没有更安全
    err = "解不开这张图（格式或大小不支持）";
    ESP_LOGW(TAG, "待机图片: 解码失败 %s", src.c_str());
    return false;
  }
  ESP_LOGI(TAG, "待机图片: %s -> %s (%dx%d)", src.c_str(), out.c_str(), w, h);
  return true;
}

bool readerSetStandbyImage(const std::string &srcPath, std::string &err) {
  if (!rdBuildStandbyImageCache(srcPath, err)) return false;
  g_settings.setString("standby_image", srcPath);
  g_settings.setString("clock_face", standbyFaceKey(StandbyFace::Image));
  // 别的屏尺寸的老缓存顺手删掉（每张几百 KB）。当前这张留着——待机就要用它。
  const std::string dir = std::string(CACHE_DIR) + "/standby";
  const std::string keep = standbyImageCacheFor(SCREEN_W, SCREEN_H);
  DIR *dp = opendir(dir.c_str());
  if (dp) {
    struct dirent *e;
    while ((e = readdir(dp)) != nullptr) {
      const std::string nm = e->d_name;
      if (nm == "." || nm == "..") continue;
      const std::string full = dir + "/" + nm;
      if (full != keep) Storage.remove(full.c_str());
    }
    closedir(dp);
  }
  return true;
}

// 「这组 (原图, 屏尺寸) 已经试过了」——成功失败都算，免得失败时每个空闲帧重解一次。
static std::string s_rdSbImageSrc;
static int s_rdSbImageW = 0, s_rdSbImageH = 0;

// 网页（/api/set_standby）那条路**只投递、不落地**。原因是栈：解一张几百万像素的
// JPEG 要十几 KB 栈（stb_image 的霍夫曼/IDCT 那几层），而 httpd 任务只有 8KB（够发
// 分块响应而已，handler_download 里那个 4KB 的 buf 就吃掉一半）。主任务 16KB
// （CONFIG_ESP_MAIN_TASK_STACK_SIZE）才是解图该待的地方 —— 目视浏览器的看图、
// 文件菜单的"设为待机"本来就在那儿解。
// 所以这里存一个"待办"，由主循环每拍调 readerStandbyPump() 取件、在主任务上解。
static std::atomic<bool> s_sbPending{false};
static std::string s_sbPendingSrc;   // 只在 s_sbPending==true 时有意义

void readerRequestStandbyImage(const std::string &srcPath) {
  // 先把设置改了：即便图暂时解不出来，语义（"以后待机看这张"）也已经生效，
  // 表盘那边会画"正在生成"的占位而不是退回上一张。
  g_settings.setString("standby_image", srcPath);
  g_settings.setString("clock_face", standbyFaceKey(StandbyFace::Image));
  s_sbPendingSrc = srcPath;
  s_sbPending.store(true, std::memory_order_release);   // 写在旗子之前，release 保证可见
}

void readerStandbyPump() {
  if (!s_sbPending.load(std::memory_order_acquire)) return;
  s_sbPending.store(false, std::memory_order_relaxed);
  const std::string src = s_sbPendingSrc;   // 先拷一份，别再跨线程读
  if (src.empty() || Storage.exists(standbyImageCacheFor(SCREEN_W, SCREEN_H).c_str())) return;
  std::string err;
  if (rdBuildStandbyImageCache(src, err)) {
    // 告诉空闲补做"这组已经好了"，省得它过 8 秒又解一遍。
    s_rdSbImageSrc = src; s_rdSbImageW = SCREEN_W; s_rdSbImageH = SCREEN_H;
  } else {
    ESP_LOGW(TAG, "网页设为待机画面: %s", err.c_str());
  }
}

// 空闲帧补做：转屏之后、或者从 web 界面（/api/set_standby）设的图，缓存可能还没生成。
// 定义在「书架空闲预建」那一段（要复用那边的"用户停手了多久"这道闸）。
static void rdStandbyImageIdlePrebuild();

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
    if (st.spineIndex > 0) return openSpineLast(st.spineIndex - 1);
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
  if (ok) {
    st.dirty = 1;
    // 「每日页数」只数往后翻（往前翻是回看，来回翻两下页数就假了，时长那栏才管频率）。
    // 这里也是全阅读器唯一的翻页漏斗：按键、电容键、触摸左右三分区、滑动都并进来。
    if (dir > 0) ReadingStats::notePageTurn();
  }
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
int sbCount(const char *key, int def, int n) {
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
//   里都会被调到，于是每翻一页白白多花 7.7 秒。
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

// 「这本书的待机封面已经试过了吗」——见 rdPrebuildAhead 开头那一段。换书时路径变了
// 自然失效，所以不用显式清。
static std::string s_rdSbTriedPath;

static void rdPrebuildAhead() {
  if (st.mode != RdMode::Reading) return;
  // （1）待机整屏封面：表盘是「书籍封面」时它总要生成，但开书那一趟被门控跳过了
  // （开书那一趟故意不做），改到这里补 —— 此时首页已经推上屏、
  // 读者正看着这一页，几秒的后台解码就藏在这段"没人按键"的空档里（跟下面排版
  // 余量同一个道理）。做成**一本书一次**：生成失败（书里没有可用原图）时文件不会
  // 出现，不记一笔的话每个空闲帧都会重试一次整趟解压（~50ms × 每 80ms 一帧）。
  if (!st.bookPath.empty() && st.bookKind != 2 && s_rdSbTriedPath != st.bookPath) {
    if (standbyFaceFromKey(g_settings.getString("clock_face").c_str()) == StandbyFace::Cover &&
        !Storage.exists(standbyCoverPathFor(st.bookPath, st.bookKind).c_str())) {
      s_rdSbTriedPath = st.bookPath;   // 试过就算，成功失败都别再进
      rdBuildStandbyCoverForOpenBook();
    }
  }
  if (st.bookKind != 0 || !st.section) return;
  // 挂起的弹注排在最前面：它要的是**锚点那一页**，可能远在几十页之外（注文常整块压在
  // 章末），比"领先读者 kPrebuildAhead 页"要紧得多。它自己带预算，也在里面把结果弹出来。
  if (st.fnWaitIdx >= 0) rdFootnoteWaitTick();
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

// ── 书架空闲预建：把"第一次打开一本书"的开销提前到空闲帧 ────────────────────
// 冷开一本 EPUB 实测 18.4 秒，可这 18 秒几乎全是一次性产物，而且**只跟这本书的文件
// 内容有关**，跟"用户此刻在读哪本书"毫无关系：
//     book.bin 元数据        ~6.5s  解 zip 里的 OPF/NCX/HTML 建 spine+TOC 索引
//     书内嵌字体解压          ~6.8s  两个 .ttf 从 zip 抠出来落缓存（正文 2.4 + 次字面 4.4）
//     书架封面                ~?     解码原图 + 缩放
// 既然只跟内容有关，就能在书架上、用户没在动的时候先做出来；真正点进去时这些步骤
// 全是 stat 命中，开书只剩"装字体 + 排首章 + 画首屏"（热开实测 3.4s）。
//
// 三条硬规矩：
//   1) **只在书架、且用户停手 kShelfIdleUs 之后**才开工。Epub::load() 内部是不可切
//      分的一整段（expat 解析 zip 里的 XML），一做就是好几秒：放在开书路径里用户
//      盯着"正在打开…"干等，放在空闲帧里用户看到的是书架停在那儿 —— 前者是白等。
//   2) **一个空闲窗口只做一本**。做完这个窗口就作废，要等下一次按键之后的新窗口。
//      宁可少建几本，也不能让"用户这时拿起机器"撞上一个已经排队的下一本。
//   3) 只补**还没有 book.bin** 的书；已有缓存的书开起来本来就快（热开 3.4s），
//      重建它纯属浪费 SD 寿命。
//
// 字体这一步是**只解压不装载**：ttf_font_open 会占用全局的内容面/次字面，那是"正在
// 读的那本书"的东西，后台不能碰（换了字面，正在看的那一页下次重排就变样了）。
// extractEmbeddedFont 只落文件，开书时那段 zip 读取和早退判断就全变成了 stat 命中。
// 这也是原先计划的 ②「内嵌字体懒加载」真正的落点：**懒加载本身不能做** ——
// makeSpec 的 fontTag 里带着次字面指纹、ChapterHtmlSlimParser 又按 altFontFamilyHash
// 给每个词打 ALT_FONT 位，装不装次字面直接改变换行；把"解压"提前则完全等价、且安全。
//
// 反过来说，这条路的**代价**：这几秒在书架上是真占 CPU/SD 的，所以规矩 1、2 必须守。
// 停手多久才算"用户不在跟前，可以开工"。这两个值是拿第一次上机日志校出来的：
// 原来只留 3 秒，结果开机落在书架上、用户一下都没碰，6 秒后就无视一切地做了 13 秒
// —— 而那正是"用户就在机器跟前"的时刻，
// 他这时去点一本**已经缓存过**的书，就要白等这 13 秒。所以：
//   · 用户动过手（在书架里翻过、从书里退出来…）再停手 kShelfIdleUs 才开工；
//   · 一个按键都还没有（刚开机/刚切回书架）则等 kShelfIdleColdUs，默认他马上会动手。
static const int64_t kShelfIdleUs = 8 * 1000 * 1000;
static const int64_t kShelfIdleColdUs = 30 * 1000 * 1000;

// 这本书有没有内嵌正文字体？读 <cache>/book_font.txt（v4：`v4\n<href>\n<size>\n<family>\n…`，
// 主 href 是空行即"没有"），见 Epub::resolveEmbeddedFonts。判不出来（文件不在、版本旧）
// 一律返回 false = 当成"有"：保守，宁可不预建，也不排一份规格必然对不上、开书即作废的 .bin。
static bool bookHasNoEmbeddedFont(const std::string &cacheDir) {
  bool ok = false;
  const std::string cached = Storage.readFile((cacheDir + "/book_font.txt").c_str(), &ok);
  if (!ok || cached.rfind("v4\n", 0) != 0) return false;
  const size_t nl = cached.find('\n', 3);
  return nl != std::string::npos && nl == 3;   // 主 href 是空行
}

static void rdShelfIdlePrebuild() {
  if (st.mode != RdMode::Browser) return;          // 只在书架；读书/别的界面不抢
  if (s_rdShelfPrebuildExhausted) return;          // 没有待建的书（scanBooks 会作废）
  const int64_t now = esp_timer_get_time();
  // 进阅读器后还没有过按键：把这一刻当作"停手的起点"。
  if (s_rdLastInputUs == 0) {
    s_rdLastInputUs = now;
    return;
  }
  const bool sawKey = (s_rdSawKey != 0);
  if (now - s_rdLastInputUs < (sawKey ? kShelfIdleUs : kShelfIdleColdUs)) return;
  if (s_rdPrebuiltForInputUs == s_rdLastInputUs) return;  // 这个空闲窗口已经做过一本

  // 找一本可以预建的 EPUB。只看 EPUB：TXT 没有这张元数据缓存（拿"文件在不在"当
  // "建没建"用会永远找不到，变成每帧重做同一本），XTC 更是一本书就是一个文件。
  // 分两级，优先做第一级：
  //   ① 还没有 book.bin —— 元数据 + 内嵌字体 + 封面一次全做（原来的范围）；
  //   ② 元数据有了、但**首章排版还没落盘** —— 点进去还要干等的正是这一段（解 CSS +
  //      解析 HTML + 逐页断行）。这一级只在"这本书没有内嵌字体"时做：内嵌字体会进
  //      makeSpec 的 fontTag、还按家族哈希给每个词打 ALT_FONT 位，用书架上（没开书，
  //      tag=0）的规格排出来的 .bin，真正打开时规格一比就作废，白排。
  const BookEntry *pick = nullptr;
  bool needMeta = false;
  for (const auto &b : st.books) {
    if (b.kind != 0) continue;
    const std::string dir = bookCacheDirFor(b.path, b.kind);
    if (!Storage.exists((dir + "/book.bin").c_str())) { pick = &b; needMeta = true; break; }
    if (Storage.exists((dir + "/sections/0.bin").c_str())) continue;  // 首章已经排过
    if (!bookHasNoEmbeddedFont(dir)) continue;                        // 有内嵌字体：规格对不上
    pick = &b;
    needMeta = false;
    break;
  }
  if (pick == nullptr) {
    s_rdShelfPrebuildExhausted = true;
    return;
  }
  s_rdPrebuiltForInputUs = s_rdLastInputUs;   // 先标记，免得中途返回时反复挑同一本

  const std::string path = pick->path, name = pick->name;
  const bool hadCover = Storage.exists(coverBmpPathFor(path, 0).c_str());
  ESP_LOGI(TAG, "空闲预建: %s (%s)", name.c_str(), needMeta ? "元数据" : "首章");
  const int64_t t0 = esp_timer_get_time();
  {
    // 一次性把开书路径上那些"只依赖文件内容"的产物做出来。Epub 对象出了这个作用域
    // 就销毁（zip 句柄、解析缓冲都是 PSRAM 上的临时量），开书时再从缓存读。
    // 用 make_shared 而不是栈对象：跟 openEpub 同一个用法，避免把几百字节的句柄
    // 摊在主任务栈上（主任务的栈本来就紧）。
    auto epub = std::make_shared<Epub>(path, CACHE_DIR);
    if (epub->load()) {
      if (needMeta) {
        // resolveEmbeddedFonts 会把结论写进 <cache>/book_font.txt（v4），开书时直接读它，
        // 不用再解一遍 OPF 里的 @font-face。
        const Epub::EmbeddedFontSet fonts = epub->resolveEmbeddedFonts();
        if (!fonts.primary.itemHref.empty()) (void)epub->extractEmbeddedFont(fonts.primary);
        // 次字面的落盘名必须和 loadAltEmbeddedFont 里的一致（book_alt.ttf），否则这里
        // 抠出来的那份开书时用不上，等于白做。
        if (!fonts.alt.itemHref.empty()) (void)epub->extractEmbeddedFont(fonts.alt, "book_alt.ttf");
        (void)epub->generateCoverBmp();
      } else {
        // 只补首章排版。顺序与 openSpine 一致：字号梯子必须在 startBuild 之前灌，
        // 排版期要按它把 CSS font-size 吸附到某一档。这一级不碰封面/字体（上头挑的
        // 时候已经保证没有内嵌字体，book.bin 也在）。
        applyCssFontLadder();
        auto section = std::make_shared<Section>(epub, 0, g_rd);
        if (!section->createSectionFile(makeSpec())) {
          ESP_LOGW(TAG, "空闲预建: %s 首章排版失败", name.c_str());
        }
      }
    } else {
      ESP_LOGW(TAG, "空闲预建: %s 打不开，跳过", name.c_str());
    }
    // 这里就放开：**别**把它带出作用域。这本的封面生成走的是它自己的 zip 句柄，
    // 但打开的书（st.epub）随时可能在换页时读图，两份 zip 同时在手上是没必要的
    // 内存开销（zip 目录 + 解析缓冲都在 PSRAM）。
  }
  ESP_LOGI(TAG, "空闲预建完成: %s 用时 %lldms", name.c_str(),
           (esp_timer_get_time() - t0) / 1000);
  // 封面刚补出来才重画：封面本来就在的话，这一趟只建了元数据/字体，屏幕上没有
  // 任何像素会变，白白走一遍差分刷。
  // 这里必须**当场刷**——空闲帧是主循环唯一不替我们推屏的拍子（同上面浮动提示那条），
  // 只标脏的话封面会一直不出现，直到用户再按一下。
  if (!hadCover && Storage.exists(coverBmpPathFor(path, 0).c_str())) {
    st.dirty = 1;
    renderCurrent();
  }
}

// 待机「图片」表盘的缓存补做（转屏后、或从 web 界面设的图都靠它）。
// 为什么要有它：从网页 /api/set_standby 设的图只写了个路径，本机没人去解码；转了屏
// 之后当前尺寸的缓存也不存在了。这一段跑在**任何**阅读器子界面（不像书架预建只在
// 书架），因为"设完图看文件列表"是最自然的姿势。
// 闸门与书架预建同一套（用户停手 kShelfIdleUs / 冷启动 kShelfIdleColdUs）：解一张
// 大图是秒级的，不能刚按完键就开跑。**一组 (原图, 屏尺寸) 只试一次** —— 失败
// （图不在卡上/解不开）也不每个空闲帧重试。
static void rdStandbyImageIdlePrebuild() {
  if (standbyFaceFromKey(g_settings.getString("clock_face").c_str()) != StandbyFace::Image) return;
  const std::string src = g_settings.getString("standby_image");
  if (src.empty()) return;
  const int w = SCREEN_W, h = SCREEN_H;
  if (src == s_rdSbImageSrc && w == s_rdSbImageW && h == s_rdSbImageH) return;  // 这组看过了
  // 停手闸（与书架预建同一套）。
  const int64_t now = esp_timer_get_time();
  if (s_rdLastInputUs == 0) return;   // 还没有"停手起点"（刚进阅读器）：下一拍再说
  if (now - s_rdLastInputUs < (s_rdSawKey ? kShelfIdleUs : kShelfIdleColdUs)) return;
  s_rdSbImageSrc = src;
  s_rdSbImageW = w;
  s_rdSbImageH = h;
  if (Storage.exists(standbyImageCacheFor(w, h).c_str())) return;   // 已经有缓存
  std::string err;
  rdBuildStandbyImageCache(src, err);
  if (!err.empty()) ESP_LOGW(TAG, "待机图片: 空闲补做失败 %s", err.c_str());
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

// 当前页的文字地图。渲染时采一次，长按命中直接查它，避免再 loadPage 一遍。
RdPageText g_pageText;

static RdPageText rdBuildPageText(const Page &page, int fontId, int xOffset, int yOffset) {
  RdPageText pt;
  pt.valid = true;
  for (const auto &el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto &line = static_cast<const PageLine &>(*el);
    const auto &blk = line.getBlock();
    if (!blk || !blk->valid()) continue;
    pt.lineFirst.push_back(static_cast<int>(pt.words.size()));
    // 与 PageLine::render 逐字对应：el->yPos 是**行顶**，这里的 y 是**基线**
    // （下面全部按 `w.y - asc` / `w.y - asc/2` 取上沿和字形中心）。少了这一跳 ascender，
    // 整块文字地图比实画的高一整个 ascender，长按选词会稳定地选到上一行。
    // 用这一行自己的字体号，和渲染侧同一个来源（CSS 放大过的标题字号不同）。
    const int lineFontId = blk->renderFontId() ? blk->renderFontId() : fontId;
    const int baseY = yOffset + el->yPos + g_rd.getFontAscenderSize(lineFontId);
    for (uint16_t i = 0; i < blk->wordCount(); i++) {
      RdWordHit wh;
      wh.x = xOffset + el->xPos + blk->wordXpos(i);
      wh.style = static_cast<uint8_t>(blk->wordStyle(i));
      wh.y = baseY;
      wh.text = blk->wordText(i);
      // 宽度也按这一行的字体量：渲染用的就是它，用正文号量会让标题行的命中框横向也对不上。
      wh.w = g_rd.getTextWidth(lineFontId, wh.text.c_str(), static_cast<EpdFontFamily::Style>(wh.style));
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
  rdWarmStrings(TTF_ROLE_CONTENT_ALT, alt);
  rdWarmStrings(TTF_ROLE_CONTENT, main);  // 最后把角色留在内容面，跟改之前一致
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
  if (st.section) {
    auto page = st.section->loadPage(st.page);
    // 菜单/书签要的两条派生信息跟着这一页一起采（见 RdState::pageInfo*）：反正页已经读
    // 出来了，offset 也是 loadPage 顺手带出来的（Page::visibleTextOffset），不要再开一次
    // 文件。页没读出来就作废缓存 —— 让 getter 现算，而不是把"没读到"当成"这页没脚注"
    // 记下来（构建中的页正是这种情况，等它排出来自然会被下一次渲染重新采）。
    st.pageInfoSpine = -1;
    st.pageInfoPage = -1;
    if (page) {
      st.pageInfoSpine = st.spineIndex;
      st.pageInfoPage = st.page;
      st.pageInfoFootnotes = !page->footnotes.empty();
      st.pageInfoOffset = page->visibleTextOffset;
      if (page->hasImages()) {
        st.fullRefresh = true;
        st.frameGray = 1;  // 插图页会在面板上留下真中灰（白底纪律记账，见 renderCurrent 尾）
      }
      // 先把文字地图建出来（它只查排版块，与帧缓冲无关），再用它做整页预取，
      // 最后才画。顺序不能反：预取必须整页一次性做，逐词做就没意义了。
      g_pageText = rdBuildPageText(*page, fontId, bodyMargin(), RD_BODY_TOP);
      // 页面链接矩形跟着页一起抄下来：PageLink 存的是页内坐标（解析时已含 leftInset），
      // 与 PageLine 同一套，所以屏幕坐标 = 页内坐标 + 渲染偏移（bodyMargin / RD_BODY_TOP）。
      st.pageLinks.clear();
      for (const auto &lk : page->links) {
        st.pageLinks.push_back({std::string(lk.href), lk.x + bodyMargin(), lk.y + RD_BODY_TOP, lk.width, lk.height});
      }
      rdWarmPageText(g_pageText);
      page->render(g_rd, fontId, bodyMargin(), RD_BODY_TOP);
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
    rdWarmStrings(TTF_ROLE_CONTENT, all);  // TXT 没有样式，只有内容面
  }
  // 3) 画。y 的推进与收集循环一一对应（每个 seg 一行，含空行）。
  for (const auto &seg : segs) {
    if (!seg.empty()) g_rd.drawText(fontId, bodyMargin(), y + asc, seg.c_str(), true);
    y += lh;
  }
  g_pageText = rdBuildPageTextTxt(segs, fontId, bodyMargin(), RD_BODY_TOP, lh);
  rdDrawReadingLines(fontId);  // 与 EPUB 同一条阅读线（TXT 的 y 推进就是标称行距）
  drawReaderStatus();
  rdDrawOverlays(g_pageText, fontId);
}

static void renderXtcPage() {
  g_rd.clearScreen();
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
  int rd = st.xtc->loadPage(st.xtcPage, buf.data(), bufsize);
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
  drawReaderStatus();
}

static void renderReading() {
  if (st.bookKind == 0) renderEpubPage();
  else if (st.bookKind == 1) renderTxtPage();
  else renderXtcPage();
}

// ── 书架封面网格 ─────────────────────────────────────────────────────────
int coverTop() { return rdHeadBottom() + 8; }  // == drawTitle / drawTabBar 返回

// 「标题 → 状态行 → 分隔线 → 列表」这一族的列表首行**上沿**（WiFi / OPDS / 词典下载 /
// 网络文件管理四个子界面）。以前它们是四个同名不同名的函数（wifiListTop / opdsListTop /
// resListTop / netListTop）抄着同一个式子，而各自的渲染函数里还写着 `ly = top + itemH`
// —— 五处必须一直保持同值，否则点按命中会整体错一行。收敛成这一个。
int rdListTop() { return coverTop() + uiLineHeight() + 12; }  // 状态行 + 分隔线

// 平铺列表的通用几何：给定首行上沿与行高，一屏正好装下 count 行（rows == count 表示
// "没有窗口外的东西"）。渲染与点按命中共用这一个调用 —— 行号就只有一处算法。
ListView flatListViewAt(int top, int itemH, int count, int sel) {
  ListView lv;
  lv.top = top;
  lv.itemH = itemH;
  lv.count = count;
  lv.rows = count;  // 平铺：可见行数就是总行数
  lv.sel = sel;
  listViewClamp(lv);
  return lv;
}

// 平铺菜单（文件菜单 / 书架菜单 / 按键映射 / 自定义状态栏…）：行高 12，top 默认在标题
// 栏正下方（coverTop）；带状态行+分隔线的那几屏（WiFi / 结果页）传 rdListTop。
// 默认实参在 screen_reader_internal.h 里给。
ListView flatMenuListView(int count, int sel, int top) {
  return flatListViewAt(top, uiLineHeight() + 12, count, sel);
}

// 「标题（或标签栏）→ 列表」这一族的居中式窗口（最近阅读 / 目录 / 书签 / 脚注 / 阅读菜单 /
// 设置 / 统计首页…）。itemH 各屏不同（+6 / +8 / +12），bottom 传列表区下沿——有底部提示栏
// 的传 statusTop()，一屏到底的标签页传 tabBottom()。渲染与点按命中共用同一个调用。
static ListView titleListView(int count, int sel, int itemH, int bottom, int page = 8) {
  ListView lv;
  lv.top = coverTop();  // == drawTitle / drawTabBar 的返回值
  lv.itemH = itemH;
  lv.count = count;
  lv.rows = std::max(1, (bottom - lv.top - 8) / itemH);
  lv.page = page;
  lv.sel = sel;
  listViewCenter(lv);
  return lv;
}

// ── 顶部搜索栏（书架 / 笔记共用）─────────────────────────────────────────
// 顶标签栏之下一条横栏：左端是占位提示，右端是动作图标（书架 2 个：搜索/刷新；
// 笔记只有搜索，另加一枚「导出」文字按钮）。书籍封面 / 笔记列表都从栏下方起排（rdBarContentTop）。
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
static int rdBarIconCount(bool shelf) { return shelf ? 2 : 1; }
static uint32_t rdBarIconCp(int i, bool shelf) {
  // 微读原来排第三（BAR_ICON_WEREAD），已随书架栏那枚图标一起删掉；那枚字形还在，
  // 现在只给「应用」标签的微读入口用。
  static const uint32_t kShelf[2] = {TAB_ICON_SEARCH, BAR_ICON_REFRESH};
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

// 笔记栏右端除放大镜外还有一个「导出」按钮（导出全部标注/书签，见 rdExportAnnot）。
// 图标字体子集里没有语义合适的导出字形，当前环境也没 fontTools 重裁（同 tab_icons.h
// 顶部那条注释），所以用**文字按钮**，不借一个意思不对的图标。
// 几何只有 rdNotesExportRect 一处，绘制和点按命中都从它取，不会错行。
static constexpr const char *kNotesExportText = "导出";
static int rdNotesExportW() { return g_rd.getTextWidth(uiFontId(), kNotesExportText) + 24; }
static void rdNotesExportRect(int *x, int *y, int *w, int *h) {
  *w = rdNotesExportW();
  *h = rdBarH() - 8;
  *x = rdBarIconX(0, rdBarIconCount(false)) - 12 - *w;
  *y = rdBarTop() + 4;
}
static bool rdNotesExportHit(int tx, int ty) {
  int x, y, w, h;
  rdNotesExportRect(&x, &y, &w, &h);
  // 命中判定放宽一圈：e-ink 没有实时反馈，指尖落点差几个像素很常见（与图标同理）。
  return tx >= x - 6 && tx <= x + w + 6 && ty >= y - 6 && ty <= y + h + 6;
}
static void rdDrawNotesExportBtn() {
  int x, y, w, h;
  rdNotesExportRect(&x, &y, &w, &h);
  g_rd.drawRect(x, y, w, h, true);
  const int tw = g_rd.getTextWidth(uiFontId(), kNotesExportText);
  drawLineText(x + (w - tw) / 2, y + (h - uiLineHeight()) / 2, kNotesExportText, true);
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
  if (!shelf) rdDrawNotesExportBtn();   // 书架栏没有导出：那是"当前这些书"，没有标注可导
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

// 封面这条链（书架缩略图 + 待机封面表盘）的量化档：跟插图/图片查看器共用设置里的
// 「图片抖动」。所以量化落点必须是 grayToLevel16（DitherUtils.h），不能再用
// `(v+8)>>4` —— 后者是"除以 16"的凑合写法，而这块屏的 4bpp 里 level*17 才是灰度，
// 且它没有抖动。换档时 s_coverThumbs 要整体作废（见 applyRdPick 的 ImageDither 分支）。
static DitherMode rdCoverDitherMode() { return ImageBlock::ditherModeEnabled(); }

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
    const DitherMode dm = rdCoverDitherMode();
    DitherRowState ds;
    for (int x = 0; x < dw; x++) {
      // 抖动就落在这里：此时 row[x] 还是 0..255（提对比之后、量化之前），
      // 再往后就没得抖了。put() 逐行、x 从左到右且 y 递增，行扩散档也是对的用法。
      // y 放大时同一 row 会填几个 dy，Ordered 按各自 y 取图案（本来就应该这样）。
      dst[x] = grayToLevel16(row[x], x, y, dm, ds);
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

// 抖动档变了：缓存里的 pix 是按旧档量化好的，留着就是"设置改了但书架不变"。
// 整个清掉 —— 重算一页 12 本约 3 秒，只有换档那一次付。
static void rdCoverThumbClearAll() {
  s_coverThumbs.clear();
  s_coverThumbBytes = 0;
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

// 书架搜索栏右端的动作：0=搜索 1=刷新（重扫书库）。
// 原来第三个是微读，2026-10-05 删了 —— 微读收进 1 号位的「应用」标签之后，书架栏上
// 再挂一枚就是重复入口，而且它挤在搜索框右端容易被误触。
static void rdShelfBarAction(int i) {
  if (i == 0) { rdEnterSearch(RdMode::ShelfSearch); return; }
  if (i == 1) {
    scanBooks();
    st.sel = clampI(st.sel, 0, std::max(0, static_cast<int>(st.books.size()) - 1));
    st.dirty = 1;
    return;
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

// 最近阅读：居中式窗口。渲染与点按命中共用这一个几何（以前各写一遍窗口式子）。
static ListView recentListView() {
  return titleListView(static_cast<int>(st.recent.size()), st.recentSel, uiLineHeight() + 8, statusTop());
}

static void renderRecent() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  drawTitle("最近阅读");
  const ListView lv = recentListView();
  int top = lv.top;
  int n = lv.count;
  if (n == 0) {
    drawCenteredLine(g_rd.getScreenHeight() / 2, "暂无阅读记录");
    drawFooter("Esc 返回");
    return;
  }
  int itemH = lv.itemH;
  int maxRows = lv.rows;
  int start = lv.first;
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
  {  // 上下/翻页的算术在 ui/list_view.h（与 renderRecent 共用同一个 recentListView 几何）
    ListView lv = recentListView();
    if (listViewKey(lv, key)) { st.recentSel = lv.sel; st.dirty = 1; return; }
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      const int row = listViewHitAt(recentListView(), y);
      if (row >= 0) st.recentSel = row;
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
  drawTitle("书架菜单");
  std::vector<std::string> items;
  shelfMenuLabels(items);
  const ListView lv = flatMenuListView(static_cast<int>(items.size()), st.shelfMenuSel);
  int n = lv.count;
  int itemH = lv.itemH;
  for (int i = 0; i < n; i++) {
    int y = lv.top + i * itemH;
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
      // 文件浏览挂在 1 号位（应用标签）下面，走和应用页那枚文件夹图标同一条进入路径
      // （rdEnterFileTab，含目录重扫）。从这儿进去，卡根 Esc 会退到应用页而不是书架。
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
  {  // 上下选择在 ui/list_view.h，与 renderShelfMenu 共用 flatMenuListView 几何
    ListView lv = flatMenuListView(n, st.shelfMenuSel);
    if (listViewKey(lv, key)) { st.shelfDelArm = false; st.shelfMenuSel = lv.sel; st.dirty = 1; return; }
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      const int row = listViewHitAt(flatMenuListView(n, st.shelfMenuSel), y);
      if (row >= 0) {
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
  return titleListView(0, 0, uiLineHeight() + 6, statusTop()).rows;
}

// 目录列表几何（渲染 / 翻页步长 / 点按命中共用）；翻页步长 = 一屏行数。
static ListView tocListView(int count, int sel) {
  return titleListView(count, sel, uiLineHeight() + 6, statusTop(), tocRowsPerPage());
}

static void renderToc() {
  g_rd.clearScreen();
  drawTitle("目录");
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
    const ListView lv = tocListView(static_cast<int>(items.size()), st.tocSel);
    int itemH = lv.itemH;
    int maxRows = lv.rows;
    int start = lv.first;
    // 条目走**内容面**（CONTENT_UI_FONT_ID），不走外壳面。目录条目是书里的字——卷名、
    // 回目、繁体书名——而内嵌字体在位时 uiFontId() 退回的 builtin.ttf 只有常用简繁字，
    // 繁体书一开目录就是一片豆腐块。内容面在内嵌字体在位时是**这本书的字面**，没内嵌
    // 字体时是用户所选字体，两者覆盖都远好于 builtin。
    // id 20 与 id 0 度量完全一致（同一个 g_uiFont.data），所以 itemH/maxRows/缩进全都
    // 不用动，只换字面；抬头「目录」「本书无目录」和底部提示仍是外壳词，照旧走 builtin。
    for (int i = 0; i < maxRows && start + i < static_cast<int>(items.size()); i++) {
      int idx = start + i;
      int y = lv.top + i * itemH;
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

// ── 图片抖动（设置标签 / 阅读菜单的排版设定 → 图片抖动）──────────────────────
// 8 位灰量化成面板的 16 级时用哪种抖动。默认"有序"（Bayer）：它是 (灰度,x,y)
// 的纯函数，同一张图每次重绘逐像素相同 —— 差分刷新不会因为图案漂移而叠影。
// "行扩散"把量化误差沿一行向右传，渐变更细腻，但图案取决于解码出点顺序；
// "关"直接量化，用于对比/排查。档位值进 .pxc 缓存名，换档必然重解码。
static const char *kRdDitherKeys[] = {"ordered", "row", "none"};
static const char *kRdDitherNames[] = {"有序", "行扩散", "关"};
static const int kRdDitherCount = 3;
static int imageDitherIndex() {
  const std::string k = g_settings.getString("reader_image_dither", "ordered");
  for (int i = 0; i < kRdDitherCount; i++) {
    if (k == kRdDitherKeys[i]) return i;
  }
  return 0;  // 有序
}
static DitherMode ditherModeOf(int idx) {
  switch (idx) {
    case 1: return DitherMode::Row;
    case 2: return DitherMode::None;
    default: return DitherMode::Ordered;
  }
}

// ── 阅读菜单：动作 + 动态条目 ───────────────────────────────────────────
enum class MenuAct {
  Toc, Font, FontFamily, LineSpacing, ParaSpacing, Indent, Align, Margin, Image, ImageDither, ReadingLine, Night,
  Orient,
  ToggleBookmark, Bookmarks, Footnotes, FootnoteBack, Percent, Qr, LayoutMenu,
  Dict, DictDl, ResDl, Weread, Wifi, Opds, NetShare, KeyMap, StatusBar, About, RefreshTest, Standby, UsbDrive,
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

// 当前页的派生信息有没有缓存（键 = 章 + 页，见 RdState::pageInfo*）。
// 只有渲染过这一页才有值；没命中就现算，绝不拿别的页的结论冒充。
static bool pageInfoCached() {
  return st.bookKind == 0 && st.section && st.pageInfoSpine == st.spineIndex && st.pageInfoPage == st.page;
}

// epub 当前页可见文本偏移（跨重排的书签标识）。txt/xtc 返回 UINT32_MAX。
static uint32_t currentVisibleOffset() {
  if (st.bookKind == 0 && st.section) {
    // 命中缓存就直接答：这一页渲染时已经从同一个 Page 上拿到了 offset（Page::visibleTextOffset
    // 是 loadPage 顺手带出来的），不必再开一次 section 文件读 offset LUT。
    if (pageInfoCached()) return st.pageInfoOffset;
    auto o = st.section->getVisibleTextOffsetForPage(static_cast<uint16_t>(st.page));
    if (o) return *o;
  }
  return UINT32_MAX;
}

static bool currentPageHasFootnotes() {
  if (st.bookKind != 0 || !st.section) return false;
  if (pageInfoCached()) return st.pageInfoFootnotes;
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

// ── 导出标注 / 书签（Markdown 落 SD）───────────────────────────────────
// 入口在**笔记标签页**搜索栏右端的「导出」按钮上，导的是**全部书**的笔记 + 书签：
// 笔记标签本来就是跨书总览（列表按书分组），导出跟着它一个口径。
// （2026-10-05 之前这是阅读菜单里的一项、只导当前这本；移到这里时改成全库。）
// 目标 <SD>/exports/标注导出_<年月日_时分秒>.md —— 用时间戳后缀而不是固定文件名，
// 因为读者常常想比对"上次导出之后又划了哪些"，固定名字会被后一次悄悄覆盖。
// 写入走 HalStorage（它对 64 字节对齐有要求，别自己 fopen）。
// 文件名固定，所以不再需要按书名消毒的 rdExportFileSafe（已随单书版一起删掉）；
// 书名照旧出现在正文的 "## 《书名》" 里，不受 FAT 长文件名的字符限制。

// 多行文本 → Markdown 引用块（逐行加 "> "）。跨行选中的原文本身就带换行，不这么做
// 后半段会掉出引用块，看起来像另起了一段正文。
static void rdAppendQuote(std::string &out, const std::string &text) {
  size_t pos = 0;
  for (;;) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    out += "> ";
    out.append(text, pos, nl - pos);
    out += "\n";
    if (nl >= text.size()) break;
    pos = nl + 1;
  }
}

// 标注里的时间戳 → "2026-10-03"；时钟当时不可信就给空串（宁缺勿错）。
static std::string rdExportNoteDate(int64_t t) {
  if (t <= 0) return "";
  const uint32_t epoch = static_cast<uint32_t>(t);
  if (!RdTime::clockValid(epoch)) return "";
  char buf[16];
  RdTime::formatOrdinal(RdTime::dayOrdinal(epoch), buf, sizeof(buf));
  return buf;
}

// 导出时某本书显示的名字：笔记里存了书名最准；没有就查书架扫出来的名字；再没有就退回
// 文件名（去扩展名）。书签表里没有书名这一列，所以书签只能这么反查。
static std::string rdExportBookTitle(const std::string &path) {
  for (const auto &n : st.notes) {
    if (n.path == path && !n.book.empty()) return n.book;
  }
  for (const auto &b : st.books) {
    if (b.path == path && !b.name.empty()) return b.name;
  }
  const size_t slash = path.find_last_of('/');
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  const size_t dot = base.find_last_of('.');
  if (dot != std::string::npos && dot > 0) base.resize(dot);
  return base.empty() ? std::string("未知书籍") : base;
}

static void rdExportAnnot() {
  // 按书分组：组序 = 笔记里首次出现的顺序，只有书签没有笔记的书追加在后面。
  // 组按 path 认（stable id），显示名另查（见 rdExportBookTitle）。
  struct Group {
    std::string path, title;
    std::vector<const RdState::RdNote *> notes;
    std::vector<const RdState::RdBookmark *> marks;
  };
  std::vector<Group> groups;
  auto groupFor = [&groups](const std::string &path) -> Group & {
    for (auto &g : groups) {
      if (g.path == path) return g;
    }
    groups.push_back(Group{});
    groups.back().path = path;
    groups.back().title = rdExportBookTitle(path);
    return groups.back();
  };
  for (const auto &n : st.notes) groupFor(n.path).notes.push_back(&n);
  for (const auto &b : st.bookmarks) groupFor(b.path).marks.push_back(&b);
  if (groups.empty()) {
    rdShowFloat("没有可导出的标注", "阅读时长按正文可标注 / 写笔记", 3000);
    return;
  }

  const time_t now = time(nullptr);
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmNow);

  std::string md;
  md.reserve(4096);
  md += "# 阅读标注导出\n\n";
  char dateBuf[32];
  strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d %H:%M", &tmNow);
  md += std::string("- 导出时间：") + (RdTime::clockValid() ? dateBuf : "时钟未同步") + "\n";
  md += "- 共 " + std::to_string(groups.size()) + " 本书 · 标注 " + std::to_string(st.notes.size()) +
        " 条 · 书签 " + std::to_string(st.bookmarks.size()) + " 条\n";

  for (auto &g : groups) {
    md += "\n## 《" + g.title + "》\n\n";
    md += "- 来源：`" + g.path + "`\n";
    if (!g.notes.empty()) {
      // 组内按记录时间升序；同一秒的多条保持原顺序（stable）。
      std::stable_sort(g.notes.begin(), g.notes.end(),
                       [](const RdState::RdNote *a, const RdState::RdNote *b) { return a->time < b->time; });
      md += "\n### 标注（" + std::to_string(g.notes.size()) + " 条）\n\n";
      for (const auto *n : g.notes) {
        rdAppendQuote(md, n->text);
        md += "\n";
        if (!n->note.empty()) {
          rdAppendQuote(md, std::string("**笔记**：") + n->note);
          md += "\n";
        }
        // 位置 + 时间：页码是"记下时"的页，重排/换字号后只作参考，所以不写成可点击的锚。
        std::string meta = "*第 " + std::to_string(n->page + 1) + " 页 · 第 " + std::to_string(n->spine + 1) +
                           " 章";
        const std::string d = rdExportNoteDate(n->time);
        if (!d.empty()) meta += " · " + d;
        meta += "*\n\n---\n\n";
        md += meta;
      }
    }
    if (!g.marks.empty()) {
      md += "\n### 书签（" + std::to_string(g.marks.size()) + " 条）\n\n";
      for (const auto *b : g.marks) {
        char line[64];
        snprintf(line, sizeof(line), "- 第 %d 页 · %d%%", b->page + 1,
                 static_cast<int>(b->percent * 100.0f + 0.5f));
        md += line;
        // summary 多数是 epub 的 "12/200" 页码串，和上面重复；只有它真的带原文时才跟上。
        const bool pageOnly = !b->summary.empty() && b->summary.find('/') != std::string::npos &&
                              b->summary.find_first_not_of("0123456789/") == std::string::npos;
        if (!b->summary.empty() && !pageOnly) md += " — " + b->summary;
        md += "\n";
      }
    }
  }

  const std::string dir = "/sdcard/exports";
  if (!Storage.exists(dir.c_str())) Storage.mkdir(dir.c_str(), true);
  const std::string path = dir + "/标注导出_" + stamp + ".md";
  if (!Storage.writeFile(path.c_str(), md)) {
    rdShowFloat("导出失败", "SD 卡写不进去", 3000);
    return;
  }
  char msg[64];
  snprintf(msg, sizeof(msg), "标注 %d · 书签 %d · %d 本", static_cast<int>(st.notes.size()),
           static_cast<int>(st.bookmarks.size()), static_cast<int>(groups.size()));
  rdShowFloat("已导出到 exports/", msg, 3000);
  ESP_LOGI(TAG, "导出标注: %s (%zu bytes, %zu 本)", path.c_str(), md.size(), groups.size());
}

// 「排版设定」子菜单：原来直接铺在阅读菜单里的那九项（字号…阅读线）。动作还是
// MenuAct 那一批，doMenuAction / 弹层（openRdPick）一行都不用改；改完值弹层收起时
// st.mode 没动，所以仍然落回本界面——和以前在平铺菜单里按一样，只是多一层返回。
// 平铺时这九项是菜单里数量最多的一类，把整页撑到要滚动，其它条目全被挤下去，这才拆出来。
static std::vector<MenuItem> layoutMenuItems() {
  char buf[64];
  std::vector<MenuItem> m;
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
  // embedded 的判定必须和 doMenuAction 里 Indent/Align 的守卫一字不差，否则会出现
  // "标着随书、按下去却真改了"。
  const bool embedded = styleEmbedded() && st.bookKind == 0;
  m.push_back({std::string("缩进: ") +
                   (embedded ? "随书" : (st.indentMode == 0 ? "自动" : st.indentMode == 1 ? "强制" : "取消")),
               MenuAct::Indent});
  m.push_back({std::string("对齐: ") + (embedded ? "随书" : kAlignLabels[clampI(st.alignMode, 0, 3)]), MenuAct::Align});
  m.push_back({std::string("边距: ") + (st.marginIdx == 0 ? "窄" : st.marginIdx == 1 ? "标准" : "宽"), MenuAct::Margin});
  m.push_back({std::string("图片: ") + (st.imageBilinear ? "双线性" : "最近邻"), MenuAct::Image});
  m.push_back({std::string("图片抖动: ") + kRdDitherNames[clampI(st.imageDither, 0, kRdDitherCount - 1)],
               MenuAct::ImageDither});
  m.push_back({std::string("阅读线: ") + kReadingLineNames[clampI(st.readingLine, 0, 3)], MenuAct::ReadingLine});
  return m;
}

static std::vector<MenuItem> menuItems() {
  char buf[64];
  std::vector<MenuItem> m;
  m.push_back({"目录", MenuAct::Toc});
  // ‹› 那一对（U+2039/203A）内置字体里都有（见 assets/builtin.ttf 的 cmap；用户字体缺字
  // 会走替补链），所以行尾那个 › 直接写进标签串就行，不用另画。
  m.push_back({"排版设定 ›", MenuAct::LayoutMenu});
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
  // 导出标注也不在这里：它是**跨书**的，入口在笔记标签页搜索栏右端（见 rdExportAnnot）。
  m.push_back({"返回书架", MenuAct::ToShelf});
  m.push_back({"返回阅读", MenuAct::Back});
  return m;
}

// 「排版设定」在阅读菜单里的行号：从子菜单 Esc 回来时把光标落回它，而不是跳回第一行
// （那样看起来像菜单被重置了）。找不到就退回第一项之后的位置。
static int rdMenuLayoutRow() {
  const auto items = menuItems();
  for (size_t i = 0; i < items.size(); i++) {
    if (items[i].act == MenuAct::LayoutMenu) return static_cast<int>(i);
  }
  return 1;
}

// 菜单列表的绘制（阅读菜单与排版子菜单共用一套几何：绘制和点按命中都从 titleListView
// 取，两处写岔了就会"看到的行"和"点到的行"错位）。
static void rdDrawMenuList(const std::vector<MenuItem> &items, const char *title, int sel) {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  drawTitle(title);
  const ListView lv = titleListView(static_cast<int>(items.size()), sel, uiLineHeight() + 12, statusTop());
  for (int i = 0; i < lv.rows && lv.first + i < static_cast<int>(items.size()); i++) {
    const int idx = lv.first + i;
    const int y = lv.top + i * lv.itemH;
    if (idx == sel) {
      g_rd.fillRect(0, y, w, lv.itemH, true);
      drawLineText(MARGIN, y + (lv.itemH - uiLineHeight()) / 2, items[idx].label.c_str(), false);
    } else {
      drawLineText(MARGIN, y + (lv.itemH - uiLineHeight()) / 2, items[idx].label.c_str(), true);
    }
  }
  drawFooter("↑↓ 选择  Enter 确认  Esc 返回");
}

// 菜单列表的按键：确认时返回选中的行号，否则 -1。Esc 由调用方处理——两个菜单的
// 返回目标不同（阅读菜单回阅读页，排版子菜单回阅读菜单）。
static int rdMenuListKey(const std::vector<MenuItem> &items, int &sel, int key) {
  const int n = static_cast<int>(items.size());
  {  // 上下/翻页在 ui/list_view.h（与 rdDrawMenuList 共用同一个几何）
    ListView lv = titleListView(n, sel, uiLineHeight() + 12, statusTop());
    if (listViewKey(lv, key)) {
      sel = lv.sel;
      st.dirty = 1;
      return -1;
    }
  }
  if (key != '\n') return -1;
  int x, y;
  if (input_tap_xy(&x, &y)) {
    const int row = listViewHitAt(titleListView(n, sel, uiLineHeight() + 12, statusTop()), y);
    if (row >= 0) sel = row;   // 点哪行就确认哪行，不用先上下移到它
  }
  return clampI(sel, 0, n - 1);
}

static void renderMenu() { rdDrawMenuList(menuItems(), "阅读菜单", st.menuSel); }
static void renderLayoutMenu() { rdDrawMenuList(layoutMenuItems(), "排版设定", st.layoutSel); }

// ── 词典 ────────────────────────────────────────────────────────────────
// 查询框的矩形必须**和 renderDict 画出来的一模一样**：renderDict 用
// `top = drawTitle("词典")` 当 qy，而 drawTitle 返回的是 rdHeadBottom() + 8
// （rdHeadBottom 里还夹着 TAB_BAND_BOTTOM=84，字号小的时候它才是大的那个）。
// 以前这里硬算 RD_TOP_INSET + uiLineHeight + 14，字号 <=20 时比画出来的框
// **高 12px**，点在框上沿到框底那一带就落不进命中区，直接被当成"没点输入框"
// → 走了查词 → 状态栏报"未找到词典"，虚拟键盘也就永远弹不出来。
// 现在直接取同一个来源，绘制和命中不可能再对不上。
static int dictQueryTop() { return coverTop(); }
int dictQueryH() { return uiLineHeight() + 12; }

// 查询缓存：同一个词反复查（选词查完退出来再选一次、在释义里来回翻看同一个词）不必
// 重走一遍索引二分 + .dict.dz 解压 + HTML 折纯文本 —— 那才是查一次词最贵的部分。
// 键是查询串，值是**一次 lookup 的全部产物**（正文/词头/状态），三者本来就同源，
// 命中就整份换掉，不会出现"新词头配旧正文"。换词典（openDictionary 失败或重新安装）
// 时作废：词典文件变了，旧正文就过期了。
namespace {
struct RdDictCacheEntry {
  std::string query;
  std::string result;
  std::string headword;
  std::string status;
};
constexpr size_t kDictCacheMaxEntries = 4;
constexpr size_t kDictCacheMaxBytes = 64 * 1024;   // 单条上限（超长释义不值得占着不放）
constexpr size_t kDictCacheMaxTotal = 128 * 1024;  // 全部条目合计上限
std::deque<RdDictCacheEntry> s_dictCache;          // 最近用过的在前
}  // namespace

void rdDictCacheClear() { s_dictCache.clear(); }

static bool rdDictCacheGet(const std::string &query, std::string &result, std::string &headword, std::string &status) {
  for (size_t i = 0; i < s_dictCache.size(); i++) {
    if (s_dictCache[i].query != query) continue;
    RdDictCacheEntry hit = s_dictCache[i];   // 拷一份再挪到队首（原地 erase/insert 会自伤）
    s_dictCache.erase(s_dictCache.begin() + static_cast<long>(i));
    s_dictCache.insert(s_dictCache.begin(), std::move(hit));
    result = s_dictCache[0].result;
    headword = s_dictCache[0].headword;
    status = s_dictCache[0].status;
    return true;
  }
  return false;
}

static void rdDictCachePut(const std::string &query, const std::string &result, const std::string &headword,
                           const std::string &status) {
  if (query.empty() || result.size() > kDictCacheMaxBytes) return;
  RdDictCacheEntry e{query, result, headword, status};
  s_dictCache.insert(s_dictCache.begin(), std::move(e));
  size_t total = 0;
  for (size_t i = 0; i < s_dictCache.size(); i++) {
    total += s_dictCache[i].result.size();
    // 条目数、总字节、单条三种上限任一超出就从队尾丢。
    if (i + 1 > kDictCacheMaxEntries || total > kDictCacheMaxTotal) {
      s_dictCache.resize(i);
      break;
    }
  }
}

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
  // 换词条 = 换正文：作废折行缓存并回到首行。放在最前面（含"未找到词典"的诊断正文），
  // 否则新释义会从上一次的滚动位置往下画，看着像开头被吃了。
  rdDictResetScroll();
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
  // 缓存命中：整份复用（含"未找到该词条"这类失败结果，省一次索引二分）。放在
  // openDictionary 之后：词典在 SD 上被换掉/删掉时，开词典这一步就会失败，缓存自然失效，
  // 不会拿上一本词典的正文继续糊弄（openDictionary 在已打开时立即返回，几乎不花钱）。
  if (rdDictCacheGet(st.dictQuery, st.dictResult, st.dictHeadword, st.dictStatus)) return;
  // 真正要查了：查完把这一份产物整条塞进缓存（下面两条出口都过这里）。
  const auto cacheIt = [&]() { rdDictCachePut(st.dictQuery, st.dictResult, st.dictHeadword, st.dictStatus); };
  if (st.dict.needsIndex()) {
    st.dictStatus = "首次建立词典索引...";
    st.dict.buildIndex();
  }
  Dictionary::LookupResult r;
  if (st.dict.lookup(st.dictQuery.c_str(), st.dictResult, st.dictHeadword, &r)) {
    st.dictStatus = "词条: " + st.dictHeadword;
    // StarDict 的释义正文没有统一格式：.ifo 写了 sametypesequence=h 的是 HTML，其余多半是
    // 带 '\n' 的纯文本，还有相当一部分词典根本没写 .ifo。统一折成纯文本再交给 wrappedText
    // —— 否则 HTML 的标签会原样画到屏上。纯文本输入基本原样通过（见 DictionaryText.cpp）。
    // 上面"没找到词典"那条诊断文字不走这里，里面的 "<名>.idx" 不会被当标签剥掉。
    st.dictResult = Dictionary::htmlToPlainText(st.dictResult);
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
  cacheIt();
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

void rdSyncVk() {
  if (editorVkVisible() != st.vkVisible) editorVkSetVisible(st.vkVisible);
}

// 键盘面板顶边 y。阅读模式原来用固定的 VK_H 算，现在统一问 editor_vk（面板高度
// 跟随当前字号，横竖屏都现算）。
int vkVkTop() { return editorVkTop(); }

void drawVk() {
  rdSyncVk();
  editorVkDraw();
}

// 开/收虚拟键盘（用户点状态栏右端那个图标，或点词典的查询框把面板叫回来）。
// **收起/展开必须整刷**：整块面板出现或消失，区域刷擦不掉旧键框（白底上留一圈灰），
// 而且收起后正文会重新排下来 —— 这跟键盘上按"换面板/换布局"是同一类"整块换图样"，
// 那几处也是置 fullRefresh（见 vkTap 的 EVK_PAGE/EVK_LAYOUT/EVK_T9/EVK_LANG）。
void rdToggleVk() {
  st.vkVisible = !st.vkVisible;
  st.fullRefresh = true;
  st.dirty = 1;
}

// 状态栏右端的键盘开关图标。绘制与命中都沿用 editor_vk 那一份几何（STATUS_BAR_Y /
// STATUS_BAR_H / FONT_H 槽宽）——阅读模式的提示栏 statusTop() 与它只差 2px，同一行，
// 不必再算一遍。**必须在状态栏画完之后调用**，否则会被提示栏的白底盖掉。
// 反白态取 editor_vk 的 s_visible，所以画之前先把可见性同步过去。
void rdDrawVkIcon() {
  rdSyncVk();
  editorVkDrawIcon();
}

bool rdVkIconHit(int x, int y) { return editorVkIconHit(x, y); }

// ── 实体键盘打字的"输入法条"（编码行 + 候选行）────────────────────────────
// 面板（drawVk）自带候选条，所以以前只有"弹键盘 / 不弹键盘"两种情形。可一旦接上蓝牙
// 键盘、或者用户用状态栏那个图标把面板收起来，就只剩一个裸输入框——候选字和编码串
// 一条都没有：中文能打进去，但人不知道现在拼到哪一步、这一页有哪些候选、按数字键会选
// 哪个字（本轮报障就是：能正确输入中文，却看不见候选和编码）。写作模式的编辑器一直有
// 这条（screen_editor.cpp 的 drawIMEUIWithStatusBar），阅读模式缺的只是"画一下"。
//
// 保留区的规矩跟编辑器一致（见 screen_editor.cpp 的 reserveIME 注释）：**输入法开着
// 就留位**，不按"这一刻有没有组合"来判——否则每上屏一次正文就上下跳一行。
bool rdImeBarOn() {
  if (st.vkVisible) return false;   // 面板自带候选条，比这条高得多，别叠着画
  return IME::getInstance().active();
}

// 正文可用底边。三种情形只能有这一个来源：虚拟键盘弹着 → 面板顶；实体键盘打字 →
// 输入法条上沿；两者都没有 → 提示行上沿。分开算的话，"留了位没画"或"画了没留位"
// 都会露馅（正文被条子盖住 / 条子下面空一条）。
// 前两种都减 6px：面板/条子的顶边就是它那块白底的上沿，正文最后一行贴上去会顶边。
int rdBodyBottom() {
  if (st.vkVisible) return vkVkTop() - 6;
  if (rdImeBarOn()) return statusTop() - imeBarPanelH() - 6;
  return statusTop();
}

// 画输入法条：底边贴阅读模式那条提示行的上沿（提示行自己画在 statusTop()）。
// 必须在正文之后、提示行之前调用。没在组合时 drawIMEUI 自己什么都不画（保留区仍然算，
// 见 rdBodyBottom），所以这里是"画一下"而不是"判要不要画"。
void drawRdImeBar() {
  if (!rdImeBarOn()) return;
  drawIMEUI(statusTop());
}

// 前向声明（OPDS/传书界面在文件后段定义，但 VK 的回车提交需要调用它们）。
void renderCurrent();
static void renderSettingsTab();  // 「设置」标签（定义在菜单动作之后）
void prepareQr();
static void rdNoteCommit();      // 笔记编辑器保存（定义在文件后段）
static void renderNotes();       // 「笔记」标签（定义在文件后段）
void renderNoteEdit();
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

// 目标串的 ImeField 形态：阅读模式的字段全是**追加型**（光标恒在末尾），所以 cursor
// 传 nullptr。vkTargetString 留在阅读模式这边 —— 它按 st.mode 选目标，是阅读模式自己
// 的状态，不该搬进共享层。
static ImeField vkTargetField() {
  return ImeField{vkTargetString(), /*cursor=*/nullptr};
}

static void commitVk(const std::string &s) {
  if (s.empty()) return;
  imeFieldInsert(vkTargetField(), s);   // 无目标字段时是空操作
  // 搜索框里每敲一个字，命中表就变一次；选中项跟着回到第一命中，免得停在
  // 已经被筛掉的第 N 项上（列表本身每帧也会 clamp，这里只是让它更直觉）。
  if (st.mode == RdMode::ShelfSearch || st.mode == RdMode::NotesSearch) st.searchSel = 0;
}

void feedVkKey(int c) {
  // 输入法优先，没接住的可打印 ASCII 当裸字符 —— 这条规矩写作/计划/阅读三处一样，
  // 收在 ime_field 里（少了它，数字面板上的 1..9/0 一个也打不出来）。
  // 这里不碰 st.dirty：所有调用点在 vkTap/按键处理里都已经置过了。
  std::string out;
  if (imeFieldKeyText(IME::getInstance(), c, /*multiline=*/false, out)) commitVk(out);
}

void feedVkBackspace() {
  auto &ime = IME::getInstance();
  if (ime.composing()) {
    std::string out;
    ime.handleKey('\b', out);
    commitVk(out);
  } else {
    imeFieldBackspace(vkTargetField());
  }
}

void vkEnter() {
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

// ── 虚拟键盘"增量帧" ─────────────────────────────────────────────────────
// 面板那块（编码行、候选行、键区）由 editorVkDraw 自己整块重铺：先把面板矩形填成
// 白底，再画键框/标签/候选 —— 也就是说**它不依赖底下画了什么**。于是只要这一拍
// 除了键盘自身没有任何东西变化，就没必要把整页重画一遍：上一帧留在帧缓冲里的
// 上半屏（标题/查询框/结果列表/笔记正文框/词典释义）本来就是当前该显示的内容，
// 让它原样待着即可，只有面板那块需要重刷。省下的是每键 100ms 上下的整屏绘制 ——
// **差分与波形一像素都不变**（差分比的是 back_fb，不是中间那份白屏），所以只是
// 把"按键→出图"里的分发包削掉一段，不会动刷新策略那套。
//
// 判据用一个最不容易出错的口径：**目标字符串在这一拍前后逐字节相同**。它一次性
// 罩住退格、英文直通上屏、句读直通（后两者根本不走 commitSeq）、以及"重新过滤
// 命中表导致选中项回到第一项"（commitVk 里那句 searchSel=0 只在串变了时才发生）。
// 另外还要求：模式没变（回车打开结果会切模式）、本帧没被置 fullRefresh（换面板/
// 换布局/T9/中英切换那几键必须整屏 GC16，见 vkTap 的注释）、键盘确实弹着、
// 且场上没有任何浮层（busy/浮动提示是盖在上半屏的，而且随时间自己变）。
static bool s_vk_incr_ok = false;

// 作用域守卫：vkTap 里所有早退（死区、EVK_* 那十来个分支、回车/退格/普通字符）
// 都从这一个析构口出去，不必在每个 return 前各写一遍判定。
struct RdVkIncrGuard {
  std::string before;
  RdMode mode0;
  RdVkIncrGuard() {
    const std::string *t = vkTargetString();
    if (t) before = *t;
    mode0 = st.mode;
    s_vk_incr_ok = false;
  }
  ~RdVkIncrGuard() {
    const std::string *t = vkTargetString();
    s_vk_incr_ok = (t != nullptr && mode0 == st.mode && *t == before);
  }
};

// 点按 → editor_vk 命中 → 翻译成"普通键码" → 走阅读模式既有的输入逻辑。
// 和写作模式一样，键盘不重复实现任何输入逻辑：候选返回 '1'+i（交给 IME 完成选词）、
// 字母返回 'a'..'z' 或一键多字母布局的组码、空格/退格/回车返回 ' ' / '\b' / '\n'，
// 其余（翻页/中英/布局/Shift/Ctrl）在 editorVkHitTest 内部已经翻转了自身状态，
// 这里重绘即可。
void vkTap(int x, int y) {
  RdVkIncrGuard incrGuard;   // 见 s_vk_incr_ok：这一拍能不能只重画键盘面板
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

// ── 渲染：词典 ─────────────────────────────────────────────────────────
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
void applyNightMode() {
  // 灰阶自检页要看的正是真实的灰度关系：夜间反色会把 16 级梯整条翻过来（白↔黑），
  // 跟"从黑到白数台阶"的直觉打架，也会把抖动对比条整条改观。这一页强制日间
  // （离开这一页的下一帧 applyNightMode 就把全局夜间恢复回去）。
  if (st.mode == RdMode::RefreshTest) {
    board_set_night(false);
    return;
  }
  board_set_night(g_settings.nightMode());
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
void buildToPage(int target) {
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

// ── 书架按最近阅读排序（声明在 scanBooks 之前）────────────────────────────
// "最近阅读时间"的唯一真源是 reader_progress.txt 的**行序**：那个文件最新在前，每次
// 翻页/换书都把当前书挪到表头（见上面这段的 insert(begin())），所以文件本身就已经是
// "按最近阅读排好序"的一份快照 —— 行号就是名次，不必再存时间戳字段（存了还要解 NVS
// 时间的坑，而且翻页时写盘的那一行会变长）。
//
// 没读过的书（刚拷进来的，或读了 kProgressMax 本之后被挤出表的）排在读过的**之后**，
// 组内按文件名排 —— 不能让 readdir 的返回顺序决定书架顺序，否则每次重扫书架都可能跳
// 一下。用 stable_sort 保证同名同组的相对次序也是确定的。
static void rdSortShelfByRecency() {
  if (st.books.size() < 2) return;
  loadProgress();
  auto lower = [](const std::string &s) {
    std::string r = s;
    for (char &c : r) c = (char)tolower((unsigned char)c);
    return r;
  };
  // 路径 → 名次（越小越近）。键取小写：books/Books 是同一个 FAT 目录（见 scanBooks
  // 的去重注释），进度表里存的是打开时拼出来的那个大小写，可能和这里扫出来的一致也可能
  // 不一致。emplace 不覆盖 → 同一本书只留最靠前的那条。
  std::map<std::string, int> rank;
  for (size_t i = 0; i < s_progress.size(); i++) {
    rank.emplace(lower(s_progress[i].path), static_cast<int>(i));
  }
  const int kUnread = 1 << 30;   // 进度表里没有这本书（没读过 / 已被挤出表）
  auto rankOf = [&](const BookEntry &b) -> int {
    auto it = rank.find(lower(b.path));
    return it == rank.end() ? kUnread : it->second;
  };
  std::stable_sort(st.books.begin(), st.books.end(), [&](const BookEntry &a, const BookEntry &b) {
    const int ra = rankOf(a), rb = rankOf(b);
    if (ra != rb) return ra < rb;
    if (ra == kUnread) return lower(a.name) < lower(b.name);   // 都没读过：按文件名
    return false;                                              // 都读过则保持原序
  });
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
      const DitherMode dm = rdCoverDitherMode();
      DitherRowState ds;
      for (int x = 0; x < sw; x++) {
        if (opacity[x] == 0) { dst[x] = 15; continue; }   // 透明 = 纸白
        dst[x] = grayToLevel16(rdCoverContrast(data[x]), x, y, dm, ds);
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
void prepareQr() {
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
// 书签 / 脚注两屏的列表几何（行高 8，居中式窗口）。渲染与点按命中共用。
static ListView bookmarkListView() {
  return titleListView(static_cast<int>(st.bookmarks.size()), st.bookmarkSel, uiLineHeight() + 8, statusTop());
}

static ListView footnoteListView() {
  return titleListView(static_cast<int>(st.footnoteNums.size()), st.footnoteSel, uiLineHeight() + 8, statusTop());
}

static void renderBookmarks() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  drawTitle("书签");
  if (st.bookmarks.empty()) {
    drawCenteredLine(g_rd.getScreenHeight() / 2, "暂无书签");
    drawFooter("Esc 返回");
    return;
  }
  const ListView lv = bookmarkListView();
  int itemH = lv.itemH;
  int maxRows = lv.rows;
  int start = lv.first;
  for (int i = 0; i < maxRows && start + i < static_cast<int>(st.bookmarks.size()); i++) {
    int idx = start + i;
    auto &b = st.bookmarks[idx];
    char line[160];
    snprintf(line, sizeof(line), "%d%%  %s", static_cast<int>(b.percent * 100), b.summary.c_str());
    int y = lv.top + i * itemH;
    if (idx == st.bookmarkSel) { g_rd.fillRect(0, y, w, itemH, true); drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, line, false); }
    else drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, line, true);
  }
  drawFooter("↑↓ 选择  Enter 跳转  Esc 返回");
}

static void handleBookmarks(int key) {
  int n = static_cast<int>(st.bookmarks.size());
  if (key == 0x1B) { st.mode = RdMode::Menu; st.fullRefresh = true; st.dirty = 1; return; }
  if (n == 0) return;
  {  // 上下/翻页在 ui/list_view.h（与 renderBookmarks 共用同一个几何）
    ListView lv = bookmarkListView();
    if (listViewKey(lv, key)) { st.bookmarkSel = lv.sel; st.dirty = 1; return; }
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      const int row = listViewHitAt(bookmarkListView(), y);
      if (row >= 0) st.bookmarkSel = row;
    }
    gotoBookmark(st.bookmarkSel);
    st.dirty = 1;
    return;
  }
}

static void renderFootnotes() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  drawTitle("脚注");
  if (st.footnoteNums.empty()) {
    drawCenteredLine(g_rd.getScreenHeight() / 2, "本页无脚注");
    drawFooter("Esc 返回");
    return;
  }
  const ListView lv = footnoteListView();
  int itemH = lv.itemH;
  int maxRows = lv.rows;
  int start = lv.first;
  for (int i = 0; i < maxRows && start + i < static_cast<int>(st.footnoteNums.size()); i++) {
    int idx = start + i;
    int y = lv.top + i * itemH;
    if (idx == st.footnoteSel) { g_rd.fillRect(0, y, w, itemH, true); drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, st.footnoteNums[idx].c_str(), false); }
    else drawLineText(MARGIN, y + (itemH - uiLineHeight()) / 2, st.footnoteNums[idx].c_str(), true);
  }
  drawFooter("↑↓ 选择  Enter 弹注  → 跳转  Esc 返回");
}

static void handleFootnotes(int key) {
  int n = static_cast<int>(st.footnoteNums.size());
  if (key == 0x1B) { st.mode = RdMode::Menu; st.fullRefresh = true; st.dirty = 1; return; }
  if (n == 0) return;
  {  // 上下/翻页在 ui/list_view.h（与 renderFootnotes 共用同一个几何）
    ListView lv = footnoteListView();
    if (listViewKey(lv, key)) { st.footnoteSel = lv.sel; st.dirty = 1; return; }
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      const int row = listViewHitAt(footnoteListView(), y);
      if (row >= 0) st.footnoteSel = row;
    }
    // 默认真弹注：在书里就地看一眼注释，不跳走（跳走再回来得走"返回脚注跳转前"）。
    // 注释不在本章（跨文件的 notes.xhtml）时弹不出来，退回老行为直接跳。
    const int sel = st.footnoteSel;
    if (!rdOpenFootnote(sel, false)) jumpToFootnote(sel);
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

// 弹注浮层（盖在正文页上的一块）开/关时用哪一档刷。
// 原来这两处一律 st.fullRefresh = true → GC16 整屏全刷：面板要闪一下，一次一两秒，
// 用户侧就是"每次弹注都很慢"。而浮层底下的正文页一个像素没动，差分刷天然既能把浮层
// 画上去、也能在关掉时擦干净（擦不净的部分记进 s_ghostAccum，攒够预算自己全刷清账，
// 见下面的自适应档）。所以它跟正文翻页是同一类帧，交给用户的刷新策略决定就好。
// 唯一例外是"全局"（策略 0）——那是用户明选的"每屏都清干净"，照旧全刷。
void rdOverlayRefresh() {
  if (refreshStrategy() == 0) st.fullRefresh = true;
  st.dirty = 1;
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

// 阅读器整条绘制路径直画 epdiy 的 front_fb（见 screen_reader_init），但 u8g2 shim 的
// fb 指针是 **core1 渲染任务**的地盘：那条路径每帧开头都会把它指向自己的工作缓冲
// （ui_render.cpp 的 ui_render_begin_frame → u8g2_set_fb），离开时没人拨回来。于是进
// 阅读器之前推过的最后一帧 UI 会把 shim 落在 s_fb[idx] 上，此后凡是走 u8g2/FontRenderer
// 的绘制 —— editor_vk 的**整个键盘面板与键帽字形**、drawIMEUI 的编码/候选条 —— 全都画进
// 那块**永远不会被推屏**的缓冲：屏上什么都看不到（输入本身不走绘制，所以中文照打，
// 只有"画出来的"东西消失，正是"看不到候选字和编码区但能正确输入"的样子）。
// 阅读器本来就直画 front_fb，shim 跟着它走即可。每帧钉一次，另在 init 里补一次。
static void rdPinShimFb() {
  if (g_u8g2) u8g2_set_fb(g_u8g2, g_rd.getFrameBuffer());
}

void renderCurrent() {
  rdPinShimFb();
  // 虚拟键盘增量帧：这一拍只有键盘面板变了，上半屏一个像素都不用重画
  // （帧缓冲里留着的就是上一帧的内容，本来也该是它）。见 s_vk_incr_ok。
  const bool vkIncr = s_vk_incr_ok && st.vkVisible && !st.fullRefresh &&
                      vkTargetString() != nullptr && st.busyMsg.empty() && st.floatMsg.empty() &&
                      !s_imgPresentSkipped;
  s_vk_incr_ok = false;
  if (vkIncr) {
    drawVk();
  } else {
  // 本帧内容默认是纯黑白（正文/菜单/列表），插图页/图片查看器/自检页会自己把它置位。
  // 这是"面板上现在是什么"的账，**在推屏之后**才交给推屏层（见 renderCurrent 末尾），
  // 放这里只是"本帧还没画，先按默认算"。
  st.frameGray = 0;
  switch (st.mode) {   // 常规帧：整页重画
    case RdMode::Browser: renderBrowser(); break;
    case RdMode::Reading: renderReading(); break;
    case RdMode::Toc: renderToc(); break;
    case RdMode::Menu: renderMenu(); break;
    case RdMode::LayoutMenu: renderLayoutMenu(); break;
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
    case RdMode::RefreshTest: renderRefreshTest(); break;
    case RdMode::Apps: renderApps(); break;
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
  }  // end 常规帧（见上面 st.frameGray 那段）
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
  // 极速刷(DU)只给**用实体键盘打字**的场景：界面上能打字（WiFi/OPDS 地址、笔记、
  // 词典查询），而且键盘不是虚拟键盘（蓝牙键盘连上了才自动收起）。虚拟键盘是手点的，
  // 一键之间有整段等待，抢不到那点刷新时间，DU 的低画质反而把残影留在屏上；那种场景
  // 与其他界面一样走常规局刷(GL16)。
  const bool physTyping = rdTypingScreen() && !st.vkVisible && g_bt.isConnected();
  // 虚拟键盘打字帧：键盘弹着，而且当前确实有一个可编辑的字段（vkTargetString 的
  // 集合恰好就是会画键盘的那 8 个界面——重命名页不在 rdTypingScreen 里，它走的是
  // 这一条）。这种帧不走整屏 HALF：整屏 GL16 会被 display 侧的"GL16 恒全像素"规则
  // 升级成整屏全像素，每按一个键闪一屏。改走区域刷，见下面 displayBufferVk。
  const bool vkTyping = st.vkVisible && vkTargetString() != nullptr;
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
      //   ≤40‰    空白页之类几乎没变 → 也走 8 灰阶正文刷。以前这里用极速 DU：
      //           DU 只有 20 相 + 二值化阈值，残影最重；翻到空白页之后下一页正文
      //           跟着差分出来，前一页的墨痕还没消净、两页叠在一起（"自适应偶发
      //           花屏"的根子）。空白页几乎没什么好画的，DU 省的百来毫秒不值当。
      //   其余    默认局刷 GL16，同时把变化量累加成残影预算，攒够 2600‰ 用一次
      //           8 灰阶 GC16 全刷清账（该驱动不变像素、本来就要闪一次，
      //           用 30 相替 36 相省 ~70ms）。
      if (frameChange >= 300) {
        m = HalDisplay::FULL_REFRESH;
      } else if (frameChange >= 120) {
        m = HalDisplay::GRAY8_TEXT_REFRESH;
      } else if (frameChange <= 40) {
        m = HalDisplay::GRAY8_TEXT_REFRESH;
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
  bool rtPresented = false;   // 本帧是自检页的自推屏（下面收尾要再置一次 dirty，见尾注）
  if (st.rtPending >= 0) {
    // 灰阶自检页（renderRefreshTest）的 Enter：这一帧的画已经画好了，但推屏要用它选的
    // 那条刷法（读者 TU 不碰波形类型，落地点在 display.c）——自推一次，跳过常规推屏。
    const int which = st.rtPending;
    st.rtPending = -1;
    st.rtMs[which] = reader_refresh_test_present(which);
    rtPresented = true;
  } else if (vkTyping && !st.fullRefresh) {
    // 虚拟键盘打字帧：只驱动与上一帧有差异的那块矩形（编码/候选两行快刷，键盘区与
    // 文本输入区局刷），与写作模式的虚拟键盘同一套判据 —— 见 reader_vk_present。
    // st.fullRefresh 那一帧不走这条：进界面首帧本来就该整屏 GC16 清场（那是应该的整屏刷）。
    display.displayBufferVk(vkVkTop(), editorVkCandH());
  } else {
    // 整屏全刷（翻页全刷/换章/进界面首帧）把每个像素都驱动了一遍，虚拟键盘快档
    // 欠的那块正文已经干净了，把账销掉，免得停手时再白闪一次。
    if (m == HalDisplay::FULL_REFRESH) ui_render_reader_vk_settle_forget();
    g_rd.displayBuffer(m);
  }
  // 白底参考帧纪律：**推屏之后**记下"面板上现在是不是中灰"（语义是面板现状，放在
  // 渲染入口记会让同一页的下一帧被自己置位 → 每帧白闪）。下一次走差分档的推屏会先
  // GC16 铺白再画（消费方在 display.c，只对差分档生效），这笔账随即清掉。
  reader_set_gray_panel(st.frameGray);
  st.fullRefresh = false;
  st.dirty = 0;
  // 自检页自推的那一帧：再画一次把刚测出的耗时显示出来（那一帧内容只差一行小字，
  // 差分刷很便宜）。放在 st.dirty = 0 之后，否则会被上面那行清掉。
  if (rtPresented) st.dirty = 1;
  // 这一帧已经推出去了，用户正盯着新页看 —— 正是把排版余量补回来的空档。
  rdPrebuildAhead();
}

// 先刷一帧"正在…"（当前界面 + 居中浮层）再进阻塞段。openBook 会建元数据、解 zip、
// 分章排版，大书要好几秒；e-ink 上这几秒整屏不动，用户会以为死机。
// renderCurrent 收尾已经把 fullRefresh/dirty 复位，这里不用再管。
void rdShowBusy(const char *msg, const std::string &sub) {
  st.busyMsg = msg;
  st.busySub = sub;
  st.fullRefresh = true;   // 浮层首帧走全刷，免得和上一屏的残影叠在一起
  renderCurrent();
  st.busyMsg.clear();
  st.busySub.clear();
}

void rdShowFloat(const std::string &msg, const std::string &sub, int ms) {
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
  // 书架按最近阅读排序：刚读完的这本已经在进度表表头了，重排一次让它跳回最前面，
  // 并把光标停在它身上 —— 排序会移动下标，光标不跟过去的话就指着另一本书了。
  {
    const std::string justRead = st.bookPath;
    rdSortShelfByRecency();
    if (!justRead.empty()) {
      for (size_t i = 0; i < st.books.size(); i++) {
        if (st.books[i].path == justRead) { st.sel = static_cast<int>(i); break; }
      }
    }
  }
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
      if (n > 0) openSpineLast(n - 1);
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
int rdUtf8Len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

// 注号归一化：把各种写法的注号压成同一串再比。
// 晋书是 〔一〕、Duokan 是 [1]/1、有的书是 (1) / （1）/ 1. —— 去掉空白与各类括号/
// 句点后：〔一〕→"一"、[1]→"1"、1.→"1"。比对时两边都过一遍这个函数。
std::string rdNormalizeNoteNumber(const std::string &s) {
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

  // ⓪ 合成注号的**私有链接**："fn:<序号>"。解析器在发那颗上标字的时候就把这条边接好
  //    了（见 ChapterHtmlSlimParser::appendAltFootnoteMarker），序号与页脚注表里的
  //    number 同源。这里按 id 直取 —— 不再拿被点中的**字形**去比号码串。字形比对正是
  //    "点 8 弹出 86"的来源：注号排完版会被切成几段、还会跟邻号的号码撞前缀。顺带
  //    靠链接矩形（而非字形）拿命中面积，半个字宽的上标也点得中。
  //    斐洞/QQ 阅读器那种"注号是一张小图、书里没有成对的 id"的书走的就是这条路。
  if (href.rfind("fn:", 0) == 0) {
    const std::string serial = href.substr(3);
    if (loadCurrentFootnotes()) {
      for (int i = 0; i < static_cast<int>(st.footnoteNums.size()); i++) {
        if (st.footnoteNums[i] != serial) continue;
        if (!rdOpenFootnote(i, false)) return false;
        rdRememberNoteRef(st.footnoteNums[i], st.spineIndex, st.page);
        ESP_LOGI(TAG, "点注号链接: 'fn:%s' → 弹注第 %d 条", serial.c_str(), i);
        return true;
      }
    }
    ESP_LOGW(TAG, "点注号链接: 'fn:%s' 本页脚注表里没有这条", serial.c_str());
    return false;
  }

  // ① 正向注号：本页脚注表里有同一条 href。
  if (loadCurrentFootnotes()) {
    for (int i = 0; i < static_cast<int>(st.footnoteHrefs.size()); i++) {
      if (st.footnoteHrefs[i] != href) continue;
      // 条目在、正文取不到（且排不出来）→ 落回下面按锚点跳。
      // 挂起态（Pending）返回 true：浮层已经摆好，按键到此为止，不再落进翻页。
      if (!rdOpenFootnote(i, false)) break;
      // 记下上标位置，注文区那边按注号回跳时就能直接命中（见 rdGotoNoteRef）。
      rdRememberNoteRef(st.footnoteNums[i], st.spineIndex, st.page);
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
//   而注号是 "一五"。
//   顺带一提，单字命中框只有注号的 1/3 宽，所以"点上标"还常常点不中。
//
// 修法：先把这个词所在的那**一整段连续上标**并成一个"注号框"，用整段文本归一化比对，
//   命中判定也用整段的框。比对和命中面积一起修好。合并只在词表里**连续**的上标词之间
//   发生，所以正文里两个相隔的注号不会被并成一个（中间夹一个非上标词就断）。
static bool rdTapOnFootnote(int x, int y) {
  if (st.bookKind != 0 || !g_pageText.valid || !st.section) return false;
  if (!loadCurrentFootnotes()) return false;
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
  // 命中不再是"扫到第一段就弹"。放宽容差之后，相邻两行的注号框会互相重叠（注文页
  // 一行一条 〔一〕〔二〕…，注号都在同一个 x 上，行距又只有 ~50px），先命中谁就弹谁
  // 会变成"点二弹一"。所以整页扫完，取**框中心离手指最近**的那一段。
  int bestFound = -1;
  long bestDist2 = 0;
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
        }
      }
    }
    i = runEnd;
  }
  if (bestFound >= 0) {
    if (rdOpenFootnote(bestFound, false)) {
      // 点中的就是正文里的上标，此刻这一页就是它的位置 —— 记下来，好让注文区那边
      // 点行首注号能跳回来（见 rdGotoNoteRef）。挂起态也一样：人还站在这一页上。
      rdRememberNoteRef(st.footnoteNums[bestFound], st.spineIndex, st.page);
      return true;
    }
    return false;
  }
  return false;
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
      // crossmux 阅读器触摸分区：左右 1/3 翻页，中间 1/3 菜单
      if (x < w / 3) turnBook(-1);
      else if (x > w * 2 / 3) turnBook(+1);
      else openMenu();
      return;
    }
    openMenu();  // 非点按回车(BLE/KEY3)，没有点按坐标
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
  // 上下滑 = 目录整屏翻。选区每帧都被拉回屏幕正中（居中窗口，见 tocListView），
  // 所以按一屏走一格，列表就正好前进/后退一屏，跟翻书一样。算术在 ui/list_view.h。
  {  // 上下/翻页在 ui/list_view.h（与 renderToc 共用同一个 tocListView 几何）
    ListView lv = tocListView(n, st.tocSel);
    if (listViewKey(lv, key)) { st.tocSel = lv.sel; st.dirty = 1; return; }
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y) && n > 0) {
      const int row = listViewHitAt(tocListView(n, st.tocSel), y);
      if (row >= 0) st.tocSel = row;
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

// 「U 盘模式」退出按钮的几何（绘制与命中测试共用，保证同一套坐标）。
struct UsbDriveBtn { int x, y, w, h; };
static UsbDriveBtn usbDriveBtn() {
  const int h = uiLineHeight() + 16;  // 触控目标高一点
  const int w = 360;                  // 居中、够宽好点
  const int y = statusTop() - h - 24; // 贴近底部、在页脚线上方
  return { (g_rd.getScreenWidth() - w) / 2, y, w, h };
}

// 「U 盘模式」全屏静态页。blocked=false 是进入前的正常提示；blocked=true 是用户点了
// 退出但主机仍占用、被 usb_msc_run 拒绝后刷新的"请先安全弹出"警告。
static void drawUsbDrivePage(bool blocked) {
  g_rd.clearScreen();
  int top = drawTitle("U盘模式");
  const char *lines[4];
  int n = 0;
  if (blocked) {
    lines[n++] = "电脑仍在使用 U 盘，尚未安全弹出";
    lines[n++] = "请先在电脑上「安全弹出」后再点退出";
  } else {
    lines[n++] = "本机 SD 卡已作为 U 盘挂载到电脑";
    lines[n++] = "电脑会直接读写整张 SD 卡";
    lines[n++] = "退出前请先在电脑上「安全弹出」并确认未在写卡";
  }
  const UsbDriveBtn b = usbDriveBtn();
  const int lineH = uiLineHeight() + 8;
  const int bodyBot = b.y - 20;   // 正文排到按钮上沿上方
  const int blockH = n * lineH;
  int y = top + (bodyBot - top - blockH) / 2;
  for (int i = 0; i < n; i++) {
    drawCenteredLine(y, lines[i]);
    y += lineH;
  }

  // 退出按钮：黑底白字，居中。
  g_rd.fillRect(b.x, b.y, b.w, b.h, true);
  const char *btnLabel = "退出 U 盘模式";
  const int tw = g_rd.getTextWidth(uiFontId(), btnLabel);
  drawLineText(b.x + (b.w - tw) / 2, b.y + (b.h - uiLineHeight()) / 2, btnLabel, false);
  g_rd.displayBuffer(HalDisplay::FULL_REFRESH);
}

// 点按是否命中"退出"按钮。
static bool usbDriveExitHit(int x, int y) {
  const UsbDriveBtn b = usbDriveBtn();
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

// 退出判定：只认点中"退出"按钮；触摸屏异常时电源键/返回键兜底（其它按键一律忽略，
// 避免误触任意键就撕下 MSC）。
static bool usbDriveShouldExit(int key) {
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) return usbDriveExitHit(x, y);
    return false;
  }
  return key == KEY_POWER || key == KEY_BACK;
}

// usb_msc_run 拒绝退出时回调：重画一屏"请先安全弹出"警告（方向在进入等待前已固定横屏）。
static void usbDriveBlockedHint() {
  drawUsbDrivePage(true);
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
      openRdPick(static_cast<int>(MenuAct::Font));
      break;
    case MenuAct::FontFamily: {
      // 本书在用内嵌字体时，这个键改的是**用户全局字体**，对当前这本没有可见效果
      // （书内字体优先，见 openEpub）。与其让用户按了没反应、以为设置坏了，不如直说
      // 并且指路到那本书的开关上。
      if (!st.bookFontLocal.empty()) {
        rdShowFloat("本书使用内嵌字体", "设置 → 内嵌字体 可关闭", 3000);
        break;
      }
      // 弹出式选择（内建 + SD 里扫到的字体，与「设置 → 字体」同一份清单）。
      // 原来的循环制在卡上放好几款字体时要按十几次才轮到目标，看不出有哪些可选。
      openRdPick(static_cast<int>(MenuAct::FontFamily));
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
      board_set_night(st.night);
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
    case MenuAct::LayoutMenu:
      // 排版子菜单：只是换个列表画，动作还是上面那一批 case，底下不用加任何东西。
      st.layoutSel = 0;
      st.mode = RdMode::LayoutMenu;
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
      rdDictResetScroll();
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
    case MenuAct::RefreshTest:
      st.rtSel = 0;
      st.rtPending = -1;
      for (int i = 0; i < 5; i++) st.rtMs[i] = -1;
      st.vkVisible = false;
      st.retMode = RdMode::Settings;  // Esc 回设置标签（与关于页同一套单层回退）
      st.mode = RdMode::RefreshTest;
      st.fullRefresh = true;
      break;
    // 轮换制条目：全部改成弹出式选择（openRdPick 弹层，落定后走 applyRdPick）。
    // 弹层比循环好在"有哪些档位、现在是哪档"一眼看清，也不用按好几次才跳到目标档。
    case MenuAct::ShelfStyle:
    case MenuAct::StyleSource:
    case MenuAct::EmbeddedFont:
    case MenuAct::ImageDither:
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
    case MenuAct::UsbDrive:
      // U 盘模式：先画提示页，再把阅读器整个退掉（释放所有 SD 文件句柄）；写作模式若
      // 为竖屏，exit 会恢复成竖屏，所以这里再拉回横屏，让触摸映射/绘制/按钮命中在
      // 整个等待期间都是同一套 1216×684 坐标。usb_msc_run() 卸载 SD、接管 USB，阻塞到
      // 点中"退出"按钮且主机已安全弹出（主机仍占用时只刷警告、不退出），最后重进阅读器。
      drawUsbDrivePage(false);
      screen_reader_exit();
      board_force_landscape();
      usb_msc_run(usbDriveShouldExit, usbDriveBlockedHint);
      screen_reader_init();
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
  if (key == 0x1B) { st.mode = RdMode::Reading; st.fullRefresh = true; st.dirty = 1; return; }
  auto items = menuItems();
  const int row = rdMenuListKey(items, st.menuSel, key);
  if (row < 0) return;
  st.retMode = RdMode::Menu;  // 从这里进的子界面，Esc 回阅读菜单
  doMenuAction(items[row].act);
  st.dirty = 1;
}

static void handleLayoutMenu(int key) {
  if (key == 0x1B) {
    st.mode = RdMode::Menu;
    st.fullRefresh = true;
    st.dirty = 1;
    st.menuSel = rdMenuLayoutRow();   // 光标落回「排版设定」那一行（见 rdMenuLayoutRow）
    return;
  }
  auto items = layoutMenuItems();
  const int row = rdMenuListKey(items, st.layoutSel, key);
  if (row < 0) return;
  // 本子菜单里的动作（字号/字体/行距/…）都不会切界面，改完值仍停在这一屏；
  // retMode 只是留给"以后在这儿加一个会切界面的条目"时有个正确的落点。
  st.retMode = RdMode::LayoutMenu;
  doMenuAction(items[row].act);
  st.dirty = 1;
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
  // 微信读书已经从这一屏搬去 1 号位的「应用」标签（RdMode::Apps 的微读图标）。
  // 词典下载留在这儿：它跟下面的「资源下载」一样是**下素材**的维护动作，不是天天用的
  // 入口；「应用」标签里那枚「字典」是查词页（RdMode::Dictionary），两回事。
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
  // 插图/书架封面的抖动档。它跟刷新策略一样是「屏幕怎么出画面」的档位，阅读页的
  // 「排版设定」子菜单里也有一条（挨着「图片: 双线性」）；两边写同一个键
  // （reader_image_dither），改哪儿都算数（同「翻页动画」的惯例）。
  m.push_back({std::string("图片抖动: ") + kRdDitherNames[clampI(st.imageDither, 0, kRdDitherCount - 1)],
               MenuAct::ImageDither});
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
  m.push_back({"U盘模式", MenuAct::UsbDrive});
  m.push_back({"屏幕自检", MenuAct::RefreshTest});
  m.push_back({"关于本机", MenuAct::About});
  return m;
}

static void renderSettingsTab() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  drawTabBar();
  auto items = settingsItems();
  int n = static_cast<int>(items.size());
  const ListView lv = titleListView(n, st.setSel, uiLineHeight() + 12, tabBottom());
  int itemH = lv.itemH;
  int maxRows = lv.rows;
  int start = lv.first;
  for (int i = 0; i < maxRows && start + i < n; i++) {
    int idx = start + i;
    int y = lv.top + i * itemH;
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
  {  // 上下/翻页在 ui/list_view.h（与 renderSettingsTab 共用同一个几何）
    ListView lv = titleListView(n, st.setSel, uiLineHeight() + 12, tabBottom());
    if (listViewKey(lv, key)) { st.setSel = lv.sel; st.dirty = 1; return; }
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      int t = tabHit(x, y);
      if (t >= 0) { switchTab(t); return; }
      const int row = listViewHitAt(titleListView(n, st.setSel, uiLineHeight() + 12, tabBottom()), y);
      if (row >= 0) st.setSel = row;
    }
    st.retMode = RdMode::Settings;  // 从这里进的子界面，Esc 回设置标签
    doMenuAction(items[st.setSel].act);
    st.dirty = 1;
    return;
  }
}

// ── 「应用」标签（1 号位）：三枚图标入口 ────────────────────────────────
// 文件管理（SD 卡浏览器）/ 微信读书 / 字典（查词页）。以前 1 号位直接就是文件浏览器，
// 另两个入口分别散在「设置」标签和阅读菜单里；现在收成一股，1 号位只做入口。
//
// 交互刻意跟书架标签一致：**左右键在这个标签里是"换选中项"**，不是换标签（书架用它们
// 翻封面，已经开了这个先例）。换标签靠点标签栏，或 Esc 回书架再用左右键。
static const int kAppCount = 3;
static const char *kAppNames[kAppCount] = {"文件管理", "微信读书", "字典"};

// 图标目标像素高。ICON_MAX_PX=112 是硬上限（超了字形被静默丢弃，画出来是空的），
// 这里留出余量；96 也比标签栏的 56 更适合"一排大图标"的入口页。
static int rdAppIconPx() { return 96; }
static int rdAppSlotW() { return g_rd.getScreenWidth() / kAppCount; }

// 图标框上沿：把"图标 + 标签"整块在正文区里居中（正文区 = 标签栏下沿到页脚上沿）。
static int rdAppIconTop() {
  const int bodyTop = coverTop();
  const int bodyBot = statusTop();   // 这一页有页脚（drawFooter），别把块排进页脚里
  const int block = rdAppIconPx() + 16 + uiLineHeight();
  int top = bodyTop + (bodyBot - bodyTop - block) / 2;
  if (top < bodyTop + 8) top = bodyTop + 8;
  return top;
}

// 点按命中哪一格：横向按整格切（手指落点糙，不瞄字形），纵向给整块留 12px 容差。
static int rdAppSlotAt(int x, int y) {
  const int top = rdAppIconTop();
  const int bot = top + rdAppIconPx() + 16 + uiLineHeight();
  if (y < top - 12 || y > bot + 12) return -1;
  const int s = x / rdAppSlotW();
  return (s >= 0 && s < kAppCount) ? s : -1;
}

// 按下某一枚入口。三个都不换标签（st.tab 停在 1），进去的子界面 Esc 都回这一页 ——
// 文件浏览器回在 handleFileBrowser 的卡根分支，另两个借 st.retMode（见 handleApps）。
static void rdOpenApp(int i) {
  switch (i) {
    case 0:
      rdEnterFileTab();   // 扫 SD + st.tab=1 + mode=FileBrowser
      return;
    case 1:
      st.retMode = RdMode::Apps;
      doMenuAction(MenuAct::Weread);
      break;
    case 2:
      st.retMode = RdMode::Apps;
      doMenuAction(MenuAct::Dict);
      break;
    default:
      return;
  }
  st.fullRefresh = true;
  st.dirty = 1;
}

static void renderApps() {
  g_rd.clearScreen();
  drawTabBar();
  const int slotW = rdAppSlotW();
  const int iconPx = rdAppIconPx();
  const int top = rdAppIconTop();
  uint8_t *fb = g_rd.getFrameBuffer();
  for (int i = 0; i < kAppCount; i++) {
    const int cx = i * slotW + slotW / 2;
    const int ix = cx - iconPx / 2;
    const bool sel = (i == st.appSel);
    if (sel) {
      // 选中 = 图标后面一块黑底 + 反白图标（跟标签栏/主菜单同一个idiom）。标签不反白：
      // 黑底只罩着图标，罩到文字会连成一片黑。
      const int pad = TAB_BOX_PAD + 4;
      g_rd.fillRect(ix - pad, top - pad, iconPx + 2 * pad, iconPx + 2 * pad, true);
    }
    if (i == 0) {
      if (fb) icon_font_draw_sized(fb, ix, top, iconPx, iconPx, TAB_ICON_FILES, sel, iconPx);
    } else if (i == 1) {
      if (fb) icon_font_draw_sized(fb, ix, top, iconPx, iconPx, BAR_ICON_WEREAD, sel, iconPx);
    } else {
      drawDictAppIcon(ix, top, iconPx, sel);
    }
    // 标签按**格子**居中（drawCenteredLine 是按整屏居中的，这儿用不上）。
    const char *nm = kAppNames[i];
    drawLineText(cx - g_rd.getTextWidth(uiFontId(), nm) / 2, top + iconPx + 16, nm, true);
  }
  drawFooter("←→ 选择  Enter 打开  Esc 返回书架");
}

static void handleApps(int key) {
  // 左右（和上下）都在这三枚之间挪：单行三格，四个方向键当同一个用，符合直觉。
  if (key == KEY_LEFT || key == KEY_UP) {
    st.appSel = (st.appSel + kAppCount - 1) % kAppCount;
    st.dirty = 1;
    return;
  }
  if (key == KEY_RIGHT || key == KEY_DOWN) {
    st.appSel = (st.appSel + 1) % kAppCount;
    st.dirty = 1;
    return;
  }
  if (key == 0x1B || key == KEY_LONG_CONFIRM) { switchTab(0); return; }
  if (key != '\n') return;
  // 点按坐标只读一次（input_tap_xy 读完即清），标签栏命中和格子命中共用这一份。
  int x = 0, y = 0;
  if (input_tap_xy(&x, &y)) {
    const int t = tabHit(x, y);
    if (t >= 0) { switchTab(t); return; }
    const int slot = rdAppSlotAt(x, y);
    if (slot < 0) return;
    st.appSel = slot;
  }
  rdOpenApp(st.appSel);
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
  auto add = [](const std::string &label, const std::string &value) {
    st.pickLabels.push_back(label);
    st.pickValues.push_back(value);
  };
  switch (static_cast<MenuAct>(act)) {
    case MenuAct::Font:
      // 字号档（kBodyPx 的像素高就是用户看得懂的那个数，菜单标签一直这么写）。
      st.pickTitle = "字号";
      for (int i = 0; i < kUserFontLevels; i++) add(std::to_string(kBodyPx[i]), std::to_string(i));
      break;
    case MenuAct::FontFamily:
      // 与「设置 → 字体」同一份清单：内建 + ttf_font_scan() 扫到的 SD 字体。值的字面量
      // 也必须一致（""=内建，其余=字体文件路径），否则弹层认不出当前档、● 会落错行。
      st.pickTitle = "字体";
      add("内建", "");
      {
        const int n = ttf_font_scan();
        for (int i = 0; i < n; i++) {
          const ttf_font_item_t *it = ttf_font_item(i);
          if (it) add(it->name, it->path);
        }
      }
      break;
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
    case MenuAct::ImageDither:
      st.pickTitle = "图片抖动";
      for (int i = 0; i < kRdDitherCount; i++) add(kRdDitherNames[i], kRdDitherKeys[i]);
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
      add(standbyFaceLabel(StandbyFace::Image), standbyFaceKey(StandbyFace::Image));
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
    case MenuAct::Font: return std::to_string(st.fontLevel);
    case MenuAct::FontFamily: {
      // 用户**设置里**选的那个字体（持久化的那份），不是 ttf_font_path()：后者读的是
      // 内容面，正在读的书有内嵌字体时它返回的是书里的字体文件，扫出来的 SD 字体一个都
      // 匹配不上，● 会跳回"内建"。与「设置 → 字体」用同一个来源（screen_settings.cpp）。
      const char *cur = font_store_get_path();
      return (cur && !ttf_font_path_is_builtin(cur)) ? std::string(cur) : std::string();
    }
    case MenuAct::ShelfStyle: return kShelfStyleKeys[clampI(static_cast<int>(shelfStyle()), 0, kShelfStyleCount - 1)];
    case MenuAct::StyleSource: return kStyleSrcKeys[clampI(styleSource(), 0, kStyleSrcCount - 1)];
    case MenuAct::EmbeddedFont: return kEmbFontKeys[clampI(embeddedFontMode(), 0, kEmbFontCount - 1)];
    case MenuAct::RefreshStrategy: return kRdRefreshKeys[clampI(refreshStrategy(), 0, kRdRefreshCount - 1)];
    case MenuAct::ImageDither: return kRdDitherKeys[clampI(st.imageDither, 0, kRdDitherCount - 1)];
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
    case MenuAct::Font:
      // 一次跳到目标档（原来要按好几次才轮到）。值就是档位下标。
      st.fontLevel = clampI(atoi(value.c_str()), 0, kUserFontLevels - 1);
      g_settings.setString("reader_font_level_v2", std::to_string(st.fontLevel));
      reopenBook();
      st.fullRefresh = true;
      break;
    case MenuAct::FontFamily:
      // 照搬原来循环版的收尾：落盘 → 立刻重新 ttf_font_open（GfxRenderer 直接调
      // ttf_font_*，开完即生效；字形度量变了所以还要重排）→ 打不开就回落内建。
      font_store_set_path(value.c_str());
      if (ttf_font_open(value.c_str()) != 0) {
        ESP_LOGW(TAG, "字体打开失败，回落内建: %s", value.c_str());
        (void)ttf_font_open_builtin();
        font_store_set_path("");
      }
      reopenBook();
      st.fullRefresh = true;
      break;
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
    case MenuAct::ImageDither: {
      // 不 reopenBook：抖动档进了 .pxc 缓存名，当前页下一帧取缓存时路径已经不同，
      // 自然落到解码分支重解一次，重排是白费的。
      g_settings.setString("reader_image_dither", value);
      for (int i = 0; i < kRdDitherCount; i++) {
        if (value == kRdDitherKeys[i]) st.imageDither = i;
      }
      ImageBlock::setDitherMode(ditherModeOf(st.imageDither));
      // 书架缩略图/待机封面共用这一档，而且它们的像素是量化后缓存进 PSRAM 的
      // （.pxc 那套"换档靠缓存名自然失效"在这儿不成立）—— 必须显式清。
      rdCoverThumbClearAll();
      rdShowFloat(std::string("图片抖动: ") + kRdDitherNames[clampI(st.imageDither, 0, kRdDitherCount - 1)],
                  "插图与书架封面都按新档重画", 1500);
      st.fullRefresh = true;
      st.dirty = 1;
      break;
    }
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
      // 刚切成「书籍封面」：开书那一趟故意没做的那张整屏封面，在这里补出来
      // （手上正好有这本书）。别的表盘不用这张图。
      // 这一趟要解原图+写盘（~3s），画面会停在设置页上，做完下一帧照常重绘。
      if (standbyFaceFromKey(value.c_str()) == StandbyFace::Cover) {
        rdBuildStandbyCoverForOpenBook();
      } else if (standbyFaceFromKey(value.c_str()) == StandbyFace::Image) {
        // 刚切成「图片」：这屏上没有选图的入口（选图在文件管理里），所以只在这里
        // 把缓存补出来 —— 转屏作废、清缓存之后都可能没有。一张都没选过就提示去哪选。
        const std::string img = g_settings.getString("standby_image");
        if (img.empty()) {
          rdShowFloat("还没选待机图片", "文件管理里长按一张图 → 设为待机画面", 2500);
        } else if (!Storage.exists(standbyImageCacheFor(SCREEN_W, SCREEN_H).c_str())) {
          std::string err;
          rdBuildStandbyImageCache(img, err);
          if (!err.empty()) rdShowFloat(std::string("待机图片: ") + err, "", 2000);
        }
      }
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


static void handleDict(int key) {
  if (key == 0x1B) {
    IME::getInstance().cancelComposition();
    st.vkVisible = false;
    // 从「应用」标签的字典图标进来的，Esc 回那一页；其余入口（阅读菜单 → 词典、
    // 长按选词 → 查字典）照旧回阅读菜单。
    if (st.retMode == RdMode::Apps) {
      st.mode = RdMode::Apps;
      st.retMode = RdMode::Browser;
    } else {
      st.mode = RdMode::Menu;
    }
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  // 释义正文滚动。触摸竖滑在 main.cpp/screen_reader_handle 里被展平成翻页键，
  // 词典和目录/书架一样保留原键（见 screen_reader_handle 的例外名单），所以
  // KEY_PAGE_UP/DOWN = 整屏翻页、KEY_UP/DOWN（BLE/方向键）= 单行。
  // 上限用渲染时算出的 dictScrollMax；键盘展开时竖滑起点落在面板上会被键盘先吃掉，
  // 收到这里的一定是正文区的滑动。
  if (key == KEY_UP) { st.dictScroll = std::max(0, st.dictScroll - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.dictScroll = std::min(st.dictScrollMax, st.dictScroll + 1); st.dirty = 1; return; }
  if (key == KEY_PAGE_UP) {
    st.dictScroll = std::max(0, st.dictScroll - std::max(1, st.dictPageLines));
    st.dirty = 1;
    return;
  }
  if (key == KEY_PAGE_DOWN) {
    st.dictScroll = std::min(st.dictScrollMax, st.dictScroll + std::max(1, st.dictPageLines));
    st.dirty = 1;
    return;
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      // 状态栏右端的键盘图标：先判它。它在键盘面板底边之下一点点，面板收起时才露出来；
      // 面板展开时点它则收起键盘（与写作模式键盘的开关同序，见 screen_editor.cpp）。
      if (rdVkIconHit(x, y)) {
        rdToggleVk();
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
  rdDrawSearchBar(false);   // 笔记搜索栏（放大镜 + 右端「导出」按钮）
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
      // 「导出」按钮排在栏体命中之前：两者在栏右端有一小段重叠，先判按钮才不会被
      // 当成"点了搜索框"（结果就是点了导出却弹出搜索）。
      if (rdNotesExportHit(x, yy)) { rdExportAnnot(); return; }
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
void renderNoteEdit() {
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

  const int bottom = rdBodyBottom();
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
  else {
    drawRdImeBar();   // 蓝牙键盘打字：候选条照样要有（见 drawRdImeBar）
    drawFooter("回车保存   Esc 取消");
  }
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
      if (rdVkIconHit(x, y)) { rdToggleVk(); return; }
      if (st.vkVisible && y >= vkVkTop()) { vkTap(x, y); st.dirty = 1; return; }
      if (st.vkVisible) { st.dirty = 1; return; }   // 键盘开着时点正文不提交
      rdNoteCommit();
      return;
    }
    // 无坐标回车 = 保存（同 handleShelfSearch：不顺手把键盘叫回来，面板开关归状态栏图标）。
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

// 搜索页结果列表的几何（书架搜索 / 笔记搜索共用）：居中式窗口，底边随虚拟键盘升降。
// top == rdDrawSearchHeader 的返回值（抬头那条分隔线之下）。渲染与点按命中共用。
static ListView searchListView(int count, int sel, int itemH) {
  ListView lv;
  lv.top = rdSearchListTop();
  lv.itemH = itemH;
  lv.count = count;
  lv.rows = std::max(1, (rdBodyBottom() - lv.top) / lv.itemH);
  lv.sel = sel;
  listViewCenter(lv);
  return lv;
}

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
  if (n == 0) {
    drawCenteredLine(top + 30, "没有匹配的书");
  } else {
    const ListView lv = searchListView(n, st.searchSel, uiLineHeight() + 6);
    st.searchSel = lv.sel;  // 照旧把夹好的选中项写回（渲染顺带做归一）
    const int itemH = lv.itemH;
    const int maxRows = lv.rows;
    const int start = lv.first;
    for (int i = 0; i < maxRows && start + i < n; i++) {
      const int idx = hits[start + i];
      const int y = lv.top + i * itemH;
      const bool sel = (start + i == st.searchSel);
      if (sel) g_rd.fillRect(0, y, w, itemH, true);
      // 命中结果用 UI 字体画（书名可能含 builtin 子集外的字，但这里和书架列表一致，
      // 保持同一套排版；换字体反而会让搜索结果和书架看着不一样）。
      std::string nm = g_rd.truncatedText(uiFontId(), st.books[idx].name.c_str(), w - 2 * MARGIN);
      drawLineText(MARGIN, y + 3, nm.c_str(), !sel);
    }
  }
  if (st.vkVisible) drawVk();
  else {
    drawRdImeBar();   // 搜索框在打拼音：编码行 + 候选行
    drawFooter("↑↓ 选择  Enter 打开  Esc 取消");
  }
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
  {  // 上下/翻页在 ui/list_view.h（与 renderShelfSearch 共用同一个几何）
    ListView lv = searchListView(static_cast<int>(rdShelfHits().size()), st.searchSel, uiLineHeight() + 6);
    if (listViewKey(lv, key)) { st.searchSel = lv.sel; st.dirty = 1; return; }
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      if (rdVkIconHit(x, y)) { rdToggleVk(); return; }
      if (st.vkVisible && y >= vkVkTop()) { vkTap(x, y); st.dirty = 1; return; }
      // 键盘面板之上（或键盘收起时）点结果行 → 直接打开那一本
      const int row = listViewHitAt(searchListView(static_cast<int>(rdShelfHits().size()), st.searchSel, uiLineHeight() + 6), y);
      if (row >= 0) {
        st.searchSel = row;
        rdShelfSearchOpen();
        return;
      }
      st.dirty = 1;
      return;
    }
    // 无坐标回车（蓝牙键盘的回车，或面板上那个回车）：打开当前命中。**这里不再尝试
    // 把虚拟键盘叫回来**——面板的开/关由用户在状态栏图标上定，rdVkWantShow() 现在
    // 一律置可见，留着那句会让"收起键盘后按回车确认"变成"键盘又弹回来"。
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
  if (n == 0) {
    drawCenteredLine(top + 30, "没有匹配的笔记");
  } else {
    // 行高两行：原文 / 《书名》+笔记正文
    const ListView lv = searchListView(n, st.searchSel, uiLineHeight() * 2 + 6);
    st.searchSel = lv.sel;  // 照旧把夹好的选中项写回（渲染顺带做归一）
    const int itemH = lv.itemH;
    const int maxRows = lv.rows;
    const int start = lv.first;
    const int maxW = w - 2 * MARGIN - 12;
    for (int i = 0; i < maxRows && start + i < n; i++) {
      const RdState::RdNote &nt = st.notes[hits[start + i]];
      const int y = lv.top + i * itemH;
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
  else {
    drawRdImeBar();   // 搜索框在打拼音：编码行 + 候选行
    drawFooter("↑↓ 选择  Enter 跳转  Esc 取消");
  }
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
  {  // 上下/翻页在 ui/list_view.h（与 renderNotesSearch 共用同一个几何）
    ListView lv = searchListView(static_cast<int>(rdNotesHits().size()), st.searchSel, uiLineHeight() * 2 + 6);
    if (listViewKey(lv, key)) { st.searchSel = lv.sel; st.dirty = 1; return; }
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      if (rdVkIconHit(x, y)) { rdToggleVk(); return; }
      if (st.vkVisible && y >= vkVkTop()) { vkTap(x, y); st.dirty = 1; return; }
      const int row = listViewHitAt(
          searchListView(static_cast<int>(rdNotesHits().size()), st.searchSel, uiLineHeight() * 2 + 6), y);
      if (row >= 0) {
        st.searchSel = row;
        rdNotesSearchOpen();
        return;
      }
      st.dirty = 1;
      return;
    }
    // 同 handleShelfSearch：回车就是"打开命中"，不顺手把键盘叫回来。
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
    // 五个根标签各有自己的常驻界面，直接按标签回来。
    case RdMode::Browser:
    case RdMode::Apps:
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
    st.tab = 1;   // 文件浏览器挂在 1 号位（应用标签）下面
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
  // 这儿**不再**对 1 号位补一次 rdEnterFileTab：1 号位现在是「应用」的图标入口页，
  // 不需要扫 SD（列表是编译期定死的三项）。真正停在文件浏览器里的那一趟，上面
  // RdMode::FileBrowser 那条分支已经把目录扫回来了。
  ESP_LOGI(TAG, "返回标签 %d，选中 %d", st.tab, st.sel);
}

// ── 公开入口 ────────────────────────────────────────────────────────────

void screen_reader_init() {
  // 阅读器整条绘制路径**绕过** core1 的渲染任务：GfxRenderer 画的就是 epdiy 的
  // front_fb，刷屏直接调 epd_hl_update_screen（都是本任务、同步）。所以进阅读器前
  // 必须等在飞的那一帧推完 —— 否则 core0 一边画 front_fb、core1 一边在
  // epd_hl_update_* 里读它，既会撕裂画面，也会踩坏 epdiy 的单份全局 render_context。
  //
  // 光 drain 不够：渲染任务里还挂着两份**会被它自己后期消费**的记账 —— 输入法那两行
  // 的残影清理（s_clean_dirty，到点由 ime_clean_tick 在 core1 直呼 epdiy）和延后的
  // 局刷（s_ime_deferred）。drain 只等"队列空 + 缓冲归还"，这两份记账不清；若上一个
  // 界面刚打完字就切进来，core1 会在这之后自己去 epd_hl_update_area_full，与 core0 的
  // 阅读器刷新同时进 epdiy。先 invalidate 让渲染任务把它们丢掉，再 drain 等它跑完。
  ui_render_invalidate();
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

  // g_rd.begin() 之后 frameBuffer 才有效，立刻把 u8g2 shim 也钉上去（理由见 rdPinShimFb）。
  rdPinShimFb();

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
  st.imageDither = imageDitherIndex();
  ImageBlock::setDitherMode(ditherModeOf(st.imageDither));
  st.night = g_settings.nightMode();  // 全设备夜间（旧键 reader_night 由访问器迁移）
  board_set_night(st.night);          // 进阅读模式时套用一次，保证与其他界面同向
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

// main.cpp 的 kScreens 生命周期钩子：就是上面两个函数的适配层（签名要能塞进表里）。
void screen_reader_enter(ScreenContext &ctx) {
  (void)ctx;
  screen_reader_init();
}

void screen_reader_leave(AppState next) {
  (void)next;
  screen_reader_exit();
}

AppState screen_reader_handle(int key, ScreenContext &ctx) {
  (void)ctx;

  // 一拍只认**这一拍**里 vkTap 设下的"可以走增量帧"。跨拍留着的话，中途任何别的
  // 状态变化（另一次分发改的东西）都会被那面"只重画键盘"的旗子瞒过去。
  s_vk_incr_ok = false;

  // 虚拟键盘"上屏刷法=快"欠下的正文那块：停手到点就在这儿坐实（区域 GC16，约 336ms）。
  // **放在所有早退之前**：下面还有两条 VK 滑动早退（候选行横划/面板竖滑）和一个
  // 微信读书任务分支，放它们后面会让这次清理被那些帧绕过、拖到不知道什么时候。
  // 空转帧也要过 —— "没人按键"正是"用户停手了"的证据。见 ui_render.h。
  ui_render_reader_vk_settle_tick();

  // 用户动不动手的锚点（书架空闲预建用）：任何非空按键都算，包括震动全刷、长按、
  // 微信读书那几拍 tick。空转 tick（key==0）不算——空闲帧本来就是"没动手"的证据。
  if (key != 0) {
    s_rdLastInputUs = esp_timer_get_time();
    s_rdSawKey = true;
    // 用户又动手了 → 挂起的弹注作废（他多半是等不及、点了别处）。挂起态只活在
    // "没人按键"的那些空闲帧里，这样它绝不会在用户已经翻到别处之后突然弹出来。
    if (st.fnWaitIdx >= 0) rdFootnoteWaitCancel();
  }

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
  // 这样空闲帧一进来状态就是对的。**但它跑在按键分发之前，用的是上一拍的 st.mode**，
  // "这一拍要进哪个界面"恰恰是分发决定的 —— 真正必需的那一次同步在渲染之前（见下面），
  // 这里留着只是为了让空闲帧不必等到渲染那一步。
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
    // 书架空闲预建（把冷开一本书的一次性产物提前做掉，见函数头）。放在排版余量后面：
    // 阅读页里 rdPrebuildAhead 才是主角，书架那一支它自己会早退。
    rdShelfIdlePrebuild();
    // 待机「图片」表盘的缓存补做（转屏后 / 网页设的图）。它自己带停手闸，别被这里的
    // "空闲帧"名义骗了——里面解一张大图是秒级的。
    rdStandbyImageIdlePrebuild();
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
  // "一行一行挪"都走不动，上下滑要整页翻。**词典**也认它：释义正文常常整屏放不下，
  // 上下滑要按屏滚（handleDict 收原键）。其余子界面（正文/菜单/书签/笔记…）保持
  // 原来的单步上下语义。
  if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
    if (st.mode != RdMode::Toc && st.mode != RdMode::Browser && st.mode != RdMode::Dictionary)
      key = (key == KEY_PAGE_UP) ? KEY_UP : KEY_DOWN;
  }

  // 网络传输心跳（按键帧和空闲帧都过这里）。放分发之前：它只置 st.dirty，不碰键。
  rdNetTick();

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
    case RdMode::LayoutMenu: handleLayoutMenu(key); break;
    case RdMode::Bookmarks: handleBookmarks(key); break;
    case RdMode::Footnotes: handleFootnotes(key); break;
    case RdMode::Percent: handlePercent(key); break;
    case RdMode::Qr: handleQr(key); break;
    case RdMode::Apps: handleApps(key); break;
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
    case RdMode::RefreshTest: handleRefreshTest(key); break;
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

  // **必需的**一次输入法同步：分发已经做完，这一拍要停在哪个界面已成定局，此时再对齐
  // 一次，首帧画出来就是对的。少了这一句，"从菜单点进词典/笔记"那一帧的 want 是按
  // 上一拍（菜单）算的 = false → IME 是关的 → 虚拟键盘画出 26 键的键位，可布局键上
  // 写的却是用户选的"14键/18键"（标签读 s_layout，与 evkAmbig() 无关），直到用户随便
  // 按一下、下一拍 want 才变成 true，键位才跳成他选的那套。
  rdSyncImeActive();
  if (st.dirty) renderCurrent();
  // 阅读页的位置一有变化就落盘（位置没变时 rdRememberProgress 自己会早退，不写 SD）。
  if (st.mode == RdMode::Reading) rdRememberProgress(false);
  // 统计记时：一次按键只记一笔（翻页/滚动/弹菜单都算），不在 turnBook 里另记。
  if (statsModeBefore == RdMode::Reading) rdStatsNoteActivity();
  return APP_READER;
}
