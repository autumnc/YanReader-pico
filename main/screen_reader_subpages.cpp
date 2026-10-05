// screen_reader_subpages.cpp — 阅读「设置」标签下面那一族叶子界面。
//
// P3b 的第三个切片。这些都是从「设置」标签或阅读菜单点进去、**只出不进**的子界面：
// 词典页（renderDict）、WiFi（renderWifi）、OPDS 书库、词典下载、字体下载、BLE 按键
// 映射、自定义状态栏、关于页、WiFi 传书（NetShare）。每个都是「st.retMode 单层回退 +
// st.fullRefresh/st.dirty 触发重绘」的三联形状，彼此不互相调用，主文件只在分发
// （renderCurrent / screen_reader_handle）里认它们的入口。
//
// 搬过来时**逻辑一行没改**：只动了 static、include，并把 20 个入口提到
// screen_reader_internal.h；它们用到的阅读器原语（VK/IME 条、renderCurrent…）
// 由同一份内部头声明。
//
// 唯一留在主文件的同族函数是 applyReaderOrientation / applyNightMode：那两个是阅读器
// 本体的全局朝向与夜间模式，不是"点进去的子界面"，被 init / 阅读页大量调用。

#include "screen_reader_internal.h"   // st / g_rd / 外壳原语
#include "screen_reader.h"            // readerEnsureWifi / 待机那两个口
#include "dictionary_store.h"
#include "opds_client.h"
#include "settings_manager.h"
#include "wifi_manager.h"
#include "ble_keymap.h"
#include "reading_stats.h"
#include "ui/list_view.h"
#include "ui/ime_field.h"
#include "ime/IME.h"
#include "hw/input.h"
#include "hw/board.h"
#include "bt_keyboard.h"           // g_bt：连接状态（WiFi 传书/关于页显示）
#include "file_manager_server.h"   // WiFi 传书：起停那个 httpd

#include <algorithm>
#include <cstdio>
#include <string>
#include <sys/statvfs.h>
#include <vector>

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <DitherUtils.h>   // 自检页的抖动三档对比条直接调真的 grayToLevel16（header-only）
#include <esp_chip_info.h>
#include <esp_mac.h>
#include <esp_timer.h>

// 释义折行的行数上限。正常词条几十行封顶，这里只是防病态输入把行表撑爆。
static const int kDictMaxLines = 2000;

// 折行缓存的操作是"谁改结果谁作废"：doDictLookup 换了词条、设置里改了字号（排版宽度变），
// 都调这个。清空 + 置 dictLinesW=-1，下一帧 renderDict 会按新宽度重建。
void rdDictResetScroll() {
  st.dictScroll = 0;
  st.dictLinesW = -1;
  st.dictLines.clear();
}

void renderDict() {
  g_rd.clearScreen();
  int top = drawTitle("词典");
  int qy = top;
  int qh = dictQueryH();
  g_rd.drawRect(MARGIN, qy, g_rd.getScreenWidth() - 2 * MARGIN, qh, true);
  std::string q = st.dictQuery.empty() ? "点击输入单词/拼音" : st.dictQuery;
  drawLineText(MARGIN + 8, qy + 6, q.c_str(), true);
  int ry = qy + qh + 12;
  int bottom = rdBodyBottom();
  if (!st.dictStatus.empty()) { drawLineText(MARGIN, ry, st.dictStatus.c_str(), true); ry += uiLineHeight() + 6; }
  if (!st.dictResult.empty()) {
    const int maxW = g_rd.getScreenWidth() - 2 * MARGIN;
    // 折行只算一次：宽度没变就复用上一帧的行表（长释义逐帧重新量宽度会拖慢滑动/打字）。
    if (st.dictLinesW != maxW) {
      st.dictLinesW = maxW;
      // 上限只是防病态输入（几万行会把 PSRAM 里的 vector 撑爆），正常释义远到不了。
      st.dictLines = g_rd.wrappedText(uiFontId(), st.dictResult.c_str(), maxW, kDictMaxLines);
    }
    const int lineH = uiLineHeight() + 4;
    const int maxLines = std::max(1, (bottom - ry) / lineH);
    const int total = static_cast<int>(st.dictLines.size());
    // 夹住首行偏移：翻页键只管加，上限在这里兜底（与脚注弹窗同一套做法）。
    int first = std::max(0, std::min(st.dictScroll, std::max(0, total - maxLines)));
    st.dictScroll = first;
    st.dictScrollMax = std::max(0, total - maxLines);
    st.dictPageLines = maxLines;
    const int last = std::min(total, first + maxLines);
    int y = ry;
    for (int i = first; i < last; i++) {
      drawLineText(MARGIN, y, st.dictLines[i].c_str(), true);
      y += lineH;
    }
    // 翻页指示：只有真看不全时才画。画在状态行右端（"词条: xxx"在同一行的左边，
    // 两者不会撞——状态文字右端与屏右边留 margin 的距离足够放一个 "12/34"）。
    if (total > maxLines && !st.dictStatus.empty()) {
      char ind[24];
      const int pageCount = (total + maxLines - 1) / maxLines;
      snprintf(ind, sizeof(ind), "%d/%d", first / maxLines + 1, pageCount);
      const int tw = g_rd.getTextWidth(uiFontId(), ind);
      drawLineText(g_rd.getScreenWidth() - MARGIN - tw, ry - uiLineHeight() - 6, ind, true);
    }
  }
  if (st.vkVisible) drawVk();
  else {
    drawRdImeBar();   // 蓝牙键盘打字：编码行 + 候选行（见 drawRdImeBar）
    drawFooter(st.dictScrollMax > 0 ? "上下划翻页  点查询框输入  Enter 查词  Esc 返回"
                                    : "点查询框输入  Enter 查词  Esc 返回");
  }
  rdDrawVkIcon();
}

// ── WiFi 管理 ────────────────────────────────────────────────────────────
static const int kWifiRows = 4;  // SSID / 密码 / 连接 / 断开

void renderWifi() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int top = drawTitle("WiFi 管理");
  const ListView lv = flatMenuListView(kWifiRows, st.wifiField, rdListTop());
  int itemH = lv.itemH;
  // 状态行
  std::string status;
  if (st.wifiBusy) status = "连接中...";
  else if (!st.wifiStatus.empty()) status = st.wifiStatus;
  else if (g_wifi.isConnected()) { std::string ip = g_wifi.getIp(); status = ip.empty() ? "已连接" : ("已连接  IP " + ip); }
  else status = "未连接";
  drawLineText(MARGIN, top, status.c_str(), true);
  int ly = lv.top;  // 状态行下的分隔线
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
  if (st.wifiEditing) {
    drawRdImeBar();      // 蓝牙键盘打字：编码行 + 候选行
    rdDrawVkIcon();      // 只有编辑字段（键盘可用）时才给开关
  }
}

void handleWifi(int key) {
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
          rdToggleVk();
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
  {  // 上下选择在 ui/list_view.h（与 renderWifi 共用同一个几何）
    ListView lv = flatMenuListView(kWifiRows, st.wifiField, rdListTop());
    if (listViewKey(lv, key)) { st.wifiField = lv.sel; st.wifiStatus.clear(); st.dirty = 1; return; }
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      const int row = listViewHitAt(flatMenuListView(kWifiRows, st.wifiField, rdListTop()), y);
      if (row >= 0) st.wifiField = row;
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

// 按屏幕宽度截断（超出加省略号），避免 e-ink 上文字画出屏外。
std::string fitWidth(const std::string &s, int maxW) {
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

void opdsBeginLoad(const std::string &url) {
  st.opdsStack.clear();
  opdsLoadFeed(url);
}

// OPDS 列表几何：渲染与点按命中共用（末行是"地址"行，所以 count = 条目数 + 1）。
// 窗口是居中式 —— 选中项尽量停在中间。foot 是底部状态栏上沿（这个界面的列表比
// 别的界面短一条状态栏）。
static ListView opdsListView() {
  ListView lv;
  lv.top = rdListTop();
  lv.itemH = uiLineHeight() + 12;
  lv.count = static_cast<int>(st.opdsEntries.size()) + 1;
  lv.rows = std::max(1, (statusTop() - lv.top - 8) / lv.itemH);
  lv.sel = st.opdsSel;
  listViewCenter(lv);
  return lv;
}

void renderOpds() {
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
    else {
      drawRdImeBar();   // 蓝牙键盘打字：编码行 + 候选行
      drawFooter("输入中  Enter 完成  Esc 取消");
    }
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
  const ListView opdslv = opdsListView();  // 末行 = 地址行
  int ly = opdslv.top;                     // 列表首行上沿（与点按命中同一个式子）
  g_rd.drawLine(0, ly, w, ly, true);

  int n = opdslv.count;
  int maxRows = opdslv.rows;
  int start = opdslv.first;
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

void handleOpds(int key) {
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
          rdToggleVk();
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
  {  // 上下/翻页/首尾：算术在 ui/list_view.h（与渲染同一个 opdsListView 几何）
    ListView lv = opdsListView();
    if (listViewKey(lv, key)) { st.opdsSel = lv.sel; st.dirty = 1; return; }
  }
  if (key != '\n') return;

  int x, y;
  if (input_tap_xy(&x, &y)) {
    // 点哪行选哪行：窗口起点/行高都来自 opdsListView（以前这里把渲染的窗口公式
    // 抄了一遍，两边一旦不同步就是"点哪行选隔壁那行"）。
    const int row = listViewHitAt(opdsListView(), y);
    if (row >= 0) st.opdsSel = row;
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
bool readerEnsureWifi(std::string &err) {
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
  rdDictCacheClear();   // 词典文件换了，缓存里的正文是上一本词典的
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

void dictDlLoadCatalog() {
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

// 词典下载列表：居中式窗口，渲染与点按命中共用（以前两边各写一遍式子）。
static ListView dictDlListView() {
  ListView lv;
  lv.top = rdListTop();
  lv.itemH = uiLineHeight() + 12;
  lv.count = dictDlRowCount();
  lv.rows = std::max(1, (statusTop() - lv.top - 8) / lv.itemH);
  lv.sel = st.dictDlSel;
  listViewCenter(lv);
  return lv;
}

void renderDictDl() {
  if (st.dictDlBusy && st.dictDlTotal > 0) {
    renderDictDlProgress();
    return;
  }
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int top = drawTitle("词典下载");

  std::string status;
  if (st.dictDlBusy) status = "网络请求中...";
  else if (!st.dictDlStatus.empty()) status = st.dictDlStatus;
  else if (!g_wifi.isConnected()) status = "未连接 WiFi（先到 WiFi 管理连接）";
  else status = "正在获取清单...";
  drawLineText(MARGIN, top, fitWidth(status, w - 2 * MARGIN).c_str(), true);
  const ListView lv = dictDlListView();
  int ly = lv.top;
  g_rd.drawLine(0, ly, w, ly, true);

  int n = lv.count;
  int itemH = lv.itemH;
  int maxRows = lv.rows;
  int start = lv.first;
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

void handleDictDl(int key) {
  if (st.dictDlBusy) return;  // 阻塞请求期间不接受输入
  if (key == 0x1B) {
    st.dictDlDelArm.clear();
    st.mode = st.retMode;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  {  // 上下/翻页在 ui/list_view.h；动一行就撤掉删除待确认
    ListView lv = dictDlListView();
    if (listViewKey(lv, key)) { st.dictDlSel = lv.sel; st.dictDlDelArm.clear(); st.dirty = 1; return; }
  }

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
    const int row = listViewHitAt(dictDlListView(), y);
    if (row >= 0) st.dictDlSel = row;
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

void renderResDl() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int top = drawTitle("资源下载");
  const ListView lv = flatMenuListView(kResRows, st.resField, rdListTop());
  int itemH = lv.itemH;
  std::string status;
  if (st.resBusy) status = "正在下载字体 " + std::to_string(st.resPct) + "%";
  else if (!st.resStatus.empty()) status = st.resStatus;
  else if (!g_wifi.isConnected()) status = "未连接 WiFi（下载前会先连）";
  else { std::string ip = g_wifi.getIp(); status = ip.empty() ? "已连接" : ("已连接  IP " + ip); }
  drawLineText(MARGIN, top, fitWidth(status, w - 2 * MARGIN).c_str(), true);
  int ly = lv.top;  // 状态行下的分隔线
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
  if (st.resEditing) {
    drawRdImeBar();      // 蓝牙键盘打字：编码行 + 候选行
    rdDrawVkIcon();      // 只有编辑字段（键盘可用）时才给开关
  }
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

void handleResDl(int key) {
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
        if (rdVkIconHit(x, y)) { rdToggleVk(); return; }
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
  {  // 上下选择在 ui/list_view.h（与 renderResDl 共用同一个几何）
    ListView lv = flatMenuListView(kResRows, st.resField, rdListTop());
    if (listViewKey(lv, key)) { st.resField = lv.sel; st.resStatus.clear(); st.dirty = 1; return; }
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      const int row = listViewHitAt(flatMenuListView(kResRows, st.resField, rdListTop()), y);
      if (row >= 0) st.resField = row;
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
void renderKeyMap() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int top = drawTitle("按键映射");

  std::string status;
  if (st.keyMapCapture >= 0) status = "请按遥控器/键盘上要绑定的键…（Esc 取消）";
  else if (!st.keyMapStatus.empty()) status = st.keyMapStatus;
  else if (!g_bt.isConnected()) status = "未连接蓝牙设备（先到「蓝牙管理」配对）";
  else status = "选中一行按 Enter，再按要绑定的键";
  drawLineText(MARGIN, top, fitWidth(status, w - 2 * MARGIN).c_str(), true);
  const ListView lv = flatMenuListView(BLE_ACT_COUNT, st.keyMapSel, rdListTop());
  int itemH = lv.itemH;
  int ly = lv.top;  // 状态行下的分隔线
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

void handleKeyMap(int key) {
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
  {  // 上下/首尾在 ui/list_view.h（8 项 < 翻页步长 8，PAGE 仍是到首/到尾）
    ListView lv = flatMenuListView(BLE_ACT_COUNT, st.keyMapSel, rdListTop());
    if (listViewKey(lv, key)) { st.keyMapSel = lv.sel; st.dirty = 1; return; }
  }

  if (key != '\n') return;

  int x, y;
  if (input_tap_xy(&x, &y)) {
    const int row = listViewHitAt(flatMenuListView(BLE_ACT_COUNT, st.keyMapSel, rdListTop()), y);
    if (row >= 0) st.keyMapSel = row;
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

// 自定义状态栏：居中式窗口，渲染与点按命中共用（以前两边各写一遍式子）。
static ListView sbListView() {
  ListView lv;
  lv.top = coverTop();  // == drawTitle 的返回值
  lv.itemH = uiLineHeight() + 12;
  lv.count = kSbRowCount;
  lv.rows = std::max(1, (statusTop() - lv.top - 8) / lv.itemH);
  lv.sel = st.sbSel;
  listViewCenter(lv);
  return lv;
}

void renderStatusBarSet() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  drawTitle("状态栏");
  const ListView lv = sbListView();
  int itemH = lv.itemH;
  int n = lv.count;
  int maxRows = lv.rows;
  int start = lv.first;
  for (int i = 0; i < maxRows && start + i < n; i++) {
    int idx = start + i;
    const SbRow &r = kSbRows[idx];
    int v = sbCount(r.key, r.def, r.nameCount);
    int y = lv.top + i * itemH;
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

void handleStatusBarSet(int key) {
  // Esc 直接回阅读页：这些是显示项，回不去就看不到效果，就地返回最顺手。
  // 但若是从「设置」标签进来的（retMode==Settings），那里没有阅读页可回，退回设置标签。
  if (key == 0x1B) {
    st.mode = (st.retMode == RdMode::Settings) ? RdMode::Settings : RdMode::Reading;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  {  // 上下/首尾在 ui/list_view.h（7 项 < 翻页步长 8，PAGE 仍是到首/到尾）
    ListView lv = sbListView();
    if (listViewKey(lv, key)) { st.sbSel = lv.sel; st.dirty = 1; return; }
  }

  if (key != KEY_LEFT && key != KEY_RIGHT && key != '\n') return;

  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {  // 点按：先选中该行，不直接改值
      const int row = listViewHitAt(sbListView(), y);
      if (row >= 0) st.sbSel = row;
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
  add("主题", "研读 | 研墨 | 研行");
  add("版本", YAN_READER_VERSION "  (" __DATE__ " " __TIME__ ")");

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

// 关于页：纯滚动（aboutTop 就是顶行下标）。渲染与按键共用同一个几何——以前两边各夹一次。
static ListView aboutListView(int count) {
  ListView lv;
  lv.top = coverTop();  // == drawTitle 的返回值
  lv.itemH = uiLineHeight() + 8;
  lv.count = count;
  lv.rows = std::max(1, (statusTop() - lv.top - 8) / lv.itemH);
  lv.first = st.aboutTop;
  listViewScroll(lv);
  return lv;
}

void renderAbout() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  drawTitle("关于");

  std::vector<AboutRow> rows;
  fillAboutRows(rows);
  const ListView lv = aboutListView(static_cast<int>(rows.size()));
  int n = lv.count;
  int itemH = lv.itemH;
  int maxRows = lv.rows;
  int start = lv.first;

  for (int i = 0; i < maxRows && start + i < n; i++) {
    const AboutRow &r = rows[start + i];
    int y = lv.top + i * itemH;
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

void handleAbout(int key) {
  if (key == 0x1B) { st.mode = st.retMode; st.fullRefresh = true; st.dirty = 1; return; }
  int maxRows = aboutListView(kAboutRowCount).rows;  // 一屏行数（与渲染同一个几何）
  int maxTop = std::max(0, kAboutRowCount - maxRows);
  if (key == KEY_UP) { st.aboutTop = std::max(0, st.aboutTop - 1); st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.aboutTop = std::min(maxTop, st.aboutTop + 1); st.dirty = 1; return; }
  if (key == KEY_PAGE_UP) { st.aboutTop = std::max(0, st.aboutTop - maxRows); st.dirty = 1; return; }
  if (key == KEY_PAGE_DOWN) { st.aboutTop = std::min(maxTop, st.aboutTop + maxRows); st.dirty = 1; return; }
}

// ── 灰阶自检页 ──────────────────────────────────────────────────────────
//
// 移植官方固件 app_refresh.c 的那块自检画面：把"这块屏到底能表达什么"一次性摆出来。
//   ① 16 级灰阶梯 —— **(a) 真 16 级写回的主验收**。这块面板的 LUT 会把 16 级并成
//      **约 11 级可分辨**（E0470_WAVEFORM 只定义 type=1/2/5，没有 DU4/GL4），所以能数出
//      ~11 根台阶就是正常，别当成 bug 去追。
//   ② 实心块 / 1px 描边块对 —— 灰阶边缘。
//   ③ 8px 棋盘 + 1px 细线 —— 对齐、串扰、细线保持。
//   ④ 文字锐度行 —— 当前正文字号下的中英混排边缘。
//   ⑤ 抖动三档对比条 —— **(d) 抖动档的主验收**：同一条 0→255 渐变用 Ordered / Row /
//      None 三档各画一遍（调的是**真的** grayToLevel16，不是复制一份公式）。
//   底部一行是"自推屏"动作：用五种刷法把**同一份 framebuffer** 各推一遍并当场记下耗时
//   （实现在 display.c 的 reader_refresh_test_present —— 这个 TU 不碰波形/模式类型，
//   见 reader_refresh_bridge.h）。
//
// 自检页自己推屏，所以 renderCurrent 的常规推屏段必须让路：st.rtPending >= 0 的那一帧
// 走 reader_refresh_test_present，不再走 g_rd.displayBuffer（照 s_vk_incr_ok 的先例）。
static const char *kRtActionNames[] = {
    "整屏 GC16（默认表）", "8 灰阶表整屏", "16 灰 from-white", "8 灰阶 from-white", "上半屏 DU",
};
static const int kRtActionCount = 5;

void renderRefreshTest() {
  g_rd.clearScreen();
  // 这一页整屏都是灰阶图元：面板上留下的就是中灰（白底参考帧纪律要记账，见 st.frameGray）。
  st.frameGray = 1;

  const int w = g_rd.getScreenWidth();
  const int M = MARGIN;
  const int lh = uiLineHeight();
  const int innerW = w - 2 * M;

  // 灰阶图元：整块填 / 1px 线（都走 drawGrayscale16Pixel，与 XTC 渲染同一条路）。
  auto fillGray = [&](int x, int y, int bw, int bh, int level) {
    for (int yy = 0; yy < bh; yy++)
      for (int xx = 0; xx < bw; xx++)
        g_rd.drawGrayscale16Pixel(x + xx, y + yy, static_cast<uint8_t>(level));
  };
  auto hLineGray = [&](int x, int y, int bw, int level) {
    for (int xx = 0; xx < bw; xx++) g_rd.drawGrayscale16Pixel(x + xx, y, static_cast<uint8_t>(level));
  };
  auto vLineGray = [&](int x, int y, int bh, int level) {
    for (int yy = 0; yy < bh; yy++) g_rd.drawGrayscale16Pixel(x, y + yy, static_cast<uint8_t>(level));
  };

  int y = drawTitle("屏幕自检") + 6;
  const int infoRowH = lh + 6;
  const int bodyBottom = statusTop() - infoRowH - 6;
  // 一屏的高度预算：横屏（684 高）是紧的那一档，下面这些基准值是照它定的；竖屏富余很多，
  // 按 sc 把各块等比放大（上限 1.9 倍，免得拉开得很难看）。
  const int base = 6 * lh + 40 + 198;
  int availH = bodyBottom - y;
  float sc = (base > 0) ? static_cast<float>(availH) / base : 1.0f;
  if (sc < 1.0f) sc = 1.0f;
  if (sc > 1.9f) sc = 1.9f;

  // ① 16 级灰阶梯 —— 0 = 全墨 … 15 = 全白。
  drawLineText(M, y, "① 16 级灰阶梯（本屏预期约 11 级可分辨）", true);
  y += lh + 2;
  {
    const int barH = static_cast<int>(70 * sc);
    const int barW = innerW / 16;
    for (int i = 0; i < 16; i++) {
      const int bx = M + i * barW;
      const int bw = (i == 15) ? (innerW - 15 * barW) : barW;  // 最后一根补齐余数
      fillGray(bx, y, bw, barH, i);
    }
    y += barH + 6;
  }

  // ② 实心块 / 1px 描边块（同灰度的两种边缘形态）。
  drawLineText(M, y, "② 实心块 / 1px 描边块", true);
  y += lh + 2;
  {
    const int blkH = static_cast<int>(40 * sc);
    const int blkW = std::min(72, std::max(24, innerW / 10));
    const int levels[4] = {3, 6, 9, 12};
    int bx = M;
    for (int k = 0; k < 4; k++) {
      const int L = levels[k];
      fillGray(bx, y, blkW, blkH, L);
      bx += blkW + 8;
      if (bx + blkW > w - M) break;
      fillGray(bx, y, blkW, blkH, 15);  // 白底 + 1px 描边
      hLineGray(bx, y, blkW, L);
      hLineGray(bx, y + blkH - 1, blkW, L);
      vLineGray(bx, y, blkH, L);
      vLineGray(bx + blkW - 1, y, blkH, L);
      bx += blkW + 10;
    }
    y += blkH + 6;
  }

  // ③ 8px 棋盘（左半） + 1px 细线（右半）。
  drawLineText(M, y, "③ 棋盘（8px） / 1px 细线", true);
  y += lh + 2;
  {
    const int h3 = static_cast<int>(34 * sc);
    const int halfW = innerW / 2 - 12;
    const int cell = std::max(4, h3 / 4);
    for (int yy = 0; yy < h3; yy++)
      for (int xx = 0; xx < halfW; xx++)
        g_rd.drawGrayscale16Pixel(M + xx, y + yy,
                                  (((xx / cell) + (yy / cell)) & 1) ? 0 : 15);
    const int lx0 = M + halfW + 24;
    const int nLines = 20;
    for (int k = 0; k < nLines; k++) vLineGray(lx0 + k * (halfW / nLines), y, h3, 0);
    y += h3 + 6;
  }

  // ④ 文字锐度（当前正文字号）。
  drawLineText(M, y, "④ 文字锐度（当前字号，中英混排）", true);
  y += lh + 2;
  drawLineText(M, y, "永和九年，岁在癸丑 Hunan 0123456789 菜单设置", true,
               BODY_FONT_ID_BASE + st.fontLevel);
  y += lh + 6;

  // ⑤ 抖动三档对比：同一条 0→255 渐变，上→下 = 有序 / 行扩散 / 关。
  drawLineText(M, y, "⑤ 抖动三档对比 上→下：有序 / 行扩散 / 关", true);
  y += lh + 2;
  {
    const int stripH = std::max(4, static_cast<int>(54 * sc) / 3);
    const DitherMode modes[3] = {DitherMode::Ordered, DitherMode::Row, DitherMode::None};
    const int denom = std::max(1, innerW - 1);
    for (int r = 0; r < 3; r++) {
      DitherRowState rs;  // 每行一份状态（Row 档靠"列不连续"在 x=0 处自动重置）
      for (int xx = 0; xx < innerW; xx++) {
        const uint8_t gray = static_cast<uint8_t>(xx * 255 / denom);
        const uint8_t lvl = grayToLevel16(gray, xx, r, modes[r], rs);
        for (int yy = 0; yy < stripH; yy++)
          g_rd.drawGrayscale16Pixel(M + xx, y + r * stripH + yy, lvl);
      }
    }
    y += 3 * stripH + 6;
  }

  // 底部：自推屏动作选择 + 上一次的耗时。
  const int iy = statusTop() - infoRowH + 2;
  char info[128];
  snprintf(info, sizeof(info), "动作 %d/%d：%s", st.rtSel + 1, kRtActionCount,
           kRtActionNames[st.rtSel]);
  drawLineText(M, iy, info, true);
  if (st.rtMs[st.rtSel] >= 0) {
    char msb[48];
    snprintf(msb, sizeof(msb), "上次 %d ms", st.rtMs[st.rtSel]);
    const int mw = g_rd.getTextWidth(uiFontId(), msb);
    drawLineText(w - M - mw, iy, msb, true);
  }
  drawFooter("↑↓ 换动作  Enter 推屏  Esc 返回");
}

void handleRefreshTest(int key) {
  const int n = kRtActionCount;
  if (key == 0x1B) { st.mode = st.retMode; st.fullRefresh = true; st.dirty = 1; return; }
  if (key == KEY_UP) { st.rtSel = (st.rtSel + n - 1) % n; st.dirty = 1; return; }
  if (key == KEY_DOWN) { st.rtSel = (st.rtSel + 1) % n; st.dirty = 1; return; }
  if (key == '\n') { st.rtPending = st.rtSel; st.dirty = 1; return; }
}

// ── WiFi 传书 ───────────────────────────────────────────────────────────
// 复用写作模式的 web 文件管理器（main/file_manager_server.cpp）：手机/电脑浏览器
// 打开 http://<IP>/ 即可浏览 /sdcard，把 epub/txt 传进 /sdcard/books。
static const int kNetRows = 3;  // 启动 / 停止 / 二维码

void netShareConnect() {
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

void renderNetShare() {
  g_rd.clearScreen();
  int w = g_rd.getScreenWidth();
  int top = drawTitle("WiFi 传书");
  const ListView lv = flatMenuListView(kNetRows, st.netSel, rdListTop());
  int itemH = lv.itemH;

  std::string ip = g_wifi.isConnected() ? g_wifi.getIp() : std::string();
  std::string status;
  if (st.netBusy) status = "处理中...";
  else if (!st.netStatus.empty()) status = st.netStatus;
  else if (g_wifi.isConnected()) status = ip.empty() ? "WiFi 已连接" : ("WiFi 已连接  " + ip);
  else status = "未连接 WiFi";
  drawLineText(MARGIN, top, fitWidth(status, w - 2 * MARGIN).c_str(), true);
  int ly = lv.top;  // 状态行下的分隔线
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

void handleNetShare(int key) {
  if (st.netBusy) return;
  if (key == 0x1B) { st.mode = st.retMode; st.fullRefresh = true; st.dirty = 1; return; }
  {  // 上下选择在 ui/list_view.h（与 renderNetShare 共用同一个几何）
    ListView lv = flatMenuListView(kNetRows, st.netSel, rdListTop());
    if (listViewKey(lv, key)) { st.netSel = lv.sel; st.dirty = 1; return; }
  }
  if (key != '\n') return;

  int x, y;
  if (input_tap_xy(&x, &y)) {
    const int row = listViewHitAt(flatMenuListView(kNetRows, st.netSel, rdListTop()), y);
    if (row >= 0) st.netSel = row;
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
