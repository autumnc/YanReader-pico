// screen_reader_weread.cpp — 微信读书子应用（从 screen_reader.cpp 拆出来的第一块）。
//
// P3b 的第一个切片：screen_reader.cpp 有 12343 行 / 561KB，编译和翻页都受影响。微读
// 这块边界最干净——只碰 st.we* 那一组字段，加上几个外壳原语（标题栏/底栏/标签栏/
// openBook/switchTab），不与翻页、排版、词典那些东西纠缠。共享的状态量与外壳原语在
// screen_reader_internal.h，主文件的分发只认这里的 9 个入口。
//
// 搬过来时**只动了 static 和 include**：逻辑一行没改。
//
// 状态机由 weDrive() 在按键循环里逐拍推进；这里做四件事：
// 书架列表（RdMode::Weread）、扫码（WereadQr）、书目菜单（WereadMenu）、缓存进度（WereadDl）。

#include "screen_reader_internal.h"

#include <algorithm>
#include <cstdio>
#include <new>
#include <utility>

#include <EpdFontFamily.h>
#include <esp_heap_caps.h>

#include "qrcodegen.h"
#include "hw/input.h"   // input_tap_xy：点按只读一次（读后即清）
#include "wifi_manager.h"

// 整库移植自 crossmux 的 WeReadWebApi（登录/书架/章节下载/EPUB 打包）。
// Operation 约 8KB（内含 4KB 收发缓冲），内部 RAM 紧张，优先放 PSRAM。
std::unique_ptr<WeReadClient::Operation, WeOpDeleter> weMakeOperation() {
  void *raw = heap_caps_malloc(sizeof(WeReadClient::Operation), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!raw) raw = heap_caps_malloc(sizeof(WeReadClient::Operation), MALLOC_CAP_8BIT);
  if (!raw) return nullptr;
  return std::unique_ptr<WeReadClient::Operation, WeOpDeleter>(new (raw) WeReadClient::Operation());
}

// ── 微信读书 ────────────────────────────────────────────────────────────

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

static const char *weStageName(WeReadClient::Operation::ProgressStage s, bool coverOnly) {
  switch (s) {
    // 补封面这一趟里"章节"那一程只是把卡上的正文核对一遍（缺的才补），一个字都没下，
    // 照整本缓存的字面写"缓存章节"是骗人的。
    case WeReadClient::Operation::ProgressStage::Chapters: return coverOnly ? "核对章节" : "缓存章节";
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
  // kind 2 = 「重新获取封面」：走同一条整本缓存链，只是留着卡上的正文、强制重抓封面源图、
  // 跳过进度同步（见 DownloadOptions::coverOnly）。
  options.coverOnly = (kind == 2);
  const WeReadClient::Operation::Kind k = (kind == 1 || kind == 2)
                                              ? WeReadClient::Operation::Kind::Download
                                              : WeReadClient::Operation::Kind::Sync;
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
void weDrive() {
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
      } else if (st.weKind == 2) {
        // 封面重抓：epub 已在原地原子替换（新的封面图嵌在里头了），但本机那几张封面产物
        // 还停在老封面上（生成端都是"文件在就跳过"，光换 epub 没用）——全部删掉重建。
        // 这一步要解一遍 zip + 缩放出两张图（秒级），进度屏就停在最后一帧，随后回书目菜单。
        const std::string path = st.weOp->finalPath();
        if (!path.empty()) rdRebuildBookCoverArtifacts(path, 0);
        st.weStatus = "封面已更新";
        st.mode = RdMode::WereadMenu;
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
      st.mode = (st.weKind == 1 || st.weKind == 2) ? RdMode::WereadMenu : RdMode::Weread;
      st.fullRefresh = true;
      st.dirty = 1;
      break;
    case WeReadClient::Operation::Event::Failed:
      st.weStatus = weErrorText(st.weOp->error());
      st.mode = (st.weKind == 1 || st.weKind == 2) ? RdMode::WereadMenu : RdMode::Weread;
      st.fullRefresh = true;
      st.dirty = 1;
      break;
    case WeReadClient::Operation::Event::DetailReady:
    case WeReadClient::Operation::Event::ChapterRangeReady:
    case WeReadClient::Operation::Event::ChapterComplete:
      weMarkDirtyIfProgressChanged();   // 进度画面刷新（内容变了才推，见上）
      break;
    case WeReadClient::Operation::Event::None:
      if (st.mode == RdMode::WereadDl && st.weOp->active()) weMarkDirtyIfProgressChanged();
      break;
  }
}

// 书架行（两行文字：书名 + 作者/本地状态）。
static int weItemH() { return uiLineHeight() * 2 + 8; }

static int weBookIndex() {
  return (st.weSel >= 0 && st.weSel < static_cast<int>(st.weShelf.size())) ? st.weSel : -1;
}

// 书目菜单：本地已有缓存就给"打开/重新获取封面/重新缓存/删除"，否则只有"缓存整本并阅读"。
// 「重新获取封面」对**已缓存**的书一律给（不去探测它到底有没有封面）：老固件缓存出来的
// 书可能压根没下过封面，这正是这一项要补的场景；有封面的书再抓一次也无非重下一张图。
static std::vector<std::string> weMenuItems() {
  const int idx = weBookIndex();
  bool cached = false;
  if (idx >= 0) cached = Storage.exists(WeReadStore::finalBookPath(st.weShelf[idx]).c_str());
  if (cached) return {"打开本书", "重新获取封面", "重新缓存", "删除本地缓存", "取消"};
  return {"缓存整本并阅读", "取消"};
}

// 书目菜单的行高与首行 y（renderWereadMenu 与命中测试共用）。
static int weMenuItemH() { return uiLineHeight() + 12; }
static int weMenuFirstY() { return coverTop() + uiLineHeight() + 14; }

// 微信读书书架列表：居中式窗口（行高 = 两行：书名 + 作者）。渲染与点按命中共用
// 同一个几何——渲染原来还负责把 sel/scroll 夹回合法范围并写回 st，这里照旧。
static ListView weShelfListView() {
  ListView lv;
  lv.top = coverTop();  // == drawTabBar 的返回值
  lv.itemH = weItemH();
  lv.count = static_cast<int>(st.weShelf.size());
  lv.rows = std::max(1, (statusTop() - lv.top) / lv.itemH);
  // 上下滑 = 整页翻（一屏行数）：微读书架动辄几十本，"一格一格挪"走不动。
  // 单步留给方向键/BLE 键盘的 KEY_UP/DOWN，与目录/本地书架同一套口径。
  lv.page = lv.rows;
  lv.sel = st.weSel;
  listViewCenter(lv);
  return lv;
}

void renderWeread() {
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
  const ListView lv = weShelfListView();
  const int itemH = lv.itemH;
  st.weSel = lv.sel;
  st.weScroll = lv.first;

  int y = lv.top;
  for (int i = st.weScroll; i < lv.count && y + itemH <= bottom; i++) {
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

void renderWereadQr() {
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

void renderWereadMenu() {
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

void renderWereadDl() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const bool coverOnly = (st.weKind == 2);
  const int top = drawTitle(coverOnly ? "微信读书 更新封面" : "微信读书 缓存图书");
  std::string title = st.weJobTitle.empty() ? "正在准备" : st.weJobTitle;
  title = g_rd.truncatedText(uiFontId(), title.c_str(), w - 2 * MARGIN, EpdFontFamily::REGULAR);
  drawLineText(MARGIN, top + 8, title.c_str(), true);

  uint32_t done = 0, total = 0;
  const char *stage = "准备中";
  if (st.weOp) {
    done = st.weOp->progressCompleted();
    total = st.weOp->progressTotal();
    stage = weStageName(st.weOp->progressStage(), coverOnly);
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

void handleWeread(int key) {
  if (key == KEY_LEFT) { switchTab(st.tab - 1); return; }
  if (key == KEY_RIGHT) { switchTab(st.tab + 1); return; }
  if (key == 0x1B || key == KEY_LONG_CONFIRM) {
    // 从「应用」标签的微读图标进来的，Esc 回那一页（retMode 由 rdOpenApp 置位）。
    // 兜底那条 switchTab(0) 现在几乎没有入口了（书架栏那枚图标 2026-10-05 已删），
    // 留着是因为 retMode 万一被别处写脏，退回书架总比留在原地强。
    if (st.retMode == RdMode::Apps) {
      st.mode = RdMode::Apps;
      st.retMode = RdMode::Browser;
      st.fullRefresh = true;
      st.dirty = 1;
      return;
    }
    switchTab(0);
    return;
  }
  {  // 上下/翻页在 ui/list_view.h（与 renderWeread 共用同一个 weShelfListView 几何）
    ListView lv = weShelfListView();
    if (listViewKey(lv, key)) { st.weSel = lv.sel; st.dirty = 1; return; }
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
  if (tapped) {
    const int idx = listViewHitAt(weShelfListView(), y);
    if (idx >= 0) st.weSel = idx;
  }
  st.weMenuSel = 0;
  st.weStatus.clear();
  st.mode = RdMode::WereadMenu;
  st.fullRefresh = true;
  st.dirty = 1;
}

void handleWereadQr(int key) {
  if (key == 0x1B || key == KEY_LONG_CONFIRM) {
    if (st.weOp) st.weOp->cancel();
    return;
  }
}

void handleWereadMenu(int key) {
  const auto items = weMenuItems();
  const int n = static_cast<int>(items.size());
  {  // 上下选择在 ui/list_view.h（与 renderWereadMenu 共用同一个几何）
    ListView lv = flatMenuListView(n, st.weMenuSel, weMenuFirstY());
    if (listViewKey(lv, key)) { st.weMenuSel = lv.sel; st.dirty = 1; return; }
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
    const int hit = listViewHitAt(flatMenuListView(n, st.weMenuSel, weMenuFirstY()), ty);
    if (hit >= 0) st.weMenuSel = hit;
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
  // 重新获取封面：卡上有正文，所以很快（不重下正文，只重抓封面源图 + 重打包 epub）。
  // 它也走进度屏——重打包是秒级的事，没有进度画面用户会以为没反应。
  if (label == "重新获取封面") {
    weBeginJob(2, &b);
    if (st.weKind == 2 && st.weStatus.empty()) {
      st.mode = RdMode::WereadDl;
      st.fullRefresh = true;
      st.dirty = 1;
    }
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

void handleWereadDl(int key) {
  if ((key == 0x1B || key == KEY_LONG_CONFIRM) && st.weOp) st.weOp->cancel();
}

