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
#include "ime/IME.h"    // 发现页的搜索框要打拼音（IME::cancelComposition）
#include "hw/input.h"   // input_tap_xy：点按只读一次（读后即清）
#include "reading_stats.h"  // 本机整书进度（lastProgressPercent），进度同步要用
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

// 进度画面的"内容变了吗"快照（stage/done/total 就是 renderWereadDl 画的全部东西）。
// 只有内容真的变了才标脏重推：原来 Event::None 分支每拍都置 dirty，于是每个空闲拍都
// 推一次屏 —— 一次整屏差分推屏是阻塞的（~300ms），一本书缓存几分钟，这些推屏既拖慢
// 缓存，又在攒残影预算（攒够就升一次整屏 GC16，见 rdSelfTickingProgress 的注释）。
static int s_weLastStage = -1;
static uint32_t s_weLastDone = 0, s_weLastTotal = 0;
static bool s_weLastValid = false;

static void weProgressForget() { s_weLastValid = false; }

// 内容变了才标脏。任务不在/已结束时也作废快照，免得下一个任务的第一拍被上一次的值挡住。
static void weMarkDirtyIfProgressChanged() {
  if (!st.weOp) { weProgressForget(); return; }
  const int stage = static_cast<int>(st.weOp->progressStage());
  const uint32_t done = st.weOp->progressCompleted();
  const uint32_t total = st.weOp->progressTotal();
  if (s_weLastValid && stage == s_weLastStage && done == s_weLastDone && total == s_weLastTotal) return;
  s_weLastStage = stage;
  s_weLastDone = done;
  s_weLastTotal = total;
  s_weLastValid = true;
  st.dirty = 1;
}

// 起一个新任务（登录/书架同步 kind=0，整本缓存 kind=1，只补封面 kind=2，书籍详情 kind=3）。
static void weBeginJob(int kind, const WeReadStore::ShelfRecord *book) {
  weProgressForget();
  // 任务结束后回哪一屏。发现页起的任务（详情 / 缓存整本）要回发现页，其余回书目菜单；
  // 调用方（发现页那两处）在返回后覆盖成 RdMode::WereadDiscover。
  st.weJobRet = (kind == 0) ? RdMode::Weread : RdMode::WereadMenu;
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
  if (kind == 3) {
    // 书籍详情：一条 /web/book/info（外加封面），不走整本缓存那条链。options 对 Detail 无意义
    // （库里只在 kind==Download 时才收它），传去也无妨。
    if (!book || !st.weOp->begin(WeReadClient::Operation::Kind::Detail, book, options)) {
      st.weStatus = book ? weErrorText(st.weOp->error()) : "书目已失效";
      st.dirty = 1;
      return;
    }
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (kind == 4) {
    // 笔记/划线：走 Browse 缓存任务（一次抓 热门划线→我的划线→热门书评，库里自动级联），
    // 用 bookRecord（Browse 不收 ShelfRecord）。它**没有中间进度事件**，只有 Complete——
    // 界面这一头就画"正在获取…"（见 renderWereadNotes）。
    if (!book) { st.weStatus = "书目已失效"; st.dirty = 1; return; }
    const WeReadStore::BookRecord rec = WeReadStore::bookRecord(*book);
    if (!st.weOp->beginBrowseCache(rec)) {
      st.weStatus = weErrorText(st.weOp->error());
      st.dirty = 1;
      return;
    }
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (kind == 5) {
    // 阅读进度同步：方向在 st.weSyncMode（Compare / UploadLocal / ApplyRemote），本机进度
    // 在 st.weSyncLocalPercent。只给 fraction、basis 留 None —— 库里会拿卡上的 toc.bin 把
    // 百分比映射到章节，不必真的打开这本书。
    if (!book) { st.weStatus = "书目已失效"; st.dirty = 1; return; }
    WeReadClient::ProgressSyncInput in;
    in.localFraction = clampI(st.weSyncLocalPercent, 0, 100) / 100.0f;
    if (!st.weOp->beginProgressSync(book->bookId, in, st.weSyncMode)) {
      st.weStatus = weErrorText(st.weOp->error());
      st.dirty = 1;
      return;
    }
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  if (!st.weOp->begin(k, book, options)) {
    st.weStatus = weErrorText(st.weOp->error());
    st.dirty = 1;
    return;
  }
  st.fullRefresh = true;
  st.dirty = 1;
}

// 详情数据从盘上读（DetailReady 之后 detail.bin 就已经写好了）。定义在文件后半
// （renderWereadDetail 一带），这里要用所以先声明。
static void weLoadDetailData();

// 笔记/划线：Browse 缓存的 manifest + 条数读回来（返回是否有可用缓存）。定义在后面。
static bool weLoadNotesData();

// 当前选中的书架行下标（定义在 weDrive 之后，进度同步那一支要用）。
static int weBookIndex();

// 任务结束后回到哪一屏：默认回书目菜单（缓存/封面/详情/笔记都是从那里进去的），登录/书架
// 回书架。发现页起的任务在 weBeginJob 之后把 weJobRet 改成 WereadDiscover。
static RdMode weAfterJobMode() {
  return (st.weKind == 0) ? RdMode::Weread : st.weJobRet;
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
      } else if (st.weKind == 3) {
        // 书籍详情：detail.bin 已经落盘，读进来摊成可滚动的简介行，切到详情屏。
        weLoadDetailData();
        st.weStatus.clear();
        st.mode = RdMode::WereadDetail;
      } else if (st.weKind == 4) {
        // 笔记/划线：Browse 缓存已 commit，读 manifest + 条数，折行留给渲染那拍。
        weLoadNotesData();
        st.weStatus.clear();
        st.mode = RdMode::WereadNotes;
      } else if (st.weKind == 5) {
        // 进度同步一拍的结论。
        const WeReadClient::ProgressSyncResult &res = st.weOp->progressSyncResult();
        st.weSyncLoaded = true;
        st.weSyncRemotePercent = clampI(static_cast<int>(res.remote.percent + 0.5f), 0, 100);
        st.mode = RdMode::WereadSync;
        if (res.outcome == WeReadClient::ProgressSyncOutcome::SelectionRequired) {
          // 两端不同：给选择屏，选完再起第二拍（库里没有任务内方向注入）。
          st.weSyncStage = 1;
          st.weSyncSel = 0;
        } else if (res.outcome == WeReadClient::ProgressSyncOutcome::ApplyRemote) {
          // 用云端：打开这本书并跳到云端位置（jumpToPercent 会把 mode 设成 Reading）。
          bool applied = false;
          const int bi = weBookIndex();
          if (bi >= 0) {
            const std::string path = WeReadStore::finalBookPath(st.weShelf[bi]);
            if (!path.empty() && openBook(path, 0)) {
              rdApplyProgressPercent(st.weSyncRemotePercent);
              applied = true;
            }
          }
          if (!applied) {
            st.weSyncStage = 2;
            st.weSyncStatus = "云端进度无法应用（先缓存这本书）";
          }
        } else {
          st.weSyncStage = 2;
          st.weSyncStatus = res.outcome == WeReadClient::ProgressSyncOutcome::AlreadySynced
                                ? "两端进度一致，无需同步"
                                : res.outcome == WeReadClient::ProgressSyncOutcome::LocalUploaded
                                      ? "已把本机进度上传到云端"
                                      : "同步未完成";
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
      st.mode = (st.weKind == 5) ? RdMode::WereadMenu : weAfterJobMode();
      st.fullRefresh = true;
      st.dirty = 1;
      break;
    case WeReadClient::Operation::Event::Failed:
      if (st.weKind == 5) {
        // 同步失败留在这一屏把原因写出来（登录失效那条最要紧——库里的 session 过期不可
        // 恢复，只能重新扫码）。
        st.weSyncLoaded = true;
        st.weSyncStage = 2;
        st.weSyncStatus = weErrorText(st.weOp->error());
        st.mode = RdMode::WereadSync;
      } else {
        st.weStatus = weErrorText(st.weOp->error());
        st.mode = weAfterJobMode();
      }
      st.fullRefresh = true;
      st.dirty = 1;
      break;
    case WeReadClient::Operation::Event::DetailReady:
      // 详情任务的进度画面就是进度屏（WereadDl），到这一拍数据已经落盘 —— 顺手读进来，
      // 这样 Complete 那一拍切屏时内容已经就绪，不会先闪一帧"获取中"。
      if (st.weKind == 3) weLoadDetailData();
      weMarkDirtyIfProgressChanged();   // 进度画面刷新（内容变了才推，见上）
      break;
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
  // 「书籍详情」「笔记与划线」两种书都给：两者都只是一两条接口、不依赖正文，缓存前先看看
  // 简介/划线圈注正是它们最有用的场景（笔记缓存在 /.crosspoint/weread/browse-cache 下，
  // 与书本身是否缓存无关）。
  // 「同步阅读进度」只有已缓存的书给：它要拿卡上的 toc.bin 把百分比映射到章节，
  // 没缓存就没有 toc，第一拍就会 Unavailable。
  if (cached)
    return {"打开本书", "书籍详情", "笔记与划线", "同步阅读进度", "重新获取封面", "重新缓存", "删除本地缓存",
            "取消"};
  return {"缓存整本并阅读", "书籍详情", "笔记与划线", "取消"};
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

// 书架页右下角的「搜索」按钮：进发现页（官方 Skill API 的搜索 / 推荐）。绘制与命中
// 共用一个式子（照 WiFi 页那条规矩——两处各写一遍迟早会漂）。物理键是 KEY_SEARCH（蓝牙
// 键盘的 `/`），触摸用户只有这一枚按钮，所以必须画出来。
static void weSearchBtnBox(int *x, int *y, int *w, int *h) {
  *w = g_rd.getTextWidth(uiFontId(), "搜索") + 20;
  *h = uiLineHeight() + 6;
  *x = g_rd.getScreenWidth() - MARGIN - *w;
  *y = statusTop() - *h - 4;
}

static void drawWeSearchButton() {
  int x, y, w, h;
  weSearchBtnBox(&x, &y, &w, &h);
  g_rd.drawRect(x, y, w, h, true);
  const int tw = g_rd.getTextWidth(uiFontId(), "搜索");
  drawLineText(x + (w - tw) / 2, y + 3, "搜索", true, uiFontId());
}

// 书架页底部那行状态：让开右下角那枚按钮，所以左对齐 + 截断（原来是居中，长文案会压到
// 按钮上）。
static void drawWeShelfStatus() {
  if (st.weStatus.empty()) return;
  int x, y, w, h;
  weSearchBtnBox(&x, &y, &w, &h);
  drawLineText(MARGIN, y + 3, g_rd.truncatedText(uiFontId(), st.weStatus.c_str(), x - MARGIN - 8).c_str(),
               true, uiFontId());
}

// 进发现页（书架页的「搜索」/ KEY_SEARCH 都走这里）。关键字清掉——上一轮搜的词留在框里
// 会让结果看着像这一次的；推荐结果留着，切回来不必再拉。
static void weEnterDiscover() {
  st.weDiscQuery.clear();
  st.weDiscTab = 0;
  st.weDiscSel = 0;
  st.weDiscStatus.clear();
  st.weDiscBusy = false;
  st.weDiscMenuFor = -1;
  st.mode = RdMode::WereadDiscover;
  st.fullRefresh = true;
  st.dirty = 1;
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
    drawWeSearchButton();
    drawFooter("回车 重新同步   Esc 回书架");
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
  drawWeShelfStatus();
  drawWeSearchButton();
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
  // 夹在 100：done 与 total 不是同一本账（progressCompleted_ 每下一片/每缓一张图都加，
  // total 按记录数算，重试还会再加），done > total 时 pct 会超 100 —— 不夹的话进度条
  // 画到框外（下面那行百分比也会是 "132%" 这种）。
  int pct = (total > 0) ? static_cast<int>((static_cast<uint64_t>(done) * 100) / total) : 0;
  if (pct > 100) pct = 100;
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
  // 发现页（搜索 / 推荐）走官方 Skill API，与扫码登录那条 cookie 链路无关；入口这一枚
  // 按钮只画在**登录之后**的书架页上（未登录时这一页是免责声明/登录引导，见 renderWeread）。
  if (key == KEY_SEARCH && weSessionValid()) { weEnterDiscover(); return; }
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
    // 「搜索」按钮（右下角）→ 发现页。命中框与绘制共用 weSearchBtnBox。
    if (weSessionValid()) {
      int bx, by, bw, bh;
      weSearchBtnBox(&bx, &by, &bw, &bh);
      if (x >= bx && x < bx + bw && y >= by && y < by + bh) { weEnterDiscover(); return; }
    }
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
  // 书籍详情：不依赖正文，未缓存的书也能看。进去就是详情屏——detail.bin 没读进来之
  // 前画"获取中"，请求返回（Complete）时 weDrive 会读盘并重画。
  if (label == "书籍详情") {
    st.weDetailLoaded = false;
    st.weDetailScroll = 0;
    st.weDetailLinesW = -1;   // 让建表逻辑重来（宽度变了/换了本书）
    st.weDetailLines.clear();
    st.weDetailIntro.clear();
    st.weDetailBookId = b.bookId;              // 详情屏按 bookId 取数，不再看书架选中项
    st.weDetailRet = RdMode::WereadMenu;
    weBeginJob(3, &b);
    if (st.weKind == 3 && st.weStatus.empty()) {
      st.mode = RdMode::WereadDetail;
      st.fullRefresh = true;
      st.dirty = 1;
    }
    return;
  }
  // 笔记与划线：本地有 browse 缓存就直接看，没有才起 Browse 任务（只有 Complete 一个
  // 终止事件 → 界面这头是"正在获取…"）。三类条数从 manifest 取，不必扫记录。
  if (label == "笔记与划线") {
    st.weNotesKind = 0;
    st.weNotesScroll = 0;
    st.weNotesLinesW = -1;
    st.weNotesLinesKind = -1;
    st.weNotesLines.clear();
    if (weLoadNotesData()) {
      st.mode = RdMode::WereadNotes;
      st.fullRefresh = true;
      st.dirty = 1;
    } else {
      st.weNotesLoaded = false;   // 本地没有 → 起任务，界面画加载态
      weBeginJob(4, &b);
      if (st.weKind == 4 && st.weStatus.empty()) {
        st.mode = RdMode::WereadNotes;   // weNotesLoaded 仍是 false → 画加载态
        st.fullRefresh = true;
        st.dirty = 1;
      }
    }
    return;
  }
  // 同步阅读进度（只有已缓存的书有这个菜单项）：本机进度直接取阅读统计里的整书百分比
  // ——不必打开这本书。第一拍 Compare，两端不同的话进选择屏再起第二拍。
  if (label == "同步阅读进度") {
    st.weSyncLoaded = false;
    st.weSyncStage = 0;
    st.weSyncSel = 0;
    st.weSyncStatus.clear();
    const ReadingBookStats *rs = ReadingStats::findBook(finalPath);
    st.weSyncLocalPercent = rs ? clampI(static_cast<int>(rs->lastProgressPercent), 0, 100) : 0;
    st.weSyncMode = WeReadClient::ProgressSyncMode::Compare;
    weBeginJob(5, &b);
    if (st.weKind == 5 && st.weStatus.empty()) {
      st.mode = RdMode::WereadSync;
      st.fullRefresh = true;
      st.dirty = 1;
    }
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

// ── 书籍详情页 ──────────────────────────────────────────────────────────
// 从书目菜单进来，看一本（本地缓存或尚未缓存的）书的元数据 + 简介。数据来自一条
// /web/book/info，落成 detail.bin（1024B 头 + 紧随的简介字节）。简介折行表缓存着滚，
// 与笔记详情/词典页同一套路——逐帧重折一个几十 KB 的串会拖垮滑动。
static constexpr int kWeDetailMaxLines = 2000;   // 只为防病态输入把折行表撑爆

// 详情从盘上读回内存。DetailReady / Complete 两拍都会调（幂等）：读失败也只是把
// loaded 置真、内容留空（画成"暂无简介"），不会卡在"获取中"那一帧。
static void weLoadDetailData() {
  st.weDetailLoaded = true;
  st.weDetailLinesW = -1;   // 换了数据（或换了本书），折行表作废
  st.weDetailLines.clear();
  st.weDetailIntro.clear();
  st.weDetailHeader = {};
  if (st.weDetailBookId.empty()) return;
  const std::string dir = WeReadStore::bookDirectory(st.weDetailBookId.c_str());
  WeReadStore::BookDetailHeader h;
  HalFile f;
  if (!WeReadStore::openBookDetail(dir, h, f)) return;   // 失败：头留空、简介留空
  st.weDetailHeader = h;
  const uint32_t n = h.introLength;
  if (n == 0 || n > WeReadStore::kMaxBookIntroBytes) return;
  std::string intro;
  intro.resize(n);
  const int got = f.read(&intro[0], n);   // 文件句柄已停在 1024B 头之后
  if (got <= 0) return;
  intro.resize(static_cast<size_t>(got));
  // 简介按 C 串喂给 wrappedText，里面若夹着 '\0'（填充）会被截在这里，免得后面画方框。
  const size_t z = intro.find('\0');
  if (z != std::string::npos) intro.resize(z);
  st.weDetailIntro = std::move(intro);
}

void renderWereadDetail() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = drawTitle("书籍详情");
  const int lh = uiLineHeight();
  const int lh4 = lh + 4;
  const int availW = w - 2 * MARGIN;

  // 还没读进来（请求在飞）：就是一句"获取中"。这一屏自推（rdSelfTickingProgress），
  // 不会因为静态停留而闪。
  if (!st.weDetailLoaded) {
    drawCenteredLine(top + 40, "正在获取书籍详情…");
    drawFooter("Esc 返回");
    return;
  }

  const WeReadStore::BookDetailHeader &h = st.weDetailHeader;
  int y = top;
  // 书名 + 作者。字段来自 detail.bin，可能是空串（老缓存 / 接口没给），兜个底。
  drawLineText(MARGIN, y, g_rd.truncatedText(uiFontId(), h.title[0] ? h.title : "（无书名）", availW).c_str(),
               true, uiFontId());
  y += lh;
  if (h.author[0]) {
    drawLineText(MARGIN, y, g_rd.truncatedText(uiFontId(), h.author, availW).c_str(), true, uiFontId());
    y += lh;
  }
  y += 2;
  // label/value 行：同「文件详情」的写法，值右移对齐、超宽截断。
  auto row = [&](const char *k, const std::string &v) {
    drawLineText(MARGIN, y, k, true, uiFontId());
    const int kx = MARGIN + g_rd.getTextWidth(uiFontId(), k) + 12;
    drawLineText(kx, y, g_rd.truncatedText(uiFontId(), v.c_str(), w - kx - MARGIN).c_str(), true, uiFontId());
    y += lh4;
  };
  if (h.publisher[0]) row("出版社", h.publisher);
  if (h.category[0]) row("分类", h.category);
  if (h.newRating > 0) {
    char b[48];
    snprintf(b, sizeof(b), "%.1f（%u 人）", h.newRating / 10.0, static_cast<unsigned>(h.newRatingCount));
    row("评分", b);   // newRating 是千分制
  }
  if (h.totalWords > 0) {
    char b[32];
    snprintf(b, sizeof(b), "%.1f 万字", h.totalWords / 10000.0);
    row("字数", b);
  }
  y += 2;
  g_rd.drawLine(MARGIN, y, w - MARGIN, y, true);
  y += 6;

  // 简介：按排版宽度折一次、缓存起来（宽度没变就不重折，同笔记详情）。
  if (st.weDetailLinesW != availW) {
    st.weDetailLinesW = availW;
    st.weDetailLines.clear();
    if (!st.weDetailIntro.empty()) {
      st.weDetailLines = g_rd.wrappedText(uiFontId(), st.weDetailIntro.c_str(), availW, kWeDetailMaxLines);
    }
  }
  const int bottom = statusTop();
  const int maxLines = std::max(1, (bottom - y) / lh4);
  const int total = static_cast<int>(st.weDetailLines.size());
  // 夹住首行偏移（翻页键只管加，上限在这里兜底，同笔记详情/词典页）。
  const int first = std::max(0, std::min(st.weDetailScroll, std::max(0, total - maxLines)));
  st.weDetailScroll = first;
  if (total == 0) {
    drawLineText(MARGIN, y, "（暂无简介）", true, uiFontId());
  } else {
    const int last = std::min(total, first + maxLines);
    for (int i = first; i < last; i++) {
      drawLineText(MARGIN, y, st.weDetailLines[i].c_str(), true, uiFontId());
      y += lh4;
    }
    if (total > maxLines) {
      char ind[24];
      const int pageCount = (total + maxLines - 1) / maxLines;
      snprintf(ind, sizeof(ind), "%d/%d", first / maxLines + 1, pageCount);
      const int tw = g_rd.getTextWidth(uiFontId(), ind);
      drawLineText(w - MARGIN - tw, bottom - lh, ind, true, uiFontId());
    }
  }
  drawFooter("上下翻页  长按返回");
}

void handleWereadDetail(int key) {
  if (key == 0x1B || key == KEY_LONG_CONFIRM || key == KEY_BACK) {
    // 从书目菜单进来就回菜单，从发现页进来就回发现页（weDetailRet 在两个入口处置位）。
    st.mode = st.weDetailRet;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  constexpr int kPage = 4;   // 一页翻几行（触屏"下半屏"与 PageUp/Down 共用）
  bool changed = false;
  if (key == KEY_UP) { st.weDetailScroll -= 1; changed = true; }
  else if (key == KEY_DOWN) { st.weDetailScroll += 1; changed = true; }
  else if (key == KEY_PAGE_UP) { st.weDetailScroll -= kPage; changed = true; }
  else if (key == KEY_PAGE_DOWN) { st.weDetailScroll += kPage; changed = true; }
  else if (key == '\n') {
    // 触摸：下半屏往下翻一页、上半屏往上翻一页（与阅读区手感一致）。
    int tx = 0, ty = 0;
    if (input_tap_xy(&tx, &ty)) {
      st.weDetailScroll += (ty > g_rd.getScreenHeight() / 2) ? kPage : -kPage;
      changed = true;
    }
  }
  if (changed) {
    if (st.weDetailScroll < 0) st.weDetailScroll = 0;   // 上限由渲染那拍夹住
    st.dirty = 1;
  }
}

// ── 笔记 / 划线页 ───────────────────────────────────────────────────────
// 三类：热门划线 / 我的划线 / 热门书评。数据来自 WeReadBrowse 的按书缓存（一次 Browse
// 任务把三类都抓齐）。左边条数从 manifest 直接取，正文折行只对当前这一类做（切 tab 重建）。
static const char *const kWeNotesTabNames[3] = {"热门划线", "我的划线", "热门书评"};
static constexpr int kWeNoteBody = 0;   // 正文
static constexpr int kWeNoteHead = 1;   // 条目头（章节名 / 作者）
static constexpr int kWeNoteMeta = 2;   // 热度 / 评分 / 提示
static constexpr int kWeNoteGap = 3;    // 条间空行
static constexpr int kWeNotesMaxLines = 4000;   // 防病态输入把折行表撑爆

// 读 manifest + 三类条数进内存。返回"本地是否有可用缓存"。
// weNotesLoaded 无论成败都置真：没有缓存时界面画的是"暂无笔记"而不是卡在加载态。
static bool weLoadNotesData() {
  st.weNotesLoaded = true;
  st.weNotesCacheValid = false;
  st.weNotesLinesW = -1;
  st.weNotesLinesKind = -1;
  st.weNotesLines.clear();
  for (int i = 0; i < 3; i++) st.weNotesCounts[i] = 0;
  st.weNotesReviewsLimited = false;
  const int idx = weBookIndex();
  if (idx < 0) return false;
  const char *bookId = st.weShelf[idx].bookId;
  WeReadStore::Session s;
  const char *vid = WeReadStore::loadSession(s) ? s.vid : "";
  WeReadBrowse::CacheManifest m;
  if (!WeReadBrowse::loadCache(bookId, vid, m)) return false;
  for (int i = 0; i < 3; i++) st.weNotesCounts[i] = m.recordCounts[i];
  st.weNotesReviewsLimited = (m.flags & WeReadBrowse::kCacheReviewsLimited) != 0;
  st.weNotesCacheValid = true;
  return true;
}

// 把当前 kind 的所有条目折成一张行表（头 + 正文 + meta + 间隔）。只在换 kind / 换宽度，
// 或缓存刚读进来时重算。
static void weBuildNoteLines() {
  const int maxW = g_rd.getScreenWidth() - 2 * MARGIN - 8;
  st.weNotesLinesW = maxW;
  st.weNotesLinesKind = st.weNotesKind;
  st.weNotesLines.clear();
  if (!st.weNotesCacheValid) return;
  const int idx = weBookIndex();
  if (idx < 0) return;
  const char *bookId = st.weShelf[idx].bookId;
  WeReadStore::Session s;
  const char *vid = WeReadStore::loadSession(s) ? s.vid : "";
  WeReadBrowse::CacheManifest m;
  if (!WeReadBrowse::loadCache(bookId, vid, m)) return;
  const auto kind = static_cast<WeReadBrowse::Kind>(st.weNotesKind);
  const size_t ki = WeReadBrowse::kindIndex(kind);
  auto push = [](std::string t, int k) { st.weNotesLines.push_back({std::move(t), k}); };
  auto full = [&]() { return static_cast<int>(st.weNotesLines.size()) >= kWeNotesMaxLines; };
  auto wrap = [&](const std::string &body, int k) {
    auto ls = g_rd.wrappedText(uiFontId(), body.c_str(), maxW, 512);
    for (auto &l : ls) {
      if (full()) return;
      push(l, k);
    }
  };
  for (uint32_t p = 0; p < m.pageCounts[ki] && !full(); p++) {
    WeReadBrowse::PageHeader ph;
    HalFile index, text;
    if (!WeReadBrowse::openPage(bookId, m, kind, p, ph, index, text)) continue;
    for (uint32_t r = 0; r < ph.count && !full(); r++) {
      WeReadBrowse::Record rec;
      if (!WeReadBrowse::readRecord(index, ph, r, rec)) continue;
      // 正文：从该页 txt 的 textOffset 取 textLength 字节
      std::string body;
      if (rec.textLength > 0 && rec.textLength <= WeReadBrowse::kMaxItemTextBytes &&
          text.seek(rec.textOffset)) {
        body.resize(rec.textLength);
        const int got = text.read(&body[0], rec.textLength);
        body.resize(got > 0 ? static_cast<size_t>(got) : 0);
      }
      const size_t z = body.find('\0');   // 末尾填充的 '\0' 会让 c_str() 提前断
      if (z != std::string::npos) body.resize(z);
      // 头：书评给作者（退回章节名），划线给章节名（空则给类别名）
      std::string head;
      if (st.weNotesKind == 2) head = rec.author[0] ? rec.author : (rec.chapter[0] ? rec.chapter : "书评");
      else head = rec.chapter[0] ? rec.chapter : (st.weNotesKind == 1 ? "我的划线" : "热门划线");
      push(std::move(head), kWeNoteHead);
      if (body.empty()) push("（无内容）", kWeNoteBody);
      else wrap(body, kWeNoteBody);
      char meta[64];
      meta[0] = '\0';
      if (st.weNotesKind == 2 && rec.rating > 0) snprintf(meta, sizeof(meta), "评分 %u%%", static_cast<unsigned>(rec.rating));
      else if (rec.heat > 0) snprintf(meta, sizeof(meta), "%u 人划过", static_cast<unsigned>(rec.heat));
      if (meta[0]) push(meta, kWeNoteMeta);
      push("", kWeNoteGap);
    }
  }
  if (st.weNotesKind == 2 && st.weNotesReviewsLimited) push("（仅显示前 50 条书评）", kWeNoteMeta);
}

// tab i 的左端 x 与宽度：渲染与命中测试共用同一套几何，免得两处各写一遍走偏。
static int weNotesTabBox(int i, int *wOut) {
  int x = MARGIN;
  for (int k = 0; k <= i; k++) {
    char label[48];
    snprintf(label, sizeof(label), "%s %u", kWeNotesTabNames[k], static_cast<unsigned>(st.weNotesCounts[k]));
    const int bw = g_rd.getTextWidth(uiFontId(), label) + 2 * 10;
    if (k == i) {
      if (wOut) *wOut = bw;
      return x;
    }
    x += bw + 8;
  }
  return x;
}

void renderWereadNotes() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = drawTitle("笔记与划线");
  const int lh = uiLineHeight();
  const int lh4 = lh + 4;

  // 加载态：Browse 任务只有一个终止事件，中途没有进度可画。
  if (!st.weNotesLoaded) {
    drawCenteredLine(top + 40, "正在获取笔记与划线…");
    drawFooter("Esc 返回");
    return;
  }

  // tab 行：三段（当前黑底白字），标签带条数。
  int y = top;
  {
    const int padX = 10, padY = 4, tabH = lh + 2 * padY;
    for (int i = 0; i < 3; i++) {
      int boxW = 0;
      const int bx = weNotesTabBox(i, &boxW);
      if (bx + boxW > w - MARGIN) break;
      char label[48];
      snprintf(label, sizeof(label), "%s %u", kWeNotesTabNames[i], static_cast<unsigned>(st.weNotesCounts[i]));
      const bool active = (i == st.weNotesKind);
      if (active) g_rd.fillRect(bx, y, boxW, tabH, true);
      drawLineText(bx + padX, y + padY, label, !active, uiFontId());
    }
    y += tabH + 6;
  }

  if (!st.weNotesCacheValid) {
    drawLineText(MARGIN, y, "这本书还没有笔记或划线", true, uiFontId());
    drawFooter("Esc 返回");
    return;
  }

  const int maxW = w - 2 * MARGIN - 8;
  if (st.weNotesLinesW != maxW || st.weNotesLinesKind != st.weNotesKind) weBuildNoteLines();

  const int bottom = statusTop();
  const int maxLines = std::max(1, (bottom - y) / lh4);
  const int total = static_cast<int>(st.weNotesLines.size());
  const int first = std::max(0, std::min(st.weNotesScroll, std::max(0, total - maxLines)));
  st.weNotesScroll = first;
  if (total == 0) {
    drawLineText(MARGIN, y, "这一类还没有内容", true, uiFontId());
  } else {
    const int last = std::min(total, first + maxLines);
    for (int i = first; i < last; i++) {
      const int k = st.weNotesLines[i].kind;
      const std::string &t = st.weNotesLines[i].text;
      if (k == kWeNoteGap) { y += lh4 / 2; continue; }
      if (k == kWeNoteHead) {
        if (y + lh4 > bottom) break;   // 半截头露出来更难看，放不下就整条不画
        g_rd.fillRect(MARGIN, y + 2, 3, lh, true);   // 左端一条竖线，标出条目起始
        drawLineText(MARGIN + 10, y, g_rd.truncatedText(uiFontId(), t.c_str(), maxW).c_str(), true, uiFontId());
        y += lh4;
        continue;
      }
      if (k == kWeNoteMeta) {
        drawLineText(MARGIN + 10, y, g_rd.truncatedText(uiFontId(), t.c_str(), maxW).c_str(), true, uiFontId());
        y += lh4;
        continue;
      }
      drawLineText(MARGIN + 10, y, t.c_str(), true, uiFontId());
      y += lh4;
    }
    if (total > maxLines) {
      char ind[24];
      const int pageCount = (total + maxLines - 1) / maxLines;
      snprintf(ind, sizeof(ind), "%d/%d", first / maxLines + 1, pageCount);
      const int tw = g_rd.getTextWidth(uiFontId(), ind);
      drawLineText(w - MARGIN - tw, bottom - lh, ind, true, uiFontId());
    }
  }
  drawFooter("左右切类  上下翻页  长按返回");
}

void handleWereadNotes(int key) {
  if (key == 0x1B || key == KEY_LONG_CONFIRM || key == KEY_BACK) {
    st.mode = RdMode::WereadMenu;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  constexpr int kPage = 4;
  bool changed = false;
  if (key == KEY_LEFT) { st.weNotesKind = (st.weNotesKind + 2) % 3; st.weNotesScroll = 0; changed = true; }
  else if (key == KEY_RIGHT) { st.weNotesKind = (st.weNotesKind + 1) % 3; st.weNotesScroll = 0; changed = true; }
  else if (key == KEY_UP) { st.weNotesScroll -= 1; changed = true; }
  else if (key == KEY_DOWN) { st.weNotesScroll += 1; changed = true; }
  else if (key == KEY_PAGE_UP) { st.weNotesScroll -= kPage; changed = true; }
  else if (key == KEY_PAGE_DOWN) { st.weNotesScroll += kPage; changed = true; }
  else if (key == '\n') {
    int tx = 0, ty = 0;
    if (input_tap_xy(&tx, &ty)) {
      // 点在 tab 行上 → 切那一段；否则下半屏下翻、上半屏上翻。
      const int tabTop = coverTop();               // == drawTitle 返回值
      const int tabH = uiLineHeight() + 8;
      bool hitTab = false;
      if (ty >= tabTop && ty < tabTop + tabH) {
        for (int i = 0; i < 3; i++) {
          int boxW = 0;
          const int bx = weNotesTabBox(i, &boxW);
          if (tx >= bx && tx < bx + boxW) {
            st.weNotesKind = i;
            st.weNotesScroll = 0;
            changed = true;
            hitTab = true;
            break;
          }
        }
      }
      if (!hitTab && !(ty >= tabTop && ty < tabTop + tabH)) {
        st.weNotesScroll += (ty > g_rd.getScreenHeight() / 2) ? kPage : -kPage;
        changed = true;
      }
    }
  }
  if (changed) {
    if (st.weNotesScroll < 0) st.weNotesScroll = 0;   // 上限由渲染那拍夹住
    st.dirty = 1;
  }
}

// ── 阅读进度同步页 ──────────────────────────────────────────────────────
// 本机进度取自 ReadingStats（不必打开书），云端进度由 Compare 那一拍回填。三段：
//   0 = 核对中（任务在飞）  1 = 需要选方向（两端不同）  2 = 结果
// 选完方向再起一拍（UploadLocal / ApplyRemote）——库里没有任务内方向注入。
// 选择行的起始 y：渲染与点按命中都用这一个式子（同 weMenuFirstY 的做法）。
static int weSyncRowTop() { return coverTop() + 2 * (uiLineHeight() + 6) + 16; }

void renderWereadSync() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = drawTitle("阅读进度同步");
  const int lh = uiLineHeight();
  int y = top;
  const int bi = weBookIndex();
  if (bi >= 0) {
    drawLineText(MARGIN, y, g_rd.truncatedText(uiFontId(), st.weShelf[bi].title, w - 2 * MARGIN).c_str(), true,
                 uiFontId());
  }
  y += lh + 6;
  char buf[64];
  snprintf(buf, sizeof(buf), "本机  %d%%", st.weSyncLocalPercent);
  drawLineText(MARGIN, y, buf, true, uiFontId());
  y += lh + 6;
  if (st.weSyncStage >= 1) {
    snprintf(buf, sizeof(buf), "云端  %d%%", st.weSyncRemotePercent);
    drawLineText(MARGIN, y, buf, true, uiFontId());
    y += lh + 6;
  }
  y += 6;
  g_rd.drawLine(MARGIN, y, w - MARGIN, y, true);

  if (st.weSyncStage == 0) {
    drawCenteredLine(weSyncRowTop() + 24, "正在与云端核对进度…");
    drawFooter("Esc 返回");
    return;
  }
  if (st.weSyncStage == 1) {
    static const char *const kLabels[2] = {"用本机进度上传", "用云端进度"};
    const int itemH = weMenuItemH();
    int ry = weSyncRowTop();
    for (int i = 0; i < 2; i++) {
      if (i == st.weSyncSel) {
        g_rd.fillRect(2, ry, w - 4, itemH - 2, true);
        drawLineText(MARGIN, ry + 6, kLabels[i], false);
      } else {
        drawLineText(MARGIN, ry + 6, kLabels[i], true);
      }
      ry += itemH;
    }
    drawFooter("↑↓ 选择  Enter 确定  Esc 返回");
    return;
  }
  drawCenteredLine(weSyncRowTop() + 24, st.weSyncStatus.c_str());
  drawFooter("Esc 返回");
}

void handleWereadSync(int key) {
  if (key == 0x1B || key == KEY_LONG_CONFIRM || key == KEY_BACK) {
    st.mode = RdMode::WereadMenu;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }
  // 任务在飞：按键由主循环的 active 分支处理（那里只认 Esc）；结果屏也只认 Esc。
  if (!st.weSyncLoaded || st.weSyncStage != 1) return;
  {
    ListView lv = flatMenuListView(2, st.weSyncSel, weSyncRowTop());
    if (listViewKey(lv, key)) {
      st.weSyncSel = lv.sel;
      st.dirty = 1;
      return;
    }
  }
  if (key != '\n') return;
  int tx = 0, ty = 0;
  if (input_tap_xy(&tx, &ty)) {
    // 点在哪一行就选哪一行；点在列表外忽略。
    const int hit = listViewHitAt(flatMenuListView(2, st.weSyncSel, weSyncRowTop()), ty);
    if (hit >= 0) st.weSyncSel = hit;
    else return;
  }
  const int bi = weBookIndex();
  if (bi < 0) return;
  st.weSyncMode = (st.weSyncSel == 0) ? WeReadClient::ProgressSyncMode::UploadLocal
                                      : WeReadClient::ProgressSyncMode::ApplyRemote;
  st.weSyncLoaded = false;
  st.weSyncStage = 0;
  weBeginJob(5, &st.weShelf[bi]);
  if (!st.weStatus.empty()) {   // 起任务就失败了（没网/登录失效）：直接把原因当结果画
    st.weSyncLoaded = true;
    st.weSyncStage = 2;
    st.weSyncStatus = st.weStatus;
  }
  st.mode = RdMode::WereadSync;
  st.fullRefresh = true;
  st.dirty = 1;
}

// ── 发现页（搜索 / 推荐）────────────────────────────────────────────────
// 走的是**官方 Skill/Gateway**（i.weread.qq.com，API Key 在设置键 weread_api_key），
// 与扫码登录那条 cookie 链路完全独立。两件事：搜书、拿推荐；结果都能「看详情」
// （复用 WereadDetail）和「缓存整本并阅读」（复用整本缓存那条链）。
//
// 请求是同步阻塞的（同微读其它请求），所以发起前先 renderCurrent() 推一帧"正在搜索…"，
// 回来再画结果。屏上大部分是静态文字，走 rdSelfTickingProgress 的 GRAY8 波形不闪。
static const char *const kWeDiscTabNames[2] = {"搜索", "推荐"};

static int weDiscResultCount() {
  return st.weDiscTab == 0 ? static_cast<int>(st.weDiscSearch.size())
                           : static_cast<int>(st.weDiscRecommend.size());
}
// 搜索 tab 的第 0 行是查询行（结果从第 1 行起）；推荐 tab 没有查询行。
static int weDiscQueryRows() { return st.weDiscTab == 0 ? 1 : 0; }
static int weDiscVirtualCount() { return weDiscQueryRows() + weDiscResultCount(); }
// 光标此刻停在查询行上吗（只有搜索 tab 有这一行）。
static bool weDiscOnQuery() { return weDiscQueryRows() == 1 && st.weDiscSel == 0; }
// 光标对应的**结果下标**；不在结果上（查询行 / 越界）返回 -1。
static int weDiscSelIndex() {
  const int i = st.weDiscSel - weDiscQueryRows();
  return (i >= 0 && i < weDiscResultCount()) ? i : -1;
}

// tab i 的左端 x 与宽度：渲染与命中测试共用同一套几何（同笔记页的 tab）。
static int weDiscTabBox(int i, int *wOut) {
  int x = MARGIN;
  for (int k = 0; k <= i; k++) {
    const int bw = g_rd.getTextWidth(uiFontId(), kWeDiscTabNames[k]) + 2 * 10;
    if (k == i) {
      if (wOut) *wOut = bw;
      return x;
    }
    x += bw + 8;
  }
  return x;
}

static int weDiscItemH() { return uiLineHeight() * 2 + 8; }
// tab 行高 + 分隔线 + 6px。渲染从 drawTitle 的返回值往下推，命中测试从 coverTop() 推，
// 两者必须同源（coverTop() == drawTitle 的返回值）。
static int weDiscListTop() { return coverTop() + uiLineHeight() + 20; }

// 列表窗口：与书架/书目菜单同一套 ListView（居中式窗口，行高一致）。
static ListView weDiscListView() {
  ListView lv;
  lv.top = weDiscListTop();
  lv.itemH = weDiscItemH();
  lv.count = weDiscVirtualCount();
  // 底边用 rdBodyBottom()（虚拟键盘弹着时会缩上去）——这一页有搜索框，键盘是常客。
  lv.rows = std::max(1, (rdBodyBottom() - lv.top) / lv.itemH);
  lv.page = lv.rows;   // 上下滑 = 整页翻（见 weShelfListView 的同一条注释）
  lv.sel = st.weDiscSel;
  listViewCenter(lv);
  return lv;
}

// 结果项的动作菜单（浮层）。推荐 tab 多一项"重新获取推荐"——推荐列表没有查询行可以
// 再按一次回车，不给这一项就没法刷新。
static std::vector<std::string> weDiscMenuItems() {
  std::vector<std::string> v;
  v.push_back("看详情");
  v.push_back("缓存整本并阅读");
  if (st.weDiscTab == 1) v.push_back("重新获取推荐");
  v.push_back("取消");
  return v;
}

static void weDiscMenuBox(int *bx, int *by, int *bw, int *bh) {
  const std::vector<std::string> items = weDiscMenuItems();
  int w = 0;
  for (const auto &s : items) w = std::max(w, g_rd.getTextWidth(uiFontId(), s.c_str()) + 2 * uiLineHeight());
  const int sw = g_rd.getScreenWidth();
  if (w < 240) w = 240;
  if (w > sw - 32) w = sw - 32;
  *bw = w;
  *bh = 8 + static_cast<int>(items.size()) * (uiLineHeight() + 6) + 8;
  *bx = (sw - w) / 2;
  *by = (g_rd.getScreenHeight() - *bh) / 2;
}

static int weDiscMenuRowTop(int by, int i) { return by + 8 + i * (uiLineHeight() + 6); }

static int weDiscMenuRowAt(int x, int y) {
  int bx, by, bw, bh;
  weDiscMenuBox(&bx, &by, &bw, &bh);
  if (x < bx || x >= bx + bw || y < by || y >= by + bh) return -1;
  const int n = static_cast<int>(weDiscMenuItems().size());
  for (int i = 0; i < n; i++) {
    const int ry = weDiscMenuRowTop(by, i);
    if (y >= ry - 2 && y < ry + uiLineHeight() + 2) return i;
  }
  return -1;
}

// 把一条发现结果合成 ShelfRecord —— 整本缓存 / 详情 / finalBookPath 都要这个类型，而
// finalBookPath 只用 title + bookId 两段（见 WeReadStore::finalBookPath），所以搜到的书
// 与书架同步下来的那本会落到同一个文件上。
static WeReadStore::ShelfRecord weDiscMakeRecord(int i) {
  WeReadStore::ShelfRecord rec{};
  const std::string id = (st.weDiscTab == 0) ? st.weDiscSearch[i].bookId : st.weDiscRecommend[i].bookId;
  const std::string title = (st.weDiscTab == 0) ? st.weDiscSearch[i].title : st.weDiscRecommend[i].title;
  const std::string author = (st.weDiscTab == 0) ? st.weDiscSearch[i].author : st.weDiscRecommend[i].author;
  snprintf(rec.bookId, sizeof(rec.bookId), "%s", id.c_str());
  snprintf(rec.title, sizeof(rec.title), "%s", title.c_str());
  snprintf(rec.author, sizeof(rec.author), "%s", author.c_str());
  return rec;
}

// 拉搜索 / 推荐。阻塞：发起前先推一帧"正在…"。
static void weDiscFetch(int tab) {
  if (st.weDiscBusy) return;   // 上一次还在飞（理论上不会，请求是同步的）
  st.weDiscTab = tab;
  st.weDiscBusy = true;
  st.weDiscStatus = (tab == 0) ? "正在搜索…" : "正在获取推荐…";
  st.fullRefresh = true;
  st.dirty = 1;
  renderCurrent();   // 先把"正在…"推出去（照 weBeginJob 连 WiFi 那一拍的写法）
  st.fullRefresh = false;

  // 发现页是随时能进的（只看有没有登录 cookie），进来时 WiFi 未必还连着——不先连上，
  // HTTP 层只会给一句含糊的"网络连接失败"（status 0）。同 weBeginJob 那条路子。
  if (!g_wifi.isConnected()) {
    st.weDiscStatus = "正在连接 WiFi...";
    st.fullRefresh = true;
    st.dirty = 1;
    renderCurrent();
    st.fullRefresh = false;
    std::string werr;
    if (!readerEnsureWifi(werr)) {
      st.weDiscBusy = false;
      st.weDiscStatus = werr;
      st.fullRefresh = true;
      st.dirty = 1;
      return;
    }
  }

  const WereadSkillClient::Result r =
      (tab == 0) ? g_weread_skill.search(st.weDiscQuery, 20) : g_weread_skill.recommend(20);

  st.weDiscBusy = false;
  if (tab == 0) {
    st.weDiscSearch = r.search;
    st.weDiscSearchLoaded = true;
  } else {
    st.weDiscRecommend = r.recommend;
    st.weDiscRecLoaded = true;
  }
  if (r.no_key) {
    st.weDiscStatus = "未配置微信读书 API Key（设置 → 网络与同步）";
  } else if (!r.success) {
    st.weDiscStatus = r.error.empty() ? "请求失败" : r.error;
  } else if (weDiscResultCount() == 0) {
    st.weDiscStatus = (tab == 0) ? "没有找到相关的书" : "暂无推荐";
  } else {
    st.weDiscStatus.clear();
  }
  // 光标落到第一条结果上（有结果的话）；再按 ↑ 就回查询行重新搜。
  st.weDiscSel = (weDiscResultCount() > 0) ? (0 + weDiscQueryRows()) : 0;
  st.fullRefresh = true;
  st.dirty = 1;
}

// 「看详情」：把当前结果当成本书开详情屏（详情屏按 weDetailBookId 取数，不看书架）。
static void weDiscOpenDetail() {
  const int i = weDiscSelIndex();
  if (i < 0) return;
  const WeReadStore::ShelfRecord rec = weDiscMakeRecord(i);
  st.weDetailLoaded = false;
  st.weDetailScroll = 0;
  st.weDetailLinesW = -1;
  st.weDetailLines.clear();
  st.weDetailIntro.clear();
  st.weDetailBookId = rec.bookId;
  st.weDetailRet = RdMode::WereadDiscover;
  weBeginJob(3, &rec);
  st.weJobRet = RdMode::WereadDiscover;   // 任务结束 / 失败都回发现页
  if (st.weKind == 3 && st.weStatus.empty()) {
    st.mode = RdMode::WereadDetail;
    st.fullRefresh = true;
    st.dirty = 1;
  } else {
    st.weDiscStatus = st.weStatus;   // 起不来（没网 / Key 问题）就在发现页写出来
    st.dirty = 1;
  }
}

// 「缓存整本并阅读」：走已有的整本缓存那条链（kind=1）。Complete 时会 scanBooks 并开书。
static void weDiscCacheBook() {
  const int i = weDiscSelIndex();
  if (i < 0) return;
  const WeReadStore::ShelfRecord rec = weDiscMakeRecord(i);
  weBeginJob(1, &rec);
  st.weJobRet = RdMode::WereadDiscover;
  if (st.weKind == 1 && st.weStatus.empty()) {
    st.mode = RdMode::WereadDl;
    st.fullRefresh = true;
    st.dirty = 1;
  } else {
    st.weDiscStatus = st.weStatus;
    st.dirty = 1;
  }
}

void renderWereadDiscover() {
  g_rd.clearScreen();
  const int w = g_rd.getScreenWidth();
  const int top = drawTitle("发现");
  const int lh = uiLineHeight();

  // tab 行：搜索 / 推荐（当前黑底白字）。分隔线与 weDiscListTop() 同一套几何。
  {
    const int padX = 10, padY = 4, tabH = lh + 2 * padY;
    for (int i = 0; i < 2; i++) {
      int boxW = 0;
      const int bx = weDiscTabBox(i, &boxW);
      const bool active = (i == st.weDiscTab);
      if (active) g_rd.fillRect(bx, top, boxW, tabH, true);
      drawLineText(bx + padX, top + padY, kWeDiscTabNames[i], !active, uiFontId());
    }
    const int sepY = top + tabH + 6;
    g_rd.drawLine(MARGIN, sepY, w - MARGIN, sepY, true);
  }

  const int n = weDiscResultCount();
  if (n == 0 && !weDiscOnQuery()) {
    // 空态：搜索 tab 还没搜过 / 推荐 tab 还没拉过 / 拉过但没有结果，三种文案分开。
    const char *msg = nullptr;
    if (!st.weDiscStatus.empty()) msg = nullptr;   // 下面统一画状态行
    else if (st.weDiscTab == 0) msg = st.weDiscSearchLoaded ? "没有找到相关的书" : "输入关键字后回车搜索";
    else msg = st.weDiscRecLoaded ? "暂无推荐" : "回车获取推荐";
    if (msg) drawCenteredLine(weDiscListTop() + 30, msg);
  } else {
    const ListView lv = weDiscListView();
    st.weDiscSel = lv.sel;
    const int itemH = lv.itemH;
    const int bottom = rdBodyBottom();
    int y = lv.top;
    for (int row = lv.first; row < lv.count && y + itemH <= bottom; row++) {
      const bool sel = (row == st.weDiscSel);
      if (sel) g_rd.fillRect(2, y, w - 4, itemH - 2, true);
      if (row < weDiscQueryRows()) {
        // 查询行：第一行是关键字（空的就给提示），第二行是提示。选中时补一根光标条。
        const std::string q = st.weDiscQuery;
        if (q.empty()) {
          drawLineText(MARGIN, y + 3, "输入书名 / 作者", !sel, uiFontId());
        } else {
          const std::string qt = g_rd.truncatedText(uiFontId(), q.c_str(), w - 2 * MARGIN - 8, EpdFontFamily::REGULAR);
          drawLineText(MARGIN, y + 3, qt.c_str(), !sel, uiFontId());
          if (sel) {
            const int qw = g_rd.getTextWidth(uiFontId(), qt.c_str());
            if (MARGIN + qw + 6 < w - MARGIN) {
              g_rd.fillRect(MARGIN + qw + 3, y + 5, 2, lh - 6, !sel);
            }
          }
        }
        drawLineText(MARGIN, y + 3 + lh + 2, "回车搜索", !sel, uiFontId());
      } else {
        const int i = row - weDiscQueryRows();
        std::string line1, line2;
        if (st.weDiscTab == 0) {
          const WereadSkillClient::SearchItem &it = st.weDiscSearch[i];
          line1 = it.title.empty() ? "(无书名)" : it.title;
          line2 = it.author;
          if (!it.category.empty()) line2 += (line2.empty() ? "" : "  ·  ") + it.category;
          if (it.rating > 0) {
            char b[32];
            snprintf(b, sizeof(b), "  ·  %.1f", it.rating);
            line2 += b;
          }
        } else {
          const WereadSkillClient::RecommendItem &it = st.weDiscRecommend[i];
          line1 = it.title.empty() ? "(无书名)" : it.title;
          line2 = it.author;
          if (!it.reason.empty()) line2 += (line2.empty() ? "" : "  ·  ") + it.reason;
        }
        drawLineText(MARGIN, y + 3,
                     g_rd.truncatedText(uiFontId(), line1.c_str(), w - 2 * MARGIN, EpdFontFamily::REGULAR).c_str(),
                     !sel, uiFontId());
        drawLineText(MARGIN, y + 3 + lh + 2,
                     g_rd.truncatedText(uiFontId(), line2.c_str(), w - 2 * MARGIN, EpdFontFamily::REGULAR).c_str(),
                     !sel, uiFontId());
      }
      y += itemH;
    }
  }

  // 状态行（空结果 / 错误 / "正在…"）：画在列表下沿上面一行，居中。
  if (!st.weDiscStatus.empty()) drawCenteredLine(rdBodyBottom() - lh - 4, st.weDiscStatus.c_str());

  // 动作菜单浮层（盖在底图上面）。
  if (st.weDiscMenuFor >= 0) {
    int bx, by, bw, bh;
    weDiscMenuBox(&bx, &by, &bw, &bh);
    g_rd.fillRect(bx, by, bw, bh, true);
    g_rd.drawRect(bx, by, bw, bh, false);
    const std::vector<std::string> items = weDiscMenuItems();
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
      const int ry = weDiscMenuRowTop(by, i);
      const int tw = g_rd.getTextWidth(uiFontId(), items[i].c_str());
      const int tx = bx + (bw - tw) / 2;
      if (i == st.weDiscMenuSel) {
        g_rd.fillRect(bx + 4, ry - 2, bw - 8, lh + 4, false);
        drawLineText(tx, ry, items[i].c_str(), true, uiFontId());
      } else {
        drawLineText(tx, ry, items[i].c_str(), true, uiFontId());
      }
    }
    drawFooter("↑↓ 选择  Enter 确定  Esc 取消");
    return;
  }

  if (st.vkVisible) {
    drawVk();
  } else {
    drawRdImeBar();   // 实体键盘打拼音：编码行 + 候选行
    drawFooter(weDiscOnQuery() ? "回车 搜索  ←→ 切页  Esc 返回"
                                  : "↑↓ 选择  回车 打开  ←→ 切页  Esc 返回");
  }
  rdDrawVkIcon();   // 搜索框可编辑时才给键盘开关（同书架搜索页）
}

void handleWereadDiscover(int key) {
  if (key == 0x1B || key == KEY_LONG_CONFIRM || key == KEY_BACK) {
    if (st.weDiscMenuFor >= 0) {   // 菜单开着：Esc 先关菜单
      st.weDiscMenuFor = -1;
      st.dirty = 1;
      return;
    }
    IME::getInstance().cancelComposition();
    st.vkVisible = false;
    st.mode = RdMode::Weread;
    st.fullRefresh = true;
    st.dirty = 1;
    return;
  }

  // 动作菜单开着：↑↓ 选、回车确定（点按同样）。
  if (st.weDiscMenuFor >= 0) {
    const std::vector<std::string> items = weDiscMenuItems();
    const int n = static_cast<int>(items.size());
    if (key == KEY_UP) { st.weDiscMenuSel = std::max(0, st.weDiscMenuSel - 1); st.dirty = 1; return; }
    if (key == KEY_DOWN) { st.weDiscMenuSel = std::min(n - 1, st.weDiscMenuSel + 1); st.dirty = 1; return; }
    if (key != '\n') return;
    int tx = 0, ty = 0;
    if (input_tap_xy(&tx, &ty)) {
      const int hit = weDiscMenuRowAt(tx, ty);
      if (hit < 0) return;   // 点在菜单外：不动
      st.weDiscMenuSel = hit;
    }
    const std::string label = items[clampI(st.weDiscMenuSel, 0, n - 1)];
    st.weDiscMenuFor = -1;
    if (label == "看详情") { weDiscOpenDetail(); return; }
    if (label == "缓存整本并阅读") { weDiscCacheBook(); return; }
    if (label == "重新获取推荐") { weDiscFetch(1); return; }
    st.dirty = 1;   // 取消
    return;
  }

  // tab 切换（←→）。
  if (key == KEY_LEFT || key == KEY_RIGHT) {
    const int t = (key == KEY_LEFT) ? 0 : 1;
    if (t != st.weDiscTab) {
      st.weDiscTab = t;
      st.weDiscSel = 0;
      st.weDiscStatus.clear();
      st.fullRefresh = true;
      st.dirty = 1;
    }
    return;
  }

  // 列表导航：虚拟行（搜索 tab 的第 0 行是查询行）。
  if (weDiscVirtualCount() > 0) {
    ListView lv = weDiscListView();
    if (listViewKey(lv, key)) { st.weDiscSel = lv.sel; st.dirty = 1; return; }
  }

  if (key == '\n') {
    int tx = 0, ty = 0;
    if (input_tap_xy(&tx, &ty)) {
      // 先给 VK/输入法：键盘图标、键盘面板上的键。
      if (rdVkIconHit(tx, ty)) { rdToggleVk(); return; }
      if (st.vkVisible && ty >= vkVkTop()) { vkTap(tx, ty); st.dirty = 1; return; }
      // tab 行 / 列表行。
      for (int i = 0; i < 2; i++) {
        int bw = 0;
        const int bx = weDiscTabBox(i, &bw);
        const int by = coverTop();
        if (tx >= bx && tx < bx + bw && ty >= by && ty < by + uiLineHeight() + 8) {
          if (i != st.weDiscTab) {
            st.weDiscTab = i;
            st.weDiscSel = 0;
            st.weDiscStatus.clear();
          }
          st.fullRefresh = true;
          st.dirty = 1;
          return;
        }
      }
      const int row = listViewHitAt(weDiscListView(), ty);
      if (row >= 0) {
        st.weDiscSel = row;
        st.dirty = 1;
        // 点查询行 = 搜一次；点结果行 = 弹动作菜单（见下，落到同一段逻辑）。
      } else {
        st.dirty = 1;
        return;
      }
    }
    // 回车（或刚落到某一行上）：查询行 → 搜索；结果行 → 动作菜单。
    if (weDiscOnQuery()) {
      if (st.weDiscTab == 0) weDiscFetch(0);
      return;   // 推荐 tab 不会有查询行（weDiscOnQuery 恒 false）
    }
    if (weDiscSelIndex() >= 0) {
      st.weDiscMenuFor = weDiscSelIndex();
      st.weDiscMenuSel = 0;
      st.dirty = 1;
      return;
    }
    // 光标在空列表上：搜索 tab 就再搜一次，推荐 tab 就拉推荐。
    if (st.weDiscTab == 0) weDiscFetch(0);
    else weDiscFetch(1);
    return;
  }

  // 打字进搜索框（VK/IME 的胶水与其它搜索页同一套）。
  if (key == 0x08) { feedVkBackspace(); st.weDiscSel = 0; st.dirty = 1; return; }
  if (key >= 0x20 && key <= 0x7E) { feedVkKey(key); st.weDiscSel = 0; st.dirty = 1; return; }
}

