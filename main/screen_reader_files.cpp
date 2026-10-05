// screen_reader_files.cpp — 阅读器的「文件」根标签：文件浏览器 + 文件操作菜单 + 图片查看器。
//
// P3b 的第四个切片。这里装的是整个"文件"标签这一族：SD 目录浏览（fbScan /
// renderFileBrowser / handleFileBrowser）、长按弹出的文件菜单（打开/重命名/删除/详情/
// 复制剪切粘贴）、重命名与详情两个子页，以及图片查看器（renderImage/handleImage，
// 含缩放与"设为待机画面"）。它们互相调用（文件菜单 → 重命名/详情，浏览器 → 图片查看器），
// 对外则只被主文件的"切标签 / 分发"和阅读菜单的"打开文件"叫到。
//
// 搬过来时**逻辑一行没改**：只动了 static、include，并把 15 个入口提到
// screen_reader_internal.h；用到的阅读器原语（VK 层、rdShowBusy、renderNoteEdit…）
// 由同一份内部头声明。

#include "screen_reader_internal.h"
#include "screen_reader.h"            // readerSetStandbyImage（图片查看器的"设为待机"）
#include "settings_manager.h"
#include "ui/list_view.h"
#include "ui/ime_field.h"
#include "hw/input.h"
#include "hw/board.h"
#include "wifi_manager.h"          // g_wifi（传书地址）
#include "file_manager_server.h"   // FmXfer / 传书服务器起停
#include "icon_font.h"             // 新建/刷新 FAB 的图标字形
#include "bt_keyboard.h"           // g_bt（蓝牙键盘连接状态）
#include "editor_vk.h"             // 重命名框复用编辑器那套虚拟键盘
#include "ime/IME.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <vector>

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <ImageBlock.h>  // ImageBlock::ditherModeEnabled()（图片查看器与插图同一档抖动）
#include <ImageDecoderFactory.h>
#include <esp_log.h>
#include <esp_timer.h>

static const char *TAG = "Reader";

// 图片查看器的两个口在下面定义，fbScan / 文件菜单先要用。
static bool rdIsImageName(const std::string &n);
static void imgBuildList(const std::string &path);

void fbScan(const std::string &dir) {
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

// 进文件浏览器：重扫当前目录。三个入口（1 号位应用页的文件夹图标、书架菜单的
// 「文件浏览」、退出阅读模式后恢复到文件浏览器）都得过这一道。原先只有书架菜单那条路
// 会扫描，从标签直接进来（或重启后恢复）时 st.fbEntries 是空的 —— 列表空白，而且空列表
// 会提前 return 吃掉点按，连标签都切不走，看上去就是"卡死"。
void rdEnterFileTab() {
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

// 这几个动作定义在后面的「文件长按菜单」段（那边才拿到 FileRename 的输入页设施），
// 而 handleFileBrowser 的键盘快捷键块（n/r/c/x/v/e/d/i）要先调它们——和「+」一样
// 得在这里前置声明。注意 fm* 读的是 st.fmIdx（"长按锁定的那一行"），调用前要把它
// 对齐到 st.fbSel。
static void fmBeginMkdir();
const BookEntry *fmTarget();
static void fmCopy(bool cut);
static void fmPaste();
static void fmBeginRename();
static void fmDeleteConfirmed();
static void fmBackToBrowser();     // 回文件浏览器（顺手把 fmStatus 转成浮动提示）
static void fmSetStandbyImage();   // 图片 → 待机表盘「图片」（定义在「文件长按菜单」段）

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
std::string rdNetXferText() {
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
bool rdWifiEnsure() {
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
void rdNetTick() {
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

// 列表几何：**渲染和命中共用这一个**。以前两边各抄一遍同样的 maxRows/start 式子
// （fbRowAtY 就是照 renderFileBrowser 抄的），行高或页脚一改就只改一处，症状是
// "滚动过之后点哪行开隔壁那行"。
static ListView fbListView() {
  ListView lv;
  lv.top = fbListTop();
  lv.itemH = uiLineHeight() + 6;
  lv.count = static_cast<int>(st.fbEntries.size());
  lv.rows = std::max(1, (tabBottom() - lv.top - 8) / lv.itemH);
  lv.sel = st.fbSel;
  listViewCenter(lv);  // 居中式窗口：选中项尽量停在窗口中段
  return lv;
}

// 点按 y → 条目下标（-1 = 不在任何一行上：面包屑/空白/页脚那半行余量）。
static int fbRowAtY(int y) { return listViewHitAt(fbListView(), y); }

// 「设为待机画面」：把这张图解成待机缓存、设置键指过来、表盘切到「图片」。
// 这一趟是**阻塞**的（解一张几百万像素的图是秒级），所以先刷一帧"正在…"再进去
// （同 rdShowBusy 那套），跟开大书一个道理：e-ink 上几秒不动的屏幕看着就像死机。
// 失败原因经 st.fmStatus 交给 fmBackToBrowser 转成浮动提示。
static void fmSetStandbyImage() {
  const BookEntry *e = fmTarget();
  if (!e) { fmBackToBrowser(); return; }
  const std::string path = e->path, name = e->name;
  rdShowBusy("正在生成待机图片…", name);
  std::string err;
  const bool ok = readerSetStandbyImage(path, err);
  ESP_LOGI(TAG, "文件菜单设为待机画面: %s (%d) %s", path.c_str(), static_cast<int>(ok), err.c_str());
  st.fmStatus = ok ? std::string("已设为待机画面") : (std::string("设置失败: ") + err);
  fmBackToBrowser();
}

// 打开一个条目：目录往下走、图片进看图器、书进阅读页。列表回车和长按菜单的「打开」
// 共用同一条路径，免得两处行为漂移。
// 参数**按值收**（不是 const&）：目录分支里 fbScan(e.path) 会 clear() 掉 st.fbEntries，
// 而调用方常常直接传 st.fbEntries[st.fbSel] —— 传引用的话 e.path 指向的那个 std::string
// 此刻已被析构，后面 opendir(dir.c_str()) 读的就是已释放内存。
static void fbOpenEntry(BookEntry e) {
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

void renderFileBrowser() {
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
  // 几何（首行上沿 / 行高 / 可见行数 / 窗口起点）走 fbListView —— 点按命中用的是
  // 同一个函数，两边不可能再错位。
  const ListView fblv = fbListView();
  int top = fblv.top;
  int n = fblv.count;
  if (n == 0) drawCenteredLine(g_rd.getScreenHeight() / 2, "空目录");
  int itemH = fblv.itemH;
  int maxRows = fblv.rows;
  int start = fblv.first;
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
  const std::string xf = rdNetXferText();
  const int hintY = tabBottom() - uiLineHeight();
  if (!xf.empty()) {
    g_rd.fillRect(0, hintY, w, uiLineHeight(), false);   // 先擦白，免得和末行文字叠字
    drawLineText(MARGIN, hintY, xf.c_str(), true);
  } else if (g_bt.isConnected()) {
    // 没在传书时这一行改画实体键盘快捷键提示——**只在接了蓝牙键盘时画**：这几行字正是
    // 给"摸不到右下角那三个浮动按钮、只能敲键盘"的人看的（键位见 handleFileBrowser 里
    // 的那块）。右端必须让开 FAB 那一列：「传书」按钮就压在这一行的右段上，擦白和文字
    // 都只到 fbFabX()-8 为止，越界就把按钮蹭花了。
    const std::string hint = g_rd.truncatedText(
        uiFontId(), "n 新建  r 刷新  e 改名  d 删除  c/x/v 复制  f 传书", fbFabX() - 8 - MARGIN);
    g_rd.fillRect(0, hintY, fbFabX() - 8, uiLineHeight(), false);
    drawLineText(MARGIN, hintY, hint.c_str(), true);
  }
  // 浮动按钮画在列表之后（压住右下角那格的一部分，这是 FAB 的常态）。
  fbDrawFab();
  fbDrawNewFab();
  fbDrawRefreshFab();
}

void handleFileBrowser(int key) {
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
      // 已经在卡根：退回「应用」标签的图标入口页 —— 文件浏览器现在是它的子界面
      // （1 号位改成应用页之后，卡根再往上退就是那一页；想回书架再 Esc 一次）。
      // 标签保持 1（st.tab 不动），所以标签栏的高亮本来就是对的。
      st.fbSel = 0;
      st.mode = RdMode::Apps;
      st.fullRefresh = true;
      st.dirty = 1;
      return;
    }
    st.dirty = 1;
    return;
  }
  // ── 实体键盘快捷键（蓝牙键盘）───────────────────────────────────────────
  // 上面这些事原来只能点右下角那三个浮动按钮、或长按列表项弹菜单——插着键盘的人得腾出
  // 一只手去戳屏幕。键位取文件管理器的惯例：
  //   n 新建文件夹   r 刷新   c 复制   x 剪切   v 粘贴   e 改名
  //   d 删除（连按两次，别的键撤销）   i 详情   f 网络传书   Enter 打开
  // 作用对象一律取当前选中行：fm* 那几个动作读的是 st.fmIdx（"长按锁定的那一行"），
  // 先把它对齐到 st.fbSel 再动，免得作用在上一次长按选中的条目上。
  // 提示行画在 renderFileBrowser 的底边（只在蓝牙键盘连着时出现）。
  if (key >= 'A' && key <= 'Z') key = key - 'A' + 'a';   // Shift+N 与 n 等价
  if (key >= 'a' && key <= 'z') {
    // n/r/f/v 与选中行无关（空目录里也能用），其余的在空目录里直接吃掉。
    if (n == 0 && key != 'n' && key != 'r' && key != 'f' && key != 'v') { st.dirty = 1; return; }
    st.fmIdx = st.fbSel;
    switch (key) {
      case 'n': fmBeginMkdir(); return;
      case 'r': fbRefreshAction(); return;
      case 'f': fbFabAction(); return;
      case 'c': fmCopy(false); return;
      case 'x': fmCopy(true); return;
      case 'v': fmPaste(); return;
      case 'e': fmBeginRename(); return;
      case 'i': st.mode = RdMode::FileInfo; st.fullRefresh = true; st.dirty = 1; return;
      case 'd': {
        // 两次确认，和长按菜单同一条规矩（共用 st.fmDelArm，标签也共用）：第一下只置位
        // 并说一声，第二下才真删。撤位的责任在下面那一条（**除 d 以外**的任何键都撤）。
        if (!st.fmDelArm) {
          st.fmDelArm = true;
          const BookEntry *e = fmTarget();
          rdShowFloat(std::string("再按一次 d 确认删除") + (e ? " " + e->name : ""), "",
                      4000);
          st.dirty = 1;
          return;
        }
        fmDeleteConfirmed();
        return;
      }
      default: break;
    }
  }
  // 撤销"删除待确认"：只在真的按了别的键时撤，**空转帧（key==0）绝不能撤**——那两下中间
  // 夹着一个空闲帧，撤了就永远等不到第二次 d。
  if (key > 0 && key != 'd' && st.fmDelArm) st.fmDelArm = false;
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
  {  // 上下/翻页/首尾的算术在 ui/list_view.h（与渲染同一个 fbListView 几何）
    ListView lv = fbListView();
    if (listViewKey(lv, key)) { st.fbSel = lv.sel; st.dirty = 1; return; }
  }
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
void drawVk();
void vkTap(int x, int y);
void rdToggleVk();
int vkVkTop();
void rdDrawVkIcon();
bool rdVkIconHit(int x, int y);
void feedVkKey(int c);
void feedVkBackspace();
bool rdImeBarOn();
int rdBodyBottom();          // 正文可用底边（虚拟键盘 / 输入法条 / 提示行三选一）
void drawRdImeBar();  // 实体键盘打字时的编码行+候选行

static const int kFileMenuItemMax = 9;

const BookEntry *fmTarget() {
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
enum { FM_OPEN = 0, FM_STANDBY, FM_COPY, FM_CUT, FM_PASTE, FM_RENAME, FM_DELETE, FM_INFO };
static int fileMenuActions(int *acts, int maxN) {
  const BookEntry *e = fmTarget();
  int n = 0;
  if (e && e->kind != 4) acts[n++] = FM_OPEN;   // kind 4（其它文件）没有可打开的动作
  // 「设为待机画面」只对图片出现（kind 3 = 看图器认得的那三种后缀）。放在「打开」
  // 后面：打开是看，设为待机是留，先看后留。
  if (e && e->kind == 3) acts[n++] = FM_STANDBY;
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
      case FM_STANDBY: out.push_back("设为待机画面"); break;
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

void renderFileMenu() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const BookEntry *e = fmTarget();
  std::string title = e ? g_rd.truncatedText(uiFontId(), e->name.c_str(), w - 2 * MARGIN) : "文件菜单";
  drawTitle(title.c_str());
  std::vector<std::string> items;
  fileMenuLabels(items);
  const ListView lv = flatMenuListView(static_cast<int>(items.size()), st.fmSel);
  const int n = lv.count;
  const int itemH = lv.itemH;
  for (int i = 0; i < n; i++) {
    int y = lv.top + i * itemH;
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

// 进入文件名输入页时把虚拟键盘摆出来。注意不能只置 st.vkVisible 位：可见性的**唯一
// 真相**是 st.vkVisible，但真正决定画不画的是 editor_vk 的 s_visible，两者靠 rdSyncVk()
// 在绘制前对齐。这里显式调 editorVkAutoShow() 图的是它顺手做的两件事：
//   1) evkLoadLayout()——把用户上次选的键位布局读回来（阅读模式从不调 editorVkInit，
//      只有这条路能拿回布局）；
//   2) 清掉上一次输入会话的按键状态——新建文件夹是空串，挂着上一段（比如重命名）的
//      拼音组合/候选会直接往空名字里塞字。
// 它里面那句"蓝牙键盘连上就不弹"（s_userOverride）在阅读模式**不生效**：rdSyncVk()
// 每次绘制前都拿 st.vkVisible 覆盖 s_visible，用的又是会置 s_userOverride 的
// editorVkSetVisible()，所以这里的状态由 st.vkVisible 说了算——面板照弹，用户想收起
// 就点状态栏右端那个键盘图标（收起来后实体键盘打字仍有 drawRdImeBar 那条候选条）。
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
  fmAutoShowVk();   // 弹面板（实体键盘连着时也弹，收起来后仍有 drawRdImeBar 的候选条）
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
  ESP_LOGI(TAG, "新建文件夹: 虚拟键盘弹出(可点状态栏图标收起)");
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
    case FM_STANDBY: fmSetStandbyImage(); break;
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

void handleFileMenu(int key) {
  std::vector<std::string> items;
  fileMenuLabels(items);
  const int n = static_cast<int>(items.size());
  if (key == 0x1B || key == KEY_LONG_CONFIRM || key == KEY_BACK) { fmBackToBrowser(); return; }
  {  // 上下选择在 ui/list_view.h；两行都顺手撤掉删除待确认
    ListView lv = flatMenuListView(n, st.fmSel);
    if (listViewKey(lv, key)) { st.fmDelArm = false; st.fmSel = lv.sel; st.dirty = 1; return; }
  }
  if (key == '\n') {
    int x, y;
    if (input_tap_xy(&x, &y)) {
      const int row = listViewHitAt(flatMenuListView(n, st.fmSel), y);
      if (row >= 0) {
        if (row != st.fmSel) st.fmDelArm = false;   // 换了行就取消待确认
        st.fmSel = row;
      }
    }
    fileMenuAction(st.fmSel);
    return;
  }
}

// ── 文件详情（只读）────────────────────────────────────────────────────
std::string humanSize(long long bytes) {
  char b[48];
  if (bytes < 1024) snprintf(b, sizeof(b), "%lld B", bytes);
  else if (bytes < 1024 * 1024) snprintf(b, sizeof(b), "%.1f KB", bytes / 1024.0);
  else snprintf(b, sizeof(b), "%.1f MB", bytes / (1024.0 * 1024.0));
  return b;
}

void renderFileInfo() {
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

void handleFileInfo(int key) {
  (void)key;   // 只读：任意键都退回文件列表
  fmBackToBrowser();
}

// ── 重命名输入（虚拟键盘）──────────────────────────────────────────────
void renderFileRename() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const BookEntry *e = fmTarget();
  const int top = drawTitle(st.fmMkdir ? "新建文件夹" : "重命名");
  const std::string ext = (!st.fmMkdir && e) ? nameExt(e->name) : std::string();

  int y = top;
  drawLineText(MARGIN, y, st.fmMkdir ? "文件夹名" : "新名字", true);
  y += uiLineHeight() + 5;
  // 输入行：主干 + 固定的扩展名 + 光标。扩展名是灰色概念，这里用普通文字标出来即可。
  // 空串的占位话分两种：面板开着时说"点下方键盘"，没开时（接了蓝牙键盘，面板被收起）
  // 得告诉人**往哪儿点才能把面板叫回来**——那个图标在状态栏右端，不在这行文字下面。
  std::string shown = !st.fmRenameBuf.empty() ? st.fmRenameBuf
                    : st.vkVisible ? std::string("点下方键盘输入")
                                   : std::string("直接打字，或点右下角键盘图标");
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
  else {
    drawRdImeBar();   // 蓝牙键盘打字：候选条照样要有（见 drawRdImeBar）
    drawFooter("回车保存   Esc 取消");
  }
  rdDrawVkIcon();
}

void handleFileRename(int key) {
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
      if (rdVkIconHit(x, y)) { rdToggleVk(); return; }
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
bool s_imgPresentSkipped = false;  // 中止的那一帧是半张图，别推屏（renderCurrent 也要看）

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

void renderImage() {
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
    cfg.ditherMode = ImageBlock::ditherModeEnabled();
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
  // 图片查看器整屏都是灰阶照片：面板上会留下真中灰，记账给白底纪律用（见 renderCurrent 尾）。
  if (ok) st.frameGray = 1;
  if (!ok) drawCenteredLine(areaH / 2, "无法解码此图片");

  // 页脚：索引/文件名/缩放 + 设为待机的快捷键（s）。没有列就只报个数 —— 页脚一行
  // 放不下时由 drawFooter 自己截，这里不预先量宽。
  size_t slash = path.rfind('/');
  std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
  char info[256];
  snprintf(info, sizeof(info), "%d/%d %s  %d%%  s:设为待机", st.imgSel + 1,
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

void handleImage(int key) {
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
  if (key == 's' || key == 'S') {
    // 设为待机画面：与文件长按菜单那条同一个动作（readerSetStandbyImage 里把表盘
    // 也切到「图片」）。解图是秒级的，先给一帧"正在…"。
    const std::string p = st.imgList[st.imgSel];
    rdShowBusy("正在生成待机图片…", p);
    std::string err;
    const bool ok = readerSetStandbyImage(p, err);
    ESP_LOGI(TAG, "看图器设为待机画面: %s (%d) %s", p.c_str(), static_cast<int>(ok), err.c_str());
    // 这里不能走 fmBackToBrowser 那套：看图器没有"状态行"，直接弹浮动提示，
    // 提示清掉之后这一帧自己重绘（图片本来就是刚解好的，重绘只花一次解码）。
    if (ok) rdShowFloat("已设为待机画面", "文件管理里长按图片也能设", 3000);
    else rdShowFloat(std::string("设置失败: ") + err, "", 3000);
    st.dirty = 1;
    st.fullRefresh = true;
    return;
  }
  if (st.imgZoom > 1.01f) return;
  if (key == KEY_LEFT) { imgSet(st.imgSel - 1); return; }
  if (key == KEY_RIGHT) { imgSet(st.imgSel + 1); return; }
  if (key == KEY_HOME) { imgSet(0); return; }
  if (key == KEY_END) { imgSet(static_cast<int>(st.imgList.size()) - 1); return; }
}

// ── 清理缓存 ─────────────────────────────────────────────────────────────
// 删除 .crossmux 下所有书缓存（封面/排版/书签索引），再重建空目录。
