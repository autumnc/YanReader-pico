// screen_reader_wifi.cpp — 阅读模式的安卓式 WiFi 管理界面。
//
// 从 screen_reader_subpages.cpp 搬出来单独成 TU（同 screen_reader_stats.cpp /
// screen_reader_files.cpp 的先例）：原来那版（renderWifi/handleWifi）只有 SSID / 密码 /
// 连接 / 断开 四行，用户得手打网络名、看不见周围有什么网、只能存**一个**网络。
//
// 新版对齐安卓那套：一个射频总开关 + 状态行 + 可点「扫描」+ 网络列表 + 右下角浮动
// 「+ 添加网络」+ 长按弹菜单（连接/断开/忘记）—— 存储换成多网络的 g_wifiStore
// （/sdcard/wifi_networks.txt，见 wifi_store.h）。
//
// 键位与蓝牙设备管理页（screen_bt_manage.cpp）同一套：点开关行/`B` 切开关、点上/下
// 选行、Enter 或点行 = 连接、长按 = 菜单、`?` = 帮助、Esc 逐层返回。
// 全部几何（开关行 / 扫描按钮 / 浮动按钮 / 菜单浮层 / 列表）都是"绘制与命中共用一个
// 式子"，照 BT 页那条规矩来（两处各写一遍迟早会漂）。
//
// 屏幕外的状态放本文件的 g_wui（照 BT 页的 g_btState）；只有 VK/IME 胶水要用的三个
// 字段（wifiField / wifiSsidEdit / wifiPassEdit，见 vkTargetString）留在 RdState。

#include "screen_reader_internal.h"   // st / g_rd / 外壳原语
#include "wifi_manager.h"
#include "wifi_store.h"
#include "ui/list_view.h"
#include "ime/IME.h"
#include "hw/input.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <esp_wifi.h>   // WIFI_AUTH_OPEN
#include <esp_timer.h>

// ── 子状态 ───────────────────────────────────────────────────────────────
enum WifiView {
  WV_LIST,     // 默认：开关 + 状态 + 列表 + 浮动按钮
  WV_PASS,     // 网络名/密码表单（点加密网络 / 点「+ 添加网络」）
  WV_MENU,     // 长按某一行弹出的浮层菜单
  WV_CONFIRM,  // 「忘记网络？」确认框
  WV_HELP,     // 快捷键帮助
};

// 长按菜单的项（表与动作一起动态建，见 wifiOpenMenu）。
enum WifiMenuAct { WM_CONNECT, WM_DISCONNECT, WM_FORGET, WM_CANCEL };

struct WifiUiState {
  WifiView view = WV_LIST;
  // 选中行的**身份**（用网络名而不是下标）：列表每帧按"连接中 → 信号强 → 垫底的
  // 已保存"重建，顺序会变，存下标的话连上之后选中项会跑到别的网络上去。
  std::string selSsid;
  int scroll = 0;
  std::string status;   // 一次性提示（连接失败的人话原因等）
  // 菜单浮层
  std::string menuSsid;
  std::string menuPass;   // 菜单弹出时从已保存表里抄下来的密码（"连接"那一项直接用）
  std::string menuLabels[4];
  int menuActs[4] = {0, 0, 0, 0};
  int menuN = 0;
  int menuSel = 0;
  // 确认框
  std::string confirmSsid;
  // 帮助
  int helpScroll = 0;
  // 上一帧观察到的连接态：空闲帧靠它发现"后台自动重连成功了 / 掉线了"。
  bool lastConn = false;
  std::string lastConnSsid;
};

static WifiUiState g_wui;

// ── 一行网络（扫描结果 + 已保存但不在范围的，合成同一张表）──────────────
struct WifiRow {
  std::string ssid;
  int rssi = 0;         // 不在范围时无意义
  bool inRange = false;
  bool saved = false;
  bool open = false;    // 无密码
  bool connected = false;
};

// ── 几何（绘制与命中共用；改一处就是改两处）─────────────────────────────
static const char *kFabLabel = "+ 添加网络";

static int wifiSwitchRowTop() { return coverTop(); }
static int wifiStatusRowTop() { return coverTop() + uiLineHeight() + 6; }
static int wifiSepY() { return wifiStatusRowTop() + uiLineHeight() + 8; }
static int wifiListTop() { return wifiSepY() + 4; }
static int wifiRowH() { return uiLineHeight() + 12; }

// 总开关那一行的触摸命中带（整行都给，文字基线上下各留一点）。
static bool wifiSwitchRowHit(int y) {
  const int top = wifiSwitchRowTop() - 2;
  return y >= top && y < top + uiLineHeight() + 4;
}

// 开关药丸：贴右边缘。黑底白字 = 开，白底黑框 = 关（与 BT 页同一套样子）。
static void wifiSwitchPillRect(int *x, int *y, int *w, int *h) {
  const int sw = g_rd.getScreenWidth();
  *h = uiLineHeight() + 4;
  *w = g_rd.getTextWidth(uiFontId(), "开") + 24;
  *x = sw - *w - MARGIN;
  *y = wifiSwitchRowTop() - 2;
}

// 状态行右端的「扫描」按钮。
static void wifiScanBtnRect(int *x, int *y, int *w, int *h) {
  const int sw = g_rd.getScreenWidth();
  *h = uiLineHeight() + 4;
  // 宽度按最长的那个标签算（"扫描中"），免得扫描时按钮跟着字变宽、跳一下。
  *w = g_rd.getTextWidth(uiFontId(), "扫描中") + 18;
  *x = sw - *w - MARGIN;
  *y = wifiStatusRowTop() - 2;
}
static bool wifiScanBtnHit(int x, int y) {
  int bx, by, bw, bh;
  wifiScanBtnRect(&bx, &by, &bw, &bh);
  return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

// 浮动按钮「+ 添加网络」：锚在底部提示栏上方右下角。做成浮动而不是列表最后一行 ——
// 网络多了列表会一直排到底，常驻一行会跟列表抢位置（照 BT 页的 fabHit）。
static int wifiFabW() { return g_rd.getTextWidth(uiFontId(), kFabLabel) + 36; }
static int wifiFabH() { return uiLineHeight() + 10; }
static int wifiFabX() { return g_rd.getScreenWidth() - wifiFabW() - 10; }
static int wifiFabY() { return statusTop() - wifiFabH() - 8; }
static bool wifiFabHit(int x, int y) {
  return x >= wifiFabX() && x < wifiFabX() + wifiFabW() && y >= wifiFabY() &&
         y < wifiFabY() + wifiFabH();
}

// 列表几何。rows 按"浮动按钮上沿"推（多算一行的话最后一行会被按钮压住）。
// 跟随式窗口（sel 滚出可见区才滚），first 存在 g_wui.scroll 里。
static ListView wifiListView(int count, int sel, int scroll) {
  ListView lv;
  lv.top = wifiListTop();
  lv.itemH = wifiRowH();
  lv.count = count;
  lv.rows = std::max(1, (wifiFabY() - 4 - lv.top) / lv.itemH);
  lv.sel = sel;
  lv.first = scroll;
  listViewFollow(lv);
  return lv;
}

// 密码表单（3 行：网络名 / 密码 / 连接），几何与旧版一致。
static ListView wifiPassListView() {
  ListView lv;
  lv.top = rdListTop();
  lv.itemH = uiLineHeight() + 12;
  lv.count = 3;
  lv.rows = 3;
  lv.sel = st.wifiField;
  return lv;
}

// ── 数据：合成列表 ───────────────────────────────────────────────────────
static int wifiBarsForRssi(int rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -78) return 2;
  return 1;
}

static void wifiBuildRows(std::vector<WifiRow> &out) {
  out.clear();
  const bool conn = g_wifi.isConnected();
  const std::string cur = conn ? std::string(g_wifi.connectedSsid()) : std::string();

  // ① 这次扫到的（g_wifi 那边已经去重 + 按 RSSI 降序）
  const int n = g_wifi.scanCount();
  for (int i = 0; i < n; i++) {
    const WifiScanItem &it = g_wifi.scanResult(i);
    if (it.ssid.empty()) continue;
    WifiRow r;
    r.ssid = it.ssid;
    r.rssi = it.rssi;
    r.inRange = true;
    r.saved = g_wifiStore.has(it.ssid.c_str());
    const WifiSavedNet *sn = g_wifiStore.find(it.ssid.c_str());
    r.open = sn ? sn->open : (it.authmode == WIFI_AUTH_OPEN);
    r.connected = conn && cur == it.ssid;
    out.push_back(std::move(r));
  }

  // ② 已保存但这次没扫到 → 垫底（显示「不在范围」）。用户明确要的：别让存过的网
  //    从列表里消失，否则"我明明连过这个网，怎么找不到了"。
  const int savedN = g_wifiStore.count();
  for (int i = 0; i < savedN; i++) {
    const WifiSavedNet *sn = g_wifiStore.at(i);
    if (!sn) continue;
    bool dup = false;
    for (const auto &r : out) {
      if (r.ssid == sn->ssid) {
        dup = true;
        break;
      }
    }
    if (dup) continue;
    WifiRow r;
    r.ssid = sn->ssid;
    r.saved = true;
    r.open = sn->open;
    r.connected = conn && cur == sn->ssid;
    out.push_back(std::move(r));
  }

  // ③ 连接中的那条挪到最前（安卓也这么排；用户进这一页多半就是为了看它）
  for (size_t i = 0; i < out.size(); i++) {
    if (out[i].connected) {
      std::swap(out[0], out[i]);
      break;
    }
  }
}

// 当前选中项在 rows 里的下标（找不到就是 0 —— 列表刚重建、原来那条没了）。
static int wifiIndexForSel(const std::vector<WifiRow> &rows) {
  for (int i = 0; i < static_cast<int>(rows.size()); i++)
    if (rows[i].ssid == g_wui.selSsid) return i;
  return 0;
}
static ListView wifiCurrentList(const std::vector<WifiRow> &rows) {
  return wifiListView(static_cast<int>(rows.size()), wifiIndexForSel(rows), g_wui.scroll);
}

static const char *wifiRowTag(const WifiRow &r) {
  if (r.connected) return "已连接";
  if (!r.inRange) return "不在范围";
  if (r.saved) return "已保存";
  if (r.open) return "开放";
  return "需密码";
}

// ── 动作 ─────────────────────────────────────────────────────────────────
// 退出本页：收起射频。这个仓库的 WiFi 一直是"按需起"（省电），本页也不例外 ——
// 开关的**意图**（wifi_enabled）持久化着，射频本身只在用它的这段时间里立着。
// 不管的话，进一次这页就等于把射频永久打开，几小时就把电池耗光。
static void wifiLeave() {
  g_wifi.scanStop();
  g_wifi.disconnect();
  st.mode = st.retMode;
  st.fullRefresh = true;
  st.dirty = 1;
}

static void wifiStartScan() {
  g_wui.status.clear();
  g_wifi.scanStart();
  st.dirty = 1;
}

static void wifiToggleEnabled() {
  const bool on = !g_wifi.enabled();
  g_wifi.setEnabled(on);
  g_wui.status.clear();
  g_wui.selSsid.clear();
  g_wui.scroll = 0;
  if (on) g_wifi.scanStart();   // 开回来立刻扫一次，别让用户再点一下「扫描」
  st.fullRefresh = true;
  st.dirty = 1;
}

// 进密码表单。which = 0 光标落在网络名上（浮动按钮那条路），1 落在密码上。
static void wifiEnterPass(const std::string &ssid, int which) {
  st.wifiSsidEdit = ssid;
  st.wifiPassEdit.clear();
  st.wifiField = which;
  st.wifiEditing = true;
  g_wui.status.clear();
  g_wui.view = WV_PASS;
  rdVkWantShow();   // 置 st.vkVisible + 必要时整屏重绘
  st.dirty = 1;
}

// 真的去连（阻塞最长 10 秒，进度浮层由 readerWifiConnect 负责）。
static void wifiDoConnect() {
  const std::string ssid = st.wifiSsidEdit;
  if (ssid.empty()) {
    g_wui.status = "网络名不能为空";
    st.dirty = 1;
    return;
  }
  const std::string pass = st.wifiPassEdit;
  // 从列表点一行进来时不会有键盘开着；显式收干净，免得连着连着屏上还挂着一块面板。
  st.wifiEditing = false;
  st.vkVisible = false;
  g_wifi.begin();
  const bool ok = readerWifiConnect(ssid.c_str(), pass.c_str());
  if (ok) {
    g_wifi.savedRemember(ssid.c_str(), pass.c_str());   // 记住 + 镜像旧键 wifi_ssid/pass
    g_wui.status.clear();
    g_wui.selSsid = ssid;
    g_wui.scroll = 0;
    g_wui.view = WV_LIST;
  } else {
    // 失败留在密码屏：最常见的就是密码错，改一个字符重试比重新走一遍流程省事。
    g_wui.status = std::string("连接失败: ") + g_wifi.lastReasonText();
    st.wifiField = 1;
    g_wui.view = WV_PASS;
  }
  st.fullRefresh = true;   // 这两条路都换了整屏内容
  st.dirty = 1;
}

static void wifiOpenMenu(const WifiRow &r) {
  g_wui.menuSsid = r.ssid;
  const WifiSavedNet *sn = g_wifiStore.find(r.ssid.c_str());
  g_wui.menuPass = sn ? sn->pass : std::string();
  g_wui.menuN = 0;
  g_wui.menuSel = 0;
  if (r.connected) {
    g_wui.menuLabels[g_wui.menuN] = "断开";
    g_wui.menuActs[g_wui.menuN++] = WM_DISCONNECT;
  } else {
    g_wui.menuLabels[g_wui.menuN] = "连接";
    g_wui.menuActs[g_wui.menuN++] = WM_CONNECT;
  }
  if (r.saved) {
    g_wui.menuLabels[g_wui.menuN] = "忘记网络";
    g_wui.menuActs[g_wui.menuN++] = WM_FORGET;
  }
  g_wui.menuLabels[g_wui.menuN] = "取消";
  g_wui.menuActs[g_wui.menuN++] = WM_CANCEL;
  g_wui.view = WV_MENU;
  st.dirty = 1;
}

// 点行 / Enter：按这一行的性质决定下一步。
static void wifiActivateRow(const WifiRow &r) {
  if (r.connected) {
    wifiOpenMenu(r);   // 已经连着：弹菜单（里面是"断开/忘记"），别重复连
    return;
  }
  const WifiSavedNet *n = g_wifiStore.find(r.ssid.c_str());
  if (n) {
    // 存过的：直接用存好的密码连（开放网络存的密码就是空串）
    st.wifiSsidEdit = r.ssid;
    st.wifiPassEdit = n->pass;
    wifiDoConnect();
    return;
  }
  if (r.inRange && r.open) {
    st.wifiSsidEdit = r.ssid;
    st.wifiPassEdit.clear();
    wifiDoConnect();
    return;
  }
  // 需密码且没存过 → 进密码屏，光标直接落在密码上
  wifiEnterPass(r.ssid, 1);
}

static void wifiDoForget(const std::string &ssid) {
  if (g_wifi.isConnected() && ssid == g_wifi.connectedSsid()) g_wifi.disconnect();
  g_wifi.savedForget(ssid.c_str());
  if (g_wui.selSsid == ssid) g_wui.selSsid.clear();
  g_wui.status = "已忘记 " + ssid;
  g_wui.view = WV_LIST;
  st.fullRefresh = true;
  st.dirty = 1;
}

// ── 绘制：底图（列表页）──────────────────────────────────────────────────
static void drawWifiSwitchRow() {
  const bool on = g_wifi.enabled();
  drawLineText(MARGIN, wifiSwitchRowTop(), "WiFi", true);
  int x, y, w, h;
  wifiSwitchPillRect(&x, &y, &w, &h);
  const char *t = on ? "开" : "关";
  const int tw = g_rd.getTextWidth(uiFontId(), t);
  const int ty = y + (h - uiLineHeight()) / 2;
  if (on) {
    g_rd.fillRect(x, y, w, h, true);                   // 黑底
    g_rd.drawRect(x + 2, y + 2, w - 4, h - 4, false);  // 内描边：看着像按钮而不是色块
    drawLineText(x + (w - tw) / 2, ty, t, false);      // 白字
  } else {
    g_rd.drawRect(x, y, w, h, true);
    drawLineText(x + (w - tw) / 2, ty, t, true);
  }
}

// 信号条：只用"填充了几格"表达强弱（1 位屏上画空心槽会糊成一团）。
static void drawWifiBars(int right, int rowTop, int rssi) {
  const int barW = 3, gap = 2;
  const int total = 4 * barW + 3 * gap;
  const int x0 = right - total;
  const int base = rowTop + uiLineHeight() - 2;
  const int lvl = wifiBarsForRssi(rssi);
  for (int i = 0; i < lvl; i++) {
    const int bh = 4 + i * 3;
    g_rd.fillRect(x0 + i * (barW + gap), base - bh, barW, bh, true);
  }
}

static void drawWifiRow(const WifiRow &r, int y, int itemH, bool sel) {
  const int w = g_rd.getScreenWidth();
  if (sel) g_rd.fillRect(0, y, w, itemH, true);
  const bool black = !sel;
  const int ty = y + (itemH - uiLineHeight()) / 2;

  // 左端标记：连接中一个实心点（直接画方块，不走字体的 U+25CF —— 字形子集里有没有
  // 它得碰运气，几个 fillRect 稳当）。
  int x = MARGIN;
  if (r.connected) {
    g_rd.fillRect(MARGIN, ty + uiLineHeight() / 2 - 3, 6, 6, black);
    x = MARGIN + 12;
  }

  // 右端：标签；范围内且没在连 → 标签左边再放信号条
  const char *tag = wifiRowTag(r);
  const int tagW = g_rd.getTextWidth(uiFontId(), tag);
  int right = w - MARGIN - tagW;
  drawLineText(right, ty, tag, black);
  right -= 10;
  if (r.inRange && !r.connected) {
    drawWifiBars(right, y + (itemH - uiLineHeight()) / 2, r.rssi);
    right -= (4 * 3 + 3 * 2) + 8;
  }

  const int maxW = right - x;
  if (maxW > 20) drawLineText(x, ty, fitWidth(r.ssid, maxW).c_str(), black);
}

static void drawWifiFab() {
  const int x = wifiFabX(), y = wifiFabY(), w = wifiFabW(), h = wifiFabH();
  g_rd.fillRect(x, y, w, h, true);
  g_rd.drawRect(x + 2, y + 2, w - 4, h - 4, false);
  const int tw = g_rd.getTextWidth(uiFontId(), kFabLabel);
  drawLineText(x + (w - tw) / 2, y + (h - uiLineHeight()) / 2, kFabLabel, false);
}

static void renderWifiList() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  drawTitle("WiFi 管理");
  drawWifiSwitchRow();

  const bool on = g_wifi.enabled();
  int bx, by, bw, bh;
  wifiScanBtnRect(&bx, &by, &bw, &bh);

  // 状态行（左） + 「扫描」按钮（右）
  std::string s;
  if (!on) {
    s = "WiFi 已关闭";
  } else if (g_wifi.isConnected()) {
    s = std::string("已连接 ") + g_wifi.connectedSsid();
    const std::string ip = g_wifi.getIp();
    if (!ip.empty()) s += "  IP " + ip;
  } else if (g_wifi.scanBusy()) {
    s = "正在扫描…";
  } else if (!g_wui.status.empty()) {
    s = g_wui.status;
  } else {
    s = "未连接";
  }
  const int statusRight = on ? (bx - 10) : (w - MARGIN);
  drawLineText(MARGIN, wifiStatusRowTop(), fitWidth(s, statusRight - MARGIN).c_str(), true);
  if (on) {
    const char *t = g_wifi.scanBusy() ? "扫描中" : "扫描";
    const int tw = g_rd.getTextWidth(uiFontId(), t);
    g_rd.drawRect(bx, by, bw, bh, true);
    drawLineText(bx + (bw - tw) / 2, by + (bh - uiLineHeight()) / 2, t, true);
  }

  g_rd.drawLine(0, wifiSepY(), w, wifiSepY(), true);

  // 关着时列表必然为空（射频没起）：别画成"未找到网络"，那会让人以为周围真没网。
  if (!on) {
    int y = wifiListTop() + 12;
    drawCenteredLine(y, "WiFi 已关闭");
    y += uiLineHeight() + 6;
    drawCenteredLine(y, "点上面那一行或按 B 打开");
    drawFooter("B:开关  ?:帮助  Esc:返回");
    return;
  }

  std::vector<WifiRow> rows;
  wifiBuildRows(rows);
  const int n = static_cast<int>(rows.size());
  const int lt = wifiListTop();
  if (n == 0) {
    int y = lt + 12;
    drawCenteredLine(y, g_wifi.scanBusy() ? "正在扫描…" : "未找到网络");
    y += uiLineHeight() + 6;
    drawCenteredLine(y, "点右下角「+ 添加网络」手动输入");
  } else {
    ListView lv = wifiCurrentList(rows);
    g_wui.scroll = lv.first;
    g_wui.selSsid = rows[lv.sel].ssid;
    for (int i = 0; i < lv.rows && lv.first + i < n; i++) {
      const int idx = lv.first + i;
      drawWifiRow(rows[idx], lt + i * lv.itemH, lv.itemH, idx == lv.sel);
    }
  }

  drawWifiFab();
  drawFooter("↑↓ 选择  Enter 连接  长按 菜单  Esc 返回");
}

// ── 绘制：密码表单 ───────────────────────────────────────────────────────
static void renderWifiPass() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  drawTitle("WiFi 密码");
  const ListView lv = wifiPassListView();
  const int itemH = lv.itemH;
  const std::string s = g_wui.status.empty() ? std::string("输入后 Enter 连接") : g_wui.status;
  drawLineText(MARGIN, coverTop(), fitWidth(s, w - 2 * MARGIN).c_str(), true);
  const int ly = lv.top;
  g_rd.drawLine(0, ly, w, ly, true);

  std::string rows[3];
  rows[0] = "网络名: " + st.wifiSsidEdit;
  rows[1] = "密码: " + std::string(st.wifiPassEdit.size(), '*');
  rows[2] = "连接";
  for (int i = 0; i < 3; i++) {
    const int ry = ly + i * itemH;
    const std::string t = fitWidth(rows[i], w - 2 * MARGIN);
    const bool sel = (i == st.wifiField);
    if (sel) {
      g_rd.fillRect(0, ry, w, itemH, true);
      drawLineText(MARGIN, ry + (itemH - uiLineHeight()) / 2, t.c_str(), false);
    } else {
      drawLineText(MARGIN, ry + (itemH - uiLineHeight()) / 2, t.c_str(), true);
    }
  }

  if (st.vkVisible) drawVk();
  else drawFooter(st.wifiEditing ? "输入中  Enter 完成  Esc 取消"
                                 : "↑↓ 选择  Enter 编辑/连接  Esc 返回");
  if (st.wifiEditing) {
    drawRdImeBar();   // 蓝牙键盘打字：编码行 + 候选行
    rdDrawVkIcon();   // 只有编辑字段（键盘可用）时才给开关
  }
}

// ── 绘制：菜单浮层 / 确认框 / 帮助 ───────────────────────────────────────
static void wifiMenuBoxRect(int *bx, int *by, int *bw, int *bh) {
  int w = 0;
  for (int i = 0; i < g_wui.menuN; i++) {
    const int tw = g_rd.getTextWidth(uiFontId(), g_wui.menuLabels[i].c_str()) + 2 * uiLineHeight();
    if (tw > w) w = tw;
  }
  const int tw = g_rd.getTextWidth(uiFontId(), g_wui.menuSsid.c_str()) + 2 * uiLineHeight();
  if (tw > w) w = tw;
  const int sw = g_rd.getScreenWidth();
  if (w < 240) w = 240;
  if (w > sw - 32) w = sw - 32;
  *bw = w;
  *bh = 8 + uiLineHeight() + 6 + g_wui.menuN * (uiLineHeight() + 6) + 8;
  *bx = (sw - w) / 2;
  *by = (g_rd.getScreenHeight() - *bh) / 2;
}
// 菜单项 i 的文字上缘（绘制与命中共用）。
static int wifiMenuRowTop(int by, int i) {
  const int sepY = by + 8 + uiLineHeight() + 2;
  return sepY + 6 + i * (uiLineHeight() + 6);
}
static int wifiMenuRowAt(int x, int y) {
  int bx, by, bw, bh;
  wifiMenuBoxRect(&bx, &by, &bw, &bh);
  if (x < bx || x >= bx + bw || y < by || y >= by + bh) return -1;
  for (int i = 0; i < g_wui.menuN; i++) {
    const int ry = wifiMenuRowTop(by, i);
    if (y >= ry - 2 && y < ry + uiLineHeight() + 2) return i;
  }
  return -1;
}

static void renderWifiMenu() {
  renderWifiList();   // 底图照画，浮层盖在上面（一次提交一个绘制目标）
  int bx, by, bw, bh;
  wifiMenuBoxRect(&bx, &by, &bw, &bh);
  g_rd.fillRect(bx, by, bw, bh, true);
  g_rd.drawRect(bx, by, bw, bh, false);

  // 标题写网络名：不写清楚是哪一台，"忘记网络"就是盲删。
  drawLineText(bx + 10, by + 8, fitWidth(g_wui.menuSsid, bw - 20).c_str(), false);
  const int sepY = by + 8 + uiLineHeight() + 2;
  g_rd.drawLine(bx + 4, sepY, bx + bw - 4, sepY, false);

  for (int i = 0; i < g_wui.menuN; i++) {
    const int ry = wifiMenuRowTop(by, i);
    const int tw = g_rd.getTextWidth(uiFontId(), g_wui.menuLabels[i].c_str());
    const int tx = bx + (bw - tw) / 2;
    if (i == g_wui.menuSel) {
      g_rd.fillRect(bx + 4, ry - 2, bw - 8, uiLineHeight() + 4, false);
      drawLineText(tx, ry, g_wui.menuLabels[i].c_str(), true);
    } else {
      drawLineText(tx, ry, g_wui.menuLabels[i].c_str(), false);
    }
  }
}

static void renderWifiConfirm() {
  renderWifiList();
  const int sw = g_rd.getScreenWidth();
  const std::string l1 = "忘记网络？";
  const std::string l2 = fitWidth(g_wui.confirmSsid, sw - 140);
  const std::string l3 = "Enter 确认   Esc 取消";
  int bw = g_rd.getTextWidth(uiFontId(), l1.c_str());
  bw = std::max(bw, g_rd.getTextWidth(uiFontId(), l2.c_str()));
  bw = std::max(bw, g_rd.getTextWidth(uiFontId(), l3.c_str()));
  bw += 60;
  const int bh = 24 + 3 * uiLineHeight() + 22;
  const int bx = (sw - bw) / 2;
  int by = (g_rd.getScreenHeight() - bh) / 2 - 20;
  if (by < 0) by = 0;
  g_rd.fillRect(bx, by, bw, bh, true);
  g_rd.drawRect(bx, by, bw, bh, false);
  const int base = by + 24;
  drawCenteredLine(base, l1.c_str(), false);
  drawCenteredLine(base + uiLineHeight(), l2.c_str(), false);
  drawCenteredLine(base + 2 * uiLineHeight(), l3.c_str(), false);
}

static const char *WIFI_HELP_LINES[] = {
    "── WiFi 管理页 ──",
    "WiFi     最上面一行,开/关射频",
    "B        同上",
    "扫描     状态行右端,或按 R",
    "↑↓       选择网络",
    "Enter    连接选中 / 点哪行连哪行",
    "长按网络  弹出菜单(连接/断开/忘记)",
    "+添加网络 右下角,手动输入名字",
    "?        帮助",
    "Esc      返回",
    "",
    "── 说明 ──",
    "· 列表里「已保存」= 存过密码,",
    "  「不在范围」= 存过但这次没扫到,",
    "  「需密码」= 没存过且要密码。",
    "· 退出本页会收起射频(省电);",
    "  NTP/传书/书库这些要联网时",
    "  会自动再连,不用手动开。",
    "· 总开关关掉 = 连按需也不开。",
};
static const int WIFI_HELP_COUNT = sizeof(WIFI_HELP_LINES) / sizeof(WIFI_HELP_LINES[0]);

static int wifiHelpRows() {
  const int contentY = rdListTop();
  const int v = (statusTop() - contentY - 4) / (uiLineHeight() + 6);
  return v < 1 ? 1 : v;
}

static void renderWifiHelp() {
  g_rd.clearScreen();
  drawTitle("WiFi 帮助");
  const int contentY = rdListTop();
  const int rowH = uiLineHeight() + 6;
  const int maxVis = wifiHelpRows();
  const int maxScroll = std::max(0, WIFI_HELP_COUNT - maxVis);
  if (g_wui.helpScroll > maxScroll) g_wui.helpScroll = maxScroll;
  if (g_wui.helpScroll < 0) g_wui.helpScroll = 0;
  for (int i = 0; i < maxVis && g_wui.helpScroll + i < WIFI_HELP_COUNT; i++) {
    drawLineText(MARGIN, contentY + i * rowH, WIFI_HELP_LINES[g_wui.helpScroll + i], true);
  }
  drawFooter("↑↓ 滚动  Esc 返回");
}

// ── 按键：各子状态 ───────────────────────────────────────────────────────
static void handleWifiHelp(int key) {
  if (key == KEY_UP) {
    if (g_wui.helpScroll > 0) g_wui.helpScroll--;
  } else if (key == KEY_DOWN) {
    g_wui.helpScroll++;
  } else if (key == KEY_PAGE_UP) {
    const int step = wifiHelpRows();
    g_wui.helpScroll -= step;
    if (g_wui.helpScroll < 0) g_wui.helpScroll = 0;
  } else if (key == KEY_PAGE_DOWN) {
    g_wui.helpScroll += wifiHelpRows();   // 上限在画的时候夹住
  } else {
    g_wui.view = WV_LIST;
    st.dirty = 1;
    return;
  }
  st.dirty = 1;
}

static void handleWifiConfirm(int key) {
  if (key == 0x1B || key == 'q' || key == 'Q') {
    g_wui.view = WV_LIST;
    st.dirty = 1;
    return;
  }
  if (key == '\n' || key == 0x0D) {
    wifiDoForget(g_wui.confirmSsid);
    return;
  }
}

static void handleWifiMenu(int key) {
  if (key == KEY_UP) {
    if (g_wui.menuSel > 0) g_wui.menuSel--;
    st.dirty = 1;
    return;
  }
  if (key == KEY_DOWN) {
    if (g_wui.menuSel < g_wui.menuN - 1) g_wui.menuSel++;
    st.dirty = 1;
    return;
  }
  if (key == 0x1B || key == 'q' || key == 'Q') {
    g_wui.view = WV_LIST;
    st.dirty = 1;
    return;
  }
  if (key == '\n' || key == 0x0D) {
    int tx = 0, ty = 0;
    int act = g_wui.menuActs[g_wui.menuSel];
    if (input_tap_xy(&tx, &ty)) {
      const int r = wifiMenuRowAt(tx, ty);
      if (r < 0) {   // 点浮层外 = 关掉菜单
        g_wui.view = WV_LIST;
        st.dirty = 1;
        return;
      }
      act = g_wui.menuActs[r];
    }
    const std::string ssid = g_wui.menuSsid;
    g_wui.view = WV_LIST;
    switch (act) {
      case WM_CONNECT:
        // 菜单只在"已保存 / 已连接"的行上弹出，所以密码在弹菜单时就抄好了。
        st.wifiSsidEdit = ssid;
        st.wifiPassEdit = g_wui.menuPass;
        wifiDoConnect();
        return;
      case WM_DISCONNECT:
        g_wifi.disconnect();
        g_wui.status = "已断开";
        st.fullRefresh = true;
        st.dirty = 1;
        return;
      case WM_FORGET:
        g_wui.confirmSsid = ssid;
        g_wui.view = WV_CONFIRM;
        st.dirty = 1;
        return;
      default:
        st.dirty = 1;
        return;
    }
  }
}

static void handleWifiPass(int key) {
  if (key == 0x1B) {
    IME::getInstance().cancelComposition();
    st.wifiEditing = false;
    st.vkVisible = false;
    g_wui.view = WV_LIST;
    g_wui.status.clear();
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (st.wifiEditing) {
    if (key == '\n' || key == 0x0D) {
      int x = 0, y = 0;
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
      vkEnter();   // BLE/KEY3 回车 → 完成；键盘收起时的点按同样走"完成"
      st.dirty = 1;
      return;
    }
    if (key == 0x08) {
      feedVkBackspace();
      st.dirty = 1;
      return;
    }
    if (key >= 0x20 && key <= 0x7E) {
      feedVkKey(key);
      st.dirty = 1;
      return;
    }
    return;
  }

  // 非编辑态：↑↓ 在三个字段间跳
  {
    ListView lv = wifiPassListView();
    if (listViewKey(lv, key)) {
      st.wifiField = lv.sel;
      st.dirty = 1;
      return;
    }
  }
  if (key == '\n' || key == 0x0D) {
    int x = 0, y = 0;
    if (input_tap_xy(&x, &y)) {
      const int row = listViewHitAt(wifiPassListView(), y);
      if (row >= 0) st.wifiField = row;
    }
    if (st.wifiField == 0 || st.wifiField == 1) {
      st.wifiEditing = true;
      rdVkWantShow();
      st.dirty = 1;
    } else {
      wifiDoConnect();
    }
  }
}

static void handleWifiList(int key) {
  const bool on = g_wifi.enabled();

  if (key == '?') {
    g_wui.helpScroll = 0;
    g_wui.view = WV_HELP;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (key == 'b' || key == 'B') {
    wifiToggleEnabled();
    return;
  }
  // 点开关那一行 = 切开关。**必须拦在下面"点哪行连哪行"之前** —— 那一段对落在列表
  // 上方的点是"没点中行"，会拿旧选中项去连，于是点开关会顺手连上某个网络。
  // 先 peek：input_tap_xy 取走即清，没点在这一行的话落点要原样留给下面那一段。
  if (key == '\n' || key == 0x0D) {
    int tx = 0, ty = 0;
    if (input_tap_peek_xy(&tx, &ty) && wifiSwitchRowHit(ty)) {
      input_tap_xy(&tx, &ty);
      wifiToggleEnabled();
      return;
    }
  }

  if (!on) {
    // 关着时网络那一套动作全不接（射频没起，列表也不画）；只剩开关、帮助、返回。
    if (key == 0x1B) {
      wifiLeave();
      return;
    }
    // 关着时点哪儿都不动作，但落点照吞：留着不取，下一帧会变成一颗不知哪来的幽灵点按。
    if (key == '\n' || key == 0x0D) {
      int tx, ty;
      input_tap_xy(&tx, &ty);
    }
    return;
  }

  if (key == 0x1B) {
    wifiLeave();
    return;
  }
  if (key == 'r' || key == 'R') {
    wifiStartScan();
    return;
  }

  // 扫描按钮 / 浮动按钮的点按
  if (key == '\n' || key == 0x0D) {
    int tx = 0, ty = 0;
    if (input_tap_peek_xy(&tx, &ty) && wifiScanBtnHit(tx, ty)) {
      input_tap_xy(&tx, &ty);
      wifiStartScan();
      return;
    }
    if (input_tap_peek_xy(&tx, &ty) && wifiFabHit(tx, ty)) {
      input_tap_xy(&tx, &ty);
      wifiEnterPass("", 0);
      return;
    }
  }

  std::vector<WifiRow> rows;
  wifiBuildRows(rows);
  const int n = static_cast<int>(rows.size());

  // 长按某一行 → 弹菜单（按下点落在哪一行就操作哪一行，与"先选中再 Enter"等价）。
  if (key == KEY_TOUCH_LONG) {
    int tx = 0, ty = 0;
    if (n > 0 && input_tap_xy(&tx, &ty)) {
      const ListView lv = wifiCurrentList(rows);
      const int idx = (ty < wifiFabY() - 4) ? listViewHitAt(lv, ty) : -1;
      if (idx >= 0 && idx < n) {
        g_wui.selSsid = rows[idx].ssid;
        if (rows[idx].saved || rows[idx].connected) {
          wifiOpenMenu(rows[idx]);
        } else {
          st.dirty = 1;   // 没存过的网络没有菜单可弹（只有"连接"）
        }
      }
    }
    return;
  }

  if (n > 0) {
    ListView lv = wifiCurrentList(rows);
    if (listViewKey(lv, key)) {
      g_wui.selSsid = rows[lv.sel].ssid;
      g_wui.scroll = lv.first;
      st.dirty = 1;
      return;
    }
  }

  if (key == '\n' || key == 0x0D) {
    if (n > 0) {
      int tx = 0, ty = 0;
      if (input_tap_xy(&tx, &ty)) {
        const ListView lv = wifiCurrentList(rows);
        const int idx = listViewHitAt(lv, ty);
        if (idx < 0) return;   // 点空白：落点已吞，不动
        g_wui.selSsid = rows[idx].ssid;
        st.dirty = 1;
        wifiActivateRow(rows[idx]);
        return;
      }
      wifiActivateRow(rows[wifiIndexForSel(rows)]);
    }
    return;
  }
}

// ── 对外入口 ─────────────────────────────────────────────────────────────
void renderWifi() {
  switch (g_wui.view) {
    case WV_MENU: renderWifiMenu(); break;
    case WV_CONFIRM: renderWifiConfirm(); break;
    case WV_HELP: renderWifiHelp(); break;
    case WV_PASS: renderWifiPass(); break;
    default: renderWifiList(); break;
  }
}

void handleWifi(int key) {
  // 进本屏后第一次按键时，扫描可能早就完成了 —— 先让结果落地，再处理这一键
  // （空闲帧那条路见 rdWifiIdleTick）。
  g_wifi.scanPoll();

  switch (g_wui.view) {
    case WV_MENU: handleWifiMenu(key); return;
    case WV_CONFIRM: handleWifiConfirm(key); return;
    case WV_HELP: handleWifiHelp(key); return;
    case WV_PASS: handleWifiPass(key); return;
    default: handleWifiList(key); return;
  }
}

// 进本页（screen_reader.cpp 的 MenuAct::Wifi 调）：复位界面态 + 起扫描。
void rdWifiEnter() {
  g_wui = WifiUiState();
  st.wifiField = 0;
  st.wifiEditing = false;
  st.wifiBusy = false;
  st.wifiSsidEdit.clear();
  st.wifiPassEdit.clear();
  st.vkVisible = false;
  g_wifiStore.load();   // 懒加载 + 旧键（wifi_ssid/wifi_pass）迁移到内存表
  g_wui.lastConn = g_wifi.isConnected();
  g_wui.lastConnSsid = g_wifi.connectedSsid();
  if (g_wifi.enabled()) {
    g_wifi.begin();
    g_wifi.scanStart();
  }
}

// VK/IME 的回车提交（screen_reader.cpp 的 vkEnter 调）。
void wifiCommitEdit() {
  st.wifiEditing = false;
  st.vkVisible = false;
  if (g_wui.view != WV_PASS) {
    st.dirty = 1;
    return;
  }
  if (st.wifiField == 0) {
    // 刚填完网络名：跳到密码那一格接着打（「+ 添加网络」手动输入那条路）。
    // 别在这里直接连 —— 那会拿一个空密码去试。
    if (st.wifiSsidEdit.empty()) {
      g_wui.status = "网络名不能为空";
      st.dirty = 1;
      return;
    }
    st.wifiField = 1;
    st.wifiEditing = true;
    rdVkWantShow();
    st.dirty = 1;
    return;
  }
  wifiDoConnect();
}

// 空闲帧推进：扫描结果落地（SCAN_DONE 只立旗，取回在 UI 线程）+ 连接态变化。
// 返回 true = 界面该重绘（空闲帧没有帧末统一推屏，调用方收帧）。
bool rdWifiIdleTick() {
  if (st.mode != RdMode::Wifi) return false;
  bool changed = false;
  if (g_wifi.scanPoll()) changed = true;

  const bool conn = g_wifi.isConnected();
  const std::string ssid = g_wifi.connectedSsid();
  if (conn != g_wui.lastConn || ssid != g_wui.lastConnSsid) {
    g_wui.lastConn = conn;
    g_wui.lastConnSsid = ssid;
    g_wui.status.clear();   // 连接态变了，旧的一次性提示（多半是"连接失败"）作废
    changed = true;
  }
  return changed;
}
